/*
 * 闭包作为**聚合值**的所有权补充（2026-10 完备性审计修复）。
 *
 * 审计发现（闭包 = 捕获聚合结构体之后的遗留缺口，均已修复）：
 *   1. `f = clone(...)` 覆盖旧闭包：`func_assign` 只 memcpy → 旧 env 泄漏
 *      → 赋值点销毁补上 FUNC（`FREE f`，按 capture_type 聚合释放）；
 *   2. `own Holder{ .f: own func(...) }`：结构体字段的 `own func` 所有权编码
 *      没被识别（`ast_type_expr_ownership_code` 缺 AST_OWNED_TYPE）→ 构造不产
 *      FATAL、字段不销毁；
 *   3. `_ = move(cl)`：丢弃路径的"含资源"判定缺 FUNC → 直接 POP，env 泄漏；
 *   4. 闭包值整体不可 move（旧模型遗留）→ 无法转移进 own 字段/选项/数组；
 *   5. 闭包槽位的克隆/释放：`clone_needs_deep` 缺 FUNC（own 槽位不深拷贝）、
 *      `slot_needs_visit` 对 FUNC 无条件下钻 → 自捕获闭包（`func |fib| fib`）
 *      释放时无限递归（栈溢出）；
 *   6. `defer |(q = move(p))| {...}`：defer 闭包的 env 从不释放 → 泄漏。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

const char *kClosure =
    "func main(): i32 {\n"
    "  var p: own *i32 = new i32{7};\n"
    "  var cl = clone(func |(q: own *i32 = move(p))| get(): i32 { return q.*; });\n";

} // namespace

/* ==================== 闭包可以 move / 显式销毁 ==================== */

TEST(ClosureValueOwn, ExplicitDestroy) {
  clux_expect_ok(std::string(kClosure) +
                 "  _ = move(cl);\n"   /* 显式销毁：FREE 释放 env 的 own 捕获 */
                 "  return 0;\n"
                 "}\n",
                 "cv_destroy");
}

TEST(ClosureValueOwn, MoveIntoStructField) {
  clux_expect_ok(
      "struct Holder { f: own func()->i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var cl = clone(func |(q: own *i32 = move(p))| get(): i32 { return q.*; });\n"
      "  var h: own Holder = .Holder{ .f = move(cl), .n = 1 };\n"
      "  if (h.f() != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cv_field");
}

TEST(ClosureValueOwn, StructWithClosureFieldClone) {
  clux_expect_ok(
      "struct Holder { f: own func()->i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var cl = clone(func |(q: own *i32 = move(p))| get(): i32 { return q.*; });\n"
      "  var h: own Holder = .Holder{ .f = move(cl), .n = 1 };\n"
      "  var h2: own Holder = clone(h);\n"   /* own 闭包字段深拷贝 env */
      "  if (h2.f() != 7) { return 1; }\n"
      "  if (h.f() != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "cv_struct_clone");
}

TEST(ClosureValueOwn, StructWithClosureFieldReassign) {
  clux_expect_ok(
      "struct Holder { f: own func()->i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var cl = clone(func |(q: own *i32 = move(p))| get(): i32 { return q.*; });\n"
      "  var h: own Holder = .Holder{ .f = move(cl), .n = 1 };\n"
      "  var p2: own *i32 = new i32{8};\n"
      "  var cl2 = clone(func |(q: own *i32 = move(p2))| get(): i32 { return q.*; });\n"
      "  h = .Holder{ .f = move(cl2), .n = 2 };\n"  /* 赋值点释放旧闭包字段 */
      "  if (h.f() != 8) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cv_struct_reassign");
}

TEST(ClosureValueOwn, OptionAndSliceOfOwnClosures) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var cl = clone(func |(q: own *i32 = move(p))| get(): i32 { return q.*; });\n"
      "  var o: own ?own func |q: own *i32| () -> i32 =\n"
      "      .?own func |q: own *i32| () -> i32{ move(cl) };\n"
      "  if (o.!() != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cv_option");
}

TEST(ClosureValueOwn, SliceOfOwnClosuresClone) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var cl = clone(func |(q: own *i32 = move(p))| get(): i32 { return q.*; });\n"
      "  var s: own []own func |q: own *i32| () -> i32 =\n"
      "      make(own func |q: own *i32| () -> i32, 1, move(cl));\n"
      "  var c: own []own func |q: own *i32| () -> i32 = clone(s);\n"
      "  if (c[0]() != 7) { return 1; }\n"
      "  if (s[0]() != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "cv_slice");
}

TEST(ClosureValueOwn, ArrayOfOwnClosuresDestroy) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var cl = clone(func |(q: own *i32 = move(p))| get(): i32 { return q.*; });\n"
      "  var a: own [2]own func()->i32 = .[2]own func()->i32{ move(cl),\n"
      "      clone(func |(q: i32 = 1)| get(): i32 { return q; }) };\n"
      "  if (a[0]() != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cv_array");
}

/* ==================== 闭包变量重新赋值 ==================== */

