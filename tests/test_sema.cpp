#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/location.h"
#include "core/program.h"
#include "engine/context.h"
#include "engine/diag.h"
#include "parser/node.h"
#include "parser/node_func_decl.h"
#include "parser/node_program.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_type.h"
#include "parser/node_stmt_var.h"
#include "parser/node_struct_decl.h"
#include "parser/parser.h"
#include "parser/program.h"
#include "sema/sema.h"
#include <stdlib.h>
#include <string.h>

static node_t *parse_clux(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_program(a, *p);
}

static void dispose_programs(vec_t *programs) {
  if (!programs) {
    return;
  }
  for (size_t idx = 0; idx < vec_get_size(programs); idx++) {
    program_t *program = (program_t *)vec_get(programs, idx);
    delete_program(&program);
  }
  delete_vec(&programs);
}

/* Phase 1 骨架的正路径：干净程序 walk 无错，emit 产出主模块 program_t，
   幂等（两次 emit 产出独立集合），产物可独立销毁。 */
TEST(SemaTest, CleanProgramWalksAndEmits) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "var x: i32 = 1;\n"
                           "func f(): void { return; }\n"
                           "struct S { a: i32; }\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  ASSERT_EQ(vec_get_size(programs), 1u);
  program_t *program = (program_t *)vec_get(programs, 0);
  EXPECT_STREQ(program_get_module_name(program), "main");
  /* emit 只读幂等：第二次产出独立集合。 */
  vec_t *again = sema_emit(sema);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(vec_get_size(again), 1u);
  dispose_programs(again);
  dispose_programs(programs);
  sema_destroy(&sema);
  EXPECT_EQ(sema, nullptr);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 逐 stmt 诊断收集：重复声明报 diag 后继续（不快速失败），
   一次 walk 产出全部错误；有 ERROR 时不 emit。 */
TEST(SemaTest, DuplicateDeclarationsAreAllCollected) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "var x: i32 = 1;\n"
                           "var x: i32 = 2;\n"
                           "var x: i32 = 3;\n"
                           "func f(): void { return; }\n"
                           "struct x { a: i32; }\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_TRUE(sema_has_error(sema));
  EXPECT_EQ(clux_context_diag_count(ctx), 3u);
  const clux_diag_t *first = clux_context_get_diag(ctx, 0);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(clux_diag_get_level(first), CLUX_DIAG_ERROR);
  string_t *text = clux_diag_to_string((clux_diag_t *)first, a);
  EXPECT_NE(strstr(string_get_data(text), "duplicate declaration 'x'"),
            nullptr);
  delete_string(&text);
  /* 错误即终止：不 emit。 */
  EXPECT_EQ(sema_emit(sema), nullptr);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 不同名字不互扰；跨声明种类（var vs func vs struct）同才算冲突。 */
TEST(SemaTest, DistinctNamesAcrossKindsDoNotConflict) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "type T = i32;\n"
                           "enum E { A }\n"
                           "union U { a: i32; }\n"
                           "interface I { func g(): void; }\n"
                           "bitfield B: u8 { f: 1; }\n");
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

/* 非 PROGRAM 节点拒收：结构错误也是 diag，不返回半成品。 */
TEST(SemaTest, WalkRejectsNonProgramNode) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  node_t *not_program = create_node(a, CLUX_NODE_STMT_BLOCK);
  EXPECT_EQ(sema_walk(sema, not_program), -1);
  EXPECT_TRUE(sema_has_error(sema));
  EXPECT_EQ(clux_context_diag_count(ctx), 1u);
  EXPECT_EQ(sema_emit(sema), nullptr);
  /* walk 只允许一次：结构失败后再次 walk 也拒绝。 */
  parser_t *p;
  node_t *ast = parse_clux(a, &p, "var x: i32 = 1;");
  EXPECT_EQ(sema_walk(sema, ast), -1);
  EXPECT_EQ(clux_context_diag_count(ctx), 2u);
  delete_node(&ast);
  delete_parser(&p);
  delete_node(&not_program);
  sema_destroy(&sema);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 未 walk 不 emit：阶段二的执行以阶段一完成为前置。 */
