/* exec 模块单元测试：exec_node 分派 + 5 个 literal 求值接口。
   AST 节点只记 location 区间，literal 求值直接用 loc->begin.offset 从
   源码解码文本——测试用持久字符串字面量手工构造 node+location。 */
#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/position.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/exec.h"
#include "exec/literal.h"
#include "parser/node.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int8_t read_i8(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(int8_t *)raw : 0;
}

static int16_t read_i16(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(int16_t *)raw : 0;
}

static int32_t read_i32(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(int32_t *)raw : 0;
}

static int64_t read_i64(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(int64_t *)raw : 0;
}

static uint8_t read_u8(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(uint8_t *)raw : 0;
}

static uint16_t read_u16(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(uint16_t *)raw : 0;
}

static uint32_t read_u32(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(uint32_t *)raw : 0;
}

static uint64_t read_u64(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(uint64_t *)raw : 0;
}

static intptr_t read_isize(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(intptr_t *)raw : 0;
}

static size_t read_usize(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(size_t *)raw : 0;
}

static float read_f32(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(float *)raw : 0;
}

static double read_f64(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(double *)raw : 0;
}

static bool read_bool(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(bool *)raw : false;
}

static const char *read_str(clux_value_t *value) {
  char **raw = (char **)ptr_to_raw(clux_value_get_data(value), 0);
  return (raw && *raw) ? *raw : "";
}

static const char *error_message_of(clux_value_t *value) {
  clux_error_t **stored =
      (clux_error_t **)ptr_to_raw(clux_value_get_data(value), 0);
  return stored ? error_get_message(*stored) : nullptr;
}

class ExecLiteralTest : public ::testing::Test {
protected:
  allocator_t *a = nullptr;
  clux_context_t *ctx = nullptr;

  void SetUp() override {
    a = create_allocator(malloc, free);
    ctx = create_clux_context(a);
    ASSERT_NE(ctx, nullptr);
  }

  void TearDown() override {
    delete_clux_context(&ctx);
    delete_allocator(&a);
  }

  /* 构造 kind 节点，其 location 覆盖 text 的完整区间。
     text 必须是持久存储（字符串字面量），node_set_location 按值拷贝
     location，但 begin/end.offset 仍指向 text 本身。 */
  node_t *make_node(node_kind_t kind, const char *text) {
    node_t *node = create_node(a, kind);
    location_t loc;
    loc.filename = "test.clux";
    loc.begin.offset = text;
    loc.begin.column = 1;
    loc.begin.line = 1;
    loc.end.offset = text + strlen(text);
    loc.end.column = 1;
    loc.end.line = 1;
    node_set_location(node, &loc);
    return node;
  }
};

/* ---- exec_literal_numeric ---- */

/* 场景: 无后缀十进制整数，默认产出 i32
   预期: 值类型为 i32，值为 42 */
TEST_F(ExecLiteralTest, NumericDecimalDefaultsToI32) {
  node_t *node = make_node(CLUX_NODE_LITERAL_NUMERIC, "42");
  clux_value_t *value = exec_literal_numeric(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(value), 42);
  delete_node(&node);
}

/* 场景: 数字 "0x" 长度为 2，不满足 len>2 的进制前缀条件，按十进制处理
   预期: strtoull("0x", 10) = 0，产出 i32 0 而非报错 */
TEST_F(ExecLiteralTest, NumericShortZeroXPrefixIsDecimal) {
  node_t *node = make_node(CLUX_NODE_LITERAL_NUMERIC, "0x");
  clux_value_t *value = exec_literal_numeric(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(value), 0);
  delete_node(&node);
}

/* 场景: 0x/0X 十六进制前缀（大小写 x 与大小写数字均支持）
   预期: 0x1A=26，0XF=15，类型 i32 */
TEST_F(ExecLiteralTest, NumericHexLiteral) {
  node_t *lower = make_node(CLUX_NODE_LITERAL_NUMERIC, "0x1A");
  clux_value_t *lower_value = exec_literal_numeric(ctx, lower);
  ASSERT_NE(lower_value, nullptr);
  EXPECT_EQ(clux_value_get_type(lower_value),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(lower_value), 26);
  delete_node(&lower);

  node_t *upper = make_node(CLUX_NODE_LITERAL_NUMERIC, "0XF");
  clux_value_t *upper_value = exec_literal_numeric(ctx, upper);
  ASSERT_NE(upper_value, nullptr);
  EXPECT_EQ(read_i32(upper_value), 15);
  delete_node(&upper);
}

