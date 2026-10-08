#include "tcp_bridge.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "i18n.h"
#include "log.h"

namespace sysdvr {

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// 接收超时切片：Stop() 最多等这么久就能让线程退出
constexpr int kRecvSliceMs = 300;
constexpr int kConnectTimeoutMs = 3000;
constexpr int kHandshakeTimeoutMs = 5000;
constexpr int kReconnectDelayMs = 1000;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void SetRecvTimeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void SetInt(int fd, int level, int opt, int value) { setsockopt(fd, level, opt, &value, sizeof(value)); }

bool ResolveIPv4(const std::string& host, in_addr* out) {
    if (inet_pton(AF_INET, host.c_str(), out) == 1) return true;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) return false;
    *out = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

int ConnectWithTimeout(const std::string& host, uint16_t port, int timeoutMs) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!ResolveIPv4(host, &addr.sin_addr)) {
        errno = EHOSTUNREACH;
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
#ifdef SO_NOSIGPIPE
    SetInt(fd, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif

    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        const int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    if (rc < 0) {
        pollfd pfd{fd, POLLOUT, 0};
        rc = poll(&pfd, 1, timeoutMs);
        int err = 0;
        socklen_t len = sizeof(err);
        if (rc <= 0) {
            err = (rc == 0) ? ETIMEDOUT : errno;
        } else {
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        }
        if (err != 0) {
            close(fd);
            errno = err;
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);

    SetInt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
    SetInt(fd, SOL_SOCKET, SO_RCVBUF, int(kMaxPayload + kHeaderSize));
    // 让 Switch 休眠/断网这类“静默断开”在几秒内被发现
    SetInt(fd, SOL_SOCKET, SO_KEEPALIVE, 1);
#if defined(TCP_KEEPIDLE)
    SetInt(fd, IPPROTO_TCP, TCP_KEEPIDLE, 5);
#elif defined(TCP_KEEPALIVE)
    SetInt(fd, IPPROTO_TCP, TCP_KEEPALIVE, 5);
#endif
#ifdef TCP_KEEPINTVL
    SetInt(fd, IPPROTO_TCP, TCP_KEEPINTVL, 2);
#endif
#ifdef TCP_KEEPCNT
    SetInt(fd, IPPROTO_TCP, TCP_KEEPCNT, 3);
#endif
    SetRecvTimeout(fd, kRecvSliceMs);
    return fd;
}

bool SendAll(int fd, const uint8_t* p, size_t n) {
    while (n > 0) {
        ssize_t r = send(fd, p, n, kSendFlags);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// TcpBridgeClient

TcpBridgeClient::TcpBridgeClient(std::string host, StreamOptions options, Callbacks callbacks)
    : host_(std::move(host)), options_(options), callbacks_(std::move(callbacks)),
      extAudio_(ClampExtAudioConfig(options.extAudio)) {}

TcpBridgeClient::~TcpBridgeClient() { Stop(); }

void TcpBridgeClient::Start() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    stop_ = false;
    if (options_.video) video_.thread = std::thread(&TcpBridgeClient::ChannelThread, this, &video_);
    if (options_.audio) audio_.thread = std::thread(&TcpBridgeClient::ChannelThread, this, &audio_);
}

void TcpBridgeClient::Stop() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    stop_ = true;
    if (video_.thread.joinable()) video_.thread.join();
    if (audio_.thread.joinable()) audio_.thread.join();
}

void TcpBridgeClient::SetAudioEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (stop_) return;
    if (enabled) {
        if (audio_.thread.joinable()) return;
        audio_.stop = false;
        audio_.thread = std::thread(&TcpBridgeClient::ChannelThread, this, &audio_);
        Status("%s", L("音频流：已开启", "音訊串流：已開啟", "Audio stream: on"));
    } else {
        if (!audio_.thread.joinable()) return;
        audio_.stop = true;
        audio_.thread.join();
        Status("%s", L("音频流：已关闭（游戏机停止发送音频）", "音訊串流：已關閉（遊戲機停止傳送音訊）",
                     "Audio stream: off (the console stopped sending audio)"));
    }
}

std::vector<int> TcpBridgeClient::SocketFds() const {
    std::vector<int> fds;
    if (video_.connected && video_.fd >= 0) fds.push_back(video_.fd);
    if (audio_.connected && audio_.fd >= 0) fds.push_back(audio_.fd);
    return fds;
}

BridgeStats TcpBridgeClient::GetStats() const {
    BridgeStats s;
    dispatcher_.FillStats(&s);
    s.videoConnected = video_.connected;
    s.audioConnected = audio_.connected;
    {
        std::lock_guard<std::mutex> lock(extMutex_);
        s.extAudioRequested = extAudio_;
    }
    // 音频连着、服务端支持扩展，但最近的包还不是请求的那组参数：切换还没生效
    s.extAudioPending = options_.nextExt && s.extSupported && s.audioConnected && s.audioCodec >= 0 &&
                        !ExtAudioMatches(s.extAudioRequested, s);
    return s;
}

bool TcpBridgeClient::SetExtAudio(const ExtAudioConfig& config) {
    const ExtAudioConfig c = ClampExtAudioConfig(config);
    {
        std::lock_guard<std::mutex> lock(extMutex_);
        extAudio_ = c;
    }
    if (!options_.nextExt || !dispatcher_.ExtSupported()) return false;
    const auto msg = BuildExtControl(c);
    std::lock_guard<std::mutex> lock(audio_.fdMutex);
    const int fd = audio_.fd;
    if (!audio_.connected || fd < 0) return false;  // 音频没连着：下次握手时带上
    // 只有 8 字节，发送缓冲不可能满；非阻塞发送，保证不卡住界面线程
    const ssize_t r = send(fd, msg.data(), msg.size(), kSendFlags | MSG_DONTWAIT);
    if (r != ssize_t(msg.size())) {
        Logf(LogLevel::Warn, "发送音频控制消息失败（%zd）：%s", r, std::strerror(errno));
        return false;
    }
    Logf(LogLevel::Info, "已请求音频编码 %s（Opus %d kbps · 复杂度 %d · %d ms）", AudioCodecName(c.codec), c.opusKbps,
         c.opusComplexity, c.opusFrameMs);
    return true;
}

const char* TcpBridgeClient::ChannelName(const Channel* ch) {
    return ch->kind == StreamKind::Video ? L("视频", "視訊", "Video") : L("音频", "音訊", "Audio");
}

void TcpBridgeClient::Status(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Logf(LogLevel::Info, "%s", buf);
    if (callbacks_.onStatus) callbacks_.onStatus(buf);
}

void TcpBridgeClient::ChannelThread(Channel* ch) {
    while (!Stopping(ch)) {
        const int fd = ConnectAndHandshake(ch);
        if (fd >= 0) {
            {
                std::lock_guard<std::mutex> lock(ch->fdMutex);
                ch->fd = fd;
                ch->connected = true;
            }
            // sysmodule 每次新连接都会清空自己的哈希表，客户端的重放表也要跟着清
            if (ch->kind == StreamKind::Video) dispatcher_.ResetReplay();
            // 音频重连后服务端的编码器是新的，解码器历史也要清
            if (ch->kind == StreamKind::Audio) dispatcher_.ResetAudio();

            ReceiveLoop(ch, fd);

            {
                std::lock_guard<std::mutex> lock(ch->fdMutex);
                ch->connected = false;
                ch->fd = -1;
                close(fd);
            }
            if (Stopping(ch)) break;
            ++dispatcher_.reconnects;
            Status(L("%s流断开，准备重连", "%s串流中斷，準備重新連線", "%s stream lost, reconnecting"), ChannelName(ch));
        }
        // 可被 Stop() 打断的等待
        for (int waited = 0; waited < kReconnectDelayMs && !Stopping(ch); waited += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int TcpBridgeClient::ConnectAndHandshake(Channel* ch) {
    Status(L("%s流：连接 %s:%u …", "%s串流：連線 %s:%u …", "%s stream: connecting to %s:%u …"), ChannelName(ch), host_.c_str(),
           unsigned(ch->port));
    const int fd = ConnectWithTimeout(host_, ch->port, kConnectTimeoutMs);
    if (fd < 0) {
        Status(L("%s流：连接失败（%s）", "%s串流：連線失敗（%s）", "%s stream: connection failed (%s)"), ChannelName(ch),
               std::strerror(errno));
        return -1;
    }

    auto fail = [&](const char* why) {
        Status(L("%s流：%s", "%s串流：%s", "%s stream: %s"), ChannelName(ch), why);
        close(fd);
        return -1;
    };

    uint8_t hello[kHelloSize];
    if (!RecvExact(ch, fd, hello, sizeof(hello), kHandshakeTimeoutMs))
        return fail(L("没有收到 hello（游戏机端是否选了 TCP Bridge 模式并已启动游戏？）",
                      "沒有收到 hello（遊戲機端是否選了 TCP Bridge 模式並已啟動遊戲？）",
                      "no hello received (is the console set to TCP Bridge mode with a game running?)"));

    std::string protocol;
    if (!ParseHello(hello, sizeof(hello), &protocol))
        return fail(L("hello 格式不对，对端可能不是 SysDVR", "hello 格式不對，對端可能不是 SysDVR",
                      "invalid hello, the peer may not be SysDVR"));
    if (!IsProtocolSupported(protocol)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      L("sysmodule 协议版本 %s 不受支持（本客户端支持 02/03）", "sysmodule 協定版本 %s 不受支援（本用戶端支援 02/03）",
                        "sysmodule protocol %s is not supported (supported: 02/03)"),
                      protocol.c_str());
        return fail(buf);
    }

    // 音频路带上当前请求的编码（运行中切换过的话，重连后保持切换后的设置）
    StreamOptions opt = options_;
    {
        std::lock_guard<std::mutex> lock(extMutex_);
        opt.extAudio = extAudio_;
    }
    const auto req = BuildHandshake(protocol, ch->kind, opt);
    if (!SendAll(fd, req.data(), req.size()))
        return fail(L("发送握手失败", "傳送交握失敗", "failed to send handshake"));

    uint8_t resp[72];
    const size_t respSize = HandshakeResponseSize(protocol);
    if (!RecvExact(ch, fd, resp, respSize, kHandshakeTimeoutMs))
        return fail(L("没有收到握手应答", "沒有收到交握回應", "no handshake response"));

    const uint32_t code = ParseHandshakeResult(resp);
    if (code != kHandshakeOk) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), L("握手被拒绝：%s", "交握被拒絕：%s", "handshake rejected: %s"),
                      HandshakeResultName(code));
        return fail(buf);
    }

    Status(L("%s流：握手成功（协议 %s）", "%s串流：交握成功（協定 %s）", "%s stream: handshake OK (protocol %s)"),
           ChannelName(ch), protocol.c_str());
    if (ch->kind == StreamKind::Video) {
        // 协议 03 的应答里带着 Switch 内存报告（请求时置了 MemoryDiag）；02 没有，记为无效
        const SwitchMemory mem = ParseMemoryReport(resp, respSize);
        dispatcher_.SetSwitchMemory(mem);
        Logf(LogLevel::Info, "游戏机内存：%s", DescribeMemoryReport(mem).c_str());
    }
    return fd;
}

