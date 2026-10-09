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

/* ---- extends：编译期类型计算 ---- */

TEST(Driver, RunFileExtendsCompileTimeFold) {
  /* extends 是纯编译期运算：sema 阶段 ctfe 求值后折叠为 bool 常量，
     运行期零指令。同类型/数组同型/const 兼容 → true；不同类型/长度不同 → false。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = i32 extends i32;\n"
      "  var b = i32 extends i64;\n"
      "  var c = [2]i32 extends [2]i32;\n"
      "  var d = [2]i32 extends [3]i32;\n"
      "  var e = const i32 extends i32;\n"
      "  if (a && !b && c && !d && e) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileExtendsFoldToBoolConst) {
  /* 折叠验证：反汇编 .cxs 中 extends 表达式应全部为 PUSH_BOOL 常量，
     不残留任何运行时类型计算指令。 */
  auto dir = std::filesystem::temp_directory_path() / "clux_ext_fold";
  std::filesystem::create_directories(dir);
  std::string src = (dir / "prog.cx").string();
  std::string cxs = (dir / "prog.cxs").string();

  {
    FILE *fp = fopen(src.c_str(), "wb");
    const char *code = "func main():i32 {\n"
                       "  var a = i32 extends i32;\n"
                       "  var b = i32 extends i64;\n"
                       "  if (a && !b) { return 1; }\n"
                       "  return 0;\n"
                       "}\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }

  ASSERT_EQ(driver_build_asm(src.c_str(), cxs.c_str()), 0);
  std::string text = slurp(cxs);
  ASSERT_FALSE(text.empty());
  EXPECT_NE(text.find("PUSH_BOOL"), std::string::npos);
  /* extends 无字节码助记符（纯编译期），折叠后也不得出现 LOAD_TYPE 拉取 */
  EXPECT_EQ(text.find("EXTENDS"), std::string::npos);

  std::filesystem::remove_all(dir);
}

TEST(Driver, RunFileExtendsNonTypeRejected) {
  /* 非类型操作数：sema 阶段 ctfe 求值失败（value_extends → "extends: type
     value required"），编译报错退出 1。 */
  std::string path = write_temp_file("func main():i32 {\n"
                                     "  var a = 5 extends i32;\n"
                                     "  return a;\n"
                                     "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* ---- extends + 三元选择类型：type RHS 编译期折叠 ---- */

TEST(Driver, RunFileTypeDefExtendsTernary) {
  /* 需求 2 核心：type RHS 用 extends + 三元选择类型。extends 二元遇到
     即折叠为 bool（ctfe 求值），三元按真实 bool 惰性选分支——选中的分支
     是类型值，整个 rhs 收敛为 LOAD_TYPE。真/假两分支与内建/别名/数组
     操作数全覆盖。 */
  std::string path = write_temp_file(
      "type A = i32;\n"
      "type B = i64;\n"
      "type T = A extends i32 ? B : A;\n"   // true → i64
      "type U = A extends i64 ? B : A;\n"   // false → i32
      "type V = B extends i64 ? i32 : f64;\n" // true → i32
      "func main():i32 {\n"
      "  var t:T = 100;\n"
      "  var u:U = 5;\n"
      "  var v:V = 3;\n"
      "  if (t != 100 || u != 5 || v != 3) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefExtendsTernaryFalseBranch) {
  /* extends 条件为假选 else 分支：别名操作数 + f32/f64 分支类型区分。
     Alt=f64：Alt extends i32 为 false → 选 f32（f64 字面量赋 f32 会报错，
     用 1.5f32 验证类型确实选中 f32 分支）。 */
  std::string path = write_temp_file(
      "type Alt = f64;\n"
      "type P = Alt extends i32 ? i64 : f32;\n"   // false → f32
      "type Q = Alt extends f64 ? i64 : f32;\n"   // true → i64
      "func main():i32 {\n"
      "  var p:P = 1.5f32;\n"
      "  var q:Q = 2;\n"
      "  if (p != 1.5 || q != 2) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefExtendsTernaryArray) {
  /* 数组操作数：同型 extends true，长度不同 false → 三元选对应分支 */
  std::string path = write_temp_file(
      "type A = [2]i32;\n"
      "type T = A extends [2]i32 ? i64 : f32;\n"  // true → i64
      "type U = A extends [3]i32 ? i64 : f32;\n"  // false → f32
      "func main():i32 {\n"
      "  var t:T = 2;\n"
      "  var u:U = 1.5f32;\n"
      "  if (t != 2 || u != 1.5) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefExtendsTernarySignature) {
  /* 选择出的类型用于函数签名（参数/返回类型），别名透明 */
  std::string path = write_temp_file(
      "type A = i32;\n"
      "type R = A extends i32 ? i64 : i32;\n"
      "func double_it(v:R):R { return v * 2; }\n"
      "func main():i32 {\n"
      "  var r = double_it(21);\n"
      "  if (r != 42) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefExtendsTernaryAliasChain) {
  /* 三元两分支引用其它 type def（别名链），选中的分支即目标类型 */
  std::string path = write_temp_file(
      "type Base = i32;\n"
      "type Alt = f64;\n"
      "type Pick1 = Base extends i32 ? Base : Alt;\n"  // true → i32
      "type Pick2 = Alt extends i32 ? Base : Alt;\n"   // false → f64
      "func main():i32 {\n"
      "  var p1:Pick1 = 3;\n"
      "  var p2:Pick2 = 1.5f64;\n"
      "  if (p1 != 3 || p2 != 1.5) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- extends + 三元混合运算（普通值上下文） ---- */

TEST(Driver, RunFileExtendsTernaryMixedValue) {
  /* 用户场景：var val = i32 extends i32 ? 1 : 0。extends 折叠为 bool
     字面量 → 三元 cond 编译期确定 → 运行期返回选中分支值。内建/别名/
     数组操作数 + 真/假分支全覆盖。 */
  std::string path = write_temp_file(
      "type A = i32;\n"
      "type B = f64;\n"
      "func main():i32 {\n"
      "  var val  = i32 extends i32 ? 1 : 0;\n"
      "  var val2 = i32 extends i64 ? 1 : 0;\n"
      "  var val3 = [2]i32 extends [2]i32 ? 1 : 0;\n"
      "  var val4 = [2]i32 extends [3]i32 ? 1 : 0;\n"
      "  var a = A extends i32 ? 100 : 200;\n"
      "  var b = A extends f64 ? 100 : 200;\n"
      "  var c = B extends f64 ? 1 : 0;\n"
      "  if (val != 1 || val2 != 0 || val3 != 1 || val4 != 0 ||\n"
      "      a != 100 || b != 200 || c != 1) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileExtendsTernaryNestedAndIf) {
  /* extends 折叠后用于嵌套三元与 if 条件：cond 均编译期确定 */
  std::string path = write_temp_file(
      "type A = i32;\n"
      "type B = f64;\n"
      "func main():i32 {\n"
      "  var d = (A extends i32) ? (B extends f64 ? 5 : 6) : 7;\n"
      "  var r:i32 = 0;\n"
      "  if (A extends i32) { r = 42; }\n"
      "  if (d != 5 || r != 42) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
