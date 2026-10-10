#include "video_decoder.h"

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <native_window/external_window.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "i18n.h"
#include "log.h"
#include "sysdvr_protocol.h"
#include "vsync_monitor.h"

using sysdvr::Logf;
using sysdvr::LogLevel;
using sysdvr::L;
using sysdvr::Format;

namespace {
// 积压超过 1 秒说明解码跟不上，清空并等下一个关键帧，避免延迟越积越大；
// 平滑模式下队列里本来就压着一段缓冲，放宽到 3 秒
constexpr size_t kMaxQueuedPackets = 30;
constexpr size_t kMaxQueuedPacketsSmooth = 90;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

int64_t NowNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// 丢帧后最多定格这么久：关键帧间隔异常长时宁可短暂花屏，也不让画面一直停着
constexpr int64_t kMaxFreezeMs = 3000;
// 顺延上屏最多排到当前时间之后这么多个刷新周期；再多说明积压了，跳过这一帧追实时
constexpr int kMaxDeferPeriods = 3;
// 网络预警时的缓冲下限：给到最大，由 FramePacer 夹到当前档位的上限（延迟优先 80ms / 流畅优先 300ms）
constexpr int64_t kGuardFloorUs = 300'000;
}  // namespace

VideoDecoder::~VideoDecoder() { Release(); }

bool VideoDecoder::Init(OHNativeWindow* window, std::string* error) {
    codec_ = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    if (codec_ == nullptr) {
        *error = L("创建 H.264 解码器失败", "建立 H.264 解碼器失敗", "failed to create the H.264 decoder");
        return false;
    }

    OH_AVCodecCallback cb = {&VideoDecoder::OnError, &VideoDecoder::OnStreamChanged,
                             &VideoDecoder::OnNeedInputBuffer, &VideoDecoder::OnNewOutputBuffer};
    int32_t rc = OH_VideoDecoder_RegisterCallback(codec_, cb, this);
    if (rc != AV_ERR_OK) {
        *error = Format(L("RegisterCallback 失败：%d", "RegisterCallback 失敗：%d", "RegisterCallback failed: %d"), int(rc));
        return false;
    }

    OH_AVFormat* format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, sysdvr::kVideoWidth);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, sysdvr::kVideoHeight);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    OH_AVFormat_SetDoubleValue(format, OH_MD_KEY_FRAME_RATE, 30.0);
    // API 12：解码器不额外缓存帧，对串流延迟影响很大
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 1);
    rc = OH_VideoDecoder_Configure(codec_, format);
    OH_AVFormat_Destroy(format);
    if (rc != AV_ERR_OK) {
        *error = Format(L("Configure 失败：%d", "Configure 失敗：%d", "Configure failed: %d"), int(rc));
        return false;
    }

    rc = OH_VideoDecoder_SetSurface(codec_, window);
    if (rc != AV_ERR_OK) {
        *error = Format(L("SetSurface 失败：%d", "SetSurface 失敗：%d", "SetSurface failed: %d"), int(rc));
        return false;
    }
    rc = OH_VideoDecoder_Prepare(codec_);
    if (rc != AV_ERR_OK) {
        *error = Format(L("Prepare 失败：%d", "Prepare 失敗：%d", "Prepare failed: %d"), int(rc));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
    }
    feeder_ = std::thread(&VideoDecoder::FeedLoop, this);

    rc = OH_VideoDecoder_Start(codec_);
    if (rc != AV_ERR_OK) {
        *error = Format(L("Start 失败：%d", "Start 失敗：%d", "Start failed: %d"), int(rc));
        return false;
    }
    Logf(LogLevel::Info, "H.264 解码器已启动（Surface 模式，低延迟）");
    return true;
}

void VideoDecoder::SetFallbackParamSets(std::vector<uint8_t> paramSets) { fallbackParamSets_ = std::move(paramSets); }

void VideoDecoder::Replay(std::vector<sysdvr::CachedPacket> gop) {
    if (gop.empty()) return;  // 没有可用的 GOP，按正常流程等下一个 IDR

    waitingKeyframe_ = false;
    spsSent_ = true;  // GopCache::Snapshot 保证第一包带参数集
    renderFromPts_ = int64_t(gop.back().timestampUs);
    catchUpStartMs_ = NowMs();
    lastCatchUpMs_ = -1;
    catchingUp_ = true;
    Logf(LogLevel::Info, "重放缓存 GOP：%zu 包，只渲染最后一帧", gop.size());

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : gop) packets_.push_back({std::move(p.data), int64_t(p.timestampUs), true});
    pacer_.Reanchor();  // 追帧完后，实时包重新建立平滑排期
    cv_.notify_one();
}

