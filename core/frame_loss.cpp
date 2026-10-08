#include "frame_loss.h"

#include <algorithm>

namespace sysdvr {

namespace {
// 断档判定：超过 1.6 个帧间隔，且至少多出 15ms（避免高帧率时把正常抖动当成丢帧）
constexpr int64_t kGapNum = 16;
constexpr int64_t kGapDen = 10;
constexpr int64_t kMinExtraUs = 15'000;
// 帧间隔的合理范围：120fps ~ 10fps
constexpr int64_t kMinIntervalUs = 8'000;
constexpr int64_t kMaxIntervalUs = 100'000;
}  // namespace

void FrameGapDetector::Reset() {
    hasLast_ = false;
    lastUs_ = 0;
    count_ = 0;
    next_ = 0;
}

void FrameGapDetector::Learn(int64_t d) {
    // 只用“正常”的间隔更新估计，断档和重复时间戳不参与
    if (d < kMinIntervalUs || d > kMaxIntervalUs) return;
    if (count_ >= 8 && (d * 2 < intervalUs_ || d * kGapDen > intervalUs_ * kGapNum)) return;
    window_[next_] = d;
    next_ = (next_ + 1) % kWindow;
    if (count_ < kWindow) ++count_;
    if (count_ < 8) return;  // 样本太少时先用默认的 30fps
    int64_t sorted[kWindow];
    std::copy(window_, window_ + count_, sorted);
    std::nth_element(sorted, sorted + count_ / 2, sorted + count_);
    intervalUs_ = sorted[count_ / 2];
}

int FrameGapDetector::OnFrame(uint64_t timestampUs) {
    if (!hasLast_) {
        hasLast_ = true;
        lastUs_ = timestampUs;
        return 0;
    }
    const int64_t d = int64_t(timestampUs) - int64_t(lastUs_);
    lastUs_ = timestampUs;
    if (d <= 0) {
        // 时间戳回退：换了游戏或 sysmodule 重启，编码器会从关键帧重新开始，不算丢帧
        count_ = 0;
        next_ = 0;
        return 0;
    }
    if (d * kGapDen > intervalUs_ * kGapNum && d - intervalUs_ > kMinExtraUs) {
        // 很长的断档（例如回主页后再回游戏）也按丢帧处理：宁可定格到下一个关键帧，也不冒花屏的险
        const int64_t lost = (d + intervalUs_ / 2) / intervalUs_ - 1;
        return int(std::max<int64_t>(1, std::min<int64_t>(lost, 1000000)));
    }
    Learn(d);
    return 0;
}

}  // namespace sysdvr
