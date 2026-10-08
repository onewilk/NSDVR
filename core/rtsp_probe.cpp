#include "rtsp_probe.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "i18n.h"

namespace sysdvr {

namespace {

using Clock = std::chrono::steady_clock;

int64_t MsSince(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

class Conn {
public:
    ~Conn() {
        if (fd_ >= 0) close(fd_);
    }

    bool Connect(const std::string& host, uint16_t port, int timeoutMs, std::string* err) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            *err = L("IP 地址格式不对", "IP 位址格式不正確", "invalid IP address");
            return false;
        }
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            *err = Format(L("创建 socket 失败：%s", "建立 socket 失敗：%s", "failed to create socket: %s"), std::strerror(errno));
            return false;
        }
        const int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc < 0 && errno != EINPROGRESS) {
            *err = Format(L("连接失败：%s", "連線失敗：%s", "connection failed: %s"), std::strerror(errno));
            return false;
        }
        if (rc < 0) {
            pollfd p{fd_, POLLOUT, 0};
            if (poll(&p, 1, timeoutMs) <= 0) {
                *err = L("连接超时（网络不通，或游戏机不在这个地址）", "連線逾時（網路不通，或遊戲機不在這個位址）",
                         "connection timed out (no network route, or the console is not at this address)");
                return false;
            }
            int soErr = 0;
            socklen_t len = sizeof(soErr);
            getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soErr, &len);
            if (soErr != 0) {
                *err = soErr == ECONNREFUSED
                           ? std::string(L("端口 6666 拒绝连接：SysDVR 不在 RTSP 模式，或服务端还卡在上一个连接上",
                                           "連接埠 6666 拒絕連線：SysDVR 不在 RTSP 模式，或伺服端還卡在上一個連線上",
                                           "port 6666 refused the connection: SysDVR is not in RTSP mode, or the "
                                           "server is still stuck on the previous connection"))
                           : Format(L("连接失败：%s", "連線失敗：%s", "connection failed: %s"), std::strerror(soErr));
                return false;
            }
        }
        fcntl(fd_, F_SETFL, flags);
        return true;
    }

    bool Send(const std::string& s) {
        size_t sent = 0;
        while (sent < s.size()) {
            const ssize_t n = send(fd_, s.data() + sent, s.size() - sent, MSG_NOSIGNAL_FLAG);
            if (n <= 0) return false;
            sent += size_t(n);
        }
        return true;
    }

    // 读一个 RTSP 应答（头部 + Content-Length 指定的正文）；中间夹着的 interleaved 数据帧跳过
    bool ReadResponse(int timeoutMs, std::string* head, std::string* body) {
        const auto t0 = Clock::now();
        while (MsSince(t0) < timeoutMs) {
            const size_t end = buf_.find("\r\n\r\n");
            if (!buf_.empty() && buf_[0] == '$') {
                if (buf_.size() < 4) {
                    if (!Fill(t0, timeoutMs)) return false;
                    continue;
                }
                const size_t len = (uint8_t(buf_[2]) << 8) | uint8_t(buf_[3]);
                if (buf_.size() < 4 + len) {
                    if (!Fill(t0, timeoutMs)) return false;
                    continue;
                }
                buf_.erase(0, 4 + len);
                continue;
            }
            if (end == std::string::npos) {
                if (!Fill(t0, timeoutMs)) return false;
                continue;
            }
            *head = buf_.substr(0, end + 4);
            size_t contentLength = 0;
            const size_t cl = head->find("Content-Length:");
            if (cl != std::string::npos) contentLength = size_t(std::atoi(head->c_str() + cl + 15));
            while (buf_.size() < end + 4 + contentLength) {
                if (!Fill(t0, timeoutMs)) return false;
            }
            *body = buf_.substr(end + 4, contentLength);
            buf_.erase(0, end + 4 + contentLength);
            return true;
        }
        return false;
    }

    // 在 durationMs 内统计 interleaved 帧
    void CountData(int durationMs, RtspProbeResult* r) {
        const auto t0 = Clock::now();
        while (MsSince(t0) < durationMs) {
            if (buf_.size() >= 4 && buf_[0] == '$') {
                const size_t len = (uint8_t(buf_[2]) << 8) | uint8_t(buf_[3]);
                if (buf_.size() < 4 + len) {
                    if (!Fill(t0, durationMs)) return;
                    continue;
                }
                const int ch = uint8_t(buf_[1]);
                if (ch == 0) ++r->videoPackets;
                if (ch == 2) ++r->audioPackets;
                r->bytes += len;
                buf_.erase(0, 4 + len);
                continue;
            }
            if (!buf_.empty() && buf_[0] != '$') {
                // 夹在数据里的文本应答：丢到下一个 '$'
                const size_t next = buf_.find('$');
                buf_.erase(0, next == std::string::npos ? buf_.size() : next);
                continue;
            }
            if (!Fill(t0, durationMs)) return;
        }
    }

