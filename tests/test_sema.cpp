#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/location.h"
#include "core/program.h"
#include "engine/context.h"
#include "engine/diag.h"
#include "parser/node.h"
#include "parser/node_assign.h"
#include "parser/node_builtin.h"
#include "parser/node_binary.h"
#include "parser/node_call.h"
#include "parser/node_construct.h"
#include "parser/node_field.h"
#include "parser/node_func_decl.h"
#include "parser/node_program.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_for.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_type.h"
#include "parser/node_stmt_var.h"
#include "parser/node_struct_decl.h"
#include "parser/node_struct_member.h"
#include "parser/node_subscript.h"
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

/* ---- Phase 3：常量回填（§2.1）+ 推断显式化（§2.8） ---- */

/* comptime var 求值后定义语句摘除、引用点折叠为字面量；
   未经类型注解的 var 同时完成推断显式化（TYPE 槽回填）。 */
TEST(SemaTest, ComptimeVarIsFoldedIntoUseSites) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, "comptime var X = 2 + 3;\nvar a = X;\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  /* comptime var 摘除后只剩 var a。 */
  node_t *var_clone = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  ASSERT_NE(var_clone, nullptr);
  EXPECT_EQ(decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 1), nullptr);
  char *text = node_text(a, stmt_var_node_get_name(var_clone));
  EXPECT_STREQ(text, "a");
  allocator_free(a, (void **)&text);
  /* 引用点 X 折叠为 5i32，TYPE 槽回填 i32（§2.8 联动）。 */
  node_t *value = stmt_var_node_get_value(var_clone);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(node_get_kind(value), CLUX_NODE_LITERAL_NUMERIC);
  text = node_text(a, value);
  EXPECT_STREQ(text, "5i32");
  allocator_free(a, (void **)&text);
  node_t *type_slot = stmt_var_node_get_type(var_clone);
  ASSERT_NE(type_slot, nullptr);
  EXPECT_EQ(node_get_kind(type_slot), CLUX_NODE_LITERAL_IDENTIFIER);
  text = node_text(a, type_slot);
  EXPECT_STREQ(text, "i32");
  allocator_free(a, (void **)&text);
  /* 原 AST 只读：X 定义语句仍在原树。 */
  size_t orig_comptime = 0;
  for (size_t idx = 0; idx < program_node_get_stmt_count(ast); idx++) {
    node_t *stmt = program_node_get_stmt(ast, idx);
    if (stmt && node_get_kind(stmt) == CLUX_NODE_STMT_VAR &&
        stmt_var_node_get_comptime(stmt)) {
      orig_comptime++;
    }
  }
  EXPECT_EQ(orig_comptime, 1u);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 引用链：后定义引用先定义（A → A+2），函数体内引用顶层常量同样折叠。 */
TEST(SemaTest, ComptimeReferenceChainAndFuncBodyUse) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "comptime var A = 1;\n"
                           "comptime var B = A + 2;\n"
                           "func f(): i32 { return B; }\n"
                           "var c = B;\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  /* 两个 comptime var 均摘除，业务 var 只剩 c。 */
  node_t *c_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  ASSERT_NE(c_var, nullptr);
  EXPECT_EQ(decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 1), nullptr);
  char *text = node_text(a, stmt_var_node_get_value(c_var));
  EXPECT_STREQ(text, "3i32");
  allocator_free(a, (void **)&text);
  /* 函数体 return B 的 B 折叠为 3i32。 */
  node_t *f_clone = decls_nth_of_kind(decls, CLUX_NODE_STMT_FUNC, 0);
  ASSERT_NE(f_clone, nullptr);
  node_t *body = func_body_of(f_clone);
  ASSERT_NE(body, nullptr);
  ASSERT_EQ(body_stmt_count(body), 1u);
  node_t *ret = body_nth_stmt(body, 0);
  ASSERT_EQ(node_get_kind(ret), CLUX_NODE_STMT_RETURN);
  node_t *ret_value = node_get_child(ret, CLUX_STMT_RETURN_VALUE);
  ASSERT_NE(ret_value, nullptr);
  EXPECT_EQ(node_get_kind(ret_value), CLUX_NODE_LITERAL_NUMERIC);
  text = node_text(a, ret_value);
  EXPECT_STREQ(text, "3i32");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 推断显式化（§2.8）：var a = 1 → var a: i32 = 1i32；
   布尔推导为 bool 且文本不补后缀。 */
TEST(SemaTest, InferenceExplicitizesUnsuffixedLiterals) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast =
      parse_clux(a, &p, "var a = 1;\nvar b = true;\nvar f = 1.5f64;\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *a_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  ASSERT_NE(a_var, nullptr);
  char *text = node_text(a, stmt_var_node_get_type(a_var));
  EXPECT_STREQ(text, "i32");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_value(a_var));
  EXPECT_STREQ(text, "1i32");
  allocator_free(a, (void **)&text);
  node_t *b_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 1);
  ASSERT_NE(b_var, nullptr);
  text = node_text(a, stmt_var_node_get_type(b_var));
  EXPECT_STREQ(text, "bool");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_value(b_var));
  EXPECT_STREQ(text, "true");
  allocator_free(a, (void **)&text);
  /* 后缀字面量直接定类型，文本不再补。 */
  node_t *f_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 2);
  ASSERT_NE(f_var, nullptr);
  text = node_text(a, stmt_var_node_get_type(f_var));
  EXPECT_STREQ(text, "f64");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_value(f_var));
  EXPECT_STREQ(text, "1.5f64");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 显式 TYPE 不回填、已带后缀的 VALUE 文本不动（§2.8 只补缺失）。 */
