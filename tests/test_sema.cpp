#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/program.h"
#include "engine/context.h"
#include "engine/diag.h"
#include "parser/node.h"
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
