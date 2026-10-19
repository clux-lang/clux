/*
 * File: decorator_test.cpp
 * Description: 装饰器语法 `[[<expr>]]`（全局变量 / 顶层函数 / struct 方法）。
 *
 * 语义（前脱糖展开，纯 AST 变形）：
 *   [[ann]] var a = 123;      ⇒  var a = ann(123);
 *   [[ann]] func f(...) {…}   ⇒  func __decor_N_f(...) {…}
 *                                var f = ann(__decor_N_f);
 *   struct S { [[ann]] func m(self: ref *S, …) {…} }
 *                             ⇒  struct S { func m(…) { return __decorval_N_m(self, …); } }
 *                                func __decorsrc_N_m(self: ref *S, …) {…}
 *                                var __decorval_N_m = ann(__decorsrc_N_m);
 *
 * 装饰器必须是 **comptime 函数**（展开结果是全局变量的编译期常量初始化器）。
 * 应用顺序与 Python 一致：`[[a]] [[b]] f` ⇒ `a(b(f))`。
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "driver/driver.h"
#include "parser/fmt.h"
}

#include "test_common.h"

namespace {

/* 常用装饰器：数值翻倍 / 函数包一层 +100 / 方法包一层 +100 */
const char *kDecorators =
    "comptime func twice(n: i32): i32 { return n * 2; }\n"
    "comptime func plus1(n: i32): i32 { return n + 1; }\n"
    "comptime func add100(f: func(i32) -> i32): func(i32) -> i32 {\n"
    "  return func |f| w(x: i32): i32 { return f(x) + 100; };\n"
    "}\n"
    "func double(x: i32): i32 { return x * 2; }\n";

} // namespace

/* 全局变量装饰：`[[twice]] var a = 21;` ⇒ `var a = twice(21);` */
TEST(Decorator, GlobalVarIsDecodedThroughCall) {
  clux_expect_ok(std::string(kDecorators) +
                     "[[twice]] var a = 21;\n"
                     "func main(): i32 { return a == 42 ? 0 : 1; }\n",
                 "decor_var");
}

/* 多个装饰器：源码靠前的先包（Python 顺序）`[[twice]] [[plus1]] var a = 3;`
   ⇒ twice(plus1(3)) = 8 */
TEST(Decorator, MultipleDecoratorsApplyOutsideIn) {
  clux_expect_ok(std::string(kDecorators) +
                     "[[twice]] [[plus1]] var a = 3;\n"
                     "func main(): i32 { return a == 8 ? 0 : 1; }\n",
                 "decor_multi");
}

/* 顶层函数装饰：f 仍是函数（可递归、可当值），装饰在模块初始化期做一次 */
TEST(Decorator, TopLevelFunctionIsWrappedOnce) {
  clux_expect_ok(std::string(kDecorators) +
                     "[[add100]] func dbl(x: i32): i32 { return double(x); }\n"
                     "func main(): i32 {\n"
                     "  if (dbl(5) != 110) { return 1; }\n"
                     "  var g = dbl;             // 仍是函数值\n"
                     "  if (g(1) != 102) { return 2; }\n"
                     "  return 0;\n"
                     "}\n",
                 "decor_func");
}

/* struct 方法装饰：成员变成转发包装方法，原方法体搬到模块级源函数 */
TEST(Decorator, StructMethodIsWrapped) {
  clux_expect_ok(
      std::string(kDecorators) +
          "struct Wrap {\n"
          "  x: i32;\n"
          "  [[idm]] func get(self: ref *Wrap): i32 { return self.x; }\n"
          "  func plain(self: ref *Wrap): i32 { return self.x + 1; }\n"
          "}\n"
          "comptime func idm(f: func(ref *Wrap) -> i32): func(ref *Wrap) -> i32 {\n"
          "  return f;\n"
          "}\n"
          "func main(): i32 {\n"
          "  var w = .Wrap{ .x = 9 };\n"
          "  if (w.get() != 9) { return 1; }\n"
          "  if (w.plain() != 10) { return 2; }\n"
          "  return 0;\n"
          "}\n",
      "decor_method");
}

/* 装饰器在**编译期**求值：没有 `comptime` 标记的普通函数，只要函数体可折叠，
   同样能当装饰器用（展开结果仍是全局变量的常量初始化器）。 */
TEST(Decorator, PlainFunctionWorksIfFoldable) {
  const std::string src =
      "func plain(n: i32): i32 { return n * 3; }\n"
      "[[plain]] var a = 7;\n"
      "func main(): i32 { return a == 21 ? 0 : 1; }\n";
  clux_expect_ok(src, "decor_nonct");
}