void VideoDecoder::SetSmoothLevel(int level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level == smoothLevel_) return;
    const bool wasOn = smoothLevel_ > 0;
    smoothLevel_ = level;
    if (level == 1) {
        pacer_.SetLimits(20'000, 80'000, 15'000);
    } else {
        pacer_.SetLimits(40'000, 300'000, 30'000);
    }
    pacer_.SetFloor(netGuard_ ? kGuardFloorUs : 0);
    if ((level > 0) != wasOn) pacer_.SetEnabled(level > 0, NowUs());
    smoothing_ = level > 0;
    targetDelayUs_ = pacer_.TargetDelayUs();
    static const char* kNames[] = {"关闭", "延迟优先", "流畅优先"};
    Logf(LogLevel::Info, "平滑模式：%s", kNames[level < 0 || level > 2 ? 0 : level]);
    cv_.notify_one();
}

void VideoDecoder::SetNetworkGuard(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (on == netGuard_) return;
    netGuard_ = on;
    pacer_.SetFloor(on ? kGuardFloorUs : 0);
    cv_.notify_one();
}

int64_t VideoDecoder::ScheduleFront(int64_t nowUs) {
    const Packet& front = packets_.front();
    if (!smoothing_ || front.immediate) return 0;
    const int64_t due = pacer_.Schedule(nowUs, front.ptsUs, packets_.back().ptsUs - front.ptsUs);
    targetDelayUs_ = pacer_.TargetDelayUs();
    latePackets_ = pacer_.LateCount();
    return due;
}

void VideoDecoder::SetFreezeOnLoss(bool on) {
    freezeOnLoss_ = on;
    Logf(LogLevel::Info, "丢帧定格：%s", on ? "开启" : "关闭");
}

void VideoDecoder::OnFrameLoss(sysdvr::LossCause) {
    if (!freezeOnLoss_ || lossFreeze_) return;
    lossFreeze_ = true;
    waitingKeyframe_ = true;
    freezeStartMs_ = NowMs();
    frozen_ = true;
    ++freezeEvents_;
}

void VideoDecoder::EndFreeze(bool gaveUp) {
    if (!lossFreeze_) return;
    lossFreeze_ = false;
    frozen_ = false;
    const int64_t ms = NowMs() - freezeStartMs_;
    freezeTotalMs_ += uint64_t(ms);
    lastFreezeMs_ = int(ms);
    if (gaveUp) {
        ++freezeGiveUps_;
        Logf(LogLevel::Warn, "丢帧后 %lld ms 仍没等到关键帧，放弃定格", (long long)ms);
    }
}

DecoderCounters VideoDecoder::Counters() const {
    DecoderCounters c;
    c.freezeEvents = freezeEvents_;
    c.freezeTotalMs = freezeTotalMs_;
    c.freezeGiveUps = freezeGiveUps_;
    c.oversize = oversize_;
    c.vsyncDeferred = vsyncDeferred_;
    c.vsyncDropped = vsyncDropped_;
    c.stutters = stutters_;
    return c;
}

RenderTiming VideoDecoder::TakeRenderTiming() {
    std::lock_guard<std::mutex> lock(timingMutex_);
    RenderTiming t;
    t.jitterMs10 = jitterCount_ > 0 ? int(jitterSumUs_ / jitterCount_ / 100) : 0;
    t.maxGapMs = int(maxGapUs_ / 1000);
    jitterSumUs_ = 0;
    jitterCount_ = 0;
    maxGapUs_ = 0;
    return t;
}

void VideoDecoder::Submit(const uint8_t* data, size_t size, uint64_t timestampUs) {
    const bool hasSps = sysdvr::ContainsNalType(data, size, 7);
    const bool hasIdr = sysdvr::ContainsNalType(data, size, 5);

    // 送解码线程丢掉了超大的包：同样是参考链断了
    if (lossPending_.exchange(false)) OnFrameLoss(sysdvr::LossCause::Oversize);

    if (waitingKeyframe_) {
        // 从 IDR 开始送解码器，之前的 P 帧没有参考帧，送进去只会花屏或报错
        if (!hasIdr) {
            if (!lossFreeze_ || NowMs() - freezeStartMs_ <= kMaxFreezeMs) {
                ++packetsDropped_;
                return;
            }
            EndFreeze(true);  // 等太久了：照常解码，接受短暂花屏
        } else {
            EndFreeze(false);
        }
        waitingKeyframe_ = false;
    }

    Packet pkt;
    // 第一个 IDR 如果自己不带 SPS/PPS，就补一组在前面（与 sysmodule 注入方式相同）
    if (hasIdr && !hasSps && !spsSent_) {
        if (!fallbackParamSets_.empty()) {
            pkt.data = fallbackParamSets_;
        } else {
            pkt.data.insert(pkt.data.end(), std::begin(sysdvr::kSps), std::end(sysdvr::kSps));
            pkt.data.insert(pkt.data.end(), std::begin(sysdvr::kPps), std::end(sysdvr::kPps));
        }
        Logf(LogLevel::Info, "首个 IDR 不含 SPS/PPS，补上%s参数集", fallbackParamSets_.empty() ? "内置" : "最近的");
    }
    if (hasSps || !pkt.data.empty()) spsSent_ = true;
    pkt.data.insert(pkt.data.end(), data, data + size);
    // 直接用 Switch 的微秒时间戳作 pts，追帧阈值和缓存里的时间戳是同一基准
    pkt.ptsUs = int64_t(timestampUs);

    Enqueue(std::move(pkt), hasIdr);
}

void VideoDecoder::Enqueue(Packet pkt, bool isIdr) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    // 兜底：解码器迟迟不出帧时别让追帧状态一直挂着
    if (catchingUp_ && NowMs() - catchUpStartMs_ > 5000) {
        Logf(LogLevel::Warn, "追帧超时，放弃跳帧");
        catchingUp_ = false;
    }
    // 追帧期间队列里本来就压着整段 GOP，不做积压丢弃
    const size_t cap = smoothing_ ? kMaxQueuedPacketsSmooth : kMaxQueuedPackets;
    if (!catchingUp_ && packets_.size() >= cap) {
        packetsDropped_ += packets_.size();
        packets_.clear();
        // 当前包若是 IDR 就从它重新开始，否则继续等下一个
        if (!isIdr) {
            waitingKeyframe_ = true;
            ++packetsDropped_;
            return;
        }
    }
    if (smoothing_ && !pkt.immediate) pacer_.OnArrival(NowUs(), pkt.ptsUs);
    packets_.push_back(std::move(pkt));
    cv_.notify_one();
}

