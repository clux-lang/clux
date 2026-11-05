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

/* ================= 三元表达式 ================= */

static node_t *parse_ternary(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_expression_ternary(a, *p);
}

/* 断言三元节点的三个子节点形状。 */
static void expect_ternary(node_t *expr,
                           node_kind_t condition_kind,
                           node_kind_t then_kind,
                           node_kind_t else_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_TERNARY);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_TERNARY_CHILDREN_SIZE);

  node_t *condition = node_get_child(expr, CLUX_EXPR_TERNARY_CONDITION);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(node_get_kind(condition), condition_kind);

  node_t *then = node_get_child(expr, CLUX_EXPR_TERNARY_THEN);
  ASSERT_NE(then, nullptr);
  EXPECT_EQ(node_get_kind(then), then_kind);

  node_t *else_ = node_get_child(expr, CLUX_EXPR_TERNARY_ELSE);
  ASSERT_NE(else_, nullptr);
  EXPECT_EQ(node_get_kind(else_), else_kind);
}

/* 子节点下标必须互不相同（防止宏复制粘贴错误）。 */
TEST(ExpressionTernaryTest, ChildIndexesAreDistinct) {
  EXPECT_NE(CLUX_EXPR_TERNARY_CONDITION, CLUX_EXPR_TERNARY_THEN);
  EXPECT_NE(CLUX_EXPR_TERNARY_CONDITION, CLUX_EXPR_TERNARY_ELSE);
  EXPECT_NE(CLUX_EXPR_TERNARY_THEN, CLUX_EXPR_TERNARY_ELSE);
}

TEST(ExpressionTernaryTest, SimpleTernary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a ? b : c");
  expect_ternary(expr,
                 CLUX_NODE_LITERAL_IDENTIFIER,
                 CLUX_NODE_LITERAL_IDENTIFIER,
                 CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTernaryTest, NumericBranches) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "1 ? 2 : 3");
  expect_ternary(expr,
                 CLUX_NODE_LITERAL_NUMERIC,
                 CLUX_NODE_LITERAL_NUMERIC,
                 CLUX_NODE_LITERAL_NUMERIC);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTernaryTest, MixedLiteralKinds) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "true ? \"s\" : 'c'");
  expect_ternary(expr,
                 CLUX_NODE_LITERAL_BOOLEAN,
                 CLUX_NODE_LITERAL_STRING,
                 CLUX_NODE_LITERAL_CHARACTER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTernaryTest, NilAndUndefinedBranches) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "nil ? undefined : nil");
  expect_ternary(expr,
                 CLUX_NODE_LITERAL_NIL,
                 CLUX_NODE_LITERAL_UNDEFINED,
                 CLUX_NODE_LITERAL_NIL);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 三元是右结合：a ? b : (c ? d : e) */
TEST(ExpressionTernaryTest, NestedInElseBranchIsRightAssociative) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a ? b : c ? d : e");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_TERNARY);
  node_t *outer_else = node_get_child(expr, CLUX_EXPR_TERNARY_ELSE);
  ASSERT_NE(outer_else, nullptr);
  EXPECT_EQ(node_get_kind(outer_else), CLUX_NODE_EXPRESSION_TERNARY);
  expect_ternary(outer_else,
                 CLUX_NODE_LITERAL_IDENTIFIER,
                 CLUX_NODE_LITERAL_IDENTIFIER,
                 CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* then 分支同样是三元表达式：a ? (b ? c : d) : e */
TEST(ExpressionTernaryTest, NestedInThenBranch) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a ? b ? c : d : e");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_TERNARY);
  node_t *then = node_get_child(expr, CLUX_EXPR_TERNARY_THEN);
  ASSERT_NE(then, nullptr);
  EXPECT_EQ(node_get_kind(then), CLUX_NODE_EXPRESSION_TERNARY);
  node_t *outer_else = node_get_child(expr, CLUX_EXPR_TERNARY_ELSE);
  ASSERT_NE(outer_else, nullptr);
  EXPECT_EQ(node_get_kind(outer_else), CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTernaryTest, TriviaAroundOperators) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a  /* c */  ?  b  // d\n :  c");
  expect_ternary(expr,
                 CLUX_NODE_LITERAL_IDENTIFIER,
                 CLUX_NODE_LITERAL_IDENTIFIER,
                 CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 不是三元时原样返回条件，且游标停在条件之后（不吞掉后续 trivia）。 */
TEST(ExpressionTernaryTest, FallsBackToConditionWithoutTernary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTernaryTest, FallbackDoesNotConsumeTrailingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a   ");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  /* token 序列：identifier, whitespace */
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTernaryTest, FallbackWhenFollowedByNonQuestionSymbol) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a : b");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  /* ':' 未被消费，游标停在条件之后 */
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTernaryTest, ReturnsNullOnNonLiteral) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_ternary(a, &p, "?"), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTernaryTest, ReturnsNullOnEmptyStream) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_ternary(a, &p, ""), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 与赋值组合 ---- */

