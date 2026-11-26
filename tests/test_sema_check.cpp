#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/vec.h"
#include "engine/context.h"
#include "engine/diag.h"
#include "engine/type.h"
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

/* ---- §8.8 P8：表达式 shadow 推断 + 调用签名交验 + 类型结果表 ---- */

/* 先序递归收集至多 max 个指定 kind 的节点（类型结果表断言用定位器）。 */
static size_t
collect_of_kind(node_t *node, node_kind_t kind, node_t **out, size_t max) {
  if (!node || !max) {
    return 0;
  }
  size_t filled = 0;
  if (node_get_kind(node) == kind) {
    out[filled++] = node;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children) && filled < max; idx++) {
    filled += collect_of_kind(
        (node_t *)vec_get(children, idx), kind, out + filled, max - filled);
  }
  return filled;
}

/* 合法推断程序零诊断：字面量各族 + 算术/比较演算 + 跨函数调用签名交验
   + 闭包定义定型与调用，全链路 shadow 推断不产诊断。 */
TEST(SemaCheckTest, CleanInferenceProgramHasNoDiag) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func add(a: i32, b: i32): i32 {\n"
                           "  return a + b;\n"
                           "}\n"
                           "func main(): void {\n"
                           "  var x = 1 + 2;\n"
                           "  var y = add(x, 3);\n"
                           "  var f = 1.0f32;\n"
                           "  var s = \"hello\";\n"
                           "  var t = true;\n"
                           "  var h = x > 1;\n"
                           "  var g = func(v: i32): i32 {\n"
                           "    return v;\n"
                           "  };\n"
                           "  var z = g(y);\n"
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

/* 调用点签名交验——个数错：实参数与形参数不符，消息与引擎 invoke 一致。 */
TEST(SemaCheckTest, CallArityMismatchIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(x: i32): void {\n"
                           "  return;\n"
                           "}\n"
                           "func main(): void {\n"
                           "  f(1, 2);\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "function expects 1 argument, got 2");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 调用点签名交验——类型错：实参类型与形参不符，下标从 0 计，消息与
   引擎 invoke 一致（浮点后缀由语法层强制，故用 1.0f64 构造异型实参）。 */
TEST(SemaCheckTest, CallArgTypeMismatchIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(x: i32): void {\n"
                           "  return;\n"
                           "}\n"
                           "func main(): void {\n"
                           "  f(1.0f64);\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(
      ctx, 1u, 0, "function argument 0 requires type 'i32', got 'f64'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 类型结果表（§8.5 第一版）：STMT_VAR 锚点标注优先、推断兜底——
   var x = 1 + 2 推断 i32；var y = 1.0f32 推断 f32；var z: f64 = 1
   标注 f64。 */
TEST(SemaCheckTest, VarTypesAreRecorded) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func main(): void {\n"
                           "  var x = 1 + 2;\n"
                           "  var y = 1.0f32;\n"
                           "  var z: f64 = 1;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  node_t *vars[3];
  ASSERT_EQ(collect_of_kind(ast, CLUX_NODE_STMT_VAR, vars, 3), 3u);
  clux_type_t *x_type = sema_get_node_type(sema, vars[0]);
  ASSERT_NE(x_type, nullptr);
  EXPECT_STREQ(clux_type_get_name(x_type), "i32");
  clux_type_t *y_type = sema_get_node_type(sema, vars[1]);
  clux_type_t *z_type = sema_get_node_type(sema, vars[2]);
  ASSERT_NE(y_type, nullptr);
  EXPECT_STREQ(clux_type_get_name(y_type), "f32");
  ASSERT_NE(z_type, nullptr);
  EXPECT_STREQ(clux_type_get_name(z_type), "f64");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}
