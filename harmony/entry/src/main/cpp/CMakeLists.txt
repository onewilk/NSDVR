cmake_minimum_required(VERSION 3.5.0)
project(sysdvr_harmony)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 协议核心与桌面测试工具共用，位于仓库根目录 core/
set(SYSDVR_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core)

include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${SYSDVR_CORE_DIR})

add_library(entry SHARED
    napi_init.cpp
    session.cpp
    usb_host.cpp
    video_decoder.cpp
    video_enhancer.cpp
    vsync_monitor.cpp
    audio_player.cpp
    ${SYSDVR_CORE_DIR}/sysdvr_protocol.cpp
    ${SYSDVR_CORE_DIR}/gop_cache.cpp
    ${SYSDVR_CORE_DIR}/frame_pacer.cpp
    ${SYSDVR_CORE_DIR}/frame_loss.cpp
    ${SYSDVR_CORE_DIR}/tcp_bridge.cpp
    ${SYSDVR_CORE_DIR}/stream_source.cpp
    ${SYSDVR_CORE_DIR}/ext_audio.cpp
    ${SYSDVR_CORE_DIR}/usb_stream.cpp
    ${SYSDVR_CORE_DIR}/rtsp_probe.cpp
    ${SYSDVR_CORE_DIR}/log.cpp
    ${SYSDVR_CORE_DIR}/i18n.cpp
)

target_compile_options(entry PRIVATE -Wall -Wextra)

# 实验扩展的 Opus 音频解码：libopus 1.6.1 的解码部分（定点，静态链接，与服务端同版本），见 third_party/opus/CMakeLists.txt。
# 系统 AVCodec 的 Opus 解码不在 OpenHarmony 公开支持的解码格式里、与设备强相关，所以不依赖它
add_subdirectory(${SYSDVR_CORE_DIR}/../third_party/opus ${CMAKE_CURRENT_BINARY_DIR}/opus_dec)
target_compile_definitions(entry PRIVATE SYSDVR_WITH_OPUS)

target_link_libraries(entry PUBLIC
    opus_dec
    libace_napi.z.so
    libhilog_ndk.z.so
    libnative_media_codecbase.so
    libnative_media_core.so
    libnative_media_vdec.so
    libohaudio.so
    libnative_window.so
    libnative_vsync.so
    libvideo_processing.so
)
