#!/usr/bin/env python3
"""把 audio_codec_eval.py 的输出做成本地试听页：同一首曲子在各方案间切换时保持播放位置，支持盲听。
用法：audio_eval_page.py <输出目录> <曲目名>...   （生成 <输出目录>/index.html，用浏览器直接打开）"""
import html, json, os, sys

HIGH = {'1_original': '24 kHz', '2_mono': '24 kHz', '3_24k': '约 11 kHz', '4_mono_24k': '约 11 kHz',
        '5_adpcm': '24 kHz', '6_adpcm_cleanup': '15 kHz', '7_adpcm_shaped': '15 kHz',
        '8_opus64_c0': '20 kHz', '9_opus96_c0': '20 kHz', '10_opus96_c10': '20 kHz'}
# Switch（Cortex-A57 @1.02GHz）上的 CPU 占用估算，单位：一个核心的百分比。
# 非 Opus 方案按运算量估算；Opus 用本机（Apple M4）实测编码耗时 × 10–20 倍的性能差距粗估
CPU = {'1_original': '—', '2_mono': '<0.05%', '3_24k': '约 0.1%', '4_mono_24k': '约 0.1%',
       '5_adpcm': '约 0.3–0.5%', '6_adpcm_cleanup': '约 0.3–0.5%', '7_adpcm_shaped': '约 0.3–0.5%'}
DELAY = {'3_24k': '<1 ms', '4_mono_24k': '<1 ms'}


def cpu_text(r):
    if 'encode_cpu_pct' in r:
        lo, hi = r['encode_cpu_pct'] * 10, r['encode_cpu_pct'] * 20
        return f'约 {lo:.0f}–{hi:.0f}%'
    return CPU.get(r['key'], '')
CREDITS = {
    'drozerix': 'Drozerix – 4 RNDD!（CC0，Wikimedia Commons）',
    'aliens': 'Raspberrymusic – Aliens（CC BY 3.0，Wikimedia Commons）',
}

PAGE = """<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>音频压缩试听</title>
<style>
:root{--bg:#fafaf9;--fg:#1c1917;--muted:#78716c;--card:#fff;--line:#e7e5e4;--accent:#2563eb;--good:#15803d;--bad:#b91c1c}
@media (prefers-color-scheme:dark){:root{--bg:#1c1917;--fg:#f5f5f4;--muted:#a8a29e;--card:#292524;--line:#44403c;--accent:#60a5fa;--good:#4ade80;--bad:#f87171}}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.6 -apple-system,"PingFang SC",sans-serif}
main{max-width:980px;margin:0 auto;padding:24px 16px 64px}
h1{font-size:22px;margin:0 0 4px}h2{font-size:18px;margin:32px 0 8px}
p.note{color:var(--muted);margin:4px 0 16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin:12px 0}
.btns{display:flex;flex-wrap:wrap;gap:8px;margin:12px 0}
button{font:inherit;border:1px solid var(--line);background:transparent;color:var(--fg);border-radius:8px;padding:6px 12px;cursor:pointer}
button.on{background:var(--accent);border-color:var(--accent);color:#fff}
audio{width:100%}
table{width:100%;border-collapse:collapse;font-size:14px;margin-top:8px}
th,td{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line)}th{color:var(--muted);font-weight:500}
td.num{font-variant-numeric:tabular-nums}.good{color:var(--good)}.bad{color:var(--bad)}
.hidden{display:none}.row{display:flex;gap:12px;align-items:center;flex-wrap:wrap}
@media (max-width:640px){table{font-size:12px}th,td{padding:4px}}
</style></head><body><main>
<h1>SysDVR 音频压缩方案试听</h1>
<p class="note">戴耳机听。点按钮切换方案，播放位置保持不变，方便同一段来回对比。“盲听”会隐藏方案名并打乱顺序，听完再点“揭晓”。<br>
信噪比只统计所有方案都保持平直的 20 Hz–8 kHz 主频段里多出来的噪声和混叠；被切掉的高频不算噪声，看“高频上限”一列。数值越大越干净，一般认为 40 dB 以上在音乐里不太容易听出来，20 dB 左右能明显听到。<br>
Opus 是感知编码：它不保留波形，而是丢掉耳朵听不出的部分，所以波形信噪比很低也可能听不出区别，只能靠耳朵判断。Switch CPU 一列是占一个核心的百分比估算，Opus 按本机实测编码耗时粗估。</p>
<div class="row"><button id="blind">盲听</button><button id="reveal" class="hidden">揭晓</button></div>
__TRACKS__
<p class="note">素材：__CREDITS__。仅用于本地编解码评估。</p>
</main><script>
const tracks=document.querySelectorAll('[data-track]');
tracks.forEach(t=>{
  const audio=t.querySelector('audio');
  t.querySelectorAll('button[data-src]').forEach(b=>b.onclick=()=>{
    const time=audio.currentTime, playing=!audio.paused;
    audio.src=b.dataset.src; audio.currentTime=time;
    audio.addEventListener('loadedmetadata',()=>{audio.currentTime=time; if(playing) audio.play();},{once:true});
    t.querySelectorAll('button[data-src]').forEach(x=>x.classList.toggle('on',x===b));
  });
});
let blind=false;
document.getElementById('blind').onclick=()=>{
  blind=true;
  tracks.forEach(t=>{
    const box=t.querySelector('.btns'); const bs=[...box.children];
    bs.sort(()=>Math.random()-0.5).forEach((b,i)=>{b.dataset.label=b.textContent; b.textContent='方案 '+String.fromCharCode(65+i); box.appendChild(b);});
    t.querySelector('table').classList.add('hidden');
  });
  document.getElementById('reveal').classList.remove('hidden');
};
document.getElementById('reveal').onclick=()=>{
  tracks.forEach(t=>{t.querySelectorAll('button[data-src]').forEach(b=>{if(b.dataset.label) b.textContent=b.textContent+' = '+b.dataset.label;}); t.querySelector('table').classList.remove('hidden');});
  document.getElementById('reveal').classList.add('hidden');
};
</script></body></html>"""


