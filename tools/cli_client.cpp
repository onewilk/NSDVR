// 桌面端测试工具：用和鸿蒙端同一份 core 代码连接 SysDVR（真机或 mock_sysdvr.py），
// 把收到的视频/音频原样写成文件，便于用 ffplay 或 diff 校验。
//
//   sysdvr_cli scan [秒数]
//   sysdvr_cli <host> [--seconds N] [--video out.h264] [--audio out.pcm] [--no-audio] [--no-video]
//                     [--no-replay] [--batching N] [--gop snapshot.h264] [--toggle-audio N]
//                     [--no-ext] [--codec 编码] [--switch 时刻:编码,时刻:编码...]
//   --toggle-audio N：第 N 秒关闭音频通道、第 2N 秒重新打开（验证运行中开关音频不影响视频）
//   实验扩展（docs/nsdvr-ext-protocol.md，默认和 App 一样请求扩展，官方 SysDVR 会忽略）：
//   --no-ext：不请求扩展（握手与旧版逐字节相同）
//   --codec 编码：初始音频编码，pcm | 24k | adpcm | opus[/码率kbps[/复杂度[/帧长ms]]]，如 opus/64/0/10
//   --switch 5:adpcm,10:opus/96/5/20：第 5 秒、第 10 秒运行中切换（发控制消息，不重连）
//   --audio 写出的是解码后的 48 kHz 立体声 PCM（压缩编码也一样）
//
// 播放验证：ffplay -f h264 out.h264 ；ffplay -f s16le -ar 48000 -ch_layout stereo out.pcm
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/frame_loss.h"
#include "../core/gop_cache.h"
#include "../core/log.h"
#include "../core/sysdvr_protocol.h"
#include "../core/tcp_bridge.h"

