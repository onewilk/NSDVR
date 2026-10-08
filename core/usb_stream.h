// SysDVR USB 串流客户端（与平台无关，底层读写由 UsbTransport 提供）。
// 协议（sysmodule/source/modes/USBmode.c）：
//   1. Switch 循环往 IN 端点写 10 字节 hello "SysDVR|03\0"，直到有人读走；
//   2. 客户端往 OUT 端点写 16 字节握手请求（音视频一起订阅），再从 IN 读握手应答（协议 03 为 72 字节）；
//   3. 之后每个包（18 字节包头 + 负载）一次 bulk 传输发出，音视频共用 IN 端点；
//   4. Switch 超过 1.5 秒发不出去就判定断开，回到第 1 步。
// 读取按连续字节流解析，不依赖“一次读取正好一个包”：粘包、拆包都能处理。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "stream_source.h"

namespace sysdvr {

class UsbTransport {
public:
    virtual ~UsbTransport() = default;
    // 读 IN 端点：返回读到的字节数；0 表示超时；<0 表示出错（设备断开等）
    virtual int Read(uint8_t* buf, size_t capacity, int timeoutMs) = 0;
    // 写 OUT 端点：返回写入的字节数；<0 表示出错
    virtual int Write(const uint8_t* buf, size_t length, int timeoutMs) = 0;
    // 最近一次出错的原因（给用户看），没有就返回空串
    virtual std::string LastError() const { return {}; }
};

class UsbStreamClient : public StreamSource {
public:
    UsbStreamClient(std::unique_ptr<UsbTransport> transport, StreamOptions options, StreamCallbacks callbacks);
    ~UsbStreamClient() override;

    void Start() override;
    void Stop() override;
    BridgeStats GetStats() const override;
    // USB 带宽充足，音视频总是一起订阅；关闭声音只是本地不播放（重新握手会打断画面，不值得）
    void SetAudioEnabled(bool enabled) override { dispatcher_.SetAudioMuted(!enabled); }

private:
    void Run();
    // helloProtocol 非空表示 hello 已经在收流时读到了，直接发请求
    bool Handshake(std::string helloProtocol);
    // 正常收流直到出错或停止
    void ReceiveLoop();
    void Status(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    std::unique_ptr<UsbTransport> transport_;
    const StreamOptions options_;
    const StreamCallbacks callbacks_;
    PacketDispatcher dispatcher_{options_, callbacks_};

    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::thread thread_;
    std::vector<uint8_t> buffer_;  // 读缓冲：一次最多一个完整包，外加可能粘上的下一个包
    std::string pendingHello_;     // 收流时读到了 Switch 的 hello（它那边已判定断开）
};

}  // namespace sysdvr
