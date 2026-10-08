// 视频丢帧检测。
// Switch 上 SysDVR 的发送线程是“读一帧 → 阻塞发送 → 再读下一帧”，网络一卡，系统录像服务（grc）
// 来不及被读走的帧会被直接丢掉，客户端收到的相邻两帧之间就出现时间戳断档。断档之后的 P 帧
// 参考链已经断了，照常解码会花屏到下一个关键帧。
#pragma once

#include <cstdint>

namespace sysdvr {

// 丢失原因（统计分类用）
enum class LossCause : int {
    TimestampGap = 0,  // 相邻帧时间戳断档（Switch 端丢帧）
    ErrorPacket = 1,   // sysmodule 发来的视频错误包（例如关键帧超过它的缓冲）
    ReplayMiss = 2,    // 重放包指向的缓存槽为空
    Resync = 3,        // 包头失步，丢了一段字节后重新对齐
    Oversize = 4,      // 包超过解码器输入缓冲，只能丢弃
    Count = 5,
};

class FrameGapDetector {
public:
    // 每收到一帧视频调用一次；返回估计丢掉的帧数，0 表示连续
    int OnFrame(uint64_t timestampUs);
    void Reset();
    // 估计的正常帧间隔（微秒），grc 通常是 30 帧/秒，即约 33333
    int64_t IntervalUs() const { return intervalUs_; }

private:
    void Learn(int64_t deltaUs);

    static constexpr int kWindow = 31;
    bool hasLast_ = false;
    uint64_t lastUs_ = 0;
    int64_t window_[kWindow] = {};
    int count_ = 0;
    int next_ = 0;
    int64_t intervalUs_ = 33333;
};

}  // namespace sysdvr