TEST(SemaTest, InferenceDoesNotTouchExplicitTypes) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, "var x: i32 = 1;\nvar y: i64 = 2i64;\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *x_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  ASSERT_NE(x_var, nullptr);
  char *text = node_text(a, stmt_var_node_get_value(x_var));
  EXPECT_STREQ(text, "1");
  allocator_free(a, (void **)&text);
  node_t *y_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 1);
  ASSERT_NE(y_var, nullptr);
  text = node_text(a, stmt_var_node_get_value(y_var));
  EXPECT_STREQ(text, "2i64");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 不可求值（引用普通 var）与除零：各报一条 ERROR diag，不 emit。 */
TEST(SemaTest, NonConstantOrDivZeroComptimeIsDiagnosed) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "var y: i32 = 1;\n"
                           "comptime var X = y;\n"
                           "comptime var Z = 1 / 0;\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_TRUE(sema_has_error(sema));
  ASSERT_EQ(clux_context_diag_count(ctx), 2u);
  string_t *text =
      clux_diag_to_string((clux_diag_t *)clux_context_get_diag(ctx, 0), a);
  EXPECT_NE(strstr(string_get_data(text), "'y' is not a comptime value"),
            nullptr);
  delete_string(&text);
  text = clux_diag_to_string((clux_diag_t *)clux_context_get_diag(ctx, 1), a);
  EXPECT_NE(strstr(string_get_data(text), "division by zero"), nullptr);
  delete_string(&text);
  EXPECT_EQ(sema_emit(sema), nullptr);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- Phase 4：defer 脱糖（§2.4）+ comptime if 剪枝（§2.5）+
   comptime foreach 展开（§2.6）+ foreach 脱糖（§2.7） ---- */

/* helper：STMT_EXPR(EXPR_CALL) 语句的 callee 文本（defer 调用断言）。 */
static char *call_stmt_callee_text(allocator_t *a, node_t *stmt) {
  node_t *expr = stmt_expr_node_get_expr(stmt);
  if (!expr || node_get_kind(expr) != CLUX_NODE_EXPR_CALL) {
    return NULL;
  }
  return node_text(a, call_node_get_callee(expr));
}

/* comptime if 剪枝：真臂保留为块、假臂无 else 整条摘除、
   假臂有 else 替换为 else 块（块语句位，不残留 COMPTIME_IF）。 */
TEST(SemaTest, ComptimeIfPrunesToSelectedArm) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast =
      parse_clux(a,
                 &p,
                 "func f(): void {\n"
                 "  comptime if (true) { var a = 1; }\n"
                 "  comptime if (false) { var b = 2; }\n"
                 "  comptime if (false) { var c = 3; } else { var d = 4; }\n"
                 "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *f_clone = decls_nth_of_kind(decls, CLUX_NODE_STMT_FUNC, 0);
  ASSERT_NE(f_clone, nullptr);
  node_t *body = func_body_of(f_clone);
  /* 假臂无 else 整条摘除：只剩两个臂块。 */
  ASSERT_EQ(body_stmt_count(body), 2u);
  node_t *arm0 = body_nth_stmt(body, 0);
  ASSERT_EQ(node_get_kind(arm0), CLUX_NODE_STMT_BLOCK);
  ASSERT_EQ(body_stmt_count(arm0), 1u);
  node_t *a_var = body_nth_stmt(arm0, 0);
  ASSERT_EQ(node_get_kind(a_var), CLUX_NODE_STMT_VAR);
  char *text = node_text(a, stmt_var_node_get_name(a_var));
  EXPECT_STREQ(text, "a");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_value(a_var));
  EXPECT_STREQ(text, "1i32");
  allocator_free(a, (void **)&text);
  node_t *arm1 = body_nth_stmt(body, 1);
  ASSERT_EQ(node_get_kind(arm1), CLUX_NODE_STMT_BLOCK);
  node_t *d_var = body_nth_stmt(arm1, 0);
  ASSERT_EQ(node_get_kind(d_var), CLUX_NODE_STMT_VAR);
  text = node_text(a, stmt_var_node_get_name(d_var));
  EXPECT_STREQ(text, "d");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* else-if 链：外假 → ELSE 槽嵌套 comptime if（真）→ 最终选中第二臂。 */
TEST(SemaTest, ComptimeIfElseIfChainSelectsInnerArm) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  comptime if (false) { var a = 1; }"
                           " else comptime if (true) { var b = 2; }"
                           " else { var c = 3; }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *f_clone = decls_nth_of_kind(decls, CLUX_NODE_STMT_FUNC, 0);
  ASSERT_NE(f_clone, nullptr);
  node_t *body = func_body_of(f_clone);
  ASSERT_EQ(body_stmt_count(body), 1u);
  node_t *arm = body_nth_stmt(body, 0);
  ASSERT_EQ(node_get_kind(arm), CLUX_NODE_STMT_BLOCK);
  ASSERT_EQ(body_stmt_count(arm), 1u);
  node_t *b_var = body_nth_stmt(arm, 0);
  ASSERT_EQ(node_get_kind(b_var), CLUX_NODE_STMT_VAR);
  char *text = node_text(a, stmt_var_node_get_name(b_var));
  EXPECT_STREQ(text, "b");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 顶层 comptime if：选中臂是块，声明逐条拼进 PROGRAM（升为模块级）；
   假臂无 else 整条消失。 */
