#include "audio_player.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "i18n.h"
#include "log.h"
#include "sysdvr_protocol.h"

using sysdvr::Logf;
using sysdvr::LogLevel;
using sysdvr::L;
using sysdvr::Format;

namespace {
constexpr size_t kBytesPerMs = size_t(sysdvr::kAudioSampleRate) * sysdvr::kAudioChannels * 2 / 1000;  // 192
// 自适应预缓冲：batching=1 时每包约 43ms，起步攒 80ms；欠载一次 +40ms，最多 400ms；
// 连续 15 秒没有欠载，每 5 秒 -20ms，最少 60ms（再小就兜不住一个包的间隔）
constexpr int kTargetInitialMs = 80;
constexpr int kTargetMinMs = 60;
constexpr int kTargetMaxMs = 400;
constexpr int kTargetStepUpMs = 40;
constexpr int kTargetStepDownMs = 20;
constexpr int64_t kStableBeforeShrinkMs = 15000;
constexpr int64_t kShrinkIntervalMs = 5000;
// 缓冲超过“目标 + 120ms”就丢掉最旧的数据，防止延迟越积越大。
// 实测 200ms 余量时，网络一阵突发就把缓冲顶到 220–250ms，声音明显落后画面
constexpr int kHeadroomMs = 120;
constexpr size_t kRingCapacity = (kTargetMaxMs + kHeadroomMs + 100) * kBytesPerMs;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

AudioPlayer::~AudioPlayer() { Release(); }

bool AudioPlayer::Init(std::string* error) {
    ring_.assign(kRingCapacity, 0);
    targetMs_ = kTargetInitialMs;
    lastUnderrunMs_ = lastAdjustMs_ = NowMs();
    // 优先低时延通路，设备不支持时退回普通通路
    if (TryCreate(true, error)) return true;
    Logf(LogLevel::Warn, "低时延音频不可用（%s），改用普通模式", error->c_str());
    return TryCreate(false, error);
}

bool AudioPlayer::TryCreate(bool fast, std::string* error) {
    OH_AudioStreamBuilder* builder = nullptr;
    if (OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS) {
        *error = L("创建 AudioStreamBuilder 失败", "建立 AudioStreamBuilder 失敗", "failed to create AudioStreamBuilder");
        return false;
    }
    OH_AudioStreamBuilder_SetSamplingRate(builder, sysdvr::kAudioSampleRate);
    OH_AudioStreamBuilder_SetChannelCount(builder, sysdvr::kAudioChannels);
    OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
    OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
    OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_GAME);
    OH_AudioStreamBuilder_SetLatencyMode(builder, fast ? AUDIOSTREAM_LATENCY_MODE_FAST : AUDIOSTREAM_LATENCY_MODE_NORMAL);

    OH_AudioRenderer_Callbacks callbacks;
    callbacks.OH_AudioRenderer_OnWriteData = &AudioPlayer::OnWriteData;
    callbacks.OH_AudioRenderer_OnStreamEvent = &AudioPlayer::OnStreamEvent;
    callbacks.OH_AudioRenderer_OnInterruptEvent = &AudioPlayer::OnInterruptEvent;
    callbacks.OH_AudioRenderer_OnError = &AudioPlayer::OnError;
    OH_AudioStreamBuilder_SetRendererCallback(builder, callbacks, this);

    OH_AudioStream_Result rc = OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer_);
    OH_AudioStreamBuilder_Destroy(builder);
    if (rc != AUDIOSTREAM_SUCCESS || renderer_ == nullptr) {
        *error = Format(L("GenerateRenderer 失败：%d", "GenerateRenderer 失敗：%d", "GenerateRenderer failed: %d"), int(rc));
        renderer_ = nullptr;
        return false;
    }

    rc = OH_AudioRenderer_Start(renderer_);
    if (rc != AUDIOSTREAM_SUCCESS) {
        *error = Format(L("AudioRenderer_Start 失败：%d", "AudioRenderer_Start 失敗：%d", "AudioRenderer_Start failed: %d"), int(rc));
        OH_AudioRenderer_Release(renderer_);
        renderer_ = nullptr;
        return false;
    }
    fastMode_ = fast;
    Logf(LogLevel::Info, "音频输出已启动（%s）", fast ? "低时延模式" : "普通模式");
    return true;
}

