/*
 * RC 系（share / weak，2026-10 重新引入）
 *
 * **`share *T` / `weak *T` 是真实类型**，不是所有权修饰（语法上看起来像）：
 *   - 类型层：ptr_type_t.rc = 1/2，独立 intern 实例与 vtable（VTABLE_PTR_SHARE /
 *     VTABLE_PTR_WEAK）；`PUSH_PTR <rc>` 让字节码类型表也带形态。
 *   - copy = 计数 +1（clone/assign/传参/返回/字段拷贝），drop = 计数 -1；
 *     strong→0 析构 payload（递归释放内嵌 own/share）但保留内存，weak→0 才释放。
 *   - 创建通道（§12.1）：`var s: share *T = <fatal/own *T>` —— sema 改写成
 *     `__make_share(...)`（源码不写 `__` 名字：保留空间）。
 *   - weak 只从 share 派生（隐式 `share *T → weak *T`）。
 *   - 升级不再用全局 `upgrade`（太泛，容易和业务函数重名），改用**成员调用语法**
 *     `w.get()`：sema 改写成 `__weak_get(w)`，返回 `?share *T`（死对象得 nil）。
 *   - 单向隔离：weak → share 只能经 `.get()`；share 不能转 own/ref。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

std::string rc_prog(const std::string &body) {
  return "func main(): i32 {\n" + body + "  return 0;\n}\n";
}

} // namespace

/* ==================== 创建 / 解引用 / copy ==================== */

TEST(ShareWeak, CreateAndDeref) {
  clux_expect_ok(rc_prog("  var s: share *i32 = new i32{7};\n"
                         "  if (s.* != 7) { return 1; }\n"),
                 "rc_create");
}

TEST(ShareWeak, CopyKeepsObjectAlive) {
  /* 外层块里的 share 释放后，外层副本仍然持有对象。 */
  clux_expect_ok(rc_prog("  var s: share *i32 = new i32{7};\n"
                         "  var keep: share *i32 = undefined;\n"
                         "  {\n"
                         "    var s2: share *i32 = s;   /* copy → +1 */\n"
                         "    keep = s2;\n"
                         "    if (keep.* != 7) { return 1; }\n"
                         "  }\n"
                         "  if (keep.* != 7) { return 2; }\n"),
                 "rc_copy");
}

TEST(ShareWeak, StructFieldCopies) {
  clux_expect_ok(
      "struct Holder { s: share *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var s: share *i32 = new i32{7};\n"
      "  var h: Holder = .Holder{ .s = s, .n = 1 };\n"
      "  var h2: Holder = h;\n"      /* 字段 copy = 计数 +1 */
      "  if (h2.s.* != 7) { return 1; }\n"
      "  if (h.n != 1) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "rc_field");
}

TEST(ShareWeak, ArrayOfShares) {
  clux_expect_ok(rc_prog("  var s: share *i32 = new i32{3};\n"
                         "  var a: [2]share *i32 = .[2]share *i32{ s, s };\n"
                         "  if (a[0].* != 3) { return 1; }\n"),
                 "rc_array");
}

TEST(ShareWeak, ClosureCapture) {
  clux_expect_ok(rc_prog(
      "  var s: share *i32 = new i32{7};\n"
      "  var f = clone(func |(c: share *i32 = s)| get(): i32 { return c.*; });\n"
      "  var g = clone(f);\n"   /* 闭包 clone → env 里的 share 计数 +1 */
      "  if (f() != 7) { return 1; }\n"
      "  if (g() != 7) { return 2; }\n"),
      "rc_closure");
}

/* ==================== weak + `.get()` ==================== */

TEST(ShareWeak, GetUpgradesWhileAlive) {
  clux_expect_ok(rc_prog("  var s: share *i32 = new i32{7};\n"
                         "  var w: weak *i32 = s;\n"
                         "  var g = w.get();\n"
                         "  if (g == nil) { return 1; }\n"
                         "  if (g.!.* != 7) { return 2; }\n"),
                 "rc_get_alive");
}

TEST(ShareWeak, GetReturnsNilAfterLastShareDropped) {
  clux_expect_ok(rc_prog(
      "  var w: weak *i32 = undefined;\n"
      "  {\n"
      "    var s: share *i32 = new i32{7};\n"
      "    w = s;\n"
      "    var g1 = w.get();\n"
      "    if (g1 == nil) { return 1; }\n"
      "  }\n"                       /* 最后一个 share 丢弃 → payload 析构 */
      "  var g2 = w.get();\n"
      "  if (g2 != nil) { return 2; }\n"),
      "rc_get_dead");
}

TEST(ShareWeak, WeakCountKeepsBlockAlive) {
  /* 对象析构后 weak 仍存活：`.get()` 得 nil 且不崩溃；weak 也释放后内存才回收
     （无泄漏由 clux_expect_ok 的 leak 检查保证）。 */
  clux_expect_ok(rc_prog(
      "  var w1: weak *i32 = undefined;\n"
      "  var w2: weak *i32 = undefined;\n"
      "  {\n"
      "    var s: share *i32 = new i32{9};\n"
      "    w1 = s;\n"
      "    w2 = w1;\n"                 /* weak copy → 弱计数 +1 */
      "  }\n"
      "  if (w1.get() != nil) { return 1; }\n"
      "  if (w2.get() != nil) { return 2; }\n"),
      "rc_weak_count");
}

/* ==================== 类型规则 ==================== */

TEST(ShareWeak, WeakCannotBeDereferenced) {
  clux_expect_reject(rc_prog("  var s: share *i32 = new i32{7};\n"
                             "  var w: weak *i32 = s;\n"
                             "  if (w.* != 7) { return 1; }\n"),
                     "weak", "rc_weak_deref");
}

TEST(ShareWeak, GetOnlyOnWeak) {
  clux_expect_reject(rc_prog("  var s: share *i32 = new i32{7};\n"
                             "  var g = s.get();\n"),
                     ".get()", "rc_get_only_weak");
}

TEST(ShareWeak, CreationNeedsOwnOrFatalSource) {
  clux_expect_reject(rc_prog("  var s: share *i32 = new i32{7};\n"
                             "  var s2: share *i32 = s.*.&;\n"),
                     "share", "rc_create_source");
}

TEST(ShareWeak, PointeeWithOwnResourcesDestroyed) {
  clux_expect_ok(
      "struct Inner { p: own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var s: share *Inner = new Inner{ .p = new i32{9}, .n = 1 };\n"
      "  var s2: share *Inner = s;\n"
      "  if (s2.*.p.* != 9) { return 1; }\n"   /* strong→0 时 p 递归释放 */
      "  return 0;\n"
      "}\n",
      "rc_pointee_own");
}
