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
#include "engine/qualifier.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/exec.h"
#include "exec/expr.h"
#include "exec/literal.h"
#include "exec/stmt.h"
#include "parser/node.h"
#include "parser/node_assign.h"
#include "parser/node_binary.h"
#include "parser/node_group.h"
#include "parser/node_stmt_var.h"
#include "parser/node_slice.h"
#include "parser/node_subscript.h"
#include "parser/node_ternary.h"
#include <cstdio>
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

  /* 合成完整赋值节点 left op value：left 是 identifier 文本（经 lookup
     解析目标变量），op 结点的 location 覆盖运算符文本（符号分派依赖
     location_is 对该文本的匹配），根节点 delete 递归释放全部子节点。 */
  node_t *make_assign(node_kind_t value_kind,
                      const char *op_text,
                      const char *left_text,
                      const char *value_text) {
    node_t *node = create_assign_node(a);
    assign_node_set_left(node,
                         make_node(CLUX_NODE_LITERAL_IDENTIFIER, left_text));
    assign_node_set_symbol(node, make_node(CLUX_NODE_SYMBOL, op_text));
    assign_node_set_value(node, make_node(value_kind, value_text));
    return node;
  }

  /* 合成括号分组结点（子结点调用方构造，delete 根结点时一并释放）。 */
  node_t *make_group_of(node_t *expr) {
    node_t *node = create_group_node(a);
    group_node_set_expr(node, expr);
    return node;
  }

  /* 合成 var 声明语句结点：type_text 为 NULL 时省略类型标注，
     type_kind/type_text 是类型表达式的结点种类与文本。 */
  node_t *make_stmt_var(const char *name_text,
                        node_kind_t type_kind,
                        const char *type_text,
                        node_kind_t value_kind,
                        const char *value_text) {
    node_t *node = create_stmt_var_node(a);
    stmt_var_node_set_keyword(node, make_node(CLUX_NODE_SYMBOL, "var"));
    stmt_var_node_set_name(node,
                           make_node(CLUX_NODE_LITERAL_IDENTIFIER, name_text));
    if (type_text) {
      stmt_var_node_set_type(node, make_node(type_kind, type_text));
    }
    stmt_var_node_set_value(node, make_node(value_kind, value_text));
    return node;
  }

  /* 合成下标节点：location 覆盖 text 区间（错误值定位用），base 与
     args 由调用方经 subscript_node_set_base/add_arg 填充，
     delete 根节点时递归释放全部子节点。 */
  node_t *make_subscript(const char *text) {
    node_t *node = create_subscript_node(a);
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

  /* 合成完整下标节点 base[arg]：arg 为 NULL 时无实参（0 参场景），
     delete 根节点时递归释放全部子节点。 */
  node_t *make_subscript_of(node_t *base, node_t *arg) {
    node_t *node = make_subscript("subscript");
    subscript_node_set_base(node, base);
    if (arg) {
      subscript_node_add_arg(node, arg);
    }
    return node;
  }

  /* 合成完整切片节点 base[lo:hi]：location 覆盖 text 区间（错误值定位
     用），lo/hi 为 NULL 表示省略该端（开放端点），delete 根节点时递归
     释放全部子节点。 */
  node_t *
  make_slice_of(const char *text, node_t *base, node_t *lo, node_t *hi) {
    node_t *node = create_slice_node(a);
    location_t loc;
    loc.filename = "test.clux";
    loc.begin.offset = text;
    loc.begin.column = 1;
    loc.begin.line = 1;
    loc.end.offset = text + strlen(text);
    loc.end.column = 1;
    loc.end.line = 1;
    node_set_location(node, &loc);
    slice_node_set_base(node, base);
    slice_node_set_lo(node, lo);
    slice_node_set_hi(node, hi);
    return node;
  }

  /* 构造 [len]i32 数组并以 name 注册进作用域（identifier 可 lookup），
     元素 i 预填非零互异值 10+i（经 engine index/assign 写入），
     避免断言撞上零初始化默认值。失败返回 NULL 并由调用方 ASSERT。 */
  clux_value_t *make_i32_array(const char *name, size_t len) {
    clux_type_t *arr_type = clux_context_get_array_type(
        ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32), len);
    clux_value_t *arr = clux_context_create_array(ctx, arr_type, name, NULL);
    if (!arr) {
      return nullptr;
    }
    for (size_t i = 0; i < len; i++) {
      clux_value_t *view = clux_context_index(
          ctx, arr, clux_context_create_usize(ctx, i, NULL, NULL), NULL);
      if (!view ||
          clux_type_get_kind(clux_value_get_type(view)) == CLUX_TYPE_ERROR) {
        return nullptr;
      }
      clux_value_t *init =
          clux_context_create_i32(ctx, (int32_t)(10 + i), NULL, NULL);
      if (!init || clux_context_assign(ctx, view, init, NULL) != view) {
        return nullptr;
      }
    }
    return arr;
  }
};

/* 右值求值探针：64 位纯数字（触发 "numeric literal too long"）。
   短路路径不产生 error，非短路路径必产生该精确错误。 */
static const char kTooLongNumeric[] = "1111111111111111111111111111111111111111"
                                      "111111111111111111111111";

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

/* 场景: 未注册 kind（EXPR_REST 无回调）
   预期: 返回 error 值，消息 "unsupported node kind: EXPR_REST" */
TEST_F(ExecLiteralTest, ExecNodeUnsupportedKindError) {
  node_t *node = make_node(CLUX_NODE_EXPR_REST, "a[1:2]");
  clux_value_t *value = exec_node(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "unsupported node kind: EXPR_REST");
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
   预期: 已注册 kind 的合成节点（无子节点）不符合 AST 构造契约，跳过——
         真实覆盖在 assign/var/group 专用用例；其余所有 kind 均返回
         "unsupported node kind: " 前缀的 error */
TEST_F(ExecLiteralTest, ExecNodeAllKindsDispatch) {
  static const char kUnsupportedPrefix[] = "unsupported node kind: ";
  const size_t kUnsupportedPrefixLen = sizeof(kUnsupportedPrefix) - 1;
  for (int k = CLUX_NODE_SYMBOL; k < CLUX_NODE_KIND_MAX; k++) {
    node_kind_t kind = static_cast<node_kind_t>(k);
    bool synthetic_skip =
        kind == CLUX_NODE_LITERAL_STRING ||
        kind == CLUX_NODE_LITERAL_CHARACTER ||
        kind == CLUX_NODE_LITERAL_NUMERIC ||
        kind == CLUX_NODE_LITERAL_BOOLEAN ||
        kind == CLUX_NODE_LITERAL_IDENTIFIER || kind == CLUX_NODE_EXPR_GROUP ||
        kind == CLUX_NODE_EXPR_ASSIGN || kind == CLUX_NODE_EXPR_TERNARY ||
        kind == CLUX_NODE_EXPR_BINARY || kind == CLUX_NODE_EXPR_SUBSCRIPT ||
        kind == CLUX_NODE_EXPR_SLICE || kind == CLUX_NODE_STMT_VAR;
    if (synthetic_skip) {
      continue;
    }
    node_t *node = make_node(kind, "true");
    clux_value_t *value = exec_node(ctx, node);
    ASSERT_NE(value, nullptr) << "kind " << k << " returned null";
    bool is_unsupported_error =
        clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR &&
        strncmp(error_message_of(value),
                kUnsupportedPrefix,
                kUnsupportedPrefixLen) == 0;
    EXPECT_TRUE(is_unsupported_error)
        << "kind " << k << " should report unsupported, got: "
        << (clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR
                ? error_message_of(value)
                : "<non-error value>");
    delete_node(&node);
  }
}

/* ---- exec_expr_assign：12 种赋值运算符 ---- */

/* 场景: 12 种赋值运算符逐一验证（"=" 与 10 种 compound 数值/位运算）
   预期: "=" 经 safe_cast 覆写目标；compound 经 binary 以当前目标值为左
   操作数计算后写回；assign 返回左值本身且 scope 中的变量同步更新 */
TEST_F(ExecLiteralTest, AssignOperatorMatrix) {
  struct {
    const char *op;
    const char *name;
    int32_t init;
    const char *rhs;
    int32_t expected;
  } cases[] = {
      {"=", "m_plain", 10, "5", 5},
      {"+=", "m_add", 10, "5", 15},
      {"-=", "m_sub", 10, "3", 7},
      {"*=", "m_mul", 6, "7", 42},
      {"/=", "m_div", 42, "6", 7},
      {"%=", "m_mod", 10, "3", 1},
      {"&=", "m_and", 10, "3", 2},
      {"|=", "m_or", 8, "3", 11},
      {"^=", "m_xor", 10, "3", 9},
      {"<<=", "m_shl", 1, "4", 16},
      {">>=", "m_shr", 256, "4", 16},
  };
  for (const auto &c : cases) {
    clux_value_t *target = clux_context_create_i32(ctx, c.init, c.name, NULL);
    ASSERT_NE(target, nullptr) << c.op;
    node_t *node = make_assign(CLUX_NODE_LITERAL_NUMERIC, c.op, c.name, c.rhs);
    clux_value_t *result = exec_expr_assign(ctx, node);
    ASSERT_NE(result, nullptr) << c.op;
    EXPECT_EQ(result, target) << c.op << ": assign returns the target itself";
    EXPECT_EQ(clux_value_get_type(result),
              clux_context_builtin_type(ctx, CLUX_TYPE_I32))
        << c.op;
    EXPECT_EQ(read_i32(result), c.expected) << c.op;
    EXPECT_EQ(read_i32(clux_context_lookup(ctx, c.name, NULL)), c.expected)
        << c.op << ": scope variable must reflect the write";
    delete_node(&node);
  }
}

/* 场景: "=" 对 i64 目标赋 i32 字面量（隐式 widening）
   预期: safe_cast 将 i32 提升为 i64 后写入，值与目标类型均保持 i64
   （右值取 i32 域内 2000000000——无后缀字面量按 i32 解码） */
TEST_F(ExecLiteralTest, AssignWidensI32ValueToI64Target) {
  clux_value_t *target = clux_context_create_i64(ctx, 1, "wl", NULL);
  ASSERT_NE(target, nullptr);
  node_t *node =
      make_assign(CLUX_NODE_LITERAL_NUMERIC, "=", "wl", "2000000000");
  clux_value_t *result = exec_expr_assign(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, target);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  EXPECT_EQ(read_i64(result), 2000000000LL);
  delete_node(&node);
}

/* 场景: 赋值目标左侧的括号分组（含单层与多层嵌套）
   预期: (x) = 5 与 ((x)) = 7 均解包到 identifier x 正常写入，
   不把括号文本当变量名查找 */
TEST_F(ExecLiteralTest, AssignUnwrapsGroupedTarget) {
  clux_value_t *target = clux_context_create_i32(ctx, 10, "gx", NULL);
  ASSERT_NE(target, nullptr);

  node_t *single = create_assign_node(a);
  assign_node_set_left(
      single, make_group_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "gx")));
  assign_node_set_symbol(single, make_node(CLUX_NODE_SYMBOL, "="));
  assign_node_set_value(single, make_node(CLUX_NODE_LITERAL_NUMERIC, "5"));
  clux_value_t *single_result = exec_expr_assign(ctx, single);
  ASSERT_NE(single_result, nullptr);
  EXPECT_EQ(single_result, target);
  EXPECT_EQ(read_i32(target), 5);
  delete_node(&single);

  node_t *nested = create_assign_node(a);
  assign_node_set_left(nested,
                       make_group_of(make_group_of(
                           make_node(CLUX_NODE_LITERAL_IDENTIFIER, "gx"))));
  assign_node_set_symbol(nested, make_node(CLUX_NODE_SYMBOL, "="));
  assign_node_set_value(nested, make_node(CLUX_NODE_LITERAL_NUMERIC, "7"));
  clux_value_t *nested_result = exec_expr_assign(ctx, nested);
  ASSERT_NE(nested_result, nullptr);
  EXPECT_EQ(nested_result, target);
  EXPECT_EQ(read_i32(target), 7);
  delete_node(&nested);
}

/* 场景: 赋值目标未声明 nope_t = 5
   预期: lookup 失败传播 "use of undeclared identifier 'nope_t'" */
TEST_F(ExecLiteralTest, AssignUndefinedTargetPropagatesError) {
  node_t *node = make_assign(CLUX_NODE_LITERAL_NUMERIC, "=", "nope_t", "5");
  clux_value_t *result = exec_expr_assign(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'nope_t'");
  delete_node(&node);
}

/* 场景: 未支持的赋值运算符（"?=" 与多字符前缀陷阱 "**="）
   预期: 返回 "unsupported assign operator '...'"（含运算符原文） */
TEST_F(ExecLiteralTest, AssignUnsupportedOperatorError) {
  struct {
    const char *op;
    const char *name;
  } cases[] = {{"?=", "uop_q"}, {"**=", "uop_star"}};
  for (const auto &c : cases) {
    const char *op = c.op;
    clux_value_t *target = clux_context_create_i32(ctx, 10, c.name, NULL);
    ASSERT_NE(target, nullptr) << op;
    node_t *node = make_assign(CLUX_NODE_LITERAL_NUMERIC, op, c.name, "5");
    clux_value_t *result = exec_expr_assign(ctx, node);
    ASSERT_NE(result, nullptr) << op;
    EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR)
        << op;
    char expected[64];
    snprintf(
        expected, sizeof(expected), "unsupported assign operator '%s'", op);
    EXPECT_STREQ(error_message_of(result), expected) << op;
    delete_node(&node);
  }
}

