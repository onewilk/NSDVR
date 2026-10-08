// 画质增强：系统视频处理引擎的“细节增强”（VPE Detail Enhancer，API 12+）接在解码器和屏幕之间，
// 把 Switch 的 720p 画面放大到屏幕上 16:9 区域的实际像素并增强细节。
//   解码器 → InputWindow()（增强器的输入 surface）→ 增强 + 放大 → 屏幕（XComponent 的窗口）
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

struct OH_VideoProcessing;
struct VideoProcessing_Callback;
struct NativeWindow;
typedef struct NativeWindow OHNativeWindow;

class VideoEnhancer {
public:
    ~VideoEnhancer();

    // outWindow：屏幕窗口（调用方负责它的生命周期，且必须比本对象活得久）
    // level：1 标准（速度快）/ 2 高（效果好、更慢）
    bool Init(OHNativeWindow* outWindow, int level, std::string* error);
    // 解码器应输出到这个窗口
    OHNativeWindow* InputWindow() const { return input_; }
    void SetLevel(int level);
    int Level() const { return level_; }
    void Release();

    // 解码器每送出一帧调用：用 FIFO 估算“送进增强器 → 增强完上屏”的耗时
    void OnFrameQueued();
    int AvgLatencyUs() const { return avgLatencyUs_; }
    uint64_t FramesOut() const { return framesOut_; }
    std::string LastError();

    // 以下由 VPE 回调（.cpp 里的静态函数）转调
    void HandleOutput(uint32_t index);
    void HandleError(int error);

private:

    OH_VideoProcessing* vp_ = nullptr;
    VideoProcessing_Callback* callback_ = nullptr;
    OHNativeWindow* input_ = nullptr;
    std::atomic<int> level_{0};

    std::mutex mutex_;
    std::deque<int64_t> queuedNs_;
    std::string lastError_;
    std::atomic<int> avgLatencyUs_{0};
    std::atomic<uint64_t> framesOut_{0};
};
