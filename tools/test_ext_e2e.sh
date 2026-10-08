#!/bin/sh
# 实验扩展端到端（docs/nsdvr-ext-protocol.md）：另一端的 sysdvr_hostmock（与 sysmodule 同一份扩展代码）模拟 Switch，
# sysdvr_cli 用 core/ 代码接收。覆盖：扩展握手、各编码解码（PCM 无损、有损编码与原始 WAV 对齐比较）、运行中切换
# （声音不断）、诊断包、发送卡顿与源头丢帧两种断档的区分、Opus 不可用时的回落、不请求扩展时与官方一致。
# 用法：bash tools/test_ext_e2e.sh [hostmock 路径]（test_e2e.sh 在 hostmock 就绪时会自动调用）
set -e
cd "$(dirname "$0")/.."
HOSTMOCK=${1:-$(cd "$(dirname "$0")/.." && pwd)/../NSDVR-server/host/build/sysdvr_hostmock}
WAV=audio_eval/src/aliens_48k.wav
H264=$(dirname "$HOSTMOCK")/demo.h264
[ -x "$HOSTMOCK" ] || { echo "✗ 找不到 hostmock：$HOSTMOCK"; exit 1; }
[ -f "$WAV" ] || { echo "✗ 缺少 $WAV"; exit 1; }
[ -x build/sysdvr_cli ] || sh tools/build_cli.sh >/dev/null
VIDEO_ARGS=""
[ -f "$H264" ] && VIDEO_ARGS="--h264 $H264"
OUT=$(mktemp -d)
HM=""
trap 'rm -rf "$OUT"; [ -n "$HM" ] && kill $HM 2>/dev/null || true' EXIT
fail() { echo "✗ $1"; exit 1; }

# 起一个 hostmock（额外参数透传），等端口就绪
start_mock() {
    "$HOSTMOCK" $VIDEO_ARGS --wav "$WAV" --no-beacon --quiet-diag "$@" > "$OUT/hm.log" 2>&1 &
    HM=$!
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        grep -q 'listening on .*9922' "$OUT/hm.log" 2>/dev/null && return 0
        sleep 0.2
    done
    cat "$OUT/hm.log"
    fail "hostmock 没有启动"
}
stop_mock() {
    kill $HM 2>/dev/null || true
    wait $HM 2>/dev/null || true
    HM=""
}

# 解码后的音频与 WAV 比较：PCM 要求逐字节相同；有损编码在 0–400 帧延迟里找最佳对齐，报 SNR
check_audio() {  # $1=pcm 文件 $2=说明 $3=最低 SNR（dB，"exact" 表示逐字节）
    python3 - "$1" "$WAV" "$2" "$3" <<'PY'
import math, struct, sys, wave
pcm = open(sys.argv[1], 'rb').read()
w = wave.open(sys.argv[2]); wav = w.readframes(w.getnframes())
name, need = sys.argv[3], sys.argv[4]
frames = len(pcm) // 4
assert frames > 48000 * 2, f'{name}：解码后的音频太短（{frames} 帧）'
if need == 'exact':
    assert wav[:len(pcm)] == pcm, f'{name}：PCM 与 WAV 不一致'
    print(f'{name}：{frames} 帧与 WAV 逐字节一致')
    sys.exit(0)
# 取第 1.5 秒起 0.2 秒（mid 声道）找对齐
def mid(buf, start, n):
    v = struct.unpack_from(f'<{2 * n}h', buf, start * 4)
    return [(v[2 * i] + v[2 * i + 1]) / 2 for i in range(n)]
s0, n = 72000, 9600
dec = mid(pcm, s0, n)
best = (-99, 0)
for d in range(0, 401):
    ref = mid(wav, s0 - d, n)
    sig = sum(x * x for x in ref)
    err = sum((a - b) ** 2 for a, b in zip(ref, dec))
    snr = 10 * math.log10((sig + 1e-9) / (err + 1e-9))
    if snr > best[0]:
        best = (snr, d)
print(f'{name}：{frames} 帧，与 WAV 对齐延迟 {best[1]} 帧，SNR {best[0]:.1f} dB')
assert best[0] >= float(need), f'{name}：SNR {best[0]:.1f} dB 低于 {need}'
PY
}

# 每秒的“输出 N 帧/s”（解码后交给播放器的 48 kHz 帧数）不能断：从第 from 秒起都要 ≥ min
check_continuous() {  # $1=cli 日志 $2=起始秒 $3=说明
    python3 - "$1" "$2" "$3" <<'PY'
import re, sys
rows = [int(m.group(1)) for m in re.finditer(r'输出 (\d+) 帧/s', open(sys.argv[1], encoding='utf-8').read())]
start, name = int(sys.argv[2]), sys.argv[3]
tail = rows[start - 1:]
assert tail and min(tail) >= 40000, f'{name}：声音中断，每秒输出帧数 {rows}'
print(f'{name}：每秒输出 {min(tail)}–{max(tail)} 帧')
PY
}

