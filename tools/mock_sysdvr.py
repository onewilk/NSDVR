#!/usr/bin/env python3
"""模拟 Switch 上的 SysDVR sysmodule（TCP Bridge 模式），用于在没有 Switch 的情况下测试客户端。

行为照着 sysmodule 源码实现：
  - 每 2 秒向 UDP 19999 广播 "SysDVR|6.3|03|<序列号>"
  - TCP 9911（视频）/ 9922（音频）各自独立：发 hello -> 收 16 字节握手 -> 回结果码 -> 持续推流
  - 视频：支持 SPS/PPS 注入（首包 + 每 5 个 IDR）、NAL 哈希重放（重复的 IDR 只发槽位号）
  - 音频：48kHz / 16bit / 立体声 PCM，按 (1 + batching) * 0x1000 字节打包，实时节奏发送

用法：
  python3 mock_sysdvr.py                          # 合成数据（协议测试用，不能解码）
  python3 mock_sysdvr.py --h264 test.h264         # 用真实 H.264（Annex-B）文件循环推流，可在鸿蒙端看到画面
  python3 mock_sysdvr.py --video-packets 300 --dump-video sent.h264 --corrupt-every 50   # 自动化校验用
  python3 mock_sysdvr.py --h264 demo.h264 --drop-every 45 --error-every 150   # 模拟 Switch 端丢帧和视频错误包

生成测试视频（任选其一）：
  ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 20 -c:v libx264 -profile:v high -bf 0 -g 60 -an test.h264
  swift tools/make_test_h264.swift test.h264      # macOS 自带 VideoToolbox，无需 ffmpeg
"""
import argparse
import math
import os
import socket
import struct
import threading
import time
import zlib

PACKET_MAGIC = 0xCCCCCCCC
REQUEST_MAGIC = 0xAAAAAAAA
META_VIDEO, META_AUDIO = 1 << 0, 1 << 1
META_DATA, META_REPLAY, META_MULTINAL, META_ERROR = 1 << 2, 1 << 3, 1 << 4, 1 << 5
ABUF = 0x1000
VBUF = 0x54000

SPS = bytes([0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B, 0x40, 0x28, 0x02, 0xDD, 0x35, 0x01, 0x0D,
             0x01, 0xE0, 0x80])
PPS = bytes([0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0])

args = None
start_us = time.monotonic_ns() // 1000


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def now_us():
    return time.monotonic_ns() // 1000 - start_us


def header(size, ts, meta, slot=0xFF):
    return struct.pack('<IIQBB', PACKET_MAGIC, size, ts, meta, slot)


def recv_exact(sock, n):
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError('closed')
        buf += chunk
    return buf


# ---------------------------------------------------------------- 视频源

def split_nals(data):
    """把 Annex-B 字节流切成 NAL（保留起始码）。"""
    starts = []
    i, n = 0, len(data)
    while i + 3 <= n:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            s = i - 1 if i > 0 and data[i - 1] == 0 else i
            starts.append(s)
            i += 3
        else:
            i += 1
    return [data[s:e] for s, e in zip(starts, starts[1:] + [n])]


def nal_type(nal):
    i = 2 if nal[2] == 1 else 3
    return nal[i + 1] & 0x1F


def load_h264_frames(path):
    """每个 VCL NAL（slice）作为一个包，前面的 SPS/PPS/SEI 等非 VCL NAL 并入该包，和 Switch 一样一包一帧。"""
    with open(path, 'rb') as f:
        nals = split_nals(f.read())
    frames, pending = [], b''
    for nal in nals:
        t = nal_type(nal)
        if t in (1, 5):
            frames.append((t == 5, pending + nal))
            pending = b''
        elif t == 9:  # AUD 丢弃
            continue
        else:
            pending += nal
    log(f'载入 {path}: {len(frames)} 帧，其中 IDR {sum(1 for k, _ in frames if k)} 个')
    return frames


def synthetic_frames():
    """合成数据：每 30 帧一个“IDR”，IDR 内容固定（循环时会触发哈希重放），P 帧内容随帧号变化。"""
    idr = b'\x00\x00\x00\x01\x65' + bytes((i * 7) & 0xFF or 1 for i in range(40000))
    frames = []
    for i in range(300):
        if i % 30 == 0:
            frames.append((True, idr))
        else:
            size = 3000 + (i * 997) % 9000
            frames.append((False, b'\x00\x00\x00\x01\x41' + bytes(((i + j) * 13) & 0xFF or 1 for j in range(size))))
    return frames


# ---------------------------------------------------------------- 握手