bool TcpBridgeClient::RecvExact(Channel* ch, int fd, uint8_t* buf, size_t n, int idleTimeoutMs) {
    size_t got = 0;
    int64_t lastData = NowMs();
    while (got < n) {
        if (Stopping(ch)) return false;
        const ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r > 0) {
            got += size_t(r);
            lastData = NowMs();
#ifdef TCP_QUICKACK
            // Linux 的快速 ACK 是一次性的，每次收完都要重新打开
            if (quickAck_) SetInt(fd, IPPROTO_TCP, TCP_QUICKACK, 1);
#endif
            continue;
        }
        if (r == 0) return false;  // 对端关闭
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (idleTimeoutMs > 0 && NowMs() - lastData > idleTimeoutMs) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool TcpBridgeClient::Resync(Channel* ch, int fd, uint8_t* header) {
    // Switch 端 socket 缓冲很小，极端情况下会丢数据；逐字节找 CC CC CC CC 重新对齐
    int run = 0;
    uint8_t b = 0;
    while (run < 4) {
        if (!RecvExact(ch, fd, &b, 1, 0)) return false;
        run = (b == 0xCC) ? run + 1 : 0;
    }
    std::memset(header, 0xCC, 4);
    return RecvExact(ch, fd, header + 4, kHeaderSize - 4, 0);
}

void TcpBridgeClient::ReceiveLoop(Channel* ch, int fd) {
    std::vector<uint8_t> payload(kMaxPayload);
    uint8_t hdr[kHeaderSize];
    bool inSync = true;
    const bool isVideoChannel = ch->kind == StreamKind::Video;
    // 只有请求了扩展的视频路才接受诊断包；其他情况与官方客户端完全一致
    const bool allowDiag = isVideoChannel && options_.nextExt;

    while (!Stopping(ch)) {
        if (inSync) {
            if (!RecvExact(ch, fd, hdr, kHeaderSize, 0)) return;
        } else {
            if (!Resync(ch, fd, hdr)) return;
            inSync = true;
        }

        const PacketHeader h = ParseHeader(hdr);
        if (!ValidateHeader(h, allowDiag) || h.IsVideo() != isVideoChannel) {
            Logf(LogLevel::Warn, "%s流包头异常（magic=%08x size=%u meta=%02x），开始重同步", ch->name, h.magic,
                 h.dataSize, h.meta);
            dispatcher_.NotifyResync(isVideoChannel);
            inSync = false;
            continue;
        }

        if (h.dataSize > 0 && !RecvExact(ch, fd, payload.data(), h.dataSize, 0)) return;

        dispatcher_.Dispatch(h, payload.data());
    }
}

// ---------------------------------------------------------------------------
// Discovery

Discovery::~Discovery() { Stop(); }

bool Discovery::Start() {
    if (thread_.joinable()) return true;

    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        Logf(LogLevel::Error, "创建 UDP socket 失败：%s", std::strerror(errno));
        return false;
    }
    SetInt(fd_, SOL_SOCKET, SO_REUSEADDR, 1);
#ifdef SO_REUSEPORT
    SetInt(fd_, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
    SetInt(fd_, SOL_SOCKET, SO_BROADCAST, 1);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kDiscoveryPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        Logf(LogLevel::Error, "绑定 UDP %u 失败：%s", unsigned(kDiscoveryPort), std::strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }
    SetRecvTimeout(fd_, 500);

    stop_ = false;
    thread_ = std::thread(&Discovery::Run, this);
    return true;
}

void Discovery::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

std::vector<DeviceInfo> Discovery::Devices() const {
    std::vector<DeviceInfo> out;
    const int64_t now = NowMs();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& e : devices_)
        if (now - e.lastSeenMs < 10000) out.push_back(e.info);
    return out;
}

void Discovery::Run() {
    uint8_t buf[512];
    while (!stop_) {
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        const ssize_t r = recvfrom(fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (r <= 0) continue;

        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        DeviceInfo info;
        if (!ParseBeacon(buf, size_t(r), ip, &info)) continue;

        std::lock_guard<std::mutex> lock(mutex_);
        bool found = false;
        for (auto& e : devices_) {
            if (e.info.ip == info.ip) {
                e.info = info;
                e.lastSeenMs = NowMs();
                found = true;
            }
        }
        if (!found) {
            Logf(LogLevel::Info, "发现 SysDVR %s（协议 %s）@ %s", info.version.c_str(), info.protocol.c_str(), ip);
            devices_.push_back({info, NowMs()});
        }
    }
}

}  // namespace sysdvr
