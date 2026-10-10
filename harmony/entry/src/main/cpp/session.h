// 一次串流会话：StreamSource（TCP Bridge / USB / RTSP）→ VideoDecoder（画面）/ AudioPlayer（声音）
// 切后台时 PauseRendering：网络照常收，解码器和音频释放，只维护 GOP 缓存；
// 回前台时 ResumeRendering：新建解码器并重放缓存，直接追到最新画面。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "audio_player.h"
#include "gop_cache.h"
#include "stream_source.h"
#include "video_decoder.h"
#include "video_enhancer.h"

struct SessionStats {
    bool active = false;
    bool renderingPaused = false;
    std::string message;
    sysdvr::BridgeStats bridge;
    uint64_t framesRendered = 0;
    uint64_t videoDropped = 0;
    int smoothLevel = 0;
    int smoothDelayMs = 0;
    bool netGuard = false;
    bool netOpt = false;
    uint64_t videoLate = 0;
    int lastCatchUpMs = -1;
    uint64_t gopPackets = 0;
    uint64_t gopBytes = 0;
    bool audioEnabled = false;
    bool audioFastMode = false;
    uint64_t audioUnderruns = 0;
    int audioBufferedMs = 0;
    int audioTargetMs = 0;
    std::string decoderError;

    // 丢帧与定格
    bool freezeOnLoss = true;
    bool frozen = false;
    uint64_t lossEvents[int(sysdvr::LossCause::Count)] = {};  // 按原因分类的丢失事件
    uint64_t lostFrames = 0;       // 时间戳断档估计丢掉的帧数
    int frameIntervalUs = 0;       // 估计的正常帧间隔
    int keyframeIntervalMs = 0;    // 关键帧间隔（滑动平均）
    int lastFreezeMs = -1;
    DecoderCounters decoder;       // 定格、VSync、卡顿等解码器侧计数（跨重建累计）
    RenderTiming timing;           // 最近一个统计窗口的上屏节奏
    int vsyncPeriodUs = 0;

    // 画质增强：enhanceLevel 是实际生效的档位（设备不支持时为 0，原因见 enhanceError）
    int enhanceRequested = 0;
    int enhanceLevel = 0;
    int enhanceLatencyUs = 0;
    std::string enhanceError;
};

// 按选项和回调创建串流来源（由 NAPI 层决定用哪种传输）
using SourceFactory =
    std::function<std::unique_ptr<sysdvr::StreamSource>(const sysdvr::StreamOptions&, const sysdvr::StreamCallbacks&)>;

struct SessionConfig {
    std::string label;  // 只用于状态提示，例如 "192.168.49.5" 或 "USB"
    uint64_t surfaceId = 0;
    bool audio = true;
    int smoothLevel = 0;
    bool netOpt = true;
    bool screenOff = false;  // 串流时关闭 Switch 屏幕（协议 03，TCP Bridge / USB）
    bool freezeOnLoss = true;  // 丢帧后定格到下一个关键帧，不花屏
    int enhanceLevel = 0;      // 画质增强：0 关闭 / 1 标准 / 2 高
    // 实验扩展（仅 TCP Bridge）：请求扩展标志和初始音频编码；官方 SysDVR 忽略
    bool nextExt = false;
    sysdvr::ExtAudioConfig extAudio;
};

class Session {
public:
    ~Session();