namespace {

std::atomic<bool> g_quit{false};

void OnSignal(int) { g_quit = true; }

// 解析 pcm | 24k | adpcm | opus[/kbps[/cx[/ms]]]
bool ParseCodec(const std::string& spec, sysdvr::ExtAudioConfig* out) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t slash = spec.find('/', start);
        parts.push_back(spec.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    sysdvr::ExtAudioConfig c = *out;
    if (parts[0] == "pcm") c.codec = sysdvr::AudioCodec::Pcm48;
    else if (parts[0] == "24k") c.codec = sysdvr::AudioCodec::Pcm24;
    else if (parts[0] == "adpcm") c.codec = sysdvr::AudioCodec::Adpcm;
    else if (parts[0] == "opus") c.codec = sysdvr::AudioCodec::Opus;
    else return false;
    if (parts.size() > 1) c.opusKbps = std::atoi(parts[1].c_str());
    if (parts.size() > 2) c.opusComplexity = std::atoi(parts[2].c_str());
    if (parts.size() > 3) c.opusFrameMs = std::atoi(parts[3].c_str());
    *out = sysdvr::ClampExtAudioConfig(c);
    return true;
}

std::string DescribeCodec(int codec, int kbps, int complexity, int frameMs) {
    char buf[96];
    if (codec < 0) return "官方 PCM";
    if (codec == int(sysdvr::AudioCodec::Opus))
        std::snprintf(buf, sizeof(buf), "Opus %d kbps 复杂度 %d %d ms", kbps, complexity, frameMs);
    else if (kbps > 0)
        std::snprintf(buf, sizeof(buf), "%s %d kbps", sysdvr::AudioCodecName(sysdvr::AudioCodec(codec)), kbps);
    else
        std::snprintf(buf, sizeof(buf), "%s", sysdvr::AudioCodecName(sysdvr::AudioCodec(codec)));
    return buf;
}

double Permille(uint32_t v) { return v == sysdvr::kExtUnavailable ? -1.0 : v / 10.0; }

int Scan(int seconds) {
    sysdvr::Discovery discovery;
    if (!discovery.Start()) return 1;
    std::printf("监听 UDP %u，%d 秒...\n", unsigned(sysdvr::kDiscoveryPort), seconds);
    for (int i = 0; i < seconds * 10 && !g_quit; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto devices = discovery.Devices();
    for (const auto& d : devices)
        std::printf("  SysDVR %s  协议 %s  序列号 %s  @ %s\n", d.version.c_str(), d.protocol.c_str(), d.serial.c_str(),
                    d.ip.c_str());
    std::printf("共发现 %zu 台\n", devices.size());
    return devices.empty() ? 2 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        std::fprintf(stderr, "用法: %s scan [秒数] | %s <host> [--seconds N] [--video f] [--audio f] "
                             "[--no-audio] [--no-video] [--no-replay] [--batching N] [--gop f] [--toggle-audio N] "
                             "[--no-ext] [--codec c] [--switch t:c,...]\n",
                     argv[0], argv[0]);
        return 1;
    }
    if (std::strcmp(argv[1], "scan") == 0) return Scan(argc > 2 ? std::atoi(argv[2]) : 5);

    const std::string host = argv[1];
    int seconds = 10;
    int toggleAudioAt = 0;
    std::string videoPath, audioPath, gopPath;
    sysdvr::StreamOptions opt;
    opt.nextExt = true;
    struct Switch {
        int at;
        sysdvr::ExtAudioConfig config;
    };
    std::vector<Switch> switches;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--seconds") seconds = std::atoi(next());
        else if (a == "--video") videoPath = next();
        else if (a == "--audio") audioPath = next();
        else if (a == "--gop") gopPath = next();
        else if (a == "--toggle-audio") toggleAudioAt = std::atoi(next());
        else if (a == "--no-audio") opt.audio = false;
        else if (a == "--no-video") opt.video = false;
        else if (a == "--no-replay") opt.nalReplay = false;
        else if (a == "--batching") opt.audioBatching = std::atoi(next());
        else if (a == "--no-ext") opt.nextExt = false;
        else if (a == "--codec") {
            if (!ParseCodec(next(), &opt.extAudio)) {
                std::fprintf(stderr, "编码格式不对（pcm | 24k | adpcm | opus[/kbps[/复杂度[/帧长]]]）\n");
                return 1;
            }
        } else if (a == "--switch") {
            const std::string list = next();
            size_t start = 0;
            sysdvr::ExtAudioConfig last = opt.extAudio;
            while (start < list.size()) {
                size_t comma = list.find(',', start);
                const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                const size_t colon = item.find(':');
                Switch sw{colon == std::string::npos ? 0 : std::atoi(item.c_str()), last};
                if (colon == std::string::npos || !ParseCodec(item.substr(colon + 1), &sw.config)) {
                    std::fprintf(stderr, "--switch 格式：秒:编码,秒:编码…\n");
                    return 1;
                }
                last = sw.config;
                switches.push_back(sw);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else {
            std::fprintf(stderr, "未知参数 %s\n", a.c_str());
            return 1;
        }
    }

    FILE* vf = videoPath.empty() ? nullptr : std::fopen(videoPath.c_str(), "wb");
    FILE* af = audioPath.empty() ? nullptr : std::fopen(audioPath.c_str(), "wb");

    std::atomic<bool> sawSps{false}, sawIdr{false};
    // 与鸿蒙端 Session 相同：每个视频包都进 GOP 缓存，结束时导出快照（模拟“切回前台时送解码器的内容”）
    sysdvr::GopCache gop;
    std::atomic<uint64_t> firstVideoTs{0}, lastVideoTs{0};

    // 与鸿蒙端 Session 相同的丢帧检测：时间戳断档 + 分发层上报的错误包 / 缓存未命中 / 失步
    sysdvr::FrameGapDetector gapDetector;  // 只在视频接收线程里访问
    std::atomic<uint64_t> lossCount[int(sysdvr::LossCause::Count)] = {};
    std::atomic<uint64_t> lostFrames{0};
    std::atomic<bool> explicitLoss{false};  // 与 Session 一致：明确上报过的丢失，随后的断档不重复计次

    sysdvr::TcpBridgeClient::Callbacks cb;
    cb.onVideoLoss = [&](sysdvr::LossCause cause) {
        ++lossCount[int(cause)];
        explicitLoss = true;
    };
    cb.onVideo = [&](const uint8_t* p, size_t n, uint64_t ts) {
        const int lost = gapDetector.OnFrame(ts);
        if (lost > 0) {
            if (!explicitLoss) ++lossCount[int(sysdvr::LossCause::TimestampGap)];
            lostFrames += uint64_t(lost);
        }
        explicitLoss = false;
        if (!sawSps && sysdvr::ContainsNalType(p, n, 7)) sawSps = true;
        if (!sawIdr && sysdvr::ContainsNalType(p, n, 5)) sawIdr = true;
        if (firstVideoTs == 0) firstVideoTs = ts;
        lastVideoTs = ts;
        if (vf) std::fwrite(p, 1, n, vf);
        gop.Push(p, n, ts);
    };
    std::atomic<uint64_t> audioFrames{0};  // 解码后交给“播放器”的 48 kHz 帧数（每秒应接近 48000）
    cb.onAudio = [&](const uint8_t* p, size_t n, uint64_t) {
        audioFrames += n / 4;
        if (af) std::fwrite(p, 1, n, af);
    };
    cb.onStatus = [](const std::string&) {};  // 已经通过 Logf 打印

    sysdvr::TcpBridgeClient client(host, opt, cb);
    client.Start();

    sysdvr::BridgeStats prev;
    uint64_t prevFrames = 0;
    for (int t = 0; t < seconds && !g_quit; ++t) {
        if (toggleAudioAt > 0 && t == toggleAudioAt) client.SetAudioEnabled(false);
        if (toggleAudioAt > 0 && t == toggleAudioAt * 2) client.SetAudioEnabled(true);
        for (const Switch& sw : switches) {
            if (sw.at != t) continue;
            const bool sent = client.SetExtAudio(sw.config);
            std::printf("[%2ds] 切换音频编码 → %s：%s\n", t, DescribeCodec(int(sw.config.codec), sw.config.codec == sysdvr::AudioCodec::Opus ? sw.config.opusKbps : 0,
                        sw.config.opusComplexity, sw.config.opusFrameMs).c_str(),
                        sent ? "已发控制消息" : "服务端不支持扩展或音频未连接（已记下，下次握手生效）");
        }
        for (int i = 0; i < 10 && !g_quit; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto s = client.GetStats();
        std::printf("[%2ds] 视频 %s %4llu 包/s %7.1f KB/s | 音频 %s %4llu 包/s %6.1f KB/s | 重放 %llu/%llu 重同步 %llu "
                    "错误包 %llu 重连 %llu\n",
                    t + 1, s.videoConnected ? "●" : "○", (unsigned long long)(s.videoPackets - prev.videoPackets),
                    (s.videoBytes - prev.videoBytes) / 1024.0, s.audioConnected ? "●" : "○",
                    (unsigned long long)(s.audioPackets - prev.audioPackets), (s.audioBytes - prev.audioBytes) / 1024.0,
                    (unsigned long long)s.replayHits, (unsigned long long)(s.replayHits + s.replayMisses),
                    (unsigned long long)s.resyncs, (unsigned long long)s.errorPackets,
                    (unsigned long long)s.reconnects);
        if (s.extSupported) {
            // 实验扩展：当前编码（带“切换中”标记）、每秒解码帧数、服务端编码耗时，以及最近一个诊断窗口
            const uint64_t frames = audioFrames.load();
            std::printf("      扩展 音频 %s%s · 输出 %llu 帧/s · 编码 %.2f ms/s · 解码错误 %llu",
                        DescribeCodec(s.audioCodec, s.audioKbpsConfig, s.opusComplexity, s.opusFrameMs).c_str(),
                        s.extAudioPending ? "（切换中）" : "", (unsigned long long)(frames - prevFrames),
                        (s.audioEncodeUs - prev.audioEncodeUs) / 1000.0, (unsigned long long)s.audioDecodeErrors);
            prevFrames = frames;
            if (s.diagPackets > 0) {
                const sysdvr::ExtDiag& d = s.diagLast;
                std::printf(" | 诊断#%llu 窗口 %u ms 发 %u 帧 阻塞 %.1f/%.1f ms 超20ms %u 断档 %u 慢发后断档 %u "
                            "音频编码 %.2f%% 核3空闲 %.1f%% CPU %.1f%% TOS %s%s",
                            (unsigned long long)s.diagPackets, d.intervalMs, d.videoFramesSent,
                            d.videoSendBlockTotalUs / 1000.0, d.videoSendBlockMaxUs / 1000.0, d.videoSendsOver20ms,
                            d.videoGrcGaps, d.gapsAfterSlowSend,
                            d.intervalMs > 0 ? d.audioEncodeTotalUs / 10.0 / d.intervalMs : 0.0,
                            Permille(d.core3IdlePermille), Permille(d.sysdvrCpuPermille),
                            (d.tosFlags & 1) ? "视频✓" : "视频✗", (d.tosFlags & 2) ? "音频✓" : "音频✗");
            }
            std::printf("\n");
        }
        std::fflush(stdout);
        prev = s;
    }
    client.Stop();

    const auto s = client.GetStats();
    std::printf("\n合计：视频 %llu 包 %llu 字节，音频 %llu 包 %llu 字节；重放命中 %llu 未命中 %llu；重同步 %llu\n",
                (unsigned long long)s.videoPackets, (unsigned long long)s.videoBytes,
                (unsigned long long)s.audioPackets, (unsigned long long)s.audioBytes,
                (unsigned long long)s.replayHits, (unsigned long long)s.replayMisses, (unsigned long long)s.resyncs);
    std::printf("视频流中出现过 SPS：%s，出现过 IDR：%s，时间戳跨度 %.2f 秒\n", sawSps ? "是" : "否",
                sawIdr ? "是" : "否", (lastVideoTs - firstVideoTs) / 1e6);
    std::printf("Switch 内存：%s\n", sysdvr::DescribeMemoryReport(s.switchMemory).c_str());
    if (!opt.nextExt) {
        std::printf("扩展：未请求\n");
    } else if (!s.extSupported) {
        std::printf("扩展：服务端不支持（按官方 PCM 处理）\n");
    } else {
        const sysdvr::ExtDiagTotals& t = s.diagTotal;
        std::printf("扩展：服务端支持 · 最后音频编码 %s · 解码错误 %llu · 诊断包 %llu 个（累计 %.1f s，发送 %llu 帧，"
                    "阻塞合计 %.1f ms、最长 %.1f ms，超 20 ms %llu 次，grc 断档 %llu，其中发送慢 %llu）\n",
                    DescribeCodec(s.audioCodec, s.audioKbpsConfig, s.opusComplexity, s.opusFrameMs).c_str(),
                    (unsigned long long)s.audioDecodeErrors, (unsigned long long)s.diagPackets, t.intervalMs / 1000.0,
                    (unsigned long long)t.videoFramesSent, t.videoSendBlockTotalUs / 1000.0,
                    t.videoSendBlockMaxUs / 1000.0, (unsigned long long)t.videoSendsOver20ms,
                    (unsigned long long)t.videoGrcGaps, (unsigned long long)t.gapsAfterSlowSend);
    }
    std::printf("音频输出：%llu 帧（%.2f 秒 48 kHz）\n", (unsigned long long)audioFrames.load(), audioFrames.load() / 48000.0);
    std::printf("丢帧检测：断档 %llu 次（估计 %llu 帧）· 错误包 %llu · 缓存未命中 %llu · 失步 %llu · 帧间隔 %.1f ms\n",
                (unsigned long long)lossCount[int(sysdvr::LossCause::TimestampGap)].load(),
                (unsigned long long)lostFrames.load(),
                (unsigned long long)lossCount[int(sysdvr::LossCause::ErrorPacket)].load(),
                (unsigned long long)lossCount[int(sysdvr::LossCause::ReplayMiss)].load(),
                (unsigned long long)lossCount[int(sysdvr::LossCause::Resync)].load(), gapDetector.IntervalUs() / 1000.0);

    if (!gopPath.empty()) {
        FILE* gf = std::fopen(gopPath.c_str(), "wb");
        const auto snapshot = gop.Snapshot();
        for (const auto& pkt : snapshot) std::fwrite(pkt.data.data(), 1, pkt.data.size(), gf);
        std::fclose(gf);
        std::printf("GOP 快照：%zu 包，%zu 字节 → %s\n", snapshot.size(), gop.Bytes(), gopPath.c_str());
    }
    if (vf) std::fclose(vf);
    if (af) std::fclose(af);
    return 0;
}
