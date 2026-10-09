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

/* ---- Stage ④: sema ---- */

TEST(Driver, RunFileSemaErrorReturnsOne) {
  /* 语义错误：实参类型不匹配（str → i32），sema 应快速失败返回 1 */
  std::string path =
      write_temp_file("func foo(a:i32): void { } func main(): void { foo(\"s\"); }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileValidSemaPassesReturnsZero) {
  /* 合法程序：带返回类型（:i32）+ 函数调用 + 变量推断，sema 全通过 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 { return a + b; }"
      "func main(): void { var x = add(1, 2); }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileBareBlockReturnsZero) {
  /* 裸块语句（独立作用域）应完整通过流水线 */
  std::string path = write_temp_file(
      "func main():i32 { var x = 1; { var y = 2; x = x + y; } return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEmptyBareBlockReturnsZero) {
  /* 空裸块语句也应完整通过流水线 */
  std::string path =
      write_temp_file("func main():i32 { var x = 1; { } return x; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileOptionalNilEndToEndPasses) {
  /* nil 端到端（M2 optional）：?func 变量 nil 初始化 + ==nil 判定 + 赋值 f1
     （T → ?T 隐式提升）+ SOME 分支 .! 解包后调用 + 再赋 nil */
  std::string path = write_temp_file(
      "func f1(a:i32):i32 { return a + 1; }"
      "func main():i32 {\n"
      "  var g:?func(i32)->i32 = nil;\n"
      "  if (g != nil) { return 1; }\n"
      "  g = f1;\n"
      "  if (g == nil) { return 2; }\n"
      "  if (g != nil) { var f = g.!; var r = f(10); if (r != 11) { return 3; } }\n"
      "  g = nil;\n"
      "  if (g != nil) { return 4; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileNilAsTypeAnnotationRejected) {
  /* var a:nil 应被拒绝：nil 是值字面量，不是类型名（M2 下 nil 仍非类型） */
  std::string path =
      write_temp_file("func main():i32 { var a:nil = nil; return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileOptionalStrEndToEndPasses) {
  /* ?str 端到端：nil 初始化 + 双向 ==nil 判定 + 赋值"world"（str → ?str 隐式
     提升）+ SOME 分支 .! 解包读取 + 再赋 nil */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var s:?str = nil;\n"
      "  if (s != nil) { return 1; }\n"
      "  if (nil != s) { return 2; }\n"
      "  s = \"world\";\n"
      "  if (s == nil) { return 5; }\n"
      "  if (s != nil) { var t:str = s.!; if (t != \"world\") { return 6; } }\n"
      "  s = nil;\n"
      "  if (s != nil) { return 7; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnwrapAssertEndToEndPasses) {
  /* .! assert 解包端到端：SOME 分支解包读取 inner 值参与运算返回 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var s:?str = nil;\n"
      "  s = \"world\";\n"
      "  if (s != nil) {\n"
      "    var t:str = s.!;\n"
      "    if (t != \"world\") { return 1; }\n"
      "  } else { return 2; }\n"
      "  var n:?i32 = 40;\n"
      "  if (n != nil) { var v:i32 = n.!; return v + 2; }\n"
      "  return 3;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnwrapAssertOnNonePanics) {
  /* .! 对 none 解包 → 运行期 panic（error 值，driver 返回非 0） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:?i32 = nil;\n"
      "  var v:i32 = x.!;\n"
      "  return v;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnwrapAssertOnNonOptionalRejected) {
  /* .! 作用于非 optional 值 → 编译期诊断 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:i32 = 5;\n"
      "  var v:i32 = x.!;\n"
      "  return v;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileUnwrapTryRejected) {
  /* .? try 语义未实现 → 编译期诊断 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:?i32 = 5;\n"
      "  var v = x.?;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileOptionalStrArrayNilElementPasses) {
  /* optional str 数组：显式 str 元素 + 显式 nil 元素（M2 无自动 0 填充）。
     e0 非 nil（SOME），e1/e2 为 nil */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var sarr: [3]?str = .[3]?str{ \"a\", nil, nil };\n"
      "  var e0:?str = sarr[0];\n"
      "  var e1:?str = sarr[1];\n"
      "  var e2:?str = sarr[2];\n"
      "  if (e0 == nil) { return 1; }\n"
      "  if (e1 != nil) { return 2; }\n"
      "  if (e2 != nil) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEmptyStatementsReturnsZero) {
  /* 单独分号空语句：顶层（函数定义后）、语句间、声明后均应通过流水线 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 { return a + b; };\n"
      "func main():i32 {\n"
      "  ;\n"
      "  var x:i32 = 1; ;\n"
      "  return add(x, 2);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileParamShadowsCaptureReturnsZero) {
  /* 参数位于 closure_scope 的子 scope：捕获 x 与参数 x 同名时参数遮蔽
     捕获（函数体 return x 命中参数，返回 7 而非捕获的 100） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:i32 = 100;\n"
      "  var f = func |x| (x:i32):i32 { return x; };\n"
      "  return f(7);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