private:
#ifdef MSG_NOSIGNAL
    static constexpr int MSG_NOSIGNAL_FLAG = MSG_NOSIGNAL;
#else
    static constexpr int MSG_NOSIGNAL_FLAG = 0;
#endif

    bool Fill(Clock::time_point t0, int timeoutMs) {
        const int64_t left = timeoutMs - MsSince(t0);
        if (left <= 0) return false;
        pollfd p{fd_, POLLIN, 0};
        if (poll(&p, 1, int(left)) <= 0) return false;
        char tmp[16384];
        const ssize_t n = recv(fd_, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf_.append(tmp, size_t(n));
        return true;
    }

    int fd_ = -1;
    std::string buf_;
};

std::string FirstLine(const std::string& s) { return s.substr(0, s.find("\r\n")); }

}  // namespace

RtspProbeResult ProbeRtsp(const std::string& host, uint16_t port, int sampleMs) {
    RtspProbeResult r;
    const std::string base = "rtsp://" + host + ":" + std::to_string(port) + "/";
    Conn c;
    auto logf = [&r](const std::string& line) { r.log += line + "\n"; };

    if (!c.Connect(host, port, 3000, &r.error)) {
        logf(Format(L("连接 %s:%u：%s", "連線 %s:%u：%s", "connect %s:%u: %s"), host.c_str(), unsigned(port), r.error.c_str()));
        return r;
    }
    r.reachable = true;
    logf(Format(L("TCP 已连接 %s:%u", "TCP 已連線 %s:%u", "TCP connected to %s:%u"), host.c_str(), unsigned(port)));

    int cseq = 0;
    std::string head, body;
    auto request = [&](const std::string& method, const std::string& url, const std::string& extra) -> bool {
        const std::string req = method + " " + url + " RTSP/1.0\r\nCSeq: " + std::to_string(++cseq) +
                                "\r\nUser-Agent: NSDVR\r\n" + extra + "\r\n";
        if (!c.Send(req) || !c.ReadResponse(3000, &head, &body)) {
            r.error = Format(L("%s 没有应答", "%s 沒有回應", "%s: no response"), method.c_str());
            logf(Format(L("%s：没有应答", "%s：沒有回應", "%s: no response"), method.c_str()));
            return false;
        }
        logf(method + "：" + FirstLine(head));
        if (head.find(" 200 ") == std::string::npos) {
            r.error = Format(L("%s 被拒绝：%s", "%s 被拒絕：%s", "%s rejected: %s"), method.c_str(), FirstLine(head).c_str());
            return false;
        }
        return true;
    };

    if (!request("OPTIONS", base, "")) return r;
    if (!request("DESCRIBE", base, "Accept: application/sdp\r\n")) return r;
    r.described = body.find("m=video") != std::string::npos;
    const bool hasAudio = body.find("m=audio") != std::string::npos;
    logf(std::string("SDP：") +
         (r.described ? L("有视频轨", "有視訊軌", "video track") : L("没有视频轨", "沒有視訊軌", "no video track")) +
         (hasAudio ? L("、有音频轨", "、有音訊軌", ", audio track") : ""));
    if (!request("SETUP", base + "video", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")) return r;
    if (!request("SETUP", base + "audio", "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\nSession: 69\r\n"))
        return r;
    if (!request("PLAY", base, "Session: 69\r\nRange: npt=0.000-\r\n")) return r;
    r.playing = true;

    c.CountData(sampleMs, &r);
    char line[160];
    std::snprintf(line, sizeof(line),
                  L("%d 秒内收到视频 %d 包、音频 %d 包，共 %.1f KB", "%d 秒內收到視訊 %d 包、音訊 %d 包，共 %.1f KB",
                    "%d s: received %d video and %d audio packets, %.1f KB in total"),
                  sampleMs / 1000, r.videoPackets, r.audioPackets, double(r.bytes) / 1024.0);
    logf(line);
    if (r.videoPackets == 0) {
        r.error = L("服务端正常但没有画面数据：请先启动一个支持录像的游戏",
                    "伺服端正常但沒有畫面資料：請先啟動一個支援錄影的遊戲",
                    "the server is fine but sends no video: start a game that allows recording");
    }

    // 正常结束会话，服务端约 1 秒后重新监听
    const std::string teardown = "TEARDOWN " + base + " RTSP/1.0\r\nCSeq: " + std::to_string(++cseq) +
                                 "\r\nSession: 69\r\n\r\n";
    c.Send(teardown);
    logf(L("TEARDOWN 已发送", "TEARDOWN 已傳送", "TEARDOWN sent"));
    return r;
}

}  // namespace sysdvr
