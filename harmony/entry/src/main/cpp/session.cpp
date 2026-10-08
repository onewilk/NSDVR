#include "session.h"

#include <native_window/external_window.h>

#include <chrono>

#include "i18n.h"
#include "log.h"
#include "sysdvr_protocol.h"
#include "vsync_monitor.h"

using sysdvr::Logf;
using sysdvr::LogLevel;
using sysdvr::L;
using sysdvr::Format;

namespace {
int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

Session::~Session() { Stop(); }

namespace {
// 画质增强的输出尺寸：屏幕窗口里 16:9 画面区域的实际像素（放大 + 增强都由系统完成）
void FitVideoGeometry(OHNativeWindow* w, int32_t* outW, int32_t* outH) {
    int32_t h = 0, wd = 0;
    OH_NativeWindow_NativeWindowHandleOpt(w, GET_BUFFER_GEOMETRY, &h, &wd);
    if (wd <= 0 || h <= 0) {
        wd = 1920;
        h = 1080;
    }
    int32_t tw = wd, th = h;
    if (int64_t(wd) * 9 > int64_t(h) * 16) {
        tw = int32_t(int64_t(h) * 16 / 9);
    } else {
        th = int32_t(int64_t(wd) * 9 / 16);
    }
    *outW = tw & ~1;
    *outH = th & ~1;
}
}  // namespace

std::string Session::CreateVideo(uint64_t surfaceId, VideoPipeline* out) {
    OHNativeWindow* w = nullptr;
    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &w) != 0 || w == nullptr)
        return L("无法从 XComponent surfaceId 创建 NativeWindow", "無法從 XComponent surfaceId 建立 NativeWindow",
                 "cannot create a NativeWindow from the XComponent surfaceId");
    // 保持 16:9，不拉伸
    OH_NativeWindow_NativeWindowSetScalingModeV2(w, OH_SCALING_MODE_SCALE_FIT_V2);

    // 画质增强：解码器 → 增强器输入 surface → 增强 + 放大 → 屏幕；失败就退回直出，不影响播放
    std::unique_ptr<VideoEnhancer> enhancer;
    std::string enhanceError;
    const int level = enhanceBroken_ ? 0 : int(enhanceLevel_);
    if (level > 0) {
        int32_t tw = 0, th = 0;
        FitVideoGeometry(w, &tw, &th);
        OH_NativeWindow_NativeWindowHandleOpt(w, SET_BUFFER_GEOMETRY, tw, th);
        enhancer = std::make_unique<VideoEnhancer>();
        if (enhancer->Init(w, level, &enhanceError)) {
            Logf(LogLevel::Info, "画质增强输出 %dx%d", int(tw), int(th));
        } else {
            Logf(LogLevel::Warn, "画质增强不可用，改为直出：%s", enhanceError.c_str());
            enhancer.reset();
        }
    }

    auto d = std::make_unique<VideoDecoder>();
    std::string error;
    d->SetSmoothLevel(smoothLevel_);
    d->SetFreezeOnLoss(freezeOnLoss_);
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        d->SetFrameIntervalUs(gap_.IntervalUs());
    }
    if (enhancer) {
        VideoEnhancer* e = enhancer.get();
        d->SetFrameQueuedHook([e] { e->OnFrameQueued(); });
    }
    if (!d->Init(enhancer ? enhancer->InputWindow() : w, &error)) {
        d->Release();
        enhancer.reset();
        OH_NativeWindow_DestroyNativeWindow(w);
        return error;
    }
    out->window = w;
    out->enhancer = std::move(enhancer);
    out->decoder = std::move(d);
    out->enhanceError = enhanceError;
    return "";
}

size_t Session::InstallVideoLocked(VideoPipeline* p, bool replayGop) {
    size_t replayed = 0;
    if (replayGop) {
        // “取快照 + 送入解码器 + 切换”在同一把锁里完成，保证缓存与之后的实时包之间不漏帧、不乱序
        p->decoder->SetFallbackParamSets(gop_.ParamSets());
        auto gop = gop_.Snapshot();
        replayed = gop.size();
        p->decoder->Replay(std::move(gop));
    }
    window_ = p->window;
    enhancer_ = std::move(p->enhancer);
    enhanceError_ = p->enhanceError;
    video_ = std::move(p->decoder);
    p->window = nullptr;
    return replayed;
}

