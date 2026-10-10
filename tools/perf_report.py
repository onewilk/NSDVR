#!/usr/bin/env python3
"""Audio compression test report: console CPU (SysDVR perf logger) joined with the client's stats.csv.

The perf logger is a test-only build of the sysmodule (NSDVR-server, branches perflog / nsdvr-perflog,
`make PERFLOG=1`). Once per second it writes /config/sysdvr/perf/perf_<unix time>.csv on the SD card:
busy permille of every CPU core, CPU time of SysDVR's threads, bytes sent, the foreground title and the
audio path that is streaming. This script groups those seconds by build, game and audio path and, when
the client's stats.csv is given, adds the network side (frame loss, grc gaps, send blocking).

  python3 tools/perf_report.py official=perf_1791600000.csv nsdvr=perf_1791700000.csv --stats stats.csv
  python3 tools/perf_report.py perf_*.csv --stats stats.csv --lang en --csv report.csv

A console file may be prefixed with `label=` (official / nsdvr); without a label the build is detected
from the audio column (any extension encoding means nsdvr). The console clock and the phone clock are
aligned automatically: by the audio encoding per second when the extension switched encodings, otherwise
by the bitrate curve. --offset overrides it (seconds to add to the console time).
"""
import argparse
import csv
import datetime as dt
import math
import os
import re
import statistics
import sys

# Titles that are likely to be tested; anything else is printed as its title ID
TITLES = {
    '0100f2c0115b6000': 'Tears of the Kingdom',
    '01007ef00011e000': 'Breath of the Wild',
    '0100152000022000': 'Mario Kart 8 Deluxe',
    '0100000000010000': 'Super Mario Odyssey',
    '01006a800016e000': 'Smash Bros. Ultimate',
    '01006f8002326000': 'Animal Crossing: NH',
    '0100c2500fc20000': 'Splatoon 3',
    '0000000000000000': '(home menu)',
}

# Seconds dropped around every change of game / streaming state / audio path (edge effects)
SETTLE_AFTER = 3
SETTLE_BEFORE = 1

TEXT = {
    'zh': {
        'cpu_title': '## 主机 CPU（每组的有效秒数；占用为平均值，括号里为 P90）',
        'cpu_cols': ['版本', '游戏', '状态', '秒', '核心0', '核心1', '核心2', '核心3', '核心3 增量',
                     'SysDVR 线程', '视频线程', '音频线程', '记录器', '编码 ms/s', '发送 kbps', 'CPU MHz', '系统池剩余 MB'],
        'net_title': '## 网络（手机 stats.csv，按同一秒对齐）',
        'net_cols': ['版本', '游戏', '状态', '秒', '丢帧率', 'grc 断档/分', '断档前发送慢/分', '发送阻塞 ms/s',
                     '卡顿/分', '音频欠载/分', '音频 kbps'],
        'state_idle': '未串流', 'state_video': '只有画面',
        'delta_note': '核心3 增量：相对同一游戏、同一版本“未串流”时的核心 3 占用（百分点）。',
        'align': '时钟对齐：主机时间 + {off} 秒 = 手机时间（{how}，{match}）',
        'align_none': '没有找到和手机记录重叠的时间段，网络表跳过（可用 --offset 手动指定）',
        'by_tag': '按音频编码一致率', 'by_rate': '按码率曲线相关性',
        'files': '主机记录：{n} 个文件，{rows} 秒；手机记录：{p} 秒',
    },
    'en': {
        'cpu_title': '## Console CPU (valid seconds per group; busy is the mean, P90 in brackets)',
        'cpu_cols': ['Build', 'Game', 'State', 'Secs', 'Core 0', 'Core 1', 'Core 2', 'Core 3', 'Core 3 delta',
                     'SysDVR threads', 'Video thread', 'Audio thread', 'Logger', 'Encode ms/s', 'Sent kbps', 'CPU MHz',
                     'Sys pool free MB'],
        'net_title': '## Network (client stats.csv, joined per second)',
        'net_cols': ['Build', 'Game', 'State', 'Secs', 'Frame loss', 'grc gaps/min', 'Gaps after slow send/min',
                     'Send blocking ms/s', 'Stutters/min', 'Audio underruns/min', 'Audio kbps'],
        'state_idle': 'not streaming', 'state_video': 'video only',
        'delta_note': 'Core 3 delta: core 3 busy minus the same game\'s "not streaming" baseline on the same build '
                      '(percentage points).',
        'align': 'Clock alignment: console time + {off} s = phone time ({how}, {match})',
        'align_none': 'No overlap with the phone log; network table skipped (use --offset)',
        'by_tag': 'audio encoding agreement', 'by_rate': 'bitrate correlation',
        'files': 'Console logs: {n} files, {rows} s; phone log: {p} s',
    },
}

EXT_TAGS = re.compile(r'^(pcm|24k|adpcm|opus/\d+/\d+/\d+)$')


def num(v, default=None):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


# ----------------------------------------------------------------------------- console log