/* 场景: "=" 右值与目标类型不兼容（str 赋给 i32）
   预期: safe_cast 失败返回 error 值（不写入目标） */
TEST_F(ExecLiteralTest, AssignSafeCastFailurePropagatesError) {
  clux_value_t *target = clux_context_create_i32(ctx, 10, "sc", NULL);
  ASSERT_NE(target, nullptr);
  node_t *node = make_assign(CLUX_NODE_LITERAL_STRING, "=", "sc", "\"s\"");
  clux_value_t *result = exec_expr_assign(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_EQ(read_i32(target), 10) << "failed assign must not write the target";
  delete_node(&node);
}

/* 场景: &&= / ||= 用于非 bool 目标（i32）
   预期: 均拒绝 "type 'i32' does not support operator '&&='/'||='" */
TEST_F(ExecLiteralTest, AssignLogicalOperatorsRequireBoolTarget) {
  struct {
    const char *op;
    const char *name;
  } cases[] = {{"&&=", "nb_and"}, {"||=", "nb_or"}};
  for (const auto &c : cases) {
    const char *op = c.op;
    clux_value_t *target = clux_context_create_i32(ctx, 10, c.name, NULL);
    ASSERT_NE(target, nullptr) << op;
    node_t *node = make_assign(CLUX_NODE_LITERAL_NUMERIC, op, c.name, "1");
    clux_value_t *result = exec_expr_assign(ctx, node);
    ASSERT_NE(result, nullptr) << op;
    EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR)
        << op;
    char expected[64];
    snprintf(expected,
             sizeof(expected),
             "type 'i32' does not support operator '%s'",
             op);
    EXPECT_STREQ(error_message_of(result), expected) << op;
    delete_node(&node);
  }
}

/* 场景: b &&= 短路——b 为 false、右值为超长数字（求值必错）
   预期: 不求值右值直接返回目标：无 error、值仍为 false、返回值即目标 */
TEST_F(ExecLiteralTest, AssignAndShortCircuitsWhenTargetFalse) {
  clux_value_t *target = clux_context_create_bool(ctx, false, "bt", NULL);
  ASSERT_NE(target, nullptr);
  node_t *node =
      make_assign(CLUX_NODE_LITERAL_NUMERIC, "&&=", "bt", kTooLongNumeric);
  clux_value_t *result = exec_expr_assign(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, target);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
  EXPECT_FALSE(read_bool(result));
  delete_node(&node);
}

/* 场景: b ||= 短路——b 为 true、右值为超长数字（求值必错）
   预期: 不求值右值直接返回目标：无 error、值仍为 true */
TEST_F(ExecLiteralTest, AssignOrShortCircuitsWhenTargetTrue) {
  clux_value_t *target = clux_context_create_bool(ctx, true, "bf", NULL);
  ASSERT_NE(target, nullptr);
  node_t *node =
      make_assign(CLUX_NODE_LITERAL_NUMERIC, "||=", "bf", kTooLongNumeric);
  clux_value_t *result = exec_expr_assign(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, target);
  EXPECT_TRUE(read_bool(result));
  delete_node(&node);
}

/* 场景: 求值右值的路径（&&= 左值 true / ||= 左值 false 时不短路）
   预期: 均求值右值并 assign 写回：b=true &&= true → true；
   b=false ||= true → true */
TEST_F(ExecLiteralTest, AssignLogicalEvaluatesRhsWhenNotShortCircuit) {
  clux_value_t *and_target = clux_context_create_bool(ctx, true, "af", NULL);
  ASSERT_NE(and_target, nullptr);
  node_t *and_node =
      make_assign(CLUX_NODE_LITERAL_BOOLEAN, "&&=", "af", "true");
  clux_value_t *and_result = exec_expr_assign(ctx, and_node);
  ASSERT_NE(and_result, nullptr);
  EXPECT_EQ(and_result, and_target);
  EXPECT_TRUE(read_bool(and_result));
  delete_node(&and_node);

  clux_value_t *or_target = clux_context_create_bool(ctx, false, "ot", NULL);
  ASSERT_NE(or_target, nullptr);
  node_t *or_node = make_assign(CLUX_NODE_LITERAL_BOOLEAN, "||=", "ot", "true");
  clux_value_t *or_result = exec_expr_assign(ctx, or_node);
  ASSERT_NE(or_result, nullptr);
  EXPECT_EQ(or_result, or_target);
  EXPECT_TRUE(read_bool(or_result));
  delete_node(&or_node);
}

/* 场景: 求值路径上右值求值出错（b=true 时 b &&= <超长数字>）
   预期: 右值 error 优先传播——"numeric literal too long" */
TEST_F(ExecLiteralTest, AssignLogicalPropagatesRhsError) {
  clux_value_t *target = clux_context_create_bool(ctx, true, "be", NULL);
  ASSERT_NE(target, nullptr);
  node_t *node =
      make_assign(CLUX_NODE_LITERAL_NUMERIC, "&&=", "be", kTooLongNumeric);
  clux_value_t *result = exec_expr_assign(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "numeric literal too long");
  delete_node(&node);
}

/* 场景: 求值路径上右值非 bool（&&= 用 true 目标 / ||= 用 false 目标才进入
   求值路径）
   预期: 类型检查拒绝 "type 'i32' does not support operator '&&='/'||='" */
TEST_F(ExecLiteralTest, AssignLogicalRejectsNonBoolRhs) {
  struct {
    const char *op;
    const char *name;
    bool init; /* 进入求值路径所需的左值 */
  } cases[] = {
      {"&&=", "bn_and", true},
      {"||=", "bn_or", false},
  };
  for (const auto &c : cases) {
    clux_value_t *target = clux_context_create_bool(ctx, c.init, c.name, NULL);
    ASSERT_NE(target, nullptr) << c.op;
    node_t *node = make_assign(CLUX_NODE_LITERAL_NUMERIC, c.op, c.name, "5");
    clux_value_t *result = exec_expr_assign(ctx, node);
    ASSERT_NE(result, nullptr) << c.op;
    ASSERT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR)
        << c.op << ": expected an error value for non-bool rhs";
    char expected[64];
    snprintf(expected,
             sizeof(expected),
             "type 'i32' does not support operator '%s'",
             c.op);
    EXPECT_STREQ(error_message_of(result), expected) << c.op;
    delete_node(&node);
  }
}

/* 场景: exec 禁止 shadow 值——shadow 目标/右值/初值一律拒绝
   预期: shadow bool 变量做 &&= 目标、shadow 右值、var 声明 shadow 初值
         均返回 error "shadow value is not allowed in execution" */
TEST_F(ExecLiteralTest, ExecRejectsShadowValues) {
  const char *kShadowMsg = "shadow value is not allowed in execution";

  /* shadow 赋值目标（lookup 结果经 declare 具名化的 shadow） */
  clux_type_t *bool_type = clux_context_builtin_type(ctx, CLUX_TYPE_BOOL);
  clux_value_t *shadow = clux_context_create_shadow_value(ctx, bool_type);
  ASSERT_NE(shadow, nullptr);
  clux_value_t *declared = clux_context_declare_value(ctx, shadow, "sb", NULL);
  ASSERT_EQ(declared, shadow);

  node_t *node = make_assign(CLUX_NODE_LITERAL_BOOLEAN, "&&=", "sb", "true");
  clux_value_t *result = exec_expr_assign(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), kShadowMsg);
  delete_node(&node);

  /* var 声明 shadow 初值：真实右值路径上不可合成 shadow 字面量，
     改用声明 shadow 后的具名引用作初值（identifier lookup 返回 shadow） */
  node_t *var = make_stmt_var("sv",
                              CLUX_NODE_LITERAL_IDENTIFIER,
                              NULL,
                              CLUX_NODE_LITERAL_IDENTIFIER,
                              "sb");
  clux_value_t *var_result = exec_stmt_var(ctx, var);
  ASSERT_NE(var_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(var_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(var_result), kShadowMsg);
  delete_node(&var);
}

/* 场景: compound 运算符右值求值出错（x += <超长数字>）
   预期: 右值 error 在 binary 前传播——"numeric literal too long" */
TEST_F(ExecLiteralTest, AssignCompoundPropagatesRhsError) {
  clux_value_t *target = clux_context_create_i32(ctx, 10, "ce", NULL);
  ASSERT_NE(target, nullptr);
  node_t *node =
      make_assign(CLUX_NODE_LITERAL_NUMERIC, "+=", "ce", kTooLongNumeric);
  clux_value_t *result = exec_expr_assign(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "numeric literal too long");
  delete_node(&node);
}

/* 场景: compound binary 对不兼容操作数报错（i32 += str），经 assign 传播
   预期: 返回 error 值且目标值保持不变 */
TEST_F(ExecLiteralTest, AssignCompoundBinaryErrorPropagates) {
  clux_value_t *target = clux_context_create_i32(ctx, 10, "cs", NULL);
  ASSERT_NE(target, nullptr);
  node_t *node = make_assign(CLUX_NODE_LITERAL_STRING, "+=", "cs", "\"s\"");
  clux_value_t *result = exec_expr_assign(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_EQ(read_i32(target), 10)
      << "failed compound must not write the target";
  delete_node(&node);
}

/* ---- exec_expr_group：括号分组求值 ---- */

/* 场景: 括号分组包裹数字字面量
   预期: 解包执行子表达式，产出 42（与直接求值一致） */
TEST_F(ExecLiteralTest, GroupEvaluatesWrappedExpression) {
  node_t *node = make_group_of(make_node(CLUX_NODE_LITERAL_NUMERIC, "42"));
  clux_value_t *result = exec_expr_group(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(result), 42);
  delete_node(&node);
}

/* 场景: 括号分组内缺表达式（容错 NULL 子结点）
   预期: 委托 exec_node(NULL) 报 "execution requires a node" */
TEST_F(ExecLiteralTest, GroupNullExprRequiresNode) {
  node_t *node = make_group_of(nullptr);
  clux_value_t *result = exec_expr_group(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "execution requires a node");
  delete_node(&node);
}

/* ---- exec_node：空结点防御 ---- */

/* 场景: exec_node 传入 NULL
   预期: 返回 error 值，消息 "execution requires a node" */
TEST_F(ExecLiteralTest, ExecNodeNullRequiresNode) {
  clux_value_t *result = exec_node(ctx, NULL);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "execution requires a node");
}

/* ---- exec_stmt_var：var 声明语句 ---- */

/* 场景: var vx = 10 省略类型标注
   预期: 按初值自身类型 i32 落地，declare_value 具名化，lookup 可见同一值 */
TEST_F(ExecLiteralTest, VarInfersTypeFromValueWithoutAnnotation) {
  node_t *node = make_stmt_var(
      "vx", CLUX_NODE_SYMBOL, NULL, CLUX_NODE_LITERAL_NUMERIC, "10");
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(result), 10);
  EXPECT_EQ(clux_context_lookup(ctx, "vx", NULL), result);
  delete_node(&node);
}

/* 场景: var vy: i64 = 5 带类型标注
   预期: 类型表达式求值为 i64 type value，safe_cast 将 i32 初值提升为 i64 */
TEST_F(ExecLiteralTest, VarWithTypeAnnotationCastsInitialValue) {
  node_t *node = make_stmt_var("vy",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               "i64",
                               CLUX_NODE_LITERAL_NUMERIC,
                               "5");
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  EXPECT_EQ(read_i64(result), 5);
  delete_node(&node);
}

/* 场景: 类型标注不是类型表达式（var vt: 10 = 5，数字字面量当类型）
   预期: 报 "expected type expression in var declaration" */
TEST_F(ExecLiteralTest, VarAnnotationMustBeTypeExpression) {
  node_t *node = make_stmt_var(
      "vt", CLUX_NODE_LITERAL_NUMERIC, "10", CLUX_NODE_LITERAL_NUMERIC, "5");
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "expected type expression in var declaration");
  delete_node(&node);
}

/* 场景: 类型表达式产出无载荷的 type 值（data 为 0——即 shadow 值）
   预期: exec 禁止 shadow，报 "shadow value is not allowed in execution"
         （shadow 检查在 kind/raw 检查之前命中） */
TEST_F(ExecLiteralTest, VarAnnotationShadowTypeValueErrors) {
  clux_value_t *empty_type = clux_context_create_value(
      ctx, clux_context_get_type_type(ctx), 0, "empty_type", NULL);
  ASSERT_NE(empty_type, nullptr);
  node_t *node = make_stmt_var("vq",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               "empty_type",
                               CLUX_NODE_LITERAL_NUMERIC,
                               "5");
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "shadow value is not allowed in execution");
  delete_node(&node);
}

/* 场景: 初值求值出错（var ve = <超长数字>）
   预期: 初值 error 优先于类型处理传播——"numeric literal too long" */
TEST_F(ExecLiteralTest, VarPropagatesInitialValueError) {
  node_t *node = make_stmt_var(
      "ve", CLUX_NODE_SYMBOL, NULL, CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric);
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "numeric literal too long");
  delete_node(&node);
}

