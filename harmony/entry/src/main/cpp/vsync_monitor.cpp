#include "vsync_monitor.h"

#include <native_vsync/native_vsync.h>

#include <cstring>

#include "log.h"

using sysdvr::Logf;
using sysdvr::LogLevel;

VsyncMonitor& VsyncMonitor::Get() {
    static VsyncMonitor* instance = new VsyncMonitor();  // 故意不释放，见头文件说明
    return *instance;
}

VsyncMonitor::VsyncMonitor() {
    const char* name = "sysdvr_video";
    vsync_ = OH_NativeVSync_Create(name, unsigned(std::strlen(name)));
    if (vsync_ == nullptr) Logf(LogLevel::Warn, "创建 NativeVSync 失败，按 60Hz 估算刷新周期");
}

void VsyncMonitor::MaybeRefresh(int64_t nowNs) {
    if (vsync_ == nullptr || pending_) return;
    if (nowNs - lastQueryNs_ < 1'000'000'000) return;
    lastQueryNs_ = nowNs;
    pending_ = true;
    if (OH_NativeVSync_RequestFrame(vsync_, &VsyncMonitor::OnFrame, this) != 0) pending_ = false;
}

void VsyncMonitor::OnFrame(long long, void* data) {
    auto* self = static_cast<VsyncMonitor*>(data);
    long long period = 0;
    if (OH_NativeVSync_GetPeriod(self->vsync_, &period) == 0 && period > 1'000'000 && period < 200'000'000) {
        if (period != self->periodNs_) Logf(LogLevel::Info, "屏幕刷新周期：%.2f ms", double(period) / 1e6);
        self->periodNs_ = period;
    }
    self->pending_ = false;
}