def read_console(path, label):
    rows = []
    with open(path, newline='') as f:
        lines = [ln for ln in f if not ln.startswith('#')]
    for r in csv.DictReader(lines):
        if not r.get('unix'):
            continue
        rows.append(r)
    if label is None:
        label = 'nsdvr' if any(EXT_TAGS.match(r.get('audio', '')) for r in rows) else 'official'
    for r in rows:
        r['_build'] = label
        r['_t'] = int(num(r['unix'], 0))
        sent = num(r.get('send_kbps'), 0)
        video = num(r.get('video_cpu'), 0)
        audio = r.get('audio', '-') or '-'
        if sent < 100 and video <= 0:
            state = 'idle'
        elif audio == '-':
            state = 'video'
        else:
            state = audio
        r['_state'] = state
        r['_title'] = (r.get('title_id') or '').lower()
    return rows


def mark_settled(rows):
    """Flags seconds that are far enough from a change of game / state (or a gap in the log) to count."""
    key = [(r['_build'], r['_title'], r['_state']) for r in rows]
    n = len(rows)
    for i in range(n):
        ok = i >= SETTLE_AFTER
        if ok:
            for j in range(i - SETTLE_AFTER, i):
                if key[j] != key[i] or rows[j + 1]['_t'] - rows[j]['_t'] > 2:
                    ok = False
        for j in range(i + 1, min(n, i + 1 + SETTLE_BEFORE)):
            if key[j] != key[i]:
                ok = False
        rows[i]['_ok'] = ok


# ----------------------------------------------------------------------------- phone log

def read_phone(path):
    """stats.csv: '# session <ISO time> …' then a header and one row per second (t_s from session start)."""
    out = []
    start = None
    header = None
    with open(path, newline='') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('# session '):
                iso = line.split()[2]
                try:
                    start = dt.datetime.fromisoformat(iso.replace('Z', '+00:00')).timestamp()
                except ValueError:
                    start = None
                header = None
                continue
            if line.startswith('#') or not line:
                continue
            if line.startswith('t_s,'):
                header = line.split(',')
                continue
            if header is None or start is None:
                continue
            vals = line.split(',')
            r = dict(zip(header, vals))
            t = num(r.get('t_s'))
            if t is None:
                continue
            r['_t'] = int(round(start + t))
            r['_tag'] = phone_tag(r)
            out.append(r)
    return out


def phone_tag(r):
    codec = num(r.get('audio_codec'), -2)
    if num(r.get('audio_on'), 0) <= 0:
        return '-'
    if codec == -1:
        return 'official'
    if codec == 0:
        return 'pcm'
    if codec == 1:
        return '24k'
    if codec == 2:
        return 'adpcm'
    if codec == 3:
        return 'opus/%d/%d/%d' % (num(r.get('audio_kbps_cfg'), 0), num(r.get('opus_complexity'), 0),
                                  num(r.get('opus_frame_ms'), 0))
    return '?'