/* 场景: 类型表达式求值出错（var vn: nope = 5，类型名未声明）
   预期: 类型 error 优先于 safe_cast 传播——"use of undeclared identifier
   'nope'" */
TEST_F(ExecLiteralTest, VarPropagatesTypeErrorBeforeCast) {
  node_t *node = make_stmt_var("vn",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               "nope",
                               CLUX_NODE_LITERAL_NUMERIC,
                               "5");
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "use of undeclared identifier 'nope'");
  delete_node(&node);
}

/* 场景: 类型兼容性检查失败（var vc: i32 = "str"，str 初值配 i32 标注）
   预期: safe_cast 失败返回 error 值，不落地声明 */
TEST_F(ExecLiteralTest, VarIncompatibleCastPropagatesError) {
  node_t *node = make_stmt_var("vc",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               "i32",
                               CLUX_NODE_LITERAL_STRING,
                               "\"str\"");
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  clux_value_t *missing = clux_context_lookup(ctx, "vc", NULL);
  ASSERT_NE(missing, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(missing)), CLUX_TYPE_ERROR)
      << "failed cast must not declare 'vc'";
  EXPECT_STREQ(error_message_of(missing), "use of undeclared identifier 'vc'");
  delete_node(&node);
}

/* 场景: 重复声明（var vd = 1 后再次 var vd = 2）
   预期: 第二次 declare_value 报 "duplicate declaration 'vd'" */
TEST_F(ExecLiteralTest, VarDuplicateDeclarationErrors) {
  node_t *first = make_stmt_var(
      "vd", CLUX_NODE_SYMBOL, NULL, CLUX_NODE_LITERAL_NUMERIC, "1");
  clux_value_t *first_result = exec_stmt_var(ctx, first);
  ASSERT_NE(first_result, nullptr);
  EXPECT_EQ(read_i32(first_result), 1);
  delete_node(&first);

  node_t *second = make_stmt_var(
      "vd", CLUX_NODE_SYMBOL, NULL, CLUX_NODE_LITERAL_NUMERIC, "2");
  clux_value_t *second_result = exec_stmt_var(ctx, second);
  ASSERT_NE(second_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(second_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(second_result), "duplicate declaration 'vd'");
  EXPECT_EQ(read_i32(first_result), 1)
      << "duplicate must not clobber the first";
  delete_node(&second);
}

/* 合成三元条件表达式结点 cond ? then : else（三个子结点由调用方构造
   并保证齐全——合成节点须符合 AST 契约；delete 根结点时一并释放）。 */
static node_t *make_ternary_of(allocator_t *allocator,
                               node_t *condition,
                               node_t *then,
                               node_t *else_) {
  node_t *node = create_ternary_node(allocator);
  ternary_node_set_condition(node, condition);
  ternary_node_set_then(node, then);
  ternary_node_set_else(node, else_);
  return node;
}

/* ---- exec_expr_ternary：三元条件表达式 ---- */

/* 场景: cond 为 true——只求值 then 分支，else 分支为超长数字（求值必错）
   预期: 惰性求值选中 then 产出 i32 7，未选中的 else 绝不求值 */
TEST_F(ExecLiteralTest, TernaryTrueConditionSelectsThenLazily) {
  node_t *node =
      make_ternary_of(a,
                      make_node(CLUX_NODE_LITERAL_BOOLEAN, "true"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "7"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric));
  clux_value_t *result = exec_expr_ternary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(result), 7);
  delete_node(&node);
}

/* 场景: cond 为 false——只求值 else 分支，then 分支为超长数字（求值必错）
   预期: 惰性求值选中 else 产出 i32 9，未选中的 then 绝不求值 */
TEST_F(ExecLiteralTest, TernaryFalseConditionSelectsElseLazily) {
  node_t *node =
      make_ternary_of(a,
                      make_node(CLUX_NODE_LITERAL_BOOLEAN, "false"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "9"));
  clux_value_t *result = exec_expr_ternary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(result), 9);
  delete_node(&node);
}

/* 场景: 非 bool 条件（i32 数字与 str 字符串）
   预期: 拒绝 "type 'i32'/'str' does not support operator '?:'" */
TEST_F(ExecLiteralTest, TernaryRejectsNonBoolCondition) {
  struct {
    node_kind_t kind;
    const char *text;
    const char *type_name;
  } cases[] = {
      {CLUX_NODE_LITERAL_NUMERIC, "5", "i32"},
      {CLUX_NODE_LITERAL_STRING, "\"s\"", "str"},
  };
  for (const auto &c : cases) {
    node_t *node = make_ternary_of(a,
                                   make_node(c.kind, c.text),
                                   make_node(CLUX_NODE_LITERAL_NUMERIC, "7"),
                                   make_node(CLUX_NODE_LITERAL_NUMERIC, "9"));
    clux_value_t *result = exec_expr_ternary(ctx, node);
    ASSERT_NE(result, nullptr) << c.type_name;
    ASSERT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR)
        << c.type_name << ": expected an error value for non-bool condition";
    char expected[64];
    snprintf(expected,
             sizeof(expected),
             "type '%s' does not support operator '?:'",
             c.type_name);
    EXPECT_STREQ(error_message_of(result), expected) << c.type_name;
    delete_node(&node);
  }
}

/* 场景: 条件求值出错（cond = 超长数字）
   预期: 条件 error 最先传播——"numeric literal too long" */
TEST_F(ExecLiteralTest, TernaryPropagatesConditionError) {
  node_t *node =
      make_ternary_of(a,
                      make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "7"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "9"));
  clux_value_t *result = exec_expr_ternary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "numeric literal too long");
  delete_node(&node);
}

/* 场景: exec 禁止 shadow 值——shadow 条件 / shadow then / shadow else
   预期: 三者均拒绝 "shadow value is not allowed in execution" */
TEST_F(ExecLiteralTest, TernaryRejectsShadowValues) {
  const char *kShadowMsg = "shadow value is not allowed in execution";
  clux_type_t *bool_type = clux_context_builtin_type(ctx, CLUX_TYPE_BOOL);
  clux_value_t *shadow = clux_context_create_shadow_value(ctx, bool_type);
  ASSERT_NE(shadow, nullptr);
  clux_value_t *declared = clux_context_declare_value(ctx, shadow, "tsb", NULL);
  ASSERT_EQ(declared, shadow);

  /* shadow 条件：identifier 求值返回 shadow */
  node_t *cond_node =
      make_ternary_of(a,
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tsb"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "7"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "9"));
  clux_value_t *cond_result = exec_expr_ternary(ctx, cond_node);
  ASSERT_NE(cond_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(cond_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(cond_result), kShadowMsg);
  delete_node(&cond_node);

  /* shadow then 分支：cond 为 true 时选中 */
  node_t *then_node =
      make_ternary_of(a,
                      make_node(CLUX_NODE_LITERAL_BOOLEAN, "true"),
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tsb"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "9"));
  clux_value_t *then_result = exec_expr_ternary(ctx, then_node);
  ASSERT_NE(then_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(then_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(then_result), kShadowMsg);
  delete_node(&then_node);

  /* shadow else 分支：cond 为 false 时选中 */
  node_t *else_node =
      make_ternary_of(a,
                      make_node(CLUX_NODE_LITERAL_BOOLEAN, "false"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "7"),
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tsb"));
  clux_value_t *else_result = exec_expr_ternary(ctx, else_node);
  ASSERT_NE(else_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(else_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(else_result), kShadowMsg);
  delete_node(&else_node);
}

/* 场景: 选中分支求值出错（cond 为 true，then = 超长数字）
   预期: 分支 error 自然传播——"numeric literal too long" */
TEST_F(ExecLiteralTest, TernaryPropagatesSelectedBranchError) {
  node_t *node =
      make_ternary_of(a,
                      make_node(CLUX_NODE_LITERAL_BOOLEAN, "true"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "9"));
  clux_value_t *result = exec_expr_ternary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "numeric literal too long");
  delete_node(&node);
}

/* 场景: exec_node 按 CLUX_NODE_EXPR_TERNARY 分派到 exec_expr_ternary
   预期: cond 为 false 时惰性跳过 then（超长探针），经 exec_node 产出
         else 分支的 str 值 "ok" */
TEST_F(ExecLiteralTest, TernaryDispatchesViaExecNode) {
  node_t *node =
      make_ternary_of(a,
                      make_node(CLUX_NODE_LITERAL_BOOLEAN, "false"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric),
                      make_node(CLUX_NODE_LITERAL_STRING, "\"ok\""));
  clux_value_t *result = exec_node(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_STR));
  EXPECT_STREQ(read_str(result), "ok");
  delete_node(&node);
}

/* 合成二元表达式结点 left op right（三个子结点由调用方构造并保证齐全；
   前缀一元形态传 left = NULL；delete 根结点时一并释放）。 */
static node_t *make_binary_of(allocator_t *allocator,
                              node_t *left,
                              node_t *symbol,
                              node_t *right) {
  node_t *node = create_binary_node(allocator);
  binary_node_set_left(node, left);
  binary_node_set_symbol(node, symbol);
  binary_node_set_right(node, right);
  return node;
}

/* ---- exec_expr_binary：三元形态（前缀 / 短路 / 二元 / 未识别） ---- */

/* 场景: 16 种二元运算符逐一验证（算术/位/移位产 i32，比较产 bool）
   预期: 经 clux_context_binary 按运算符计算，结果类型与值精确匹配
         （比较类结果类型为 bool，其余为 i32） */
TEST_F(ExecLiteralTest, BinaryOperatorMatrix) {
  struct {
    const char *op;
    const char *lhs;
    const char *rhs;
    bool result_is_bool;
    int64_t expected;
  } cases[] = {
      {"+", "10", "3", false, 13},
      {"-", "10", "3", false, 7},
      {"*", "6", "7", false, 42},
      {"/", "42", "6", false, 7},
      {"%", "10", "3", false, 1},
      {"&", "10", "3", false, 2},
      {"|", "8", "3", false, 11},
      {"^", "10", "3", false, 9},
      {"<<", "1", "4", false, 16},
      {">>", "256", "4", false, 16},
      {"==", "10", "3", true, 0},
      {"!=", "10", "3", true, 1},
      {"<", "10", "3", true, 0},
      {"<=", "10", "10", true, 1},
      {">", "10", "3", true, 1},
      {">=", "10", "10", true, 1},
  };
  for (const auto &c : cases) {
    node_t *node = make_binary_of(a,
                                  make_node(CLUX_NODE_LITERAL_NUMERIC, c.lhs),
                                  make_node(CLUX_NODE_SYMBOL, c.op),
                                  make_node(CLUX_NODE_LITERAL_NUMERIC, c.rhs));
    clux_value_t *result = exec_expr_binary(ctx, node);
    ASSERT_NE(result, nullptr) << c.op;
    EXPECT_EQ(clux_value_get_type(result),
              clux_context_builtin_type(
                  ctx, c.result_is_bool ? CLUX_TYPE_BOOL : CLUX_TYPE_I32))
        << c.op;
    if (c.result_is_bool) {
      EXPECT_EQ(read_bool(result), (bool)c.expected) << c.op;
    } else {
      EXPECT_EQ(read_i32(result), (int32_t)c.expected) << c.op;
    }
    delete_node(&node);
  }
}

/* 场景: i64 与 u32 宽度上的二元运算（带后缀字面量）
   预期: 引擎按操作数类型分派，i64 减法与 u32 除法结果类型、值均正确 */
TEST_F(ExecLiteralTest, BinaryOperatorsAcrossIntegerWidths) {
  node_t *i64_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "20i64"),
                     make_node(CLUX_NODE_SYMBOL, "-"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "8i64"));
  clux_value_t *i64_result = exec_expr_binary(ctx, i64_node);
  ASSERT_NE(i64_result, nullptr);
  EXPECT_EQ(clux_value_get_type(i64_result),
            clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  EXPECT_EQ(read_i64(i64_result), 12);
  delete_node(&i64_node);

  node_t *u32_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "20u32"),
                     make_node(CLUX_NODE_SYMBOL, "/"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "3u32"));
  clux_value_t *u32_result = exec_expr_binary(ctx, u32_node);
  ASSERT_NE(u32_result, nullptr);
  EXPECT_EQ(clux_value_get_type(u32_result),
            clux_context_builtin_type(ctx, CLUX_TYPE_U32));
  EXPECT_EQ(read_u32(u32_result), 6u);
  delete_node(&u32_node);
}

/* 场景: 前缀一元 4 运算符（+ - ~ 作用于整数，! 作用于 bool）
   预期: LEFT 为 NULL 时按 k_prefix_ops 分派引擎 prefix：
         +5→5、-5→-5、~5→-6（按位取反）、!true→false、!false→true */
TEST_F(ExecLiteralTest, PrefixOperatorMatrix) {
  struct {
    const char *op;
    const char *operand;
    node_kind_t operand_kind;
    bool result_is_bool;
    int64_t expected;
  } cases[] = {
      {"+", "5", CLUX_NODE_LITERAL_NUMERIC, false, 5},
      {"-", "5", CLUX_NODE_LITERAL_NUMERIC, false, -5},
      {"~", "5", CLUX_NODE_LITERAL_NUMERIC, false, -6},
      {"!", "true", CLUX_NODE_LITERAL_BOOLEAN, true, 0},
      {"!", "false", CLUX_NODE_LITERAL_BOOLEAN, true, 1},
  };
  for (const auto &c : cases) {
    node_t *node = make_binary_of(a,
                                  NULL,
                                  make_node(CLUX_NODE_SYMBOL, c.op),
                                  make_node(c.operand_kind, c.operand));
    clux_value_t *result = exec_expr_binary(ctx, node);
    ASSERT_NE(result, nullptr) << c.op;
    EXPECT_EQ(clux_value_get_type(result),
              clux_context_builtin_type(
                  ctx, c.result_is_bool ? CLUX_TYPE_BOOL : CLUX_TYPE_I32))
        << c.op;
    if (c.result_is_bool) {
      EXPECT_EQ(read_bool(result), (bool)c.expected) << c.op;
    } else {
      EXPECT_EQ(read_i32(result), (int32_t)c.expected) << c.op;
    }
    delete_node(&node);
  }
}

/* 场景: 前缀一元右操作数求值出错（- <超长数字>）
   预期: 右值经 exec_operand 求值，error 优先传播——
         "numeric literal too long" */
TEST_F(ExecLiteralTest, PrefixPropagatesOperandError) {
  node_t *node =
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "-"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "numeric literal too long");
  delete_node(&node);
}

