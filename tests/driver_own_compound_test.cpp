// driver_own_compound_test.cpp —— 含 own 字段的复合类型遵守单所有权体系
//
// 所有权**扩散**（m3-design §3.1/§4/§7）：复合类型只要（递归地）含 own 指针，
// 它整体就进入单所有权体系——与 `own *T` 同规则：
//
//   1. **构造表达式** `.T{...}`（T 含 own 字段）产出 **fatal T**（新堆块 = 将亡值），
//      不能隐式拷贝：必须由 own 变量接管、或 move()/clone()、或作为 fatal 形参/
//      返回值的实际值；
//   2. **函数参数**含 own 字段时**只能写 `fatal T`**（按值传 T 会与调用方共享
//      内嵌堆块 → 双释放）；只想借用写 `ref *T`；
//   3. **变量声明/赋值禁止 copy**：只能由 fatal 将亡值初始化（构造表达式 /
//      move() / clone() / fatal 返回值）；
//   4. **参数位含 ref 字段必须标注作用域**（与变量声明、返回值同一条 §5 规则）；
//   5. **销毁**：pop_scope 只递归释放 **own** 指针指向的堆块；fatal 永不由销毁
//      路径释放（它的归宿是被接管或 `_ = ` 显式丢弃）——故 fatal 复合值与其
//      base 走同一条按字段偏移递归的路径，且不会 double-free。
//
// 覆盖 struct / tuple / union / option / array（cunion 刻意例外：dispose 是
// no-op，与所有权规则脱节）。

#include <gtest/gtest.h>
#include <cstdio>
#include <string>

extern "C" {
#include "driver/driver.h"
}

#include "test_common.h"

namespace {

std::string write_temp_file(const std::string &content) {
  static int counter = 0;
  std::string path = "clux_own_compound_" + std::to_string(counter++) + ".cx";
  FILE *fp = fopen(path.c_str(), "wb");
  if (!fp) {
    ADD_FAILURE() << "cannot open temp source file: " << path;
    return path;
  }
  fwrite(content.data(), 1, content.size(), fp);
  fclose(fp);
  return path;
}

const char *kBox = "struct Box { p: own *i32; n: i32; }\n";

} // namespace

/* ---- 1. 构造表达式产出 fatal T ---- */

