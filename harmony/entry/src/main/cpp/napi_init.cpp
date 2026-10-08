// ArkTS <-> C++ 接口（libentry.so），声明见 types/libentry/Index.d.ts。
// 所有函数都在 ArkTS 主线程调用；状态由 ArkTS 定时轮询，避免跨线程回调。
#include <hilog/log.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <napi/native_api.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "i18n.h"
#include "log.h"
#include "rtsp_probe.h"
#include "session.h"
#include "tcp_bridge.h"
#include "usb_host.h"
#include "usb_stream.h"

using sysdvr::L;

namespace {

std::unique_ptr<sysdvr::Discovery> g_discovery;
std::unique_ptr<Session> g_session;
// USB 退回 ArkTS 传输时：泵归串流客户端所有，这里只留一个指针给 NAPI 喂数据（都在主线程访问）
JsPumpTransport* g_pump = nullptr;
napi_threadsafe_function g_usbWriteFn = nullptr;

enum ConnectMode { kModeTcp = 0, kModeUsb = 1, kModeRtsp = 2 };

std::string GetString(napi_env env, napi_value v) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) return "";
    std::vector<char> buf(len + 1, '\0');
    napi_get_value_string_utf8(env, v, buf.data(), buf.size(), &len);
    return std::string(buf.data(), len);
}

bool GetBool(napi_env env, napi_value v, bool fallback) {
    bool b = fallback;
    if (napi_get_value_bool(env, v, &b) != napi_ok) return fallback;
    return b;
}

napi_value MakeString(napi_env env, const std::string& s) {
    napi_value v;
    napi_create_string_utf8(env, s.c_str(), s.size(), &v);
    return v;
}

napi_value MakeBool(napi_env env, bool b) {
    napi_value v;
    napi_get_boolean(env, b, &v);
    return v;
}

napi_value MakeNumber(napi_env env, double d) {
    napi_value v;
    napi_create_double(env, d, &v);
    return v;
}

napi_value Undefined(napi_env env) {
    napi_value v;
    napi_get_undefined(env, &v);
    return v;
}

void Set(napi_env env, napi_value obj, const char* key, napi_value v) { napi_set_named_property(env, obj, key, v); }

// startDiscovery(): boolean
napi_value StartDiscovery(napi_env env, napi_callback_info) {
    if (!g_discovery) g_discovery = std::make_unique<sysdvr::Discovery>();
    return MakeBool(env, g_discovery->Start());
}

// stopDiscovery(): void
napi_value StopDiscovery(napi_env env, napi_callback_info) {
    if (g_discovery) {
        g_discovery->Stop();
        g_discovery.reset();
    }
    return Undefined(env);
}

// getDevices(): DeviceInfo[]
napi_value GetDevices(napi_env env, napi_callback_info) {
    napi_value arr;
    napi_create_array(env, &arr);
    if (!g_discovery) return arr;
    const auto devices = g_discovery->Devices();
    for (size_t i = 0; i < devices.size(); ++i) {
        napi_value obj;
        napi_create_object(env, &obj);
        Set(env, obj, "ip", MakeString(env, devices[i].ip));
        Set(env, obj, "version", MakeString(env, devices[i].version));
        Set(env, obj, "protocol", MakeString(env, devices[i].protocol));
        Set(env, obj, "serial", MakeString(env, devices[i].serial));
        napi_set_element(env, arr, uint32_t(i), obj);
    }
    return arr;
}

int32_t GetInt(napi_env env, napi_value v, int32_t fallback) {
    int32_t i = fallback;
    if (napi_get_value_int32(env, v, &i) != napi_ok) return fallback;
    return i;
}

napi_value Prop(napi_env env, napi_value obj, const char* key) {
    napi_value v = nullptr;
    bool has = false;
    if (napi_has_named_property(env, obj, key, &has) != napi_ok || !has) return nullptr;
    napi_get_named_property(env, obj, key, &v);
    return v;
}