/* 场景: 前缀一元右操作数为 shadow 值（具名 shadow 变量的 identifier 引用）
   预期: exec_operand 拒绝——"shadow value is not allowed in execution" */
TEST_F(ExecLiteralTest, PrefixRejectsShadowOperand) {
  clux_type_t *bool_type = clux_context_builtin_type(ctx, CLUX_TYPE_BOOL);
  clux_value_t *shadow = clux_context_create_shadow_value(ctx, bool_type);
  ASSERT_NE(shadow, nullptr);
  clux_value_t *declared = clux_context_declare_value(ctx, shadow, "psh", NULL);
  ASSERT_EQ(declared, shadow);

  node_t *node = make_binary_of(a,
                                NULL,
                                make_node(CLUX_NODE_SYMBOL, "!"),
                                make_node(CLUX_NODE_LITERAL_IDENTIFIER, "psh"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "shadow value is not allowed in execution");
  delete_node(&node);
}

/* 场景: 未识别的一元前缀符号（"++" 与 ".&"，不在 k_prefix_ops 表中）
   预期: 报 "unsupported unary operator '<原文>'" */
TEST_F(ExecLiteralTest, PrefixUnsupportedOperatorError) {
  const char *ops[] = {"++", ".&"};
  for (const char *op : ops) {
    node_t *node = make_binary_of(a,
                                  NULL,
                                  make_node(CLUX_NODE_SYMBOL, op),
                                  make_node(CLUX_NODE_LITERAL_NUMERIC, "5"));
    clux_value_t *result = exec_expr_binary(ctx, node);
    ASSERT_NE(result, nullptr) << op;
    ASSERT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR)
        << op << ": expected an error value for unknown prefix operator";
    char expected[64];
    snprintf(expected, sizeof(expected), "unsupported unary operator '%s'", op);
    EXPECT_STREQ(error_message_of(result), expected) << op;
    delete_node(&node);
  }
}

/* 场景: && 短路——左值为 false、右值为超长数字（求值必错）
   预期: 不求值右值直接返回左值本身：result 即 scope 中的左值指针、
         类型 bool、值为 false，无 error */
TEST_F(ExecLiteralTest, LogicalAndShortCircuitsWhenLeftFalse) {
  clux_value_t *left = clux_context_create_bool(ctx, false, "laf", NULL);
  ASSERT_NE(left, nullptr);
  node_t *node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "laf"),
                     make_node(CLUX_NODE_SYMBOL, "&&"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, left) << "short-circuit must return the left value itself";
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
  EXPECT_FALSE(read_bool(result));
  delete_node(&node);
}

/* 场景: || 短路——左值为 true、右值为超长数字（求值必错）
   预期: 不求值右值直接返回左值本身：result 即左值指针、值为 true */
TEST_F(ExecLiteralTest, LogicalOrShortCircuitsWhenLeftTrue) {
  clux_value_t *left = clux_context_create_bool(ctx, true, "lot", NULL);
  ASSERT_NE(left, nullptr);
  node_t *node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "lot"),
                     make_node(CLUX_NODE_SYMBOL, "||"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, left) << "short-circuit must return the left value itself";
  EXPECT_TRUE(read_bool(result));
  delete_node(&node);
}

/* 场景: 非短路时求值右值（true && 右 / false || 右）
   预期: 返回右值本身（scope 中的右值指针）：true&&false→false、
         false||true→true */
TEST_F(ExecLiteralTest, LogicalEvaluatesRightWhenNotShortCircuit) {
  clux_value_t *and_right = clux_context_create_bool(ctx, false, "raf", NULL);
  ASSERT_NE(and_right, nullptr);
  node_t *and_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_BOOLEAN, "true"),
                     make_node(CLUX_NODE_SYMBOL, "&&"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "raf"));
  clux_value_t *and_result = exec_expr_binary(ctx, and_node);
  ASSERT_NE(and_result, nullptr);
  EXPECT_EQ(and_result, and_right)
      << "non-short-circuit must return the right value itself";
  EXPECT_FALSE(read_bool(and_result));
  delete_node(&and_node);

  clux_value_t *or_right = clux_context_create_bool(ctx, true, "rot", NULL);
  ASSERT_NE(or_right, nullptr);
  node_t *or_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_BOOLEAN, "false"),
                     make_node(CLUX_NODE_SYMBOL, "||"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "rot"));
  clux_value_t *or_result = exec_expr_binary(ctx, or_node);
  ASSERT_NE(or_result, nullptr);
  EXPECT_EQ(or_result, or_right)
      << "non-short-circuit must return the right value itself";
  EXPECT_TRUE(read_bool(or_result));
  delete_node(&or_node);
}

/* 场景: &&/|| 的左操作数非 bool（i32 数字与 str 字符串，表驱动）
   预期: 拒绝 "type 'i32'/'str' does not support operator '&&'/'||'" */
TEST_F(ExecLiteralTest, LogicalRejectsNonBoolLeftOperand) {
  struct {
    node_kind_t kind;
    const char *text;
    const char *type_name;
  } operands[] = {
      {CLUX_NODE_LITERAL_NUMERIC, "5", "i32"},
      {CLUX_NODE_LITERAL_STRING, "\"s\"", "str"},
  };
  const char *ops[] = {"&&", "||"};
  for (const char *op : ops) {
    for (const auto &c : operands) {
      node_t *node =
          make_binary_of(a,
                         make_node(c.kind, c.text),
                         make_node(CLUX_NODE_SYMBOL, op),
                         make_node(CLUX_NODE_LITERAL_BOOLEAN, "true"));
      clux_value_t *result = exec_expr_binary(ctx, node);
      ASSERT_NE(result, nullptr) << op << " " << c.type_name;
      ASSERT_EQ(clux_type_get_kind(clux_value_get_type(result)),
                CLUX_TYPE_ERROR)
          << op << " " << c.type_name
          << ": expected an error value for non-bool left operand";
      char expected[64];
      snprintf(expected,
               sizeof(expected),
               "type '%s' does not support operator '%s'",
               c.type_name,
               op);
      EXPECT_STREQ(error_message_of(result), expected)
          << op << " " << c.type_name;
      delete_node(&node);
    }
  }
}

/* 场景: &&/|| 求值右操作数时非 bool（true&&非bool / false||非bool，
   i32 与 str 表驱动）
   预期: 拒绝 "type 'i32'/'str' does not support operator '&&'/'||'" */
TEST_F(ExecLiteralTest, LogicalRejectsNonBoolRightOperand) {
  struct {
    node_kind_t kind;
    const char *text;
    const char *type_name;
  } operands[] = {
      {CLUX_NODE_LITERAL_NUMERIC, "5", "i32"},
      {CLUX_NODE_LITERAL_STRING, "\"s\"", "str"},
  };
  struct {
    const char *op;
    const char *left_text;
  } paths[] = {
      {"&&", "true"},
      {"||", "false"},
  };
  for (const auto &p : paths) {
    for (const auto &c : operands) {
      node_t *node =
          make_binary_of(a,
                         make_node(CLUX_NODE_LITERAL_BOOLEAN, p.left_text),
                         make_node(CLUX_NODE_SYMBOL, p.op),
                         make_node(c.kind, c.text));
      clux_value_t *result = exec_expr_binary(ctx, node);
      ASSERT_NE(result, nullptr) << p.op << " " << c.type_name;
      ASSERT_EQ(clux_type_get_kind(clux_value_get_type(result)),
                CLUX_TYPE_ERROR)
          << p.op << " " << c.type_name
          << ": expected an error value for non-bool right operand";
      char expected[64];
      snprintf(expected,
               sizeof(expected),
               "type '%s' does not support operator '%s'",
               c.type_name,
               p.op);
      EXPECT_STREQ(error_message_of(result), expected)
          << p.op << " " << c.type_name;
      delete_node(&node);
    }
  }
}

/* 场景: &&/|| 操作数求值出错（左值为超长数字；或非短路路径上右值为
   超长数字）
   预期: error 优先传播——"numeric literal too long" */
TEST_F(ExecLiteralTest, LogicalPropagatesOperandError) {
  /* 左值 error：不求值右值直接返回 */
  node_t *left_err_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric),
                     make_node(CLUX_NODE_SYMBOL, "&&"),
                     make_node(CLUX_NODE_LITERAL_BOOLEAN, "true"));
  clux_value_t *left_err_result = exec_expr_binary(ctx, left_err_node);
  ASSERT_NE(left_err_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(left_err_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(left_err_result), "numeric literal too long");
  delete_node(&left_err_node);

  /* 右值 error：true && <超长数字> 不短路，右值 error 传播 */
  node_t *right_err_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_BOOLEAN, "true"),
                     make_node(CLUX_NODE_SYMBOL, "&&"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric));
  clux_value_t *right_err_result = exec_expr_binary(ctx, right_err_node);
  ASSERT_NE(right_err_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(right_err_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(right_err_result), "numeric literal too long");
  delete_node(&right_err_node);
}

/* 场景: &&/|| 操作数为 shadow 值（具名 shadow 变量的 identifier 引用）
   预期: 左/右操作数均拒绝——"shadow value is not allowed in execution" */
TEST_F(ExecLiteralTest, LogicalRejectsShadowOperands) {
  const char *kShadowMsg = "shadow value is not allowed in execution";
  clux_type_t *bool_type = clux_context_builtin_type(ctx, CLUX_TYPE_BOOL);
  clux_value_t *shadow = clux_context_create_shadow_value(ctx, bool_type);
  ASSERT_NE(shadow, nullptr);
  clux_value_t *declared = clux_context_declare_value(ctx, shadow, "bsh", NULL);
  ASSERT_EQ(declared, shadow);

  /* shadow 左操作数 */
  node_t *left_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "bsh"),
                     make_node(CLUX_NODE_SYMBOL, "&&"),
                     make_node(CLUX_NODE_LITERAL_BOOLEAN, "true"));
  clux_value_t *left_result = exec_expr_binary(ctx, left_node);
  ASSERT_NE(left_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(left_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(left_result), kShadowMsg);
  delete_node(&left_node);

  /* shadow 右操作数：true && shadow 不短路进入右值求值 */
  node_t *right_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_BOOLEAN, "true"),
                     make_node(CLUX_NODE_SYMBOL, "&&"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "bsh"));
  clux_value_t *right_result = exec_expr_binary(ctx, right_node);
  ASSERT_NE(right_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(right_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(right_result), kShadowMsg);
  delete_node(&right_node);
}

/* 场景: 二元形态操作数求值出错（+ 的左/右值为超长数字）
   预期: 任一操作数 error 优先传播——"numeric literal too long" */
TEST_F(ExecLiteralTest, BinaryPropagatesOperandError) {
  /* 左值 error */
  node_t *left_err_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric),
                     make_node(CLUX_NODE_SYMBOL, "+"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "3"));
  clux_value_t *left_err_result = exec_expr_binary(ctx, left_err_node);
  ASSERT_NE(left_err_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(left_err_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(left_err_result), "numeric literal too long");
  delete_node(&left_err_node);

  /* 右值 error */
  node_t *right_err_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "5"),
                     make_node(CLUX_NODE_SYMBOL, "+"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric));
  clux_value_t *right_err_result = exec_expr_binary(ctx, right_err_node);
  ASSERT_NE(right_err_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(right_err_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(right_err_result), "numeric literal too long");
  delete_node(&right_err_node);
}

/* 场景: 二元形态操作数为 shadow 值（+ 的左/右为具名 shadow 引用）
   预期: 均拒绝——"shadow value is not allowed in execution" */
