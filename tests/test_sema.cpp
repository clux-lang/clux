#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/location.h"
#include "core/program.h"
#include "core/string.h"
#include "core/vec.h"
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
#include "parser/unparse.h"
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

/* §2.2 正路径：函数体内的类型提升到 PROGRAM 尾部（f$Foo），原位置声明
   语句摘除、引用点改写为提升名；块级 func 走闭包退化（§2.12）；
   原 AST 保持只读不变。 */
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
  /* 顶层 f 退化为闭包 var 占位 + 提升项 f$Foo（收集序 Foo 先于 g）；
     块级 g 零捕获同批退化（§2.12 P2）——struct 先全输出（$closure$f、
     $closure$f$g）、callee 后全输出（$callee$f、$callee$f$g）。 */
  ASSERT_EQ(program_node_get_stmt_count(decls), 6u);
  node_t *f_clone = program_node_get_stmt(decls, 0);
  node_t *foo_clone = program_node_get_stmt(decls, 1);
  node_t *closure_struct = program_node_get_stmt(decls, 2);
  node_t *g_closure_struct = program_node_get_stmt(decls, 3);
  node_t *callee_clone = program_node_get_stmt(decls, 4);
  node_t *g_callee_clone = program_node_get_stmt(decls, 5);
  EXPECT_EQ(node_get_kind(f_clone), CLUX_NODE_STMT_VAR);
  EXPECT_EQ(node_get_kind(foo_clone), CLUX_NODE_STMT_STRUCT);
  EXPECT_EQ(node_get_kind(closure_struct), CLUX_NODE_STMT_STRUCT);
  EXPECT_EQ(node_get_kind(g_closure_struct), CLUX_NODE_STMT_STRUCT);
  EXPECT_EQ(node_get_kind(callee_clone), CLUX_NODE_STMT_FUNC);
  EXPECT_EQ(node_get_kind(g_callee_clone), CLUX_NODE_STMT_FUNC);
  /* 提升项的名字是 mangled 名。 */
  node_t *foo_name =
      struct_decl_node_get_name(stmt_struct_node_get_struct_decl(foo_clone));
  char *text = node_text(a, foo_name);
  EXPECT_STREQ(text, "f$Foo$1");
  allocator_free(a, (void **)&text);
  /* g 的闭包 struct/callee 名。 */
  node_t *g_struct_name = struct_decl_node_get_name(
      stmt_struct_node_get_struct_decl(g_closure_struct));
  text = node_text(a, g_struct_name);
  EXPECT_STREQ(text, "$closure$f$g$2");
  allocator_free(a, (void **)&text);
  node_t *g_callee_name =
      func_decl_node_get_name(stmt_func_node_get_func_decl(g_callee_clone));
  text = node_text(a, g_callee_name);
  EXPECT_STREQ(text, "$callee$f$g$2");
  allocator_free(a, (void **)&text);
  /* f 克隆体（$callee$f）：Foo 声明摘除，g 定义点原位构造，两条 var 保留
     且类型引用已改写（var 尾分号是独立空语句，计数与取位均跳过）。 */
  node_t *body = func_body_of(callee_clone);
  ASSERT_EQ(body_stmt_count(body), 3u);
  node_t *g_var = body_nth_stmt(body, 0);
  ASSERT_EQ(node_get_kind(g_var), CLUX_NODE_STMT_VAR);
  node_t *g_var_name = stmt_var_node_get_name(g_var);
  text = node_text(a, g_var_name);
  EXPECT_STREQ(text, "g");
  allocator_free(a, (void **)&text);
  node_t *x_clone = body_nth_stmt(body, 1);
  ASSERT_EQ(node_get_kind(x_clone), CLUX_NODE_STMT_VAR);
  text = node_text(a, stmt_var_node_get_type(x_clone));
  EXPECT_STREQ(text, "f$Foo$1");
  allocator_free(a, (void **)&text);
  node_t *y_clone = body_nth_stmt(body, 2);
  text = node_text(a, stmt_var_node_get_type(y_clone));
  EXPECT_STREQ(text, "f$Foo$1");
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
  /* f 退化为闭包 var 占位 + T 提升项（f$g$T）；块级 g 零捕获同批退化
     （§2.12 P2）——struct 先全输出、callee 后全输出，g 定义点原位构造。 */
  ASSERT_EQ(program_node_get_stmt_count(decls), 6u);
  node_t *t_clone = program_node_get_stmt(decls, 1);
  ASSERT_EQ(node_get_kind(t_clone), CLUX_NODE_STMT_TYPE);
  char *text = node_text(a, stmt_type_node_get_name(t_clone));
  EXPECT_STREQ(text, "f$g$1$T$2");
  allocator_free(a, (void **)&text);
  /* f 体内：g 定义点原位构造 var g + 调用点改写为 g.$callee(g.&)。 */
  node_t *f_body = func_body_of(program_node_get_stmt(decls, 4));
  ASSERT_EQ(body_stmt_count(f_body), 2u);
  node_t *g_def = body_nth_stmt(f_body, 0);
  ASSERT_EQ(node_get_kind(g_def), CLUX_NODE_STMT_VAR);
  /* g 克隆体（$callee$f$g）内 type 声明摘除、var t 类型改写。 */
  node_t *g_clone = program_node_get_stmt(decls, 5);
  node_t *g_body = func_body_of(g_clone);
  ASSERT_EQ(body_stmt_count(g_body), 1u);
  node_t *t_var = body_nth_stmt(g_body, 0);
  ASSERT_EQ(node_get_kind(t_var), CLUX_NODE_STMT_VAR);
  text = node_text(a, stmt_var_node_get_type(t_var));
  EXPECT_STREQ(text, "f$g$1$T$2");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* §2.12 P2 收紧：块级条目退化失败（敏感名字出现在提升声明子树的名字槽）
   不再静默跌回旧提升通道（语义漂移），改报显式编译错误，emit 短路 NULL。 */
TEST(SemaTest, ClosureBailIsDiagnosedInsteadOfSilentHoist) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func f(): void {\n"
                           "  struct S { g: i32; }\n"
                           "  func g(): void { return; }\n"
                           "  var s: S = .S{ .g = 1 };\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  /* check 层合法（字段名与函数名不同命名空间），walk 零错误。 */
  EXPECT_EQ(sema_walk(sema, ast), 0);
  /* emit 期收集扫描发现敏感名字（提升 struct f$S 的字段 g 命中条目名）
   → 显式 diag + 短路，不产出 Program。 */
  vec_t *programs = sema_emit(sema);
  EXPECT_EQ(programs, nullptr);
  EXPECT_TRUE(sema_has_error(sema));
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
  /* f、g 均退化为闭包 var 占位 + 两个提升项 + 尾部合成的两组
     struct/callee，名字互异。 */
  ASSERT_EQ(program_node_get_stmt_count(decls), 8u);
  char *text =
      node_text(a, stmt_type_node_get_name(program_node_get_stmt(decls, 2)));
  EXPECT_STREQ(text, "f$Foo$1");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_type_node_get_name(program_node_get_stmt(decls, 3)));
  EXPECT_STREQ(text, "g$Foo$2");
  allocator_free(a, (void **)&text);
  /* f 内 x 类型指向 f$Foo（不被 g 的表误改）；f 已退化为 $callee$f。 */
  node_t *f_body = func_body_of(program_node_get_stmt(decls, 6));
  node_t *x_var = body_nth_stmt(f_body, 0);
  text = node_text(a, stmt_var_node_get_type(x_var));
  EXPECT_STREQ(text, "f$Foo$1");
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

/* 顶层按名定位 func：归一化后 struct 方法混入顶层（Type$method），
   用户函数保持原名，须按名区分。 */
static node_t *
decls_func_named(node_t *decls, allocator_t *a, const char *name) {
  for (size_t idx = 0; idx < program_node_get_stmt_count(decls); idx++) {
    node_t *stmt = program_node_get_stmt(decls, idx);
    if (!stmt || node_get_kind(stmt) != CLUX_NODE_STMT_FUNC) {
      continue;
    }
    char *text = node_text(
        a, func_decl_node_get_name(stmt_func_node_get_func_decl(stmt)));
    int match = text && strcmp(text, name) == 0;
    allocator_free(a, (void **)&text);
    if (match) {
      return stmt;
    }
  }
  return NULL;
}

/* 函数体内定位：全局初始化器必须是编译期常量（C 规范），调用类测试
   须把 var 移进函数体内，块内同样按 kind 扫描取第 nth 条。 */
static node_t *block_nth_of_kind(node_t *block, node_kind_t kind, size_t nth) {
  size_t seen = 0;
  for (size_t idx = 0; idx < stmt_block_node_get_stmt_count(block); idx++) {
    node_t *stmt = stmt_block_node_get_stmt(block, idx);
    if (stmt && node_get_kind(stmt) == kind) {
      if (seen == nth) {
        return stmt;
      }
      seen++;
    }
  }
  return NULL;
}

/* 同函数内同名（作用域遮蔽）仍拒绝：合成名 id 只保证提升名互异，
   引用改写通道（闭包退化引用收集 + 提升 rename 表）按名字文本工作，
   AST 包含判定落地前同名遮蔽是显式错误而非静默改错（§2.12 P3 分期）。 */
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

/* 兄弟块嵌套函数（互易名）：退化 callee 名带全局自增 id 消歧
   （§2.11 P3-1）——$callee$main$aaa$1 与 $callee$main$bbb$2。 */
TEST(SemaTest, SiblingBlockClosuresGetSequentialSynthIds) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "func main(): void {\n"
                           "  func aaa(): void { }\n"
                           "  { func bbb(): void { } }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  EXPECT_EQ(clux_context_diag_count(ctx), 0u);
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  node_t *aaa_fn = decls_func_named(decls, a, "$callee$main$aaa$1");
  ASSERT_NE(aaa_fn, nullptr);
  node_t *bbb_fn = decls_func_named(decls, a, "$callee$main$bbb$2");
  ASSERT_NE(bbb_fn, nullptr);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
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
  /* 两个 comptime var 均摘除；func f 退化为闭包 var 占位（VAR #0），
     业务 var c 是 VAR #1。 */
  node_t *c_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 1);
  ASSERT_NE(c_var, nullptr);
  EXPECT_EQ(decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 2), nullptr);
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

