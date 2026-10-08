#include "stream_source.h"

#include "log.h"

namespace sysdvr {

void PacketDispatcher::Dispatch(const PacketHeader& h, const uint8_t* payload) {
    if (h.IsError()) {
        ++errorPackets_;
        Logf(LogLevel::Warn, "sysmodule 报告：%s", DescribeErrorPacket(payload, h.dataSize).c_str());
        // 视频错误包顶替了本该发来的那一帧（例如关键帧太大读取失败）
        if (h.IsVideo() && callbacks_.onVideoLoss) callbacks_.onVideoLoss(LossCause::ErrorPacket);
        return;
    }
    // 扩展诊断包：类型两位都置位，IsVideo() 也为真，必须先分出来（只有请求了扩展的 TCP 视频路会放行它）
    if (h.IsDiag()) {
        DispatchDiag(h, payload);
        return;
    }

    if (h.IsVideo()) {
        const uint8_t* data = payload;
        size_t size = h.dataSize;
        if (h.IsReplay()) {
            const auto& cached = replay_[h.replaySlot];
            if (cached.empty()) {
                ++replayMisses_;
                if (callbacks_.onVideoLoss) callbacks_.onVideoLoss(LossCause::ReplayMiss);
                return;
            }
            ++replayHits_;
            data = cached.data();
            size = cached.size();
        } else if (options_.nalReplay && h.replaySlot < kReplaySlots) {
            replay_[h.replaySlot].assign(data, data + size);
        }
        ++videoPackets_;
        videoBytes_ += size;
        if (callbacks_.onVideo && size > 0) callbacks_.onVideo(data, size, h.timestampUs);
    } else {
        DispatchAudio(h, payload);
    }
}

void PacketDispatcher::DispatchAudio(const PacketHeader& h, const uint8_t* payload) {
    ++audioPackets_;
    audioBytes_ += h.dataSize;  // 线上字节数（压缩后），就是实际音频码率
    if (h.dataSize == 0) return;

    AudioPacketInfo info;
    const uint8_t* pcm = nullptr;
    size_t pcmBytes = 0;
    const bool ok = audioDecoder_.Decode(h.replaySlot, payload, h.dataSize, &info, &pcm, &pcmBytes);
    audioDecodeErrors_ = audioDecoder_.DecodeErrors();

    if (info.codec != -1) {
        // 0xE0–0xEF：扩展版服务端（官方对音频一律写 0xFF）
        if (!extSeen_.exchange(true))
            Logf(LogLevel::Info, "服务端支持 NSDVR 实验扩展（音频标记 0x%02x）", unsigned(h.replaySlot));
    }
    if (info.codec >= 0) {
        const bool opus = info.codec == int(AudioCodec::Opus);
        const int kbps = info.hasHeader ? int(info.header.bitrateKbps) : 1536;
        const int complexity = opus ? int(info.header.complexity) : -1;
        const int frameMs = opus ? info.header.FrameMs() : 0;
        if (audioCodec_ != info.codec || audioKbps_ != kbps || opusComplexity_ != complexity || opusFrameMs_ != frameMs) {
            if (opus)
                Logf(LogLevel::Info, "音频编码生效：Opus %d kbps · 复杂度 %d · 帧长 %d ms", kbps, complexity, frameMs);
            else
                Logf(LogLevel::Info, "音频编码生效：%s（%d kbps）", AudioCodecName(AudioCodec(info.codec)), kbps);
        }
        audioCodec_ = info.codec;
        audioKbps_ = kbps;
        opusComplexity_ = complexity;
        opusFrameMs_ = frameMs;
        if (info.hasHeader) audioEncodeUs_ += info.header.encodeUs;
    } else if (info.codec == -1) {
        audioCodec_ = -1;
    }

    if (ok && pcmBytes > 0 && !audioMuted_ && callbacks_.onAudio) callbacks_.onAudio(pcm, pcmBytes, h.timestampUs);
}

void PacketDispatcher::DispatchDiag(const PacketHeader& h, const uint8_t* payload) {
    ExtDiag d;
    if (!ParseExtDiag(payload, h.dataSize, &d)) {
        Logf(LogLevel::Warn, "诊断包格式不对（%u 字节，版本 %u），忽略", h.dataSize, h.dataSize > 0 ? unsigned(payload[0]) : 0u);
        return;
    }
    if (!extSeen_.exchange(true)) Logf(LogLevel::Info, "服务端支持 NSDVR 实验扩展（收到诊断包）");
    std::lock_guard<std::mutex> lock(diagMutex_);
    ++diagPackets_;
    diagLast_ = d;
    ExtDiagTotals& t = diagTotal_;
    t.intervalMs += d.intervalMs;
    t.videoFramesSent += d.videoFramesSent;
    t.videoGrcGaps += d.videoGrcGaps;
    t.videoSendBlockTotalUs += d.videoSendBlockTotalUs;
    if (d.videoSendBlockMaxUs > t.videoSendBlockMaxUs) t.videoSendBlockMaxUs = d.videoSendBlockMaxUs;
    t.videoSendsOver20ms += d.videoSendsOver20ms;
    t.gapsAfterSlowSend += d.gapsAfterSlowSend;
    t.audioPackets += d.audioPackets;
    t.audioEncodeTotalUs += d.audioEncodeTotalUs;
    t.audioSendBlockTotalUs += d.audioSendBlockTotalUs;
}

bool ExtAudioMatches(const ExtAudioConfig& want, const BridgeStats& s) {
    const ExtAudioConfig w = ClampExtAudioConfig(want);
    if (s.audioCodec != int(w.codec)) return false;
    if (w.codec != AudioCodec::Opus) return true;
    return s.audioKbpsConfig == w.opusKbps && s.opusComplexity == w.opusComplexity && s.opusFrameMs == w.opusFrameMs;
}

void PacketDispatcher::NotifyResync(bool videoChannel) {
    ++resyncs;
    if (videoChannel && callbacks_.onVideoLoss) callbacks_.onVideoLoss(LossCause::Resync);
}

void PacketDispatcher::ResetReplay() {
    for (auto& slot : replay_) slot.clear();
}

void PacketDispatcher::FillStats(BridgeStats* s) const {
    s->videoPackets = videoPackets_;
    s->videoBytes = videoBytes_;
    s->audioPackets = audioPackets_;
    s->audioBytes = audioBytes_;
    s->replayHits = replayHits_;
    s->replayMisses = replayMisses_;
    s->errorPackets = errorPackets_;
    s->resyncs = resyncs;
    s->reconnects = reconnects;
    s->extSupported = extSeen_;
    s->audioCodec = audioCodec_;
    s->audioKbpsConfig = audioKbps_;
    s->opusComplexity = opusComplexity_;
    s->opusFrameMs = opusFrameMs_;
    s->audioEncodeUs = audioEncodeUs_;
    s->audioDecodeErrors = audioDecodeErrors_;
    {
        std::lock_guard<std::mutex> lock(diagMutex_);
        s->diagPackets = diagPackets_;
        s->diagLast = diagLast_;
        s->diagTotal = diagTotal_;
    }
    std::lock_guard<std::mutex> lock(memoryMutex_);
    s->switchMemory = switchMemory_;
}

void PacketDispatcher::SetSwitchMemory(const SwitchMemory& m) {
    std::lock_guard<std::mutex> lock(memoryMutex_);
    switchMemory_ = m;
}

}  // namespace sysdvr