void VideoDecoder::FeedLoop() {
    while (true) {
        InputSlot slot{};
        Packet pkt;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !running_ || !packets_.empty(); });
            if (!running_) return;

            // 平滑模式：等到队首包的排期时间。期间有新包到达只会唤醒后重新计算
            const int64_t due = ScheduleFront(NowUs());
            if (due > 0) {
                const int64_t waitUs = due - NowUs();
                if (waitUs > 0) {
                    cv_.wait_for(lock, std::chrono::microseconds(waitUs));
                    continue;  // 重新检查（可能被停止、关闭平滑，或者时间已到）
                }
            }

            cv_.wait(lock, [this] { return !running_ || (!inputs_.empty() && !packets_.empty()); });
            if (!running_) return;
            slot = inputs_.front();
            inputs_.pop_front();
            pkt = std::move(packets_.front());
            packets_.pop_front();
        }

        const int32_t capacity = OH_AVBuffer_GetCapacity(slot.buffer);
        if (int64_t(pkt.data.size()) > capacity) {
            Logf(LogLevel::Warn, "视频包 %zu 字节超过输入缓冲 %d，丢弃", pkt.data.size(), capacity);
            ++packetsDropped_;
            ++oversize_;
            lossPending_ = true;
            std::lock_guard<std::mutex> lock(mutex_);
            inputs_.push_front(slot);
            continue;
        }

        std::memcpy(OH_AVBuffer_GetAddr(slot.buffer), pkt.data.data(), pkt.data.size());
        OH_AVCodecBufferAttr attr{};
        attr.pts = pkt.ptsUs;
        attr.size = int32_t(pkt.data.size());
        attr.offset = 0;
        attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
        OH_AVBuffer_SetBufferAttr(slot.buffer, &attr);
        const int32_t rc = OH_VideoDecoder_PushInputBuffer(codec_, slot.index);
        if (rc != AV_ERR_OK) Logf(LogLevel::Warn, "PushInputBuffer 失败：%d", rc);
    }
}

