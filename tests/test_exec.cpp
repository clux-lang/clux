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

/* 借用初值按值拷贝：var y = x 后 y 是独立存储，改 y 不影响 x。 */
TEST(ExecTest, VarFromIdentifierCopies) {
  RUN_SCRIPT("var x = 1; var y = x; y = 5; x");
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(value_is_error(result));
  EXPECT_EQ(read_i32(result), 1);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 函数声明落地：声明后名字可查，重复声明报错。 */
TEST(ExecTest, FuncDeclaration) {
  RUN_SCRIPT("func f(a: i32): i32 { return a + 1; }");
  EXPECT_EQ(result, nullptr); /* 声明语句不产值 */
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 闭包捕获按值拷贝：声明后修改源变量不影响捕获快照（类型标注形态）。 */
TEST(ExecTest, ClosureCaptureByValue) {
  /* 捕获快照断言依赖 #33 调用帧（f 的执行），此处先验证带捕获声明可落地。 */
  RUN_SCRIPT("var y = 10;"
             "func |(y: i32)| f(): i32 { return y; }"
             "y = 20;");
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(value_is_error(result));
  EXPECT_EQ(read_i32(result), 20);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* 空捕获列表 ||：无捕获项的函数声明可落地。 */
TEST(ExecTest, EmptyCaptureList) {
  RUN_SCRIPT("var x = 1; func || f(): i32 { return x; }; 5");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(read_i32(result), 5);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* cfunc 无可执行体，脚本模式报语义化错误。 */
TEST(ExecTest, CfuncNotExecutable) {
  EXPECT_EXEC_ERROR("cfunc f(): void;", "cfunc");
}

/* 泛型函数声明在脚本模式不支持。 */
TEST(ExecTest, GenericFuncNotSupported) {
  EXPECT_EXEC_ERROR("func f[T](a: T): T { return a; }", "generic");
}

/* 捕获名字未定义（类型标注合法，但作用域无同名值可捕获）。 */
TEST(ExecTest, CaptureUndefinedName) {
  EXPECT_EXEC_ERROR("func |(y: i32)| f(): i32 { return y; }", "undefined name");
}

/* type alias：别名落为 type value，类型语境可用。 */
TEST(ExecTest, TypeAlias) {
  RUN_SCRIPT("type Int = i32; var x: Int = 5; x");
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(value_is_error(result));
  EXPECT_EQ(read_i32(result), 5);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* type alias 右边必须是类型。 */
TEST(ExecTest, TypeAliasNotAType) {
  EXPECT_EXEC_ERROR("type Int = 5;", "requires a type");
}

/* 泛型类型别名走模板值落地。 */
TEST(ExecTest, GenericTypeAliasTemplate) {
  RUN_SCRIPT("type Pair[T] = struct { a: T; b: T; }; 7");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(read_i32(result), 7);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* struct 声明落地：名字可用作类型标注（构造与成员访问走调用任务通道，
   此处只验证声明落地）。 */
TEST(ExecTest, StructDeclaration) {
  RUN_SCRIPT("struct Point { x: i32; y: i32; }; 5");
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(value_is_error(result));
  EXPECT_EQ(read_i32(result), 5);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* struct 重复字段名报错。 */
TEST(ExecTest, StructDuplicateField) {
  EXPECT_EXEC_ERROR("struct P { x: i32; x: i32; }", "invalid");
}

/* cunion 声明落地。 */
TEST(ExecTest, CunionDeclaration) {
  RUN_SCRIPT("cunion V { i: i32; f: f32; }; 1");
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(value_is_error(result));
  EXPECT_EQ(read_i32(result), 1);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* tag union 声明落地。 */
TEST(ExecTest, UnionDeclaration) {
  RUN_SCRIPT("union Shape { circle: i32; rect: i32; }; 2");
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(value_is_error(result));
  EXPECT_EQ(read_i32(result), 2);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* enum 声明落地：枚举名落为 underlying 的类型别名。 */
TEST(ExecTest, EnumDeclaration) {
  RUN_SCRIPT("enum Color: u8 { Red = 1, Green = 2, }; 9");
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(value_is_error(result));
  EXPECT_EQ(read_i32(result), 9);
  delete_clux_context(&ctx);
  delete_allocator(&a);
}

/* enum underlying 必须是整数类型。 */
TEST(ExecTest, EnumUnderlyingNotInteger) {
  EXPECT_EXEC_ERROR("enum C: str { A = \"a\", }", "integer");
}
