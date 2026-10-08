// 缓存“最近一个 IDR + 之后的所有帧”（一个 GOP）。
// 切回前台时把它重新送进新建的解码器，只渲染最后一帧，画面立刻追到最新，不用等下一个关键帧。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace sysdvr {

struct CachedPacket {
    std::vector<uint8_t> data;
    uint64_t timestampUs = 0;
};

class GopCache {
public:
    // 超过任一上限就放弃当前 GOP，等下一个 IDR（Switch 关键帧间隔异常长时兜底）
    explicit GopCache(size_t maxBytes = 24u << 20, size_t maxPackets = 900)
        : maxBytes_(maxBytes), maxPackets_(maxPackets) {}

    void Push(const uint8_t* data, size_t size, uint64_t timestampUs);
    void Clear();

    // 返回可直接送解码器的序列：第一包必为 IDR，且保证前面带有 SPS/PPS
    // （IDR 自己不带时用最近见过的参数集，从没见过则用 Switch 固定参数集）
    std::vector<CachedPacket> Snapshot() const;

    size_t Packets() const { return packets_.size(); }
    size_t Bytes() const { return bytes_; }
    bool HasKeyframe() const { return !packets_.empty(); }
    // 最近见过的 SPS+PPS（带起始码），没见过时为空
    std::vector<uint8_t> ParamSets() const;

private:
    const size_t maxBytes_;
    const size_t maxPackets_;
    std::deque<CachedPacket> packets_;
    size_t bytes_ = 0;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
};

}  // namespace sysdvr