void StopSession() {
    // 先让阻塞在 ArkTS 泵上的读写立刻返回，串流线程才能尽快退出
    if (g_pump) g_pump->Close();
    if (g_session) {
        g_session->Stop();
        g_session.reset();
    }
    g_pump = nullptr;
    if (g_usbWriteFn) {
        napi_release_threadsafe_function(g_usbWriteFn, napi_tsfn_release);
        g_usbWriteFn = nullptr;
    }
}

// 实验扩展：ArkTS 传来的编码参数（0 PCM48 / 1 PCM24 / 2 ADPCM / 3 Opus）
sysdvr::ExtAudioConfig MakeExtAudio(int codec, int kbps, int complexity, int frameMs) {
    sysdvr::ExtAudioConfig c;
    c.codec = sysdvr::AudioCodec(codec >= 0 && codec < sysdvr::kAudioCodecCount ? codec : 0);
    c.opusKbps = kbps;
    c.opusComplexity = complexity;
    c.opusFrameMs = frameMs;
    return sysdvr::ClampExtAudioConfig(c);
}

// 只记一次日志：本机系统 AVCodec 有没有 Opus 解码器（我们用内置的 libopus，这里只是留个记录便于排查）
void LogSystemOpusOnce() {
    static bool logged = false;
    if (logged) return;
    logged = true;
    OH_AVCapability* cap = OH_AVCodec_GetCapability(OH_AVCODEC_MIMETYPE_AUDIO_OPUS, false);
    sysdvr::Logf(sysdvr::LogLevel::Info, "系统 Opus 解码器：%s（实验扩展用内置 libopus 定点解码）",
                 cap != nullptr ? OH_AVCapability_GetName(cap) : "无");
}

// 线程安全函数的回调：在 ArkTS 主线程上调用 onUsbWrite()，让 ArkTS 来取要写的数据
void CallUsbWrite(napi_env env, napi_value jsCb, void*, void*) {
    if (env == nullptr || jsCb == nullptr) return;
    napi_value undefined;
    napi_get_undefined(env, &undefined);
    napi_call_function(env, undefined, jsCb, 0, nullptr, nullptr);
}

