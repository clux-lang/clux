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

std::string write_temp_file(const std::string &content) {
  auto path = std::filesystem::temp_directory_path() / "clux_test_XXXXXX";
  auto path_str = path.string();
  /* mkstemps is not available on Windows; use a simple unique name. */
  static int counter = 0;
  path_str += std::to_string(counter++);
  FILE *fp = fopen(path_str.c_str(), "wb");
  fwrite(content.data(), 1, content.size(), fp);
  fclose(fp);
  return path_str;
}

} // namespace

/* ---- M3 指针与所有权（own/ref/fatal，m3-design §3/§7/§8） ---- */

TEST(Driver, PtrNewDerefWrite) {
  /* new 堆分配 + 解引用读 + 解引用写（m3-design §8.1/§8.2） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{123};\n"
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
      "  var p: own *i32 = new i32{123};\n"
      "  var c: own *i32 = clone(p);\n"
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
      "  var p: own *i32 = new i32{7};\n"
      "  var m: own *i32 = move(p);\n"
      "  if (m.* != 7) { return 1; }\n"
      "  m.* = 99;\n"
      "  if (m.* != 99) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrRefBorrowDoesNotOwn) {
  /* ref 借用（§3.2）：q 借 p 目标，写入经 q 可见于 p；q 退出不释放 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{456};\n"
      "  var q: ref *i32 = p;\n"
      "  q.* = 789;\n"
      "  if (p.* != 789) { return 1; }\n"
      "  if (q.* != 789) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrAddrBorrowsStackValue) {
  /* 后置取址 x.&（§8.2）：ref 指针借用栈上值，写回影响原值；
     指针退出作用域不释放栈值（owns=false） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var y: i32 = 42;\n"
      "  var a: ref *i32 = y.&;\n"
      "  a.* = 99;\n"
      "  if (y != 99) { return 1; }\n"
      "  if (a.* != 99) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrOpaqueRoundtrip) {
  /* opaque 往返（§8.4）：own → opaque（隐式）→ as 恢复（显式），
     恢复指针是借用（owns=false），读写仍命中原堆块 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var q: opaque = p;\n"
      "  var r: ref *i32 = q as ref *i32;\n"
      "  if (r.* != 7) { return 1; }\n"
      "  r.* = 88;\n"
      "  if (p.* != 88) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, PtrNestedDeref) {
  /* 嵌套指针 new own *i32{p} + 双重解引用（§8.2 链式） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var pp: own *own *i32 = new own *i32{move(p)};\n"
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
      "    var p: own *i64 = new i64{1234567890123};\n"
      "    if (p.* != 1234567890123) { return 1; }\n"
      "    var c: own *i64 = clone(p);\n"
      "    c.* = 1;\n"
      "    i = i + 1;\n"
      "  }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- M3 Step B 逃逸分析（R1 禁止 copy / R2 move TDZ / R3 借用存活，§3-§7） ---- */

TEST(Driver, StepBOwnCopyRejected) {
  /* R1（§3.1）：own → own 直接赋值 = copy → 编译错误 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  var q: own *i32 = p;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBOwnInitializedByMoveOrNew) {
  /* R1 正向：own 只接受 new / move / clone 接管源 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  var q: own *i32 = move(p);\n"
      "  var r: own *i32 = clone(q);\n"
      "  if (r.* != 1) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBOwnParamAndReturnRejected) {
  /* R1（§4）：参数/返回值不能是 own *T——own 是单所有权，跨函数传会分裂 */
  std::string path = write_temp_file(
      "func bad(p: own *i32): void {\n"
      "  var q: own *i32 = move(p);\n"
      "}\n"
      "func bad2(): own *i32 {\n"
      "  return new i32{1};\n"
      "}\n"
      "func main(): i32 {\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBFatalParamMustBeConsumed) {
  /* R1（§4）：fatal 参数必须被 move 接管或 return 出去 */
  std::string path = write_temp_file(
      "func take(p: fatal *i32): void {\n"
      "  var q: i32 = p.*;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  take(move(p));\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBFatalParamConsumedByMovePasses) {
  /* R1 正向：fatal 参数 move 接管放行 */
  std::string path = write_temp_file(
      "func take(p: fatal *i32): i32 {\n"
      "  var q: own *i32 = move(p);\n"
      "  return q.*;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var v: i32 = take(move(p));\n"
      "  if (v != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBFatalChainThroughReturnPasses) {
  /* §4：return new 产物（fatal）+ fatal 参数接收 + move 接管，完整传递链 */
  std::string path = write_temp_file(
      "func make(): fatal *i32 {\n"
      "  return new i32{5};\n"
      "}\n"
      "func take(p: fatal *i32): i32 {\n"
      "  var q: own *i32 = move(p);\n"
      "  return q.*;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var v: i32 = take(make());\n"
      "  if (v != 5) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBMoveAfterMoveRejected) {
  /* R2（§7）：move 后源进 TDZ，再次 move 报错 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  var q: own *i32 = move(p);\n"
      "  var r: own *i32 = move(p);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBReadAfterMoveRejected) {
  /* R2：move 后读取源报 used after move */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  var q: own *i32 = move(p);\n"
      "  var y: i32 = p.*;\n"
      "  return y;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBCloneKeepsSourceUsable) {
  /* R2 正向：clone 深拷贝，源保持可用（不标记 moved） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var c: own *i32 = clone(p);\n"
      "  var v: i32 = p.* + c.*;\n"
      "  if (v != 14) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBMoveWithLiveBorrowRejected) {
  /* R3（§7）：借用存活期间 move 源 → 借用悬空 → 编译错误 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  var r: ref *i32 = p;\n"
      "  var q: own *i32 = move(p);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBMoveAfterBorrowScopeEndsPasses) {
  /* R3 正向：借用作用域结束后 move 源放行（借用消亡恢复可 move） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  {\n"
      "    var r: ref *i32 = p;\n"
      "    if (r.* != 1) { return 1; }\n"
      "  }\n"
      "  var q: own *i32 = move(p);\n"
      "  if (q.* != 1) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBOpaqueBorrowBlocksMove) {
  /* R3（§8.4）：opaque 变量是借用载体，存活期间 move 源同样拦截 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  var q: opaque = p;\n"
      "  var r: ref *i32 = q as ref *i32;\n"
      "  var c: own *i32 = move(p);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBReturnRefFromLocalRejected) {
  /* R3（§6）：返回 ref 借用自本地变量 → 悬垂 → 编译错误 */
  std::string path = write_temp_file(
      "func bad(): ref *i32 {\n"
      "  var x: i32 = 42;\n"
      "  var r: ref *i32 = x.&;\n"
      "  return r;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var r: ref *i32 = bad();\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBReturnRefFromParamPasses) {
  /* R3（§6）正向：返回 ref 参数（借用自参数，放行） */
  std::string path = write_temp_file(
      "func get(p: ref *i32): ref *i32 {\n"
      "  return p;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var x: i32 = 42;\n"
      "  var r: ref *i32 = get(x.&);\n"
      "  if (r.* != 42) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBReturnI32FromPtrFunctionRejected) {
  /* 指针返回类型校验：return 0 赋给 ref *i32 应报错（ptr_assign shadow
     模式也做类型协商——回归：曾静默通过） */
  std::string path = write_temp_file(
      "func bad(): ref *i32 {\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 {\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}