def do_handshake(sock, channel):
    hello = f'SysDVR|{args.protocol}'.encode() + b'\x00'
    assert len(hello) == 10
    sock.sendall(hello)
    req = recv_exact(sock, 16)
    magic, ver, meta, vflags, batching, features = struct.unpack('<I2sBBBB', req[:10])

    code = 6
    if magic != REQUEST_MAGIC:
        code = 5
    elif ver != args.protocol.encode():
        code = 1
    elif not (meta & 3):
        code = 4
    elif channel == 'video' and meta & META_AUDIO or channel == 'audio' and meta & META_VIDEO:
        code = 7

    if args.protocol >= '03' and features & 2 and channel == 'video' and code == 6:
        # ExtraFeatureFlags_MemoryDiag：回填一组假的内存池数据（应用 / 小程序 / 系统 / 系统非安全，单位字节）
        mb = 1024 * 1024
        resp = struct.pack('<II8Q', code, 0, 3200 * mb, 1800 * mb, 512 * mb, 400 * mb, 64 * mb, 60 * mb,
                           32 * mb, 10 * mb)
        log(f'[{channel}] 回填内存报告')
    elif args.protocol >= '03':
        resp = struct.pack('<II', code, 0xFFFFFFFF) + b'\x00' * 64
    else:
        resp = struct.pack('<I', code)
    sock.sendall(resp)
    log(f'[{channel}] 握手 meta={meta:#x} video_flags={vflags:#x} batching={batching} features={features:#x} '
        f'-> {"OK" if code == 6 else "拒绝 %d" % code}')
    return code == 6, vflags, batching


# ---------------------------------------------------------------- 推流

def stream_video(sock, vflags):
    use_hash = bool(vflags & 1)
    inject = bool(vflags & 2)
    hash_only_idr = bool(vflags & 4)
    frames = load_h264_frames(args.h264) if args.h264 else synthetic_frames()
    table = [0] * 64
    client_cache = {}  # 模拟客户端重放表：槽位 -> 客户端实际收到的负载
    force_meta, idr_count = True, 0
    dump = open(args.dump_video, 'wb') if args.dump_video else None
    sent = 0
    frame_index = 0   # 非重放的帧计数，用于 --drop-every / --error-every
    dropped = errors = 0
    frame_interval = 1.0 / args.fps
    next_t = time.monotonic()

    try:
        while args.video_packets == 0 or sent < args.video_packets:
            for is_idr, data in frames:
                if args.video_packets and sent >= args.video_packets:
                    break
                ts = now_us()
                frame_index += 1
                if args.drop_every and not is_idr and frame_index % args.drop_every == 0:
                    # 模拟 Switch 端网络阻塞时 grc 丢帧：这一帧不发，时间照常往前走（客户端看到时间戳断档）
                    dropped += 1
                    next_t = pace(next_t, frame_interval)
                    continue
                if args.error_every and not is_idr and frame_index % args.error_every == 0:
                    # 模拟关键帧超过 VbufSz 读取失败：sysmodule 用一个视频错误包顶替这一帧（ErrorPacket 32 字节）
                    body = struct.pack('<IIQQQ', 1, 0xCD4, len(data), 0, 0)
                    send_packet(sock, header(len(body), ts, META_VIDEO | META_ERROR) + body, sent)
                    errors += 1
                    sent += 1
                    next_t = pace(next_t, frame_interval)
                    continue
                slot = 0xFF
                if use_hash and (is_idr or not hash_only_idr):
                    crc = zlib.crc32(data)
                    slot = crc & 63
                    if table[slot] == crc:
                        pkt = header(0, ts, META_VIDEO | META_REPLAY, slot)
                        if dump:
                            dump.write(client_cache[slot])
                        send_packet(sock, pkt, sent)
                        sent += 1
                        next_t = pace(next_t, frame_interval)
                        continue
                    table[slot] = crc

                meta = META_VIDEO | META_DATA
                payload = data
                if inject:
                    if is_idr:
                        idr_count += 1
                    if (force_meta or (is_idr and idr_count >= 5)) and VBUF - len(data) >= len(SPS) + len(PPS):
                        payload = SPS + PPS + data
                        meta |= META_MULTINAL
                        force_meta, idr_count = False, 0
                if len(payload) > VBUF:
                    log(f'跳过超大帧 {len(payload)} 字节（超过 VbufSz）')
                    continue
                if slot != 0xFF:
                    client_cache[slot] = payload
                if dump:
                    dump.write(payload)
                send_packet(sock, header(len(payload), ts, meta, slot) + payload, sent)
                sent += 1
                next_t = pace(next_t, frame_interval)
    finally:
        if dump:
            dump.close()
    if args.drop_every or args.error_every:
        log(f'[video] 模拟丢帧 {dropped} 帧，错误包 {errors} 个')
    log(f'[video] 已发送 {sent} 包，结束')