// connect(config: ConnectConfig): string（空串 = 成功），字段见 Index.d.ts
napi_value Connect(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return MakeString(env, L("参数不足", "參數不足", "missing arguments"));
    const napi_value cfg = args[0];
    auto str = [&](const char* k) { napi_value v = Prop(env, cfg, k); return v ? GetString(env, v) : std::string(); };
    auto num = [&](const char* k, int32_t d) { napi_value v = Prop(env, cfg, k); return v ? GetInt(env, v, d) : d; };
    auto flag = [&](const char* k, bool d) { napi_value v = Prop(env, cfg, k); return v ? GetBool(env, v, d) : d; };

    SessionConfig config;
    config.surfaceId = std::strtoull(str("surfaceId").c_str(), nullptr, 10);
    config.audio = flag("audio", true);
    config.smoothLevel = num("smoothLevel", 0);
    config.netOpt = flag("netOpt", true);
    config.screenOff = flag("screenOff", false);
    config.freezeOnLoss = flag("freezeOnLoss", true);
    config.enhanceLevel = num("enhanceLevel", 0);
    config.extAudio = MakeExtAudio(num("extCodec", 0), num("extOpusKbps", 96), num("extOpusComplexity", 5),
                                   num("extOpusFrameMs", 20));
    if (config.surfaceId == 0) return MakeString(env, L("surfaceId 无效", "surfaceId 無效", "invalid surfaceId"));

    StopSession();
    const int mode = num("mode", kModeTcp);
    SourceFactory factory;
    if (mode == kModeUsb) {
        const int fd = num("usbFd", -1);
        const int iface = num("usbInterface", 0);
        const uint8_t epIn = uint8_t(num("usbEpIn", 0x81));
        const uint8_t epOut = uint8_t(num("usbEpOut", 0x01));
        if (flag("usbPump", false)) {
            napi_value cb = Prop(env, cfg, "onUsbWrite");
            if (cb == nullptr) return MakeString(env, L("缺少 onUsbWrite 回调", "缺少 onUsbWrite 回呼", "missing onUsbWrite callback"));
            napi_value name;
            napi_create_string_utf8(env, "usbWrite", NAPI_AUTO_LENGTH, &name);
            if (napi_create_threadsafe_function(env, cb, nullptr, name, 0, 1, nullptr, nullptr, nullptr, CallUsbWrite,
                                                &g_usbWriteFn) != napi_ok)
                return MakeString(env, L("创建 USB 写回调失败", "建立 USB 寫入回呼失敗", "failed to create the USB write callback"));
            const napi_threadsafe_function fn = g_usbWriteFn;
            auto pump = std::make_unique<JsPumpTransport>(
                [fn] { napi_call_threadsafe_function(fn, nullptr, napi_tsfn_nonblocking); });
            g_pump = pump.get();
            auto holder = std::make_shared<std::unique_ptr<JsPumpTransport>>(std::move(pump));
            factory = [holder](const sysdvr::StreamOptions& o, const sysdvr::StreamCallbacks& c) {
                return std::make_unique<sysdvr::UsbStreamClient>(std::move(*holder), o, c);
            };
            config.label = L("USB（兼容传输）", "USB（相容傳輸）", "USB (compatibility mode)");
        } else {
            if (fd < 0) return MakeString(env, L("USB 描述符无效", "USB 描述元無效", "invalid USB file descriptor"));
            factory = [fd, iface, epIn, epOut](const sysdvr::StreamOptions& o, const sysdvr::StreamCallbacks& c) {
                return std::make_unique<sysdvr::UsbStreamClient>(
                    std::make_unique<UsbFsTransport>(fd, iface, epIn, epOut), o, c);
            };
            config.label = "USB";
        }
        config.netOpt = false;  // 快速确认、网络加速只对网络模式有意义
    } else if (mode == kModeTcp) {
        const std::string host = str("host");
        if (host.empty()) return MakeString(env, L("IP 地址无效", "IP 位址無效", "invalid IP address"));
        factory = [host](const sysdvr::StreamOptions& o, const sysdvr::StreamCallbacks& c) {
            return std::make_unique<sysdvr::TcpBridgeClient>(host, o, c);
        };
        config.label = host;
        // 实验扩展只用于 TCP Bridge（USB 客户端也会强制去掉扩展标志）
        config.nextExt = flag("nextExt", false);
        if (config.nextExt) LogSystemOpusOnce();
    } else {
        return MakeString(env, L("RTSP 模式由播放器处理，不走这里", "RTSP 模式由播放器處理，不走這裡", "RTSP is handled by the player, not here"));
    }

    g_session = std::make_unique<Session>();
    std::string err = g_session->Start(factory, config);
    if (!err.empty()) StopSession();
    return MakeString(env, err);
}

// disconnect(): void
napi_value Disconnect(napi_env env, napi_callback_info) {
    StopSession();
    return Undefined(env);
}

// usbProbe(fd: number, interfaceId: number): UsbProbeResult —— 验证能否直接对 usbfs 发 ioctl，并读出序列号
napi_value UsbProbe(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    UsbProbeResult r;
    if (argc < 2) {
        r.error = L("参数不足", "參數不足", "missing arguments");
    } else {
        r = ProbeUsbFs(GetInt(env, args[0], -1), GetInt(env, args[1], 0), argc >= 3 ? GetInt(env, args[2], -1) : -1);
    }
    napi_value obj;
    napi_create_object(env, &obj);
    Set(env, obj, "ok", MakeBool(env, r.ok));
    Set(env, obj, "serial", MakeString(env, r.serial));
    Set(env, obj, "error", MakeString(env, r.error));
    Set(env, obj, "bulkRefused", MakeBool(env, r.bulkRefused));
    return obj;
}

// setLanguage(lang: number): void —— 0 简体 / 1 繁体 / 2 英文，影响之后 native 层返回的状态和错误文字
napi_value SetLanguage(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    sysdvr::SetLanguage(argc >= 1 ? GetInt(env, args[0], sysdvr::kLangHans) : sysdvr::kLangHans);
    return Undefined(env);
}

