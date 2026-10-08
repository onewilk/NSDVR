#!/usr/bin/env python3
"""音频压缩方案离线对比：把 48 kHz 立体声 16 位 PCM（和 Switch 发来的一样）按各方案编码再解码，
输出可试听的 WAV 和客观指标，帮助决定服务端（Switch）要实现哪种压缩。

用法：audio_codec_eval.py <输入.wav> <起始秒> <时长秒> <输出目录> <名字>
需要 numpy。服务端的做法尽量贴近 sysmodule 里能便宜实现的版本（短滤波器、定点 ADPCM），
客户端（手机）算力充足，重建时用更长的滤波器。
"""
import json, os, resource, subprocess, sys, tempfile, wave
import numpy as np

SR = 48000

# ---------- 基础 ----------

def read_wav(path, start_s, dur_s):
    w = wave.open(path)
    assert w.getframerate() == SR and w.getnchannels() == 2 and w.getsampwidth() == 2
    w.setpos(int(start_s * SR))
    x = np.frombuffer(w.readframes(int(dur_s * SR)), dtype=np.int16).reshape(-1, 2)
    return x.astype(np.float64)


def write_wav(path, x, sr=SR):
    y = np.clip(np.round(x), -32768, 32767).astype(np.int16)
    if y.ndim == 1:
        y = np.stack([y, y], axis=1)
    w = wave.open(path, 'wb')
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes(y.tobytes())
    w.close()


def q16(x):
    return np.clip(np.round(x), -32768, 32767)


def lowpass_fir(cutoff_hz, taps, sr=SR):
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * cutoff_hz / sr * n) * np.kaiser(taps, 8.0)
    return h / h.sum()


def filt(x, h):
    return np.stack([np.convolve(x[:, c], h, mode='same') for c in range(x.shape[1])], axis=1)

# ---------- 各方案 ----------

def mono(x):
    """服务端：(L+R)/2，码率减半；客户端复制成双声道"""
    m = q16((x[:, 0] + x[:, 1]) / 2)
    return np.stack([m, m], axis=1)


# 服务端降采样用 63 抽头半带滤波器（截止在 fs/4）：约一半系数为 0、左右对称，每个输出采样约 16 次乘加
HALFBAND_63 = lowpass_fir(12000, 63)
# 客户端升采样用 127 抽头，手机上开销可以忽略
UPSAMPLE_127 = lowpass_fir(11500, 127)


def down24(x):
    return q16(filt(x, HALFBAND_63)[::2])


def up48(y):
    z = np.zeros((y.shape[0] * 2, y.shape[1]))
    z[::2] = y * 2
    return filt(z, UPSAMPLE_127)


def rate24(x):
    return up48(down24(x))


def mono24(x):
    return rate24(mono(x))


# IMA ADPCM（和 WAV/DVI 标准一致的步长表）
STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
         97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
         724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
         4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
         18500, 20350, 22385, 24623, 27086, 29794, 32767]
INDEX_ADJ = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_channel(samples, shaping=0.0):
    """编码 + 解码一个声道，返回重建信号。shaping > 0 时做一阶噪声整形：
    把上一采样的量化误差按系数反馈进目标值，噪声谱变成 (1 - c·z⁻¹)，从中低频挪到高频，
    再由客户端低通切掉一部分。服务端每采样只多两次加减乘。"""
    pred, idx, err_prev = 0, 0, 0.0
    out = np.empty(len(samples))
    for i, s in enumerate(samples.tolist()):
        target = s - shaping * err_prev
        step = STEPS[idx]
        diff = target - pred
        code = 0
        if diff < 0:
            code, diff = 8, -diff
        delta = step >> 3
        if diff >= step:
            code |= 4; diff -= step; delta += step
        if diff >= step >> 1:
            code |= 2; diff -= step >> 1; delta += step >> 1
        if diff >= step >> 2:
            code |= 1; delta += step >> 2
        pred = pred - delta if code & 8 else pred + delta
        pred = -32768 if pred < -32768 else (32767 if pred > 32767 else pred)
        idx += INDEX_ADJ[code & 7]
        idx = 0 if idx < 0 else (88 if idx > 88 else idx)
        err_prev = pred - target
        out[i] = pred
    return out


def adpcm(x, shaping=0.0):
    xi = q16(x).astype(np.int64)
    return np.stack([adpcm_channel(xi[:, c], shaping) for c in range(2)], axis=1)


def client_cleanup(x, lpf_hz=15000, gate_db=-58.0, ratio=2.0):
    """客户端后处理：轻度低通 + 很温和的向下扩展（接近无声时再压低一点，尾音和安静段更干净）"""
    y = filt(x, lowpass_fir(lpf_hz, 127))
    env = np.sqrt(np.convolve(np.mean(y ** 2, axis=1), np.ones(480) / 480, mode='same')) / 32768 + 1e-9
    level = 20 * np.log10(env)
    gain_db = np.where(level < gate_db, (level - gate_db) * (ratio - 1), 0.0)
    gain_db = np.maximum(gain_db, -24.0)
    # 平滑增益，避免抽动（攻击 5ms、释放 80ms 的简化版：统一用 40ms 平滑）
    g = 10 ** (np.convolve(gain_db, np.ones(1920) / 1920, mode='same') / 20)
    return y * g[:, None]

