// SysDVR TCP Bridge / USB 通用协议定义（与平台无关）。
// 参考：SysDVR protocol.md、sysmodule/source/modes/proto.h、sysmodule/source/capture.h
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sysdvr {

constexpr uint16_t kVideoPort = 9911;
constexpr uint16_t kAudioPort = 9922;
constexpr uint16_t kDiscoveryPort = 19999;

constexpr uint32_t kPacketMagic = 0xCCCCCCCC;
constexpr uint32_t kRequestMagic = 0xAAAAAAAA;
constexpr uint32_t kHandshakeOk = 6;

constexpr size_t kHeaderSize = 18;
constexpr size_t kHelloSize = 10;  // "SysDVR|03\0"
constexpr size_t kRequestSize = 16;

// 视频单包最大负载（sysmodule 的 VbufSz），也是协议允许的最大负载
constexpr size_t kMaxPayload = 0x54000;
constexpr int kMaxAudioBatching = 5;
constexpr int kReplaySlots = 64;  // sysmodule HASH_BITS = 6
constexpr uint8_t kNoReplaySlot = 0xFF;

constexpr int kVideoWidth = 1280;
constexpr int kVideoHeight = 720;
constexpr int kAudioSampleRate = 48000;
constexpr int kAudioChannels = 2;

// Switch 固定的 SPS/PPS（与 sysmodule capture.c、客户端 StreamInfo.cs 一致）
constexpr uint8_t kSps[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B,
                            0x40, 0x28, 0x02, 0xDD, 0x35, 0x01, 0x0D, 0x01, 0xE0, 0x80};
constexpr uint8_t kPps[] = {0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0};

enum class StreamKind { Video, Audio };

// PacketHeader.MetaData 位定义
enum PacketMeta : uint8_t {
    kMetaVideo = 1 << 0,
    kMetaAudio = 1 << 1,
    kMetaData = 1 << 2,
    kMetaReplay = 1 << 3,
    kMetaMultiNal = 1 << 4,
    kMetaError = 1 << 5,
};

struct PacketHeader {
    uint32_t magic = 0;
    uint32_t dataSize = 0;
    uint64_t timestampUs = 0;
    uint8_t meta = 0;
    uint8_t replaySlot = kNoReplaySlot;

    bool IsVideo() const { return meta & kMetaVideo; }
    bool IsAudio() const { return meta & kMetaAudio; }
    bool IsReplay() const { return meta & kMetaReplay; }
    bool IsError() const { return meta & kMetaError; }
    // 扩展协议的诊断包：类型两位都置位（官方没有用到这个组合）。注意此时 IsVideo()/IsAudio() 也都为真，要先判断它
    bool IsDiag() const { return (meta & (kMetaVideo | kMetaAudio)) == (kMetaVideo | kMetaAudio); }
};

// ---------------------------------------------------------------------------
// NSDVR 实验扩展协议 v1（docs/nsdvr-ext-protocol.md），只用于 TCP Bridge、协议 03。
// 官方 SysDVR 忽略未知的 FeatureFlags 位和 Reserved 字节（上游 ProtoHandshakeVersion 不检查它们），行为不变。

constexpr uint8_t kFeatureNextExt = 1 << 2;           // ExtraFeatureFlags_NsdvrExt
constexpr uint32_t kExtControlMagic = 0x58564453;     // 控制消息 magic，字节序列 "SDVX"
constexpr size_t kExtControlSize = 8;
constexpr size_t kExtAudioHeaderSize = 12;
constexpr size_t kExtDiagSize = 56;
constexpr uint8_t kExtAudioMarkBase = 0xE0;           // 音频包 ReplaySlot = 0xE0 | codec
constexpr uint32_t kExtUnavailable = 0xFFFFFFFF;       // 诊断字段“拿不到”

enum class AudioCodec : uint8_t { Pcm48 = 0, Pcm24 = 1, Adpcm = 2, Opus = 3 };
constexpr int kAudioCodecCount = 4;

// 客户端想要的音频编码与 Opus 参数（握手 Reserved[0..2] 和控制消息共用）
struct ExtAudioConfig {
    AudioCodec codec = AudioCodec::Pcm48;
    int opusKbps = 96;       // 协议单位 2 kbps；服务端限制在 16–256
    int opusComplexity = 5;  // 0–10
    int opusFrameMs = 20;    // 20 / 10
};

// 夹到协议允许的范围（码率取偶数、帧长只有 10/20）
ExtAudioConfig ClampExtAudioConfig(ExtAudioConfig c);

// 压缩音频包负载前的 12 字节头
struct ExtAudioHeader {
    uint8_t version = 0;
    uint8_t codec = 0;
    uint8_t complexity = 0;       // 低 4 位
    uint8_t frameCode = 0;        // 高 4 位：0 = 20 ms，1 = 10 ms
    uint8_t frameCount = 0;
    uint16_t bitrateKbps = 0;
    uint16_t samplesPerChannel = 0;
    uint32_t encodeUs = 0;

    int FrameMs() const { return frameCode == 1 ? 10 : 20; }
};

// 诊断包负载 ExtDiag v1（服务端约每秒一个，视频路）
struct ExtDiag {
    uint8_t version = 0;
    uint32_t intervalMs = 0;
    uint32_t videoFramesSent = 0;
    uint32_t videoGrcGaps = 0;
    uint32_t videoSendBlockTotalUs = 0;
    uint32_t videoSendBlockMaxUs = 0;
    uint32_t videoSendsOver20ms = 0;
    uint32_t gapsAfterSlowSend = 0;
    uint32_t audioPackets = 0;
    uint32_t audioEncodeTotalUs = 0;
    uint32_t audioSendBlockTotalUs = 0;
    uint32_t core3IdlePermille = kExtUnavailable;
    uint32_t sysdvrCpuPermille = kExtUnavailable;
    uint32_t tosFlags = 0;  // bit0 视频 socket 设置成功，bit1 音频 socket
};

// 音频包 ReplaySlot 的含义：-1 = 官方（0xFF 等，按 PCM48 处理），0–3 = 扩展编码，-2 = 扩展范围内的未知编码（丢弃）
int ExtAudioCodecFromSlot(uint8_t replaySlot);
bool ParseExtAudioHeader(const uint8_t* p, size_t n, ExtAudioHeader* out);
bool ParseExtDiag(const uint8_t* p, size_t n, ExtDiag* out);
// 运行中切换音频编码的 8 字节控制消息（音频 socket 上行）
std::array<uint8_t, kExtControlSize> BuildExtControl(const ExtAudioConfig& c);
const char* AudioCodecName(AudioCodec c);

struct StreamOptions {
    bool video = true;
    bool audio = true;
    // 静态画面时 sysmodule 只发一个“重放槽位号”而不重发大关键帧
    bool nalReplay = true;
    bool nalReplayOnlyKeyframes = true;
    // 每包额外合并的音频块数（0~5），越大越省带宽、延迟越高
    int audioBatching = 3;
    // 仅协议 03：串流时关闭 Switch 屏幕
    bool turnOffConsoleScreen = false;
    // 仅协议 03：视频握手时请求内存报告（ExtraFeatureFlags_MemoryDiag）。sysmodule 只做
    // svcGetSystemInfo 查询、没有副作用，用来评估 Switch 上还剩多少系统内存
    bool memoryDiag = true;
    // 仅 TCP Bridge、协议 03：请求 NSDVR 实验扩展（ExtraFeatureFlags_NsdvrExt）。
    // 官方 SysDVR 忽略这些位，所以可以一直开着；USB / RTSP 不发
    bool nextExt = false;
    ExtAudioConfig extAudio;  // 音频路：初始编码与 Opus 参数
    bool extDiag = true;      // 视频路：每秒发诊断包
    bool extTos = true;       // 两路：对 socket 设置 IP_TOS（WMM 视频队列）
};

// 协议 03 握手应答里的 Switch 内存报告：各物理内存池的总量与已用（字节）
struct SwitchMemory {
    bool valid = false;                 // 收到了有效报告（协议 03、sysmodule 处理了请求、查询成功）
    uint32_t queryResult = 0xFFFFFFFF;  // libnx 结果码：0 成功；UINT32_MAX 表示没有请求/sysmodule 不支持
    uint64_t applicationSize = 0, applicationUsed = 0;
    uint64_t appletSize = 0, appletUsed = 0;
    uint64_t systemSize = 0, systemUsed = 0;
    uint64_t systemUnsafeSize = 0, systemUnsafeUsed = 0;
};

struct DeviceInfo {
    std::string ip;
    std::string version;   // 如 "6.3"
    std::string protocol;  // 如 "03"
    std::string serial;
};

// 小端解析 18 字节包头；只做格式解析，合法性用 ValidateHeader 判断
PacketHeader ParseHeader(const uint8_t* p);
// allowDiag：接受扩展协议的诊断包（只有请求了扩展的 TCP 视频路才传 true；默认与官方完全一致）
bool ValidateHeader(const PacketHeader& h, bool allowDiag = false);

// 解析 "SysDVR|03\0"，成功时输出两位协议版本号
bool ParseHello(const uint8_t* p, size_t n, std::string* protocol);
bool IsProtocolSupported(const std::string& protocol);

// 构造握手请求；TCP Bridge 每个 socket 只能订阅自己那一路
std::array<uint8_t, kRequestSize> BuildHandshake(const std::string& protocol, StreamKind kind,
                                                 const StreamOptions& opt);
// 握手应答长度：02 为 4 字节，03 为 72 字节（结果码 + 可选内存报告）
size_t HandshakeResponseSize(const std::string& protocol);
uint32_t ParseHandshakeResult(const uint8_t* p);
// 解析 72 字节握手应答（p 指向应答开头）里的内存报告；n 不足 72 字节时返回 valid=false
SwitchMemory ParseMemoryReport(const uint8_t* p, size_t n);
// 日志用的一行描述，例如 “系统池剩余 4.1 MB / 共 … MB · 小程序池 … · 应用池 …”
std::string DescribeMemoryReport(const SwitchMemory& m);
const char* HandshakeResultName(uint32_t code);

// 解析 UDP 19999 广播：SysDVR|6.3|03|SERIAL（可能带 NUL 填充）
bool ParseBeacon(const uint8_t* p, size_t n, const std::string& ip, DeviceInfo* out);

// 错误包（MetaData 带 kMetaError）的可读描述
std::string DescribeErrorPacket(const uint8_t* p, size_t n);

// H.264 Annex-B 工具：遍历 NAL 类型
bool ContainsNalType(const uint8_t* p, size_t n, int nalType);

// 逐个回调 Annex-B 字节流里的 NAL：fn(含起始码的 NAL 指针, 长度, nal_unit_type)
template <typename Fn>
void ForEachNal(const uint8_t* p, size_t n, Fn&& fn) {
    size_t nalStart = n;  // n 表示“还没遇到第一个起始码”
    size_t header = 0;
    size_t i = 0;
    while (i + 3 <= n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            const size_t startCode = (i > 0 && p[i - 1] == 0) ? i - 1 : i;  // 兼容 4 字节起始码
            if (nalStart < n) fn(p + nalStart, startCode - nalStart, p[header] & 0x1F);
            nalStart = startCode;
            header = i + 3;
            i += 3;
        } else {
            ++i;
        }
    }
    if (nalStart < n && header < n) fn(p + nalStart, n - nalStart, p[header] & 0x1F);
}

}  // namespace sysdvr
