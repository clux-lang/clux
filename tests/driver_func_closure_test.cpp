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

/* ================================================================ */
/* 闭包（closure）端到端：捕获列表 |a,(b:i32=expr)|                   */
/* ================================================================ */

TEST(Driver, RunFileClosureBasicLiteral) {
  /* 字面量纯 id 捕获：func |base| add(x) 捕获外层 base（clone），
     定义点绑定 → f(5) → 10 + 5 = 15 */
  std::string path = write_temp_file(
      "func make_adder():func(i32)->i32 {\n"
      "  var base = 10;\n"
      "  return func |base| add(x: i32): i32 { return base + x; };\n"
      "}\n"
      "func main():void {\n"
      "  var f = make_adder();\n"
      "  printf(\"%d\\n\", f(5));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureCloneSemantics) {
  /* 捕获是 clone 值而非引用：闭包创建后修改外层变量不影响闭包内捕获值 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var base = 10;\n"
      "  var f = func |base| add(x: i32): i32 { return base + x; };\n"
      "  base = 1000;\n"
      "  printf(\"%d\\n\", f(5));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureParenCapture) {
  /* 括号捕获（VALUE DECL 临时构造）：(b: i32 = c + d) 定义点求值，
     a 捕获外层 5，b 临时构造 107 → f(1) = 5 + 107 + 1 = 113 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var a = 5;\n"
      "  var c = 100;\n"
      "  var d = 7;\n"
      "  var f = func |a, (b: i32 = c + d)| add(x: i32): i32 { return a + b + x; };\n"
      "  printf(\"%d\\n\", f(1));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureLocalFunc) {
  /* 语句级局部函数带捕获：提升 + 定义点绑定 → f(1) = 42 + 1 = 43 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var x = 42;\n"
      "  func |x| f(v: i32): i32 { return x + v; }\n"
      "  printf(\"%d\\n\", f(1));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureTdzCompileTimeRejected) {
  /* 闭包捕获 TDZ：定义点前引用有捕获局部函数 → 编译期报错（exit≠0），
     不静默到运行期（捕获槽 undefined 占位参与运算报 "operator +"） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  printf(\"%d\\n\", f(1));\n"
      "  var x = 42;\n"
      "  func |x| f(v: i32): i32 { return x + v; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureTdzAfterDefinitionOk) {
  /* 定义点之后引用有捕获局部函数：捕获已绑定 → 正常输出 43 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var x = 42;\n"
      "  func |x| f(v: i32): i32 { return x + v; }\n"
      "  printf(\"%d\\n\", f(1));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureNoCaptureForwardRefOk) {
  /* 无捕获局部函数定义点前调用：hoist 只绑定地址，无捕获槽 → 合法 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  printf(\"%d\\n\", g(1));\n"
      "  func g(v: i32): i32 { return v + 1; }\n"
      "  printf(\"%d\\n\", g(1));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureLoopRebind) {
  /* 循环内重建闭包：每次迭代捕获当前 i 的 clone → 0*10 + 1*10 + 2*10 = 30 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var total = 0;\n"
      "  for (var i = 0; i < 3; i = i + 1) {\n"
      "    var f = func |i| mul(n: i32): i32 { return i * n; };\n"
      "    total = total + f(10);\n"
      "  }\n"
      "  printf(\"%d\\n\", total);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureCaptureUndefinedRejected) {
  /* 捕获列表中引用未定义变量 → 编译期诊断 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var f = func |nope| add(x: i32): i32 { return nope + x; };\n"
      "  var val = f(1);\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureFuncTypeCaptureRejected) {
  /* 函数类型签名不能带捕获列表（闭包不参与类型） */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var f: func |x| (i32)->i32 = func(x:i32):i32 { return x; };\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileClosureGlobalFuncCaptureRejected) {
  /* 全局函数不允许捕获列表 */
  std::string path = write_temp_file(
      "func |x| g(v: i32): i32 { return x + v; }\n"
      "func main():void {\n"
      "  var val = g(1);\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ================================================================ */
/* 全局变量（运行时实体，init 编译期折叠）                             */
/* ================================================================ */

TEST(Driver, RunFileGlobalVarRead) {
  /* 全局变量函数体内读取（经 func_vcall root_scope 接线） */
  std::string path = write_temp_file(
      "var g: i32 = 42;\n"
      "func main():i32 {\n"
      "  printf(\"%d\\n\", g);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileGlobalVarMutateAcrossFuncs) {
  /* 全局变量跨函数修改：setg 写 root_scope，main 读到新值 */
  std::string path = write_temp_file(
      "var g: i32 = 42;\n"
      "var s: str = \"hello\";\n"
      "func setg(v: i32):i32 { g = v; return 0; }\n"
      "func main():i32 {\n"
      "  printf(\"%d %s\\n\", g, s);\n"
      "  _ = setg(100);\n"
      "  printf(\"%d\\n\", g);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileGlobalVarFuncRefInit) {
  /* 全局变量 init 折叠为函数引用：var f = add; 调用 f */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 { return a + b; }\n"
      "var f = add;\n"
      "func main():i32 {\n"
      "  printf(\"%d\\n\", f(3, 4));\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileGlobalVarExprInit) {
  /* 全局变量 init 为编译期可计算表达式：折叠为字面量发射 */
  std::string path = write_temp_file(
      "var x = (1 + 2) * 3 - 4;\n"
      "func main():i32 {\n"
      "  printf(\"%d\\n\", x);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileGlobalVarInitNotConstantRejected) {
  /* 全局变量 init 引用其他全局变量（运行期实体）→ 编译期拒绝 */
  std::string path = write_temp_file(
      "var a: i32 = 1;\n"
      "var b: i32 = a;\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileGlobalVarUnknownTypeRejected) {
  /* 非法类型名（string 非内建，内建为 str）→ sema 拒绝，不落到运行时 */
  std::string path = write_temp_file(
      "var s: string = \"hi\";\n"
      "func main():i32 { return 0; }\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
