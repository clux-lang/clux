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

/* ---- M3 指针（raw pointer，生命周期照搬 C） ---- */

TEST(Driver, PtrDerefWrite) {
  /* 栈上值 + 取址 + 解引用读/写 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var x: i32 = 123;\n"
      "  var p: *i32 = x.&;\n"
      "  var v: i32 = p.*;\n"
      "  if (v != 123) { return 1; }\n"
      "  p.* = 456;\n"
      "  if (x != 456) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrAliasSeesWrites) {
  /* 指针拷贝是别名（与 C 一致）：经 q 写入对 p 可见 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var x: i32 = 456;\n"
      "  var p: *i32 = x.&;\n"
      "  var q: *i32 = p;\n"
      "  q.* = 789;\n"
      "  if (p.* != 789) { return 1; }\n"
      "  if (x != 789) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrOpaqueRoundtrip) {
  /* opaque 往返：*T → opaque（隐式）→ as 恢复，读写命中原值 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var x: i32 = 7;\n"
      "  var p: *i32 = x.&;\n"
      "  var q: opaque = p;\n"
      "  var r: *i32 = q as *i32;\n"
      "  if (r.* != 7) { return 1; }\n"
      "  r.* = 88;\n"
      "  if (x != 88) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrNestedDeref) {
  /* 指针的指针 + 双重解引用 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var x: i32 = 7;\n"
      "  var p: *i32 = x.&;\n"
      "  var pp: **i32 = p.&;\n"
      "  var v: i32 = pp.*.*;\n"
      "  if (v != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, AnonConstructFieldEvaluatedOnce) {
  /* 匿名构造 .{...} 与具名构造行为一致，字段表达式只求值一次 */
  std::string path = write_temp_file(
      "struct Cell { v: i32; }\n"
      "func main(): i32 {\n"
      "  var x: i32 = 7;\n"
      "  var p: *i32 = x.&;\n"
      "  var c: Cell = .{ .v = p.* };\n"
      "  if (c.v != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, SliceParamReturnedIsConsumed) {
  /* []T 形参直接 return；切片由数组 + make 视图构造 */
  std::string path = write_temp_file(
      "func id(p: []i32): []i32 {\n"
      "  return p;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: [2]i32 = .[2]i32{ 9 as i32, 9 as i32 };\n"
      "  var s: []i32 = make(a[0].&, 2);\n"
      "  var t: []i32 = id(s);\n"
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