def align(console, phone):
    """Seconds to add to console time to get phone time, plus how it was found."""
    if not console or not phone:
        return None
    by_t = {r['_t']: r for r in phone}
    ext = [r for r in console if EXT_TAGS.match(r['_state'])]
    if len(ext) >= 30:
        scores = {}
        for off in range(-900, 901):
            hits = total = 0
            for r in ext:
                p = by_t.get(r['_t'] + off)
                if p is None:
                    continue
                total += 1
                hits += p['_tag'] == r['_state']
            if total >= 30:
                scores[off] = hits / total
        if scores:
            top = max(scores.values())
            # Several neighbouring offsets can match equally well: take the middle of that plateau
            plateau = sorted(o for o, v in scores.items() if v >= top - 1e-9)
            if top >= 0.8:
                return plateau[len(plateau) // 2], 'tag', top
    # Bitrate curve: console bytes sent vs the phone's received video + audio
    cons = [(r['_t'], num(r.get('send_kbps'), 0)) for r in console if r['_state'] != 'idle']
    if len(cons) < 30:
        return None
    best = None
    for off in range(-900, 901):
        xs, ys = [], []
        for t, k in cons:
            p = by_t.get(t + off)
            if p is not None:
                xs.append(k)
                ys.append(num(p.get('video_kbps'), 0) + num(p.get('audio_kbps'), 0))
        if len(xs) < 30:
            continue
        try:
            c = statistics.correlation(xs, ys)
        except statistics.StatisticsError:
            continue
        if best is None or c > best[1]:
            best = (off, c, len(xs))
    if best is None or best[1] < 0.5:
        return None
    return best[0], 'rate', best[1]


# ----------------------------------------------------------------------------- report

def title_name(tid):
    return TITLES.get(tid, tid or '?')


def state_name(state, tx):
    if state == 'idle':
        return tx['state_idle']
    if state == 'video':
        return tx['state_video']
    return state


def state_order(state):
    order = ['idle', 'video', 'official', 'pcm', '24k', 'adpcm']
    return (order.index(state), '') if state in order else (len(order), state)


def mean(xs):
    return sum(xs) / len(xs) if xs else None


def p90(xs):
    if not xs:
        return None
    s = sorted(xs)
    return s[min(len(s) - 1, int(math.ceil(len(s) * 0.9)) - 1)]


def pct(permille):
    return None if permille is None else permille / 10.0


def fmt(v, digits=1):
    return '—' if v is None else f'{v:.{digits}f}'


def column(rows, name):
    return [v for v in (num(r.get(name)) for r in rows) if v is not None and v >= 0]


def table(cols, body):
    out = ['| ' + ' | '.join(cols) + ' |', '|' + '|'.join(['---'] * len(cols)) + '|']
    out += ['| ' + ' | '.join(str(c) for c in row) + ' |' for row in body]
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('console', nargs='+', help='perf_*.csv from the SD card, optionally label=path')
    ap.add_argument('--stats', help="the client's stats.csv")
    ap.add_argument('--offset', type=int, help='seconds to add to the console time (skips auto alignment)')
    ap.add_argument('--lang', choices=['zh', 'en'], default='zh')
    ap.add_argument('--csv', help='also write the CPU table as CSV')
    args = ap.parse_args()
    tx = TEXT[args.lang]

    console = []
    for spec in args.console:
        label, path = (spec.split('=', 1) if '=' in spec and not os.path.exists(spec) else (None, spec))
        console += read_console(path, label)
    console.sort(key=lambda r: (r['_build'], r['_t']))
    mark_settled(console)
    phone = read_phone(args.stats) if args.stats else []
    print(tx['files'].format(n=len(args.console), rows=len(console), p=len(phone)))

    groups = {}
    for r in console:
        if r['_ok']:
            groups.setdefault((r['_build'], r['_title'], r['_state']), []).append(r)

    idle_c3 = {}
    for (build, title, state), rows in groups.items():
        if state == 'idle':
            idle_c3[(build, title)] = mean(column(rows, 'c3'))

    cpu_body, csv_rows = [], []
    for key in sorted(groups, key=lambda k: (k[0], title_name(k[1]), state_order(k[2]))):
        build, title, state = key
        rows = groups[key]
        cores = []
        for c in ('c0', 'c1', 'c2', 'c3'):
            xs = column(rows, c)
            cores.append(f'{fmt(pct(mean(xs)))} ({fmt(pct(p90(xs)), 0)})')
        c3 = mean(column(rows, 'c3'))
        base = idle_c3.get((build, title))
        delta = None if c3 is None or base is None or state == 'idle' else (c3 - base) / 10.0
        enc = mean(column(rows, 'enc_us'))
        mhz = column(rows, 'cpu_mhz')
        free = column(rows, 'sys_free_kb')
        line = [build, title_name(title), state_name(state, tx), len(rows)] + cores + [
            fmt(delta), fmt(pct(mean(column(rows, 'sysdvr_cpu')))), fmt(pct(mean(column(rows, 'video_cpu')))),
            fmt(pct(mean(column(rows, 'audio_cpu')))), fmt(pct(mean(column(rows, 'log_cpu'))), 2),
            fmt(None if enc is None else enc / 1000, 2), fmt(mean(column(rows, 'send_kbps')), 0),
            fmt(statistics.median(mhz), 0) if mhz else '—', fmt(min(free) / 1024, 2) if free else '—']
        cpu_body.append(line)
        csv_rows.append(line)

    print()
    print(tx['cpu_title'])
    print()
    print(table(tx['cpu_cols'], cpu_body))
    print()
    print(tx['delta_note'])

    if args.csv:
        with open(args.csv, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(tx['cpu_cols'])
            w.writerows(csv_rows)

    if not phone:
        return
    print()
    print(tx['net_title'])
    print()
    if args.offset is not None:
        found = (args.offset, 'manual', 1.0)
    else:
        found = align(console, phone)
    if found is None:
        print(tx['align_none'])
        return
    off, how, score = found
    how_text = {'tag': tx['by_tag'], 'rate': tx['by_rate']}.get(how, how)
    print(tx['align'].format(off=off, how=how_text, match=f'{score:.2f}'))
    print()
    by_t = {r['_t']: r for r in phone}
    net_body = []
    for key in sorted(groups, key=lambda k: (k[0], title_name(k[1]), state_order(k[2]))):
        build, title, state = key
        if state == 'idle':
            continue
        joined = [by_t[r['_t'] + off] for r in groups[key] if r['_t'] + off in by_t]
        if not joined:
            continue
        secs = len(joined)
        lost = sum(num(p.get('lost_frames'), 0) for p in joined)
        recv = sum(num(p.get('recv_fps'), 0) for p in joined)
        per_min = lambda name: sum(num(p.get(name), 0) for p in joined) * 60 / secs
        net_body.append([
            build, title_name(title), state_name(state, tx), secs,
            fmt(100 * lost / (recv + lost), 2) + '%' if recv + lost > 0 else '—',
            fmt(per_min('grc_gaps')), fmt(per_min('gaps_after_slow')),
            fmt(sum(num(p.get('block_total_ms'), 0) for p in joined) / secs),
            fmt(per_min('stutters')), fmt(per_min('underruns')),
            fmt(mean([num(p.get('audio_kbps'), 0) for p in joined]), 0)])
    print(table(tx['net_cols'], net_body))


if __name__ == '__main__':
    sys.exit(main())
