#!/bin/sh
# 端到端协议自测：mock_sysdvr.py 模拟 Switch，sysdvr_cli 用 core/ 代码接收，逐字节比对。
# 覆盖：UDP 发现、双通道握手（协议 03 和 02）、SPS/PPS 注入、NAL 哈希重放、包间垃圾数据重同步、音频 PCM 完整性，
# 以及后台恢复用的 GOP 缓存（单测 + 快照与实际码流比对）。
# 实验扩展（docs/nsdvr-ext-protocol.md）：core 单测；CLI 默认请求扩展，连官方行为的 mock 时必须一切如旧；
# 另一端的 hostmock 就绪时再跑 tools/test_ext_e2e.sh（没就绪就跳过）。
set -e
cd "$(dirname "$0")/.."
sh tools/build_cli.sh >/dev/null
./build/gop_cache_test > /dev/null || { ./build/gop_cache_test; echo "✗ GOP 缓存单测失败"; exit 1; }
echo "✓ GOP 缓存单测"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"; kill $MOCK 2>/dev/null || true' EXIT
fail() { echo "✗ $1"; exit 1; }

./build/frame_loss_test > "$OUT/loss.log" || { cat "$OUT/loss.log"; echo "✗ 丢帧检测单测失败"; exit 1; }
echo "✓ 丢帧检测单测（抖动不误报、丢帧计数、60fps 自适应、时间戳回退）"

./build/frame_pacer_test > "$OUT/pacer.log" || { cat "$OUT/pacer.log"; echo "✗ 平滑模式模拟测试失败"; exit 1; }
echo "✓ 平滑模式模拟测试（热点/一般 Wi-Fi/稳定网络三种场景）"

./build/i18n_test > "$OUT/i18n.log" || { cat "$OUT/i18n.log"; echo "✗ native 多语言单测失败"; exit 1; }
python3 tools/i18n_native_check.py > "$OUT/i18n_check.log" || { cat "$OUT/i18n_check.log"; echo "✗ native 多语言占位符检查失败"; exit 1; }
echo "✓ native 多语言（$(tail -1 "$OUT/i18n_check.log")）"

./build/usb_stream_test > "$OUT/usb.log" 2>&1 || { cat "$OUT/usb.log"; echo "✗ USB 串流测试失败"; exit 1; }
echo "✓ USB 串流测试（握手 / 粘包拆包 / 重放 / 中途重连 / 协议 02 / 拔线）"

./build/ext_audio_test > "$OUT/ext.log" 2>&1 || { cat "$OUT/ext.log"; echo "✗ 实验扩展单测失败"; exit 1; }
echo "✓ 实验扩展单测（$(tail -1 "$OUT/ext.log")：协议字节 / PCM24 / ADPCM / Opus / 切换 / 异常输入 / 诊断包）"