def send_packet(sock, pkt, index):
    if args.corrupt_every and index and index % args.corrupt_every == 0:
        # 在两个包之间插入 25 字节垃圾（全 0，不会被误认为包头），测试客户端重同步
        sock.sendall(b'\x00' * 25)
    sock.sendall(pkt)


def pace(next_t, interval):
    next_t += interval
    delay = next_t - time.monotonic()
    if delay > 0:
        time.sleep(delay)
    else:
        next_t = time.monotonic()
    return next_t


def stream_audio(sock, batching):
    chunk_samples = ABUF // 4
    per_packet = 1 + batching
    interval = chunk_samples * per_packet / 48000.0
    phase = 0
    sent = 0
    next_t = time.monotonic()
    while args.audio_packets == 0 or sent < args.audio_packets:
        samples = bytearray()
        if args.silent_audio:
            samples = bytearray(chunk_samples * per_packet * 4)
        else:
            for _ in range(chunk_samples * per_packet):
                l = int(8000 * math.sin(2 * math.pi * 440 * phase / 48000))
                r = int(8000 * math.sin(2 * math.pi * 660 * phase / 48000))
                samples += struct.pack('<hh', l, r)
                phase += 1
        sock.sendall(header(len(samples), now_us(), META_AUDIO | META_DATA) + bytes(samples))
        sent += 1
        next_t = pace(next_t, interval)
    log(f'[audio] 已发送 {sent} 包，结束')


def serve(port, channel):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', port))
    srv.listen(1)
    log(f'[{channel}] 监听 TCP {port}')
    while True:
        conn, addr = srv.accept()
        log(f'[{channel}] 客户端连接 {addr[0]}:{addr[1]}')
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            ok, vflags, batching = do_handshake(conn, channel)
            if ok:
                time.sleep(0.5)  # sysmodule 握手后同样会等 500ms
                if channel == 'video':
                    stream_video(conn, vflags)
                else:
                    stream_audio(conn, batching)
        except (ConnectionError, BrokenPipeError, OSError) as e:
            log(f'[{channel}] 连接结束：{e}')
        finally:
            conn.close()
        if args.once:
            return


def beacon():
    msg = f'SysDVR|6.3|{args.protocol}|{args.serial}'.encode() + b'\x00'
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    targets = [t for t in args.beacon.split(',') if t]
    log(f'UDP 广播目标：{targets}')
    while True:
        for t in targets:
            try:
                s.sendto(msg, (t, 19999))
            except OSError as e:
                log(f'广播到 {t} 失败：{e}')
        time.sleep(2)


def main():
    global args
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--h264', help='Annex-B H.264 文件；不指定则发送合成数据')
    p.add_argument('--fps', type=float, default=30)
    p.add_argument('--protocol', default='03', choices=['02', '03'])
    p.add_argument('--serial', default='XAW00000000000')
    p.add_argument('--beacon', default='255.255.255.255,127.0.0.1', help='逗号分隔的广播/单播目标，空字符串关闭')
    p.add_argument('--video-packets', type=int, default=0, help='发送多少个视频包后断开（0 = 无限）')
    p.add_argument('--audio-packets', type=int, default=0)
    p.add_argument('--silent-audio', action='store_true', help='音频发静音（录演示视频、截图时不出测试音）')
    p.add_argument('--dump-video', help='把“客户端应该还原出的”视频字节流写到文件，用于和客户端输出比对')
    p.add_argument('--corrupt-every', type=int, default=0, help='每 N 个视频包插入一段垃圾数据')
    p.add_argument('--drop-every', type=int, default=0, help='每 N 帧丢一帧非关键帧（模拟 Switch 端丢帧，时间戳断档）')
    p.add_argument('--error-every', type=int, default=0, help='每 N 帧用一个视频错误包顶替一帧非关键帧')
    p.add_argument('--once', action='store_true', help='每个端口只服务一个连接后退出')
    args = p.parse_args()

    threads = [threading.Thread(target=serve, args=(9911, 'video'), daemon=True),
               threading.Thread(target=serve, args=(9922, 'audio'), daemon=True)]
    if args.beacon:
        threading.Thread(target=beacon, daemon=True).start()
    for t in threads:
        t.start()
    try:
        for t in threads:
            t.join()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
