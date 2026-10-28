#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <string>

extern "C" {
#include "driver/driver.h"
}

#include "test_common.h"

namespace {


/* 运行一段源码，返回退出码（0 = 程序内自检全通过） */
int run_src(const std::string &src) {
  std::string path = write_temp_file(src);
  int rc = driver_run_file(path.c_str());
  std::remove(path.c_str());
  return rc;
}

} // namespace

/* ================================================================
 * comptime var：编译期常量，引用点折叠为常量（m2-design §7/§10）
 *
 * 关键契约：comptime var 是编译期常量，**任何常量位置**都能用，且**支持所有
 * 类型**。常量位置（数组边界/type 别名 rhs/签名槽位/make 长度/枚举值…）由
 * CTFE 求值——CTFE 的标识符解析查 vm 作用域链，因此常量值必须绑定进该链
 * （全局 → root_scope，局部 → 当前块作用域），见 sema_eval_comptime_var 步骤 5。
 * ================================================================ */

TEST(DriverComptime, GlobalComptimeVarAsArrayBound) {
  EXPECT_EQ(run_src(
      "comptime var N = 3;\n"
      "func main(): i32 {\n"
      "  var a: [N]i32 = .[3]i32{1, 2, 3};\n"
      "  return a[0] - 1;\n"
      "}\n"), 0);
}

TEST(DriverComptime, LocalComptimeVarAsArrayBound) {
  /* 局部 comptime var 在同块定义点求值；引用它的类型槽位必须等到 3b 才解析
     （3a 提前解析会因值未就绪失败并触发快速失败闸门，使 3b 不运行）。 */
  EXPECT_EQ(run_src(
      "func main(): i32 {\n"
      "  comptime var N = 3;\n"
      "  var a: [N]i32 = .[3]i32{1, 2, 3};\n"
      "  return a[0] - 1;\n"
      "}\n"), 0);
}

TEST(DriverComptime, GlobalComptimeVarInTypeAlias) {
  /* 全局 type 别名的 rhs 在 pass1b 解析，而 comptime var 常规求值在 pass2 之后
     → 需要 pass_comptime_early 提前求值（仅常量 init 能早跑）。 */
  EXPECT_EQ(run_src(
      "comptime var N = 3;\n"
      "type T = [N]i32;\n"
      "func main(): i32 {\n"
      "  var a: T = .[3]i32{1, 2, 3};\n"
      "  return a[0] - 1;\n"
      "}\n"), 0);
}

TEST(DriverComptime, LocalComptimeVarInLocalTypeAlias) {
  /* 局部 type def 是块级提升的（walk_block 提升预扫里求值），因此 comptime var
     也要在预扫里按源序先求值；且 comptime var 是编译期常量，**不**参与
     prior_vars 遮蔽预检（否则被判成"引用变量遮蔽类型名"）。 */
  EXPECT_EQ(run_src(
      "func main(): i32 {\n"
      "  comptime var C = 3;\n"
      "  type T = [C]i32;\n"
      "  var a: T = .[3]i32{1, 2, 3};\n"
      "  return a[0] - 1;\n"
      "}\n"), 0);
}

TEST(DriverComptime, ComptimeVarInFunctionSignatureBound) {
  /* 签名槽位在 pass2 解析 → 需 pass1b 之前的早期求值 */
  EXPECT_EQ(run_src(
      "comptime var N = 2;\n"
      "func f(a: [N + 1]i32): i32 { return a[0]; }\n"
      "func main(): i32 {\n"
      "  var a: [3]i32 = .[3]i32{9, 0, 0};\n"
      "  return f(a) - 9;\n"
      "}\n"), 0);
}

TEST(DriverComptime, ComptimeVarInExpressionBoundAndMakeLen) {
  EXPECT_EQ(run_src(
      "func main(): i32 {\n"
      "  comptime var N = 2;\n"
      "  var a: [N + 1]i32 = .[3]i32{5, 0, 0};\n"
      "  var s: []i32 = make(i32, N, 7, 8);\n"
      "  if (a[0] != 5) { return 1; }\n"
      "  if (s[1] != 8) { return 2; }\n"
      "  return 0;\n"
      "}\n"), 0);
}

TEST(DriverComptime, SizeofComptimeVarBoundedArray) {
  EXPECT_EQ(run_src(
      "comptime var N = 4;\n"
      "func main(): i32 {\n"
      "  var a: [N]i32 = .[4]i32{0, 0, 0, 0};\n"
      "  if (sizeof(a) != 16u64) { return 1; }\n"
      "  return 0;\n"
      "}\n"), 0);
}

