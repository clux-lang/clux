#include <gtest/gtest.h>
#include "core/allocator.h"
#include "engine/context.h"
#include "engine/diag.h"
#include "parser/node.h"
#include "parser/parser.h"
#include "parser/program.h"
#include "sema/sema.h"
#include <stdlib.h>
#include <string.h>

/* §8.8 P7 类型环境 + 名字解析：函数体内自由名字解析、类型位置求值、
   内建遮蔽与局部判重；诊断全收集不 fast-fail。 */

static node_t *parse_clux(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_program(a, *p);
}

/* 走一遍 walk 并断言 ERROR 级诊断恰为 count 条；返回 ctx 供消息断言。
   idx 号诊断的渲染文本包含 expected 子串（idx<0 跳过消息断言）。 */
static void expect_diag_count(clux_context_t *ctx,
                              size_t count,
                              int idx,
                              const char *expected) {
  ASSERT_EQ(clux_context_diag_count(ctx), count);
  if (idx >= 0 && (size_t)idx < count) {
    const clux_diag_t *diag = clux_context_get_diag(ctx, (size_t)idx);
    ASSERT_NE(diag, nullptr);
    allocator_t *a = clux_context_get_allocator(ctx);
    string_t *text = clux_diag_to_string((clux_diag_t *)diag, a);
    ASSERT_NE(text, nullptr);
    EXPECT_NE(strstr(string_get_data(text), expected), nullptr);
    delete_string(&text);
  }
}

/* 函数体内自由名字：未声明标识符恰报一次 "undefined name"。 */
TEST(SemaCheckTest, UndefinedNameIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  var a = missing;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "undefined name 'missing'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 多错误全收集：两个函数各一个未声明名字，一次 walk 产出两条互不掩盖。 */
TEST(SemaCheckTest, MultipleErrorsAreAllCollected) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  var a = missing1;\n"
                           "}\n"
                           "func g(): void {\n"
                           "  var b = missing2;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  ASSERT_EQ(clux_context_diag_count(ctx), 2u);
  expect_diag_count(ctx, 2u, 0, "undefined name 'missing1'");
  expect_diag_count(ctx, 2u, 1, "undefined name 'missing2'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 类型位置求值：局部 var 的标注类型未声明 → "undeclared type"。 */
TEST(SemaCheckTest, LocalVarUndeclaredTypeIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  var v: Nope = 1;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "undeclared type 'Nope'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 形参类型位置同样求值：未声明类型报一次。 */
TEST(SemaCheckTest, ParamUndeclaredTypeIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, "func f(x: Nope): void {\n}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "undeclared type 'Nope'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 内建类型遮蔽：以 i32 作值名 → "cannot shadow builtin type"。 */
TEST(SemaCheckTest, BuiltinTypeShadowingIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, "var i32: i32 = 1;\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "cannot shadow builtin type 'i32'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 局部判重：同一函数内两个同名 var → 恰一次 "duplicate declaration"。 */
TEST(SemaCheckTest, LocalDuplicateVarIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  var x: i32 = 1;\n"
                           "  var x: i32 = 2;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "duplicate declaration 'x'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 合法富程序零诊断：模块级 var/type/struct/union/enum + 函数前向调用 +
   构造/enum 成员/defer/foreach 全链路名字可解析。 */
TEST(SemaCheckTest, CleanRichProgramHasNoDiag) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "var limit: i32 = 10;\n"
                           "type Alias = i32;\n"
                           "struct S { a: i32; }\n"
                           "union U { a: i32; b: f32; }\n"
                           "enum E { A, B }\n"
                           "func main(): void {\n"
                           "  forward();\n"
                           "  var s = .S{ .a = 1 };\n"
                           "  var e = E.A;\n"
                           "  var t: Alias = 2;\n"
                           "  defer { var d: i32 = limit; }\n"
                           "  foreach (v of limit) { var w: i32 = v; }\n"
                           "}\n"
                           "func forward(): void {\n"
                           "  return;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 防假阳性回归：函数内局部类型别名（type Foo = i32）急切求值注册后，
   后续 var 标注引用 Foo 可解析，零诊断。 */
TEST(SemaCheckTest, LocalTypeAliasResolves) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  type Foo = i32;\n"
                           "  var x: Foo = 1;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}