TEST(SemaTest, EmitRequiresWalkFirst) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  EXPECT_EQ(sema_emit(sema), nullptr);
  sema_destroy(&sema);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* NULL 契约：与 engine 侧 API 同风格，NULL 一律安全返回。 */
TEST(SemaTest, NullArgumentsAreSafe) {
  EXPECT_EQ(sema_create(nullptr, nullptr), nullptr);
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  EXPECT_EQ(sema_create(ctx, nullptr), nullptr);
  EXPECT_EQ(sema_create(nullptr, a), nullptr);
  EXPECT_EQ(sema_walk(nullptr, nullptr), -1);
  EXPECT_FALSE(sema_has_error(nullptr));
  EXPECT_EQ(sema_emit(nullptr), nullptr);
  sema_destroy(nullptr);
  sema_destroy((clux_sema_t **)nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- Phase 2：提升（§2.2） ---- */

/* helper：取声明语句名节点的当前文本（克隆树的提升名即区间内容）。 */
static char *node_text(allocator_t *a, node_t *node) {
  return node ? location_get(node_get_location(node), a) : NULL;
}

static node_t *func_body_of(node_t *stmt) {
  node_t *decl = stmt_func_node_get_func_decl(stmt);
  return decl ? func_decl_node_get_body(decl) : NULL;
}

/* 块内计数/取位跳过 STMT_EMPTY：分号是独立合法空语句（§4.14），
   不参与提升摘除后的业务断言排序。 */
static size_t body_stmt_count(node_t *body) {
  size_t count = 0;
  for (size_t idx = 0; idx < stmt_block_node_get_stmt_count(body); idx++) {
    node_t *stmt = stmt_block_node_get_stmt(body, idx);
    if (stmt && node_get_kind(stmt) != CLUX_NODE_STMT_EMPTY) {
      count++;
    }
  }
  return count;
}

static node_t *body_nth_stmt(node_t *body, size_t nth) {
  size_t seen = 0;
  for (size_t idx = 0; idx < stmt_block_node_get_stmt_count(body); idx++) {
    node_t *stmt = stmt_block_node_get_stmt(body, idx);
    if (stmt && node_get_kind(stmt) != CLUX_NODE_STMT_EMPTY) {
      if (seen == nth) {
        return stmt;
      }
      seen++;
    }
  }
  return NULL;
}

/* §2.2 正路径：函数体内的类型/函数提升到 PROGRAM 尾部（f$Foo、f$g），
   原位置声明语句摘除、引用点改写为提升名；原 AST 保持只读不变。 */
TEST(SemaTest, HoistsLocalDeclsToProgramTailAndRenamesRefs) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  struct Foo { a: i32; }\n"
                           "  func g(): void { return; }\n"
                           "  var x: Foo;\n"
                           "  var y: Foo;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  size_t orig_top = program_node_get_stmt_count(ast);
  node_t *orig_body = func_body_of(program_node_get_stmt(ast, 0));
  size_t orig_body_count = stmt_block_node_get_stmt_count(orig_body);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  ASSERT_NE(decls, nullptr);
  /* 顶层 f + 两个提升项（收集序：Foo 先于 g）。 */
  ASSERT_EQ(program_node_get_stmt_count(decls), 3u);
  node_t *f_clone = program_node_get_stmt(decls, 0);
  node_t *foo_clone = program_node_get_stmt(decls, 1);
  node_t *g_clone = program_node_get_stmt(decls, 2);
  EXPECT_EQ(node_get_kind(f_clone), CLUX_NODE_STMT_FUNC);
  EXPECT_EQ(node_get_kind(foo_clone), CLUX_NODE_STMT_STRUCT);
  EXPECT_EQ(node_get_kind(g_clone), CLUX_NODE_STMT_FUNC);
  /* 提升项的名字是 mangled 名。 */
  node_t *foo_name =
      struct_decl_node_get_name(stmt_struct_node_get_struct_decl(foo_clone));
  char *text = node_text(a, foo_name);
  EXPECT_STREQ(text, "f$Foo");
  allocator_free(a, (void **)&text);
  node_t *g_name =
      func_decl_node_get_name(stmt_func_node_get_func_decl(g_clone));
  text = node_text(a, g_name);
  EXPECT_STREQ(text, "f$g");
  allocator_free(a, (void **)&text);
  /* f 克隆体：声明语句摘除，两条 var 保留且类型引用已改写
     （var 尾分号是独立空语句，计数与取位均跳过）。 */
  node_t *body = func_body_of(f_clone);
  ASSERT_EQ(body_stmt_count(body), 2u);
  node_t *x_clone = body_nth_stmt(body, 0);
  ASSERT_EQ(node_get_kind(x_clone), CLUX_NODE_STMT_VAR);
  text = node_text(a, stmt_var_node_get_type(x_clone));
  EXPECT_STREQ(text, "f$Foo");
  allocator_free(a, (void **)&text);
  node_t *y_clone = body_nth_stmt(body, 1);
  text = node_text(a, stmt_var_node_get_type(y_clone));
  EXPECT_STREQ(text, "f$Foo");
  allocator_free(a, (void **)&text);
  /* 原 AST 不可变：结构计数与 walk 前一致。 */
  EXPECT_EQ(program_node_get_stmt_count(ast), orig_top);
  EXPECT_EQ(stmt_block_node_get_stmt_count(orig_body), orig_body_count);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 嵌套递归拼接：f 内 g 内的 type 提升为 f$g$T；g 的克隆体内引用改写。 */
TEST(SemaTest, NestedHoistChainUsesEnclosingFunctionNames) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  func g(): void {\n"
                           "    type T = i32;\n"
                           "    var t: T;\n"
                           "  }\n"
                           "  g();\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  /* f + g + T 两个提升项。 */
  ASSERT_EQ(program_node_get_stmt_count(decls), 3u);
  node_t *t_clone = program_node_get_stmt(decls, 2);
  ASSERT_EQ(node_get_kind(t_clone), CLUX_NODE_STMT_TYPE);
  char *text = node_text(a, stmt_type_node_get_name(t_clone));
  EXPECT_STREQ(text, "f$g$T");
  allocator_free(a, (void **)&text);
  /* f 体内 g 调用保留（g 定义摘除后非空 stmt 只剩调用）。 */
  node_t *f_body = func_body_of(program_node_get_stmt(decls, 0));
  ASSERT_EQ(body_stmt_count(f_body), 1u);
  /* g 克隆体内 type 声明摘除、var t 类型改写。 */
  node_t *g_clone = program_node_get_stmt(decls, 1);
  node_t *g_body = func_body_of(g_clone);
  ASSERT_EQ(body_stmt_count(g_body), 1u);
  node_t *t_var = body_nth_stmt(g_body, 0);
  ASSERT_EQ(node_get_kind(t_var), CLUX_NODE_STMT_VAR);
  text = node_text(a, stmt_var_node_get_type(t_var));
  EXPECT_STREQ(text, "f$g$T");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 同函数内同名（作用域遮蔽）Phase 2 拒绝：ambiguous diag，不 emit。 */
TEST(SemaTest, ShadowedLocalDeclIsDiagnosed) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  if (true) { struct Foo { a: i32; } }\n"
                           "  if (false) { struct Foo { a: i32; } }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_TRUE(sema_has_error(sema));
  ASSERT_EQ(clux_context_diag_count(ctx), 1u);
  string_t *text =
      clux_diag_to_string((clux_diag_t *)clux_context_get_diag(ctx, 0), a);
  EXPECT_NE(strstr(string_get_data(text), "ambiguous local declaration 'Foo'"),
            nullptr);
  delete_string(&text);
  EXPECT_EQ(sema_emit(sema), nullptr);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 不同函数的局部同名类型互不冲突：f$Foo 与 g$Foo 各自提升合法。 */
TEST(SemaTest, SameLocalNameInDifferentFunctionsDoesNotConflict) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void { type Foo = i32; var x: Foo; }\n"
                           "func g(): void { type Foo = i32; var y: Foo; }\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  /* f、g + 两个提升项，名字互异。 */
  ASSERT_EQ(program_node_get_stmt_count(decls), 4u);
  char *text =
      node_text(a, stmt_type_node_get_name(program_node_get_stmt(decls, 2)));
  EXPECT_STREQ(text, "f$Foo");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_type_node_get_name(program_node_get_stmt(decls, 3)));
  EXPECT_STREQ(text, "g$Foo");
  allocator_free(a, (void **)&text);
  /* f 内 x 类型指向 f$Foo（不被 g 的表误改）。 */
  node_t *f_body = func_body_of(program_node_get_stmt(decls, 0));
  node_t *x_var = body_nth_stmt(f_body, 0);
  text = node_text(a, stmt_var_node_get_type(x_var));
  EXPECT_STREQ(text, "f$Foo");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- Phase 2：实例化名回填最小版（§2.3 只类型） ---- */

/* 顶层定位：分号空语句（STMT_EMPTY）混在 program stmts 中，
   按 kind 扫描取第 nth 条业务语句。 */
static node_t *decls_nth_of_kind(node_t *decls, node_kind_t kind, size_t nth) {
  size_t seen = 0;
  for (size_t idx = 0; idx < program_node_get_stmt_count(decls); idx++) {
    node_t *stmt = program_node_get_stmt(decls, idx);
    if (stmt && node_get_kind(stmt) == kind) {
      if (seen == nth) {
        return stmt;
      }
      seen++;
    }
  }
  return NULL;
}

/* 泛型使用点折叠：List[i32] → 单个名字节点 List$i32（intern 文本）。 */
TEST(SemaTest, InstantiatesTypeNameAtUseSite) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, "type List[T] = [4]T;\nvar a: List[i32];\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *var_clone = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  ASSERT_NE(var_clone, nullptr);
  node_t *type_slot = stmt_var_node_get_type(var_clone);
  ASSERT_NE(type_slot, nullptr);
  EXPECT_EQ(node_get_kind(type_slot), CLUX_NODE_LITERAL_IDENTIFIER);
  char *text = node_text(a, type_slot);
  EXPECT_STREQ(text, "List$i32");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 同一组合多处使用点都改写，module_declare 静默去重（零 diag）。 */
TEST(SemaTest, SameInstantCombinationIsDeduped) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "type P[A,B] = [4]A;\n"
                           "var a: P[i32,str];\n"
                           "var b: P[i32,str];\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *a_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  node_t *b_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 1);
  ASSERT_NE(a_var, nullptr);
  ASSERT_NE(b_var, nullptr);
  char *text = node_text(a, stmt_var_node_get_type(a_var));
  EXPECT_STREQ(text, "P$i32$str");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_type(b_var));
  EXPECT_STREQ(text, "P$i32$str");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* Phase 2 边界固化：复合实参（指针类型）不是名字形态，使用点不折叠，
   SUBSCRIPT 原样保留。 */
TEST(SemaTest, CompositeInstantArgumentIsNotFolded) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, "type W[T] = [4]T;\nvar a: W[*i32];\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *var_clone = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  ASSERT_NE(var_clone, nullptr);
  node_t *type_slot = stmt_var_node_get_type(var_clone);
  ASSERT_NE(type_slot, nullptr);
  EXPECT_EQ(node_get_kind(type_slot), CLUX_NODE_EXPR_SUBSCRIPT);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}