// usbFeed(data: ArrayBuffer, length: number): void —— 兼容传输：ArkTS 读到的 IN 端点数据
napi_value UsbFeed(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (!g_pump || argc < 2) return Undefined(env);
    void* data = nullptr;
    size_t size = 0;
    if (napi_get_arraybuffer_info(env, args[0], &data, &size) != napi_ok) return Undefined(env);
    const int32_t n = GetInt(env, args[1], 0);
    if (n > 0 && size_t(n) <= size) g_pump->Feed(static_cast<const uint8_t*>(data), size_t(n));
    return Undefined(env);
}

// usbFeedError(): void —— 兼容传输：设备断开或读取出错
napi_value UsbFeedError(napi_env env, napi_callback_info) {
    if (g_pump) g_pump->FeedError();
    return Undefined(env);
}

// usbTakeWrite(): ArrayBuffer | undefined —— 兼容传输：取出待写入 OUT 端点的数据
napi_value UsbTakeWrite(napi_env env, napi_callback_info) {
    std::vector<uint8_t> out;
    if (!g_pump || !g_pump->TakeWrite(&out)) return Undefined(env);
    void* data = nullptr;
    napi_value buf;
    napi_create_arraybuffer(env, out.size(), &data, &buf);
    std::memcpy(data, out.data(), out.size());
    return buf;
}

// usbWriteDone(result: number): void —— 兼容传输：bulkTransfer 的返回值（写入字节数，<0 失败）
napi_value UsbWriteDone(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (g_pump) g_pump->WriteDone(argc >= 1 ? GetInt(env, args[0], -1) : -1);
    return Undefined(env);
}

// pauseVideo(): void —— 切后台：继续接收，释放解码器与音频，只维护 GOP 缓存
napi_value PauseVideo(napi_env env, napi_callback_info) {
    if (g_session) g_session->PauseRendering();
    return Undefined(env);
}

// resumeVideo(surfaceId: string): string —— 回前台：重建解码器并重放 GOP（空串 = 成功）
napi_value ResumeVideo(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (!g_session) return MakeString(env, L("没有进行中的串流", "沒有進行中的串流", "no active stream"));
    if (argc < 1) return MakeString(env, L("参数不足", "參數不足", "missing arguments"));
    const uint64_t surfaceId = std::strtoull(GetString(env, args[0]).c_str(), nullptr, 10);
    if (surfaceId == 0) return MakeString(env, L("surfaceId 无效", "surfaceId 無效", "invalid surfaceId"));
    return MakeString(env, g_session->ResumeRendering(surfaceId));
}

// setAudioEnabled(enabled: boolean): string —— 播放中开关音频通道，空串 = 成功
napi_value SetAudioEnabled(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (!g_session) return MakeString(env, "");
    return MakeString(env, g_session->SetAudioEnabled(argc >= 1 && GetBool(env, args[0], true)));
}

// setSmoothLevel(level: number): void —— 0 关闭 / 1 游戏 / 2 观看
napi_value SetSmoothLevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (g_session) g_session->SetSmoothLevel(argc >= 1 ? GetInt(env, args[0], 0) : 0);
    return Undefined(env);
}

// setNetOptimization(enabled: boolean): void —— TCP 快速确认
napi_value SetNetOptimization(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (g_session) g_session->SetNetOptimization(argc >= 1 && GetBool(env, args[0], true));
    return Undefined(env);
}

// setFreezeOnLoss(on: boolean): void —— 丢帧时定格画面
napi_value SetFreezeOnLoss(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (g_session) g_session->SetFreezeOnLoss(argc >= 1 && GetBool(env, args[0], true));
    return Undefined(env);
}

// setEnhanceLevel(level: number): string —— 画质增强 0 关闭 / 1 标准 / 2 高；空串 = 成功
napi_value SetEnhanceLevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (!g_session) return MakeString(env, "");
    return MakeString(env, g_session->SetEnhanceLevel(argc >= 1 ? GetInt(env, args[0], 0) : 0));
}

