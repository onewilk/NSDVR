#!/usr/bin/env python3
"""检查 native 代码里所有 L("简体", "繁體", "English") 调用：参数必须正好三个字符串，且三者的 printf 占位符一致。
用法：python3 tools/i18n_native_check.py"""
import glob, os, re, sys

ROOT = os.path.join(os.path.dirname(__file__), '..')
FILES = glob.glob(os.path.join(ROOT, 'core', '*.cpp')) + glob.glob(os.path.join(ROOT, 'core', '*.h')) + \
    glob.glob(os.path.join(ROOT, 'harmony', 'entry', 'src', 'main', 'cpp', '*.cpp'))
PH = re.compile(r'%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:ll|l|z|h)?[sdufxc])')

def parse_args(src, i):
    """从 L( 后的位置解析参数，返回 ([参数字符串...], 结束位置)；参数不是纯字符串字面量时返回 None"""
    args, cur, depth = [], None, 0
    while i < len(src):
        c = src[i]
        if c == '"':
            j = i + 1
            buf = []
            while src[j] != '"':
                if src[j] == '\\':
                    buf.append(src[j:j + 2]); j += 2; continue
                buf.append(src[j]); j += 1
            cur = (cur or '') + ''.join(buf)
            i = j + 1
            continue
        if c in ' \t\r\n':
            i += 1; continue
        if c == ',' and depth == 0:
            args.append(cur); cur = None; i += 1; continue
        if c == ')' and depth == 0:
            args.append(cur)
            return args, i
        return None, i  # 非字面量参数
    return None, i

def main():
    bad = 0; total = 0
    for f in FILES:
        if f.endswith('i18n.h') or f.endswith('i18n.cpp'):
            continue
        src = open(f, encoding='utf-8').read()
        for m in re.finditer(r'(?<![A-Za-z0-9_])L\(', src):
            line = src.count('\n', 0, m.start()) + 1
            args, _ = parse_args(src, m.end())
            rel = os.path.relpath(f, ROOT)
            if args is None or len(args) != 3 or any(a is None for a in args):
                print(f'✗ {rel}:{line} L() 需要三个字符串字面量'); bad += 1; continue
            total += 1
            phs = [PH.findall(a) for a in args]
            if not (phs[0] == phs[1] == phs[2]):
                print(f'✗ {rel}:{line} 占位符不一致：{phs}'); bad += 1
    print(f'L() 调用 {total} 处' + ('，全部一致' if not bad else f'，{bad} 处有问题'))
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
