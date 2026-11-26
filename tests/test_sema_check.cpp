#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/json.h"
#include "core/program.h"
#include "core/vec.h"
#include "engine/context.h"
#include "engine/diag.h"
#include "engine/type.h"
#include "parser/node.h"
#include "parser/node_generic_param.h"
#include "parser/parser.h"
#include "parser/program.h"
#include "sema/sema.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* §8.8 P7 类型环境 + 名字解析：函数体内自由名字解析、类型位置求值、
   内建遮蔽与局部判重；诊断全收集不 fast-fail。
   §8.7 P11 泛型实例体 walk 期物化：物化体与原树同过 check pass，
   定义体错误在错误实参组合上暴露；检查失败 emit 返回 NULL。 */

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
   var x = 1 + 2 推断 i32；var y = 1.0f32 推断 f32；var z: f64 = 1.0f64
   标注 f64（P9：标注与初值走 safe_cast 兼容判定，i32 到 f64 的
   裸整数字面量被拒——与运行期 var 初始化同通道）。 */
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
                           "  var z: f64 = 1.0f64;\n"
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

/* ---- §8.8 P9：语句级检查（初始化/赋值兼容 + 条件 bool + return 匹配 +
   构造完备性），负正测对照，消息与 exec 运行期逐字一致 ---- */

/* 负测：var 初始化窄化——var x: i8 = 300 的 i32 字面量窄化到 i8 被拒
   （与运行期 var 初始化的 safe_cast 同通道同消息）。 */