TEST(SemaTest, TopLevelComptimeIfSplicesArmToProgram) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "comptime if (true) { struct S { a: i32; } }\n"
                           "comptime if (false) { var dead = 1; }\n"
                           "var live = 2i32;\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *s_clone = decls_nth_of_kind(decls, CLUX_NODE_STMT_STRUCT, 0);
  ASSERT_NE(s_clone, nullptr);
  char *text = node_text(
      a, struct_decl_node_get_name(stmt_struct_node_get_struct_decl(s_clone)));
  EXPECT_STREQ(text, "S");
  allocator_free(a, (void **)&text);
  /* dead 不存在，业务 var 只剩 live。 */
  node_t *live_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  ASSERT_NE(live_var, nullptr);
  text = node_text(a, stmt_var_node_get_name(live_var));
  EXPECT_STREQ(text, "live");
  allocator_free(a, (void **)&text);
  EXPECT_EQ(decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 1), nullptr);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 条件不可求值（引用普通 var）= 硬错误：ERROR diag，emit 阻断。 */
TEST(SemaTest, UnevaluableComptimeIfConditionIsDiagnosed) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast =
      parse_clux(a,
                 &p,
                 "var y = 1;\n"
                 "func f(): void { comptime if (y) { var a = 1; } }\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_TRUE(sema_has_error(sema));
  string_t *text =
      clux_diag_to_string((clux_diag_t *)clux_context_get_diag(ctx, 0), a);
  EXPECT_NE(strstr(string_get_data(text), "'y' is not a comptime value"),
            nullptr);
  delete_string(&text);
  EXPECT_EQ(sema_emit(sema), nullptr);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* defer 脱糖：定义点换 var __deferN = 匿名闭包（(): void { body }），
   块尾追加 __deferN(); 调用（STMT_EXPR 包 EXPR_CALL）。 */
TEST(SemaTest, DeferDesugarsToClosureAndTailCall) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  defer { var t = 1; }\n"
                           "  var x = 2;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *f_clone = decls_nth_of_kind(decls, CLUX_NODE_STMT_FUNC, 0);
  ASSERT_NE(f_clone, nullptr);
  node_t *body = func_body_of(f_clone);
  /* 闭包 var、x、尾调用：三条。 */
  ASSERT_EQ(body_stmt_count(body), 3u);
  node_t *defer_var = body_nth_stmt(body, 0);
  ASSERT_EQ(node_get_kind(defer_var), CLUX_NODE_STMT_VAR);
  char *text = node_text(a, stmt_var_node_get_name(defer_var));
  EXPECT_STREQ(text, "__defer0");
  allocator_free(a, (void **)&text);
  node_t *fn = stmt_var_node_get_value(defer_var);
  ASSERT_NE(fn, nullptr);
  EXPECT_EQ(node_get_kind(fn), CLUX_NODE_EXPR_FUNC_DECL);
  /* 匿名闭包：NAME 槽 NULL，RETURN 为 void，body 是原 defer 体克隆。 */
  EXPECT_EQ(func_decl_node_get_name(fn), nullptr);
  text = node_text(a, func_decl_node_get_return(fn));
  EXPECT_STREQ(text, "void");
  allocator_free(a, (void **)&text);
  node_t *fn_body = func_decl_node_get_body(fn);
  ASSERT_NE(fn_body, nullptr);
  ASSERT_EQ(body_stmt_count(fn_body), 1u);
  node_t *t_var = body_nth_stmt(fn_body, 0);
  ASSERT_EQ(node_get_kind(t_var), CLUX_NODE_STMT_VAR);
  text = node_text(a, stmt_var_node_get_name(t_var));
  EXPECT_STREQ(text, "t");
  allocator_free(a, (void **)&text);
  /* 中间 var x 原样保留。 */
  node_t *x_var = body_nth_stmt(body, 1);
  ASSERT_EQ(node_get_kind(x_var), CLUX_NODE_STMT_VAR);
  text = node_text(a, stmt_var_node_get_name(x_var));
  EXPECT_STREQ(text, "x");
  allocator_free(a, (void **)&text);
  /* 尾部：__defer0(); 调用。 */
  node_t *tail = body_nth_stmt(body, 2);
  ASSERT_EQ(node_get_kind(tail), CLUX_NODE_STMT_EXPR);
  text = call_stmt_callee_text(a, tail);
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(text, "__defer0");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 双 defer 的 LIFO：先 __defer1(); 再 __defer0();；语句流中直接子级
   return 前同样逆序插入调用。 */
TEST(SemaTest, DeferLifoOrderAndReturnPreCalls) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  defer { var a = 1; }\n"
                           "  defer { var b = 2; }\n"
                           "  return;\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *f_clone = decls_nth_of_kind(decls, CLUX_NODE_STMT_FUNC, 0);
  ASSERT_NE(f_clone, nullptr);
  node_t *body = func_body_of(f_clone);
  /* 闭包 var ×2 + return 前调用 ×2 + return。 */
  ASSERT_EQ(body_stmt_count(body), 5u);
  char *text = node_text(a, stmt_var_node_get_name(body_nth_stmt(body, 0)));
  EXPECT_STREQ(text, "__defer0");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_name(body_nth_stmt(body, 1)));
  EXPECT_STREQ(text, "__defer1");
  allocator_free(a, (void **)&text);
  /* LIFO：return 前先调 __defer1 再调 __defer0。 */
  node_t *call1 = body_nth_stmt(body, 2);
  ASSERT_EQ(node_get_kind(call1), CLUX_NODE_STMT_EXPR);
  text = call_stmt_callee_text(a, call1);
  EXPECT_STREQ(text, "__defer1");
  allocator_free(a, (void **)&text);
  node_t *call0 = body_nth_stmt(body, 3);
  ASSERT_EQ(node_get_kind(call0), CLUX_NODE_STMT_EXPR);
  text = call_stmt_callee_text(a, call0);
  EXPECT_STREQ(text, "__defer0");
  allocator_free(a, (void **)&text);
  EXPECT_EQ(node_get_kind(body_nth_stmt(body, 4)), CLUX_NODE_STMT_RETURN);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* foreach 脱糖（§2.7）：{ var __i: i32 = 0i32;
   for (; __i < #len(items); __i = __i + 1) { var x = items[__i]; <body>; } } */
TEST(SemaTest, ForeachDesugarsToIndexedFor) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "var items: [3]i32;\n"
                           "func f(): void {\n"
                           "  foreach (x of items) { var y = x; }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *f_clone = decls_nth_of_kind(decls, CLUX_NODE_STMT_FUNC, 0);
  ASSERT_NE(f_clone, nullptr);
  node_t *body = func_body_of(f_clone);
  /* 脱糖产物整体是一个外包块。 */
  ASSERT_EQ(body_stmt_count(body), 1u);
  node_t *wrap = body_nth_stmt(body, 0);
  ASSERT_EQ(node_get_kind(wrap), CLUX_NODE_STMT_BLOCK);
  ASSERT_EQ(body_stmt_count(wrap), 2u);
  /* var __i = 0（§2.8 联动回填 i32）。 */
  node_t *idx_var = body_nth_stmt(wrap, 0);
  ASSERT_EQ(node_get_kind(idx_var), CLUX_NODE_STMT_VAR);
  char *text = node_text(a, stmt_var_node_get_name(idx_var));
  EXPECT_STREQ(text, "__i");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_value(idx_var));
  EXPECT_STREQ(text, "0i32");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_type(idx_var));
  EXPECT_STREQ(text, "i32");
  allocator_free(a, (void **)&text);
  /* for：INIT NULL、COND 为 __i < #len(items)、UPDATE 为
     __i = __i + 1、BODY 含 item var（items[__i]）。 */
  node_t *loop = body_nth_stmt(wrap, 1);
  ASSERT_EQ(node_get_kind(loop), CLUX_NODE_STMT_FOR);
  EXPECT_EQ(stmt_for_node_get_init(loop), nullptr);
  node_t *cond = stmt_for_node_get_condition(loop);
  ASSERT_NE(cond, nullptr);
  EXPECT_EQ(node_get_kind(cond), CLUX_NODE_EXPR_BINARY);
  text = node_text(a, node_get_child(cond, 0));
  EXPECT_STREQ(text, "__i");
  allocator_free(a, (void **)&text);
  text = node_text(a, node_get_child(cond, 1));
  EXPECT_STREQ(text, "<");
  allocator_free(a, (void **)&text);
  node_t *len = node_get_child(cond, 2);
  ASSERT_NE(len, nullptr);
  EXPECT_EQ(node_get_kind(len), CLUX_NODE_EXPR_BUILTIN);
  text = node_text(a, builtin_node_get_name(len));
  EXPECT_STREQ(text, "len");
  allocator_free(a, (void **)&text);
  ASSERT_EQ(builtin_node_get_arg_count(len), 1u);
  text = node_text(a, builtin_node_get_arg(len, 0));
  EXPECT_STREQ(text, "items");
  allocator_free(a, (void **)&text);
  node_t *update = stmt_for_node_get_update(loop);
  ASSERT_NE(update, nullptr);
  EXPECT_EQ(node_get_kind(update), CLUX_NODE_EXPR_ASSIGN);
  text = node_text(a, assign_node_get_left(update));
  EXPECT_STREQ(text, "__i");
  allocator_free(a, (void **)&text);
  node_t *step = assign_node_get_value(update);
  ASSERT_NE(step, nullptr);
  EXPECT_EQ(node_get_kind(step), CLUX_NODE_EXPR_BINARY);
  text = node_text(a, node_get_child(step, 1));
  EXPECT_STREQ(text, "+");
  allocator_free(a, (void **)&text);
  text = node_text(a, node_get_child(step, 2));
  EXPECT_STREQ(text, "1");
  allocator_free(a, (void **)&text);
  /* 循环体：var x = items[__i]; + 原 body 块。 */
  node_t *loop_body = stmt_for_node_get_body(loop);
  ASSERT_NE(loop_body, nullptr);
  EXPECT_EQ(node_get_kind(loop_body), CLUX_NODE_STMT_BLOCK);
  ASSERT_EQ(body_stmt_count(loop_body), 2u);
  node_t *item_var = body_nth_stmt(loop_body, 0);
  ASSERT_EQ(node_get_kind(item_var), CLUX_NODE_STMT_VAR);
  text = node_text(a, stmt_var_node_get_name(item_var));
  EXPECT_STREQ(text, "x");
  allocator_free(a, (void **)&text);
  node_t *index = stmt_var_node_get_value(item_var);
  ASSERT_NE(index, nullptr);
  EXPECT_EQ(node_get_kind(index), CLUX_NODE_EXPR_SUBSCRIPT);
  text = node_text(a, subscript_node_get_base(index));
  EXPECT_STREQ(text, "items");
  allocator_free(a, (void **)&text);
  text = node_text(a, subscript_node_get_arg(index, 0));
  EXPECT_STREQ(text, "__i");
  allocator_free(a, (void **)&text);
  node_t *inner_body = body_nth_stmt(loop_body, 1);
  ASSERT_EQ(node_get_kind(inner_body), CLUX_NODE_STMT_BLOCK);
  ASSERT_EQ(body_stmt_count(inner_body), 1u);
  node_t *y_var = body_nth_stmt(inner_body, 0);
  ASSERT_EQ(node_get_kind(y_var), CLUX_NODE_STMT_VAR);
  text = node_text(a, stmt_var_node_get_name(y_var));
  EXPECT_STREQ(text, "y");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* comptime foreach 展开（§2.6）：元组字面按元素逐份拷贝 body，
   迭代变量引用替换为元素克隆（§2.8 联动回填类型）。 */
