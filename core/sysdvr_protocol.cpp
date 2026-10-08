#include "sysdvr_protocol.h"

#include <cstdio>
#include <cstring>

#include "i18n.h"

namespace sysdvr {

namespace {

uint32_t ReadU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t ReadU64(const uint8_t* p) { return uint64_t(ReadU32(p)) | (uint64_t(ReadU32(p + 4)) << 32); }

uint16_t ReadU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

void WriteU32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

}  // namespace

PacketHeader ParseHeader(const uint8_t* p) {
    PacketHeader h;
    h.magic = ReadU32(p);
    h.dataSize = ReadU32(p + 4);
    h.timestampUs = ReadU64(p + 8);
    h.meta = p[16];
    h.replaySlot = p[17];
    return h;
}

bool ValidateHeader(const PacketHeader& h, bool allowDiag) {
    if (h.magic != kPacketMagic) return false;
    if (h.dataSize > kMaxPayload) return false;
    // 扩展诊断包：类型两位都置位，不是重放/错误包，负载至少是 v1 的长度
    if (allowDiag && h.IsDiag()) return !h.IsReplay() && !h.IsError() && h.dataSize >= kExtDiagSize;
    // 恰好是音频或视频之一
    const uint8_t type = h.meta & (kMetaVideo | kMetaAudio);
    if (type != kMetaVideo && type != kMetaAudio) return false;
    // 重放包只允许出现在视频流，且不带负载
    if (h.IsReplay() && (!h.IsVideo() || h.dataSize != 0 || h.replaySlot >= kReplaySlots)) return false;
    return true;
}

bool ParseHello(const uint8_t* p, size_t n, std::string* protocol) {
    static const char kPrefix[] = "SysDVR|";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (n != kHelloSize) return false;
    if (std::memcmp(p, kPrefix, kPrefixLen) != 0) return false;
    if (p[kHelloSize - 1] != 0) return false;
    const char hi = char(p[kPrefixLen]);
    const char lo = char(p[kPrefixLen + 1]);
    if (hi < '0' || hi > '9' || lo < '0' || lo > '9') return false;
    *protocol = std::string{hi, lo};
    return true;
}

bool IsProtocolSupported(const std::string& protocol) { return protocol == "02" || protocol == "03"; }

std::array<uint8_t, kRequestSize> BuildHandshake(const std::string& protocol, StreamKind kind,
                                                 const StreamOptions& opt) {
    std::array<uint8_t, kRequestSize> req{};
    WriteU32(req.data(), kRequestMagic);
    // sysmodule 用 memcmp 比较两个 ASCII 字符，所以按内存顺序直接写入
    req[4] = uint8_t(protocol[0]);
    req[5] = uint8_t(protocol[1]);

    uint8_t metaFlags = 0, videoFlags = 0, audioBatching = 0, featureFlags = 0;
    if (kind == StreamKind::Video) {
        metaFlags = 1 << 0;
        if (opt.nalReplay) videoFlags |= 1 << 0;
        videoFlags |= 1 << 1;  // InjectPPSSPS：让 sysmodule 定期在关键帧前补 SPS/PPS
        if (opt.nalReplayOnlyKeyframes) videoFlags |= 1 << 2;
    } else {
        metaFlags = 1 << 1;
        int b = opt.audioBatching;
        audioBatching = uint8_t(b < 0 ? 0 : (b > kMaxAudioBatching ? kMaxAudioBatching : b));
    }
    if (protocol >= "03" && opt.turnOffConsoleScreen) featureFlags |= 1 << 0;
    // 内存报告只需要一份：只在视频握手里请求（USB 的合并请求也是按视频构造的）
    if (protocol >= "03" && opt.memoryDiag && kind == StreamKind::Video) featureFlags |= 1 << 1;

    // NSDVR 实验扩展：Reserved[0..3] 按 docs/nsdvr-ext-protocol.md 第 1 节解释，Reserved[4..5] 为 0
    if (protocol >= "03" && opt.nextExt) {
        featureFlags |= kFeatureNextExt;
        if (kind == StreamKind::Audio) {
            const ExtAudioConfig c = ClampExtAudioConfig(opt.extAudio);
            req[10] = uint8_t(c.codec);
            req[11] = uint8_t(c.opusKbps / 2);
            req[12] = uint8_t(c.opusComplexity | ((c.opusFrameMs == 10 ? 1 : 0) << 4));
        }
        uint8_t ext = 0;
        if (kind == StreamKind::Video && opt.extDiag) ext |= 1 << 0;
        if (opt.extTos) ext |= 1 << 1;
        req[13] = ext;
    }

    req[6] = metaFlags;
    req[7] = videoFlags;
    req[8] = audioBatching;
    req[9] = featureFlags;
    // 没有扩展时 req[10..15] 保留，为 0
    return req;
}

ExtAudioConfig ClampExtAudioConfig(ExtAudioConfig c) {
    if (uint8_t(c.codec) >= kAudioCodecCount) c.codec = AudioCodec::Pcm48;
    int kbps = c.opusKbps <= 0 ? 96 : c.opusKbps;
    kbps = kbps < 16 ? 16 : (kbps > 256 ? 256 : kbps);
    c.opusKbps = kbps & ~1;
    c.opusComplexity = c.opusComplexity < 0 ? 0 : (c.opusComplexity > 10 ? 10 : c.opusComplexity);
    c.opusFrameMs = c.opusFrameMs == 10 ? 10 : 20;
    return c;
}

int ExtAudioCodecFromSlot(uint8_t replaySlot) {
    if ((replaySlot & 0xF0) != kExtAudioMarkBase) return -1;
    const int codec = replaySlot & 0x0F;
    return codec < kAudioCodecCount ? codec : -2;
}

bool ParseExtAudioHeader(const uint8_t* p, size_t n, ExtAudioHeader* out) {
    if (n < kExtAudioHeaderSize) return false;
    out->version = p[0];
    out->codec = p[1];
    out->complexity = p[2] & 0x0F;
    out->frameCode = p[2] >> 4;
    out->frameCount = p[3];
    out->bitrateKbps = ReadU16(p + 4);
    out->samplesPerChannel = ReadU16(p + 6);
    out->encodeUs = ReadU32(p + 8);
    return out->version == 1;
}

bool ParseExtDiag(const uint8_t* p, size_t n, ExtDiag* out) {
    // 以后的版本只会在末尾加字段：v1 的前 56 字节照样能读
    if (n < kExtDiagSize || p[0] < 1) return false;
    out->version = p[0];
    out->intervalMs = ReadU32(p + 4);
    out->videoFramesSent = ReadU32(p + 8);
    out->videoGrcGaps = ReadU32(p + 12);
    out->videoSendBlockTotalUs = ReadU32(p + 16);
    out->videoSendBlockMaxUs = ReadU32(p + 20);
    out->videoSendsOver20ms = ReadU32(p + 24);
    out->gapsAfterSlowSend = ReadU32(p + 28);
    out->audioPackets = ReadU32(p + 32);
    out->audioEncodeTotalUs = ReadU32(p + 36);
    out->audioSendBlockTotalUs = ReadU32(p + 40);
    out->core3IdlePermille = ReadU32(p + 44);
    out->sysdvrCpuPermille = ReadU32(p + 48);
    out->tosFlags = ReadU32(p + 52);
    return true;
}

std::array<uint8_t, kExtControlSize> BuildExtControl(const ExtAudioConfig& config) {
    const ExtAudioConfig c = ClampExtAudioConfig(config);
    std::array<uint8_t, kExtControlSize> msg{};
    WriteU32(msg.data(), kExtControlMagic);
    msg[4] = uint8_t(c.codec);
    msg[5] = uint8_t(c.opusKbps / 2);
    msg[6] = uint8_t(c.opusComplexity | ((c.opusFrameMs == 10 ? 1 : 0) << 4));
    msg[7] = 0;
    return msg;
}

const char* AudioCodecName(AudioCodec c) {
    switch (c) {
        case AudioCodec::Pcm48: return "PCM";
        case AudioCodec::Pcm24: return "PCM 24 kHz";
        case AudioCodec::Adpcm: return "ADPCM";
        case AudioCodec::Opus: return "Opus";
    }
    return "?";
}

size_t HandshakeResponseSize(const std::string& protocol) { return protocol >= "03" ? 72 : 4; }

uint32_t ParseHandshakeResult(const uint8_t* p) { return ReadU32(p); }

SwitchMemory ParseMemoryReport(const uint8_t* p, size_t n) {
    SwitchMemory m;
    if (n < 72) return m;
    // 布局：u32 Code；packed { u32 QueryResult; u64 Application(Size, Used), Applet(…), System(…), SystemUnsafe(…) }
    m.queryResult = ReadU32(p + 4);
    const uint8_t* q = p + 8;
    m.applicationSize = ReadU64(q);
    m.applicationUsed = ReadU64(q + 8);
    m.appletSize = ReadU64(q + 16);
    m.appletUsed = ReadU64(q + 24);
    m.systemSize = ReadU64(q + 32);
    m.systemUsed = ReadU64(q + 40);
    m.systemUnsafeSize = ReadU64(q + 48);
    m.systemUnsafeUsed = ReadU64(q + 56);
    m.valid = m.queryResult == 0 && m.systemSize > 0 && m.systemUsed <= m.systemSize;
    return m;
}

std::string DescribeMemoryReport(const SwitchMemory& m) {
    if (!m.valid) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), L("未提供（结果码 %08x）", "未提供（結果碼 %08x）", "not provided (result %08x)"),
                      unsigned(m.queryResult));
        return buf;
    }
    auto mb = [](uint64_t b) { return double(b) / (1024.0 * 1024.0); };
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  L("系统池剩余 %.1f MB / 共 %.1f MB · 小程序池剩余 %.1f / %.1f MB · 应用池剩余 %.1f / %.1f MB · "
                    "系统非安全池剩余 %.1f / %.1f MB",
                    "系統池剩餘 %.1f MB / 共 %.1f MB · 小程式池剩餘 %.1f / %.1f MB · 應用程式池剩餘 %.1f / %.1f MB · "
                    "系統非安全池剩餘 %.1f / %.1f MB",
                    "system pool %.1f MB free of %.1f MB · applet %.1f / %.1f MB free · application %.1f / %.1f MB "
                    "free · system unsafe %.1f / %.1f MB free"),
                  mb(m.systemSize - m.systemUsed), mb(m.systemSize), mb(m.appletSize - m.appletUsed), mb(m.appletSize),
                  mb(m.applicationSize - m.applicationUsed), mb(m.applicationSize),
                  mb(m.systemUnsafeSize - m.systemUnsafeUsed), mb(m.systemUnsafeSize));
    return buf;
}

