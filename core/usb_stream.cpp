#include "usb_stream.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "i18n.h"
#include "log.h"

namespace sysdvr {

namespace {
constexpr size_t kMaxTransfer = kMaxPayload + kHeaderSize;
constexpr int kReadTimeoutMs = 800;       // 与官方客户端一致
constexpr int kHandshakeTimeoutMs = 1500;
constexpr int kRetryDelayMs = 1000;
constexpr int kIdleReconnectMs = 5000;
}  // namespace

namespace {
// 实验扩展只用于 TCP Bridge：USB 握手与官方完全一致
StreamOptions WithoutNextExt(StreamOptions o) {
    o.nextExt = false;
    return o;
}
}  // namespace

UsbStreamClient::UsbStreamClient(std::unique_ptr<UsbTransport> transport, StreamOptions options,
                                 StreamCallbacks callbacks)
    : transport_(std::move(transport)), options_(WithoutNextExt(options)), callbacks_(std::move(callbacks)) {
    dispatcher_.SetAudioMuted(!options_.audio);
}

UsbStreamClient::~UsbStreamClient() { Stop(); }

void UsbStreamClient::Start() {
    stop_ = false;
    buffer_.assign(kMaxTransfer * 2, 0);
    thread_ = std::thread(&UsbStreamClient::Run, this);
}

void UsbStreamClient::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

BridgeStats UsbStreamClient::GetStats() const {
    BridgeStats s;
    dispatcher_.FillStats(&s);
    s.videoConnected = s.audioConnected = connected_;
    return s;
}

void UsbStreamClient::Status(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Logf(LogLevel::Info, "%s", buf);
    if (callbacks_.onStatus) callbacks_.onStatus(buf);
}

void UsbStreamClient::Run() {
    bool first = true;
    while (!stop_) {
        std::string hello;
        hello.swap(pendingHello_);
        if (Handshake(hello)) {
            connected_ = true;
            dispatcher_.ResetReplay();
            ReceiveLoop();
            connected_ = false;
            if (stop_) break;
            ++dispatcher_.reconnects;
            Status("%s", L("USB：连接中断，准备重新握手", "USB：連線中斷，準備重新交握", "USB: connection lost, re-handshaking"));
            if (!pendingHello_.empty()) continue;  // Switch 正在等请求，立刻握手
        } else if (first) {
            Status("%s", L("USB：等待游戏机响应（确认 SysDVR 设为 USB 模式、已启动游戏、掌机模式未插底座）",
                         "USB：等待遊戲機回應（確認 SysDVR 設為 USB 模式、已啟動遊戲、掌機模式未插底座）",
                         "USB: waiting for the console (SysDVR in USB mode, a game running, handheld mode, not docked)"));
        }
        first = false;
        for (int waited = 0; waited < kRetryDelayMs && !stop_; waited += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool UsbStreamClient::Handshake(std::string protocol) {
    uint8_t* buf = buffer_.data();
    int n = 0;
    if (protocol.empty()) {
        // 1. 读 hello。Switch 每次写 hello 最多等 1 秒，所以多读几次；
        //    读到的若是上一次连接残留的数据包就跳过，直到 Switch 那边超时断开、开始发 hello
        for (int i = 0; i < 8 && !stop_; ++i) {
            n = transport_->Read(buf, kMaxTransfer, kHandshakeTimeoutMs);
            if (n < 0) {
                const std::string why = transport_->LastError();
                Status(L("USB：读取失败（%s）", "USB：讀取失敗（%s）", "USB: read failed (%s)"),
                       why.empty() ? L("设备已断开？", "裝置已中斷連線？", "device disconnected?") : why.c_str());
                return false;
            }
            if (n == int(kHelloSize) && ParseHello(buf, kHelloSize, &protocol)) break;
        }
        if (protocol.empty()) return false;
    }
    if (!IsProtocolSupported(protocol)) {
        Status(L("USB：sysmodule 协议版本 %s 不受支持", "USB：sysmodule 協定版本 %s 不受支援", "USB: sysmodule protocol %s is not supported"),
               protocol.c_str());
        return false;
    }

    // 2. 发握手请求：USB 下音视频走同一个端点，一次订阅两路
    auto req = BuildHandshake(protocol, StreamKind::Video, options_);
    const auto audioReq = BuildHandshake(protocol, StreamKind::Audio, options_);
    req[6] = 0x3;          // MetaFlags：视频 | 音频
    req[8] = audioReq[8];  // AudioBatching
    if (transport_->Write(req.data(), req.size(), kHandshakeTimeoutMs) != int(req.size())) {
        const std::string why = transport_->LastError();
        Status(L("USB：发送握手失败%s%s", "USB：傳送交握失敗%s%s", "USB: failed to send handshake%s%s"),
               why.empty() ? "" : L("：", "：", ": "), why.c_str());
        return false;
    }

    // 3. 读应答
    const size_t respSize = HandshakeResponseSize(protocol);
    n = transport_->Read(buf, kMaxTransfer, kHandshakeTimeoutMs);
    if (n < int(respSize)) {
        Status(L("USB：没有收到握手应答（%d 字节）", "USB：沒有收到交握回應（%d 位元組）", "USB: no handshake response (%d bytes)"), n);
        return false;
    }
    const uint32_t code = ParseHandshakeResult(buf);
    if (code != kHandshakeOk) {
        Status(L("USB：握手被拒绝：%s", "USB：交握被拒絕：%s", "USB: handshake rejected: %s"), HandshakeResultName(code));
        return false;
    }
    Status(L("USB：握手成功（协议 %s）", "USB：交握成功（協定 %s）", "USB: handshake OK (protocol %s)"), protocol.c_str());
    const SwitchMemory mem = ParseMemoryReport(buf, size_t(n));
    dispatcher_.SetSwitchMemory(mem);
    Logf(LogLevel::Info, "游戏机内存：%s", DescribeMemoryReport(mem).c_str());
    return true;
}

void UsbStreamClient::ReceiveLoop() {
    uint8_t* buf = buffer_.data();
    size_t have = 0;  // buf 里尚未处理的字节数
    auto lastData = std::chrono::steady_clock::now();
    while (!stop_) {
        // have 不超过一个完整包，剩余空间总能放下下一个包
        const int n = transport_->Read(buf + have, buffer_.size() - have, kReadTimeoutMs);
        if (n < 0) {
            const std::string why = transport_->LastError();
            Status(L("USB：读取出错（%s）", "USB：讀取出錯（%s）", "USB: read error (%s)"),
                   why.empty() ? L("数据线松动或设备已断开？", "傳輸線鬆脫或裝置已中斷連線？", "loose cable or device disconnected?")
                               : why.c_str());
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (n == 0) {
            // 画面静止或游戏暂停时 sysmodule 可能一直不发；超过 5 秒没数据就重新握手
            if (now - lastData > std::chrono::milliseconds(kIdleReconnectMs)) return;
            continue;
        }
        lastData = now;
        // Switch 发送超时后会断开并重新发 hello：单独一次 10 字节的读取就是它
        std::string protocol;
        if (n == int(kHelloSize) && ParseHello(buf + have, kHelloSize, &protocol)) {
            pendingHello_ = protocol;
            return;
        }
        have += size_t(n);

        size_t pos = 0;
        while (have - pos >= kHeaderSize) {
            const PacketHeader h = ParseHeader(buf + pos);
            if (!ValidateHeader(h)) {
                // 数据错位：往后找下一个 CC CC CC CC（USB 音视频同一个流，按视频丢失处理更稳妥）
                dispatcher_.NotifyResync(true);
                size_t next = pos + 1;
                while (next + 4 <= have && !(buf[next] == 0xCC && buf[next + 1] == 0xCC && buf[next + 2] == 0xCC &&
                                              buf[next + 3] == 0xCC))
                    ++next;
                pos = next;
                continue;
            }
            if (have - pos < kHeaderSize + h.dataSize) break;  // 拆包：等下一次读取补齐
            dispatcher_.Dispatch(h, buf + pos + kHeaderSize);
            pos += kHeaderSize + h.dataSize;
        }
        // 把未处理的尾巴挪到开头
        if (pos > 0) {
            std::memmove(buf, buf + pos, have - pos);
            have -= pos;
        }
    }
}

}  // namespace sysdvr