std::string Session::Start(const SourceFactory& factory, const SessionConfig& config) {
    Stop();
    smoothLevel_ = config.smoothLevel;
    netOpt_ = config.netOpt;
    freezeOnLoss_ = config.freezeOnLoss;
    enhanceLevel_ = config.enhanceLevel;
    lastSurfaceId_ = config.surfaceId;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        gap_.Reset();
        for (auto& n : lossEvents_) n = 0;
        lostFrames_ = 0;
        explicitLossPending_ = false;
        lastIdrUs_ = 0;
        keyframeIntervalMs_ = 0;
        decoderBase_ = DecoderCounters();
    }

    VideoPipeline pipeline;
    std::string error = CreateVideo(config.surfaceId, &pipeline);
    if (!error.empty()) return error;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        InstallVideoLocked(&pipeline, false);
        gop_.Clear();
        renderedBase_ = droppedBase_ = lateBase_ = 0;
        underrunBase_ = 0;
        lastCatchUpMs_ = -1;
    }

    audioEnabled_ = false;
    if (config.audio) {
        error = CreateAudio();
        if (error.empty()) {
            audioEnabled_ = true;
        } else {
            // 没声音也能看画面，不算致命错误
            Logf(LogLevel::Warn, "音频初始化失败，仅播放画面：%s", error.c_str());
        }
    }
    paused_ = false;

    sysdvr::StreamOptions opt;
    opt.video = true;
    opt.audio = audioEnabled_;
    // 每包 2 块（约 43ms）而不是 4 块（约 85ms）：音频延迟少约 40ms，字节数不变
    opt.audioBatching = 1;
    opt.turnOffConsoleScreen = config.screenOff;
    opt.nextExt = config.nextExt;
    opt.extAudio = config.extAudio;

    sysdvr::StreamCallbacks cb;
    cb.onVideo = [this](const uint8_t* p, size_t n, uint64_t ts) { OnVideo(p, n, ts); };
    // 始终挂上音频回调：播放中可以随时打开音频通道
    cb.onAudio = [this](const uint8_t* p, size_t n, uint64_t) { OnAudio(p, n); };
    cb.onStatus = [this](const std::string& msg) { SetMessage(msg); };
    cb.onVideoLoss = [this](sysdvr::LossCause cause) {
        std::lock_guard<std::mutex> lock(videoMutex_);
        HandleLossLocked(cause);
    };

    client_ = factory(opt, cb);
    if (!client_) {
        ReleaseRendering();
        return L("无法创建串流连接", "無法建立串流連線", "cannot create the stream connection");
    }
    client_->SetQuickAck(netOpt_);
    client_->Start();
    SetMessage(Format(L("正在连接 %s", "正在連線 %s", "Connecting to %s"), config.label.c_str()));
    return "";
}

void Session::OnVideo(const uint8_t* data, size_t size, uint64_t ts) {
    std::lock_guard<std::mutex> lock(videoMutex_);
    // Switch 端丢帧表现为时间戳断档：先处理丢失，再送当前帧（当前帧若不是关键帧也要一起丢）
    const int lost = gap_.OnFrame(ts);
    if (lost > 0) {
        lostFrames_ += uint64_t(lost);
        // 错误包顶替的帧不会走到这里，下一帧看到的断档就是它：已经按错误包处理过，不重复计次
        if (!explicitLossPending_) HandleLossLocked(sysdvr::LossCause::TimestampGap);
    }
    explicitLossPending_ = false;
    if (sysdvr::ContainsNalType(data, size, 5)) {
        if (lastIdrUs_ > 0 && ts > lastIdrUs_ && ts - lastIdrUs_ < 60'000'000) {
            const int ms = int((ts - lastIdrUs_) / 1000);
            keyframeIntervalMs_ = keyframeIntervalMs_ == 0 ? ms : (keyframeIntervalMs_ * 4 + ms) / 5;
        }
        lastIdrUs_ = ts;
    }
    // 前后台都维护 GOP 缓存（每帧一次拷贝，开销很小），切后台那一刻缓存里就有当前 GOP
    gop_.Push(data, size, ts);
    if (video_) {
        video_->SetFrameIntervalUs(gap_.IntervalUs());
        video_->Submit(data, size, ts);
    }
}