/* defer 脱糖：定义点换 var $defer$N = 匿名闭包（(): void { body }），
   块尾追加 $defer$N(); 调用（STMT_EXPR 包 EXPR_CALL）。 */
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
  EXPECT_STREQ(text, "$defer$0");
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
  /* 尾部：$defer$0(); 调用。 */
  node_t *tail = body_nth_stmt(body, 2);
  ASSERT_EQ(node_get_kind(tail), CLUX_NODE_STMT_EXPR);
  text = call_stmt_callee_text(a, tail);
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(text, "$defer$0");
  allocator_free(a, (void **)&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 双 defer 的 LIFO：先 $defer$1(); 再 $defer$0();；语句流中直接子级
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
  EXPECT_STREQ(text, "$defer$0");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_name(body_nth_stmt(body, 1)));
  EXPECT_STREQ(text, "$defer$1");
  allocator_free(a, (void **)&text);
  /* LIFO：return 前先调 $defer$1 再调 $defer$0。 */
  node_t *call1 = body_nth_stmt(body, 2);
  ASSERT_EQ(node_get_kind(call1), CLUX_NODE_STMT_EXPR);
  text = call_stmt_callee_text(a, call1);
  EXPECT_STREQ(text, "$defer$1");
  allocator_free(a, (void **)&text);
  node_t *call0 = body_nth_stmt(body, 3);
  ASSERT_EQ(node_get_kind(call0), CLUX_NODE_STMT_EXPR);
  text = call_stmt_callee_text(a, call0);
  EXPECT_STREQ(text, "$defer$0");
  allocator_free(a, (void **)&text);
  EXPECT_EQ(node_get_kind(body_nth_stmt(body, 4)), CLUX_NODE_STMT_RETURN);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* foreach 脱糖（§2.7）：{ var $iter = items; var $i: i32 = 0i32;
   for (; $i < #len($iter); $i = $i + 1) { var x = $iter[$i]; <body>; } }
   iterable 提升为 $iter 只求值一次；var 形态每轮重新定义迭代变量。 */
TEST(SemaTest, ForeachDesugarsToIndexedFor) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "var items: [3]i32;\n"
                           "func f(): void {\n"
                           "  foreach (var x of items) { var y = x; }\n"
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
  ASSERT_EQ(body_stmt_count(wrap), 3u);
  /* var $iter = items;（iterable 求值一次；类型从 iterable 锚点渲染）。 */
  node_t *iter_var = body_nth_stmt(wrap, 0);
  ASSERT_EQ(node_get_kind(iter_var), CLUX_NODE_STMT_VAR);
  char *text = node_text(a, stmt_var_node_get_name(iter_var));
  EXPECT_STREQ(text, "$iter");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_value(iter_var));
  EXPECT_STREQ(text, "items");
  allocator_free(a, (void **)&text);
  /* var $i = 0（§2.8 联动回填 i32）。 */
  node_t *idx_var = body_nth_stmt(wrap, 1);
  ASSERT_EQ(node_get_kind(idx_var), CLUX_NODE_STMT_VAR);
  text = node_text(a, stmt_var_node_get_name(idx_var));
  EXPECT_STREQ(text, "$i");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_value(idx_var));
  EXPECT_STREQ(text, "0i32");
  allocator_free(a, (void **)&text);
  text = node_text(a, stmt_var_node_get_type(idx_var));
  EXPECT_STREQ(text, "i32");
  allocator_free(a, (void **)&text);
  /* for：INIT NULL、COND 为 $i < #len($iter)、UPDATE 为
     $i = $i + 1、BODY 含 item var（$iter[$i]）。 */
  node_t *loop = body_nth_stmt(wrap, 2);
  ASSERT_EQ(node_get_kind(loop), CLUX_NODE_STMT_FOR);
  EXPECT_EQ(stmt_for_node_get_init(loop), nullptr);
  node_t *cond = stmt_for_node_get_condition(loop);
  ASSERT_NE(cond, nullptr);
  EXPECT_EQ(node_get_kind(cond), CLUX_NODE_EXPR_BINARY);
  text = node_text(a, node_get_child(cond, 0));
  EXPECT_STREQ(text, "$i");
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
  EXPECT_STREQ(text, "$iter");
  allocator_free(a, (void **)&text);
  node_t *update = stmt_for_node_get_update(loop);
  ASSERT_NE(update, nullptr);
  EXPECT_EQ(node_get_kind(update), CLUX_NODE_EXPR_ASSIGN);
  text = node_text(a, assign_node_get_left(update));
  EXPECT_STREQ(text, "$i");
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
  /* 循环体：var x = $iter[$i]; + 原 body 块。 */
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
  EXPECT_STREQ(text, "$iter");
  allocator_free(a, (void **)&text);
  text = node_text(a, subscript_node_get_arg(index, 0));
  EXPECT_STREQ(text, "$i");
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