/* 场景: 0o 八进制前缀
   预期: 0o17 = 15，类型 i32 */
TEST_F(ExecLiteralTest, NumericOctalLiteral) {
  node_t *node = make_node(CLUX_NODE_LITERAL_NUMERIC, "0o17");
  clux_value_t *value = exec_literal_numeric(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(value), 15);
  delete_node(&node);
}

/* 场景: 0b 二进制前缀
   预期: 0b101 = 5，类型 i32 */
TEST_F(ExecLiteralTest, NumericBinaryLiteral) {
  node_t *node = make_node(CLUX_NODE_LITERAL_NUMERIC, "0b101");
  clux_value_t *value = exec_literal_numeric(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(value), 5);
  delete_node(&node);
}

/* 场景: 负号（数字部分首字符 '-'），含负十六进制的组合路径
   预期: -7 → i32 -7；-0x10 → 负号剥离后按 16 进制解码 16，产出 i32 -16 */
TEST_F(ExecLiteralTest, NumericNegativeLiteral) {
  node_t *decimal = make_node(CLUX_NODE_LITERAL_NUMERIC, "-7");
  clux_value_t *decimal_value = exec_literal_numeric(ctx, decimal);
  ASSERT_NE(decimal_value, nullptr);
  EXPECT_EQ(clux_value_get_type(decimal_value),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(decimal_value), -7);
  delete_node(&decimal);

  node_t *hex = make_node(CLUX_NODE_LITERAL_NUMERIC, "-0x10");
  clux_value_t *hex_value = exec_literal_numeric(ctx, hex);
  ASSERT_NE(hex_value, nullptr);
  EXPECT_EQ(read_i32(hex_value), -16);
  delete_node(&hex);
}

/* 场景: 全部 10 种整数后缀决定产出类型
   预期: 后缀决定 type kind，值按目标宽度解码 */
TEST_F(ExecLiteralTest, NumericSuffixSelectsIntegerType) {
  struct {
    const char *text;
    clux_type_kind_t kind;
    int64_t expected;
  } cases[] = {
      {"7i8", CLUX_TYPE_I8, 7},
      {"300i16", CLUX_TYPE_I16, 300},
      {"100000i32", CLUX_TYPE_I32, 100000},
      {"5000000000i64", CLUX_TYPE_I64, 5000000000LL},
      {"9isize", CLUX_TYPE_ISIZE, 9},
      {"200u8", CLUX_TYPE_U8, 200},
      {"60000u16", CLUX_TYPE_U16, 60000},
      {"4000000000u32", CLUX_TYPE_U32, 4000000000LL},
      {"5u64", CLUX_TYPE_U64, 5},
      {"11usize", CLUX_TYPE_USIZE, 11},
  };
  for (const auto &c : cases) {
    node_t *node = make_node(CLUX_NODE_LITERAL_NUMERIC, c.text);
    clux_value_t *value = exec_literal_numeric(ctx, node);
    ASSERT_NE(value, nullptr) << c.text;
    EXPECT_EQ(clux_value_get_type(value),
              clux_context_builtin_type(ctx, c.kind))
        << c.text;
    int64_t actual = 0;
    switch (c.kind) {
    case CLUX_TYPE_I8:
      actual = read_i8(value);
      break;
    case CLUX_TYPE_I16:
      actual = read_i16(value);
      break;
    case CLUX_TYPE_I32:
      actual = read_i32(value);
      break;
    case CLUX_TYPE_I64:
      actual = read_i64(value);
      break;
    case CLUX_TYPE_ISIZE:
      actual = (int64_t)read_isize(value);
      break;
    case CLUX_TYPE_U8:
      actual = read_u8(value);
      break;
    case CLUX_TYPE_U16:
      actual = read_u16(value);
      break;
    case CLUX_TYPE_U32:
      actual = (int64_t)read_u32(value);
      break;
    case CLUX_TYPE_U64:
      actual = (int64_t)read_u64(value);
      break;
    case CLUX_TYPE_USIZE:
      actual = (int64_t)read_usize(value);
      break;
    default:
      FAIL() << "unhandled kind for " << c.text;
    }
    EXPECT_EQ(actual, c.expected) << c.text;
    delete_node(&node);
  }
}