void Session::HandleLossLocked(sysdvr::LossCause cause) {
    ++lossEvents_[int(cause)];
    if (cause != sysdvr::LossCause::TimestampGap) explicitLossPending_ = true;
    // 缓存里的 GOP 从这里断了：作废，切回前台时从下一个关键帧开始，而不是重放一段花屏
    gop_.Clear();
    if (video_) video_->OnFrameLoss(cause);
}

void Session::SetFreezeOnLoss(bool on) {
    freezeOnLoss_ = on;
    std::lock_guard<std::mutex> lock(videoMutex_);
    if (video_) video_->SetFreezeOnLoss(on);
}

void Session::OnAudio(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (audio_) audio_->Write(data, size);  // 后台时 audio_ 为空，数据直接丢弃
}

void Session::ReleaseRendering() {
    ReleaseVideo();
    // 后台不能留着音频流：未接入 AVSession 的应用在后台出声会被系统静音并冻结
    ReleaseAudio();
}

void Session::ReleaseVideo() {
    std::unique_ptr<VideoDecoder> decoder;
    std::unique_ptr<VideoEnhancer> enhancer;
    OHNativeWindow* window = nullptr;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        decoder = std::move(video_);
        enhancer = std::move(enhancer_);
        window = window_;
        window_ = nullptr;
    }
    // 顺序：先停解码器（它往增强器的输入 surface 写），再停增强器（它往屏幕窗口写），最后销毁窗口
    if (decoder) {
        decoder->Release();
        std::lock_guard<std::mutex> lock(videoMutex_);
        renderedBase_ += decoder->FramesRendered();
        droppedBase_ += decoder->PacketsDropped();
        decoderBase_ += decoder->Counters();
        lateBase_ += decoder->LatePackets();
        if (decoder->LastCatchUpMs() >= 0) lastCatchUpMs_ = decoder->LastCatchUpMs();
    }
    if (enhancer) enhancer->Release();
    if (window != nullptr) OH_NativeWindow_DestroyNativeWindow(window);
}

std::string Session::SetEnhanceLevel(int level) {
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    enhanceLevel_ = level;
    enhanceBroken_ = false;  // 用户重新选了档位：再试一次
    // 没在播放或在后台：只记下档位，下次建视频链路时生效
    if (!client_ || paused_) return "";
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        if (enhancer_ && level > 0) {
            enhancer_->SetLevel(level);  // 只是换档：不用重建
            return "";
        }
        if (!enhancer_ && level == 0) return "";
    }
    // 开 ↔ 关：换一条视频链路（音频不受影响）
    return RebuildVideo();
}

std::string Session::RebuildVideo() {
    const int64_t t0 = NowMs();
    ReleaseVideo();
    VideoPipeline pipeline;
    std::string error = CreateVideo(lastSurfaceId_, &pipeline);
    if (!error.empty()) {
        Logf(LogLevel::Error, "重建视频链路失败：%s", error.c_str());
        return error;
    }
    const std::string enhanceError = pipeline.enhanceError;
    size_t replayed = 0;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        replayed = InstallVideoLocked(&pipeline, true);
    }
    Logf(LogLevel::Info, "视频链路已重建（画质增强 %s）：重放 %zu 包，耗时 %lld ms", enhancer_ ? "开" : "关", replayed,
         (long long)(NowMs() - t0));
    return enhanceError;
}

void Session::CheckEnhancerStall() {
    if (!client_ || paused_) return;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        // 解码器已经送进去约 1.5 秒的帧，增强器一帧都没吐出来：这台设备上引擎建得起来但不出画面
        if (!enhancer_ || !video_ || enhancer_->FramesOut() > 0 || video_->FramesRendered() < 45) return;
    }
    Logf(LogLevel::Warn, "画质增强没有输出画面，改为直出");
    enhanceBroken_ = true;
    RebuildVideo();
    std::lock_guard<std::mutex> lock(videoMutex_);
    enhanceError_ = L("本机的视频处理引擎没有输出画面，已改为直出", "本機的視訊處理引擎沒有輸出畫面，已改為直出",
                      "the video processing engine produced no picture on this device; switched to direct output");
}

std::string Session::CreateAudio() {
    auto audio = std::make_unique<AudioPlayer>();
    std::string error;
    if (!audio->Init(&error)) return error;
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        audio_ = std::move(audio);
    }
    SyncAudioToVideo();
    return "";
}

