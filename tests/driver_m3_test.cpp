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
      "func mk_fatal(): fatal *i32 {\n"
      "  return new i32{5};\n"
      "}\n"
      "func take(p: fatal *i32): i32 {\n"
      "  var q: own *i32 = move(p);\n"
      "  return q.*;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var v: i32 = take(mk_fatal());\n"
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
  /* R3（§6）正向：返回 ref 参数（借用自参数，放行）。
     §5：返回 ref 必标注（'<p> = 借自参数 p）；调用点接收 ref 变量
     可显式标注（'<x> = 借自 x） */
  std::string path = write_temp_file(
      "func get(p: ref *i32): '<p> ref *i32 {\n"
      "  return p;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var x: i32 = 42;\n"
      "  var r: '<x> ref *i32 = get(x.&);\n"
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

TEST(Driver, StepBReturnRefUnannotatedRejected) {
  /* §5 强制规则：函数返回 ref 必须声明作用域标注（'<p> ref *T），
     未标注 → 编译错误（用户澄清：仅函数返回值 ref 强制标注） */
  std::string path = write_temp_file(
      "func get(p: ref *i32): ref *i32 {\n"
      "  return p;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var x: i32 = 42;\n"
      "  var r: ref *i32 = get(x.&);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBLocalRefUnannotatedInferredPasses) {
  /* §5 推断语义（用户澄清）：局部 ref 变量未标注不强制——按定义作用域
     推断（ref 存活 = 当前定义作用域）。借 p 且在 p 存活作用域内 → 放行 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var q: ref *i32 = p;\n"
      "  if (q.* != 7) { return 1; }\n"
      "  q.* = 8;\n"
      "  if (p.* != 8) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBAnnotEscapePasses) {
  /* §5 检查算法：显式标注的 ref 变量定义作用域必须嵌套于锚点作用域。
     r 定义在块内，锚点 '<q>（q 在块外定义）→ r 作用域嵌套于 q 作用域
     → 放行 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  var q: '<p> ref *i32 = p;\n"
      "  {\n"
      "    var r: '<q> ref *i32 = q;\n"
      "    if (r.* != 1) { return 1; }\n"
      "  }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBAnnotUnknownNameRejected) {
  /* §5 锚点解析：标注名必须命名参数、全局或局部变量（或 '*' = global）。
     '<nope> 未定义 → 编译错误 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  var q: '<nope> ref *i32 = p;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBAnnotGlobalStarPasses) {
  /* §5 '<*>' = global：全局作用域恒放行（全局值存活到程序结束） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{5};\n"
      "  var q: '<*> ref *i32 = p;\n"
      "  if (q.* != 5) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBAnnotOnI32TypeIgnored) {
  /* §5 标注仅对含 ref 指针的类型有效：'<x> i32（标量）不触发逃逸检查
     （type_contains_ref=false），放行 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var x: i32 = 1;\n"
      "  var y: '<x> i32 = 2;\n"
      "  return x + y;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBStructWithRefUnannotatedRejected) {
  /* §5 强制规则 B：含 ref 字段的聚合类型（struct）变量定义必须标注。
     未标注 → 编译错误（聚合内 ref 字段借源无法从 init 静态推断） */
  std::string path = write_temp_file(
      "struct Pair { a: ref *i32; b: i32; }\n"
      "func main(): i32 {\n"
      "  var x: i32 = 42;\n"
      "  var s: Pair = .Pair{ .a = x.&, .b = 1 };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBStructWithRefAnnotatedPasses) {
  /* §5 强制规则 B 正向：含 ref 字段的 struct 标注后放行
     （'<x> = a 字段借自 x） */
  std::string path = write_temp_file(
      "struct Pair { a: ref *i32; b: i32; }\n"
      "func main(): i32 {\n"
      "  var x: i32 = 42;\n"
      "  var s: '<x> Pair = .Pair{ .a = x.&, .b = 1 };\n"
      "  if (s.a.* != 42) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBRebindRefEscapeRejected) {
  /* §5 重绑定逃逸校验：r 标注 '<p>（存活不超过 p），重绑定到 outer
     （定义在外层，比 p 长命）→ r 通过 outer 获得超过 p 的存活期 → 逃逸 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var outer: own *i32 = new i32{1};\n"
      "  {\n"
      "    var p: own *i32 = new i32{2};\n"
      "    var r: '<p> ref *i32 = p;\n"
      "    r = outer;\n"
      "  }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBRebindRefSameScopePasses) {
  /* §5 重绑定正向：r 标注 '<p>，重绑定到同作用域的 q（同级，存活期相同
     → 不逃逸 → 放行） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  var q: own *i32 = new i32{2};\n"
      "  var r: '<p> ref *i32 = p;\n"
      "  r = q;\n"
      "  if (r.* != 2) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBFieldAccessInheritsAnnotPasses) {
  /* §5 字段访问继承（m3-design §5"var p = s.aaa; 中 p 继承 s 的作用域
     绑定"）：s 标注 '<x>，未标注的 var p = s.a 继承 s 的标注集合
     （p 借自 s 的 ref 字段，p 存活 ≤ s 存活 ≤ x 存活）→ 放行 */
  std::string path = write_temp_file(
      "struct Pair { a: ref *i32; b: i32; }\n"
      "func main(): i32 {\n"
      "  var x: i32 = 42;\n"
      "  {\n"
      "    var s: '<x> Pair = .Pair{ .a = x.&, .b = 1 };\n"
      "    var p = s.a;\n"
      "    if (p.* != 42) { return 1; }\n"
      "  }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBGlobalOwnRejected) {
  /* §10 全局规则（m3-design §10）：禁止全局 own（含任何嵌套位置）。
     全局变量类型 own *T → 编译错误（检查器失去确定性） */
  std::string path = write_temp_file(
      "var g: own *i32 = new i32{42};\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBGlobalStructWithOwnRejected) {
  /* §10 全局规则：含 own 字段的聚合结构作全局变量 → 编译错误 */
  std::string path = write_temp_file(
      "struct Pair { a: own *i32; b: i32; }\n"
      "var g: Pair = .Pair{ .a = new i32{1}, .b = 2 };\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBGlobalScalarPasses) {
  /* §10 全局规则正向：标量全局变量放行（全局值域 = 复制语义） */
  std::string path = write_temp_file(
      "var g: i32 = 42;\n"
      "func main(): i32 {\n"
      "  if (g != 42) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBClosureOwnBareCaptureRejected) {
  /* §9.1 own 裸捕获禁止：纯 id 捕获 own *T = copy → 编译错误
     （必须 move()/clone()） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{42};\n"
      "  var f = func |p| get(): i32 { return p.*; };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBClosureMoveCapturePasses) {
  /* §9.1 move 捕获：|(q = move(p))| 推断 own *T，转移所有权 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{42};\n"
      "  var f = func |(q = move(p))| get(): i32 { return q.*; };\n"
      "  if (f() != 42) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBClosureCloneCapturePasses) {
  /* §9.1 clone 捕获：|(q = clone(p))| 深拷贝，原对象不变 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{42};\n"
      "  var f = func |(q = clone(p))| get(): i32 { return q.*; };\n"
      "  if (f() != 42) { return 1; }\n"
      "  if (p.* != 42) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBClosureRefCaptureUnannotatedRejected) {
  /* §9.3 ref 捕获强制标注：捕获 ref 且闭包无作用域标注 → 编译错误 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var x: i32 = 42;\n"
      "  var r: '<x> ref *i32 = x.&;\n"
      "  var f = func |r| get(): i32 { return r.*; };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBClosureRefCaptureAnnotatedPasses) {
  /* §9.3 ref 捕获带标注放行：func '<x> |r| ——闭包存活不超过 x */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var x: i32 = 42;\n"
      "  var r: '<x> ref *i32 = x.&;\n"
      "  var f = func '<x> |r| get(): i32 { return r.*; };\n"
      "  if (f() != 42) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBClosureReturnCapturedRefPasses) {
  /* §9 闭包返回捕获 ref：捕获视同特殊参数，返回 ref 可来自捕获变量
     （调用闭包时闭包对象必然存活，捕获的 ref 有效） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var x: i32 = 42;\n"
      "  var r: '<x> ref *i32 = x.&;\n"
      "  var f = func '<x> |r| get(): '<x> ref *i32 { return r; };\n"
      "  var result = f();\n"
      "  if (result.* != 42) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, StepBClosureReturnRefAnnotEscapesRejected) {
  /* §9 闭包返回捕获 ref 的标注关系：返回值标注 '<outer> 比闭包标注
     '<inner> 宽 → 逃逸（调用方以为返回值活到 outer，实际活不过 inner） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var outer: i32 = 99;\n"
      "  {\n"
      "    var inner: i32 = 42;\n"
      "    var r: '<inner> ref *i32 = inner.&;\n"
      "    var f = func '<inner> |r| get(): '<outer> ref *i32 { return r; };\n"
      "  }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* ---- M3 §12 RC 引用计数系（share/weak/upgrade） ---- */

TEST(Driver, ShareCreateAndDeref) {
  /* share 创建（fatal→share 隐式）+ 解引用读写（§12.1/§12.2） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: share *i32 = new i32{42};\n"
      "  if (s.* != 42) { return 1; }\n"
      "  s.* = 99;\n"
      "  if (s.* != 99) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ShareCopySharedMutation) {
  /* share copy = strong+1，两个 share 指向同一对象（§12.2） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: share *i32 = new i32{42};\n"
      "  var s2: share *i32 = s;\n"
      "  if (s2.* != 42) { return 1; }\n"
      "  s2.* = 99;\n"
      "  if (s.* != 99) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, WeakFromShareUpgradeSuccess) {
  /* weak 从 share 派生 + upgrade 成功返回 some share（§12.1/§12.4） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: share *i32 = new i32{42};\n"
      "  var w: weak *i32 = s;\n"
      "  var r: ?share *i32 = upgrade(w);\n"
      "  if (r == nil) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, UpgradeFailureAfterDestroy) {
  /* strong 归零后 upgrade 返回 none（§12.4：对象已析构）。
     weak 保命不保访问——控制块存活但 payload 已析构 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s0: share *i32 = new i32{0};\n"
      "  var w: weak *i32 = s0;\n"
      "  {\n"
      "    var s: share *i32 = new i32{42};\n"
      "    w = s;\n"
      "  }\n"
      "  var r: ?share *i32 = upgrade(w);\n"
      "  if (r != nil) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ShareWeakStressLoop) {
  /* 循环 1 万次 share 创建/copy/weak/upgrade，验证引用计数无泄露无 double free */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var i: i32 = 0;\n"
      "  while (i < 10000) {\n"
      "    var s: share *i64 = new i64{1234567890123};\n"
      "    if (s.* != 1234567890123) { return 1; }\n"
      "    var s2: share *i64 = s;\n"
      "    s2.* = i;\n"
      "    if (s.* != i) { return 2; }\n"
      "    var w: weak *i64 = s;\n"
      "    var r: ?share *i64 = upgrade(w);\n"
      "    if (r == nil) { return 3; }\n"
      "    i = i + 1;\n"
      "  }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}