TEST(SemaTest, ComptimeForeachExpandsTupleWithSubstitution) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  comptime foreach (x of <1, 2>) { var y = x; }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *f_clone = decls_nth_of_kind(decls, CLUX_NODE_STMT_FUNC, 0);
  ASSERT_NE(f_clone, nullptr);
  node_t *body = func_body_of(f_clone);
  /* 两份 body 展开块。 */
  ASSERT_EQ(body_stmt_count(body), 2u);
  const char *expected[] = {"1i32", "2i32"};
  for (size_t idx = 0; idx < 2; idx++) {
    node_t *copy = body_nth_stmt(body, idx);
    ASSERT_EQ(node_get_kind(copy), CLUX_NODE_STMT_BLOCK);
    ASSERT_EQ(body_stmt_count(copy), 1u);
    node_t *y_var = body_nth_stmt(copy, 0);
    ASSERT_EQ(node_get_kind(y_var), CLUX_NODE_STMT_VAR);
    char *text = node_text(a, stmt_var_node_get_name(y_var));
    EXPECT_STREQ(text, "y");
    allocator_free(a, (void **)&text);
    /* 迭代变量 x 已替换为元素字面量并回填 i32。 */
    text = node_text(a, stmt_var_node_get_value(y_var));
    EXPECT_STREQ(text, expected[idx]);
    allocator_free(a, (void **)&text);
    text = node_text(a, stmt_var_node_get_type(y_var));
    EXPECT_STREQ(text, "i32");
    allocator_free(a, (void **)&text);
  }
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* comptime foreach 的 iterable 非元组字面 = 硬错误：ERROR diag，emit 阻断。 */
TEST(SemaTest, ComptimeForeachNonTupleIterableIsDiagnosed) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  comptime foreach (x of items) { var y = x; }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_TRUE(sema_has_error(sema));
  ASSERT_GE(clux_context_diag_count(ctx), 1u);
  string_t *text =
      clux_diag_to_string((clux_diag_t *)clux_context_get_diag(ctx, 0), a);
  EXPECT_NE(strstr(string_get_data(text),
                   "comptime foreach requires a tuple literal"),
            nullptr);
  delete_string(&text);
  EXPECT_EQ(sema_emit(sema), nullptr);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- Phase 5：泛型实例化完整版（§2.3） ---- */

