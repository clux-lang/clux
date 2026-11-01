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
      write_temp_file("func main():i32 { var x = 1; { } if (x != 1) { return 1; } return 0; }\n");
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
      "  if (n != nil) { var v:i32 = n.!; if (v != 40) { return 3; } } else { return 4; }\n"
      "  return 0;\n"
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
      "  if (add(x, 2) != 3) { return 1; }\n"
      "  return 0;\n"
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
      "  if (f(7) != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ================================================================
 * 回归：编译失败路径不得泄漏 sema 作用域树
 *
 * `driver_compile_to_bytecode`（clux bc emit / build -asm / build -bin 走的路径）
 * 在 sema 失败 / compiler_new 失败 / compile 失败三条路径上都必须释放作用域树：
 * `sema_destroy` 明确不释放它，而 `out->scope_tree` 只在成功路径末尾赋值。
 * 修复前实测 stderr 会打印 40+ 条 "memory leak detected ...（type='sema_scope_t'
 * / 'sema_symbol_t' / 'strmap_t' / 'rbtree_t' / 'vec_t'）"。
 * 分配器在 delete_allocator 时自检，这里捕获 stderr 断言没有泄漏报告。
 * ================================================================ */



TEST(Driver, RunFileTypeExpressionsInTypePositionPass) {
  /* 回归：类型表达式在**类型位置**求值为类型值（类型即表达式）。此前部分节点
     没有 case，落入 sema_expr 的 default 静默返回 void shadow，类型槽位会以
     误导性诊断失败。 */
  std::string path = write_temp_file(
      "type TF = func(i32)->i32;\n"
      "type TP = *i32;\n"
      "type TA = [2]i32;\n"
      "func main(): i32 {\n"
      "  var f: TF = undefined;\n"
      "  var p: TP = undefined;\n"
      "  var a: TA = .[2]i32{ 3, 4 };\n"
      "  var t: <i32, i32> = .<i32, i32>{ 1, 2 };\n"
      "  var o: ?i32 = nil;\n"
      "  if (a[0] != 3 || a[1] != 4) { return 1; }\n"
      "  if (t[0] != 1 || t[1] != 2) { return 2; }\n"
      "  if (o != nil) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeExpressionInVarInitRejected) {
  /* 同一批类型表达式放进 **var 初始化位置**是错误：类型不是运行期值，只能由
     `type` 绑定。四种形态逐个验证（每次独立流水线），诊断必须指出写法。 */
  const char *cases[] = {
      "var t = <i32, i32>;",
      "var f = func(i32)->i32;",
      "var o = ?i32;",
  };
  for (const char *decl : cases) {
    std::string src = std::string("func main(): i32 {\n  ") + decl +
                      "\n  return 0;\n}\n";
    std::string path = write_temp_file(src, "type_value_in_var");
    std::string err;
    const int rc = clux_run_capture(path, &err);
    EXPECT_EQ(rc, 1) << decl << "\n" << err;
    EXPECT_NE(err.find("with a type value"), std::string::npos)
        << decl << "\n--- stderr ---\n" << err;
    std::remove(path.c_str());
  }
}

/* ================================================================
 * 方法调用实参校验（sema）
 *
 * 回归背景：成员调用分支只对实参求 shadow、**不做可赋值性校验**，形参类型错误
 * 会一路漏到 C 后端，以 clang 的 "passing ... to parameter of incompatible type"
 * 暴露——那是 C 类型错误，不是 clux 诊断，也不指出实参下标。现在与普通函数调用
 * 用同一判据（implicit_cast）：失败即 clux 诊断，成功则由后端做布局兼容转换。
 * ================================================================ */

TEST(Driver, RunFileMethodCallArgTypeMismatchRejected) {
  clux_expect_reject(
      "struct V { x: i32; func add(self: *V, o: i32): i32 { return self.x + o; } }\n"
      "func main(): i32 {\n"
      "  var a: V = .V{ .x = 1 };\n"
      "  var n = a.add(\"bad\");\n"
      "  return n;\n"
      "}\n",
      "method 'add': argument 1: cannot convert str to i32", "method_arg_bad");
}

TEST(Driver, RunFileStaticMethodCallArgTypeMismatchRejected) {
  clux_expect_reject(
      "struct V { func add(self: *V, o: i32): i32 { return o; } }\n"
      "func main(): i32 {\n"
      "  var a: V = .V{};\n"
      "  var n = V::add(a.&, \"bad\");\n"
      "  return n;\n"
      "}\n",
      "cannot convert str to i32", "static_method_arg_bad");
}

TEST(Driver, RunFileMethodCallArgCountMismatchRejected) {
  clux_expect_reject(
      "struct V { func add(self: *V, o: i32): i32 { return o; } }\n"
      "func main(): i32 {\n"
      "  var a: V = .V{};\n"
      "  var n = a.add(1, 2);\n"
      "  return n;\n"
      "}\n",
      "method 'add': expected 1 argument(s), got 2", "method_arg_count");
}

TEST(Driver, RunFileMethodCallArgNarrowLiteralRejected) {
  /* 与普通函数调用一致：窄化（i32 字面量 → i8 形参）不是隐式转换。 */
  clux_expect_reject(
      "struct V { func m(self: *V, x: i8): i32 { return x as i32; } }\n"
      "func main(): i32 {\n"
      "  var a: V = .V{};\n"
      "  return a.m(1);\n"
      "}\n",
      "cannot convert i32 to i8", "method_arg_narrow");
}

TEST(Driver, RunFileMethodCallArgLiteralWideningAccepted) {
  /* 拓宽（i32 字面量 → i64 形参）是隐式转换，两处都必须放行。 */
  clux_expect_program_ok(
      "struct V { func m(self: *V, x: i64): i64 { return x; } }\n"
      "func f(x: i64): i64 { return x; }\n"
      "func main(): i32 {\n"
      "  var a: V = .V{};\n"
      "  var p = a.m(1);\n"
      "  var q = f(1);\n"
      "  if (p != q) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "method_arg_widen");
}

TEST(Driver, RunFileMethodCallArgDuckStructAccepted) {  /* 结构兼容的不同具名 struct 可以传给形参（与赋值/普通调用同一规则）：
     sema 放行 + 后端按形参类型重解释（此前会在 C 侧撞 incompatible type）。 */
  clux_expect_program_ok(
      "struct A { x: i32; }\n"
      "struct B { x: i32; }\n"
      "struct S { func m(self: *S, o: B): i32 { return o.x; } }\n"
      "func f(o: B): i32 { return o.x; }\n"
      "func main(): i32 {\n"
      "  var s: S = .S{};\n"
      "  var a: A = .A{ .x = 9 };\n"
      "  if (s.m(a) != 9) { return 1; }\n"
      "  if (f(a) != 9) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "method_arg_duck");
}

TEST(Driver, RunFileStaticPropertyAssignRejected) {
  /* `self.total = ...`（total 是静态属性）必须给出诊断——此前**无限递归**
     （`__deref__` 回退原地重写同一目标）→ 编译器栈溢出。 */
  clux_expect_reject(
      "struct C {\n"
      "  var total: i32 = 0;\n"
      "  value: i32;\n"
      "  func bump(self: *C): void { self.total = self.total + 1; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var c: C = .C{ .value = 1 };\n"
      "  c.bump();\n"
      "  return 0;\n"
      "}\n",
      "static properties are assigned as `T::total`", "static_prop_assign");
}

TEST(Driver, RunFileStaticPropertyReadRejected) {
  clux_expect_reject(
      "struct C {\n"
      "  var total: i32 = 0;\n"
      "  func read(self: *C): i32 { return self.total; }\n"
      "}\n"
      "func main(): i32 { var c: C = .C{}; return c.read(); }\n",
      "static properties are accessed as `T::total`", "static_prop_read");
}

TEST(Driver, ScopeAnnotationSyntaxRemovedRejected) {
  /* 作用域标注 `'<a,b> T` 已随编译期所有权体系删除：必须给出明确迁移提示，
     而不是"字符字面量只能有一个字符"。 */
  clux_expect_reject(
      "func f(p: i32): i32 {\n"
      "  var t: '<a> i32 = p;\n"
      "  return t;\n"
      "}\n"
      "func main(): i32 { return f(1) - 1; }\n",
      "scope annotations", "scope_annot_removed");
}