# Opus：调用 libopus 参考编码器（opus-tools 的 opusenc/opusdec），20ms 一帧、恒定码率，模拟串流场景。
# 同时记下编码用掉的 CPU 时间，用来估算 Switch 上的开销
OPUS_CPU = {}


def opus(x, kbps, complexity, key):
    with tempfile.TemporaryDirectory() as d:
        src, enc, dec = f'{d}/in.wav', f'{d}/a.opus', f'{d}/out.wav'
        write_wav(src, x)
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        subprocess.run(['opusenc', '--quiet', '--bitrate', str(kbps), '--comp', str(complexity), '--framesize', '20',
                        '--hard-cbr', src, enc], check=True)
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        OPUS_CPU[key] = (after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime)
        subprocess.run(['opusdec', '--quiet', '--rate', str(SR), enc, dec], check=True)
        w = wave.open(dec)
        y = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, w.getnchannels()).astype(np.float64)
    return y[: len(x)]


# ---------- 指标 ----------
# 只在所有方案频响都平直的 20 Hz–8 kHz 主频段里比较“附加噪声”（量化噪声、混叠），
# 降采样/低通切掉的高频不算噪声，另外在表里说明。取中间声道，单声道丢掉的左右差另算。

LP8K = lowpass_fir(8000, 255)


def mid(x):
    return (x[:, 0] + x[:, 1]) / 2 / 32768


def db(p):
    return 10 * np.log10(p + 1e-20)


def main_band(x):
    return np.convolve(mid(x), LP8K, mode='same')


def snr_main(ref, test):
    r, t = main_band(ref), main_band(test)
    return db(np.mean(r ** 2)) - db(np.mean((t - r) ** 2))


def quiet_snr(ref, test, frac=0.1):
    """原始信号最安静的 10% 片段（每段 100ms）里的信噪比——底噪最容易被听出来的地方"""
    r, t = main_band(ref), main_band(test)
    seg = SR // 10
    n = len(r) // seg
    rs, es = r[: n * seg].reshape(n, seg), (t - r)[: n * seg].reshape(n, seg)
    pick = np.argsort(np.mean(rs ** 2, axis=1))[: max(1, int(n * frac))]
    return db(np.mean(rs[pick] ** 2)) - db(np.mean(es[pick] ** 2)), db(np.mean(rs[pick] ** 2))


def side_kept(ref, test):
    s_ref = ref[:, 0] - ref[:, 1]
    s_test = test[:, 0] - test[:, 1]
    return float(np.dot(s_ref, s_test) / (np.dot(s_ref, s_ref) + 1e-9))


VARIANTS = [
    ('1_original', '原始 PCM', 1536, lambda x: q16(x)),
    ('2_mono', '单声道', 768, mono),
    ('3_24k', '立体声 24 kHz', 768, rate24),
    ('4_mono_24k', '单声道 24 kHz', 384, mono24),
    ('5_adpcm', 'IMA ADPCM', 384, lambda x: adpcm(x)),
    ('6_adpcm_cleanup', 'ADPCM + 客户端低通/静音门', 384, lambda x: client_cleanup(adpcm(x))),
    ('7_adpcm_shaped', 'ADPCM + 噪声整形 + 客户端低通/静音门', 384, lambda x: client_cleanup(adpcm(x, 0.7))),
    ('8_opus64_c0', 'Opus 64 kbps（复杂度 0）', 64, lambda x: opus(x, 64, 0, '8_opus64_c0')),
    ('9_opus96_c0', 'Opus 96 kbps（复杂度 0）', 96, lambda x: opus(x, 96, 0, '9_opus96_c0')),
    ('10_opus96_c10', 'Opus 96 kbps（复杂度 10）', 96, lambda x: opus(x, 96, 10, '10_opus96_c10')),
]


def main():
    src, start, dur, outdir, name = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4], sys.argv[5]
    os.makedirs(outdir, exist_ok=True)
    x = read_wav(src, start, dur)
    ref = q16(x)
    rows = []
    for key, label, kbps, fn in VARIANTS:
        y = fn(x)[: len(ref)]
        write_wav(os.path.join(outdir, f'{name}_{key}.wav'), y)
        rows.append({
            'track': name, 'key': key, 'label': label, 'kbps': kbps,
            'snr_db': round(min(snr_main(ref, y), 99), 1),
            'quiet_snr_db': round(min(quiet_snr(ref, y)[0], 99), 1),
            'side_kept': round(side_kept(ref, y), 2),
        })
        if key in OPUS_CPU:
            rows[-1]['perceptual'] = True
            rows[-1]['encode_cpu_pct'] = round(OPUS_CPU[key] / dur * 100, 2)  # 本机编码 CPU 时间占音频时长的百分比
        print(json.dumps(rows[-1], ensure_ascii=False), flush=True)
    meta = {'track': name, 'level_db': round(db(np.mean(mid(ref) ** 2)), 1),
            'quiet_level_db': round(quiet_snr(ref, ref)[1], 1), 'rows': rows}
    json.dump(meta, open(os.path.join(outdir, f'{name}_metrics.json'), 'w'), ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()
