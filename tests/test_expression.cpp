#include <gtest/gtest.h>
#include "parser/expression.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/parser.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/vec.h"
#include <stdlib.h>
#include <string.h>

/* ---- 辅助 ---- */

static node_t *parse_expr(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_expression(a, *p);
}

/* 断言 assign 节点的三个子节点形状。 */
static void expect_assign(node_t *expr,
                          const char *id_src,
                          const char *symbol_src,
                          node_kind_t value_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_ASSIGN);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_ASSIGN_CHILDREN_SIZE);

  node_t *id = node_get_child(expr, CLUX_EXPR_ASSIGN_ID);
  ASSERT_NE(id, nullptr);
  EXPECT_EQ(node_get_kind(id), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(node_get_location(id)->end.offset -
                node_get_location(id)->begin.offset,
            (ptrdiff_t)strlen(id_src));

  node_t *symbol = node_get_child(expr, CLUX_EXPR_ASSIGN_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_kind(symbol), CLUX_NODE_SYMBOL);
  EXPECT_EQ(node_get_location(symbol)->end.offset -
                node_get_location(symbol)->begin.offset,
            (ptrdiff_t)strlen(symbol_src));

  node_t *value = node_get_child(expr, CLUX_EXPR_ASSIGN_VALUE);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(node_get_kind(value), value_kind);
}

/* ---- 简单赋值 ---- */

TEST(ExpressionAssignTest, SimpleNumericAssign) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "x = 1");
  expect_assign(expr, "x", "=", CLUX_NODE_LITERAL_NUMERIC);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 调用方负责在进入 read 函数前清掉前导 trivia，这里显式模拟该契约。 */
TEST(ExpressionAssignTest, AssignWithSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  x   =   1  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expression(a, p);
  expect_assign(expr, "x", "=", CLUX_NODE_LITERAL_NUMERIC);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 前导 trivia 未清理时，第一个 token 不是 identifier → 回退到 literal 并返回 NULL。 */
TEST(ExpressionAssignTest, LeadingTriviaNotSkippedByReader) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_expr(a, &p, "  x = 1"), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, AssignWithCommentBetweenTokens) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "x /* c */ = /* d */ 1");
  expect_assign(expr, "x", "=", CLUX_NODE_LITERAL_NUMERIC);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 各类值 ---- */

TEST(ExpressionAssignTest, AssignStringValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "s = \"hi\"");
  expect_assign(expr, "s", "=", CLUX_NODE_LITERAL_STRING);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, AssignCharacterValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "c = 'a'");
  expect_assign(expr, "c", "=", CLUX_NODE_LITERAL_CHARACTER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, AssignBooleanValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "b = true");
  expect_assign(expr, "b", "=", CLUX_NODE_LITERAL_BOOLEAN);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, AssignNilValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "n = nil");
  expect_assign(expr, "n", "=", CLUX_NODE_LITERAL_NIL);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, AssignUndefinedValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "u = undefined");
  expect_assign(expr, "u", "=", CLUX_NODE_LITERAL_UNDEFINED);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, AssignIdentifierValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "a = b");
  expect_assign(expr, "a", "=", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, AssignNumericWithSuffix) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "x = 42i32");
  expect_assign(expr, "x", "=", CLUX_NODE_LITERAL_NUMERIC);
  /* 后缀属于数字节点 */
  node_t *value = node_get_child(expr, CLUX_EXPR_ASSIGN_VALUE);
  EXPECT_EQ(node_get_location(value)->end.offset -
                node_get_location(value)->begin.offset,
            5);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 全部复合赋值运算符 ---- */

TEST(ExpressionAssignTest, AllCompoundOperators) {
  const char *ops[] = {"=",
                       "+=",
                       "-=",
                       "*=",
                       "/=",
                       "%=",
                       "&=",
                       "|=",
                       "~=",
                       "^=",
                       "&&=",
                       "||=",
                       ">>=",
                       "<<=",
                       nullptr};
  for (size_t i = 0; ops[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    char src[32];
    snprintf(src, sizeof(src), "x %s 1", ops[i]);
    parser_t *p;
    node_t *expr = parse_expr(a, &p, src);
    ASSERT_NE(expr, nullptr) << "op=" << ops[i];
    EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_ASSIGN)
        << "op=" << ops[i];
    node_t *symbol = node_get_child(expr, CLUX_EXPR_ASSIGN_SYMBOL);
    ASSERT_NE(symbol, nullptr) << "op=" << ops[i];
    EXPECT_EQ(node_get_location(symbol)->end.offset -
                  node_get_location(symbol)->begin.offset,
              (ptrdiff_t)strlen(ops[i]))
        << "op=" << ops[i];
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* ---- 回退：不是赋值表达式时交给 literal ---- */

TEST(ExpressionAssignTest, FallsBackToLiteralForPlainNumeric) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "42");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_NUMERIC);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, FallsBackToLiteralForPlainIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "foo");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, FallsBackWhenIdentifierNotFollowedByAssign) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  /* "a + b"：identifier 后是 '+'，不是赋值运算符 → 回退 */
  node_t *expr = parse_expr(a, &p, "a + b");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  /* 游标停在 identifier 之后，'+' 未被消费 */
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, FallsBackWhenIdentifierFollowedByKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "a var");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, ReturnsNullOnKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_expr(a, &p, "var"), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, ReturnsNullOnEmptyStream) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_expr(a, &p, ""), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 不完整 / 错误输入 ---- */

/* 容错设计：缺右值时仍返回不完整的 assign 节点（可安全释放），
   同时向 parser 推入一条错误。 */
TEST(ExpressionAssignTest, MissingValueReturnsPartialNodeAndError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "x =");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_ASSIGN);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_ASSIGN_ID), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_ASSIGN_SYMBOL), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_ASSIGN_VALUE), nullptr);

  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "expected expression after assignment");
  EXPECT_STREQ(error_get_location(err)->filename, "test.clux");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺右值且后面跟着无法解析为 literal 的 token 时，错误位置指向该 token。 */
TEST(ExpressionAssignTest, MissingValueErrorPointsAtBadToken) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "x = var";
  node_t *expr = parse_expr(a, &p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_ASSIGN_VALUE), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 4);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 正常赋值不产生错误。 */
TEST(ExpressionAssignTest, ValidAssignProducesNoError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "x = 1");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionAssignTest, AssignAtEndOfStream) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "x = 1");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 子节点 parent 未自动挂载（设计如此，见 node 手工维护约定） ---- */

TEST(ExpressionAssignTest, ChildrenParentIsNotAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "x = 1");
  ASSERT_NE(expr, nullptr);
  node_t *id = node_get_child(expr, CLUX_EXPR_ASSIGN_ID);
  ASSERT_NE(id, nullptr);
  EXPECT_EQ(node_get_parent(id), nullptr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

static size_t g_alloc_count;
static size_t g_free_count;
static void *counting_alloc(size_t size) {
  g_alloc_count++;
  return malloc(size);
}
static void counting_free(void *ptr) {
  if (ptr) g_free_count++;
  free(ptr);
}

TEST(ExpressionAssignTest, NoLeakOnAssign) {
  const char *sources[] = {"x = 1",
                           "x = 42i32",
                           "x = \"s\"",
                           "x = 'c'",
                           "x = true",
                           "x = nil",
                           "x = a",
                           "x += 1",
                           "x =",
                           "42",
                           "foo",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expression(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}