TEST(DriverComptime, AllScalarAndStringTypes) {
  /* comptime var 支持所有类型：整型/浮点/布尔/字符串/枚举/数组 */
  EXPECT_EQ(run_src(
      "comptime var I = 5;\n"
      "comptime var U: const u64 = 7u64;\n"
      "comptime var F = 1.5;\n"
      "comptime var B = true;\n"
      "comptime var S = \"hi\";\n"
      "enum E:i32 { A = 0, B = 1 }\n"
      "comptime var V = E::B;\n"
      "comptime var ARR = .[3]i32{1, 2, 3};\n"
      "func main(): i32 {\n"
      "  var i: i32 = I;\n"
      "  var u: u64 = U;\n"
      "  var f: f64 = F;\n"
      "  var s: str = S;\n"
      "  var e: E = V;\n"
      "  var a: [3]i32 = ARR;\n"
      "  if (!B) { return 1; }\n"
      "  if (i != 5) { return 2; }\n"
      "  if (u != 7u64) { return 3; }\n"
      "  if (f != 1.5) { return 4; }\n"
      "  if (e != E::B) { return 5; }\n"
      "  if (a[2] != 3) { return 6; }\n"
      "  return 0;\n"
      "}\n"), 0);
}

TEST(DriverComptime, StructAndTupleTypes) {
  /* comptime var 支持复合类型：struct / tuple 常量（CTFE 构造聚合值 → 编码
     → 引用点折叠为 AST_CONSTRUCT）。 */
  EXPECT_EQ(run_src(
      "struct P { x: i32; y: i32; }\n"
      "comptime var S = .P{ .x = 1, .y = 2 };\n"
      "comptime var T = .<i32, i32>{ 3, 4 };\n"
      "func main(): i32 {\n"
      "  var p: P = S;\n"
      "  var t: <i32, i32> = T;\n"
      "  if (p.x != 1 || p.y != 2) { return 1; }\n"
      "  if (t[0] != 3 || t[1] != 4) { return 2; }\n"
      "  return 0;\n"
      "}\n"), 0);
}

TEST(DriverComptime, ComptimeFuncCallResultUsedInConstantSlots) {
  /* 求值顺序无关（不动点轮次）：comptime var 的 init 需要函数签名（pass2 才
     解析），而引用它的常量槽位在 pass1b/pass2 就要解析——依赖成环时按轮次
     解开，不再因为书写顺序而挑食。 */
  const std::string k_add =
      "comptime func add(a: i32, b: i32): i32 { return a + b; }\n";
  /* 1. 函数体槽位 */
  EXPECT_EQ(run_src(k_add +
      "comptime var N = add(1, 2);\n"
      "func main(): i32 { var a: [N]i32 = .[3]i32{1,2,3}; return a[0] - 1; }\n"), 0);
  /* 2. 全局 type 别名 */
  EXPECT_EQ(run_src(k_add +
      "comptime var N = add(1, 2);\n"
      "type T = [N]i32;\n"
      "func main(): i32 { var a: T = .[3]i32{1,2,3}; return a[0] - 1; }\n"), 0);
  /* 3. 函数签名槽位 */
  EXPECT_EQ(run_src(k_add +
      "comptime var N = add(1, 2);\n"
      "func f(a: [N]i32): i32 { return a[0]; }\n"
      "func main(): i32 { var a: [3]i32 = .[3]i32{9,0,0}; return f(a) - 9; }\n"), 0);
  /* 4. 链式：type 别名 → 签名 */
  EXPECT_EQ(run_src(k_add +
      "comptime var N = add(1, 2);\n"
      "type T = [N]i32;\n"
      "func f(a: T): i32 { return a[0]; }\n"
      "func main(): i32 { var a: T = .[3]i32{7,0,0}; return f(a) - 7; }\n"), 0);
  /* 5. 全局变量槽位 + struct 字段槽位 */
  EXPECT_EQ(run_src(k_add +
      "comptime var N = add(2, 1);\n"
      "var g: [N]i32 = .[3]i32{4,5,6};\n"
      "struct S { a: [N]i32; }\n"
      "func main(): i32 {\n"
      "  var s: S = .S{ .a = .[3]i32{7,8,9} };\n"
      "  if (g[0] != 4) { return 1; }\n"
      "  if (s.a[2] != 9) { return 2; }\n"
      "  return 0;\n"
      "}\n"), 0);
  /* 6. 逆序书写：type 别名出现在 comptime var 之前 */
  EXPECT_EQ(run_src(k_add +
      "type T = [N]i32;\n"
      "comptime var N = add(1, 2);\n"
      "func main(): i32 { var a: T = .[3]i32{1,2,3}; return a[0] - 1; }\n"), 0);
}

TEST(DriverComptime, NonComptimeFunctionCallInConstantSlot) {
  /* m2-design §9 决策点 2：**非 comptime 函数**在常量槽位也被尝试编译期求值
     （失败才报错）。顺序无关同样适用：函数定义在引用之后也能求值。 */
  EXPECT_EQ(run_src(
      "comptime var N = len3();\n"
      "func main(): i32 { var a: [N]i32 = .[3]i32{1,2,3}; return a[0] - 1; }\n"
      "func len3(): i32 { return 3; }\n"), 0);
}

TEST(DriverComptime, AssignToComptimeVarRejected) {
  /* comptime var 是编译期常量，不可赋值。注意其常量**真实值**也会绑定进
     vm 作用域（常量槽位解析需要），因此拒绝不能靠"vm scope 查不到"实现。 */
  EXPECT_NE(run_src(
      "comptime var A = 1;\n"
      "func main(): void { A = 2; }\n"), 0);
}
