#include "video_enhancer.h"

#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/video_processing_engine/video_processing.h>
#include <multimedia/video_processing_engine/video_processing_types.h>
#include <native_window/external_window.h>

#include <chrono>

#include "i18n.h"
#include "log.h"

using sysdvr::Format;
using sysdvr::L;
using sysdvr::Logf;
using sysdvr::LogLevel;

namespace {
int64_t NowNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// 进程内只初始化一次运行环境（引擎的全局资源），之后一直保留
bool EnsureEnvironment() {
    static const bool ok = OH_VideoProcessing_InitializeEnvironment() == VIDEO_PROCESSING_SUCCESS;
    return ok;
}

int ToQualityLevel(int level) {
    return level >= 2 ? VIDEO_DETAIL_ENHANCER_QUALITY_LEVEL_HIGH : VIDEO_DETAIL_ENHANCER_QUALITY_LEVEL_LOW;
}

void OnVpeError(OH_VideoProcessing*, VideoProcessing_ErrorCode error, void* userData) {
    static_cast<VideoEnhancer*>(userData)->HandleError(int(error));
}

void OnVpeState(OH_VideoProcessing*, VideoProcessing_State state, void*) {
    Logf(LogLevel::Info, "画质增强状态：%s", state == VIDEO_PROCESSING_STATE_RUNNING ? "运行中" : "已停止");
}

void OnVpeOutput(OH_VideoProcessing*, uint32_t index, void* userData) {
    static_cast<VideoEnhancer*>(userData)->HandleOutput(index);
}
}  // namespace

VideoEnhancer::~VideoEnhancer() { Release(); }

bool VideoEnhancer::Init(OHNativeWindow* outWindow, int level, std::string* error) {
    if (!EnsureEnvironment()) {
        *error = L("本机不支持画质增强（视频处理环境初始化失败）", "本機不支援畫質增強（視訊處理環境初始化失敗）",
                   "picture enhancement is not supported on this device (video processing init failed)");
        return false;
    }
    VideoProcessing_ErrorCode rc = OH_VideoProcessing_Create(&vp_, VIDEO_PROCESSING_TYPE_DETAIL_ENHANCER);
    if (rc != VIDEO_PROCESSING_SUCCESS || vp_ == nullptr) {
        vp_ = nullptr;
        *error = Format(L("本机不支持画质增强（%d）", "本機不支援畫質增強（%d）",
                          "picture enhancement is not supported on this device (%d)"), int(rc));
        return false;
    }
    OH_VideoProcessingCallback_Create(&callback_);
    OH_VideoProcessingCallback_BindOnError(callback_, &OnVpeError);
    OH_VideoProcessingCallback_BindOnState(callback_, &OnVpeState);
    OH_VideoProcessingCallback_BindOnNewOutputBuffer(callback_, &OnVpeOutput);
    rc = OH_VideoProcessing_RegisterCallback(vp_, callback_, this);
    if (rc == VIDEO_PROCESSING_SUCCESS) rc = OH_VideoProcessing_SetSurface(vp_, outWindow);
    if (rc == VIDEO_PROCESSING_SUCCESS) rc = OH_VideoProcessing_GetSurface(vp_, &input_);
    if (rc != VIDEO_PROCESSING_SUCCESS || input_ == nullptr) {
        *error = Format(L("画质增强初始化失败（%d）", "畫質增強初始化失敗（%d）", "picture enhancement init failed (%d)"),
                        int(rc));
        Release();
        return false;
    }
    SetLevel(level);
    rc = OH_VideoProcessing_Start(vp_);
    if (rc != VIDEO_PROCESSING_SUCCESS) {
        *error = Format(L("画质增强启动失败（%d）", "畫質增強啟動失敗（%d）", "picture enhancement failed to start (%d)"),
                        int(rc));
        Release();
        return false;
    }
    Logf(LogLevel::Info, "画质增强已启动（%s）", level >= 2 ? "高" : "标准");
    return true;
}

void VideoEnhancer::SetLevel(int level) {
    level_ = level;
    if (vp_ == nullptr) return;
    OH_AVFormat* p = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(p, VIDEO_DETAIL_ENHANCER_PARAMETER_KEY_QUALITY_LEVEL, ToQualityLevel(level));
    const VideoProcessing_ErrorCode rc = OH_VideoProcessing_SetParameter(vp_, p);
    OH_AVFormat_Destroy(p);
    if (rc != VIDEO_PROCESSING_SUCCESS) Logf(LogLevel::Warn, "设置画质增强档位失败：%d", int(rc));
}

void VideoEnhancer::Release() {
    if (vp_ != nullptr) {
        OH_VideoProcessing_Stop(vp_);
        OH_VideoProcessing_Destroy(vp_);
        vp_ = nullptr;
    }
    if (callback_ != nullptr) {
        OH_VideoProcessingCallback_Destroy(callback_);
        callback_ = nullptr;
    }
    if (input_ != nullptr) {
        // 输入 surface 由调用方销毁（见 OH_VideoProcessing_GetSurface 的说明）
        OH_NativeWindow_DestroyNativeWindow(input_);
        input_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    queuedNs_.clear();
}

void VideoEnhancer::OnFrameQueued() {
    std::lock_guard<std::mutex> lock(mutex_);
    queuedNs_.push_back(NowNs());
    // 增强器内部丢帧时 FIFO 会对不上，只保留最近几帧，偏差不会累积
    while (queuedNs_.size() > 6) queuedNs_.pop_front();
}

void VideoEnhancer::HandleOutput(uint32_t index) {
    if (vp_ == nullptr) return;
    OH_VideoProcessing_RenderOutputBuffer(vp_, index);
    ++framesOut_;
    std::lock_guard<std::mutex> lock(mutex_);
    if (queuedNs_.empty()) return;
    const int us = int((NowNs() - queuedNs_.front()) / 1000);
    queuedNs_.pop_front();
    if (us < 0 || us > 500'000) return;
    const int prev = avgLatencyUs_;
    avgLatencyUs_ = prev == 0 ? us : (prev * 7 + us) / 8;
}

void VideoEnhancer::HandleError(int error) {
    Logf(LogLevel::Error, "画质增强出错：%d", error);
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = Format(L("画质增强出错 %d", "畫質增強出錯 %d", "picture enhancement error %d"), error);
}

std::string VideoEnhancer::LastError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}
