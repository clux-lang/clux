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

/* ---- switch 语句端到端（docs m2-design §4：if 语法糖） ---- */

TEST(Driver, RunFileSwitchMatchReturnsCase) {
  /* 基本匹配：命中 case 分支执行并返回 */
  std::string path = write_temp_file(
      "func pick(x:i32):i32 {\n"
      "  switch (x) {\n"
      "    (1)->{ return 10; }\n"
      "    (2)->{ return 20; }\n"
      "    default->{ return 30; }\n"
      "  }\n"
      "}\n"
      "func main():i32 {\n"
      "  if (pick(1) != 10) { return 1; }\n"
      "  if (pick(2) != 20) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchDefaultFallback) {
  /* default 兜底：未命中任何 case 走 default */
  std::string path = write_temp_file(
      "func pick(x:i32):i32 {\n"
      "  switch (x) {\n"
      "    (1)->{ return 10; }\n"
      "    (2)->{ return 20; }\n"
      "    default->{ return 30; }\n"
      "  }\n"
      "}\n"
      "func main():i32 {\n"
      "  if (pick(7) != 30) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchMultiPattern) {
  /* 多模式（逗号 = || 链）：命中任一模式即执行该分支 */
  std::string path = write_temp_file(
      "func pick(x:i32):i32 {\n"
      "  switch (x) {\n"
      "    (1, 2)->{ return 10; }\n"
      "    (3, 4, 5)->{ return 20; }\n"
      "    default->{ return 30; }\n"
      "  }\n"
      "}\n"
      "func main():i32 {\n"
      "  if (pick(1) != 10) { return 1; }\n"
      "  if (pick(2) != 10) { return 2; }\n"
      "  if (pick(4) != 20) { return 3; }\n"
      "  if (pick(5) != 20) { return 4; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchNoFallthrough) {
  /* 无 fallthrough：命中分支后不落入后续分支（default 不被执行） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var r = 0;\n"
      "  switch (1) {\n"
      "    (1)->{ r = 100; }\n"
      "    (2)->{ r = 200; }\n"
      "    default->{ r = 300; }\n"
      "  }\n"
      "  if (r != 100) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchRuntimeCond) {
  /* 运行期条件（函数调用 + 算术）：文档定义 switch 条件为运行期表达式 */
  std::string path = write_temp_file(
      "func f():i32 { return 3; }\n"
      "func main():i32 {\n"
      "  var x = 2;\n"
      "  var r = 0;\n"
      "  switch (x + f()) {\n"
      "    (5)->{ r = 50; }\n"
      "    default->{ r = 99; }\n"
      "  }\n"
      "  if (r != 50) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchNested) {
  /* 嵌套 switch：内外层独立判定 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x = 1;\n"
      "  var y = 2;\n"
      "  var r = 0;\n"
      "  switch (x) {\n"
      "    (1)->{\n"
      "      switch (y) {\n"
      "        (2)->{ r = 12; }\n"
      "        default->{ r = 19; }\n"
      "      }\n"
      "    }\n"
      "    default->{ r = 90; }\n"
      "  }\n"
      "  if (r != 12) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchStrCond) {
  /* str 条件：模式可为字符串字面量 */
  std::string path = write_temp_file(
      "func pick(s:str):i32 {\n"
      "  switch (s) {\n"
      "    (\"one\")->{ return 1; }\n"
      "    (\"two\", \"three\")->{ return 23; }\n"
      "    default->{ return 0; }\n"
      "  }\n"
      "}\n"
      "func main():i32 {\n"
      "  if (pick(\"one\") != 1) { return 1; }\n"
      "  if (pick(\"three\") != 23) { return 2; }\n"
      "  if (pick(\"zzz\") != 0) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchDefiniteAssignAllPaths) {
  /* 有 default：全分支赋值 → 确定性初始化，switch 后读取合法 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x = 2;\n"
      "  var r:i32 = undefined;\n"
      "  switch (x) {\n"
      "    (1)->{ r = 10; }\n"
      "    (2)->{ r = 20; }\n"
      "    default->{ r = 30; }\n"
      "  }\n"
      "  if (r != 20) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchTypeMismatchRejected) {
  /* 模式与 cond 类型不可比 → 编译期拒绝，运行时不产出 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x = 2;\n"
      "  switch (x) {\n"
      "    (\"a\")->{ return 1; }\n"
      "    default->{ return 2; }\n"
      "  }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSwitchDuplicateDefaultRejected) {
  /* 重复 default → 编译期拒绝 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x = 2;\n"
      "  switch (x) {\n"
      "    (1)->{ return 1; }\n"
      "    default->{ return 2; }\n"
      "    default->{ return 3; }\n"
      "  }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}




/* ---- enum（枚举）---- */

TEST(Driver, RunFileEnumDeclAndComparePasses) {
  /* enum 声明 + main 内 var 初始化 + 同 enum 判等（值断言写在程序内） */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1, Green = 2, Blue = 3 }\n"
      "func main():i32 {\n"
      "  var c: Color = Color::Red;\n"
      "  if (c == Color::Red) { printf(\"ok\\n\"); return 0; }\n"
      "  return 1;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumTwoStepCastPasses) {
  /* enum → 声明底层 i32 → i8 两步 cast 通过 */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1 }\n"
      "func main():i32 {\n"
      "  var c: Color = Color::Red;\n"
      "  var v: i8 = c as i32 as i8;\n"
      "  if (v == 1) { printf(\"ok\\n\"); return 0; }\n"
      "  return 1;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumMultiVariantCompare) {
  /* 多 variant 判等：Red==Green → false 分支 */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1, Green = 2 }\n"
      "func main():i32 {\n"
      "  var c: Color = Color::Green;\n"
      "  if (c == Color::Red) { return 1; }\n"
      "  if (c != Color::Green) { return 2; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumGlobalVarInit) {
  /* 全局 enum 变量 init 折叠（Color::Red 折叠为 AST_ENUM_REF 字面量） */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1, Green = 2 }\n"
      "var g: Color = Color::Green;\n"
      "func main():i32 {\n"
      "  if (g == Color::Green) { printf(\"ok\\n\"); return 0; }\n"
      "  return 1;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumU8UnderlyingPasses) {
  /* u8 底层 + 值超出底层范围（300 超出 u8）→ 编译期拒绝 */
  std::string path = write_temp_file(
      "enum Color:u8 { Red = 300 }\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumStrictSeparationRejected) {
  /* 严格分离：enum vs 底层 i32 判等 → 编译期拒绝 */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1 }\n"
      "func main():i32 {\n"
      "  var c: Color = Color::Red;\n"
      "  if (c == 1) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumVariantValueNarrowingRejected) {
  /* variant 值与底层不兼容（i32 字面量赋给 i8 底层）→ 编译期拒绝 */
  std::string path = write_temp_file(
      "enum Color:i8 { Red = 300 }\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumSkipStepCastRejected) {
  /* 跳步 cast（enum → i8 而非声明底层 i32）→ 编译期拒绝 */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1 }\n"
      "func main():i32 {\n"
      "  var c: Color = Color::Red;\n"
      "  var v: i8 = c as i8;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumAssignIntRejected) {
  /* enum 变量被整型赋值（i32 → enum 无隐式转换）→ 编译期拒绝 */
  std::string path = write_temp_file(
      "enum Color:i32 { Red = 1 }\n"
      "func main():i32 {\n"
      "  var c: Color = Color::Red;\n"
      "  c = 5;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumLocalDefInFunc) {
  /* 局部 enum（函数体内定义）：声明/使用/判等/两步 cast/三元组合 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  enum Color:i32 { Red = 1, Green = 2, Blue = 3 }\n"
      "  var c: Color = Color::Red;\n"
      "  if (c == Color::Red) {} else { return 1; }\n"
      "  var v: i8 = c as i32 as i8;\n"
      "  if (v != 1) { return 2; }\n"
      "  var tag: i32 = (c == Color::Blue) ? 100 : 200;\n"
      "  if (tag != 200) { return 3; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumLocalNestedBlocksShadow) {
  /* 局部 enum 在嵌套块中定义 + 同名遮蔽（外层全局 vs 内层块） */
  std::string path = write_temp_file(
      "enum A:i32 { X = 1, Y = 2 }\n"
      "func main():i32 {\n"
      "  var total: i32 = 0;\n"
      "  {\n"
      "    enum A:i32 { X = 10, Y = 20 }\n"
      "    var a: A = A::Y;\n"
      "    total += a as i32;\n"
      "  }\n"
      "  var g: A = A::X;\n"
      "  total += g as i32;\n"
      "  if (total != 21) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumLocalForwardRef) {
  /* 局部 enum 前向引用（提升语义：名字整个块内可见，与局部 type def 一致） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var c: Color = Color::Red;\n"
      "  enum Color:i32 { Red = 1, Green = 2 }\n"
      "  if (c != Color::Red) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumLocalInLoopBody) {
  /* 局部 enum 在 for 循环体内：每轮定义独立（运行时名字绑定在块作用域） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  for (var i: i32 = 0; i < 2; i = i + 1) {\n"
      "    enum E:i32 { A = 1, B = 2 }\n"
      "    var e: E = E::B;\n"
      "    if (e != E::B) { return 1; }\n"
      "  }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumLocalStrictSeparationRejected) {
  /* 局部 enum 严格分离同样生效：variant 不能赋给整型变量 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  enum Color:i32 { Red = 1 }\n"
      "  var x: i32 = Color::Red;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEnumLocalVariantNotConstRejected) {
  /* 局部 enum variant 值引用运行时变量 → 编译期拒绝（须编译期常量） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var n: i32 = 5;\n"
      "  enum Color:i32 { Red = n }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