// setExtAudio(codec, kbps, complexity, frameMs): string —— 实验扩展：运行中切换音频编码；
// 空串 = 已发出（或音频关着、已记下），否则为原因（例如当前服务端不支持）
napi_value SetExtAudio(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (!g_session) return MakeString(env, "");
    if (argc < 4) return MakeString(env, L("参数不足", "參數不足", "missing arguments"));
    return MakeString(env, g_session->SetExtAudio(MakeExtAudio(GetInt(env, args[0], 0), GetInt(env, args[1], 96),
                                                               GetInt(env, args[2], 5), GetInt(env, args[3], 20))));
}

// getSocketFds(): number[] —— 当前已连接的 socket，给网络加速接口标记高优先级
napi_value GetSocketFds(napi_env env, napi_callback_info) {
    napi_value arr;
    napi_create_array(env, &arr);
    if (!g_session) return arr;
    const auto fds = g_session->SocketFds();
    for (size_t i = 0; i < fds.size(); ++i) napi_set_element(env, arr, uint32_t(i), MakeNumber(env, fds[i]));
    return arr;
}

// getStats(): SessionStats
napi_value GetStats(napi_env env, napi_callback_info) {
    SessionStats s;
    if (g_session) s = g_session->Stats();

    napi_value obj;
    napi_create_object(env, &obj);
    Set(env, obj, "active", MakeBool(env, s.active));
    Set(env, obj, "renderingPaused", MakeBool(env, s.renderingPaused));
    Set(env, obj, "message", MakeString(env, s.message));
    Set(env, obj, "videoConnected", MakeBool(env, s.bridge.videoConnected));
    Set(env, obj, "audioConnected", MakeBool(env, s.bridge.audioConnected));
    Set(env, obj, "videoPackets", MakeNumber(env, double(s.bridge.videoPackets)));
    Set(env, obj, "videoBytes", MakeNumber(env, double(s.bridge.videoBytes)));
    Set(env, obj, "audioPackets", MakeNumber(env, double(s.bridge.audioPackets)));
    Set(env, obj, "audioBytes", MakeNumber(env, double(s.bridge.audioBytes)));
    Set(env, obj, "replayHits", MakeNumber(env, double(s.bridge.replayHits)));
    Set(env, obj, "resyncs", MakeNumber(env, double(s.bridge.resyncs)));
    Set(env, obj, "reconnects", MakeNumber(env, double(s.bridge.reconnects)));
    Set(env, obj, "framesRendered", MakeNumber(env, double(s.framesRendered)));
    Set(env, obj, "videoDropped", MakeNumber(env, double(s.videoDropped)));
    Set(env, obj, "lastCatchUpMs", MakeNumber(env, double(s.lastCatchUpMs)));
    Set(env, obj, "smoothLevel", MakeNumber(env, s.smoothLevel));
    Set(env, obj, "netOpt", MakeBool(env, s.netOpt));
    Set(env, obj, "smoothDelayMs", MakeNumber(env, double(s.smoothDelayMs)));
    Set(env, obj, "videoLate", MakeNumber(env, double(s.videoLate)));
    Set(env, obj, "gopPackets", MakeNumber(env, double(s.gopPackets)));
    Set(env, obj, "gopBytes", MakeNumber(env, double(s.gopBytes)));
    Set(env, obj, "audioEnabled", MakeBool(env, s.audioEnabled));
    Set(env, obj, "audioFastMode", MakeBool(env, s.audioFastMode));
    Set(env, obj, "audioUnderruns", MakeNumber(env, double(s.audioUnderruns)));
    Set(env, obj, "audioBufferedMs", MakeNumber(env, double(s.audioBufferedMs)));
    Set(env, obj, "audioTargetMs", MakeNumber(env, double(s.audioTargetMs)));
    Set(env, obj, "decoderError", MakeString(env, s.decoderError));

    using sysdvr::LossCause;
    Set(env, obj, "freezeOnLoss", MakeBool(env, s.freezeOnLoss));
    Set(env, obj, "frozen", MakeBool(env, s.frozen));
    Set(env, obj, "lossGap", MakeNumber(env, double(s.lossEvents[int(LossCause::TimestampGap)])));
    Set(env, obj, "lossError", MakeNumber(env, double(s.lossEvents[int(LossCause::ErrorPacket)])));
    Set(env, obj, "lossReplayMiss", MakeNumber(env, double(s.lossEvents[int(LossCause::ReplayMiss)])));
    Set(env, obj, "lossResync", MakeNumber(env, double(s.lossEvents[int(LossCause::Resync)])));
    Set(env, obj, "lossOversize", MakeNumber(env, double(s.decoder.oversize)));
    Set(env, obj, "lostFrames", MakeNumber(env, double(s.lostFrames)));
    Set(env, obj, "frameIntervalUs", MakeNumber(env, double(s.frameIntervalUs)));
    Set(env, obj, "keyframeIntervalMs", MakeNumber(env, double(s.keyframeIntervalMs)));
    Set(env, obj, "freezeEvents", MakeNumber(env, double(s.decoder.freezeEvents)));
    Set(env, obj, "freezeTotalMs", MakeNumber(env, double(s.decoder.freezeTotalMs)));
    Set(env, obj, "freezeGiveUps", MakeNumber(env, double(s.decoder.freezeGiveUps)));
    Set(env, obj, "lastFreezeMs", MakeNumber(env, double(s.lastFreezeMs)));
    Set(env, obj, "vsyncPeriodUs", MakeNumber(env, double(s.vsyncPeriodUs)));
    Set(env, obj, "vsyncDeferred", MakeNumber(env, double(s.decoder.vsyncDeferred)));
    Set(env, obj, "vsyncDropped", MakeNumber(env, double(s.decoder.vsyncDropped)));
    Set(env, obj, "stutters", MakeNumber(env, double(s.decoder.stutters)));
    Set(env, obj, "renderJitterMs", MakeNumber(env, double(s.timing.jitterMs10) / 10.0));
    Set(env, obj, "renderMaxGapMs", MakeNumber(env, double(s.timing.maxGapMs)));
    Set(env, obj, "enhanceRequested", MakeNumber(env, double(s.enhanceRequested)));
    Set(env, obj, "enhanceLevel", MakeNumber(env, double(s.enhanceLevel)));
    Set(env, obj, "enhanceLatencyUs", MakeNumber(env, double(s.enhanceLatencyUs)));
    Set(env, obj, "enhanceError", MakeString(env, s.enhanceError));

    // Switch 内存报告（协议 03 握手应答），单位字节
    const sysdvr::SwitchMemory& m = s.bridge.switchMemory;
    Set(env, obj, "switchMemValid", MakeBool(env, m.valid));
    Set(env, obj, "switchMemQueryResult", MakeNumber(env, double(m.queryResult)));
    Set(env, obj, "systemPoolSize", MakeNumber(env, double(m.systemSize)));
    Set(env, obj, "systemPoolUsed", MakeNumber(env, double(m.systemUsed)));
    Set(env, obj, "appletPoolSize", MakeNumber(env, double(m.appletSize)));
    Set(env, obj, "appletPoolUsed", MakeNumber(env, double(m.appletUsed)));
    Set(env, obj, "applicationPoolSize", MakeNumber(env, double(m.applicationSize)));
    Set(env, obj, "applicationPoolUsed", MakeNumber(env, double(m.applicationUsed)));
    Set(env, obj, "systemUnsafeSize", MakeNumber(env, double(m.systemUnsafeSize)));
    Set(env, obj, "systemUnsafeUsed", MakeNumber(env, double(m.systemUnsafeUsed)));

    // 实验扩展：音频编码（来自音频包标记和 ExtAudioHeader）与请求值
    const sysdvr::BridgeStats& b = s.bridge;
    Set(env, obj, "extSupported", MakeBool(env, b.extSupported));
    Set(env, obj, "audioCodec", MakeNumber(env, b.audioCodec));
    Set(env, obj, "audioKbpsConfig", MakeNumber(env, b.audioKbpsConfig));
    Set(env, obj, "opusComplexity", MakeNumber(env, b.opusComplexity));
    Set(env, obj, "opusFrameMs", MakeNumber(env, b.opusFrameMs));
    Set(env, obj, "audioEncodeUs", MakeNumber(env, double(b.audioEncodeUs)));
    Set(env, obj, "audioDecodeErrors", MakeNumber(env, double(b.audioDecodeErrors)));
    Set(env, obj, "extAudioPending", MakeBool(env, b.extAudioPending));
    Set(env, obj, "extReqCodec", MakeNumber(env, int(b.extAudioRequested.codec)));
    Set(env, obj, "extReqKbps", MakeNumber(env, b.extAudioRequested.opusKbps));
    Set(env, obj, "extReqComplexity", MakeNumber(env, b.extAudioRequested.opusComplexity));
    Set(env, obj, "extReqFrameMs", MakeNumber(env, b.extAudioRequested.opusFrameMs));
    // 诊断包：最近一个窗口 + 累计
    const sysdvr::ExtDiag& d = b.diagLast;
    const sysdvr::ExtDiagTotals& t = b.diagTotal;
    Set(env, obj, "diagPackets", MakeNumber(env, double(b.diagPackets)));
    Set(env, obj, "diagIntervalMs", MakeNumber(env, d.intervalMs));
    Set(env, obj, "diagFramesSent", MakeNumber(env, d.videoFramesSent));
    Set(env, obj, "diagGrcGaps", MakeNumber(env, d.videoGrcGaps));
    Set(env, obj, "diagBlockTotalUs", MakeNumber(env, d.videoSendBlockTotalUs));
    Set(env, obj, "diagBlockMaxUs", MakeNumber(env, d.videoSendBlockMaxUs));
    Set(env, obj, "diagSendsOver20ms", MakeNumber(env, d.videoSendsOver20ms));
    Set(env, obj, "diagGapsAfterSlowSend", MakeNumber(env, d.gapsAfterSlowSend));
    Set(env, obj, "diagAudioPackets", MakeNumber(env, d.audioPackets));
    Set(env, obj, "diagAudioEncodeUs", MakeNumber(env, d.audioEncodeTotalUs));
    Set(env, obj, "diagAudioBlockUs", MakeNumber(env, d.audioSendBlockTotalUs));
    Set(env, obj, "diagCore3IdlePermille", MakeNumber(env, d.core3IdlePermille));
    Set(env, obj, "diagCpuPermille", MakeNumber(env, d.sysdvrCpuPermille));
    Set(env, obj, "diagTosFlags", MakeNumber(env, d.tosFlags));
    Set(env, obj, "diagTotalIntervalMs", MakeNumber(env, double(t.intervalMs)));
    Set(env, obj, "diagTotalFramesSent", MakeNumber(env, double(t.videoFramesSent)));
    Set(env, obj, "diagTotalGrcGaps", MakeNumber(env, double(t.videoGrcGaps)));
    Set(env, obj, "diagTotalBlockUs", MakeNumber(env, double(t.videoSendBlockTotalUs)));
    Set(env, obj, "diagTotalBlockMaxUs", MakeNumber(env, double(t.videoSendBlockMaxUs)));
    Set(env, obj, "diagTotalSendsOver20ms", MakeNumber(env, double(t.videoSendsOver20ms)));
    Set(env, obj, "diagTotalGapsAfterSlowSend", MakeNumber(env, double(t.gapsAfterSlowSend)));
    Set(env, obj, "diagTotalAudioPackets", MakeNumber(env, double(t.audioPackets)));
    Set(env, obj, "diagTotalAudioEncodeUs", MakeNumber(env, double(t.audioEncodeTotalUs)));
    Set(env, obj, "diagTotalAudioBlockUs", MakeNumber(env, double(t.audioSendBlockTotalUs)));
    return obj;
}