    // 以下都在 ArkTS 主线程调用。返回空字符串表示成功，否则为错误信息
    std::string Start(const SourceFactory& factory, const SessionConfig& config);
    void Stop();
    void PauseRendering();
    std::string ResumeRendering(uint64_t surfaceId);
    // 播放中快捷开关：不断开视频
    std::string SetAudioEnabled(bool enabled);
    void SetSmoothLevel(int level);
    // 网络预警（ArkTS 侧根据系统的网络质量/场景信息判断）：平滑缓冲提前垫到当前档位上限
    void SetNetworkGuard(bool on);
    void SetNetOptimization(bool enabled);
    void SetFreezeOnLoss(bool on);
    // 画质增强档位；开关状态变化时重建视频链路（重放 GOP，画面直接追到最新）。
    // 返回空串表示成功，否则为错误信息（例如本机不支持，此时已退回直出）
    std::string SetEnhanceLevel(int level);
    // 实验扩展：运行中切换音频编码/Opus 参数（控制消息，不重连）。
    // 返回空串表示已发出或已记下（音频关着时下次打开生效）；服务端不支持扩展时返回提示文字
    std::string SetExtAudio(const sysdvr::ExtAudioConfig& config);
    std::vector<int> SocketFds() const;
    SessionStats Stats();

private:
    // 网络线程回调
    void OnVideo(const uint8_t* data, size_t size, uint64_t ts);
    void OnAudio(const uint8_t* data, size_t size);
    // 视频数据丢失（持 videoMutex_ 调用）：计数、作废 GOP 缓存、通知解码器定格
    void HandleLossLocked(sysdvr::LossCause cause);
    void SetMessage(const std::string& msg);
    // 每秒随统计调用：检查回前台后的追帧是否完成
    void UpdateResumeMessage();
    // 一条视频链路：屏幕窗口 +（可选）画质增强 + 解码器
    struct VideoPipeline {
        OHNativeWindow* window = nullptr;
        std::unique_ptr<VideoEnhancer> enhancer;
        std::unique_ptr<VideoDecoder> decoder;
        std::string enhanceError;  // 画质增强没能启用的原因（不影响播放）
    };
    // 创建视频链路（不持锁，耗时几十毫秒）
    std::string CreateVideo(uint64_t surfaceId, VideoPipeline* out);
    // 持锁把链路装上：GOP 重放（可选）+ 切换
    size_t InstallVideoLocked(VideoPipeline* p, bool replayGop);
    // 只释放视频（解码器 → 增强器 → 窗口），不动音频
    void ReleaseVideo();
    // 换一条视频链路（开关画质增强、增强器不出画面时退回直出），GOP 重放让画面直接追到最新
    std::string RebuildVideo();
    // 画质增强建起来了却迟迟不出画面（部分设备、模拟器）：退回直出（主线程调用）
    void CheckEnhancerStall();
    std::string CreateAudio();
    void SyncAudioToVideo();
    void ReleaseAudio();
    void ReleaseRendering();

    std::unique_ptr<sysdvr::StreamSource> client_;
    std::atomic<bool> audioEnabled_{false};
    std::atomic<int> smoothLevel_{0};
    std::atomic<bool> netGuard_{false};
    std::atomic<bool> netOpt_{true};
    std::atomic<bool> paused_{false};
    std::atomic<bool> freezeOnLoss_{true};
    std::atomic<int> enhanceLevel_{0};
    uint64_t lastSurfaceId_ = 0;  // 主线程读写
    bool enhanceBroken_ = false;  // 本机上画质增强不出画面：本次会话不再尝试，直到用户重新选档位（主线程读写）
    // 回前台后的恢复进度：追帧完成（或等到关键帧出第一帧）后把状态文字更新成“已恢复实时画面”
    enum class ResumeState { None, CatchingUp, WaitingKeyframe };
    std::atomic<ResumeState> resumeState_{ResumeState::None};
    int64_t resumeAtMs_ = 0;

    // 网络线程与主线程共享，受 videoMutex_ 保护
    std::mutex videoMutex_;
    OHNativeWindow* window_ = nullptr;
    std::unique_ptr<VideoEnhancer> enhancer_;
    std::string enhanceError_;
    std::unique_ptr<VideoDecoder> video_;
    sysdvr::GopCache gop_;
    // 已释放解码器的累计统计，保证暂停/恢复后数字连续
    uint64_t renderedBase_ = 0;
    uint64_t droppedBase_ = 0;
    uint64_t lateBase_ = 0;
    // 音频播放器在开关音频、切前后台时会重建，计数要累加，统计才不会倒退
    uint64_t underrunBase_ = 0;
    int lastCatchUpMs_ = -1;
    DecoderCounters decoderBase_;
    // 丢帧检测（网络线程，受 videoMutex_ 保护）
    sysdvr::FrameGapDetector gap_;
    uint64_t lossEvents_[int(sysdvr::LossCause::Count)] = {};
    uint64_t lostFrames_ = 0;
    // 上一帧之后已经报过明确的丢失（错误包/缓存未命中/失步）：随后的时间戳断档是同一次丢失，不再重复计次
    bool explicitLossPending_ = false;
    uint64_t lastIdrUs_ = 0;
    int keyframeIntervalMs_ = 0;

    std::mutex audioMutex_;
    std::unique_ptr<AudioPlayer> audio_;

    std::mutex messageMutex_;
    std::string message_;
};
