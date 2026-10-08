#include "ext_audio.h"

#include <cmath>
#include <cstring>

#include "log.h"

#ifdef SYSDVR_WITH_OPUS
#include <opus.h>
#endif

namespace sysdvr {

namespace {

// tools/audio_codec_eval.py：UPSAMPLE_127 = lowpass_fir(11500, 127)（sinc × Kaiser β=8，归一化到和为 1）。
// 这里是前 64 个系数的 repr()，后 63 个与之对称（h[126 - j] == h[j]）。重新生成：
//   uv run --no-project --with numpy python3 -c "import audio_codec_eval as e; print([repr(float(v)) for v in e.UPSAMPLE_127[:64]])"
constexpr double kUp127Half[64] = {
    6.565234491337441e-06, -1.4966417524856322e-05, -1.834065055758461e-05, 2.7559879497933156e-05,
    3.9602260109318414e-05, -4.217023571278741e-05, -7.413554737885842e-05, 5.638303667351242e-05,
    0.00012604565219521776, -6.619235518125318e-05, -0.0001994023215291456, 6.571979767635838e-05,
    0.00029776024987873527, -4.701843749272241e-05, -0.0004235648386666693, 3.165928757628889e-18,
    0.0005774650402807636, 8.748035140207744e-05, -0.0007575660014045815, -0.00022935519116887548,
    0.0009586633143636154, 0.00044087715267823995, -0.001171506383082263, -0.0007379522895388475,
    0.001382139277956417, 0.0011362911829883844, -0.0015713621113991618, -0.0016504121065696066,
    0.001714343060832536, 0.002292547441865558, -0.001780389214216682, -0.003071519098271084,
    0.0017328513975198147, 0.003991659389701134, -0.0015290906682790832, -0.005051859343647584,
    0.0011203657446097548, 0.00624482580519178, -0.0004513982448088896, -0.007556621469763867,
    -0.0005407906963214954, 0.008966548208303378, 0.0019294602015598786, -0.010447414396471259,
    -0.0038033585678347013, 0.011966202644985017, 0.006276833573039045, -0.013485127050641732,
    -0.00950943054590691, 0.014963040898362073, 0.013745074444998513, -0.01635712887868871,
    -0.019395462734018837, 0.01762479456960951, 0.02723591085282358, -0.018725636186794438,
    -0.038937710970216324, 0.01962339305993449, 0.05887792632229642, -0.020287743023727864,
    -0.10318575755504228, 0.020695837340388698, 0.3173312581818017, 0.479169915927688,
};

constexpr double Tap(int j) { return kUp127Half[j <= 63 ? j : 126 - j]; }

// 多相分解：补零后偶数输出只碰到偶数下标的抽头（64 个），奇数输出只碰到奇数下标的（63 个）
struct Phases {
    double even[64];
    double odd[63];
    Phases() {
        for (int i = 0; i < 64; ++i) even[i] = Tap(2 * i);
        for (int i = 0; i < 63; ++i) odd[i] = Tap(2 * i + 1);
    }
};
const Phases kPhases;

int16_t RoundClip(double v) {
    // lrint 用当前舍入模式（默认就近、五成双），与 numpy 的 np.round 一致；输入有界（|v| < 2^21），不会溢出 long
    const long r = std::lrint(v);
    return int16_t(r < -32768 ? -32768 : (r > 32767 ? 32767 : r));
}

constexpr int kAdpcmSteps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kAdpcmIndexAdj[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

// 单包每声道采样数上限：官方一包最多 6 × 1024 个 48 kHz 采样（batching 5），留足余量
constexpr int kMaxSamplesPerPacket = 16384;
constexpr int kMaxOpusFrames = 64;
constexpr int kOpusMaxFrameSamples = 5760;  // 120 ms @ 48 kHz，libopus 单帧上限

}  // namespace

// ---------------------------------------------------------------------------
// Upsampler2x

double Upsampler2x::Coefficient(int j) { return j < 0 || j >= kTaps ? 0.0 : Tap(j); }

void Upsampler2x::Reset() {
    for (auto& b : buf_) b.assign(kHistory, 0.0);
}

void Upsampler2x::Process(const int16_t* in, size_t frames, std::vector<int16_t>* out) {
    const size_t base = out->size();
    out->resize(base + frames * 4);
    int16_t* o = out->data() + base;
    for (int c = 0; c < 2; ++c) {
        std::vector<double>& b = buf_[c];
        b.resize(kHistory + frames);
        for (size_t i = 0; i < frames; ++i) b[kHistory + i] = in[i * 2 + c];
        for (size_t n = 0; n < frames; ++n) {
            const double* x = b.data() + kHistory + n;  // x[0] 是当前输入采样，x[-i] 是前 i 个
            // 补零时输入乘 2（保持增益），与参考实现 z[::2] = y * 2 相同；乘 2 在浮点里是精确的
            double even = 0.0, odd = 0.0;
            for (int i = 0; i < 64; ++i) even += kPhases.even[i] * (2.0 * x[-i]);
            for (int i = 0; i < 63; ++i) odd += kPhases.odd[i] * (2.0 * x[-i]);
            o[(2 * n) * 2 + c] = RoundClip(even);
            o[(2 * n + 1) * 2 + c] = RoundClip(odd);
        }
        b.erase(b.begin(), b.begin() + std::ptrdiff_t(frames));  // 留下最后 kHistory 个做下一包的历史
    }
}

// ---------------------------------------------------------------------------
// IMA ADPCM

int AdpcmDecodeNibble(AdpcmChannelState* st, uint8_t code) {
    const int step = kAdpcmSteps[st->stepIndex];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    int pred = (code & 8) ? st->predictor - delta : st->predictor + delta;
    pred = pred < -32768 ? -32768 : (pred > 32767 ? 32767 : pred);
    st->predictor = pred;
    int idx = st->stepIndex + kAdpcmIndexAdj[code & 7];
    st->stepIndex = idx < 0 ? 0 : (idx > 88 ? 88 : idx);
    return pred;
}

bool DecodeAdpcmBody(const uint8_t* body, size_t n, int samplesPerChannel, std::vector<int16_t>* out) {
    if (samplesPerChannel <= 0 || samplesPerChannel > kMaxSamplesPerPacket) return false;
    if (n < 8 + size_t(samplesPerChannel)) return false;
    AdpcmChannelState st[2];
    for (int c = 0; c < 2; ++c) {
        const uint8_t* s = body + c * 4;
        st[c].predictor = int16_t(uint16_t(s[0] | (s[1] << 8)));
        st[c].stepIndex = s[2];
        if (st[c].stepIndex > 88) return false;  // 状态不合法：包损坏
    }
    const uint8_t* data = body + 8;
    const size_t base = out->size();
    out->resize(base + size_t(samplesPerChannel) * 2);
    int16_t* o = out->data() + base;
    for (int i = 0; i < samplesPerChannel; ++i) {
        o[i * 2] = int16_t(AdpcmDecodeNibble(&st[0], data[i] & 0x0F));
        o[i * 2 + 1] = int16_t(AdpcmDecodeNibble(&st[1], data[i] >> 4));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Opus

class OpusStream {
public:
#ifdef SYSDVR_WITH_OPUS
    OpusStream() {
        int err = OPUS_OK;
        dec_ = opus_decoder_create(kAudioSampleRate, kAudioChannels, &err);
        if (err != OPUS_OK) {
            Logf(LogLevel::Error, "创建 Opus 解码器失败：%s", opus_strerror(err));
            dec_ = nullptr;
        }
    }
    ~OpusStream() {
        if (dec_ != nullptr) opus_decoder_destroy(dec_);
    }
    bool Ok() const { return dec_ != nullptr; }
    void Reset() {
        if (dec_ != nullptr) opus_decoder_ctl(dec_, OPUS_RESET_STATE);
    }
    // 返回每声道采样数，< 0 表示失败
    int Decode(const uint8_t* p, size_t n, int16_t* pcm) {
        return opus_decode(dec_, p, opus_int32(n), pcm, kOpusMaxFrameSamples, 0);
    }
    // 丢包补偿：按帧长生成一段衔接得上的声音
    int Conceal(int16_t* pcm, int frames) { return opus_decode(dec_, nullptr, 0, pcm, frames, 0); }

private:
    OpusDecoder* dec_ = nullptr;
#else
    bool Ok() const { return false; }
    void Reset() {}
    int Decode(const uint8_t*, size_t, int16_t*) { return -1; }
    int Conceal(int16_t*, int) { return -1; }
#endif
};

bool ExtAudioDecoder::OpusAvailable() {
#ifdef SYSDVR_WITH_OPUS
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// ExtAudioDecoder

ExtAudioDecoder::ExtAudioDecoder() : opus_(new OpusStream()) {}

ExtAudioDecoder::~ExtAudioDecoder() = default;

void ExtAudioDecoder::Reset() {
    up_.Reset();
    opus_->Reset();
    lastCodec_ = -1;
}

bool ExtAudioDecoder::Decode(uint8_t replaySlot, const uint8_t* payload, size_t size, AudioPacketInfo* info,
                             const uint8_t** pcm, size_t* pcmBytes) {
    *pcm = nullptr;
    *pcmBytes = 0;
    info->codec = ExtAudioCodecFromSlot(replaySlot);
    info->hasHeader = false;
    if (info->codec == -2) {  // 扩展范围里不认识的编码：不能当 PCM 播（会是噪声）
        ++errors_;
        return false;
    }
    if (info->codec < 0 || info->codec == int(AudioCodec::Pcm48)) {
        // 官方包（0xFF）和扩展 PCM48（0xE0）：负载就是 48 kHz 立体声 s16，原样交出去
        lastCodec_ = int(AudioCodec::Pcm48);
        *pcm = payload;
        *pcmBytes = size - size % 4;
        return true;
    }

    ExtAudioHeader h;
    if (!ParseExtAudioHeader(payload, size, &h) || h.codec != info->codec) {
        ++errors_;
        return false;
    }
    info->hasHeader = true;
    info->header = h;
    const auto codec = AudioCodec(info->codec);
    if (lastCodec_ != info->codec) {
        // 切换编码时服务端会重置编码器：客户端也从干净的状态开始，免得拿别的时间线上的历史去滤波/预测
        if (codec == AudioCodec::Pcm24) up_.Reset();
        if (codec == AudioCodec::Opus) opus_->Reset();
        lastCodec_ = info->codec;
    }
    out_.clear();
    if (!DecodeCompressed(codec, h, payload + kExtAudioHeaderSize, size - kExtAudioHeaderSize)) {
        ++errors_;
        out_.clear();
        return false;
    }
    *pcm = reinterpret_cast<const uint8_t*>(out_.data());
    *pcmBytes = out_.size() * sizeof(int16_t);
    return true;
}

bool ExtAudioDecoder::DecodeCompressed(AudioCodec codec, const ExtAudioHeader& h, const uint8_t* body, size_t n) {
    const int spc = h.samplesPerChannel;
    switch (codec) {
        case AudioCodec::Pcm24: {
            if (spc <= 0 || spc > kMaxSamplesPerPacket || n < size_t(spc) * 4) return false;
            // 负载按小端 s16 排列；逐个组装，不依赖本机字节序和对齐
            std::vector<int16_t> in(size_t(spc) * 2);
            for (size_t i = 0; i < in.size(); ++i) in[i] = int16_t(uint16_t(body[i * 2] | (body[i * 2 + 1] << 8)));
            up_.Process(in.data(), size_t(spc), &out_);
            return true;
        }
        case AudioCodec::Adpcm:
            return DecodeAdpcmBody(body, n, spc, &out_);
        case AudioCodec::Opus: {
            if (!opus_->Ok()) return false;
            if (h.frameCount == 0 || h.frameCount > kMaxOpusFrames) return false;
            const int frameSamples = h.FrameMs() * kAudioSampleRate / 1000;
            // 单帧最大 120 ms 立体声约 23 KB：放在堆上（鸿蒙 musl 的线程栈比桌面小）
            frame_.resize(size_t(kOpusMaxFrameSamples) * 2);
            int16_t* frame = frame_.data();
            size_t pos = 0;
            for (int f = 0; f < h.frameCount; ++f) {
                if (pos + 2 > n) return false;
                const size_t len = size_t(body[pos] | (body[pos + 1] << 8));
                pos += 2;
                if (len == 0 || pos + len > n) return false;
                int got = opus_->Decode(body + pos, len, frame);
                pos += len;
                if (got < 0) {
                    // 这一帧坏了：计一次错误，用丢包补偿填上同样长度，后面的帧照常解
                    ++errors_;
                    got = opus_->Conceal(frame, frameSamples);
                    if (got < 0) {
                        std::memset(frame, 0, size_t(frameSamples) * 4);
                        got = frameSamples;
                    }
                }
                out_.insert(out_.end(), frame, frame + size_t(got) * 2);
            }
            return true;
        }
        case AudioCodec::Pcm48:
            break;
    }
    return false;
}

}  // namespace sysdvr
