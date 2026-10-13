#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""跑完所有权矩阵探针，按 (操作 × 族) 汇总 exit code。

分类：
  0        OK
  1        编译/语义拒绝（可能是**正确**的拒绝，需人工判定）
  0xC0000005 访问违例（段错误）
  0xC0000409 栈缓冲溢出 / fail-fast
  TIMEOUT  挂起（可能是析构死循环）
"""
import os
import subprocess
import sys
from collections import defaultdict

DIR = r"D:\projects\clux\build\tmp\matrix"
CLUX = r"D:\projects\clux\build\clux.exe"

CRASH = {
    0xC0000005: "ACCESS_VIOLATION",
    0xC0000409: "STACK_BUFFER_OVERRUN",
    0xC0000374: "HEAP_CORRUPTION",
    0xC00000FD: "STACK_OVERFLOW",
    0x80000003: "BREAKPOINT",
}

results = {}
for name in sorted(os.listdir(DIR)):
    if not name.endswith(".cx"):
        continue
    path = os.path.join(DIR, name)
    try:
        p = subprocess.run([CLUX, "run", path], capture_output=True, text=True,
                           timeout=20, cwd=r"D:\projects\clux")
        rc = p.returncode
        err = (p.stderr or "").strip()
    except subprocess.TimeoutExpired:
        rc = "TIMEOUT"
        err = ""
    fam, op = name[:-3].split("__", 1)
    results[(op, fam)] = (rc, err)

# 汇总表
fams = sorted({f for (_, f) in results})
ops = sorted({o for (o, _) in results})
print("=== 矩阵结果（行=操作，列=族）===")
header = "%-22s" % "op" + "".join("%-11s" % f for f in fams)
print(header)
bad = []
for op in ops:
    row = "%-22s" % op
    for f in fams:
        rc, err = results.get((op, f), ("-", ""))
        if rc == 0:
            cell = "ok"
        elif rc == 1:
            cell = "REJECT"
        elif rc == "TIMEOUT":
            cell = "TIMEOUT"
        elif isinstance(rc, int) and rc in CRASH:
            cell = CRASH[rc]
        else:
            cell = "rc=%s" % rc
        row += "%-11s" % cell
        if cell not in ("ok", "REJECT"):
            bad.append((op, f, rc, err))
    print(row)

print()
print("=== 非 ok / 非 REJECT（真正的坏格）: %d ===" % len(bad))
for op, f, rc, err in bad:
    print("  %s / %s -> %s" % (op, f, rc))
    if err:
        print("      %s" % err.splitlines()[0][:160])

# REJECT 明细（需人工判定是否"正确的拒绝"）
print()
print("=== REJECT 明细 ===")
for op in ops:
    cells = [(f, results[(op, f)][1]) for f in fams
             if results.get((op, f), (None,))[0] == 1]
    if cells:
        print("  %s:" % op)
        for f, err in cells:
            msg = ""
            for line in err.splitlines():
                if "error:" in line:
                    msg = line.split("error:", 1)[1].strip()
                    break
            print("    %-10s %s" % (f, msg[:130]))