/* 场景: u64 后缀承载 64 位上限值
   预期: 18446744073709551615u64 精确解码为 UINT64_MAX */
TEST_F(ExecLiteralTest, NumericU64MaxValue) {
  node_t *node =
      make_node(CLUX_NODE_LITERAL_NUMERIC, "18446744073709551615u64");
  clux_value_t *value = exec_literal_numeric(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_U64));
  EXPECT_EQ(read_u64(value), UINT64_MAX);
  delete_node(&node);
}

/* 场景: f32 浮点后缀
   预期: 3.5f32 经 strtof 解码为 f32 3.5 */
TEST_F(ExecLiteralTest, NumericFloatF32) {
  node_t *node = make_node(CLUX_NODE_LITERAL_NUMERIC, "3.5f32");
  clux_value_t *value = exec_literal_numeric(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_F32));
  EXPECT_FLOAT_EQ(read_f32(value), 3.5f);
  delete_node(&node);
}

/* 场景: f64 浮点后缀 + 负号（浮点路径的负数）
   预期: -7.5f64 解码为 f64 -7.5 */
TEST_F(ExecLiteralTest, NumericFloatF64Negative) {
  node_t *node = make_node(CLUX_NODE_LITERAL_NUMERIC, "-7.5f64");
  clux_value_t *value = exec_literal_numeric(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_F64));
  EXPECT_DOUBLE_EQ(read_f64(value), -7.5);
  delete_node(&node);
}

/* 场景: 整数路径数字部分达到 64 字节 buffer 上限
   预期: 返回 error 值，消息 "numeric literal too long" */
TEST_F(ExecLiteralTest, NumericIntegerTooLong) {
  const char *text = "1111111111111111111111111111111111111111"
                     "111111111111111111111111"; /* 64 位数字 */
  ASSERT_EQ(strlen(text), 64u);
  node_t *node = make_node(CLUX_NODE_LITERAL_NUMERIC, text);
  clux_value_t *value = exec_literal_numeric(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "numeric literal too long");
  delete_node(&node);
}

/* 场景: 浮点路径数字部分达到 make_float 的 64 字节 buffer 上限
   预期: 返回 error 值，消息 "numeric literal too long"（浮点独立检查点） */
TEST_F(ExecLiteralTest, NumericFloatTooLong) {
  const char *text = "1111111111111111111111111111111111111111"
                     "111111111111111111111111f64"; /* 64 位数字 + f64 */
  ASSERT_EQ(strlen(text), 67u);
  node_t *node = make_node(CLUX_NODE_LITERAL_NUMERIC, text);
  clux_value_t *value = exec_literal_numeric(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "numeric literal too long");
  delete_node(&node);
}

/* ---- exec_literal_string ---- */

/* 场景: 普通字符串字面量（去引号）
   预期: 产出 str 值，内容 "hi" */
TEST_F(ExecLiteralTest, StringSimple) {
  node_t *node = make_node(CLUX_NODE_LITERAL_STRING, "\"hi\"");
  clux_value_t *value = exec_literal_string(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_STR));
  EXPECT_STREQ(read_str(value), "hi");
  delete_node(&node);
}

/* 场景: 空字符串 ""
   预期: 产出空 str 值 */
TEST_F(ExecLiteralTest, StringEmpty) {
  node_t *node = make_node(CLUX_NODE_LITERAL_STRING, "\"\"");
  clux_value_t *value = exec_literal_string(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_STR));
  EXPECT_STREQ(read_str(value), "");
  delete_node(&node);
}

/* 场景: 全部简单转义 \n \t \r \\ \' \" 加 \0
   预期: 解码为对应字节，\0 恰好落在结尾终止符位置 */
TEST_F(ExecLiteralTest, StringDecodesFullEscapeSet) {
  node_t *node =
      make_node(CLUX_NODE_LITERAL_STRING, "\"\\n\\t\\r\\\\\\'\\\"\\0\"");
  clux_value_t *value = exec_literal_string(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_STR));
  EXPECT_STREQ(read_str(value), "\n\t\r\\'\"");
  delete_node(&node);
}

/* 场景: \xNN 十六进制转义（含大写 FF 上限位与单 hex 位 \xA）
   预期: \x41\x42 按 ASCII 解码，\xFF 解码为 255，\xA 解码为 10 */
