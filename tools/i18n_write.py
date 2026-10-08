#!/usr/bin/env python3
"""把 {键: (简体, 繁體, English)} 写成三种语言的 strings_<name>.json（覆盖同名文件）。
用法：在脚本里 import 后调用 write('app', TABLE)，或 python3 i18n_write.py <name> <table.json>
table.json 格式：{"键": ["简体", "繁體", "English"], ...}"""
import json, os, sys

RES = os.path.join(os.path.dirname(__file__), '..', 'harmony', 'entry', 'src', 'main', 'resources')

def write(name, table):
    for i, locale in enumerate(['zh_CN', 'zh_TW', 'base']):
        items = [{'name': k, 'value': v[i]} for k, v in table.items()]
        d = os.path.join(RES, locale, 'element')
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, f'strings_{name}.json'), 'w', encoding='utf-8') as f:
            json.dump({'string': items}, f, ensure_ascii=False, indent=2)
            f.write('\n')

if __name__ == '__main__':
    write(sys.argv[1], json.load(open(sys.argv[2], encoding='utf-8')))
