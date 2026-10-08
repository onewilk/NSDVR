#include "gop_cache.h"

#include <iterator>

#include "sysdvr_protocol.h"

namespace sysdvr {

void GopCache::Push(const uint8_t* data, size_t size, uint64_t timestampUs) {
    bool isIdr = false;
    ForEachNal(data, size, [&](const uint8_t* nal, size_t len, int type) {
        if (type == 7) sps_.assign(nal, nal + len);
        else if (type == 8) pps_.assign(nal, nal + len);
        else if (type == 5) isIdr = true;
    });

    if (isIdr) {
        packets_.clear();
        bytes_ = 0;
    } else if (packets_.empty()) {
        return;  // 还没有关键帧，P 帧无法单独解码，不缓存
    }

    packets_.push_back({std::vector<uint8_t>(data, data + size), timestampUs});
    bytes_ += size;
    if (bytes_ > maxBytes_ || packets_.size() > maxPackets_) Clear();
}

void GopCache::Clear() {
    packets_.clear();
    bytes_ = 0;
}

std::vector<uint8_t> GopCache::ParamSets() const {
    if (sps_.empty() || pps_.empty()) return {};
    std::vector<uint8_t> out(sps_);
    out.insert(out.end(), pps_.begin(), pps_.end());
    return out;
}

std::vector<CachedPacket> GopCache::Snapshot() const {
    std::vector<CachedPacket> out(packets_.begin(), packets_.end());
    if (out.empty()) return out;

    auto& first = out.front().data;
    if (!ContainsNalType(first.data(), first.size(), 7)) {
        std::vector<uint8_t> prefix = ParamSets();
        if (prefix.empty()) {
            prefix.assign(std::begin(kSps), std::end(kSps));
            prefix.insert(prefix.end(), std::begin(kPps), std::end(kPps));
        }
        first.insert(first.begin(), prefix.begin(), prefix.end());
    }
    return out;
}

}  // namespace sysdvr
