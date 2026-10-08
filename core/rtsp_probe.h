// RTSP 连接检测：按 SysDVR 的 RTSP 服务端（sysmodule/source/rtsp/RTSP.c）走一遍
// OPTIONS → DESCRIBE → SETUP 视频/音频（RTP over TCP）→ PLAY，数几秒内收到的 RTP 包，最后发 TEARDOWN 正常结束。
//
// 为什么要正常结束：这个服务端一次只接一个客户端，并且把 recv 返回 0（对方已断开）当成“暂时没数据”，
// 客户端在它发出第一帧数据之前直接断开，它就会一直卡在这个死连接上、不再接受新连接，
// 直到有数据要发（游戏开始画面输出）时发送失败才恢复。发 TEARDOWN 它会立刻退出并在约 1 秒后重新监听。
#pragma once

#include <cstdint>
#include <string>

namespace sysdvr {

struct RtspProbeResult {
    bool reachable = false;  // TCP 能连上 6666
    bool described = false;  // 拿到 SDP
    bool playing = false;    // PLAY 成功
    int videoPackets = 0;    // 采样期间收到的 RTP 包（interleaved 通道 0 / 2）
    int audioPackets = 0;
    uint64_t bytes = 0;
    std::string error;       // 失败原因（中文）
    std::string log;         // 逐步记录，便于排查
};

// 阻塞调用，耗时约 sampleMs + 几百毫秒；不要在 UI 线程调用
RtspProbeResult ProbeRtsp(const std::string& host, uint16_t port = 6666, int sampleMs = 3000);

}  // namespace sysdvr
