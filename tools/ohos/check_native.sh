#!/bin/sh
# 不开 DevEco，用 OpenHarmony SDK 的 CMake + clang 交叉编译 harmony 端 native 库（arm64），检查接口与链接。
# 用法：OHOS_NATIVE=<sdk>/native sh tools/ohos/check_native.sh
# SDK 可从 https://repo.huaweicloud.com/openharmony/os/5.0.0-Release/ 下载（API 12，与工程 compatibleSdkVersion 一致）
set -e
: "${OHOS_NATIVE:?请设置 OHOS_NATIVE 为 SDK 的 native 目录}"
cd "$(dirname "$0")/../.."
B=build/ohos-native
rm -rf "$B"
"$OHOS_NATIVE/build-tools/cmake/bin/cmake" -S harmony/entry/src/main/cpp -B "$B" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$OHOS_NATIVE/build-tools/cmake/bin/ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$OHOS_NATIVE/build/cmake/ohos.toolchain.cmake" \
    -DOHOS_ARCH=arm64-v8a -DOHOS_PLATFORM=OHOS -DOHOS_STL=c++_shared -DCMAKE_BUILD_TYPE=Release >/dev/null
"$OHOS_NATIVE/build-tools/cmake/bin/cmake" --build "$B" 2>&1 | grep -v 'unused-command-line-argument' || true
test -f "$B/libentry.so" && echo "✓ 已生成 $B/libentry.so" && "$OHOS_NATIVE/llvm/bin/llvm-readelf" -d "$B/libentry.so" | grep NEEDED