TEST(ClosureValueOwn, ReassignReleasesOldEnv) {
  clux_expect_ok(std::string(kClosure) +
                 "  var q: own *i32 = new i32{8};\n"
                 "  cl = clone(func |(r: own *i32 = move(q))| get(): i32 {"
                 " return r.*; });\n"   /* 旧 env（q=7）必须释放 */
                 "  if (cl() != 8) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "cv_reassign");
}

/* ==================== defer 闭包的 env 释放 ==================== */

TEST(ClosureValueOwn, DeferOwnCaptureFreed) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  defer |(q: own *i32 = move(p))| { }\n"
      "  return 0;\n"
      "}\n",
      "cv_defer");
}

/* ==================== 自捕获递归闭包（释放不得自递归） ==================== */

TEST(ClosureValueOwn, SelfCapturingRecursiveClosure) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  func |fib| fib(n: i32): i32 {\n"
      "    if (n <= 1) { return n; }\n"
      "    return fib(n - 1) + fib(n - 2);\n"
      "  }\n"
      "  if (fib(10) != 55) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cv_self_capture");
}

/* ==================== 嵌套闭包捕获闭包 ==================== */

TEST(ClosureValueOwn, ClosureCapturingClosure) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var inner = clone(func |(q: own *i32 = move(p))| get(): i32 {"
      " return q.*; });\n"
      "  var outer = clone(func |(f: own func()->i32 = move(inner))| call(): i32 {"
      " return f(); });\n"
      "  if (outer() != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cv_nested");
}

/* ==================== 闭包类型注解 + 泛型 + own 捕获 clone ==================== */

/* 闭包类型作为一等类型：own 捕获的闭包 move 进变量、clone 出独立 env、
 * 通过泛型形参调用；VM 侧结束时 allocator 必须无泄漏（env 释放一次）。 */
TEST(ClosureValueOwn, OwnCaptureCloneAndGenericCall) {
  clux_expect_ok(
      "type F = func |p: own *i32| (i32) -> i32;\n"
      "func apply[G](f: G, x: i32): i32 { return f(x); }\n"
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{5};\n"
      "  var f: own F = move(func |(p = move(p))| (x: i32): i32 { return p.* + x; });\n"
      "  var g: own F = clone(f);\n"
      "  if (f(1) != 6) { return 1; }\n"
      "  if (g(2) != 7) { return 2; }\n"
      "  if (apply[F](f, 3) != 8) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "cv_own_clone_generic");
}

/* ==================== 捕获可写 + 值语义拷贝 ==================== */

/* 捕获槽在闭包体内可写（读写都走 __ctx.env）；拷贝闭包必须深拷贝 env：
 * 修改副本不影响源（C 与 VM 一致）。 */
TEST(ClosureValueOwn, CaptureMutationAndCopyIsIndependent) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var f = func |(n: i32 = 0)| (): i32 { n = n + 1; return n; };\n"
      "  if (f() != 1) { return 1; }\n"
      "  if (f() != 2) { return 2; }\n"
      "  var g = f;\n"
      "  if (g() != 3) { return 3; }\n"
      "  if (f() != 3) { return 4; }\n"
      "  if (g() != 4) { return 5; }\n"
      "  return 0;\n"
      "}\n",
      "cv_mutate_copy");
}

/* 赋值同样是值语义：`g = f` 之后两者 env 独立。 */
TEST(ClosureValueOwn, CaptureMutationAssignmentCopyIsIndependent) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var f = func |(n: i32 = 0)| (): i32 { n = n + 1; return n; };\n"
      "  var g = func |(m: i32 = 100)| (): i32 { m = m + 1; return m; };\n"
      "  g = f;\n"
      "  if (f() != 1) { return 1; }\n"
      "  if (g() != 1) { return 2; }\n"
      "  if (f() != 2) { return 3; }\n"
      "  if (g() != 2) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "cv_mutate_assign");
}

/* 聚合内部闭包字段的值语义：struct 拷贝/赋值不得共享闭包 env。 */
TEST(ClosureValueOwn, StructClosureFieldCopyIsIndependent) {
  clux_expect_ok(
      "struct Holder { f: func |n: i32| () -> i32; }\n"
      "func main(): i32 {\n"
      "  var f = func |(n: i32 = 0)| (): i32 { n = n + 1; return n; };\n"
      "  var h = .Holder{ .f = f };\n"
      "  var h2 = h;\n"
      "  if (h.f() != 1) { return 1; }\n"
      "  if (h2.f() != 1) { return 2; }\n"
      "  if (h.f() != 2) { return 3; }\n"
      "  if (h2.f() != 2) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "cv_struct_field_copy");
}

/* 数组元素里的闭包同样按值语义拷贝。 */
TEST(ClosureValueOwn, ArrayClosureElementCopyIsIndependent) {
  clux_expect_ok(
      "type F = func |n: i32| () -> i32;\n"
      "func main(): i32 {\n"
      "  var a = .[2]F{\n"
      "    func |(n: i32 = 0)| (): i32 { n = n + 1; return n; },\n"
      "    func |(n: i32 = 10)| (): i32 { n = n + 1; return n; } };\n"
      "  var b = a;\n"
      "  if (a[0]() != 1) { return 1; }\n"
      "  if (b[0]() != 1) { return 2; }\n"
      "  if (a[0]() != 2) { return 3; }\n"
      "  if (b[0]() != 2) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "cv_array_elem_copy");
}