TEST_F(ExecLiteralTest, StringHexEscapes) {
  node_t *ascii = make_node(CLUX_NODE_LITERAL_STRING, "\"\\x41\\x42\\x7a\"");
  clux_value_t *ascii_value = exec_literal_string(ctx, ascii);
  ASSERT_NE(ascii_value, nullptr);
  EXPECT_EQ(clux_value_get_type(ascii_value),
            clux_context_builtin_type(ctx, CLUX_TYPE_STR));
  EXPECT_STREQ(read_str(ascii_value), "ABz");
  delete_node(&ascii);

  node_t *upper = make_node(CLUX_NODE_LITERAL_STRING, "\"\\xFF\"");
  clux_value_t *upper_value = exec_literal_string(ctx, upper);
  ASSERT_NE(upper_value, nullptr);
  EXPECT_EQ((unsigned char)read_str(upper_value)[0], 0xFF);
  EXPECT_EQ(read_str(upper_value)[1], '\0');
  delete_node(&upper);

  node_t *single = make_node(CLUX_NODE_LITERAL_STRING, "\"\\xA\"");
  clux_value_t *single_value = exec_literal_string(ctx, single);
  ASSERT_NE(single_value, nullptr);
  EXPECT_EQ((unsigned char)read_str(single_value)[0], 0x0A);
  delete_node(&single);
}

/* 场景: 未知转义 \q
   预期: 返回 error 值，消息 "invalid escape sequence in string literal" */
TEST_F(ExecLiteralTest, StringInvalidEscapeError) {
  node_t *node = make_node(CLUX_NODE_LITERAL_STRING, "\"\\q\"");
  clux_value_t *value = exec_literal_string(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value),
               "invalid escape sequence in string literal");
  delete_node(&node);
}

/* 场景: \x 后无任何 hex 位（digits == 0 才失败）
   预期: 返回 error 值（转义错误） */
TEST_F(ExecLiteralTest, StringHexEscapeWithoutDigitsError) {
  node_t *node = make_node(CLUX_NODE_LITERAL_STRING, "\"\\xz\"");
  clux_value_t *value = exec_literal_string(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value),
               "invalid escape sequence in string literal");
  delete_node(&node);
}

/* 场景: 内容以尾部反斜杠结束（decode_escape 的 s+1>=end 守卫）
   预期: 返回 error 值（转义错误） */
TEST_F(ExecLiteralTest, StringTrailingBackslashError) {
  node_t *node = make_node(CLUX_NODE_LITERAL_STRING, "\"a\\\"");
  clux_value_t *value = exec_literal_string(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value),
               "invalid escape sequence in string literal");
  delete_node(&node);
}

/* ---- exec_literal_charactor ---- */

/* 场景: 普通字符字面量
   预期: 产出 u8 值 'a' = 97 */
TEST_F(ExecLiteralTest, CharacterSimple) {
  node_t *node = make_node(CLUX_NODE_LITERAL_CHARACTER, "'a'");
  clux_value_t *value = exec_literal_charactor(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_U8));
  EXPECT_EQ(read_u8(value), 97);
  delete_node(&node);
}

/* 场景: 转义字符 \n 与 \'（引号转义在字符字面量中合法）
   预期: \n → u8 10；\' → u8 39 */
TEST_F(ExecLiteralTest, CharacterEscapes) {
  node_t *newline = make_node(CLUX_NODE_LITERAL_CHARACTER, "'\\n'");
  clux_value_t *newline_value = exec_literal_charactor(ctx, newline);
  ASSERT_NE(newline_value, nullptr);
  EXPECT_EQ(clux_value_get_type(newline_value),
            clux_context_builtin_type(ctx, CLUX_TYPE_U8));
  EXPECT_EQ(read_u8(newline_value), 10);
  delete_node(&newline);

  node_t *quote = make_node(CLUX_NODE_LITERAL_CHARACTER, "'\\''");
  clux_value_t *quote_value = exec_literal_charactor(ctx, quote);
  ASSERT_NE(quote_value, nullptr);
  EXPECT_EQ(clux_value_get_type(quote_value),
            clux_context_builtin_type(ctx, CLUX_TYPE_U8));
  EXPECT_EQ(read_u8(quote_value), 39);
  delete_node(&quote);
}

