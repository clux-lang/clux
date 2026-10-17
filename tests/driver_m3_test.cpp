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
  /* own 变量在作用域退出点被显式释放（m3-design：销毁序列编译期生成）。
     覆盖三条退出路径——块尾 / return / break——以及"释放发生在块内变量上"。
     本用例同时锁定 FREE 的新形态：`FREE <expr>` 由 sema 生成 expr
     （简单指针 = 变量引用 → PUSH <name>），compiler 编译 expr 后发 FREE。
     若 FREE 退化成按名查表或 expr 未压栈，运行期会读到错误值或崩溃。 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var acc: i32 = 0;\n"
      "  {\n"                                  /* 块尾退出 */
      "    var a: own *i32 = new i32{1};\n"
      "    acc = acc + a.*;\n"
      "  }\n"
      "  for (var i: i32 = 0; i < 3; i = i + 1) {\n"
      "    var b: own *i32 = new i32{10};\n"
      "    acc = acc + b.*;\n"
      "    if (i == 1) { break; }\n"           /* break 退出（仍有块要释放） */
      "  }\n"
      "  {\n"
      "    var c: own *i32 = new i32{100};\n"
      "    acc = acc + c.*;\n"
      "    return acc - 131;\n"                /* return 退出（块内 own 未到块尾） */
      "  }\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

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

TEST(Driver, StepBOwnParamConsumedPasses) {
  /* 所有权模型 §0.5：**参数位用 `own` 接收所有权**（不是 `fatal`）。
     调用点以将亡值传入（`take(move(p))`：FATAL → 形参 OWN），
     函数内形参的值状态是 OWN，由函数负责释放。
     （`fatal` 只用于返回值——它描述的是"调用表达式"的值。） */
  std::string path = write_temp_file(
      "func take(p: own *i32): i32 {\n"
      "  return p.*;\n"
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

TEST(Driver, StepBFatalParamIsRejected) {
  /* §0.5：`fatal` **不能作形参**——它描述的是"函数调用表达式"整体的值，
     不是形参的接收能力。要接所有权请写 `own`。 */
  std::string path = write_temp_file(
      "func take(p: fatal *i32): i32 {\n"
      "  return p.*;\n"
      "}\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBFatalChainThroughReturnPasses) {
  /* §0.3/§0.5：`fatal` 返回的函数 → **调用表达式产 FATAL** → 可被 `own` 形参接收。
     完整链：mk_fatal() 产 FATAL → take(own) 接收为 OWN。 */
  std::string path = write_temp_file(
      "func mk_fatal(): fatal *i32 {\n"
      "  return new i32{5};\n"
      "}\n"
      "func take(p: own *i32): i32 {\n"
      "  return p.*;\n"
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

/* ---- own 指针的两处 move 约束 ----
 *
 * 约束 1：**聚合的 own 字段不能单独 move**。TDZ 是**符号级**的（只跟踪变量名），
 *   字段没有"局部 TDZ"——`move(s.p)` 把资源掏走却不留任何标记，可以重复执行。
 *   所以含 own 资源的字段/元素只能连**整个**聚合一起 move。
 *
 * 约束 2：**闭包捕获不能 move**（含捕获里的结构体）。捕获作用域由闭包的**所有
 *   调用共享**：move 把捕获槽标进 TDZ 后无法恢复，第二次调用读到已转移的槽位。
 *   闭包内只能用 clone / 借用。
 */

TEST(Driver, StepBClosureCaptureMoveRejected) {
  /* 约束 2：闭包内 move 捕获的 own 指针 → 拒绝（捕获槽由所有调用共享） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var f = func |(q: own *i32 = move(p))| get(): i32 {\n"
      "    var r: own *i32 = move(q);\n"
      "    return r.*;\n"
      "  };\n"
      "  return f();\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, StepBClosureLocalMoveStillAllowed) {
  /* 约束 3 的边界：闭包体内**自己定义**的局部变量（非捕获）仍可正常 move ——
     它的 TDZ 随该次调用消亡，不存在跨调用残留。
     注：本语言的闭包捕获列表**不能为空**（`func ||` 解析失败，实测
     `error: expected '('`），所以用一个带默认值的捕获槽建立合法闭包；
     被测的局部变量与该捕获无关。 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var f = func |(seed: i32 = 0)| get(): i32 {\n"
      "    var a: own *i32 = new i32{5};\n"
      "    var b: own *i32 = move(a);\n"   /* 局部变量（非捕获）：可 move */
      "    return b.* + seed;\n"
      "  };\n"
      "  return f() - 5;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
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

TEST(Driver, StepBRebindRefEscapeRejected) {  /* §5 重绑定逃逸校验：r 标注 '<p>（存活不超过 p），重绑定到 outer
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

TEST(Driver, StepBGlobalOwnRejected) {
  /* §10 全局规则（m3-design §10）：禁止全局 own（含任何嵌套位置）。
     全局变量类型 own *T → 编译错误（检查器失去确定性） */
  std::string path = write_temp_file(
      "var g: own *i32 = new i32{42};\n"
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

/* ---- M3 §12 RC 引用计数系（share/weak/upgrade）已移除 ---- */

TEST(Driver, AnonConstructFieldEvaluatedOnce) {
  /* 回归：匿名构造 `.{...}` 的推断分支曾漏 `return`，控制流继续落入具名构造
     分支，对同一批字段表达式**二次求值**——第二次求值看到第一次留下的
     `moved` 标记，于是含 move() 的字段被误报
     "variable 'p' used after move"。同样代码写具名构造 `.Cell{...}` 本来就正常，
     此测试锁定匿名形式必须与具名形式行为一致。 */
  std::string path = write_temp_file(
      "struct Cell { v: i32; }\n"
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{7};\n"
      "  var c: Cell = .{ .v = (move(p)).* };\n"
      "  if (c.v != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- R1 fatal 参数消费：return 出去 = 所有权继续传递（指针与切片两种）----
 *
 * 回归背景：`walk_return` 的"return fatal 参数 → 标记已消费"原先写在
 * `: ref *T` 的分支内部，因此只有 ref 返回才生效；fatal 指针/切片返回走不到
 * 那段，`func id(p: fatal []i32): fatal []i32 { return p; }` 被误报
 * "fatal parameter 'p' is not consumed"（诊断文本自己写的是
 * "take ownership with move() or return it"）。判据现与函数结束的
 * 未消费检查（PTR_FATAL || SLICE_FATAL）一致。 */
TEST(Driver, OwnSliceParamReturnedIsConsumed) {
  /* §0.5：`own []T` 作形参（参数位用 own，不是 fatal）。
     调用点 `id(move(s))`：FATAL → 形参 OWN；形参再作为 `fatal []T` 返回位交出。 */
  std::string path = write_temp_file(
      "func id(p: own []i32): fatal []i32 {\n"
      "  return move(p);\n"
      "}\n"
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 2, 9, 9);\n"
      "  var t: own []i32 = id(move(s));\n"
      "  if (t[1] != 9) { return 1; }\n"
      "  if (t[0] != 9) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}


TEST(Driver, FatalSliceParamNotConsumedStillRejected) {
  /* 只读取元素不算消费：p 仍存活 → 必须报错（修复不得放宽规则） */
  std::string path = write_temp_file(
      "func peek(p: fatal []i32): i32 {\n"
      "  return p[0];\n"
      "}\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