void VideoDecoder::Release() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();
    if (feeder_.joinable()) feeder_.join();

    if (codec_ != nullptr) {
        OH_VideoDecoder_Stop(codec_);
        OH_VideoDecoder_Destroy(codec_);
        codec_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    inputs_.clear();
    packets_.clear();
}

std::string VideoDecoder::LastError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

void VideoDecoder::OnError(OH_AVCodec*, int32_t errorCode, void* userData) {
    auto* self = static_cast<VideoDecoder*>(userData);
    Logf(LogLevel::Error, "解码器错误：%d", errorCode);
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->lastError_ = Format(L("解码器错误 %d", "解碼器錯誤 %d", "decoder error %d"), int(errorCode));
}

void VideoDecoder::OnStreamChanged(OH_AVCodec*, OH_AVFormat* format, void*) {
    int32_t w = 0, h = 0;
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &w);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &h);
    Logf(LogLevel::Info, "解码输出格式：%dx%d", w, h);
}

void VideoDecoder::OnNeedInputBuffer(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData) {
    auto* self = static_cast<VideoDecoder*>(userData);
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->inputs_.push_back({index, buffer});
    self->cv_.notify_one();
}

void VideoDecoder::OnNewOutputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData) {
    auto* self = static_cast<VideoDecoder*>(userData);
    if (self->catchingUp_) {
        OH_AVCodecBufferAttr attr{};
        OH_AVBuffer_GetBufferAttr(buffer, &attr);
        if (attr.pts < self->renderFromPts_) {
            // 追帧中的旧帧：只解码（维持参考帧链）不上屏
            OH_VideoDecoder_FreeOutputBuffer(codec, index);
            ++self->framesSkipped_;
            return;
        }
        self->catchingUp_ = false;
        self->lastCatchUpMs_ = int(NowMs() - self->catchUpStartMs_);
        Logf(LogLevel::Info, "追帧完成：跳过 %llu 帧，耗时 %d ms", (unsigned long long)self->framesSkipped_.load(),
             self->lastCatchUpMs_.load());
    }
    // 不做音画同步，解出来就上屏，延迟最低；只按 VSync 错开，避免同一刷新周期里的多帧被合成器覆盖
    self->Present(codec, index);
}

void VideoDecoder::Present(OH_AVCodec* codec, uint32_t index) {
    const int64_t nowNs = NowNs();
    VsyncMonitor& vsync = VsyncMonitor::Get();
    vsync.MaybeRefresh(nowNs);
    const int64_t period = vsync.PeriodNs();

    int64_t target = nowNs;
    if (lastPresentNs_ > 0 && nowNs < lastPresentNs_ + period) {
        // 和上一帧落在同一个刷新周期：顺延到下一个 VSync。原来的做法是立即上屏，
        // 合成器每个 VSync 只取最新一帧，网络抖动后连着解出来的几帧只有最后一帧能被看到
        target = lastPresentNs_ + period;
        if (target - nowNs > kMaxDeferPeriods * period) {
            // 积压太多（长时间卡顿后一下子来了一串帧）：跳过这一帧，别让延迟越排越大
            OH_VideoDecoder_FreeOutputBuffer(codec, index);
            ++vsyncDropped_;
            return;
        }
        ++vsyncDeferred_;
    }

    const int32_t rc = target > nowNs ? OH_VideoDecoder_RenderOutputBufferAtTime(codec, index, target)
                                      : OH_VideoDecoder_RenderOutputBuffer(codec, index);
    if (rc != AV_ERR_OK) return;
    ++framesRendered_;
    if (frameQueuedHook_) frameQueuedHook_();

    // 上屏节奏统计：间隔与正常帧间隔之差（抖动），以及明显的卡顿
    if (lastPresentNs_ > 0) {
        const int64_t gapNs = target - lastPresentNs_;
        const int64_t frameNs = frameIntervalNs_;
        if (gapNs * 10 > frameNs * 17 && gapNs - frameNs > 20'000'000) ++stutters_;
        std::lock_guard<std::mutex> lock(timingMutex_);
        jitterSumUs_ += std::abs(gapNs - frameNs) / 1000;
        ++jitterCount_;
        if (gapNs / 1000 > maxGapUs_) maxGapUs_ = gapNs / 1000;
    }
    lastPresentNs_ = target;
}