TEST(ExpressionTernaryTest, AssignWithTernaryValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "x = a ? b : c");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_ASSIGN);
  node_t *value = node_get_child(expr, CLUX_EXPR_ASSIGN_VALUE);
  expect_ternary(value,
                 CLUX_NODE_LITERAL_IDENTIFIER,
                 CLUX_NODE_LITERAL_IDENTIFIER,
                 CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTernaryTest, CompoundAssignWithTernaryValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "x += a ? 1 : 2");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_ASSIGN);
  node_t *value = node_get_child(expr, CLUX_EXPR_ASSIGN_VALUE);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(node_get_kind(value), CLUX_NODE_EXPRESSION_TERNARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

/* 缺 then 分支：返回不完整节点，THEN/ELSE 为 NULL，并上报错误。 */
TEST(ExpressionTernaryTest, MissingThenReturnsPartialNodeAndError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ?";
  node_t *expr = parse_ternary(a, &p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_TERNARY);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_TERNARY_CONDITION), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_TERNARY_THEN), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_TERNARY_ELSE), nullptr);

  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "expected expression after '?'");
  EXPECT_STREQ(error_get_location(err)->filename, "test.clux");
  /* EOF：错误位置退化为 '?' 的终点 */
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 3);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺 then 且后面跟着无法解析为 literal 的 token 时，错误位置指向该 token。 */
TEST(ExpressionTernaryTest, MissingThenErrorPointsAtBadToken) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ? : b";
  node_t *expr = parse_ternary(a, &p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_TERNARY_THEN), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 4);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺 ':'：返回带 CONDITION/THEN 的不完整节点。 */
TEST(ExpressionTernaryTest, MissingColonReturnsPartialNodeAndError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ? b";
  node_t *expr = parse_ternary(a, &p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_TERNARY);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_TERNARY_THEN), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_TERNARY_ELSE), nullptr);

  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "expected ':' in ternary expression");
  /* EOF：错误位置退化为 then 的终点 */
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 5);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTernaryTest, MissingColonErrorPointsAtBadToken) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ? b = c";
  node_t *expr = parse_ternary(a, &p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_TERNARY_ELSE), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 6);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺 else 分支。 */
TEST(ExpressionTernaryTest, MissingElseReturnsPartialNodeAndError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ? b :";
  node_t *expr = parse_ternary(a, &p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_TERNARY);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_TERNARY_THEN), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_TERNARY_ELSE), nullptr);

  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "expected expression after ':'");
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 7);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTernaryTest, MissingElseErrorPointsAtBadToken) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ? b : ?";
  node_t *expr = parse_ternary(a, &p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_TERNARY_ELSE), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 8);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 嵌套错误：内层缺 else 只报一条，外层照常完成。 */