TEST_F(ExecLiteralTest, BinaryRejectsShadowOperands) {
  const char *kShadowMsg = "shadow value is not allowed in execution";
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_value_t *shadow = clux_context_create_shadow_value(ctx, i32_type);
  ASSERT_NE(shadow, nullptr);
  clux_value_t *declared = clux_context_declare_value(ctx, shadow, "nsh", NULL);
  ASSERT_EQ(declared, shadow);

  node_t *left_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nsh"),
                     make_node(CLUX_NODE_SYMBOL, "+"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "3"));
  clux_value_t *left_result = exec_expr_binary(ctx, left_node);
  ASSERT_NE(left_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(left_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(left_result), kShadowMsg);
  delete_node(&left_node);

  node_t *right_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "5"),
                     make_node(CLUX_NODE_SYMBOL, "+"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nsh"));
  clux_value_t *right_result = exec_expr_binary(ctx, right_node);
  ASSERT_NE(right_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(right_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(right_result), kShadowMsg);
  delete_node(&right_node);
}

/* 场景: 未识别的二元符号（后缀形态与成员访问，本批未实现）
   预期: 报 "unsupported binary operator '<原文>'"（含 ".&" ".*" ".!" ".?"
         "." "::" "++"） */
TEST_F(ExecLiteralTest, BinaryUnsupportedOperatorError) {
  const char *ops[] = {".&", ".*", ".!", ".?", ".", "::", "++"};
  for (const char *op : ops) {
    node_t *node = make_binary_of(a,
                                  make_node(CLUX_NODE_LITERAL_NUMERIC, "5"),
                                  make_node(CLUX_NODE_SYMBOL, op),
                                  make_node(CLUX_NODE_LITERAL_NUMERIC, "3"));
    clux_value_t *result = exec_expr_binary(ctx, node);
    ASSERT_NE(result, nullptr) << op;
    ASSERT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR)
        << op << ": expected an error value for unknown binary operator";
    char expected[64];
    snprintf(
        expected, sizeof(expected), "unsupported binary operator '%s'", op);
    EXPECT_STREQ(error_message_of(result), expected) << op;
    delete_node(&node);
  }
}

/* 场景: exec_node 按 CLUX_NODE_EXPR_BINARY 分派到 exec_expr_binary
   预期: 2 + 3 经 exec_node 分派产出 i32 5 */
TEST_F(ExecLiteralTest, BinaryDispatchesViaExecNode) {
  node_t *node = make_binary_of(a,
                                make_node(CLUX_NODE_LITERAL_NUMERIC, "2"),
                                make_node(CLUX_NODE_SYMBOL, "+"),
                                make_node(CLUX_NODE_LITERAL_NUMERIC, "3"));
  clux_value_t *result = exec_node(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(result), 5);
  delete_node(&node);
}

/* ---- TDZ（temporal dead zone）：exec_operand 统一消费通道 ---- */

/* 场景: exec_operand 消费 TDZ identifier
   预期: 返回 error 值，消息精确为 "use of a value before initialization" */
TEST_F(ExecLiteralTest, ExecOperandRejectsTdzIdentifier) {
  clux_value_t *x = clux_context_create_i32(ctx, 1, "tz_x", NULL);
  ASSERT_NE(x, nullptr);
  clux_value_set_tdz(x, true);

  node_t *node = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tz_x");
  clux_value_t *value = exec_operand(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "use of a value before initialization");
  delete_node(&node);
}

/* 场景: 二元表达式左右操作数均为 TDZ 变量（消费位置覆盖）
   预期: exec_operand 在 binary 通道中拒绝，消息为 TDZ 精确错误 */
TEST_F(ExecLiteralTest, BinaryConsumesTdzOperandsViaExecOperand) {
  clux_value_t *tzv = clux_context_create_i32(ctx, 1, "tz_bv", NULL);
  ASSERT_NE(tzv, nullptr);
  clux_value_set_tdz(tzv, true);

  node_t *left_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tz_bv"),
                     make_node(CLUX_NODE_SYMBOL, "+"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "3"));
  clux_value_t *left_result = exec_expr_binary(ctx, left_node);
  ASSERT_NE(left_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(left_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(left_result),
               "use of a value before initialization");
  delete_node(&left_node);

  node_t *right_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "5"),
                     make_node(CLUX_NODE_SYMBOL, "+"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tz_bv"));
  clux_value_t *right_result = exec_expr_binary(ctx, right_node);
  ASSERT_NE(right_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(right_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(right_result),
               "use of a value before initialization");
  delete_node(&right_node);
}

/* 场景: 前缀一元形态的操作数为 TDZ 变量
   预期: exec_operand 在 prefix 通道中拒绝（TDZ 精确错误） */
TEST_F(ExecLiteralTest, PrefixConsumesTdzOperandViaExecOperand) {
  clux_value_t *tzv = clux_context_create_i32(ctx, 8, "tz_p", NULL);
  ASSERT_NE(tzv, nullptr);
  clux_value_set_tdz(tzv, true);

  node_t *node =
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "-"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tz_p"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of a value before initialization");
  delete_node(&node);
}

/* 场景: 逻辑短路读取 TDZ bool 左操作数（tz && true）
   预期: 即使短路可能跳过右操作数，左操作数消费仍被 TDZ 拒绝 */
TEST_F(ExecLiteralTest, LogicalShortCircuitConsumesTdzBoolOperand) {
  clux_value_t *tz_bool = clux_context_create_bool(ctx, false, "tz_lb", NULL);
  ASSERT_NE(tz_bool, nullptr);
  clux_value_set_tdz(tz_bool, true);

  node_t *node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tz_lb"),
                     make_node(CLUX_NODE_SYMBOL, "&&"),
                     make_node(CLUX_NODE_LITERAL_BOOLEAN, "true"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of a value before initialization");
  delete_node(&node);
}

/* 场景: 三元条件的 cond 与选中分支均为 TDZ 变量（消费位置覆盖）
   预期: cond TDZ 直接拒绝；cond 为 true 时 then 分支 TDZ 同样拒绝 */
TEST_F(ExecLiteralTest, TernaryConsumesTdzCondAndSelectedBranch) {
  clux_value_t *tz_cond = clux_context_create_bool(ctx, true, "tz_tc", NULL);
  ASSERT_NE(tz_cond, nullptr);
  clux_value_set_tdz(tz_cond, true);
  node_t *cond_node =
      make_ternary_of(a,
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tz_tc"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "1"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "2"));
  clux_value_t *cond_result = exec_expr_ternary(ctx, cond_node);
  ASSERT_NE(cond_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(cond_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(cond_result),
               "use of a value before initialization");
  delete_node(&cond_node);

  clux_value_t *tz_branch = clux_context_create_i32(ctx, 7, "tz_tb", NULL);
  ASSERT_NE(tz_branch, nullptr);
  clux_value_set_tdz(tz_branch, true);
  node_t *branch_node =
      make_ternary_of(a,
                      make_node(CLUX_NODE_LITERAL_BOOLEAN, "true"),
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tz_tb"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "2"));
  clux_value_t *branch_result = exec_expr_ternary(ctx, branch_node);
  ASSERT_NE(branch_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(branch_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(branch_result),
               "use of a value before initialization");
  delete_node(&branch_node);
}

/* 场景: var 初值引用 TDZ 变量 / var 类型标注引用 TDZ type value
   预期: stmt_var 的两条消费通道（初值 + 类型标注）均拒绝 TDZ */
TEST_F(ExecLiteralTest, VarRejectsTdzInitialValueAndAnnotation) {
  clux_value_t *tz_src = clux_context_create_i32(ctx, 3, "tz_src", NULL);
  ASSERT_NE(tz_src, nullptr);
  clux_value_set_tdz(tz_src, true);
  node_t *var_node = make_stmt_var("tz_y",
                                   CLUX_NODE_SYMBOL /* type omitted */,
                                   NULL,
                                   CLUX_NODE_LITERAL_IDENTIFIER,
                                   "tz_src");
  clux_value_t *var_result = exec_stmt_var(ctx, var_node);
  ASSERT_NE(var_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(var_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(var_result),
               "use of a value before initialization");
  delete_node(&var_node);

  clux_value_t *tz_type = clux_context_create_type_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32), "tz_ty", NULL);
  ASSERT_NE(tz_type, nullptr);
  clux_value_set_tdz(tz_type, true);
  node_t *ann_node = make_stmt_var("tz_v",
                                   CLUX_NODE_LITERAL_IDENTIFIER,
                                   "tz_ty",
                                   CLUX_NODE_LITERAL_NUMERIC,
                                   "1");
  clux_value_t *ann_result = exec_stmt_var(ctx, ann_node);
  ASSERT_NE(ann_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(ann_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(ann_result),
               "use of a value before initialization");
  delete_node(&ann_node);
}

/* 场景: 纯 "=" 对 TDZ 目标是首次赋值通道
   预期: 赋值成功返回目标本身、TDZ 清除、值写入，随后可正常消费 */
TEST_F(ExecLiteralTest, AssignPureEqualsOnTdzTargetSucceedsAndClears) {
  clux_value_t *target = clux_context_create_i32(ctx, 0, "tz_eq", NULL);
  ASSERT_NE(target, nullptr);
  clux_value_set_tdz(target, true);

  node_t *node = make_assign(CLUX_NODE_LITERAL_NUMERIC, "=", "tz_eq", "5");
  clux_value_t *result = exec_expr_assign(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, target) << "first assignment must return the target";
  EXPECT_EQ(read_i32(target), 5);
  EXPECT_FALSE(clux_value_is_tdz(target))
      << "first assignment must clear the TDZ flag";
  node_t *read_node = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tz_eq");
  EXPECT_EQ(exec_operand(ctx, read_node), target)
      << "after first assignment the variable becomes consumable";
  delete_node(&read_node);
  delete_node(&node);
}

/* 构造带真实存储的具名 const i32 目标（TDZ 首赋值验收点）：
   create_i32 产出裸 i32 类型，const 目标须按 qualified 类型建值，
   存储块走 context 的 safe_allocator（context 销毁统一释放）。 */
static clux_value_t *make_const_i32_target(clux_context_t *ctx,
                                           const char *name) {
  clux_type_t *const_i32 = clux_type_get_qualified_type(
      clux_context_builtin_type(ctx, CLUX_TYPE_I32), CLUX_TYPE_CONST);
  ptr_handle_t data =
      safe_alloc(clux_context_get_safe_allocator(ctx), sizeof(int32_t), NULL);
  if (data == 0) {
    return nullptr;
  }
  void *raw = ptr_to_raw(data, 0);
  if (raw != nullptr) {
    *(int32_t *)raw = 0;
  }
  return clux_context_create_value(ctx, const_i32, data, name, NULL);
}

/* 场景: 纯 "=" 对 const 类型 + TDZ 目标（关键验收点：TDZ 旁路 const 检查）
   预期: 首次 "=" 赋值成功且清除 TDZ；清除后再次 "=" 恢复 const 拒绝 */
TEST_F(ExecLiteralTest, AssignPureEqualsOnTdzConstTargetSucceeds) {
  clux_value_t *target = make_const_i32_target(ctx, "tz_c");
  ASSERT_NE(target, nullptr);
  clux_value_set_tdz(target, true);

  node_t *node = make_assign(CLUX_NODE_LITERAL_NUMERIC, "=", "tz_c", "9");
  clux_value_t *result = exec_expr_assign(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result, target);
  EXPECT_EQ(read_i32(target), 9);
  EXPECT_FALSE(clux_value_is_tdz(target));
  delete_node(&node);

  node_t *again = make_assign(CLUX_NODE_LITERAL_NUMERIC, "=", "tz_c", "1");
  clux_value_t *bad = exec_expr_assign(ctx, again);
  ASSERT_NE(bad, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad),
               "cannot assign to a value of type 'const i32'");
  delete_node(&again);
}

/* 场景: compound 赋值（+= -= *=）对 TDZ 目标读取当前值
   预期: 报 "use of a value before initialization"（目标读取在写入之前拒绝） */
TEST_F(ExecLiteralTest, AssignCompoundRejectsTdzTarget) {
  const char *ops[] = {"+=", "-=", "*="};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    const char *op = ops[i];
    char name[32];
    snprintf(name, sizeof(name), "tz_cmp_%zu", i);
    clux_value_t *target = clux_context_create_i32(ctx, 10, name, NULL);
    ASSERT_NE(target, nullptr) << op;
    clux_value_set_tdz(target, true);
    node_t *node = make_assign(CLUX_NODE_LITERAL_NUMERIC, op, name, "3");
    clux_value_t *result = exec_expr_assign(ctx, node);
    ASSERT_NE(result, nullptr) << op;
    EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR)
        << op;
    EXPECT_STREQ(error_message_of(result),
                 "use of a value before initialization")
        << op;
    delete_node(&node);
  }
}

/* 场景: 逻辑复合赋值（&&= 与 ||=）对 TDZ bool 目标读取当前值
   预期: 同样报 "use of a value before initialization" */
TEST_F(ExecLiteralTest, AssignLogicalCompoundRejectsTdzTarget) {
  const char *ops[] = {"&&=", "||="};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    const char *op = ops[i];
    char name[32];
    snprintf(name, sizeof(name), "tz_lc_%zu", i);
    clux_value_t *target = clux_context_create_bool(ctx, true, name, NULL);
    ASSERT_NE(target, nullptr) << op;
    clux_value_set_tdz(target, true);
    node_t *node = make_assign(CLUX_NODE_LITERAL_BOOLEAN, op, name, "true");
    clux_value_t *result = exec_expr_assign(ctx, node);
    ASSERT_NE(result, nullptr) << op;
    EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR)
        << op;
    EXPECT_STREQ(error_message_of(result),
                 "use of a value before initialization")
        << op;
    delete_node(&node);
  }
}