run_case() {  # $1=协议版本 $2=说明
    python3 tools/mock_sysdvr.py --once --protocol "$1" --video-packets 400 --audio-packets 60 \
        --dump-video "$OUT/sent.h264" --corrupt-every 50 --beacon 127.0.0.1 > "$OUT/mock.log" 2>&1 &
    MOCK=$!
    sleep 1.5
    ./build/sysdvr_cli scan 3 > "$OUT/scan.log" 2>&1 || fail "协议 $1：UDP 发现失败"
    grep -q "协议 $1" "$OUT/scan.log" || fail "协议 $1：广播内容解析不对"
    ./build/sysdvr_cli 127.0.0.1 --seconds 16 --video "$OUT/recv.h264" --audio "$OUT/recv.pcm" --gop "$OUT/gop.h264" > "$OUT/cli.log" 2>&1
    wait $MOCK 2>/dev/null || true
    cmp -s "$OUT/sent.h264" "$OUT/recv.h264" || fail "协议 $1：视频字节流不一致"
    grep -q '重放命中 1[0-9]' "$OUT/cli.log" || fail "协议 $1：NAL 重放没有触发"
    grep -q '重同步 7' "$OUT/cli.log" || fail "协议 $1：重同步次数不对"
    # 内存报告：协议 03 解析出 mock 回填的数据（系统池 64 MB 用了 60 MB），协议 02 没有这些字段
    if [ "$1" = "03" ]; then
        grep -q 'Switch 内存：系统池剩余 4.0 MB / 共 64.0 MB · 小程序池剩余 112.0 / 512.0 MB · 应用池剩余 1400.0 / 3200.0 MB' \
            "$OUT/cli.log" || fail "协议 03：内存报告解析不对"
        grep -q '\[video\] 回填内存报告' "$OUT/mock.log" || fail "协议 03：视频握手没有请求内存报告"
        ! grep -q '\[audio\] 回填内存报告' "$OUT/mock.log" || fail "协议 03：音频握手不应请求内存报告"
        # 新客户端 + 官方服务端：握手带扩展位（视频 0x2|0x4、音频 0x4），官方忽略；没有扩展标记和诊断包，按 PCM 处理
        grep -q '\[video\] 握手 .* features=0x6' "$OUT/mock.log" || fail "协议 03：视频握手没带扩展位"
        grep -q '\[audio\] 握手 .* features=0x4' "$OUT/mock.log" || fail "协议 03：音频握手没带扩展位"
    else
        grep -q 'Switch 内存：未提供' "$OUT/cli.log" || fail "协议 02：不应有内存报告"
        # 扩展只在协议 03 上请求
        ! grep -q 'features=0x[4-7]' "$OUT/mock.log" || fail "协议 02：不应请求扩展"
    fi
    grep -q '扩展：服务端不支持（按官方 PCM 处理）' "$OUT/cli.log" || fail "协议 $1：官方服务端被误认成扩展版"
    ! grep -q '扩展 音频' "$OUT/cli.log" || fail "协议 $1：官方服务端下不应出现扩展统计"
    python3 - "$OUT/recv.h264" "$OUT/gop.h264" <<'PY' || fail "协议 $1：GOP 快照与码流不一致"
import sys
recv, gop = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
last_idr = max(recv.rfind(b'\x00\x00\x00\x01\x65'), recv.rfind(b'\x00\x00\x00\x01\x25'))
tail = recv[last_idr:]
# 快照 = 最近一个 IDR 起的全部数据；IDR 自带或被补上 SPS/PPS，所以快照以 SPS 开头、以码流尾部结尾
assert gop.startswith(b'\x00\x00\x00\x01\x67') or gop.startswith(b'\x00\x00\x00\x01\x27'), gop[:8]
assert gop.endswith(tail) and len(gop) - len(tail) < 64, (len(gop), len(tail))
PY
    python3 - "$OUT/recv.pcm" <<'PY' || fail "协议 $1：音频数据不一致"
import math, struct, sys
d = open(sys.argv[1], 'rb').read()
assert len(d) == 60 * 4 * 0x1000, len(d)
for i in range(0, len(d) // 4, 997):
    l, r = struct.unpack_from('<hh', d, i * 4)
    assert l == int(8000 * math.sin(2 * math.pi * 440 * i / 48000)), i
    assert r == int(8000 * math.sin(2 * math.pi * 660 * i / 48000)), i
PY
    echo "✓ 协议 $1：$2"
}

run_case 03 "发现 / 握手 / 400 视频包逐字节一致（含重放与 7 次重同步）/ GOP 快照 / 音频完整 / 内存报告"
run_case 02 "旧版 4 字节握手应答同样通过（无内存报告）"

# 播放中开关音频：第 4 秒关、第 8 秒开，视频全程不能断
python3 tools/mock_sysdvr.py --beacon "" > "$OUT/mock.log" 2>&1 &
MOCK=$!
sleep 1.5
./build/sysdvr_cli 127.0.0.1 --seconds 12 --toggle-audio 4 > "$OUT/toggle.log" 2>&1
kill $MOCK 2>/dev/null; wait $MOCK 2>/dev/null || true
python3 - "$OUT/toggle.log" <<'PY' || fail "播放中开关音频"
import re, sys
rows = {}
for line in open(sys.argv[1], encoding='utf-8'):
    m = re.match(r'\[\s*(\d+)s\] 视频 . +(\d+) 包/s .*音频 . +(\d+) 包/s', line)
    if m: rows[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
assert all(rows[t][0] >= 25 for t in range(2, 13)), ('视频中断', rows)
assert all(rows[t][1] == 0 for t in (6, 7, 8)), ('关闭后仍有音频', rows)
assert rows[3][1] > 0 and rows[11][1] > 0, ('音频没有恢复', rows)
PY
echo "✓ 播放中开关音频：关闭期间 Switch 停发音频，视频全程不中断"

# 丢帧检测：mock 模拟 Switch 端丢帧（时间戳断档）和视频错误包，客户端要逐次发现，且没有误报
python3 tools/mock_sysdvr.py --once --video-packets 300 --audio-packets 30 --drop-every 40 --error-every 97 \
    --beacon "" > "$OUT/mock_loss.log" 2>&1 &
MOCK=$!
sleep 1.5
./build/sysdvr_cli 127.0.0.1 --seconds 13 > "$OUT/cli_loss.log" 2>&1
wait $MOCK 2>/dev/null || true
python3 - "$OUT/mock_loss.log" "$OUT/cli_loss.log" <<'PY' || fail "丢帧检测"
import re, sys
mock = open(sys.argv[1], encoding='utf-8').read()
cli = open(sys.argv[2], encoding='utf-8').read()
dropped, errors = map(int, re.search(r'模拟丢帧 (\d+) 帧，错误包 (\d+) 个', mock).groups())
m = re.search(r'丢帧检测：断档 (\d+) 次（估计 (\d+) 帧）· 错误包 (\d+) · 缓存未命中 (\d+) · 失步 (\d+)', cli)
gaps, lost, errs, misses, resyncs = map(int, m.groups())
assert dropped >= 5 and errors >= 2, ('mock 没按预期丢帧', dropped, errors)
# 错误包顶替的帧按错误包计次，随后的断档不重复计；Mac 调度偶尔慢一下最多多报一两次
assert dropped <= gaps <= dropped + 2, ('断档次数不对', dropped, gaps)
# 估计丢帧数包括被错误包顶替的帧
assert dropped + errors <= lost <= dropped + errors + 3, ('估计丢帧数不对', dropped + errors, lost)
assert errs == errors, ('错误包次数不对', errors, errs)
assert misses == 0 and resyncs == 0, ('误报', misses, resyncs)
print(f'丢帧 {dropped} → 断档 {gaps}（估计 {lost} 帧），错误包 {errors} → {errs}')
PY
echo "✓ 丢帧检测：模拟 Switch 端丢帧与视频错误包，逐次识别、无误报"

# 不请求扩展（--no-ext）：握手与旧版逐字节相同（mock 看到的 features 只有内存报告位）
python3 tools/mock_sysdvr.py --once --video-packets 60 --audio-packets 10 --beacon "" > "$OUT/mock_noext.log" 2>&1 &
MOCK=$!
sleep 1.5
./build/sysdvr_cli 127.0.0.1 --seconds 4 --no-ext > "$OUT/cli_noext.log" 2>&1
wait $MOCK 2>/dev/null || true
grep -q '\[video\] 握手 .* features=0x2' "$OUT/mock_noext.log" && grep -q '\[audio\] 握手 .* features=0x0' "$OUT/mock_noext.log" \
    && grep -q '扩展：未请求' "$OUT/cli_noext.log" || fail "--no-ext 时握手应与旧版相同"
echo "✓ 实验扩展兼容性：连官方行为的 mock 按 PCM 处理、不显示扩展数据；--no-ext 握手与旧版相同"

HOSTMOCK=${HOSTMOCK:-$(cd "$(dirname "$0")/.." && pwd)/../NSDVR-server/host/build/sysdvr_hostmock}
if [ -x "$HOSTMOCK" ] && [ -f "$(dirname "$(dirname "$HOSTMOCK")")/READY" ]; then
    bash tools/test_ext_e2e.sh "$HOSTMOCK" || fail "实验扩展端到端（hostmock）"
else
    echo "• 跳过实验扩展端到端：hostmock 还没就绪（${HOSTMOCK}）"
fi
echo "全部通过"
