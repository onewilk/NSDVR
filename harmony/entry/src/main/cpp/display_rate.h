// 播放时向系统请求的屏幕刷新率（可变帧率投票）。游戏画面最高 30 帧，屏幕却常跑在 90/120Hz：
// 请求 60Hz，LTPO 屏可以降档省电、减少发热；每帧仍占两个刷新周期，网络抖动时上屏节奏不受影响。
// 用 DisplaySoloist（API 12）投票：它只参与整机刷新率决策，回调里什么也不做。最终刷新率仍由系统决定。
// 进程内单例、永不销毁：VSync 回调可能在任意时刻到达。
#pragma once

#include <mutex>

struct OH_DisplaySoloist;

class DisplayRateVote {
public:
    static DisplayRateVote& Get();

    // fps > 0：请求这个刷新率；0：撤销请求，交还系统决定
    void Set(int fps);

private:
    DisplayRateVote() = default;
    static void OnFrame(long long timestamp, long long targetTimestamp, void* data);

    std::mutex mutex_;
    OH_DisplaySoloist* soloist_ = nullptr;
    int fps_ = 0;
};
