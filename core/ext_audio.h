// NSDVR 实验扩展的音频解码（docs/nsdvr-ext-protocol.md 第 3 节）：按音频包 ReplaySlot 上的编码标记，
// 把 PCM48 / PCM24 / IMA ADPCM / Opus 统一还原成 48 kHz 立体声 s16 交错，交给原有的播放器。
// 与平台无关。Opus 用 libopus：鸿蒙端把解码部分编进 libentry.so（third_party/opus），Mac 上链接 Homebrew 的 libopus；
// 编译时没有定义 SYSDVR_WITH_OPUS 的话，Opus 包按解码失败处理（输出为空）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "sysdvr_protocol.h"

namespace sysdvr {

// 24 kHz → 48 kHz 立体声升采样：补零 + 127 抽头低通（Kaiser β=8，截止 11.5 kHz），滤波器状态跨包保留。
// 系数与 tools/audio_codec_eval.py 的 UPSAMPLE_127 逐位相同，双精度累加、四舍六入五成双，
// 所以输出与参考实现（numpy 'same' 卷积）逐采样一致，只是因果实现晚 kDelay 个采样（约 1.3 ms）
class Upsampler2x {
public:
    static constexpr int kTaps = 127;
    static constexpr int kDelay = 63;  // 48 kHz 采样数

    Upsampler2x() { Reset(); }
    void Reset();
    // in：frames 帧 24 kHz 立体声交错；向 out 追加 2 × frames 帧 48 kHz 立体声交错
    void Process(const int16_t* in, size_t frames, std::vector<int16_t>* out);
    // 第 j 个抽头（0–126），单测对照用
    static double Coefficient(int j);

private:
    static constexpr int kHistory = 63;  // 偶数相位 64 个抽头，需要前 63 个输入采样
    std::vector<double> buf_[2];         // 每声道：kHistory 个历史采样 + 本包输入
};

// IMA ADPCM（标准步长表/索引表，与 tools/audio_codec_eval.py 一致）
struct AdpcmChannelState {
    int predictor = 0;
    int stepIndex = 0;
};
int AdpcmDecodeNibble(AdpcmChannelState* st, uint8_t code);
// 解一个 ADPCM 包的数据体：2 × 4 字节声道状态 + samplesPerChannel 字节（低 4 位左声道、高 4 位右声道）。
// 每包都按包头状态重新同步，前一个包丢了也不影响本包。成功时向 out 追加 samplesPerChannel 帧
bool DecodeAdpcmBody(const uint8_t* body, size_t n, int samplesPerChannel, std::vector<int16_t>* out);

// 一个音频包的解析结果（给统计用）
struct AudioPacketInfo {
    int codec = -1;          // -1 = 没有扩展标记（官方 PCM），0–3 = AudioCodec
    bool hasHeader = false;  // 带 ExtAudioHeader（压缩编码）
    ExtAudioHeader header;
};

class OpusStream;  // libopus 解码器的薄封装，定义在 ext_audio.cpp

class ExtAudioDecoder {
public:
    ExtAudioDecoder();
    ~ExtAudioDecoder();
    ExtAudioDecoder(const ExtAudioDecoder&) = delete;
    ExtAudioDecoder& operator=(const ExtAudioDecoder&) = delete;

    // 解码一个音频包。PCM48（官方 0xFF 或扩展 0xE0）不拷贝，*pcm 直接指向负载；其他编码解到内部缓冲，
    // 在下一次调用前有效。返回 false 表示包损坏或编码不认识（*pcm 为空，计入 DecodeErrors）
    bool Decode(uint8_t replaySlot, const uint8_t* payload, size_t size, AudioPacketInfo* info, const uint8_t** pcm,
                size_t* pcmBytes);
    // 重新连接后调用：清掉滤波器、Opus 解码器的历史
    void Reset();
    uint64_t DecodeErrors() const { return errors_; }
    // 本次编译是否带 Opus 解码
    static bool OpusAvailable();

private:
    bool DecodeCompressed(AudioCodec codec, const ExtAudioHeader& h, const uint8_t* body, size_t n);

    Upsampler2x up_;
    std::unique_ptr<OpusStream> opus_;
    std::vector<int16_t> out_;
    std::vector<int16_t> frame_;  // Opus 单帧解码缓冲
    int lastCodec_ = -1;
    uint64_t errors_ = 0;
};

}  // namespace sysdvr
