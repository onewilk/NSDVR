// NSDVR 实验扩展单测（docs/nsdvr-ext-protocol.md）：
//   协议字节（握手 Reserved、控制消息、ExtAudioHeader、ExtDiag、诊断包包头校验）、
//   PCM24 升采样（流式与整段卷积逐采样一致、频响）、IMA ADPCM（与编码器重建逐采样一致、丢包后重新同步）、
//   Opus（third_party 定点解码器 vs Homebrew libopus 参考解码）、运行中切换编码、异常输入不崩溃、
//   包分发器的统计与诊断包，以及 tools/ext_vectors/ 里另一端生成的测试向量（存在时）。
// Opus 编码器和参考解码器用 dlopen 从 Homebrew 的 libopus 里取（macOS 两级命名空间，不会和静态链接的
// 定点解码器冲突）；找不到 libopus 时跳过依赖它的用例。
//
//   ext_audio_test [--vectors 目录]
#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "../core/ext_audio.h"
#include "../core/stream_source.h"
#include "../core/sysdvr_protocol.h"

using namespace sysdvr;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond, ...)                                          \
    do {                                                          \
        ++g_checks;                                               \
        if (!(cond)) {                                            \
            ++g_failures;                                         \
            std::printf("  ✗ %s:%d %s — ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                             \
            std::printf("\n");                                    \
        }                                                         \
    } while (0)

void PutU16(std::vector<uint8_t>* v, unsigned x) {
    v->push_back(uint8_t(x));
    v->push_back(uint8_t(x >> 8));
}
void PutU32(std::vector<uint8_t>* v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v->push_back(uint8_t(x >> (8 * i)));
}

// 测试信号：48 kHz 立体声 s16。左声道 440 Hz + 5 kHz，右声道 100 Hz→10 kHz 扫频，再加一点噪声
std::vector<int16_t> TestSignal(int frames, uint32_t seed = 1) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, 30.0);
    std::vector<int16_t> x(size_t(frames) * 2);
    double phase = 0;
    for (int i = 0; i < frames; ++i) {
        const double t = double(i) / 48000.0;
        const double l = 9000 * std::sin(2 * M_PI * 440 * t) + 3000 * std::sin(2 * M_PI * 5000 * t) + noise(rng);
        const double f = 100.0 * std::pow(100.0, double(i) / frames);
        phase += 2 * M_PI * f / 48000.0;
        const double r = 8000 * std::sin(phase) + noise(rng);
        x[size_t(i) * 2] = int16_t(std::lrint(l));
        x[size_t(i) * 2 + 1] = int16_t(std::lrint(r));
    }
    return x;
}

double SnrDb(const std::vector<int16_t>& ref, const std::vector<int16_t>& test, size_t refOffset, size_t testOffset,
             size_t n) {
    double s = 0, e = 0;
    for (size_t i = 0; i < n; ++i) {
        const double a = ref[refOffset + i], b = test[testOffset + i];
        s += a * a;
        e += (a - b) * (a - b);
    }
    return 10 * std::log10((s + 1e-9) / (e + 1e-9));
}

// ---------------------------------------------------------------- 服务端行为模拟（只给测试造数据）

// 63 抽头半带低通（截止 fs/4）降采样，状态跨包保留；系数不需要与服务端逐位相同
struct Downsampler2x {
    std::vector<double> h;
    std::vector<double> hist[2];
    Downsampler2x() {
        const int taps = 63;
        auto i0 = [](double x) {
            double sum = 1, term = 1;
            for (int k = 1; k < 30; ++k) {
                term *= (x / 2 / k) * (x / 2 / k);
                sum += term;
            }
            return sum;
        };
        double total = 0;
        for (int i = 0; i < taps; ++i) {
            const double n = i - (taps - 1) / 2.0;
            const double arg = 2 * 12000.0 / 48000.0 * n;
            const double sinc = n == 0 ? 1 : std::sin(M_PI * arg) / (M_PI * arg);
            const double r = 2.0 * i / (taps - 1) - 1;
            const double w = i0(8 * std::sqrt(std::max(0.0, 1 - r * r))) / i0(8);
            h.push_back(sinc * w);
            total += sinc * w;
        }
        for (auto& v : h) v /= total;
        for (auto& b : hist) b.assign(h.size(), 0.0);
    }
    // in：frames（偶数）帧 48 kHz；输出 frames/2 帧 24 kHz
    std::vector<int16_t> Process(const int16_t* in, int frames) {
        std::vector<int16_t> out(size_t(frames / 2) * 2);
        for (int c = 0; c < 2; ++c) {
            std::vector<double>& b = hist[c];
            for (int i = 0; i < frames; ++i) {
                b.erase(b.begin());
                b.push_back(in[i * 2 + c]);
                if (i % 2 == 1) {
                    double acc = 0;
                    for (size_t j = 0; j < h.size(); ++j) acc += h[j] * b[b.size() - 1 - j];
                    out[size_t(i / 2) * 2 + c] = int16_t(std::max(-32768.0, std::min(32767.0, std::nearbyint(acc))));
                }
            }
        }
        return out;
    }
};

// IMA ADPCM 编码器（tools/audio_codec_eval.py adpcm_channel，shaping = 0），同时给出编码器侧的重建值
struct AdpcmEncoder {
    AdpcmChannelState st[2];
    // 编码 frames 帧，返回数据体（2 × 4 字节状态 + frames 字节），recon 追加编码器重建值
    std::vector<uint8_t> Encode(const int16_t* in, int frames, std::vector<int16_t>* recon) {
        static const int kSteps[89] = {
            7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
            97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
            724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
            4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
            18500, 20350, 22385, 24623, 27086, 29794, 32767};
        std::vector<uint8_t> body;
        for (int c = 0; c < 2; ++c) {
            PutU16(&body, uint16_t(int16_t(st[c].predictor)));
            body.push_back(uint8_t(st[c].stepIndex));
            body.push_back(0);
        }
        for (int i = 0; i < frames; ++i) {
            uint8_t byte = 0;
            for (int c = 0; c < 2; ++c) {
                const int s = in[i * 2 + c];
                const int step = kSteps[st[c].stepIndex];
                int diff = s - st[c].predictor;
                uint8_t code = 0;
                if (diff < 0) {
                    code = 8;
                    diff = -diff;
                }
                if (diff >= step) {
                    code |= 4;
                    diff -= step;
                }
                if (diff >= (step >> 1)) {
                    code |= 2;
                    diff -= step >> 1;
                }
                if (diff >= (step >> 2)) code |= 1;
                // 编码器重建与解码器同一套公式
                const int pred = AdpcmDecodeNibble(&st[c], code);
                recon->push_back(int16_t(pred));
                byte |= uint8_t(code << (c * 4));
            }
            body.push_back(byte);
        }
        return body;
    }
};

std::vector<uint8_t> ExtPayload(AudioCodec codec, int complexity, int frameCode, int frameCount, int kbps, int spc,
                                uint32_t encodeUs, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> p;
    p.push_back(1);
    p.push_back(uint8_t(codec));
    p.push_back(uint8_t(complexity | (frameCode << 4)));
    p.push_back(uint8_t(frameCount));
    PutU16(&p, unsigned(kbps));
    PutU16(&p, unsigned(spc));
    PutU32(&p, encodeUs);
    p.insert(p.end(), body.begin(), body.end());
    return p;
}

std::vector<uint8_t> PcmBytes(const int16_t* x, size_t frames) {
    std::vector<uint8_t> b;
    for (size_t i = 0; i < frames * 2; ++i) PutU16(&b, uint16_t(x[i]));
    return b;
}

// ---------------------------------------------------------------- Homebrew libopus（dlopen）

struct LibOpus {
    void* handle = nullptr;
    void* (*encoderCreate)(int, int, int, int*) = nullptr;
    int (*encode)(void*, const int16_t*, int, unsigned char*, int) = nullptr;
    int (*encoderCtl)(void*, int, ...) = nullptr;
    void (*encoderDestroy)(void*) = nullptr;
    void* (*decoderCreate)(int, int, int*) = nullptr;
    int (*decode)(void*, const unsigned char*, int, int16_t*, int, int) = nullptr;
    void (*decoderDestroy)(void*) = nullptr;
    const char* (*version)() = nullptr;

    bool Load() {
        const char* paths[] = {"/opt/homebrew/opt/opus/lib/libopus.0.dylib", "/usr/local/opt/opus/lib/libopus.0.dylib",
                               "libopus.so.0"};
        for (const char* p : paths) {
            handle = dlopen(p, RTLD_NOW | RTLD_LOCAL);
            if (handle) break;
        }
        if (!handle) return false;
        encoderCreate = reinterpret_cast<decltype(encoderCreate)>(dlsym(handle, "opus_encoder_create"));
        encode = reinterpret_cast<decltype(encode)>(dlsym(handle, "opus_encode"));
        encoderCtl = reinterpret_cast<decltype(encoderCtl)>(dlsym(handle, "opus_encoder_ctl"));
        encoderDestroy = reinterpret_cast<decltype(encoderDestroy)>(dlsym(handle, "opus_encoder_destroy"));
        decoderCreate = reinterpret_cast<decltype(decoderCreate)>(dlsym(handle, "opus_decoder_create"));
        decode = reinterpret_cast<decltype(decode)>(dlsym(handle, "opus_decode"));
        decoderDestroy = reinterpret_cast<decltype(decoderDestroy)>(dlsym(handle, "opus_decoder_destroy"));
        version = reinterpret_cast<decltype(version)>(dlsym(handle, "opus_get_version_string"));
        return encoderCreate && encode && encoderCtl && encoderDestroy && decoderCreate && decode && decoderDestroy;
    }
};

LibOpus g_opus;
bool g_haveOpus = false;

// 按服务端的做法把 48 kHz PCM 切成 Opus 帧、每 framesPerPacket 帧打成一个音频包负载
std::vector<std::vector<uint8_t>> EncodeOpusPackets(const std::vector<int16_t>& pcm, int kbps, int complexity,
                                                    int frameMs, int framesPerPacket) {
    int err = 0;
    void* enc = g_opus.encoderCreate(48000, 2, 2049 /* OPUS_APPLICATION_AUDIO */, &err);
    g_opus.encoderCtl(enc, 4002 /* OPUS_SET_BITRATE */, kbps * 1000);
    g_opus.encoderCtl(enc, 4010 /* OPUS_SET_COMPLEXITY */, complexity);
    g_opus.encoderCtl(enc, 4006 /* OPUS_SET_VBR */, 0);  // 恒定码率
    const int frameSamples = frameMs * 48;
    const int frames = int(pcm.size() / 2) / frameSamples;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> body;
    int inPacket = 0;
    unsigned char buf[1500];
    for (int f = 0; f < frames; ++f) {
        const int n = g_opus.encode(enc, pcm.data() + size_t(f) * frameSamples * 2, frameSamples, buf, sizeof(buf));
        PutU16(&body, unsigned(n));
        body.insert(body.end(), buf, buf + n);
        if (++inPacket == framesPerPacket || f == frames - 1) {
            packets.push_back(ExtPayload(AudioCodec::Opus, complexity, frameMs == 10 ? 1 : 0, inPacket, kbps,
                                         inPacket * frameSamples, 1234, body));
            body.clear();
            inPacket = 0;
        }
    }
    g_opus.encoderDestroy(enc);
    return packets;
}

// 用 Homebrew（浮点版）libopus 解码同一批包，作参考
std::vector<int16_t> ReferenceOpusDecode(const std::vector<std::vector<uint8_t>>& packets) {
    int err = 0;
    void* dec = g_opus.decoderCreate(48000, 2, &err);
    std::vector<int16_t> out;
    std::vector<int16_t> frame(5760 * 2);
    for (const auto& p : packets) {
        ExtAudioHeader h;
        ParseExtAudioHeader(p.data(), p.size(), &h);
        size_t pos = kExtAudioHeaderSize;
        for (int f = 0; f < h.frameCount; ++f) {
            const int len = p[pos] | (p[pos + 1] << 8);
            pos += 2;
            const int n = g_opus.decode(dec, p.data() + pos, len, frame.data(), 5760, 0);
            pos += size_t(len);
            out.insert(out.end(), frame.begin(), frame.begin() + n * 2);
        }
    }
    g_opus.decoderDestroy(dec);
    return out;
}

// 把一串负载按给定标记依次送进解码器，拼出输出
std::vector<int16_t> DecodeAll(ExtAudioDecoder* d, uint8_t slot, const std::vector<std::vector<uint8_t>>& packets,
                               int* failures = nullptr) {
    std::vector<int16_t> out;
    for (const auto& p : packets) {
        AudioPacketInfo info;
        const uint8_t* pcm = nullptr;
        size_t n = 0;
        if (!d->Decode(slot, p.data(), p.size(), &info, &pcm, &n)) {
            if (failures) ++*failures;
            continue;
        }
        const size_t base = out.size();
        out.resize(base + n / 2);
        std::memcpy(out.data() + base, pcm, n);
    }
    return out;
}

// ---------------------------------------------------------------- 用例

void TestProtocol() {
    std::printf("协议字节\n");
    StreamOptions opt;
    opt.audioBatching = 1;
    // 不请求扩展：与之前的实现逐字节相同
    auto v = BuildHandshake("03", StreamKind::Video, opt);
    const uint8_t legacyVideo[16] = {0xAA, 0xAA, 0xAA, 0xAA, '0', '3', 0x01, 0x07, 0x00, 0x02, 0, 0, 0, 0, 0, 0};
    CHECK(std::memcmp(v.data(), legacyVideo, 16) == 0, "未请求扩展的视频握手变了");
    auto a = BuildHandshake("03", StreamKind::Audio, opt);
    const uint8_t legacyAudio[16] = {0xAA, 0xAA, 0xAA, 0xAA, '0', '3', 0x02, 0x00, 0x01, 0x00, 0, 0, 0, 0, 0, 0};
    CHECK(std::memcmp(a.data(), legacyAudio, 16) == 0, "未请求扩展的音频握手变了");

    opt.nextExt = true;
    opt.extAudio = {AudioCodec::Opus, 96, 5, 10};
    a = BuildHandshake("03", StreamKind::Audio, opt);
    CHECK(a[9] == 0x04, "音频路 FeatureFlags=%02x", a[9]);
    CHECK(a[10] == 3 && a[11] == 48 && a[12] == 0x15, "音频 Reserved[0..2]=%02x %02x %02x", a[10], a[11], a[12]);
    CHECK(a[13] == 0x02, "音频路只请求 TOS，不请求诊断：%02x", a[13]);
    CHECK(a[14] == 0 && a[15] == 0, "Reserved[4..5] 必须为 0");
    v = BuildHandshake("03", StreamKind::Video, opt);
    CHECK(v[9] == 0x06, "视频路 FeatureFlags=%02x（内存报告 + 扩展）", v[9]);
    CHECK(v[10] == 0 && v[11] == 0 && v[12] == 0, "视频路不带音频参数");
    CHECK(v[13] == 0x03, "视频路请求诊断 + TOS：%02x", v[13]);
    opt.extDiag = false;
    opt.extTos = false;
    v = BuildHandshake("03", StreamKind::Video, opt);
    CHECK(v[9] == 0x06 && v[13] == 0, "关掉诊断和 TOS 后 Reserved[3]=%02x", v[13]);
    // 协议 02 没有扩展
    v = BuildHandshake("02", StreamKind::Video, opt);
    CHECK((v[9] & 0x04) == 0 && v[13] == 0, "协议 02 不应带扩展");
    opt.extDiag = opt.extTos = true;
    opt.extAudio = {AudioCodec::Pcm48, 0, 0, 20};
    a = BuildHandshake("03", StreamKind::Audio, opt);
    CHECK(a[10] == 0 && a[11] == 48 && a[12] == 0, "PCM48 + 默认码率：%02x %02x %02x", a[10], a[11], a[12]);

    // 参数夹紧
    ExtAudioConfig c = ClampExtAudioConfig({AudioCodec(7), 300, 12, 15});
    CHECK(c.codec == AudioCodec::Pcm48 && c.opusKbps == 256 && c.opusComplexity == 10 && c.opusFrameMs == 20,
          "夹紧：codec=%d kbps=%d cx=%d frame=%d", int(c.codec), c.opusKbps, c.opusComplexity, c.opusFrameMs);
    c = ClampExtAudioConfig({AudioCodec::Opus, 97, -1, 10});
    CHECK(c.opusKbps == 96 && c.opusComplexity == 0 && c.opusFrameMs == 10, "夹紧 2");
    CHECK(ClampExtAudioConfig({AudioCodec::Opus, 3, 5, 20}).opusKbps == 16, "码率下限 16");

    // 控制消息
    auto m = BuildExtControl({AudioCodec::Opus, 128, 10, 20});
    const uint8_t expect[8] = {'S', 'D', 'V', 'X', 3, 64, 10, 0};
    CHECK(std::memcmp(m.data(), expect, 8) == 0, "控制消息 %02x %02x %02x %02x %02x %02x %02x %02x", m[0], m[1], m[2],
          m[3], m[4], m[5], m[6], m[7]);
    m = BuildExtControl({AudioCodec::Opus, 64, 3, 10});
    CHECK(m[5] == 32 && m[6] == 0x13, "10 ms 帧长代码");

    // 标记
    CHECK(ExtAudioCodecFromSlot(0xFF) == -1 && ExtAudioCodecFromSlot(0x05) == -1, "官方标记");
    CHECK(ExtAudioCodecFromSlot(0xE0) == 0 && ExtAudioCodecFromSlot(0xE3) == 3, "扩展标记");
    CHECK(ExtAudioCodecFromSlot(0xE4) == -2 && ExtAudioCodecFromSlot(0xEF) == -2, "未知扩展编码");

    // ExtAudioHeader
    auto p = ExtPayload(AudioCodec::Opus, 7, 1, 3, 160, 1440, 0xFFFFFFFF, {});
    ExtAudioHeader h;
    CHECK(ParseExtAudioHeader(p.data(), p.size(), &h), "解析 ExtAudioHeader");
    CHECK(h.codec == 3 && h.complexity == 7 && h.FrameMs() == 10 && h.frameCount == 3 && h.bitrateKbps == 160 &&
              h.samplesPerChannel == 1440 && h.encodeUs == 0xFFFFFFFF,
          "ExtAudioHeader 字段");
    p[0] = 2;
    CHECK(!ParseExtAudioHeader(p.data(), p.size(), &h), "未知版本应拒绝");
    CHECK(!ParseExtAudioHeader(p.data(), 11, &h), "长度不足应拒绝");

    // ExtDiag
    std::vector<uint8_t> d = {1, 0, 0, 0};
    for (uint32_t i = 1; i <= 13; ++i) PutU32(&d, i * 1000 + i);
    ExtDiag diag;
    CHECK(ParseExtDiag(d.data(), d.size(), &diag), "解析 ExtDiag");
    CHECK(diag.intervalMs == 1001 && diag.videoFramesSent == 2002 && diag.videoGrcGaps == 3003 &&
              diag.videoSendBlockTotalUs == 4004 && diag.videoSendBlockMaxUs == 5005 && diag.videoSendsOver20ms == 6006 &&
              diag.gapsAfterSlowSend == 7007 && diag.audioPackets == 8008 && diag.audioEncodeTotalUs == 9009 &&
              diag.audioSendBlockTotalUs == 10010 && diag.core3IdlePermille == 11011 && diag.sysdvrCpuPermille == 12012 &&
              diag.tosFlags == 13013,
          "ExtDiag 字段偏移");
    CHECK(!ParseExtDiag(d.data(), 55, &diag), "ExtDiag 长度不足应拒绝");

    // 诊断包包头：只有 allowDiag 时放行
    PacketHeader ph;
    ph.magic = kPacketMagic;
    ph.dataSize = 56;
    ph.meta = kMetaVideo | kMetaAudio;
    CHECK(ph.IsDiag() && !ValidateHeader(ph), "默认不接受诊断包（USB、未请求扩展时与官方一致）");
    CHECK(ValidateHeader(ph, true), "请求扩展的视频路接受诊断包");
    ph.dataSize = 40;
    CHECK(!ValidateHeader(ph, true), "诊断包负载太短");
    ph.dataSize = 56;
    ph.meta |= kMetaReplay;
    CHECK(!ValidateHeader(ph, true), "诊断包不能带重放位");
    ph.meta = kMetaVideo;
    CHECK(ValidateHeader(ph, true) && !ph.IsDiag(), "普通视频包不受影响");
}

void TestUpsampler() {
    std::printf("PCM24 升采样\n");
    // 系数：对称、和为 1
    double sum = 0;
    bool symmetric = true;
    for (int j = 0; j < Upsampler2x::kTaps; ++j) {
        sum += Upsampler2x::Coefficient(j);
        symmetric = symmetric && Upsampler2x::Coefficient(j) == Upsampler2x::Coefficient(126 - j);
    }
    CHECK(symmetric && std::fabs(sum - 1) < 1e-12, "系数对称且和为 1（sum=%.17g）", sum);

    // 流式（随机包长）与整段直接卷积逐采样一致
    const auto x48 = TestSignal(48000);
    Downsampler2x down;
    const auto x24 = down.Process(x48.data(), 48000);
    const size_t n24 = x24.size() / 2;
    std::vector<int16_t> ref(n24 * 4);
    for (size_t m = 0; m < n24 * 2; ++m) {
        for (int c = 0; c < 2; ++c) {
            // y[m] = Σ_j h[j] z[m-j]，z 是补零并乘 2 的输入（与 numpy 的 z[::2] = y * 2 相同），按 j 升序累加
            double acc = 0;
            for (int j = 0; j < Upsampler2x::kTaps; ++j) {
                const long k = long(m) - j;
                const double z = (k >= 0 && k % 2 == 0) ? 2.0 * x24[size_t(k / 2) * 2 + c] : 0.0;
                acc += Upsampler2x::Coefficient(j) * z;
            }
            const double r = std::nearbyint(acc);
            ref[m * 2 + c] = int16_t(r < -32768 ? -32768 : (r > 32767 ? 32767 : r));
        }
    }
    Upsampler2x up;
    std::vector<int16_t> out;
    std::mt19937 rng(7);
    size_t pos = 0;
    while (pos < n24) {
        const size_t len = std::min(n24 - pos, size_t(1 + rng() % 700));
        up.Process(x24.data() + pos * 2, len, &out);
        pos += len;
    }
    size_t mismatch = 0;
    for (size_t i = 0; i < ref.size(); ++i) mismatch += ref[i] != out[i];
    CHECK(out.size() == ref.size() && mismatch == 0, "流式输出与整段卷积不一致：%zu / %zu", mismatch, ref.size());

    // 频响：24 kHz 采样的 1 kHz 正弦升采样后，与理想 48 kHz 正弦（延迟 63 个采样）相比
    std::vector<int16_t> s24(12000 * 2), ideal(24000 * 2);
    for (int i = 0; i < 12000; ++i) s24[size_t(i) * 2] = s24[size_t(i) * 2 + 1] = int16_t(std::lrint(10000 * std::sin(2 * M_PI * 1000 * i / 24000.0)));
    for (int i = 0; i < 24000; ++i) ideal[size_t(i) * 2] = ideal[size_t(i) * 2 + 1] = int16_t(std::lrint(10000 * std::sin(2 * M_PI * 1000 * i / 48000.0)));
    Upsampler2x up2;
    std::vector<int16_t> o2;
    up2.Process(s24.data(), 12000, &o2);
    const double snr = SnrDb(ideal, o2, 1000 * 2, (1000 + Upsampler2x::kDelay) * 2, 20000 * 2);
    CHECK(snr > 60, "1 kHz 正弦升采样 SNR %.1f dB", snr);
    std::printf("  流式 = 整段卷积（%zu 个采样逐一相同）；1 kHz 正弦 SNR %.1f dB\n", ref.size(), snr);
}

void TestAdpcm() {
    std::printf("IMA ADPCM\n");
    const auto x = TestSignal(48000, 3);
    AdpcmEncoder enc;
    std::vector<int16_t> recon;
    std::vector<std::vector<uint8_t>> packets;
    const int spc = 2048;  // batching 1：每包 2 × 1024 个采样
    for (int pos = 0; pos + spc <= 48000; pos += spc) {
        const auto body = enc.Encode(x.data() + size_t(pos) * 2, spc, &recon);
        packets.push_back(ExtPayload(AudioCodec::Adpcm, 0, 0, 1, 384, spc, 55, body));
    }
    ExtAudioDecoder dec;
    int failures = 0;
    const auto out = DecodeAll(&dec, 0xE2, packets, &failures);
    size_t mismatch = 0;
    for (size_t i = 0; i < std::min(out.size(), recon.size()); ++i) mismatch += out[i] != recon[i];
    CHECK(failures == 0 && out.size() == recon.size() && mismatch == 0, "ADPCM 与编码器重建不一致：%zu（失败 %d）",
          mismatch, failures);
    const double snr = SnrDb(x, out, 0, 0, out.size());
    std::printf("  %zu 包逐采样一致，对原始信号 SNR %.1f dB\n", packets.size(), snr);

    // 丢掉一个包：下一个包按包头状态重新同步，照样逐采样一致
    ExtAudioDecoder dec2;
    std::vector<std::vector<uint8_t>> lossy = packets;
    lossy.erase(lossy.begin() + 5);
    const auto out2 = DecodeAll(&dec2, 0xE2, lossy);
    const size_t after = size_t(5) * spc * 2;  // 丢包位置之后的第一个包，在 out2 里的起点
    mismatch = 0;
    for (size_t i = 0; i < size_t(spc) * 2; ++i) mismatch += out2[after + i] != recon[after + size_t(spc) * 2 + i];
    CHECK(mismatch == 0, "丢包后没有重新同步：%zu", mismatch);

    // 损坏的状态（步长下标 > 88）按坏包处理
    auto bad = packets[0];
    bad[kExtAudioHeaderSize + 2] = 200;
    AudioPacketInfo info;
    const uint8_t* pcm = nullptr;
    size_t n = 0;
    CHECK(!dec.Decode(0xE2, bad.data(), bad.size(), &info, &pcm, &n) && n == 0, "坏状态应拒绝");
    // 数据体比 samplesPerChannel 短
    auto shortPkt = packets[0];
    shortPkt.resize(shortPkt.size() - 10);
    CHECK(!dec.Decode(0xE2, shortPkt.data(), shortPkt.size(), &info, &pcm, &n), "数据体过短应拒绝");
}

void TestPcm24Packets() {
    std::printf("PCM24 音频包\n");
    const auto x = TestSignal(48000, 5);
    Downsampler2x down;
    const auto x24 = down.Process(x.data(), 48000);
    std::vector<std::vector<uint8_t>> packets;
    for (size_t pos = 0; pos < x24.size() / 2; pos += 1024) {
        const size_t len = std::min<size_t>(1024, x24.size() / 2 - pos);
        packets.push_back(ExtPayload(AudioCodec::Pcm24, 0, 0, 1, 768, int(len), 99, PcmBytes(x24.data() + pos * 2, len)));
    }
    ExtAudioDecoder dec;
    int failures = 0;
    const auto out = DecodeAll(&dec, 0xE1, packets, &failures);
    CHECK(failures == 0 && out.size() == x.size(), "PCM24 输出长度 %zu（期望 %zu）", out.size(), x.size());
    // 与原始信号比较：测试用降采样器的延迟（约 30 个采样）加上升采样的 63 个，搜索最佳对齐
    double snr = -100;
    size_t delay = 0;
    for (size_t dly = 0; dly < 200; ++dly) {
        const double v = SnrDb(x, out, 2000 * 2, (2000 + dly) * 2, 40000 * 2);
        if (v > snr) {
            snr = v;
            delay = dly;
        }
    }
    CHECK(snr > 30, "PCM24 往返 SNR %.1f dB", snr);
    std::printf("  往返（服务端半带降采样 + 客户端升采样）延迟 %zu 个采样，SNR %.1f dB\n", delay, snr);
}

void TestOpus() {
    std::printf("Opus\n");
    if (!ExtAudioDecoder::OpusAvailable()) {
        CHECK(false, "编译时没有带 Opus 解码（SYSDVR_WITH_OPUS）");
        return;
    }
    if (!g_haveOpus) {
        std::printf("  跳过：没找到 Homebrew libopus（brew install opus），无法生成 Opus 包\n");
        return;
    }
    const auto x = TestSignal(48000 * 2, 9);
    struct Case {
        int kbps, complexity, frameMs, framesPerPacket;
    } cases[] = {{96, 5, 20, 2}, {64, 0, 10, 4}, {192, 10, 20, 3}, {32, 3, 10, 5}};
    for (const Case& c : cases) {
        const auto packets = EncodeOpusPackets(x, c.kbps, c.complexity, c.frameMs, c.framesPerPacket);
        ExtAudioDecoder dec;
        int failures = 0;
        const auto out = DecodeAll(&dec, 0xE3, packets, &failures);
        const auto ref = ReferenceOpusDecode(packets);
        const size_t n = std::min(out.size(), ref.size());
        // 定点解码器与浮点参考解码器不逐位相同，但差别应远小于编码失真
        const double vsRef = SnrDb(ref, out, 0, 0, n);
        // 对原始信号：Opus 解码输出比输入晚 312 个采样（6.5 ms 前瞻）
        const double vsInput = SnrDb(x, out, 0, 312 * 2, n - 312 * 2);
        CHECK(failures == 0 && out.size() == ref.size(), "Opus 输出长度 %zu（参考 %zu，失败 %d）", out.size(), ref.size(),
              failures);
        CHECK(vsRef > 40, "Opus %d kbps：与参考解码的差别太大（%.1f dB）", c.kbps, vsRef);
        // 感知编码对波形 SNR 不友好，只对中高码率设个下限，确认解码输出和输入对得上
        CHECK(c.kbps < 64 || vsInput > 10, "Opus %d kbps：对原始信号 SNR %.1f dB", c.kbps, vsInput);
        std::printf("  %3d kbps · 复杂度 %2d · %2d ms · 每包 %d 帧：%zu 包，与参考解码 %.1f dB，对原始信号 %.1f dB\n", c.kbps,
                    c.complexity, c.frameMs, c.framesPerPacket, packets.size(), vsRef, vsInput);
    }
    // 坏帧：用丢包补偿填上同样长度，后续帧照常解
    auto packets = EncodeOpusPackets(x, 96, 5, 20, 2);
    auto bad = packets[3];
    const int len = bad[kExtAudioHeaderSize] | (bad[kExtAudioHeaderSize + 1] << 8);
    // 把第一帧的 TOC 改成非法的 code 3 帧数 0
    bad[kExtAudioHeaderSize + 2] = 0x03;
    if (len > 1) bad[kExtAudioHeaderSize + 3] = 0x00;
    ExtAudioDecoder dec;
    AudioPacketInfo info;
    const uint8_t* pcm = nullptr;
    size_t n = 0;
    const uint64_t before = dec.DecodeErrors();
    const bool ok = dec.Decode(0xE3, bad.data(), bad.size(), &info, &pcm, &n);
    CHECK(ok && n == 2 * 960 * 4 && dec.DecodeErrors() == before + 1, "坏帧应计错并补齐长度（ok=%d n=%zu）", ok, n);
}

void TestSwitching() {
    std::printf("运行中切换编码\n");
    const auto x = TestSignal(48000, 11);
    ExtAudioDecoder dec;
    size_t expected = 0, got = 0;
    auto feed = [&](uint8_t slot, const std::vector<uint8_t>& p, size_t expectFrames) {
        AudioPacketInfo info;
        const uint8_t* pcm = nullptr;
        size_t n = 0;
        const bool ok = dec.Decode(slot, p.data(), p.size(), &info, &pcm, &n);
        CHECK(ok, "切换序列中解码失败（标记 %02x）", slot);
        got += n / 4;
        expected += expectFrames;
    };
    // 官方 PCM → 扩展 PCM48 → PCM24 → ADPCM → Opus（20 ms、10 ms）→ PCM48
    feed(0xFF, PcmBytes(x.data(), 2048), 2048);
    feed(0xE0, PcmBytes(x.data(), 2048), 2048);
    Downsampler2x down;
    const auto x24 = down.Process(x.data(), 4096);
    feed(0xE1, ExtPayload(AudioCodec::Pcm24, 0, 0, 1, 768, 2048, 1, PcmBytes(x24.data(), 2048)), 4096);
    AdpcmEncoder enc;
    std::vector<int16_t> recon;
    feed(0xE2, ExtPayload(AudioCodec::Adpcm, 0, 0, 1, 384, 2048, 1, enc.Encode(x.data(), 2048, &recon)), 2048);
    if (g_haveOpus) {
        for (const auto& p : EncodeOpusPackets(std::vector<int16_t>(x.begin(), x.begin() + 9600 * 2), 96, 5, 20, 2))
            feed(0xE3, p, 1920);
        for (const auto& p : EncodeOpusPackets(std::vector<int16_t>(x.begin(), x.begin() + 9600 * 2), 64, 0, 10, 4))
            feed(0xE3, p, 1920);
    }
    feed(0xE0, PcmBytes(x.data(), 2048), 2048);
    feed(0xE1, ExtPayload(AudioCodec::Pcm24, 0, 0, 1, 768, 2048, 1, PcmBytes(x24.data(), 2048)), 4096);
    CHECK(got == expected, "切换后输出帧数 %zu（期望 %zu）", got, expected);
    std::printf("  官方 PCM → PCM48 → 24 kHz → ADPCM → Opus 20/10 ms → PCM48 → 24 kHz：输出 %zu 帧，无丢失\n", got);
}

void TestFuzz() {
    std::printf("异常输入\n");
    std::mt19937 rng(12345);
    ExtAudioDecoder dec;
    size_t accepted = 0;
    const uint8_t slots[] = {0xE1, 0xE2, 0xE3, 0xE4, 0xE0, 0xFF};
    for (int iter = 0; iter < 30000; ++iter) {
        std::vector<uint8_t> p(rng() % 2000);
        for (auto& b : p) b = uint8_t(rng());
        const uint8_t slot = slots[rng() % 6];
        if (p.size() >= 12 && (rng() % 4) != 0) {
            // 大多数时候给一个像样的头，让数据体解析路径被充分走到
            p[0] = 1;
            p[1] = slot & 0x0F;
            p[3] = uint8_t(rng() % 70);
            p[6] = uint8_t(rng());
            p[7] = uint8_t(rng() % 80);
        }
        AudioPacketInfo info;
        const uint8_t* pcm = nullptr;
        size_t n = 0;
        if (dec.Decode(slot, p.data(), p.size(), &info, &pcm, &n)) ++accepted;
        if (n > 0 && pcm != nullptr) {
            volatile uint8_t sink = pcm[n - 1];  // 输出缓冲必须可读
            (void)sink;
        }
    }
    std::printf("  30000 个随机包：%zu 个被接受、%llu 次解码错误，没有崩溃\n", accepted,
                (unsigned long long)dec.DecodeErrors());
    CHECK(true, "");
}

void TestDispatcher() {
    std::printf("包分发与统计\n");
    StreamOptions opt;
    opt.nextExt = true;
    StreamCallbacks cb;
    std::vector<uint8_t> audio;
    int videoCalls = 0;
    cb.onAudio = [&](const uint8_t* p, size_t n, uint64_t) { audio.insert(audio.end(), p, p + n); };
    cb.onVideo = [&](const uint8_t*, size_t, uint64_t) { ++videoCalls; };
    PacketDispatcher d(opt, cb);
    const auto x = TestSignal(4096, 13);

    auto header = [](size_t size, uint8_t meta, uint8_t slot) {
        PacketHeader h;
        h.magic = kPacketMagic;
        h.dataSize = uint32_t(size);
        h.meta = meta;
        h.replaySlot = slot;
        return h;
    };
    // 官方音频包：原样透传，不认为支持扩展
    const auto pcm = PcmBytes(x.data(), 1024);
    d.Dispatch(header(pcm.size(), kMetaAudio, 0xFF), pcm.data());
    BridgeStats s;
    d.FillStats(&s);
    CHECK(audio == pcm, "官方 PCM 应原样交给播放器");
    CHECK(!s.extSupported && s.audioCodec == -1, "官方包不应被认成扩展");

    // PCM24 包
    audio.clear();
    Downsampler2x down;
    const auto x24 = down.Process(x.data(), 2048);
    const auto p24 = ExtPayload(AudioCodec::Pcm24, 0, 0, 1, 768, 1024, 321, PcmBytes(x24.data(), 1024));
    d.Dispatch(header(p24.size(), kMetaAudio, 0xE1), p24.data());
    d.FillStats(&s);
    CHECK(s.extSupported && s.audioCodec == 1 && s.audioKbpsConfig == 768 && s.audioEncodeUs == 321,
          "PCM24 统计：ext=%d codec=%d kbps=%d enc=%llu", s.extSupported, s.audioCodec, s.audioKbpsConfig,
          (unsigned long long)s.audioEncodeUs);
    CHECK(audio.size() == 2048 * 4, "PCM24 应输出 2048 帧（实际 %zu 字节）", audio.size());
    CHECK(ExtAudioMatches({AudioCodec::Pcm24, 64, 0, 10}, s), "PCM24 与 Opus 参数无关");
    CHECK(!ExtAudioMatches({AudioCodec::Opus, 96, 5, 20}, s), "编码不同");

    // Opus 包的参数进统计
    if (g_haveOpus) {
        const auto packets = EncodeOpusPackets(std::vector<int16_t>(x.begin(), x.begin() + 1920 * 2), 128, 7, 10, 4);
        d.Dispatch(header(packets[0].size(), kMetaAudio, 0xE3), packets[0].data());
        d.FillStats(&s);
        CHECK(s.audioCodec == 3 && s.audioKbpsConfig == 128 && s.opusComplexity == 7 && s.opusFrameMs == 10,
              "Opus 统计 codec=%d kbps=%d cx=%d frame=%d", s.audioCodec, s.audioKbpsConfig, s.opusComplexity,
              s.opusFrameMs);
        CHECK(ExtAudioMatches({AudioCodec::Opus, 128, 7, 10}, s) && !ExtAudioMatches({AudioCodec::Opus, 128, 7, 20}, s),
              "Opus 参数比对");
    }

    // 诊断包：不进视频回调，字段进统计并累计
    std::vector<uint8_t> diag = {1, 0, 0, 0};
    const uint32_t f1[13] = {1000, 30, 2, 15000, 9000, 1, 1, 94, 2800, 300, 650, 120, 3};
    for (uint32_t v : f1) PutU32(&diag, v);
    // 服务端实际发的是 MetaData = 0x07（类型 3 + Data 位）
    d.Dispatch(header(diag.size(), kMetaVideo | kMetaAudio | kMetaData, 0xFF), diag.data());
    std::vector<uint8_t> diag2 = {1, 0, 0, 0};
    const uint32_t f2[13] = {1002, 29, 1, 25000, 21000, 1, 0, 94, 2900, 100, 700, 110, 3};
    for (uint32_t v : f2) PutU32(&diag2, v);
    d.Dispatch(header(diag2.size(), kMetaVideo | kMetaAudio, 0xFF), diag2.data());
    d.FillStats(&s);
    CHECK(videoCalls == 0, "诊断包不应当成视频帧");
    CHECK(s.diagPackets == 2 && s.diagLast.videoSendBlockMaxUs == 21000 && s.diagLast.core3IdlePermille == 700,
          "诊断最近值");
    CHECK(s.diagTotal.intervalMs == 2002 && s.diagTotal.videoGrcGaps == 3 && s.diagTotal.videoSendBlockTotalUs == 40000 &&
              s.diagTotal.videoSendBlockMaxUs == 21000 && s.diagTotal.gapsAfterSlowSend == 1 &&
              s.diagTotal.audioEncodeTotalUs == 5700,
          "诊断累计");

    // grc 采集失败的错误包保持官方格式（ReplaySlot = 0xFF、没有 ExtAudioHeader）：不能因此判定服务端不支持扩展，
    // 也不能改掉当前编码
    const int codecBefore = s.audioCodec;
    std::vector<uint8_t> err;
    PutU32(&err, 2);
    PutU32(&err, 0x1234);
    for (int i = 0; i < 24; ++i) err.push_back(0);
    audio.clear();
    d.Dispatch(header(err.size(), kMetaAudio | kMetaError, 0xFF), err.data());
    d.FillStats(&s);
    CHECK(s.extSupported && s.audioCodec == codecBefore && s.errorPackets == 1 && audio.empty(),
          "音频错误包之后：ext=%d codec=%d（之前 %d）错误包=%llu", s.extSupported, s.audioCodec, codecBefore,
          (unsigned long long)s.errorPackets);

    // 只有诊断包、没有音频（音频关着）也算支持扩展
    PacketDispatcher d2(opt, cb);
    d2.Dispatch(header(diag.size(), kMetaVideo | kMetaAudio, 0xFF), diag.data());
    BridgeStats s2;
    d2.FillStats(&s2);
    CHECK(s2.extSupported && s2.audioCodec == -1, "诊断包也表明支持扩展");
}

// ---------------------------------------------------------------- tools/ext_vectors（另一端生成）

bool ReadFile(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

int RunVectors(const std::string& dir);  // 定义在后面（按向量目录 README 的格式）

}  // namespace

int main(int argc, char** argv) {
    std::string vectors = "tools/ext_vectors";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vectors") == 0 && i + 1 < argc) vectors = argv[++i];
    }
    g_haveOpus = g_opus.Load();
    if (g_haveOpus && g_opus.version) std::printf("参考 libopus：%s（Homebrew，dlopen）\n", g_opus.version());

    TestProtocol();
    TestUpsampler();
    TestAdpcm();
    TestPcm24Packets();
    TestOpus();
    TestSwitching();
    TestFuzz();
    TestDispatcher();
    RunVectors(vectors);

    std::printf("\n%d 项检查，%d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

namespace {

struct VecPacket {
    PacketHeader h;
    std::vector<uint8_t> payload;
};

// <name>.stream.bin：服务端写进 9922 的原始字节（18 字节包头 + 负载，一个接一个）
bool LoadStream(const std::string& path, std::vector<VecPacket>* out) {
    std::vector<uint8_t> b;
    if (!ReadFile(path, &b)) return false;
    size_t pos = 0;
    while (pos + kHeaderSize <= b.size()) {
        VecPacket p;
        p.h = ParseHeader(b.data() + pos);
        if (!ValidateHeader(p.h) || pos + kHeaderSize + p.h.dataSize > b.size()) return false;
        p.payload.assign(b.begin() + long(pos + kHeaderSize), b.begin() + long(pos + kHeaderSize + p.h.dataSize));
        pos += kHeaderSize + p.h.dataSize;
        out->push_back(std::move(p));
    }
    return pos == b.size();
}

// <name>.packets.tsv：每行一个包的包头与 ExtAudioHeader 字段
std::vector<std::vector<long>> LoadTsv(const std::string& path) {
    std::vector<std::vector<long>> rows;
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);  // 表头：index offset packet_bytes data_size timestamp_us meta replay_slot codec complexity
                            //       frame_code frame_count bitrate_kbps samples_per_channel encode_us
    while (std::getline(f, line)) {
        std::vector<long> row;
        size_t start = 0;
        while (start <= line.size()) {
            const size_t tab = line.find('\t', start);
            const std::string cell = line.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
            row.push_back(std::strtol(cell.c_str(), nullptr, 0));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        if (row.size() >= 14) rows.push_back(row);
    }
    return rows;
}

std::vector<int16_t> Samples(const std::vector<uint8_t>& b) {
    std::vector<int16_t> v(b.size() / 2);
    for (size_t i = 0; i < v.size(); ++i) v[i] = int16_t(uint16_t(b[i * 2] | (b[i * 2 + 1] << 8)));
    return v;
}

// 把一个向量的全部包送进包分发器（与真实接收路径相同），收集交给播放器的 PCM，并逐包核对标记、ExtAudioHeader 与 tsv
std::vector<int16_t> RunStream(const std::vector<VecPacket>& packets, const std::vector<std::vector<long>>& tsv,
                               const char* name, BridgeStats* stats) {
    StreamOptions opt;
    opt.nextExt = true;
    StreamCallbacks cb;
    std::vector<uint8_t> audio;
    cb.onAudio = [&](const uint8_t* p, size_t n, uint64_t) { audio.insert(audio.end(), p, p + n); };
    PacketDispatcher d(opt, cb);
    size_t headerMismatch = 0;
    for (size_t i = 0; i < packets.size(); ++i) {
        const VecPacket& p = packets[i];
        d.Dispatch(p.h, p.payload.data());
        if (i >= tsv.size()) continue;
        const auto& r = tsv[i];
        BridgeStats s;
        d.FillStats(&s);
        bool ok = long(p.h.dataSize) == r[3] && long(p.h.timestampUs) == r[4] && p.h.meta == r[5] &&
                  p.h.replaySlot == r[6] && s.audioCodec == r[7] && s.audioKbpsConfig == (r[7] == 0 ? 1536 : r[11]);
        if (r[7] != 0) {
            ExtAudioHeader h;
            ok = ok && ParseExtAudioHeader(p.payload.data(), p.payload.size(), &h) && h.codec == r[7] &&
                 h.complexity == r[8] && h.frameCode == r[9] && h.frameCount == r[10] && h.bitrateKbps == r[11] &&
                 h.samplesPerChannel == r[12] && long(h.encodeUs) == r[13];
        }
        if (r[7] == 3) ok = ok && s.opusComplexity == r[8] && s.opusFrameMs == (r[9] == 1 ? 10 : 20);
        headerMismatch += !ok;
    }
    CHECK(packets.size() == tsv.size() && headerMismatch == 0, "%s：%zu 包，其中 %zu 包的标记/包头与 tsv 不符（tsv %zu 行）",
          name, packets.size(), headerMismatch, tsv.size());
    d.FillStats(stats);
    return Samples(audio);
}

int RunVectors(const std::string& dir) {
    std::vector<uint8_t> readme;
    if (!ReadFile(dir + "/README.md", &readme)) {
        std::printf("测试向量：%s 还没有（另一端生成后自动纳入）\n", dir.c_str());
        return 0;
    }
    std::printf("测试向量（%s）\n", dir.c_str());

    // README 里给的握手与控制消息字节
    StreamOptions opt;
    opt.audioBatching = 3;
    opt.memoryDiag = false;
    opt.nextExt = true;
    opt.extAudio = {AudioCodec::Opus, 96, 5, 20};
    const auto a = BuildHandshake("03", StreamKind::Audio, opt);
    const uint8_t expectA[16] = {0xAA, 0xAA, 0xAA, 0xAA, 0x30, 0x33, 0x02, 0x00, 0x03, 0x04, 0x03, 0x30, 0x05, 0x02, 0, 0};
    CHECK(std::memcmp(a.data(), expectA, 16) == 0, "音频握手字节与向量 README 不同");
    const auto v = BuildHandshake("03", StreamKind::Video, opt);
    const uint8_t expectV[16] = {0xAA, 0xAA, 0xAA, 0xAA, 0x30, 0x33, 0x01, 0x07, 0x00, 0x04, 0x00, 0x00, 0x00, 0x03, 0, 0};
    CHECK(std::memcmp(v.data(), expectV, 16) == 0, "视频握手字节与向量 README 不同");
    const auto c = BuildExtControl({AudioCodec::Opus, 64, 10, 10});
    const uint8_t expectC[8] = {0x53, 0x44, 0x56, 0x58, 0x03, 0x20, 0x1A, 0x00};
    CHECK(std::memcmp(c.data(), expectC, 8) == 0, "控制消息字节与向量 README 不同");
    const auto c2 = BuildExtControl({AudioCodec::Adpcm, 96, 5, 20});
    // 切到非 Opus 编码时我们照样带上 Opus 参数（服务端只校验编码值），只比较前 5 个字节
    CHECK(std::memcmp(c2.data(), "SDVX\x02", 5) == 0 && c2[7] == 0, "ADPCM 控制消息");

    std::vector<uint8_t> raw;
    auto load = [&](const std::string& f) {
        raw.clear();
        const bool ok = ReadFile(dir + "/" + f, &raw);
        CHECK(ok, "缺少向量文件 %s", f.c_str());
        return Samples(raw);
    };
    auto stream = [&](const std::string& name, std::vector<VecPacket>* pk) {
        const bool ok = LoadStream(dir + "/" + name + ".stream.bin", pk);
        CHECK(ok && !pk->empty(), "%s.stream.bin 解析失败", name.c_str());
        return LoadTsv(dir + "/" + name + ".packets.tsv");
    };
    auto countDiff = [](const std::vector<int16_t>& x, const std::vector<int16_t>& y, size_t xo, size_t yo, size_t n,
                        int* maxAbs) {
        size_t diff = 0;
        *maxAbs = 0;
        for (size_t i = 0; i < n; ++i) {
            const int d = std::abs(int(x[xo + i]) - int(y[yo + i]));
            diff += d != 0;
            if (d > *maxAbs) *maxAbs = d;
        }
        return diff;
    };
    BridgeStats st;
    int maxAbs = 0;

    // PCM48：与输入逐采样相同
    {
        std::vector<VecPacket> pk;
        const auto tsv = stream("pcm48", &pk);
        const auto out = RunStream(pk, tsv, "pcm48", &st);
        const auto expect = load("pcm48.expected_48k_s16le_stereo.raw");
        CHECK(out == expect, "PCM48 输出与期望不同（%zu / %zu 个采样）", out.size(), expect.size());
        std::printf("  pcm48：%zu 包，%zu 帧逐采样一致\n", pk.size(), out.size() / 2);
    }
    // PCM24：24 kHz 数据体逐采样一致；升采样结果与参考 up48()（居中卷积）对齐 63 个采样后比较
    {
        std::vector<VecPacket> pk;
        const auto tsv = stream("pcm24", &pk);
        const auto out = RunStream(pk, tsv, "pcm24", &st);
        std::vector<int16_t> bodies;
        for (const auto& p : pk) {
            const auto b = std::vector<uint8_t>(p.payload.begin() + kExtAudioHeaderSize, p.payload.end());
            const auto sm = Samples(b);
            bodies.insert(bodies.end(), sm.begin(), sm.end());
        }
        const auto expect24 = load("pcm24.expected_24k_s16le_stereo.raw");
        CHECK(bodies == expect24, "PCM24 数据体与期望不同");
        const auto ref48 = load("pcm24.ref_up48_s16le_stereo.raw");
        CHECK(out.size() == ref48.size(), "PCM24 升采样长度 %zu（参考 %zu）", out.size(), ref48.size());
        const size_t dly = size_t(Upsampler2x::kDelay) * 2;
        // 参考实现在末尾补零，因果实现拿不到“未来”的采样：只比到倒数 64 帧
        const size_t n = ref48.size() - dly - 2;
        const size_t diff = countDiff(out, ref48, dly, 0, n, &maxAbs);
        CHECK(diff == 0, "PCM24 升采样与参考 up48() 不一致：%zu 个采样不同，差值峰值 %d", diff, maxAbs);
        std::printf("  pcm24：%zu 包；24 kHz 数据逐采样一致；升采样与参考 up48() 对齐 %d 帧后 %zu 个采样逐一相同\n",
                    pk.size(), Upsampler2x::kDelay, n);
    }
    // ADPCM：逐采样一致
    {
        std::vector<VecPacket> pk;
        const auto tsv = stream("adpcm", &pk);
        const auto out = RunStream(pk, tsv, "adpcm", &st);
        const auto expect = load("adpcm.expected_48k_s16le_stereo.raw");
        CHECK(out == expect, "ADPCM 输出与期望不同（%zu / %zu 个采样）", out.size(), expect.size());
        std::printf("  adpcm：%zu 包，%zu 帧逐采样一致\n", pk.size(), out.size() / 2);
    }
    // Opus：与参考解码（服务端同版本 libopus 定点）比较
    for (const char* name : {"opus_96k_20ms_c5", "opus_64k_10ms_c0", "opus_256k_20ms_c10"}) {
        std::vector<VecPacket> pk;
        const auto tsv = stream(name, &pk);
        const auto out = RunStream(pk, tsv, name, &st);
        const auto ref = load(std::string(name) + ".ref_decoded_48k_s16le_stereo.raw");
        CHECK(out.size() == ref.size() && st.audioDecodeErrors == 0, "%s：输出 %zu（参考 %zu），解码错误 %llu", name,
              out.size(), ref.size(), (unsigned long long)st.audioDecodeErrors);
        const size_t n = std::min(out.size(), ref.size());
        const size_t diff = countDiff(out, ref, 0, 0, n, &maxAbs);
        const double snr = SnrDb(ref, out, 0, 0, n);
        CHECK(snr > 40, "%s：与参考解码 SNR %.1f dB", name, snr);
        std::printf("  %s：%zu 包，%zu 帧，与参考解码%s（SNR %.1f dB，差值峰值 %d）\n", name, pk.size(), out.size() / 2,
                    diff == 0 ? "逐采样一致" : "有差异", snr, maxAbs);
    }
    // 运行中切换：逐包核对编码标记，总帧数与 manifest 一致，没有解码错误
    {
        std::vector<VecPacket> pk;
        const auto tsv = stream("switch", &pk);
        const auto out = RunStream(pk, tsv, "switch", &st);
        std::string seq;
        std::string last;
        for (const auto& r : tsv) {
            const char* names[] = {"PCM48", "PCM24", "ADPCM", "Opus"};
            std::string item = names[r[7] & 3];
            if (r[7] == 3) item += "/" + std::to_string(r[11]) + "k/" + (r[9] == 1 ? "10" : "20") + "ms/c" + std::to_string(r[8]);
            if (item == last) continue;
            last = item;
            seq += (seq.empty() ? "" : " → ") + item;
        }
        // 按包累加期望的 48 kHz 帧数（tsv 的 samples_per_channel：PCM48 是负载帧数，PCM24 按 24 kHz 计、升采样后翻倍）
        size_t expectFrames = 0;
        for (const auto& r : tsv) expectFrames += size_t(r[12]) * (r[7] == 1 ? 2 : 1);
        CHECK(out.size() / 2 == expectFrames && st.audioDecodeErrors == 0, "switch：输出 %zu 帧（期望 %zu），解码错误 %llu",
              out.size() / 2, expectFrames, (unsigned long long)st.audioDecodeErrors);
        std::printf("  switch：%zu 包（%s），输出 %zu 帧，无解码错误\n", pk.size(), seq.c_str(), out.size() / 2);
    }
    return 0;
}

}  // namespace
