import os, subprocess
D = r"D:\projects\clux\build\tmp\matrix"
CLUX = r"D:\projects\clux\build\clux.exe"
os.makedirs(D, exist_ok=True)

# 族：defs / 类型名 / 构造式 / **闭包体内返回的整数表达式**（值语义校验）
FAM = {
 "struct": ("struct T { p: own *i32; n: i32; }", "T",
            ".T{ .p = new i32{7}, .n = 3 }", "{0}.n"),
 "nested": ("struct I { p: own *i32; }\nstruct T { inner: I; n: i32; }", "T",
            ".T{ .inner = .I{ .p = new i32{7} }, .n = 3 }", "{0}.n"),
 "deep":   ("struct I { p: own *i32; }\nstruct M { i: I; n: i32; }\nstruct T { m: M; n: i32; }", "T",
            ".T{ .m = .M{ .i = .I{ .p = new i32{7} }, .n = 1 }, .n = 3 }", "{0}.m.n"),
 "tuple":  ("", "<own *i32, i32>",
            ".<own *i32, i32>{ new i32{7}, 3 }", "{0}[1]"),
 "array":  ("struct I { p: own *i32; }", "[2]I",
            ".[2]I{ .I{ .p = new i32{7} }, .I{ .p = new i32{8} } }", "7"),
 "union":  ("union T { p: own *i32; n: i32 }", "T",
            ".T{ .p = new i32{7} }", "7"),
 "optinner": ("struct I { p: own *i32; }", "?I",
            ".?I{ .I{ .p = new i32{7} } }", "7"),
}

OPS = {
 "C1_capture_direct":
   "  var f = func |a| get(): i32 {{ return {E}; }};\n  if (f() != 3) {{ return 1; }}\n  return 0;",
 "C2_capture_paren_move":
   "  var f = func |(bb: {T} = move(a))| get(): i32 {{ return {E2}; }};\n  if (f() != 3) {{ return 1; }}\n  return 0;",
 "C3_capture_then_discard":
   "  var f = func |a| get(): i32 {{ return {E}; }};\n  if (f() != 3) {{ return 1; }}\n  _ = move(a);\n  return 0;",
 "C4_capture_then_clone":
   "  var f = func |a| get(): i32 {{ return {E}; }};\n  var c: {T} = clone(a);\n  if (f() != 3) {{ return 1; }}\n  _ = move(c);\n  return 0;",
 "C5_two_captures":
   "  var b: {T} = {C};\n  var f = func |a, b| get(): i32 {{ return {E}; }};\n  if (f() != 3) {{ return 1; }}\n  return 0;",
 "C6_defer_capture":
   "  defer |a| {{ }};\n  return 0;",
 "C7_capture_in_block":
   "  var r: i32 = 0;\n  {{\n    var f = func |a| get(): i32 {{ return {E}; }};\n    r = f();\n  }}\n  if (r != 3) {{ return 1; }}\n  return 0;",
}

made = 0
fails = []
for fn, (defs, T, C, E) in FAM.items():
    for on, body in OPS.items():
        e = E.replace("{0}", "a")
        e2 = E.replace("{0}", "bb")
        src = body.format(T=T, C=C, E=e, E2=e2)
        text = (defs + "\n" if defs else "") + \
               "func main(): i32 {\n  var a: %s = %s;\n%s\n}\n" % (T, C, src)
        p = os.path.join(D, "%s__%s.cx" % (fn, on))
        open(p, "w", encoding="utf-8").write(text)
        made += 1

print("generated %d closure-capture cells\n" % made)
for fn in FAM:
    row = []
    for on in OPS:
        p = os.path.join(D, "%s__%s.cx" % (fn, on))
        r = subprocess.run([CLUX, "run", p], capture_output=True, text=True,
                           timeout=25, cwd=r"D:\projects\clux\build")
        err = (r.stderr or "")
        leaks = err.lower().count("leak:")
        diag = [l.strip() for l in err.splitlines() if "error" in l.lower()]
        if diag:
            tag = "DIAG"
        elif leaks:
            tag = "LEAK"
        elif r.returncode != 0:
            tag = "rc=%d" % r.returncode
        else:
            tag = "ok"
        row.append(tag)
        if tag != "ok":
            fails.append((fn, on, tag, (diag[0][:90] if diag else "")))
    print("  %-9s %s" % (fn, "  ".join("%-6s" % x for x in row)))
print("\nops:", "  ".join("%-6s" % o.split("_")[0] for o in OPS))
print("\nnon-ok cells: %d" % len(fails))
for fn, on, tag, msg in fails:
    print("  %-9s %-24s %-6s %s" % (fn, on, tag, msg))
