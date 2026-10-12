// driver_fatal_compound_test.cpp —— 复合类型跨函数边界的 move/clone（fatal T）
//
// 回归背景（缺陷：结构体等复合类型含 own 指针并穿越函数边界）
//
// 浅拷贝（整块 memcpy）会把内嵌的 own 裸指针复制一份：源与目标各自在作用域退出
// 时递归销毁**同一堆块** → double free（实测 access violation，退出码
// 0xC0000005）。同一缺陷有两个入口，此前都无人拦截：
//   1. `move(x)` / `clone(x)`：op_move/op_clone 只处理 PTR/SLICE，复合类型落到
//      `value_clone` → struct_clone 的整块 memcpy（浅拷贝）；
//   2. 跨函数边界：`func read(b: Box)` + `read(a)` —— Box 的 ownership 是
//      OWN_NONE，参数校验里 `type_is_own(Box)` 为假，浅拷贝静默通过；返回值同理
//      （walk_return 的 `type_contains_own_or_fatal` 只覆盖 return）。
//
// 修法（本文件覆盖的契约）：
//   - **fatal T**（`fatal T` 类型表达式）：与 T 同布局、ownership=OWN_FATAL 的
//     将亡值变体，是含 own 资源的复合值跨函数边界的唯一载体；
//   - **move** = 整块 memcpy + 递归清空源顶层 own/fatal 指针（源不再持有）；
//   - **clone** = ptr_clone_block 递归深拷贝（own 字段分配独立新堆块），
//     源保持有效；
//   - 参数/返回值位置：目标是 fatal T（或经 fatal T 传参）→ 所有权转移；
//     目标持有 own 资源而实参不是将亡值 → 编译期报错（要求 move()/clone()）；
//   - 销毁：pop_scope → ptr_free_owned_recursive 逐字段递归（fatal 变体同布局，
//     ownership 不参与布局）。

#include <gtest/gtest.h>
#include <cstdio>
#include <string>

extern "C" {
#include "driver/driver.h"
}

#include "test_common.h"

namespace {

/* 临时源文件写到**当前工作目录**（而非系统临时目录）：测试环境可能限制
 * temp_directory_path 的写入（沙箱），而 cwd 是本仓库的构建目录，必然可写。
 * fopen 失败直接 FAIL，避免把 NULL 流喂给 fwrite（CRT 断言崩溃）。 */
std::string write_temp_file(const std::string &content) {
  static int counter = 0;
  std::string path = "clux_fatal_compound_" + std::to_string(counter++) + ".cx";
  FILE *fp = fopen(path.c_str(), "wb");
  if (!fp) {
    ADD_FAILURE() << "cannot open temp source file: " << path;
    return path;
  }
  fwrite(content.data(), 1, content.size(), fp);
  fclose(fp);
  return path;
}

/* 含一个 own 指针字段的结构体（缺陷的最小载体） */
const char *kBoxStruct =
    "struct Box {\n"
    "  p: own *i32;\n"
    "  n: i32;\n"
    "}\n";

} // namespace

/* ---- 1. 局部 move / clone：不再双释放 ---- */

