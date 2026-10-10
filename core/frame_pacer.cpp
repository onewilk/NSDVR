#include "frame_pacer.h"

#include <algorithm>

namespace sysdvr {

void FramePacer::SetEnabled(bool enabled, int64_t /*nowUs*/) {
    enabled_ = enabled;
    anchored_ = false;
    jitterWantedUs_ = p_.initialUs;
    targetUs_ = wantedUs_ = enabled ? Wanted() : 0;
    adjustedPts_ = -1;
    transit_.clear();
    transitPos_ = 0;
}

void FramePacer::SetLimits(int64_t minUs, int64_t maxUs, int64_t marginUs) {
    p_.minUs = minUs;
    p_.maxUs = maxUs;
    p_.marginUs = marginUs;
    p_.initialUs = std::clamp(p_.initialUs, minUs, maxUs);
    UpdateTarget();
}

void FramePacer::SetFloor(int64_t floorUs) {
    floorUs_ = std::max<int64_t>(0, floorUs);
    if (enabled_) wantedUs_ = Wanted();
}

int64_t FramePacer::JitterWanted() const { return std::clamp(jitterWantedUs_, p_.minUs, p_.maxUs); }

int64_t FramePacer::Wanted() const { return std::clamp(std::max(jitterWantedUs_, floorUs_), p_.minUs, p_.maxUs); }

void FramePacer::OnArrival(int64_t nowUs, int64_t ptsUs) {
    if (!enabled_) return;
    const int64_t transit = nowUs - ptsUs;
    if (transit_.size() < p_.windowFrames) {
        transit_.push_back(transit);
    } else {
        transit_[transitPos_] = transit;
        transitPos_ = (transitPos_ + 1) % p_.windowFrames;
    }
    UpdateTarget();
}

void FramePacer::UpdateTarget() {
    if (transit_.size() >= 15) {  // 样本太少（不到半秒）先用初始值
        scratch_ = transit_;
        const int64_t lo = *std::min_element(scratch_.begin(), scratch_.end());
        const size_t k = scratch_.size() * size_t(p_.percentile) / 100;
        std::nth_element(scratch_.begin(), scratch_.begin() + k, scratch_.end());
        const int64_t jitter = scratch_[k] - lo;
        jitterWantedUs_ = std::clamp(jitter + p_.marginUs, p_.minUs, p_.maxUs);
    }
    wantedUs_ = Wanted();
}

int64_t FramePacer::Schedule(int64_t nowUs, int64_t ptsUs, int64_t backlogSpanUs) {
    if (!enabled_) return 0;

    // 缓冲调整：抖动变大立即生效（锚点后移，下一帧多等一会儿）；网络预警抬高的部分每帧垫一点，
    // 画面略微放慢、不卡一下；变小则每帧缩一点，画面不会突然加速
    const bool newFrame = ptsUs != adjustedPts_;
    adjustedPts_ = ptsUs;
    if (wantedUs_ > targetUs_) {
        int64_t next = std::max(targetUs_, std::min(JitterWanted(), wantedUs_));
        if (next < wantedUs_ && (newFrame || !anchored_)) {
            next = anchored_ ? std::min(wantedUs_, next + p_.growPerFrameUs) : wantedUs_;
        }
        if (anchored_) anchorLocalUs_ += next - targetUs_;
        targetUs_ = next;
    } else if (wantedUs_ < targetUs_ && newFrame) {
        const int64_t step = std::min(p_.shrinkPerFrameUs, targetUs_ - wantedUs_);
        if (anchored_) anchorLocalUs_ -= step;
        targetUs_ -= step;
    }

    if (!anchored_) {
        anchored_ = true;
        anchorLocalUs_ = nowUs + targetUs_;
        anchorPtsUs_ = ptsUs;
    }
    int64_t due = anchorLocalUs_ + (ptsUs - anchorPtsUs_);

    if (due - nowUs > 3 * p_.maxUs || ptsUs < anchorPtsUs_) {
        // 时间戳跳变（比如 Switch 切换游戏、时间戳回绕），重新锚定
        anchorLocalUs_ = nowUs + targetUs_;
        anchorPtsUs_ = ptsUs;
        due = anchorLocalUs_;
    } else if (nowUs - due > p_.lateToleranceUs) {
        // 迟到：网络卡了一下（超出缓冲能兜住的范围）。以当前帧重新锚定，把缓冲垫回来
        ++late_;
        anchorLocalUs_ = nowUs + targetUs_;
        anchorPtsUs_ = ptsUs;
        due = anchorLocalUs_;
    } else if (backlogSpanUs > targetUs_ + p_.catchUpSlackUs) {
        // 积压：卡顿恢复后网络一次性涌进来一大段。锚点前移，快速追到最新，不让延迟积累
        anchorLocalUs_ -= backlogSpanUs - targetUs_;
        due = anchorLocalUs_ + (ptsUs - anchorPtsUs_);
    }
    return due;
}

}  // namespace sysdvr
