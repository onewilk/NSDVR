// SysDVR TCP Bridge 客户端：视频走 9911、音频走 9922，各自独立连接、握手、断线重连。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "stream_source.h"
#include "sysdvr_protocol.h"

namespace sysdvr {

class TcpBridgeClient : public StreamSource {
public:
    using Callbacks = StreamCallbacks;

    TcpBridgeClient(std::string host, StreamOptions options, Callbacks callbacks);
    ~TcpBridgeClient() override;

    TcpBridgeClient(const TcpBridgeClient&) = delete;
    TcpBridgeClient& operator=(const TcpBridgeClient&) = delete;

    void Start() override;
    // 阻塞到两个接收线程退出；返回后不会再有回调
    void Stop() override;
    // 运行中单独开关音频通道：关闭时断开 9922，Switch 就不再发音频（省约 1.5 Mbps），视频不受影响。
    // 关闭会阻塞到音频线程退出（最多约 300ms），之后不再有 onAudio 回调
    void SetAudioEnabled(bool enabled) override;
    // 接收端每次收完数据立即回 ACK（TCP_QUICKACK，仅 Linux/鸿蒙内核有效）。
    // Switch 发送缓冲很小，吞吐约等于“缓冲 / 往返时间”，延迟 ACK 会让它干等
    void SetQuickAck(bool enabled) override { quickAck_ = enabled; }
    // 当前已连接的 socket（给系统网络加速接口标记高优先级用）
    std::vector<int> SocketFds() const override;
    // 实验扩展：记下新的音频编码（之后的音频握手都带它）；服务端已表明支持扩展且音频连着时，
    // 立即在 9922 上发 8 字节控制消息，不重连。生效与否看之后音频包的标记（GetStats 里的 extAudioPending）
    bool SetExtAudio(const ExtAudioConfig& config) override;

    BridgeStats GetStats() const override;

private:
    struct Channel {
        Channel(StreamKind k, uint16_t p, const char* n) : kind(k), port(p), name(n) {}
        const StreamKind kind;
        const uint16_t port;
        const char* const name;
        std::atomic<bool> stop{false};  // 单独停这一路（音频开关用）
        std::mutex fdMutex;             // 主线程往 socket 上发控制消息时，防止接收线程同时关掉它
        std::atomic<int> fd{-1};
        std::atomic<bool> connected{false};
        std::thread thread;
    };

    void ChannelThread(Channel* ch);
    int ConnectAndHandshake(Channel* ch);
    // 正常收流直到出错或停止
    void ReceiveLoop(Channel* ch, int fd);
    bool RecvExact(Channel* ch, int fd, uint8_t* buf, size_t n, int idleTimeoutMs);
    bool Resync(Channel* ch, int fd, uint8_t* header);
    bool Stopping(const Channel* ch) const { return stop_ || ch->stop; }
    void Status(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    // 界面上显示的通道名（按当前语言）；ch->name 只用于开发日志
    static const char* ChannelName(const Channel* ch);

    const std::string host_;
    const StreamOptions options_;
    const Callbacks callbacks_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> quickAck_{true};
    // 实验扩展：当前请求的音频编码（主线程写，握手时读）
    mutable std::mutex extMutex_;
    ExtAudioConfig extAudio_;
    std::mutex controlMutex_;  // 串行化 Start/Stop/SetAudioEnabled
    Channel video_{StreamKind::Video, kVideoPort, "视频"};
    Channel audio_{StreamKind::Audio, kAudioPort, "音频"};

    // 包分发（重放表只在视频线程里用）
    PacketDispatcher dispatcher_{options_, callbacks_};
};

// 监听 UDP 19999 上 sysmodule 每 2 秒一次的广播
class Discovery {
public:
    ~Discovery();
    bool Start();
    void Stop();
    // 返回最近 10 秒内出现过的设备
    std::vector<DeviceInfo> Devices() const;

private:
    void Run();

    struct Entry {
        DeviceInfo info;
        int64_t lastSeenMs;
    };

    std::atomic<bool> stop_{false};
    int fd_ = -1;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<Entry> devices_;
};

}  // namespace sysdvr
