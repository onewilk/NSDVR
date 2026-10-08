// 48kHz / 16bit / 立体声 PCM 播放（OHAudio），网络包进环形缓冲，系统回调按需取数。
#pragma once

#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class AudioPlayer {
public:
    ~AudioPlayer();

    bool Init(std::string* error);
    // 网络线程调用
    void Write(const uint8_t* data, size_t size);
    void Release();

    uint64_t Underruns() const { return underruns_; }
    uint64_t DroppedBytes() const { return droppedBytes_; }
    int BufferedMs();
    // 自适应预缓冲目标：欠载时加大，长时间稳定后减小；实际生效值还受下限/上限约束
    int TargetMs() const { return EffectiveTargetMs(); }
    // 音画对齐：视频开了平滑缓冲时，音频至少缓冲同样久，避免声音跑在画面前面
    void SetFloorMs(int ms) { floorMs_ = ms; }
    // 上限：游戏档压低，观看档放宽
    void SetMaxMs(int ms) { maxMs_ = ms; }
    bool FastMode() const { return fastMode_; }

private:
    static int32_t OnWriteData(OH_AudioRenderer* renderer, void* userData, void* buffer, int32_t length);
    static int32_t OnStreamEvent(OH_AudioRenderer* renderer, void* userData, OH_AudioStream_Event event);
    static int32_t OnInterruptEvent(OH_AudioRenderer* renderer, void* userData, OH_AudioInterrupt_ForceType type,
                                    OH_AudioInterrupt_Hint hint);
    static int32_t OnError(OH_AudioRenderer* renderer, void* userData, OH_AudioStream_Result error);

    bool TryCreate(bool fast, std::string* error);
    int EffectiveTargetMs() const;
    void Fill(uint8_t* out, size_t length);

    OH_AudioRenderer* renderer_ = nullptr;
    bool fastMode_ = false;

    std::mutex mutex_;
    std::vector<uint8_t> ring_;
    size_t head_ = 0;  // 读位置
    size_t size_ = 0;  // 已缓冲字节数
    bool primed_ = false;
    std::atomic<int> targetMs_{0};
    std::atomic<int> floorMs_{0};
    std::atomic<int> maxMs_{400};
    int64_t lastUnderrunMs_ = 0;
    int64_t lastAdjustMs_ = 0;

    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> droppedBytes_{0};
};