/* 场景: 首 "=" 清除 TDZ 后再 compound 赋值（tz_seq = 5; tz_seq += 1 序列）
   预期: 首次 "=" 成功，后续 += 读取当前值合法，最终值为 6 */
TEST_F(ExecLiteralTest, AssignEqualsThenCompoundAfterTdzClearSucceeds) {
  clux_value_t *target = clux_context_create_i32(ctx, 0, "tz_seq", NULL);
  ASSERT_NE(target, nullptr);
  clux_value_set_tdz(target, true);

  node_t *init = make_assign(CLUX_NODE_LITERAL_NUMERIC, "=", "tz_seq", "5");
  clux_value_t *init_result = exec_expr_assign(ctx, init);
  ASSERT_NE(init_result, nullptr);
  EXPECT_EQ(init_result, target);
  delete_node(&init);

  node_t *inc = make_assign(CLUX_NODE_LITERAL_NUMERIC, "+=", "tz_seq", "1");
  clux_value_t *inc_result = exec_expr_assign(ctx, inc);
  ASSERT_NE(inc_result, nullptr);
  EXPECT_EQ(inc_result, target);
  EXPECT_EQ(read_i32(target), 6);
  EXPECT_EQ(read_i32(clux_context_lookup(ctx, "tz_seq", NULL)), 6)
      << "scope variable must reflect the compound write";
  delete_node(&inc);
}

/* ---- exec_stmt_var：var x: T = undefined → TDZ 变量声明 ---- */

/* 持久 undefined 字面量文本（节点 location 与 error location 断言共用）。 */
static const char kUndefinedText[] = "undefined";

/* 场景: var tu: i32 = undefined（右值是 undefined 字面量 + 类型标注）
   预期: 走 TDZ 分支跳过初值求值，返回 is_tdz=true 的 i32 值（真实存储、
         零初始化），scope 按名可查到同一指针 */
TEST_F(ExecLiteralTest, VarUndefinedCreatesTdzVariable) {
  node_t *node = make_stmt_var("tu",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               "i32",
                               CLUX_NODE_LITERAL_UNDEFINED,
                               kUndefinedText);
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_TRUE(clux_value_is_tdz(result))
      << "undefined initializer must produce a TDZ-marked variable";
  EXPECT_NE(clux_value_get_data(result), (ptr_handle_t)0)
      << "TDZ variable must own real (non-shadow) storage";
  EXPECT_EQ(read_i32(result), 0) << "TDZ storage is zero-initialized";
  EXPECT_EQ(clux_context_lookup(ctx, "tu", NULL), result);
  delete_node(&node);
}

/* 场景: TDZ 变量生命周期（var tu: i32 = undefined 后读取/赋值/再读取）
   预期: identifier 读取被拒（"use of a value before initialization"）；
         首次 "=" 赋值成功并清除 TDZ；随后 identifier 读取放行且值为 5 */
TEST_F(ExecLiteralTest, VarUndefinedTdzLifecycleAssignThenRead) {
  node_t *decl = make_stmt_var("tu",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               "i32",
                               CLUX_NODE_LITERAL_UNDEFINED,
                               kUndefinedText);
  clux_value_t *declared = exec_stmt_var(ctx, decl);
  ASSERT_NE(declared, nullptr);
  ASSERT_TRUE(clux_value_is_tdz(declared));
  delete_node(&decl);

  /* TDZ 期间 identifier 读取被拒 */
  node_t *read = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tu");
  clux_value_t *early = exec_operand(ctx, read);
  ASSERT_NE(early, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(early)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(early), "use of a value before initialization");

  /* 首次 "=" 赋值写入并清除 TDZ */
  node_t *assign = make_assign(CLUX_NODE_LITERAL_NUMERIC, "=", "tu", "5");
  clux_value_t *assigned = exec_expr_assign(ctx, assign);
  ASSERT_NE(assigned, nullptr);
  EXPECT_EQ(assigned, declared)
      << "first assignment must return the declared target";
  EXPECT_EQ(read_i32(declared), 5);
  EXPECT_FALSE(clux_value_is_tdz(declared));
  delete_node(&assign);

  /* TDZ 清除后读取放行且值正确 */
  clux_value_t *after = exec_operand(ctx, read);
  EXPECT_EQ(after, declared);
  EXPECT_EQ(read_i32(after), 5);
  delete_node(&read);
}

/* 场景: var 无类型标注 + undefined 右值（var nu = undefined）
   预期: 报 "undefined initializer requires a type annotation"，
         error 位置为 value 节点（begin.offset 指向 undefined 文本） */
TEST_F(ExecLiteralTest, VarUndefinedWithoutAnnotationErrorsAtValueNode) {
  node_t *node = make_stmt_var("nu",
                               CLUX_NODE_SYMBOL /* type omitted */,
                               NULL,
                               CLUX_NODE_LITERAL_UNDEFINED,
                               kUndefinedText);
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "undefined initializer requires a type annotation");
  clux_error_t *err =
      *(clux_error_t **)ptr_to_raw(clux_value_get_data(result), 0);
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(error_get_location(err)->begin.offset, kUndefinedText)
      << "error location must point at the undefined value node";
  delete_node(&node);
}

/* 场景: 类型标注不是 type value（var nu: <普通 i32 值> = undefined）
   预期: 仍报 "expected type expression in var declaration" */
TEST_F(ExecLiteralTest, VarUndefinedAnnotationNotTypeValueErrors) {
  clux_value_t *plain = clux_context_create_i32(ctx, 3, "plain_i32", NULL);
  ASSERT_NE(plain, nullptr);
  node_t *node = make_stmt_var("nu",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               "plain_i32",
                               CLUX_NODE_LITERAL_UNDEFINED,
                               kUndefinedText);
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "expected type expression in var declaration");
  delete_node(&node);
}

/* 场景: 类型标注求值出错（kTooLongNumeric 探针作标注）
   预期: 标注 error 优先传播——"numeric literal too long" */
TEST_F(ExecLiteralTest, VarUndefinedAnnotationErrorPropagates) {
  node_t *node = make_stmt_var("ne",
                               CLUX_NODE_LITERAL_NUMERIC,
                               kTooLongNumeric,
                               CLUX_NODE_LITERAL_UNDEFINED,
                               kUndefinedText);
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "numeric literal too long");
  delete_node(&node);
}

/* 场景: 重复声明 TDZ 变量（两次 var td: i32 = undefined）
   预期: 第二次报 "duplicate declaration 'td'"，首个 TDZ 变量保持原状 */
TEST_F(ExecLiteralTest, VarUndefinedDuplicateDeclarationErrors) {
  node_t *first = make_stmt_var("td",
                                CLUX_NODE_LITERAL_IDENTIFIER,
                                "i32",
                                CLUX_NODE_LITERAL_UNDEFINED,
                                kUndefinedText);
  clux_value_t *first_result = exec_stmt_var(ctx, first);
  ASSERT_NE(first_result, nullptr);
  ASSERT_TRUE(clux_value_is_tdz(first_result));
  delete_node(&first);

  node_t *second = make_stmt_var("td",
                                 CLUX_NODE_LITERAL_IDENTIFIER,
                                 "i32",
                                 CLUX_NODE_LITERAL_UNDEFINED,
                                 kUndefinedText);
  clux_value_t *second_result = exec_stmt_var(ctx, second);
  ASSERT_NE(second_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(second_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(second_result), "duplicate declaration 'td'");
  EXPECT_TRUE(clux_value_is_tdz(first_result))
      << "duplicate must not clear the first variable's TDZ flag";
  delete_node(&second);
}

/* 场景: 单独执行 undefined 字面量节点（不经 var 声明包装）
   预期: exec 分派表中该 kind 为 NULL 槽，报
         "unsupported node kind: LITERAL_UNDEFINED"（预期行为回归保护） */
TEST_F(ExecLiteralTest, StandaloneUndefinedLiteralUnsupported) {
  node_t *node = make_node(CLUX_NODE_LITERAL_UNDEFINED, kUndefinedText);
  clux_value_t *value = exec_node(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value),
               "unsupported node kind: LITERAL_UNDEFINED");
  delete_node(&node);
}

/* ---- exec_expr_subscript ---- */

/* 场景: [4]i32 数组的正常下标取项 a[1] / a[3]
   预期: 返回 i32 类型的 view，值为预填的 11 / 13 */
TEST_F(ExecLiteralTest, SubscriptReadsArrayElement) {
  ASSERT_NE(make_i32_array("arr_r", 4), nullptr);

  node_t *one =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_r"),
                        make_node(CLUX_NODE_LITERAL_NUMERIC, "1"));
  clux_value_t *value1 = exec_expr_subscript(ctx, one);
  ASSERT_NE(value1, nullptr);
  EXPECT_EQ(clux_value_get_type(value1),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_EQ(read_i32(value1), 11);
  delete_node(&one);

  node_t *three =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_r"),
                        make_node(CLUX_NODE_LITERAL_NUMERIC, "3"));
  clux_value_t *value3 = exec_expr_subscript(ctx, three);
  ASSERT_NE(value3, nullptr);
  EXPECT_EQ(read_i32(value3), 13);
  delete_node(&three);
}

/* 场景: 下标返回 base 类型 view（与数组内联块内存共享）
   预期: 经通用 assign 写入 view 后原数组元素可见变化（写穿） */
TEST_F(ExecLiteralTest, SubscriptViewAliasToBaseArray) {
  clux_value_t *arr = make_i32_array("arr_w", 4);
  ASSERT_NE(arr, nullptr);

  node_t *two =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_w"),
                        make_node(CLUX_NODE_LITERAL_NUMERIC, "2"));
  clux_value_t *view = exec_expr_subscript(ctx, two);
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(read_i32(view), 12);

  clux_value_t *v42 = clux_context_create_i32(ctx, 42, NULL, NULL);
  ASSERT_NE(v42, nullptr);
  EXPECT_EQ(clux_context_assign(ctx, view, v42, NULL), view);

  /* engine 层独立通道读取同一元素，确认写穿到数组本体 */
  clux_value_t *check = clux_context_index(
      ctx, arr, clux_context_create_usize(ctx, 2, NULL, NULL), NULL);
  ASSERT_NE(check, nullptr);
  EXPECT_EQ(read_i32(check), 42);

  /* 再次经 exec 下标读取也可见 */
  clux_value_t *again = exec_expr_subscript(ctx, two);
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(read_i32(again), 42);
  delete_node(&two);
}

/* 场景: 越界下标（负数 -1 与 >=len 的 4，数组长度 4）
   预期: 均为 error，消息精确为 "index -1 is out of bounds for array of
         length 4" / "index 4 is out of bounds for array of length 4" */
TEST_F(ExecLiteralTest, SubscriptIndexOutOfBoundsErrors) {
  ASSERT_NE(make_i32_array("arr_b", 4), nullptr);

  node_t *neg =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_b"),
                        make_node(CLUX_NODE_LITERAL_NUMERIC, "-1"));
  clux_value_t *neg_value = exec_expr_subscript(ctx, neg);
  ASSERT_NE(neg_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(neg_value)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(neg_value),
               "index -1 is out of bounds for array of length 4");
  delete_node(&neg);

  node_t *big =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_b"),
                        make_node(CLUX_NODE_LITERAL_NUMERIC, "4"));
  clux_value_t *big_value = exec_expr_subscript(ctx, big);
  ASSERT_NE(big_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(big_value)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(big_value),
               "index 4 is out of bounds for array of length 4");
  delete_node(&big);
}

/* 场景: 非整数下标（bool 字面量 true 与 str 字面量 "s"）
   预期: 均为 error，消息精确为 "index must be an integer, got 'bool'" /
         "index must be an integer, got 'str'" */
TEST_F(ExecLiteralTest, SubscriptNonIntegerIndexErrors) {
  ASSERT_NE(make_i32_array("arr_n", 4), nullptr);

  node_t *bool_node =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_n"),
                        make_node(CLUX_NODE_LITERAL_BOOLEAN, "true"));
  clux_value_t *bool_value = exec_expr_subscript(ctx, bool_node);
  ASSERT_NE(bool_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bool_value)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bool_value),
               "index must be an integer, got 'bool'");
  delete_node(&bool_node);

  node_t *str_node =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_n"),
                        make_node(CLUX_NODE_LITERAL_STRING, "\"s\""));
  clux_value_t *str_value = exec_expr_subscript(ctx, str_node);
  ASSERT_NE(str_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(str_value)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(str_value),
               "index must be an integer, got 'str'");
  delete_node(&str_node);
}

/* 场景: 泛型实例化形态 Type[arg]（base "i32" 求值为 type value）
   预期: 报 "generic instantiation is not supported yet" */
TEST_F(ExecLiteralTest, SubscriptGenericInstantiationNotSupported) {
  node_t *node =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_LITERAL_NUMERIC, "1"));
  clux_value_t *value = exec_expr_subscript(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value),
               "generic instantiation is not supported yet");
  delete_node(&node);
}

/* 场景: 实参个数不为 1（0 参 a[] 与 2 参 a[0,1]）
   预期: 均报 "subscript takes exactly one index" */
