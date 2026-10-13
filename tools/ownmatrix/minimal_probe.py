import os, subprocess
D = r"D:\projects\clux\build\tmp\clos"
CLUX = r"D:\projects\clux\build\clux.exe"

# 最小对照：同一程序，逐步把"捕获的负载"换成平凡类型，看泄漏在哪一步出现
cases = {
 # 0) 不捕获（基线）：只是构造 + 使用，不涉及闭包
 "m0_no_capture": """
struct Box { p: own *i32; n: i32; }
func main(): i32 {
  var b: Box = .Box{ .p = new i32{7}, .n = 3 };
  return b.n - 3;
}
""",
 # 1) 捕获平凡标量（基线：闭包机制本身）
 "m1_capture_scalar": """
func main(): i32 {
  var n: i32 = 3;
  var f = func |n| add(x: i32): i32 { return n + x; };
  return f(1) - 4;
}
""",
 # 2) 捕获平凡 struct（无 own）
 "m2_capture_plain_struct": """
struct P { x: i32; y: i32; }
func main(): i32 {
  var s: P = .P{ .x = 3, .y = 4 };
  var f = func |s| add(z: i32): i32 { return s.x + z; };
  return f(1) - 4;
}
""",
 # 3) 构造含 own 的 struct 但**不捕获**，直接丢弃（对照 c1 的构造部分）
 "m3_ctor_only": """
struct Box { p: own *i32; n: i32; }
func main(): i32 {
  _ = .Box{ .p = new i32{7}, .n = 3 };
  return 0;
}
""",
 # 4) 捕获含 own 的 struct（问题格）
 "m4_capture_own_struct": """
struct Box { p: own *i32; n: i32; }
func main(): i32 {
  var b: Box = .Box{ .p = new i32{7}, .n = 3 };
  var f = func |b| add(x: i32): i32 { return b.n + x; };
  return f(1) - 4;
}
""",
 # 5) 含 own 的 struct 只做 clone（不闭包）——对照 clone 路径
 "m5_clone_only": """
struct Box { p: own *i32; n: i32; }
func main(): i32 {
  var b: Box = .Box{ .p = new i32{7}, .n = 3 };
  var c: Box = clone(b);
  return c.n - 3;
}
""",
}

for k, src in cases.items():
    p = os.path.join(D, k + ".cx")
    open(p, "w", encoding="utf-8").write(src)
    r = subprocess.run([CLUX, "run", p], capture_output=True, text=True, timeout=25,
                       cwd=r"D:\projects\clux\build")
    err = [l.strip() for l in (r.stderr or "").splitlines() if l.strip()]
    diag = [l for l in err if "error" in l.lower()]
    leaks = [l for l in err if "leak:" in l.lower()]
    tag = ("DIAG: " + diag[0][:60]) if diag else ("LEAK x%d" % len(leaks) if leaks else "clean")
    print("%-26s rc=%-12s %s" % (k, r.returncode, tag))
