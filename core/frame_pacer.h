// 平滑模式的出帧排期（自适应抖动缓冲），与平台无关，便于单测。
// 思路：把一帧的 Switch 时间戳锚定到“本地时间 + 目标缓冲”，之后每帧按时间戳差值排期。
// 目标缓冲按实测抖动定：最近约 3 秒的“到达时间 - 时间戳”取第 90 百分位减最小值，再加一点余量。
// 偶发大卡顿（超出缓冲）不去硬扛：卡完重新锚定，积压的帧快速追到最新，避免延迟越积越多。
#pragma once

#include <cstdint>
#include <vector>

namespace sysdvr {

class FramePacer {
public:
    struct Params {
        int64_t initialUs = 100'000;       // 还没统计到抖动时先缓冲 100ms
        int64_t minUs = 40'000;
        int64_t maxUs = 300'000;
        int64_t marginUs = 30'000;         // 抖动之外再留的余量
        size_t windowFrames = 90;          // 统计窗口：约 3 秒
        int percentile = 90;               // 用第 90 百分位，偶发大卡顿不会把缓冲拉满
        int64_t shrinkPerFrameUs = 2'000;  // 缓冲缩小时每帧最多缩 2ms（约 60ms/秒），避免突然加速
        int64_t lateToleranceUs = 50'000;  // 比排期晚 50ms 以上算迟到（网络卡顿）
        int64_t catchUpSlackUs = 100'000;  // 积压超过“缓冲 + 100ms”就追赶
    };

    FramePacer() = default;
    explicit FramePacer(Params p) : p_(p) {}

    void SetEnabled(bool enabled, int64_t nowUs);
    bool Enabled() const { return enabled_; }
    // 调整缓冲范围（例如“游戏”档压到 80ms 以内），不清空已统计的抖动
    void SetLimits(int64_t minUs, int64_t maxUs, int64_t marginUs);
    // 每帧到达时调用（网络线程送入队列时），用来统计抖动
    void OnArrival(int64_t nowUs, int64_t ptsUs);
    // 下一帧重新锚定（例如追帧结束后）
    void Reanchor() { anchored_ = false; }

    // 队首帧（Switch 时间戳 ptsUs）应在哪个本地时间送出；backlogSpanUs = 队尾时间戳 - 队首时间戳。
    // 未开启时返回 0（立即送出）
    int64_t Schedule(int64_t nowUs, int64_t ptsUs, int64_t backlogSpanUs);

    int64_t TargetDelayUs() const { return enabled_ ? targetUs_ : 0; }
    uint64_t LateCount() const { return late_; }

private:
    void UpdateTarget();

    Params p_;
    bool enabled_ = false;
    bool anchored_ = false;
    int64_t targetUs_ = 0;        // 当前实际使用的缓冲
    int64_t wantedUs_ = 0;        // 按抖动算出来的理想缓冲
    int64_t anchorLocalUs_ = 0;
    int64_t anchorPtsUs_ = 0;
    uint64_t late_ = 0;
    std::vector<int64_t> transit_;  // 环形窗口：到达时间 - 时间戳
    size_t transitPos_ = 0;
    std::vector<int64_t> scratch_;
};

}  // namespace sysdvr
