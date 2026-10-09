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
/* 函数值（function as value）端到端                                    */
/* ================================================================ */

TEST(Driver, RunFileFuncValueAssignAndCall) {
  /* var f = add; f(3,4) 经变量调用函数值 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "func main():void {\n"
      "  var f = add;\n"
      "  var val = f(3, 4);\n"
      "  printf(\"%d\\n\", val);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueAsArgument) {
  /* 函数值传参：apply(add,10,20) */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "func apply(f:func(i32,i32)->i32, x:i32, y:i32):i32 {\n"
      "  return f(x, y);\n"
      "}\n"
      "func main():void {\n"
      "  var r = apply(add, 10, 20);\n"
      "  printf(\"%d\\n\", r);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueAsReturn) {
  /* 函数值返回：get_add() 返回 add，再经变量调用 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "func get_add():func(i32,i32)->i32 {\n"
      "  return add;\n"
      "}\n"
      "func main():void {\n"
      "  var f = get_add();\n"
      "  printf(\"%d\\n\", f(5, 6));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueExplicitTypeAndReassign) {
  /* 显式 func 类型 + 函数值再赋值（f = g） */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "func sub(a:i32, b:i32):i32 {\n"
      "  return a - b;\n"
      "}\n"
      "func main():void {\n"
      "  var f: func(i32,i32)->i32 = add;\n"
      "  var g = sub;\n"
      "  f = g;\n"
      "  printf(\"%d\\n\", f(20, 8));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueComptimeFold) {
  /* comptime func 返回函数值：编译期折叠，add_fn(1,2) 输出 3 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "comptime func get_add():func(i32,i32)->i32 {\n"
      "  return add;\n"
      "}\n"
      "func main():void {\n"
      "  var add_fn = get_add();\n"
      "  var val = add_fn(1,2);\n"
      "  printf(\"%d\\n\", val);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueComptimeReturnsLiteral) {
  /* comptime func 返回匿名函数字面量：折叠产物 LOAD_FUNCTION <fid>，
     运行期调用字面量函数体（+1 得 42） */
  std::string path = write_temp_file(
      "comptime func get_f():func(i32)->i32 {\n"
      "  return func(x:i32):i32 { return x + 1; };\n"
      "}\n"
      "func main():void {\n"
      "  var f = get_f();\n"
      "  printf(\"%d\\n\", f(41));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueComptimeReturnsLocalFunc) {
  /* comptime func 内定义局部函数并返回：局部函数进入运行时字节码
     （prescan 无条件递归 comptime body），折叠产物按 fid 加载调用 */
  std::string path = write_temp_file(
      "comptime func get_f():func(i32)->i32 {\n"
      "  func inc(x:i32):i32 { return x + 1; }\n"
      "  return inc;\n"
      "}\n"
      "func main():void {\n"
      "  var f = get_f();\n"
      "  printf(\"%d\\n\", f(41));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueCallExpressionCallee) {
  /* 一般 callee 表达式（函数值调用）：comptime func 返回的函数值立即调用
     get_fn()()、嵌套返回再调用、函数字面量直接调用 */
  std::string path = write_temp_file(
      "comptime func get_fn():func()->i32 {\n"
      "  return func():i32 { return 123; };\n"
      "}\n"
      "comptime func get_add():func(i32,i32)->i32 {\n"
      "  return func(a:i32, b:i32): i32 { return a + b; };\n"
      "}\n"
      "func mk_fn(): func(i32)->i32 {\n"
      "  return func(x: i32): i32 { return x * 2; };\n"
      "}\n"
      "func main():void {\n"
      "  var a = get_fn()();\n"
      "  var b = get_add()(19, 23);\n"
      "  var c = mk_fn()(21);\n"
      "  var d = func(x:i32):i32 { return x + 1; }(41);\n"
      "  printf(\"%d %d %d %d\\n\", a, b, c, d);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueCallExpressionRejected) {
  /* 非函数值表达式作 callee：编译期拒绝（cannot call value of type i32） */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var x: i32 = 5;\n"
      "  var r = x(1);\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueComptimeNestedCalls) {
  /* comptime func body 内调用另一个 comptime func（嵌套折叠）：walk 阶段
     只做 shadow 类型检查，真实调用点 CTFE 折叠 */
  std::string path = write_temp_file(
      "comptime func double_(a:i32):i32 {\n"
      "  return a * 2;\n"
      "}\n"
      "comptime func quad(a:i32):i32 {\n"
      "  return double_(double_(a));\n"
      "}\n"
      "comptime var Q = quad(3);\n"
      "func main():void {\n"
      "  var x = Q;\n"
      "  printf(\"%d\\n\", x);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueComptimeMultipleFactories) {
  /* comptime func 多次调用各自返回字面量：fid 按定义点唯一分配，折叠产物
     分别加载不同函数对象 */
  std::string path = write_temp_file(
      "comptime func make_plus():func(i32,i32)->i32 {\n"
      "  return func(a:i32, b:i32):i32 { return a + b; };\n"
      "}\n"
      "comptime func make_mul():func(i32,i32)->i32 {\n"
      "  return func(a:i32, b:i32):i32 { return a * b; };\n"
      "}\n"
      "func main():void {\n"
      "  var f = make_plus();\n"
      "  var g = make_mul();\n"
      "  printf(\"%d %d\\n\", f(20, 22), g(6, 7));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueBuiltinPrintf) {
  /* 内建 printf 与普通函数同等：函数值赋值 + 调用 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var p = printf;\n"
      "  p(\"hello %s %d\\n\", \"world\", 42);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueVarShadowsNameRejected) {
  /* 遮蔽平等：var add=2 遮蔽函数名，调用报错（不可调用而非函数不存在） */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "func main():void {\n"
      "  var add = 2;\n"
      "  var val = add(1,2);\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncValueTypeMismatchRejected) {
  /* 签名不匹配：func(i32)->i32 槽位收 func(i32,i32)->i32 报错 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 {\n"
      "  return a + b;\n"
      "}\n"
      "func main():void {\n"
      "  var f: func(i32)->i32 = add;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ================================================================ */
/* 函数字面量（function literal）端到端                                */
/* ================================================================ */

TEST(Driver, RunFileFuncLiteralBasic) {
  /* 匿名函数字面量：var f = func(...){...}; f(41) → 42 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var f = func(x: i32): i32 { return x + 1; };\n"
      "  var val = f(41);\n"
      "  printf(\"%d\\n\", val);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncLiteralNamed) {
  /* 具名字面量：函数有显示名，但不绑定作用域名字（外部不可按名调用） */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var f = func inc(x: i32): i32 { return x * 2; };\n"
      "  var val = f(21);\n"
      "  printf(\"%d\\n\", val);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncLiteralMultiParam) {
  /* 多参数 + 显式 func 类型槽位 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var f: func(i32,i32)->i32 = func(a: i32, b: i32): i32 { return a * b; };\n"
      "  var val = f(6, 7);\n"
      "  printf(\"%d\\n\", val);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncLiteralAsArgument) {
  /* 字面量直接传给函数参数（高阶调用）：apply(func(...), 10, 20) */
  std::string path = write_temp_file(
      "func apply(f:func(i32,i32)->i32, x:i32, y:i32):i32 {\n"
      "  return f(x, y);\n"
      "}\n"
      "func main():void {\n"
      "  var r = apply(func(a: i32, b: i32): i32 { return a + b; }, 10, 20);\n"
      "  printf(\"%d\\n\", r);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncLiteralNested) {
  /* 嵌套字面量：内层字面量作为外层字面量求值产物，outer(1) → 102 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var outer = func(n: i32): i32 {\n"
      "    var inner = func(x: i32): i32 { return x + 100; };\n"
      "    return inner(n + 1);\n"
      "  };\n"
      "  printf(\"%d\\n\", outer(1));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncLiteralLocalFuncInBody) {
  /* 字面量 body 内可定义局部函数并调用：f(14) → 42 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var f = func(n: i32): i32 {\n"
      "    func triple(x: i32): i32 { return x * 3; }\n"
      "    return triple(n);\n"
      "  };\n"
      "  printf(\"%d\\n\", f(14));\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFuncLiteralCaptureRejected) {
  /* 未声明捕获即访问外层局部变量：仍编译期拒绝（需显式捕获列表） */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var base = 10;\n"
      "  var f = func(x: i32): i32 { return x + base; };\n"
      "  var val = f(1);\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileLoopClosuresIndependentInstances) {
  /* 循环内函数定义每次求值生成独立实例：fns[i] 三个闭包各自捕获
     items[0]=0 / items[1]=1 / items[2]=2，输出 "0 1 2"。
     若共享同一函数对象则全部捕获最后一次迭代值 → "2 2 2"。 */
  std::string path = write_temp_file(
      "func main():void {\n"
      "  var fns = .[3](func()->i32){ func():i32 { return -1; }, func():i32 { return -1; }, func():i32 { return -1; } };\n"
      "  var items = .[3]i32 {0,1,2};\n"
      "  for(var i = 0;i<3;i+=1) {\n"
      "    fns[i] = func|(val = items[i])|():i32 { return val; };\n"
      "  }\n"
      "  printf(\"%d %d %d\\n\", fns[0](), fns[1](), fns[2]());\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
