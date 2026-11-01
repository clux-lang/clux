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

/* ---- struct（结构体）---- */

TEST(Driver, RunFileStructDeclAndVarPasses) {
  /* struct 声明 + 声明 struct 类型变量（无值构造，仅类型定义） */
  std::string path = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "func main():i32 {\n"
      "  var p: Point = undefined;\n"
      "  _ = libc::printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructLocalDefPasses) {
  /* 局部 struct（函数体内定义） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  struct Point { x: i32; y: i32; }\n"
      "  var p: Point = undefined;\n"
      "  _ = libc::printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructNestedFieldTypePasses) {
  /* 字段类型可为其他 struct 类型（布局依赖构造） */
  std::string path = write_temp_file(
      "struct Inner { a: i32; }\n"
      "struct Outer { i: Inner; b: i64; }\n"
      "func main():i32 {\n"
      "  var o: Outer = undefined;\n"
      "  _ = libc::printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructDuplicateFieldRejected) {
  /* 重复字段名 → 编译期拒绝 */
  std::string path = write_temp_file(
      "struct Point { x: i32; x: i64; }\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructUnknownFieldTypeRejected) {
  /* 未知字段类型 → 编译期拒绝 */
  std::string path = write_temp_file(
      "struct Point { x: Nope; }\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructEmptyFieldListPasses) {
  /* 空字段列表 {} 合法（C 语义 size=1）：可构造、可运行 */
  std::string path = write_temp_file(
      "struct Empty { }\n"
      "func main(): i32 {\n"
      "  var e: Empty = .Empty { };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructConstructFieldAccessPasses) {
  /* struct 构造 + 字段读写 + 复合赋值端到端：
     - 具名构造 .Point{ .x=1, .y=2 } 字段序可乱
     - 字段读取 p.x / 写入 p.x=5 / 复合赋值 p.y+=3
     - 匿名构造 .{...} 鸭子类型推断 */
  std::string path = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "func main(): i32 {\n"
      "  var p: Point = .Point { .x = 1, .y = 2 };\n"
      "  if (p.x != 1) { return 1; }\n"
      "  if (p.y != 2) { return 1; }\n"
      "  p.x = 5;\n"
      "  if (p.x != 5) { return 1; }\n"
      "  p.y += 3;\n"
      "  if (p.y != 5) { return 1; }\n"
      "  var q: Point = .{ .x = 7, .y = 8 };\n"
      "  if (q.x != 7 || q.y != 8) { return 1; }\n"
      "  var a: Point = .Point { .y = 2, .x = 1 };\n"
      "  if (a.x != 1 || a.y != 2) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructNestedFieldAccessPasses) {
  /* 嵌套 struct p.a.b 链式借用 + 写入 */
  std::string path = write_temp_file(
      "struct Inner { v: i32; }\n"
      "struct Outer { a: Inner; b: i32; }\n"
      "func main(): i32 {\n"
      "  var o: Outer = .Outer { .a = .Inner { .v = 3 }, .b = 9 };\n"
      "  if (o.a.v != 3) { return 1; }\n"
      "  if (o.b != 9) { return 1; }\n"
      "  o.a.v = 10;\n"
      "  if (o.a.v != 10) { return 1; }\n"
      "  if (o.b != 9) { return 1; }\n"
      "  o.a.v += 5;\n"
      "  if (o.a.v != 15) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructFieldAssignRejected) {
  /* 不存在的字段 → 编译期拒绝 */
  std::string path = write_temp_file(
      "struct Point { x: i32; }\n"
      "func main():i32 { var p: Point = .Point { .x = 1 }; p.z = 2; return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructNonStructFieldAccessRejected) {
  /* 非 struct 值取字段 → 编译期拒绝 */
  std::string path = write_temp_file(
      "func main():i32 { var x = 1; return x.a; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructFieldTypeMismatchRejected) {
  /* 字段赋值类型不匹配 → 编译期拒绝 */
  std::string path = write_temp_file(
      "struct Point { x: i32; }\n"
      "func main():i32 { var p: Point = .Point { .x = 1 }; p.x = \"s\"; return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructConstructUnknownFieldRejected) {
  /* 构造时给出不存在的字段 → 编译期拒绝 */
  std::string path = write_temp_file(
      "struct Point { x: i32; }\n"
      "func main():i32 { var p: Point = .Point { .z = 1 }; return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructLayoutCompatibleAssignPasses) {
  /* 结构兼容赋值端到端：两个具名 struct 字段名+类型+顺序一致 → 双向赋值，
     值正确落入字段 */
  std::string path = write_temp_file(
      "struct A { x: i32; y: i32; }\n"
      "struct B { x: i32; y: i32; }\n"
      "func main():i32 {\n"
      "  var a: A = .A { .x = 1, .y = 2 };\n"
      "  var b: B = .B { .x = 9, .y = 9 };\n"
      "  b = a;\n"
      "  if (b.x != 1 || b.y != 2) { return 1; }\n"
      "  b.x = 10;\n"
      "  a = b;\n"
      "  if (a.x != 10 || a.y != 2) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructLayoutMismatchAssignRejected) {
  /* 字段类型不同（i32 vs i64）→ 结构不兼容，赋值编译期拒绝 */
  std::string path = write_temp_file(
      "struct A { x: i32; }\n"
      "struct B { x: i64; }\n"
      "func main():i32 {\n"
      "  var a: A = .A { .x = 1 };\n"
      "  var b: B = .B { .x = 2 };\n"
      "  b = a;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructAnonConstructCreatesAnonTypePasses) {
  /* 匿名构造端到端：推断匿名类型 + 乱序具名字段重排 + 跨命名类型赋值 */
  std::string path = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "func main():i32 {\n"
      "  var p: Point = .{ .x = 1, .y = 2 };\n"
      "  if (p.x != 1 || p.y != 2) { return 1; }\n"
      "  var q: Point = .{ .y = 4, .x = 3 };\n"
      "  if (q.x != 3 || q.y != 4) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructAnonConstructOptionalFieldPasses) {
  /* 匿名构造 optional 字段：显式构造 .?i32{nil}/.?i32{42}（用户确认语义），
     值经变量取出判空/解包 */
  std::string path = write_temp_file(
      "struct Box { opt: ?i32; name: str; }\n"
      "func main():i32 {\n"
      "  var b: Box = .{ .opt = .?i32{nil}, .name = \"box\" };\n"
      "  var e: ?i32 = b.opt;\n"
      "  if (e != nil) { return 1; }\n"
      "  if (b.name != \"box\") { return 2; }\n"
      "  var c: Box = .{ .opt = .?i32{42}, .name = \"full\" };\n"
      "  var f: ?i32 = c.opt;\n"
      "  if (f == nil) { return 3; }\n"
      "  if (f.! != 42) { return 4; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructAnonConstructBareNilRejected) {
  /* 匿名构造 optional 字段裸 nil → 编译期引导显式构造 */
  std::string path = write_temp_file(
      "struct Box { opt: ?i32; }\n"
      "func main():i32 {\n"
      "  var b: Box = .{ .opt = nil };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructAnonConstructInCallArgPasses) {
  /* 实参位置匿名构造端到端：参数类型已知 → 推断 + 结构兼容转换，
     乱序字段按名匹配 */
  std::string path = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "func sum(p: Point):i32 { return p.x + p.y; }\n"
      "func main():i32 {\n"
      "  if (sum(.{ .x = 1, .y = 2 }) != 3) { return 1; }\n"
      "  if (sum(.{ .y = 4, .x = 5 }) != 9) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructAnonConstructInAssignRhsPasses) {
  /* 赋值 RHS 匿名构造端到端：左值类型已知 → 推断 + 结构兼容转换 */
  std::string path = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "func main():i32 {\n"
      "  var p: Point = .Point { .x = 0, .y = 0 };\n"
      "  p = .{ .x = 1, .y = 2 };\n"
      "  if (p.x != 1 || p.y != 2) { return 1; }\n"
      "  p = .{ .y = 4, .x = 3 };\n"
      "  if (p.x != 3 || p.y != 4) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* 聚合类型的拷贝语义（用户可见契约）：`var b = a` 是**拷贝**而非别名。
   回归背景：DEFINE/STORE 的"接管"守卫误把"变量的值"（ownership=OWN_OWN）当成
   fresh 临时值，导致 b 与 a 绑定同一 value 本体——改 b 会连带改 a。 */
TEST(Driver, RunFileStructCopyInitIsNotAlias) {
  std::string path = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "func main():i32 {\n"
      "  var a: Point = .Point { .x = 1, .y = 2 };\n"
      "  var b: Point = a;\n"          /* 必须是拷贝 */
      "  b.x = 9;\n"
      "  if (a.x != 1) { return 1; }\n" /* 别名实现下会读到 9 */
      "  if (b.x != 9) { return 2; }\n"
      /* 赋值语句同样不得别名 */
      "  var c: Point = .Point { .x = 3, .y = 4 };\n"
      "  var d: Point = .Point { .x = 5, .y = 6 };\n"
      "  c = d;\n"
      "  c.y = 8;\n"
      "  if (d.y != 6) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* 聚合类型的赋值在驱动层也不得泄漏：driver_run_file 内部的 allocator
   销毁时会向 stderr 报 leak（此处以退出码 + 无崩溃为主，泄漏由
   exec_test.cpp 的 ExecTest.Aggregate* 用例以分配器存活计数精确断言）。 */
TEST(Driver, RunFileAggregateAssignChain) {
  std::string path = write_temp_file(
      "struct Cell { v: i32; }\n"
      "func main():i32 {\n"
      "  var cells: [2]Cell = .[2]Cell{ .Cell{ .v = 1 }, .Cell{ .v = 2 } };\n"
      "  cells[0] = .Cell{ .v = 7 };\n"
      "  if (cells[0].v != 7) { return 1; }\n"
      "  var one: Cell = .Cell{ .v = 9 };\n"
      "  var two: Cell = one;\n"
      "  two.v = 11;\n"
      "  if (one.v != 9) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- struct 值相等（`==` / `!=`）----
 *
 * sema 一直允许 struct 值比较（VTABLE_STRUCT.eq 按字段递归），但 C 后端此前把
 * `a == b` 原样发给宿主编译器 → clang 报 "invalid operands to binary expression"。
 * 现在 C 后端为被比较的类型生成 `clux_eq_<类型>` 辅助函数（按字段递归；str 比
 * 内容、array 逐元素、option 先比 tag；其余类型按字节），下面覆盖各字段形态。 */

TEST(Driver, RunFileStructEqualityScalarFields) {
  std::string path = write_temp_file(
      "struct P { x: i32; y: i64; }\n"
      "func main():i32 {\n"
      "  var a: P = .P{ .x = 1, .y = 2 };\n"
      "  var b: P = .P{ .x = 1, .y = 2 };\n"
      "  var c: P = .P{ .x = 1, .y = 3 };\n"
      "  if (!(a == b)) { return 1; }\n"
      "  if (a != b) { return 2; }\n"
      "  if (a == c) { return 3; }\n"
      "  if (!(a != c)) { return 4; }\n"
      "  return 0;\n"
      "}\n");
  clux_expect_program_file_ok(path);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructEqualityStrFieldUsesContent) {
  /* str 字段必须比内容而不是 data 指针（同一内容经不同构造路径也要相等）。 */
  std::string path = write_temp_file(
      "struct S { name: str; n: i32; }\n"
      "func mk(name: str): S { return .S{ .name = name, .n = 1 }; }\n"
      "func main():i32 {\n"
      "  var a: S = mk(\"clux\");\n"
      "  var b: S = mk(\"clux\");\n"
      "  var c: S = mk(\"clux!\") ;\n"
      "  if (!(a == b)) { return 1; }\n"
      "  if (a == c) { return 2; }\n"
      "  if (!(a != c)) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  clux_expect_program_file_ok(path);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructEqualityNestedAndArrayFields) {
  std::string path = write_temp_file(
      "struct Inner { a: i32; b: i64; }\n"
      "struct Outer { inner: Inner; xs: [3]i32; }\n"
      "func main():i32 {\n"
      "  var a: Outer = .Outer{ .inner = .Inner{ .a = 1, .b = 2 },\n"
      "                         .xs = .[3]i32{ 4, 5, 6 } };\n"
      "  var b: Outer = a;\n"
      "  if (!(a == b)) { return 1; }\n"
      "  var c: Outer = a;\n"
      "  c.inner.b = 3;\n"     /* 嵌套 struct 字段差异 */
      "  if (a == c) { return 2; }\n"
      "  var d: Outer = a;\n"
      "  d.xs[2] = 7;\n"       /* 数组字段差异 */
      "  if (a == d) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  clux_expect_program_file_ok(path);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructEqualityOptionalAndEmptyFields) {
  std::string path = write_temp_file(
      "struct Empty { }\n"
      "struct O { v: ?i32; }\n"
      "func main():i32 {\n"
      "  var e1: Empty = .Empty{};\n"
      "  var e2: Empty = .Empty{};\n"
      "  if (!(e1 == e2)) { return 1; }\n"
      "  var a: O = .O{ .v = 7 };\n"
      "  var b: O = .O{ .v = 7 };\n"
      "  var none: O = .O{ .v = nil };\n"
      "  if (!(a == b)) { return 2; }\n"
      "  if (a == none) { return 3; }\n"   /* tag 不同 */
      "  var c: O = .O{ .v = 8 };\n"
      "  if (a == c) { return 4; }\n"      /* 载荷不同 */
      "  return 0;\n"
      "}\n");
  clux_expect_program_file_ok(path);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructEqualityRvalueOperands) {
  /* 右值操作数（调用结果）不能取地址：后端先物化成临时左值。 */
  std::string path = write_temp_file(
      "struct P { x: i32; }\n"
      "func mk(x: i32): P { return .P{ .x = x }; }\n"
      "func main():i32 {\n"
      "  if (!(mk(1) == mk(1))) { return 1; }\n"
      "  if (mk(1) == mk(2)) { return 2; }\n"
      "  if (!(mk(1) != mk(2))) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  clux_expect_program_file_ok(path);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructEqualityDuckCompatibleTypes) {
  /* 结构兼容（字段名+类型+顺序一致）的不同具名 struct：sema 允许比较，
     后端按左值类型重解释后逐字段比较。 */
  std::string path = write_temp_file(
      "struct A { x: i32; s: str; }\n"
      "struct B { x: i32; s: str; }\n"
      "func main():i32 {\n"
      "  var a: A = .A{ .x = 1, .s = \"q\" };\n"
      "  var b: B = .B{ .x = 1, .s = \"q\" };\n"
      "  if (!(a == b)) { return 1; }\n"
      "  var c: B = .B{ .x = 2, .s = \"q\" };\n"
      "  if (a == c) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  clux_expect_program_file_ok(path);
  std::remove(path.c_str());
}

TEST(Driver, RunFileStructEqualityBitfieldType) {
  /* bitfield 类型没有成员级判等语义：整块按字节比较（不走字段辅助函数）。 */
  std::string path = write_temp_file(
      "bitfield Flags: u32 { a: 3; b: 5; }\n"
      "func main():i32 {\n"
      "  var f1: Flags = .Flags{ .a = 3, .b = 9 };\n"
      "  var f2: Flags = .Flags{ .a = 3, .b = 9 };\n"
      "  var f3: Flags = .Flags{ .a = 3, .b = 8 };\n"
      "  if (!(f1 == f2)) { return 1; }\n"
      "  if (f1 == f3) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  clux_expect_program_file_ok(path);
  std::remove(path.c_str());
}