/* 深度优先取第一个 identifier 叶子（代换结果验证用）。 */
static node_t *first_identifier_leaf(node_t *node) {
  if (!node) {
    return nullptr;
  }
  if (node_get_kind(node) == CLUX_NODE_LITERAL_IDENTIFIER) {
    return node;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    node_t *found = first_identifier_leaf((node_t *)vec_get(children, idx));
    if (found) {
      return found;
    }
  }
  return nullptr;
}

/* 泛型定义不进输出，尾部追加代换后的实例声明；同组合多使用点只产
   一个实例体；GENERICS 槽摘除。 */
TEST(SemaTest, GenericDefinitionIsReplacedBySubstitutedInstanceBody) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(
      a, &p, "type List[T] = [4]T;\nvar a: List[i32];\nvar b: List[i32];\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  /* 定义被替代且同组合只产一个实例体：STMT_TYPE 恰一条。 */
  node_t *inst = decls_nth_of_kind(decls, CLUX_NODE_STMT_TYPE, 0);
  ASSERT_NE(inst, nullptr);
  EXPECT_EQ(decls_nth_of_kind(decls, CLUX_NODE_STMT_TYPE, 1), nullptr);
  char *text = node_text(a, stmt_type_node_get_name(inst));
  EXPECT_STREQ(text, "List$i32");
  allocator_free(a, (void **)&text);
  /* 实例体非泛型：GENERICS 槽摘除。 */
  EXPECT_EQ(stmt_type_node_get_generics(inst), nullptr);
  /* 体内形参 T 代换为 i32。 */
  node_t *elem = first_identifier_leaf(stmt_type_node_get_value(inst));
  ASSERT_NE(elem, nullptr);
  text = node_text(a, elem);
  EXPECT_STREQ(text, "i32");
  allocator_free(a, (void **)&text);
  /* 两个使用点都折叠为实例名。 */
  node_t *a_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  node_t *b_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 1);
  ASSERT_NE(a_var, nullptr);
  ASSERT_NE(b_var, nullptr);
  text = node_text(a, stmt_var_node_get_type(a_var));
  EXPECT_STREQ(text, "List$i32");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_type(b_var));
  EXPECT_STREQ(text, "List$i32");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 多形参 struct 实例：字段类型逐一按序代换。 */