TEST(ExpressionTernaryTest, NestedErrorReportsOnce) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a ? b : c ?");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_TERNARY);
  node_t *outer_else = node_get_child(expr, CLUX_EXPR_TERNARY_ELSE);
  ASSERT_NE(outer_else, nullptr);
  EXPECT_EQ(node_get_kind(outer_else), CLUX_NODE_EXPRESSION_TERNARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTernaryTest, ValidTernaryProducesNoError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a ? b : c");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 子节点 parent 未自动挂载 ---- */

TEST(ExpressionTernaryTest, ChildrenParentIsNotAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a ? b : c");
  ASSERT_NE(expr, nullptr);
  node_t *condition = node_get_child(expr, CLUX_EXPR_TERNARY_CONDITION);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(node_get_parent(condition), nullptr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionTernaryTest, NoLeakOnTernary) {
  const char *sources[] = {"a ? b : c",
                           "1 ? 2 : 3",
                           "a ? b : c ? d : e",
                           "a ? b ? c : d : e",
                           "a  ?  b  :  c",
                           "x = a ? b : c",
                           "a ?",
                           "a ? b",
                           "a ? b :",
                           "a",
                           "",
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

/* ================= 表达式自身 location ================= */

/* 表达式区间 = 首个子节点起点 .. 末个子节点终点，不含前后 trivia。
   expected 为 src 中的子串，用 strstr 定位其偏移。 */
static void
expect_location_span(node_t *node, const char *src, const char *expected) {
  const location_t *loc = node_get_location(node);
  ASSERT_NE(loc, nullptr);
  ASSERT_NE(loc->begin.offset, nullptr);
  ASSERT_NE(loc->end.offset, nullptr);
  const char *at = strstr(src, expected);
  ASSERT_NE(at, nullptr) << "expected not found in src";
  EXPECT_EQ(loc->begin.offset, at)
      << "begin: got '" << loc->begin.offset << "'";
  EXPECT_EQ(loc->end.offset, at + strlen(expected))
      << "end: got '" << loc->end.offset << "'";
  EXPECT_STREQ(loc->filename, "test.clux");
}

TEST(ExpressionLocationTest, AssignSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "x = 1";
  node_t *expr = parse_expr(a, &p, src);
  expect_location_span(expr, src, "x = 1");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLocationTest, AssignSpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  x   =   1  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expression(a, p);
  expect_location_span(expr, "  x   =   1  ", "x   =   1");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLocationTest, AssignSpanWithCommentBetweenTokens) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "x /* c */ = /* d */ 1";
  node_t *expr = parse_expr(a, &p, src);
  expect_location_span(expr, src, "x /* c */ = /* d */ 1");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLocationTest, AssignSpanWithTernaryValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "x = a ? b : c";
  node_t *expr = parse_expr(a, &p, src);
  expect_location_span(expr, src, "x = a ? b : c");
  node_t *value = node_get_child(expr, CLUX_EXPR_ASSIGN_VALUE);
  expect_location_span(value, src, "a ? b : c");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺右值：区间退化为 id .. symbol。 */
TEST(ExpressionLocationTest, AssignSpanWhenValueMissing) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "x =";
  node_t *expr = parse_expr(a, &p, src);
  expect_location_span(expr, src, "x =");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLocationTest, TernarySpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ? b : c";
  node_t *expr = parse_ternary(a, &p, src);
  expect_location_span(expr, src, "a ? b : c");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLocationTest, TernarySpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  a  ?  b  :  c  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expression_ternary(a, p);
  expect_location_span(expr, "  a  ?  b  :  c  ", "a  ?  b  :  c");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLocationTest, TernarySpanWithComments) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a /* x */ ? /* y */ b // z\n : /* w */ c";
  node_t *expr = parse_ternary(a, &p, src);
  expect_location_span(expr, src, src);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLocationTest, NestedTernarySpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ? b : c ? d : e";
  node_t *expr = parse_ternary(a, &p, src);
  expect_location_span(expr, src, "a ? b : c ? d : e");
  node_t *inner = node_get_child(expr, CLUX_EXPR_TERNARY_ELSE);
  expect_location_span(inner, src, "c ? d : e");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺 then：区间只覆盖条件。 */
TEST(ExpressionLocationTest, TernarySpanWhenThenMissing) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ?";
  node_t *expr = parse_ternary(a, &p, src);
  expect_location_span(expr, src, "a");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺 ':'：区间覆盖 条件 .. then。 */
TEST(ExpressionLocationTest, TernarySpanWhenColonMissing) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ? b";
  node_t *expr = parse_ternary(a, &p, src);
  expect_location_span(expr, src, "a ? b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺 else：区间覆盖 条件 .. then。 */
TEST(ExpressionLocationTest, TernarySpanWhenElseMissing) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ? b :";
  node_t *expr = parse_ternary(a, &p, src);
  expect_location_span(expr, src, "a ? b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 逻辑或表达式 ================= */

static node_t *parse_lor(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_expression_lor(a, *p);
}

/* 断言二元节点的三个子节点形状。 */
static void expect_binary(node_t *expr,
                          node_kind_t left_kind,
                          const char *symbol_src,
                          node_kind_t right_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_BINARY_CHILDREN_SIZE);

  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(left, nullptr);
  EXPECT_EQ(node_get_kind(left), left_kind);

  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_kind(symbol), CLUX_NODE_SYMBOL);
  EXPECT_EQ(node_get_location(symbol)->end.offset -
                node_get_location(symbol)->begin.offset,
            (ptrdiff_t)strlen(symbol_src));

  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(node_get_kind(right), right_kind);
}

/* 子节点下标必须互不相同（防止宏复制粘贴错误）。 */
TEST(ExpressionLorTest, ChildIndexesAreDistinct) {
  EXPECT_NE(CLUX_EXPR_BINARY_LEFT, CLUX_EXPR_BINARY_SYMBOL);
  EXPECT_NE(CLUX_EXPR_BINARY_LEFT, CLUX_EXPR_BINARY_RIGHT);
  EXPECT_NE(CLUX_EXPR_BINARY_SYMBOL, CLUX_EXPR_BINARY_RIGHT);
}

TEST(ExpressionLorTest, SimpleLor) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a || b");
  expect_binary(
      expr, CLUX_NODE_LITERAL_IDENTIFIER, "||", CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLorTest, BooleanOperands) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "true || false");
  expect_binary(
      expr, CLUX_NODE_LITERAL_BOOLEAN, "||", CLUX_NODE_LITERAL_BOOLEAN);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 右结合：a || b || c  =>  a || (b || c)，与短路求值顺序一致。 */
TEST(ExpressionLorTest, RightAssociativeChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a || b || c");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(left, nullptr);
  EXPECT_EQ(node_get_kind(left), CLUX_NODE_LITERAL_IDENTIFIER);
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(node_get_kind(right), CLUX_NODE_EXPRESSION_BINARY);
  expect_binary(
      right, CLUX_NODE_LITERAL_IDENTIFIER, "||", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLorTest, FourOperandChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a || b || c || d");
  ASSERT_NE(expr, nullptr);
  /* 逐层剥右，应得到 3 层嵌套 */
  node_t *cur = expr;
  int depth = 0;
  while (cur && node_get_kind(cur) == CLUX_NODE_EXPRESSION_BINARY) {
    depth++;
    cur = node_get_child(cur, CLUX_EXPR_BINARY_RIGHT);
  }
  EXPECT_EQ(depth, 3);
  EXPECT_EQ(node_get_kind(cur), CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLorTest, TriviaAroundOperator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a  /* c */  ||  b  // d\n ");
  expect_binary(
      expr, CLUX_NODE_LITERAL_IDENTIFIER, "||", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 不是逻辑或时原样返回左操作数，游标停在左操作数之后。 */
TEST(ExpressionLorTest, FallsBackToLeftWithoutOperator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLorTest, FallbackDoesNotConsumeTrailingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a   ");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* '|' 是位或，不是逻辑或，不应被消费。 */
TEST(ExpressionLorTest, FallsBackOnSinglePipe) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a | b");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLorTest, ReturnsNullOnNonLiteral) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_lor(a, &p, "||"), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLorTest, ReturnsNullOnEmptyStream) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_lor(a, &p, ""), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

/* 缺右操作数：返回不完整节点（RIGHT 为 NULL），并上报错误。 */
TEST(ExpressionLorTest, MissingRightReturnsPartialNodeAndError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ||";
  node_t *expr = parse_lor(a, &p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);

  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "expected expression after '||'");
  EXPECT_STREQ(error_get_location(err)->filename, "test.clux");
  /* EOF：错误位置退化为 '||' 的终点 */
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 4);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺右操作数且后面跟着无法解析为 literal 的 token 时，错误位置指向该 token。 */
TEST(ExpressionLorTest, MissingRightErrorPointsAtBadToken) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a || )";
  node_t *expr = parse_lor(a, &p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 5);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 链式表达式中途缺右操作数：只报一条错误，已完成的左链保留。 */
TEST(ExpressionLorTest, ChainMissingRightReportsOnce) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a || b ||");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  /* 最外层是 a || (b || <缺>) */
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(node_get_kind(right), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_EQ(node_get_child(right, CLUX_EXPR_BINARY_RIGHT), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLorTest, ValidLorProducesNoError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a || b");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 与其他表达式组合 ---- */

/* 三元优先级低于 ||：条件可以是逻辑或表达式。 */
TEST(ExpressionLorTest, LorAsTernaryCondition) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a || b ? c : d");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_TERNARY);
  node_t *condition = node_get_child(expr, CLUX_EXPR_TERNARY_CONDITION);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(node_get_kind(condition), CLUX_NODE_EXPRESSION_BINARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 三元分支也可以是逻辑或表达式。 */
TEST(ExpressionLorTest, LorInTernaryBranches) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a ? b || c : d || e");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_TERNARY);
  node_t *then = node_get_child(expr, CLUX_EXPR_TERNARY_THEN);
  ASSERT_NE(then, nullptr);
  EXPECT_EQ(node_get_kind(then), CLUX_NODE_EXPRESSION_BINARY);
  node_t *else_ = node_get_child(expr, CLUX_EXPR_TERNARY_ELSE);
  ASSERT_NE(else_, nullptr);
  EXPECT_EQ(node_get_kind(else_), CLUX_NODE_EXPRESSION_BINARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLorTest, AssignWithLorValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "x = a || b");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_ASSIGN);
  node_t *value = node_get_child(expr, CLUX_EXPR_ASSIGN_VALUE);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(node_get_kind(value), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 子节点 parent 未自动挂载 ---- */

TEST(ExpressionLorTest, ChildrenParentIsNotAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a || b");
  ASSERT_NE(expr, nullptr);
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(left, nullptr);
  EXPECT_EQ(node_get_parent(left), nullptr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

TEST(ExpressionLorTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a || b";
  node_t *expr = parse_lor(a, &p, src);
  expect_location_span(expr, src, "a || b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLorTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  a  ||  b  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expression_lor(a, p);
  expect_location_span(expr, "  a  ||  b  ", "a  ||  b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLorTest, ChainSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a || b || c";
  node_t *expr = parse_lor(a, &p, src);
  expect_location_span(expr, src, "a || b || c");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_location_span(inner, src, "b || c");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺右操作数：区间退化为 left .. symbol。 */
TEST(ExpressionLorTest, SpanWhenRightMissing) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ||";
  node_t *expr = parse_lor(a, &p, src);
  expect_location_span(expr, src, "a ||");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionLorTest, NoLeakOnLor) {
  const char *sources[] = {"a || b",
                           "a || b || c",
                           "a || b || c || d",
                           "a  ||  b",
                           "x = a || b",
                           "a || b ? c : d",
                           "a ? b || c : d",
                           "a ||",
                           "a",
                           "a | b",
                           "",
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

/* ================= 逻辑与表达式 ================= */

static node_t *parse_land(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_expression_land(a, *p);
}

TEST(ExpressionLandTest, SimpleLand) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_land(a, &p, "a && b");
  expect_binary(
      expr, CLUX_NODE_LITERAL_IDENTIFIER, "&&", CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, BooleanOperands) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_land(a, &p, "true && false");
  expect_binary(
      expr, CLUX_NODE_LITERAL_BOOLEAN, "&&", CLUX_NODE_LITERAL_BOOLEAN);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 右结合：a && b && c  =>  a && (b && c)，与短路求值顺序一致。 */
TEST(ExpressionLandTest, RightAssociativeChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_land(a, &p, "a && b && c");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(left, nullptr);
  EXPECT_EQ(node_get_kind(left), CLUX_NODE_LITERAL_IDENTIFIER);
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(node_get_kind(right), CLUX_NODE_EXPRESSION_BINARY);
  expect_binary(
      right, CLUX_NODE_LITERAL_IDENTIFIER, "&&", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, FourOperandChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_land(a, &p, "a && b && c && d");
  ASSERT_NE(expr, nullptr);
  node_t *cur = expr;
  int depth = 0;
  while (cur && node_get_kind(cur) == CLUX_NODE_EXPRESSION_BINARY) {
    depth++;
    cur = node_get_child(cur, CLUX_EXPR_BINARY_RIGHT);
  }
  EXPECT_EQ(depth, 3);
  EXPECT_EQ(node_get_kind(cur), CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, TriviaAroundOperator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_land(a, &p, "a  /* c */  &&  b  // d\n ");
  expect_binary(
      expr, CLUX_NODE_LITERAL_IDENTIFIER, "&&", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, FallsBackToLeftWithoutOperator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_land(a, &p, "a");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, FallbackDoesNotConsumeTrailingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_land(a, &p, "a   ");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* '&' 是位与，不是逻辑与，不应被消费。 */
TEST(ExpressionLandTest, FallsBackOnSingleAmpersand) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_land(a, &p, "a & b");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* '||' 不属于 && 层，应原样返回左操作数。 */
TEST(ExpressionLandTest, FallsBackOnLogicalOr) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_land(a, &p, "a || b");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, ReturnsNullOnNonLiteral) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_land(a, &p, "&&"), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, ReturnsNullOnEmptyStream) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_land(a, &p, ""), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

TEST(ExpressionLandTest, MissingRightReturnsPartialNodeAndError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a &&";
  node_t *expr = parse_land(a, &p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);

  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "expected expression after '&&'");
  EXPECT_STREQ(error_get_location(err)->filename, "test.clux");
  /* EOF：错误位置退化为 '&&' 的终点 */
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 4);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, MissingRightErrorPointsAtBadToken) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a && )";
  node_t *expr = parse_land(a, &p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 5);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, ChainMissingRightReportsOnce) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_land(a, &p, "a && b &&");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(node_get_kind(right), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_EQ(node_get_child(right, CLUX_EXPR_BINARY_RIGHT), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, ValidLandProducesNoError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_land(a, &p, "a && b");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 优先级：&& 高于 || ---- */

/* a || b && c  =>  a || (b && c) */
TEST(ExpressionLandTest, LandBindsTighterThanLor) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a || b && c");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  /* 顶层运算符是 || */
  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_location(symbol)->end.offset -
                node_get_location(symbol)->begin.offset,
            (ptrdiff_t)2);
  EXPECT_EQ(node_get_location(symbol)->begin.offset[0], '|');
  /* 右操作数是 && 表达式 */
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(node_get_kind(right), CLUX_NODE_EXPRESSION_BINARY);
  node_t *right_symbol = node_get_child(right, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(right_symbol, nullptr);
  EXPECT_EQ(node_get_location(right_symbol)->begin.offset[0], '&');
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* a && b || c  =>  (a && b) || c */
TEST(ExpressionLandTest, LandOnLeftOfLor) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a && b || c");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_location(symbol)->begin.offset[0], '|');
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(left, nullptr);
  EXPECT_EQ(node_get_kind(left), CLUX_NODE_EXPRESSION_BINARY);
  node_t *left_symbol = node_get_child(left, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(left_symbol, nullptr);
  EXPECT_EQ(node_get_location(left_symbol)->begin.offset[0], '&');
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 混合链：a && b || c && d  =>  (a && b) || (c && d) */
TEST(ExpressionLandTest, MixedChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_lor(a, &p, "a && b || c && d");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_location(symbol)->begin.offset[0], '|');
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(left, nullptr);
  EXPECT_EQ(node_get_kind(left), CLUX_NODE_EXPRESSION_BINARY);
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(node_get_kind(right), CLUX_NODE_EXPRESSION_BINARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 与其他表达式组合 ---- */

TEST(ExpressionLandTest, LandAsTernaryCondition) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a && b ? c : d");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_TERNARY);
  node_t *condition = node_get_child(expr, CLUX_EXPR_TERNARY_CONDITION);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(node_get_kind(condition), CLUX_NODE_EXPRESSION_BINARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, AssignWithLandValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "x = a && b");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_ASSIGN);
  node_t *value = node_get_child(expr, CLUX_EXPR_ASSIGN_VALUE);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(node_get_kind(value), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, ChildrenParentIsNotAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_land(a, &p, "a && b");
  ASSERT_NE(expr, nullptr);
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(left, nullptr);
  EXPECT_EQ(node_get_parent(left), nullptr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

TEST(ExpressionLandTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a && b";
  node_t *expr = parse_land(a, &p, src);
  expect_location_span(expr, src, "a && b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  a  &&  b  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expression_land(a, p);
  expect_location_span(expr, "  a  &&  b  ", "a  &&  b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, ChainSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a && b && c";
  node_t *expr = parse_land(a, &p, src);
  expect_location_span(expr, src, "a && b && c");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_location_span(inner, src, "b && c");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionLandTest, SpanWhenRightMissing) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a &&";
  node_t *expr = parse_land(a, &p, src);
  expect_location_span(expr, src, "a &&");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionLandTest, NoLeakOnLand) {
  const char *sources[] = {"a && b",
                           "a && b && c",
                           "a && b || c",
                           "a || b && c",
                           "a && b || c && d",
                           "x = a && b",
                           "a && b ? c : d",
                           "a &&",
                           "a",
                           "a & b",
                           "",
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