# ---------------------------------------------------------------- 1. 各编码
for spec in "pcm:PCM:exact" "24k:PCM 24 kHz:30" "adpcm:ADPCM:20" "opus/96/5/20:Opus 96 kbps 复杂度 5 20 ms:8" \
            "opus/64/0/10:Opus 64 kbps 复杂度 0 10 ms:6" "opus/192/10/20:Opus 192 kbps 复杂度 10 20 ms:10"; do
    codec=${spec%%:*}; rest=${spec#*:}; label=${rest%:*}; need=${rest##*:}
    start_mock
    ./build/sysdvr_cli 127.0.0.1 --seconds 5 --codec "$codec" --audio "$OUT/a.pcm" > "$OUT/cli.log" 2>&1
    stop_mock
    grep -q "扩展：服务端支持 · 最后音频编码 ${label}" "$OUT/cli.log" || { cat "$OUT/cli.log"; fail "${codec}：编码没有生效"; }
    grep -q '解码错误 0 ·' "$OUT/cli.log" || fail "${codec}：有解码错误"
    grep -q '\[audio\] handshake OK .*NSDVR ext' "$OUT/hm.log" || fail "${codec}：音频握手没带扩展"
    check_continuous "$OUT/cli.log" 2 "${codec}" > "$OUT/cont.txt" || { cat "$OUT/cont.txt"; fail "${codec}：声音不连续"; }
    check_audio "$OUT/a.pcm" "${codec}" "${need}" > "$OUT/snr.txt" || { cat "$OUT/snr.txt"; fail "${codec}：解码结果不对"; }
    echo "  ✓ $(cat "$OUT/snr.txt")；$(sed 's/^[^：]*：//' "$OUT/cont.txt")"
done
echo "✓ 扩展握手与各编码解码（初始编码由握手 Reserved 字节指定）"

# ---------------------------------------------------------------- 2. 运行中切换：控制消息生效、声音不断
start_mock
./build/sysdvr_cli 127.0.0.1 --seconds 20 --codec pcm \
    --switch "3:24k,6:adpcm,9:opus/96/5/20,12:opus/32/3/10,15:opus/128/8/20,17:pcm" > "$OUT/cli.log" 2>&1
stop_mock
python3 - "$OUT/cli.log" "$OUT/hm.log" <<'PY' || fail "运行中切换"
import re, sys
cli = open(sys.argv[1], encoding='utf-8').read()
hm = open(sys.argv[2], encoding='utf-8').read()
sent = re.findall(r'切换音频编码 → .*：已发控制消息', cli)
assert len(sent) == 6, ('控制消息没有全部发出', sent)
seq = re.findall(r'扩展 音频 (.+?)(（切换中）)? · 输出 (\d+) 帧/s', cli)
codecs = []
for name, pending, frames in seq:
    if not codecs or codecs[-1] != name:
        codecs.append(name)
want = ['PCM 1536 kbps', 'PCM 24 kHz 768 kbps', 'ADPCM 384 kbps', 'Opus 96 kbps 复杂度 5 20 ms',
        'Opus 32 kbps 复杂度 3 10 ms', 'Opus 128 kbps 复杂度 8 20 ms', 'PCM 1536 kbps']
assert codecs == want, ('切换顺序不对', codecs)
pend = sum(1 for _, p, _ in seq if p)
assert pend <= 6, ('“切换中”持续太久', pend)
frames = [int(f) for _, _, f in seq]
assert min(frames[1:]) >= 40000, ('切换时声音中断', frames)
switches = re.findall(r'now sending (\S+)', hm)
assert len(switches) >= 7, ('hostmock 没看到全部切换', switches)
print(f'  7 种设置依次生效（{" → ".join(codecs)}），每秒输出 {min(frames[1:])}–{max(frames)} 帧，“切换中”共 {pend} 秒')
PY
grep -q '解码错误 0 ·' "$OUT/cli.log" || fail "运行中切换：有解码错误"
echo "✓ 运行中切换编码/Opus 码率/复杂度/帧长：8 字节控制消息，不重连，包标记确认生效，声音不断"

# ---------------------------------------------------------------- 3. 诊断包：正常、发送卡顿、源头丢帧
diag_run() {  # $1=说明，其余参数给 hostmock
    name=$1; shift
    start_mock "$@"
    ./build/sysdvr_cli 127.0.0.1 --seconds 12 --codec opus/96/5/20 > "$OUT/cli.log" 2>&1
    stop_mock
}
summary() {
    python3 - "$OUT/cli.log" "$@" <<'PY'
import re, sys
cli = open(sys.argv[1], encoding='utf-8').read()
m = re.search(r'诊断包 (\d+) 个（累计 ([\d.]+) s，发送 (\d+) 帧，阻塞合计 ([\d.]+) ms、最长 ([\d.]+) ms，超 20 ms (\d+) 次，'
              r'grc 断档 (\d+)，其中发送慢 (\d+)）', cli)
assert m, '没有诊断汇总'
n, secs, frames, block, peak, over, gaps, slow = m.groups()
loss = re.search(r'丢帧检测：断档 (\d+) 次', cli).group(1)
tos = re.findall(r'TOS (\S+)', cli)
print(n, secs, frames, block, peak, over, gaps, slow, loss, tos[-1] if tos else '-')
PY
}
diag_run "正常"
read n secs frames block peak over gaps slow loss tos <<EOF
$(summary)
EOF
[ "$n" -ge 9 ] && [ "$over" -eq 0 ] && [ "$gaps" -eq 0 ] && [ "$tos" = "视频✓音频✓" ] \
    || fail "正常情况下的诊断：${n} 个 超20ms=${over} 断档=${gaps} TOS=${tos}"
grep -q 'core3\|核3空闲' "$OUT/cli.log" || fail "诊断里没有核心空闲"
echo "  ✓ 正常：诊断包 ${n} 个（${secs} s）、发送 ${frames} 帧、阻塞合计 ${block} ms、超 20 ms ${over} 次、grc 断档 ${gaps}、TOS ${tos}"

diag_run "发送卡顿" --stall-every 2 --stall-ms 300
read n secs frames block peak over gaps slow loss tos <<EOF
$(summary)
EOF
python3 -c "import sys; sys.exit(0 if float('${peak}') >= 250 else 1)" || fail "卡顿：单次阻塞峰值 ${peak} ms"
[ "$over" -ge 3 ] && [ "$gaps" -ge 3 ] && [ "$slow" -ge 3 ] && [ "$loss" -ge 3 ] \
    || fail "卡顿：超20ms=${over} 断档=${gaps} 其中发送慢=${slow} 客户端断档=${loss}"
echo "  ✓ 每 2 秒卡 300 ms：单次阻塞峰值 ${peak} ms、超 20 ms ${over} 次、grc 断档 ${gaps}（其中发送慢 ${slow}）、客户端检测到断档 ${loss} 次"

diag_run "源头丢帧" --drop-every 25
read n secs frames block peak over gaps slow loss tos <<EOF
$(summary)
EOF
[ "$gaps" -ge 5 ] && [ "$slow" -eq 0 ] && [ "$over" -eq 0 ] && [ "$loss" -ge 5 ] \
    || fail "源头丢帧：断档=${gaps} 其中发送慢=${slow} 超20ms=${over} 客户端断档=${loss}"
echo "  ✓ 源头每 25 帧丢 1 帧：grc 断档 ${gaps}、其中发送慢 ${slow}（与卡顿区分开）、客户端检测到断档 ${loss} 次"
echo "✓ 诊断包：发送阻塞/峰值/超 20 ms/grc 断档/断档前发送慢/核心空闲/CPU/TOS 进入统计"

# ---------------------------------------------------------------- 4. Opus 不可用时回落 PCM48；不请求扩展时与官方一致
start_mock --no-opus
./build/sysdvr_cli 127.0.0.1 --seconds 5 --codec opus/96/5/20 --audio "$OUT/a.pcm" > "$OUT/cli.log" 2>&1
stop_mock
grep -q '扩展：服务端支持 · 最后音频编码 PCM 1536 kbps · 解码错误 0' "$OUT/cli.log" || fail "Opus 不可用：应回落 PCM48"
grep -q '（切换中）' "$OUT/cli.log" || fail "Opus 不可用：请求的编码没生效时应显示“切换中”"
check_audio "$OUT/a.pcm" "no-opus" exact > "$OUT/snr.txt" || { cat "$OUT/snr.txt"; fail "Opus 不可用：回落后的 PCM 不对"; }
echo "✓ 服务端 Opus 不可用（--no-opus）：回落 PCM48，声音无损，界面显示请求未生效"

start_mock
./build/sysdvr_cli 127.0.0.1 --seconds 5 --no-ext --audio "$OUT/a.pcm" > "$OUT/cli.log" 2>&1
stop_mock
grep -q '扩展：未请求' "$OUT/cli.log" || fail "--no-ext：客户端不应请求扩展"
! grep -q 'NSDVR ext' "$OUT/hm.log" || fail "--no-ext：握手里不应有扩展"
! grep -q '扩展 音频' "$OUT/cli.log" || fail "--no-ext：不应有扩展统计"
check_audio "$OUT/a.pcm" "legacy" exact > "$OUT/snr.txt" || { cat "$OUT/snr.txt"; fail "--no-ext：音频不对"; }
echo "✓ 不请求扩展：扩展版服务端走官方流程（原始 PCM、无诊断包），与官方客户端行为一致"
echo "实验扩展端到端全部通过"
