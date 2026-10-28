#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "core/allocator.h"
#include "core/vec.h"
#include "driver/driver.h"
#include "parser/lexer.h"
}

#include "test_common.h"

namespace {


} // namespace

/* ---- M3 指针与所有权（own/ref/fatal，m3-design §3/§7/§8） ---- */

TEST(Driver, OwnReleasedAtScopeExit) {
  /* 变量在作用域退出点被显式释放（m3-design：销毁序列编译期生成）。
     覆盖三条退出路径——块尾 / return / break——以及"释放发生在块内变量上"。
     本用例同时锁定 FREE 的新形态：`FREE <expr>` 由 sema 生成 expr
     （简单指针 = 变量引用 → PUSH <name>），compiler 编译 expr 后发 FREE。
     若 FREE 退化成按名查表或 expr 未压栈，运行期会读到错误值或崩溃。 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var acc: i32 = 0;\n"
      "  {\n"                                  /* 块尾退出 */
      "    var a: *i32 = new i32{1};\n"
      "    acc = acc + a.*;\n"
      "  }\n"
      "  for (var i: i32 = 0; i < 3; i = i + 1) {\n"
      "    var b: *i32 = new i32{10};\n"
      "    acc = acc + b.*;\n"
      "    if (i == 1) { break; }\n"           /* break 退出（仍有块要释放） */
      "  }\n"
      "  {\n"
      "    var c: *i32 = new i32{100};\n"
      "    acc = acc + c.*;\n"
      "    return acc - 121;\n"                /* return 退出（块内 未到块尾） */
      "  }\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrNewDerefWrite) {
  /* new 堆分配 + 解引用读 + 解引用写（m3-design §8.1/§8.2） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: *i32 = new i32{123};\n"
      "  var v: i32 = p.*;\n"
      "  if (v != 123) { return 1; }\n"
      "  p.* = 456;\n"
      "  if (p.* != 456) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrCloneDeepCopyIsolation) {
  /* clone 深拷贝（§7）：c 与 p 各自独立堆块，互不影响 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: *i32 = new i32{123};\n"
      "  var c: *i32 = (p);\n"
      "  c.* = 456;\n"
      "  if (p.* != 123) { return 1; }\n"
      "  if (c.* != 456) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrMoveTransfersOwnership) {
  /* move 转移所有权（§7）：m 接管堆块，读写正常 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: *i32 = new i32{7};\n"
      "  var m: *i32 = (p);\n"
      "  if (m.* != 7) { return 1; }\n"
      "  m.* = 99;\n"
      "  if (m.* != 99) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrRefBorrowDoesNotOwn) {
  /* 借用（§3.2）：q 借 p 目标，写入经 q 可见于 p；q 退出不释放 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: *i32 = new i32{456};\n"
      "  var q: *i32 = p;\n"
      "  q.* = 789;\n"
      "  if (p.* != 789) { return 1; }\n"
      "  if (q.* != 789) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrAddrBorrowsStackValue) {
  /* 后置取址 x.&（§8.2）：指针借用栈上值，写回影响原值；
     指针退出作用域不释放栈值（owns=false） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var y: i32 = 42;\n"
      "  var a: *i32 = y.&;\n"
      "  a.* = 99;\n"
      "  if (y != 99) { return 1; }\n"
      "  if (a.* != 99) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrOpaqueRoundtrip) {
  /* opaque 往返（§8.4）：→ opaque（隐式）→ as 恢复（显式），
     恢复指针是借用（owns=false），读写仍命中原堆块 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: *i32 = new i32{7};\n"
      "  var q: opaque = p;\n"
      "  var r: *i32 = q as *i32;\n"
      "  if (r.* != 7) { return 1; }\n"
      "  r.* = 88;\n"
      "  if (p.* != 88) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrNestedDeref) {
  /* 嵌套指针 new *i32{p} + 双重解引用（§8.2 链式） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: *i32 = new i32{7};\n"
      "  var pp: **i32 = new *i32{(p)};\n"
      "  var v: i32 = pp.*.*;\n"
      "  if (v != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrScopeExitReleasesHeap) {
  /* 作用域退出销毁堆块（§3.1）：循环 1 万次 new/clone，堆块随作用域
     释放（无累积泄露、无 double free） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var i: i32 = 0;\n"
      "  while (i < 10000) {\n"
      "    var p: *i64 = new i64{1234567890123};\n"
      "    if (p.* != 1234567890123) { return 1; }\n"
      "    var c: *i64 = (p);\n"
      "    c.* = 1;\n"
      "    i = i + 1;\n"
      "  }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- M3 Step B 逃逸分析（R1 禁止 copy / R2 move TDZ / R3 借用存活，§3-§7） ---- */

/* ---- 指针的两处 move 约束 ----
 *
 * 约束 1：**聚合的 字段不能单独 move**。TDZ 是**符号级**的（只跟踪变量名），
 *   字段没有"局部 TDZ"——`(s.p)` 把资源掏走却不留任何标记，可以重复执行。
 *   所以含 资源的字段/元素只能连**整个**聚合一起 move。
 *
 * 约束 2：**闭包捕获不能 move**（含捕获里的结构体）。捕获作用域由闭包的**所有
 *   调用共享**：move 把捕获槽标进 TDZ 后无法恢复，第二次调用读到已转移的槽位。
 *   闭包内只能用 clone / 借用。
 */

/* ---- M3 §12 RC 引用计数系（share/weak/upgrade）已移除 ---- */

TEST(Driver, AnonConstructFieldEvaluatedOnce) {
  /* 回归：匿名构造 `.{...}` 的推断分支曾漏 `return`，控制流继续落入具名构造
     分支，对同一批字段表达式**二次求值**——第二次求值看到第一次留下的
     `moved` 标记，于是含 () 的字段被误报
     "variable 'p' used after move"。同样代码写具名构造 `.Cell{...}` 本来就正常，
     此测试锁定匿名形式必须与具名形式行为一致。 */
  std::string path = write_temp_file(
      "struct Cell { v: i32; }\n"
      "func main(): i32 {\n"
      "  var p: *i32 = new i32{7};\n"
      "  var c: Cell = .{ .v = ((p)).* };\n"
      "  if (c.v != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- R1 参数消费：return 出去 = 所有权继续传递（指针与切片两种）----
 *
 * 回归背景：`walk_return` 的"return 参数 → 标记已消费"原先写在
 * `: *T` 的分支内部，因此只有 返回才生效；指针/切片返回走不到
 * 那段，`func id(p: []i32): []i32 { return p; }` 被误报
 * "parameter 'p' is not consumed"（诊断文本自己写的是
 * "take ownership with () or return it"）。判据现与函数结束的
 * 未消费检查（PTR_FATAL || SLICE_FATAL）一致。 */
TEST(Driver, OwnSliceParamReturnedIsConsumed) {
  /* §0.5：`[]T` 作形参（参数位用 own，不是 fatal）。
     调用点 `id((s))`：FATAL → 形参 OWN；形参再作为 `[]T` 返回位交出。 */
  std::string path = write_temp_file(
      "func id(p: []i32): []i32 {\n"
      "  return (p);\n"
      "}\n"
      "func main(): i32 {\n"
      "  var s: []i32 = make(i32, 2, 9, 9);\n"
      "  var t: []i32 = id((s));\n"
      "  if (t[1] != 9) { return 1; }\n"
      "  if (t[0] != 9) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}


TEST(ComptimeForeach, ArrayUnrolled) {
  clux_expect_ok(
      "var SUM: i32 = 0;\n"
      "func main(): i32 {\n"
      "  var arr = .[3]i32{ 10, 20, 30 };\n"
      "  comptime foreach (var x of arr) { SUM = SUM + x; }\n"
      "  if (SUM != 60) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ctfe_fe_array");
}

TEST(ComptimeForeach, ExistingVariableForm) {
  clux_expect_ok(
      "var LAST: i32 = 0;\n"
      "func main(): i32 {\n"
      "  var arr = .[3]i32{ 1, 2, 3 };\n"
      "  var y: i32 = 0;\n"
      "  comptime foreach (y of arr) { LAST = y; }\n"
      "  if (LAST != 3) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ctfe_fe_assign_form");
}

/* 元组：每份副本按**该元素的实际类型**检查（i32 / f64 宽度不同） */
TEST(ComptimeForeach, TuplePerElementTypes) {
  clux_expect_ok(
      "var W: i32 = 0;\n"
      "func main(): i32 {\n"
      "  var t = .<i32, f64>{ 1, 2.5 };\n"
      "  comptime foreach (var e of t) {\n"
      "    W = W + (sizeof(typeof(e)) as i32);\n"
      "  }\n"
      "  if (W != 12) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ctfe_fe_tuple_types");
}

TEST(ComptimeForeach, Nested) {
  clux_expect_ok(
      "var N: i32 = 0;\n"
      "func main(): i32 {\n"
      "  var a = .[2]i32{ 1, 2 };\n"
      "  var b = .[2]i32{ 10, 20 };\n"
      "  comptime foreach (var p of a) {\n"
      "    comptime foreach (var q of b) { N = N + p * q; }\n"
      "  }\n"
      "  if (N != 90) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "ctfe_fe_nested");
}

TEST(ComptimeForeach, ComptimeVarContainer) {
  clux_expect_ok(
      "var S: i32 = 0;\n"
      "func main(): i32 {\n"
      "  comptime var C = .[2]i32{ 5, 6 };\n"
      "  comptime foreach (var v of C) { S = S + v; }\n"
      "  if (S != 11) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ctfe_fe_comptime_var");
}

/* 切片长度运行期才知道 → 拒绝 */
TEST(ComptimeForeach, SliceRejected) {
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var arr = .[3]i32{ 1, 2, 3 };\n"
      "  var s = arr[0:3];\n"
      "  comptime foreach (var x of s) { }\n"
      "  return 0;\n"
      "}\n",
      "ctfe_fe_slice");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("container is a slice"), std::string::npos) << err;
  std::remove(path.c_str());
}

/* 非数组/元组容器 → 拒绝 */
TEST(ComptimeForeach, NonSequenceRejected) {
  std::string path = write_temp_file(
      "struct P { x: i32; }\n"
      "func main(): i32 {\n"
      "  var p = .P{ .x = 1 };\n"
      "  comptime foreach (var x of p) { }\n"
      "  return 0;\n"
      "}\n",
      "ctfe_fe_struct");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("requires an array or tuple"), std::string::npos) << err;
  std::remove(path.c_str());
}