// RTSP 连接检测的命令行版：./rtsp_probe <ip> [采样秒数]，桌面和鸿蒙（hdc shell）都能跑
#include <cstdio>
#include <cstdlib>

#include "../core/rtsp_probe.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "用法：%s <Switch IP> [采样秒数]\n", argv[0]);
        return 2;
    }
    const int seconds = argc >= 3 ? std::atoi(argv[2]) : 3;
    const sysdvr::RtspProbeResult r = sysdvr::ProbeRtsp(argv[1], 6666, seconds * 1000);
    std::printf("%s", r.log.c_str());
    std::printf("结论：%s\n", r.error.empty() ? "正常，可以播放" : r.error.c_str());
    return r.error.empty() ? 0 : 1;
}
