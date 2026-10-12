#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
leak_scan.py — 受保护模式扫描器（哈希规则版）

设计：规则原文不入库——patterns.sha256 只存 sha256 前 16 位与边界标志；
      扫描时对文本做归一化（小写 / 非 [a-z0-9 及 CJK] 折叠为单空格），
      再按规则长度窗口逐一哈希比对。命中也只报位置与哈希，不还原词。
      另有一组「形态检查」（明文 regex）：MAC / tailscale CGNAT / SSH 公钥头——
      这些形态本身不含可哈希隐藏的敏感原文，与词哈希表互补（2026-10-12 补）。

用法：
  python3 .githooks/leak_scan.py --staged     # 扫暂存区（pre-commit）
  python3 .githooks/leak_scan.py --all        # 扫全部跟踪文件（pre-push）
  python3 .githooks/leak_scan.py <file>...    # 扫指定文件
"""
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PATTERNS = os.path.join(HERE, 'patterns.sha256')
MAX_BYTES = 10 * 1024 * 1024
BAD_NAME_RES = [re.compile(p) for p in (r'\.bak', r'-bak-', r'__pycache__', r'\.pyc$', r'~$')]

# 形态检查（明文 regex；形态本身非敏感，无需哈希化）
FORM_RES = [
    (re.compile(r'(?<![0-9A-Fa-f:])([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:])'), 'MAC'),
    (re.compile(r'[Mm][Aa][Cc][:=]([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}'), 'MAC(前缀)'),
    (re.compile(r'(?<![0-9A-Fa-f:])([0-9A-Fa-f]{2}:){6}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:])'), 'MAC(7组)'),
    (re.compile(r'(?<![0-9.])100\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\.\d{1,3}\.\d{1,3}'), 'tailscale'),
    (re.compile(r'(?<![A-Za-z0-9_])bubu(?![A-Za-z0-9_])'), 'SSID'),
    (re.compile(r'ssh-(rsa|ed25519) AAAA[0-9A-Za-z+/]{40,}'), 'SSH公钥'),
]


def load_patterns():
    hashes, lengths = {}, []
    with open(PATTERNS, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith('# lens='):
                lengths = [int(x) for x in line.split('=', 1)[1].split(',') if x.strip()]
                continue
            if line.startswith('#'):
                continue
            h, _, flags = line.partition(':')
            hashes[h] = int(flags or 0)
    return hashes, sorted(lengths)


def norm(s):
    s = s.lower()
    s = re.sub(r'[^a-z0-9\u4e00-\u9fff]+', ' ', s)
    return s.strip()


def read_text(path):
    try:
        b = open(path, 'rb').read()
    except OSError:
        return None
    if len(b) > MAX_BYTES or b'\x00' in b[:65536]:
        return None
    for enc in ('utf-8', 'gbk'):
        try:
            return b.decode(enc)
        except UnicodeDecodeError:
            continue
    return None


def boundary_ok(n, i, L, flags):
    if flags & 1 and i > 0 and n[i - 1].isascii() and n[i - 1].isalnum():
        return False
    if flags & 2:
        j = i + L
        if j < len(n) and n[j].isascii() and n[j].isalnum():
            return False
    return True


def scan_file(path, hashes, lengths):
    text = read_text(path)
    if text is None:
        return []
    n = norm(text)
    hits = []
    for i in range(len(n)):
        for L in lengths:
            j = i + L
            if j > len(n):
                break
            h = hashlib.sha256(n[i:j].encode()).hexdigest()[:16]
            if h in hashes and boundary_ok(n, i, L, hashes[h]):
                hits.append((i, h))
    return hits


def git(*args):
    return subprocess.run(['git'] + list(args), capture_output=True, text=True).stdout.split('\n')


def main():
    args = sys.argv[1:]
    if not os.path.exists(PATTERNS):
        print('leak-scan: 缺少 patterns.sha256 —— 拒绝提交（fail-closed）', file=sys.stderr)
        return 1
    hashes, lengths = load_patterns()

    if '--staged' in args:
        files = [f for f in git('diff', '--cached', '--name-only', '--diff-filter=ACM') if f]
    elif '--all' in args:
        files = [f for f in git('ls-files') if f]
    else:
        files = args
    # .githooks/ 是守护工具自身（源码含规则字面量，每次导出整体覆盖）——不参与内容扫描
    files = [f for f in files if not f.startswith('.githooks/')]

    problems = []
    for f in files:
        if not os.path.isfile(f):
            continue
        for rx in BAD_NAME_RES:
            if rx.search(f):
                problems.append(f'[命名] {f}')
        try:
            if os.path.getsize(f) > MAX_BYTES:
                problems.append(f'[超大] {f}')
                continue
        except OSError:
            continue
        for pos, h in scan_file(f, hashes, lengths):
            problems.append(f'[受保护模式] {f} @{pos} (hash {h})')
        text = read_text(f)
        if text is not None:
            for rx, label in FORM_RES:
                if rx.search(text):
                    problems.append(f'[形态] {f} :: {label}')

    if problems:
        print('leak-scan 拦截（疑似受保护内容；请对照维护者侧规则表核对命中原位）：', file=sys.stderr)
        for p in sorted(set(problems)):
            print('  ✗ ' + p, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