void Session::ReleaseAudio() {
    std::unique_ptr<AudioPlayer> audio;
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        audio = std::move(audio_);
        if (audio) underrunBase_ += audio->Underruns();
    }
    if (audio) audio->Release();
}

std::string Session::SetAudioEnabled(bool enabled) {
    if (!client_) {
        audioEnabled_ = enabled;
        return "";
    }
    if (enabled == audioEnabled_) return "";
    if (enabled) {
        // 后台时不建播放器（回前台 ResumeRendering 会建），但音频通道照常打开
        if (!paused_) {
            std::string error = CreateAudio();
            if (!error.empty()) return Format(L("音频初始化失败：%s", "音訊初始化失敗：%s", "audio init failed: %s"), error.c_str());
        }
        audioEnabled_ = true;
        client_->SetAudioEnabled(true);
    } else {
        audioEnabled_ = false;
        client_->SetAudioEnabled(false);  // TCP Bridge 下断开 9922，Switch 停发音频；USB/RTSP 下本地静音
        ReleaseAudio();
    }
    return "";
}

void Session::SetSmoothLevel(int level) {
    smoothLevel_ = level;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        if (video_) video_->SetSmoothLevel(level);
    }
    SyncAudioToVideo();
}

void Session::SetNetOptimization(bool enabled) {
    netOpt_ = enabled;
    if (client_) client_->SetQuickAck(enabled);
    Logf(LogLevel::Info, "TCP 快速确认：%s", enabled ? "开启" : "关闭");
}

std::string Session::SetExtAudio(const sysdvr::ExtAudioConfig& config) {
    if (!client_) return "";
    if (client_->SetExtAudio(config)) return "";
    // 没发出去：服务端没表明支持扩展（官方 SysDVR、USB）；或者支持但音频关着——已记下，下次音频握手带上
    if (!client_->GetStats().extSupported)
        return L("当前服务端不支持", "目前的伺服器不支援", "Not supported by the current server");
    return "";
}

std::vector<int> Session::SocketFds() const {
    return client_ ? client_->SocketFds() : std::vector<int>{};
}

void Session::SyncAudioToVideo() {
    // 音画对齐：视频有平滑缓冲时，音频至少缓冲同样久；游戏档把音频上限也压低
    int videoDelay = 0;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        if (video_) videoDelay = video_->SmoothDelayMs();
    }
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (!audio_) return;
    audio_->SetFloorMs(videoDelay);
    // 音频缓冲上限：关闭平滑时 250ms（玩游戏声音别落后太多）；游戏档 200ms；观看档 400ms
    const int level = smoothLevel_;
    audio_->SetMaxMs(level == 1 ? 200 : (level == 2 ? 400 : 250));
}

void Session::PauseRendering() {
    if (!client_ || paused_) return;
    paused_ = true;
    ReleaseRendering();
    Logf(LogLevel::Info, "已暂停渲染，后台继续接收");
    resumeState_ = ResumeState::None;
    SetMessage(L("后台接收中（不渲染）", "背景接收中（不繪製）", "Receiving in background (not rendering)"));
}

std::string Session::ResumeRendering(uint64_t surfaceId) {
    if (!client_) return L("没有进行中的串流", "沒有進行中的串流", "no active stream");
    if (!paused_) return "";

    const int64_t t0 = NowMs();
    VideoPipeline pipeline;
    std::string error = CreateVideo(surfaceId, &pipeline);
    if (!error.empty()) return error;  // 保持暂停状态，调用方可以换 surface 再试
    lastSurfaceId_ = surfaceId;

    size_t replayed = 0;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        replayed = InstallVideoLocked(&pipeline, true);
    }

    if (audioEnabled_) {
        error = CreateAudio();
        if (!error.empty()) Logf(LogLevel::Warn, "恢复音频失败：%s", error.c_str());
    }
    paused_ = false;
    Logf(LogLevel::Info, "已恢复渲染：重放 %zu 包，准备耗时 %lld ms", replayed, (long long)(NowMs() - t0));
    resumeAtMs_ = NowMs();
    resumeState_ = replayed > 0 ? ResumeState::CatchingUp : ResumeState::WaitingKeyframe;
    SetMessage(replayed > 0 ? L("已恢复，正在追到最新画面", "已恢復，正在追到最新畫面", "Resumed, catching up to live")
                            : L("已恢复，等待下一个关键帧", "已恢復，等待下一個關鍵影格", "Resumed, waiting for the next keyframe"));
    return "";
}