TEST_F(ExecLiteralTest, SubscriptRequiresExactlyOneIndex) {
  ASSERT_NE(make_i32_array("arr_c", 4), nullptr);

  node_t *zero_arg = make_subscript_of(
      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_c"), nullptr);
  clux_value_t *zero_value = exec_expr_subscript(ctx, zero_arg);
  ASSERT_NE(zero_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(zero_value)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(zero_value),
               "subscript takes exactly one index");
  delete_node(&zero_arg);

  node_t *two_args =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_c"),
                        make_node(CLUX_NODE_LITERAL_NUMERIC, "0"));
  subscript_node_add_arg(two_args, make_node(CLUX_NODE_LITERAL_NUMERIC, "1"));
  clux_value_t *two_value = exec_expr_subscript(ctx, two_args);
  ASSERT_NE(two_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(two_value)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(two_value),
               "subscript takes exactly one index");
  delete_node(&two_args);
}

/* 场景: base 子节点求值为 error（未注册 kind STMT_BLOCK）
   预期: error 透传——"unsupported node kind: STMT_BLOCK" */
TEST_F(ExecLiteralTest, SubscriptPropagatesBaseError) {
  node_t *node = make_subscript_of(make_node(CLUX_NODE_STMT_BLOCK, "..."),
                                   make_node(CLUX_NODE_LITERAL_NUMERIC, "1"));
  clux_value_t *value = exec_expr_subscript(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "unsupported node kind: STMT_BLOCK");
  delete_node(&node);
}

/* 场景: index 子节点求值为 error（未注册 kind STMT_BLOCK），base 正常
   预期: index 的 error 透传——"unsupported node kind: STMT_BLOCK" */
TEST_F(ExecLiteralTest, SubscriptPropagatesIndexError) {
  ASSERT_NE(make_i32_array("arr_e", 4), nullptr);

  node_t *node =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_e"),
                        make_node(CLUX_NODE_STMT_BLOCK, "..."));
  clux_value_t *value = exec_expr_subscript(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "unsupported node kind: STMT_BLOCK");
  delete_node(&node);
}

/* 场景: TDZ 拒绝——TDZ 数组作 base 与 TDZ i32 作 index（均经 exec_operand
   消费通道）
   预期: 均报 "use of a value before initialization" */
TEST_F(ExecLiteralTest, SubscriptRejectsTdzBaseAndIndex) {
  clux_value_t *tdz_arr = make_i32_array("tz_arr", 2);
  ASSERT_NE(tdz_arr, nullptr);
  clux_value_set_tdz(tdz_arr, true);

  node_t *base_node =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tz_arr"),
                        make_node(CLUX_NODE_LITERAL_NUMERIC, "0"));
  clux_value_t *base_value = exec_expr_subscript(ctx, base_node);
  ASSERT_NE(base_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(base_value)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(base_value),
               "use of a value before initialization");
  delete_node(&base_node);

  clux_value_t *arr = make_i32_array("arr_t", 2);
  ASSERT_NE(arr, nullptr);
  clux_value_t *tdz_idx = clux_context_create_i32(ctx, 1, "tz_idx", NULL);
  ASSERT_NE(tdz_idx, nullptr);
  clux_value_set_tdz(tdz_idx, true);

  node_t *idx_node =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_t"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tz_idx"));
  clux_value_t *idx_value = exec_expr_subscript(ctx, idx_node);
  ASSERT_NE(idx_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(idx_value)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(idx_value),
               "use of a value before initialization");
  delete_node(&idx_node);
}

/* ---- exec_expr_slice ---- */

/* 场景: AST 数组切片 a[1:3]（base=identifier lo=numeral hi=numeral）
   预期: 产出 []i32 切片值，len 为 2，s[0]==11、s[1]==12（ptr 偏移正确） */
TEST_F(ExecLiteralTest, SliceReadsArrayRange) {
  ASSERT_NE(make_i32_array("arr_sl", 4), nullptr);

  node_t *node =
      make_slice_of("arr_sl[1:3]",
                    make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_sl"),
                    make_node(CLUX_NODE_LITERAL_NUMERIC, "1"),
                    make_node(CLUX_NODE_LITERAL_NUMERIC, "3"));
  clux_value_t *s = exec_expr_slice(ctx, node);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(clux_value_get_type(s),
            clux_context_get_slice_type(
                ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32)));
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, s, NULL)), 2u);
  EXPECT_EQ(read_i32(clux_context_index(
                ctx, s, clux_context_create_usize(ctx, 0, NULL, NULL), NULL)),
            11);
  EXPECT_EQ(read_i32(clux_context_index(
                ctx, s, clux_context_create_usize(ctx, 1, NULL, NULL), NULL)),
            12);
  delete_node(&node);
}

/* 场景: 开放端点 AST（lo 子节点 NULL 的 a[:2]、hi NULL 的 a[1:]、
         双 NULL 的 a[:]）
   预期: exec 走 NULL 端点 → engine 缺省 0 / 数组长度，读回首末元素值 */
TEST_F(ExecLiteralTest, SliceOpenEndpoints) {
  ASSERT_NE(make_i32_array("arr_op", 4), nullptr);

  node_t *front =
      make_slice_of("arr_op[:2]",
                    make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_op"),
                    nullptr,
                    make_node(CLUX_NODE_LITERAL_NUMERIC, "2"));
  clux_value_t *front_value = exec_expr_slice(ctx, front);
  ASSERT_NE(front_value, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, front_value, NULL)), 2u);
  EXPECT_EQ(
      read_i32(clux_context_index(ctx,
                                  front_value,
                                  clux_context_create_usize(ctx, 1, NULL, NULL),
                                  NULL)),
      11);
  delete_node(&front);

  node_t *tail =
      make_slice_of("arr_op[1:]",
                    make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_op"),
                    make_node(CLUX_NODE_LITERAL_NUMERIC, "1"),
                    nullptr);
  clux_value_t *tail_value = exec_expr_slice(ctx, tail);
  ASSERT_NE(tail_value, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, tail_value, NULL)), 3u);
  EXPECT_EQ(
      read_i32(clux_context_index(ctx,
                                  tail_value,
                                  clux_context_create_usize(ctx, 2, NULL, NULL),
                                  NULL)),
      13);
  delete_node(&tail);

  node_t *whole =
      make_slice_of("arr_op[:]",
                    make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_op"),
                    nullptr,
                    nullptr);
  clux_value_t *whole_value = exec_expr_slice(ctx, whole);
  ASSERT_NE(whole_value, nullptr);
  EXPECT_EQ(read_usize(clux_context_slice_len(ctx, whole_value, NULL)), 4u);
  EXPECT_EQ(
      read_i32(clux_context_index(ctx,
                                  whole_value,
                                  clux_context_create_usize(ctx, 0, NULL, NULL),
                                  NULL)),
      10);
  delete_node(&whole);
}

/* 场景: base 子节点求值为 error（未注册 kind STMT_BLOCK）
   预期: error 透传——"unsupported node kind: STMT_BLOCK" */
TEST_F(ExecLiteralTest, SlicePropagatesBaseError) {
  node_t *node = make_slice_of("bad[1:3]",
                               make_node(CLUX_NODE_STMT_BLOCK, "..."),
                               make_node(CLUX_NODE_LITERAL_NUMERIC, "1"),
                               make_node(CLUX_NODE_LITERAL_NUMERIC, "3"));
  clux_value_t *value = exec_expr_slice(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "unsupported node kind: STMT_BLOCK");
  delete_node(&node);
}

/* 场景: lo/hi 子节点求值为 error（未注册 kind STMT_BLOCK），base 正常
   预期: 端点 error 透传——"unsupported node kind: STMT_BLOCK"。
         base 成功在先，lo 先于 hi 求值 */
TEST_F(ExecLiteralTest, SlicePropagatesEndpointErrors) {
  ASSERT_NE(make_i32_array("arr_pe", 4), nullptr);

  node_t *lo_node =
      make_slice_of("arr_pe[bad:3]",
                    make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_pe"),
                    make_node(CLUX_NODE_STMT_BLOCK, "..."),
                    make_node(CLUX_NODE_LITERAL_NUMERIC, "3"));
  clux_value_t *lo_value = exec_expr_slice(ctx, lo_node);
  ASSERT_NE(lo_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(lo_value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(lo_value), "unsupported node kind: STMT_BLOCK");
  delete_node(&lo_node);

  node_t *hi_node =
      make_slice_of("arr_pe[1:bad]",
                    make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_pe"),
                    make_node(CLUX_NODE_LITERAL_NUMERIC, "1"),
                    make_node(CLUX_NODE_STMT_BLOCK, "..."));
  clux_value_t *hi_value = exec_expr_slice(ctx, hi_node);
  ASSERT_NE(hi_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(hi_value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(hi_value), "unsupported node kind: STMT_BLOCK");
  delete_node(&hi_node);
}

/* 场景: TDZ 拒绝——TDZ 数组作 base（经 exec_operand 消费通道）
   预期: 报 "use of a value before initialization" */
TEST_F(ExecLiteralTest, SliceRejectsTdzBase) {
  clux_value_t *tdz_arr = make_i32_array("tz_sarr", 2);
  ASSERT_NE(tdz_arr, nullptr);
  clux_value_set_tdz(tdz_arr, true);

  node_t *node =
      make_slice_of("tz_sarr[:]",
                    make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tz_sarr"),
                    nullptr,
                    nullptr);
  clux_value_t *value = exec_expr_slice(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "use of a value before initialization");
  delete_node(&node);
}

/* 场景: engine 层范围错误在 exec 层的传播（lo>hi / 越界 / 负数）
   预期: 消息与 engine 精确一致 */
TEST_F(ExecLiteralTest, SlicePropagatesRangeErrors) {
  ASSERT_NE(make_i32_array("arr_re", 4), nullptr);

  node_t *reversed =
      make_slice_of("arr_re[3:1]",
                    make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_re"),
                    make_node(CLUX_NODE_LITERAL_NUMERIC, "3"),
                    make_node(CLUX_NODE_LITERAL_NUMERIC, "1"));
  clux_value_t *reversed_value = exec_expr_slice(ctx, reversed);
  ASSERT_NE(reversed_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(reversed_value)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(reversed_value), "slice start 3 exceeds end 1");
  delete_node(&reversed);

  node_t *oob = make_slice_of("arr_re[5:]",
                              make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_re"),
                              make_node(CLUX_NODE_LITERAL_NUMERIC, "5"),
                              nullptr);
  clux_value_t *oob_value = exec_expr_slice(ctx, oob);
  ASSERT_NE(oob_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(oob_value)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(oob_value),
               "slice index 5 is out of bounds for array of length 4");
  delete_node(&oob);

  node_t *neg = make_slice_of("arr_re[-1:]",
                              make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_re"),
                              make_node(CLUX_NODE_LITERAL_NUMERIC, "-1"),
                              nullptr);
  clux_value_t *neg_value = exec_expr_slice(ctx, neg);
  ASSERT_NE(neg_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(neg_value)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(neg_value), "slice index must not be negative");
  delete_node(&neg);
}

/* ---- exec_expr_binary：复合类型定义表达式（前缀 *T/[]T/?T/const/volatile
       与数组类型 [N]T）---- */

/* 从 type value 提取被描述的 clux_type_t*（raw 首槽位是类型指针）。 */
static clux_type_t *type_described_by(clux_value_t *value) {
  clux_type_t **stored =
      (clux_type_t **)ptr_to_raw(clux_value_get_data(value), 0);
  return stored ? *stored : nullptr;
}

/* 场景: 前缀类型 *i32 / []i32 / ?i32（LEFT=NULL，SYMBOL 存运算符文本）
   预期: 产出 type value（值类型 unwrap 为 TYPE），被描述类型与引擎
         clux_context_get_*_type 通道 interning 恒等（指针相同） */
TEST_F(ExecLiteralTest, TypePrefixBuildsCompositeTypeValue) {
  struct {
    const char *op;
    clux_type_kind_t kind;
  } cases[] = {
      {"*", CLUX_TYPE_PTR},
      {"[]", CLUX_TYPE_SLICE},
      {"?", CLUX_TYPE_OPTIONAL},
  };
  for (const auto &c : cases) {
    node_t *node =
        make_binary_of(a,
                       NULL,
                       make_node(CLUX_NODE_SYMBOL, c.op),
                       make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
    clux_value_t *result = exec_expr_binary(ctx, node);
    ASSERT_NE(result, nullptr) << c.op;
    EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)),
              CLUX_TYPE_TYPE)
        << c.op << " prefix must produce a type value";
    clux_type_t *described = type_described_by(result);
    ASSERT_NE(described, nullptr) << c.op;
    EXPECT_EQ(clux_type_get_kind(described), c.kind) << c.op;
    clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
    clux_type_t *expected = nullptr;
    if (c.op[0] == '*') {
      expected = clux_context_get_ptr_type(ctx, i32_type);
    } else if (c.op[0] == '[') {
      expected = clux_context_get_slice_type(ctx, i32_type);
    } else {
      expected = clux_context_get_optional_type(ctx, i32_type);
    }
    EXPECT_EQ(described, expected)
        << c.op << " composite must be interned with the engine channel";
    delete_node(&node);
  }
}

/* 场景: 同一 *i32 表达式两次求值
   预期: interning 生效——两次提取的被描述类型指针相同 */
