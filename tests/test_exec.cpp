/* 执行器端到端冒烟测试：源码字符串 → parser → exec walk → 断言值。
   值由 context 托管，断言在 context 销毁前完成；error 值用消息子串断言。 */
#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/error.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/exec.h"
#include "parser/node.h"
#include "parser/parser.h"
#include "parser/program.h"
#include <stdlib.h>
#include <string.h>

/* 执行一段源码，返回 run 的结果（借用，归 ctx 所有）。
   program/exec/parser 在返回前释放，值本体由 out_ctx 托管。 */
static clux_value_t *
exec_source(allocator_t *a, clux_context_t **out_ctx, const char *src) {
  parser_t *parser = create_parser(a, "test.clux", src);
  node_t *program = parser_read_program(a, parser);
  *out_ctx = create_clux_context(a);
  clux_exec_t *exec = create_clux_exec(a, *out_ctx, src);
  clux_value_t *result = clux_exec_run(exec, program);
  delete_node(&program);
  delete_clux_exec(&exec);
  delete_parser(&parser);
  return result;
}

static int32_t read_i32(clux_value_t *value) {
  return *(int32_t *)ptr_to_raw(clux_value_get_data(value), 0);
}

static double read_f64(clux_value_t *value) {
  return *(double *)ptr_to_raw(clux_value_get_data(value), 0);
}

static bool read_bool(clux_value_t *value) {
  return *(bool *)ptr_to_raw(clux_value_get_data(value), 0);
}

static const char *read_str(clux_value_t *value) {
  return *(char **)ptr_to_raw(clux_value_get_data(value), 0);
}

static bool value_is_error(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static const char *error_message_of(clux_value_t *value) {
  clux_error_t **stored =
      (clux_error_t **)ptr_to_raw(clux_value_get_data(value), 0);
  return stored ? error_get_message(*stored) : nullptr;
}

/* 成功用例骨架：执行 + 断言非 error + 取值。 */
#define RUN_SCRIPT(src)                                                        \
  allocator_t *a = create_allocator(malloc, free);                             \
  clux_context_t *ctx;                                                         \
  clux_value_t *result = exec_source(a, &ctx, src);

/* 错误用例骨架：执行 + 断言 error + 消息包含子串。 */
#define EXPECT_EXEC_ERROR(src, fragment)                                       \
  do {                                                                         \
    allocator_t *a = create_allocator(malloc, free);                           \
    clux_context_t *ctx;                                                       \
    clux_value_t *result = exec_source(a, &ctx, src);                          \
    ASSERT_NE(result, nullptr);                                                \
    EXPECT_TRUE(value_is_error(result));                                       \
    if (value_is_error(result)) {                                              \
      EXPECT_NE(strstr(error_message_of(result), fragment), nullptr)           \
          << "message: " << error_message_of(result);                          \
    }                                                                          \
    delete_clux_context(&ctx);                                                 \
    delete_allocator(&a);                                                      \
  } while (0);

/* 最后一条有值语句：裸 identifier 表达式。 */
TEST(ExecTest, LastStatementValue) {
  RUN_SCRIPT("var x = 1 + 2; x");
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(value_is_error(result));
  EXPECT_EQ(read_i32(result), 3);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_I32);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 无值程序返回 NULL。 */
TEST(ExecTest, NoValueProgram) {
  RUN_SCRIPT("var x = 1;");
  EXPECT_EQ(result, nullptr);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 后缀字面量：f64。 */
TEST(ExecTest, SuffixLiteral) {
  RUN_SCRIPT("3.14f64");
  ASSERT_NE(result, nullptr);
  EXPECT_DOUBLE_EQ(read_f64(result), 3.14);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_F64);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 数字后缀与进制。 */
TEST(ExecTest, IntSuffixAndBases) {
  RUN_SCRIPT("var a = 0x10i32; var b = 0o7i32; var c = 0b101i32;"
             "a + b + c");
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(value_is_error(result));
  EXPECT_EQ(read_i32(result), 28);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 字符串转义解码。 */
TEST(ExecTest, StringEscape) {
  RUN_SCRIPT("var s = \"a\\tb\\n\"; s");
  ASSERT_NE(result, nullptr);
  EXPECT_STREQ(read_str(result), "a\tb\n");
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 布尔与逻辑短路。 */
TEST(ExecTest, BoolAndShortCircuit) {
  RUN_SCRIPT("var a = true; var b = false; a && (b || a)");
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(read_bool(result));
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 短路右侧不求值：右侧含未定义名也不报错。 */
TEST(ExecTest, ShortCircuitSkipsRight) {
  RUN_SCRIPT("false && undefined_name");
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(value_is_error(result));
  EXPECT_FALSE(read_bool(result));
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* if/else 分支。 */
TEST(ExecTest, IfElse) {
  RUN_SCRIPT("var x = 0; if (true) { x = 1; } else { x = 2; } x");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(read_i32(result), 1);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* while 累加。 */
TEST(ExecTest, WhileAccumulate) {
  RUN_SCRIPT("var i = 0; var s = 0;"
             "while (i < 5) { s = s + i; i = i + 1; }"
             "s");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(read_i32(result), 10);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* break 终止循环。 */
TEST(ExecTest, BreakExitsLoop) {
  RUN_SCRIPT("var i = 0;"
             "while (true) { i = i + 1; if (i == 3) { break; } }"
             "i");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(read_i32(result), 3);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* continue 跳过本次迭代。 */
TEST(ExecTest, ContinueSkipsIteration) {
  RUN_SCRIPT("var i = 0; var s = 0;"
             "while (i < 5) {"
             "  i = i + 1;"
             "  if (i % 2 == 0) { continue; }"
             "  s = s + i;"
             "}"
             "s");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(read_i32(result), 9);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 显式类型标注 + safe_cast。 */
TEST(ExecTest, VarAnnotationCast) {
  RUN_SCRIPT("var x: i64 = 42; x");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_I64);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 标注类型不兼容报 error。 */
TEST(ExecTest, VarAnnotationMismatch) {
  EXPECT_EXEC_ERROR("var x: bool = 42;", "cannot");
}

/* 未定义名称。 */
TEST(ExecTest, UndefinedName) {
  EXPECT_EXEC_ERROR("missing_name", "undefined name");
}

/* 顶层 break 逃逸报语义化 error。 */
TEST(ExecTest, TopLevelBreak) { EXPECT_EXEC_ERROR("break;", "outside"); }

/* 顶层 continue 逃逸。 */
TEST(ExecTest, TopLevelContinue) { EXPECT_EXEC_ERROR("continue;", "outside"); }

/* 重复声明。 */
TEST(ExecTest, DuplicateDeclaration) {
  EXPECT_EXEC_ERROR("var x = 1; var x = 2;", "duplicate declaration");
}

/* 块正常结果不跨作用域泄漏：块内值不出块。 */
TEST(ExecTest, BlockValueScoped) {
  RUN_SCRIPT("var a = 1; { var b = 2; a = a + b; } a");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(read_i32(result), 3);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}