void Session::Stop() {
    // 顺序很重要：先停网络线程（之后不再有回调），再释放解码器/音频/窗口
    if (client_) {
        client_->Stop();
        client_.reset();
    }
    ReleaseRendering();
    std::lock_guard<std::mutex> lock(videoMutex_);
    gop_.Clear();
    paused_ = false;
}

SessionStats Session::Stats() {
    CheckEnhancerStall();  // 统计每秒轮询一次（主线程），顺便检查画质增强是否在出画面
    SyncAudioToVideo();  // 每秒随统计轮询一次，跟上平滑缓冲的变化
    UpdateResumeMessage();
    SessionStats s;
    s.active = client_ != nullptr;
    s.renderingPaused = paused_;
    {
        std::lock_guard<std::mutex> lock(messageMutex_);
        s.message = message_;
    }
    if (client_) s.bridge = client_->GetStats();
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        s.framesRendered = renderedBase_;
        s.videoDropped = droppedBase_;
        s.smoothLevel = smoothLevel_;
        s.videoLate = lateBase_;
        s.netOpt = netOpt_;
        if (video_) {
            s.framesRendered += video_->FramesRendered();
            s.videoDropped += video_->PacketsDropped();
            s.smoothDelayMs = video_->SmoothDelayMs();
            s.videoLate += video_->LatePackets();
            if (video_->LastCatchUpMs() >= 0) lastCatchUpMs_ = video_->LastCatchUpMs();
            s.decoderError = video_->LastError();
        }
        s.lastCatchUpMs = lastCatchUpMs_;
        s.gopPackets = gop_.Packets();
        s.gopBytes = gop_.Bytes();

        s.freezeOnLoss = freezeOnLoss_;
        for (int i = 0; i < int(sysdvr::LossCause::Count); ++i) s.lossEvents[i] = lossEvents_[i];
        s.lostFrames = lostFrames_;
        s.frameIntervalUs = int(gap_.IntervalUs());
        s.keyframeIntervalMs = keyframeIntervalMs_;
        s.decoder = decoderBase_;
        if (video_) {
            s.decoder += video_->Counters();
            s.frozen = video_->Frozen();
            s.lastFreezeMs = video_->LastFreezeMs();
            s.timing = video_->TakeRenderTiming();
        }
        s.enhanceRequested = enhanceLevel_;
        s.enhanceError = enhanceError_;
        if (enhancer_) {
            s.enhanceLevel = enhancer_->Level();
            s.enhanceLatencyUs = enhancer_->AvgLatencyUs();
            const std::string runtimeError = enhancer_->LastError();
            if (!runtimeError.empty()) s.enhanceError = runtimeError;
        }
    }
    s.vsyncPeriodUs = int(VsyncMonitor::Get().PeriodNs() / 1000);
    s.audioEnabled = audioEnabled_;
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        s.audioUnderruns = underrunBase_;
        if (audio_) {
            s.audioFastMode = audio_->FastMode();
            s.audioUnderruns += audio_->Underruns();
            s.audioBufferedMs = audio_->BufferedMs();
            s.audioTargetMs = audio_->TargetMs();
        }
    }
    return s;
}

void Session::UpdateResumeMessage() {
    const ResumeState state = resumeState_;
    if (state == ResumeState::None) return;
    int catchUpMs = -1;
    uint64_t rendered = 0;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        if (!video_) return;
        catchUpMs = video_->LastCatchUpMs();
        rendered = video_->FramesRendered();
    }
    if (state == ResumeState::CatchingUp && catchUpMs >= 0) {
        resumeState_ = ResumeState::None;
        SetMessage(Format(L("已恢复实时画面（追帧 %d ms）", "已恢復即時畫面（追幀 %d ms）", "Back to live (caught up in %d ms)"),
                          catchUpMs));
    } else if (rendered > 0 && (state == ResumeState::WaitingKeyframe || NowMs() - resumeAtMs_ > 6000)) {
        // 等到关键帧出了第一帧；或解码器追帧超时（5 秒）放弃跳帧后已经在正常出帧
        resumeState_ = ResumeState::None;
        SetMessage(L("已恢复实时画面", "已恢復即時畫面", "Back to live"));
    }
}

void Session::SetMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lock(messageMutex_);
    message_ = msg;
}