TEST(SemaCheckTest, VarInitNarrowingIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func main(): void {\n"
                           "  var x: i8 = 300;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(
      ctx, 1u, 0, "cannot safely cast 'i32' to 'i8': narrowing conversion");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 负测：赋值不兼容——x = 1.0f64 对 i32 目标被拒。 */
TEST(SemaCheckTest, AssignIncompatibleIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func main(): void {\n"
                           "  var x: i32 = 1;\n"
                           "  x = 1.0f64;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "cannot safely cast 'f64' to 'i32'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 负测：if/while/for 条件非 bool（整数条件），消息逐字对齐 exec。
   条件必须带圆括号（if/while/for 同一语法规范）。 */
TEST(SemaCheckTest, NonBoolConditionsAreReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func main(): void {\n"
                           "  if (1) { }\n"
                           "  while (2) { }\n"
                           "  for (var i = 0; 3; ) { }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 3u, 0, "if condition must be 'bool', got 'i32'");
  expect_diag_count(ctx, 3u, 1, "while condition must be 'bool', got 'i32'");
  expect_diag_count(ctx, 3u, 2, "for condition must be 'bool', got 'i32'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 负测：三元条件非 bool——消息对齐 exec 的 '?:' 不支持报错。 */
TEST(SemaCheckTest, TernaryConditionMustBeBool) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func main(): void {\n"
                           "  var x = 1 ? 2 : 3;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "type 'i32' does not support operator '?:'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 负测：return 值与签名返回类型不匹配（窄化），与帧边界运行期 cast
   同通道同消息。 */
TEST(SemaCheckTest, ReturnMismatchIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): i8 {\n"
                           "  return 300;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(
      ctx, 1u, 0, "cannot safely cast 'i32' to 'i8': narrowing conversion");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 负测：构造完备性——缺字段 / 字段值类型不匹配，消息对齐 exec 构造。 */
TEST(SemaCheckTest, ConstructionIssuesAreReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "struct P { a: i32; b: i32; }\n"
                           "func main(): void {\n"
                           "  var s = .P{ .a = 1 };\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "missing field 'b' in construction of 'P'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(SemaCheckTest, ConstructionFieldTypeMismatchIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "struct S { a: i32; }\n"
                           "func main(): void {\n"
                           "  var s = .S{ .a = 1.0f64 };\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "cannot safely cast 'f64' to 'i32'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 正测：P9 全链路合法程序零诊断——var 宽化放行（i32→i64）、bool 条件
   （if/while/for/三元）、return 匹配（含宽化）、构造完备且字段类型
   匹配、赋值兼容。 */
TEST(SemaCheckTest, P9CompatibleProgramHasNoDiag) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "struct S { a: i32; b: f64; }\n"
                           "func wide(): i64 {\n"
                           "  return 1;\n"
                           "}\n"
                           "func main(): void {\n"
                           "  var w: i64 = 1;\n"
                           "  w = 2;\n"
                           "  if (true) { }\n"
                           "  while (false) { }\n"
                           "  for (var i = 0; i < 3; i = i + 1) { }\n"
                           "  var t = true ? 1 : 2;\n"
                           "  var s = .S{ .a = 1, .b = 2.0f64 };\n"
                           "  var r = wide();\n"
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

/* ---- §8.8 P10：DFA 确定性赋值（§8.6）+ 模块级 var 类型表回填（§8.5）
   + member_call 静态推断（正负测对照）---- */

/* DFA 负测：= undefined 登记"未赋"（初值 AST 形态通道），定义后读取即报
   静态消息（与运行期 TDZ 消息分离，§8.6）。 */
TEST(SemaCheckTest, UseBeforeAssignmentIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  var x: i32 = undefined;\n"
                           "  var y = x;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "use before assignment 'x'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* DFA 正测：读取前有显式赋值（赋值目标先置位再消费）零诊断。 */
TEST(SemaCheckTest, AssignmentBeforeUseHasNoDiag) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  var x: i32 = undefined;\n"
                           "  x = 1;\n"
                           "  var y = x;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 0u, -1, "");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* DFA 合流保守：if 无 else 时 then 臂赋值不提升（else 路径可能漏赋）。 */
TEST(SemaCheckTest, IfSingleArmAssignDoesNotPromote) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(c: bool): void {\n"
                           "  var x: i32 = undefined;\n"
                           "  if (c) { x = 1; }\n"
                           "  var y = x;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "use before assignment 'x'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* DFA 合流正测：两臂都赋值，合流后视为已赋（未赋集合取并集的补）。 */
TEST(SemaCheckTest, IfBothArmsAssignPromotes) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(c: bool): void {\n"
                           "  var x: i32 = undefined;\n"
                           "  if (c) { x = 1; } else { x = 2; }\n"
                           "  var y = x;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 0u, -1, "");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* DFA 循环保守：while 体可能零次执行，体内赋值出口不提升。 */
TEST(SemaCheckTest, WhileBodyAssignDoesNotPromote) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(c: bool): void {\n"
                           "  var x: i32 = undefined;\n"
                           "  while (c) { x = 1; }\n"
                           "  var y = x;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "use before assignment 'x'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* DFA 捕获消费点：捕获即拷贝，捕获点即消费点——defer 捕获未赋外层
   变量在捕获处报（帧换栈之前按外层条目检查）。 */
TEST(SemaCheckTest, CaptureConsumesUnassignedOuterVar) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  var x: i32 = undefined;\n"
                           "  defer |x| { var t = 1; }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "use before assignment 'x'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* §8.5 模块级 var 值检查循环：register 只登记占位，值循环演算并回填
   STMT_VAR 锚点类型（渲染器消费通道）。 */
TEST(SemaCheckTest, ModuleVarTypeIsRecorded) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "var a = 1;\n"
                           "var b = \"hi\";\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  node_t *vars[2];
  ASSERT_EQ(collect_of_kind(ast, CLUX_NODE_STMT_VAR, vars, 2), 2u);
  clux_type_t *a_type = sema_get_node_type(sema, vars[0]);
  ASSERT_NE(a_type, nullptr);
  EXPECT_STREQ(clux_type_get_name(a_type), "i32");
  clux_type_t *b_type = sema_get_node_type(sema, vars[1]);
  ASSERT_NE(b_type, nullptr);
  EXPECT_STREQ(clux_type_get_name(b_type), "str");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* member_call 静态推断（exec_member_call 同构）：register 装 struct 方法
   名单，值接收者前置 *Box 首参，返回类型回填 r 锚点。 */
TEST(SemaCheckTest, MemberCallTypeIsInferred) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "struct Box { v: i32; func get(self: *Box): i32 "
                           "{ return self.v; } }\n"
                           "var r = .Box{ .v = 7 }.get();\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  node_t *vars[1];
  ASSERT_EQ(collect_of_kind(ast, CLUX_NODE_STMT_VAR, vars, 1), 1u);
  clux_type_t *r_type = sema_get_node_type(sema, vars[0]);
  ASSERT_NE(r_type, nullptr);
  EXPECT_STREQ(clux_type_get_name(r_type), "i32");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* member_call 负测：方法名单不含该名字，消息与 exec_member_call 逐字
   一致。 */
TEST(SemaCheckTest, MemberCallUnknownMethodIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "struct Box { v: i32; }\n"
                           "func f(b: Box): void {\n"
                           "  var r = b.get();\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "type 'Box' has no method 'get'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* [N]T 数组形参（SYMBOL location 结构判别通道）+ foreach 迭代变量退化
   元素类型回填 z 锚点。 */
TEST(SemaCheckTest, ArrayParamForeachElemTypeIsRecorded) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(items: [2]i32): void {\n"
                           "  foreach (q of items) { var z = q; }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  node_t *vars[1];
  ASSERT_EQ(collect_of_kind(ast, CLUX_NODE_STMT_VAR, vars, 1), 1u);
  clux_type_t *z_type = sema_get_node_type(sema, vars[0]);
  ASSERT_NE(z_type, nullptr);
  EXPECT_STREQ(clux_type_get_name(z_type), "i32");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 模块级 var 不动点（顺序无关）：先声明者引用后定义的 var，依赖解析
   定型后占位升级，引用点取到真实类型——声明顺序不限制推断。 */
TEST(SemaCheckTest, ModuleVarForwardReferenceTypeIsInferred) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "var c = b;\n"
                           "var b = a;\n"
                           "var a = 1;\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  node_t *vars[3];
  ASSERT_EQ(collect_of_kind(ast, CLUX_NODE_STMT_VAR, vars, 3), 3u);
  clux_type_t *a_type = sema_get_node_type(sema, vars[2]);
  ASSERT_NE(a_type, nullptr);
  EXPECT_STREQ(clux_type_get_name(a_type), "i32");
  clux_type_t *b_type = sema_get_node_type(sema, vars[1]);
  ASSERT_NE(b_type, nullptr);
  EXPECT_STREQ(clux_type_get_name(b_type), "i32");
  clux_type_t *c_type = sema_get_node_type(sema, vars[0]);
  ASSERT_NE(c_type, nullptr);
  EXPECT_STREQ(clux_type_get_name(c_type), "i32");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 循环依赖负测（互引用）：一轮无进展仍有未定型——唯一不可推断场景，
   每个未定型 var 各报一条语法错误（不重复演算防连锁假诊断）。 */
TEST(SemaCheckTest, ModuleVarMutualCircularReferenceIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "var a = b;\n"
                           "var b = a;\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 2u, 0, "circular reference in module-level var 'a'");
  expect_diag_count(ctx, 2u, 1, "circular reference in module-level var 'b'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 循环依赖负测（自引用）：var a = a 直接引用自身（遮蔽外层同名），
   同样按循环依赖报错。 */
TEST(SemaCheckTest, ModuleVarSelfReferenceIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, "var a = a;\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "circular reference in module-level var 'a'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 类型标注不解除门控：var a: i32 = b 的值仍依赖未定型 b，须等 b 定型
   后演算（标注只是占位类型，不等于依赖满足）。 */
TEST(SemaCheckTest, ModuleVarAnnotatedForwardReferenceIsInferred) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "var a: i32 = b;\n"
                           "var b = 1;\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  node_t *vars[2];
  ASSERT_EQ(collect_of_kind(ast, CLUX_NODE_STMT_VAR, vars, 2), 2u);
  clux_type_t *a_type = sema_get_node_type(sema, vars[0]);
  ASSERT_NE(a_type, nullptr);
  EXPECT_STREQ(clux_type_get_name(a_type), "i32");
  clux_type_t *b_type = sema_get_node_type(sema, vars[1]);
  ASSERT_NE(b_type, nullptr);
  EXPECT_STREQ(clux_type_get_name(b_type), "i32");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* struct 方法体纳入检查通道：方法体与模块级函数体同等待遇——体内
   DFA（use before assignment）照常上报。 */
TEST(SemaCheckTest, StructMethodBodyDfaIsChecked) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "struct B {\n"
                           "  func m(self: *B): void {\n"
                           "    var x: i32 = undefined;\n"
                           "    var y = x;\n"
                           "  }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "use before assignment 'x'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* struct 方法体 return 交验：签名返回类型进 return_type 上下文，体内
   return 不兼容经 safe_cast 报错（与模块级函数体同规则）。 */
TEST(SemaCheckTest, StructMethodBodyReturnIsChecked) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "struct B {\n"
                           "  func m(self: *B): i32 { return true; }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "cannot safely cast 'bool' to 'i32'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 正测：方法体正常代码零诊断，且方法体互调/多方法不产生重复登记。 */
TEST(SemaCheckTest, StructMethodBodyCleanHasNoDiag) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "struct B {\n"
                           "  func m(self: *B): i32 { var x = 1; return x; }\n"
                           "  func n(self: *B): void { var y: i32 = 2; }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- §8.7 P11：泛型体检查（walk 期物化 → 实例体纳入检查通道） ---- */

/* 无使用点的泛型定义不实例化 → 定义体不检查：合理省略（无实例即无
   可检查对象）。 */
TEST(SemaCheckTest, GenericDefWithoutUseIsNotInstantiated) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func id[T](x: T): T { return missing; }\n"
                           "var v: i32 = 0;\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 泛型 func 体内未定义名：使用点触发物化（id$i32），物化体当普通函数
   检查 → walk 期报 undefined name，emit 因 has_error 返回 NULL。 */
TEST(SemaCheckTest, GenericFuncBodyErrorIsReportedAndEmitBlocked) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func id[T](x: T): T { return missing; }\n"
                           "var v: i32 = id[i32](1);\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "undefined name 'missing'");
  /* 检查失败 → 不产出：emit 必须返回 NULL。 */
  EXPECT_EQ(sema_emit(sema), nullptr);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 同一 body 只在坏实参组合上报错：body 语句 `var y: *i32 = x;` 在
   T=i32 组合下类型不匹配（物化体 f$i32 报 safe_cast），在 T=P（*i32）
   组合下合法；两个调用点自身均签名匹配，不产生额外诊断。 */
TEST(SemaCheckTest, GenericBodyErrorOnlyInBadInstance) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "type P = *i32;\n"
                           "func f[T](x: T): void { var y: *i32 = x; }\n"
                           "var a: i32 = 0;\n"
                           "var p: *i32;\n"
                           "var use1 = f[i32](a);\n"
                           "var use2 = f[P](p);\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "cannot safely cast 'i32' to '*i32'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 泛型体内 DFA：use-before-assignment 经物化体在 walk 期暴露。 */
TEST(SemaCheckTest, GenericBodyUseBeforeAssignmentIsReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f[T](x: T): void {\n"
                           "  var v: T = undefined;\n"
                           "  var w = v;\n"
                           "}\n"
                           "var a: i32 = 0;\n"
                           "var use = f[i32](a);\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "use before assignment 'v'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 泛型 struct 实例体方法体错误：使用点触发 Box$i32 物化，方法体当
   普通方法检查 → 未定义字段访问在 walk 期上报。 */
TEST(SemaCheckTest, GenericStructMethodBodyIsChecked) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast =
      parse_clux(a,
                 &p,
                 "struct Box[T] {\n"
                 "  v: T;\n"
                 "  func get(self: *Box[T]): T { return self.missing; }\n"
                 "}\n"
                 "var b: Box[i32];\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_GE(clux_context_diag_count(ctx), 1u);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 正测：干净的泛型 func + struct 实例化零诊断，emit 正常产出。 */
TEST(SemaCheckTest, GenericInstancesCleanHaveNoDiag) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func id[T](x: T): T { return x; }\n"
                           "struct Box[T] { v: T; }\n"
                           "var a: i32 = 0;\n"
                           "var use = id[i32](a);\n"
                           "var b: Box[i32];\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  ASSERT_NE(program, nullptr);
  delete_program(&program);
  delete_vec(&programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- §8.9 P12：泛型约束检查 ---- */

/* 正测：interface 约束结构判定——File 提供 IW 要求的全部成员（名字 +
   签名一致），好组合零诊断。 */
TEST(SemaCheckTest, GenericInterfaceConstraintSatisfiedHasNoDiag) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "struct File {\n"
                           "  func write(self: *File, data: []u8): void { }\n"
                           "}\n"
                           "interface IW {\n"
                           "  func write(self: *File, data: []u8): void;\n"
                           "}\n"
                           "func g[W extends IW](w: *W): void { }\n"
                           "var pf: *File;\n"
                           "var use = g[File](pf);\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 负测：实参类型缺成员——与 exec（generic.c）逐字对齐报第一个缺失成员；
   约束失败实例体剔除检查域（body 的 safe_cast 错误不再级联），单一诊断。 */
TEST(SemaCheckTest, GenericConstraintMissingMemberReportedOnce) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "interface IW {\n"
                           "  func write(self: *i32, data: []u8): void;\n"
                           "}\n"
                           "func g[W extends IW](w: *W): void {\n"
                           "  var z: str = w;\n"
                           "}\n"
                           "var pi: *i32;\n"
                           "var use = g[i32](pi);\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx,
                    1u,
                    0,
                    "type 'i32' does not satisfy interface constraint 'IW': "
                    "missing member 'write'");
  EXPECT_EQ(sema_emit(sema), nullptr);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 负测：成员存在但签名不符——i64 vs i32 形参不匹配。 */
TEST(SemaCheckTest, GenericConstraintSignatureMismatchReported) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "struct S {\n"
                           "  func write(self: *S, n: i64): void { }\n"
                           "}\n"
                           "interface IW {\n"
                           "  func write(self: *S, n: i32): void;\n"
                           "}\n"
                           "func g[W extends IW](w: *W): void { }\n"
                           "var ps: *S;\n"
                           "var use = g[S](ps);\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx,
                    1u,
                    0,
                    "type 'S' does not satisfy interface constraint 'IW': "
                    "member 'write' signature mismatch");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 类型模式约束（非 interface）：按 clux_type_extends 派生判定——B 字段
   前缀兼容 A 满足，i32 不满足；好组合零诊断 + 坏组合单一诊断并存。 */
TEST(SemaCheckTest, GenericPatternConstraintExtendsJudged) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "struct A { x: i32; }\n"
                           "struct B { x: i32; y: i32; }\n"
                           "func g[T extends A](t: T): void {\n"
                           // 满足约束的实例体里合法（B 有字段 x）；
                           // 坏组合若未被剔除，这里会对 i32 报二次错误。
                           "  var z: i32 = t.x;\n"
                           "}\n"
                           "var b: B;\n"
                           "var i: i32 = 0;\n"
                           "var use1 = g[B](b);\n"
                           "var use2 = g[i32](i);\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "type 'i32' does not satisfy constraint 'A'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- §8.9 P13：泛型默认值与参数包（物化对齐 §6.2/§6.4） ---- */

/* 正测：右侧参数省略由 DEFAULT 槽补齐（§6.2）——实例体中 U 代换为
   i32（var z: U = 0 合法）；若未填充，U 会报 undeclared type。 */
TEST(SemaCheckTest, GenericDefaultOmittedIsFilledAndSubstituted) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f[T, U = i32](x: T): U {\n"
                           "  var z: U = 0;\n"
                           "  return 0;\n"
                           "}\n"
                           "var s: str = \"a\";\n"
                           "var u = f[str](s);\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  ASSERT_NE(program, nullptr);
  delete_program(&program);
  delete_vec(&programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 折叠：省略槽默认值文本并入拼名（f[i32] 与 f[i32, i32] 同实例名
   f$i32$i32）——同组合只物化一次，body 错误单条诊断；未折叠则两条。 */
TEST(SemaCheckTest, GenericDefaultOmittedFoldsWithExplicit) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f[T, U = i32](x: T): void {\n"
                           "  var z: str = 0;\n"
                           "}\n"
                           "var a: i32 = 0;\n"
                           "f[i32](a);\n"
                           "f[i32, i32](a);\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "cannot safely cast 'i32' to 'str'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 负测：无包时多给实参保留旧计数消息（对齐 exec 计数协议），emit 阻断。 */
TEST(SemaCheckTest, GenericNoPackExtraArgsKeepOldMessage) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, "type R[A] = [4]A;\nvar v: R[i32, str];\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "generic 'R' expects 1 type arguments, got 2");
  EXPECT_EQ(sema_emit(sema), nullptr);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 正测：参数包空包（§6.4）——剩余实参为空聚合为空 TUPLE，物化体
   rest: ...<> 的签名在 check 侧降级，零诊断 emit OK。 */
TEST(SemaCheckTest, GenericPackEmptyHasNoDiag) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func h[T, ...Args](x: T, rest: ...Args): void {\n"
                           "  var z: i32 = 0;\n"
                           "}\n"
                           "var a: i32 = 0;\n"
                           "h[i32](a);\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  ASSERT_NE(program, nullptr);
  delete_program(&program);
  delete_vec(&programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 正测：包展开——剩余实参 [str] 聚合为 tuple 代换（rest: ...<str>），
   exec 层 exec_pack_tuple_type 同协议，零诊断 emit OK。 */
TEST(SemaCheckTest, GenericPackExpandedHasNoDiag) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func h[T, ...Args](x: T, rest: ...Args): void {\n"
                           "  var z: i32 = 0;\n"
                           "}\n"
                           "var a: i32 = 0;\n"
                           "h[i32, str](a, \"s\");\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  ASSERT_NE(program, nullptr);
  delete_program(&program);
  delete_vec(&programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 正测：comptime 值参数默认（§6.2）——空下标省略由 DEFAULT 补齐并
   物化进检查域（body 中 str ← 0 的错误被单条诊断证实）。 */
TEST(SemaCheckTest, GenericComptimeDefaultOmittedIsFilled) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func k[N: comptime usize = 4](): void {\n"
                           "  var z: str = 0;\n"
                           "}\n"
                           "k[]();\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  expect_diag_count(ctx, 1u, 0, "cannot safely cast 'i32' to 'str'");
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}
