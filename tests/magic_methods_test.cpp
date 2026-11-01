#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

/* ============================================================================
 * 魔法方法（P0）：运算符 / 下标 / len 的派发
 *
 * 规则（docs/whitepaper/03-declarations.md）：
 *  - 方法 = 类型命名空间里的函数；`a op b` 展开成 `typeof(a)::__op__(a.&, b)`；
 *  - 只派发**前导操作数**（lhs），内建类型优先；
 *  - 接收者是结构体值 → 传 `a.&`；值形参会在调用点自然报错。
 * ========================================================================== */

TEST(Magic, ArithmeticAndBitwiseOperators) {
  clux_expect_program_ok(
      "struct N {\n"
      "  v: i32;\n"
      "  func __add__(self: *const N, o: *const N): N { return .N{ .v = self.v + o.v }; }\n"
      "  func __sub__(self: *const N, o: *const N): N { return .N{ .v = self.v - o.v }; }\n"
      "  func __mul__(self: *const N, o: *const N): N { return .N{ .v = self.v * o.v }; }\n"
      "  func __div__(self: *const N, o: *const N): N { return .N{ .v = self.v / o.v }; }\n"
      "  func __mod__(self: *const N, o: *const N): N { return .N{ .v = self.v % o.v }; }\n"
      "  func __and__(self: *const N, o: *const N): N { return .N{ .v = self.v & o.v }; }\n"
      "  func __or__(self: *const N, o: *const N): N { return .N{ .v = self.v | o.v }; }\n"
      "  func __xor__(self: *const N, o: *const N): N { return .N{ .v = self.v ^ o.v }; }\n"
      "  func __shl__(self: *const N, o: *const N): N { return .N{ .v = self.v << o.v }; }\n"
      "  func __shr__(self: *const N, o: *const N): N { return .N{ .v = self.v >> o.v }; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: N = .N{ .v = 7 };\n"
      "  var b: N = .N{ .v = 3 };\n"
      "  if ((a + b).v != 10) { return 1; }\n"
      "  if ((a - b).v != 4) { return 2; }\n"
      "  if ((a * b).v != 21) { return 3; }\n"
      "  if ((a / b).v != 2) { return 4; }\n"
      "  if ((a % b).v != 1) { return 5; }\n"
      "  if ((a & b).v != 3) { return 6; }\n"
      "  if ((a | b).v != 7) { return 7; }\n"
      "  if ((a ^ b).v != 4) { return 8; }\n"
      "  if ((a << b).v != 56) { return 9; }\n"
      "  if ((a >> b).v != 0) { return 10; }\n"
      "  return 0;\n"
      "}\n",
      "magic_arith");
}

TEST(Magic, UnaryOperators) {
  clux_expect_program_ok(
      "struct N {\n"
      "  v: i32;\n"
      "  func __neg__(self: *const N): N { return .N{ .v = 0 - self.v }; }\n"
      "  func __not__(self: *const N): bool { return self.v == 0; }\n"
      "  func __bnot__(self: *const N): N { return .N{ .v = ~self.v }; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: N = .N{ .v = 5 };\n"
      "  if ((-a).v != 0 - 5) { return 1; }\n"
      "  if ((-a).v + 105 != 100) { return 2; }\n"
      "  if (!a) { return 3; }\n"
      "  var z: N = .N{ .v = 0 };\n"
      "  if (!z != true) { return 4; }\n"
      "  if ((~a).v != ~5) { return 5; }\n"
      "  return 0;\n"
      "}\n",
      "magic_unary");
}

TEST(Magic, ComparisonOperators) {
  clux_expect_program_ok(
      "struct N {\n"
      "  v: i32;\n"
      "  func __eq__(self: *const N, o: *const N): bool { return self.v == o.v; }\n"
      "  func __lt__(self: *const N, o: *const N): bool { return self.v < o.v; }\n"
      "  func __le__(self: *const N, o: *const N): bool { return self.v <= o.v; }\n"
      "  func __gt__(self: *const N, o: *const N): bool { return self.v > o.v; }\n"
      "  func __ge__(self: *const N, o: *const N): bool { return self.v >= o.v; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: N = .N{ .v = 2 };\n"
      "  var b: N = .N{ .v = 3 };\n"
      "  var c: N = .N{ .v = 2 };\n"
      "  if (a == c != true) { return 1; }\n"
      "  if (a != b != true) { return 2; }\n"
      "  if (a < b != true) { return 3; }\n"
      "  if (a <= c != true) { return 4; }\n"
      "  if (b > a != true) { return 5; }\n"
      "  if (b >= a != true) { return 6; }\n"
      "  if (a > b) { return 7; }\n"
      "  return 0;\n"
      "}\n",
      "magic_cmp");
}

TEST(Magic, ComparisonUsesLhsOnly) {
  /* 只派发前导操作数：lhs 没有魔法方法、rhs 有，也不派发。 */
  clux_expect_reject(
      "struct P { v: i32; func __add__(self: *const P, o: *const P): P { return o.*; } }\n"
      "struct Q { v: i32; }\n"
      "func main(): i32 {\n"
      "  var q: Q = .Q{ .v = 1 };\n"
      "  var p: P = .P{ .v = 2 };\n"
      "  return (q + p).v;\n"
      "}\n",
      "cannot apply '+'", "magic_lhs_only");
}

TEST(Magic, IndexReadWrite) {
  clux_expect_program_ok(
      "struct Buf {\n"
      "  items: []i32;\n"
      "  func __index__(self: *Buf, i: i32): *i32 { return self.items[i].&; }\n"
      "  func __len__(self: *const Buf): usize { return len(self.items); }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var raw: [4]i32 = .[4]i32{ 7, 8, 9, 10 };\n"
      "  var b: Buf = .Buf{ .items = raw[0:4] };\n"
      "  if (b[1] != 8) { return 1; }\n"
      "  b[1] = 20;\n"
      "  if (b[1] != 20) { return 2; }\n"
      "  if (raw[1] != 20) { return 3; }\n"   /* 写穿到原存储 */
      "  b[2] += 5;\n"
      "  if (b[2] != 14) { return 4; }\n"
      "  if (len(b) != 4 as usize) { return 5; }\n"
      "  if (b[3] != 10) { return 6; }\n"
      "  return 0;\n"
      "}\n",
      "magic_index");
}