const char* HandshakeResultName(uint32_t code) {
    switch (code) {
        case 0: return "UnknownFailure";
        case 1:
            return L("WrongVersion（客户端与 sysmodule 协议版本不一致）", "WrongVersion（用戶端與 sysmodule 協定版本不一致）",
                     "WrongVersion (client and sysmodule protocol versions differ)");
        case 2: return "InvalidArg";
        case 3: return "InvalidSize";
        case 4: return "InvalidMeta";
        case 5: return "WrongMagic";
        case 6: return "Ok";
        case 7: return "InvalidChannel";
        default: return "Unknown";
    }
}

bool ParseBeacon(const uint8_t* p, size_t n, const std::string& ip, DeviceInfo* out) {
    size_t len = 0;
    while (len < n && p[len] != 0) ++len;
    const std::string s(reinterpret_cast<const char*>(p), len);

    std::string parts[4];
    size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        size_t bar = (i < 3) ? s.find('|', start) : std::string::npos;
        if (i < 3 && bar == std::string::npos) return false;
        parts[i] = s.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        start = bar + 1;
    }
    if (parts[0] != "SysDVR" || parts[2].size() != 2) return false;

    out->ip = ip;
    out->version = parts[1];
    out->protocol = parts[2];
    out->serial = parts[3];
    // 去掉序列号尾部空白
    while (!out->serial.empty() && (out->serial.back() == ' ' || out->serial.back() == '\t'))
        out->serial.pop_back();
    return true;
}