/* 不可折叠的装饰器（引用运行期状态）→ 编译期错误，而不是运行期未定义行为 */
TEST(Decorator, NonFoldableDecoratorIsRejected) {
  const std::string src =
      "extern lib libc from \"*\";\n"
      "extern cfunc libc::printf(format: str, ...): void;\n"
      "func loud(n: i32): i32 { libc::printf(\"%d\\n\", n); return n; }\n"
      "[[loud]] var a = 1;\n"
      "func main(): i32 { return 0; }\n";
  std::string path = write_temp_file(src, "decor_nonfold");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  std::remove(path.c_str());
}

/* 装饰器只能修饰 var / func：其它声明报明确诊断 */
TEST(Decorator, OnlyVarAndFuncCanBeDecorated) {
  const std::string src =
      "comptime func twice(n: i32): i32 { return n * 2; }\n"
      "[[twice]] type T = i32;\n"
      "func main(): i32 { return 0; }\n";
  std::string path = write_temp_file(src, "decor_bad_kind");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("only allowed on 'var' and 'func'"), std::string::npos)
      << err;
  std::remove(path.c_str());
}

/* 缺少 `]]`：语法错误（不能把半截装饰器当成普通声明） */
TEST(Decorator, UnterminatedDecoratorIsSyntaxError) {
  const std::string src =
      "comptime func twice(n: i32): i32 { return n * 2; }\n"
      "[[twice] var a = 1;\n"
      "func main(): i32 { return 0; }\n";
  std::string path = write_temp_file(src, "decor_unterminated");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  std::remove(path.c_str());
}

/* fmt：装饰器前缀必须原样保留（幂等），否则格式化会悄悄丢掉装饰 */
TEST(Decorator, FmtPreservesDecorators) {
  const char *src =
      "comptime func twice(n: i32): i32 { return n * 2; }\n"
      "[[twice]] var a = 21;\n"
      "[[twice]] [[twice]] func f(x: i32): i32 { return x; }\n";
  allocator_t *a = create_allocator(malloc, free);
  char *out = fmt_format_source(a, src, std::strlen(src), nullptr);
  ASSERT_NE(out, nullptr);
  const std::string once(out);
  allocator_free(a, (void **)&out);
  delete_allocator(&a);
  EXPECT_NE(once.find("[[twice]] var a = 21;"), std::string::npos) << once;
  EXPECT_NE(once.find("[[twice]] [[twice]] func f"), std::string::npos) << once;
}

/* 装饰器**只允许顶层**：局部变量上的装饰器必须明确拒绝（而不是落成难懂的
   语法错误或悄悄忽略）。 */
TEST(Decorator, LocalVarDecoratorRejected) {
  const std::string src =
      "comptime func twice(n: i32): i32 { return n * 2; }\n"
      "func main(): i32 {\n"
      "  [[twice]] var a = 21;\n"
      "  return a;\n"
      "}\n";
  std::string path = write_temp_file(src, "decor_local_var");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("only allowed on top-level"), std::string::npos) << err;
  std::remove(path.c_str());
}

/* 局部函数同理（装饰器展开是"全局绑定 + 模块初始化期折叠"语义）。 */
TEST(Decorator, LocalFunctionDecoratorRejected) {
  const std::string src =
      "comptime func twice(n: i32): i32 { return n * 2; }\n"
      "func main(): i32 {\n"
      "  [[twice]] func inc(x: i32): i32 { return x + 1; }\n"
      "  return inc(1);\n"
      "}\n";
  std::string path = write_temp_file(src, "decor_local_func");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("only allowed on top-level"), std::string::npos) << err;
  std::remove(path.c_str());
}

/* 单个 `[` 的数组/下标表达式不受影响（装饰器识别是 `[[` 两连字符）。 */
TEST(Decorator, SingleBracketExpressionsStillWork) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var a = .[3]i32{ 7, 8, 9 };\n"
      "  var i: i32 = 1;\n"
      "  if (a[i] != 8) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "decor_single_bracket");
}

/* 方法装饰器可以返回**带 ref 参数的闭包**（"包一层"的真装饰器）：
   `p.get()` → 装饰后的闭包 → 捕获的源函数。这条覆盖了此前两个缺陷：
   ① 捕获读取改写后的 `__ctx.env.<cap>` 在重复 walk 时报 undefined variable；
   ② 具名字面量在重复折叠时报 duplicate variable definition。 */
