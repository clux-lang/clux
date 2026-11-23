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
#include "engine/error.h"
#include "engine/func.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/tuple.h"
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
#include "parser/node_decorator.h"
#include "parser/node_slice.h"
#include "parser/node_func_capture.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_func_type.h"
#include "parser/node_call.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_union.h"
#include "parser/node_stmt_enum.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_type.h"
#include "parser/node_struct_decl.h"
#include "parser/node_struct_member.h"
#include "parser/node_union.h"
#include "parser/node_enum_decl.h"
#include "parser/node_enum_variant.h"
#include "engine/enum.h"
#include "parser/node_field.h"
#include "parser/node_interface_decl.h"
#include "parser/node_stmt_interface.h"
#include "parser/node_generic_param.h"
#include "engine/interface_type.h"
#include "engine/struct.h"
#include "engine/scope.h"
#include "exec/call.h"
#include "parser/node_construct.h"
#include "parser/node_construct_field.h"
#include "parser/node_rest.h"
#include "parser/node_subscript.h"
#include "parser/node_tuple.h"
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

/* 场景: opaque 类型已移除，"opaque" 不再是内建类型名
   预期: 返回 error 值——类型擦除边界改由 *void 复合类型承载 */
TEST_F(ExecLiteralTest, IdentifierOpaqueNameRemoved) {
  node_t *node = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "opaque");
  clux_value_t *value = exec_literal_identifier(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value),
               "use of undeclared identifier 'opaque'");
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

/* 场景: 逐一触发一个未注册 kind 的精确名字（node_kind_name 的 STMT_EMPTY case）
   预期: 错误消息包含 kind 的准确名字 */
TEST_F(ExecLiteralTest, ExecNodeUnsupportedKindNameStmtEmpty) {
  node_t *node = make_node(CLUX_NODE_STMT_EMPTY, "...");
  clux_value_t *value = exec_node(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "unsupported node kind: STMT_EMPTY");
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
        kind == CLUX_NODE_EXPR_SLICE || kind == CLUX_NODE_EXPR_TUPLE ||
        kind == CLUX_NODE_EXPR_FUNC_TYPE || kind == CLUX_NODE_EXPR_FUNC_DECL ||
        kind == CLUX_NODE_EXPR_CALL || kind == CLUX_NODE_STMT_BLOCK ||
        kind == CLUX_NODE_STMT_EXPR || kind == CLUX_NODE_STMT_RETURN ||
        kind == CLUX_NODE_STMT_VAR || kind == CLUX_NODE_STMT_FUNC ||
        kind == CLUX_NODE_EXPR_UNION || kind == CLUX_NODE_STMT_UNION ||
        kind == CLUX_NODE_EXPR_ENUM_DECL || kind == CLUX_NODE_STMT_ENUM ||
        kind == CLUX_NODE_EXPR_INTERFACE_DECL ||
        kind == CLUX_NODE_STMT_INTERFACE || kind == CLUX_NODE_STMT_TYPE ||
        kind == CLUX_NODE_EXPR_STRUCT_DECL || kind == CLUX_NODE_STMT_STRUCT ||
        kind == CLUX_NODE_EXPR_CONSTRUCT;
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

/* 场景: base 子节点求值为 error（未注册 kind STMT_EMPTY）
   预期: error 透传——"unsupported node kind: STMT_EMPTY" */
TEST_F(ExecLiteralTest, SubscriptPropagatesBaseError) {
  node_t *node = make_subscript_of(make_node(CLUX_NODE_STMT_EMPTY, "..."),
                                   make_node(CLUX_NODE_LITERAL_NUMERIC, "1"));
  clux_value_t *value = exec_expr_subscript(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "unsupported node kind: STMT_EMPTY");
  delete_node(&node);
}

/* 场景: index 子节点求值为 error（未注册 kind STMT_EMPTY），base 正常
   预期: index 的 error 透传——"unsupported node kind: STMT_EMPTY" */
TEST_F(ExecLiteralTest, SubscriptPropagatesIndexError) {
  ASSERT_NE(make_i32_array("arr_e", 4), nullptr);

  node_t *node =
      make_subscript_of(make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_e"),
                        make_node(CLUX_NODE_STMT_EMPTY, "..."));
  clux_value_t *value = exec_expr_subscript(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "unsupported node kind: STMT_EMPTY");
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

/* 场景: base 子节点求值为 error（未注册 kind STMT_EMPTY）
   预期: error 透传——"unsupported node kind: STMT_EMPTY" */
TEST_F(ExecLiteralTest, SlicePropagatesBaseError) {
  node_t *node = make_slice_of("bad[1:3]",
                               make_node(CLUX_NODE_STMT_EMPTY, "..."),
                               make_node(CLUX_NODE_LITERAL_NUMERIC, "1"),
                               make_node(CLUX_NODE_LITERAL_NUMERIC, "3"));
  clux_value_t *value = exec_expr_slice(ctx, node);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(value), "unsupported node kind: STMT_EMPTY");
  delete_node(&node);
}

/* 场景: lo/hi 子节点求值为 error（未注册 kind STMT_EMPTY），base 正常
   预期: 端点 error 透传——"unsupported node kind: STMT_EMPTY"。
         base 成功在先，lo 先于 hi 求值 */
TEST_F(ExecLiteralTest, SlicePropagatesEndpointErrors) {
  ASSERT_NE(make_i32_array("arr_pe", 4), nullptr);

  node_t *lo_node =
      make_slice_of("arr_pe[bad:3]",
                    make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_pe"),
                    make_node(CLUX_NODE_STMT_EMPTY, "..."),
                    make_node(CLUX_NODE_LITERAL_NUMERIC, "3"));
  clux_value_t *lo_value = exec_expr_slice(ctx, lo_node);
  ASSERT_NE(lo_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(lo_value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(lo_value), "unsupported node kind: STMT_EMPTY");
  delete_node(&lo_node);

  node_t *hi_node =
      make_slice_of("arr_pe[1:bad]",
                    make_node(CLUX_NODE_LITERAL_IDENTIFIER, "arr_pe"),
                    make_node(CLUX_NODE_LITERAL_NUMERIC, "1"),
                    make_node(CLUX_NODE_STMT_EMPTY, "..."));
  clux_value_t *hi_value = exec_expr_slice(ctx, hi_node);
  ASSERT_NE(hi_value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(hi_value)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(hi_value), "unsupported node kind: STMT_EMPTY");
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

/* 场景: 类型前缀操作数子节点求值出错（RIGHT=STMT_EMPTY 裸节点）
   预期: error 透传——"unsupported node kind: STMT_EMPTY" */
TEST_F(ExecLiteralTest, TypePrefixPropagatesOperandError) {
  node_t *node = make_binary_of(a,
                                NULL,
                                make_node(CLUX_NODE_SYMBOL, "*"),
                                make_node(CLUX_NODE_STMT_EMPTY, "..."));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "unsupported node kind: STMT_EMPTY");
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
   预期: 均报 "unsupported node kind: STMT_EMPTY" */
TEST_F(ExecLiteralTest, ArrayPropagatesLengthAndElementErrors) {
  node_t *bad_length =
      make_binary_of(a,
                     make_node(CLUX_NODE_STMT_EMPTY, "..."),
                     make_node(CLUX_NODE_SYMBOL, "[]"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *length_result = exec_expr_binary(ctx, bad_length);
  ASSERT_NE(length_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(length_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(length_result),
               "unsupported node kind: STMT_EMPTY");
  delete_node(&bad_length);

  node_t *bad_element =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "2"),
                     make_node(CLUX_NODE_SYMBOL, "[]"),
                     make_node(CLUX_NODE_STMT_EMPTY, "..."));
  clux_value_t *element_result = exec_expr_binary(ctx, bad_element);
  ASSERT_NE(element_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(element_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(element_result),
               "unsupported node kind: STMT_EMPTY");
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

/* ---- exec_expr_func_type / exec_expr_tuple：类型定义表达式 ---- */

/* 合成函数类型结点 func|captures|(params) -> ret（三个子结点由调用方构造；
   captures / ret 允许为 NULL 走无捕获与缺返回类型路径，params 须为
   FUNC_PARAMS 结点，delete 根结点时一并释放）。 */
static node_t *make_func_type_of(allocator_t *allocator,
                                 node_t *captures,
                                 node_t *params,
                                 node_t *ret) {
  node_t *node = create_func_type_node(allocator);
  func_type_node_set_captures(node, captures);
  func_type_node_set_params(node, params);
  func_type_node_set_return(node, ret);
  return node;
}

/* 合成捕获项 name:type（name/type 为 NULL 时走缺名/缺类型容错路径，
   delete 捕获列表根结点时一并释放）。 */
static node_t *
make_capture_of(allocator_t *allocator, node_t *name, node_t *type) {
  node_t *node = create_func_capture_node(allocator);
  func_capture_node_set_name(node, name);
  func_capture_node_set_type(node, type);
  return node;
}

/* 场景: func(i32, str) -> bool 基本形态
   预期: 产出 type value（unwrap 为 TYPE），被描述类型 kind FUNC，签名
         （捕获表/参数序列/返回类型）与引擎 clux_context_create_func_type
         通道结构恒等（指针相同），exec_node 分派命中同一求值器 */
TEST_F(ExecLiteralTest, FuncTypeBuildsTypeValue) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *str_type = clux_context_builtin_type(ctx, CLUX_TYPE_STR);
  clux_type_t *bool_type = clux_context_builtin_type(ctx, CLUX_TYPE_BOOL);

  node_t *params = create_func_params_node(a);
  func_params_node_add_param(params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  func_params_node_add_param(params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "str"));
  node_t *node = make_func_type_of(
      a, NULL, params, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "bool"));
  clux_value_t *result = exec_expr_func_type(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE)
      << "func type expression must produce a type value";
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_FUNC);
  EXPECT_EQ(clux_func_capture_count(described), (size_t)0);
  EXPECT_EQ(clux_func_param_count(described), (size_t)2);
  EXPECT_EQ(clux_func_get_param(described, 0), i32_type);
  EXPECT_EQ(clux_func_get_param(described, 1), str_type);
  EXPECT_EQ(clux_func_get_return_type(described), bool_type);
  clux_type_t *engine_params[] = {i32_type, str_type};
  clux_type_t *expected = clux_context_create_func_type(
      ctx, "func", NULL, 0, engine_params, 2, bool_type);
  EXPECT_EQ(described, expected)
      << "exec func type must be interned with the engine channel";
  delete_node(&node);

  /* exec_node(CLUX_NODE_EXPR_FUNC_TYPE) 分派到 exec_expr_func_type，
     相同签名经结构去重恒等返回同一实例 */
  node_t *params_again = create_func_params_node(a);
  func_params_node_add_param(params_again,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  func_params_node_add_param(params_again,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "str"));
  node_t *dispatch_node = make_func_type_of(
      a, NULL, params_again, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "bool"));
  clux_value_t *dispatched = exec_node(ctx, dispatch_node);
  ASSERT_NE(dispatched, nullptr);
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(dispatched)),
            CLUX_TYPE_TYPE);
  clux_type_t *dispatch_described = type_described_by(dispatched);
  ASSERT_NE(dispatch_described, nullptr);
  EXPECT_EQ(dispatch_described, described)
      << "exec_node dispatch must hit the same interned func type";
  delete_node(&dispatch_node);
}

/* 场景: func|a:i32, b:*i32|(i32) -> i32 捕获列表是签名的一部分
   预期: 捕获表按名可查、按位置可取；与无捕获 func(i32)->i32 类型不等；
         相同签名二次求值与引擎通道均结构去重恒等 */
TEST_F(ExecLiteralTest, FuncTypeCapturesArePartOfSignature) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);

  node_t *captures = create_func_captures_node(a);
  func_captures_node_add_capture(
      captures,
      make_capture_of(a,
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"),
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_captures_node_add_capture(
      captures,
      make_capture_of(
          a,
          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "b"),
          make_binary_of(a,
                         NULL,
                         make_node(CLUX_NODE_SYMBOL, "*"),
                         make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"))));
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *node = make_func_type_of(
      a, captures, params, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *result = exec_expr_func_type(ctx, node);
  ASSERT_NE(result, nullptr);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_func_capture_count(described), (size_t)2);
  const clux_func_capture_t *capture_a = clux_func_find_capture(described, "a");
  ASSERT_NE(capture_a, nullptr);
  EXPECT_EQ(capture_a->type, i32_type);
  const clux_func_capture_t *capture_b = clux_func_find_capture(described, "b");
  ASSERT_NE(capture_b, nullptr);
  EXPECT_EQ(capture_b->type, ptr_i32);
  EXPECT_EQ(clux_func_find_capture(described, "z"), nullptr)
      << "unknown capture name must not be found";
  delete_node(&node);

  /* 捕获表不同（无捕获）即不同类型 */
  node_t *bare_params = create_func_params_node(a);
  func_params_node_add_param(bare_params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *bare = make_func_type_of(
      a, NULL, bare_params, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *bare_result = exec_expr_func_type(ctx, bare);
  ASSERT_NE(bare_result, nullptr);
  clux_type_t *bare_described = type_described_by(bare_result);
  ASSERT_NE(bare_described, nullptr);
  EXPECT_FALSE(clux_type_equal(described, bare_described))
      << "captures are part of the func type signature";
  delete_node(&bare);

  /* 相同签名二次求值经结构去重恒等 */
  node_t *captures2 = create_func_captures_node(a);
  func_captures_node_add_capture(
      captures2,
      make_capture_of(a,
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"),
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_captures_node_add_capture(
      captures2,
      make_capture_of(
          a,
          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "b"),
          make_binary_of(a,
                         NULL,
                         make_node(CLUX_NODE_SYMBOL, "*"),
                         make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"))));
  node_t *params2 = create_func_params_node(a);
  func_params_node_add_param(params2,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *again = make_func_type_of(
      a, captures2, params2, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *again_result = exec_expr_func_type(ctx, again);
  ASSERT_NE(again_result, nullptr);
  EXPECT_EQ(type_described_by(again_result), described)
      << "two evaluations of the same signature must be interned";
  delete_node(&again);

  /* 与引擎通道恒等 */
  clux_func_capture_t engine_captures[] = {{"a", i32_type}, {"b", ptr_i32}};
  clux_type_t *engine_params[] = {i32_type};
  clux_type_t *expected = clux_context_create_func_type(
      ctx, "func", engine_captures, 2, engine_params, 1, i32_type);
  EXPECT_EQ(described, expected);
}

/* 场景: func([]i32, [2]str) -> ?i32 复合参数与返回类型
   预期: 参数/返回可为任意类型表达式（切片/定长数组/可选），逐项与
         引擎对应通道恒等 */
TEST_F(ExecLiteralTest, FuncTypeCompositeParamsAndReturn) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *str_type = clux_context_builtin_type(ctx, CLUX_TYPE_STR);

  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "[]"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_params_node_add_param(
      params,
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "2"),
                     make_node(CLUX_NODE_SYMBOL, "[]"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  node_t *node = make_func_type_of(
      a,
      NULL,
      params,
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "?"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *result = exec_expr_func_type(ctx, node);
  ASSERT_NE(result, nullptr);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_FUNC);
  EXPECT_EQ(clux_func_param_count(described), (size_t)2);
  EXPECT_EQ(clux_func_get_param(described, 0),
            clux_context_get_slice_type(ctx, i32_type));
  EXPECT_EQ(clux_func_get_param(described, 1),
            clux_context_get_array_type(ctx, str_type, 2));
  EXPECT_EQ(clux_func_get_return_type(described),
            clux_context_get_optional_type(ctx, i32_type));
  delete_node(&node);
}

/* 场景: func() -> func(i32) -> i32 函数类型的函数类型（嵌套返回）
   预期: RETURN 槽位是完整的 func_type 结点，外层签名递归求值；内层
         实例与引擎通道恒等 */
TEST_F(ExecLiteralTest, FuncTypeFuncReturnType) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);

  node_t *inner_params = create_func_params_node(a);
  func_params_node_add_param(inner_params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *inner = make_func_type_of(
      a, NULL, inner_params, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  /* 外层无参数：PARAMS 槽位允许为 NULL（等价空参数表） */
  node_t *outer = make_func_type_of(a, NULL, NULL, inner);
  clux_value_t *result = exec_expr_func_type(ctx, outer);
  ASSERT_NE(result, nullptr);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_FUNC);
  EXPECT_EQ(clux_func_param_count(described), (size_t)0);
  clux_type_t *nested_return = clux_func_get_return_type(described);
  ASSERT_NE(nested_return, nullptr);
  EXPECT_EQ(clux_type_get_kind(nested_return), CLUX_TYPE_FUNC)
      << "nested return must itself be a func type";
  clux_type_t *engine_params[] = {i32_type};
  clux_type_t *expected_inner = clux_context_create_func_type(
      ctx, "func", NULL, 0, engine_params, 1, i32_type);
  EXPECT_EQ(nested_return, expected_inner)
      << "nested func type must be interned with the engine channel";
  delete_node(&outer);
}

/* 场景: 操作数不是类型表达式——func(1)->i32 / func(i32)->"s" /
        捕获类型 a:1
   预期: 三处均报 "expected a type expression, got '<类型名>'"，
         捕获错误路径与参数/返回错误路径一致 */
TEST_F(ExecLiteralTest, FuncTypeOperandNotTypeValueErrors) {
  /* 参数位置 */
  node_t *bad_params = create_func_params_node(a);
  func_params_node_add_param(bad_params,
                             make_node(CLUX_NODE_LITERAL_NUMERIC, "1"));
  node_t *bad_param = make_func_type_of(
      a, NULL, bad_params, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *param_result = exec_expr_func_type(ctx, bad_param);
  ASSERT_NE(param_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(param_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(param_result),
               "expected a type expression, got 'i32'");
  delete_node(&bad_param);

  /* 返回类型位置（str 字面量的值类型是 str） */
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *bad_return = make_func_type_of(
      a, NULL, params, make_node(CLUX_NODE_LITERAL_STRING, "\"s\""));
  clux_value_t *return_result = exec_expr_func_type(ctx, bad_return);
  ASSERT_NE(return_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(return_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(return_result),
               "expected a type expression, got 'str'");
  delete_node(&bad_return);

  /* 捕获类型位置 */
  node_t *captures = create_func_captures_node(a);
  func_captures_node_add_capture(
      captures,
      make_capture_of(a,
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"),
                      make_node(CLUX_NODE_LITERAL_NUMERIC, "1")));
  node_t *params2 = create_func_params_node(a);
  func_params_node_add_param(params2,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *bad_capture = make_func_type_of(
      a, captures, params2, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *capture_result = exec_expr_func_type(ctx, bad_capture);
  ASSERT_NE(capture_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(capture_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(capture_result),
               "expected a type expression, got 'i32'");
  delete_node(&bad_capture);
}

/* 场景: 两个同名捕获 a:i32 与 a:*i32
   预期: 引擎校验前拦截，报 "duplicate capture 'a'" */
TEST_F(ExecLiteralTest, FuncTypeDuplicateCaptureErrors) {
  node_t *captures = create_func_captures_node(a);
  func_captures_node_add_capture(
      captures,
      make_capture_of(a,
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"),
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_captures_node_add_capture(
      captures,
      make_capture_of(
          a,
          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"),
          make_binary_of(a,
                         NULL,
                         make_node(CLUX_NODE_SYMBOL, "*"),
                         make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"))));
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *node = make_func_type_of(
      a, captures, params, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *result = exec_expr_func_type(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "duplicate capture 'a'");
  delete_node(&node);
}

/* 场景: 捕获缺名（只有类型）/ 捕获缺类型（只有名）
   预期: 分别报 "capture %zu requires a name and a type" 与
         "capture '%s' requires a type" */
TEST_F(ExecLiteralTest, FuncTypeCaptureMissingNameOrTypeErrors) {
  /* 缺名：name 槽位为 NULL */
  node_t *unnamed_captures = create_func_captures_node(a);
  func_captures_node_add_capture(
      unnamed_captures,
      make_capture_of(a, NULL, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *unnamed =
      make_func_type_of(a,
                        unnamed_captures,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *unnamed_result = exec_expr_func_type(ctx, unnamed);
  ASSERT_NE(unnamed_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(unnamed_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(unnamed_result),
               "capture 0 requires a name and a type");
  delete_node(&unnamed);

  /* 缺类型：type 槽位为 NULL */
  node_t *untyped_captures = create_func_captures_node(a);
  func_captures_node_add_capture(
      untyped_captures,
      make_capture_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"), NULL));
  node_t *params2 = create_func_params_node(a);
  func_params_node_add_param(params2,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *untyped =
      make_func_type_of(a,
                        untyped_captures,
                        params2,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *untyped_result = exec_expr_func_type(ctx, untyped);
  ASSERT_NE(untyped_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(untyped_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(untyped_result), "capture 'a' requires a type");
  delete_node(&untyped);
}

/* 场景: 参数表含 rest（变参）/ 缺返回类型
   预期: 分别报 "variadic function type is not supported yet" 与
         "function type requires a return type" */
TEST_F(ExecLiteralTest, FuncTypeRestParamAndMissingReturnErrors) {
  /* rest 参数：params 表中混入 CLUX_NODE_EXPR_REST 结点 */
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  func_params_node_add_param(params, make_node(CLUX_NODE_EXPR_REST, "..."));
  node_t *rest_node = make_func_type_of(
      a, NULL, params, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *rest_result = exec_expr_func_type(ctx, rest_node);
  ASSERT_NE(rest_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(rest_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(rest_result),
               "variadic function type is not supported yet");
  delete_node(&rest_node);

  /* 缺返回类型：RETURN 槽位为 NULL */
  node_t *params2 = create_func_params_node(a);
  func_params_node_add_param(params2,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *no_return = make_func_type_of(a, NULL, params2, NULL);
  clux_value_t *no_return_result = exec_expr_func_type(ctx, no_return);
  ASSERT_NE(no_return_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(no_return_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(no_return_result),
               "function type requires a return type");
  delete_node(&no_return);
}

/* 场景: <i32, str> 基本形态
   预期: 产出 type value，被描述类型 kind TUPLE、名字 "<i32, str>"，
         元素序与引擎 get_tuple_type 通道恒等，二次求值与 exec_node
         分派均命中同一 interned 实例 */
TEST_F(ExecLiteralTest, TupleBuildsTypeValue) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *str_type = clux_context_builtin_type(ctx, CLUX_TYPE_STR);

  node_t *tuple = create_tuple_node(a);
  tuple_node_add_elem(tuple, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  tuple_node_add_elem(tuple, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "str"));
  clux_value_t *result = exec_expr_tuple(ctx, tuple);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE)
      << "tuple type expression must produce a type value";
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_TUPLE);
  EXPECT_STREQ(clux_type_get_name(described), "<i32, str>");
  EXPECT_EQ(clux_tuple_elem_count(described), (size_t)2);
  EXPECT_EQ(clux_tuple_get_elem(described, 0), i32_type);
  EXPECT_EQ(clux_tuple_get_elem(described, 1), str_type);
  clux_type_t *engine_elems[] = {i32_type, str_type};
  clux_type_t *expected = clux_context_get_tuple_type(ctx, engine_elems, 2);
  EXPECT_EQ(described, expected)
      << "exec tuple must be interned with the engine channel";
  delete_node(&tuple);

  /* 相同元素序列二次求值 interned 恒等 */
  node_t *again = create_tuple_node(a);
  tuple_node_add_elem(again, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  tuple_node_add_elem(again, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "str"));
  clux_value_t *again_result = exec_expr_tuple(ctx, again);
  ASSERT_NE(again_result, nullptr);
  EXPECT_EQ(type_described_by(again_result), described);
  delete_node(&again);

  /* exec_node(CLUX_NODE_EXPR_TUPLE) 分派到 exec_expr_tuple */
  node_t *third = create_tuple_node(a);
  tuple_node_add_elem(third, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  tuple_node_add_elem(third, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "str"));
  clux_value_t *dispatched = exec_node(ctx, third);
  ASSERT_NE(dispatched, nullptr);
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(dispatched)),
            CLUX_TYPE_TYPE);
  EXPECT_EQ(type_described_by(dispatched), described)
      << "exec_node dispatch must hit the same interned tuple type";
  delete_node(&third);
}

/* 场景: 嵌套 <<i32>, i32>、限定/指针元素 <const i32, *i32>、空元组 <>
   预期: 元素可为任意类型表达式（嵌套 tuple / 限定 / 指针），逐项与
         引擎通道恒等；空元组合法（size 0，与引擎空元组恒等） */
TEST_F(ExecLiteralTest, TupleNestedAndQualifiedElems) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);

  /* 嵌套 <<i32>, i32>：首个元素是完整的 tuple 结点 */
  node_t *inner = create_tuple_node(a);
  tuple_node_add_elem(inner, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *nested = create_tuple_node(a);
  tuple_node_add_elem(nested, inner);
  tuple_node_add_elem(nested, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *nested_result = exec_expr_tuple(ctx, nested);
  ASSERT_NE(nested_result, nullptr);
  clux_type_t *nested_described = type_described_by(nested_result);
  ASSERT_NE(nested_described, nullptr);
  EXPECT_EQ(clux_type_get_kind(nested_described), CLUX_TYPE_TUPLE);
  clux_type_t *engine_inner[] = {i32_type};
  EXPECT_EQ(clux_tuple_get_elem(nested_described, 0),
            clux_context_get_tuple_type(ctx, engine_inner, 1))
      << "nested tuple element must be interned with the engine channel";
  EXPECT_EQ(clux_tuple_get_elem(nested_described, 1), i32_type);
  delete_node(&nested);

  /* <const i32, *i32>：限定与指针元素 */
  node_t *qualified = create_tuple_node(a);
  tuple_node_add_elem(
      qualified,
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "const"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  tuple_node_add_elem(
      qualified,
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "*"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *qualified_result = exec_expr_tuple(ctx, qualified);
  ASSERT_NE(qualified_result, nullptr);
  clux_type_t *qualified_described = type_described_by(qualified_result);
  ASSERT_NE(qualified_described, nullptr);
  EXPECT_EQ(clux_tuple_get_elem(qualified_described, 0),
            clux_type_get_qualified_type(i32_type, CLUX_TYPE_CONST));
  EXPECT_EQ(clux_tuple_get_elem(qualified_described, 1),
            clux_context_get_ptr_type(ctx, i32_type));
  delete_node(&qualified);

  /* 空元组 <>：空聚合 seal 兜底 size 至 1 */
  node_t *empty = create_tuple_node(a);
  clux_value_t *empty_result = exec_expr_tuple(ctx, empty);
  ASSERT_NE(empty_result, nullptr);
  clux_type_t *empty_described = type_described_by(empty_result);
  ASSERT_NE(empty_described, nullptr);
  EXPECT_EQ(clux_type_get_kind(empty_described), CLUX_TYPE_TUPLE);
  EXPECT_EQ(clux_tuple_elem_count(empty_described), (size_t)0);
  EXPECT_EQ(clux_type_get_size(empty_described), (size_t)1);
  EXPECT_EQ(empty_described, clux_context_get_tuple_type(ctx, NULL, 0));
  delete_node(&empty);
}

/* 场景: 元素不是类型表达式 <i32, 1> / 元素是参数包 <...Ts>
   预期: 分别报 "expected a type expression, got 'i32'" 与
         "generic parameter pack is not supported yet" */
TEST_F(ExecLiteralTest, TupleElemErrors) {
  /* 非类型元素 */
  node_t *bad = create_tuple_node(a);
  tuple_node_add_elem(bad, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  tuple_node_add_elem(bad, make_node(CLUX_NODE_LITERAL_NUMERIC, "1"));
  clux_value_t *bad_result = exec_expr_tuple(ctx, bad);
  ASSERT_NE(bad_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_result),
               "expected a type expression, got 'i32'");
  delete_node(&bad);

  /* rest 元素（泛型参数包） */
  node_t *packed = create_tuple_node(a);
  tuple_node_add_elem(packed, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  tuple_node_add_elem(packed, make_node(CLUX_NODE_EXPR_REST, "...Ts"));
  clux_value_t *packed_result = exec_expr_tuple(ctx, packed);
  ASSERT_NE(packed_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(packed_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(packed_result),
               "generic parameter pack is not supported yet");
  delete_node(&packed);
}

/* 场景: 端到端 var tf: func(i32)->i32 = undefined 与
        var tt: <i32, str> = undefined（类型标注是新式类型表达式结点）
   预期: stmt_var 类型标注通道消费两个求值器的产物，创建对应类型的
         TDZ 变量，类型与引擎通道恒等，scope 按名可查到同一指针 */
TEST_F(ExecLiteralTest, VarUndefinedWithFuncAndTupleAnnotations) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *str_type = clux_context_builtin_type(ctx, CLUX_TYPE_STR);

  /* func 标注 */
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *func_annotation = make_func_type_of(
      a, NULL, params, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *func_var = create_stmt_var_node(a);
  stmt_var_node_set_keyword(func_var, make_node(CLUX_NODE_SYMBOL, "var"));
  stmt_var_node_set_name(func_var,
                         make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tf"));
  stmt_var_node_set_type(func_var, func_annotation);
  stmt_var_node_set_value(
      func_var, make_node(CLUX_NODE_LITERAL_UNDEFINED, kUndefinedText));
  clux_value_t *func_result = exec_stmt_var(ctx, func_var);
  ASSERT_NE(func_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(func_result)),
            CLUX_TYPE_FUNC)
      << "annotation must type the TDZ variable as func(i32)->i32";
  clux_type_t *engine_params[] = {i32_type};
  EXPECT_EQ(clux_value_get_type(func_result),
            clux_context_create_func_type(
                ctx, "func", NULL, 0, engine_params, 1, i32_type));
  EXPECT_TRUE(clux_value_is_tdz(func_result));
  EXPECT_EQ(clux_context_lookup(ctx, "tf", NULL), func_result);
  delete_node(&func_var);

  /* tuple 标注 */
  node_t *tuple_annotation = create_tuple_node(a);
  tuple_node_add_elem(tuple_annotation,
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  tuple_node_add_elem(tuple_annotation,
                      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "str"));
  node_t *tuple_var = create_stmt_var_node(a);
  stmt_var_node_set_keyword(tuple_var, make_node(CLUX_NODE_SYMBOL, "var"));
  stmt_var_node_set_name(tuple_var,
                         make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tt"));
  stmt_var_node_set_type(tuple_var, tuple_annotation);
  stmt_var_node_set_value(
      tuple_var, make_node(CLUX_NODE_LITERAL_UNDEFINED, kUndefinedText));
  clux_value_t *tuple_result = exec_stmt_var(ctx, tuple_var);
  ASSERT_NE(tuple_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(tuple_result)),
            CLUX_TYPE_TUPLE)
      << "annotation must type the TDZ variable as <i32, str>";
  clux_type_t *engine_elems[] = {i32_type, str_type};
  EXPECT_EQ(clux_value_get_type(tuple_result),
            clux_context_get_tuple_type(ctx, engine_elems, 2));
  EXPECT_TRUE(clux_value_is_tdz(tuple_result));
  EXPECT_EQ(clux_context_lookup(ctx, "tt", NULL), tuple_result);
  delete_node(&tuple_var);
}

/* ---- exec_expr_func_decl：函数定义表达式 ---- */

/* 合成函数定义结点 func|captures| name[generics](params): ret {body}
   （七槽：captures/generics/params/name/ret/body 由调用方构造并允许为
   NULL 走各容错路径，KEYWORD 槽 exec 不读故不填，delete 根结点时一并
   释放全部子结点）。 */
static node_t *make_func_decl_of(allocator_t *allocator,
                                 node_t *captures,
                                 node_t *generics,
                                 node_t *params,
                                 node_t *name,
                                 node_t *ret,
                                 node_t *body) {
  node_t *node = create_func_decl_node(allocator);
  func_decl_node_set_captures(node, captures);
  func_decl_node_set_generics(node, generics);
  func_decl_node_set_params(node, params);
  func_decl_node_set_name(node, name);
  func_decl_node_set_return(node, ret);
  func_decl_node_set_body(node, body);
  return node;
}

/* 合成带名形参 name: type（name/type 为 NULL 时走缺名/缺类型容错路径，
   delete 形参列表根结点时一并释放）。 */
static node_t *
make_named_param_of(allocator_t *allocator, node_t *name, node_t *type) {
  node_t *node = create_func_param_node(allocator);
  func_param_node_set_name(node, name);
  func_param_node_set_type(node, type);
  return node;
}

/* 场景: func add(a: i32, b: i32): i32 {body} 基本形态
   预期: 返回函数值（值类型 kind FUNC），签名（参数序列/返回类型/捕获表）
         与引擎 clux_context_create_func_type 通道结构恒等（指针相同），
         函数声明 AST 借用挂接可读回（clux_func_get_ast 返回 decl 结点），
         按名可从作用域 lookup 到同一值 */
TEST_F(ExecLiteralTest, FuncDeclBuildsFunctionValue) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);

  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "b"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *body = make_node(CLUX_NODE_STMT_BLOCK, "{ a + b }");
  node_t *node =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "add"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *result = exec_expr_func_decl(ctx, node);
  ASSERT_NE(result, nullptr);
  clux_type_t *func_type = clux_value_get_type(result);
  ASSERT_NE(func_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(func_type), CLUX_TYPE_FUNC)
      << "function definition must produce a func-typed value";
  EXPECT_EQ(clux_func_capture_count(func_type), (size_t)0);
  EXPECT_EQ(clux_func_param_count(func_type), (size_t)2);
  EXPECT_EQ(clux_func_get_param(func_type, 0), i32_type);
  EXPECT_EQ(clux_func_get_param(func_type, 1), i32_type);
  EXPECT_EQ(clux_func_get_return_type(func_type), i32_type);
  EXPECT_EQ(clux_func_get_ast(result), node)
      << "decl AST must be attached (borrowed) on the function value";
  EXPECT_EQ(clux_context_lookup(ctx, "add", NULL), result)
      << "named function must be reachable from the current scope";
  clux_type_t *engine_params[] = {i32_type, i32_type};
  EXPECT_EQ(func_type,
            clux_context_create_func_type(
                ctx, "func", NULL, 0, engine_params, 2, i32_type))
      << "implicit func type must be interned with the engine channel";
  delete_node(&node);
}

/* 场景: 同签名隐含类型与显式类型表达式恒等——
        func add(a: i32, b: i32): i32{} 的值类型 vs 显式
        func(i32, i32) -> i32 类型表达式的被描述类型
   预期: 定义隐含生成的函数类型与显式 EXPR_FUNC_TYPE 产出的 type value
         描述同一 clux_type_t*（指针恒等，类型名不参与去重） */
TEST_F(ExecLiteralTest, FuncDeclImplicitTypeIdenticalToFuncType) {
  /* 显式函数类型 func(i32, i32) -> i32 */
  node_t *type_params = create_func_params_node(a);
  func_params_node_add_param(type_params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  func_params_node_add_param(type_params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *type_node = make_func_type_of(
      a, NULL, type_params, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *type_result = exec_expr_func_type(ctx, type_node);
  ASSERT_NE(type_result, nullptr);
  clux_type_t *explicit_type = type_described_by(type_result);
  ASSERT_NE(explicit_type, nullptr);

  /* 函数定义隐含生成的函数类型 */
  node_t *decl_params = create_func_params_node(a);
  func_params_node_add_param(
      decl_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_params_node_add_param(
      decl_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "b"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        decl_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_add"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *decl_result = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(decl_result, nullptr);
  EXPECT_EQ(clux_value_get_type(decl_result), explicit_type)
      << "implicit func type of a definition must be pointer-identical "
         "with the explicit func type expression of the same signature";
  delete_node(&decl);
  delete_node(&type_node);
}

/* 场景: 匿名函数 func(x: i32): i32 {body}（NAME 槽为 NULL）
   预期: 成功创建函数值（kind FUNC，AST 挂接），匿名不进命名查找——
         lookup 任意名均报未声明错误 */
TEST_F(ExecLiteralTest, FuncDeclAnonymousSucceedsWithoutNameLookup) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *body = make_node(CLUX_NODE_STMT_BLOCK, "{}");
  node_t *node =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        NULL,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *result = exec_expr_func_decl(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_FUNC)
      << "anonymous function definition must still succeed";
  EXPECT_EQ(clux_func_get_ast(result), node);
  clux_value_t *probe = clux_context_lookup(ctx, "fd_anon", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(probe)), CLUX_TYPE_ERROR)
      << "anonymous function must not be reachable by name";
  EXPECT_STREQ(error_message_of(probe),
               "use of undeclared identifier 'fd_anon'");
  delete_node(&node);
}

/* 场景: func |n: i32, acc: *i32| step(x: i32): i32 {body} 带捕获列表
   预期: 捕获名+类型正确进签名（按名可查、按位置可取），与引擎通道
         结构恒等（指针相同） */
TEST_F(ExecLiteralTest, FuncDeclCapturesArePartOfSignature) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);

  /* 定义点捕获快照：先在作用域备好被捕获的同名现值 */
  clux_value_t *seed_n = clux_context_create_i32(ctx, 1, "n", NULL);
  ASSERT_NE(seed_n, nullptr);
  clux_value_t *seed_acc = clux_context_create_ptr(
      ctx, ptr_i32, clux_value_get_data(seed_n), "acc", NULL);
  ASSERT_NE(seed_acc, nullptr);

  /* 捕获列表：CAPTURES 的唯一子节点是含两个 FUNC_PARAM 的 FUNC_PARAMS */
  node_t *capture_entries = create_func_params_node(a);
  func_params_node_add_param(
      capture_entries,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "n"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_params_node_add_param(
      capture_entries,
      make_named_param_of(
          a,
          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "acc"),
          make_binary_of(a,
                         NULL,
                         make_node(CLUX_NODE_SYMBOL, "*"),
                         make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"))));
  node_t *captures = create_func_captures_node(a);
  func_captures_node_add_capture(captures, capture_entries);

  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *node =
      make_func_decl_of(a,
                        captures,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "step"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *result = exec_expr_func_decl(ctx, node);
  ASSERT_NE(result, nullptr);
  clux_type_t *func_type = clux_value_get_type(result);
  ASSERT_NE(func_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(func_type), CLUX_TYPE_FUNC);
  EXPECT_EQ(clux_func_capture_count(func_type), (size_t)2);
  const clux_func_capture_t *capture_n = clux_func_find_capture(func_type, "n");
  ASSERT_NE(capture_n, nullptr);
  EXPECT_EQ(capture_n->type, i32_type);
  const clux_func_capture_t *capture_acc =
      clux_func_find_capture(func_type, "acc");
  ASSERT_NE(capture_acc, nullptr);
  EXPECT_EQ(capture_acc->type, ptr_i32);
  clux_func_capture_t engine_captures[] = {{"n", i32_type}, {"acc", ptr_i32}};
  clux_type_t *engine_params[] = {i32_type};
  EXPECT_EQ(func_type,
            clux_context_create_func_type(
                ctx, "func", engine_captures, 2, engine_params, 1, i32_type))
      << "capture entries must be interned with the engine channel";
  EXPECT_EQ(clux_context_lookup(ctx, "step", NULL), result);
  delete_node(&node);
}

/* 场景: 缺返回类型 / 缺函数体 / GENERICS 槽非 NULL
   预期: 分别报 "function definition requires a return type" /
         "function definition requires a body" /
         "generic function is not supported yet" */
TEST_F(ExecLiteralTest, FuncDeclMissingReturnBodyAndGenericsErrors) {
  /* 缺返回类型：RETURN 槽为 NULL */
  node_t *no_return_params = create_func_params_node(a);
  func_params_node_add_param(no_return_params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *no_return =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        no_return_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_nr"),
                        NULL,
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *no_return_result = exec_expr_func_decl(ctx, no_return);
  ASSERT_NE(no_return_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(no_return_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(no_return_result),
               "function definition requires a return type");
  delete_node(&no_return);

  /* 缺函数体：BODY 槽为 NULL */
  node_t *no_body_params = create_func_params_node(a);
  func_params_node_add_param(no_body_params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *no_body =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        no_body_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_nb"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        NULL);
  clux_value_t *no_body_result = exec_expr_func_decl(ctx, no_body);
  ASSERT_NE(no_body_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(no_body_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(no_body_result),
               "function definition requires a body");
  delete_node(&no_body);

  /* 泛型参数：GENERICS 槽非 NULL */
  node_t *generic_params = create_func_params_node(a);
  func_params_node_add_param(generic_params,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *generics = make_node(CLUX_NODE_EXPR_TUPLE, "<T>");
  node_t *generic_node =
      make_func_decl_of(a,
                        NULL,
                        generics,
                        generic_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_g"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *generic_result = exec_expr_func_decl(ctx, generic_node);
  ASSERT_NE(generic_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(generic_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(generic_result),
               "generic function is not supported yet");
  delete_node(&generic_node);
}

/* 场景: rest 参数 / 捕获表内非 FUNC_PARAM（泛型捕获）/ 形参缺类型节点 /
        形参类型与返回类型非 type value
   预期: 分别报 "variadic function parameter is not supported yet" /
         "generic capture is not supported yet" /
         "parameter %zu requires a type" /
         "expected a type expression, got '<类型名>'" */
TEST_F(ExecLiteralTest, FuncDeclRestGenericCaptureAndParamTypeErrors) {
  /* rest 参数：PARAMS 中混入 CLUX_NODE_EXPR_REST 结点 */
  node_t *rest_params = create_func_params_node(a);
  func_params_node_add_param(
      rest_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_params_node_add_param(rest_params,
                             make_node(CLUX_NODE_EXPR_REST, "..."));
  node_t *rest_node =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        rest_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_rest"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *rest_result = exec_expr_func_decl(ctx, rest_node);
  ASSERT_NE(rest_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(rest_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(rest_result),
               "variadic function parameter is not supported yet");
  delete_node(&rest_node);

  /* 泛型捕获：CAPTURES 里的 FUNC_PARAMS 混入裸类型表达式（非 FUNC_PARAM） */
  node_t *generic_entries = create_func_params_node(a);
  func_params_node_add_param(generic_entries,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "T"));
  node_t *generic_captures = create_func_captures_node(a);
  func_captures_node_add_capture(generic_captures, generic_entries);
  node_t *generic_capture_node =
      make_func_decl_of(a,
                        generic_captures,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_gc"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *generic_capture_result =
      exec_expr_func_decl(ctx, generic_capture_node);
  ASSERT_NE(generic_capture_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(generic_capture_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(generic_capture_result),
               "generic capture is not supported yet");
  delete_node(&generic_capture_node);

  /* 形参缺类型节点：FUNC_PARAM 的 TYPE 槽为 NULL */
  node_t *untyped_params = create_func_params_node(a);
  func_params_node_add_param(
      untyped_params,
      make_named_param_of(
          a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"), NULL));
  node_t *untyped_node =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        untyped_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_ut"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *untyped_result = exec_expr_func_decl(ctx, untyped_node);
  ASSERT_NE(untyped_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(untyped_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(untyped_result), "parameter 0 requires a type");
  delete_node(&untyped_node);

  /* 形参类型非 type value：TYPE 槽是数值字面量 */
  node_t *bad_param_params = create_func_params_node(a);
  func_params_node_add_param(
      bad_param_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"),
                          make_node(CLUX_NODE_LITERAL_NUMERIC, "1")));
  node_t *bad_param_node =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        bad_param_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_bp"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *bad_param_result = exec_expr_func_decl(ctx, bad_param_node);
  ASSERT_NE(bad_param_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_param_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_param_result),
               "expected a type expression, got 'i32'");
  delete_node(&bad_param_node);

  /* 返回类型非 type value：RETURN 槽是 str 字面量 */
  node_t *bad_return_node =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_br"),
                        make_node(CLUX_NODE_LITERAL_STRING, "\"s\""),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *bad_return_result = exec_expr_func_decl(ctx, bad_return_node);
  ASSERT_NE(bad_return_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_return_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_return_result),
               "expected a type expression, got 'str'");
  delete_node(&bad_return_node);
}

/* 场景: 捕获缺名 / 捕获重名 / 捕获缺类型 / 捕获类型非 type value
   预期: 分别报 "capture %zu requires a name and a type" /
         "duplicate capture '%s'" / "capture '%s' requires a type" /
         "expected a type expression, got '<类型名>'"（与 func_type 共用
         公共校验通道） */
TEST_F(ExecLiteralTest, FuncDeclCaptureValidationErrors) {
  /* 缺名：FUNC_PARAM 的 NAME 槽为 NULL */
  node_t *unnamed_entries = create_func_params_node(a);
  func_params_node_add_param(
      unnamed_entries,
      make_named_param_of(
          a, NULL, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *unnamed_captures = create_func_captures_node(a);
  func_captures_node_add_capture(unnamed_captures, unnamed_entries);
  node_t *unnamed =
      make_func_decl_of(a,
                        unnamed_captures,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_cn"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *unnamed_result = exec_expr_func_decl(ctx, unnamed);
  ASSERT_NE(unnamed_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(unnamed_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(unnamed_result), "capture 0 requires a name");
  delete_node(&unnamed);

  /* 重名：两个同名捕获 c */
  node_t *dup_entries = create_func_params_node(a);
  func_params_node_add_param(
      dup_entries,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "c"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_params_node_add_param(
      dup_entries,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "c"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *dup_captures = create_func_captures_node(a);
  func_captures_node_add_capture(dup_captures, dup_entries);
  node_t *dup_node =
      make_func_decl_of(a,
                        dup_captures,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_cd"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *dup_result = exec_expr_func_decl(ctx, dup_node);
  ASSERT_NE(dup_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(dup_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(dup_result), "duplicate capture 'c'");
  delete_node(&dup_node);

  /* 无标注捕获 |c| 且作用域无同名变量：定义路径按定义点作用域查询
     现值推断类型，查询失败报上下文查找错误 */
  node_t *untyped_entries = create_func_params_node(a);
  func_params_node_add_param(
      untyped_entries,
      make_named_param_of(
          a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "c"), NULL));
  node_t *untyped_captures = create_func_captures_node(a);
  func_captures_node_add_capture(untyped_captures, untyped_entries);
  node_t *untyped =
      make_func_decl_of(a,
                        untyped_captures,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_ct"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *untyped_result = exec_expr_func_decl(ctx, untyped);
  ASSERT_NE(untyped_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(untyped_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(untyped_result),
               "use of undeclared identifier 'c'");
  delete_node(&untyped);

  /* 捕获类型非 type value：TYPE 槽是数值字面量 */
  node_t *bad_type_entries = create_func_params_node(a);
  func_params_node_add_param(
      bad_type_entries,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "c"),
                          make_node(CLUX_NODE_LITERAL_NUMERIC, "1")));
  node_t *bad_type_captures = create_func_captures_node(a);
  func_captures_node_add_capture(bad_type_captures, bad_type_entries);
  node_t *bad_type_node =
      make_func_decl_of(a,
                        bad_type_captures,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_cb"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *bad_type_result = exec_expr_func_decl(ctx, bad_type_node);
  ASSERT_NE(bad_type_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_type_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_type_result),
               "expected a type expression, got 'i32'");
  delete_node(&bad_type_node);
}

/* 场景: 同名函数重复定义（同一作用域 define 两次）
   预期: 第一次成功，第二次报 "duplicate declaration 'fd_dup'"，
         lookup 仍返回第一个函数值 */
TEST_F(ExecLiteralTest, FuncDeclDuplicateNameErrors) {
  node_t *first_params = create_func_params_node(a);
  func_params_node_add_param(
      first_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *first =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        first_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_dup"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *first_result = exec_expr_func_decl(ctx, first);
  ASSERT_NE(first_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(first_result)),
            CLUX_TYPE_FUNC);
  delete_node(&first);

  /* 同名第二次定义：类型不同（两参数）也因重名被拒 */
  node_t *second_params = create_func_params_node(a);
  func_params_node_add_param(
      second_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_params_node_add_param(
      second_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "b"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *second =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        second_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_dup"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *second_result = exec_expr_func_decl(ctx, second);
  ASSERT_NE(second_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(second_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(second_result),
               "duplicate declaration 'fd_dup'");
  delete_node(&second);

  /* 首个函数保持原状：lookup 仍返回第一个值，签名未被第二次定义破坏 */
  clux_value_t *lookup = clux_context_lookup(ctx, "fd_dup", NULL);
  ASSERT_NE(lookup, nullptr);
  EXPECT_EQ(lookup, first_result);
  EXPECT_EQ(clux_func_param_count(clux_value_get_type(lookup)), (size_t)1);
}

/* 场景: 无标注捕获 |ic|——作用域先有 ic: i32 = 7
   预期: 以查得现值的类型推断捕获类型（i32 builtin），exec_expr_func_decl
         成功产出函数值且捕获槽已种入定义点现值 7 */
TEST_F(ExecLiteralTest, FuncDeclUnannotatedCaptureInfersTypeFromScopeValue) {
  clux_value_t *outer = clux_context_create_i32(ctx, 7, "ic", NULL);
  ASSERT_NE(outer, nullptr);

  node_t *capture_params = create_func_params_node(a);
  func_params_node_add_param(
      capture_params,
      make_named_param_of(
          a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "ic"), NULL));
  node_t *captures = create_func_captures_node(a);
  func_captures_node_add_capture(captures, capture_params);
  node_t *decl =
      make_func_decl_of(a,
                        captures,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_inf"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(func)), CLUX_TYPE_FUNC)
      << "unannotated capture must infer the type from the scope value";

  clux_type_t *func_type = clux_value_get_type(func);
  const clux_func_capture_t *capture = clux_func_find_capture(func_type, "ic");
  ASSERT_NE(capture, nullptr);
  EXPECT_EQ(capture->type, clux_context_builtin_type(ctx, CLUX_TYPE_I32))
      << "inferred capture type must be the scope value's i32";

  clux_value_t *seeded = clux_context_func_capture(ctx, func, "ic", NULL);
  ASSERT_NE(seeded, nullptr);
  EXPECT_EQ(read_i32(seeded), 7)
      << "capture slot must hold the definition-point snapshot of the scope "
         "value";
  delete_node(&decl);
}

/* 场景: 带标注 |mm: i32| 但定义点作用域同名值是 bool
   预期: 捕获类型照常经 exec_operand_type 求值为 i32；写入初值时
         assign 的 safe_cast 拦截 bool→i32，定义失败返回 error */
TEST_F(ExecLiteralTest, FuncDeclAnnotatedCaptureTypeMismatchErrors) {
  clux_value_t *outer = clux_context_create_bool(ctx, true, "mm", NULL);
  ASSERT_NE(outer, nullptr);

  node_t *capture_params = create_func_params_node(a);
  func_params_node_add_param(
      capture_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "mm"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *captures = create_func_captures_node(a);
  func_captures_node_add_capture(captures, capture_params);
  node_t *decl =
      make_func_decl_of(a,
                        captures,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_mm"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *result = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR)
      << "safe_cast must reject snapping a bool scope value into an i32 "
         "capture";
  delete_node(&decl);
}

/* 场景: 默认值捕获 |dv: i32 = 30 + 12|——DEFAULT 槽填数值表达式 AST，
        作用域无同名变量
   预期: 初值取默认表达式求值结果（不查作用域），定义成功且捕获槽为 42 */
TEST_F(ExecLiteralTest, FuncDeclCaptureDefaultExpressionUsedAsInitialValue) {
  node_t *capture_param =
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "dv"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  func_param_node_set_default(
      capture_param,
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "30"),
                     make_node(CLUX_NODE_SYMBOL, "+"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "12")));
  node_t *capture_params = create_func_params_node(a);
  func_params_node_add_param(capture_params, capture_param);
  node_t *captures = create_func_captures_node(a);
  func_captures_node_add_capture(captures, capture_params);
  node_t *decl =
      make_func_decl_of(a,
                        captures,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_dv"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(func)), CLUX_TYPE_FUNC)
      << "a capture with a default expression must not require a scope value";

  clux_value_t *view = clux_context_func_capture(ctx, func, "dv", NULL);
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(read_i32(view), 42)
      << "the capture slot must hold the evaluated default expression";
  delete_node(&decl);
}

/* 场景: 无标注捕获源是 TDZ 变量（已创建未初始化的 tz_f: i32）
   预期: consume_value 校验拒绝，定义失败返回 error，消息精确为
         "use of a value before initialization" */
TEST_F(ExecLiteralTest, FuncDeclCaptureFromTdzSourceErrors) {
  clux_value_t *tz_src = clux_context_create_i32(ctx, 0, "tz_f", NULL);
  ASSERT_NE(tz_src, nullptr);
  clux_value_set_tdz(tz_src, true);

  node_t *capture_params = create_func_params_node(a);
  func_params_node_add_param(
      capture_params,
      make_named_param_of(
          a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tz_f"), NULL));
  node_t *captures = create_func_captures_node(a);
  func_captures_node_add_capture(captures, capture_params);
  node_t *decl =
      make_func_decl_of(a,
                        captures,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "fd_tz"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        make_node(CLUX_NODE_STMT_BLOCK, "{}"));
  clux_value_t *result = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of a value before initialization");
  delete_node(&decl);
}

/* ---- exec：函数调用（栈帧协议） ---- */

/* 合成调用结点 callee(args...)：callee 为 NULL 走容错路径，args 逐个
   add，argc 由结点自身记录。delete 根结点时一并释放全部子结点。 */
static node_t *make_call_of(allocator_t *allocator,
                            node_t *callee,
                            size_t argc,
                            node_t **args) {
  node_t *node = create_call_node(allocator);
  call_node_set_callee(node, callee);
  for (size_t idx = 0; idx < argc; idx++) {
    call_node_add_arg(node, args[idx]);
  }
  return node;
}

/* 场景: func f(x: i32): i32 { return x; } 调用 f(7)
   预期: 首次 exec_expr_call 前执行器未安装（懒安装），调用后已安装；
         调用产出 clone 出帧的 i32 值 7 */
TEST_F(ExecLiteralTest, CallReturnsValueFromFunction) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *body = create_stmt_block_node(a);
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(ret, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"));
  stmt_block_node_add_stmt(body, ret);
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(func)), CLUX_TYPE_FUNC);

  EXPECT_EQ(clux_context_get_executor(ctx), nullptr)
      << "executor must be lazily installed on first call";
  node_t *call_arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "7");
  node_t *call = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"), 1, &call_arg);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_I32)
      << "call must return the payload cloned out of the frame";
  EXPECT_EQ(read_i32(result), 7);
  EXPECT_NE(clux_context_get_executor(ctx), nullptr)
      << "executor must be installed after the first call";
  delete_node(&call);
  delete_node(&decl);
}

/* ---- 场景: 栈帧协议——形参 copy / 静态作用域 / 捕获写穿 / return 三形态
   / missing return / 深度护栏 / 校验错误路径 / 分派语句（block/expr/return） ---- */

/* 场景: func pc(x: i32): i32 { x = 99; return x; } 以调用方变量调用
   预期: 形参按 copy 克隆进帧——体内对形参赋值生效（返回 99）、调用方
         变量不受影响（仍为 7）、再次调用仍从实参重新克隆（返回 99） */
TEST_F(ExecLiteralTest, CallClonesArgumentsIntoFrame) {
  clux_value_t *caller_v = clux_context_create_i32(ctx, 7, "pc_v", NULL);
  ASSERT_NE(caller_v, nullptr);

  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(
      body, make_assign(CLUX_NODE_LITERAL_NUMERIC, "=", "x", "99"));
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(ret, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"));
  stmt_block_node_add_stmt(body, ret);
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pc"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(func)), CLUX_TYPE_FUNC);

  node_t *arg = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pc_v");
  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pc"), 1, &arg);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(read_i32(result), 99) << "body must see the frame-local copy";

  clux_value_t *after = clux_context_lookup(ctx, "pc_v", NULL);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(read_i32(after), 7)
      << "argument is copied into the frame; caller value must not change";

  node_t *arg2 = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pc_v");
  node_t *call2 =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pc"), 1, &arg2);
  clux_value_t *result2 = exec_expr_call(ctx, call2);
  ASSERT_NE(result2, nullptr);
  EXPECT_EQ(read_i32(result2), 99) << "each call clones its arguments afresh";
  delete_node(&call);
  delete_node(&call2);
  delete_node(&decl);
}

/* 场景: 静态作用域——帧挂 global 而非调用方：
        global 定义 gv、g0 返回 gv；push 作用域后其中定义 lv、f2 返回 lv
   预期: f2() 报 "use of undeclared identifier 'lv'"（调用方局部名帧内
         不可见——那是动态作用域漏洞）；g0() 返回 11（global 名经挂链
         可达），pop 后 g0() 依然返回 11 */
TEST_F(ExecLiteralTest, CallFrameUsesStaticScoping) {
  clux_value_t *gv = clux_context_create_i32(ctx, 11, "gv", NULL);
  ASSERT_NE(gv, nullptr);

  /* g0: func g0(): i32 { return gv; } —— 定义在 global */
  node_t *g0_body = create_stmt_block_node(a);
  node_t *g0_ret = create_stmt_return_node(a);
  stmt_return_node_set_value(g0_ret,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "gv"));
  stmt_block_node_add_stmt(g0_body, g0_ret);
  node_t *g0 = make_func_decl_of(a,
                                 NULL,
                                 NULL,
                                 create_func_params_node(a),
                                 make_node(CLUX_NODE_LITERAL_IDENTIFIER, "g0"),
                                 make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                                 g0_body);
  clux_value_t *g0_func = exec_expr_func_decl(ctx, g0);
  ASSERT_NE(g0_func, nullptr);

  clux_context_push_scope(ctx);
  clux_value_t *lv = clux_context_create_i32(ctx, 5, "lv", NULL);
  ASSERT_NE(lv, nullptr);

  /* f2: func f2(): i32 { return lv; } —— 定义在 push 出的作用域 */
  node_t *f2_body = create_stmt_block_node(a);
  node_t *f2_ret = create_stmt_return_node(a);
  stmt_return_node_set_value(f2_ret,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "lv"));
  stmt_block_node_add_stmt(f2_body, f2_ret);
  node_t *f2 = make_func_decl_of(a,
                                 NULL,
                                 NULL,
                                 create_func_params_node(a),
                                 make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f2"),
                                 make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                                 f2_body);
  clux_value_t *f2_func = exec_expr_func_decl(ctx, f2);
  ASSERT_NE(f2_func, nullptr);

  node_t *f2_call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f2"), 0, NULL);
  clux_value_t *leaked = exec_expr_call(ctx, f2_call);
  ASSERT_NE(leaked, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(leaked)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(leaked), "use of undeclared identifier 'lv'")
      << "caller-local names must not leak into the frame";

  node_t *g0_call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "g0"), 0, NULL);
  clux_value_t *via_global = exec_expr_call(ctx, g0_call);
  ASSERT_NE(via_global, nullptr);
  EXPECT_EQ(read_i32(via_global), 11)
      << "module-level names stay reachable through the global chain";

  clux_context_pop_scope(ctx);
  clux_value_t *after_pop = exec_expr_call(ctx, g0_call);
  ASSERT_NE(after_pop, nullptr);
  EXPECT_EQ(read_i32(after_pop), 11);
  delete_node(&f2_call);
  delete_node(&g0_call);
  delete_node(&f2);
  delete_node(&g0);
}

/* 场景: 捕获写穿——func|c: i32| wc(): i32 { c = 42; return c; }
        调用后经 clux_context_func_capture 读到 42
   预期: 帧内对捕获名的赋值写穿函数值存储；外层同名变量不受影响（捕获
         存储属函数值，与外层变量相互独立） */
TEST_F(ExecLiteralTest, CallWritesThroughCaptures) {
  clux_value_t *outer_c = clux_context_create_i32(ctx, 1, "c", NULL);
  ASSERT_NE(outer_c, nullptr);

  node_t *captures = create_func_captures_node(a);
  node_t *capture_params = create_func_params_node(a);
  func_params_node_add_param(
      capture_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "c"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_captures_node_add_capture(captures, capture_params);
  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(
      body, make_assign(CLUX_NODE_LITERAL_NUMERIC, "=", "c", "42"));
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(ret, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "c"));
  stmt_block_node_add_stmt(body, ret);
  node_t *decl =
      make_func_decl_of(a,
                        captures,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "wc"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(func)), CLUX_TYPE_FUNC);

  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "wc"), 0, NULL);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(read_i32(result), 42) << "body sees the capture view";

  clux_value_t *written = clux_context_func_capture(ctx, func, "c", NULL);
  ASSERT_NE(written, nullptr);
  EXPECT_EQ(read_i32(written), 42)
      << "capture assignment must write through to the function's storage";

  clux_value_t *after = clux_context_lookup(ctx, "c", NULL);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(read_i32(after), 1)
      << "capture storage is independent of the outer variable";
  delete_node(&call);
  delete_node(&decl);
}

/* 场景: 定义点快照时机与独立性（无标注路径）——外层 sc = 1，无标注
        捕获 |sc|，闭包体赋 42 后调用
   预期: 无标注捕获在定义点快照外层现值 1；体内赋值写穿到函数捕获槽
         （调用返回 42、capture view 为 42），外层变量保持定义点原值 1 */
TEST_F(ExecLiteralTest, FuncDeclUnannotatedCaptureSnapshotsAtDefinitionPoint) {
  clux_value_t *outer = clux_context_create_i32(ctx, 1, "sc", NULL);
  ASSERT_NE(outer, nullptr);

  node_t *capture_params = create_func_params_node(a);
  func_params_node_add_param(
      capture_params,
      make_named_param_of(
          a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "sc"), NULL));
  node_t *captures = create_func_captures_node(a);
  func_captures_node_add_capture(captures, capture_params);
  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(
      body, make_assign(CLUX_NODE_LITERAL_NUMERIC, "=", "sc", "42"));
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(ret,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "sc"));
  stmt_block_node_add_stmt(body, ret);
  node_t *decl =
      make_func_decl_of(a,
                        captures,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "sc_fn"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(func)), CLUX_TYPE_FUNC);

  node_t *call = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "sc_fn"), 0, NULL);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(read_i32(result), 42) << "body sees the captured snapshot, then "
                                     "writes 42 through to the capture slot";

  clux_value_t *view = clux_context_func_capture(ctx, func, "sc", NULL);
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(read_i32(view), 42)
      << "in-body assignment must write through to the function's storage";

  clux_value_t *after = clux_context_lookup(ctx, "sc", NULL);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(read_i32(after), 1)
      << "the outer variable keeps its definition-point value";
  delete_node(&call);
  delete_node(&decl);
}

/* 场景: return; 无值形态——func nv(): i32 { return; }
   预期: 调用产出 NULL（无载荷 signal 在帧出口解包为 NULL，
         按无返回值函数处理） */
TEST_F(ExecLiteralTest, CallReturnWithoutValueYieldsNull) {
  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(body, create_stmt_return_node(a));
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nv"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);

  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nv"), 0, NULL);
  clux_value_t *result = exec_expr_call(ctx, call);
  EXPECT_EQ(result, nullptr)
      << "void return unwraps to NULL at the frame boundary";
  delete_node(&call);
  delete_node(&decl);
}

/* 场景: return 从嵌套块上抛——func nb(x: i32): i32 { { return x; } }
   预期: signal 穿越块出口（载荷 clone 再包 signal）后到帧边界解包，
         返回 7 */
TEST_F(ExecLiteralTest, CallReturnFromNestedBlock) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *inner = create_stmt_block_node(a);
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(ret, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"));
  stmt_block_node_add_stmt(inner, ret);
  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(body, inner);
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nb"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);

  node_t *arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "7");
  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nb"), 1, &arg);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_I32);
  EXPECT_EQ(read_i32(result), 7);
  delete_node(&call);
  delete_node(&decl);
}

/* 场景: missing return——函数体 fallthrough（有产出但非 return）与空体
        两种形态，具名函数错误消息带函数名
   预期: func mrf(x: i32): i32 { x } 报 "missing return in function 'mrf'"；
         func mre(): i32 {} 报 "missing return in function 'mre'" */
TEST_F(ExecLiteralTest, CallMissingReturnReportsFunctionName) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *fallthrough_body = create_stmt_block_node(a);
  node_t *expr_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(expr_stmt,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"));
  stmt_block_node_add_stmt(fallthrough_body, expr_stmt);
  node_t *mrf =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "mrf"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        fallthrough_body);
  clux_value_t *mrf_func = exec_expr_func_decl(ctx, mrf);
  ASSERT_NE(mrf_func, nullptr);

  node_t *mrf_arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "1");
  node_t *mrf_call = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "mrf"), 1, &mrf_arg);
  clux_value_t *result = exec_expr_call(ctx, mrf_call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "missing return in function 'mrf'");

  node_t *mre =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "mre"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        create_stmt_block_node(a));
  clux_value_t *mre_func = exec_expr_func_decl(ctx, mre);
  ASSERT_NE(mre_func, nullptr);
  node_t *mre_call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "mre"), 0, NULL);
  clux_value_t *empty_result = exec_expr_call(ctx, mre_call);
  ASSERT_NE(empty_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(empty_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(empty_result),
               "missing return in function 'mre'");
  delete_node(&mrf_call);
  delete_node(&mre_call);
  delete_node(&mrf);
  delete_node(&mre);
}

/* 场景: 匿名函数的 missing return——类型隐含名为 "func"（decl 无名）
   预期: 匿名函数值经 declare_value 具名化后可按名调用，空体 fallthrough
         报 "missing return in function 'func'" */
TEST_F(ExecLiteralTest, CallMissingReturnImplicitTypeName) {
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        create_func_params_node(a),
                        NULL,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        create_stmt_block_node(a));
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(func)), CLUX_TYPE_FUNC);

  clux_value_t *named = clux_context_declare_value(ctx, func, "anon_fn", NULL);
  ASSERT_NE(named, nullptr);
  EXPECT_EQ(named, func) << "anonymous owning value must be named in place";

  node_t *call = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "anon_fn"), 0, NULL);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "missing return in function 'func'")
      << "unnamed decl interns its type under the implicit name";
  delete_node(&call);
  delete_node(&decl);
}

/* 场景: 深度护栏——CLUX_MAX_CALL_DEPTH=3 的 ctx 上无限递归
   预期: 第 4 次 enter false，报 "maximum call depth exceeded" 完整传播
         到顶层调用表达式；配对的 leave 让调用深度归零。env 只在
         create_clux_context 读取，用例内自建 ctx，结束还原 env */
TEST_F(ExecLiteralTest, CallDepthGuardStopsRunawayRecursion) {
  const char *prev = getenv("CLUX_MAX_CALL_DEPTH");
#ifdef _WIN32
  _putenv_s("CLUX_MAX_CALL_DEPTH", "3");
#else
  setenv("CLUX_MAX_CALL_DEPTH", "3", 1);
#endif
  allocator_t *a2 = create_allocator(malloc, free);
  clux_context_t *ctx2 = create_clux_context(a2);

  /* f: func f(x: i32): i32 { return f(x); } —— body 内调用自身 */
  node_t *body = create_stmt_block_node(a2);
  node_t *self_arg = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x");
  node_t *self_call = make_call_of(
      a2, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"), 1, &self_arg);
  node_t *ret = create_stmt_return_node(a2);
  stmt_return_node_set_value(ret, self_call);
  stmt_block_node_add_stmt(body, ret);
  node_t *params = create_func_params_node(a2);
  func_params_node_add_param(
      params,
      make_named_param_of(a2,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl =
      make_func_decl_of(a2,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *func = exec_expr_func_decl(ctx2, decl);
  EXPECT_NE(func, nullptr);

  node_t *arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "1");
  node_t *call =
      make_call_of(a2, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"), 1, &arg);
  clux_value_t *result = exec_expr_call(ctx2, call);
  EXPECT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "maximum call depth exceeded");
  EXPECT_EQ(clux_context_get_call_depth(ctx2), (size_t)0)
      << "enter/leave must stay paired after the guard trips";

  delete_node(&call);
  delete_node(&decl);
  delete_clux_context(&ctx2);
  delete_allocator(&a2);
  if (prev) {
#ifdef _WIN32
    _putenv_s("CLUX_MAX_CALL_DEPTH", prev);
#else
    setenv("CLUX_MAX_CALL_DEPTH", prev, 1);
#endif
  } else {
#ifdef _WIN32
    _putenv_s("CLUX_MAX_CALL_DEPTH", "");
#else
    unsetenv("CLUX_MAX_CALL_DEPTH");
#endif
  }
}

/* 场景: spread 实参——f(…x)（EXPR_REST 实参）
   预期: 报 "spread argument is not supported yet" */
TEST_F(ExecLiteralTest, CallRejectsSpreadArgument) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "spf"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        create_stmt_block_node(a));
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);

  node_t *args[] = {make_node(CLUX_NODE_EXPR_REST, "x")};
  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "spf"), 1, args);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "spread argument is not supported yet");
  delete_node(&call);
  delete_node(&decl);
}

/* 场景: 无 callee 的调用结点
   预期: 报 "function call requires a callee" */
TEST_F(ExecLiteralTest, CallRequiresACallee) {
  node_t *call = make_call_of(a, NULL, 0, NULL);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "function call requires a callee");
  delete_node(&call);
}

/* 场景: callee 求值失败——调用未定义的函数名
   预期: 报 "use of undeclared identifier 'nope_fn'"（error 优先传播） */
TEST_F(ExecLiteralTest, CallPropagatesCalleeError) {
  node_t *call = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nope_fn"), 0, NULL);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'nope_fn'");
  delete_node(&call);
}

/* 场景: 实参求值失败——实参是未定义标识符
   预期: 报 "use of undeclared identifier 'nope_arg'"（实参 error 透传） */
TEST_F(ExecLiteralTest, CallPropagatesArgumentError) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "aef"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        create_stmt_block_node(a));
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);

  node_t *args[] = {make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nope_arg")};
  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "aef"), 1, args);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'nope_arg'");
  delete_node(&call);
  delete_node(&decl);
}

/* 场景: 实参个数不匹配——单参函数零实参调用
   预期: 报 "function expects 1 argument, got 0"（单数形式） */
TEST_F(ExecLiteralTest, CallRejectsWrongArgCount) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "acf"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        create_stmt_block_node(a));
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);

  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "acf"), 0, NULL);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "function expects 1 argument, got 0");
  delete_node(&call);
  delete_node(&decl);
}

/* 场景: 实参类型不匹配——i32 形参传 bool
   预期: 报 "function argument 0 requires type 'i32', got 'bool'" */
TEST_F(ExecLiteralTest, CallRejectsArgumentTypeMismatch) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "atf"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        create_stmt_block_node(a));
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);

  node_t *args[] = {make_node(CLUX_NODE_LITERAL_BOOLEAN, "true")};
  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "atf"), 1, args);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "function argument 0 requires type 'i32', got 'bool'");
  delete_node(&call);
  delete_node(&decl);
}

/* 场景: 无函数体 AST——engine 通道构造的 func 值（零值，无 AST）经
        exec 调用链路调用
   预期: 报 "function call has no executable environment attached" */
TEST_F(ExecLiteralTest, CallWithoutAstHasNoExecutableEnvironment) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *ft =
      clux_context_create_func_type(ctx, "nhf", NULL, 0, NULL, 0, i32_type);
  ASSERT_NE(ft, nullptr);
  clux_value_t *raw = clux_context_create_func(ctx, ft, NULL, NULL);
  ASSERT_NE(raw, nullptr);
  clux_value_t *named = clux_context_declare_value(ctx, raw, "nh", NULL);
  ASSERT_NE(named, nullptr);

  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nh"), 0, NULL);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "function call has no executable environment attached");
  delete_node(&call);
}

/* 场景: AST kind 校验——函数值的 AST 被换成非 EXPR_FUNC_DECL 结点
   预期: 报 "function AST is not a function definition" */
TEST_F(ExecLiteralTest, CallRejectsNonFuncDeclAst) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "kaf"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        create_stmt_block_node(a));
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);

  node_t *bogus = make_node(CLUX_NODE_STMT_BLOCK, "{}");
  EXPECT_EQ(clux_context_func_set_ast(ctx, func, bogus, NULL), func);
  node_t *arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "1");
  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "kaf"), 1, &arg);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "function AST is not a function definition");
  delete_node(&call);
  delete_node(&bogus);
  delete_node(&decl);
}

/* 场景: 无名形参——定义期只读 TYPE 槽可过，调用帧绑定时报错
   预期: 报 "parameter 0 requires a name" */
TEST_F(ExecLiteralTest, CallRejectsUnnamedParameter) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(
          a, NULL, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "upf"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        create_stmt_block_node(a));
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr)
      << "definition-only path reads the TYPE slot, unnamed param passes";

  node_t *arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "7");
  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "upf"), 1, &arg);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "parameter 0 requires a name");
  delete_node(&call);
  delete_node(&decl);
}

/* 场景: return signal 顶层逃逸——STMT_RETURN 在调用帧之外直接执行
   预期: 产出 return signal（语句层不消费，唯一消费点是帧边界，
         顶层逃逸按语义原样上抛），载荷可读 */
TEST_F(ExecLiteralTest, TopLevelReturnSignalEscapes) {
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(ret, make_node(CLUX_NODE_LITERAL_NUMERIC, "9"));
  clux_value_t *result = exec_node(ctx, ret);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(clux_value_is_return_signal(result))
      << "top-level return escapes as a signal (not an error)";
  clux_value_t *payload = clux_return_signal_get_payload(result);
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(read_i32(payload), 9);
  delete_node(&ret);
}

/* 场景: 空块——STMT_BLOCK 无语句
   预期: NULL（无值；空块的唯一合法消费点是调用帧边界的 fallthrough） */
TEST_F(ExecLiteralTest, StmtBlockEmptyReturnsNull) {
  node_t *block = create_stmt_block_node(a);
  clux_value_t *result = exec_stmt_block(ctx, block);
  EXPECT_EQ(result, nullptr);
  delete_node(&block);
}

/* 场景: 块值语义——最后语句值 clone 出块（块作用域销毁后仍可读）
   预期: { 5 } 返回 clone 的 i32 5；{ 6; 7 } 返回最后语句值 7 */
TEST_F(ExecLiteralTest, StmtBlockClonesLastStatementValue) {
  node_t *block = create_stmt_block_node(a);
  node_t *expr_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(expr_stmt, make_node(CLUX_NODE_LITERAL_NUMERIC, "5"));
  stmt_block_node_add_stmt(block, expr_stmt);
  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_I32);
  EXPECT_EQ(read_i32(result), 5) << "value survives block scope deletion";
  delete_node(&block);

  node_t *two = create_stmt_block_node(a);
  node_t *first = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(first, make_node(CLUX_NODE_LITERAL_NUMERIC, "6"));
  node_t *last = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(last, make_node(CLUX_NODE_LITERAL_NUMERIC, "7"));
  stmt_block_node_add_stmt(two, first);
  stmt_block_node_add_stmt(two, last);
  clux_value_t *last_result = exec_stmt_block(ctx, two);
  ASSERT_NE(last_result, nullptr);
  EXPECT_EQ(read_i32(last_result), 7) << "last statement wins";
  delete_node(&two);
}

/* 场景: 块内 error 终止——首语句求值失败后续语句不执行
   预期: 报 "use of undeclared identifier 'nowhere'" */
TEST_F(ExecLiteralTest, StmtBlockStopsOnError) {
  node_t *block = create_stmt_block_node(a);
  node_t *bad = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(bad,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nowhere"));
  node_t *good = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(good, make_node(CLUX_NODE_LITERAL_NUMERIC, "7"));
  stmt_block_node_add_stmt(block, bad);
  stmt_block_node_add_stmt(block, good);
  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'nowhere'");
  delete_node(&block);
}

/* 场景: 表达式语句空槽——STMT_EXPR 的 EXPR 槽为 NULL
   预期: 报 "expression statement requires an expression" */
TEST_F(ExecLiteralTest, StmtExprRequiresExpression) {
  node_t *node = create_stmt_expr_node(a);
  clux_value_t *result = exec_stmt_expr(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "expression statement requires an expression");
  delete_node(&node);
}

/* 场景: return 语句两形态——带载荷 / return;（VALUE 槽 NULL）
   预期: 带 payload 的 signal 载荷可读；无值形态 payload 为 NULL */
TEST_F(ExecLiteralTest, StmtReturnCarriesOptionalPayload) {
  node_t *with_value = create_stmt_return_node(a);
  stmt_return_node_set_value(with_value,
                             make_node(CLUX_NODE_LITERAL_NUMERIC, "9"));
  clux_value_t *signal = exec_stmt_return(ctx, with_value);
  ASSERT_NE(signal, nullptr);
  EXPECT_TRUE(clux_value_is_return_signal(signal));
  clux_value_t *payload = clux_return_signal_get_payload(signal);
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(read_i32(payload), 9);
  delete_node(&with_value);

  node_t *bare = create_stmt_return_node(a);
  clux_value_t *bare_signal = exec_stmt_return(ctx, bare);
  ASSERT_NE(bare_signal, nullptr);
  EXPECT_TRUE(clux_value_is_return_signal(bare_signal));
  EXPECT_EQ(clux_return_signal_get_payload(bare_signal), nullptr)
      << "return; carries no payload";
  delete_node(&bare);
}

/* ---- 作用域栈平衡：error / return signal 逐层上抛后各层出口自恢复
   （defer 接入点位于各层出口 swap 之前，此结构是前提） ---- */

/* 场景 A: 块内 STMT_VAR 声明 x 后，后续语句产生 error 上抛出块；
        error 出块后在当前作用域重新声明同名 x
   预期: 块出口先恢复外层作用域再转换 error——若块作用域未正确换出
         销毁，current scope 仍挂在块上，同名 x 会报 duplicate
         declaration；正确实现下重新声明成功且 lookup 读到新值 */
TEST_F(ExecLiteralTest, BlockErrorUnwindKeepsScopeStackBalanced) {
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(block,
                           make_stmt_var("x",
                                         CLUX_NODE_LITERAL_IDENTIFIER,
                                         "i32",
                                         CLUX_NODE_LITERAL_NUMERIC,
                                         "1"));
  node_t *bad = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(bad,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "boom_a"));
  stmt_block_node_add_stmt(block, bad);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'boom_a'");
  delete_node(&block);

  /* 出块后同名 x 在当前作用域可重新声明（块作用域已换出销毁） */
  node_t *redecl = make_stmt_var(
      "x", CLUX_NODE_LITERAL_IDENTIFIER, "i32", CLUX_NODE_LITERAL_NUMERIC, "9");
  clux_value_t *declared = exec_node(ctx, redecl);
  ASSERT_NE(declared, nullptr);
  EXPECT_NE(clux_type_get_kind(clux_value_get_type(declared)), CLUX_TYPE_ERROR)
      << "scope stack must be restored; block-local x must not leak";
  clux_value_t *lookup = clux_context_lookup(ctx, "x", NULL);
  ASSERT_NE(lookup, nullptr);
  EXPECT_EQ(lookup, declared);
  EXPECT_EQ(read_i32(lookup), 9);
  delete_node(&redecl);
}

/* 场景 B: 函数体嵌套块内 return——signal 逐层穿过多层块出口，
        调用结果载荷正确 clone 出帧，调用完成后调用方作用域继续正常
   预期: 调用返回 77；调用后调用方可继续声明/lookup 新名字 */
TEST_F(ExecLiteralTest, CallSignalUnwindKeepsCallerScopeUsable) {
  /* nb2: func nb2(): i32 { { { return 77; } } } */
  node_t *innermost = create_stmt_block_node(a);
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(ret, make_node(CLUX_NODE_LITERAL_NUMERIC, "77"));
  stmt_block_node_add_stmt(innermost, ret);
  node_t *middle = create_stmt_block_node(a);
  stmt_block_node_add_stmt(middle, innermost);
  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(body, middle);
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nb2"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);

  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nb2"), 0, NULL);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_I32);
  EXPECT_EQ(read_i32(result), 77)
      << "payload must survive cloning through every block exit and the frame";

  /* 调用后调用方作用域未被帧/块残留破坏：新声明可用 */
  node_t *after = make_stmt_var("post_b",
                                CLUX_NODE_LITERAL_IDENTIFIER,
                                "i32",
                                CLUX_NODE_LITERAL_NUMERIC,
                                "5");
  clux_value_t *declared = exec_node(ctx, after);
  ASSERT_NE(declared, nullptr);
  EXPECT_NE(clux_type_get_kind(clux_value_get_type(declared)), CLUX_TYPE_ERROR);
  clux_value_t *looked_up = clux_context_lookup(ctx, "post_b", NULL);
  ASSERT_NE(looked_up, nullptr);
  EXPECT_EQ(read_i32(looked_up), 5);
  delete_node(&after);
  delete_node(&call);
  delete_node(&decl);
}

/* 场景 C: error 从函数体 2 层嵌套块深处上抛，穿块出口到帧边界（帧出口
        借 err 指针重建），调用结果仍是该 error；之后 current scope 正常
   预期: 调用结果消息 "use of undeclared identifier 'deep_boom'"；
         调用后继续声明/lookup 成功 */
TEST_F(ExecLiteralTest, CallErrorUnwindRestoresScopeThroughFrame) {
  /* ef: func ef(): i32 { { deep_boom; } } */
  node_t *inner = create_stmt_block_node(a);
  node_t *bad = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(bad,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "deep_boom"));
  stmt_block_node_add_stmt(inner, bad);
  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(body, inner);
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        create_func_params_node(a),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "ef"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *func = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(func, nullptr);

  node_t *call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "ef"), 0, NULL);
  clux_value_t *result = exec_expr_call(ctx, call);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'deep_boom'")
      << "the error must survive every block exit and the frame rebuild";

  /* 帧与各层块全部换出后，current scope 状态正常 */
  node_t *after = make_stmt_var("post_c",
                                CLUX_NODE_LITERAL_IDENTIFIER,
                                "i32",
                                CLUX_NODE_LITERAL_NUMERIC,
                                "3");
  clux_value_t *declared = exec_node(ctx, after);
  ASSERT_NE(declared, nullptr);
  EXPECT_NE(clux_type_get_kind(clux_value_get_type(declared)), CLUX_TYPE_ERROR);
  clux_value_t *looked_up = clux_context_lookup(ctx, "post_c", NULL);
  ASSERT_NE(looked_up, nullptr);
  EXPECT_EQ(read_i32(looked_up), 3);
  delete_node(&after);
  delete_node(&call);
  delete_node(&decl);
}

/* ---- exec_stmt_var：var 声明装饰器（值级包装，白皮书 §3.2/§9.1） ---- */

/* 合成 location 覆盖 text 的 kind 结点（text 须持久存储）——与 fixture
   make_node 同构，供文件级辅助函数构造具名 identifier 等子结点。 */
static node_t *
make_text_node(allocator_t *allocator, node_kind_t kind, const char *text) {
  node_t *node = create_node(allocator, kind);
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

/* 合成 [[expr]] 装饰器结点（expr 允许为 NULL 构造缺表达式容错路径，
   delete 根结点时一并释放全部子结点）。 */
static node_t *make_decorator_of(allocator_t *allocator, node_t *expr) {
  node_t *node = create_decorator_node(allocator);
  decorator_node_set_expr(node, expr);
  return node;
}

/* 定义 func name(x: i32): ret_text { return ret_value; } 并注册进当前
   作用域——decl AST 被函数值借用，经 decl_out 交调用方持有，须在全部
   调用结束后再释放（中途释放即悬空借用）。 */
static clux_value_t *define_i32_wrapper(clux_context_t *ctx,
                                        allocator_t *allocator,
                                        const char *name,
                                        const char *ret_text,
                                        node_t *ret_value,
                                        node_t **decl_out) {
  node_t *params = create_func_params_node(allocator);
  func_params_node_add_param(
      params,
      make_named_param_of(
          allocator,
          make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "x"),
          make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *body = create_stmt_block_node(allocator);
  node_t *ret = create_stmt_return_node(allocator);
  stmt_return_node_set_value(ret, ret_value);
  stmt_block_node_add_stmt(body, ret);
  node_t *decl = make_func_decl_of(
      allocator,
      NULL,
      NULL,
      params,
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, name),
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, ret_text),
      body);
  *decl_out = decl;
  return exec_expr_func_decl(ctx, decl);
}

/* 场景: [[twice]] var x = 21（twice = func(x: i32): i32 { return x * 2; }）
   预期: 装饰器按值级包装落地 x = twice(21) → i32 42（对应 conformance
         decorated_var == 42 断言）；invoke 前懒安装帧协议执行器；
         变量按名 lookup 到同一值 */
TEST_F(ExecLiteralTest, DecoratorWrapsInitialValueWithFuncValue) {
  node_t *twice_decl = nullptr;
  clux_value_t *twice = define_i32_wrapper(
      ctx,
      a,
      "twice",
      "i32",
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                     make_node(CLUX_NODE_SYMBOL, "*"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "2")),
      &twice_decl);
  ASSERT_NE(twice, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(twice)), CLUX_TYPE_FUNC);

  node_t *node = make_stmt_var("x",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               nullptr,
                               CLUX_NODE_LITERAL_NUMERIC,
                               "21");
  stmt_var_node_add_decorator(
      node,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "twice")));
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_I32);
  EXPECT_EQ(read_i32(result), 42) << "[[twice]] must wrap the initial value";
  EXPECT_EQ(clux_context_lookup(ctx, "x", NULL), result)
      << "decorated value must be declared under the variable name";
  EXPECT_NE(clux_context_get_executor(ctx), nullptr)
      << "decorator invoke path must lazily install the frame executor";
  delete_node(&node);
  delete_node(&twice_decl);
}

/* 场景: [[d1]] [[d2]] var y = 1 按源码顺序链式包装
        （d1 = func(x: i32): i32 { return x + 10; }，d2 = ... { return x * 2; }）
   预期: y = d2(d1(1)) = 22——先 d1 后 d2；反序结果 12 必须被断言区分 */
TEST_F(ExecLiteralTest, DecoratorChainAppliesInSourceOrder) {
  node_t *add10_decl = nullptr;
  clux_value_t *add10 = define_i32_wrapper(
      ctx,
      a,
      "add10",
      "i32",
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                     make_node(CLUX_NODE_SYMBOL, "+"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "10")),
      &add10_decl);
  ASSERT_NE(add10, nullptr);
  node_t *mul2_decl = nullptr;
  clux_value_t *mul2 = define_i32_wrapper(
      ctx,
      a,
      "mul2",
      "i32",
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                     make_node(CLUX_NODE_SYMBOL, "*"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "2")),
      &mul2_decl);
  ASSERT_NE(mul2, nullptr);

  node_t *node = make_stmt_var("y",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               nullptr,
                               CLUX_NODE_LITERAL_NUMERIC,
                               "1");
  stmt_var_node_add_decorator(
      node,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "add10")));
  stmt_var_node_add_decorator(
      node,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "mul2")));
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(read_i32(result), 22)
      << "decorators must chain in source order: mul2(add10(1)) == 22";
  EXPECT_EQ(clux_context_lookup(ctx, "y", NULL), result);
  delete_node(&node);
  delete_node(&add10_decl);
  delete_node(&mul2_decl);
}

/* 场景: 非 func 装饰器求值结果跳过——[[i32]]（type value，编译期标记）
        与 [[7]]（整数字面量）混排 var s = 21
   预期: 两类非 func 求值结果均不包装初值（s == 21）；全程无 invoke
         （帧执行器保持未安装） */
TEST_F(ExecLiteralTest, DecoratorNonFuncValueIsSkipped) {
  node_t *node = make_stmt_var("s",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               nullptr,
                               CLUX_NODE_LITERAL_NUMERIC,
                               "21");
  stmt_var_node_add_decorator(
      node,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  stmt_var_node_add_decorator(
      node, make_decorator_of(a, make_node(CLUX_NODE_LITERAL_NUMERIC, "7")));
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_I32);
  EXPECT_EQ(read_i32(result), 21)
      << "non-func decorator results (type value / integer) must not alter "
         "the initial value";
  EXPECT_EQ(clux_context_lookup(ctx, "s", NULL), result);
  EXPECT_EQ(clux_context_get_executor(ctx), nullptr)
      << "skipped decorators must never reach the invoke path";
  delete_node(&node);
}

/* 场景: 装饰器表达式求值产出 error——[[missing_fn]] var x = 21
   预期: 求值错误直接传播为声明结果（use of undeclared identifier），
         变量不落地（lookup 不到） */
TEST_F(ExecLiteralTest, DecoratorExpressionErrorPropagates) {
  node_t *node = make_stmt_var("x",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               nullptr,
                               CLUX_NODE_LITERAL_NUMERIC,
                               "21");
  stmt_var_node_add_decorator(
      node,
      make_decorator_of(a,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "missing_fn")));
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'missing_fn'")
      << "decorator expression evaluation error must propagate verbatim";
  clux_value_t *looked_up = clux_context_lookup(ctx, "x", NULL);
  ASSERT_NE(looked_up, nullptr);
  EXPECT_TRUE(clux_value_is_error(looked_up))
      << "the variable must not be declared when decoration fails "
         "(lookup miss yields an error value)";
  delete_node(&node);
}

/* 场景: 装饰器 invoke 内部 error——boom = func(x: i32): i32
        { return boom_var; }（体内未声明名），[[boom]] var x = 21
   预期: 函数体执行错误穿帧边界传播为声明结果，错误消息保持原样 */
TEST_F(ExecLiteralTest, DecoratorInvokeBodyErrorPropagates) {
  node_t *boom_decl = nullptr;
  clux_value_t *boom =
      define_i32_wrapper(ctx,
                         a,
                         "boom",
                         "i32",
                         make_node(CLUX_NODE_LITERAL_IDENTIFIER, "boom_var"),
                         &boom_decl);
  ASSERT_NE(boom, nullptr);

  node_t *node = make_stmt_var("x",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               nullptr,
                               CLUX_NODE_LITERAL_NUMERIC,
                               "21");
  stmt_var_node_add_decorator(
      node,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "boom")));
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'boom_var'")
      << "the in-body error must propagate through the frame boundary";
  clux_value_t *declared = clux_context_lookup(ctx, "x", NULL);
  ASSERT_NE(declared, nullptr);
  EXPECT_TRUE(clux_value_is_error(declared))
      << "the variable must not be declared when the wrapper body fails "
         "(lookup miss yields an error value)";
  delete_node(&node);
  delete_node(&boom_decl);
}

/* 场景: 装饰器 invoke 签名校验失败——双参函数 [[pair]] var x = 21
   预期: func_invoke 校验实参个数（期望 2 实收 1）报 error 并传播 */
TEST_F(ExecLiteralTest, DecoratorInvokeArityErrorPropagates) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "b"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *body = create_stmt_block_node(a);
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(ret, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "a"));
  stmt_block_node_add_stmt(body, ret);
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pair"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *pair = exec_expr_func_decl(ctx, decl);
  ASSERT_NE(pair, nullptr);

  node_t *node = make_stmt_var("x",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               nullptr,
                               CLUX_NODE_LITERAL_NUMERIC,
                               "21");
  stmt_var_node_add_decorator(
      node,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pair")));
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "function expects 2 arguments, got 1")
      << "invoke signature validation failure must propagate";
  delete_node(&node);
  delete_node(&decl);
}

/* 场景: TDZ 路径装饰器跳过——[[twice]] var t: i32 = undefined
   预期: 无初值可包装，装饰器整体跳过（执行器保持未安装）；i32 类型
         TDZ 变量正常创建并按名可达 */
TEST_F(ExecLiteralTest, DecoratorSkippedOnTdzInitializer) {
  node_t *twice_decl = nullptr;
  clux_value_t *twice = define_i32_wrapper(
      ctx,
      a,
      "twice",
      "i32",
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                     make_node(CLUX_NODE_SYMBOL, "*"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "2")),
      &twice_decl);
  ASSERT_NE(twice, nullptr);

  node_t *node = make_stmt_var("t",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               "i32",
                               CLUX_NODE_LITERAL_UNDEFINED,
                               kUndefinedText);
  stmt_var_node_add_decorator(
      node,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "twice")));
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(clux_value_is_tdz(result))
      << "undefined initializer must still create a TDZ variable";
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_I32);
  EXPECT_EQ(clux_context_lookup(ctx, "t", NULL), result);
  EXPECT_EQ(clux_context_get_executor(ctx), nullptr)
      << "decorators must be skipped entirely on the TDZ path";
  delete_node(&node);
  delete_node(&twice_decl);
}

/* 场景: 装饰器槽位损坏两条容错路径——DECORATOR 结点缺 expr（手工空造）
        与装饰器槽位本身为 NULL（get_decorator 取到空槽）
   预期: 均报 "decorator requires an expression"，不触发 invoke */
TEST_F(ExecLiteralTest, DecoratorMissingExpressionReports) {
  /* 路径 1：DECORATOR 结点存在但 expr 槽为空 */
  node_t *first = make_stmt_var("x",
                                CLUX_NODE_LITERAL_IDENTIFIER,
                                nullptr,
                                CLUX_NODE_LITERAL_NUMERIC,
                                "21");
  stmt_var_node_add_decorator(first, make_decorator_of(a, nullptr));
  clux_value_t *result = exec_stmt_var(ctx, first);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "decorator requires an expression");
  clux_value_t *declared = clux_context_lookup(ctx, "x", NULL);
  ASSERT_NE(declared, nullptr);
  EXPECT_TRUE(clux_value_is_error(declared))
      << "the variable must not be declared on the malformed decorator path "
         "(lookup miss yields an error value)";
  delete_node(&first);
  declared = nullptr;

  /* 路径 2：装饰器槽位为 NULL（node_add_child NULL 空槽容忍） */
  node_t *second = make_stmt_var("x",
                                 CLUX_NODE_LITERAL_IDENTIFIER,
                                 nullptr,
                                 CLUX_NODE_LITERAL_NUMERIC,
                                 "21");
  stmt_var_node_add_decorator(second, nullptr);
  clux_value_t *result2 = exec_stmt_var(ctx, second);
  ASSERT_NE(result2, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result2)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result2), "decorator requires an expression");
  declared = clux_context_lookup(ctx, "x", NULL);
  ASSERT_NE(declared, nullptr);
  EXPECT_TRUE(clux_value_is_error(declared))
      << "the variable must not be declared on the malformed decorator path "
         "(lookup miss yields an error value)";
  delete_node(&second);
}

/* 场景: 装饰结果与类型标注交互（拓宽）——[[twice]] var w: i64 = 21
   预期: 包装产出的 i32 42 经 safe_cast 拓宽到标注 i64 后落地——变量
         类型精确为 i64、值 42、按名可达 */
TEST_F(ExecLiteralTest, DecoratedValueWidensThroughSafeCast) {
  node_t *twice_decl = nullptr;
  clux_value_t *twice = define_i32_wrapper(
      ctx,
      a,
      "twice",
      "i32",
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                     make_node(CLUX_NODE_SYMBOL, "*"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "2")),
      &twice_decl);
  ASSERT_NE(twice, nullptr);

  node_t *node = make_stmt_var("w",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               "i64",
                               CLUX_NODE_LITERAL_NUMERIC,
                               "21");
  stmt_var_node_add_decorator(
      node,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "twice")));
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_I64)
      << "the wrapped i32 value must be safely cast to the annotated i64";
  EXPECT_EQ(read_i64(result), 42);
  EXPECT_EQ(clux_context_lookup(ctx, "w", NULL), result);
  delete_node(&node);
  delete_node(&twice_decl);
}

/* 场景: 装饰结果与类型标注交互（窄化拒绝）——[[twice]] var n: i8 = 21
   预期: 包装产出的 i32 42 到标注 i8 的 safe_cast 因窄化被拒——报
         narrowing conversion，变量不落地（lookup 不到） */
TEST_F(ExecLiteralTest, DecoratedValueNarrowingCastRejected) {
  node_t *twice_decl = nullptr;
  clux_value_t *twice = define_i32_wrapper(
      ctx,
      a,
      "twice",
      "i32",
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                     make_node(CLUX_NODE_SYMBOL, "*"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "2")),
      &twice_decl);
  ASSERT_NE(twice, nullptr);

  node_t *node = make_stmt_var(
      "n", CLUX_NODE_LITERAL_IDENTIFIER, "i8", CLUX_NODE_LITERAL_NUMERIC, "21");
  stmt_var_node_add_decorator(
      node,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "twice")));
  clux_value_t *result = exec_stmt_var(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "cannot safely cast 'i32' to 'i8': narrowing conversion")
      << "the decorated value must still flow through safe_cast";
  clux_value_t *declared = clux_context_lookup(ctx, "n", NULL);
  ASSERT_NE(declared, nullptr);
  EXPECT_TRUE(clux_value_is_error(declared))
      << "the variable must not be declared when the cast is rejected "
         "(lookup miss yields an error value)";
  delete_node(&node);
  delete_node(&twice_decl);
}

/* ---- exec_stmt_func：函数语句包装层（无装饰器直通 / 装饰器值级包装） ----
   被测语义（conformance/09-metadata.cx 权威断言）：[[add100]] func raw
   落地为 raw = add100(raw)，包装函数接管名字，raw(5) == 105。 */

/* 合成 func(i32) -> i32 类型表达式结点（独立子树——节点不可跨父共享）。 */
static node_t *make_i32_to_i32_type(allocator_t *allocator) {
  node_t *params = create_func_params_node(allocator);
  func_params_node_add_param(
      params, make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  return make_func_type_of(
      allocator,
      NULL,
      params,
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
}

/* 合成源函数声明 func name(x: i32): i32 { return x; }（identity）。 */
static node_t *make_i32_identity_decl(allocator_t *allocator,
                                      const char *name) {
  node_t *params = create_func_params_node(allocator);
  func_params_node_add_param(
      params,
      make_named_param_of(
          allocator,
          make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "x"),
          make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *ret = create_stmt_return_node(allocator);
  stmt_return_node_set_value(
      ret, make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "x"));
  node_t *body = create_stmt_block_node(allocator);
  stmt_block_node_add_stmt(body, ret);
  return make_func_decl_of(
      allocator,
      NULL,
      NULL,
      params,
      name ? make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, name)
           : NULL,
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
      body);
}

/* 合成带捕获形态的函数类型节点 func |f: func(i32)->i32| (i32) -> i32
   （类型层捕获项为 FUNC_CAPTURE，与声明层 FUNC_PARAMS 包装不同）——
   链式装饰器第 2 个起形参用：包装产物类型带捕获，捕获表参与类型判等，
   裸 func(i32)->i32 形参过不了 func_invoke 交验。 */
static node_t *make_captured_i32_to_i32_type(allocator_t *allocator) {
  node_t *ret_captures = create_func_captures_node(allocator);
  func_captures_node_add_capture(
      ret_captures,
      make_capture_of(
          allocator,
          make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_i32_to_i32_type(allocator)));
  node_t *ret_params = create_func_params_node(allocator);
  func_params_node_add_param(
      ret_params,
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  return make_func_type_of(
      allocator,
      ret_captures,
      ret_params,
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
}

/* 合成 add100 形状装饰器并 define 进当前作用域：
   func name(f: <f_type 形态>): func |f| (i32) -> i32
     { return func |f| w(x: i32): i32 { return f(x) <op> <k>; }; }
   param_type_captures 选 f 形态：false = 裸 func(i32)->i32（链头装饰器，
   实参是无捕获源函数）；true = func |f| (i32)->i32（链第 2 个起，实参是
   前一装饰器带捕获的包装产物）。decl AST 被函数值借用，经 decl_out 交
   调用方持有，调用全部结束后释放。 */
static clux_value_t *define_func_decorator(clux_context_t *ctx,
                                           allocator_t *allocator,
                                           const char *name,
                                           const char *op_text,
                                           const char *k_text,
                                           bool param_type_captures,
                                           node_t **decl_out) {
  /* 内层包装函数 w 的捕获 f——注意 DECL 捕获表的第 0 项必须是
     FUNC_PARAMS 包装（FUNC_PARAM 列表） */
  node_t *capture_params = create_func_params_node(allocator);
  func_params_node_add_param(
      capture_params,
      make_named_param_of(
          allocator,
          make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          param_type_captures ? make_captured_i32_to_i32_type(allocator)
                              : make_i32_to_i32_type(allocator)));
  node_t *captures = create_func_captures_node(allocator);
  func_captures_node_add_capture(captures, capture_params);
  node_t *inner_params = create_func_params_node(allocator);
  func_params_node_add_param(
      inner_params,
      make_named_param_of(
          allocator,
          make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "x"),
          make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  /* 内层体：return f(x) <op> k; */
  node_t *inner_arg =
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "x");
  node_t *inner_body_ret = create_stmt_return_node(allocator);
  stmt_return_node_set_value(
      inner_body_ret,
      make_binary_of(
          allocator,
          make_call_of(
              allocator,
              make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
              1,
              &inner_arg),
          make_text_node(allocator, CLUX_NODE_SYMBOL, op_text),
          make_text_node(allocator, CLUX_NODE_LITERAL_NUMERIC, k_text)));
  node_t *inner_body = create_stmt_block_node(allocator);
  stmt_block_node_add_stmt(inner_body, inner_body_ret);
  node_t *inner_decl = make_func_decl_of(
      allocator,
      captures,
      NULL,
      inner_params,
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "w"),
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
      inner_body);
  /* 外层体：return func |f| w(...); */
  node_t *outer_ret = create_stmt_return_node(allocator);
  stmt_return_node_set_value(outer_ret, inner_decl);
  node_t *outer_body = create_stmt_block_node(allocator);
  stmt_block_node_add_stmt(outer_body, outer_ret);
  /* 外层形参 f 与返回类型捕获项与内层捕获同形态（类型层捕获项为
     FUNC_CAPTURE，与声明层 FUNC_PARAMS 包装不同） */
  node_t *outer_params = create_func_params_node(allocator);
  func_params_node_add_param(
      outer_params,
      make_named_param_of(
          allocator,
          make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          param_type_captures ? make_captured_i32_to_i32_type(allocator)
                              : make_i32_to_i32_type(allocator)));
  node_t *ret_captures = create_func_captures_node(allocator);
  func_captures_node_add_capture(
      ret_captures,
      make_capture_of(
          allocator,
          make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          param_type_captures ? make_captured_i32_to_i32_type(allocator)
                              : make_i32_to_i32_type(allocator)));
  node_t *ret_params = create_func_params_node(allocator);
  func_params_node_add_param(
      ret_params,
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *ret_type = make_func_type_of(
      allocator,
      ret_captures,
      ret_params,
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *decl = make_func_decl_of(
      allocator,
      NULL,
      NULL,
      outer_params,
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, name),
      ret_type,
      outer_body);
  *decl_out = decl;
  return exec_expr_func_decl(ctx, decl);
}

/* 合成函数语句结点（FUNC_DECL 槽挂 decl，装饰器由调用方追加）。 */
static node_t *make_stmt_func_of(allocator_t *allocator, node_t *decl) {
  node_t *node = create_stmt_func_node(allocator);
  stmt_func_node_set_func_decl(node, decl);
  return node;
}

/* 合成 callee(arg_text) 调用并执行（call AST 经 call_out 交调用方释放）。 */
static clux_value_t *call_unary_i32(clux_context_t *ctx,
                                    allocator_t *allocator,
                                    const char *callee,
                                    const char *arg_text,
                                    node_t **call_out) {
  node_t *arg = make_text_node(allocator, CLUX_NODE_LITERAL_NUMERIC, arg_text);
  node_t *call = make_call_of(
      allocator,
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, callee),
      1,
      &arg);
  *call_out = call;
  return exec_expr_call(ctx, call);
}

/* 场景: 端到端 conformance 语义——[[add100]] func raw(x: i32): i32
        （add100 = func(f: func(i32)->i32): func |f| (i32) -> i32
           { return func |f| w(x: i32): i32 { return f(x) + 100; }; }）
   预期: raw = add100(raw)——包装函数接管名字；内层 |f| 捕获在定义点
         （add100 帧内 f = raw 实参）由 exec 定义点捕获初始化自动种入；
         raw(5) == 105、raw(0) == 100（捕获链真实走 invoke，非按名转发
         源函数）；执行器懒安装。 */
TEST_F(ExecLiteralTest, StmtFuncDecoratorWrapsFunctionEndToEnd) {
  node_t *add100_decl = nullptr;
  clux_value_t *add100 =
      define_func_decorator(ctx, a, "add100", "+", "100", false, &add100_decl);
  ASSERT_NE(add100, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(add100)), CLUX_TYPE_FUNC);

  /* 装饰器调用时 raw 实参绑定进 add100 帧，内层 |f| 捕获由实现自动
     在定义点快照，无需测试代垫种入。 */
  node_t *raw_decl = make_i32_identity_decl(a, "raw");
  node_t *stmt = make_stmt_func_of(a, raw_decl);
  stmt_func_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "add100")));
  clux_value_t *result = exec_stmt_func(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_FUNC)
      << "the decorated function statement must yield the wrapper function";
  EXPECT_EQ(clux_context_lookup(ctx, "raw", NULL), result)
      << "the wrapper value must take over the declared name in place";
  EXPECT_NE(clux_context_get_executor(ctx), nullptr)
      << "decorator invoke must lazily install the frame executor";

  node_t *call = nullptr;
  clux_value_t *wrapped_call = call_unary_i32(ctx, a, "raw", "5", &call);
  ASSERT_NE(wrapped_call, nullptr);
  EXPECT_EQ(read_i32(wrapped_call), 105)
      << "raw(5) must run through the wrapper: raw(5) + 100 == 105";
  delete_node(&call);
  /* 第二次调用换 0：证明 105 非常数折叠、捕获绑定在多次调用间稳定 */
  node_t *call_zero = nullptr;
  clux_value_t *zero_call = call_unary_i32(ctx, a, "raw", "0", &call_zero);
  ASSERT_NE(zero_call, nullptr);
  EXPECT_EQ(read_i32(zero_call), 100)
      << "raw(0) must run the captured source function again: 0 + 100";
  delete_node(&call_zero);
  delete_node(&stmt);
  delete_node(&add100_decl);
}

/* 场景: 无装饰器直通——func plain(x: i32): i32 语句经 exec_node 分派
        （STMT_FUNC 已注册分派表）
   预期: 等价 exec_expr_func_decl 具名路径——名字可查、可调用、
         返回值即 lookup 到的同一值 */
TEST_F(ExecLiteralTest, StmtFuncWithoutDecoratorDefinesByName) {
  node_t *stmt = make_stmt_func_of(a, make_i32_identity_decl(a, "plain"));
  clux_value_t *result = exec_node(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_FUNC);
  EXPECT_EQ(clux_context_lookup(ctx, "plain", NULL), result)
      << "undecorated function statement defines the name directly";
  node_t *call = nullptr;
  clux_value_t *called = call_unary_i32(ctx, a, "plain", "3", &call);
  ASSERT_NE(called, nullptr);
  EXPECT_EQ(read_i32(called), 3);
  delete_node(&call);
  delete_node(&stmt);
}

/* 场景: 多装饰器链式——[[add10f]] [[mul2f]] func raw(x: i32): i32
   预期: 按源码顺序 raw = mul2f(add10f(raw))，raw(3) == (3+10)*2 == 26；
         反序结果 16 必须被断言区分。链第 2 个装饰器形参须声明带捕获
         形态（捕获表参与类型判等，包装产物类型带捕获）。两层 |f| 捕获
         均由定义点捕获初始化自动种入（w1.f ← raw 帧实参、
         result.f ← w1 帧实参），无需测试代垫。 */
TEST_F(ExecLiteralTest, StmtFuncDecoratorChainAppliesInSourceOrder) {
  node_t *add10f_decl = nullptr;
  clux_value_t *add10f =
      define_func_decorator(ctx, a, "add10f", "+", "10", false, &add10f_decl);
  ASSERT_NE(add10f, nullptr);
  node_t *mul2f_decl = nullptr;
  clux_value_t *mul2f =
      define_func_decorator(ctx, a, "mul2f", "*", "2", true, &mul2f_decl);
  ASSERT_NE(mul2f, nullptr);

  node_t *raw_decl = make_i32_identity_decl(a, "raw");
  node_t *stmt = make_stmt_func_of(a, raw_decl);
  stmt_func_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "add10f")));
  stmt_func_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "mul2f")));
  clux_value_t *result = exec_stmt_func(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_FUNC);

  node_t *call = nullptr;
  clux_value_t *called = call_unary_i32(ctx, a, "raw", "3", &call);
  ASSERT_NE(called, nullptr);
  EXPECT_EQ(read_i32(called), 26)
      << "decorators must chain in source order: mul2f(add10f(raw))(3) == 26";
  delete_node(&call);
  delete_node(&stmt);
  delete_node(&add10f_decl);
  delete_node(&mul2f_decl);
}

/* 场景: 非 func 装饰器求值结果跳过——[[i32]]（type value 编译期标记）
        与 [[7]]（整数字面量）混排 func raw(x: i32): i32
   预期: 两类非 func 求值结果均不包装（raw(5) == 5 identity）；
         全程无 invoke（帧执行器保持未安装） */
TEST_F(ExecLiteralTest, StmtFuncNonFuncDecoratorsAreSkipped) {
  node_t *stmt = make_stmt_func_of(a, make_i32_identity_decl(a, "raw"));
  stmt_func_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  stmt_func_node_add_decorator(
      stmt, make_decorator_of(a, make_node(CLUX_NODE_LITERAL_NUMERIC, "7")));
  clux_value_t *result = exec_stmt_func(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_FUNC)
      << "non-func decorator results must leave the source function intact";
  EXPECT_EQ(clux_context_lookup(ctx, "raw", NULL), result);
  EXPECT_EQ(clux_context_get_executor(ctx), nullptr)
      << "skipped decorators must never reach the invoke path";
  node_t *call = nullptr;
  clux_value_t *called = call_unary_i32(ctx, a, "raw", "5", &call);
  ASSERT_NE(called, nullptr);
  EXPECT_EQ(read_i32(called), 5)
      << "identity body must run unwrapped: raw(5) == 5";
  delete_node(&call);
  delete_node(&stmt);
}

/* 场景: 装饰器表达式求值产出 error——[[missing_fn]] func raw(x: i32): i32
   预期: 求值错误直接传播为语句结果，raw 不落地（lookup 拿到 error 值） */
TEST_F(ExecLiteralTest, StmtFuncDecoratorExpressionErrorPropagates) {
  node_t *stmt = make_stmt_func_of(a, make_i32_identity_decl(a, "raw"));
  stmt_func_node_add_decorator(
      stmt,
      make_decorator_of(a,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "missing_fn")));
  clux_value_t *result = exec_stmt_func(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'missing_fn'")
      << "decorator expression evaluation error must propagate verbatim";
  clux_value_t *declared = clux_context_lookup(ctx, "raw", NULL);
  ASSERT_NE(declared, nullptr);
  EXPECT_TRUE(clux_value_is_error(declared))
      << "the function must not be declared when decoration fails "
         "(lookup miss yields an error value)";
  delete_node(&stmt);
}

/* 场景: 装饰器 invoke 元数校验失败——双参函数 [[pair]] func raw(x: i32): i32
   预期: func_invoke 校验实参个数（期望 2 实收 1）报 error 并传播，
         raw 不落地 */
TEST_F(ExecLiteralTest, StmtFuncDecoratorInvokeArityErrorPropagates) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pa"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pb"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *body = create_stmt_block_node(a);
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(ret,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pa"));
  stmt_block_node_add_stmt(body, ret);
  node_t *pair_decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pair"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        body);
  clux_value_t *pair = exec_expr_func_decl(ctx, pair_decl);
  ASSERT_NE(pair, nullptr);

  node_t *stmt = make_stmt_func_of(a, make_i32_identity_decl(a, "raw"));
  stmt_func_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pair")));
  clux_value_t *result = exec_stmt_func(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "function expects 2 arguments, got 1")
      << "invoke arity validation failure must propagate";
  clux_value_t *declared = clux_context_lookup(ctx, "raw", NULL);
  ASSERT_NE(declared, nullptr);
  EXPECT_TRUE(clux_value_is_error(declared))
      << "the function must not be declared when the wrapper signature "
         "rejects the source function";
  delete_node(&stmt);
  delete_node(&pair_decl);
}

/* 场景: 装饰器 invoke 类型校验失败——接收 i32 的装饰器拿到函数值
        （expect_i32 = func(y: i32): str，返回类型取 str 与 raw 的
         (i32)->i32 结构错开，保证 raw 类型名的确定性）
   预期: 报 "function argument 0 requires type 'i32', got 'raw'" 并传播；
         函数体不被执行 */
TEST_F(ExecLiteralTest, StmtFuncDecoratorInvokeTypeErrorPropagates) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "y"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(body, create_stmt_return_node(a));
  node_t *expect_decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "expect_i32"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "str"),
                        body);
  clux_value_t *expect_i32 = exec_expr_func_decl(ctx, expect_decl);
  ASSERT_NE(expect_i32, nullptr);

  node_t *stmt = make_stmt_func_of(a, make_i32_identity_decl(a, "raw"));
  stmt_func_node_add_decorator(
      stmt,
      make_decorator_of(a,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "expect_i32")));
  clux_value_t *result = exec_stmt_func(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "function argument 0 requires type 'i32', got 'raw'")
      << "invoke parameter type validation failure must propagate";
  delete_node(&stmt);
  delete_node(&expect_decl);
}

/* 场景: func_decl 槽缺失——手工造空 STMT_FUNC 结点
   预期: 报 "function statement requires a function declaration" */
TEST_F(ExecLiteralTest, StmtFuncMissingFuncDeclReports) {
  node_t *stmt = create_stmt_func_node(a);
  clux_value_t *result = exec_stmt_func(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "function statement requires a function declaration");
  delete_node(&stmt);
}

/* 场景: 装饰器 + 匿名函数声明（NAME 槽为 NULL）
   预期: 报 "decorated function declaration requires a name"，
         匿名值不可命名化（报错在 exec_func_decl_value 求值之前） */
TEST_F(ExecLiteralTest, StmtFuncDecoratedAnonymousRequiresName) {
  node_t *stmt = make_stmt_func_of(a, make_i32_identity_decl(a, nullptr));
  stmt_func_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *result = exec_stmt_func(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "decorated function declaration requires a name");
  delete_node(&stmt);
}

/* 场景: export/comptime 标记存在——exec 层忽略（export 是 run 阶段产出、
        comptime 解释执行下与普通函数等价）
   预期: 函数正常定义并按名调用（raw(4) == 4），行为与无标记完全一致 */
TEST_F(ExecLiteralTest, StmtFuncExportComptimeMarkersAreIgnored) {
  node_t *stmt = make_stmt_func_of(a, make_i32_identity_decl(a, "raw"));
  stmt_func_node_set_export(stmt, make_node(CLUX_NODE_SYMBOL, "export"));
  stmt_func_node_set_comptime(stmt, make_node(CLUX_NODE_SYMBOL, "comptime"));
  clux_value_t *result = exec_stmt_func(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_FUNC)
      << "export/comptime markers must not affect exec semantics";
  EXPECT_EQ(clux_context_lookup(ctx, "raw", NULL), result);
  node_t *call = nullptr;
  clux_value_t *called = call_unary_i32(ctx, a, "raw", "4", &call);
  ASSERT_NE(called, nullptr);
  EXPECT_EQ(read_i32(called), 4)
      << "a comptime-marked function must behave as a plain function";
  delete_node(&call);
  delete_node(&stmt);
}

/* 场景: 装饰器结点缺 expr（手工空造）——func 侧容错路径
        （与 var 侧共用 apply_decorator）
   预期: 报 "decorator requires an expression"，raw 不落地、不触发求值 */
TEST_F(ExecLiteralTest, StmtFuncDecoratorMissingExpressionReports) {
  node_t *stmt = make_stmt_func_of(a, make_i32_identity_decl(a, "raw"));
  stmt_func_node_add_decorator(stmt, make_decorator_of(a, nullptr));
  clux_value_t *result = exec_stmt_func(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "decorator requires an expression");
  clux_value_t *declared = clux_context_lookup(ctx, "raw", NULL);
  ASSERT_NE(declared, nullptr);
  EXPECT_TRUE(clux_value_is_error(declared))
      << "the function must not be declared on the malformed decorator path";
  delete_node(&stmt);
}

/* 场景: 装饰路径源声明求值失败——func raw(x: i32) 缺返回类型（RETURN 槽
        NULL），exec_func_decl_value 在装饰前报错
   预期: 报 "function definition requires a return type" 并传播，
         raw 不落地 */
TEST_F(ExecLiteralTest, StmtFuncDecoratedDeclEvaluationErrorPropagates) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *bad_decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "raw"),
                        NULL,
                        create_stmt_block_node(a));
  node_t *stmt = make_stmt_func_of(a, bad_decl);
  stmt_func_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *result = exec_stmt_func(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "function definition requires a return type")
      << "source declaration evaluation failure must propagate before "
         "decoration";
  clux_value_t *declared = clux_context_lookup(ctx, "raw", NULL);
  ASSERT_NE(declared, nullptr);
  EXPECT_TRUE(clux_value_is_error(declared));
  delete_node(&stmt);
}

/* 场景: exec_func_decl_value 匿名求值——求值 func fv(x: i32): i32 但
        不按名 define；随后 declare_value 具名化；对照具名路径
        exec_expr_func_decl 不回归
   预期: 产出值 kind FUNC、AST 已挂接；作用域中无 fv 名（lookup 报
         undeclared）；declare_value 后按名可调用（fv(2) == 2）；
         具名路径 lookup 正常命中 */
TEST_F(ExecLiteralTest, FuncDeclValueEvaluationYieldsUsableValueWithoutName) {
  node_t *decl = make_i32_identity_decl(a, "fv");
  clux_value_t *value = exec_func_decl_value(ctx, decl);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(value)), CLUX_TYPE_FUNC);
  EXPECT_EQ(clux_func_get_ast(value), decl)
      << "the anonymous evaluation must still attach the declaration AST";
  clux_value_t *probe = clux_context_lookup(ctx, "fv", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(probe)), CLUX_TYPE_ERROR)
      << "the anonymous evaluation must not define the name";
  EXPECT_STREQ(error_message_of(probe), "use of undeclared identifier 'fv'");

  /* 具名化（exec_stmt_func 装饰路径的收尾通道）——就地命名不拷贝 */
  clux_value_t *bound =
      clux_context_declare_value(ctx, value, "fv", node_get_location(decl));
  ASSERT_NE(bound, nullptr);
  node_t *call = nullptr;
  clux_value_t *called = call_unary_i32(ctx, a, "fv", "2", &call);
  ASSERT_NE(called, nullptr);
  EXPECT_EQ(read_i32(called), 2)
      << "the anonymous value must be callable after naming";
  delete_node(&call);
  delete_node(&decl);

  /* 具名路径回归：exec_expr_func_decl 仍按名 define */
  node_t *named_decl = make_i32_identity_decl(a, "fv_named");
  clux_value_t *named = exec_expr_func_decl(ctx, named_decl);
  ASSERT_NE(named, nullptr);
  EXPECT_EQ(clux_context_lookup(ctx, "fv_named", NULL), named)
      << "the named declaration path must not regress";
  delete_node(&named_decl);
}

/* ---- exec_expr_union / exec_stmt_union：union 定义与语句（§3.7/§3.8） ---- */

/* 合成 union 成员字段节点 name: type（type_text 为 NULL 构造缺类型容错
   路径），delete 根结点时一并释放子结点。 */
static node_t *make_union_field_of(allocator_t *allocator,
                                   const char *name_text,
                                   const char *type_text) {
  node_t *field = create_field_node(allocator);
  if (name_text) {
    field_node_set_name(
        field,
        make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, name_text));
  }
  if (type_text) {
    field_node_set_type(
        field,
        make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, type_text));
  }
  return field;
}

/* 合成 union/cunion 声明节点 <keyword> NAME { body }：keyword_text 为
   NULL 构造 KEYWORD 槽缺省（exec 默认走 tag union），name_text 为 NULL
   构造缺名容错路径，generics/body 允许为 NULL。 */
static node_t *make_union_decl_of(allocator_t *allocator,
                                  const char *keyword_text,
                                  const char *name_text,
                                  node_t *generics,
                                  node_t *body) {
  node_t *node = create_union_node(allocator);
  const char *loc_text = keyword_text ? keyword_text : "union";
  location_t loc;
  loc.filename = "test.clux";
  loc.begin.offset = loc_text;
  loc.begin.column = 1;
  loc.begin.line = 1;
  loc.end.offset = loc_text + strlen(loc_text);
  loc.end.column = 1;
  loc.end.line = 1;
  node_set_location(node, &loc);
  if (keyword_text) {
    union_node_set_keyword(
        node, make_text_node(allocator, CLUX_NODE_SYMBOL, keyword_text));
  }
  if (name_text) {
    union_node_set_name(
        node,
        make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, name_text));
  }
  union_node_set_generics(node, generics);
  union_node_set_body(node, body);
  return node;
}

/* 合成两成员字段表 { a: i32; b: f64 }（e2e 与结构去重用例共用）。 */
static node_t *make_two_member_fields(allocator_t *allocator) {
  node_t *fields = create_fields_node(allocator);
  fields_node_add_field(fields, make_union_field_of(allocator, "a", "i32"));
  fields_node_add_field(fields, make_union_field_of(allocator, "b", "f64"));
  return fields;
}

/* 场景: tag union 定义端到端——union U { a: i32; b: f64 }（KEYWORD 槽
        文本 "union"）
   预期: 产出 type value（类型 kind TYPE），lookup("U") 取回同一值；
         被描述类型 kind UNION、类型名 "U"、成员数 2，find_member 按名
         取回成员且成员类型与 i32/f64 builtin 恒等，未知成员查无 */
TEST_F(ExecLiteralTest, UnionDefinesTagUnionEndToEnd) {
  node_t *decl =
      make_union_decl_of(a, "union", "U", NULL, make_two_member_fields(a));
  clux_value_t *result = exec_expr_union(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE);
  EXPECT_EQ(clux_context_lookup(ctx, "U", NULL), result)
      << "the named path must define the type value in the current scope";

  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_UNION);
  EXPECT_STREQ(clux_type_get_name(described), "U");
  EXPECT_EQ(clux_union_member_count(described), 2u);
  const clux_union_member_t *ma = clux_union_find_member(described, "a");
  ASSERT_NE(ma, nullptr);
  EXPECT_STREQ(ma->name, "a");
  EXPECT_EQ(ma->type, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  const clux_union_member_t *mb = clux_union_find_member(described, "b");
  ASSERT_NE(mb, nullptr);
  EXPECT_STREQ(mb->name, "b");
  EXPECT_EQ(mb->type, clux_context_builtin_type(ctx, CLUX_TYPE_F64));
  EXPECT_EQ(clux_union_find_member(described, "zzz"), nullptr)
      << "unknown member lookup must not match";
  delete_node(&decl);
}

/* 场景: cunion 关键字分派——cunion CU { a: i32; b: f64 }（KEYWORD 槽
        文本 "cunion"）
   预期: 被描述类型走 C union 构造器：kind CUNION、成员按声明序可查
         （get_member(0)="a"、(1)="b"，全部共享偏移 0 由构造器保证）、
         lookup("CU") 取回同一值 */
TEST_F(ExecLiteralTest, UnionCunionKeywordDispatchesToCunion) {
  node_t *decl =
      make_union_decl_of(a, "cunion", "CU", NULL, make_two_member_fields(a));
  clux_value_t *result = exec_expr_union(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_context_lookup(ctx, "CU", NULL), result);

  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_CUNION);
  EXPECT_EQ(clux_cunion_member_count(described), 2u);
  const clux_cunion_member_t *m0 = clux_cunion_get_member(described, 0);
  ASSERT_NE(m0, nullptr);
  EXPECT_STREQ(m0->name, "a");
  EXPECT_EQ(m0->type, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  const clux_cunion_member_t *m1 = clux_cunion_get_member(described, 1);
  ASSERT_NE(m1, nullptr);
  EXPECT_STREQ(m1->name, "b");
  EXPECT_EQ(m1->type, clux_context_builtin_type(ctx, CLUX_TYPE_F64));
  ASSERT_NE(clux_cunion_find_member(described, "a"), nullptr);
  EXPECT_EQ(clux_cunion_find_member(described, "zzz"), nullptr);
  delete_node(&decl);
}

/* 场景: KEYWORD 槽缺省——union 声明节点不带关键字（容错形态）
   预期: exec 按 tag union 处理（is_cunion=false），被描述类型 kind
         UNION 与显式 "union" 关键字路径一致 */
TEST_F(ExecLiteralTest, UnionNullKeywordDefaultsToTagUnion) {
  node_t *decl =
      make_union_decl_of(a, NULL, "NK", NULL, make_two_member_fields(a));
  clux_value_t *result = exec_expr_union(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_UNION);
  delete_node(&decl);
}

/* 场景: 结构去重恒等——union U { a: i32; b: f64 } 与 union V { a: i32;
        b: f64 } 成员表一致（类型名不同），对照 union W { a: i32 } 成员
        表不同
   预期: U/V 解出的被描述类型指针恒等（find-or-dedup 命中已存实例）；
         W 与 U 指针不同（结构判等失败须新建实例） */
TEST_F(ExecLiteralTest, UnionStructuralDedupYieldsIdenticalType) {
  node_t *decl_u =
      make_union_decl_of(a, "union", "U", NULL, make_two_member_fields(a));
  clux_value_t *value_u = exec_expr_union(ctx, decl_u);
  ASSERT_NE(value_u, nullptr);
  clux_type_t *type_u = type_described_by(value_u);
  ASSERT_NE(type_u, nullptr);

  node_t *decl_v =
      make_union_decl_of(a, "union", "V", NULL, make_two_member_fields(a));
  clux_value_t *value_v = exec_expr_union(ctx, decl_v);
  ASSERT_NE(value_v, nullptr);
  clux_type_t *type_v = type_described_by(value_v);
  ASSERT_NE(type_v, nullptr);
  EXPECT_EQ(type_u, type_v)
      << "identical member tables must dedup to the same type instance";

  node_t *fields_w = create_fields_node(a);
  fields_node_add_field(fields_w, make_union_field_of(a, "a", "i32"));
  node_t *decl_w = make_union_decl_of(a, "union", "W", NULL, fields_w);
  clux_value_t *value_w = exec_expr_union(ctx, decl_w);
  ASSERT_NE(value_w, nullptr);
  clux_type_t *type_w = type_described_by(value_w);
  ASSERT_NE(type_w, nullptr);
  EXPECT_NE(type_u, type_w)
      << "different member tables must not dedup to the same instance";
  delete_node(&decl_u);
  delete_node(&decl_v);
  delete_node(&decl_w);
}

/* 场景: 匿名 union（无 NAME 槽）——表达式位置合法
   预期: 成功产出 type value（描述 tag union 类型），不按名 define */
TEST_F(ExecLiteralTest, UnionAnonymousDefinitionYieldsUnnamedTypeValue) {
  node_t *decl =
      make_union_decl_of(a, "union", NULL, NULL, make_two_member_fields(a));
  clux_value_t *result = exec_expr_union(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_TYPE);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_UNION);
  EXPECT_EQ(clux_union_member_count(described), (size_t)2);
  delete_node(&decl);
}

/* 场景: 泛型暂不支持——union G<T> 带 GENERICS 槽（名与成员齐备，
        证明泛型检查在缺名检查之前独立拦截）
   预期: 报 "generic union is not supported yet" */
TEST_F(ExecLiteralTest, UnionGenericsNotSupportedReports) {
  node_t *decl =
      make_union_decl_of(a,
                         "union",
                         "G",
                         make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "T"),
                         make_two_member_fields(a));
  clux_value_t *result = exec_expr_union(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "generic union is not supported yet");
  delete_node(&decl);
}

/* 场景: 空成员表——BODY 为 NULL 与空字段节点 {} 两种容错形态
   预期: 均报 "union 'UB' requires at least one member" */
TEST_F(ExecLiteralTest, UnionEmptyBodyReports) {
  node_t *null_body = make_union_decl_of(a, "union", "UB", NULL, NULL);
  clux_value_t *null_result = exec_expr_union(ctx, null_body);
  ASSERT_NE(null_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(null_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(null_result),
               "union 'UB' requires at least one member");
  delete_node(&null_body);

  node_t *empty_body =
      make_union_decl_of(a, "union", "UB", NULL, create_fields_node(a));
  clux_value_t *empty_result = exec_expr_union(ctx, empty_body);
  ASSERT_NE(empty_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(empty_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(empty_result),
               "union 'UB' requires at least one member");
  delete_node(&empty_body);
}

/* 场景: 成员缺名——union MN { : i32 } 首成员无 NAME
   预期: 报 "union 'MN' member 0 requires a name" */
TEST_F(ExecLiteralTest, UnionMemberMissingNameReports) {
  node_t *fields = create_fields_node(a);
  fields_node_add_field(fields, make_union_field_of(a, NULL, "i32"));
  node_t *decl = make_union_decl_of(a, "union", "MN", NULL, fields);
  clux_value_t *result = exec_expr_union(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "union 'MN' member 0 requires a name");
  delete_node(&decl);
}

/* 场景: 成员缺类型——union MT { a } 首成员无 TYPE
   预期: 报 "union 'MT' member 'a' requires a type" */
TEST_F(ExecLiteralTest, UnionMemberMissingTypeReports) {
  node_t *fields = create_fields_node(a);
  fields_node_add_field(fields, make_union_field_of(a, "a", NULL));
  node_t *decl = make_union_decl_of(a, "union", "MT", NULL, fields);
  clux_value_t *result = exec_expr_union(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "union 'MT' member 'a' requires a type");
  delete_node(&decl);
}

/* 场景: 重名成员——union DP { a: i32; a: f64 } 同名成员两次
   预期: 报 "union 'DP' has duplicate member 'a'" */
TEST_F(ExecLiteralTest, UnionDuplicateMemberReports) {
  node_t *fields = create_fields_node(a);
  fields_node_add_field(fields, make_union_field_of(a, "a", "i32"));
  fields_node_add_field(fields, make_union_field_of(a, "a", "f64"));
  node_t *decl = make_union_decl_of(a, "union", "DP", NULL, fields);
  clux_value_t *result = exec_expr_union(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "union 'DP' has duplicate member 'a'");
  delete_node(&decl);
}

/* 场景: 成员类型非 type value——union TN { a: 7 } 成员类型表达式求值
        出 i32 数值
   预期: 经公共类型通道拦截，报 "expected a type expression, got 'i32'" */
TEST_F(ExecLiteralTest, UnionMemberTypeNotTypeValueReports) {
  node_t *fields = create_fields_node(a);
  node_t *numeric_field = create_field_node(a);
  field_node_set_name(numeric_field,
                      make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "a"));
  field_node_set_type(numeric_field,
                      make_text_node(a, CLUX_NODE_LITERAL_NUMERIC, "7"));
  fields_node_add_field(fields, numeric_field);
  node_t *decl = make_union_decl_of(a, "union", "TN", NULL, fields);
  clux_value_t *result = exec_expr_union(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "expected a type expression, got 'i32'");
  delete_node(&decl);
}

/* 场景: 按名 define 重名——union DU 连续定义两次（成员表一致触发类型
        去重，但名字重复触发 scope duplicate 拦截）
   预期: 首次成功并按名 define；第二次报 "duplicate declaration 'DU'" */
TEST_F(ExecLiteralTest, UnionDefineDuplicateNameReports) {
  node_t *decl =
      make_union_decl_of(a, "union", "DU", NULL, make_two_member_fields(a));
  clux_value_t *first = exec_expr_union(ctx, decl);
  ASSERT_NE(first, nullptr);
  EXPECT_FALSE(clux_value_is_error(first));
  EXPECT_EQ(clux_context_lookup(ctx, "DU", NULL), first);

  clux_value_t *second = exec_expr_union(ctx, decl);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(second)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(second), "duplicate declaration 'DU'");
  delete_node(&decl);
}

/* 场景: 匿名求值——exec_expr_union_value 求 union UV { a: i32; b: f64 }
        但不按名 define；对照具名路径 exec_expr_union 仍按名 define
   预期: 产出 type value 且被描述类型 kind UNION，作用域中无 UV 名
         （lookup 返回 error 值）；具名路径 lookup 正常命中 */
TEST_F(ExecLiteralTest, UnionValueEvaluationIsAnonymous) {
  node_t *decl =
      make_union_decl_of(a, "union", "UV", NULL, make_two_member_fields(a));
  clux_value_t *value = exec_expr_union_value(ctx, decl);
  ASSERT_NE(value, nullptr);
  EXPECT_FALSE(clux_value_is_error(value));
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(value)), CLUX_TYPE_TYPE);
  clux_type_t *described = type_described_by(value);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_UNION);
  clux_value_t *probe = clux_context_lookup(ctx, "UV", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_TRUE(clux_value_is_error(probe))
      << "the anonymous evaluation must not define the name";
  EXPECT_STREQ(error_message_of(probe), "use of undeclared identifier 'UV'");
  delete_node(&decl);

  /* 具名路径回归：exec_expr_union 按名 define */
  node_t *named_decl =
      make_union_decl_of(a, "union", "UV2", NULL, make_two_member_fields(a));
  clux_value_t *named = exec_expr_union(ctx, named_decl);
  ASSERT_NE(named, nullptr);
  EXPECT_EQ(clux_context_lookup(ctx, "UV2", NULL), named)
      << "the named declaration path must not regress";
  delete_node(&named_decl);
}

/* 场景: STMT_UNION 无装饰器——union 语句包装 union SU 声明，经 exec_node
        顶层分派（覆盖 CLUX_NODE_STMT_UNION 槽位表注册）
   预期: 与直接执行声明本体等价：lookup("SU") 取回同一 type value，
         被描述类型 kind UNION */
TEST_F(ExecLiteralTest, StmtUnionWithoutDecoratorDefinesByName) {
  node_t *stmt = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt,
      make_union_decl_of(a, "union", "SU", NULL, make_two_member_fields(a)));
  clux_value_t *result = exec_node(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_context_lookup(ctx, "SU", NULL), result)
      << "the union statement must define the type value by name";
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_UNION);
  delete_node(&stmt);
}

/* 场景: STMT_UNION 缺声明——union 语句 UNION_DECL 槽为 NULL
   预期: 报 "union statement requires a union declaration" */
TEST_F(ExecLiteralTest, StmtUnionMissingDeclReports) {
  node_t *stmt = create_stmt_union_node(a);
  clux_value_t *result = exec_stmt_union(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "union statement requires a union declaration");
  delete_node(&stmt);
}

/* 场景: STMT_UNION 语句层声明必须具名——union {} 无名声明，
        无装饰器与带装饰器两种形态
   预期: 均报 "union statement requires a named union declaration"
        （语句包裹层强制具名，匿名只在表达式位置合法） */
TEST_F(ExecLiteralTest, StmtUnionMissingNameReports) {
  node_t *bare = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      bare,
      make_union_decl_of(a, "union", NULL, NULL, make_two_member_fields(a)));
  clux_value_t *bare_result = exec_stmt_union(ctx, bare);
  ASSERT_NE(bare_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bare_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bare_result),
               "union statement requires a named union declaration");
  delete_node(&bare);

  node_t *stmt = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt,
      make_union_decl_of(a, "union", NULL, NULL, make_two_member_fields(a)));
  stmt_union_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *result = exec_stmt_union(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "union statement requires a named union declaration");
  delete_node(&stmt);
}

/* 场景: STMT_UNION 装饰器链 e2e——装饰器是 comptime func 值
        wrap_t(u: type): type { return i32; }，union DW { a: i32; b: f64 }
        经 [[wrap_t]] 包装
   预期: 匿名求值源类型值 → invoke wrap_t（type value 作实参进帧，
         return i32 出帧）→ 最终名字 DW 绑定包装后的返回值：被描述
         类型恒等于 i32 builtin（证明包装真实发生而非透传）；
         invoke 前懒安装帧协议执行器 */
TEST_F(ExecLiteralTest, StmtUnionDecoratorWrapsTypeValueEndToEnd) {
  /* 元类型 type 不在 builtin 命名注册表（AST 无法表达"接收 type value
     的形参类型"），装饰器函数经引擎直建：create_func_type 以 meta 为
     形参/返回类型，AST 只被帧协议消费（形参类型槽惰性不读）。 */
  clux_type_t *meta = clux_context_get_type_type(ctx);
  clux_type_t *wrap_params[1] = {meta};
  clux_type_t *wrap_type = clux_context_create_func_type(
      ctx, "wrap_t", NULL, 0, wrap_params, 1, meta);
  ASSERT_NE(wrap_type, nullptr);
  /* AST 声明 func wrap_t(u: type): type { return i32; }——形参 u 的
     类型槽放占位 identifier（帧协议只读形参名）。 */
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(
          a,
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "u"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "type")));
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(
      ret, make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(body, ret);
  node_t *wrap_decl = make_func_decl_of(
      a,
      NULL,
      NULL,
      params,
      make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "wrap_t"),
      make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "type"),
      body);
  clux_value_t *wrap_func =
      clux_context_create_func(ctx, wrap_type, NULL, NULL);
  ASSERT_NE(wrap_func, nullptr);
  ASSERT_EQ(clux_context_func_set_ast(ctx, wrap_func, wrap_decl, NULL),
            wrap_func);
  clux_value_t *wrap_bound = clux_context_declare_value(
      ctx, wrap_func, "wrap_t", node_get_location(wrap_decl));
  ASSERT_NE(wrap_bound, nullptr);
  EXPECT_FALSE(clux_value_is_error(wrap_bound));

  node_t *stmt = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt,
      make_union_decl_of(a, "union", "DW", NULL, make_two_member_fields(a)));
  stmt_union_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "wrap_t")));
  clux_value_t *result = exec_stmt_union(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_context_lookup(ctx, "DW", NULL), result)
      << "the decorated path must bind the wrapped value under the name";
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(described, clux_context_builtin_type(ctx, CLUX_TYPE_I32))
      << "the decorator must substitute the source union type with its "
         "return type";
  EXPECT_NE(clux_context_get_executor(ctx), nullptr)
      << "invoking the func decorator must lazily install the executor";
  delete_node(&stmt);
  delete_node(&wrap_decl);
}

/* 场景: STMT_UNION 非函数装饰器透传——[[7]]（数值）与 [[i32]]（type
        value）都求值成功但非 func，按编译期标记跳过包装
   预期: 源 type value 原样透传：被描述类型仍为 UNION（成员数 2），
         lookup("DN") 取回同一值；不触发执行器安装（保持 NULL） */
TEST_F(ExecLiteralTest, StmtUnionNonFuncDecoratorsAreSkipped) {
  node_t *stmt = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt,
      make_union_decl_of(a, "union", "DN", NULL, make_two_member_fields(a)));
  stmt_union_node_add_decorator(
      stmt, make_decorator_of(a, make_node(CLUX_NODE_LITERAL_NUMERIC, "7")));
  stmt_union_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *result = exec_stmt_union(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_context_lookup(ctx, "DN", NULL), result);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_UNION);
  EXPECT_EQ(clux_union_member_count(described), 2u)
      << "non-func decorators must pass the source type value through";
  EXPECT_EQ(clux_context_get_executor(ctx), nullptr)
      << "non-func decorators must not touch the executor";
  delete_node(&stmt);
}

/* ---- exec_expr_enum_decl / exec_stmt_enum：enum 定义与语句（§3.6） ---- */

/* 合成 enum 成员节点 Name = value（name_text 为 NULL 构造缺名容错路径，
   value_text 为 NULL 构造缺值容错路径），delete 根结点时一并释放。 */
static node_t *make_enum_variant_of(allocator_t *allocator,
                                    const char *name_text,
                                    node_kind_t value_kind,
                                    const char *value_text) {
  node_t *variant = create_enum_variant_node(allocator);
  if (name_text) {
    enum_variant_node_set_name(
        variant,
        make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, name_text));
  }
  if (value_text) {
    enum_variant_node_set_value(
        variant, make_text_node(allocator, value_kind, value_text));
  }
  return variant;
}

/* 合成 enum 声明节点 enum NAME: UNDERLYING { body }：name_text/underlying_text
   为 NULL 构造缺名/缺底层类型容错路径，body 允许为 NULL，delete 根结点时
   一并释放子结点。 */
static node_t *make_enum_decl_of(allocator_t *allocator,
                                 const char *name_text,
                                 const char *underlying_text,
                                 node_t *body) {
  node_t *node = create_enum_decl_node(allocator);
  const char *loc_text = "enum";
  location_t loc;
  loc.filename = "test.clux";
  loc.begin.offset = loc_text;
  loc.begin.column = 1;
  loc.begin.line = 1;
  loc.end.offset = loc_text + strlen(loc_text);
  loc.end.column = 1;
  loc.end.line = 1;
  node_set_location(node, &loc);
  enum_decl_node_set_keyword(
      node, make_text_node(allocator, CLUX_NODE_SYMBOL, "enum"));
  if (name_text) {
    enum_decl_node_set_name(
        node,
        make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, name_text));
  }
  if (underlying_text) {
    enum_decl_node_set_underlying(node,
                                  make_text_node(allocator,
                                                 CLUX_NODE_LITERAL_IDENTIFIER,
                                                 underlying_text));
  }
  enum_decl_node_set_body(node, body);
  return node;
}

/* 合成三成员成员表 { Red=1, Green=2, Blue=4 }（e2e 与结构去重用例共用）。 */
static node_t *make_color_variants(allocator_t *allocator) {
  node_t *variants = create_enum_variants_node(allocator);
  enum_variants_node_add_variant(
      variants,
      make_enum_variant_of(allocator, "Red", CLUX_NODE_LITERAL_NUMERIC, "1"));
  enum_variants_node_add_variant(
      variants,
      make_enum_variant_of(allocator, "Green", CLUX_NODE_LITERAL_NUMERIC, "2"));
  enum_variants_node_add_variant(
      variants,
      make_enum_variant_of(allocator, "Blue", CLUX_NODE_LITERAL_NUMERIC, "4"));
  return variants;
}

/* 场景: enum 定义端到端——enum Color: u8 { Red=1, Green=2, Blue=4 }
   预期: 产出 type value（值类型 kind TYPE），lookup("Color") 取回同一值；
         被描述类型 kind ENUM、名 "Color"、成员数 3，按声明序可查成员且
         值为 1/2/4，find_variant 按名命中、未知名查无；底层类型恒等于
         u8 builtin、布局（size/align）继承底层类型（1/1） */
TEST_F(ExecLiteralTest, EnumDefinesColorEndToEnd) {
  node_t *decl = make_enum_decl_of(a, "Color", "u8", make_color_variants(a));
  clux_value_t *result = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE);
  EXPECT_EQ(clux_context_lookup(ctx, "Color", NULL), result)
      << "the named path must define the type value in the current scope";

  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_ENUM);
  EXPECT_STREQ(clux_type_get_name(described), "Color");
  EXPECT_EQ(clux_enum_variant_count(described), 3u);
  const clux_enum_variant_t *red = clux_enum_get_variant(described, 0);
  ASSERT_NE(red, nullptr);
  EXPECT_STREQ(red->name, "Red");
  EXPECT_EQ(red->value, 1u);
  const clux_enum_variant_t *green = clux_enum_get_variant(described, 1);
  ASSERT_NE(green, nullptr);
  EXPECT_STREQ(green->name, "Green");
  EXPECT_EQ(green->value, 2u);
  const clux_enum_variant_t *blue = clux_enum_get_variant(described, 2);
  ASSERT_NE(blue, nullptr);
  EXPECT_STREQ(blue->name, "Blue");
  EXPECT_EQ(blue->value, 4u);
  const clux_enum_variant_t *found = clux_enum_find_variant(described, "Green");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->value, 2u);
  EXPECT_EQ(clux_enum_find_variant(described, "zzz"), nullptr)
      << "unknown variant lookup must not match";
  EXPECT_EQ(clux_enum_get_variant(described, 3), nullptr)
      << "out-of-bounds variant index must not match";
  EXPECT_EQ(clux_enum_get_underlying(described),
            clux_context_builtin_type(ctx, CLUX_TYPE_U8));
  EXPECT_EQ(clux_type_get_size(described),
            clux_type_get_size(clux_context_builtin_type(ctx, CLUX_TYPE_U8)));
  EXPECT_EQ(clux_type_get_align(described),
            clux_type_get_align(clux_context_builtin_type(ctx, CLUX_TYPE_U8)));
  delete_node(&decl);
}

/* 场景: 结构去重恒等——enum E1: u8 { Red=1; Green=2 } 与 enum E2: u8
        { Red=1; Green=2 } 成员表一致（类型名不同），对照 enum E3: u8
        { Red=1; Green=3 } 成员值不同
   预期: E1/E2 解出的被描述类型指针恒等（结构判等不看类型名）；
         E3 与 E1 指针不同 */
TEST_F(ExecLiteralTest, EnumStructuralDedupYieldsIdenticalType) {
  node_t *variants_12 = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      variants_12,
      make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_NUMERIC, "1"));
  enum_variants_node_add_variant(
      variants_12,
      make_enum_variant_of(a, "Green", CLUX_NODE_LITERAL_NUMERIC, "2"));
  node_t *decl_e1 = make_enum_decl_of(a, "E1", "u8", variants_12);
  clux_value_t *value_e1 = exec_expr_enum_decl(ctx, decl_e1);
  ASSERT_NE(value_e1, nullptr);
  EXPECT_FALSE(clux_value_is_error(value_e1));
  clux_type_t *type_e1 = type_described_by(value_e1);
  ASSERT_NE(type_e1, nullptr);

  node_t *variants_12b = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      variants_12b,
      make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_NUMERIC, "1"));
  enum_variants_node_add_variant(
      variants_12b,
      make_enum_variant_of(a, "Green", CLUX_NODE_LITERAL_NUMERIC, "2"));
  node_t *decl_e2 = make_enum_decl_of(a, "E2", "u8", variants_12b);
  clux_value_t *value_e2 = exec_expr_enum_decl(ctx, decl_e2);
  ASSERT_NE(value_e2, nullptr);
  EXPECT_FALSE(clux_value_is_error(value_e2));
  clux_type_t *type_e2 = type_described_by(value_e2);
  ASSERT_NE(type_e2, nullptr);
  EXPECT_EQ(type_e1, type_e2)
      << "identical variant tables must dedup to the same type instance";

  node_t *variants_13 = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      variants_13,
      make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_NUMERIC, "1"));
  enum_variants_node_add_variant(
      variants_13,
      make_enum_variant_of(a, "Green", CLUX_NODE_LITERAL_NUMERIC, "3"));
  node_t *decl_e3 = make_enum_decl_of(a, "E3", "u8", variants_13);
  clux_value_t *value_e3 = exec_expr_enum_decl(ctx, decl_e3);
  ASSERT_NE(value_e3, nullptr);
  clux_type_t *type_e3 = type_described_by(value_e3);
  ASSERT_NE(type_e3, nullptr);
  EXPECT_NE(type_e1, type_e3)
      << "different variant values must not dedup to the same instance";
  delete_node(&decl_e1);
  delete_node(&decl_e2);
  delete_node(&decl_e3);
}

namespace {

/* 便捷构造：enum <name>: u8 { 指定成员表 } 并 exec 求值，返回被描述类型。 */
clux_type_t *describe_enum_of(clux_context_t *ctx,
                              allocator_t *a,
                              const char *name,
                              node_t *(*make_variants)(allocator_t *)) {
  node_t *decl = make_enum_decl_of(a, name, "u8", make_variants(a));
  clux_value_t *value = exec_expr_enum_decl(ctx, decl);
  if (!value || clux_value_is_error(value)) {
    delete_node(&decl);
    return nullptr;
  }
  clux_type_t *described = type_described_by(value);
  delete_node(&decl);
  return described;
}

node_t *make_red_green_variants(allocator_t *a) {
  node_t *variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      variants, make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_NUMERIC, "1"));
  enum_variants_node_add_variant(
      variants,
      make_enum_variant_of(a, "Green", CLUX_NODE_LITERAL_NUMERIC, "2"));
  return variants;
}

node_t *make_crimson_green_variants(allocator_t *a) {
  node_t *variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      variants,
      make_enum_variant_of(a, "Crimson", CLUX_NODE_LITERAL_NUMERIC, "1"));
  enum_variants_node_add_variant(
      variants,
      make_enum_variant_of(a, "Green", CLUX_NODE_LITERAL_NUMERIC, "2"));
  return variants;
}

node_t *make_red_green_high_variants(allocator_t *a) {
  node_t *variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      variants, make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_NUMERIC, "1"));
  enum_variants_node_add_variant(
      variants,
      make_enum_variant_of(a, "Green", CLUX_NODE_LITERAL_NUMERIC, "9"));
  return variants;
}

} // namespace

/* 场景: enum 类型经 vtable type_equal 通道的结构判等（enum_type_equal）
   预期: 自反判等 true；与结构不同（值不同/成员名不同/成员数不同）的
         enum 判等 false；与非 enum 类型（u8 builtin）判等 false；
         find_variant 对 NULL name 走容错返回 NULL */
TEST_F(ExecLiteralTest, EnumTypeEqualIsStructuralViaVtable) {
  clux_type_t *type_self =
      describe_enum_of(ctx, a, "TE1", make_red_green_variants);
  ASSERT_NE(type_self, nullptr);
  EXPECT_EQ(clux_type_get_kind(type_self), CLUX_TYPE_ENUM);

  /* 自反：底层一致 + 成员名值全匹配 → true（相等路径） */
  EXPECT_TRUE(clux_type_equal(type_self, type_self))
      << "an enum type must be equal to itself via the vtable channel";

  /* 值不同（Green=2 vs Green=9）→ false */
  clux_type_t *type_value =
      describe_enum_of(ctx, a, "TE2", make_red_green_high_variants);
  ASSERT_NE(type_value, nullptr);
  EXPECT_NE(type_self, type_value)
      << "different variant values must not dedup to the same instance";
  EXPECT_FALSE(clux_type_equal(type_self, type_value))
      << "variant value mismatch must fail type equality";

  /* 成员名不同（Red vs Crimson）→ false */
  clux_type_t *type_name =
      describe_enum_of(ctx, a, "TE3", make_crimson_green_variants);
  ASSERT_NE(type_name, nullptr);
  EXPECT_FALSE(clux_type_equal(type_self, type_name))
      << "variant name mismatch must fail type equality";

  /* 成员数不同（2 vs 3）→ false */
  clux_type_t *type_count =
      describe_enum_of(ctx, a, "TE4", make_color_variants);
  ASSERT_NE(type_count, nullptr);
  EXPECT_FALSE(clux_type_equal(type_self, type_count))
      << "variant count mismatch must fail type equality";

  /* 对端非 enum 类型（u8 builtin）→ false（enum_kind 检查分支） */
  clux_type_t *u8_type = clux_context_builtin_type(ctx, CLUX_TYPE_U8);
  ASSERT_NE(u8_type, nullptr);
  EXPECT_FALSE(clux_type_equal(type_self, u8_type))
      << "an enum type must not equal its underlying builtin type";

  /* find_variant 的 NULL name 容错分支 */
  EXPECT_EQ(clux_enum_find_variant(type_self, NULL), nullptr)
      << "NULL variant name must be tolerated and miss";
}

/* 场景: 匿名 enum（无 NAME 槽）——表达式位置合法
   预期: 成功产出 type value（描述 ENUM 类型、成员数 3），不按名 define
         （lookup 报 undeclared） */
TEST_F(ExecLiteralTest, EnumAnonymousDefinitionYieldsUnnamedTypeValue) {
  node_t *decl = make_enum_decl_of(a, NULL, "u8", make_color_variants(a));
  clux_value_t *result = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_TYPE);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_ENUM);
  EXPECT_EQ(clux_enum_variant_count(described), 3u);
  clux_value_t *probe = clux_context_lookup(ctx, "AnonEnumProbe", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_TRUE(clux_value_is_error(probe))
      << "the anonymous enum must not define any name";
  EXPECT_STREQ(error_message_of(probe),
               "use of undeclared identifier 'AnonEnumProbe'");
  delete_node(&decl);
}

/* 场景: 匿名求值——exec_expr_enum_decl_value 求 enum EV: u8 { ... } 但
        不按名 define；随后 declare_value 具名化；对照具名路径回归
   预期: 产出 type value 且被描述类型 kind ENUM，作用域中无 EV 名；
         具名化后 lookup 命中；具名路径 lookup 正常命中 */
TEST_F(ExecLiteralTest, EnumValueEvaluationIsAnonymous) {
  node_t *decl = make_enum_decl_of(a, "EV", "u8", make_color_variants(a));
  clux_value_t *value = exec_expr_enum_decl_value(ctx, decl);
  ASSERT_NE(value, nullptr);
  EXPECT_FALSE(clux_value_is_error(value));
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(value)), CLUX_TYPE_TYPE);
  clux_type_t *described = type_described_by(value);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_ENUM);
  clux_value_t *probe = clux_context_lookup(ctx, "EV", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_TRUE(clux_value_is_error(probe))
      << "the anonymous evaluation must not define the name";
  EXPECT_STREQ(error_message_of(probe), "use of undeclared identifier 'EV'");

  /* 具名化（exec_stmt_enum 装饰路径的收尾通道） */
  clux_value_t *bound =
      clux_context_declare_value(ctx, value, "EV", node_get_location(decl));
  ASSERT_NE(bound, nullptr);
  EXPECT_EQ(clux_context_lookup(ctx, "EV", NULL), bound);
  delete_node(&decl);

  /* 具名路径回归：exec_expr_enum_decl 按名 define */
  node_t *named_decl =
      make_enum_decl_of(a, "EV2", "u8", make_color_variants(a));
  clux_value_t *named = exec_expr_enum_decl(ctx, named_decl);
  ASSERT_NE(named, nullptr);
  EXPECT_EQ(clux_context_lookup(ctx, "EV2", NULL), named)
      << "the named declaration path must not regress";
  delete_node(&named_decl);
}

/* 场景: 缺底层类型——UNDERLYING 槽缺省，具名与匿名（回退名 "enum"）
        两种形态
   预期: 具名报 "enum 'NU' requires an underlying type"；
         匿名报 "enum 'enum' requires an underlying type"（关键字回退名） */
TEST_F(ExecLiteralTest, EnumMissingUnderlyingReports) {
  node_t *named = make_enum_decl_of(a, "NU", NULL, make_color_variants(a));
  clux_value_t *named_result = exec_expr_enum_decl(ctx, named);
  ASSERT_NE(named_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(named_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(named_result),
               "enum 'NU' requires an underlying type");
  delete_node(&named);

  node_t *anon = make_enum_decl_of(a, NULL, NULL, make_color_variants(a));
  clux_value_t *anon_result = exec_expr_enum_decl(ctx, anon);
  ASSERT_NE(anon_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(anon_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(anon_result),
               "enum 'enum' requires an underlying type")
      << "the anonymous form must fall back to the keyword text as type name";
  delete_node(&anon);
}

/* 场景: 底层非整数且非 str——enum NI: f64 { ... } 底层类型求值为浮点类型
   预期: 报 "enum 'NI' underlying type must be an integer or str type" */
TEST_F(ExecLiteralTest, EnumUnderlyingNotIntegerReports) {
  node_t *decl = make_enum_decl_of(a, "NI", "f64", make_color_variants(a));
  clux_value_t *result = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "enum 'NI' underlying type must be an integer or str type");
  delete_node(&decl);
}

/* 场景: 底层类型表达式非 type value——enum NT: 7 底层求值出 i32 数值
   预期: 经公共类型通道拦截，报 "expected a type expression, got 'i32'" */
TEST_F(ExecLiteralTest, EnumUnderlyingNotTypeValueReports) {
  node_t *decl = create_enum_decl_node(a);
  const char *loc_text = "enum";
  location_t loc;
  loc.filename = "test.clux";
  loc.begin.offset = loc_text;
  loc.begin.column = 1;
  loc.begin.line = 1;
  loc.end.offset = loc_text + strlen(loc_text);
  loc.end.column = 1;
  loc.end.line = 1;
  node_set_location(decl, &loc);
  enum_decl_node_set_keyword(decl, make_text_node(a, CLUX_NODE_SYMBOL, "enum"));
  enum_decl_node_set_name(
      decl, make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "NT"));
  enum_decl_node_set_underlying(
      decl, make_text_node(a, CLUX_NODE_LITERAL_NUMERIC, "7"));
  enum_decl_node_set_body(decl, make_color_variants(a));
  clux_value_t *result = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "expected a type expression, got 'i32'");
  delete_node(&decl);
}

/* 场景: 空成员表——BODY 为 NULL 与空成员节点 {} 两种容错形态
   预期: 均报 "enum 'EB' requires at least one variant" */
TEST_F(ExecLiteralTest, EnumEmptyBodyReports) {
  node_t *null_body = make_enum_decl_of(a, "EB", "u8", NULL);
  clux_value_t *null_result = exec_expr_enum_decl(ctx, null_body);
  ASSERT_NE(null_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(null_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(null_result),
               "enum 'EB' requires at least one variant");
  delete_node(&null_body);

  node_t *empty_body =
      make_enum_decl_of(a, "EB", "u8", create_enum_variants_node(a));
  clux_value_t *empty_result = exec_expr_enum_decl(ctx, empty_body);
  ASSERT_NE(empty_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(empty_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(empty_result),
               "enum 'EB' requires at least one variant");
  delete_node(&empty_body);
}

/* 场景: 成员缺名——enum MN: u8 { = 1 } 首成员无 NAME
   预期: 报 "enum 'MN' variant 0 requires a name" */
TEST_F(ExecLiteralTest, EnumVariantMissingNameReports) {
  node_t *variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      variants, make_enum_variant_of(a, NULL, CLUX_NODE_LITERAL_NUMERIC, "1"));
  node_t *decl = make_enum_decl_of(a, "MN", "u8", variants);
  clux_value_t *result = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "enum 'MN' variant 0 requires a name");
  delete_node(&decl);
}

/* 场景: 成员缺值——enum MV: u8 { Red } 首成员无 VALUE
   预期: 报 "enum 'MV' variant 'Red' requires a value" */
TEST_F(ExecLiteralTest, EnumVariantMissingValueReports) {
  node_t *variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      variants,
      make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_NUMERIC, NULL));
  node_t *decl = make_enum_decl_of(a, "MV", "u8", variants);
  clux_value_t *result = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "enum 'MV' variant 'Red' requires a value");
  delete_node(&decl);
}

/* 场景: 成员值非整数常量——enum VT: u8 { Red = 1.5f64 }（浮点后缀字面量；
        无后缀浮点由语法层强制禁止，exec 整数通道对 "1.5" 宽容截断为 1，
        故须带 f64 后缀才能求值出浮点 kind）与 enum VS: u8 { Red = i32 }
        （type value，shadow 值无位型可取）
   预期: 均报 "enum '%s' variant 'Red' requires an integer constant value" */
TEST_F(ExecLiteralTest, EnumVariantValueNotIntegerReports) {
  node_t *float_variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      float_variants,
      make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_NUMERIC, "1.5f64"));
  node_t *float_decl = make_enum_decl_of(a, "VT", "u8", float_variants);
  clux_value_t *float_result = exec_expr_enum_decl(ctx, float_decl);
  ASSERT_NE(float_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(float_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(float_result),
               "enum 'VT' variant 'Red' requires an integer constant value");
  delete_node(&float_decl);

  node_t *shadow_variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      shadow_variants,
      make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *shadow_decl = make_enum_decl_of(a, "VS", "u8", shadow_variants);
  clux_value_t *shadow_result = exec_expr_enum_decl(ctx, shadow_decl);
  ASSERT_NE(shadow_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(shadow_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(shadow_result),
               "enum 'VS' variant 'Red' requires an integer constant value");
  delete_node(&shadow_decl);
}

/* 场景: 成员值求值错误透传——enum VE: u8 { Red = <STMT_EMPTY> } 值表达式
        求值即失败
   预期: 值求值错误原样透传，报 "unsupported node kind: STMT_EMPTY" */
TEST_F(ExecLiteralTest, EnumVariantValueErrorPropagates) {
  node_t *variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      variants, make_enum_variant_of(a, "Red", CLUX_NODE_STMT_EMPTY, "stmt"));
  node_t *decl = make_enum_decl_of(a, "VE", "u8", variants);
  clux_value_t *result = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "unsupported node kind: STMT_EMPTY")
      << "the variant value evaluation failure must propagate unchanged";
  delete_node(&decl);
}

/* 场景: 重名成员——enum DP: u8 { Red = 1; Red = 2 } 同名成员两次
   预期: 报 "enum 'DP' has duplicate variant 'Red'" */
TEST_F(ExecLiteralTest, EnumDuplicateVariantReports) {
  node_t *variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      variants, make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_NUMERIC, "1"));
  enum_variants_node_add_variant(
      variants, make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_NUMERIC, "2"));
  node_t *decl = make_enum_decl_of(a, "DP", "u8", variants);
  clux_value_t *result = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "enum 'DP' has duplicate variant 'Red'");
  delete_node(&decl);
}

/* 场景: 按名 define 重名——enum DU: u8 { ... } 连续定义两次（成员表一致
        触发类型去重，但名字重复触发 scope duplicate 拦截）
   预期: 首次成功并按名 define；第二次报 "duplicate declaration 'DU'" */
TEST_F(ExecLiteralTest, EnumDefineDuplicateNameReports) {
  node_t *decl = make_enum_decl_of(a, "DU", "u8", make_color_variants(a));
  clux_value_t *first = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(first, nullptr);
  EXPECT_FALSE(clux_value_is_error(first));
  EXPECT_EQ(clux_context_lookup(ctx, "DU", NULL), first);

  clux_value_t *second = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(second)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(second), "duplicate declaration 'DU'");
  delete_node(&decl);
}

/* 场景: STMT_ENUM 无装饰器——enum 语句包装 enum SE: u8 { ... } 声明，
        经 exec_node 顶层分派（覆盖 CLUX_NODE_STMT_ENUM 槽位表注册）
   预期: 与直接执行声明本体等价：lookup("SE") 取回同一 type value，
         被描述类型 kind ENUM */
TEST_F(ExecLiteralTest, StmtEnumWithoutDecoratorDefinesByName) {
  node_t *stmt = create_stmt_enum_node(a);
  stmt_enum_node_set_enum_decl(
      stmt, make_enum_decl_of(a, "SE", "u8", make_color_variants(a)));
  clux_value_t *result = exec_node(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_context_lookup(ctx, "SE", NULL), result)
      << "the enum statement must define the type value by name";
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_ENUM);
  delete_node(&stmt);
}

/* 场景: STMT_ENUM 缺声明——enum 语句 ENUM_DECL 槽为 NULL
   预期: 报 "enum statement requires an enum declaration" */
TEST_F(ExecLiteralTest, StmtEnumMissingDeclReports) {
  node_t *stmt = create_stmt_enum_node(a);
  clux_value_t *result = exec_stmt_enum(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "enum statement requires an enum declaration");
  delete_node(&stmt);
}

/* 场景: STMT_ENUM 语句层声明必须具名——enum {} 无名声明，
        无装饰器与带装饰器两种形态
   预期: 均报 "enum statement requires a named enum declaration"
        （语句包裹层强制具名，匿名只在表达式位置合法） */
TEST_F(ExecLiteralTest, StmtEnumMissingNameReports) {
  node_t *bare = create_stmt_enum_node(a);
  stmt_enum_node_set_enum_decl(
      bare, make_enum_decl_of(a, NULL, "u8", make_color_variants(a)));
  clux_value_t *bare_result = exec_stmt_enum(ctx, bare);
  ASSERT_NE(bare_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bare_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bare_result),
               "enum statement requires a named enum declaration");
  delete_node(&bare);

  node_t *stmt = create_stmt_enum_node(a);
  stmt_enum_node_set_enum_decl(
      stmt, make_enum_decl_of(a, NULL, "u8", make_color_variants(a)));
  stmt_enum_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *result = exec_stmt_enum(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "enum statement requires a named enum declaration");
  delete_node(&stmt);
}

/* 场景: STMT_ENUM 装饰器链 e2e——装饰器是 comptime func 值
        wrap_t(u: type): type { return i32; }，enum DW: u8 { Red = 1 }
        经 [[wrap_t]] 包装
   预期: 匿名求值源类型值 → invoke wrap_t（type value 作实参进帧，
         return i32 出帧）→ 最终名字 DW 绑定包装后的返回值：被描述
         类型恒等于 i32 builtin（证明包装真实发生而非透传）；
         invoke 前懒安装帧协议执行器 */
TEST_F(ExecLiteralTest, StmtEnumDecoratorWrapsTypeValueEndToEnd) {
  clux_type_t *meta = clux_context_get_type_type(ctx);
  clux_type_t *wrap_params[1] = {meta};
  clux_type_t *wrap_type = clux_context_create_func_type(
      ctx, "wrap_t", NULL, 0, wrap_params, 1, meta);
  ASSERT_NE(wrap_type, nullptr);
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(
          a,
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "u"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "type")));
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(
      ret, make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(body, ret);
  node_t *wrap_decl = make_func_decl_of(
      a,
      NULL,
      NULL,
      params,
      make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "wrap_t"),
      make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "type"),
      body);
  clux_value_t *wrap_func =
      clux_context_create_func(ctx, wrap_type, NULL, NULL);
  ASSERT_NE(wrap_func, nullptr);
  ASSERT_EQ(clux_context_func_set_ast(ctx, wrap_func, wrap_decl, NULL),
            wrap_func);
  clux_value_t *wrap_bound = clux_context_declare_value(
      ctx, wrap_func, "wrap_t", node_get_location(wrap_decl));
  ASSERT_NE(wrap_bound, nullptr);
  EXPECT_FALSE(clux_value_is_error(wrap_bound));

  node_t *stmt = create_stmt_enum_node(a);
  node_t *dw_variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      dw_variants,
      make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_NUMERIC, "1"));
  stmt_enum_node_set_enum_decl(stmt,
                               make_enum_decl_of(a, "DW", "u8", dw_variants));
  stmt_enum_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "wrap_t")));
  clux_value_t *result = exec_stmt_enum(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_context_lookup(ctx, "DW", NULL), result)
      << "the decorated path must bind the wrapped value under the name";
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(described, clux_context_builtin_type(ctx, CLUX_TYPE_I32))
      << "the decorator must substitute the source enum type with its "
         "return type";
  EXPECT_NE(clux_context_get_executor(ctx), nullptr)
      << "invoking the func decorator must lazily install the executor";
  delete_node(&stmt);
  delete_node(&wrap_decl);
}

/* 场景: STMT_ENUM 非函数装饰器透传——[[7]]（数值）与 [[i32]]（type
        value）都求值成功但非 func，按编译期标记跳过包装
   预期: 源 type value 原样透传：被描述类型仍为 ENUM（成员数 3），
         lookup("DN") 取回同一值；不触发执行器安装（保持 NULL） */
TEST_F(ExecLiteralTest, StmtEnumNonFuncDecoratorsAreSkipped) {
  node_t *stmt = create_stmt_enum_node(a);
  stmt_enum_node_set_enum_decl(
      stmt, make_enum_decl_of(a, "DN", "u8", make_color_variants(a)));
  stmt_enum_node_add_decorator(
      stmt, make_decorator_of(a, make_node(CLUX_NODE_LITERAL_NUMERIC, "7")));
  stmt_enum_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *result = exec_stmt_enum(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_context_lookup(ctx, "DN", NULL), result);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_ENUM);
  EXPECT_EQ(clux_enum_variant_count(described), 3u)
      << "non-func decorators must pass the source type value through";
  EXPECT_EQ(clux_context_get_executor(ctx), nullptr)
      << "non-func decorators must not touch the executor";
  delete_node(&stmt);
}

/* 合成两成员 str 成员表 { Red="red", Green="green" }：VALUE 槽放字符串
   字面量节点（str 底层 e2e 与结构去重用例共用）。 */
static node_t *make_str_variants_red_green(allocator_t *allocator) {
  node_t *variants = create_enum_variants_node(allocator);
  enum_variants_node_add_variant(
      variants,
      make_enum_variant_of(
          allocator, "Red", CLUX_NODE_LITERAL_STRING, "\"red\""));
  enum_variants_node_add_variant(
      variants,
      make_enum_variant_of(
          allocator, "Green", CLUX_NODE_LITERAL_STRING, "\"green\""));
  return variants;
}

/* 场景: str 底层 enum 端到端——enum S: str { Red="red", Green="green" }
   预期: 产出 type value 并按名 define；被描述类型 kind ENUM、名 "S"；
         成员数 2，按声明序可查成员且 str 内容正确、value 恒 0；
         find_variant 按名命中、未知名查无；底层为 str builtin，布局
         取 underlying 的 size/align（即一个 char* 的大小与对齐） */
TEST_F(ExecLiteralTest, EnumStrUnderlyingDefinesEndToEnd) {
  node_t *decl =
      make_enum_decl_of(a, "S", "str", make_str_variants_red_green(a));
  clux_value_t *result = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE);
  EXPECT_EQ(clux_context_lookup(ctx, "S", NULL), result)
      << "the named path must define the type value in the current scope";

  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_ENUM);
  EXPECT_STREQ(clux_type_get_name(described), "S");
  EXPECT_EQ(clux_enum_variant_count(described), 2u);
  const clux_enum_variant_t *red = clux_enum_get_variant(described, 0);
  ASSERT_NE(red, nullptr);
  EXPECT_STREQ(red->name, "Red");
  EXPECT_STREQ(red->str, "red");
  EXPECT_EQ(red->value, 0u) << "str variants must not use the value field";
  const clux_enum_variant_t *green = clux_enum_get_variant(described, 1);
  ASSERT_NE(green, nullptr);
  EXPECT_STREQ(green->name, "Green");
  EXPECT_STREQ(green->str, "green");
  EXPECT_EQ(green->value, 0u);
  const clux_enum_variant_t *found = clux_enum_find_variant(described, "Red");
  ASSERT_NE(found, nullptr);
  EXPECT_STREQ(found->str, "red");
  EXPECT_EQ(clux_enum_find_variant(described, "zzz"), nullptr)
      << "unknown variant lookup must not match";
  EXPECT_EQ(clux_enum_get_variant(described, 2), nullptr)
      << "out-of-bounds variant index must not match";

  clux_type_t *str_type = clux_context_builtin_type(ctx, CLUX_TYPE_STR);
  ASSERT_NE(str_type, nullptr);
  EXPECT_EQ(clux_enum_get_underlying(described), str_type)
      << "the underlying type must resolve to the str builtin";
  EXPECT_EQ(clux_type_get_size(described), sizeof(char *))
      << "a str enum must be laid out as one char*";
  EXPECT_EQ(clux_type_get_size(described), clux_type_get_size(str_type));
  EXPECT_EQ(clux_type_get_align(described), clux_type_get_align(str_type));
  delete_node(&decl);
}

/* 场景: str 底层结构去重——enum SD1: str { Red="red"; Green="green" }
        与 enum SD2: str { Red="red"; Green="green" } 成员表一致（声明名
        不同、字面量节点独立），对照 enum SD3: str { Red="scarlet";
        Green="green" } str 内容不同
   预期: SD1/SD2 解出的被描述类型指针恒等（str 按内容判等、类型名不
         参与去重）；SD3 与 SD1 指针不同 */
TEST_F(ExecLiteralTest, EnumStrStructuralDedupYieldsIdenticalType) {
  node_t *decl_s1 =
      make_enum_decl_of(a, "SD1", "str", make_str_variants_red_green(a));
  clux_value_t *value_s1 = exec_expr_enum_decl(ctx, decl_s1);
  ASSERT_NE(value_s1, nullptr);
  EXPECT_FALSE(clux_value_is_error(value_s1));
  clux_type_t *type_s1 = type_described_by(value_s1);
  ASSERT_NE(type_s1, nullptr);

  node_t *decl_s2 =
      make_enum_decl_of(a, "SD2", "str", make_str_variants_red_green(a));
  clux_value_t *value_s2 = exec_expr_enum_decl(ctx, decl_s2);
  ASSERT_NE(value_s2, nullptr);
  EXPECT_FALSE(clux_value_is_error(value_s2));
  clux_type_t *type_s2 = type_described_by(value_s2);
  ASSERT_NE(type_s2, nullptr);
  EXPECT_EQ(type_s1, type_s2)
      << "identical str variant tables must dedup to the same type instance";

  node_t *scarlet_variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      scarlet_variants,
      make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_STRING, "\"scarlet\""));
  enum_variants_node_add_variant(
      scarlet_variants,
      make_enum_variant_of(a, "Green", CLUX_NODE_LITERAL_STRING, "\"green\""));
  node_t *decl_s3 = make_enum_decl_of(a, "SD3", "str", scarlet_variants);
  clux_value_t *value_s3 = exec_expr_enum_decl(ctx, decl_s3);
  ASSERT_NE(value_s3, nullptr);
  EXPECT_FALSE(clux_value_is_error(value_s3));
  clux_type_t *type_s3 = type_described_by(value_s3);
  ASSERT_NE(type_s3, nullptr);
  EXPECT_NE(type_s1, type_s3)
      << "different str contents must not dedup to the same instance";
  delete_node(&decl_s1);
  delete_node(&decl_s2);
  delete_node(&decl_s3);
}

/* 场景: str 底层配整数值——enum SI: str { Red = 1 } 成员值是整数常量
   预期: 报 "enum 'SI' variant 'Red' requires a string constant value" */
TEST_F(ExecLiteralTest, EnumStrUnderlyingWithIntegerValueReports) {
  node_t *variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      variants, make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_NUMERIC, "1"));
  node_t *decl = make_enum_decl_of(a, "SI", "str", variants);
  clux_value_t *result = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "enum 'SI' variant 'Red' requires a string constant value");
  delete_node(&decl);
}

/* 场景: 整数底层配字符串值——enum IS: u8 { Red = "red" } 成员值是
        字符串常量（integer 通道的 str kind 分支）
   预期: 报 "enum 'IS' variant 'Red' requires an integer constant value" */
TEST_F(ExecLiteralTest, EnumIntegerUnderlyingWithStringValueReports) {
  node_t *variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      variants,
      make_enum_variant_of(a, "Red", CLUX_NODE_LITERAL_STRING, "\"red\""));
  node_t *decl = make_enum_decl_of(a, "IS", "u8", variants);
  clux_value_t *result = exec_expr_enum_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "enum 'IS' variant 'Red' requires an integer constant value");
  delete_node(&decl);
}

/* 场景: STMT_ENUM + str 底层——bare 形态（无装饰器）语句包装具名
        enum SS: str { Red="red", Green="green" }，经 exec_node 顶层分派
   预期: lookup("SS") 取回 type value；被描述类型 kind ENUM、str 成员
         内容正确 */
TEST_F(ExecLiteralTest, StmtEnumWithStrUnderlyingDefinesByName) {
  node_t *stmt = create_stmt_enum_node(a);
  stmt_enum_node_set_enum_decl(
      stmt, make_enum_decl_of(a, "SS", "str", make_str_variants_red_green(a)));
  clux_value_t *result = exec_node(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_context_lookup(ctx, "SS", NULL), result)
      << "the bare enum statement must define the str enum type by name";
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_ENUM);
  EXPECT_EQ(clux_enum_variant_count(described), 2u);
  const clux_enum_variant_t *red = clux_enum_get_variant(described, 0);
  ASSERT_NE(red, nullptr);
  EXPECT_STREQ(red->str, "red");
  EXPECT_EQ(red->value, 0u);
  delete_node(&stmt);
}

/* ---- exec "as"：EXPR_BINARY 接线显式转换（enum 值经 underlying 委托） ---- */

namespace {

/* 经 exec 定义 enum <name>: <underlying_text> { Red=1, Green=2, Blue=4 }
   并返回被描述类型（失败返回 nullptr，节点已释放）。 */
clux_type_t *describe_color_underlying(clux_context_t *ctx,
                                       allocator_t *a,
                                       const char *name,
                                       const char *underlying_text) {
  node_t *decl =
      make_enum_decl_of(a, name, underlying_text, make_color_variants(a));
  clux_value_t *value = exec_expr_enum_decl(ctx, decl);
  clux_type_t *described = value ? type_described_by(value) : nullptr;
  delete_node(&decl);
  if (!value || clux_value_is_error(value)) {
    return nullptr;
  }
  return described;
}

/* 构造持有指定位型的具名枚举值（i32 位型），lookup 按名可达。 */
clux_value_t *make_named_enum_bits_i32(clux_context_t *ctx,
                                       clux_type_t *enum_type,
                                       const char *name,
                                       int32_t bits) {
  ptr_handle_t data =
      safe_alloc(clux_context_get_safe_allocator(ctx), sizeof(int32_t), NULL);
  if (data == 0) {
    return nullptr;
  }
  void *raw = ptr_to_raw(data, 0);
  if (!raw) {
    return nullptr;
  }
  *(int32_t *)raw = bits;
  return clux_context_create_value(ctx, enum_type, data, name, NULL);
}

} /* namespace */

/* 场景: exec e2e 宽化——var ev: Color(i32) = 2; ev as i64
   预期: "as" 左操作数求值、右操作数走类型表达式通道，经 clux_context_cast
         委托 underlying 宽化成功，产出 i64 值 2 */
TEST_F(ExecLiteralTest, EnumAsWidensToI64) {
  clux_type_t *color = describe_color_underlying(ctx, a, "Color", "i32");
  ASSERT_NE(color, nullptr);
  clux_value_t *ev = make_named_enum_bits_i32(ctx, color, "ev", 2);
  ASSERT_NE(ev, nullptr);

  node_t *node = make_binary_of(a,
                                make_node(CLUX_NODE_LITERAL_IDENTIFIER, "ev"),
                                make_node(CLUX_NODE_SYMBOL, "as"),
                                make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i64"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_I64));
  EXPECT_EQ(read_i64(result), 2);
  delete_node(&node);
}

/* 场景: exec e2e 显式 narrowing——ev(300) as i8（exec 走宽松 cast 通道）
   预期: 截断为 i8（300 & 0xFF == 44），类型 i8 */
TEST_F(ExecLiteralTest, EnumAsExplicitNarrowingTruncates) {
  clux_type_t *color = describe_color_underlying(ctx, a, "Color", "i32");
  ASSERT_NE(color, nullptr);
  clux_value_t *ev = make_named_enum_bits_i32(ctx, color, "ev", 300);
  ASSERT_NE(ev, nullptr);

  node_t *node = make_binary_of(a,
                                make_node(CLUX_NODE_LITERAL_IDENTIFIER, "ev"),
                                make_node(CLUX_NODE_SYMBOL, "as"),
                                make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i8"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_builtin_type(ctx, CLUX_TYPE_I8));
  EXPECT_EQ(read_i8(result), 44);
  delete_node(&node);
}

/* 场景: exec e2e 枚举 as 同布局枚举——E1(i32) 值 7 as E2(i32)
   预期: 产出 E2 被描述类型的枚举值，位型保留 */
TEST_F(ExecLiteralTest, EnumAsToSameLayoutEnumPreservesBits) {
  clux_type_t *e1 = describe_color_underlying(ctx, a, "E1", "i32");
  ASSERT_NE(e1, nullptr);
  /* E2 成员表不同名，结构判等不去重为 E1 */
  node_t *e2_variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      e2_variants,
      make_enum_variant_of(a, "Crimson", CLUX_NODE_LITERAL_NUMERIC, "1"));
  enum_variants_node_add_variant(
      e2_variants,
      make_enum_variant_of(a, "Emerald", CLUX_NODE_LITERAL_NUMERIC, "2"));
  enum_variants_node_add_variant(
      e2_variants,
      make_enum_variant_of(a, "Azure", CLUX_NODE_LITERAL_NUMERIC, "4"));
  node_t *decl_e2 = make_enum_decl_of(a, "E2", "i32", e2_variants);
  clux_value_t *e2_value = exec_expr_enum_decl(ctx, decl_e2);
  ASSERT_NE(e2_value, nullptr);
  EXPECT_FALSE(clux_value_is_error(e2_value));
  clux_type_t *e2 = type_described_by(e2_value);
  ASSERT_NE(e2, nullptr);
  ASSERT_NE(e2, e1) << "different variant names must not dedup";

  clux_value_t *ev = make_named_enum_bits_i32(ctx, e1, "ev_se", 7);
  ASSERT_NE(ev, nullptr);

  node_t *node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "ev_se"),
                     make_node(CLUX_NODE_SYMBOL, "as"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "E2"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_value_get_type(result), e2);
  EXPECT_EQ(read_i32(result), 7);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ENUM);
  EXPECT_STREQ(clux_type_get_name(clux_value_get_type(result)), "E2");
  delete_node(&node);
  delete_node(&decl_e2);
}

/* 场景: exec e2e 枚举 as 更窄底层枚举——E1(i32) 值 300 as E2(i16)
   预期: 宽松通道放行（i32→i16 显式 narrowing 合法），产出 E2 枚举值，
         按 i16 布局落地（值 300 域内保留） */
TEST_F(ExecLiteralTest, EnumAsToEnumWithNarrowerUnderlying) {
  clux_type_t *e1 = describe_color_underlying(ctx, a, "E1", "i32");
  ASSERT_NE(e1, nullptr);
  node_t *e2_variants = create_enum_variants_node(a);
  enum_variants_node_add_variant(
      e2_variants,
      make_enum_variant_of(a, "Crimson", CLUX_NODE_LITERAL_NUMERIC, "1"));
  enum_variants_node_add_variant(
      e2_variants,
      make_enum_variant_of(a, "Emerald", CLUX_NODE_LITERAL_NUMERIC, "2"));
  node_t *decl_e2 = make_enum_decl_of(a, "E2", "i16", e2_variants);
  clux_value_t *e2_value = exec_expr_enum_decl(ctx, decl_e2);
  ASSERT_NE(e2_value, nullptr);
  EXPECT_FALSE(clux_value_is_error(e2_value));
  clux_type_t *e2 = type_described_by(e2_value);
  ASSERT_NE(e2, nullptr);

  clux_value_t *ev = make_named_enum_bits_i32(ctx, e1, "ev_nw", 300);
  ASSERT_NE(ev, nullptr);

  node_t *node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "ev_nw"),
                     make_node(CLUX_NODE_SYMBOL, "as"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "E2"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_value_get_type(result), e2);
  EXPECT_EQ(read_i16(result), 300);
  delete_node(&node);
  delete_node(&decl_e2);
}

/* 场景: exec e2e 数值化转换——ev(2) as f64 / ev(2) as bool
   预期: integer_cast 数值通道放行：f64 == 2.0、bool == true */
TEST_F(ExecLiteralTest, EnumAsToF64AndBoolConvertsNumerically) {
  clux_type_t *color = describe_color_underlying(ctx, a, "Color", "i32");
  ASSERT_NE(color, nullptr);
  clux_value_t *ev = make_named_enum_bits_i32(ctx, color, "ev_num", 2);
  ASSERT_NE(ev, nullptr);

  node_t *f64_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "ev_num"),
                     make_node(CLUX_NODE_SYMBOL, "as"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f64"));
  clux_value_t *as_f64 = exec_expr_binary(ctx, f64_node);
  ASSERT_NE(as_f64, nullptr);
  EXPECT_FALSE(clux_value_is_error(as_f64));
  EXPECT_EQ(clux_value_get_type(as_f64),
            clux_context_builtin_type(ctx, CLUX_TYPE_F64));
  EXPECT_DOUBLE_EQ(read_f64(as_f64), 2.0);
  delete_node(&f64_node);

  node_t *bool_node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "ev_num"),
                     make_node(CLUX_NODE_SYMBOL, "as"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "bool"));
  clux_value_t *as_bool = exec_expr_binary(ctx, bool_node);
  ASSERT_NE(as_bool, nullptr);
  EXPECT_EQ(clux_value_get_type(as_bool),
            clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
  EXPECT_TRUE(read_bool(as_bool));
  delete_node(&bool_node);
}

/* 场景: exec e2e 枚举 as str——integer_cast 拒绝，错误透传
   预期: "cannot cast 'i32' to 'str'"（底层类型名，underlying 决定规则） */
TEST_F(ExecLiteralTest, EnumAsToStrRejectedViaUnderlying) {
  clux_type_t *color = describe_color_underlying(ctx, a, "Color", "i32");
  ASSERT_NE(color, nullptr);
  clux_value_t *ev = make_named_enum_bits_i32(ctx, color, "ev_str", 2);
  ASSERT_NE(ev, nullptr);

  node_t *node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "ev_str"),
                     make_node(CLUX_NODE_SYMBOL, "as"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "str"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "cannot cast 'i32' to 'str'");
  delete_node(&node);
}

/* 场景: "as" 右操作数不是类型表达式——ev as 5（数值字面量）
   预期: 报 "expected a type expression, got 'i32'"（右值类型名） */
TEST_F(ExecLiteralTest, EnumAsRightOperandNotTypeExpressionReports) {
  clux_type_t *color = describe_color_underlying(ctx, a, "Color", "i32");
  ASSERT_NE(color, nullptr);
  clux_value_t *ev = make_named_enum_bits_i32(ctx, color, "ev_ty", 2);
  ASSERT_NE(ev, nullptr);

  node_t *node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "ev_ty"),
                     make_node(CLUX_NODE_SYMBOL, "as"),
                     make_node(CLUX_NODE_LITERAL_NUMERIC, "5"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "expected a type expression, got 'i32'");
  delete_node(&node);
}

/* 场景: "as" 左操作数 error 传播——未声明标识符 as i64
   预期: lookup 失败的 error 原样返回："use of undeclared identifier ..." */
TEST_F(ExecLiteralTest, EnumAsLeftOperandErrorPropagates) {
  node_t *node =
      make_binary_of(a,
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "missing_ev"),
                     make_node(CLUX_NODE_SYMBOL, "as"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i64"));
  clux_value_t *result = exec_expr_binary(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'missing_ev'");
  delete_node(&node);
}

/* ---- 全类型 seal 完整性机制（exec e2e） ---- */

/* 场景: var vv: void = undefined——void 类型标注经 identifier lookup 命中
   global scope 的 void type value，TDZ 通道 create_tdz_value 过 seal 门禁
   预期: 返回 error 值，消息精确为
         "cannot create a value of incomplete type 'void'"；
         对照 var ok: i32 = undefined 正常产出 TDZ 变量 */
TEST_F(ExecLiteralTest, VarVoidAnnotationIncompleteRejected) {
  node_t *bad = make_stmt_var("vv",
                              CLUX_NODE_LITERAL_IDENTIFIER,
                              "void",
                              CLUX_NODE_LITERAL_UNDEFINED,
                              kUndefinedText);
  clux_value_t *result = exec_stmt_var(ctx, bad);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "cannot create a value of incomplete type 'void'");
  delete_node(&bad);

  /* i32 对照：TDZ 通道不受门禁影响 */
  node_t *good = make_stmt_var("ok",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               "i32",
                               CLUX_NODE_LITERAL_UNDEFINED,
                               kUndefinedText);
  clux_value_t *tdz = exec_stmt_var(ctx, good);
  ASSERT_NE(tdz, nullptr);
  EXPECT_EQ(clux_value_get_type(tdz),
            clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  EXPECT_TRUE(clux_value_is_tdz(tdz));
  delete_node(&good);
}

/* ---- *void 类型擦除边界（exec e2e） ---- */

/* 场景: var pv: *void = undefined——类型标注是 * void 前缀类型节点
   预期: 标注通道消费前缀节点产出 *void（seal 完整），创建 TDZ 变量；
         对照裸 void 标注被完整性门禁拒绝（见上方用例） */
TEST_F(ExecLiteralTest, VarVoidPtrAnnotationCreatesVoidPtrTdz) {
  node_t *type_node =
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "*"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "void"));
  node_t *var_node = create_stmt_var_node(a);
  stmt_var_node_set_keyword(var_node, make_node(CLUX_NODE_SYMBOL, "var"));
  stmt_var_node_set_name(var_node,
                         make_node(CLUX_NODE_LITERAL_IDENTIFIER, "pvz"));
  stmt_var_node_set_type(var_node, type_node);
  stmt_var_node_set_value(
      var_node, make_node(CLUX_NODE_LITERAL_UNDEFINED, kUndefinedText));
  clux_value_t *result = exec_stmt_var(ctx, var_node);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result))
      << "*void is sealed (ptr layout is immediate), TDZ creation must pass";
  EXPECT_EQ(clux_value_get_type(result),
            clux_context_get_ptr_type(
                ctx, clux_context_builtin_type(ctx, CLUX_TYPE_VOID)));
  EXPECT_STREQ(clux_type_get_name(clux_value_get_type(result)), "*void");
  EXPECT_TRUE(clux_value_is_tdz(result));
  EXPECT_EQ(clux_context_lookup(ctx, "pvz", NULL), result);
  delete_node(&var_node);
}

/* 场景: exec e2e *void 显式恢复——vpv as *i32 转出具体指针；
   对照 vpv as i32 落入指针族拒绝
   预期: as *i32 产出 *i32 且解引用读回原值；as i32 报
         "cannot cast '*void' to 'i32'"（ptr_cast 指针↔非指针拒绝） */
TEST_F(ExecLiteralTest, VoidPtrExplicitAsRecoversTypedPointer) {
  clux_type_t *void_ptr = clux_context_get_ptr_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_VOID));
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  clux_value_t *x = clux_context_create_i32(ctx, 21, "wpx", NULL);
  ASSERT_NE(x, nullptr);
  clux_value_t *vpv = clux_context_create_ptr(
      ctx, void_ptr, clux_value_get_data(x), "vpv", NULL);
  ASSERT_NE(vpv, nullptr);

  /* 显式 as 恢复: 右操作数是 *i32 前缀类型节点 */
  node_t *recover = make_binary_of(
      a,
      make_node(CLUX_NODE_LITERAL_IDENTIFIER, "vpv"),
      make_node(CLUX_NODE_SYMBOL, "as"),
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "*"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *typed = exec_expr_binary(ctx, recover);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(clux_value_get_type(typed), ptr_i32);
  clux_value_t *view = clux_context_deref(ctx, typed, NULL);
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(read_i32(view), 21) << "recover must keep the original handle";
  delete_node(&recover);

  /* as 非指针类型拒绝: 指针↔非指针无转换通道 */
  node_t *bad = make_binary_of(a,
                               make_node(CLUX_NODE_LITERAL_IDENTIFIER, "vpv"),
                               make_node(CLUX_NODE_SYMBOL, "as"),
                               make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *bad_result = exec_expr_binary(ctx, bad);
  ASSERT_NE(bad_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(bad_result), "cannot cast '*void' to 'i32'");
  delete_node(&bad);
}

/* 场景: exec e2e assign 通道——*i32 值隐式吸收进 *void 变量放行；
   反向 *void 值隐式写回 *i32 变量被拒
   预期: 正向 result 是目标本身且类型变为 *void；反向报
         "cannot safely cast '*void' to '*i32'"（隐式转出必须显式 as） */
TEST_F(ExecLiteralTest, VoidPtrAssignAbsorbsInAndRejectsImplicitOut) {
  clux_type_t *void_ptr = clux_context_get_ptr_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_VOID));
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32));
  clux_value_t *x = clux_context_create_i32(ctx, 7, "vqx", NULL);
  ASSERT_NE(x, nullptr);
  clux_value_t *vpi = clux_context_create_ptr(
      ctx, ptr_i32, clux_value_get_data(x), "vpi", NULL);
  ASSERT_NE(vpi, nullptr);
  clux_value_t *vpw = clux_context_create_ptr(ctx, void_ptr, 0, "vpw", NULL);
  ASSERT_NE(vpw, nullptr);
  clux_value_t *vpd = clux_context_create_ptr(ctx, void_ptr, 0, "vpd", NULL);
  ASSERT_NE(vpd, nullptr);

  /* 正向: *i32 → *void 变量隐式吸收（safe_cast ptr_base_is_void 分支） */
  node_t *absorb = make_assign(CLUX_NODE_LITERAL_IDENTIFIER, "=", "vpw", "vpi");
  clux_value_t *absorbed = exec_expr_assign(ctx, absorb);
  ASSERT_NE(absorbed, nullptr);
  EXPECT_EQ(absorbed, vpw) << "absorb must return the target identity";
  EXPECT_EQ(clux_value_get_type(vpw), void_ptr);
  delete_node(&absorb);

  /* 反向: *void → *i32 变量隐式写回拒绝 */
  node_t *leak =
      make_assign(CLUX_NODE_LITERAL_IDENTIFIER, "=", "vqi_slot", "vpd");
  clux_value_t *vqi_slot =
      clux_context_create_ptr(ctx, ptr_i32, 0, "vqi_slot", NULL);
  ASSERT_NE(vqi_slot, nullptr);
  clux_value_t *rejected = exec_expr_assign(ctx, leak);
  ASSERT_NE(rejected, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(rejected)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(rejected),
               "cannot safely cast '*void' to '*i32'");
  delete_node(&leak);
}

/* ---- exec_stmt_type：type 别名语句（白皮书 §3.11、§8.1）---- */

/* 合成 type 别名语句结点：location 覆盖 text 区间（缺子节点的错误定位
   用），name_text/value 为 NULL 时不设对应子节点（缺子节点分支用），
   value 由调用方构造（内建类型用 identifier 节点、复合类型用前缀
   binary 节点），delete 根结点时递归释放全部子节点。 */
static node_t *make_stmt_type(allocator_t *a,
                              const char *text,
                              const char *name_text,
                              node_t *value) {
  node_t *node = create_stmt_type_node(a);
  location_t loc;
  loc.filename = "test.clux";
  loc.begin.offset = text;
  loc.begin.column = 1;
  loc.begin.line = 1;
  loc.end.offset = text + strlen(text);
  loc.end.column = 1;
  loc.end.line = 1;
  node_set_location(node, &loc);
  if (name_text) {
    node_t *name = create_node(a, CLUX_NODE_LITERAL_IDENTIFIER);
    location_t name_loc;
    name_loc.filename = "test.clux";
    name_loc.begin.offset = name_text;
    name_loc.begin.column = 1;
    name_loc.begin.line = 1;
    name_loc.end.offset = name_text + strlen(name_text);
    name_loc.end.column = 1;
    name_loc.end.line = 1;
    node_set_location(name, &name_loc);
    stmt_type_node_set_name(node, name);
  }
  if (value) {
    stmt_type_node_set_value(node, value);
  }
  return node;
}

/* 场景: type Alias = i32（右值是内建类型名）
   预期: 返回 type value（unwrap 为 TYPE），被描述类型与引擎内建 i32
         interning 恒等，declare_value 具名化后 lookup 可见同一值 */
TEST_F(ExecLiteralTest, TypeAliasBindsTypeValueByName) {
  node_t *node = make_stmt_type(a,
                                "type Alias = i32",
                                "Alias",
                                make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *result = exec_stmt_type(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE)
      << "type statement must produce a type value";
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(described, clux_context_builtin_type(ctx, CLUX_TYPE_I32))
      << "alias must describe the engine builtin i32";
  EXPECT_EQ(clux_context_lookup(ctx, "Alias", NULL), result)
      << "declare_value must register the alias in scope";
  delete_node(&node);
}

/* 场景: type IntList = []i32（右值是前缀复合类型表达式）
   预期: 被描述类型是引擎通道 intern 出的 []i32（slice kind + 指针恒等） */
TEST_F(ExecLiteralTest, TypeAliasCompositeTypeExpression) {
  node_t *node = make_stmt_type(
      a,
      "type IntList = []i32",
      "IntList",
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "[]"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *result = exec_stmt_type(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_SLICE);
  EXPECT_EQ(described,
            clux_context_get_slice_type(
                ctx, clux_context_builtin_type(ctx, CLUX_TYPE_I32)))
      << "composite alias must be interned with the engine channel";
  EXPECT_EQ(clux_context_lookup(ctx, "IntList", NULL), result);
  delete_node(&node);
}

/* 场景: 右值不是类型表达式（type Bad = 42，数值字面量当别名目标）
   预期: 报 "expected type expression in type alias"，别名不落地 */
TEST_F(ExecLiteralTest, TypeAliasValueMustBeTypeExpression) {
  node_t *node = make_stmt_type(
      a, "type Bad = 42", "Bad", make_node(CLUX_NODE_LITERAL_NUMERIC, "42"));
  clux_value_t *result = exec_stmt_type(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "expected type expression in type alias");
  clux_value_t *missing = clux_context_lookup(ctx, "Bad", NULL);
  ASSERT_NE(missing, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(missing)), CLUX_TYPE_ERROR)
      << "failed alias must not be declared in scope";
  EXPECT_STREQ(error_message_of(missing), "use of undeclared identifier 'Bad'");
  delete_node(&node);
}

/* 场景: 缺 name 子节点（只设 value 不设 name）
   预期: 报 "type statement requires a name"（右值不求值） */
TEST_F(ExecLiteralTest, TypeAliasRequiresName) {
  node_t *node = make_stmt_type(
      a, "type  = i32", NULL, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  clux_value_t *result = exec_stmt_type(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "type statement requires a name");
  delete_node(&node);
}

/* 场景: 缺 value 子节点（只设 name 不设 value）
   预期: 报 "type statement requires a type expression" */
TEST_F(ExecLiteralTest, TypeAliasRequiresValueExpression) {
  node_t *node = make_stmt_type(a, "type Missing", "Missing", NULL);
  clux_value_t *result = exec_stmt_type(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "type statement requires a type expression");
  delete_node(&node);
}

/* 场景: 右值求值出错（type Ev = <超长数字>）
   预期: 右值 error 经 exec_operand 统一通道原样传播——"numeric literal
         too long" */
TEST_F(ExecLiteralTest, TypeAliasPropagatesValueError) {
  node_t *node =
      make_stmt_type(a,
                     "type Ev = <toolong>",
                     "Ev",
                     make_node(CLUX_NODE_LITERAL_NUMERIC, kTooLongNumeric));
  clux_value_t *result = exec_stmt_type(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "numeric literal too long");
  delete_node(&node);
}

/* 场景: TDZ 右值（var tx: i32 = undefined 后 type Td = tx）
   预期: exec_operand 的 consume_value 内置校验拒绝——"use of a value
         before initialization" */
TEST_F(ExecLiteralTest, TypeAliasTdzValueErrors) {
  node_t *decl = make_stmt_var("tx",
                               CLUX_NODE_LITERAL_IDENTIFIER,
                               "i32",
                               CLUX_NODE_LITERAL_UNDEFINED,
                               kUndefinedText);
  clux_value_t *declared = exec_stmt_var(ctx, decl);
  ASSERT_NE(declared, nullptr);
  ASSERT_TRUE(clux_value_is_tdz(declared));
  delete_node(&decl);

  node_t *node = make_stmt_type(
      a, "type Td = tx", "Td", make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tx"));
  clux_value_t *result = exec_stmt_type(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of a value before initialization");
  delete_node(&node);
}

/* 场景: shadow 右值（无载荷 type 值经标识符注入——真值路径无法合成
   shadow 字面量，改用 create_value(data=0) 具名后引用，与既有
   VarAnnotationShadowTypeValueErrors 同模式）
   预期: exec_operand 拒绝 shadow——"shadow value is not allowed in
         execution" */
TEST_F(ExecLiteralTest, TypeAliasShadowValueErrors) {
  clux_value_t *empty_type = clux_context_create_value(
      ctx, clux_context_get_type_type(ctx), 0, "tsh", NULL);
  ASSERT_NE(empty_type, nullptr);
  node_t *node = make_stmt_type(
      a, "type Sh = tsh", "Sh", make_node(CLUX_NODE_LITERAL_IDENTIFIER, "tsh"));
  clux_value_t *result = exec_stmt_type(ctx, node);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "shadow value is not allowed in execution");
  delete_node(&node);
}

/* 场景: 右值求值产出无值（空块节点是 exec_operand 统一通道唯一的
   NULL 上抛来源——"空块产出 NULL，唯一合法消费点是调用帧边界"）
   预期: !value 短路分支命中，NULL 原样上抛（别名不落地、不误报类型错误） */
TEST_F(ExecLiteralTest, TypeAliasPropagatesNullValue) {
  node_t *node =
      make_stmt_type(a, "type Nl = { }", "Nl", create_stmt_block_node(a));
  clux_value_t *result = exec_stmt_type(ctx, node);
  EXPECT_EQ(result, nullptr) << "valueless operand must propagate NULL as-is";
  clux_value_t *missing = clux_context_lookup(ctx, "Nl", NULL);
  ASSERT_NE(missing, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(missing)), CLUX_TYPE_ERROR)
      << "valueless alias must not be declared in scope";
  delete_node(&node);
}

/* ===== exec_stmt_block 块内提升（hoist pass 正式用例） ===== */

/* 场景: 块级前向调用——调用语句先于 func 声明（体内无自由名字）
   预期: pass 1b 预执行函数定义，f(3) 返回 3 */
TEST_F(ExecLiteralTest, BlockHoistForwardCallExecutesHoistedFunction) {
  node_t *block = create_stmt_block_node(a);
  node_t *call_arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "3");
  node_t *f_call_node = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"), 1, &call_arg);
  node_t *call_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(call_stmt, f_call_node);
  stmt_block_node_add_stmt(block, call_stmt);
  stmt_block_node_add_stmt(
      block, make_stmt_func_of(a, make_i32_identity_decl(a, "f")));

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  ASSERT_FALSE(clux_value_is_error(result)) << error_message_of(result);
  EXPECT_EQ(read_i32(result), 3);
  delete_node(&block);
}

/* 场景: 源码序捕获回归——g 先声明，f 以 |g| 捕获后调用（非前向）
   预期: f(3) 经捕获的 g 返回 3 */
TEST_F(ExecLiteralTest, BlockHoistCaptureBindsInSourceOrder) {
  node_t *f_params = create_func_params_node(a);
  func_params_node_add_param(
      f_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *g_arg = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x");
  node_t *g_call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "g"), 1, &g_arg);
  node_t *f_ret = create_stmt_return_node(a);
  stmt_return_node_set_value(f_ret, g_call);
  node_t *f_body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(f_body, f_ret);
  node_t *captures = create_func_captures_node(a);
  node_t *cap_params = create_func_params_node(a);
  func_params_node_add_param(
      cap_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "g"),
                          make_i32_to_i32_type(a)));
  func_captures_node_add_capture(captures, cap_params);
  node_t *f_decl =
      make_func_decl_of(a,
                        captures,
                        NULL,
                        f_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        f_body);

  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(
      block, make_stmt_func_of(a, make_i32_identity_decl(a, "g")));
  stmt_block_node_add_stmt(block, make_stmt_func_of(a, f_decl));
  node_t *call_arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "3");
  node_t *f_call_node = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"), 1, &call_arg);
  node_t *call_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(call_stmt, f_call_node);
  stmt_block_node_add_stmt(block, call_stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  ASSERT_FALSE(clux_value_is_error(result)) << error_message_of(result);
  EXPECT_EQ(read_i32(result), 3);
  delete_node(&block);
}

/* 场景: 前向捕获——f 以 |g| 捕获但 g 后声明（两阶段捕获：phase 1 全部
        仅定义，phase 2 在全部名字就位后统一做定义点快照）
   预期: 前向捕获成立，f(3) 经后声明的 g 返回 3 */
TEST_F(ExecLiteralTest, BlockHoistForwardCaptureBindsAfterAllDefines) {
  node_t *f_params = create_func_params_node(a);
  func_params_node_add_param(
      f_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *g_arg = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x");
  node_t *g_call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "g"), 1, &g_arg);
  node_t *f_ret = create_stmt_return_node(a);
  stmt_return_node_set_value(f_ret, g_call);
  node_t *f_body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(f_body, f_ret);
  node_t *captures = create_func_captures_node(a);
  node_t *cap_params = create_func_params_node(a);
  func_params_node_add_param(
      cap_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "g"),
                          make_i32_to_i32_type(a)));
  func_captures_node_add_capture(captures, cap_params);
  node_t *f_decl =
      make_func_decl_of(a,
                        captures,
                        NULL,
                        f_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        f_body);

  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(block, make_stmt_func_of(a, f_decl));
  stmt_block_node_add_stmt(
      block, make_stmt_func_of(a, make_i32_identity_decl(a, "g")));
  node_t *call_arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "3");
  node_t *f_call_node = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"), 1, &call_arg);
  node_t *call_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(call_stmt, f_call_node);
  stmt_block_node_add_stmt(block, call_stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  ASSERT_FALSE(clux_value_is_error(result)) << error_message_of(result);
  EXPECT_EQ(read_i32(result), 3);
  delete_node(&block);
}

/* 场景: 双向相互递归被类型判等拒绝（语义决策 2026-11-20）——func 类型
        判等含捕获表（func.c signature_equal），f 的捕获槽要装 g 就得
        闭式写出 g 的类型，而 g 的类型又含捕获 f 的类型，无限回归无法
        书写。两阶段化只解决名字就位，类型层不放过。
   预期: g 的带捕获类型与 f 捕获槽的无捕获类型不相等，assign 的
         safe_cast 拦截报 "type 'g' cannot be safely cast to 'func'" */
TEST_F(ExecLiteralTest, BlockHoistMutualRecursionRejectedByTypeEquality) {
  /* f: func |g: func(i32)->i32| f(x: i32): i32 { return g(x); } */
  node_t *f_params = create_func_params_node(a);
  func_params_node_add_param(
      f_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *g_arg = make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x");
  node_t *g_call =
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "g"), 1, &g_arg);
  node_t *f_ret = create_stmt_return_node(a);
  stmt_return_node_set_value(f_ret, g_call);
  node_t *f_body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(f_body, f_ret);
  node_t *f_captures = create_func_captures_node(a);
  node_t *f_cap_params = create_func_params_node(a);
  func_params_node_add_param(
      f_cap_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "g"),
                          make_i32_to_i32_type(a)));
  func_captures_node_add_capture(f_captures, f_cap_params);
  node_t *f_decl =
      make_func_decl_of(a,
                        f_captures,
                        NULL,
                        f_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        f_body);

  /* g: func |f: func(i32)->i32| g(x: i32): i32
              { return x > 0 ? f(x - 1) : x; } */
  node_t *g_params = create_func_params_node(a);
  func_params_node_add_param(
      g_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *dec_arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "1");
  node_t *f_call = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"), 1, &dec_arg);
  node_t *cond = make_binary_of(a,
                                make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                                make_node(CLUX_NODE_SYMBOL, ">"),
                                make_node(CLUX_NODE_LITERAL_NUMERIC, "0"));
  node_t *ternary = make_ternary_of(
      a, cond, f_call, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"));
  node_t *g_ret = create_stmt_return_node(a);
  stmt_return_node_set_value(g_ret, ternary);
  node_t *g_body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(g_body, g_ret);
  node_t *g_captures = create_func_captures_node(a);
  node_t *g_cap_params = create_func_params_node(a);
  func_params_node_add_param(
      g_cap_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"),
                          make_i32_to_i32_type(a)));
  func_captures_node_add_capture(g_captures, g_cap_params);
  node_t *g_decl =
      make_func_decl_of(a,
                        g_captures,
                        NULL,
                        g_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "g"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        g_body);

  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(block, make_stmt_func_of(a, f_decl));
  stmt_block_node_add_stmt(block, make_stmt_func_of(a, g_decl));
  node_t *call_arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "3");
  node_t *f_call_node = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"), 1, &call_arg);
  node_t *call_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(call_stmt, f_call_node);
  stmt_block_node_add_stmt(block, call_stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "type 'g' cannot be safely cast to 'func'");
  delete_node(&block);
}

/* 场景: 提升期函数定义失败上抛——func 声明缺函数体
   预期: pass 1b 快速失败，块出口转换 error，报
         "function definition requires a body" */
TEST_F(ExecLiteralTest, BlockHoistFuncDefineErrorPropagates) {
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl =
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        NULL);
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(block, make_stmt_func_of(a, decl));

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "function definition requires a body");
  delete_node(&block);
}

/* 场景: 装饰 func 不提升——调用先于装饰 func f 声明
   预期: 保持源码序，前向调用报 "use of undeclared identifier 'f'" */
TEST_F(ExecLiteralTest, BlockHoistDecoratedFuncStaysInSourceOrder) {
  node_t *block = create_stmt_block_node(a);
  node_t *call_arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "3");
  node_t *f_call_node = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"), 1, &call_arg);
  node_t *call_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(call_stmt, f_call_node);
  stmt_block_node_add_stmt(block, call_stmt);
  node_t *stmt = make_stmt_func_of(a, make_i32_identity_decl(a, "f"));
  stmt_func_node_add_decorator(stmt, make_decorator_of(a, nullptr));
  stmt_block_node_add_stmt(block, stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "use of undeclared identifier 'f'");
  delete_node(&block);
}

/* 场景: 捕获 TDZ 源拒绝——func f |x: i32| 先声明，var x = 5 后声明；
        phase 2 在 var TDZ 预注册后做捕获快照，按 TDZ 拒绝
   预期: "use of a value before initialization"（定义点快照语义，
         捕获 var 必须在捕获方定义点之前完成定义） */
TEST_F(ExecLiteralTest, BlockHoistCaptureOfTdzVarRejected) {
  node_t *f_params = create_func_params_node(a);
  func_params_node_add_param(
      f_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *f_ret = create_stmt_return_node(a);
  stmt_return_node_set_value(f_ret,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"));
  node_t *f_body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(f_body, f_ret);
  node_t *captures = create_func_captures_node(a);
  node_t *cap_params = create_func_params_node(a);
  func_params_node_add_param(
      cap_params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_captures_node_add_capture(captures, cap_params);
  node_t *f_decl =
      make_func_decl_of(a,
                        captures,
                        NULL,
                        f_params,
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"),
                        make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                        f_body);

  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(block, make_stmt_func_of(a, f_decl));
  stmt_block_node_add_stmt(
      block,
      make_stmt_var("x", (node_kind_t)0, NULL, CLUX_NODE_LITERAL_NUMERIC, "5"));
  node_t *call_arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "1");
  node_t *f_call_node = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"), 1, &call_arg);
  node_t *call_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(call_stmt, f_call_node);
  stmt_block_node_add_stmt(block, call_stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of a value before initialization");
  delete_node(&block);
}

/* 场景: TDZ 前向读拦截——提升预注册 var y 后，源码序定义之前的读取按
        TDZ 语义报错（而非 undeclared）
   预期: "use of a value before initialization" */
TEST_F(ExecLiteralTest, BlockHoistVarForwardReadReportsTdz) {
  node_t *block = create_stmt_block_node(a);
  node_t *read_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(read_stmt,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "y"));
  stmt_block_node_add_stmt(block, read_stmt);
  stmt_block_node_add_stmt(
      block,
      make_stmt_var("y", (node_kind_t)0, NULL, CLUX_NODE_LITERAL_NUMERIC, "7"));

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of a value before initialization");
  delete_node(&block);
}

/* 场景: 提升预注册 var 的定义处首赋值——var y = 7 后块级读取（本块无
        func 声明，顺带覆盖 hoist 的 func_count 为 0 路径）
   预期: TDZ 槽被首赋值清标记，读出 7 */
TEST_F(ExecLiteralTest, BlockHoistVarDefinitionAssignsTdzSlot) {
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(
      block,
      make_stmt_var("y", (node_kind_t)0, NULL, CLUX_NODE_LITERAL_NUMERIC, "7"));
  node_t *read_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(read_stmt,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "y"));
  stmt_block_node_add_stmt(block, read_stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  ASSERT_FALSE(clux_value_is_error(result)) << error_message_of(result);
  EXPECT_EQ(read_i32(result), 7);
  delete_node(&block);
}

/* 场景: 有标注 var 提升——var y: i64 = 7，TDZ 预注册按标注类型
   预期: 定义处首赋值经 safe_cast 拓宽落地，读回类型 i64、值 7
         （无标注 shadow 推断会得 i32，断言 i64 证明走的是标注路径） */
TEST_F(ExecLiteralTest, BlockHoistAnnotatedVarHoistsWithAnnotationType) {
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(block,
                           make_stmt_var("y",
                                         CLUX_NODE_LITERAL_IDENTIFIER,
                                         "i64",
                                         CLUX_NODE_LITERAL_NUMERIC,
                                         "7"));
  node_t *read_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(read_stmt,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "y"));
  stmt_block_node_add_stmt(block, read_stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  ASSERT_FALSE(clux_value_is_error(result)) << error_message_of(result);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_I64)
      << "the hoisted TDZ must be pre-registered with the annotated i64";
  EXPECT_EQ(read_i64(result), 7);
  delete_node(&block);
}

/* 场景: var u = undefined（无标注）——提升 pass 不抢，交回源码序声明
   预期: 源码序路径报 "undefined initializer requires a type annotation" */
TEST_F(ExecLiteralTest,
       BlockHoistUndefinedInitializerWithoutAnnotationDeferred) {
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(block,
                           make_stmt_var("u",
                                         (node_kind_t)0,
                                         NULL,
                                         CLUX_NODE_LITERAL_UNDEFINED,
                                         kUndefinedText));

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "undefined initializer requires a type annotation");
  delete_node(&block);
}

/* 场景: var z: i32 = undefined——提升按标注注册 TDZ，定义处的目标状态
        就是 TDZ（不转首赋值），后续读取按 TDZ 拒绝
   预期: "use of a value before initialization" */
TEST_F(ExecLiteralTest, BlockHoistUndefinedInitializerKeepsTdzAtDefinition) {
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(block,
                           make_stmt_var("z",
                                         CLUX_NODE_LITERAL_IDENTIFIER,
                                         "i32",
                                         CLUX_NODE_LITERAL_UNDEFINED,
                                         kUndefinedText));
  node_t *read_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(read_stmt,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "z"));
  stmt_block_node_add_stmt(block, read_stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of a value before initialization");
  delete_node(&block);
}

/* 场景: 无标注 var 初值探测失败——var z = nosuch（未声明标识符）
   预期: shadow 模式探测的错误原样上抛，
         "use of undeclared identifier 'nosuch'" */
TEST_F(ExecLiteralTest, BlockHoistInitializerProbeErrorPropagates) {
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(
      block,
      make_stmt_var(
          "z", (node_kind_t)0, NULL, CLUX_NODE_LITERAL_IDENTIFIER, "nosuch"));

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'nosuch'");
  delete_node(&block);
}

/* 场景: 装饰 var 不提升——读取先于装饰 var w 声明
   预期: 保持源码序，前向读取报 "use of undeclared identifier 'w'" */
TEST_F(ExecLiteralTest, BlockHoistDecoratedVarStaysInSourceOrder) {
  node_t *block = create_stmt_block_node(a);
  node_t *read_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(read_stmt,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "w"));
  stmt_block_node_add_stmt(block, read_stmt);
  node_t *stmt =
      make_stmt_var("w", (node_kind_t)0, NULL, CLUX_NODE_LITERAL_NUMERIC, "7");
  stmt_var_node_add_decorator(stmt, make_decorator_of(a, nullptr));
  stmt_block_node_add_stmt(block, stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "use of undeclared identifier 'w'");
  delete_node(&block);
}

/* 场景: 块内重复 var 名——提升期快速失败
   预期: error，消息含 duplicate */
TEST_F(ExecLiteralTest, BlockHoistDuplicateVarFailsFast) {
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(
      block,
      make_stmt_var("x", (node_kind_t)0, NULL, CLUX_NODE_LITERAL_NUMERIC, "1"));
  stmt_block_node_add_stmt(
      block,
      make_stmt_var("x", (node_kind_t)0, NULL, CLUX_NODE_LITERAL_NUMERIC, "2"));
  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "duplicate declaration 'x'");
  delete_node(&block);
}

/* 场景: union 前向指针 base——union A { f: *B } 先于 union B 声明
   预期: *B 以空壳为 base 合法（ptr 构造即 seal），A/B 都成功 seal，
         末尾 type Check = A 产出 type value */
TEST_F(ExecLiteralTest, BlockHoistUnionForwardPtrBaseAllowed) {
  node_t *a_fields = create_fields_node(a);
  node_t *field_f = create_field_node(a);
  field_node_set_name(field_f, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"));
  field_node_set_type(
      field_f,
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "*"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "B")));
  fields_node_add_field(a_fields, field_f);

  node_t *block = create_stmt_block_node(a);
  node_t *stmt_a = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt_a, make_union_decl_of(a, "union", "A", NULL, a_fields));
  stmt_block_node_add_stmt(block, stmt_a);
  node_t *b_fields = create_fields_node(a);
  fields_node_add_field(b_fields, make_union_field_of(a, "a", "i32"));
  node_t *stmt_b = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt_b, make_union_decl_of(a, "union", "B", NULL, b_fields));
  stmt_block_node_add_stmt(block, stmt_b);
  stmt_block_node_add_stmt(
      block,
      make_stmt_type(a,
                     "type Check = A",
                     "Check",
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "A")));

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  ASSERT_FALSE(clux_value_is_error(result)) << error_message_of(result);
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE)
      << "type Check = A must succeed after both unions sealed";
  delete_node(&block);
}

/* 场景: union 值位置前向引用——union A { b: B }，B 后声明
   预期: 报 "union 'A' member 'b' has incomplete type 'B'" */
TEST_F(ExecLiteralTest, BlockHoistUnionValueMemberIncompleteRejected) {
  node_t *a_fields = create_fields_node(a);
  fields_node_add_field(a_fields, make_union_field_of(a, "b", "B"));
  node_t *b_fields = create_fields_node(a);
  fields_node_add_field(b_fields, make_union_field_of(a, "a", "i32"));

  node_t *block = create_stmt_block_node(a);
  node_t *stmt_a = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt_a, make_union_decl_of(a, "union", "A", NULL, a_fields));
  stmt_block_node_add_stmt(block, stmt_a);
  node_t *stmt_b = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt_b, make_union_decl_of(a, "union", "B", NULL, b_fields));
  stmt_block_node_add_stmt(block, stmt_b);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "union 'A' member 'b' has incomplete type 'B'");
  delete_node(&block);
}

/* 场景: 别名指向空壳——type X = B，union B 后声明
   预期: 报 "cannot alias incomplete type 'B'" */
TEST_F(ExecLiteralTest, BlockHoistAliasToIncompleteShellRejected) {
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(
      block,
      make_stmt_type(
          a, "type X = B", "X", make_node(CLUX_NODE_LITERAL_IDENTIFIER, "B")));
  node_t *stmt_b = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt_b,
      make_union_decl_of(a, "union", "B", NULL, make_two_member_fields(a)));
  stmt_block_node_add_stmt(block, stmt_b);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "cannot alias incomplete type 'B'");
  delete_node(&block);
}

/* 场景: cunion 空壳路径——cunion A { f: *B } 先于 cunion B 声明
        （pass 1a 按 KEYWORD 文本判别预建 C union 空壳）
   预期: *B 前向 base 合法，A/B 均 seal，末尾 type Check = A 成功 */
TEST_F(ExecLiteralTest, BlockHoistCunionForwardPtrBaseAllowed) {
  node_t *a_fields = create_fields_node(a);
  node_t *field_f = create_field_node(a);
  field_node_set_name(field_f, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"));
  field_node_set_type(
      field_f,
      make_binary_of(a,
                     NULL,
                     make_node(CLUX_NODE_SYMBOL, "*"),
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "B")));
  fields_node_add_field(a_fields, field_f);

  node_t *block = create_stmt_block_node(a);
  node_t *stmt_a = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt_a, make_union_decl_of(a, "cunion", "A", NULL, a_fields));
  stmt_block_node_add_stmt(block, stmt_a);
  node_t *b_fields = create_fields_node(a);
  fields_node_add_field(b_fields, make_union_field_of(a, "a", "i32"));
  node_t *stmt_b = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt_b, make_union_decl_of(a, "cunion", "B", NULL, b_fields));
  stmt_block_node_add_stmt(block, stmt_b);
  stmt_block_node_add_stmt(
      block,
      make_stmt_type(a,
                     "type Check = A",
                     "Check",
                     make_node(CLUX_NODE_LITERAL_IDENTIFIER, "A")));

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  ASSERT_FALSE(clux_value_is_error(result)) << error_message_of(result);
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE)
      << "type Check = A must succeed after both cunions sealed";
  delete_node(&block);
}

/* 场景: 块内重复 union 名——pass 1a 第二个同名空壳按名注册时撞 duplicate
   预期: "duplicate declaration 'A'" */
TEST_F(ExecLiteralTest, BlockHoistDuplicateUnionNameFailsFast) {
  node_t *block = create_stmt_block_node(a);
  node_t *first_fields = create_fields_node(a);
  fields_node_add_field(first_fields, make_union_field_of(a, "a", "i32"));
  node_t *stmt_first = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt_first, make_union_decl_of(a, "union", "A", NULL, first_fields));
  stmt_block_node_add_stmt(block, stmt_first);
  node_t *second_fields = create_fields_node(a);
  fields_node_add_field(second_fields, make_union_field_of(a, "b", "f64"));
  node_t *stmt_second = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt_second, make_union_decl_of(a, "union", "A", NULL, second_fields));
  stmt_block_node_add_stmt(block, stmt_second);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "duplicate declaration 'A'");
  delete_node(&block);
}

/* 场景: 装饰 union 不提升——type X = B 先于装饰 union B 声明
   预期: 保持源码序，前向别名查无 B 报 "use of undeclared identifier 'B'" */
TEST_F(ExecLiteralTest, BlockHoistDecoratedUnionStaysInSourceOrder) {
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(
      block,
      make_stmt_type(
          a, "type X = B", "X", make_node(CLUX_NODE_LITERAL_IDENTIFIER, "B")));
  node_t *stmt = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      stmt,
      make_union_decl_of(a, "union", "B", NULL, make_two_member_fields(a)));
  stmt_union_node_add_decorator(stmt, make_decorator_of(a, nullptr));
  stmt_block_node_add_stmt(block, stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "use of undeclared identifier 'B'");
  delete_node(&block);
}

/* 场景: enum 不提升——前向读取 enum 名先于 enum 语句
   预期: 保持源码序，"use of undeclared identifier 'E'" */
TEST_F(ExecLiteralTest, BlockHoistEnumStaysInSourceOrder) {
  node_t *block = create_stmt_block_node(a);
  node_t *read_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(read_stmt,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "E"));
  stmt_block_node_add_stmt(block, read_stmt);
  node_t *stmt = create_stmt_enum_node(a);
  stmt_enum_node_set_enum_decl(
      stmt, make_enum_decl_of(a, "E", "u8", make_color_variants(a)));
  stmt_block_node_add_stmt(block, stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "use of undeclared identifier 'E'");
  delete_node(&block);
}

/* 场景: 嵌套块独立提升——内层块前向调用自身声明的 g；外层块 var y = 7
        先经首赋值落地，内层块作为外层最后语句
   预期: 内层 g(3) 经内层提升执行返回 3（若未提升会报 undeclared 使块
         以 error 中断），外层 y 读取语句无错执行 */
TEST_F(ExecLiteralTest, BlockHoistNestedBlockHoistsIndependently) {
  node_t *inner = create_stmt_block_node(a);
  node_t *call_arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "3");
  node_t *g_call_node = make_call_of(
      a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "g"), 1, &call_arg);
  node_t *call_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(call_stmt, g_call_node);
  stmt_block_node_add_stmt(inner, call_stmt);
  stmt_block_node_add_stmt(
      inner, make_stmt_func_of(a, make_i32_identity_decl(a, "g")));

  node_t *outer = create_stmt_block_node(a);
  stmt_block_node_add_stmt(
      outer,
      make_stmt_var("y", (node_kind_t)0, NULL, CLUX_NODE_LITERAL_NUMERIC, "7"));
  node_t *read_stmt = create_stmt_expr_node(a);
  stmt_expr_node_set_expr(read_stmt,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "y"));
  stmt_block_node_add_stmt(outer, read_stmt);
  stmt_block_node_add_stmt(outer, inner);

  clux_value_t *result = exec_stmt_block(ctx, outer);
  ASSERT_NE(result, nullptr);
  ASSERT_FALSE(clux_value_is_error(result)) << error_message_of(result);
  EXPECT_EQ(read_i32(result), 3)
      << "the nested block value must come from the forward call to g";
  delete_node(&outer);
}

/* 场景: 提升期类型标注求值失败——var v: nosuch = 1（标注类型未声明）
   预期: pass 1c 有标注路径快速失败，"use of undeclared identifier 'nosuch'" */
TEST_F(ExecLiteralTest, BlockHoistTypeAnnotationErrorPropagates) {
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(block,
                           make_stmt_var("v",
                                         CLUX_NODE_LITERAL_IDENTIFIER,
                                         "nosuch",
                                         CLUX_NODE_LITERAL_NUMERIC,
                                         "1"));

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'nosuch'");
  delete_node(&block);
}

/* 场景: 无标注 var 的初值探测无值——var v = {}（空块初值，探产出 NULL）
   预期: "variable hoist cannot infer the initializer type" */
TEST_F(ExecLiteralTest, BlockHoistValuelessInitializerCannotInfer) {
  /* 初值是非 kind+text 形态的块表达式：手工构造 stmt，避免 make_stmt_var
     的占位初值被 set_value 顶替成孤儿 node（children 手工维护契约）。 */
  node_t *stmt = create_stmt_var_node(a);
  stmt_var_node_set_keyword(stmt, make_node(CLUX_NODE_SYMBOL, "var"));
  stmt_var_node_set_name(stmt, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "v"));
  stmt_var_node_set_value(stmt, create_stmt_block_node(a));
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(block, stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "variable hoist cannot infer the initializer type");
  delete_node(&block);
}

/* 场景: 自引用初始化——var x = x。pass 1c 的 shadow 探测发生在 TDZ 预注册
        之前，探测期读 x 时按未声明拒绝（探测即失败，不进入源码序）
   预期: hoist 失败上抛，"use of undeclared identifier 'x'" */
TEST_F(ExecLiteralTest, BlockHoistSelfReferentialInitializerRejectedAtProbe) {
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(
      block,
      make_stmt_var(
          "x", (node_kind_t)0, NULL, CLUX_NODE_LITERAL_IDENTIFIER, "x"));

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result), "use of undeclared identifier 'x'");
  delete_node(&block);
}

/* 场景: 残缺/匿名节点容错——无 FUNC_DECL 槽的 func 语句在提升 pass 1b
        被无害跳过；匿名 union 语句提升 pass 1a 跳过（不抢报错），
        语句层 union 强制具名的错误在源码序由 pass 2 报出
   预期: 块返回错误 "union statement requires a named union declaration" */
TEST_F(ExecLiteralTest, BlockHoistDegenerateNodesSkippedAndBlockProceeds) {
  node_t *block = create_stmt_block_node(a);
  stmt_block_node_add_stmt(block, create_stmt_func_node(a));
  node_t *anon_union = create_stmt_union_node(a);
  stmt_union_node_set_union_decl(
      anon_union,
      make_union_decl_of(a, "union", NULL, NULL, make_two_member_fields(a)));
  stmt_block_node_add_stmt(block, anon_union);
  stmt_block_node_add_stmt(
      block,
      make_stmt_var("y", (node_kind_t)0, NULL, CLUX_NODE_LITERAL_NUMERIC, "7"));

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "union statement requires a named union declaration");
  delete_node(&block);
}

/* 场景: 定义期执行错误中断块——var v = f(1)，f 的函数体引用未声明名字。
        pass 1c shadow 探测只取返回类型不执行函数体（probe 成功），
        pass 2 定义处真实 invoke 执行函数体报错
   预期: 首赋值求值失败上抛并中断块（exec_stmt_var_define error 返回 +
         exec_stmt_block break），"use of undeclared identifier 'nosuch'" */
TEST_F(ExecLiteralTest, BlockHoistDefineTimeExecutionErrorBreaksBlock) {
  node_t *block = create_stmt_block_node(a);
  /* func f(x: i32): i32 { return nosuch; } */
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(a,
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "x"),
                          make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(ret,
                             make_node(CLUX_NODE_LITERAL_IDENTIFIER, "nosuch"));
  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(body, ret);
  stmt_block_node_add_stmt(
      block,
      make_stmt_func_of(
          a,
          make_func_decl_of(a,
                            NULL,
                            NULL,
                            params,
                            make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"),
                            make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32"),
                            body)));
  /* var v = f(1)：初值是调用表达式，手工构造 stmt 避免 make_stmt_var 的
     占位初值被 set_value 顶替成孤儿 node（children 手工维护契约）。 */
  node_t *var_stmt = create_stmt_var_node(a);
  stmt_var_node_set_keyword(var_stmt, make_node(CLUX_NODE_SYMBOL, "var"));
  stmt_var_node_set_name(var_stmt,
                         make_node(CLUX_NODE_LITERAL_IDENTIFIER, "v"));
  node_t *arg = make_node(CLUX_NODE_LITERAL_NUMERIC, "1");
  stmt_var_node_set_value(
      var_stmt,
      make_call_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "f"), 1, &arg));
  stmt_block_node_add_stmt(block, var_stmt);

  clux_value_t *result = exec_stmt_block(ctx, block);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "use of undeclared identifier 'nosuch'");
  delete_node(&block);
}

/* ---- exec_expr_interface_decl / exec_stmt_interface：interface 类型
       （纯编译期结构约束，永不 seal——seal 门禁拒绝值位置）---- */

/* 合成 interface 声明结点：name_text 为 NULL 表示匿名，generics/body
   允许为 NULL 构造容错路径（成员表为 NULL 即空成员表），delete 根结点
   时递归释放全部子结点。 */
static node_t *make_interface_decl_of(allocator_t *allocator,
                                      const char *name_text,
                                      node_t *generics,
                                      node_t *body) {
  node_t *node = create_interface_decl_node(allocator);
  interface_decl_node_set_keyword(
      node, make_text_node(allocator, CLUX_NODE_SYMBOL, "interface"));
  if (name_text) {
    interface_decl_node_set_name(
        node,
        make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, name_text));
  }
  interface_decl_node_set_generics(node, generics);
  interface_decl_node_set_body(node, body);
  return node;
}

/* 合成单成员成员表 interface { func write(data: i32): void; }——成员是
   无函数体的 FUNC_DECL 签名。with_body/captures 供校验路径构造非法
   成员（带体/带捕获），member_name 覆盖成员名。 */
static node_t *make_interface_members_of(allocator_t *allocator,
                                         const char *member_name,
                                         bool with_body,
                                         node_t *captures) {
  node_t *params = create_func_params_node(allocator);
  func_params_node_add_param(
      params,
      make_named_param_of(
          allocator,
          make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "data"),
          make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *body = NULL;
  if (with_body) {
    body = create_stmt_block_node(allocator);
  }
  node_t *member = make_func_decl_of(
      allocator,
      captures,
      NULL,
      params,
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, member_name),
      make_text_node(allocator, CLUX_NODE_LITERAL_IDENTIFIER, "void"),
      body);
  node_t *members = create_interface_members_node(allocator);
  interface_members_node_add_member(members, member);
  return members;
}

/* ---- 引擎层：clux_context_create_interface_type / interface_type.c ---- */

/* 场景: 合法成员表经 context 创建 interface 类型
   预期: kind INTERFACE、布局无意义（size 0 / align 1，与 void 一致）、
         永不 seal；成员表按声明序可查：count、get_member 命中与越界
         NULL、find_member 命中/未命中/NULL 名容错、签名借用恒等；
         NULL 类型查询全部安全容错 */
TEST_F(ExecLiteralTest, InterfaceEngineCreatesZeroLayoutUnsealedType) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  clux_type_t *params[] = {i32_type};
  clux_type_t *sig = clux_context_create_func_type(
      ctx, "write", NULL, 0, params, 1, void_type);
  ASSERT_NE(sig, nullptr);
  clux_interface_member_t members[] = {{"write", sig}};
  clux_type_t *type =
      clux_context_create_interface_type(ctx, "IEng", members, 1);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(clux_type_get_kind(type), CLUX_TYPE_INTERFACE);
  EXPECT_STREQ(clux_type_get_name(type), "IEng");
  EXPECT_EQ(clux_type_get_size(type), 0u)
      << "interface has no runtime object (size 0 like void)";
  EXPECT_EQ(clux_type_get_align(type), 1u);
  EXPECT_FALSE(clux_type_is_seal(type)) << "interface never seals";
  EXPECT_EQ(clux_interface_member_count(type), 1u);
  const clux_interface_member_t *member = clux_interface_get_member(type, 0);
  ASSERT_NE(member, nullptr);
  EXPECT_STREQ(member->name, "write");
  EXPECT_EQ(member->func, sig)
      << "the member signature is borrowed as-is from the caller table";
  EXPECT_EQ(clux_interface_get_member(type, 1), nullptr)
      << "an out-of-range index must yield NULL";
  EXPECT_EQ(clux_interface_find_member(type, "write"), member);
  EXPECT_EQ(clux_interface_find_member(type, "flush"), nullptr);
  EXPECT_EQ(clux_interface_find_member(type, NULL), nullptr);
  EXPECT_EQ(clux_interface_member_count(NULL), 0u);
  EXPECT_EQ(clux_interface_get_member(NULL, 0), nullptr);
  EXPECT_EQ(clux_interface_find_member(NULL, "write"), nullptr);
}

/* 场景: context 入参校验——缺名 / 空成员表 / 成员缺名 / 成员缺签名 /
        签名非 FUNC / 签名带捕获 / 成员重名
   预期: 均返回 NULL 且不建型（引擎错误经内部 errors 通道记录，公开 API
         无取回接口——NULL 返回即全部可观测行为）；对照组的合法入参
         建型成功（校验不误伤） */
TEST_F(ExecLiteralTest, InterfaceEngineRejectsInvalidInputs) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  clux_type_t *params[] = {i32_type};
  clux_type_t *sig =
      clux_context_create_func_type(ctx, "sig", NULL, 0, params, 1, void_type);
  ASSERT_NE(sig, nullptr);
  clux_func_capture_t captures[] = {{"c", i32_type}};
  clux_type_t *closure_sig = clux_context_create_func_type(
      ctx, "csig", captures, 1, NULL, 0, void_type);
  ASSERT_NE(closure_sig, nullptr);
  clux_interface_member_t ok[] = {{"write", sig}};
  clux_interface_member_t nameless[] = {{NULL, sig}};
  clux_interface_member_t sigless[] = {{"write", NULL}};
  clux_interface_member_t nonfunc[] = {{"write", i32_type}};
  clux_interface_member_t captured[] = {{"write", closure_sig}};
  clux_interface_member_t duplicate[] = {{"write", sig}, {"write", sig}};
  EXPECT_EQ(clux_context_create_interface_type(ctx, NULL, ok, 1), nullptr)
      << "a missing name must be rejected";
  EXPECT_EQ(clux_context_create_interface_type(ctx, "I", NULL, 1), nullptr)
      << "a missing member table must be rejected";
  EXPECT_EQ(clux_context_create_interface_type(ctx, "I", ok, 0), nullptr)
      << "an empty member table must be rejected";
  EXPECT_EQ(clux_context_create_interface_type(ctx, "I", nameless, 1), nullptr)
      << "a member without a name must be rejected";
  EXPECT_EQ(clux_context_create_interface_type(ctx, "I", sigless, 1), nullptr)
      << "a member without a signature must be rejected";
  EXPECT_EQ(clux_context_create_interface_type(ctx, "I", nonfunc, 1), nullptr)
      << "a non-func signature must be rejected";
  EXPECT_EQ(clux_context_create_interface_type(ctx, "I", captured, 1), nullptr)
      << "a signature with captures must be rejected";
  EXPECT_EQ(clux_context_create_interface_type(ctx, "I", duplicate, 2), nullptr)
      << "duplicate member names must be rejected";
  clux_type_t *type = clux_context_create_interface_type(ctx, "IOk", ok, 1);
  ASSERT_NE(type, nullptr) << "the valid control input must still build";
  EXPECT_EQ(clux_type_get_kind(type), CLUX_TYPE_INTERFACE);
}

/* 场景: 结构判等与去重——members_equal / clux_type_equal / context
        find-or-dedup 三通道
   预期: 结构一致（成员名 + 签名按声明序）即判等：context 路径恒等返回
         既有实例（类型名不参与）；成员名不同 / 签名不同 / 成员数不同 /
         声明序不同均判不等；非 interface 类型 members_equal 恒 false；
         引擎直连构造的两个同构实例 clux_type_equal 为 true 且指针不同 */
TEST_F(ExecLiteralTest, InterfaceEngineStructuralEqualityAndDedup) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *str_type = clux_context_builtin_type(ctx, CLUX_TYPE_STR);
  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  clux_type_t *one_i32[] = {i32_type};
  clux_type_t *one_str[] = {str_type};
  clux_type_t *sig_write = clux_context_create_func_type(
      ctx, "write", NULL, 0, one_i32, 1, void_type);
  ASSERT_NE(sig_write, nullptr);
  clux_type_t *sig_write_str = clux_context_create_func_type(
      ctx, "write_str", NULL, 0, one_str, 1, void_type);
  ASSERT_NE(sig_write_str, nullptr);
  clux_type_t *sig_flush =
      clux_context_create_func_type(ctx, "flush", NULL, 0, NULL, 0, void_type);
  ASSERT_NE(sig_flush, nullptr);
  clux_interface_member_t m_write[] = {{"write", sig_write}};
  clux_interface_member_t m_write_str[] = {{"write", sig_write_str}};
  clux_interface_member_t m_flush[] = {{"flush", sig_flush}};
  clux_interface_member_t m_two[] = {{"write", sig_write},
                                     {"flush", sig_flush}};
  clux_interface_member_t m_swapped[] = {{"flush", sig_flush},
                                         {"write", sig_write}};
  clux_type_t *type =
      clux_context_create_interface_type(ctx, "ID1", m_write, 1);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(clux_context_create_interface_type(ctx, "ID1", m_write, 1), type)
      << "identical input must dedupe by returning the existing instance";
  EXPECT_EQ(clux_context_create_interface_type(ctx, "ID2", m_write, 1), type)
      << "the type name does not participate in structural equality";
  EXPECT_NE(clux_context_create_interface_type(ctx, "ID3", m_write_str, 1),
            type)
      << "a different signature with the same member name must not dedupe";
  EXPECT_NE(clux_context_create_interface_type(ctx, "ID4", m_flush, 1), type)
      << "a different member name must not dedupe";
  EXPECT_NE(clux_context_create_interface_type(ctx, "ID5", m_two, 2), type)
      << "a different member count must not dedupe";
  EXPECT_NE(clux_context_create_interface_type(ctx, "ID6", m_swapped, 2), type)
      << "a different declaration order must not dedupe";
  EXPECT_TRUE(clux_interface_members_equal(type, m_write, 1));
  EXPECT_FALSE(clux_interface_members_equal(type, m_write_str, 1));
  EXPECT_FALSE(clux_interface_members_equal(type, m_two, 2));
  EXPECT_FALSE(clux_interface_members_equal(i32_type, m_write, 1))
      << "a non-interface type is never structurally equal";
  clux_type_t *raw_a = create_clux_interface_type(a, "RawA", m_write, 1);
  clux_type_t *raw_b = create_clux_interface_type(a, "RawB", m_write, 1);
  ASSERT_NE(raw_a, nullptr);
  ASSERT_NE(raw_b, nullptr);
  EXPECT_NE(raw_a, raw_b)
      << "the engine constructor always builds a fresh instance";
  EXPECT_TRUE(clux_type_equal(raw_a, raw_b))
      << "clux_type_equal on interfaces compares member structure only";
  EXPECT_TRUE(clux_type_equal(raw_a, raw_a));
  EXPECT_FALSE(clux_type_equal(raw_a, i32_type))
      << "an interface is never structurally equal to a non-interface type";
  delete_clux_type(&raw_a);
  delete_clux_type(&raw_b);
}

/* 场景: 成员名驻留——context 创建路径经 clux_context_alloc_str 拷贝登记
   预期: meta 持有的成员名是 context 侧拷贝（与调用方表项指针不同、
         内容相同）；重复查询指针稳定；结构去重命中时回读的成员表
         指针恒等 */
TEST_F(ExecLiteralTest, InterfaceEngineInternsMemberNames) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  clux_type_t *params[] = {i32_type};
  clux_type_t *sig = clux_context_create_func_type(
      ctx, "write", NULL, 0, params, 1, void_type);
  ASSERT_NE(sig, nullptr);
  clux_interface_member_t members[] = {{"write", sig}};
  clux_type_t *type =
      clux_context_create_interface_type(ctx, "IStr", members, 1);
  ASSERT_NE(type, nullptr);
  const clux_interface_member_t *member = clux_interface_get_member(type, 0);
  ASSERT_NE(member, nullptr);
  EXPECT_NE(member->name, members[0].name)
      << "the member name must be an interned copy, not the caller pointer";
  EXPECT_STREQ(member->name, "write");
  EXPECT_EQ(clux_interface_find_member(type, "write"), member)
      << "repeated lookups must yield the same stable pointer";
  clux_type_t *dedup =
      clux_context_create_interface_type(ctx, "IStr2", members, 1);
  ASSERT_EQ(dedup, type)
      << "structurally identical input must dedupe to the existing instance";
  EXPECT_EQ(clux_interface_get_member(dedup, 0), member)
      << "a dedup hit must read back the same interned member table";
}

/* 场景: interface IN { func write(data: i32): void; } 具名声明
   预期: 产出 type value 并按名 define（lookup 恒等）；被描述类型 kind
         INTERFACE、名 "IN"、永不 seal；成员表可查且越界返回 NULL */
TEST_F(ExecLiteralTest, InterfaceDeclNamedDefinesTypeValue) {
  node_t *decl = make_interface_decl_of(
      a, "IN", NULL, make_interface_members_of(a, "write", false, NULL));
  clux_value_t *declared = exec_expr_interface_decl(ctx, decl);
  ASSERT_NE(declared, nullptr);
  EXPECT_FALSE(clux_value_is_error(declared));
  EXPECT_EQ(clux_context_lookup(ctx, "IN", NULL), declared)
      << "the named declaration must define the type value by name";
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(declared)),
            CLUX_TYPE_TYPE);
  clux_type_t *described = type_described_by(declared);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_INTERFACE);
  EXPECT_STREQ(clux_type_get_name(described), "IN");
  EXPECT_FALSE(clux_type_is_seal(described));
  EXPECT_EQ(clux_interface_member_count(described), 1u);
  const clux_interface_member_t *member =
      clux_interface_get_member(described, 0);
  ASSERT_NE(member, nullptr);
  EXPECT_STREQ(member->name, "write");
  EXPECT_EQ(clux_interface_get_member(described, 1), nullptr)
      << "an out-of-range index must yield NULL";
  delete_node(&decl);
}

/* 场景: 匿名 interface { func write(data: i32): void; }——表达式位置合法，
        含空成员表（body 槽为 NULL）的类型名回退路径
   预期: 合法形态产出不具名 type value（不 define：lookup("interface")
         为 NULL）、引擎类型名回退关键字文本 "interface"；空成员表报
         "interface 'interface' requires at least one member" */
TEST_F(ExecLiteralTest, InterfaceDeclAnonymousYieldsUnnamedValue) {
  node_t *decl = make_interface_decl_of(
      a, NULL, NULL, make_interface_members_of(a, "write", false, NULL));
  clux_value_t *result = exec_expr_interface_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_INTERFACE);
  EXPECT_STREQ(clux_type_get_name(described), "interface")
      << "an anonymous declaration falls back to the keyword text";
  clux_value_t *probe = clux_context_lookup(ctx, "interface", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(probe)), CLUX_TYPE_ERROR)
      << "an anonymous declaration must not define a name";
  delete_node(&decl);

  node_t *empty = make_interface_decl_of(a, NULL, NULL, NULL);
  clux_value_t *empty_result = exec_expr_interface_decl(ctx, empty);
  ASSERT_NE(empty_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(empty_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(empty_result),
               "interface 'interface' requires at least one member");
  delete_node(&empty);
}

/* 场景: interface 作值类型——直接 create_value 与 var x: IWriter 标注
        （经 create_tdz_value）两条通道
   预期: 均被 create_value 的 seal 门禁拒绝，统一报
         "cannot create a value of incomplete type 'IWriter'" */
TEST_F(ExecLiteralTest, InterfaceDeclSealGateRejectsValuePositions) {
  node_t *decl = make_interface_decl_of(
      a, "IWriter", NULL, make_interface_members_of(a, "write", false, NULL));
  clux_value_t *declared = exec_expr_interface_decl(ctx, decl);
  ASSERT_NE(declared, nullptr);
  ASSERT_FALSE(clux_value_is_error(declared));
  clux_type_t *described = type_described_by(declared);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_INTERFACE);
  ASSERT_FALSE(clux_type_is_seal(described));

  /* 通道一：直接以 interface 类型创建值。 */
  clux_value_t *direct =
      clux_context_create_value(ctx, described, 0, "x", NULL);
  ASSERT_NE(direct, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(direct)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(direct),
               "cannot create a value of incomplete type 'IWriter'");

  /* 通道二：var x: IWriter = undefined（TDZ 声明内调 create_value）。 */
  node_t *var = make_stmt_var("x",
                              CLUX_NODE_LITERAL_IDENTIFIER,
                              "IWriter",
                              CLUX_NODE_LITERAL_UNDEFINED,
                              "undefined");
  clux_value_t *tdz = exec_stmt_var(ctx, var);
  ASSERT_NE(tdz, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(tdz)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(tdz),
               "cannot create a value of incomplete type 'IWriter'");
  delete_node(&var);
  delete_node(&decl);
}

/* 场景: interface IA { ... } 与 interface IB { ... } 成员结构完全一致
   预期: 引擎结构去重（成员名 + 签名判等，类型名不参与）——两份声明
         描述同一个被驻留类型（指针恒等） */
TEST_F(ExecLiteralTest, InterfaceDeclDeduplicatesStructurallyIdentical) {
  node_t *ia = make_interface_decl_of(
      a, "IA", NULL, make_interface_members_of(a, "write", false, NULL));
  clux_value_t *ia_result = exec_expr_interface_decl(ctx, ia);
  ASSERT_NE(ia_result, nullptr);
  EXPECT_FALSE(clux_value_is_error(ia_result));
  node_t *ib = make_interface_decl_of(
      a, "IB", NULL, make_interface_members_of(a, "write", false, NULL));
  clux_value_t *ib_result = exec_expr_interface_decl(ctx, ib);
  ASSERT_NE(ib_result, nullptr);
  EXPECT_FALSE(clux_value_is_error(ib_result));
  EXPECT_EQ(clux_context_lookup(ctx, "IA", NULL), ia_result);
  EXPECT_EQ(clux_context_lookup(ctx, "IB", NULL), ib_result);
  EXPECT_EQ(type_described_by(ia_result), type_described_by(ib_result))
      << "structurally identical interfaces must dedupe to one type";
  delete_node(&ib);
  delete_node(&ia);
}

/* 场景: 泛型 interface IG[W] { ... }
   预期: 报 "generic interface is not supported yet"（等泛型批次接入） */
TEST_F(ExecLiteralTest, InterfaceDeclGenericFormRejected) {
  node_t *generics = create_generic_params_node(a);
  node_t *decl = make_interface_decl_of(
      a, "IG", generics, make_interface_members_of(a, "write", false, NULL));
  clux_value_t *result = exec_expr_interface_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(result)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(result),
               "generic interface is not supported yet");
  clux_value_t *probe = clux_context_lookup(ctx, "IG", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(probe)), CLUX_TYPE_ERROR)
      << "a rejected declaration must not define a name";
  delete_node(&decl);
}

/* 场景: 成员形状校验五路——成员非 FUNC_DECL / 成员缺名 / 成员带函数体 /
        成员带捕获 / 成员带泛型
   预期: 各报对应语义错误消息（均在引擎构造前拦截以携带成员源位置） */
TEST_F(ExecLiteralTest, InterfaceDeclMemberShapeErrors) {
  node_t *not_sig_members = create_interface_members_node(a);
  interface_members_node_add_member(
      not_sig_members, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "oops"));
  node_t *not_sig = make_interface_decl_of(a, "MS", NULL, not_sig_members);
  clux_value_t *not_sig_result = exec_expr_interface_decl(ctx, not_sig);
  ASSERT_NE(not_sig_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(not_sig_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(not_sig_result),
               "interface 'MS' member must be a function signature");
  clux_value_t *probe = clux_context_lookup(ctx, "MS", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(probe)), CLUX_TYPE_ERROR)
      << "a rejected declaration must not define a name";
  delete_node(&not_sig);

  node_t *nameless_members = create_interface_members_node(a);
  interface_members_node_add_member(
      nameless_members,
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        create_func_params_node(a),
                        NULL,
                        make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "void"),
                        NULL));
  node_t *nameless = make_interface_decl_of(a, "MN", NULL, nameless_members);
  clux_value_t *nameless_result = exec_expr_interface_decl(ctx, nameless);
  ASSERT_NE(nameless_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(nameless_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(nameless_result),
               "interface 'MN' member 0 requires a name");
  delete_node(&nameless);

  node_t *with_body = make_interface_decl_of(
      a, "MB", NULL, make_interface_members_of(a, "write", true, NULL));
  clux_value_t *body_result = exec_expr_interface_decl(ctx, with_body);
  ASSERT_NE(body_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(body_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(body_result),
               "interface 'MB' member 'write' must be a signature, not a "
               "function body");
  delete_node(&with_body);

  node_t *captures = create_func_captures_node(a);
  func_captures_node_add_capture(
      captures,
      make_capture_of(a,
                      make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "c"),
                      make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *with_captures = make_interface_decl_of(
      a, "MC", NULL, make_interface_members_of(a, "write", false, captures));
  clux_value_t *captures_result = exec_expr_interface_decl(ctx, with_captures);
  ASSERT_NE(captures_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(captures_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(captures_result),
               "interface 'MC' member 'write' cannot have captures");
  delete_node(&with_captures);

  node_t *generic_members = create_interface_members_node(a);
  interface_members_node_add_member(
      generic_members,
      make_func_decl_of(
          a,
          NULL,
          create_generic_params_node(a),
          create_func_params_node(a),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "write"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "void"),
          NULL));
  node_t *generic = make_interface_decl_of(a, "MG", NULL, generic_members);
  clux_value_t *generic_result = exec_expr_interface_decl(ctx, generic);
  ASSERT_NE(generic_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(generic_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(generic_result),
               "interface 'MG' member 'write' cannot be generic");
  delete_node(&generic);
}

/* 场景: 成员签名校验四路——成员缺返回类型 / 成员重名 / 空成员表（空结点
        与 body 槽 NULL 两种形态）
   预期: 各报对应语义错误消息 */
TEST_F(ExecLiteralTest, InterfaceDeclMemberSignatureErrors) {
  node_t *no_ret_members = create_interface_members_node(a);
  interface_members_node_add_member(
      no_ret_members,
      make_func_decl_of(
          a,
          NULL,
          NULL,
          create_func_params_node(a),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "write"),
          NULL,
          NULL));
  node_t *no_ret = make_interface_decl_of(a, "MR", NULL, no_ret_members);
  clux_value_t *no_ret_result = exec_expr_interface_decl(ctx, no_ret);
  ASSERT_NE(no_ret_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(no_ret_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(no_ret_result),
               "interface 'MR' member 'write' requires a return type");
  delete_node(&no_ret);

  node_t *duplicate_member = create_interface_members_node(a);
  interface_members_node_add_member(
      duplicate_member,
      make_func_decl_of(
          a,
          NULL,
          NULL,
          create_func_params_node(a),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "write"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "void"),
          NULL));
  interface_members_node_add_member(
      duplicate_member,
      make_func_decl_of(
          a,
          NULL,
          NULL,
          create_func_params_node(a),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "write"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "void"),
          NULL));
  node_t *dup = make_interface_decl_of(a, "MD", NULL, duplicate_member);
  clux_value_t *dup_result = exec_expr_interface_decl(ctx, dup);
  ASSERT_NE(dup_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(dup_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(dup_result),
               "interface 'MD' has duplicate member 'write'");
  delete_node(&dup);

  node_t *empty =
      make_interface_decl_of(a, "MQ", NULL, create_interface_members_node(a));
  clux_value_t *empty_result = exec_expr_interface_decl(ctx, empty);
  ASSERT_NE(empty_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(empty_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(empty_result),
               "interface 'MQ' requires at least one member");
  delete_node(&empty);

  node_t *null_body = make_interface_decl_of(a, "MQ0", NULL, NULL);
  clux_value_t *null_body_result = exec_expr_interface_decl(ctx, null_body);
  ASSERT_NE(null_body_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(null_body_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(null_body_result),
               "interface 'MQ0' requires at least one member");
  delete_node(&null_body);
}

/* 场景: 参数级校验三路——rest 变参成员 / 参数缺类型 / 参数类型解析失败
   预期: rest 与缺类型报精确消息；类型解析失败传播公共求值通道的 error
         值（kind ERROR），且声明名不被 define */
TEST_F(ExecLiteralTest, InterfaceDeclParameterErrors) {
  node_t *rest_params = create_func_params_node(a);
  func_params_node_add_param(rest_params,
                             make_node(CLUX_NODE_EXPR_REST, "..."));
  node_t *rest_members_real = create_interface_members_node(a);
  interface_members_node_add_member(
      rest_members_real,
      make_func_decl_of(
          a,
          NULL,
          NULL,
          rest_params,
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "write"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "void"),
          NULL));
  node_t *rest = make_interface_decl_of(a, "MV", NULL, rest_members_real);
  clux_value_t *rest_result = exec_expr_interface_decl(ctx, rest);
  ASSERT_NE(rest_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(rest_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(rest_result),
               "variadic interface member is not supported yet");
  delete_node(&rest);

  node_t *typeless_params = create_func_params_node(a);
  func_params_node_add_param(
      typeless_params,
      make_named_param_of(
          a, make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "data"), NULL));
  node_t *typeless_members = create_interface_members_node(a);
  interface_members_node_add_member(
      typeless_members,
      make_func_decl_of(
          a,
          NULL,
          NULL,
          typeless_params,
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "write"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "void"),
          NULL));
  node_t *typeless = make_interface_decl_of(a, "MP", NULL, typeless_members);
  clux_value_t *typeless_result = exec_expr_interface_decl(ctx, typeless);
  ASSERT_NE(typeless_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(typeless_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(typeless_result),
               "interface 'MP' member 'write' parameter 0 requires a type");
  delete_node(&typeless);

  node_t *unknown_params = create_func_params_node(a);
  func_params_node_add_param(
      unknown_params,
      make_named_param_of(
          a,
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "data"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "no_such_type")));
  node_t *unknown_members = create_interface_members_node(a);
  interface_members_node_add_member(
      unknown_members,
      make_func_decl_of(
          a,
          NULL,
          NULL,
          unknown_params,
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "write"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "void"),
          NULL));
  node_t *unknown = make_interface_decl_of(a, "MU", NULL, unknown_members);
  clux_value_t *unknown_result = exec_expr_interface_decl(ctx, unknown);
  ASSERT_NE(unknown_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(unknown_result)),
            CLUX_TYPE_ERROR)
      << "a param type resolution failure must propagate the error value";
  clux_value_t *probe = clux_context_lookup(ctx, "MU", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(probe)), CLUX_TYPE_ERROR)
      << "a rejected declaration must not define a name";
  delete_node(&unknown);

  /* 参数槽为 NULL（func_params_node_get_param 返回 NULL 的容错路径）。 */
  node_t *null_params = create_func_params_node(a);
  func_params_node_add_param(null_params, NULL);
  node_t *null_member_members = create_interface_members_node(a);
  interface_members_node_add_member(
      null_member_members,
      make_func_decl_of(
          a,
          NULL,
          NULL,
          null_params,
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "write"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "void"),
          NULL));
  node_t *null_param_decl =
      make_interface_decl_of(a, "MP0", NULL, null_member_members);
  clux_value_t *null_param_result =
      exec_expr_interface_decl(ctx, null_param_decl);
  ASSERT_NE(null_param_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(null_param_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(null_param_result),
               "interface 'MP0' member 'write' parameter 0 requires a type");
  delete_node(&null_param_decl);

  /* 返回类型求值失败：错误经公共类型求值通道传播。 */
  node_t *bad_ret_members = create_interface_members_node(a);
  interface_members_node_add_member(
      bad_ret_members,
      make_func_decl_of(
          a,
          NULL,
          NULL,
          create_func_params_node(a),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "write"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "no_such_ret"),
          NULL));
  node_t *bad_ret = make_interface_decl_of(a, "MR0", NULL, bad_ret_members);
  clux_value_t *bad_ret_result = exec_expr_interface_decl(ctx, bad_ret);
  ASSERT_NE(bad_ret_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(bad_ret_result)),
            CLUX_TYPE_ERROR)
      << "a return type resolution failure must propagate the error value";
  delete_node(&bad_ret);
}

/* 场景: 复合签名——interface IC { func f(a: i32, b: *i32): *i32;
        func g(): void; } 多成员、多参数、指针复合类型
   预期: 成员按声明序可查；签名经 create_func_type 通道构造并与显式
         构造的期望类型结构恒等（指针相同——func 类型按结构驻留） */
TEST_F(ExecLiteralTest, InterfaceDeclCompositeSignatures) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *void_type = clux_context_builtin_type(ctx, CLUX_TYPE_VOID);
  clux_type_t *ptr_i32 = clux_context_get_ptr_type(ctx, i32_type);

  node_t *f_params = create_func_params_node(a);
  func_params_node_add_param(
      f_params,
      make_named_param_of(
          a,
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "a"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  func_params_node_add_param(
      f_params,
      make_named_param_of(
          a,
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "b"),
          make_binary_of(
              a,
              NULL,
              make_node(CLUX_NODE_SYMBOL, "*"),
              make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"))));
  node_t *f_members = create_interface_members_node(a);
  interface_members_node_add_member(
      f_members,
      make_func_decl_of(
          a,
          NULL,
          NULL,
          f_params,
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_binary_of(
              a,
              NULL,
              make_node(CLUX_NODE_SYMBOL, "*"),
              make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")),
          NULL));
  interface_members_node_add_member(
      f_members,
      make_func_decl_of(a,
                        NULL,
                        NULL,
                        create_func_params_node(a),
                        make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "g"),
                        make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "void"),
                        NULL));
  node_t *decl = make_interface_decl_of(a, "IC", NULL, f_members);
  clux_value_t *declared = exec_expr_interface_decl(ctx, decl);
  ASSERT_NE(declared, nullptr);
  EXPECT_FALSE(clux_value_is_error(declared));
  EXPECT_EQ(clux_context_lookup(ctx, "IC", NULL), declared);
  clux_type_t *described = type_described_by(declared);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_INTERFACE);
  EXPECT_EQ(clux_interface_member_count(described), 2u);

  clux_type_t *f_signature_params[] = {i32_type, ptr_i32};
  clux_type_t *expected_f = clux_context_create_func_type(
      ctx, "f", NULL, 0, f_signature_params, 2, ptr_i32);
  ASSERT_NE(expected_f, nullptr);
  clux_type_t *expected_g =
      clux_context_create_func_type(ctx, "g", NULL, 0, NULL, 0, void_type);
  ASSERT_NE(expected_g, nullptr);
  const clux_interface_member_t *member_f =
      clux_interface_get_member(described, 0);
  ASSERT_NE(member_f, nullptr);
  EXPECT_STREQ(member_f->name, "f");
  EXPECT_EQ(member_f->func, expected_f)
      << "the f signature must be interned with the explicit create_func_type "
         "channel";
  const clux_interface_member_t *member_g =
      clux_interface_get_member(described, 1);
  ASSERT_NE(member_g, nullptr);
  EXPECT_STREQ(member_g->name, "g");
  EXPECT_EQ(member_g->func, expected_g);
  EXPECT_EQ(clux_interface_find_member(described, "f"), member_f);
  EXPECT_EQ(clux_interface_find_member(described, "g"), member_g);
  EXPECT_EQ(clux_interface_find_member(described, "h"), nullptr);
  delete_node(&decl);
}

/* 场景: STMT_INTERFACE 语句层校验三路——缺声明 / 匿名声明无装饰器 /
        匿名声明带装饰器
   预期: 前者报 "interface statement requires an interface declaration"，
         后两者报 "interface statement requires a named interface
         declaration"（语句包裹层强制具名，匿名只在表达式位置合法） */
TEST_F(ExecLiteralTest, InterfaceStmtValidationErrors) {
  node_t *missing = create_stmt_interface_node(a);
  clux_value_t *missing_result = exec_stmt_interface(ctx, missing);
  ASSERT_NE(missing_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(missing_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(missing_result),
               "interface statement requires an interface declaration");
  delete_node(&missing);

  node_t *anon = create_stmt_interface_node(a);
  stmt_interface_node_set_interface_decl(
      anon,
      make_interface_decl_of(
          a, NULL, NULL, make_interface_members_of(a, "write", false, NULL)));
  clux_value_t *anon_result = exec_stmt_interface(ctx, anon);
  ASSERT_NE(anon_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(anon_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(anon_result),
               "interface statement requires a named interface declaration");
  delete_node(&anon);

  node_t *decorated = create_stmt_interface_node(a);
  stmt_interface_node_set_interface_decl(
      decorated,
      make_interface_decl_of(
          a, NULL, NULL, make_interface_members_of(a, "write", false, NULL)));
  stmt_interface_node_add_decorator(
      decorated,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *decorated_result = exec_stmt_interface(ctx, decorated);
  ASSERT_NE(decorated_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(decorated_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(decorated_result),
               "interface statement requires a named interface declaration");
  delete_node(&decorated);

  /* 装饰路径的匿名求值失败：结构非法的声明经 exec_expr_interface_decl_value
     求值出 error，直接返回且名字不被 define。 */
  node_t *invalid = create_stmt_interface_node(a);
  stmt_interface_node_set_interface_decl(
      invalid,
      make_interface_decl_of(a, "DQ", NULL, create_interface_members_node(a)));
  stmt_interface_node_add_decorator(
      invalid,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *invalid_result = exec_stmt_interface(ctx, invalid);
  ASSERT_NE(invalid_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(invalid_result)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(invalid_result),
               "interface 'DQ' requires at least one member");
  delete_node(&invalid);
}

/* 场景: STMT_INTERFACE 与 EXPR_INTERFACE_DECL 分派——两者经 exec_node
        顶层槽位表注册递归
   预期: stmt 路径按 exec_stmt_interface 定义具名 type value；expr 路径
         按 exec_expr_interface_decl 定义；两者结构一致故描述同一被驻留
         interface 类型（引擎去重指针恒等） */
TEST_F(ExecLiteralTest, InterfaceStmtDispatchDefinesByName) {
  node_t *stmt = create_stmt_interface_node(a);
  stmt_interface_node_set_interface_decl(
      stmt,
      make_interface_decl_of(
          a,
          "IStmt",
          NULL,
          make_interface_members_of(a, "write", false, NULL)));
  clux_value_t *result = exec_node(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_context_lookup(ctx, "IStmt", NULL), result)
      << "the interface statement must define the type value by name";
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_INTERFACE);
  EXPECT_FALSE(clux_type_is_seal(described));
  EXPECT_EQ(clux_interface_member_count(described), 1u);
  delete_node(&stmt);

  node_t *decl = make_interface_decl_of(
      a, "IExpr", NULL, make_interface_members_of(a, "write", false, NULL));
  clux_value_t *expr_result = exec_node(ctx, decl);
  ASSERT_NE(expr_result, nullptr);
  EXPECT_FALSE(clux_value_is_error(expr_result));
  EXPECT_EQ(clux_context_lookup(ctx, "IExpr", NULL), expr_result)
      << "the interface expression must define the type value by name";
  EXPECT_EQ(type_described_by(expr_result), described)
      << "structurally identical declarations must dedupe to one type";
  delete_node(&decl);
}

/* 场景: STMT_INTERFACE 非函数装饰器透传——[[7]]（数值）与 [[i32]]（type
        value）求值成功但非 func，按编译期标记跳过包装
   预期: 源 type value 原样透传并按名 define：被描述类型仍为 INTERFACE
         （成员数 1）、lookup 恒等；不触发执行器安装（保持 NULL） */
TEST_F(ExecLiteralTest, InterfaceStmtNonFuncDecoratorsPassThrough) {
  node_t *stmt = create_stmt_interface_node(a);
  stmt_interface_node_set_interface_decl(
      stmt,
      make_interface_decl_of(
          a, "IDN", NULL, make_interface_members_of(a, "write", false, NULL)));
  stmt_interface_node_add_decorator(
      stmt, make_decorator_of(a, make_node(CLUX_NODE_LITERAL_NUMERIC, "7")));
  stmt_interface_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  clux_value_t *result = exec_stmt_interface(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_context_lookup(ctx, "IDN", NULL), result)
      << "non-func decorators must still bind the value under the name";
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_INTERFACE);
  EXPECT_EQ(clux_interface_member_count(described), 1u)
      << "non-func decorators must pass the source type value through";
  EXPECT_EQ(clux_context_get_executor(ctx), nullptr)
      << "non-func decorators must not touch the executor";
  delete_node(&stmt);
}

/* 场景: STMT_INTERFACE 装饰器链 e2e——装饰器是 comptime func 值
        wrap_t(u: type): type { return i32; }，interface DI { ... }
        经 [[wrap_t]] 包装；失败装饰器 [[no_such_decorator]] 传播 error
   预期: 匿名求值源类型值 → invoke wrap_t（type value 作实参进帧，
         return i32 出帧）→ 最终名字 DI 绑定包装后的返回值：被描述类型
         恒等于 i32 builtin（证明包装真实发生而非透传）、invoke 前懒安装
         帧协议执行器；失败装饰器返回 error 值且名字不被 define */
TEST_F(ExecLiteralTest, InterfaceStmtFuncDecoratorWrapsEndToEnd) {
  clux_type_t *meta = clux_context_get_type_type(ctx);
  clux_type_t *wrap_params[1] = {meta};
  clux_type_t *wrap_type = clux_context_create_func_type(
      ctx, "wrap_t", NULL, 0, wrap_params, 1, meta);
  ASSERT_NE(wrap_type, nullptr);
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(
          a,
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "u"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "type")));
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(
      ret, make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(body, ret);
  node_t *wrap_decl = make_func_decl_of(
      a,
      NULL,
      NULL,
      params,
      make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "wrap_t"),
      make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "type"),
      body);
  clux_value_t *wrap_func =
      clux_context_create_func(ctx, wrap_type, NULL, NULL);
  ASSERT_NE(wrap_func, nullptr);
  ASSERT_EQ(clux_context_func_set_ast(ctx, wrap_func, wrap_decl, NULL),
            wrap_func);
  clux_value_t *wrap_bound = clux_context_declare_value(
      ctx, wrap_func, "wrap_t", node_get_location(wrap_decl));
  ASSERT_NE(wrap_bound, nullptr);
  EXPECT_FALSE(clux_value_is_error(wrap_bound));

  node_t *stmt = create_stmt_interface_node(a);
  stmt_interface_node_set_interface_decl(
      stmt,
      make_interface_decl_of(
          a, "DI", NULL, make_interface_members_of(a, "write", false, NULL)));
  stmt_interface_node_add_decorator(
      stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "wrap_t")));
  clux_value_t *result = exec_stmt_interface(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_context_lookup(ctx, "DI", NULL), result)
      << "the decorated path must bind the wrapped value under the name";
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE);
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(described, clux_context_builtin_type(ctx, CLUX_TYPE_I32))
      << "the decorator must substitute the source interface type with its "
         "return type";
  EXPECT_NE(clux_context_get_executor(ctx), nullptr)
      << "invoking the func decorator must lazily install the executor";
  delete_node(&stmt);
  delete_node(&wrap_decl);

  node_t *fail_stmt = create_stmt_interface_node(a);
  stmt_interface_node_set_interface_decl(
      fail_stmt,
      make_interface_decl_of(
          a, "DZ", NULL, make_interface_members_of(a, "write", false, NULL)));
  stmt_interface_node_add_decorator(
      fail_stmt,
      make_decorator_of(
          a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "no_such_decorator")));
  clux_value_t *fail_result = exec_stmt_interface(ctx, fail_stmt);
  ASSERT_NE(fail_result, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(fail_result)),
            CLUX_TYPE_ERROR)
      << "a failing decorator must propagate its error value";
  clux_value_t *probe = clux_context_lookup(ctx, "DZ", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(probe)), CLUX_TYPE_ERROR)
      << "a failing decorator must not define the name";
  delete_node(&fail_stmt);
}

/* ---- exec_expr_struct_decl / exec_stmt_struct：struct 声明 exec ---- */

/* 合成 name: type 字段成员（name/type 结点由调用方构造并持有）。 */
static node_t *
make_field_member_of(allocator_t *allocator, node_t *name, node_t *type) {
  node_t *node = create_field_node(allocator);
  field_node_set_name(node, name);
  field_node_set_type(node, type);
  return node;
}

/* 合成 kind 结点，location 覆盖 text 完整区间（text 须为持久存储，
   与 fixture make_node 同构，供静态构造 helper 使用）。 */
static node_t *make_static_node_of(allocator_t *allocator,
                                   node_kind_t kind,
                                   const char *text) {
  node_t *node = create_node(allocator, kind);
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

/* 合成 struct name_text { members } 声明体（name_text 为 NULL 表示匿名，
   generics 允许为 NULL，members 允许为 NULL 走容错路径）。 */
static node_t *make_struct_decl_of(allocator_t *allocator,
                                   const char *name_text,
                                   node_t *generics,
                                   node_t *members) {
  node_t *node = create_struct_decl_node(allocator);
  struct_decl_node_set_name(
      node,
      name_text ? make_static_node_of(
                      allocator, CLUX_NODE_LITERAL_IDENTIFIER, name_text)
                : NULL);
  struct_decl_node_set_generics(node, generics);
  struct_decl_node_set_body(node, members);
  return node;
}

/* 合成 var name = value 成员（attribute 路径，name/value 结点由调用方
   构造并持有，delete 根结点时一并释放）。 */
static node_t *
make_var_member_of(allocator_t *allocator, node_t *name, node_t *value) {
  node_t *node = create_stmt_var_node(allocator);
  stmt_var_node_set_name(node, name);
  stmt_var_node_set_value(node, value);
  return node;
}

/* 合成 ...type 展开成员（embed 路径，type 结点由调用方构造并持有）。 */
static node_t *make_embed_member_of(allocator_t *allocator, node_t *type) {
  node_t *node = create_struct_embed_node(allocator);
  struct_embed_node_set_type(node, type);
  return node;
}

/* 场景: struct User { name: str; var count = 0; func rename(self: *User,
        new: str): str { return new; } } 具名两阶段构造快乐路径
   预期: 壳按名 define（lookup 恒等）；字段表 1 项（name: str 指针与
         builtin 恒等）；attribute 表 2 项（count 值可读回 i32 0，
         rename 为 func 值）；method 名单只含 rename；方法首参 *User
         与引擎 ptr 通道指针恒等（self 引用穿过壳的按名 define），
         次参等于 str builtin；未知 attr 查询返回 nullptr */
TEST_F(ExecLiteralTest,
       StructDeclNamedPathInstallsShellAndSelfReferencingMethod) {
  node_t *members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "name"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  struct_members_node_add_member(
      members,
      make_var_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "count"),
          make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "0")));
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "self"),
          make_binary_of(
              a,
              NULL,
              make_static_node_of(a, CLUX_NODE_SYMBOL, "*"),
              make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "User"))));
  func_params_node_add_param(
      params,
      make_named_param_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "new"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  node_t *body = create_stmt_block_node(a);
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(
      ret, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "new"));
  stmt_block_node_add_stmt(body, ret);
  node_t *rename_decl = make_func_decl_of(
      a,
      NULL,
      NULL,
      params,
      make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "rename"),
      make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str"),
      body);
  struct_members_node_add_member(members, make_stmt_func_of(a, rename_decl));
  node_t *decl = make_struct_decl_of(a, "User", NULL, members);

  clux_value_t *result = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(result)), CLUX_TYPE_TYPE);
  EXPECT_EQ(clux_context_lookup(ctx, "User", NULL), result)
      << "the named path must bind the shell type value under the name";
  clux_type_t *user_type = type_described_by(result);
  ASSERT_NE(user_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(user_type), CLUX_TYPE_STRUCT);
  clux_type_t *str_type = clux_context_builtin_type(ctx, CLUX_TYPE_STR);
  EXPECT_EQ(clux_struct_field_count(user_type), 1u);
  const clux_struct_field_t *name_field =
      clux_struct_find_field(user_type, "name");
  ASSERT_NE(name_field, nullptr);
  EXPECT_STREQ(name_field->name, "name");
  EXPECT_EQ(name_field->type, str_type);
  EXPECT_EQ(clux_struct_attr_count(user_type), 2u);
  const clux_struct_attr_t *count_attr =
      clux_struct_find_attr(user_type, "count");
  ASSERT_NE(count_attr, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(count_attr->value)),
            CLUX_TYPE_I32);
  EXPECT_EQ(read_i32(count_attr->value), 0);
  const clux_struct_attr_t *rename_attr =
      clux_struct_find_attr(user_type, "rename");
  ASSERT_NE(rename_attr, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(rename_attr->value)),
            CLUX_TYPE_FUNC);
  EXPECT_TRUE(clux_struct_is_method(user_type, "rename"));
  EXPECT_FALSE(clux_struct_is_method(user_type, "count"))
      << "the count attribute is a var member and must not enter the method "
         "list";
  clux_type_t *rename_type = clux_value_get_type(rename_attr->value);
  EXPECT_EQ(clux_func_param_count(rename_type), 2u);
  EXPECT_EQ(clux_func_get_param(rename_type, 0),
            clux_context_get_ptr_type(ctx, user_type))
      << "the self parameter must resolve to the ptr of the shell itself";
  EXPECT_EQ(clux_func_get_param(rename_type, 1), str_type);
  EXPECT_EQ(clux_struct_find_attr(user_type, "absent"), nullptr)
      << "an unknown attribute lookup must return NULL";
  delete_node(&decl);
}

/* 场景: struct Base { a: i32; b: str } 前置声明 + struct Derived
        { pre: u8; ...Base; post: bool } embed 在字段间展开
   预期: Derived 字段表按声明序 4 项 pre/a/b/post，embed 槽位展开保持
         Base 声明顺序；展开字段类型指针与来源字段恒等（借用展开） */
TEST_F(ExecLiteralTest, StructDeclEmbedExpandsFieldsInDeclarationOrder) {
  node_t *base_members = create_struct_members_node(a);
  struct_members_node_add_member(
      base_members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "a"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  struct_members_node_add_member(
      base_members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "b"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  node_t *base_decl = make_struct_decl_of(a, "Base", NULL, base_members);
  clux_value_t *base_result = exec_expr_struct_decl(ctx, base_decl);
  ASSERT_NE(base_result, nullptr);
  EXPECT_FALSE(clux_value_is_error(base_result));
  clux_type_t *base_type = type_described_by(base_result);
  ASSERT_NE(base_type, nullptr);

  node_t *members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "pre"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "u8")));
  struct_members_node_add_member(
      members,
      make_embed_member_of(
          a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Base")));
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "post"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "bool")));
  node_t *decl = make_struct_decl_of(a, "Derived", NULL, members);
  clux_value_t *result = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  clux_type_t *derived = type_described_by(result);
  ASSERT_NE(derived, nullptr);
  ASSERT_EQ(clux_struct_field_count(derived), 4u);
  const char *names[] = {"pre", "a", "b", "post"};
  clux_type_kind_t kinds[] = {
      CLUX_TYPE_U8, CLUX_TYPE_I32, CLUX_TYPE_STR, CLUX_TYPE_BOOL};
  for (size_t idx = 0; idx < 4; idx++) {
    const clux_struct_field_t *field = clux_struct_get_field(derived, idx);
    ASSERT_NE(field, nullptr) << "field " << idx;
    EXPECT_STREQ(field->name, names[idx]) << "field " << idx;
    EXPECT_EQ(clux_type_get_kind(field->type), kinds[idx]) << "field " << idx;
  }
  EXPECT_EQ(clux_struct_get_field(derived, 1)->type,
            clux_struct_get_field(base_type, 0)->type)
      << "embed expansion must borrow the source field type pointers";
  EXPECT_EQ(clux_struct_get_field(derived, 2)->type,
            clux_struct_get_field(base_type, 1)->type)
      << "embed expansion must borrow the source field type pointers";
  delete_node(&decl);
  delete_node(&base_decl);
}

/* 场景: exec_stmt_struct / exec_expr_struct_decl 拒绝路径——语句缺声明
        体/语句匿名声明体/声明体带泛型参数表
   预期: 各路径返回精确错误文本；泛型失败时名字不 define（lookup 产出
         error 哨兵而非 nullptr） */
TEST_F(ExecLiteralTest, StructStatementRejectsMissingAndGenericDeclarations) {
  node_t *empty = create_stmt_struct_node(a);
  clux_value_t *missing = exec_stmt_struct(ctx, empty);
  ASSERT_NE(missing, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(missing)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(missing),
               "struct statement requires a struct declaration");
  delete_node(&empty);

  node_t *anon_stmt = create_stmt_struct_node(a);
  stmt_struct_node_set_struct_decl(anon_stmt,
                                   make_struct_decl_of(a, NULL, NULL, NULL));
  clux_value_t *anon = exec_stmt_struct(ctx, anon_stmt);
  ASSERT_NE(anon, nullptr);
  EXPECT_STREQ(error_message_of(anon),
               "struct statement requires a named struct declaration");
  delete_node(&anon_stmt);

  node_t *members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "v"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *generic_decl = make_struct_decl_of(
      a,
      "Cell",
      make_static_node_of(a, CLUX_NODE_GENERIC_PARAMS, "<T>"),
      members);
  clux_value_t *generic = exec_expr_struct_decl(ctx, generic_decl);
  ASSERT_NE(generic, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(generic)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(generic),
               "generic struct is not supported yet");
  clux_value_t *probe = clux_context_lookup(ctx, "Cell", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(probe)), CLUX_TYPE_ERROR)
      << "the failed generic declaration must not define the name";
  delete_node(&generic_decl);
}

/* 场景: embed 三条错误路径——embed 成员缺类型结点/嵌入非 struct 类型
        （i32 builtin）/嵌入未声明标识符 Nope
   预期: 前两条产出精确 embed 错误文本；第三条透传标识符求值错误文本 */
TEST_F(ExecLiteralTest, StructDeclEmbedErrorPathsReportSemanticErrors) {
  node_t *members = create_struct_members_node(a);
  struct_members_node_add_member(members, create_struct_embed_node(a));
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  node_t *decl = make_struct_decl_of(a, "SE", NULL, members);
  clux_value_t *missing_type = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(missing_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(missing_type)),
            CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(missing_type), "struct embed requires a type");
  delete_node(&decl);

  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_embed_member_of(
          a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  decl = make_struct_decl_of(a, "SE", NULL, members);
  clux_value_t *not_struct = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(not_struct, nullptr);
  EXPECT_STREQ(error_message_of(not_struct),
               "struct embed requires a struct type");
  delete_node(&decl);

  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_embed_member_of(
          a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Nope")));
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  decl = make_struct_decl_of(a, "SE", NULL, members);
  clux_value_t *undeclared = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(undeclared, nullptr);
  EXPECT_STREQ(error_message_of(undeclared),
               "use of undeclared identifier 'Nope'");
  delete_node(&decl);
}

/* 场景: 字段表四条错误路径——FIELD 缺名（成员索引 0）/缺类型/类型求值
        失败（未声明标识符 Nada）/重复字段名触发 begin 校验失败
   预期: 前三条精确错误文本（缺名消息的 %zu 是成员索引而非字段计数）；
         重复字段被 context 字段校验拦截后 exec 产出统一 collection
         failed 消息，且名字不 define（lookup 为 error 哨兵） */
TEST_F(ExecLiteralTest, StructDeclFieldErrorPathsAndCollectionFailure) {
  node_t *members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          NULL,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  node_t *decl = make_struct_decl_of(a, "SF", NULL, members);
  clux_value_t *no_name = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(no_name, nullptr);
  EXPECT_STREQ(error_message_of(no_name),
               "struct 'SF' field 0 requires a name");
  delete_node(&decl);

  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "x"), NULL));
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  decl = make_struct_decl_of(a, "SF", NULL, members);
  clux_value_t *no_type = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(no_type, nullptr);
  EXPECT_STREQ(error_message_of(no_type),
               "struct 'SF' field 'x' requires a type");
  delete_node(&decl);

  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "x"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Nada")));
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  decl = make_struct_decl_of(a, "SF", NULL, members);
  clux_value_t *bad_type = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(bad_type, nullptr);
  EXPECT_STREQ(error_message_of(bad_type),
               "use of undeclared identifier 'Nada'");
  delete_node(&decl);

  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "x"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "x"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  decl = make_struct_decl_of(a, "SF", NULL, members);
  clux_value_t *dup = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(dup, nullptr);
  EXPECT_STREQ(error_message_of(dup), "struct 'SF' field collection failed");
  clux_value_t *probe = clux_context_lookup(ctx, "SF", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(probe)), CLUX_TYPE_ERROR)
      << "the failed declaration must not define the name";
  delete_node(&decl);
}

/* 场景: 同名 struct 二次声明——壳已按名 define 后再次求值同名声明体
   预期: 第二次 begin 成功（壳不去重）但 define 撞名，create_type_value
         返回 duplicate declaration error 并作为整体结果传播 */
TEST_F(ExecLiteralTest, StructDeclDuplicateNamePropagatesDefineError) {
  node_t *members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "v"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl = make_struct_decl_of(a, "Twice", NULL, members);
  clux_value_t *first = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(first, nullptr);
  EXPECT_FALSE(clux_value_is_error(first));
  clux_value_t *second = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(second)), CLUX_TYPE_ERROR);
  EXPECT_STREQ(error_message_of(second), "duplicate declaration 'Twice'");
  delete_node(&decl);
}

/* 场景: 无装饰器 STMT_STRUCT 语句——语句层主路径（decorator_count == 0
        经 exec_operand 直接执行声明本体）
   预期: 声明按名 define（lookup 恒等），被描述类型为 STRUCT 且字段表
         与声明体一致 */
TEST_F(ExecLiteralTest, StructStatementWithoutDecoratorsDefinesByName) {
  node_t *members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *stmt = create_stmt_struct_node(a);
  stmt_struct_node_set_struct_decl(
      stmt, make_struct_decl_of(a, "Plain", NULL, members));
  clux_value_t *result = exec_stmt_struct(ctx, stmt);
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(clux_value_is_error(result));
  EXPECT_EQ(clux_context_lookup(ctx, "Plain", NULL), result)
      << "the undecorated statement path must define the name";
  clux_type_t *described = type_described_by(result);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(clux_type_get_kind(described), CLUX_TYPE_STRUCT);
  EXPECT_EQ(clux_struct_field_count(described), 1u);
  const clux_struct_field_t *plain_field =
      clux_struct_find_field(described, "f");
  ASSERT_NE(plain_field, nullptr)
      << "the declared field must be present after the define";
  EXPECT_STREQ(plain_field->name, "f");
  delete_node(&stmt);
}

/* 场景: attribute 四条错误路径——var 成员带装饰器/缺名（成员索引 1）/
        缺初值/重名 'a'
   预期: 各路径精确错误文本（缺名消息的 %zu 是成员索引——首个成员是
         合法字段，nameless var 位于成员位 1） */
TEST_F(ExecLiteralTest, StructDeclAttributeErrorPaths) {
  node_t *members = create_struct_members_node(a);
  node_t *decorated = make_var_member_of(
      a,
      make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "d"),
      make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "0"));
  stmt_var_node_add_decorator(
      decorated,
      make_decorator_of(
          a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  struct_members_node_add_member(members, decorated);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  node_t *decl = make_struct_decl_of(a, "SA", NULL, members);
  clux_value_t *decorated_attr = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(decorated_attr, nullptr);
  EXPECT_STREQ(error_message_of(decorated_attr),
               "decorated struct attribute is not supported yet");
  delete_node(&decl);

  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  struct_members_node_add_member(
      members,
      make_var_member_of(
          a, NULL, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "0")));
  decl = make_struct_decl_of(a, "SA1", NULL, members);
  clux_value_t *no_name = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(no_name, nullptr);
  EXPECT_STREQ(error_message_of(no_name),
               "struct 'SA1' attribute 1 requires a name");
  delete_node(&decl);

  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  struct_members_node_add_member(
      members,
      make_var_member_of(
          a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "a"), NULL));
  decl = make_struct_decl_of(a, "SA2", NULL, members);
  clux_value_t *no_init = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(no_init, nullptr);
  EXPECT_STREQ(error_message_of(no_init),
               "struct attribute requires an initializer");
  delete_node(&decl);

  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  struct_members_node_add_member(
      members,
      make_var_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "a"),
          make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "0")));
  struct_members_node_add_member(
      members,
      make_var_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "a"),
          make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
  decl = make_struct_decl_of(a, "SA3", NULL, members);
  clux_value_t *dup = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(dup, nullptr);
  EXPECT_STREQ(error_message_of(dup),
               "struct 'SA3' has duplicate attribute 'a'");
  delete_node(&decl);

  /* attribute 初值表达式求值失败：error 值原样传播。 */
  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  struct_members_node_add_member(
      members,
      make_var_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "b"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "nope")));
  decl = make_struct_decl_of(a, "SA4", NULL, members);
  clux_value_t *bad_init = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(bad_init, nullptr);
  EXPECT_STREQ(error_message_of(bad_init),
               "use of undeclared identifier 'nope'");
  delete_node(&decl);
}

/* 场景: method 四条错误路径——func 成员带装饰器/缺函数声明体（成员
        索引 1）/函数声明缺名（成员索引 1）/与既有 attribute 'act' 重名
   预期: 各路径精确错误文本（重名走 attribute 重名通道而非 method 通道；
         缺声明体/缺名消息的 %zu 是成员索引） */
TEST_F(ExecLiteralTest, StructDeclMethodErrorPaths) {
  node_t *members = create_struct_members_node(a);
  node_t *decorated_decl = make_func_decl_of(
      a,
      NULL,
      NULL,
      NULL,
      make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "m"),
      NULL,
      create_stmt_block_node(a));
  node_t *decorated_method = make_stmt_func_of(a, decorated_decl);
  stmt_func_node_add_decorator(
      decorated_method,
      make_decorator_of(
          a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  struct_members_node_add_member(members, decorated_method);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  node_t *decl = make_struct_decl_of(a, "SM", NULL, members);
  clux_value_t *decorated_method_result = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(decorated_method_result, nullptr);
  EXPECT_STREQ(error_message_of(decorated_method_result),
               "decorated struct method is not supported yet");
  delete_node(&decl);

  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  struct_members_node_add_member(members, create_stmt_func_node(a));
  decl = make_struct_decl_of(a, "SM1", NULL, members);
  clux_value_t *no_decl = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(no_decl, nullptr);
  EXPECT_STREQ(error_message_of(no_decl),
               "struct 'SM1' method 1 requires a function declaration");
  delete_node(&decl);

  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  struct_members_node_add_member(
      members,
      make_stmt_func_of(
          a,
          make_func_decl_of(
              a, NULL, NULL, NULL, NULL, NULL, create_stmt_block_node(a))));
  decl = make_struct_decl_of(a, "SM2", NULL, members);
  clux_value_t *no_name = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(no_name, nullptr);
  EXPECT_STREQ(error_message_of(no_name),
               "struct 'SM2' method 1 requires a name");
  delete_node(&decl);

  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  struct_members_node_add_member(
      members,
      make_var_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "act"),
          make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "0")));
  struct_members_node_add_member(
      members,
      make_stmt_func_of(
          a,
          make_func_decl_of(
              a,
              NULL,
              NULL,
              NULL,
              make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "act"),
              NULL,
              create_stmt_block_node(a))));
  decl = make_struct_decl_of(a, "SM3", NULL, members);
  clux_value_t *conflict = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(conflict, nullptr);
  EXPECT_STREQ(error_message_of(conflict),
               "struct 'SM3' has duplicate attribute 'act'");
  delete_node(&decl);

  /* method 函数求值失败（形参类型为未声明标识符）：error 原样传播。 */
  members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str")));
  node_t *bad_params = create_func_params_node(a);
  func_params_node_add_param(
      bad_params,
      make_named_param_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "x"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Nada")));
  struct_members_node_add_member(
      members,
      make_stmt_func_of(
          a,
          make_func_decl_of(
              a,
              NULL,
              NULL,
              bad_params,
              make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "m"),
              make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "str"),
              create_stmt_block_node(a))));
  decl = make_struct_decl_of(a, "SM4", NULL, members);
  clux_value_t *bad_method = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(bad_method, nullptr);
  EXPECT_STREQ(error_message_of(bad_method),
               "use of undeclared identifier 'Nada'");
  delete_node(&decl);
}

/* 场景: 匿名 struct 声明（无名，字段 n: i32 + attribute v = 5）经
        exec_expr_struct_decl_value 与 exec_expr_struct_decl 双通道求值
   预期: 两通道都产出无名的 type value（不 define 名字）：字段表与
         attribute 表完整（attr 值可读回 5）；同一声明体两次求值产出
         不同类型实例（两阶段构造不做结构去重） */
TEST_F(ExecLiteralTest, StructDeclAnonymousExprEvaluatesUnnamedTypeValue) {
  node_t *members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "n"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  struct_members_node_add_member(
      members,
      make_var_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "v"),
          make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "5")));
  node_t *decl = make_struct_decl_of(a, NULL, NULL, members);

  clux_value_t *first = exec_expr_struct_decl_value(ctx, decl);
  ASSERT_NE(first, nullptr);
  EXPECT_FALSE(clux_value_is_error(first));
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(first)), CLUX_TYPE_TYPE);
  clux_type_t *first_type = type_described_by(first);
  ASSERT_NE(first_type, nullptr);
  EXPECT_EQ(clux_type_get_kind(first_type), CLUX_TYPE_STRUCT);
  EXPECT_EQ(clux_struct_field_count(first_type), 1u);
  EXPECT_EQ(clux_struct_attr_count(first_type), 1u);
  const clux_struct_attr_t *v_attr = clux_struct_find_attr(first_type, "v");
  ASSERT_NE(v_attr, nullptr);
  EXPECT_EQ(read_i32(v_attr->value), 5);

  clux_value_t *second = exec_expr_struct_decl_value(ctx, decl);
  ASSERT_NE(second, nullptr);
  EXPECT_FALSE(clux_value_is_error(second));
  EXPECT_NE(type_described_by(second), first_type)
      << "two-phase struct construction must not dedupe structurally "
         "identical shells";

  clux_value_t *third = exec_expr_struct_decl(ctx, decl);
  ASSERT_NE(third, nullptr);
  EXPECT_FALSE(clux_value_is_error(third));
  EXPECT_EQ(clux_type_kind_unwrap(clux_value_get_type(third)), CLUX_TYPE_TYPE)
      << "the anonymous declaration via the declare_named path must fall "
         "through to the unnamed type value";
  delete_node(&decl);
}

/* 场景: STMT_STRUCT 装饰器链——[[7]] 编译期标记透传；wrap_t(u: type):
        type { return i32; } func 装饰器真实包装（参照 interface 批次
        e2e 模式）；[[no_such_decorator]] 失败传播
   预期: 透传路径 DP 绑定源 type value（字段表 1 项保持、执行器不安装）；
         包装路径 DW 被描述类型恒等 i32 builtin、执行器懒安装、名字绑定
         包装结果；失败路径 DZ 返回 error 且名字不 define */
TEST_F(ExecLiteralTest, StructStmtDecoratorChainWrapsAndDeclaresByName) {
  node_t *pass_members = create_struct_members_node(a);
  struct_members_node_add_member(
      pass_members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *pass_stmt = create_stmt_struct_node(a);
  stmt_struct_node_set_struct_decl(
      pass_stmt, make_struct_decl_of(a, "DP", NULL, pass_members));
  stmt_struct_node_add_decorator(
      pass_stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_NUMERIC, "7")));
  clux_value_t *passed = exec_stmt_struct(ctx, pass_stmt);
  ASSERT_NE(passed, nullptr);
  EXPECT_FALSE(clux_value_is_error(passed));
  EXPECT_EQ(clux_context_lookup(ctx, "DP", NULL), passed)
      << "non-func decorators must still bind the value under the name";
  clux_type_t *dp_type = type_described_by(passed);
  ASSERT_NE(dp_type, nullptr);
  EXPECT_EQ(clux_struct_field_count(dp_type), 1u)
      << "non-func decorators must pass the source type value through";
  EXPECT_EQ(clux_context_get_executor(ctx), nullptr)
      << "non-func decorators must not touch the executor";
  delete_node(&pass_stmt);

  clux_type_t *meta = clux_context_get_type_type(ctx);
  clux_type_t *wrap_params[1] = {meta};
  clux_type_t *wrap_type = clux_context_create_func_type(
      ctx, "wrap_t", NULL, 0, wrap_params, 1, meta);
  ASSERT_NE(wrap_type, nullptr);
  node_t *params = create_func_params_node(a);
  func_params_node_add_param(
      params,
      make_named_param_of(
          a,
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "u"),
          make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "type")));
  node_t *ret = create_stmt_return_node(a);
  stmt_return_node_set_value(
      ret, make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *body = create_stmt_block_node(a);
  stmt_block_node_add_stmt(body, ret);
  node_t *wrap_decl = make_func_decl_of(
      a,
      NULL,
      NULL,
      params,
      make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "wrap_t"),
      make_text_node(a, CLUX_NODE_LITERAL_IDENTIFIER, "type"),
      body);
  clux_value_t *wrap_func =
      clux_context_create_func(ctx, wrap_type, NULL, NULL);
  ASSERT_NE(wrap_func, nullptr);
  ASSERT_EQ(clux_context_func_set_ast(ctx, wrap_func, wrap_decl, NULL),
            wrap_func);
  clux_value_t *wrap_bound = clux_context_declare_value(
      ctx, wrap_func, "wrap_t", node_get_location(wrap_decl));
  ASSERT_NE(wrap_bound, nullptr);
  EXPECT_FALSE(clux_value_is_error(wrap_bound));

  node_t *wrap_members = create_struct_members_node(a);
  struct_members_node_add_member(
      wrap_members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *wrap_stmt = create_stmt_struct_node(a);
  stmt_struct_node_set_struct_decl(
      wrap_stmt, make_struct_decl_of(a, "DW", NULL, wrap_members));
  stmt_struct_node_add_decorator(
      wrap_stmt,
      make_decorator_of(a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "wrap_t")));
  clux_value_t *wrapped = exec_stmt_struct(ctx, wrap_stmt);
  ASSERT_NE(wrapped, nullptr);
  EXPECT_FALSE(clux_value_is_error(wrapped));
  EXPECT_EQ(clux_context_lookup(ctx, "DW", NULL), wrapped)
      << "the decorated path must bind the wrapped value under the name";
  clux_type_t *described = type_described_by(wrapped);
  ASSERT_NE(described, nullptr);
  EXPECT_EQ(described, clux_context_builtin_type(ctx, CLUX_TYPE_I32))
      << "the decorator must substitute the source struct type with its "
         "return type";
  EXPECT_NE(clux_context_get_executor(ctx), nullptr)
      << "invoking the func decorator must lazily install the executor";
  delete_node(&wrap_stmt);
  delete_node(&wrap_decl);

  node_t *fail_members = create_struct_members_node(a);
  struct_members_node_add_member(
      fail_members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *fail_stmt = create_stmt_struct_node(a);
  stmt_struct_node_set_struct_decl(
      fail_stmt, make_struct_decl_of(a, "DZ", NULL, fail_members));
  stmt_struct_node_add_decorator(
      fail_stmt,
      make_decorator_of(
          a, make_node(CLUX_NODE_LITERAL_IDENTIFIER, "no_such_decorator")));
  clux_value_t *failed = exec_stmt_struct(ctx, fail_stmt);
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(failed)), CLUX_TYPE_ERROR)
      << "a failing decorator must propagate its error value";
  clux_value_t *probe = clux_context_lookup(ctx, "DZ", NULL);
  ASSERT_NE(probe, nullptr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(probe)), CLUX_TYPE_ERROR)
      << "a failing decorator must not define the name";
  delete_node(&fail_stmt);
}

/* 场景: context 层两阶段构造直连——begin 字段校验六条 NULL 路径（缺名/
        缺表/空表/字段缺名/字段缺类型/重名），install 对非 struct 类型与
        attr/method/subtype 九类非法表返回 false，正向安装后三表可读，
        二次 install（含全空）拒绝
   预期: 校验失败通道只可观测返回值（错误值被 (void) 丢弃）；正向安装后
         attr_count/mask/subtype 表与注入数据一致（method 判定看名单，
         名单外同名 attr 只是普通 attribute） */
TEST_F(ExecLiteralTest, StructContextTwoPhaseBeginAndInstallValidationPaths) {
  clux_type_t *i32_type = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  clux_type_t *str_type = clux_context_builtin_type(ctx, CLUX_TYPE_STR);
  clux_struct_field_t fields[2] = {{"x", i32_type}, {"y", str_type}};

  clux_type_t *shell = clux_context_begin_struct_type(ctx, "SB", fields, 2);
  ASSERT_NE(shell, nullptr);
  EXPECT_EQ(clux_struct_field_count(shell), 2u);
  ASSERT_NE(clux_struct_find_field(shell, "x"), nullptr);
  EXPECT_STREQ(clux_struct_find_field(shell, "x")->name, "x");

  EXPECT_EQ(clux_context_begin_struct_type(ctx, NULL, fields, 2), nullptr)
      << "a missing name must be rejected";
  EXPECT_EQ(clux_context_begin_struct_type(ctx, "SN", NULL, 2), nullptr)
      << "a missing field table must be rejected";
  EXPECT_EQ(clux_context_begin_struct_type(ctx, "SN", fields, 0), nullptr)
      << "an empty field table must be rejected";
  clux_struct_field_t no_name[1] = {{NULL, i32_type}};
  EXPECT_EQ(clux_context_begin_struct_type(ctx, "SN", no_name, 1), nullptr)
      << "a field without a name must be rejected";
  clux_struct_field_t no_type[1] = {{"x", NULL}};
  EXPECT_EQ(clux_context_begin_struct_type(ctx, "SN", no_type, 1), nullptr)
      << "a field without a type must be rejected";
  clux_struct_field_t dup[2] = {{"x", i32_type}, {"x", str_type}};
  EXPECT_EQ(clux_context_begin_struct_type(ctx, "SN", dup, 2), nullptr)
      << "duplicate field names must be rejected";

  clux_value_t *num = clux_context_create_i32(ctx, 4, NULL, NULL);
  ASSERT_NE(num, nullptr);
  clux_type_t *m_params[1] = {i32_type};
  clux_type_t *m_type =
      clux_context_create_func_type(ctx, "m", NULL, 0, m_params, 1, i32_type);
  ASSERT_NE(m_type, nullptr);
  clux_value_t *func = clux_context_create_func(ctx, m_type, NULL, NULL);
  ASSERT_NE(func, nullptr);

  clux_struct_field_t one[1] = {{"n", i32_type}};
  clux_type_t *target = clux_context_begin_struct_type(ctx, "SIN", one, 1);
  ASSERT_NE(target, nullptr);

  clux_struct_attr_t bad_attr_name[1] = {{NULL, num}};
  clux_struct_attr_t bad_attr_value[1] = {{"n", NULL}};
  clux_struct_attr_t dup_attr[2] = {{"a", num}, {"a", num}};
  clux_struct_attr_t mixed_attrs[2] = {{"n", num}, {"m", func}};
  clux_struct_attr_t nonfunc_attrs[1] = {{"m", num}};
  const char *null_method[1] = {NULL};
  const char *missing_method[1] = {"gone"};
  const char *dup_method[2] = {"m", "m"};
  const char *valid_method[1] = {"m"};
  clux_struct_subtype_t bad_sub_name[1] = {{NULL, i32_type}};
  clux_struct_subtype_t bad_sub_type[1] = {{"s", NULL}};
  clux_struct_subtype_t dup_sub[2] = {{"s", i32_type}, {"s", i32_type}};
  clux_struct_subtype_t subtypes[1] = {{"s", i32_type}};

  EXPECT_FALSE(clux_context_install_struct_tables(
      ctx, i32_type, NULL, 0, NULL, 0, NULL, 0))
      << "installing into a non-struct type must be rejected";
  EXPECT_FALSE(clux_context_install_struct_tables(
      ctx, target, bad_attr_name, 1, valid_method, 1, subtypes, 1))
      << "an attribute without a name must be rejected";
  EXPECT_FALSE(clux_context_install_struct_tables(
      ctx, target, bad_attr_value, 1, valid_method, 1, subtypes, 1))
      << "an attribute without a value must be rejected";
  EXPECT_FALSE(clux_context_install_struct_tables(
      ctx, target, dup_attr, 2, NULL, 0, NULL, 0))
      << "duplicate attribute names must be rejected";
  EXPECT_FALSE(clux_context_install_struct_tables(
      ctx, target, NULL, 0, null_method, 1, NULL, 0))
      << "a method without a name must be rejected";
  EXPECT_FALSE(clux_context_install_struct_tables(
      ctx, target, mixed_attrs, 2, missing_method, 1, NULL, 0))
      << "a method missing from the attribute table must be rejected";
  EXPECT_FALSE(clux_context_install_struct_tables(
      ctx, target, nonfunc_attrs, 1, valid_method, 1, NULL, 0))
      << "a method backed by a non-func attribute value must be rejected";
  EXPECT_FALSE(clux_context_install_struct_tables(
      ctx, target, mixed_attrs, 2, dup_method, 2, NULL, 0))
      << "duplicate method names must be rejected";
  EXPECT_FALSE(clux_context_install_struct_tables(
      ctx, target, NULL, 0, NULL, 0, bad_sub_name, 1))
      << "a subtype without a name must be rejected";
  EXPECT_FALSE(clux_context_install_struct_tables(
      ctx, target, NULL, 0, NULL, 0, bad_sub_type, 1))
      << "a subtype without a type must be rejected";
  EXPECT_FALSE(clux_context_install_struct_tables(
      ctx, target, NULL, 0, NULL, 0, dup_sub, 2))
      << "duplicate subtype names must be rejected";

  EXPECT_TRUE(clux_context_install_struct_tables(
      ctx, target, mixed_attrs, 2, valid_method, 1, subtypes, 1));
  EXPECT_EQ(clux_struct_attr_count(target), 2u);
  const clux_struct_attr_t *n_attr = clux_struct_find_attr(target, "n");
  ASSERT_NE(n_attr, nullptr)
      << "the installed attribute must be retrievable by name";
  EXPECT_EQ(read_i32(n_attr->value), 4);
  EXPECT_TRUE(clux_struct_is_method(target, "m"));
  EXPECT_FALSE(clux_struct_is_method(target, "n"))
      << "the method list determines the method mask, not the attribute table";
  EXPECT_EQ(clux_struct_subtype_count(target), 1u);
  ASSERT_NE(clux_struct_find_subtype(target, "s"), nullptr);
  EXPECT_EQ(clux_struct_find_subtype(target, "s")->type, i32_type);
  EXPECT_FALSE(clux_context_install_struct_tables(
      ctx, target, NULL, 0, NULL, 0, NULL, 0))
      << "a second install (even an empty one) must be rejected";
}

/* ---- exec_expr_construct：构造表达式（白皮书 §5.10）---- */

/* 合成 .name = value 具名构造字段（value 结点由调用方构造并持有）。 */
static node_t *make_construct_field_of(allocator_t *allocator,
                                       const char *name_text,
                                       node_t *value_node) {
  node_t *node = create_construct_field_node(allocator);
  construct_field_node_set_name(
      node,
      make_static_node_of(allocator, CLUX_NODE_LITERAL_IDENTIFIER, name_text));
  construct_field_node_set_value(node, value_node);
  return node;
}

/* 合成 .name = <缺 value> 容错构造字段（覆盖 value 槽缺失分支）。 */
static node_t *make_construct_field_without_value(allocator_t *allocator,
                                                  const char *name_text) {
  node_t *node = create_construct_field_node(allocator);
  construct_field_node_set_name(
      node,
      make_static_node_of(allocator, CLUX_NODE_LITERAL_IDENTIFIER, name_text));
  return node;
}

/* 合成 ...<expr> spread 展开元素（value_node 为 NULL 时构造缺 expr 容错
   路径）。 */
static node_t *make_spread_elem_of(allocator_t *allocator, node_t *value_node) {
  node_t *node = create_rest_node(allocator);
  if (value_node) {
    rest_node_set_expr(node, value_node);
  }
  return node;
}

/* 合成 .<type?>{ ... } 构造表达式（type_node 为 NULL 时是匿名构造），
   元素由调用方经 construct_node_add_elem 追加。 */
static node_t *make_construct_of(allocator_t *allocator, node_t *type_node) {
  node_t *node = create_construct_node(allocator);
  if (type_node) {
    construct_node_set_type(node, type_node);
  }
  return node;
}

/* 场景: .Point{ .x = 1, .y = 2 } 穷尽构造 + spread 合并 + 全错误分支
   预期: 快乐路径字段读回 x=1/y=2；缺字段报 missing field 'y'；未知字段
         报 has no field 'z'；重复字段报 duplicate field 'x'；混用报
         cannot be mixed；.i32{ 1 } 报 does not support construction；
         spread 源/字段名不合规与缺 value 均报语义化 error；字段值 error
         原样传播；type 槽求值失败（undeclared/非 type）原样传播 */
TEST_F(ExecLiteralTest, ConstructStructExhaustiveFieldChecks) {
  node_t *members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "x"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "y"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl = make_struct_decl_of(a, "Point", NULL, members);
  clux_value_t *declared = exec_expr_struct_decl(ctx, decl);
  ASSERT_FALSE(clux_value_is_error(declared));

  node_t *ok = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
  construct_node_add_elem(
      ok,
      make_construct_field_of(
          a, "x", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
  construct_node_add_elem(
      ok,
      make_construct_field_of(
          a, "y", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
  clux_value_t *point = exec_expr_construct(ctx, ok);
  ASSERT_NE(point, nullptr);
  EXPECT_FALSE(clux_value_is_error(point)) << error_message_of(point);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(point)), CLUX_TYPE_STRUCT);
  clux_value_t *x = clux_context_field(ctx, point, "x", NULL);
  clux_value_t *y = clux_context_field(ctx, point, "y", NULL);
  ASSERT_FALSE(clux_value_is_error(x));
  ASSERT_FALSE(clux_value_is_error(y));
  EXPECT_EQ(read_i32(x), 1);
  EXPECT_EQ(read_i32(y), 2);

  /* spread 快乐路径：Vec2{ x, y } 展开补 x/y，显式 .z 补第三字段，
     spread 展开的字段计入穷尽覆盖表。 */
  node_t *vec2_members = create_struct_members_node(a);
  struct_members_node_add_member(
      vec2_members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "x"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  struct_members_node_add_member(
      vec2_members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "y"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *vec2_decl = make_struct_decl_of(a, "Vec2", NULL, vec2_members);
  ASSERT_FALSE(clux_value_is_error(exec_expr_struct_decl(ctx, vec2_decl)));

  node_t *point3_members = create_struct_members_node(a);
  struct_members_node_add_member(
      point3_members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "x"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  struct_members_node_add_member(
      point3_members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "y"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  struct_members_node_add_member(
      point3_members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "z"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *point3_decl = make_struct_decl_of(a, "Point3", NULL, point3_members);
  ASSERT_FALSE(clux_value_is_error(exec_expr_struct_decl(ctx, point3_decl)));

  node_t *vec2 = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Vec2"));
  construct_node_add_elem(
      vec2,
      make_construct_field_of(
          a, "x", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
  construct_node_add_elem(
      vec2,
      make_construct_field_of(
          a, "y", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
  node_t *spread_ctor = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point3"));
  construct_node_add_elem(spread_ctor, make_spread_elem_of(a, vec2));
  construct_node_add_elem(
      spread_ctor,
      make_construct_field_of(
          a, "z", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "3")));
  clux_value_t *spread_point = exec_expr_construct(ctx, spread_ctor);
  ASSERT_NE(spread_point, nullptr);
  ASSERT_FALSE(clux_value_is_error(spread_point))
      << error_message_of(spread_point);
  clux_value_t *sx = clux_context_field(ctx, spread_point, "x", NULL);
  clux_value_t *sy = clux_context_field(ctx, spread_point, "y", NULL);
  clux_value_t *sz = clux_context_field(ctx, spread_point, "z", NULL);
  ASSERT_FALSE(clux_value_is_error(sx));
  EXPECT_EQ(read_i32(sx), 1) << "the spread source must fill field 'x'";
  EXPECT_EQ(read_i32(sy), 2) << "the spread source must fill field 'y'";
  EXPECT_EQ(read_i32(sz), 3) << "the explicit field must fill field 'z'";

  /* spread 合并的后写覆盖语义（白皮书 §5.10）：spread 展开后显式同名字段
     覆盖重写——覆盖是合法构造，两个显式同名字段才是 duplicate。 */
  /* override 用独立源树：vec2 已挂接进 spread_ctor，同一子树再挂第二个父
     会在两根 delete_node 级联时 double free，这里重建一份等价的 Vec2 源。 */
  node_t *vec2_override = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Vec2"));
  construct_node_add_elem(
      vec2_override,
      make_construct_field_of(
          a, "x", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
  construct_node_add_elem(
      vec2_override,
      make_construct_field_of(
          a, "y", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
  node_t *override_ctor = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point3"));
  construct_node_add_elem(override_ctor,
                          make_spread_elem_of(a, vec2_override));
  construct_node_add_elem(
      override_ctor,
      make_construct_field_of(
          a, "x", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "9")));
  construct_node_add_elem(
      override_ctor,
      make_construct_field_of(
          a, "z", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "3")));
  clux_value_t *overridden = exec_expr_construct(ctx, override_ctor);
  ASSERT_NE(overridden, nullptr);
  ASSERT_FALSE(clux_value_is_error(overridden)) << error_message_of(overridden);
  clux_value_t *ox = clux_context_field(ctx, overridden, "x", NULL);
  clux_value_t *oy = clux_context_field(ctx, overridden, "y", NULL);
  ASSERT_FALSE(clux_value_is_error(ox));
  ASSERT_FALSE(clux_value_is_error(oy));
  EXPECT_EQ(read_i32(ox), 9) << "the explicit field must override the spread";
  EXPECT_EQ(read_i32(oy), 2) << "the non-overridden spread field survives";

  /* spread 错误分支用例所需的源值：SpUser{ age = 1 }（其字段 age 不在
     Point3 字段表中）。 */
  node_t *sp_user_members = create_struct_members_node(a);
  struct_members_node_add_member(
      sp_user_members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "age"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *sp_user_decl =
      make_struct_decl_of(a, "SpUser", NULL, sp_user_members);
  ASSERT_FALSE(
      clux_value_is_error(exec_expr_struct_decl(ctx, sp_user_decl)));
  node_t *sp_user = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "SpUser"));
  construct_node_add_elem(
      sp_user,
      make_construct_field_of(
          a, "age", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));

  struct {
    const char *name;
    node_t *ctor;
    const char *message;
  } cases[] = {
      {"missing field",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "x",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
         return n;
       }(),
       "missing field 'y' in construction of 'Point'"},
      {"unknown field",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "z",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "x",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "y",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
         return n;
       }(),
       "type 'Point' has no field 'z'"},
      {"duplicate field",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "x",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "x",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "y",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "3")));
         return n;
       }(),
       "duplicate field 'x' in construction of 'Point'"},
      {"mixed elements",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "x",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "9"));
         return n;
       }(),
       "named and positional elements cannot be mixed in construction"},
      {"scalar construction",
       make_construct_of(
           a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")),
       "type 'i32' does not support construction"},
      {"spread with an unknown target field",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point3"));
         construct_node_add_elem(n, make_spread_elem_of(a, sp_user));
         return n;
       }(),
       "type 'Point3' has no field 'age'"},
      {"spread of a non-struct value",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
         construct_node_add_elem(
             n,
             make_spread_elem_of(
                 a, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
         return n;
       }(),
       "spread of non-struct type 'i32' is not supported in construction"},
      {"spread without a value",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
         construct_node_add_elem(n, make_spread_elem_of(a, NULL));
         return n;
       }(),
       "spread element requires a value"},
      {"construct field without a value",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
         construct_node_add_elem(n, make_construct_field_without_value(a, "x"));
         return n;
       }(),
       "construction field requires a name and a value"},
      {"field value error propagation",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "x",
                 make_static_node_of(
                     a, CLUX_NODE_LITERAL_IDENTIFIER, "missing_value")));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "y",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
         return n;
       }(),
       "use of undeclared identifier 'missing_value'"},
      {"undeclared type slot",
       [&]() {
         node_t *n = make_construct_of(
             a,
             make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "GhostType"));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         return n;
       }(),
       "use of undeclared identifier 'GhostType'"},
      {"non-type type slot",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         return n;
       }(),
       "expected a type expression, got 'i32'"},
  };
  for (const auto &c : cases) {
    clux_value_t *value = exec_expr_construct(ctx, c.ctor);
    ASSERT_NE(value, nullptr) << c.name;
    EXPECT_TRUE(clux_value_is_error(value)) << c.name;
    EXPECT_STREQ(error_message_of(value), c.message) << c.name;
  }
  /* 独立 AST 根逐个释放：elems/type/成员子树经 set/add 挂接随根级联；
     vec2/vec2_override 分别挂接进 spread_ctor/override_ctor，sp_user 挂进
     cases 元素，均随其所属根释放，无 double free。 */
  delete_node(&decl);
  delete_node(&vec2_decl);
  delete_node(&point3_decl);
  delete_node(&sp_user_decl);
  delete_node(&ok);
  delete_node(&spread_ctor);
  delete_node(&override_ctor);
  for (auto &c : cases) {
    delete_node(&c.ctor);
  }
}

/* 场景: .Shape{ .circle = 5 } tag union / cunion 单成员构造 + 全错误分支
   预期: 成员经 union_set_member 激活后写入（is_member 回报 true 且值可读）；
         双字段、位置元素、零成员、未知成员报语义化 error；cunion 共享块
         直写同构；字段值 error 原样传播；缺 value 报语义化 error */
TEST_F(ExecLiteralTest, ConstructUnionAndCunionSingleMember) {
  node_t *fields = create_fields_node(a);
  fields_node_add_field(fields, make_union_field_of(a, "circle", "i32"));
  fields_node_add_field(fields, make_union_field_of(a, "rect", "i32"));
  node_t *decl = make_union_decl_of(a, NULL, "Shape", NULL, fields);
  clux_value_t *declared = exec_expr_union(ctx, decl);
  ASSERT_FALSE(clux_value_is_error(declared));

  node_t *ok = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Shape"));
  construct_node_add_elem(
      ok,
      make_construct_field_of(
          a, "circle", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "5")));
  clux_value_t *shape = exec_expr_construct(ctx, ok);
  ASSERT_NE(shape, nullptr);
  EXPECT_FALSE(clux_value_is_error(shape)) << error_message_of(shape);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(shape)), CLUX_TYPE_UNION);
  clux_value_t *is_circle = clux_context_is_member(ctx, shape, "circle", NULL);
  ASSERT_FALSE(clux_value_is_error(is_circle));
  EXPECT_TRUE(*(bool *)clux_value_get_raw(is_circle, 0));
  clux_value_t *circle = clux_context_field(ctx, shape, "circle", NULL);
  ASSERT_FALSE(clux_value_is_error(circle));
  EXPECT_EQ(read_i32(circle), 5);

  node_t *two = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Shape"));
  construct_node_add_elem(
      two,
      make_construct_field_of(
          a, "circle", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
  construct_node_add_elem(
      two,
      make_construct_field_of(
          a, "rect", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
  clux_value_t *two_err = exec_expr_construct(ctx, two);
  ASSERT_TRUE(clux_value_is_error(two_err));
  EXPECT_STREQ(error_message_of(two_err),
               "tag union construction takes exactly one named member");

  node_t *unknown = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Shape"));
  construct_node_add_elem(
      unknown,
      make_construct_field_of(
          a, "oval", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
  clux_value_t *unknown_err = exec_expr_construct(ctx, unknown);
  ASSERT_TRUE(clux_value_is_error(unknown_err));
  EXPECT_STREQ(error_message_of(unknown_err),
               "tag union 'Shape' has no member 'oval'");

  node_t *cfields = create_fields_node(a);
  fields_node_add_field(cfields, make_union_field_of(a, "a", "i32"));
  fields_node_add_field(cfields, make_union_field_of(a, "b", "i32"));
  node_t *cdecl = make_union_decl_of(a, "cunion", "Pair", NULL, cfields);
  clux_value_t *cdeclared = exec_expr_union(ctx, cdecl);
  ASSERT_FALSE(clux_value_is_error(cdeclared));

  node_t *cok = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Pair"));
  construct_node_add_elem(
      cok,
      make_construct_field_of(
          a, "b", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "7")));
  clux_value_t *pair = exec_expr_construct(ctx, cok);
  ASSERT_NE(pair, nullptr);
  EXPECT_FALSE(clux_value_is_error(pair)) << error_message_of(pair);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(pair)), CLUX_TYPE_CUNION);
  clux_value_t *b = clux_context_field(ctx, pair, "b", NULL);
  ASSERT_FALSE(clux_value_is_error(b));
  EXPECT_EQ(read_i32(b), 7);

  struct {
    const char *name;
    node_t *ctor;
    const char *message;
  } cases[] = {
      {"union positional element",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Shape"));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         return n;
       }(),
       "tag union construction takes exactly one named member"},
      {"union with no member",
       [&]() {
         return make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Shape"));
       }(),
       "tag union construction takes one named member"},
      {"union field without a value",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Shape"));
         construct_node_add_elem(
             n, make_construct_field_without_value(a, "circle"));
         return n;
       }(),
       "construction field requires a name and a value"},
      {"union field value error propagation",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Shape"));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "circle",
                 make_static_node_of(
                     a, CLUX_NODE_LITERAL_IDENTIFIER, "missing_union_value")));
         return n;
       }(),
       "use of undeclared identifier 'missing_union_value'"},
      {"cunion with two members",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Pair"));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "a",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "b",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
         return n;
       }(),
       "cunion construction takes exactly one named member"},
      {"cunion with an unknown member",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Pair"));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "zzz",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
         return n;
       }(),
       "cunion 'Pair' has no member 'zzz'"},
  };
  for (const auto &c : cases) {
    clux_value_t *value = exec_expr_construct(ctx, c.ctor);
    ASSERT_NE(value, nullptr) << c.name;
    EXPECT_TRUE(clux_value_is_error(value)) << c.name;
    EXPECT_STREQ(error_message_of(value), c.message) << c.name;
  }
  /* 独立 AST 根释放：decl/cdecl 为声明根，字段/元素子树已挂接级联。 */
  delete_node(&decl);
  delete_node(&ok);
  delete_node(&two);
  delete_node(&unknown);
  delete_node(&cdecl);
  delete_node(&cok);
  for (auto &c : cases) {
    delete_node(&c.ctor);
  }
}

/* 场景: .[2]i32{ 1, 2 } 数组 / 具名 <i32, i32>{ 4, 5 } / 匿名 .{ 1, 2 }
   预期: 数组与具名/匿名 tuple 逐位写入后下标读回；元素不足/溢出报
         requires N elements, got M；位置构造中出现具名字段报混用、
         出现 spread 报 not supported yet */
TEST_F(ExecLiteralTest, ConstructArrayAndAnonymousTuplePositional) {
  node_t *array_ok = make_construct_of(
      a,
      make_binary_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2"),
          make_static_node_of(a, CLUX_NODE_SYMBOL, "[]"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  construct_node_add_elem(
      array_ok, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
  construct_node_add_elem(
      array_ok, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2"));
  clux_value_t *arr = exec_expr_construct(ctx, array_ok);
  ASSERT_NE(arr, nullptr);
  EXPECT_FALSE(clux_value_is_error(arr)) << error_message_of(arr);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(arr)), CLUX_TYPE_ARRAY);
  clux_value_t *idx1 = clux_context_create_i32(ctx, 1, NULL, NULL);
  clux_value_t *second = clux_context_index(ctx, arr, idx1, NULL);
  ASSERT_FALSE(clux_value_is_error(second));
  EXPECT_EQ(read_i32(second), 2);

  node_t *array_short = make_construct_of(
      a,
      make_binary_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2"),
          make_static_node_of(a, CLUX_NODE_SYMBOL, "[]"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  construct_node_add_elem(
      array_short, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
  clux_value_t *short_err = exec_expr_construct(ctx, array_short);
  ASSERT_TRUE(clux_value_is_error(short_err));
  EXPECT_STREQ(error_message_of(short_err),
               "array construction requires 2 elements, got 1");

  node_t *anon = make_construct_of(a, NULL);
  construct_node_add_elem(
      anon, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
  construct_node_add_elem(
      anon, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2"));
  clux_value_t *tuple = exec_expr_construct(ctx, anon);
  ASSERT_NE(tuple, nullptr);
  EXPECT_FALSE(clux_value_is_error(tuple)) << error_message_of(tuple);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(tuple)), CLUX_TYPE_TUPLE);
  clux_value_t *idx0 = clux_context_create_i32(ctx, 0, NULL, NULL);
  clux_value_t *first = clux_context_index(ctx, tuple, idx0, NULL);
  ASSERT_FALSE(clux_value_is_error(first));
  EXPECT_EQ(read_i32(first), 1);

  /* 具名 tuple 构造 <i32, i32>{ 4, 5 }：type 槽是 tuple 类型表达式。 */
  node_t *tuple_type = create_tuple_node(a);
  tuple_node_add_elem(
      tuple_type, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  tuple_node_add_elem(
      tuple_type, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
  node_t *named_tuple = make_construct_of(a, tuple_type);
  construct_node_add_elem(
      named_tuple, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "4"));
  construct_node_add_elem(
      named_tuple, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "5"));
  clux_value_t *named = exec_expr_construct(ctx, named_tuple);
  ASSERT_NE(named, nullptr);
  EXPECT_FALSE(clux_value_is_error(named)) << error_message_of(named);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(named)), CLUX_TYPE_TUPLE);
  clux_value_t *named_first = clux_context_index(ctx, named, idx0, NULL);
  clux_value_t *named_second = clux_context_index(ctx, named, idx1, NULL);
  ASSERT_FALSE(clux_value_is_error(named_first));
  ASSERT_FALSE(clux_value_is_error(named_second));
  EXPECT_EQ(read_i32(named_first), 4);
  EXPECT_EQ(read_i32(named_second), 5);

  struct {
    const char *name;
    node_t *ctor;
    const char *message;
  } cases[] = {
      {"array with extra elements",
       [&]() {
         node_t *n = make_construct_of(
             a,
             make_binary_of(
                 a,
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2"),
                 make_static_node_of(a, CLUX_NODE_SYMBOL, "[]"),
                 make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2"));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "3"));
         return n;
       }(),
       "array construction requires 2 elements, got 3"},
      {"array with a named field",
       [&]() {
         node_t *n = make_construct_of(
             a,
             make_binary_of(
                 a,
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2"),
                 make_static_node_of(a, CLUX_NODE_SYMBOL, "[]"),
                 make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "x",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
         return n;
       }(),
       "named and positional elements cannot be mixed in construction"},
      {"array with a spread element",
       [&]() {
         node_t *inner = make_construct_of(
             a,
             make_binary_of(
                 a,
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"),
                 make_static_node_of(a, CLUX_NODE_SYMBOL, "[]"),
                 make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
         construct_node_add_elem(
             inner, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         node_t *n = make_construct_of(
             a,
             make_binary_of(
                 a,
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2"),
                 make_static_node_of(a, CLUX_NODE_SYMBOL, "[]"),
                 make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         construct_node_add_elem(n, make_spread_elem_of(a, inner));
         return n;
       }(),
       "array construction does not support spread elements yet"},
      {"named tuple with extra elements",
       [&]() {
         node_t *tt = create_tuple_node(a);
         tuple_node_add_elem(
             tt, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
         tuple_node_add_elem(
             tt, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
         node_t *n = make_construct_of(a, tt);
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2"));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "3"));
         return n;
       }(),
       "tuple construction requires 2 elements, got 3"},
      {"named tuple with a spread element",
       [&]() {
         node_t *tt = create_tuple_node(a);
         tuple_node_add_elem(
             tt, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
         tuple_node_add_elem(
             tt, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
         node_t *n = make_construct_of(a, tt);
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         construct_node_add_elem(
             n,
             make_spread_elem_of(
                 a, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
         return n;
       }(),
       "tuple construction does not support spread elements yet"},
  };
  for (const auto &c : cases) {
    clux_value_t *value = exec_expr_construct(ctx, c.ctor);
    ASSERT_NE(value, nullptr) << c.name;
    EXPECT_TRUE(clux_value_is_error(value)) << c.name;
    EXPECT_STREQ(error_message_of(value), c.message) << c.name;
  }
  /* 独立 AST 根释放：tuple_type 经 construct_node_set_type 挂为 type 槽，
     随 named_tuple 级联释放；cases 内的 inner/tt 均挂在各自 ctor 根上。 */
  delete_node(&array_ok);
  delete_node(&array_short);
  delete_node(&anon);
  delete_node(&named_tuple);
  for (auto &c : cases) {
    delete_node(&c.ctor);
  }
}

/* 场景: 匿名 .{ .age = 1, ...inner } 产出匿名 struct + spread 按名合并
   （后写覆盖先展开）+ safe_cast 鸭子类型转换到接收方 User + 匿名全分支
   预期: 匿名构造先独立产出匿名 struct 值；字段表一致时 safe_cast 吸收为
         User 类型；spread 展开的同名字段被后写覆盖；连续两个 spread 的
         同名字段后者覆盖前者；显式重名报 duplicate；混用/spread 非法 /
         缺 value 报语义化 error；匿名空构造产出空 tuple */
TEST_F(ExecLiteralTest, ConstructAnonymousStructSpreadAndDuckCast) {
  node_t *members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "age"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *decl = make_struct_decl_of(a, "User", NULL, members);
  clux_value_t *declared = exec_expr_struct_decl(ctx, decl);
  ASSERT_FALSE(clux_value_is_error(declared));
  clux_type_t *user_type = type_described_by(declared);
  ASSERT_NE(user_type, nullptr);

  node_t *anon = make_construct_of(a, NULL);
  construct_node_add_elem(
      anon,
      make_construct_field_of(
          a, "age", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
  clux_value_t *anon_user = exec_expr_construct(ctx, anon);
  ASSERT_NE(anon_user, nullptr);
  EXPECT_FALSE(clux_value_is_error(anon_user)) << error_message_of(anon_user);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(anon_user)),
            CLUX_TYPE_STRUCT);
  clux_value_t *ducked =
      clux_context_safe_cast(ctx, anon_user, user_type, NULL);
  ASSERT_FALSE(clux_value_is_error(ducked)) << error_message_of(ducked);
  EXPECT_EQ(clux_value_get_type(ducked), user_type);
  clux_value_t *ducked_age = clux_context_field(ctx, ducked, "age", NULL);
  ASSERT_FALSE(clux_value_is_error(ducked_age));
  EXPECT_EQ(read_i32(ducked_age), 1)
      << "the duck-cast view must carry the constructed field value";

  /* spread：...(.User{ .age = 3 }) 展开，后写 .age = 9 覆盖。 */
  node_t *inner = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "User"));
  construct_node_add_elem(
      inner,
      make_construct_field_of(
          a, "age", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "3")));
  node_t *spread = make_construct_of(a, NULL);
  node_t *rest = create_rest_node(a);
  rest_node_set_expr(rest, inner);
  construct_node_add_elem(spread, rest);
  construct_node_add_elem(
      spread,
      make_construct_field_of(
          a, "age", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "9")));
  clux_value_t *merged = exec_expr_construct(ctx, spread);
  ASSERT_NE(merged, nullptr);
  EXPECT_FALSE(clux_value_is_error(merged)) << error_message_of(merged);
  clux_value_t *age = clux_context_field(ctx, merged, "age", NULL);
  ASSERT_FALSE(clux_value_is_error(age));
  EXPECT_EQ(read_i32(age), 9) << "the later explicit field must override the "
                                 "spread one";

  /* 双 spread 同名：后一个 spread 展开的 age 覆盖前一个；显式异名字段
     .other 保证走匿名 struct 分支（纯 spread 无具名字段会落入 tuple 分支）。 */
  node_t *first_src = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "User"));
  construct_node_add_elem(
      first_src,
      make_construct_field_of(
          a, "age", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "4")));
  node_t *second_src = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "User"));
  construct_node_add_elem(
      second_src,
      make_construct_field_of(
          a, "age", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "6")));
  node_t *twice = make_construct_of(a, NULL);
  construct_node_add_elem(twice, make_spread_elem_of(a, first_src));
  construct_node_add_elem(twice, make_spread_elem_of(a, second_src));
  construct_node_add_elem(
      twice,
      make_construct_field_of(
          a, "other", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "0")));
  clux_value_t *twice_merged = exec_expr_construct(ctx, twice);
  ASSERT_NE(twice_merged, nullptr);
  EXPECT_FALSE(clux_value_is_error(twice_merged))
      << error_message_of(twice_merged);
  clux_value_t *twice_age = clux_context_field(ctx, twice_merged, "age", NULL);
  ASSERT_FALSE(clux_value_is_error(twice_age));
  EXPECT_EQ(read_i32(twice_age), 6)
      << "the later spread must override the earlier spread field";
  clux_value_t *twice_other =
      clux_context_field(ctx, twice_merged, "other", NULL);
  ASSERT_FALSE(clux_value_is_error(twice_other));
  EXPECT_EQ(read_i32(twice_other), 0);

  /* 匿名空构造 .{ }：位置零元素 → 空 tuple（unit）。 */
  node_t *empty_ctor = make_construct_of(a, NULL);
  clux_value_t *empty = exec_expr_construct(ctx, empty_ctor);
  ASSERT_NE(empty, nullptr);
  EXPECT_FALSE(clux_value_is_error(empty)) << error_message_of(empty);
  EXPECT_EQ(clux_type_get_kind(clux_value_get_type(empty)), CLUX_TYPE_TUPLE);

  node_t *dup = make_construct_of(a, NULL);
  construct_node_add_elem(
      dup,
      make_construct_field_of(
          a, "age", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
  construct_node_add_elem(
      dup,
      make_construct_field_of(
          a, "age", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
  clux_value_t *dup_err = exec_expr_construct(ctx, dup);
  ASSERT_TRUE(clux_value_is_error(dup_err));
  EXPECT_STREQ(error_message_of(dup_err),
               "duplicate field 'age' in construction");

  struct {
    const char *name;
    node_t *ctor;
    const char *message;
  } cases[] = {
      {"anonymous mixed elements",
       [&]() {
         node_t *n = make_construct_of(a, NULL);
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "age",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2"));
         return n;
       }(),
       "named and positional elements cannot be mixed in construction"},
      {"anonymous spread of a non-struct value",
       [&]() {
         node_t *n = make_construct_of(a, NULL);
         /* 显式具名字段保证落入匿名 struct 分支（纯 spread 会进入 tuple 分支） */
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "other",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "0")));
         construct_node_add_elem(
             n,
             make_spread_elem_of(
                 a, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
         return n;
       }(),
       "spread of non-struct type 'i32' is not supported in construction"},
      {"anonymous spread without a value",
       [&]() {
         node_t *n = make_construct_of(a, NULL);
         /* 显式具名字段保证落入匿名 struct 分支（纯 spread 会进入 tuple 分支） */
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "other",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "0")));
         construct_node_add_elem(n, make_spread_elem_of(a, NULL));
         return n;
       }(),
       "spread element requires a value"},
      {"anonymous field without a value",
       [&]() {
         node_t *n = make_construct_of(a, NULL);
         construct_node_add_elem(n,
                                 make_construct_field_without_value(a, "age"));
         return n;
       }(),
       "construction field requires a name and a value"},
      {"anonymous positional mixed with a spread element",
       [&]() {
         node_t *n = make_construct_of(a, NULL);
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         construct_node_add_elem(
             n,
             make_spread_elem_of(
                 a, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
         return n;
       }(),
       /* spread 产出具名字段集（具名语义），与位置元素混用即报 mixed。 */
       "named and positional elements cannot be mixed in construction"},
  };
  for (const auto &c : cases) {
    clux_value_t *value = exec_expr_construct(ctx, c.ctor);
    ASSERT_NE(value, nullptr) << c.name;
    EXPECT_TRUE(clux_value_is_error(value)) << c.name;
    EXPECT_STREQ(error_message_of(value), c.message) << c.name;
  }
  /* 独立 AST 根释放：inner 经 rest 挂进 spread，first_src/second_src 挂进
     twice，均随所属根级联；anon/spread/twice 独立成根。 */
  delete_node(&decl);
  delete_node(&anon);
  delete_node(&spread);
  delete_node(&twice);
  delete_node(&empty_ctor);
  delete_node(&dup);
  for (auto &c : cases) {
    delete_node(&c.ctor);
  }
}

/* 覆盖率补漏（Step 3）：未覆盖的 error 传播分支与 NULL 元素槽容忍路径。
   - construct_write_view 的 safe_cast 失败（字段类型不匹配）
   - construct_spread_struct 的求值 error / spread 字段类型不匹配
   - construct_anonymous 的 spread/字段求值 error、tuple 缺元素与求值 error
   - construct_positional_fill 的求值 error（数组与具名 tuple）
   - 各构造分支对 NULL 元素槽的跳过（node_add_child 允许空槽） */
TEST_F(ExecLiteralTest, ConstructErrorPropagationAndNullElemTolerance) {
  /* 声明 Point{ x: i32, y: i32 } 与 SrcS{ x: str }（spread 类型不匹配用） */
  node_t *members = create_struct_members_node(a);
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "x"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  struct_members_node_add_member(
      members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "y"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
  node_t *point_decl = make_struct_decl_of(a, "Point", NULL, members);
  ASSERT_FALSE(
      clux_value_is_error(exec_expr_struct_decl(ctx, point_decl)));

  node_t *src_members = create_struct_members_node(a);
  struct_members_node_add_member(
      src_members,
      make_field_member_of(
          a,
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "x"),
          make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "f64")));
  node_t *src_decl = make_struct_decl_of(a, "SrcS", NULL, src_members);
  ASSERT_FALSE(clux_value_is_error(exec_expr_struct_decl(ctx, src_decl)));

  node_t *src_value = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "SrcS"));
  construct_node_add_elem(
      src_value,
      make_construct_field_of(
          a, "x", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1.5f64")));

  /* NULL 元素槽应被跳过，构造仍然成功穷尽 */
  node_t *with_null = make_construct_of(
      a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
  construct_node_add_elem(with_null, NULL);
  construct_node_add_elem(
      with_null,
      make_construct_field_of(
          a, "x", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
  construct_node_add_elem(
      with_null,
      make_construct_field_of(
          a, "y", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
  clux_value_t *null_ok = exec_expr_construct(ctx, with_null);
  ASSERT_NE(null_ok, nullptr);
  ASSERT_FALSE(clux_value_is_error(null_ok)) << error_message_of(null_ok);
  clux_value_t *nx = clux_context_field(ctx, null_ok, "x", NULL);
  clux_value_t *ny = clux_context_field(ctx, null_ok, "y", NULL);
  ASSERT_FALSE(clux_value_is_error(nx));
  ASSERT_FALSE(clux_value_is_error(ny));
  EXPECT_EQ(read_i32(nx), 1);
  EXPECT_EQ(read_i32(ny), 2);

  /* 匿名 struct 同样跳过 NULL 元素槽 */
  node_t *anon_null = make_construct_of(a, NULL);
  construct_node_add_elem(anon_null, NULL);
  construct_node_add_elem(
      anon_null,
      make_construct_field_of(
          a, "age", make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "7")));
  clux_value_t *anon_null_ok = exec_expr_construct(ctx, anon_null);
  ASSERT_NE(anon_null_ok, nullptr);
  ASSERT_FALSE(clux_value_is_error(anon_null_ok))
      << error_message_of(anon_null_ok);
  clux_value_t *anon_age = clux_context_field(ctx, anon_null_ok, "age", NULL);
  ASSERT_FALSE(clux_value_is_error(anon_age));
  EXPECT_EQ(read_i32(anon_age), 7);

  struct {
    const char *name;
    node_t *ctor;
    const char *message;
  } cases[] = {
      {"named struct field type mismatch",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "x",
                 make_static_node_of(a, CLUX_NODE_LITERAL_STRING, "\"hi\"")));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "y",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
         return n;
       }(),
       "type 'str' cannot be safely cast to 'i32'"},
      {"named struct spread operand evaluation error",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
         construct_node_add_elem(
             n,
             make_spread_elem_of(
                 a,
                 make_static_node_of(
                     a, CLUX_NODE_LITERAL_IDENTIFIER, "missing_spread")));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "x",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1")));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "y",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
         return n;
       }(),
       "use of undeclared identifier 'missing_spread'"},
      {"named struct spread field type mismatch",
       [&]() {
         node_t *n = make_construct_of(
             a, make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "Point"));
         construct_node_add_elem(n, make_spread_elem_of(a, src_value));
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "y",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2")));
         return n;
       }(),
       "cannot safely cast 'f64' to 'i32'"},
      {"anonymous named field evaluation error",
       [&]() {
         node_t *n = make_construct_of(a, NULL);
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "age",
                 make_static_node_of(
                     a, CLUX_NODE_LITERAL_IDENTIFIER, "missing_value")));
         return n;
       }(),
       "use of undeclared identifier 'missing_value'"},
      {"anonymous spread evaluation error",
       [&]() {
         node_t *n = make_construct_of(a, NULL);
         construct_node_add_elem(
             n,
             make_construct_field_of(
                 a,
                 "other",
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "0")));
         construct_node_add_elem(
             n,
             make_spread_elem_of(
                 a,
                 make_static_node_of(
                     a, CLUX_NODE_LITERAL_IDENTIFIER, "missing_spread")));
         return n;
       }(),
       "use of undeclared identifier 'missing_spread'"},
      {"anonymous tuple with a null element slot",
       [&]() {
         node_t *n = make_construct_of(a, NULL);
         construct_node_add_elem(n, NULL);
         return n;
       }(),
       "tuple construction element 0 is missing"},
      {"anonymous tuple element evaluation error",
       [&]() {
         node_t *n = make_construct_of(a, NULL);
         construct_node_add_elem(
             n,
             make_static_node_of(
                 a, CLUX_NODE_LITERAL_IDENTIFIER, "missing_value"));
         return n;
       }(),
       "use of undeclared identifier 'missing_value'"},
      {"array element evaluation error",
       [&]() {
         node_t *n = make_construct_of(
             a,
             make_binary_of(
                 a,
                 make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "2"),
                 make_static_node_of(a, CLUX_NODE_SYMBOL, "[]"),
                 make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32")));
         construct_node_add_elem(
             n,
             make_static_node_of(
                 a, CLUX_NODE_LITERAL_IDENTIFIER, "missing_value"));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         return n;
       }(),
       "use of undeclared identifier 'missing_value'"},
      {"named tuple element evaluation error",
       [&]() {
         node_t *tuple_type = create_tuple_node(a);
         tuple_node_add_elem(
             tuple_type,
             make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
         tuple_node_add_elem(
             tuple_type,
             make_static_node_of(a, CLUX_NODE_LITERAL_IDENTIFIER, "i32"));
         node_t *n = make_construct_of(a, tuple_type);
         construct_node_add_elem(
             n,
             make_static_node_of(
                 a, CLUX_NODE_LITERAL_IDENTIFIER, "missing_value"));
         construct_node_add_elem(
             n, make_static_node_of(a, CLUX_NODE_LITERAL_NUMERIC, "1"));
         return n;
       }(),
       "use of undeclared identifier 'missing_value'"},
  };
  for (const auto &c : cases) {
    clux_value_t *value = exec_expr_construct(ctx, c.ctor);
    ASSERT_NE(value, nullptr) << c.name;
    EXPECT_TRUE(clux_value_is_error(value)) << c.name;
    EXPECT_STREQ(error_message_of(value), c.message) << c.name;
  }
  /* 独立 AST 根释放：src_value 挂进 cases 中 spread 类型不匹配用例的 ctor
     根，随 cases 循环释放；point_decl/src_decl 为声明根。 */
  delete_node(&point_decl);
  delete_node(&src_decl);
  delete_node(&with_null);
  delete_node(&anon_null);
  for (auto &c : cases) {
    delete_node(&c.ctor);
  }
}