/* 场景: \xNN 十六进制转义字符
   预期: '\x41' → u8 65 */
TEST_F(ExecLiteralTest, CharacterHexEscape) {
  node_t *node = make_node(CLUX_NODE_LITERAL_CHARACTER, "'\\x41'");
  clux_value_t *value = exec_literal_charactor(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_U8));
  EXPECT_EQ(read_u8(value), 65);
  delete_node(&node);
}

/* 场景: 空字符字面量 ''（解码长度 0）
   预期: 返回 error 值，消息 "character literal must contain exactly one u8
   character" */
TEST_F(ExecLiteralTest, CharacterEmptyError) {
  node_t *node = make_node(CLUX_NODE_LITERAL_CHARACTER, "''");
  clux_value_t *value = exec_literal_charactor(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value),
               "character literal must contain exactly one u8 character");
  delete_node(&node);
}

/* 场景: 多字符字面量 'ab'（解码长度 2）
   预期: 返回 error 值，同 "exactly one u8 character" 消息 */
TEST_F(ExecLiteralTest, CharacterMultiByteError) {
  node_t *node = make_node(CLUX_NODE_LITERAL_CHARACTER, "'ab'");
  clux_value_t *value = exec_literal_charactor(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value),
               "character literal must contain exactly one u8 character");
  delete_node(&node);
}

/* 场景: 字符字面量中的非法转义 '\q'
   预期: 返回 error 值，消息为 character 版本的转义错误 */
TEST_F(ExecLiteralTest, CharacterInvalidEscapeError) {
  node_t *node = make_node(CLUX_NODE_LITERAL_CHARACTER, "'\\q'");
  clux_value_t *value = exec_literal_charactor(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value),
               "invalid escape sequence in character literal");
  delete_node(&node);
}

/* ---- exec_literal_boolean ---- */

/* 场景: "true" 关键字
   预期: 产出 bool 值 true */
TEST_F(ExecLiteralTest, BooleanTrue) {
  node_t *node = make_node(CLUX_NODE_LITERAL_BOOLEAN, "true");
  clux_value_t *value = exec_literal_boolean(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
  EXPECT_TRUE(read_bool(value));
  delete_node(&node);
}

/* 场景: "false" 关键字（location_is 不匹配路径）
   预期: 产出 bool 值 false */
TEST_F(ExecLiteralTest, BooleanFalse) {
  node_t *node = make_node(CLUX_NODE_LITERAL_BOOLEAN, "false");
  clux_value_t *value = exec_literal_boolean(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
  EXPECT_FALSE(read_bool(value));
  delete_node(&node);
}

/* ---- exec_literal_identifier ---- */

/* 场景: 内建整数类型名 "i32"（context 创建时以 type value 注入 global scope）
   预期: 产出 type 值——值类型为 "type" 单例，data 指向 i32 内建类型 */
TEST_F(ExecLiteralTest, IdentifierBuiltinIntegerName) {
  node_t *node = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32");
  clux_value_t *value = exec_literal_identifier(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value), clux_context_get_type_type(ctx));
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(*(clux_type_t **)raw,
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  delete_node(&node);
}

/* 场景: 内建类型名 "opaque"
   预期: 同上，data 指向 opaque 内建类型 */
TEST_F(ExecLiteralTest, IdentifierBuiltinOpaqueName) {
  node_t *node = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "opaque");
  clux_value_t *value = exec_literal_identifier(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_value_get_type(value), clux_context_get_type_type(ctx));
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(*(clux_type_t **)raw,
            clux_context_builtin_type(ctx, CLUX_TYPE_OPAQUE));
  delete_node(&node);
}

/* 场景: 未声明的名字
   预期: 返回 error 值，消息 "use of undeclared identifier '...'" */
TEST_F(ExecLiteralTest, IdentifierUndeclaredError) {
  node_t *node = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nope_var");
  clux_value_t *value = exec_literal_identifier(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value),
               "use of undeclared identifier 'nope_var'");
  delete_node(&node);
}

/* ---- exec_node 分派 ---- */

/* 场景: exec_node 按已注册 kind 委托到对应 literal 回调
   预期: numeric/string/identifier 三种 kind 经 exec_node 的求值结果
   与直接调用 literal 函数一致（值与类型都相同） */