TEST(Decorator, MethodDecoratorMayReturnRefClosure) {
  clux_expect_ok(
      "struct Point {\n"
      "  x: i32;\n"
      "  [[add100m]] func get(self: ref *Point): i32 { return self.x; }\n"
      "  func plain(self: ref *Point): i32 { return self.x + 1; }\n"
      "}\n"
      "comptime func add100m(f: func(ref *Point) -> i32): func(ref *Point) -> i32 {\n"
      "  return func |f| w(self: ref *Point): i32 { return f(self) + 100; };\n"
      "}\n"
      "func main(): i32 {\n"
      "  var p = .Point{ .x = 7 };\n"
      "  if (p.get() != 107) { return 1; }\n"
      "  if (p.plain() != 8) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "decor_method_closure");
}

/* 同文件里"值装饰 + 函数装饰 + 方法装饰"共存（此前只有组合时才会暴露
   重复折叠 / __ctx 问题，单个用例是绿的）。 */
TEST(Decorator, MixedDecoratorsInOneProgram) {
  clux_expect_ok(
      "comptime func twice(n: i32): i32 { return n * 2; }\n"
      "comptime func add100(f: func(i32) -> i32): func(i32) -> i32 {\n"
      "  return func |f| w(x: i32): i32 { return f(x) + 100; };\n"
      "}\n"
      "func double(x: i32): i32 { return x * 2; }\n"
      "[[twice]] var a = 21;\n"
      "[[add100]] func dbl(x: i32): i32 { return double(x); }\n"
      "struct Point {\n"
      "  x: i32;\n"
      "  [[add100m]] func get(self: ref *Point): i32 { return self.x; }\n"
      "}\n"
      "comptime func add100m(f: func(ref *Point) -> i32): func(ref *Point) -> i32 {\n"
      "  return func |f| w(self: ref *Point): i32 { return f(self) + 100; };\n"
      "}\n"
      "func main(): i32 {\n"
      "  var p = .Point{ .x = 7 };\n"
      "  if (a != 42) { return 1; }\n"
      "  if (dbl(5) != 110) { return 2; }\n"
      "  if (p.get() != 107) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "decor_mixed");
}

/* 带参数的装饰器：`[[dec(args)]] decl` ⇒ `dec(args, <decl>)`——被装饰的声明
   恒为**最后一个实参**，于是装饰器就是普通 comptime 函数，不需要"返回
   comptime 函数再在编译期调用一次"。 */
TEST(Decorator, ParameterizedFunctionDecorator) {
  clux_expect_ok(
      "comptime func add(n: i32, f: func(i32) -> i32): func(i32) -> i32 {\n"
      "  return func |n, f| w(x: i32): i32 { return f(x) + n; };\n"
      "}\n"
      "func double(x: i32): i32 { return x * 2; }\n"
      "[[add(100)]] func dbl(x: i32): i32 { return double(x); }\n"
      "func main(): i32 { return dbl(5) == 110 ? 0 : 1; }\n",
      "decor_param_func");
}

/* 带参数装饰器的链：`[[scale(10)]] [[add(5)]] f` ⇒ scale(10, add(5, f)) */
TEST(Decorator, ParameterizedDecoratorChain) {
  clux_expect_ok(
      "comptime func add(n: i32, f: func(i32) -> i32): func(i32) -> i32 {\n"
      "  return func |n, f| w(x: i32): i32 { return f(x) + n; };\n"
      "}\n"
      "comptime func scale(k: i32, f: func(i32) -> i32): func(i32) -> i32 {\n"
      "  return func |k, f| s(x: i32): i32 { return f(x) * k; };\n"
      "}\n"
      "func double(x: i32): i32 { return x * 2; }\n"
      "[[scale(10)]] [[add(5)]] func combo(x: i32): i32 { return double(x); }\n"
      "func main(): i32 { return combo(1) == 70 ? 0 : 1; }\n",
      "decor_param_chain");
}

/* 带参数的值装饰器：`[[plus(1)]] var v = 41` ⇒ plus(1, 41) */
TEST(Decorator, ParameterizedValueDecorator) {
  clux_expect_ok(
      "comptime func plus(n: i32, v: i32): i32 { return v + n; }\n"
      "[[plus(1)]] var v = 41;\n"
      "func main(): i32 { return v == 42 ? 0 : 1; }\n",
      "decor_param_var");
}

/* 带参数的方法装饰器：源方法（接收者为首参）作为最后一个实参 */
TEST(Decorator, ParameterizedMethodDecorator) {
  clux_expect_ok(
      "struct P { x: i32; [[addm(100)]] func get(self: ref *P): i32 { return self.x; } }\n"
      "comptime func addm(n: i32, f: func(ref *P) -> i32): func(ref *P) -> i32 {\n"
      "  return func |n, f| w(self: ref *P): i32 { return f(self) + n; };\n"
      "}\n"
      "func main(): i32 {\n"
      "  var p = .P{ .x = 7 };\n"
      "  return p.get() == 107 ? 0 : 1;\n"
      "}\n",
      "decor_param_method");
}

/* ================================================================ */
/* cfunc 定义只能在顶层（用户补充约束）                               */
/* ================================================================ */

TEST(CfuncScope, LocalCfuncDefinitionRejected) {
  const std::string src =
      "extern lib libc from \"*\";\n"
      "extern cfunc libc::printf(format: str, ...): void;\n"
      "func main(): i32 {\n"
      "  cfunc cmp(a: opaque, b: opaque): i32 { return 0; }\n"
      "  return 0;\n"
      "}\n";
  std::string path = write_temp_file(src, "local_cfunc");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("only allowed at top level"), std::string::npos) << err;
  EXPECT_NE(err.find("no closure environment"), std::string::npos) << err;
  std::remove(path.c_str());
}

/* 顶层 cfunc 定义照常可用，并且能当 func 值传给高阶函数 */
TEST(CfuncScope, TopLevelCfuncWorksAsFunctionValue) {
  clux_expect_ok(
      "cfunc add2(a: i32, b: i32): i32 { return a + b; }\n"
      "func apply(f: func(i32, i32) -> i32): i32 { return f(10, 20); }\n"
      "func main(): i32 {\n"
      "  var f: func(i32, i32) -> i32 = add2;\n"
      "  if (apply(add2) != 30) { return 1; }\n"
      "  if (f(3, 4) != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "toplevel_cfunc_value");
}

/* ================================================================ */
/* cfunc(...) → func(...)：sema 生成无捕获包装                        */
/* ================================================================ */

TEST(CfuncToFunc, NamedCfuncPtrConverts) {
  clux_expect_ok(
      "cfunc cmp(a: i32, b: i32): i32 { return a - b; }\n"
      "func main(): i32 {\n"
      "  var f: func(i32, i32) -> i32 = cfunc_ptr(cmp);\n"
      "  if (f(5, 3) != 2) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cfunc2func_basic");
}

TEST(CfuncToFunc, RefParamPassedThrough) {
  clux_expect_ok(
      "struct Box { v: i32; }\n"
      "cfunc peek(a: ref *Box): i32 { return a.*.v; }\n"
      "func main(): i32 {\n"
      "  var b = new Box{ .v = 42 };\n"
      "  var f: func(ref *Box) -> i32 = cfunc_ptr(peek);\n"
      "  if (f(b.*.&) != 42) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cfunc2func_ref");
}

TEST(CfuncToFunc, OwnParamMovedIntoWrapper) {
  clux_expect_ok(
      "struct Box { v: i32; }\n"
      "cfunc take(a: own *Box): i32 { return a.*.v; }\n"
      "func main(): i32 {\n"
      "  var b = new Box{ .v = 43 };\n"
      "  var g: func(own *Box) -> i32 = cfunc_ptr(take);\n"
      "  if (g(move(b)) != 43) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cfunc2func_own");
}

/* 手写等价的字面量（own 形参 + move 传参）也必须工作：字面量参数符号要
   is_active，否则 sema_lookup 找不到参数，误报 "cannot move a borrowed
   value" */
TEST(CfuncToFunc, LiteralOwnParamMove) {
  clux_expect_ok(
      "struct Box { v: i32; }\n"
      "cfunc take(a: own *Box): i32 { return a.*.v; }\n"
      "func main(): i32 {\n"
      "  var f = func (a: own *Box): i32 { return take(move(a)); };\n"
      "  var b = new Box{ .v = 44 };\n"
      "  if (f(move(b)) != 44) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cfunc2func_literal");
}

/* 调用实参位置同样自动转换（`apply(cfunc_ptr(cmp))`） */
TEST(CfuncToFunc, CallArgumentPosition) {
  clux_expect_ok(
      "cfunc cmp(a: i32, b: i32): i32 { return a - b; }\n"
      "func apply(f: func(i32, i32) -> i32): i32 { return f(10, 20); }\n"
      "func main(): i32 {\n"
      "  if (apply(cfunc_ptr(cmp)) != -10) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "cfunc2func_arg");
}

/* 运行期指针（非命名源）必须给出针对性诊断 */
TEST(CfuncToFunc, RuntimePointerRejected) {
  std::string path = write_temp_file(
      "cfunc cmp(a: i32, b: i32): i32 { return a - b; }\n"
      "func main(): i32 {\n"
      "  var p: opaque = cfunc_ptr(cmp);\n"
      "  var f: func(i32, i32) -> i32 = p;\n"
      "  return 0;\n"
      "}\n",
      "cfunc2func_runtime_ptr");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  std::remove(path.c_str());
}