/* foreach 裸名未在外层定义 = 硬错误：名字解析 diag（裸名前提是已有绑定）。 */
TEST(SemaTest, ForeachBareNameRequiresExistingBinding) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a,
                           &p,
                           "var items: [3]i32 = <0, 0, 0>;\n"
                           "func f(): void {\n"
                           "  foreach (x of items) { var y = x; }\n"
                           "}\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_TRUE(sema_has_error(sema));
  ASSERT_GE(clux_context_diag_count(ctx), 1u);
  string_t *text =
      clux_diag_to_string((clux_diag_t *)clux_context_get_diag(ctx, 0), a);
  EXPECT_NE(strstr(string_get_data(text), "undefined"), nullptr);
  delete_string(&text);
  EXPECT_EQ(sema_emit(sema), nullptr);
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
  node_t *ast =
      parse_clux(a,
                 &p,
                 "func f(): void {\n"
                 "  comptime foreach (var x of <1, 2>) { var y = x; }\n"
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
  node_t *ast =
      parse_clux(a,
                 &p,
                 "func f(): void {\n"
                 "  comptime foreach (var x of items) { var y = x; }\n"
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
  EXPECT_STREQ(text, "AAA$get$2");
  allocator_free(a, (void **)&text);
  text = node_text(
      a, func_decl_node_get_name(stmt_func_node_get_func_decl(helper_fn)));
  EXPECT_STREQ(text, "AAA$helper$3");
  allocator_free(a, (void **)&text);
  /* 提升的 var 成员：AAA$count。 */
  node_t *count_var = decls_nth_of_kind(decls, CLUX_NODE_STMT_VAR, 1);
  ASSERT_NE(count_var, nullptr);
  text = node_text(a, stmt_var_node_get_name(count_var));
  EXPECT_STREQ(text, "AAA$count$1");
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
  /* 全局初始化器必须是编译期常量（C 规范），调用移进函数体。 */
  node_t *ast = parse_clux(
      a,
      &p,
      "struct Box { v: i32; func get(self: *Box): i32 { return self.v; } }\n"
      "func main(): void { var r = .Box{ .v = 7 }.get(); }\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  /* Box$get walk 期取号 $1，main emit 期续号 $2。 */
  node_t *main_fn = decls_func_named(decls, a, "$callee$main$2");
  ASSERT_NE(main_fn, nullptr);
  node_t *r_var = block_nth_of_kind(
      func_decl_node_get_body(stmt_func_node_get_func_decl(main_fn)),
      CLUX_NODE_STMT_VAR,
      0);
  ASSERT_NE(r_var, nullptr);
  node_t *call = stmt_var_node_get_value(r_var);
  ASSERT_NE(call, nullptr);
  ASSERT_EQ(node_get_kind(call), CLUX_NODE_EXPR_CALL);
  /* callee：identifier 文本 Box$get。 */
  node_t *callee = call_node_get_callee(call);
  ASSERT_NE(callee, nullptr);
  ASSERT_EQ(node_get_kind(callee), CLUX_NODE_LITERAL_IDENTIFIER);
  char *text = node_text(a, callee);
  EXPECT_STREQ(text, "Box$get$1");
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
  /* 全局初始化器必须是编译期常量（C 规范），调用移进函数体；
     var b 的构造初值是常量，可留全局供 main 引用。 */
  node_t *ast = parse_clux(
      a,
      &p,
      "struct P { v: i32; func id(self: *P): i32 { return self.v; } }\n"
      "var b = .P{ .v = 3 };\n"
      "func main(): void { var c = b.id(); }\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  /* P$id walk 期取号 $1，main emit 期续号 $2。 */
  node_t *main_fn = decls_func_named(decls, a, "$callee$main$2");
  ASSERT_NE(main_fn, nullptr);
  node_t *c_var = block_nth_of_kind(
      func_decl_node_get_body(stmt_func_node_get_func_decl(main_fn)),
      CLUX_NODE_STMT_VAR,
      0);
  ASSERT_NE(c_var, nullptr);
  node_t *call = stmt_var_node_get_value(c_var);
  ASSERT_NE(call, nullptr);
  ASSERT_EQ(node_get_kind(call), CLUX_NODE_EXPR_CALL);
  node_t *callee = call_node_get_callee(call);
  ASSERT_EQ(node_get_kind(callee), CLUX_NODE_LITERAL_IDENTIFIER);
  char *text = node_text(a, callee);
  EXPECT_STREQ(text, "P$id$1");
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
  /* 全局初始化器必须是编译期常量（C 规范），调用移进函数体；
     var a/var b 的构造初值是常量，可留全局供 main 引用。 */
  node_t *ast =
      parse_clux(a,
                 &p,
                 "struct A { func tag(self: *A): i32 { return 1; } }\n"
                 "struct B { func tag(self: *B): i32 { return 2; } }\n"
                 "var a = .A{};\n"
                 "var b = .B{};\n"
                 "func main(): void { var t = b.tag(); }\n");
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_FALSE(sema_has_error(sema));
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  program_t *program = (program_t *)vec_get(programs, 0);
  node_t *decls = program_get_decls(program);
  /* A$tag/B$tag walk 期取号 $1/$2，main emit 期续号 $3。 */
  node_t *main_fn = decls_func_named(decls, a, "$callee$main$3");
  ASSERT_NE(main_fn, nullptr);
  node_t *t_var = block_nth_of_kind(
      func_decl_node_get_body(stmt_func_node_get_func_decl(main_fn)),
      CLUX_NODE_STMT_VAR,
      0);
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

/* ---- 端到端文本→文本：sema 归一化结果 unparse 精确断言 ---- */

/* 端到端文本→文本：src → parse → sema（walk+emit，必须无错误）
   → unparse → 与手写期望精确比较；附加断言（roundtrip=true 时）：
   unparse 输出可零错误再解析且再 unparse 幂等。
   已知缺口（roundtrip=false 佐证用例）：sema 合成混名列（f$Foo、
   List$i32、nonamed$1、AAA$count 等）含 '$'，lexer 不接受 '$'，
   unparse 输出无法零错误再解析——记录缺口交由主线定夺，主断言
   （手写期望文本精确比较）不受影响。输入必须是零解析错误的合法源
   （无初始化式 var 不是合法语法——初值必须显式给出），helper 强制
   门禁输入侧 parse 错误。 */
static void
expect_sema_text(const char *src, const char *expect, bool roundtrip = true) {
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, src);
  ASSERT_NE(ast, nullptr) << "src=" << src;
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u) << "src=" << src;
  EXPECT_EQ(sema_walk(sema, ast), 0);
  ASSERT_FALSE(sema_has_error(sema)) << "src=" << src;
  vec_t *programs = sema_emit(sema);
  ASSERT_NE(programs, nullptr);
  ASSERT_EQ(vec_get_size(programs), 1u) << "src=" << src;
  program_t *program = (program_t *)vec_get(programs, 0);
  string_t *text = program_unparse(program, a, nullptr);
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(string_get_data(text), expect) << "src=" << src;
  /* 附加：unparse 输出可再解析且幂等。 */
  if (roundtrip) {
    parser_t *rt_p = create_parser(a, "rt.clux", string_get_data(text));
    node_t *rt_ast = parser_read_program(a, rt_p);
    ASSERT_NE(rt_ast, nullptr) << "unparsed output must re-parse";
    ASSERT_EQ(vec_get_size(parser_get_error(rt_p)), 0u);
    string_t *rt_text = node_unparse(rt_ast, a, nullptr);
    ASSERT_NE(rt_text, nullptr);
    EXPECT_STREQ(string_get_data(rt_text), expect);
    delete_string(&rt_text);
    delete_node(&rt_ast);
    delete_parser(&rt_p);
  }
  delete_string(&text);
  dispose_programs(programs);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(SemaTest, HoistsLocalDeclsToProgramTailAsNormalizedText) {
  expect_sema_text(
      "func f(): void {\n"
      "  struct Foo { a: i32; }\n"
      "  func g(): void { return; }\n"
      "  var x: Foo = .Foo{ .a = 1 };\n"
      "  var y: Foo = .Foo{ .a = 2 };\n"
      "}\n",
      "var f: $closure$f$3 = .$closure$f$3{$callee = $callee$f$3};\n"
      "struct f$Foo$1 {\n"
      "  a: i32;\n"
      "}\n"
      "struct $closure$f$3 {\n"
      "  $callee: func (*$closure$f$3) -> void;\n"
      "}\n"
      "struct $closure$f$g$2 {\n"
      "  $callee: func (*$closure$f$g$2) -> void;\n"
      "}\n"
      "func $callee$f$3($env: *$closure$f$3): void {\n"
      "  var g: $closure$f$g$2 = .$closure$f$g$2{$callee = $callee$f$g$2};\n"
      "  var x: f$Foo$1 = .f$Foo$1{a = 1};\n"
      "  var y: f$Foo$1 = .f$Foo$1{a = 2};\n"
      "}\n"
      "func $callee$f$g$2($env: *$closure$f$g$2): void {\n"
      "  return;\n"
      "}\n",
      /* 混名列含 '$'（lexer 不收）→ round-trip 不可零错误，跳过。 */
      false);
}

/* §2.12 P2：块级零捕获条目定义点原位构造（无 TDZ 占位/块尾赋值），
   定义后立即可调用；调用点改写 g() → g.$callee(g.&)。 */
TEST(SemaTest, ZeroCaptureBlockClosureConstructsInPlaceAsNormalizedText) {
  expect_sema_text(
      "func f(): void {\n"
      "  func g(): void { return; }\n"
      "  g();\n"
      "}\n",
      "var f: $closure$f$2 = .$closure$f$2{$callee = $callee$f$2};\n"
      "struct $closure$f$2 {\n"
      "  $callee: func (*$closure$f$2) -> void;\n"
      "}\n"
      "struct $closure$f$g$1 {\n"
      "  $callee: func (*$closure$f$g$1) -> void;\n"
      "}\n"
      "func $callee$f$2($env: *$closure$f$2): void {\n"
      "  var g: $closure$f$g$1 = .$closure$f$g$1{$callee = $callee$f$g$1};\n"
      "  g.$callee(g.&);\n"
      "}\n"
      "func $callee$f$g$1($env: *$closure$f$g$1): void {\n"
      "  return;\n"
      "}\n",
      /* 混名列含 '$'（lexer 不收）→ round-trip 不可零错误，跳过。 */
      false);
}

TEST(SemaTest, InstantiatesTypeNameAtUseSiteAsNormalizedText) {
  expect_sema_text("type List[T] = [4]T;\nvar a: List[i32] = <1, 2, 3, 4>;\n",
                   "var a: List$i32 = <1, 2, 3, 4>;\n"
                   "type List$i32 = [4]i32;\n",
                   false);
}

TEST(SemaTest, ComptimeVarIsFoldedIntoUseSitesAsNormalizedText) {
  expect_sema_text("comptime var N = 5;\nvar a = N;\n", "var a: i32 = 5i32;\n");
}

TEST(SemaTest, InferenceExplicitizesUnsuffixedLiteralsAsNormalizedText) {
  expect_sema_text("var a = 1;\nvar b = true;\nvar f = 1.5f64;\n",
                   "var a: i32 = 1i32;\n"
                   "var b: bool = true;\n"
                   "var f: f64 = 1.5f64;\n");
}

TEST(SemaTest, ComptimeIfPrunesToSelectedArmAsNormalizedText) {
  expect_sema_text(
      "func f(): void {\n"
      "  comptime if (true) { var a = 1; }\n"
      "  comptime if (false) { var b = 2; }\n"
      "  comptime if (false) { var c = 3; } else { var d = 4; }\n"
      "}\n",
      "var f: $closure$f$1 = .$closure$f$1{$callee = $callee$f$1};\n"
      "struct $closure$f$1 {\n"
      "  $callee: func (*$closure$f$1) -> void;\n"
      "}\n"
      "func $callee$f$1($env: *$closure$f$1): void {\n"
      "  {\n"
      "    var a: i32 = 1i32;\n"
      "  }\n"
      "  {\n"
      "    var d: i32 = 4i32;\n"
      "  }\n"
      "}\n",
      /* f 退化引入 '$' → 跳过 round-trip。 */
      false);
}

TEST(SemaTest, DeferDesugarsToClosureAndTailCallAsNormalizedText) {
  expect_sema_text(
      "func f(): void {\n"
      "  defer { var t = 1; }\n"
      "  var x = 2;\n"
      "}\n",
      "var f: $closure$f$1 = .$closure$f$1{$callee = $callee$f$1};\n"
      "struct $closure$f$1 {\n"
      "  $callee: func (*$closure$f$1) -> void;\n"
      "}\n"
      "func $callee$f$1($env: *$closure$f$1): void {\n"
      "  var $defer$0: func () -> void = func (): void {\n"
      "    var t: i32 = 1i32;\n"
      "  };\n"
      "  var x: i32 = 2i32;\n"
      "  $defer$0();\n"
      "}\n",
      /* 合成名 '$' 前缀（lexer 不收首字符）→ round-trip 跳过。 */
      false);
}

/* RAII §4.17 P1：局部 disposable var 在块尾插入 $dispose$T(x.&)，
   合成体 func $dispose$T 追加到顶层尾部（钩子先执行）。 */
TEST(SemaTest, RaiiLocalDisposableVarInsertsDisposeAtExitAsNormalizedText) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func f(): void {\n"
      "  var x: Res = .Res{ .v = 1 };\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var f: $closure$f$2 = .$closure$f$2{$callee = $callee$f$2};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$f$2 {\n"
      "  $callee: func (*$closure$f$2) -> void;\n"
      "}\n"
      "func $callee$f$2($env: *$closure$f$2): void {\n"
      "  var x: Res = .Res{v = 1};\n"
      "  $dispose$Res(x.&);\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* RAII 与 defer 共用同一条 LIFO 栈：后登记的析构先执行。 */
TEST(SemaTest, RaiiDeferDisposeLifoInterleaveAsNormalizedText) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func f(): void {\n"
      "  defer { var t = 1; }\n"
      "  var x: Res = .Res{ .v = 1 };\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var f: $closure$f$2 = .$closure$f$2{$callee = $callee$f$2};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$f$2 {\n"
      "  $callee: func (*$closure$f$2) -> void;\n"
      "}\n"
      "func $callee$f$2($env: *$closure$f$2): void {\n"
      "  var $defer$0: func () -> void = func (): void {\n"
      "    var t: i32 = 1i32;\n"
      "  };\n"
      "  var x: Res = .Res{v = 1};\n"
      "  $dispose$Res(x.&);\n"
      "  $defer$0();\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* #dispose：指针实参直传、左值自动取址、非 disposable 静默退化为裸
   实参表达式（实参照常求值）。 */
TEST(SemaTest, RaiiDisposeBuiltinDesugarsAsNormalizedText) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func g(p: *Res): void {\n"
      "  #dispose(p);\n"
      "}\n"
      "func f(): void {\n"
      "  var x: Res = .Res{ .v = 1 };\n"
      "  var n = 2;\n"
      "  #dispose(x);\n"
      "  #dispose(n);\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var g: $closure$g$2 = .$closure$g$2{$callee = $callee$g$2};\n"
      "var f: $closure$f$3 = .$closure$f$3{$callee = $callee$f$3};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$g$2 {\n"
      "  $callee: func (*$closure$g$2, *Res) -> void;\n"
      "}\n"
      "struct $closure$f$3 {\n"
      "  $callee: func (*$closure$f$3) -> void;\n"
      "}\n"
      "func $callee$g$2($env: *$closure$g$2, p: *Res): void {\n"
      "  $dispose$Res(p);\n"
      "}\n"
      "func $callee$f$3($env: *$closure$f$3): void {\n"
      "  var x: Res = .Res{v = 1};\n"
      "  var n: i32 = 2i32;\n"
      "  $dispose$Res(x.&);\n"
      "  n;\n"
      "  $dispose$Res(x.&);\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17 禁令：全局/模块级 var 禁 disposable（无作用域出口，RAII 无
   插入点）；cunion 成员禁 disposable（无 vtable，析构无法分派）。 */
TEST(SemaTest, RaiiRejectsModuleLevelDisposableVarAndCunionField) {
  const char *src = "struct Res {\n"
                    "  v: i32;\n"
                    "  func __dispose__(self: *Res): void { return; }\n"
                    "}\n"
                    "var g: Res = .Res{ .v = 1 };\n"
                    "cunion U {\n"
                    "  r: Res;\n"
                    "  n: i32;\n"
                    "}\n";
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, src);
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_TRUE(sema_has_error(sema));
  EXPECT_EQ(clux_context_diag_count(ctx), 2u);
  /* 诊断一：模块级 var。诊断二：cunion 成员。 */
  const clux_diag_t *global_diag = clux_context_get_diag(ctx, 0);
  EXPECT_EQ(clux_diag_get_level(global_diag), CLUX_DIAG_ERROR);
  string_t *text = clux_diag_to_string((clux_diag_t *)global_diag, a);
  EXPECT_NE(strstr(string_get_data(text),
                   "module-level variable 'g' has a disposable type (RAII); "
                   "use a local variable or #dispose"),
            nullptr);
  delete_string(&text);
  const clux_diag_t *cunion_diag = clux_context_get_diag(ctx, 1);
  EXPECT_EQ(clux_diag_get_level(cunion_diag), CLUX_DIAG_ERROR);
  text = clux_diag_to_string((clux_diag_t *)cunion_diag, a);
  EXPECT_NE(strstr(string_get_data(text),
                   "cunion member 'r' has a disposable type (RAII); cunion is "
                   "never disposed"),
            nullptr);
  delete_string(&text);
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* §1.3 三分法：'$' 为编译器保留（$ 前缀合成符号 / $ 中缀连接符）。
   用户声明名含 '$' 报 ERROR 并引导改名（报错后照常登记防级联）；
   物化实例名（List$i32 等）为编译器产物，豁免由既有泛型测试覆盖。 */
TEST(SemaTest, DollarReservedRejectsUserDeclaredNames) {
  const char *src = "var foo$bar: i32 = 1;\n"
                    "func f$x(): void { return; }\n"
                    "func g(): void { var y$z: i32 = 2; }\n";
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, src);
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_TRUE(sema_has_error(sema));
  EXPECT_EQ(clux_context_diag_count(ctx), 3u);
  for (size_t idx = 0; idx < clux_context_diag_count(ctx); idx++) {
    const clux_diag_t *diag = clux_context_get_diag(ctx, idx);
    EXPECT_EQ(clux_diag_get_level(diag), CLUX_DIAG_ERROR);
    string_t *text = clux_diag_to_string((clux_diag_t *)diag, a);
    EXPECT_NE(strstr(string_get_data(text),
                     "contains '$', which is reserved for "
                     "compiler-generated symbols; rename without '$'"),
              nullptr);
    delete_string(&text);
  }
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* §4.17.2 P2：?T 载荷合成——none 无操作（is_none 门控），some 经 #take
   取载荷后转交 $dispose$B；调用名 = $dispose$ + mangle("?Res")=$x3fRes。 */
TEST(SemaTest, RaiiOptionalPayloadDisposeSynthAsNormalizedText) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func f(): void {\n"
      "  var x: ?Res = #some(Res, .Res{ .v = 1 });\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var f: $closure$f$2 = .$closure$f$2{$callee = $callee$f$2};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$f$2 {\n"
      "  $callee: func (*$closure$f$2) -> void;\n"
      "}\n"
      "func $callee$f$2($env: *$closure$f$2): void {\n"
      "  var $tmp$0: Res = .Res{v = 1};\n"
      "  var x: ?Res = #some(Res, $tmp$0);\n"
      "  $dispose$Res($tmp$0.&);\n"
      "  $dispose$$x3fRes(x.&);\n"
      "}\n"
      "func $dispose$$x3fRes(self: *?Res): void {\n"
      "  if (!#is_none(self.*)) {\n"
      "    $dispose$Res(#take(self.*).&);\n"
      "  }\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.2 P2：[N]T 元素合成——N 编译期常量展开为固定 for 逆序循环；
   调用名 = $dispose$ + mangle("[2]Res")=$x5b2$x5dRes。 */
TEST(SemaTest, RaiiArrayElementDisposeSynthAsNormalizedText) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func f(): void {\n"
      "  var xs: [2]Res = <.Res{ .v = 1 }, .Res{ .v = 2 }>;\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var f: $closure$f$2 = .$closure$f$2{$callee = $callee$f$2};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$f$2 {\n"
      "  $callee: func (*$closure$f$2) -> void;\n"
      "}\n"
      "func $callee$f$2($env: *$closure$f$2): void {\n"
      "  var $tmp$0: Res = .Res{v = 1};\n"
      "  var $tmp$1: Res = .Res{v = 2};\n"
      "  var xs: [2]Res = <$tmp$0, $tmp$1>;\n"
      "  $dispose$Res($tmp$1.&);\n"
      "  $dispose$Res($tmp$0.&);\n"
      "  $dispose$$x5b2$x5dRes(xs.&);\n"
      "}\n"
      "func $dispose$$x5b2$x5dRes(self: *[2]Res): void {\n"
      "  for (var i = 2; i > 0; i = i - 1) {\n"
      "    $dispose$Res(self.*[i - 1].&);\n"
      "  }\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.2 P2：tag union variant 合成——逐 disposable variant 一条 is
   判定链（运行期至多一臂命中）。源内 `u is n` 同时压测 check 层 is
   影子通道（union 影子产 bool 影子，sema 零错误）。 */
TEST(SemaTest, RaiiUnionVariantDisposeSynthAsNormalizedText) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "union U {\n"
      "  r: Res;\n"
      "  n: i32;\n"
      "}\n"
      "func f(): void {\n"
      "  var u: U = .U{ .r = .Res{ .v = 1 } };\n"
      "  if (u is n) {\n"
      "    return;\n"
      "  }\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "union U {\n"
      "  r: Res;\n"
      "  n: i32;\n"
      "}\n"
      "var f: $closure$f$2 = .$closure$f$2{$callee = $callee$f$2};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$f$2 {\n"
      "  $callee: func (*$closure$f$2) -> void;\n"
      "}\n"
      "func $callee$f$2($env: *$closure$f$2): void {\n"
      "  var $tmp$0: Res = .Res{v = 1};\n"
      "  var u: U = .U{r = $tmp$0};\n"
      "  $dispose$Res($tmp$0.&);\n"
      "  if (u is n) {\n"
      "    $dispose$U(u.&);\n"
      "    return;\n"
      "  }\n"
      "  $dispose$U(u.&);\n"
      "}\n"
      "func $dispose$U(self: *U): void {\n"
      "  if (self.* is r) {\n"
      "    $dispose$Res(self.r.&);\n"
      "  }\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.2 P2：元组成员合成——逆序逐 disposable 成员下标转交；
   调用名 = $dispose$ + mangle("<Res, i32>")（$x3c/$x2c/$x20/$x3e）。 */
TEST(SemaTest, RaiiTupleMemberDisposeSynthAsNormalizedText) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func f(): void {\n"
      "  var t = <.Res{ .v = 1 }, 2>;\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var f: $closure$f$2 = .$closure$f$2{$callee = $callee$f$2};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$f$2 {\n"
      "  $callee: func (*$closure$f$2) -> void;\n"
      "}\n"
      "func $callee$f$2($env: *$closure$f$2): void {\n"
      "  var $tmp$0: Res = .Res{v = 1};\n"
      "  var t: <Res, i32> = <$tmp$0, 2>;\n"
      "  $dispose$Res($tmp$0.&);\n"
      "  $dispose$$x3cRes$x2c$x20i32$x3e(t.&);\n"
      "}\n"
      "func $dispose$$x3cRes$x2c$x20i32$x3e(self: *<Res, i32>): void {\n"
      "  $dispose$Res(self.*[0].&);\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.5 P3：表达式槽临时提升——STMT_EXPR 的 CALL 结果 disposable
   → var $tmp$0 提升 + 语句槽后析构；var/return 槽 skip_top 直通由
   下列用例分别覆盖。隐藏 var 不经块级 P1 登记通道（emit_vars 合成、
   块循环不感知），exit LIFO 不误捞（本用例 f 尾只一条 $tmp 析构）。
   */
TEST(SemaTest, RaiiTempCallLiftedInExprStmt) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func g(): Res {\n"
      "  return .Res{ .v = 1 };\n"
      "}\n"
      "func use(r: Res): void {\n"
      "  return;\n"
      "}\n"
      "func f(): void {\n"
      "  use(g());\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var g: $closure$g$2 = .$closure$g$2{$callee = $callee$g$2};\n"
      "var use: $closure$use$3 = .$closure$use$3{$callee = $callee$use$3};\n"
      "var f: $closure$f$4 = .$closure$f$4{$callee = $callee$f$4};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$g$2 {\n"
      "  $callee: func (*$closure$g$2) -> Res;\n"
      "}\n"
      "struct $closure$use$3 {\n"
      "  $callee: func (*$closure$use$3, Res) -> void;\n"
      "}\n"
      "struct $closure$f$4 {\n"
      "  $callee: func (*$closure$f$4) -> void;\n"
      "}\n"
      "func $callee$g$2($env: *$closure$g$2): Res {\n"
      "  return .Res{v = 1};\n"
      "}\n"
      "func $callee$use$3($env: *$closure$use$3, r: Res): void {\n"
      "  return;\n"
      "}\n"
      "func $callee$f$4($env: *$closure$f$4): void {\n"
      "  var $tmp$0: Res = g.$callee(g.&);\n"
      "  use.$callee(use.&, $tmp$0);\n"
      "  $dispose$Res($tmp$0.&);\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.5 P3：var 槽 skip_top——VALUE 顶层单一临时根直通 bind（免
   __init_copy__ 二次拷贝），无 $tmp；P1 作用域出口析构照常。 */
TEST(SemaTest, RaiiTempVarSlotTopLevelPassthrough) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func g(): Res {\n"
      "  return .Res{ .v = 1 };\n"
      "}\n"
      "func f(): void {\n"
      "  var x: Res = g();\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var g: $closure$g$2 = .$closure$g$2{$callee = $callee$g$2};\n"
      "var f: $closure$f$3 = .$closure$f$3{$callee = $callee$f$3};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$g$2 {\n"
      "  $callee: func (*$closure$g$2) -> Res;\n"
      "}\n"
      "struct $closure$f$3 {\n"
      "  $callee: func (*$closure$f$3) -> void;\n"
      "}\n"
      "func $callee$g$2($env: *$closure$g$2): Res {\n"
      "  return .Res{v = 1};\n"
      "}\n"
      "func $callee$f$3($env: *$closure$f$3): void {\n"
      "  var x: Res = g.$callee(g.&);\n"
      "  $dispose$Res(x.&);\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.5 P3：return 槽 $ret 物化——return 表达式内嵌套根捕获后，
   先物化 var $ret$N（值根 CALL 锚点取 i32），[$tmp 析构]→[return $ret]，
   $ret 所有权流出不析构。$ret 的 VALUE（consume($tmp$0)）按值传参为
   copy，$tmp$0 未被置空，随后析构为真实释放而非幂等空操作——
   walk_return 的 __init_copy__ 路径与 $tmp 来源正交。 */
TEST(SemaTest, RaiiReturnRetMaterialization) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func g(): Res {\n"
      "  return .Res{ .v = 1 };\n"
      "}\n"
      "func consume(r: Res): i32 {\n"
      "  return 1;\n"
      "}\n"
      "func f(): i32 {\n"
      "  return consume(g());\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var g: $closure$g$2 = .$closure$g$2{$callee = $callee$g$2};\n"
      "var consume: $closure$consume$3 = .$closure$consume$3{$callee = "
      "$callee$consume$3};\n"
      "var f: $closure$f$4 = .$closure$f$4{$callee = $callee$f$4};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$g$2 {\n"
      "  $callee: func (*$closure$g$2) -> Res;\n"
      "}\n"
      "struct $closure$consume$3 {\n"
      "  $callee: func (*$closure$consume$3, Res) -> i32;\n"
      "}\n"
      "struct $closure$f$4 {\n"
      "  $callee: func (*$closure$f$4) -> i32;\n"
      "}\n"
      "func $callee$g$2($env: *$closure$g$2): Res {\n"
      "  return .Res{v = 1};\n"
      "}\n"
      "func $callee$consume$3($env: *$closure$consume$3, r: Res): i32 {\n"
      "  return 1;\n"
      "}\n"
      "func $callee$f$4($env: *$closure$f$4): i32 {\n"
      "  var $tmp$0: Res = g.$callee(g.&);\n"
      "  var $ret$1: i32 = consume.$callee(consume.&, $tmp$0);\n"
      "  $dispose$Res($tmp$0.&);\n"
      "  return $ret$1;\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.5 P3 遗留1：元组 return 根锚点——return 值为元组字面量（非
   CALL/CONSTRUCT 根）时 §8.5 锚点由值位置元组分派登记（聚合类型），
   $ret 物化照常发生：[$tmp 提升]→[var $ret$N: 类型]→[$tmp 析构]→
   [return $ret]。v1 保守泄漏路径（锚点缺失静默跳过物化+析构）由此
   收口。 */
TEST(SemaTest, RaiiReturnTupleRootAnchor) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void {\n"
      "    return;\n"
      "  }\n"
      "}\n"
      "func gs(): [2]Res {\n"
      "  return <.Res{ .v = 1 }, .Res{ .v = 2 }>;\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var gs: $closure$gs$2 = .$closure$gs$2{$callee = $callee$gs$2};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$gs$2 {\n"
      "  $callee: func (*$closure$gs$2) -> [2]Res;\n"
      "}\n"
      "func $callee$gs$2($env: *$closure$gs$2): [2]Res {\n"
      "  var $tmp$0: Res = .Res{v = 1};\n"
      "  var $tmp$1: Res = .Res{v = 2};\n"
      "  var $ret$2: <Res, Res> = <$tmp$0, $tmp$1>;\n"
      "  $dispose$Res($tmp$1.&);\n"
      "  $dispose$Res($tmp$0.&);\n"
      "  return $ret$2;\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.3 $ret 物化扩展：return 值纯 identifier 引用本 return 将展开
   的 unwind 区间内待析构绑定（return x;）——析构前插先于 return 表达式
   求值即 use-after-dispose。物化条件扩展（lifted || unwind 引用命中）：
   [var $ret$N: T = x;]→[unwind 析构]→[return $ret$N]，$ret 经
   __init_copy__ 拷贝（源 x 按值传参 copy 语义未被置空，析构为真实
   释放）。类型经 unwind 条目反查（根 identifier 无 §8.5 锚点）。 */
TEST(SemaTest, RaiiReturnRefDisposeBindingMaterialized) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func g(): Res {\n"
      "  return .Res{ .v = 1 };\n"
      "}\n"
      "func f(): Res {\n"
      "  var x: Res = g();\n"
      "  return x;\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var g: $closure$g$2 = .$closure$g$2{$callee = $callee$g$2};\n"
      "var f: $closure$f$3 = .$closure$f$3{$callee = $callee$f$3};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$g$2 {\n"
      "  $callee: func (*$closure$g$2) -> Res;\n"
      "}\n"
      "struct $closure$f$3 {\n"
      "  $callee: func (*$closure$f$3) -> Res;\n"
      "}\n"
      "func $callee$g$2($env: *$closure$g$2): Res {\n"
      "  return .Res{v = 1};\n"
      "}\n"
      "func $callee$f$3($env: *$closure$f$3): Res {\n"
      "  var x: Res = g.$callee(g.&);\n"
      "  var $ret$0: Res = x;\n"
      "  $dispose$Res(x.&);\n"
      "  return $ret$0;\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.3 深形态：if 臂内深 return 引用外层块绑定——臂块是块通道克隆，
   return 为其 exit 点，unwind 栈上外层块登记的 x 未截断（func_mark..top
   全链），物化 var 与析构前插落臂块内。路径互斥保证 x 单次析构。 */
TEST(SemaTest, RaiiDeepReturnRefOuterBindingMaterialized) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func g(): Res {\n"
      "  return .Res{ .v = 1 };\n"
      "}\n"
      "func f(c: bool): Res {\n"
      "  var x: Res = g();\n"
      "  if (c) {\n"
      "    return x;\n"
      "  }\n"
      "  return .Res{ .v = 2 };\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var g: $closure$g$2 = .$closure$g$2{$callee = $callee$g$2};\n"
      "var f: $closure$f$3 = .$closure$f$3{$callee = $callee$f$3};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$g$2 {\n"
      "  $callee: func (*$closure$g$2) -> Res;\n"
      "}\n"
      "struct $closure$f$3 {\n"
      "  $callee: func (*$closure$f$3, bool) -> Res;\n"
      "}\n"
      "func $callee$g$2($env: *$closure$g$2): Res {\n"
      "  return .Res{v = 1};\n"
      "}\n"
      "func $callee$f$3($env: *$closure$f$3, c: bool): Res {\n"
      "  var x: Res = g.$callee(g.&);\n"
      "  if (c) {\n"
      "    var $ret$0: Res = x;\n"
      "    $dispose$Res(x.&);\n"
      "    return $ret$0;\n"
      "  }\n"
      "  $dispose$Res(x.&);\n"
      "  return .Res{v = 2};\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.5 P3：惰性上下文抑制——|| 子表达式条件求值，提升即提前求值
   破坏短路，v1 整棵不捕获（泄漏保守固化）。 */
TEST(SemaTest, RaiiLazyContextNoCapture) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func g(): Res {\n"
      "  return .Res{ .v = 1 };\n"
      "}\n"
      "func check(r: Res): bool {\n"
      "  return true;\n"
      "}\n"
      "func f(): void {\n"
      "  var b: bool = check(g()) || true;\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var g: $closure$g$2 = .$closure$g$2{$callee = $callee$g$2};\n"
      "var check: $closure$check$3 = .$closure$check$3{$callee = "
      "$callee$check$3};\n"
      "var f: $closure$f$4 = .$closure$f$4{$callee = $callee$f$4};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$g$2 {\n"
      "  $callee: func (*$closure$g$2) -> Res;\n"
      "}\n"
      "struct $closure$check$3 {\n"
      "  $callee: func (*$closure$check$3, Res) -> bool;\n"
      "}\n"
      "struct $closure$f$4 {\n"
      "  $callee: func (*$closure$f$4) -> void;\n"
      "}\n"
      "func $callee$g$2($env: *$closure$g$2): Res {\n"
      "  return .Res{v = 1};\n"
      "}\n"
      "func $callee$check$3($env: *$closure$check$3, r: Res): bool {\n"
      "  return true;\n"
      "}\n"
      "func $callee$f$4($env: *$closure$f$4): void {\n"
      "  var b: bool = check.$callee(check.&, g.$callee(g.&)) || true;\n"
      "}\n",
      false);
}

/* §4.17.5 P3：循环降维（零 flag）——while cond 子树含临时根 →
   for(;;) { gate; body }，gate 内每轮 [vars]→if 假臂[析构]break→
   真臂尾析构，cond 求值次数与原语义一致。 */
TEST(SemaTest, RaiiLoopLowerWhileCond) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func g(): Res {\n"
      "  return .Res{ .v = 1 };\n"
      "}\n"
      "func check(r: Res): bool {\n"
      "  return true;\n"
      "}\n"
      "func f(): void {\n"
      "  while (check(g())) {\n"
      "    break;\n"
      "  }\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var g: $closure$g$2 = .$closure$g$2{$callee = $callee$g$2};\n"
      "var check: $closure$check$3 = .$closure$check$3{$callee = "
      "$callee$check$3};\n"
      "var f: $closure$f$4 = .$closure$f$4{$callee = $callee$f$4};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$g$2 {\n"
      "  $callee: func (*$closure$g$2) -> Res;\n"
      "}\n"
      "struct $closure$check$3 {\n"
      "  $callee: func (*$closure$check$3, Res) -> bool;\n"
      "}\n"
      "struct $closure$f$4 {\n"
      "  $callee: func (*$closure$f$4) -> void;\n"
      "}\n"
      "func $callee$g$2($env: *$closure$g$2): Res {\n"
      "  return .Res{v = 1};\n"
      "}\n"
      "func $callee$check$3($env: *$closure$check$3, r: Res): bool {\n"
      "  return true;\n"
      "}\n"
      "func $callee$f$4($env: *$closure$f$4): void {\n"
      "  for (; ; ) {\n"
      "    {\n"
      "      var $tmp$0: Res = g.$callee(g.&);\n"
      "      if (check.$callee(check.&, $tmp$0)) {\n"
      "      } else {\n"
      "        $dispose$Res($tmp$0.&);\n"
      "        break;\n"
      "      }\n"
      "      $dispose$Res($tmp$0.&);\n"
      "    }\n"
      "    {\n"
      "      break;\n"
      "    }\n"
      "  }\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.5 P3 遗留3：do-while 降维的 continue 偏差收口——原语义
   continue 会再求值一次 cond，体内直接层 continue 前同构前插独立
   gate（独立 $tmp 编号与块作用域）；break 无需前插；尾 gate 照常。 */
TEST(SemaTest, RaiiLoopLowerDoWhileContinue) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func g(): Res {\n"
      "  return .Res{ .v = 1 };\n"
      "}\n"
      "func check(r: Res): bool {\n"
      "  return true;\n"
      "}\n"
      "func f(): void {\n"
      "  do {\n"
      "    continue;\n"
      "  } while (check(g()));\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var g: $closure$g$2 = .$closure$g$2{$callee = $callee$g$2};\n"
      "var check: $closure$check$3 = .$closure$check$3{$callee = "
      "$callee$check$3};\n"
      "var f: $closure$f$4 = .$closure$f$4{$callee = $callee$f$4};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$g$2 {\n"
      "  $callee: func (*$closure$g$2) -> Res;\n"
      "}\n"
      "struct $closure$check$3 {\n"
      "  $callee: func (*$closure$check$3, Res) -> bool;\n"
      "}\n"
      "struct $closure$f$4 {\n"
      "  $callee: func (*$closure$f$4) -> void;\n"
      "}\n"
      "func $callee$g$2($env: *$closure$g$2): Res {\n"
      "  return .Res{v = 1};\n"
      "}\n"
      "func $callee$check$3($env: *$closure$check$3, r: Res): bool {\n"
      "  return true;\n"
      "}\n"
      "func $callee$f$4($env: *$closure$f$4): void {\n"
      "  for (; ; ) {\n"
      "    {\n"
      "      {\n"
      "        var $tmp$1: Res = g.$callee(g.&);\n"
      "        if (check.$callee(check.&, $tmp$1)) {\n"
      "        } else {\n"
      "          $dispose$Res($tmp$1.&);\n"
      "          break;\n"
      "        }\n"
      "        $dispose$Res($tmp$1.&);\n"
      "      }\n"
      "      continue;\n"
      "    }\n"
      "    {\n"
      "      var $tmp$0: Res = g.$callee(g.&);\n"
      "      if (check.$callee(check.&, $tmp$0)) {\n"
      "      } else {\n"
      "        $dispose$Res($tmp$0.&);\n"
      "        break;\n"
      "      }\n"
      "      $dispose$Res($tmp$0.&);\n"
      "    }\n"
      "  }\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.5 P3：foreach iterable 所有权——iterable 顶层直通绑入 $iter
   （copy 语义求值一次），iterable 类型 disposable 时循环出口追加
   $iter 析构；元素 var 逐轮析构（P3 遗留2 收口：体尾追加 + 体内直接层
   continue/break 前插）。it 的初始化元组内嵌 CONSTRUCT 根照常捕获提升
   （var 槽 skip_top 只放行顶层），it 本体由 P1 作用域出口析构。 */
TEST(SemaTest, RaiiForeachIterDispose) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func f(): void {\n"
      "  var it: [2]Res = <.Res{ .v = 1 }, .Res{ .v = 2 }>;\n"
      "  foreach (var x of it) {\n"
      "  }\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var f: $closure$f$2 = .$closure$f$2{$callee = $callee$f$2};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$f$2 {\n"
      "  $callee: func (*$closure$f$2) -> void;\n"
      "}\n"
      "func $callee$f$2($env: *$closure$f$2): void {\n"
      "  var $tmp$0: Res = .Res{v = 1};\n"
      "  var $tmp$1: Res = .Res{v = 2};\n"
      "  var it: [2]Res = <$tmp$0, $tmp$1>;\n"
      "  $dispose$Res($tmp$1.&);\n"
      "  $dispose$Res($tmp$0.&);\n"
      "  {\n"
      "    var $iter: [2]Res = it;\n"
      "    var $i: i32 = 0i32;\n"
      "    for (; $i < #len($iter); $i = $i + 1) {\n"
      "      var x: Res = $iter[$i];\n"
      "      {\n"
      "      }\n"
      "      $dispose$Res(x.&);\n"
      "    }\n"
      "    $dispose$$x5b2$x5dRes($iter.&);\n"
      "  }\n"
      "  $dispose$$x5b2$x5dRes(it.&);\n"
      "}\n"
      "func $dispose$$x5b2$x5dRes(self: *[2]Res): void {\n"
      "  for (var i = 2; i > 0; i = i - 1) {\n"
      "    $dispose$Res(self.*[i - 1].&);\n"
      "  }\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.5 P3 遗留2：foreach 体内直接层 continue/break 前插元素析构
   ——signal 在循环边界被 exec 消费、跳过体尾语句，命中点由块/if 臂
   扫描收集（不穿嵌套循环体）。 */
TEST(SemaTest, RaiiForeachItemDisposeAtExits) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func f(): void {\n"
      "  var it: [3]Res = <.Res{ .v = 1 }, .Res{ .v = 2 }, .Res{ .v = 3 }>;\n"
      "  foreach (var x of it) {\n"
      "    if (x.v == 2) {\n"
      "      continue;\n"
      "    }\n"
      "    if (x.v == 3) {\n"
      "      break;\n"
      "    }\n"
      "  }\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var f: $closure$f$2 = .$closure$f$2{$callee = $callee$f$2};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$f$2 {\n"
      "  $callee: func (*$closure$f$2) -> void;\n"
      "}\n"
      "func $callee$f$2($env: *$closure$f$2): void {\n"
      "  var $tmp$0: Res = .Res{v = 1};\n"
      "  var $tmp$1: Res = .Res{v = 2};\n"
      "  var $tmp$2: Res = .Res{v = 3};\n"
      "  var it: [3]Res = <$tmp$0, $tmp$1, $tmp$2>;\n"
      "  $dispose$Res($tmp$2.&);\n"
      "  $dispose$Res($tmp$1.&);\n"
      "  $dispose$Res($tmp$0.&);\n"
      "  {\n"
      "    var $iter: [3]Res = it;\n"
      "    var $i: i32 = 0i32;\n"
      "    for (; $i < #len($iter); $i = $i + 1) {\n"
      "      var x: Res = $iter[$i];\n"
      "      {\n"
      "        if (x.v == 2) {\n"
      "          $dispose$Res(x.&);\n"
      "          continue;\n"
      "        }\n"
      "        if (x.v == 3) {\n"
      "          $dispose$Res(x.&);\n"
      "          break;\n"
      "        }\n"
      "      }\n"
      "      $dispose$Res(x.&);\n"
      "    }\n"
      "    $dispose$$x5b3$x5dRes($iter.&);\n"
      "  }\n"
      "  $dispose$$x5b3$x5dRes(it.&);\n"
      "}\n"
      "func $dispose$$x5b3$x5dRes(self: *[3]Res): void {\n"
      "  for (var i = 3; i > 0; i = i - 1) {\n"
      "    $dispose$Res(self.*[i - 1].&);\n"
      "  }\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.3：深 return 穿 foreach 体——臂内 return 沿 unwind 全链 LIFO
   展开：元素 x → $iter → 函数体块登记的 it（v1 只展开臂内直接链，
   $iter/it 会漏）。 */
TEST(SemaTest, RaiiForeachDeepReturnUnwindsFullChain) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func f(): void {\n"
      "  var it: [2]Res = <.Res{ .v = 1 }, .Res{ .v = 2 }>;\n"
      "  foreach (var x of it) {\n"
      "    if (x.v == 1) {\n"
      "      return;\n"
      "    }\n"
      "  }\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var f: $closure$f$2 = .$closure$f$2{$callee = $callee$f$2};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$f$2 {\n"
      "  $callee: func (*$closure$f$2) -> void;\n"
      "}\n"
      "func $callee$f$2($env: *$closure$f$2): void {\n"
      "  var $tmp$0: Res = .Res{v = 1};\n"
      "  var $tmp$1: Res = .Res{v = 2};\n"
      "  var it: [2]Res = <$tmp$0, $tmp$1>;\n"
      "  $dispose$Res($tmp$1.&);\n"
      "  $dispose$Res($tmp$0.&);\n"
      "  {\n"
      "    var $iter: [2]Res = it;\n"
      "    var $i: i32 = 0i32;\n"
      "    for (; $i < #len($iter); $i = $i + 1) {\n"
      "      var x: Res = $iter[$i];\n"
      "      {\n"
      "        if (x.v == 1) {\n"
      "          $dispose$Res(x.&);\n"
      "          $dispose$$x5b2$x5dRes($iter.&);\n"
      "          $dispose$$x5b2$x5dRes(it.&);\n"
      "          return;\n"
      "        }\n"
      "      }\n"
      "      $dispose$Res(x.&);\n"
      "    }\n"
      "    $dispose$$x5b2$x5dRes($iter.&);\n"
      "  }\n"
      "  $dispose$$x5b2$x5dRes(it.&);\n"
      "}\n"
      "func $dispose$$x5b2$x5dRes(self: *[2]Res): void {\n"
      "  for (var i = 2; i > 0; i = i - 1) {\n"
      "    $dispose$Res(self.*[i - 1].&);\n"
      "  }\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.3：循环体内 disposable var 的 break 前析构——break 的 unwind
   底是循环体水位：体内登记项 g 与元素 x 展开，$iter 不触发（出循环
   后落到块尾显式析构）。v1 该场景 g 直接漏析构。 */
TEST(SemaTest, RaiiLoopBodyVarDisposeAtBreak) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func f(): void {\n"
      "  var it: [2]Res = <.Res{ .v = 1 }, .Res{ .v = 2 }>;\n"
      "  foreach (var x of it) {\n"
      "    var g: Res = .Res{ .v = 0 };\n"
      "    break;\n"
      "  }\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var f: $closure$f$2 = .$closure$f$2{$callee = $callee$f$2};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$f$2 {\n"
      "  $callee: func (*$closure$f$2) -> void;\n"
      "}\n"
      "func $callee$f$2($env: *$closure$f$2): void {\n"
      "  var $tmp$0: Res = .Res{v = 1};\n"
      "  var $tmp$1: Res = .Res{v = 2};\n"
      "  var it: [2]Res = <$tmp$0, $tmp$1>;\n"
      "  $dispose$Res($tmp$1.&);\n"
      "  $dispose$Res($tmp$0.&);\n"
      "  {\n"
      "    var $iter: [2]Res = it;\n"
      "    var $i: i32 = 0i32;\n"
      "    for (; $i < #len($iter); $i = $i + 1) {\n"
      "      var x: Res = $iter[$i];\n"
      "      {\n"
      "        var g: Res = .Res{v = 0};\n"
      "        $dispose$Res(g.&);\n"
      "        $dispose$Res(x.&);\n"
      "        break;\n"
      "      }\n"
      "      $dispose$Res(x.&);\n"
      "    }\n"
      "    $dispose$$x5b2$x5dRes($iter.&);\n"
      "  }\n"
      "  $dispose$$x5b2$x5dRes(it.&);\n"
      "}\n"
      "func $dispose$$x5b2$x5dRes(self: *[2]Res): void {\n"
      "  for (var i = 2; i > 0; i = i - 1) {\n"
      "    $dispose$Res(self.*[i - 1].&);\n"
      "  }\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

/* §4.17.3：函数体边界水位——函数声明体内 return 的 unwind 底是其自身
   体边界，不外泄到外层块登记项（a 不被内层 return 前插）。 */
TEST(SemaTest, RaiiNestedFuncReturnDoesNotUnwindOuter) {
  expect_sema_text(
      "struct Res {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *Res): void { return; }\n"
      "}\n"
      "func f(): void {\n"
      "  func gg(): void {\n"
      "    return;\n"
      "  }\n"
      "  var a: Res = .Res{ .v = 1 };\n"
      "}\n",
      "struct Res {\n"
      "  v: i32;\n"
      "}\n"
      "var f: $closure$f$3 = .$closure$f$3{$callee = $callee$f$3};\n"
      "func Res$__dispose__$1(self: *Res): void {\n"
      "  return;\n"
      "}\n"
      "struct $closure$f$3 {\n"
      "  $callee: func (*$closure$f$3) -> void;\n"
      "}\n"
      "struct $closure$f$gg$2 {\n"
      "  $callee: func (*$closure$f$gg$2) -> void;\n"
      "}\n"
      "func $callee$f$3($env: *$closure$f$3): void {\n"
      "  var gg: $closure$f$gg$2 = .$closure$f$gg$2{$callee = "
      "$callee$f$gg$2};\n"
      "  var a: Res = .Res{v = 1};\n"
      "  $dispose$Res(a.&);\n"
      "}\n"
      "func $callee$f$gg$2($env: *$closure$f$gg$2): void {\n"
      "  return;\n"
      "}\n"
      "func $dispose$Res(self: *Res): void {\n"
      "  Res$__dispose__$1(self);\n"
      "}\n",
      false);
}

TEST(SemaTest, RaiiFfiBoundaryRejectsDisposableTypes) {
  const char *src = "struct Res {\n"
                    "  v: i32;\n"
                    "  func __dispose__(self: *Res): void { return; }\n"
                    "}\n"
                    "extern cfunc m::register_c(g: Res): void;\n"
                    "extern cfunc m::make_r(): Res;\n"
                    "extern cfunc m::ok(p: *Res): void;\n"
                    "extern var m::g: Res;\n";
  allocator_t *a = create_allocator(malloc, free);
  clux_context_t *ctx = create_clux_context(a);
  clux_sema_t *sema = sema_create(ctx, a);
  parser_t *p;
  node_t *ast = parse_clux(a, &p, src);
  ASSERT_NE(ast, nullptr);
  EXPECT_EQ(sema_walk(sema, ast), 0);
  EXPECT_TRUE(sema_has_error(sema));
  EXPECT_EQ(clux_context_diag_count(ctx), 3u);
  for (size_t idx = 0; idx < 3u; idx++) {
    const clux_diag_t *diag = clux_context_get_diag(ctx, idx);
    EXPECT_EQ(clux_diag_get_level(diag), CLUX_DIAG_ERROR);
    string_t *text = clux_diag_to_string((clux_diag_t *)diag, a);
    EXPECT_NE(strstr(string_get_data(text),
                     "FFI boundary rejects type 'Res': C ABI cannot run clux "
                     "dispose protocol"),
              nullptr);
    delete_string(&text);
  }
  sema_destroy(&sema);
  delete_node(&ast);
  delete_parser(&p);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

TEST(SemaTest, ForeachDesugarsToIndexedForAsNormalizedText) {
  expect_sema_text(
      "var items: [3]i32 = <0, 0, 0>;\n"
      "func f(): void {\n"
      "  foreach (var x of items) { var y = x; }\n"
      "}\n",
      "var items: [3]i32 = <0, 0, 0>;\n"
      "var f: $closure$f$1 = .$closure$f$1{$callee = $callee$f$1};\n"
      "struct $closure$f$1 {\n"
      "  $callee: func (*$closure$f$1) -> void;\n"
      "}\n"
      "func $callee$f$1($env: *$closure$f$1): void {\n"
      "  {\n"
      "    var $iter: [3]i32 = items;\n"
      "    var $i: i32 = 0i32;\n"
      "    for (; $i < #len($iter); $i = $i + 1) {\n"
      "      var x: i32 = $iter[$i];\n"
      "      {\n"
      "        var y: i32 = x;\n"
      "      }\n"
      "    }\n"
      "  }\n"
      "}\n",
      /* 合成名 '$' 前缀（lexer 不收首字符）→ round-trip 跳过。 */
      false);
}

/* foreach 裸名形态（§4.9/§2.7）：前提 x 已在外层定义，脱糖为每轮
   赋值既有绑定（x = $iter[$i]），而非重新定义。 */
TEST(SemaTest, ForeachBareNameAssignsToExistingBindingAsNormalizedText) {
  expect_sema_text(
      "var items: [3]i32 = <0, 0, 0>;\n"
      "func f(): void {\n"
      "  var x: i32 = 0i32;\n"
      "  foreach (x of items) { var y = x; }\n"
      "}\n",
      "var items: [3]i32 = <0, 0, 0>;\n"
      "var f: $closure$f$1 = .$closure$f$1{$callee = $callee$f$1};\n"
      "struct $closure$f$1 {\n"
      "  $callee: func (*$closure$f$1) -> void;\n"
      "}\n"
      "func $callee$f$1($env: *$closure$f$1): void {\n"
      "  var x: i32 = 0i32;\n"
      "  {\n"
      "    var $iter: [3]i32 = items;\n"
      "    var $i: i32 = 0i32;\n"
      "    for (; $i < #len($iter); $i = $i + 1) {\n"
      "      x = $iter[$i];\n"
      "      {\n"
      "        var y: i32 = x;\n"
      "      }\n"
      "    }\n"
      "  }\n"
      "}\n",
      /* 合成名 '$' 前缀（lexer 不收首字符）→ round-trip 跳过。 */
      false);
}

/* call 形态 iterable 单次求值：脱糖后 #len 与下标的 base 都是 $iter，
   不再出现两份 call 求值（2N 次调用回归）。 */
TEST(SemaTest, ForeachCallIterableEvaluatesOnceAsNormalizedText) {
  expect_sema_text(
      "func make(): [2]i32 { return <1, 2>; }\n"
      "func f(): void {\n"
      "  foreach (var x of make()) { }\n"
      "}\n",
      "var make: $closure$make$1 = .$closure$make$1{$callee = "
      "$callee$make$1};\n"
      "var f: $closure$f$2 = .$closure$f$2{$callee = $callee$f$2};\n"
      "struct $closure$make$1 {\n"
      "  $callee: func (*$closure$make$1) -> [2]i32;\n"
      "}\n"
      "struct $closure$f$2 {\n"
      "  $callee: func (*$closure$f$2) -> void;\n"
      "}\n"
      "func $callee$make$1($env: *$closure$make$1): [2]i32 {\n"
      "  return <1, 2>;\n"
      "}\n"
      "func $callee$f$2($env: *$closure$f$2): void {\n"
      "  {\n"
      "    var $iter: [2]i32 = make.$callee(make.&);\n"
      "    var $i: i32 = 0i32;\n"
      "    for (; $i < #len($iter); $i = $i + 1) {\n"
      "      var x: i32 = $iter[$i];\n"
      "      {\n"
      "      }\n"
      "    }\n"
      "  }\n"
      "}\n",
      /* 合成名 '$' 前缀（lexer 不收首字符）→ round-trip 跳过。 */
      false);
}

TEST(SemaTest, ComptimeForeachExpandsTupleWithSubstitutionAsNormalizedText) {
  expect_sema_text(
      "func f(): void {\n"
      "  comptime foreach (var x of <1, 2>) { var y = x; }\n"
      "}\n",
      "var f: $closure$f$1 = .$closure$f$1{$callee = $callee$f$1};\n"
      "struct $closure$f$1 {\n"
      "  $callee: func (*$closure$f$1) -> void;\n"
      "}\n"
      "func $callee$f$1($env: *$closure$f$1): void {\n"
      "  {\n"
      "    var y: i32 = 1i32;\n"
      "  }\n"
      "  {\n"
      "    var y: i32 = 2i32;\n"
      "  }\n"
      "}\n",
      /* f 退化引入 '$' → 跳过 round-trip。 */
      false);
}

TEST(SemaTest, GenericDefinitionReplacedByInstanceAsNormalizedText) {
  expect_sema_text("type List[T] = [4]T;\nvar a: List[i32] = <1, 2, 3, 4>;\n"
                   "var b: List[i32] = <5, 6, 7, 8>;\n",
                   "var a: List$i32 = <1, 2, 3, 4>;\n"
                   "var b: List$i32 = <5, 6, 7, 8>;\n"
                   "type List$i32 = [4]i32;\n",
                   false);
}

TEST(SemaTest, AnonymousConstructIsNamedAsNormalizedText) {
  expect_sema_text("var x = .{ .a = 1, .b = 2 };\n",
                   "var x: nonamed$1 = .nonamed$1{a = 1, b = 2};\n"
                   "struct nonamed$1 {\n"
                   "  a: i32;\n"
                   "  b: i32;\n"
                   "}\n",
                   false);
}

TEST(SemaTest, StructMembersAreHoistedToGlobalsAsNormalizedText) {
  expect_sema_text("struct AAA {\n"
                   "  x: i32;\n"
                   "  var count = 0;\n"
                   "  func get(self: *AAA): i32 { return self.x; }\n"
                   "  func helper(): i32 { return 1; }\n"
                   "}\n"
                   "var a = .AAA{ .x = 1 };\n",
                   "struct AAA {\n"
                   "  x: i32;\n"
                   "}\n"
                   "var a: AAA = .AAA{x = 1};\n"
                   "var AAA$count$1: i32 = 0i32;\n"
                   "func AAA$get$2(self: *AAA): i32 {\n"
                   "  return self.x;\n"
                   "}\n"
                   "func AAA$helper$3(): i32 {\n"
                   "  return 1;\n"
                   "}\n",
                   false);
}

TEST(SemaTest, MemberCallOnNamedConstructDesugarsAsNormalizedText) {
  /* 全局初始化器必须是编译期常量（C 规范），调用移进函数体。 */
  expect_sema_text(
      "struct Box { v: i32; func get(self: *Box): i32 { return self.v; } }\n"
      "func main(): void { var r = .Box{ .v = 7 }.get(); }\n",
      "struct Box {\n"
      "  v: i32;\n"
      "}\n"
      "var main: $closure$main$2 = .$closure$main$2{$callee = "
      "$callee$main$2};\n"
      "func Box$get$1(self: *Box): i32 {\n"
      "  return self.v;\n"
      "}\n"
      "struct $closure$main$2 {\n"
      "  $callee: func (*$closure$main$2) -> void;\n"
      "}\n"
      "func $callee$main$2($env: *$closure$main$2): void {\n"
      "  var r: i32 = Box$get$1(.Box{v = 7}.&);\n"
      "}\n",
      false);
}

/* §4.18 闭包退化 P1：块级带捕获闭包完整退化四件套——$closure 结构
   （callee 打头 + 捕获字段）+ $callee 纯函数（env 首参）+ 定义点 var
   TDZ 占位、块尾构造赋值 + 调用点 add.$callee(add.&, ...) 改写。 */
TEST(SemaTest, ClosureDegradeBlockCaptureFullKitAsNormalizedText) {
  expect_sema_text(
      "func main(): void {\n"
      "  func |c| add(a: i32, b: i32): i32 { return a + b + c; }\n"
      "  var c = 1;\n"
      "  var d = add(1, 2);\n"
      "}\n",
      "var main: $closure$main$2 = .$closure$main$2{$callee = "
      "$callee$main$2};\n"
      "struct $closure$main$2 {\n"
      "  $callee: func (*$closure$main$2) -> void;\n"
      "}\n"
      "struct $closure$main$add$1 {\n"
      "  $callee: func (*$closure$main$add$1, i32, i32) -> i32;\n"
      "  c: i32;\n"
      "}\n"
      "func $callee$main$2($env: *$closure$main$2): void {\n"
      "  var add: $closure$main$add$1 = undefined;\n"
      "  var c: i32 = 1i32;\n"
      "  var d: i32 = add.$callee(add.&, 1, 2);\n"
      "  add = .$closure$main$add$1{$callee = $callee$main$add$1, c = c};\n"
      "}\n"
      "func $callee$main$add$1($env: *$closure$main$add$1, a: i32, b: i32): "
      "i32 {\n"
      "  return a + b + $env.c;\n"
      "}\n",
      /* 退化产物含 '$' → 跳过 round-trip。 */ false);
}

/* §2.12：捕获名 callee 放行——协议键迁移 $callee（§1.3 三分法 '$'
   前缀隔离）后，用户捕获字段 callee 与合成字段 $callee 并存；体内
   引用改写 $env.callee 与自递归协议 $env.$callee 互不干扰。 */
TEST(SemaTest, ClosureCaptureNamedCalleeCoexistsWithProtocolKey) {
  expect_sema_text(
      "func main(): void {\n"
      "  func |callee| add(a: i32): i32 { return a + callee; }\n"
      "  var callee = 5;\n"
      "  var r = add(1);\n"
      "}\n",
      "var main: $closure$main$2 = .$closure$main$2{$callee = "
      "$callee$main$2};\n"
      "struct $closure$main$2 {\n"
      "  $callee: func (*$closure$main$2) -> void;\n"
      "}\n"
      "struct $closure$main$add$1 {\n"
      "  $callee: func (*$closure$main$add$1, i32) -> i32;\n"
      "  callee: i32;\n"
      "}\n"
      "func $callee$main$2($env: *$closure$main$2): void {\n"
      "  var add: $closure$main$add$1 = undefined;\n"
      "  var callee: i32 = 5i32;\n"
      "  var r: i32 = add.$callee(add.&, 1);\n"
      "  add = .$closure$main$add$1{$callee = $callee$main$add$1, callee = "
      "callee};\n"
      "}\n"
      "func $callee$main$add$1($env: *$closure$main$add$1, a: i32): i32 {\n"
      "  return a + $env.callee;\n"
      "}\n",
      /* 退化产物含 '$' → 跳过 round-trip。 */ false);
}

/* §2.12：捕获名 env 放行——合成首参迁移 $env（§1.3 '$' 前缀隔离）后，
   用户捕获字段 env 与合成首参 $env 并存；体内引用改写 $env.env，
   用户值经逐层解析正确取出。 */
TEST(SemaTest, ClosureCaptureNamedEnvCoexistsWithProtocolKey) {
  expect_sema_text(
      "func main(): void {\n"
      "  func |env| add(a: i32): i32 { return a + env; }\n"
      "  var env = 5;\n"
      "  var r = add(1);\n"
      "}\n",
      "var main: $closure$main$2 = .$closure$main$2{$callee = "
      "$callee$main$2};\n"
      "struct $closure$main$2 {\n"
      "  $callee: func (*$closure$main$2) -> void;\n"
      "}\n"
      "struct $closure$main$add$1 {\n"
      "  $callee: func (*$closure$main$add$1, i32) -> i32;\n"
      "  env: i32;\n"
      "}\n"
      "func $callee$main$2($env: *$closure$main$2): void {\n"
      "  var add: $closure$main$add$1 = undefined;\n"
      "  var env: i32 = 5i32;\n"
      "  var r: i32 = add.$callee(add.&, 1);\n"
      "  add = .$closure$main$add$1{$callee = $callee$main$add$1, env = env};\n"
      "}\n"
      "func $callee$main$add$1($env: *$closure$main$add$1, a: i32): i32 {\n"
      "  return a + $env.env;\n"
      "}\n",
      /* 退化产物含 '$' → 跳过 round-trip。 */ false);
}

/* §2.12：条目形参名 env 放行——合成首参 $env 与用户形参 env 并存，
   参数序不混淆（$env 打头、用户形参原序随其后）。 */
TEST(SemaTest, ClosureParamNamedEnvCoexistsWithProtocolKey) {
  expect_sema_text(
      "func add(env: i32, b: i32): i32 { return env + b; }\n"
      "func main(): void {\n"
      "  var r = add(1, 2);\n"
      "}\n",
      "var add: $closure$add$1 = .$closure$add$1{$callee = $callee$add$1};\n"
      "var main: $closure$main$2 = .$closure$main$2{$callee = "
      "$callee$main$2};\n"
      "struct $closure$add$1 {\n"
      "  $callee: func (*$closure$add$1, i32, i32) -> i32;\n"
      "}\n"
      "struct $closure$main$2 {\n"
      "  $callee: func (*$closure$main$2) -> void;\n"
      "}\n"
      "func $callee$add$1($env: *$closure$add$1, env: i32, b: i32): i32 {\n"
      "  return env + b;\n"
      "}\n"
      "func $callee$main$2($env: *$closure$main$2): void {\n"
      "  var r: i32 = add.$callee(add.&, 1, 2);\n"
      "}\n",
      /* 退化产物含 '$' → 跳过 round-trip。 */ false);
}
