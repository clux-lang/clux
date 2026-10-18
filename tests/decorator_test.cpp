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