TEST(Magic, IndexReadOnlyViaConstPointer) {
  /* 只读下标：`__index__` 返回 `*const i32` → `b[0] = v` 被拒绝。 */
  clux_expect_reject(
      "struct Buf {\n"
      "  items: []i32;\n"
      "  func __index__(self: *const Buf, i: i32): *const i32 { return self.items[i].&; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var raw: [2]i32 = .[2]i32{ 1, 2 };\n"
      "  var b: Buf = .Buf{ .items = raw[0:2] };\n"
      "  b[0] = 9;\n"
      "  return 0;\n"
      "}\n",
      "const", "magic_index_const");
}

TEST(Magic, LenAndForeach) {
  /* foreach 展开成 len(a) + a[i]：__index__ 的索引形参是 usize。 */
  clux_expect_program_ok(
      "struct Buf {\n"
      "  items: []i32;\n"
      "  func __index__(self: *const Buf, i: usize): *const i32 { return self.items[i].&; }\n"
      "  func __len__(self: *const Buf): usize { return len(self.items); }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var raw: [3]i32 = .[3]i32{ 4, 5, 6 };\n"
      "  var b: Buf = .Buf{ .items = raw[0:3] };\n"
      "  var sum: i32 = 0;\n"
      "  foreach (var x of b) { sum = sum + x; }\n"
      "  if (sum != 15) { return 1; }\n"
      "  if (b[0 as usize] != 4) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "magic_foreach");
}

TEST(Magic, NoMagicKeepsExistingDiagnostics) {
  clux_expect_reject(
      "struct P { v: i32; }\n"
      "func main(): i32 {\n"
      "  var a: P = .P{ .v = 1 };\n"
      "  var b: P = .P{ .v = 2 };\n"
      "  return (a + b).v;\n"
      "}\n",
      "cannot apply '+'", "magic_missing");
}

TEST(Magic, BuiltinTypesAreNotOverloaded) {
  /* 内建类型优先：i32 的 + 不受任何魔法表影响（结构体同名方法不参与）。 */
  clux_expect_program_ok(
      "struct N { v: i32; func __add__(self: *const N, o: *const N): N { return o.*; } }\n"
      "func main(): i32 {\n"
      "  var x: i32 = 2;\n"
      "  var y: i32 = 3;\n"
      "  if (x + y != 5) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "magic_builtin");
}

TEST(Magic, UnknownMagicNameRejected) {
  clux_expect_reject(
      "struct P { v: i32; func __nope__(self: *const P): i32 { return self.v; } }\n"
      "func main(): i32 { return 0; }\n",
      "unknown magic method '__nope__'", "magic_unknown");
}

TEST(Magic, GenericMagicAdd) {
  /* 魔法方法可以是泛型：clux 没有重载，泛型 + comptime if 承担重载表达。
     派发点像普通泛型方法调用一样按实参推断并实例化（rhs 传 `b.&`）。 */
  clux_expect_program_ok(
      "struct N {\n"
      "  v: i32;\n"
      "  func __add__[U](self: *const N, o: *const U): N {\n"
      "    comptime if (U == N) {\n"
      "      return .N{ .v = self.v + o.v };\n"
      "    } else {\n"
      "      return .N{ .v = self.v + o.* };\n"   /* U = i32：解引用指针 */
      "    }\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: N = .N{ .v = 3 };\n"
      "  var b: N = .N{ .v = 4 };\n"
      "  var c: N = a + b;\n"                    /* U = N */
      "  if (c.v != 7) { return 1; }\n"
      "  var d: N = a + 5;\n"                    /* U = i32 */
      "  if (d.v != 8) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "magic_generic_add");
}

TEST(Magic, GenericMagicEq) {
  clux_expect_program_ok(
      "struct N {\n"
      "  v: i32;\n"
      "  func __eq__[U](self: *const N, o: *const U): bool {\n"
      "    comptime if (U == N) {\n"
      "      return self.v == o.v;\n"
      "    } else {\n"
      "      return self.v == o.*;\n"
      "    }\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: N = .N{ .v = 3 };\n"
      "  var b: N = .N{ .v = 3 };\n"
      "  if (a != b) { return 1; }\n"
      "  if (a == 4) { return 2; }\n"
      "  if (a != 3) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "magic_generic_eq");
}

TEST(Magic, UnknownGenericMagicNameRejected) {
  clux_expect_reject(
      "struct P {\n"
      "  v: i32;\n"
      "  func __nope__[U](self: *const P, o: *const U): bool { return true; }\n"
      "}\n"
      "func main(): i32 { return 0; }\n",
      "unknown magic method '__nope__'", "magic_generic_unknown");
}

TEST(Magic, MagicReturnTypeChecked) {
  clux_expect_reject(
      "struct P {\n"
      "  v: i32;\n"
      "  func __eq__(self: *const P, o: *const P): i32 { return 1; }\n"
      "}\n"
      "func main(): i32 { return 0; }\n",
      "must return bool", "magic_ret_bool");
}

TEST(Magic, IndexReturnTypeMustBeDerefable) {
  clux_expect_reject(
      "struct P {\n"
      "  v: i32;\n"
      "  func __index__(self: *const P, i: i32): i32 { return self.v; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var p: P = .P{ .v = 1 };\n"
      "  return p[0];\n"
      "}\n",
      "must return a pointer or a type with '__deref__'", "magic_index_ret");
}

/* ---------------------------------------------------------------------------
 * `__deref__`：`.` 的读/写/调用回退与 `.*`
 * ------------------------------------------------------------------------- */

TEST(Magic, DerefMemberReadCallWrite) {
  clux_expect_program_ok(
      "struct Inner { x: i32; func inc(self: *const Inner): i32 { return self.x + 1; } }\n"
      "struct W {\n"
      "  p: *Inner;\n"
      "  func __deref__(self: *const W): *Inner { return self.p; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var i: Inner = .Inner{ .x = 41 };\n"
      "  var w: W = .W{ .p = i.& };\n"
      "  if (w.x != 41) { return 1; }\n"       /* 读经回退 */
      "  if (w.inc() != 42) { return 2; }\n"   /* 调用经回退 */
      "  w.x = 50;\n"                          /* 写经回退（链落在指针上） */
      "  if (i.x != 50) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "magic_deref_member");
}

TEST(Magic, DerefChainThroughStructs) {
  /* A ->__deref__ B（B 也有 __deref__）-> *Inner：递归自然展开。 */
  clux_expect_program_ok(
      "struct Inner { x: i32; }\n"
      "struct B { p: *Inner; func __deref__(self: *const B): *Inner { return self.p; } }\n"
      "struct A { b: B; func __deref__(self: *const A): B { return self.b; } }\n"
      "func main(): i32 {\n"
      "  var i: Inner = .Inner{ .x = 7 };\n"
      "  var a: A = .A{ .b = .B{ .p = i.& } };\n"
      "  if (a.x != 7) { return 1; }\n"
      "  a.x = 9;\n"
      "  if (i.x != 9) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "magic_deref_chain");
}

TEST(Magic, DerefStarExpression) {
  clux_expect_program_ok(
      "struct Inner { x: i32; }\n"
      "struct B { p: *Inner; func __deref__(self: *const B): *Inner { return self.p; } }\n"
      "struct A { b: B; func __deref__(self: *const A): B { return self.b; } }\n"
      "func main(): i32 {\n"
      "  var i: Inner = .Inner{ .x = 5 };\n"
      "  var a: A = .A{ .b = .B{ .p = i.& } };\n"
      "  var r: Inner = a.*;\n"                /* 走到指针再解引用 → Inner */
      "  if (r.x != 5) { return 1; }\n"
      "  a.*.x = 8;\n"
      "  if (i.x != 8) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "magic_deref_star");
}

TEST(Magic, DerefDirectMemberWins) {
  /* 直接成员永远优先，不回退。 */
  clux_expect_program_ok(
      "struct Inner { x: i32; }\n"
      "struct W {\n"
      "  x: i32;\n"
      "  p: *Inner;\n"
      "  func __deref__(self: *const W): *Inner { return self.p; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var i: Inner = .Inner{ .x = 1 };\n"
      "  var w: W = .W{ .x = 100, .p = i.& };\n"
      "  if (w.x != 100) { return 1; }\n"      /* 不是里层的 1 */
      "  w.x = 7;\n"                            /* 写自己的字段 */
      "  if (w.x != 7 || i.x != 1) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "magic_deref_direct");
}

TEST(Magic, DerefCycleRejected) {
  clux_expect_reject(
      "struct A {\n"
      "  func __deref__(self: *const A): A { return self.*; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: A = .A{};\n"
      "  return a.x;\n"
      "}\n",
      "'__deref__' cycle: A -> A", "magic_deref_cycle");
}

TEST(Magic, DerefConstPointerRejectsWrite) {
  clux_expect_reject(
      "struct Inner { x: i32; }\n"
      "struct W { p: *Inner; func __deref__(self: *const W): *const Inner { return self.p; } }\n"
      "func main(): i32 {\n"
      "  var i: Inner = .Inner{ .x = 1 };\n"
      "  var w: W = .W{ .p = i.& };\n"
      "  w.x = 5;\n"
      "  return 0;\n"
      "}\n",
      "const", "magic_deref_const");
}

TEST(Magic, DerefInsideComptime) {
  clux_expect_program_ok(
      "struct Inner { x: i32; }\n"
      "struct W { p: *Inner; func __deref__(self: *const W): *Inner { return self.p; } }\n"
      "comptime func f(): i32 {\n"
      "  var i: Inner = .Inner{ .x = 30 };\n"
      "  var w: W = .W{ .p = i.& };\n"
      "  w.x = w.x + 2;\n"
      "  return w.x;\n"                        /* 32 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 32) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "magic_deref_comptime");
}

/* ---------------------------------------------------------------------------
 * 类型级判等：`__eq__` 在聚合内部同样生效（C 后端助手 + CTFE 钩子）
 * ------------------------------------------------------------------------- */

TEST(Magic, EqInNestedStructField) {
  /* S 的字段 a: N 有 __eq__ → S == S 必须调用 N.__eq__，不是逐字节。 */
  clux_expect_program_ok(
      "struct N {\n"
      "  v: i32;\n"
      "  func __eq__(self: *const N, o: *const N): bool { return (self.v % 10) == (o.v % 10); }\n"
      "}\n"
      "struct S { a: N; b: i32; }\n"
      "func main(): i32 {\n"
      "  var x: S = .S{ .a = .N{ .v = 5 }, .b = 7 };\n"
      "  var y: S = .S{ .a = .N{ .v = 15 }, .b = 7 };\n"
      "  if (x == y != true) { return 1; }\n"
      "  var z: S = .S{ .a = .N{ .v = 15 }, .b = 8 };\n"
      "  if (x == z) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "magic_eq_nested");
}

TEST(Magic, EqInUnionPayload) {
  clux_expect_program_ok(
      "struct N {\n"
      "  v: i32;\n"
      "  func __eq__(self: *const N, o: *const N): bool { return (self.v % 10) == (o.v % 10); }\n"
      "}\n"
      "union U { a: N; b: i32; }\n"
      "func main(): i32 {\n"
      "  var x: U = .U{ .a = .N{ .v = 3 } };\n"
      "  var y: U = .U{ .a = .N{ .v = 13 } };\n"
      "  if (x == y != true) { return 1; }\n"
      "  var z: U = .U{ .b = 3 };\n"
      "  if (x == z) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "magic_eq_union");
}

TEST(Magic, EqInComptimeAggregate) {
  /* CTFE 的聚合判等也走 `__eq__`（vm->struct_eq_hook）。 */
  clux_expect_program_ok(
      "struct N {\n"
      "  v: i32;\n"
      "  func __eq__(self: *const N, o: *const N): bool { return (self.v % 10) == (o.v % 10); }\n"
      "}\n"
      "struct S { a: N; b: i32; }\n"
      "comptime func f(): i32 {\n"
      "  var x: S = .S{ .a = .N{ .v = 5 }, .b = 7 };\n"
      "  var y: S = .S{ .a = .N{ .v = 15 }, .b = 7 };\n"
      "  if (x == y != true) { return 1; }\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 0) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "magic_eq_comptime");
}

TEST(Magic, OperatorsInsideComptime) {
  /* 魔法派发在 comptime 函数体里同样生效（改写后就是普通方法调用）。 */
  clux_expect_program_ok(
      "struct N {\n"
      "  v: i32;\n"
      "  func __add__(self: *const N, o: *const N): N { return .N{ .v = self.v + o.v }; }\n"
      "  func __lt__(self: *const N, o: *const N): bool { return self.v < o.v; }\n"
      "}\n"
      "comptime func f(): i32 {\n"
      "  var a: N = .N{ .v = 20 };\n"
      "  var b: N = .N{ .v = 22 };\n"
      "  var c: N = a + b;\n"
      "  if (!(a < b)) { return 1; }\n"
      "  return c.v;\n"                     /* 42 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 42) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "magic_comptime_ops");
}

TEST(Magic, RhsPassedByPointer) {
  /* 二元魔法方法的右操作数按指针传（零拷贝）：右值由编译器物化临时副本
     再取地址。 */
  clux_expect_program_ok(
      "struct N {\n"
      "  v: i32;\n"
      "  func __add__(self: *const N, o: *const N): N {\n"
      "    return .N{ .v = self.v + o.v };\n"
      "  }\n"
      "}\n"
      "func mk(v: i32): N { return .N{ .v = v }; }\n"
      "func main(): i32 {\n"
      "  var a: N = .N{ .v = 3 };\n"
      "  var b: N = .N{ .v = 4 };\n"
      "  var c: N = a + b;\n"
      "  if (c.v != 7) { return 1; }\n"
      "  var d: N = a + mk(5);\n"           /* 右值 → 物化临时再取址 */
      "  if (d.v != 8) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "magic_rhs_ptr");
}

TEST(Magic, GenericEqInsideAggregate) {
  /* 聚合判等由 sema 生成 `__clux_eq_<T>`，函数体里的字段比较是**普通调用
     点**——泛型 `__eq__` 在这里能按实参实例化（不再回退逐字段）。 */
  clux_expect_program_ok(
      "struct N {\n"
      "  v: i32;\n"
      "  func __eq__[U](self: *const N, o: *const U): bool {\n"
      "    comptime if (U == N) { return (self.v % 10) == (o.v % 10); }\n"
      "    else { return false; }\n"
      "  }\n"
      "}\n"
      "struct S { a: N; }\n"
      "func main(): i32 {\n"
      "  var x: S = .S{ .a = .N{ .v = 5 } };\n"
      "  var y: S = .S{ .a = .N{ .v = 15 } };\n"
      "  if (x != y) { return 1; }\n"
      "  var z: S = .S{ .a = .N{ .v = 16 } };\n"
      "  if (x == z) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "magic_generic_eq_aggregate");
}

TEST(Magic, EqInsideOptionalAndArrayFields) {
  /* `?U` / `[N]U` 字段的判等由生成的判等函数展开（optional 走 nil + `.!`，
     数组走 for 循环），叶子上仍调 `U.__eq__`。 */
  clux_expect_program_ok(
      "struct N { v: i32; func __eq__(self: *const N, o: *const N): bool { return self.v == o.v; } }\n"
      "struct S { o: ?N; arr: [2]N; }\n"
      "func main(): i32 {\n"
      "  var x: S = .S{ .o = .?N{ .N{ .v = 1 } }, .arr = .[2]N{ .N{ .v = 2 }, .N{ .v = 3 } } };\n"
      "  var y: S = .S{ .o = .?N{ .N{ .v = 1 } }, .arr = .[2]N{ .N{ .v = 2 }, .N{ .v = 3 } } };\n"
      "  if (x != y) { return 1; }\n"
      "  var z: S = .S{ .o = nil, .arr = .[2]N{ .N{ .v = 2 }, .N{ .v = 3 } } };\n"
      "  if (x == z) { return 2; }\n"
      "  var w: S = .S{ .o = .?N{ .N{ .v = 1 } }, .arr = .[2]N{ .N{ .v = 2 }, .N{ .v = 4 } } };\n"
      "  if (x == w) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "magic_eq_opt_arr");
}

TEST(Magic, RvalueAddressRejectedForUsers) {
  /* 右值物化只服务魔法方法派发；用户写 `(5).&` 仍是错误。 */
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var p: *i32 = (5).&;\n"
      "  return p.*;\n"
      "}\n",
      "cannot take the address of a temporary value", "magic_materialize_user");
}

TEST(Magic, RvalueRhsMaterializedInDispatch) {
  /* 派发处 `a + 5` ⇒ `a.__add__(5.&)`：右值由编译器物化成复合字面量。 */
  clux_expect_program_ok(
      "struct N {\n"
      "  v: i32;\n"
      "  func __add__(self: *const N, o: *const i32): N {\n"  /* 非泛型：rhs 指针 */
      "    return .N{ .v = self.v + o.* };\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: N = .N{ .v = 3 };\n"
      "  var c: N = a + 5;\n"
      "  if (c.v != 8) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "magic_materialize_dispatch");
}

TEST(Magic, UsingCallsDisposeOnScopeExit) {
  /* `using a = e;` ⇒ `var a = e;` + 作用域退出时 `a.__dispose__()`：
     块尾、提前 return 两条路径都要调用，且块内尚未退出时不能调用。 */
  clux_expect_program_ok(
      "var disposed: i32 = 0;\n"
      "struct Res {\n"
      "  id: i32;\n"
      "  func __dispose__(self: *Res): void { disposed = disposed + self.id; }\n"
      "}\n"
      "func check(): i32 { return disposed; }\n"
      "func f(): i32 {\n"
      "  using r = .Res{ .id = 5 };\n"
      "  return 1;\n"
      "}\n"
      "func main(): i32 {\n"
      "  {\n"
      "    using r = .Res{ .id = 7 };\n"
      "    if (check() != 0) { return 1; }\n"
      "  }\n"
      "  if (check() != 7) { return 2; }\n"
      "  var rc: i32 = f();\n"
      "  if (rc != 1) { return 3; }\n"
      "  if (check() != 12) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "magic_using_exit");
}

TEST(Magic, UsingDisposesLifoWithDefer) {
  /* `using` 与 `defer` 共用同一条 LIFO 退出序列：后声明先执行。 */
  clux_expect_program_ok(
      "var trace: i32 = 0;\n"
      "struct R1 { func __dispose__(self: *R1): void { trace = trace * 10 + 1; } }\n"
      "struct R2 { func __dispose__(self: *R2): void { trace = trace * 10 + 2; } }\n"
      "func main(): i32 {\n"
      "  {\n"
      "    using a = .R1{};\n"
      "    defer { trace = trace * 10 + 9; }\n"
      "    using b = .R2{};\n"
      "  }\n"
      "  if (trace != 291) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "magic_using_lifo");
}

TEST(Magic, UsingRequiresDisposeMethod) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  using a = 1;\n"
      "  return a;\n"
      "}\n",
      "has no '__dispose__' method", "magic_using_no_method");
}

TEST(Magic, DisposeReturnTypeChecked) {
  clux_expect_reject(
      "struct R { func __dispose__(self: *R): i32 { return 1; } }\n"
      "func main(): i32 { return 0; }\n",
      "must return void", "magic_dispose_ret");
}

TEST(Magic, AssignDispatchesOnAssignmentOnly) {
  /* `a = b` 派发 `__assign__(self: *T, rhs: *const T)`；**初始化**
     （`var a = b`）不派发——两者必须区分。 */
  clux_expect_program_ok(
      "var hits: i32 = 0;\n"
      "struct V {\n"
      "  x: i32;\n"
      "  func __assign__(self: *V, o: *const V): void { hits = hits + 1; self.x = o.x; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: V = .V{ .x = 1 };\n"
      "  var b: V = .V{ .x = 2 };\n"
      "  if (hits != 0) { return 1; }\n"        /* 初始化不派发 */
      "  a = b;\n"                              /* 赋值派发 */
      "  if (hits != 1) { return 2; }\n"
      "  if (a.x != 2) { return 3; }\n"
      "  a = b;\n"
      "  if (hits != 2) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "magic_assign_init");
}

TEST(Magic, AssignMemberIndexDerefTargets) {
  /* 成员 / 下标 / 解引用 / 复合赋值：目标类型有 `__assign__` 都派发。 */
  clux_expect_program_ok(
      "var hits: i32 = 0;\n"
      "struct V {\n"
      "  x: i32;\n"
      "  func __assign__(self: *V, o: *const V): void { hits = hits + 1; self.x = o.x; }\n"
      "  func __add__(self: *const V, o: *const V): V { return .V{ .x = self.x + o.x }; }\n"
      "}\n"
      "struct Box { v: V; }\n"
      "func main(): i32 {\n"
      "  var s: Box = .Box{ .v = .V{ .x = 1 } };\n"
      "  var b: V = .V{ .x = 5 };\n"
      "  s.v = b;\n"
      "  if (hits != 1 || s.v.x != 5) { return 1; }\n"
      "  var arr: [2]V = .[2]V{ .V{ .x = 0 }, .V{ .x = 0 } };\n"
      "  arr[1] = b;\n"
      "  if (hits != 2 || arr[1].x != 5) { return 2; }\n"
      "  var p: *V = arr[1].&;\n"
      "  p.* = b;\n"
      "  if (hits != 3) { return 3; }\n"
      "  var a: V = .V{ .x = 1 };\n"
      "  a += b;\n"                              /* __add__ + __assign__ */
      "  if (hits != 4 || a.x != 6) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "magic_assign_targets");
}

TEST(Magic, AssignConstReceiverRejected) {
  clux_expect_reject(
      "struct V { x: i32; func __assign__(self: *V, o: *const V): void { self.x = o.x; } }\n"
      "func main(): i32 {\n"
      "  var a: const V = .V{ .x = 1 };\n"
      "  var b: V = .V{ .x = 2 };\n"
      "  a = b;\n"
      "  return 0;\n"
      "}\n",
      "cannot bind", "magic_assign_const");
}

TEST(Magic, AssignTrySugarDispatches) {
  /* `a = b.?` 展开成 `if (b.ok()) { a = b.value(); } else {...}`——then
     分支仍是一次赋值，目标类型有 `__assign__` 时同样派发。 */
  clux_expect_program_ok(
      "var hits: i32 = 0;\n"
      "struct V { x: i32; func __assign__(self: *V, o: *const V): void { hits = hits + 1; self.x = o.x; } }\n"
      "struct R {\n"
      "  good: bool;\n"
      "  val: V;\n"
      "  err: i32;\n"
      "  func ok(self: *const R): bool { return self.good; }\n"
      "  func value(self: *const R): V { return self.val; }\n"
      "  func error(self: *const R): i32 { return self.err; }\n"
      "  func of_error(e: i32): R { return .R{ .good = false, .val = .V{ .x = 0 }, .err = e }; }\n"
      "}\n"
      "func f(r: R): R {\n"
      "  var a: V = .V{ .x = 1 };\n"
      "  a = r.?;\n"
      "  if (a.x != 4) { return R::of_error(1); }\n"
      "  return r;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var r: R = .R{ .good = true, .val = .V{ .x = 4 }, .err = 0 };\n"
      "  var out: R = f(r);\n"
      "  if (hits != 1) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "magic_assign_try");
}

TEST(Magic, InitConstructsThroughInit) {
  /* 有 `__init__` 的 struct：`.T{arg1, arg2}` = 零初始化 + `__init__(args)`；
     字段未赋值的位置保持全 0（不是未定义）。 */
  clux_expect_program_ok(
      "struct V {\n"
      "  x: i32;\n"
      "  y: i32;\n"
      "  z: i32;\n"
      "  func __init__(self: *V, a: i32, b: i32): void {\n"
      "    self.x = a * 10;\n"
      "    self.y = b;\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var v: V = .V{ 3, 4 };\n"
      "  if (v.x != 30 || v.y != 4) { return 1; }\n"
      "  if (v.z != 0) { return 2; }\n"          /* 未赋值字段 = 0 */
      "  return 0;\n"
      "}\n",
      "magic_init_construct");
}

TEST(Magic, InitDisablesNamedConstruction) {
  clux_expect_reject(
      "struct V { a: i32; func __init__(self: *V, x: i32): void { self.a = x; } }\n"
      "func main(): i32 {\n"
      "  var v: V = .V{ .a = 1 };\n"
      "  return 0;\n"
      "}\n",
      "named-field construction is disabled", "magic_init_named");
}

TEST(Magic, InitArityChecked) {
  clux_expect_reject(
      "struct V { a: i32; func __init__(self: *V, x: i32): void { self.a = x; } }\n"
      "func main(): i32 {\n"
      "  var v: V = .V{ 1, 2 };\n"
      "  return 0;\n"
      "}\n",
      "expects 1 arguments", "magic_init_arity");
}

TEST(Magic, InitInsideComptime) {
  clux_expect_program_ok(
      "struct V {\n"
      "  x: i32;\n"
      "  func __init__(self: *V, a: i32): void { self.x = a + 100; }\n"
      "}\n"
      "comptime func f(): i32 {\n"
      "  var v: V = .V{ 5 };\n"
      "  return v.x;\n"
      "}\n"
      "func main(): i32 { return f() - 105; }\n",
      "magic_init_comptime");
}

TEST(Magic, InitImplicitSingleArgConversion) {
  /* 单参 `__init__` 的隐式构造：`var a: T = x`、实参、返回值、字段初始化
     都等价于 `.T{x}`（"传参本身就是构造，返回值也是"）。 */
  clux_expect_program_ok(
      "struct V {\n"
      "  x: i32;\n"
      "  func __init__(self: *V, a: i32): void { self.x = a * 2; }\n"
      "}\n"
      "struct S { v: V; }\n"
      "func take(v: V): i32 { return v.x; }\n"
      "func mk(a: i32): V { return a; }\n"
      "func main(): i32 {\n"
      "  var a: V = 5;\n"                       /* 初始化 */
      "  if (a.x != 10) { return 1; }\n"
      "  if (take(7) != 14) { return 2; }\n"    /* 实参 */
      "  if (mk(4).x != 8) { return 3; }\n"     /* 返回值 */
      "  var s: S = .S{ .v = 5 };\n"            /* 字段初始化 */
      "  if (s.v.x != 10) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "magic_init_implicit");
}

TEST(Magic, InitDoesNotApplyToAssignment) {
  /* 赋值不是构造：`a = 5`（a 已是 V，5 是 i32）仍要求 V 或 __assign__。 */
  clux_expect_reject(
      "struct V { x: i32; func __init__(self: *V, a: i32): void { self.x = a * 2; } }\n"
      "func main(): i32 {\n"
      "  var a: V = .V{ 1 };\n"
      "  a = 5;\n"
      "  return 0;\n"
      "}\n",
      "cannot assign", "magic_init_assign_no");
}

TEST(Magic, DeferValueCaptureMethodCall) {
  /* defer 捕获在体内作为方法接收者时也要走捕获改写（`__ctx.env.<cap>`）：
     纯 id 捕获与括号值捕获两种形态，退出点都真的执行。 */
  clux_expect_program_ok(
      "var trace: i32 = 0;\n"
      "struct Res { id: i32; func touch(self: *Res): void { trace = trace + self.id; } }\n"
      "func main(): i32 {\n"
      "  {\n"
      "    var r: Res = .Res{ .id = 3 };\n"
      "    defer |r| { r.touch(); }\n"
      "    if (trace != 0) { return 1; }\n"
      "  }\n"
      "  if (trace != 3) { return 2; }\n"
      "  {\n"
      "    var s: Res = .Res{ .id = 4 };\n"
      "    defer |(h = s)| { h.touch(); }\n"
      "  }\n"
      "  if (trace != 7) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "magic_defer_capture");
}

TEST(Magic, RhsByValueRejected) {
  clux_expect_reject(
      "struct N {\n"
      "  v: i32;\n"
      "  func __add__(self: *const N, o: N): N { return o; }\n"
      "}\n"
      "func main(): i32 { return 0; }\n",
      "must take its right-hand operand by pointer", "magic_rhs_value");
}

TEST(Magic, DerefDepthOverrideApi) {
  /* `--magic-deref-depth=N` 的驱动层入口：覆盖生效，-1 清除覆盖。 */
  std::string path = write_temp_file(
      "struct Inner { x: i32; }\n"
      "struct B { p: *Inner; func __deref__(self: *const B): *Inner { return self.p; } }\n"
      "struct A { b: B; func __deref__(self: *const A): B { return self.b; } }\n"
      "func main(): i32 {\n"
      "  var i: Inner = .Inner{ .x = 7 };\n"
      "  var a: A = .A{ .b = .B{ .p = i.& } };\n"
      "  if (a.x != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  driver_set_magic_deref_depth(1);
  EXPECT_EQ(driver_run_file(path.c_str()), 1); /* 两段链超过深度 1 */
  driver_set_magic_deref_depth(-1);
  EXPECT_EQ(driver_run_file(path.c_str()), 0); /* 恢复默认（8）*/
  std::remove(path.c_str());
}

TEST(Magic, ValueReceiverFailsAtCallSite) {
  /* 方法模型：`a.m()` 传 `a.&`；值形参在调用点被普通隐式转换拒绝。 */
  clux_expect_reject(
      "struct P {\n"
      "  v: i32;\n"
      "  func __eq__(self: P, o: *const P): bool { return self.v == o.v; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: P = .P{ .v = 1 };\n"
      "  var b: P = .P{ .v = 1 };\n"
      "  if (a == b) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cannot bind to the 'self' parameter", "magic_value_self");
}

/* `a[lo:hi]` 派发到 __slice__：两个边界实参总是有值（缺省下界 = 0，
   缺省上界 = len(a)），所以省略上界要求实现 __len__。 */
TEST(Magic, SliceDispatchAndDefaults) {
  clux_expect_program_ok(
      "struct Win {\n"
      "  data: [4]i32;\n"
      "  func __len__(self: *Win): usize { return 4usize; }\n"
      "  func __slice__(self: *Win, lo: usize, hi: usize): []i32 {\n"
      "    return self.data[lo:hi];\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var w: Win = .Win{ .data = .[4]i32{ 1, 2, 3, 4 } };\n"
      "  var a: []i32 = w[:];\n"
      "  if (len(a) != 4usize || a[0] != 1 || a[3] != 4) { return 1; }\n"
      "  var m: []i32 = w[1:3];\n"
      "  if (len(m) != 2usize || m[0] != 2 || m[1] != 3) { return 2; }\n"
      "  var t: []i32 = w[2:];\n"
      "  if (len(t) != 2usize || t[0] != 3) { return 3; }\n"
      "  var h: []i32 = w[:2];\n"
      "  if (len(h) != 2usize || h[1] != 2) { return 4; }\n"
      "  return 0;\n"
      "}\n", "magic_slice_dispatch");
}

/* 省略上界但没有 __len__：无法补出 hi，明确诊断。 */
TEST(Magic, SliceOmittedUpperBoundNeedsLen) {
  clux_expect_reject(
      "struct W2 {\n"
      "  data: [2]i32;\n"
      "  func __slice__(self: *W2, lo: usize, hi: usize): []i32 {\n"
      "    return self.data[lo:hi];\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var w: W2 = .W2{ .data = .[2]i32{ 1, 2 } };\n"
      "  var s: []i32 = w[0:];\n"
      "  return len(s) as i32;\n"
      "}\n", "omitted upper bound", "magic_slice_needs_len");
}

/* 边界形参必须是 usize：否则缺省值（0 / len(a)）无法按声明类型给出。 */
TEST(Magic, SliceBoundsMustBeUsize) {
  clux_expect_reject(
      "struct W3 {\n"
      "  data: [2]i32;\n"
      "  func __slice__(self: *W3, lo: i32, hi: i32): []i32 {\n"
      "    return self.data[0:2];\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var w: W3 = .W3{ .data = .[2]i32{ 1, 2 } };\n"
      "  var s: []i32 = w[0:2];\n"
      "  return 0;\n"
      "}\n", "must take two 'usize' bound parameters", "magic_slice_bounds");
}
/* 单参 __init__ 让类型在**所有**初始化位置都走构造：`var b: W = a;` 不再是
   值拷贝，而是 `W a` → 形参不匹配 → 编译错误。这正是 move-only 类型
   （Own/Fatal）禁止 copy 的机制。 */
TEST(Magic, InitFromSameTypeRejected) {
  clux_expect_reject(
      "struct W {\n"
      "  v: i32;\n"
      "  func __init__(self: *W, n: i32): void { self.v = n; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: W = 1;\n"
      "  var b: W = a;\n"
      "  return b.v;\n"
      "}\n",
      "initialization always goes through __init__", "magic_init_copy_var");
}

TEST(Magic, InitFromSameTypeAsArgumentRejected) {
  clux_expect_reject(
      "struct W {\n"
      "  v: i32;\n"
      "  func __init__(self: *W, n: i32): void { self.v = n; }\n"
      "}\n"
      "func take(w: W): i32 { return w.v; }\n"
      "func main(): i32 {\n"
      "  var a: W = 1;\n"
      "  return take(a);\n"
      "}\n",
      "initialization always goes through __init__", "magic_init_copy_arg");
}

TEST(Magic, InitFromSameTypeAsReturnRejected) {
  clux_expect_reject(
      "struct W {\n"
      "  v: i32;\n"
      "  func __init__(self: *W, n: i32): void { self.v = n; }\n"
      "}\n"
      "func pass(a: W): W { return a; }\n"
      "func main(): i32 {\n"
      "  var a: W = 1;\n"
      "  return pass(a).v;\n"
      "}\n",
      "initialization always goes through __init__", "magic_init_copy_ret");
}

/* 构造函数结果（`.W{...}` / 函数返回的新值）不是"已有值位置"，初始化直接放入，
   不受拦截。 */
TEST(Magic, InitFromConstructStillWorks) {
  clux_expect_program_ok(
      "struct W {\n"
      "  v: i32;\n"
      "  func __init__(self: *W, n: i32): void { self.v = n; }\n"
      "}\n"
      "func make_w(n: i32): W { return .W{ n }; }\n"
      "func main(): i32 {\n"
      "  var a: W = .W{ 3 };\n"
      "  var b: W = make_w(4);\n"
      "  return (a.v + b.v) - 7;\n"
      "}\n",
      "magic_init_construct_ok");
}

/* 编译期所有权移除后，move / clone 不再是关键字：可作为普通标识符与方法名。 */
TEST(Magic, MoveAndCloneAreOrdinaryIdentifiers) {
  clux_expect_program_ok(
      "struct H { p: ?*i32; }\n"
      "struct Holder {\n"
      "  h: H;\n"
      "  func move(self: *Holder): H {\n"
      "    var out: H = self.h;\n"
      "    self.h.p = nil;\n"
      "    return out;\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var move: i32 = 1;      // 变量名\n"
      "  var clone: i32 = 2;     // 变量名\n"
      "  var v: i32 = 3;\n"
      "  var b: Holder = .Holder{ .h = .H{ .p = v.& } };\n"
      "  var h: H = b.move();    // 方法名\n"
      "  if (h.p == nil) { return 1; }\n"
      "  return (move + clone) - 3;\n"
      "}\n",
      "move_and_clone_identifiers");
}
/* 泛型实例化之后本身就是一个类型值：sizeof / alignof 必须接受
   `sizeof(Cell[i32])`、`sizeof(Matrix[i32, 3])`，以及泛型函数体内的
   `sizeof(Cell[T])`。 */
TEST(Magic, SizeofGenericInstance) {
  clux_expect_program_ok(
      "struct Cell[T] { value: ?*T; }\n"
      "struct Matrix[T, N: comptime u64] { data: [N]T; }\n"
      "func cell_size[T](): usize { return sizeof(Cell[T]); }\n"
      "func main(): i32 {\n"
      "  if (sizeof(Matrix[i8, 3]) != sizeof([3]i8)) { return 1; }\n"
      "  if (sizeof(Matrix[i64, 3]) != sizeof([3]i64)) { return 2; }\n"
      "  if (alignof(Matrix[i64, 3]) != alignof(i64)) { return 3; }\n"
      "  if (sizeof(Cell[i32]) == 0usize) { return 4; }\n"
      "  if (cell_size[i64]() != sizeof(Cell[i64])) { return 5; }\n"
      "  return 0;\n"
      "}\n",
      "sizeof_generic_instance");
}
/* `dispose(x)` 语句：x（或 *x 指向的类型）实现 __dispose__ 时调用它，
   没有时编译期静默丢弃（容器 / Own 里的条件析构）。 */
TEST(Magic, DisposeStatementDispatchesOnType) {
  clux_expect_program_ok(
      "var disposed: i32 = 0;\n"
      "struct Res { n: i32; func __dispose__(self: *Res): void { disposed = disposed + 1; } }\n"
      "struct Plain { v: i32; }\n"
      "func main(): i32 {\n"
      "  var r: Res = .Res{ .n = 1 };\n"
      "  dispose(r.&);\n"
      "  if (disposed != 1) { return 1; }\n"
      "  var p: Plain = .Plain{ .v = 2 };\n"
      "  dispose(p.&);\n"
      "  dispose(p);\n"
      "  if (disposed != 1) { return 2; }\n"
      "  var r2: Res = .Res{ .n = 3 };\n"
      "  dispose(r2);\n"
      "  if (disposed != 2) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "dispose_dispatch");
}

/* 泛型容器里 `dispose(self.item)` 按**实例化后的元素类型**决定是否调用。 */
TEST(Magic, DisposeInGenericContainer) {
  clux_expect_program_ok(
      "var disposed: i32 = 0;\n"
      "struct Res { n: i32; func __dispose__(self: *Res): void { disposed = disposed + 1; } }\n"
      "struct Plain { v: i32; }\n"
      "struct Box[T] {\n"
      "  item: T;\n"
      "  func drop(self: *Box[T]): void { dispose(self.item); }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var r: Res = .Res{ .n = 1 };\n"
      "  var b: Box[Res] = .Box[Res]{ .item = r };\n"
      "  b.drop();\n"
      "  if (disposed != 1) { return 1; }\n"
      "  var p: Plain = .Plain{ .v = 2 };\n"
      "  var b2: Box[Plain] = .Box[Plain]{ .item = p };\n"
      "  b2.drop();\n"
      "  if (disposed != 1) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "dispose_generic");
}

/* 构造形参是指针 `*P`、源是 P 的位置表达式：初始化传地址（与 __assign__ 对齐），
   构造里能置空调用方对象 → `var o: Own[T] = f;` 就是移动。 */
TEST(Magic, InitFromPlacePassesAddress) {
  clux_expect_program_ok(
      "struct Fatal[T] {\n"
      "  ptr: ?*T;\n"
      "  func __init__(self: *Fatal[T], p: *T): void { self.ptr = p; }\n"
      "  func take(self: *Fatal[T]): ?*T { var p: ?*T = self.ptr; self.ptr = nil; return p; }\n"
      "  func __assign__(self: *Fatal[T], o: *Fatal[T]): void { self.ptr = o.ptr; o.ptr = nil; }\n"
      "}\n"
      "struct Own[T] {\n"
      "  ptr: ?*T;\n"
      "  func __init__(self: *Own[T], f: *Fatal[T]): void { self.ptr = f.take(); }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var v: i32 = 7;\n"
      "  var f: Fatal[i32] = .Fatal[i32]{ v.& };\n"
      "  var o: Own[i32] = .Own[i32]{ f.& };\n"
      "  if (f.ptr != nil) { return 1; }\n"
      "  if (o.ptr == nil || o.ptr.!.* != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "init_place_address");
}
/* 同类型初始化（var / 实参 / 返回值 / 构造）走 `__init_copy__`：形参是 `*T`，
   构造里可以移走源对象，所以"参数传递 = 构造"不会自递归。 */
TEST(Magic, InitCopyMovesAtAllPositions) {
  clux_expect_program_ok(
      "struct Box[T] {\n"
      "  ptr: ?*T;\n"
      "  func __init__(self: *Box[T], p: *T): void { self.ptr = p; }\n"
      "  func __init_copy__(self: *Box[T], o: *Box[T]): void {\n"
      "    self.ptr = o.take();\n"
      "  }\n"
      "  func take(self: *Box[T]): ?*T {\n"
      "    var p: ?*T = self.ptr;\n"
      "    self.ptr = nil;\n"
      "    return p;\n"
      "  }\n"
      "  func __assign__(self: *Box[T], o: *Box[T]): void {\n"
      "    self.ptr = o.ptr;\n"
      "    o.ptr = nil;\n"
      "  }\n"
      "}\n"
      "func take_val(b: Box[i32]): i32 {\n"
      "  if (b.ptr == nil) { return -1; }\n"
      "  return b.ptr.!.*;\n"
      "}\n"
      "var g_val: i32 = 9;\n"
      "func give(): Box[i32] {\n"
      "  return .Box[i32]{ g_val.& };\n"
      "}\n"
      "func main(): i32 {\n"
      "  var v1: i32 = 7;\n"
      "  var a: Box[i32] = .Box[i32]{ v1.& };\n"
      "  var b: Box[i32] = a;              // var 初始化\n"
      "  if (a.ptr != nil) { return 1; }\n"
      "  if (b.ptr == nil || b.ptr.!.* != 7) { return 2; }\n"
      "  if (take_val(b) != 7) { return 3; }  // 实参\n"
      "  var c: Box[i32] = give();            // 返回值\n"
      "  if (c.ptr == nil || c.ptr.!.* != 9) { return 4; }\n"
      "  var d: Box[i32] = .Box[i32]{ v1.& };\n"
      "  d = c;                                // 赋值 → __assign__\n"
      "  if (c.ptr != nil) { return 5; }\n"
      "  if (d.ptr == nil || d.ptr.!.* != 9) { return 6; }\n"
      "  return 0;\n"
      "}\n",
      "init_copy_positions");
}

/* 没有 `__init_copy__` 时，同类型初始化仍然拒绝（不可拷贝的 move-only 类型）。 */
TEST(Magic, SameTypeInitNeedsInitCopy) {
  clux_expect_reject(
      "struct F { ptr: ?*i32; func __init__(self: *F, p: *i32): void { self.ptr = p; } }\n"
      "func main(): i32 {\n"
      "  var v: i32 = 1;\n"
      "  var a: F = .F{ v.& };\n"
      "  var b: F = a;\n"
      "  return 0;\n"
      "}\n",
      "cannot initialize", "init_copy_missing");
}

/* `__init__` 不能拿自身类型当参数：那会在"参数传递 = 构造"上无限递归
   （以前是编译器栈溢出）。同类型初始化请用 `__init_copy__`。 */
TEST(Magic, InitRejectsSelfTypedParameter) {
  clux_expect_reject(
      "struct F { v: i32; func __init__(self: *F, o: F): void { self.v = o.v; } }\n"
      "func main(): i32 {\n"
      "  var a: F = .F{ 1 };\n"
      "  var b: F = a;\n"
      "  return b.v;\n"
      "}\n",
      "cannot take the type itself", "init_self_param");
}

/* `__init_copy__` 的签名被固定为 `(self: *T, src: *T) -> void`。 */
TEST(Magic, InitCopySignatureChecked) {
  clux_expect_reject(
      "struct F { v: i32; func __init_copy__(self: *F, o: F): void { self.v = o.v; } }\n"
      "func main(): i32 { return 0; }\n",
      "must take its right-hand operand by pointer", "init_copy_sig");
}