// rtspProbe(host: string, sampleMs: number): Promise<RtspProbeResult> —— 在工作线程里跑，耗时约 sampleMs + 1 秒
struct ProbeWork {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string host;
    int sampleMs = 3000;
    sysdvr::RtspProbeResult result;
};

napi_value RtspProbe(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    auto* w = new ProbeWork();
    w->host = argc >= 1 ? GetString(env, args[0]) : "";
    w->sampleMs = argc >= 2 ? GetInt(env, args[1], 3000) : 3000;
    napi_value promise;
    napi_create_promise(env, &w->deferred, &promise);
    napi_value name;
    napi_create_string_utf8(env, "rtspProbe", NAPI_AUTO_LENGTH, &name);
    napi_create_async_work(
        env, nullptr, name,
        [](napi_env, void* data) {
            auto* pw = static_cast<ProbeWork*>(data);
            pw->result = sysdvr::ProbeRtsp(pw->host, 6666, pw->sampleMs);
        },
        [](napi_env e, napi_status, void* data) {
            auto* pw = static_cast<ProbeWork*>(data);
            const sysdvr::RtspProbeResult& r = pw->result;
            napi_value obj;
            napi_create_object(e, &obj);
            Set(e, obj, "reachable", MakeBool(e, r.reachable));
            Set(e, obj, "described", MakeBool(e, r.described));
            Set(e, obj, "playing", MakeBool(e, r.playing));
            Set(e, obj, "videoPackets", MakeNumber(e, r.videoPackets));
            Set(e, obj, "audioPackets", MakeNumber(e, r.audioPackets));
            Set(e, obj, "bytes", MakeNumber(e, double(r.bytes)));
            Set(e, obj, "error", MakeString(e, r.error));
            Set(e, obj, "log", MakeString(e, r.log));
            napi_resolve_deferred(e, pw->deferred, obj);
            napi_delete_async_work(e, pw->work);
            delete pw;
        },
        w, &w->work);
    napi_queue_async_work(env, w->work);
    return promise;
}

