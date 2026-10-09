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

/* ---- tuple（元组）---- */

TEST(Driver, RunFileTupleConstructAndIndexPasses) {
  /* tuple 构造 + 下标读写 + 复合赋值端到端：
     - 具名构造 .<i32,i32>{ 1, 2 }
     - 下标读取 t[0] / 写入 t[0]=5 / 复合赋值 t[1]+=3 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var t: <i32, i32> = .<i32, i32> { 1, 2 };\n"
      "  if (t[0] != 1 || t[1] != 2) { return 1; }\n"
      "  t[0] = 5;\n"
      "  if (t[0] != 5) { return 1; }\n"
      "  t[1] += 3;\n"
      "  if (t[1] != 5) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTupleAnonConstructPasses) {
  /* 匿名构造 .{...}：按序推断匿名 tuple 类型（元素类型+顺序一致 → 同实例） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var q: <i32, i32> = .{ 7, 8 };\n"
      "  if (q[0] != 7 || q[1] != 8) { return 1; }\n"
      "  q = .{ 3, 4 };\n"
      "  if (q[0] != 3 || q[1] != 4) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTupleMixedElemPasses) {
  /* 异构元组：i32 + i64（C 对齐布局 + 隐式提升） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var h: <i32, i64> = .<i32, i64> { 1, 2000000000 };\n"
      "  if (h[0] != 1 || h[1] != 2000000000) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTupleNestedPasses) {
  /* 嵌套元组：元素类型可为元组（递归布局 + 链式下标） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var n: <<i32, i32>, i32> = .<<i32, i32>, i32> { .<i32, i32> { 3, 4 }, 9 };\n"
      "  if (n[0][0] != 3 || n[0][1] != 4 || n[1] != 9) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTupleAnonConstructInCallArgPasses) {
  /* 实参位置匿名构造端到端：参数类型已知 → 推断 */
  std::string path = write_temp_file(
      "func sum(p: <i32, i32>): i32 { return p[0] + p[1]; }\n"
      "func main(): i32 {\n"
      "  if (sum(.<i32, i32> { 1, 2 }) != 3) { return 1; }\n"
      "  if (sum(.{ 4, 5 }) != 9) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTupleEqPasses) {
  /* 判等：元素递归比较 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var a: <i32, i32> = .{ 5, 5 };\n"
      "  var b: <i32, i32> = .{ 5, 5 };\n"
      "  if (a != b) { return 1; }\n"
      "  b[1] = 6;\n"
      "  if (a == b) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTupleArrayCompatAssignPasses) {
  /* Tuple↔Array 布局兼容互转端到端：<i32,i32> ↔ [2]i32 可互赋值 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var t: <i32, i32> = .{ 1, 2 };\n"
      "  var a: [2]i32 = t;\n"
      "  if (a[0] != 1 || a[1] != 2) { return 1; }\n"
      "  var u: <i32, i32> = a;\n"
      "  if (u[0] != 1 || u[1] != 2) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTupleCountMismatchRejected) {
  /* 构造元素数 != 类型元素数 → 编译期拒绝 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var t: <i32, i32> = .<i32, i32> { 1 };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTupleNamedFieldRejected) {
  /* 元组元素匿名：具名字段 → 编译期拒绝 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var t: <i32, i32> = .<i32, i32> { .x = 1, .y = 2 };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTupleIndexOnNonTupleRejected) {
  /* 下标访问非数组/元组值 → 编译期拒绝 */
  std::string path = write_temp_file(
      "func main(): i32 { var x = 1; return x[0]; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- tag union（标签联合，struct 同构平铺字段）---- */

TEST(Driver, RunFileUnionConstructIsFieldAccessPasses) {
  /* union 构造 + is 判 tag + 字段读写端到端：
     - 平铺字段定义 union Shape { radius: f32; w: f32; h: f32; }
     - 单字段强制构造 .Shape{.radius = 1.5f32}（字段名 = member/tag 名）
     - `s is Member` 返回 bool（IS_TAG）
     - 字段读取 s.radius / 写入 s.radius = v */
  std::string path = write_temp_file(
      "union Shape {\n"
      "  radius: f32;\n"
      "  w: f32;\n"
      "  h: f32;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var s = .Shape{.radius = 1.5f32};\n"
      "  if (s is radius != true) { return 1; }\n"
      "  if (s is w != false)      { return 2; }\n"
      "  if (s is h != false)      { return 3; }\n"
      "  if (s.radius != 1.5f32)   { return 4; }\n"
      "  s.radius = 2.5f32;\n"
      "  if (s.radius != 2.5f32)   { return 5; }\n"
      "  var r = .Shape{.w = 3.0f32};\n"
      "  if (r is w != true)       { return 6; }\n"
      "  if (r is radius != false) { return 7; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnionTypedVarAssignPasses) {
  /* union 类型变量声明 + 同类型赋值 */
  std::string path = write_temp_file(
      "union Shape {\n"
      "  radius: f32;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var s: Shape = .Shape{.radius = 1.0f32};\n"
      "  var s2: Shape = s;\n"
      "  if (s2.radius != 1.0f32) { return 1; }\n"
      "  if (s2 is radius != true) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnionLocalDefPasses) {
  /* 局部 union（函数体内定义） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  union Shape {\n"
      "    radius: f32;\n"
      "  }\n"
      "  var s = .Shape{.radius = 1.0f32};\n"
      "  if (s is radius != true) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnionIsNonUnionRejected) {
  /* is 左操作数非 union → 编译期拒绝 */
  std::string path = write_temp_file(
      "func main(): i32 { var x = 1; if (x is radius) { return 1; } return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnionIsUnknownMemberRejected) {
  /* is 右操作数非 union member → 编译期拒绝 */
  std::string path = write_temp_file(
      "union Shape {\n"
      "  radius: f32;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var s = .Shape{.radius = 1.0f32};\n"
      "  if (s is width) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnionMemberCountMismatchRejected) {
  /* union 构造字段数 != 1 → 编译期拒绝（强制单字段构造） */
  std::string path = write_temp_file(
      "union Shape {\n"
      "  radius: f32;\n"
      "  w: f32;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var s = .Shape{.radius = 1.0f32, .w = 2.0f32};\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnionUnknownFieldRejected) {
  /* union 构造给出未知 member → 编译期拒绝 */
  std::string path = write_temp_file(
      "union Shape {\n"
      "  radius: f32;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var s = .Shape{.width = 1.0f32};\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnionPayloadOnPureTagRejected) {
  /* 构造字段类型与 member payload 不兼容 → 编译期拒绝：
     member radius 类型 f32，构造给 str 值不匹配 */
  std::string path = write_temp_file(
      "union Shape {\n"
      "  radius: f32;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var s = .Shape{.radius = \"s\"};\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnionFieldAccessMismatchTagPanics) {
  /* 字段访问 tag 不符 → 运行期 error（引擎级硬错误 = panic）：
     .Shape{.radius = 1.5f32} 构造后 tag=radius；访问 s.w（另一 member，
     tag 不符）→ panic。构造 + 正确 tag 访问先行验证。 */
  std::string path = write_temp_file(
      "union Shape {\n"
      "  radius: f32;\n"
      "  w: f32;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var s = .Shape{.radius = 1.5f32};\n"
      "  if (s is radius != true) { return 1; }\n"
      "  if (s.radius != 1.5f32)   { return 2; }\n"
      "  var t = s.w;              /* tag 不符 → 运行期 panic */\n"
      "  return 3;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- cunion（C 语义裸 union，无 tag，开发者自负安全）---- */

TEST(Driver, RunFileCUnionConstructSharedMemoryPasses) {
  /* cunion 构造 + 共享内存重解释端到端：
     - cunion U { i: i32; f: f32; b: bool } 平铺字段定义
     - 单字段强制构造 .U{.i = 42}
     - 字段读取 u.i / 写入 u.f = 1.5f32 后按 u.i 重解释（共享 offset 0）
     - 同类型变量赋值 */
  std::string path = write_temp_file(
      "cunion U {\n"
      "  i: i32;\n"
      "  f: f32;\n"
      "  b: bool;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var u = .U{.i = 42};\n"
      "  if (u.i != 42)          { return 1; }\n"
      "  if (u.f != 0.0f32)      { return 2; }\n"
      "  u.f = 1.5f32;\n"
      "  if (u.f != 1.5f32)      { return 3; }\n"
      "  u.i = 7;\n"
      "  if (u.i != 7)           { return 4; }\n"
      "  u.b = true;\n"
      "  if (u.b != true)        { return 5; }\n"
      "  var u2: U = u;\n"
      "  if (u2.i != 7)          { return 6; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileCUnionLocalDefPasses) {
  /* 局部 cunion（函数体内定义） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  cunion U {\n"
      "    i: i32;\n"
      "  }\n"
      "  var u = .U{.i = 5};\n"
      "  if (u.i != 5) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileCUnionEqMemcmpPasses) {
  /* memcmp 字节判等：同字节模式 == true；改一字节 != */
  std::string path = write_temp_file(
      "cunion U {\n"
      "  i: i32;\n"
      "  f: f32;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a = .U{.i = 100};\n"
      "  var b = .U{.i = 100};\n"
      "  var c = .U{.i = 101};\n"
      "  if (a == b != true)  { return 1; }\n"
      "  if (a != b != false) { return 2; }\n"
      "  if (a == c != false) { return 3; }\n"
      "  if (a != c != true)  { return 4; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileCUnionFieldCountMismatchRejected) {
  /* cunion 构造字段数 != 1 -> 编译期拒绝（强制单字段构造） */
  std::string path = write_temp_file(
      "cunion U {\n"
      "  i: i32;\n"
      "  f: f32;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var u = .U{.i = 1, .f = 2.0f32};\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileCUnionUnknownFieldRejected) {
  /* cunion 构造给出未知 member -> 编译期拒绝 */
  std::string path = write_temp_file(
      "cunion U {\n"
      "  i: i32;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var u = .U{.z = 1};\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileCUnionEmptyMemberListRejected) {
  /* 空 member 列表 cunion：定义合法（size=1 占位），但无 member 可构造 */
  std::string path = write_temp_file(
      "cunion Empty {\n"
      "}\n"
      "func main(): i32 {\n"
      "  var e = .Empty{.z = 1};\n"
      "  return 0;\n"
      "}\n");
  /* 空 cunion 无 member -> 构造未知 member 编译期拒绝 */
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