TEST(DriverOwnCompound, ConstructProducesFatalCanFeedFatalParamDirectly) {
  /* `.Box{...}` 即 fatal Box：**不必**再写 move() 就能喂给 fatal Box 形参
     （修复前构造产普通 Box，会被"含 own 资源不可按值传参"拒绝）。 */
  std::string path = write_temp_file(
      std::string(kBox) +
      "func take(b: fatal Box): i32 {\n"
      "  var owned: Box = move(b);\n"
      "  return owned.p.* + owned.n;\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (take(.Box{ .p = new i32{3}, .n = 4 }) != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, ConstructIsReceivedByOwnVariableByTransfer) {
  /* fatal Box → 普通 Box 的接管是**转移**：源不再持有，作用域退出不双释放。 */
  std::string path = write_temp_file(
      std::string(kBox) +
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{5}, .n = 6 };\n"
      "  if (a.p.* != 5 || a.n != 6) { return 1; }\n"
      "  var b: Box = .Box{ .p = new i32{7}, .n = 8 };\n"
      "  if (b.p.* != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, ConstructCanBeReturnedAsFatal) {
  /* fatal 与 own 指针同规则：**将亡值不可隐式拷贝出函数**，要显式 move()
     （构造产 fatal Box，`return move(...)` 把它交出去）。 */
  std::string path = write_temp_file(
      std::string(kBox) +
      "func mk(n: i32): fatal Box { return move(.Box{ .p = new i32{n}, .n = n }); }\n"
      "func main(): i32 {\n"
      "  var c: Box = mk(9);\n"
      "  if (c.p.* != 9 || c.n != 9) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, ConstructReturnWithoutMoveIsRejected) {
  /* 直接 `return .Box{...}`（不 move）→ 隐式拷贝将亡值 → 编译错误。 */
  std::string path = write_temp_file(
      std::string(kBox) +
      "func mk(n: i32): fatal Box { return .Box{ .p = new i32{n}, .n = n }; }\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, NestedConstructFieldIsTransferredIn) {
  /* 内层构造是 fatal Inner，外层字段类型是 Inner → 字段赋值即接管（转移）。 */
  std::string path = write_temp_file(
      "struct Inner { p: own *i32; }\n"
      "struct Outer { inner: Inner; n: i32; }\n"
      "func main(): i32 {\n"
      "  var o: Outer = .Outer{ .inner = .Inner{ .p = new i32{11} }, .n = 1 };\n"
      "  if (o.inner.p.* != 11) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- 2. 函数参数：含 own 字段只能 fatal T ---- */

TEST(DriverOwnCompound, PlainOwningParamIsRejected) {
  std::string path = write_temp_file(
      std::string(kBox) +
      "func read(b: Box): i32 { return b.n; }\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, NestedOwningParamIsRejected) {
  /* 扩散是**递归**的：Outer 自己没有 own 字段，但 Inner 有 → 同样只能 fatal。 */
  std::string path = write_temp_file(
      "struct Inner { p: own *i32; }\n"
      "struct Outer { inner: Inner; n: i32; }\n"
      "func read(o: Outer): i32 { return o.n; }\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, FatalOwningParamIsAccepted) {
  std::string path = write_temp_file(
      std::string(kBox) +
      "func read(b: fatal Box): i32 {\n"
      "  var owned: Box = move(b);\n"
      "  return owned.n;\n"
      "}\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, RefPointerParamBorrowsOwningCompound) {
  /* 只想借用 → `ref *Box`，不涉及所有权转移。 */
  std::string path = write_temp_file(
      std::string(kBox) +
      "func read(b: ref *Box): i32 { return b.n + b.p.*; }\n"
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{4}, .n = 3 };\n"
      "  if (read(a.&) != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- 3. 变量声明/赋值禁止 copy ---- */

TEST(DriverOwnCompound, CopyFromNamedVariableIsRejected) {
  std::string path = write_temp_file(
      std::string(kBox) +
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{1}, .n = 1 };\n"
      "  var b: Box = a;\n"
      "  return b.n;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, CopyIsAllowedViaExplicitClone) {
  /* 显式 clone() 深拷贝：两个值各自独立，改一个不影响另一个。 */
  std::string path = write_temp_file(
      std::string(kBox) +
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{1}, .n = 1 };\n"
      "  var b: Box = clone(a);\n"
      "  b.p.* = 2;\n"
      "  if (a.p.* != 1) { return 1; }\n"
      "  if (b.p.* != 2) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, MoveTransfersAndInvalidatesSource) {
  std::string path = write_temp_file(
      std::string(kBox) +
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{1}, .n = 1 };\n"
      "  var b: Box = move(a);\n"
      "  if (b.p.* != 1) { return 1; }\n"
      "  return a.n;\n" /* move 后读源 → 编译错误 */
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* ---- 4. 参数位含 ref 字段必须标注作用域 ---- */

TEST(DriverOwnCompound, ParamWithRefFieldRequiresAnnotation) {
  std::string path = write_temp_file(
      "struct Holder { r: ref *i32; n: i32; }\n"
      "func f(h: Holder): i32 { return h.n; }\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, AnnotatedParamWithRefFieldPasses) {
  std::string path = write_temp_file(
      "struct Holder { r: ref *i32; n: i32; }\n"
      "func f(h: '<p> Holder, p: ref *i32): i32 { return h.r.* + h.n + p.*; }\n"
      "func main(): i32 {\n"
      "  var x: i32 = 7;\n"
      "  var h: '<x> Holder = .Holder{ .r = x.&, .n = 1 };\n"
      "  if (f(h, x.&) != 15) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, TopLevelRefParamNeedsNoAnnotation) {
  /* 顶层 `ref *T` 是"借用"本身的声明形式，不在强制标注之列
     （变量声明侧有同样的 type_is_ref 例外）。 */
  std::string path = write_temp_file(
      "func f(p: ref *i32, q: ref *i32): i32 { return p.* + q.*; }\n"
      "func main(): i32 {\n"
      "  var x: i32 = 3;\n"
      "  if (f(x.&, x.&) != 6) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- 5. 销毁：显式丢弃递归释放，接管后不双释放 ---- */

TEST(DriverOwnCompound, ExplicitDiscardFreesNestedOwnBlocks) {
  /* `_ = ` 显式丢弃 fatal 复合值 → DISPOSE 递归释放内嵌 own 堆块（不泄漏）。 */
  std::string path = write_temp_file(
      "struct Inner { p: own *i32; }\n"
      "struct Outer { inner: Inner; n: i32; }\n"
      "func main(): i32 {\n"
      "  _ = .Outer{ .inner = .Inner{ .p = new i32{1} }, .n = 1 };\n"
      "  var a: Outer = .Outer{ .inner = .Inner{ .p = new i32{2} }, .n = 2 };\n"
      "  _ = move(a);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, BareFatalExpressionStatementIsRejected) {
  /* 丢弃 fatal 必须显式 `_ = `（否则静默丢块 = 泄漏）。 */
  std::string path = write_temp_file(
      std::string(kBox) +
      "func main(): i32 {\n"
      "  .Box{ .p = new i32{1}, .n = 1 };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* ---- 6. 其他复合形态：tuple / option / array / union ---- */

TEST(DriverOwnCompound, UnionWithOwnPayloadConstructAndRead) {
  /* union 含 own payload：构造产 fatal U → own 变量接管，字段访问正常。
     回归：op_construct 的 union 分支原先 memcpy payload 后**没有清空源**，payload
     临时值在作用域退出时先释放内嵌堆块、union 成员随后再释放同一个块 →
     double-free（实测 access violation）。 */
  std::string path = write_temp_file(
      "union U { p: own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var a: U = .U{ .p = new i32{5} };\n"
      "  if (a.p.* != 5) { return 1; }\n"
      "  var b: U = move(a);\n"
      "  if (b.p.* != 5) { return 2; }\n"
      "  var c: U = .U{ .p = new i32{6} };\n"
      "  var d: U = clone(c);\n"
      "  if (d.p.* != 6 || c.p.* != 6) { return 3; }\n"
      "  _ = .U{ .p = new i32{7} };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, UnionWithOwnPayloadParamNeedsFatal) {
  std::string path = write_temp_file(
      "union U { p: own *i32; n: i32 }\n"
      "func bad(u: U): i32 { return u.n; }\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, UnionWithOwnPayloadViaFatalPasses) {
  std::string path = write_temp_file(
      "union U { p: own *i32; n: i32 }\n"
      "func take(u: fatal U): i32 {\n"
      "  var owned: U = move(u);\n"
      "  return owned.p.*;\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (take(.U{ .p = new i32{7} }) != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, UnionWithOwnPayloadCopyIsRejected) {
  std::string path = write_temp_file(
      "union U { p: own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var a: U = .U{ .p = new i32{1} };\n"
      "  var b: U = a;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, TupleWithOwnElemParamNeedsFatal) {
  std::string path = write_temp_file(
      "func bad(t: <own *i32, i32>): i32 { return t[1]; }\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, TupleWithOwnElemViaFatalPasses) {
  std::string path = write_temp_file(
      "func take(t: fatal <own *i32, i32>): i32 {\n"
      "  var owned: <own *i32, i32> = move(t);\n"
      "  return owned[0].* + owned[1];\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (take(.<own *i32, i32>{ new i32{3}, 4 }) != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, ArrayWithOwnElemParamNeedsFatal) {
  std::string path = write_temp_file(
      "struct Inner { p: own *i32; }\n"
      "func bad(a: [2]Inner): i32 { return a[0].p.*; }\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, OptionWithOwnInnerCopyIsRejected) {
  std::string path = write_temp_file(
      "struct Box { p: own *i32; n: i32; }\n"
      "func main(): i32 {\n"
      "  var a: Box = .Box{ .p = new i32{1}, .n = 1 };\n"
      "  var o: ?Box = a;\n" /* ?Box 含 own → copy 禁止 */
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* ---- 7. 平凡复合类型不受影响（fatal 对无资源类型无意义） ---- */

TEST(DriverOwnCompound, ResourceFreeCompoundIsStillFreelyCopyable) {
  std::string path = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "func sum(p: Point): i32 { return p.x + p.y; }\n"
      "func main(): i32 {\n"
      "  var a: Point = .Point{ .x = 3, .y = 4 };\n"
      "  var b: Point = a;\n"          /* 无 own → copy 合法 */
      "  if (sum(a) != 7) { return 1; }\n"
      "  if (sum(b) != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- 8. option 内层含 own：构造表达式作字段值（回归） ----
 *
 * `?I` 的字段语义是"内层提升为 some"，内层类型是**非 fatal** 的 `I`；但字段
 * 表达式本身往往是构造表达式（`.I{...}`），`ex_construct_fatal` 会把它标成
 * `fatal I` —— 归一**之前**这里误报 "cannot initialize optional ?I with fatal I"。
 *
 * 根因（两层）：
 *   1. sema 的 option 字段类型比对没有把 fatal 折回 base；
 *   2. 折回时"按同布局重新 intern 一个 OWN_NONE 变体"**不可靠**——**具名 struct
 *      标记 unique、不参与 intern 去重**，每次新建实例，指针永不等于原 base。
 *      正解是 `type_fatal_variant_of` 创建变体时登记 `fatal→base`（vm->fatal_bases），
 *      折回时查登记表拿**规范身份**。
 */
TEST(DriverOwnCompound, OptionWithOwnInnerInlinedConstruct) {
  std::string path = write_temp_file(
      "struct I { p: own *i32; }\n"
      "func main(): i32 {\n"
      "  var a: ?I = .?I{ .I{ .p = new i32{7} } };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, OptionWithOwnInnerInlinedConstructPlusScalar) {
  /* 内层同时带标量字段（字段表更大的情况） */
  std::string path = write_temp_file(
      "struct I { p: own *i32; n: i32; }\n"
      "struct M { i: I; n: i32; }\n"
      "func main(): i32 {\n"
      "  var a: ?I = .?I{ .I{ .p = new i32{7}, .n = 3 } };\n"
      "  var b: ?M = .?M{ .M{ .i = .I{ .p = new i32{8}, .n = 5 }, .n = 4 } };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, OptionWithFatalInnerVariableIsRejected) {
  /* 反例：字段值类型与内层**真的**不兼容（int 不是 I）仍须拒绝。
     归一化不能让"什么都能塞进 option"。 */
  std::string path = write_temp_file(
      "struct I { p: own *i32; }\n"
      "func main(): i32 {\n"
      "  var a: ?I = .?I{ 42 };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* ---- 9. fatal 聚合类型的返回值（array / union，回归） ----
 *
 * `func mk(): fatal [2]I` 的返回值路径上有**两处**会尝试把运行期 base 值 cast 到
 * 声明的 fatal 类型：
 *   - 第 6 步（返回值 safe_cast 到声明返回类型）——**方向非法**
 *     （各族的 implicit_cast 只认 fatal→base 的接管），对 array/union 直接失败：
 *     "array: implicit cast only within layout-compatible array/tuple"；
 *   - 第 7 步（fatal 返回值的**转移 + 重贴标签**）——正解。
 * 修法是让第 6 步把"fatal 且带资源"的声明返回类型**让给**第 7 步。
 * struct / 指针族此前靠 base→fatal 的布局兜底偶然通过，array/union 暴露了它。
 */
const char *kFatalArrayRet =
    "struct I { p: own *i32; }\n"
    "func mk(): fatal [2]I {\n"
    "  return move(.[2]I{ .I{ .p = new i32{7} }, .I{ .p = new i32{8} } });\n"
    "}\n";

TEST(DriverOwnCompound, FatalArrayReturnExplicitType) {
  std::string path = write_temp_file(
      std::string(kFatalArrayRet) +
      "func main(): i32 {\n"
      "  var o: [2]I = mk();\n"
      "  if (o[0].p.* != 7 || o[1].p.* != 8) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, FatalArrayReturnInferredType) {
  std::string path = write_temp_file(
      std::string(kFatalArrayRet) +
      "func main(): i32 {\n"
      "  var o = mk();\n"
      "  if (o[0].p.* != 7 || o[1].p.* != 8) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, FatalUnionReturn) {
  std::string path = write_temp_file(
      "union U { p: own *i32; n: i32 }\n"
      "func mk(): fatal U { return move(.U{ .p = new i32{7} }); }\n"
      "func main(): i32 {\n"
      "  var o: U = mk();\n"
      "  if (o.p.* != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, FatalTupleReturn) {
  std::string path = write_temp_file(
      "func mk(): fatal <own *i32, i32> {\n"
      "  return move(.<own *i32, i32>{ new i32{7}, 3 });\n"
      "}\n"
      "func main(): i32 {\n"
      "  var o: <own *i32, i32> = mk();\n"
      "  if (o[0].* != 7 || o[1] != 3) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnCompound, FatalArrayReturnForwarded) {
  /* fatal 形参原样返回（转发）：同样走第 6/7 步组合 */
  std::string path = write_temp_file(
      "struct I { p: own *i32; }\n"
      "func fwd(x: fatal [2]I): fatal [2]I { return x; }\n"
      "func main(): i32 {\n"
      "  var o: [2]I = fwd(.[2]I{ .I{ .p = new i32{7} }, .I{ .p = new i32{8} } });\n"
      "  if (o[0].p.* != 7 || o[1].p.* != 8) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

