#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""所有权矩阵探针生成器：类型族 × 操作，每格一个文件、只放一个操作。

判定：唯一失败信号是**非 0 exit code 或崩溃码**（0xC0000005 / 0xC0000409）。
"""
import os
import shutil

DIR = r"D:\projects\clux\build\tmp\matrix"
shutil.rmtree(DIR, ignore_errors=True)
os.makedirs(DIR, exist_ok=True)

# 族：defs / 类型名 / 构造式 / 断言模板（{0}=变量）/ take() 期望返回码
FAM = [
    dict(n="struct", defs="struct T { p: own *i32; n: i32; }", t="T",
         ctor=".T{ .p = new i32{7}, .n = 3 }",
         read="{0}.p.* == 7 && {0}.n == 3"),
    dict(n="nested", defs="struct I { p: own *i32; }\nstruct T { inner: I; n: i32; }", t="T",
         ctor=".T{ .inner = .I{ .p = new i32{7} }, .n = 3 }",
         read="{0}.inner.p.* == 7 && {0}.n == 3"),
    dict(n="deep", defs="struct I { p: own *i32; }\nstruct M { i: I; n: i32; }\nstruct T { m: M; n: i32; }", t="T",
         ctor=".T{ .m = .M{ .i = .I{ .p = new i32{7} }, .n = 1 }, .n = 3 }",
         read="{0}.m.i.p.* == 7 && {0}.m.n == 1 && {0}.n == 3"),
    dict(n="tuple", defs="", t="<own *i32, i32>",
         ctor=".<own *i32, i32>{ new i32{7}, 3 }",
         read="{0}[0].* == 7 && {0}[1] == 3"),
    dict(n="array", defs="struct I { p: own *i32; }", t="[2]I",
         ctor=".[2]I{ .I{ .p = new i32{7} }, .I{ .p = new i32{8} } }",
         read="{0}[0].p.* == 7 && {0}[1].p.* == 8"),
    dict(n="union", defs="union T { p: own *i32; n: i32 }", t="T",
         ctor=".T{ .p = new i32{7} }",
         read="{0}.p.* == 7"),
    dict(n="optinner", defs="struct I { p: own *i32; }", t="?I",
         ctor=".?I{ .I{ .p = new i32{7} } }",
         read="true"),
]

made = 0


def w(fam, op, body, helpers=""):
    global made
    parts = [fam["defs"], helpers, "func main(): i32 {", body, "}"]
    src = "\n".join(p for p in parts if p) + "\n"
    path = os.path.join(DIR, "%s__%s.cx" % (fam["n"], op))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(src)
    made += 1


for f in FAM:
    T, C = f["t"], f["ctor"]
    rd = lambda v: f["read"].replace("{0}", v)

    w(f, "01_ctor_explicit", "  var a: %s = %s;\n  if (!(%s)) { return 1; }\n  return 0;" % (T, C, rd("a")))
    w(f, "02_ctor_infer",    "  var a = %s;\n  if (!(%s)) { return 1; }\n  return 0;" % (C, rd("a")))
    w(f, "03_move_local",    "  var a: %s = %s;\n  var b: %s = move(a);\n  if (!(%s)) { return 1; }\n  return 0;" % (T, C, T, rd("b")))
    w(f, "04_clone_local",   "  var a: %s = %s;\n  var b: %s = clone(a);\n  if (!(%s)) { return 1; }\n  if (!(%s)) { return 2; }\n  return 0;" % (T, C, T, rd("a"), rd("b")))
    w(f, "05_move_discard",  "  var a: %s = %s;\n  _ = move(a);\n  return 0;" % (T, C))
    w(f, "06_discard_ctor",  "  _ = %s;\n  return 0;" % C)
    w(f, "07_scope_exit",    "  {\n    var a: %s = %s;\n    if (!(%s)) { return 1; }\n  }\n  return 0;" % (T, C, rd("a")))
    w(f, "08_assign_move",   "  var a: %s = %s;\n  var b: %s = %s;\n  b = move(a);\n  if (!(%s)) { return 1; }\n  return 0;" % (T, C, T, C, rd("b")))
    w(f, "09_assign_clone",  "  var a: %s = %s;\n  var b: %s = %s;\n  b = clone(a);\n  if (!(%s)) { return 1; }\n  if (!(%s)) { return 2; }\n  return 0;" % (T, C, T, C, rd("a"), rd("b")))
    w(f, "10_elem_move",     "  var a: %s = %s;\n  var g: [1]%s = .[1]%s{ move(a) };\n  if (!(%s)) { return 1; }\n  return 0;" % (T, C, T, T, rd("g[0]")))
    w(f, "11_elem_clone",    "  var a: %s = %s;\n  var g: [1]%s = .[1]%s{ clone(a) };\n  if (!(%s)) { return 1; }\n  if (!(%s)) { return 2; }\n  return 0;" % (T, C, T, T, rd("g[0]"), rd("a")))
    w(f, "16_double_move",   "  var a: %s = %s;\n  var b: %s = move(a);\n  var c: %s = move(b);\n  if (!(%s)) { return 1; }\n  return 0;" % (T, C, T, T, rd("c")))
    w(f, "17_move_clone",    "  var a: %s = %s;\n  var b: %s = move(a);\n  var c: %s = clone(b);\n  if (!(%s)) { return 1; }\n  if (!(%s)) { return 2; }\n  return 0;" % (T, C, T, T, rd("b"), rd("c")))

    helpers = (
        "func take(x: fatal %s): i32 {\n"
        "  var o: %s = move(x);\n"
        "  if (!(%s)) { return 1; }\n"
        "  return 0;\n"
        "}\n"
        "func mkone(): fatal %s {\n"
        "  return move(%s);\n"
        "}\n"
        "func fwd(x: fatal %s): fatal %s {\n"
        "  return x;\n"
        "}\n" % (T, T, rd("o"), T, C, T, T)
    )
    w(f, "12_fatal_param",  "  if (take(%s) != 0) { return 1; }\n  return 0;" % C, helpers)
    w(f, "13_fatal_return", "  var o: %s = mkone();\n  if (!(%s)) { return 1; }\n  return 0;" % (T, rd("o")), helpers)
    w(f, "14_param_move",   "  var j: %s = %s;\n  if (take(move(j)) != 0) { return 1; }\n  return 0;" % (T, C), helpers)
    w(f, "15_fatal_forward", "  var o: %s = fwd(%s);\n  if (!(%s)) { return 1; }\n  return 0;" % (T, C, rd("o")), helpers)

print("generated %d files in %s" % (made, DIR))