def cls(v):
    return 'good' if v >= 40 else ('bad' if v < 30 else '')


def track_html(outdir, name):
    m = json.load(open(os.path.join(outdir, f'{name}_metrics.json')))
    rows = m['rows']
    btns = ''.join(f'<button data-src="{name}_{r["key"]}.wav"{" class=on" if i == 0 else ""}>{html.escape(r["label"])}</button>'
                   for i, r in enumerate(rows))
    trs = ''.join(
        f'<tr><td>{html.escape(r["label"])}</td><td class=num>{r["kbps"]}</td>'
        + ('<td colspan=2>感知编码，不看波形信噪比，以听感为准</td><td>保留</td>' if r.get('perceptual') else
           f'<td class="num {cls(r["snr_db"])}">{"无损" if r["snr_db"] >= 90 else r["snr_db"]}</td>'
           f'<td class="num {cls(r["quiet_snr_db"])}">{"无损" if r["quiet_snr_db"] >= 90 else r["quiet_snr_db"]}</td>'
           f'<td>{"保留" if r["side_kept"] > 0.9 else "丢失（单声道）"}</td>')
        + f'<td>{HIGH.get(r["key"], "")}</td><td class=num>{cpu_text(r)}</td>'
          f'<td>{"约 26 ms" if r.get("perceptual") else DELAY.get(r["key"], "0")}</td></tr>'
        for r in rows)
    return (f'<h2>{html.escape(CREDITS.get(name, name))}</h2><div class="card" data-track="{name}">'
            f'<audio controls preload="auto" src="{name}_{rows[0]["key"]}.wav"></audio><div class="btns">{btns}</div>'
            f'<table><tr><th>方案</th><th>码率 kbps</th><th>信噪比 dB</th><th>安静段信噪比 dB</th><th>立体声</th><th>高频上限</th><th>Switch CPU（估）</th><th>额外延迟</th></tr>{trs}</table>'
            f'<p class="note">这段平均电平 {m["level_db"]} dBFS，最安静 10% 的片段约 {m["quiet_level_db"]} dBFS。</p></div>')


def main():
    outdir, names = sys.argv[1], sys.argv[2:]
    page = PAGE.replace('__TRACKS__', ''.join(track_html(outdir, n) for n in names))
    page = page.replace('__CREDITS__', '；'.join(html.escape(CREDITS.get(n, n)) for n in names))
    open(os.path.join(outdir, 'index.html'), 'w').write(page)
    print(os.path.join(outdir, 'index.html'))


if __name__ == '__main__':
    main()