TEST(DriverFatalCompound, MoveLocalStructDoesNotDoubleFree) {
  /* move 后源置空（不再持有堆块），目标独占；作用域退出只释放一次。
     修复前：整块浅拷贝 → 源与目标都持有同一堆块 → access violation。 */
  std::string path = write_temp_file(
      std::string(kBoxStruct) +
      "func main(): i32 {\n"
      "  var b: Box = .Box{ .p = new i32{7}, .n = 7 };\n"
      "  var c: Box = move(b);\n"
      "  if (c.n != 7) { return 1; }\n"
      "  if (c.p.* != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, CloneLocalStructIsDeepCopyWithValidSource) {
  /* clone 是**深拷贝**：源保持有效，产物独立（改产物不影响源，反之亦然）。 */
  std::string path = write_temp_file(
      std::string(kBoxStruct) +
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{11}, .n = 1 };\n"
      "  var b: Box = clone(a);\n"
      "  b.p.* = 22;\n"
      "  if (a.p.* != 11) { return 1; }\n"  /* 源未被产物改动 */
      "  if (b.p.* != 22) { return 2; }\n"
      "  a.p.* = 33;\n"
      "  if (b.p.* != 22) { return 3; }\n"  /* 产物未被源改动 */
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- 2. fatal 复合类型跨函数边界（本缺陷的正解） ---- */

TEST(DriverFatalCompound, FatalParamTransfersOwnership) {
  /* `func take(b: fatal Box)` + `take(move(a))`：所有权转移进被调用方，
     函数内 move(b) 接管为普通 Box，作用域退出只释放一次。 */
  std::string path = write_temp_file(
      std::string(kBoxStruct) +
      "func take(b: fatal Box): i32 {\n"
      "  var owned: Box = move(b);\n"
      "  return owned.n + owned.p.*;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{4}, .n = 3 };\n"
      "  if (take(move(a)) != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, FatalReturnTransfersOwnership) {
  /* `func mk(n): fatal Box` + `var c: Box = mk(9)`：被调用方 move 出去，
     调用方接管（fatal → Box 的接管是转移，不是拷贝）。 */
  std::string path = write_temp_file(
      std::string(kBoxStruct) +
      "func mk(n: i32): fatal Box {\n"
      "  var b: Box = .Box{ .p = new i32{n}, .n = n };\n"
      "  return move(b);\n"
      "}\n"
      "func main(): i32 {\n"
      "  var c: Box = mk(9);\n"
      "  if (c.n != 9) { return 1; }\n"
      "  if (c.p.* != 9) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, FatalParamCanBeReturnedForward) {
  /* fatal 参数的另一种合法消费点：`return b`（所有权继续传给调用方）。 */
  std::string path = write_temp_file(
      std::string(kBoxStruct) +
      "func id(b: fatal Box): fatal Box {\n"
      "  return b;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{5}, .n = 6 };\n"
      "  var c: Box = id(move(a));\n"
      "  if (c.p.* != 5 || c.n != 6) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- 3. 拒绝非法隐式拷贝（跨函数边界的安全闸门） ---- */

TEST(DriverFatalCompound, PlainStructArgumentIsRejected) {
  /* `func read(b: Box)` + `read(a)`：Box 含 own 字段，隐式拷贝会让调用方与
     被调用方共享内嵌堆块（修复前：access violation）。必须编译期报错。 */
  std::string path = write_temp_file(
      std::string(kBoxStruct) +
      "func read(b: Box): i32 {\n"
      "  return b.n;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{7}, .n = 7 };\n"
      "  return read(a);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, PlainStructReturnIsRejected) {
  /* `func mk(): Box { return b; }`（b 为含 own 字段的局部）：隐式拷贝返回 →
     被调用方作用域销毁后调用方拿到悬垂指针。必须报错（用 fatal Box + move）。 */
  std::string path = write_temp_file(
      std::string(kBoxStruct) +
      "func mk(): Box {\n"
      "  var b: Box = .Box{ .p = new i32{1}, .n = 1 };\n"
      "  return b;\n"
      "}\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, FatalVariableDeclarationIsRejected) {
  /* fatal 是将亡值——禁止命名（必须立即被接管，不能存进变量）。 */
  std::string path = write_temp_file(
      std::string(kBoxStruct) +
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{1}, .n = 1 };\n"
      "  var b: fatal Box = move(a);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, FatalParamMustBeConsumed) {
  /* R1 对复合 fatal 同样成立：fatal 参数未被 move/return 消费 → 报错。 */
  std::string path = write_temp_file(
      std::string(kBoxStruct) +
      "func take(b: fatal Box): i32 {\n"
      "  return b.n;\n"  /* 未消费 */
      "}\n"
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{1}, .n = 1 };\n"
      "  return take(move(a));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* ---- 4. 嵌套 / 其他复合形态 ---- */

TEST(DriverFatalCompound, NestedStructMoveAndClone) {
  /* 嵌套复合：Outer 含 Inner，Inner 含 own 字段。move/clone 需沿字段递归。 */
  std::string path = write_temp_file(
      "struct Inner { p: own *i32; }\n"
      "struct Outer { inner: Inner; n: i32; }\n"
      "func main(): i32 {\n"
      "  var o: Outer = .Outer{ .inner = .Inner{ .p = new i32{33} }, .n = 5 };\n"
      "  var o2: Outer = move(o);\n"
      "  if (o2.inner.p.* != 33 || o2.n != 5) { return 1; }\n"
      "  var o3: Outer = clone(o2);\n"
      "  o3.inner.p.* = 44;\n"
      "  if (o2.inner.p.* != 33) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, TupleWithOwnFieldMove) {
  /* 元组含 own 字段：move 元组 = 整块 memcpy + 清空源顶层 own。 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var t: <own *i32, i32> = .<own *i32, i32>{ new i32{44}, 6 };\n"
      "  var t2: <own *i32, i32> = move(t);\n"
      "  if (t2[0].* != 44 || t2[1] != 6) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, ArrayWithOwnElemCloneDeepCopies) {
  /* 数组元素含 own 字段：clone 数组逐元素深拷贝（元素间互不影响）。 */
  std::string path = write_temp_file(
      "struct Inner { p: own *i32; }\n"
      "func main(): i32 {\n"
      "  var arr: [2]Inner = .[2]Inner{ .Inner{ .p = new i32{55} },\n"
      "                                 .Inner{ .p = new i32{66} } };\n"
      "  var arr2: [2]Inner = clone(arr);\n"
      "  if (arr2[0].p.* != 55 || arr2[1].p.* != 66) { return 1; }\n"
      "  arr2[0].p.* = 77;\n"
      "  if (arr[0].p.* != 55) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, FatalArrayAcrossFunctionBoundary) {
  /* fatal 数组（含 own 元素）跨函数边界：`fatal [2]Inner` 形参 + 实参 move。 */
  std::string path = write_temp_file(
      "struct Inner { p: own *i32; }\n"
      "func sum(a: fatal [2]Inner): i32 {\n"
      "  var owned: [2]Inner = move(a);\n"
      "  return owned[0].p.* + owned[1].p.*;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var arr: [2]Inner = .[2]Inner{ .Inner{ .p = new i32{3} },\n"
      "                                 .Inner{ .p = new i32{4} } };\n"
      "  if (sum(move(arr)) != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- 5. fatal T 对无资源类型是恒等（不引入互不兼容的类型） ---- */

TEST(DriverFatalCompound, FatalOnResourceFreeTypeIsIdentity) {
  /* Point 不含 own 资源：`fatal Point` 折叠为 Point 本身——参数与普通 Point
     完全兼容（否则 `fatal Point` 形参会与 Point 实参类型不匹配）。 */
  std::string path = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "func sum(p: Point): i32 { return p.x + p.y; }\n"
      "func id(p: fatal Point): Point { return p; }\n"
      "func main(): i32 {\n"
      "  var p: Point = .Point{ .x = 3, .y = 4 };\n"
      "  if (sum(p) != 7) { return 1; }\n"
      "  if (id(p).x != 3) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, NonFatalOwnerCannotQualifyCompound) {
  /* `own T` / `ref T`（T 非指针）不合法：复合值没有"独占/借用"这回事，
     借用走 ref *T。parser 应直接报错。 */
  std::string path = write_temp_file(
      std::string(kBoxStruct) +
      "func main(): i32 {\n"
      "  var a: own Box = .Box{ .p = new i32{1}, .n = 1 };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* ---- 7. 含 ref 字段的返回值必须标注作用域（§5 强制规则） ----
 *
 * 与变量声明侧同一条规则：含 ref 指针的类型（顶层或嵌套）出现在返回值位置时，
 * 没有 `'<a,b> T` 标注就无法确定借用的存活上限 —— 调用方会拿到借用了被调用方
 * 局部/参数的悬垂指针（指针逃逸）。修复前只查顶层 `ref *T`，含 ref 字段的复合
 * 返回类型静默放行。
 */

TEST(DriverFatalCompound, StructWithRefFieldReturnRequiresAnnotation) {
  std::string path = write_temp_file(
      "struct Holder { r: ref *i32; n: i32; }\n"
      "func f(p: ref *i32): Holder {\n"
      "  return .Holder{ .r = p, .n = 1 };\n"
      "}\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, ArrayWithRefElemReturnRequiresAnnotation) {
  /* 嵌套（数组元素含 ref 的复合类型）同样必须标注 */
  std::string path = write_temp_file(
      "struct Holder { r: ref *i32; n: i32; }\n"
      "func f(p: ref *i32): [1]Holder {\n"
      "  return .[1]Holder{ .Holder{ .r = p, .n = 1 } };\n"
      "}\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, AnnotatedStructWithRefFieldReturnPasses) {
  /* 已标注 '<p> → 放行；调用方侧含 ref 字段的变量同样标注（§5 对声明与返回
     一视同仁），借自本作用域的 x 故用 '<x>。 */
  std::string path = write_temp_file(
      "struct Holder { r: ref *i32; n: i32; }\n"
      "func annot(p: ref *i32): '<p> Holder {\n"
      "  return .Holder{ .r = p, .n = 2 };\n"
      "}\n"
      "func main(): i32 {\n"
      "  var x: i32 = 41;\n"
      "  var h: '<x> Holder = annot(x.&);\n"
      "  if (h.n != 2) { return 1; }\n"
      "  if (h.r.* != 41) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverFatalCompound, PlainRefReturnStillRequiresAnnotation) {
  /* 顶层 ref 返回的既有规则不回归 */
  std::string path = write_temp_file(
      "func f(p: ref *i32): ref *i32 { return p; }\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* ---- 8. 借用路径不被误伤 ---- */

TEST(DriverFatalCompound, RefPointerBorrowingStillWorks) {
  /* 只想借用时用 `ref *Box`：不涉及所有权转移，读字段照常。 */
  std::string path = write_temp_file(
      std::string(kBoxStruct) +
      "func read(b: ref *Box): i32 { return b.n + b.p.*; }\n"
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{4}, .n = 3 };\n"
      "  if (read(a.&) != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
