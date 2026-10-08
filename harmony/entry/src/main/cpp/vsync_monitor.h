// 屏幕刷新周期（VSync）。LTPO 屏会在 1–120Hz 之间动态切换，所以定期重新查询。
// 进程内单例、永不销毁：VSync 回调可能在任意时刻到达，回调里用到的对象必须一直有效。
#pragma once

#include <atomic>
#include <cstdint>

struct OH_NativeVSync;

class VsyncMonitor {
public:
    static VsyncMonitor& Get();

    // 最近一次查到的刷新周期（纳秒）；还没查到时按 60Hz
    int64_t PeriodNs() const { return periodNs_; }
    // 距上次查询超过 1 秒就再请求一次（不阻塞，结果在 VSync 回调里更新）
    void MaybeRefresh(int64_t nowNs);

private:
    VsyncMonitor();
    static void OnFrame(long long timestamp, void* data);

    OH_NativeVSync* vsync_ = nullptr;
    std::atomic<int64_t> periodNs_{16'666'667};
    std::atomic<int64_t> lastQueryNs_{0};
    std::atomic<bool> pending_{false};
};