TEST_F(ExecLiteralTest, TypePrefixDoubleEvaluationIsInterned) {
  clux_type_t *first = nullptr;
  clux_type_t *second = nullptr;
  for (int round = 0; round < 2; round++) {
    node_t *node =
        make_binary_of(a,
                       NULL,
                       make_node(CLUX_NODE_SYMBOL, "*"),
                       make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
    clux_value_t *result = exec_expr_binary(ctx, node);
    ASSERT_NE(result, nullptr) << round;
    EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)),
              CLUX_TYPE_TYPE)
        << round;
    clux_type_t *described = type_described_by(result);
    ASSERT_NE(described, nullptr) << round;
    if (round == 0) {
      first = described;
    } else {
      second = described;
    }
    delete_node(&node);
  }
  EXPECT_EQ(first, second)
      << "two evaluations of *i32 must describe the same interned type";
}

/* 场景: 限定前缀 const i32 / volatile i32 与链式 const volatile i32
   预期: 单层限定 kind 正确且与 clux_type_get_qualified_type 恒等；
         链式叠加外层 const、内层 volatile、底层 i32 */
TEST_F(ExecLiteralTest, TypePrefixQualifiersNest) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);

  node_t *const_node =
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "const"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *const_result = exec_expr_binary(ctx, const_node);
  ASSERT_NE(const_result, nullptr);
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(const_result)),
            CLUX_TYPE_TYPE);
  clux_type_t *const_described = type_described_by(const_result);
  ASSERT_NE(const_described, nullptr);
  EXPECT_EQ(clux_type_get_kind(const_described), CLUX_TYPE_CONST);
  EXPECT_EQ(const_described,
            clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST));
  delete_node(&const_node);

  node_t *volatile_node =
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "volatile"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *volatile_result = exec_expr_binary(ctx, volatile_node);
  ASSERT_NE(volatile_result, nullptr);
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(volatile_result)),
            CLUX_TYPE_TYPE);
  clux_type_t *volatile_described = type_described_by(volatile_result);
  ASSERT_NE(volatile_described, nullptr);
  EXPECT_EQ(clux_type_get_kind(volatile_described), CLUX_TYPE_VOLATILE);
  EXPECT_EQ(volatile_described,
            clux_type_get_qualified_type(i32_type, CLUX_TYPE_VOLATILE));
  delete_node(&volatile_node);

  /* const volatile i32：外层 const 节点的 RIGHT 是 volatile 节点 */
  node_t *inner =
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "volatile"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *outer =
      make_binary_of(a, NULL, make_node(CLUX_NODE_SYMBOL, "const"), inner);
  clux_value_t *chained_result = exec_expr_binary(ctx, outer);
  ASSERT_NE(chained_result, nullptr);
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(chained_result)),
            CLUX_TYPE_TYPE);
  clux_type_t *chained = type_described_by(chained_result);
  ASSERT_NE(chained, nullptr);
  EXPECT_EQ(clux_type_get_kind(chained), CLUX_TYPE_CONST)
      << "outermost qualifier must be const";
  EXPECT_EQ(clux_type_get_qualified_base(chained),
            clux_type_get_qualified_type(i32_type, CLUX_TYPE_VOLATILE))
      << "inner qualifier must be volatile";
  EXPECT_EQ(clux_type_unwrap(chained), i32_type)
      << "stripping all qualifiers must reach the base i32";
  EXPECT_TRUE(clux_type_is_const(chained));
  delete_node(&outer);
}

/* 场景: 数组类型 [4]i32（LEFT=长度表达式，SYMBOL="[]"，RIGHT=元素类型）
   预期: 产出 type value，被描述类型与 clux_context_get_array_type 恒等，
         类型名为 "[4]i32" */
TEST_F(ExecLiteralTest, ArrayTypeBuildsInternedArrayTypeValue) {
  node_t *node = make_binary_of(a,
                                make_node(CLUX_NODE_LITERAL_NUMERIC, "4"),
                                make_node(CLUX_NODE_SYMBOL, "[]"),
                                make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_ARRAY);
  EXPECT_EQ(described,
            clux_context_get_array_type(
                ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32), 4));
  EXPECT_STREQ(clux_type_get_name(described), "[4]i32");
  delete_node(&node);
}

/* 场景: 数组长度用全部 10 种整数宽度的字面量（is_integer_kind 全 case）
   预期: 均经 clux_context_index_to_i64 正确读取为长度 4，
         interning 下描述同一 [4]i32 类型 */
TEST_F(ExecLiteralTest, ArrayLengthAcceptsAllIntegerWidths) {
  const char *lengths[] = {
      "4",
      "4i8",
      "4i16",
      "4i32",
      "4i64",
      "4isize",
      "4u8",
      "4u16",
      "4u32",
      "4u64",
      "4usize",
  };
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *expected = clux_context_get_array_type(ctx, i32_type, 4);
  for (const char *len : lengths) {
    node_t *node =
        make_binary_of(a,
                       make_node(CLUX_NODE_LITERAL_NUMERIC, len),
                       make_node(CLUX_NODE_SYMBOL, "[]"),
                       make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
    clux_value_t *result = exec_expr_binary(ctx, node);
    ASSERT_NE(result, nullptr) << len;
    EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)),
              CLUX_TYPE_TYPE)
        << len;
    clux_type_t *described = type_described_by(result);
    ASSERT_NE(described, nullptr) << len;
    EXPECT_EQ(described, expected) << len;
    delete_node(&node);
  }
}

/* 场景: 数组长度非整数（bool 字面量 / str 字面量）
   预期: 报 "array length must be an integer, got 'bool'" /
         "array length must be an integer, got 'str'" */
TEST_F(ExecLiteralTest, ArrayLengthRejectsNonInteger) {
  struct {
    node_kind_t kind;
    const char *text;
    const char *type_name;
  } cases[] = {
      {CLUX_NODE_LITERAL_BOOLEAN, "true", "bool"},
      {CLUX_NODE_LITERAL_STRING, "\"s\"", "str"},
  };
  for (const auto &c : cases) {
    node_t *node =
        make_binary_of(a,
                       make_node(c.kind, c.text),
                       make_node(CLUX_NODE_SYMBOL, "[]"),
                       make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
    clux_value_t *result = exec_expr_binary(ctx, node);
    ASSERT_NE(result, nullptr) << c.type_name;
    EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR)
        << c.type_name;
    char expected[64];
    snprintf(expected,
             sizeof(expected),
             "array length must be an integer, got '%s'",
             c.type_name);
    EXPECT_STREQ(error_message_of(result), expected) << c.type_name;
    delete_node(&node);
  }
}

/* 场景: 数组长度为负数（-3）
   预期: 报 "array length must not be negative" */
TEST_F(ExecLiteralTest, ArrayLengthRejectsNegative) {
  node_t *node = make_binary_of(a,
                                make_node(CLUX_NODE_LITERAL_NUMERIC, "-3"),
                                make_node(CLUX_NODE_SYMBOL, "[]"),
                                make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "array length must not be negative");
  delete_node(&node);
}

/* 场景: 类型前缀操作数不是 type value（*5：RIGHT 是数值字面量）
   预期: 报 "expected a type expression, got 'i32'" */
TEST_F(ExecLiteralTest, TypePrefixOperandNotTypeValueErrors) {
  node_t *node = make_binary_of(a,
                                NULL,
                                make_node(CLUX_NODE_SYMBOL, "*"),
                                make_node(CLUX_NODE_LITERAL_NUMERIC, "5"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "expected a type expression, got 'i32'");
  delete_node(&node);
}

/* 场景: 数组元素不是 type value（[4]5：RIGHT 是数值字面量）
   预期: 同样报 "expected a type expression, got 'i32'"（元素检查在
         长度检查之后，长度 4 合法故错误由元素通道产出） */
TEST_F(ExecLiteralTest, ArrayElementNotTypeValueErrors) {
  node_t *node = make_binary_of(a,
                                make_node(CLUX_NODE_LITERAL_NUMERIC, "4"),
                                make_node(CLUX_NODE_SYMBOL, "[]"),
                                make_node(CLUX_NODE_LITERAL_NUMERIC, "5"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "expected a type expression, got 'i32'");
  delete_node(&node);
}

/* 场景: 限定前缀操作数不是 type value（const 5：RIGHT 是数值字面量）
   预期: 走与类型前缀公共的 exec_operand_type 通道——
         报 "expected a type expression, got 'i32'" */
TEST_F(ExecLiteralTest, QualifierPrefixOperandNotTypeValueErrors) {
  node_t *node = make_binary_of(a,
                                NULL,
                                make_node(CLUX_NODE_SYMBOL, "const"),
                                make_node(CLUX_NODE_LITERAL_NUMERIC, "5"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "expected a type expression, got 'i32'");
  delete_node(&node);
}

/* 场景: 类型前缀操作数子节点求值出错（RIGHT=STMT_BLOCK 裸节点）
   预期: error 透传——"unsupported node kind: STMT_BLOCK" */
TEST_F(ExecLiteralTest, TypePrefixPropagatesOperandError) {
  node_t *node = make_binary_of(a,
                                NULL,
                                make_node(CLUX_NODE_SYMBOL, "*"),
                                make_node(CLUX_NODE_STMT_BLOCK, "..."));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "unsupported node kind: STMT_BLOCK");
  delete_node(&node);
}

/* 场景: RIGHT NULL 容错残缺节点（* 缺操作数）
   预期: exec_operand 拒绝空节点——"execution requires a node" */
TEST_F(ExecLiteralTest, TypePrefixMissingOperandRequiresNode) {
  node_t *node =
      make_binary_of(a, NULL, make_node(CLUX_NODE_SYMBOL, "*"), NULL);
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "execution requires a node");
  delete_node(&node);
}

/* 场景: 数组长度/元素子节点求值出错（长度先求值故其 error 优先透传；
         长度合法时元素的 error 透传）
   预期: 均报 "unsupported node kind: STMT_BLOCK" */
TEST_F(ExecLiteralTest, ArrayPropagatesLengthAndElementErrors) {
  node_t *bad_length =
      make_binary_of(a,
                     make_node(CLUX_NODE_STMT_BLOCK, "..."),
                     make_node(CLUX_NODE_SYMBOL, "[]"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *length_result = exec_expr_binary(ctx, bad_length);
  ASSERT_NE(length_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(length_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(length_result),
               "unsupported node kind: STMT_BLOCK");
  delete_node(&bad_length);

  node_t *bad_element =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "2"),
                     make_node(CLUX_NODE_SYMBOL, "[]"),
                     make_node(CLUX_NODE_STMT_BLOCK, "..."));
  clux_value_t *element_result = exec_expr_binary(ctx, bad_element);
  ASSERT_NE(element_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(element_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(element_result),
               "unsupported node kind: STMT_BLOCK");
  delete_node(&bad_element);
}

/* 场景: 端到端 var x: *i32 = undefined（类型标注是前缀类型节点）
   预期: stmt_var 类型标注通道消费前缀节点产物，创建 *i32 的 TDZ 变量，
         scope 按名可查到同一指针 */
TEST_F(ExecLiteralTest, VarUndefinedWithPointerTypeAnnotation) {
  node_t *type_node =
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "*"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *var_node = create_stmt_var_node(a);
  stmt_var_node_set_keyword(var_node, make_node(CLUX_NODE_SYMBOL, "var"));
  stmt_var_node_set_name(var_node,
                         make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tx"));
  stmt_var_node_set_type(var_node, type_node);
  stmt_var_node_set_value(
      var_node, make_node(CLUX_NODE_LITERAL_UNDEFINED, kUndefinedText));
  clux_value_t *result = exec_stmt_var(ctx, var_node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_PTR)
      << "annotation must type the TDZ variable as *i32";
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_get_ptr_type(
                ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32)));
  EXPECT_TRUE(clux_value_is_tdz(result))
      << "undefined initializer must produce a TDZ-marked variable";
  EXPECT_EQ(clux_context_lookup(ctx, "tx", NULL), result);
  delete_node(&var_node);
}

/* 场景: 端到端 var a: [2]i32 = undefined（类型标注是数组类型节点）
   预期: 创建 [2]i32 的 TDZ 变量，类型与引擎数组通道恒等 */
TEST_F(ExecLiteralTest, VarUndefinedWithArrayTypeAnnotation) {
  node_t *type_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "2"),
                     make_node(CLUX_NODE_SYMBOL, "[]"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *var_node = create_stmt_var_node(a);
  stmt_var_node_set_keyword(var_node, make_node(CLUX_NODE_SYMBOL, "var"));
  stmt_var_node_set_name(var_node,
                         make_node(CLUX_NODE_LITERAL_IDENTIFIER, "ta"));
  stmt_var_node_set_type(var_node, type_node);
  stmt_var_node_set_value(
      var_node, make_node(CLUX_NODE_LITERAL_UNDEFINED, kUndefinedText));
  clux_value_t *result = exec_stmt_var(ctx, var_node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ARRAY)
      << "annotation must type the TDZ variable as [2]i32";
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_get_array_type(
                ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32), 2));
  EXPECT_STREQ(clux_type_get_name(clux_value_get_type(result)), "[2]i32");
  EXPECT_TRUE(clux_value_is_tdz(result));
  EXPECT_EQ(clux_context_lookup(ctx, "ta", NULL), result);
  delete_node(&var_node);
}
