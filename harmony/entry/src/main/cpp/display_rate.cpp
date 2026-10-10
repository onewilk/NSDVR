#include "display_rate.h"

#include <native_display_soloist/native_display_soloist.h>

#include "log.h"

using sysdvr::Logf;
using sysdvr::LogLevel;

DisplayRateVote& DisplayRateVote::Get() {
    static DisplayRateVote* instance = new DisplayRateVote();  // 故意不释放，见头文件说明
    return *instance;
}

void DisplayRateVote::OnFrame(long long, long long, void*) {}

void DisplayRateVote::Set(int fps) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fps < 0) fps = 0;
    if (fps == fps_) return;
    if (fps == 0) {
        if (soloist_ != nullptr) OH_DisplaySoloist_Stop(soloist_);
        fps_ = 0;
        Logf(LogLevel::Info, "撤销刷新率请求");
        return;
    }
    if (soloist_ == nullptr) {
        soloist_ = OH_DisplaySoloist_Create(false);
        if (soloist_ == nullptr) {
            Logf(LogLevel::Warn, "创建 DisplaySoloist 失败，不请求刷新率");
            return;
        }
    }
    // 允许系统按功耗策略再降到一半（例如省电模式），但不要高于请求值
    DisplaySoloist_ExpectedRateRange range{fps / 2, fps, fps};
    if (OH_DisplaySoloist_SetExpectedFrameRateRange(soloist_, &range) != 0) {
        Logf(LogLevel::Warn, "设置期望刷新率失败");
        return;
    }
    // 已经在运行时只更新范围；从 0 变过来才需要启动
    if (fps_ == 0 && OH_DisplaySoloist_Start(soloist_, &DisplayRateVote::OnFrame, nullptr) != 0) {
        Logf(LogLevel::Warn, "启动 DisplaySoloist 失败");
        return;
    }
    fps_ = fps;
    Logf(LogLevel::Info, "请求屏幕刷新率 %d Hz", fps);
}
