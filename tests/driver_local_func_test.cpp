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
/* 局部函数（local function）端到端                                      */
/* ================================================================ */

TEST(Driver, RunFileLocalFuncBasic) {
  /* 局部函数定义 + 调用：函数体内块作用域注册，运行期块入口提升 DEFINE，
     调用经 PUSH name 作用域查找（外层函数体内可见） */
  std::string path = write_temp_file(
      "func outer(n:i32):i32 {\n"
      "  func inc(x:i32): i32 { return x + 1; }\n"
      "  func dbl(x:i32): i32 { return x * 2; }\n"
      "  return inc(dbl(n));\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(10);\n"   /* inc(dbl(10)) = 21 */
      "  _ = r;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncHoistedForwardReference) {
  /* 提升语义：局部函数定义在后，外层函数体内调用先于定义点（块入口
     先 DEFINE 全部局部函数，前向引用安全） */
  std::string path = write_temp_file(
      "func outer(n:i32):i32 {\n"
      "  var r = caller(n);\n" /* 调用先于定义 */
      "  func caller(x:i32): i32 { return x + 1; }\n" /* 定义在后 */
      "  return r;\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(3);\n"
      "  _ = r;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncCallsGlobal) {
  /* 局部函数体内调用全局函数：函数体查找链 = 参数 + 全局，合法 */
  std::string path = write_temp_file(
      "func g():i32 { return 100; }\n"
      "func add(a:i32, b:i32):i32 { return a + b; }\n"
      "func outer(n:i32):i32 {\n"
      "  func inc(x:i32): i32 { return x + g(); }\n"
      "  return add(inc(n), 1);\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(5);\n"   /* inc(5)=105, add(105,1)=106 */
      "  _ = r;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncNestedBlock) {
  /* 嵌套块局部函数：块作用域内定义 + 调用 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var acc:i32 = 0;\n"
      "  {\n"
      "    func twice(x:i32): i32 { return x * 2; }\n"
      "    acc = twice(3);\n"
      "  }\n"
      "  _ = acc;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncSiblingCallRejected) {
  /* 局部函数体内调用兄弟函数：需闭包，编译期拒绝 */
  std::string path = write_temp_file(
      "func outer(n:i32):i32 {\n"
      "  func caller(x:i32): i32 { return callee(x); }\n"
      "  func callee(x:i32): i32 { return x; }\n"
      "  return caller(n);\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(3);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncRecursionRejected) {
  /* 局部函数递归：需闭包，编译期拒绝 */
  std::string path = write_temp_file(
      "func outer(n:i32):i32 {\n"
      "  func dec(x:i32): i32 {\n"
      "    if (x <= 0) { return 0; }\n"
      "    return dec(x - 1);\n"
      "  }\n"
      "  return dec(n);\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(5);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncSelfCaptureRecursion) {
  /* 显式捕获自身递归：func |fib| fib(...) 定义点 STORE 后绑定新实例 →
     fib(10) = 55 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  func |fib| fib(n: i32): i32 {\n"
      "    if (n <= 1) { return n; }\n"
      "    return fib(n - 1) + fib(n - 2);\n"
      "  }\n"
      "  var r: i32 = fib(10);\n"
      "  if (r != 55) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncSelfCaptureWithOuterVar) {
  /* 捕获外层变量 + 自身递归：[base, pow] 混合捕获 → pow(4) = 2^4 = 16 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var base: i32 = 2;\n"
      "  func |base, pow| pow(n: i32): i32 {\n"
      "    if (n == 0) { return 1; }\n"
      "    return base * pow(n - 1);\n"
      "  }\n"
      "  var r: i32 = pow(4);\n"
      "  if (r != 16) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncSiblingCapture) {
  /* 兄弟函数捕获：a 显式捕获 b（定义在后）→ a(5) = b(5) = 50 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  func |b| a(n: i32): i32 {\n"
      "    return b(n);\n"
      "  }\n"
      "  func b(n: i32): i32 {\n"
      "    return n * 10;\n"
      "  }\n"
      "  var r: i32 = a(5);\n"
      "  if (r != 50) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncValueRefBeforeDef) {
  /* 定义点前值引用有捕获局部函数（var f = b，b 定义在后）：块入口实例化
     绑定，f/b 浅拷贝共享 func_t，捕获在 b 定义点 SET_CLOSURE 填齐 →
     f(2) = 3*2 = 6（值引用放行，不报 TDZ） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var base: i32 = 3;\n"
      "  var f = b;\n"
      "  func |base| b(n: i32): i32 {\n"
      "    return base * n;\n"
      "  }\n"
      "  var r: i32 = f(2);\n"
      "  if (r != 6) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncSiblingCaptureBackwardWithCaptures) {
  /* 后向兄弟捕获 + 兄弟也有捕获：a 捕获 b（定义在后、b 捕获 base）→
     a(2) = b(2) = 3*2 = 6（兄弟捕获链运行时正确解析） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var base: i32 = 3;\n"
      "  func |b| a(n: i32): i32 {\n"
      "    return b(n);\n"
      "  }\n"
      "  func |base| b(n: i32): i32 {\n"
      "    return base * n;\n"
      "  }\n"
      "  var r: i32 = a(2);\n"
      "  if (r != 6) { return 1; }\n"
      "  printf(\"ok\\n\");\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncCallBeforeDefRejected) {
  /* 定义点前调用有捕获局部函数：值引用放行后调用点 TDZ 拦截保持不变 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var base: i32 = 3;\n"
      "  var r: i32 = b(2);\n"
      "  func |base| b(n: i32): i32 {\n"
      "    return base * n;\n"
      "  }\n"
      "  return r;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncNoCaptureSelfRefRejected) {
  /* 无捕获自身引用仍编译期拦截（提示加入捕获列表） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  func fib(n: i32): i32 {\n"
      "    if (n <= 1) { return n; }\n"
      "    return fib(n - 1) + fib(n - 2);\n"
      "  }\n"
      "  var r: i32 = fib(10);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncCaptureOuterRejected) {
  /* 局部函数捕获外层局部变量：需闭包，编译期拒绝 */
  std::string path = write_temp_file(
      "func outer(n:i32):i32 {\n"
      "  var k = 10;\n"
      "  func inc(x:i32): i32 { return x + k; }\n"
      "  return inc(n);\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(3);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLocalFuncShadowsGlobalRejected) {
  /* 局部函数遮蔽全局函数名：显式拒绝 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 { return a + b; }\n"
      "func outer(n:i32):i32 {\n"
      "  func add(x:i32): i32 { return x; }\n"
      "  return add(n);\n"
      "}\n"
      "func main():i32 {\n"
      "  var r = outer(3);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefLocalHoistedExprRef) {
  /* 提升的 type 名作表达式引用（type value）：运行期块入口 DEFINE 先于
     使用点——var t = Local 引用定义在后的 type Local */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var t = Local;\n"
      "  type Local = i32;\n"
      "  var x:i32 = 7;\n"
      "  _ = t;\n"
      "  return x;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefRhsNotTypeRejected) {
  /* rhs 非类型值：sema 诊断，退出 1 */
  std::string path = write_temp_file("func main():i32 {\n"
                                     "  type Bad = 42;\n"
                                     "  return 0;\n"
                                     "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefDuplicateRejected) {
  /* 重复定义：sema 诊断，退出 1 */
  std::string path = write_temp_file("type A = i32;\n"
                                     "type A = i64;\n"
                                     "func main():i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefUnknownTypeRejected) {
  /* var 显式类型引用不存在的类型：3b 兜底 "unknown type"，退出 1 */
  std::string path = write_temp_file("func main():i32 {\n"
                                     "  var x:Nope = 5;\n"
                                     "  return 0;\n"
                                     "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefTypeValueExpr) {
  /* type value 是真实值：可作表达式（var t = T 推断为 type 类型） */
  std::string path = write_temp_file(
      "type T = i32;\n"
      "func main():i32 {\n"
      "  var t = T;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* 函数签名类型端到端：type F = func(i32,i32)->i32（M2 函数类型）。
   typedef RHS 走 ctfe 求值构造 func 签名 → hoist 区两遍扫描
   PUSH_FUNC_TYPE/FUNC_TYPE_PARAM/FUNC_TYPE_RETURN/SEAL → DEFINE 绑定。 */
TEST(Driver, RunFileTypeDefFuncSignature) {
  std::string path = write_temp_file(
      "type add_fn_t = func(i32,i32)->i32;\n"
      "func main():i32 {\n"
      "  var f:add_fn_t = undefined;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefFuncSignatureNoReturn) {
  /* 显式 void 返回：func(i32)->void */
  std::string path = write_temp_file(
      "type void_fn_t = func(i32)->void;\n"
      "func main():i32 {\n"
      "  var f:void_fn_t = undefined;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefFuncSignatureNoArrowFails) {
  /* 无 '->' 返回类型 func(i32) → 不允许隐式 void，解析报错 */
  std::string path = write_temp_file(
      "type void_fn_t = func(i32);\n"
      "func main():i32 {\n"
      "  var f:void_fn_t = undefined;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefFuncSignatureNested) {
  /* 嵌套复合签名 func([4]i32)->func(i32)->i32 */
  std::string path = write_temp_file(
      "type complex_t = func([4]i32)->func(i32)->i32;\n"
      "func main():i32 {\n"
      "  var f:complex_t = undefined;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefFuncSignatureAsParamType) {
  /* 函数签名类型作为函数参数类型（函数作为值的前置） */
  std::string path = write_temp_file(
      "type add_fn_t = func(i32,i32)->i32;\n"
      "func apply(f:add_fn_t, x:i32, y:i32):i32 { return 0; }\n"
      "func main():i32 {\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
