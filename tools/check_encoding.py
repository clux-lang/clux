#!/usr/bin/env python3
"""校验仓库文本文件的编码与 BOM。

**目的**：本项目源码/文档统一为 **UTF-8（无 BOM）**。历史上曾有编辑器按 GBK
保存文件、也出现过带 BOM 的文件，导致中文注释在别的工具里变成乱码。本脚本把
这类问题前置暴露，避免静默混入。

用法：
    python tools/check_encoding.py            # 检查，有问题则退出码 1
    python tools/check_encoding.py --list     # 同时列出通过检查的文件数

规则：
  - 文本文件必须能按 UTF-8 解码（**不允许 GBK**）；
  - 源码与文档**不允许 UTF-8 BOM**（BOM 会让部分工具/编译器行为不一致）；
  - `third_party/` 下豁免（vendored 代码，MSVC 工程的 .sln/.vcxproj 需要 BOM 才
    能正确识别 UTF-8）；
  - 二进制文件（含 NUL 字节）跳过。

退出码：0 = 全部合规；1 = 存在违规（逐条打印）。
"""

from __future__ import annotations

import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_DIRS = {".git", "build", ".cache", "node_modules", ".vs", "out"}
# vendored 代码不参与（其编码由上游决定；MSVC 工程文件必须有 BOM）
EXEMPT_PREFIXES = ("third_party/",)
BOM = b"\xef\xbb\xbf"


def is_binary(data: bytes) -> bool:
    return b"\x00" in data[:8192]


def iter_files():
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
            if rel.startswith(EXEMPT_PREFIXES):
                continue
            yield path, rel


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list", action="store_true", help="打印通过检查的文件数")
    args = ap.parse_args()

    bad_gbk: list[str] = []
    bad_utf8: list[str] = []
    bad_bom: list[str] = []
    checked = 0

    for path, rel in iter_files():
        try:
            data = open(path, "rb").read()
        except OSError as exc:                      # 权限/竞态：报告但不中断
            bad_utf8.append(f"{rel}  (读取失败: {exc})")
            continue
        if is_binary(data):
            continue
        checked += 1

        if data.startswith(BOM):
            bad_bom.append(rel)

        try:
            data.decode("utf-8")
        except UnicodeDecodeError as exc:
            # 能按 GBK 解开 → 明确指出是 GBK，便于修复
            try:
                data.decode("gbk")
                bad_gbk.append(f"{rel}  (UTF-8 第 {exc.start} 字节处失败，可按 GBK 解码)")
            except UnicodeDecodeError:
                bad_utf8.append(f"{rel}  (第 {exc.start} 字节处非法: {exc.reason})")

    problems = 0
    if bad_gbk:
        print(f"[FAIL] {len(bad_gbk)} 个文件是 GBK（请转存为 UTF-8 无 BOM）:")
        for line in bad_gbk:
            print("   " + line)
        problems += len(bad_gbk)
    if bad_utf8:
        print(f"[FAIL] {len(bad_utf8)} 个文件不是合法 UTF-8:")
        for line in bad_utf8:
            print("   " + line)
        problems += len(bad_utf8)
    if bad_bom:
        print(f"[FAIL] {len(bad_bom)} 个文件带 UTF-8 BOM（应去掉）:")
        for line in bad_bom:
            print("   " + line)
        problems += len(bad_bom)

    if problems:
        print(f"\n合计 {problems} 处编码问题。")
        return 1
    if args.list:
        print(f"OK: {checked} 个文本文件全部为 UTF-8")
    else:
        print("OK: 编码检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
