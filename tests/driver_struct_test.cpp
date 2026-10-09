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

/* ---- struct（结构体）---- */

TEST(Driver, RunFileStructDeclAndVarPasses) {
  /* struct 声明 + 声明 struct 类型变量（无值构造，仅类型定义） */
  std::string path = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "func main():i32 {\n"
      "  var p: Point = undefined;\n"
      "  printf(\"ok\\n\");\n"
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
      "  printf(\"ok\\n\");\n"
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
      "  printf(\"ok\\n\");\n"
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
