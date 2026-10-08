#!/bin/sh
# 编译桌面测试工具 build/sysdvr_cli（macOS / Linux，只依赖系统 clang++ 或 g++）
set -e
cd "$(dirname "$0")/.."
mkdir -p build
CXX=${CXX:-c++}
CC=${CC:-cc}

# libopus 解码部分：与鸿蒙端（third_party/opus/CMakeLists.txt）同样的源文件和宏，编成 build/libopus_dec.a；
# 只在库不存在或源码更新过时重编
OPUS_LIB=build/libopus_dec.a
if [ ! -f "$OPUS_LIB" ] || [ -n "$(find third_party/opus -newer "$OPUS_LIB" -name '*.[ch]' | head -1)" ]; then
    rm -rf build/opus_obj && mkdir -p build/opus_obj
    find third_party/opus/celt third_party/opus/silk third_party/opus/src -name '*.c' -print0 |
        xargs -0 -n 8 -P 8 sh -c 'for f; do "$0" -O2 -w -DOPUS_BUILD -DFIXED_POINT -DDISABLE_FLOAT_API -DVAR_ARRAYS \
            -DPACKAGE_VERSION=\"1.6.1\" -Ithird_party/opus/include -Ithird_party/opus/celt -Ithird_party/opus/silk \
            -Ithird_party/opus/silk/fixed -c "$f" -o "build/opus_obj/$(echo "$f" | tr / _).o" || exit 255; done' "$CC"
    rm -f "$OPUS_LIB"
    ar rcs "$OPUS_LIB" build/opus_obj/*.o
fi
# 实验扩展的音频解码（core/ext_audio.cpp）带 Opus
EXT="-DSYSDVR_WITH_OPUS -Ithird_party/opus/include core/ext_audio.cpp"

$CXX -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow \
    core/sysdvr_protocol.cpp core/log.cpp core/i18n.cpp core/tcp_bridge.cpp core/stream_source.cpp core/gop_cache.cpp \
    core/frame_pacer.cpp core/frame_loss.cpp $EXT tools/cli_client.cpp \
    -o build/sysdvr_cli -lpthread "$OPUS_LIB"
$CXX -std=c++17 -O2 -Wall -Wextra -Wpedantic core/sysdvr_protocol.cpp core/i18n.cpp core/gop_cache.cpp tools/gop_cache_test.cpp \
    -o build/gop_cache_test
$CXX -std=c++17 -O2 -Wall -Wextra core/frame_pacer.cpp tools/frame_pacer_test.cpp -o build/frame_pacer_test
$CXX -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow core/frame_loss.cpp tools/frame_loss_test.cpp -o build/frame_loss_test
$CXX -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow \
    core/sysdvr_protocol.cpp core/log.cpp core/i18n.cpp core/stream_source.cpp core/frame_loss.cpp core/usb_stream.cpp $EXT \
    tools/usb_stream_test.cpp -o build/usb_stream_test -lpthread "$OPUS_LIB"
$CXX -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow \
    core/sysdvr_protocol.cpp core/i18n.cpp tools/i18n_test.cpp -o build/i18n_test -lpthread
# 实验扩展单测：带地址/未定义行为检查，异常输入测试才有意义
$CXX -std=c++17 -O1 -g -Wall -Wextra -Wshadow -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    core/sysdvr_protocol.cpp core/log.cpp core/i18n.cpp core/stream_source.cpp core/frame_loss.cpp $EXT \
    tools/ext_audio_test.cpp -o build/ext_audio_test -lpthread -ldl "$OPUS_LIB"
echo "已生成 build/sysdvr_cli、build/gop_cache_test、build/frame_pacer_test、build/frame_loss_test、build/usb_stream_test、build/i18n_test、build/ext_audio_test"
