# libopus 1.6.1 的解码部分（BSD-3-Clause，版权与许可见同目录 COPYING / AUTHORS），给 NSDVR 实验扩展的
# Opus 音频解码用（core/ext_audio.cpp）。
#   来源：https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
#   sha256：6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1（与官方 SHA256SUMS.txt 一致）
# 只拷了 include/、celt/、silk/（含 fixed/）的源文件和 src/ 里的 opus.c、opus_decoder.c、extensions.c 及其头文件，
# 没有 dnn/、测试、工具；文件内容未改动。
# 版本与服务端（NSDVR-server，同样是 1.6.1 定点）一致，解码结果可以和 tools/ext_vectors 的参考解码逐位比对。
# 定点（FIXED_POINT）、不用 SIMD 内联：arm64 真机、x86_64 模拟器、Mac 单测的解码结果逐位一致。
# 编成静态库，链接进 libentry.so 时只会拉进解码器实际引用到的目标文件（约 180 KB 代码）。
# Mac 上的 tools/build_cli.sh 用同样的源文件和宏编一份 build/libopus_dec.a。
cmake_minimum_required(VERSION 3.5.0)
project(opus_dec C)

file(GLOB OPUS_DEC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/celt/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/silk/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/silk/fixed/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(opus_dec STATIC ${OPUS_DEC_SOURCES})
target_include_directories(opus_dec
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/celt ${CMAKE_CURRENT_SOURCE_DIR}/silk ${CMAKE_CURRENT_SOURCE_DIR}/silk/fixed)
# VAR_ARRAYS：临时缓冲用 C99 变长数组（在调用线程的栈上），不用全局伪栈，多个解码器可以并存
target_compile_definitions(opus_dec PRIVATE OPUS_BUILD FIXED_POINT DISABLE_FLOAT_API VAR_ARRAYS PACKAGE_VERSION="1.6.1")
# 调试构建也用 -O2：解码在音频接收线程里跑，不能拖慢
target_compile_options(opus_dec PRIVATE -O2 -w -fPIC -ffunction-sections -fdata-sections -fvisibility=hidden)
