#!/usr/bin/env python3
"""多语言资源检查：
  1. 各语言的 strings_*.json 是合法 JSON，键不重复；
  2. 代码里 tr('键') 用到的键，在 base（英文）、zh_CN、zh_TW 里都存在；
  3. 同一个键在各语言里的占位符（%s / %d / %1$s）数量和类型一致；
  4. 列出 .ets 里还没迁移的中文字面量（注释、日志调用除外），便于收尾；
用法：python3 tools/i18n_check.py [--sync] [--strict]
  --sync   把 zh_TW 的 strings_*.json 复制到 zh_HK
  --strict 仍有中文字面量时返回非 0
"""
import json, os, re, sys, shutil, glob

ROOT = os.path.join(os.path.dirname(__file__), '..', 'harmony', 'entry', 'src', 'main')
RES = os.path.join(ROOT, 'resources')
ETS = os.path.join(ROOT, 'ets')
LOCALES = ['base', 'zh_CN', 'zh_TW']
PH = re.compile(r'%(?:\d+\$)?[sd]')

def load(locale):
    out = {}
    for f in sorted(glob.glob(os.path.join(RES, locale, 'element', 'strings_*.json'))):
        try:
            data = json.load(open(f, encoding='utf-8'))
        except Exception as e:
            print(f'✗ {f}: JSON 解析失败：{e}'); sys.exit(1)
        for item in data.get('string', []):
            k = item['name']
            if k in out:
                print(f'✗ {locale}: 键重复 {k}（{f}）')
            out[k] = item['value']
    return out

def main():
    ok = True
    if '--sync' in sys.argv:
        os.makedirs(os.path.join(RES, 'zh_HK', 'element'), exist_ok=True)
        for f in glob.glob(os.path.join(RES, 'zh_TW', 'element', 'strings_*.json')):
            shutil.copy(f, os.path.join(RES, 'zh_HK', 'element', os.path.basename(f)))
    tables = {l: load(l) for l in LOCALES}
    keys = set().union(*[set(t) for t in tables.values()])
    for k in sorted(keys):
        missing = [l for l in LOCALES if k not in tables[l]]
        if missing:
            ok = False; print(f'✗ 键 {k} 缺少语言：{", ".join(missing)}')
            continue
        phs = {l: sorted(PH.findall(tables[l][k])) for l in LOCALES}
        if len({tuple(v) for v in phs.values()}) > 1:
            ok = False; print(f'✗ 键 {k} 占位符不一致：{phs}')
    used = set()
    literal = []
    tr_re = re.compile(r"\btr\(\s*'([a-zA-Z0-9_]+)'")
    for f in glob.glob(os.path.join(ETS, '**', '*.ets'), recursive=True):
        rel = os.path.relpath(f, ETS)
        in_block = False
        for no, line in enumerate(open(f, encoding='utf-8'), 1):
            for m in tr_re.finditer(line):
                used.add((m.group(1), rel, no))
            s = line.strip()
            if in_block:
                if '*/' in s: in_block = False
                continue
            if s.startswith('/*') or s.startswith('/**'):
                in_block = '*/' not in s
                continue
            code = re.sub(r'//.*$', '', line)
            if re.search(r'(hilog\.|UsbLog\.write|this\.log\(|Logf|console\.)', code):
                continue
            if re.search(r"['`\"][^'`\"]*[一-鿿]", code):
                literal.append(f'{rel}:{no}: {s[:90]}')
    for k, rel, no in sorted(used):
        if k not in keys:
            ok = False; print(f'✗ {rel}:{no} 用到的键 {k} 不存在')
    print(f'键 {len(keys)} 个，代码引用 {len({u[0] for u in used})} 个')
    if literal:
        print(f'还有 {len(literal)} 处中文字面量未迁移：')
        for l in literal[:200]: print('  ' + l)
    if ok:
        print('✓ 资源检查通过')
    sys.exit(0 if ok and (not literal or '--strict' not in sys.argv) else 1)

if __name__ == '__main__':
    main()
