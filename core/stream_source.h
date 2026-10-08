// 串流来源的公共接口：TCP Bridge 和 USB 两种传输共用同一套包分发逻辑。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ext_audio.h"
#include "frame_loss.h"
#include "sysdvr_protocol.h"

namespace sysdvr {

// 诊断包各字段的累计值（给 CSV 算每秒增量；最长阻塞取整个会话的最大值）
struct ExtDiagTotals {
    uint64_t intervalMs = 0;
    uint64_t videoFramesSent = 0;
    uint64_t videoGrcGaps = 0;
    uint64_t videoSendBlockTotalUs = 0;
    uint64_t videoSendBlockMaxUs = 0;
    uint64_t videoSendsOver20ms = 0;
    uint64_t gapsAfterSlowSend = 0;
    uint64_t audioPackets = 0;
    uint64_t audioEncodeTotalUs = 0;
    uint64_t audioSendBlockTotalUs = 0;
};

struct BridgeStats {
    bool videoConnected = false;
    bool audioConnected = false;
    uint64_t videoPackets = 0;
    uint64_t videoBytes = 0;
    uint64_t audioPackets = 0;
    uint64_t audioBytes = 0;
    uint64_t replayHits = 0;
    uint64_t replayMisses = 0;
    uint64_t resyncs = 0;
    uint64_t errorPackets = 0;
    uint64_t reconnects = 0;
    // 最近一次视频握手拿到的 Switch 内存报告（协议 03），重连后刷新
    SwitchMemory switchMemory;

    // ---- NSDVR 实验扩展（只有扩展版服务端才有这些数据；官方服务端下保持默认值）
    bool extSupported = false;     // 本次会话见到过扩展音频标记（0xE0–0xE3）或诊断包
    int audioCodec = -1;           // 最近一个音频包的编码：-1 = 没有扩展标记（官方 PCM），0–3 = AudioCodec
    int audioKbpsConfig = 0;       // 服务端配置的码率（ExtAudioHeader.bitrateKbps；扩展 PCM48 记 1536）
    int opusComplexity = -1;       // 仅 Opus：最近一包的复杂度、帧长（ms）；其他编码为 -1 / 0
    int opusFrameMs = 0;
    uint64_t audioEncodeUs = 0;    // 累计：各音频包 ExtAudioHeader.encodeUs 之和（服务端编码耗时）
    uint64_t audioDecodeErrors = 0;
    ExtAudioConfig extAudioRequested;  // 客户端最近一次请求的编码（握手或控制消息）
    bool extAudioPending = false;  // 请求的编码还没在音频包标记上生效
    uint64_t diagPackets = 0;
    ExtDiag diagLast;              // 最近一个诊断窗口
    ExtDiagTotals diagTotal;       // 诊断累计
};

// 最近一包的编码、参数是否就是请求的那一组（控制消息没有应答，靠包标记和 ExtAudioHeader 确认）
bool ExtAudioMatches(const ExtAudioConfig& want, const BridgeStats& s);

// data 只在回调期间有效，需要保留的话由调用方自行拷贝
using PacketFn = std::function<void(const uint8_t* data, size_t size, uint64_t timestampUs)>;
using StatusFn = std::function<void(const std::string& message)>;
using LossFn = std::function<void(LossCause cause)>;

struct StreamCallbacks {
    PacketFn onVideo;
    PacketFn onAudio;
    StatusFn onStatus;
    // 发现视频数据丢失（错误包、重放缓存未命中、包头失步），与 onVideo 在同一个线程调用；
    // 时间戳断档由使用方根据 onVideo 的时间戳自己检测
    LossFn onVideoLoss;
};

class StreamSource {
public:
    virtual ~StreamSource() = default;
    virtual void Start() = 0;
    // 阻塞到接收线程退出；返回后不会再有回调
    virtual void Stop() = 0;
    virtual BridgeStats GetStats() const = 0;
    // 运行中开关音频
    virtual void SetAudioEnabled(bool enabled) = 0;
    // 网络相关的可选能力，USB 下没有意义
    virtual void SetQuickAck(bool) {}
    virtual std::vector<int> SocketFds() const { return {}; }
    // 实验扩展：运行中切换音频编码/Opus 参数。返回 true 表示已发出控制消息；
    // false 表示服务端（还）没表明支持扩展或音频没连上——请求会记下来，下次音频握手时带上
    virtual bool SetExtAudio(const ExtAudioConfig&) { return false; }
};

// 收到一个完整包（包头 + 负载）后的公共处理：错误包记录、视频 NAL 重放表、统计、回调分发。
// 视频相关状态只在一个线程里访问；计数器是原子的，可以跨线程读。
class PacketDispatcher {
public:
    PacketDispatcher(const StreamOptions& options, const StreamCallbacks& callbacks)
        : options_(options), callbacks_(callbacks) {}

    void Dispatch(const PacketHeader& h, const uint8_t* payload);
    // 重新连接后 sysmodule 会清空哈希表，客户端的重放表也要清
    void ResetReplay();
    // 包头失步、丢了一段字节：计数，视频通道的还要通知上层（之后的 P 帧参考链可能已断）
    void NotifyResync(bool videoChannel);
    void SetAudioMuted(bool muted) { audioMuted_ = muted; }
    void FillStats(BridgeStats* s) const;
    // 视频握手时拿到的 Switch 内存报告（握手线程写，统计线程读）
    void SetSwitchMemory(const SwitchMemory& m);
    // 音频重新连接：清掉解码器历史（音频线程调用）
    void ResetAudio() { audioDecoder_.Reset(); }
    // 服务端表明支持实验扩展（见到扩展音频标记或诊断包）
    bool ExtSupported() const { return extSeen_; }

    std::atomic<uint64_t> resyncs{0};
    std::atomic<uint64_t> reconnects{0};

private:
    void DispatchAudio(const PacketHeader& h, const uint8_t* payload);
    void DispatchDiag(const PacketHeader& h, const uint8_t* payload);

    const StreamOptions& options_;
    const StreamCallbacks& callbacks_;
    std::vector<uint8_t> replay_[kReplaySlots];
    // 音频：按扩展标记解码成 48 kHz PCM（官方包零拷贝透传）；只在音频线程里用
    ExtAudioDecoder audioDecoder_;
    std::atomic<bool> extSeen_{false};
    std::atomic<int> audioCodec_{-1}, audioKbps_{0}, opusComplexity_{-1}, opusFrameMs_{0};
    std::atomic<uint64_t> audioEncodeUs_{0}, audioDecodeErrors_{0};
    // 诊断包（视频线程写，统计线程读）
    mutable std::mutex diagMutex_;
    uint64_t diagPackets_ = 0;
    ExtDiag diagLast_;
    ExtDiagTotals diagTotal_;
    std::atomic<bool> audioMuted_{false};
    std::atomic<uint64_t> videoPackets_{0}, videoBytes_{0}, audioPackets_{0}, audioBytes_{0};
    std::atomic<uint64_t> replayHits_{0}, replayMisses_{0}, errorPackets_{0};
    mutable std::mutex memoryMutex_;
    SwitchMemory switchMemory_;
};

}  // namespace sysdvr