TEST_F(ExecLiteralTest, ExecNodeDispatchesRegisteredKinds) {
  node_t *numeric = make_node(CLUX_NODE_LITERAL_NUMERIC, "42");
  clux_value_t *numeric_value = exec_node(ctx, numeric);
  ASSERT_NE(numeric_value, nullptr);
  EXPECT_EQ(clux_value_get_type(numeric_value),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(numeric_value), 42);
  delete_node(&numeric);

  node_t *str = make_node(CLUX_NODE_LITERAL_STRING, "\"ok\"");
  clux_value_t *str_value = exec_node(ctx, str);
  ASSERT_NE(str_value, nullptr);
  EXPECT_EQ(clux_value_get_type(str_value),
            clux_context_builtin_type(ctx, CLUX_TYPE_STR));
  EXPECT_STREQ(read_str(str_value), "ok");
  delete_node(&str);

  node_t *boolean = make_node(CLUX_NODE_LITERAL_BOOLEAN, "true");
  clux_value_t *boolean_value = exec_node(ctx, boolean);
  ASSERT_NE(boolean_value, nullptr);
  EXPECT_TRUE(read_bool(boolean_value));
  delete_node(&boolean);
}

/* 场景: 未注册 kind（EXPR_BINARY 无回调）
   预期: 返回 error 值，消息 "unsupported node kind: EXPR_BINARY" */
TEST_F(ExecLiteralTest, ExecNodeUnsupportedKindError) {
  node_t *node = make_node(CLUX_NODE_EXPR_BINARY, "a + b");
  clux_value_t *value = exec_node(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "unsupported node kind: EXPR_BINARY");
  delete_node(&node);
}

/* 场景: 未注册的 literal kind（LITERAL_UNDEFINED 回调为空）
   预期: 走通用不支持路径而非崩溃 */
TEST_F(ExecLiteralTest, ExecNodeUnregisteredLiteralKindError) {
  node_t *node = make_node(CLUX_NODE_LITERAL_UNDEFINED, "undefined");
  clux_value_t *value = exec_node(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value),
               "unsupported node kind: LITERAL_UNDEFINED");
  delete_node(&node);
}

/* 场景: 逐一触发一个未注册 kind 的精确名字（node_kind_name 的 STMT_BLOCK case）
   预期: 错误消息包含 kind 的准确名字 */
TEST_F(ExecLiteralTest, ExecNodeUnsupportedKindNameStmtBlock) {
  node_t *node = make_node(CLUX_NODE_STMT_BLOCK, "...");
  clux_value_t *value = exec_node(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "unsupported node kind: STMT_BLOCK");
  delete_node(&node);
}

/* 场景: 遍历全部 node kind，验证分派表完整性
   预期: 5 个已注册 literal kind 不产生 unsupported 错误，
         其余所有 kind 均返回 "unsupported node kind: " 前缀的 error */
TEST_F(ExecLiteralTest, ExecNodeAllKindsDispatch) {
  static const char kUnsupportedPrefix[] = "unsupported node kind: ";
  const size_t kUnsupportedPrefixLen = sizeof(kUnsupportedPrefix) - 1;
  for (int k = CLUX_NODE_SYMBOL; k < CLUX_NODE_KIND_MAX; k++) {
    node_kind_t kind = static_cast<node_kind_t>(k);
    bool registered = kind == CLUX_NODE_LITERAL_STRING ||
                      kind == CLUX_NODE_LITERAL_CHARACTER ||
                      kind == CLUX_NODE_LITERAL_NUMERIC ||
                      kind == CLUX_NODE_LITERAL_BOOLEAN ||
                      kind == CLUX_NODE_LITERAL_IDENTIFIER;
    node_t *node = make_node(kind, "true");
    clux_value_t *value = exec_node(ctx, node);
    ASSERT_NE(value, nullptr) << "kind " << k << " returned null";
    bool is_unsupported_error =
        clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR &&
        strncmp(error_message_of(value),
                kUnsupportedPrefix,
                kUnsupportedPrefixLen) == 0;
    if (registered) {
      EXPECT_FALSE(is_unsupported_error)
          << "kind " << k << " should be registered";
    } else {
      EXPECT_TRUE(is_unsupported_error)
          << "kind " << k << " should report unsupported, got: "
          << (clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR
                  ? error_message_of(value)
                  : "<non-error value>");
    }
    delete_node(&node);
  }
}