void HilogSink(sysdvr::LogLevel level, const std::string& msg) {
    ::LogLevel l = LOG_INFO;
    switch (level) {
        case sysdvr::LogLevel::Debug: l = LOG_DEBUG; break;
        case sysdvr::LogLevel::Info: l = LOG_INFO; break;
        case sysdvr::LogLevel::Warn: l = LOG_WARN; break;
        case sysdvr::LogLevel::Error: l = LOG_ERROR; break;
    }
    OH_LOG_Print(LOG_APP, l, 0x5344, "SysDVR", "%{public}s", msg.c_str());
}

}  // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    sysdvr::SetLogSink(&HilogSink);
    napi_property_descriptor desc[] = {
        {"startDiscovery", nullptr, StartDiscovery, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopDiscovery", nullptr, StopDiscovery, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getDevices", nullptr, GetDevices, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"connect", nullptr, Connect, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"disconnect", nullptr, Disconnect, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pauseVideo", nullptr, PauseVideo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resumeVideo", nullptr, ResumeVideo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setAudioEnabled", nullptr, SetAudioEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setSmoothLevel", nullptr, SetSmoothLevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setNetOptimization", nullptr, SetNetOptimization, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setFreezeOnLoss", nullptr, SetFreezeOnLoss, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setEnhanceLevel", nullptr, SetEnhanceLevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setExtAudio", nullptr, SetExtAudio, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getSocketFds", nullptr, GetSocketFds, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getStats", nullptr, GetStats, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"usbProbe", nullptr, UsbProbe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"usbFeed", nullptr, UsbFeed, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"usbFeedError", nullptr, UsbFeedError, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"usbTakeWrite", nullptr, UsbTakeWrite, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"usbWriteDone", nullptr, UsbWriteDone, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"rtspProbe", nullptr, RtspProbe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setLanguage", nullptr, SetLanguage, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module g_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void) { napi_module_register(&g_module); }