void AudioPlayer::Write(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (renderer_ == nullptr) return;

    if (size >= kRingCapacity) {  // 理论上不会发生（单包最大 24KB）
        data += size - kRingCapacity;
        size = kRingCapacity;
    }
    // 长时间稳定：慢慢缩小目标，降低延迟
    const int64_t now = NowMs();
    if (now - lastUnderrunMs_ > kStableBeforeShrinkMs && now - lastAdjustMs_ > kShrinkIntervalMs &&
        targetMs_ > kTargetMinMs) {
        targetMs_ = std::max(kTargetMinMs, targetMs_ - kTargetStepDownMs);
        lastAdjustMs_ = now;
    }

    // 超过上限：从读位置丢掉最旧的数据
    const size_t maxBuffered = size_t(EffectiveTargetMs() + kHeadroomMs) * kBytesPerMs;
    if (size_ + size > maxBuffered) {
        size_t drop = std::min(size_, size_ + size - maxBuffered);
        drop -= drop % 4;  // 保持按帧（左右声道各 2 字节）对齐
        head_ = (head_ + drop) % kRingCapacity;
        size_ -= drop;
        droppedBytes_ += drop;
    }

    size_t tail = (head_ + size_) % kRingCapacity;
    size_t first = std::min(size, kRingCapacity - tail);
    std::memcpy(&ring_[tail], data, first);
    std::memcpy(&ring_[0], data + first, size - first);
    size_ += size;

    if (!primed_ && size_ >= size_t(EffectiveTargetMs()) * kBytesPerMs) primed_ = true;
}

void AudioPlayer::Fill(uint8_t* out, size_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    if (primed_) {
        n = std::min(length, size_);
        size_t first = std::min(n, kRingCapacity - head_);
        std::memcpy(out, &ring_[head_], first);
        std::memcpy(out + first, &ring_[0], n - first);
        head_ = (head_ + n) % kRingCapacity;
        size_ -= n;
        if (n < length) {
            // 欠载：输出静音，加大目标后重新预缓冲
            ++underruns_;
            primed_ = false;
            targetMs_ = std::min(int(maxMs_), targetMs_ + kTargetStepUpMs);
            lastUnderrunMs_ = lastAdjustMs_ = NowMs();
        }
    }
    if (n < length) std::memset(out + n, 0, length - n);
}

int AudioPlayer::EffectiveTargetMs() const {
    return std::min(std::max(int(targetMs_), int(floorMs_)), int(maxMs_));
}

int AudioPlayer::BufferedMs() {
    std::lock_guard<std::mutex> lock(mutex_);
    return int(size_ / kBytesPerMs);
}

void AudioPlayer::Release() {
    OH_AudioRenderer* renderer = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        renderer = renderer_;
        renderer_ = nullptr;
    }
    if (renderer != nullptr) {
        OH_AudioRenderer_Stop(renderer);
        OH_AudioRenderer_Release(renderer);
    }
}

int32_t AudioPlayer::OnWriteData(OH_AudioRenderer*, void* userData, void* buffer, int32_t length) {
    static_cast<AudioPlayer*>(userData)->Fill(static_cast<uint8_t*>(buffer), size_t(length));
    return 0;
}

int32_t AudioPlayer::OnStreamEvent(OH_AudioRenderer*, void*, OH_AudioStream_Event event) {
    Logf(LogLevel::Info, "音频流事件：%d", int(event));
    return 0;
}

int32_t AudioPlayer::OnInterruptEvent(OH_AudioRenderer*, void*, OH_AudioInterrupt_ForceType type,
                                      OH_AudioInterrupt_Hint hint) {
    // PoC 只记录；来电等强制打断由系统处理，恢复后继续取数
    Logf(LogLevel::Info, "音频焦点变化：type=%d hint=%d", int(type), int(hint));
    return 0;
}

int32_t AudioPlayer::OnError(OH_AudioRenderer*, void*, OH_AudioStream_Result error) {
    Logf(LogLevel::Error, "音频输出错误：%d", int(error));
    return 0;
}
