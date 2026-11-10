#!/usr/bin/env python3
"""Checks the web page's translations: web/i18n/zh.json and en.json have the same keys, and
every key the page names literally (data-i18n="...", t('...'), tr('...')) is in them. Keys
built at run time (mode.*, err.*, ...) are not checked here. Exits non-zero on a problem.

    python3 tools/i18n_check.py
"""
import glob
import json
import os
import re
import sys

web = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'web')
zh = json.load(open(os.path.join(web, 'i18n', 'zh.json'), encoding='utf-8'))
en = json.load(open(os.path.join(web, 'i18n', 'en.json'), encoding='utf-8'))
problems = [f'only in zh.json: {k}' for k in sorted(set(zh) - set(en))]
problems += [f'only in en.json: {k}' for k in sorted(set(en) - set(zh))]
for f in sorted(glob.glob(os.path.join(web, '**', '*.js'), recursive=True)) + [os.path.join(web, 'index.html')]:
    if '/vendor/' in f:
        continue
    text = open(f, encoding='utf-8').read()
    used = set(re.findall(r'data-i18n="([a-z_.]+)"', text))
    used |= set(re.findall(r"""\b(?:t|tr)\('([a-z_]+\.[a-z_.]+)'""", text))
    problems += [f'{os.path.relpath(f, web)}: missing {k}' for k in sorted(used - set(en))]
print('\n'.join(problems) or f'ok: {len(en)} keys')
sys.exit(1 if problems else 0)