std::string DescribeErrorPacket(const uint8_t* p, size_t n) {
    if (n < 16) return "错误包长度不足";
    const uint32_t type = ReadU32(p);
    const uint32_t code = ReadU32(p + 4);
    const uint64_t ctx1 = ReadU64(p + 8);
    char buf[160];
    switch (type) {
        case 1: std::snprintf(buf, sizeof(buf), "视频采集失败 0x%x（请求大小 0x%llx）", code, (unsigned long long)ctx1); break;
        case 2:
        case 3: std::snprintf(buf, sizeof(buf), "音频采集失败 0x%x（请求大小 0x%llx）", code, (unsigned long long)ctx1); break;
        case 4: std::snprintf(buf, sizeof(buf), "grc:d 视频线程启动失败 0x%x", code); break;
        case 5: std::snprintf(buf, sizeof(buf), "grc:d 音频线程启动失败 0x%x", code); break;
        default: std::snprintf(buf, sizeof(buf), "未知错误 type=%u code=0x%x", type, code); break;
    }
    return buf;
}

bool ContainsNalType(const uint8_t* p, size_t n, int nalType) {
    // 查找 00 00 01 起始码（00 00 00 01 也会被匹配到）
    for (size_t i = 0; i + 3 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if ((p[i + 3] & 0x1F) == nalType) return true;
            i += 2;
        }
    }
    return false;
}

}  // namespace sysdvr