TEST(SemaTest, GenericStructInstanceSubstitutesFieldTypes) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(
      a,
      &p,
      "struct Pair[A, B] { first: A; second: B; }\nvar p: Pair[i32, f64];\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *inst = decls_nth_of_kind(decls, CLUX_NODE_STMT_STRUCT, 0);
  ASSERT_NE(inst, nullptr);
  EXPECT_EQ(decls_nth_of_kind(decls, CLUX_NODE_STMT_STRUCT, 1), nullptr);
  node_t *struct_decl = stmt_struct_node_get_struct_decl(inst);
  ASSERT_NE(struct_decl, nullptr);
  char *text = node_text(a, struct_decl_node_get_name(struct_decl));
  EXPECT_STREQ(text, "Pair$i32$f64");
  allocator_free(a, (void **)&text);
  EXPECT_EQ(struct_decl_node_get_generics(struct_decl), nullptr);
  node_t *members = struct_decl_node_get_body(struct_decl);
  ASSERT_NE(members, nullptr);
  ASSERT_EQ(node_get_kind(members), CLUX_NODE_STRUCT_MEMBERS);
  ASSERT_EQ(struct_members_node_get_member_count(members), 2u);
  node_t *first = struct_members_node_get_member(members, 0);
  node_t *second = struct_members_node_get_member(members, 1);
  ASSERT_EQ(node_get_kind(first), CLUX_NODE_FIELD);
  ASSERT_EQ(node_get_kind(second), CLUX_NODE_FIELD);
  text = node_text(a, field_node_get_name(first));
  EXPECT_STREQ(text, "first");
  allocator_free(a, (void **)&text);
  text = node_text(a, field_node_get_type(first));
  EXPECT_STREQ(text, "i32");
  allocator_free(a, (void **)&text);
  text = node_text(a, field_node_get_name(second));
  EXPECT_STREQ(text, "second");
  allocator_free(a, (void **)&text);
  text = node_text(a, field_node_get_type(second));
  EXPECT_STREQ(text, "f64");
  allocator_free(a, (void **)&text);
  node_t *p_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  ASSERT_NE(p_var, nullptr);
  text = node_text(a, stmt_var_node_get_type(p_var));
  EXPECT_STREQ(text, "Pair$i32$f64");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 定义体内自引用（Node[T]）在实例体里折叠为代换后的实例名。 */
TEST(SemaTest, GenericSelfReferenceFoldsToInstanceName) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(
      a, &p, "struct Node[T] { next: *Node[T]; }\nvar n: Node[i32];\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *inst = decls_nth_of_kind(decls, CLUX_NODE_STMT_STRUCT, 0);
  ASSERT_NE(inst, nullptr);
  EXPECT_EQ(decls_nth_of_kind(decls, CLUX_NODE_STMT_STRUCT, 1), nullptr);
  node_t *members =
      struct_decl_node_get_body(stmt_struct_node_get_struct_decl(inst));
  ASSERT_NE(members, nullptr);
  node_t *next_field = struct_members_node_get_member(members, 0);
  ASSERT_EQ(node_get_kind(next_field), CLUX_NODE_FIELD);
  char *text = node_text(a, field_node_get_name(next_field));
  EXPECT_STREQ(text, "next");
  allocator_free(a, (void **)&text);
  node_t *next_type = first_identifier_leaf(field_node_get_type(next_field));
  ASSERT_NE(next_type, nullptr);
  text = node_text(a, next_type);
  EXPECT_STREQ(text, "Node$i32");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 实参数与形参数不一致 = 硬错误：ERROR diag，emit 阻断。缺参且无默认值
   报 "missing generic argument"（§8.9 P13：消息对齐 exec generic.c）。 */
TEST(SemaTest, GenericInstanceArityMismatchIsDiagnosed) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, "type P[A, B] = [4]A;\nvar v: P[i32];\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_TRUE(sema_has_error(sema));
  ASSERT_GE(clux_context_diag_count(ctx), 1u);
  string_t *text =
      clux_diag_to_string((clux_diag_t *)clux_context_get_diag(ctx, 0), a);
  EXPECT_NE(strstr(string_get_data(text), "missing generic argument 'B'"),
            nullptr);
  delete_string(&text);
  EXPECT_EQ(sema_emit(sema), nullptr);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- Phase 5：匿名构造具名化（§2.9） ---- */

/* 匿名构造点 TYPE 槽回填合成名，尾部追加 struct nonamed$1，字段类型
   按字面量推断（无后缀整数默认 i32）。 */
TEST(SemaTest, AnonymousConstructIsNamedAndSynthesized) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, "var x = .{ .a = 1, .b = 2 };\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  /* 构造点 TYPE 槽回填 nonamed$1。 */
  node_t *x_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  ASSERT_NE(x_var, nullptr);
  /* §2.9 设计示例完整形态：var 的 TYPE 槽同步回填合成名（§2.8
     构造值推断——构造值类型即构造点名）。 */
  char *var_type_text = node_text(a, stmt_var_node_get_type(x_var));
  EXPECT_STREQ(var_type_text, "nonamed$1");
  allocator_free(a, (void **)&var_type_text);
  node_t *construct = stmt_var_node_get_value(x_var);
  ASSERT_NE(construct, nullptr);
  ASSERT_EQ(node_get_kind(construct), CLUX_NODE_EXPR_CONSTRUCT);
  char *text = node_text(a, construct_node_get_type(construct));
  EXPECT_STREQ(text, "nonamed$1");
  allocator_free(a, (void **)&text);
  /* 尾部合成 struct nonamed$1 { a: i32; b: i32; }。 */
  node_t *synth = decls_nth_of_kind(decls, CLUX_NODE_STMT_STRUCT, 0);
  ASSERT_NE(synth, nullptr);
  node_t *struct_decl = stmt_struct_node_get_struct_decl(synth);
  ASSERT_NE(struct_decl, nullptr);
  text = node_text(a, struct_decl_node_get_name(struct_decl));
  EXPECT_STREQ(text, "nonamed$1");
  allocator_free(a, (void **)&text);
  node_t *members = struct_decl_node_get_body(struct_decl);
  ASSERT_NE(members, nullptr);
  ASSERT_EQ(struct_members_node_get_member_count(members), 2u);
  for (size_t idx = 0; idx < 2; idx++) {
    node_t *field = struct_members_node_get_member(members, idx);
    ASSERT_EQ(node_get_kind(field), CLUX_NODE_FIELD);
    text = node_text(a, field_node_get_name(field));
    EXPECT_STREQ(text, idx == 0 ? "a" : "b");
    allocator_free(a, (void **)&text);
    text = node_text(a, field_node_get_type(field));
    EXPECT_STREQ(text, "i32");
    allocator_free(a, (void **)&text);
  }
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 多个匿名构造按发现序递增编号，各构造点指向各自合成名；布尔字段
   推断 bool，带后缀数值以后缀定类型。 */
TEST(SemaTest, MultipleAnonConstructsIncrementCounter) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(
      a, &p, "var x = .{ .a = 1 };\nvar y = .{ .b = true, .c = 2u32 };\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *x_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  node_t *y_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 1);
  ASSERT_NE(x_var, nullptr);
  ASSERT_NE(y_var, nullptr);
  char *text =
      node_text(a, construct_node_get_type(stmt_var_node_get_value(x_var)));
  EXPECT_STREQ(text, "nonamed$1");
  allocator_free(a, (void **)&text);
  text = node_text(a, construct_node_get_type(stmt_var_node_get_value(y_var)));
  EXPECT_STREQ(text, "nonamed$2");
  allocator_free(a, (void **)&text);
  /* 合成体：nonamed$1（a:i32）与 nonamed$2（b:bool、c:u32）。 */
  node_t *first = decls_nth_of_kind(decls, CLUX_NODE_STMT_STRUCT, 0);
  node_t *second = decls_nth_of_kind(decls, CLUX_NODE_STMT_STRUCT, 1);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  node_t *first_members =
      struct_decl_node_get_body(stmt_struct_node_get_struct_decl(first));
  node_t *second_members =
      struct_decl_node_get_body(stmt_struct_node_get_struct_decl(second));
  ASSERT_EQ(struct_members_node_get_member_count(first_members), 1u);
  ASSERT_EQ(struct_members_node_get_member_count(second_members), 2u);
  text = node_text(
      a, field_node_get_type(struct_members_node_get_member(first_members, 0)));
  EXPECT_STREQ(text, "i32");
  allocator_free(a, (void **)&text);
  text = node_text(
      a,
      field_node_get_type(struct_members_node_get_member(second_members, 0)));
  EXPECT_STREQ(text, "bool");
  allocator_free(a, (void **)&text);
  text = node_text(
      a,
      field_node_get_type(struct_members_node_get_member(second_members, 1)));
  EXPECT_STREQ(text, "u32");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 位置元素（非 CONSTRUCT_FIELD）不进 struct 形态：合成体零字段，
   构造点仍具名化。 */
TEST(SemaTest, PositionalAnonConstructSynthesizesZeroFieldStruct) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, "var q = .{ 1, 2 };\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *q_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  ASSERT_NE(q_var, nullptr);
  char *text =
      node_text(a, construct_node_get_type(stmt_var_node_get_value(q_var)));
  EXPECT_STREQ(text, "nonamed$1");
  allocator_free(a, (void **)&text);
  node_t *synth = decls_nth_of_kind(decls, CLUX_NODE_STMT_STRUCT, 0);
  ASSERT_NE(synth, nullptr);
  node_t *members =
      struct_decl_node_get_body(stmt_struct_node_get_struct_decl(synth));
  ASSERT_NE(members, nullptr);
  EXPECT_EQ(struct_members_node_get_member_count(members), 0u);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* ---- Phase 5：struct 成员退化 + member_call 退化（§2.10） ---- */

/* struct 的 STMT_VAR/STMT_FUNC 成员提升为模块级 Type$name：members
   只剩 FIELD，提升项追加尾部（func 与 var 均改名）。 */
TEST(SemaTest, StructMembersAreHoistedToGlobals) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "struct AAA {\n"
                           "  x: i32;\n"
                           "  var count = 0;\n"
                           "  func get(self: *AAA): i32 { return self.x; }\n"
                           "  func helper(): i32 { return 1; }\n"
                           "}\n"
                           "var a = .AAA{ .x = 1 };\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  /* struct AAA 本体：members 只剩字段 x（var/func 成员已摘除）。 */
  node_t *aaa = decls_nth_of_kind(decls, CLUX_NODE_STMT_STRUCT, 0);
  ASSERT_NE(aaa, nullptr);
  node_t *members =
      struct_decl_node_get_body(stmt_struct_node_get_struct_decl(aaa));
  ASSERT_NE(members, nullptr);
  ASSERT_EQ(struct_members_node_get_member_count(members), 1u);
  ASSERT_EQ(node_get_kind(struct_members_node_get_member(members, 0)),
            CLUX_NODE_FIELD);
  /* 提升的方法：AAA$get 与 AAA$helper（无参 func 也提升）。 */
  node_t *get_fn = decls_nth_of_kind(decls, CLUX_NODE_STMT_FUNC, 0);
  node_t *helper_fn = decls_nth_of_kind(decls, CLUX_NODE_STMT_FUNC, 1);
  ASSERT_NE(get_fn, nullptr);
  ASSERT_NE(helper_fn, nullptr);
  char *text = node_text(
      a, func_decl_node_get_name(stmt_func_node_get_func_decl(get_fn)));
  EXPECT_STREQ(text, "AAA$get");
  allocator_free(a, (void **)&text);
  text = node_text(
      a, func_decl_node_get_name(stmt_func_node_get_func_decl(helper_fn)));
  EXPECT_STREQ(text, "AAA$helper");
  allocator_free(a, (void **)&text);
  /* 提升的 var 成员：AAA$count。 */
  node_t *count_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 1);
  ASSERT_NE(count_var, nullptr);
  text = node_text(a, stmt_var_node_get_name(count_var));
  EXPECT_STREQ(text, "AAA$count");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* receiver 为方法所属类型的具名构造 → 直匹配改写：callee 折叠为
   AAA$get，首实参为构造克隆；指针 self（*AAA）→ receiver 包 .&。 */
TEST(SemaTest, MemberCallOnNamedConstructReceiverIsDesugared) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(
      a,
      &p,
      "struct Box { v: i32; func get(self: *Box): i32 { return self.v; } }\n"
      "var r = .Box{ .v = 7 }.get();\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *r_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 0);
  ASSERT_NE(r_var, nullptr);
  node_t *call = stmt_var_node_get_value(r_var);
  ASSERT_NE(call, nullptr);
  ASSERT_EQ(node_get_kind(call), CLUX_NODE_EXPR_CALL);
  /* callee：identifier 文本 Box$get。 */
  node_t *callee = call_node_get_callee(call);
  ASSERT_NE(callee, nullptr);
  ASSERT_EQ(node_get_kind(callee), CLUX_NODE_LITERAL_IDENTIFIER);
  char *text = node_text(a, callee);
  EXPECT_STREQ(text, "Box$get");
  allocator_free(a, (void **)&text);
  /* 首实参：指针 self → 构造包 .&（binary，RIGHT NULL）。 */
  ASSERT_EQ(call_node_get_arg_count(call), 1u);
  node_t *self_arg = call_node_get_arg(call, 0);
  ASSERT_NE(self_arg, nullptr);
  ASSERT_EQ(node_get_kind(self_arg), CLUX_NODE_EXPR_BINARY);
  vec_t *bin_children = node_get_children(self_arg);
  ASSERT_EQ(node_get_kind((node_t *)vec_get(bin_children, 1)),
            CLUX_NODE_SYMBOL);
  text = node_text(a, (node_t *)vec_get(bin_children, 1));
  EXPECT_STREQ(text, ".&");
  allocator_free(a, (void **)&text);
  node_t *inner = (node_t *)vec_get(bin_children, 0);
  ASSERT_EQ(node_get_kind(inner), CLUX_NODE_EXPR_CONSTRUCT);
  text = node_text(a, construct_node_get_type(inner));
  EXPECT_STREQ(text, "Box");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 指针 self（self: *P）+ receiver 非构造：方法名全表唯一 → 改写，
   receiver 包 .& 取址后前置（值 self 形态由检查通道按签名不符报错）。 */
TEST(SemaTest, MemberCallValueSelfAndUniqueName) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(
      a,
      &p,
      "struct P { v: i32; func id(self: *P): i32 { return self.v; } }\n"
      "var b = .P{ .v = 3 };\n"
      "var c = b.id();\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *c_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 1);
  ASSERT_NE(c_var, nullptr);
  node_t *call = stmt_var_node_get_value(c_var);
  ASSERT_NE(call, nullptr);
  ASSERT_EQ(node_get_kind(call), CLUX_NODE_EXPR_CALL);
  node_t *callee = call_node_get_callee(call);
  ASSERT_EQ(node_get_kind(callee), CLUX_NODE_LITERAL_IDENTIFIER);
  char *text = node_text(a, callee);
  EXPECT_STREQ(text, "P$id");
  allocator_free(a, (void **)&text);
  /* 指针 self：首实参是 .& 取址（后缀 binary，RIGHT 槽为 NULL），
     left 就是 identifier b。 */
  ASSERT_EQ(call_node_get_arg_count(call), 1u);
  node_t *self_arg = call_node_get_arg(call, 0);
  ASSERT_EQ(node_get_kind(self_arg), CLUX_NODE_EXPR_BINARY);
  vec_t *addr_children = node_get_children(self_arg);
  ASSERT_EQ(vec_get_size(addr_children), 3u);
  EXPECT_EQ(vec_get(addr_children, 2), nullptr);
  text = node_text(a, (node_t *)vec_get(addr_children, 1));
  EXPECT_STREQ(text, ".&");
  allocator_free(a, (void **)&text);
  node_t *addr_left = (node_t *)vec_get(addr_children, 0);
  ASSERT_EQ(node_get_kind(addr_left), CLUX_NODE_LITERAL_IDENTIFIER);
  text = node_text(a, addr_left);
  EXPECT_STREQ(text, "b");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 跨 struct 同名方法 + receiver 非构造：全表不唯一 → 保持
   member_call 原样（callee 仍是 "." binary），交类型检查分期兜底。 */
TEST(SemaTest, MethodNameAmbiguityLeavesCallUnchanged) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast =
      parse_clux(a,
                 &p,
                 "struct A { func tag(self: *A): i32 { return 1; } }\n"
                 "struct B { func tag(self: *B): i32 { return 2; } }\n"
                 "var a = .A{};\n"
                 "var b = .B{};\n"
                 "var t = b.tag();\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *t_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 2);
  ASSERT_NE(t_var, nullptr);
  node_t *call = stmt_var_node_get_value(t_var);
  ASSERT_NE(call, nullptr);
  ASSERT_EQ(node_get_kind(call), CLUX_NODE_EXPR_CALL);
  node_t *callee = call_node_get_callee(call);
  ASSERT_EQ(node_get_kind(callee), CLUX_NODE_EXPR_BINARY);
  vec_t *bin_children = node_get_children(callee);
  char *text = node_text(a, (node_t *)vec_get(bin_children, 1));
  EXPECT_STREQ(text, ".");
  allocator_free(a, (void **)&text);
  text = node_text(a, (node_t *)vec_get(bin_children, 2));
  EXPECT_STREQ(text, "tag");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}
