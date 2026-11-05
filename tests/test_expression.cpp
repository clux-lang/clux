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
  /* "a , b"：identifier 后是 ','，不是赋值运算符 → 回退。
     用 ',' 而非 '+' 等真实运算符，避免这条用例随运算符实现推进而失效。 */
  node_t *expr = parse_expr(a, &p, "a , b");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  /* 游标停在 identifier 之后，',' 未被消费 */
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

/* ================= 二元表达式（|| / && / |） =================
   lor/land/or 三层结构同构，公共行为用参数化测试覆盖一次，
   只有跨层优先级等真正有差异的部分才写独立用例。 */

typedef node_t *(*binary_reader_t)(allocator_t *, parser_t *);

struct BinaryLevel {
  const char *name;
  binary_reader_t read;
  const char *op;
  /* 来自其他层、本层不应消费的运算符。 */
  const char *other_op;
  /* 链式结合性：位运算与逻辑运算右结合，相等比较左结合（白皮书 5.3）。 */
  bool right_associative;
};

static void PrintTo(const BinaryLevel &level, std::ostream *os) {
  *os << level.name;
}

static const BinaryLevel kBinaryLevels[] = {
    {"Lor", parser_read_expression_lor, "||", "?", true},
    {"Land", parser_read_expression_land, "&&", "||", true},
    {"Or", parser_read_expression_or, "|", "&&", true},
    {"Xor", parser_read_expression_xor, "^", "|", true},
    {"And", parser_read_expression_and, "&", "^", true},
    {"Eq", parser_read_expression_equality, "==", "&", false},
    {"Ne", parser_read_expression_equality, "!=", "&", false},
    {"Extends", parser_read_expression_equality, "extends", "&", false},
    {"Ge", parser_read_expression_relational, ">=", "==", false},
    {"Le", parser_read_expression_relational, "<=", "==", false},
    {"Gt", parser_read_expression_relational, ">", "==", false},
    {"Lt", parser_read_expression_relational, "<", "==", false},
    {"Shl", parser_read_expression_shift, "<<", "<", false},
    {"Shr", parser_read_expression_shift, ">>", "<", false},
    {"Add", parser_read_expression_additive, "+", "<<", false},
    {"Sub", parser_read_expression_additive, "-", "<<", false},
    {"Mul", parser_read_expression_multiplicative, "*", "+", false},
    {"Div", parser_read_expression_multiplicative, "/", "+", false},
    {"Mod", parser_read_expression_multiplicative, "%", "+", false},
    {"As", parser_read_expression_as_is, "as", "*", false},
    {"Is", parser_read_expression_as_is, "is", "*", false},
};

class ExpressionBinaryTest : public ::testing::TestWithParam<BinaryLevel> {
protected:
  node_t *parse(parser_t **p, const char *src) {
    *p = create_parser(allocator_, "test.clux", src);
    return GetParam().read(allocator_, *p);
  }
  allocator_t *allocator_ = create_allocator(malloc, free);
  ~ExpressionBinaryTest() override { delete_allocator(&allocator_); }
};

/* 断言二元节点的三个子节点形状。 */
static void expect_binary(node_t *expr,
                          const char *symbol_src,
                          node_kind_t left_kind,
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

/* 断言节点的顶层运算符文本。 */
static void expect_top_operator(node_t *expr, const char *op) {
  ASSERT_NE(expr, nullptr);
  ASSERT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  const location_t *loc = node_get_location(symbol);
  EXPECT_EQ(loc->end.offset - loc->begin.offset, (ptrdiff_t)strlen(op));
  EXPECT_EQ(strncmp(loc->begin.offset, op, strlen(op)), 0);
}

/* 子节点下标只需在任意一层验证一次。 */
TEST(ExpressionBinaryTest, ChildIndexesAreDistinct) {
  EXPECT_NE(CLUX_EXPR_BINARY_LEFT, CLUX_EXPR_BINARY_SYMBOL);
  EXPECT_NE(CLUX_EXPR_BINARY_LEFT, CLUX_EXPR_BINARY_RIGHT);
  EXPECT_NE(CLUX_EXPR_BINARY_SYMBOL, CLUX_EXPR_BINARY_RIGHT);
}

TEST_P(ExpressionBinaryTest, SimpleBinary) {
  parser_t *p;
  char src[32];
  snprintf(src, sizeof(src), "a %s b", GetParam().op);
  node_t *expr = parse(&p, src);
  expect_binary(expr,
                GetParam().op,
                CLUX_NODE_LITERAL_IDENTIFIER,
                CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  delete_node(&expr);
  delete_parser(&p);
}

/* 链式结合性：右结合得 a op (b op c)，左结合得 (a op b) op c。
   两种形态不等价，所以按层的实际结合性断言。 */
TEST_P(ExpressionBinaryTest, ChainAssociativity) {
  parser_t *p;
  char src[48];
  snprintf(src, sizeof(src), "a %s b %s c", GetParam().op, GetParam().op);
  node_t *expr = parse(&p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  if (GetParam().right_associative) {
    /* 左是裸标识符，右是嵌套二元节点 */
    node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(node_get_kind(left), CLUX_NODE_LITERAL_IDENTIFIER);
    node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(node_get_kind(right), CLUX_NODE_EXPRESSION_BINARY);
    expect_binary(right,
                  GetParam().op,
                  CLUX_NODE_LITERAL_IDENTIFIER,
                  CLUX_NODE_LITERAL_IDENTIFIER);
  } else {
    /* 左是嵌套二元节点，右是裸标识符 */
    node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(node_get_kind(left), CLUX_NODE_EXPRESSION_BINARY);
    node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(node_get_kind(right), CLUX_NODE_LITERAL_IDENTIFIER);
    expect_binary(left,
                  GetParam().op,
                  CLUX_NODE_LITERAL_IDENTIFIER,
                  CLUX_NODE_LITERAL_IDENTIFIER);
  }
  delete_node(&expr);
  delete_parser(&p);
}

TEST_P(ExpressionBinaryTest, TriviaAroundOperator) {
  parser_t *p;
  char src[64];
  snprintf(src, sizeof(src), "a /* c */ %s b // d\n ", GetParam().op);
  node_t *expr = parse(&p, src);
  expect_binary(expr,
                GetParam().op,
                CLUX_NODE_LITERAL_IDENTIFIER,
                CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
}

/* 无运算符时原样返回左操作数，游标停在它之后（不吞掉 trivia）。 */
TEST_P(ExpressionBinaryTest, FallsBackToLeftWithoutOperator) {
  parser_t *p;
  node_t *expr = parse(&p, "a   ");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
}

/* 其他层的运算符不应被本层消费。 */
TEST_P(ExpressionBinaryTest, FallsBackOnOtherLevelOperator) {
  parser_t *p;
  char src[32];
  snprintf(src, sizeof(src), "a %s b", GetParam().other_op);
  node_t *expr = parse(&p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
}

/* 复合赋值运算符是单个 token，任何二元层都不应把它拆成 op 后跟 '='。
   这条对所有层都成立，所以不放进参数化表，统一跑一遍。 */
TEST(ExpressionBinaryTest, FallsBackOnCompoundAssign) {
  const char *ops[] = {"||=",
                       "&&=",
                       "|=",
                       "^=",
                       "&=",
                       "+=",
                       "-=",
                       "*=",
                       "/=",
                       "%=",
                       "~=",
                       "<<=",
                       ">>=",
                       nullptr};
  for (size_t i = 0; ops[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    char src[32];
    snprintf(src, sizeof(src), "a %s b", ops[i]);
    parser_t *p = create_parser(a, "test.clux", src);
    /* 从最高二元层进入：整条回退链都不应把复合赋值拆开。
       注意不能用 parser_read_expression，那一层会把 `a ||= b` 当合法赋值。 */
    node_t *expr = parser_read_expression_lor(a, p);
    ASSERT_NE(expr, nullptr) << "op=" << ops[i];
    EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER)
        << "op=" << ops[i];
    EXPECT_EQ(parser_get_position(p), 1u) << "op=" << ops[i];
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* 单独的运算符 token 无法构成二元表达式。例外：+ 和 - 同时是一元前缀
   运算符，单独出现会进入一元层并按“缺操作数”容错，返回 RIGHT 为 NULL
   的不完整节点并上报错误。 */
TEST_P(ExpressionBinaryTest, ReturnsNullOnNonLiteral) {
  parser_t *p;
  bool is_unary =
      strcmp(GetParam().op, "+") == 0 || strcmp(GetParam().op, "-") == 0;
  if (!is_unary) {
    EXPECT_EQ(parse(&p, GetParam().op), nullptr);
    EXPECT_EQ(parser_get_position(p), 0u);
    delete_parser(&p);
    return;
  }
  node_t *expr = parse(&p, GetParam().op);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
}

TEST_P(ExpressionBinaryTest, ReturnsNullOnEmptyStream) {
  parser_t *p;
  EXPECT_EQ(parse(&p, ""), nullptr);
  delete_parser(&p);
}

/* ---- 错误路径 ---- */

TEST_P(ExpressionBinaryTest, MissingRightReturnsPartialNodeAndError) {
  parser_t *p;
  char src[32];
  snprintf(src, sizeof(src), "a %s", GetParam().op);
  node_t *expr = parse(&p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);

  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  char expected[64];
  snprintf(expected,
           sizeof(expected),
           "expected expression after '%s'",
           GetParam().op);
  EXPECT_STREQ(error_get_message(err), expected);
  EXPECT_STREQ(error_get_location(err)->filename, "test.clux");
  /* EOF：错误位置退化为运算符的终点 */
  EXPECT_EQ(error_get_location(err)->begin.offset, src + strlen(src));
  delete_node(&expr);
  delete_parser(&p);
}

TEST_P(ExpressionBinaryTest, MissingRightErrorPointsAtBadToken) {
  parser_t *p;
  char src[32];
  snprintf(src, sizeof(src), "a %s )", GetParam().op);
  node_t *expr = parse(&p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_EQ(error_get_location(err)->begin.offset, src + strlen(src) - 1);
  delete_node(&expr);
  delete_parser(&p);
}

/* 链中途缺右操作数：只报一条错误。
   右结合：外层是 a op (b op <缺>)，缺的是内层 RIGHT；
   左结合：外层是 (a op b) op <缺>，缺的是外层 RIGHT。 */
TEST_P(ExpressionBinaryTest, ChainMissingRightReportsOnce) {
  parser_t *p;
  char src[48];
  snprintf(src, sizeof(src), "a %s b %s", GetParam().op, GetParam().op);
  node_t *expr = parse(&p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  node_t *incomplete = expr;
  if (GetParam().right_associative) {
    node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(node_get_kind(right), CLUX_NODE_EXPRESSION_BINARY);
    incomplete = right;
  } else {
    node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(node_get_kind(left), CLUX_NODE_EXPRESSION_BINARY);
  }
  EXPECT_EQ(node_get_child(incomplete, CLUX_EXPR_BINARY_RIGHT), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
}

TEST_P(ExpressionBinaryTest, ValidBinaryProducesNoError) {
  parser_t *p;
  char src[32];
  snprintf(src, sizeof(src), "a %s b", GetParam().op);
  node_t *expr = parse(&p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
}

/* ---- 组合 ---- */

TEST_P(ExpressionBinaryTest, AssignWithBinaryValue) {
  parser_t *p;
  char src[48];
  snprintf(src, sizeof(src), "x = a %s b", GetParam().op);
  p = create_parser(allocator_, "test.clux", src);
  node_t *expr = parser_read_expression(allocator_, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_ASSIGN);
  node_t *value = node_get_child(expr, CLUX_EXPR_ASSIGN_VALUE);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(node_get_kind(value), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
}

TEST_P(ExpressionBinaryTest, ChildrenParentIsNotAutoSet) {
  parser_t *p;
  char src[32];
  snprintf(src, sizeof(src), "a %s b", GetParam().op);
  node_t *expr = parse(&p, src);
  ASSERT_NE(expr, nullptr);
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(left, nullptr);
  EXPECT_EQ(node_get_parent(left), nullptr);
  delete_node(&expr);
  delete_parser(&p);
}

/* ---- location ---- */

TEST_P(ExpressionBinaryTest, SpanWithoutSurroundingTrivia) {
  parser_t *p;
  char src[48];
  char expected[32];
  snprintf(src, sizeof(src), "  a  %s  b  ", GetParam().op);
  snprintf(expected, sizeof(expected), "a  %s  b", GetParam().op);
  p = create_parser(allocator_, "test.clux", src);
  parser_skip_trivia(p);
  node_t *expr = GetParam().read(allocator_, p);
  expect_location_span(expr, src, expected);
  delete_node(&expr);
  delete_parser(&p);
}

/* 链式区间：外层覆盖整条链，内层覆盖被嵌套的那一段。 */
TEST_P(ExpressionBinaryTest, ChainSpan) {
  parser_t *p;
  char src[48];
  char inner[32];
  snprintf(src, sizeof(src), "a %s b %s c", GetParam().op, GetParam().op);
  node_t *expr = parse(&p, src);
  expect_location_span(expr, src, src);
  /* 右结合内层是 b op c，左结合内层是 a op b。 */
  node_t *nested =
      node_get_child(expr,
                     GetParam().right_associative ? CLUX_EXPR_BINARY_RIGHT
                                                  : CLUX_EXPR_BINARY_LEFT);
  snprintf(inner,
           sizeof(inner),
           GetParam().right_associative ? "b %s c" : "a %s b",
           GetParam().op);
  expect_location_span(nested, src, inner);
  delete_node(&expr);
  delete_parser(&p);
}

/* 缺右操作数：区间退化为 left .. symbol。 */
TEST_P(ExpressionBinaryTest, SpanWhenRightMissing) {
  parser_t *p;
  char src[32];
  snprintf(src, sizeof(src), "a %s", GetParam().op);
  node_t *expr = parse(&p, src);
  expect_location_span(expr, src, src);
  delete_node(&expr);
  delete_parser(&p);
}

/* ---- 泄漏检查 ---- */

TEST_P(ExpressionBinaryTest, NoLeak) {
  char chain[64];
  char assign[64];
  char missing[32];
  char other[32];
  snprintf(chain, sizeof(chain), "a %s b %s c", GetParam().op, GetParam().op);
  snprintf(assign, sizeof(assign), "x = a %s b", GetParam().op);
  snprintf(missing, sizeof(missing), "a %s", GetParam().op);
  snprintf(other, sizeof(other), "a %s b", GetParam().other_op);
  const char *sources[] = {
      "a",
      chain,
      assign,
      missing,
      other,
      "",
      nullptr,
  };
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = GetParam().read(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

INSTANTIATE_TEST_SUITE_P(AllLevels,
                         ExpressionBinaryTest,
                         ::testing::ValuesIn(kBinaryLevels));

/* ================= 跨层优先级 =================
   优先级链：assign -> ternary -> lor -> land -> or -> xor -> and -> literal
   相邻两层两两验证，新增一层只需在表里加一行。 */

struct PrecedencePair {
  const char *name;
  /* 较低优先级层的读取函数。 */
  binary_reader_t read_lower;
  const char *lower_op;
  const char *higher_op;
};

static void PrintTo(const PrecedencePair &pair, std::ostream *os) {
  *os << pair.name;
}

static const PrecedencePair kPrecedencePairs[] = {
    {"LorLand", parser_read_expression_lor, "||", "&&"},
    {"LandOr", parser_read_expression_land, "&&", "|"},
    {"OrXor", parser_read_expression_or, "|", "^"},
    {"XorAnd", parser_read_expression_xor, "^", "&"},
    {"AndEq", parser_read_expression_and, "&", "=="},
    {"EqRel", parser_read_expression_equality, "==", "<"},
    {"RelShift", parser_read_expression_relational, "<", "<<"},
    {"ShiftAdd", parser_read_expression_shift, "<<", "+"},
    {"AddMul", parser_read_expression_additive, "+", "*"},
    {"MulAs", parser_read_expression_multiplicative, "*", "as"},
};

class ExpressionPrecedencePairTest
    : public ::testing::TestWithParam<PrecedencePair> {
protected:
  allocator_t *allocator_ = create_allocator(malloc, free);
  ~ExpressionPrecedencePairTest() override { delete_allocator(&allocator_); }

  node_t *parse(const char *src) {
    parser_t *p = create_parser(allocator_, "test.clux", src);
    node_t *expr = GetParam().read_lower(allocator_, p);
    delete_parser(&p);
    return expr;
  }
};

/* a lower b higher c  =>  a lower (b higher c) */
TEST_P(ExpressionPrecedencePairTest, HigherBindsTighter) {
  char src[48];
  snprintf(src,
           sizeof(src),
           "a %s b %s c",
           GetParam().lower_op,
           GetParam().higher_op);
  node_t *expr = parse(src);
  expect_top_operator(expr, GetParam().lower_op);
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_top_operator(right, GetParam().higher_op);
  delete_node(&expr);
}

/* a higher b lower c  =>  (a higher b) lower c */
TEST_P(ExpressionPrecedencePairTest, HigherOnLeft) {
  char src[48];
  snprintf(src,
           sizeof(src),
           "a %s b %s c",
           GetParam().higher_op,
           GetParam().lower_op);
  node_t *expr = parse(src);
  expect_top_operator(expr, GetParam().lower_op);
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_top_operator(left, GetParam().higher_op);
  delete_node(&expr);
}

INSTANTIATE_TEST_SUITE_P(AdjacentLevels,
                         ExpressionPrecedencePairTest,
                         ::testing::ValuesIn(kPrecedencePairs));

/* a || b && c | d ^ e & f == g < h << i + j * k as l
   =>  a || (b && (c | (d ^ (e & (f == (g < (h << (i + (j * (k as l)))))))))) */
TEST(ExpressionPrecedenceTest, FullChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(
      a, "test.clux", "a || b && c | d ^ e & f == g < h << i + j * k as l");
  node_t *expr = parser_read_expression_lor(a, p);
  expect_top_operator(expr, "||");
  node_t *land_expr = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_top_operator(land_expr, "&&");
  node_t *or_expr = node_get_child(land_expr, CLUX_EXPR_BINARY_RIGHT);
  expect_top_operator(or_expr, "|");
  node_t *xor_expr = node_get_child(or_expr, CLUX_EXPR_BINARY_RIGHT);
  expect_top_operator(xor_expr, "^");
  node_t *and_expr = node_get_child(xor_expr, CLUX_EXPR_BINARY_RIGHT);
  expect_top_operator(and_expr, "&");
  node_t *eq_expr = node_get_child(and_expr, CLUX_EXPR_BINARY_RIGHT);
  expect_top_operator(eq_expr, "==");
  node_t *rel_expr = node_get_child(eq_expr, CLUX_EXPR_BINARY_RIGHT);
  expect_top_operator(rel_expr, "<");
  node_t *shift_expr = node_get_child(rel_expr, CLUX_EXPR_BINARY_RIGHT);
  expect_top_operator(shift_expr, "<<");
  node_t *add_expr = node_get_child(shift_expr, CLUX_EXPR_BINARY_RIGHT);
  expect_top_operator(add_expr, "+");
  node_t *mul_expr = node_get_child(add_expr, CLUX_EXPR_BINARY_RIGHT);
  expect_top_operator(mul_expr, "*");
  node_t *as_expr = node_get_child(mul_expr, CLUX_EXPR_BINARY_RIGHT);
  expect_top_operator(as_expr, "as");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* as/is 是关键字，SYMBOL 子节点同样记录其文本。 */
TEST(ExpressionPrecedenceTest, KeywordOperatorsProduceSymbolNodes) {
  const char *sources[] = {"a as b", "a is b", nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expression_as_is(a, p);
    ASSERT_NE(expr, nullptr) << "src=" << sources[i];
    node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
    ASSERT_NE(symbol, nullptr);
    EXPECT_EQ(node_get_kind(symbol), CLUX_NODE_SYMBOL);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* extends 与 ==/!= 同级，混用按左结合：(a == b) extends c */
TEST(ExpressionPrecedenceTest, ExtendsSharesLevelWithEquality) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a == b extends c");
  node_t *expr = parser_read_expression_equality(a, p);
  expect_top_operator(expr, "extends");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_top_operator(left, "==");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* as/is 左结合：a as b is c => (a as b) is c */
TEST(ExpressionPrecedenceTest, AsIsLeftAssociative) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a as b is c");
  node_t *expr = parser_read_expression_as_is(a, p);
  expect_top_operator(expr, "is");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_top_operator(left, "as");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 三个乘除运算符同级，混用按左结合：(a % b) / c */
TEST(ExpressionPrecedenceTest, MultiplicativeOperatorsShareLevel) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a % b / c");
  node_t *expr = parser_read_expression_multiplicative(a, p);
  expect_top_operator(expr, "/");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_top_operator(left, "%");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* + 与 - 同级，混用按左结合：(a - b) + c */
TEST(ExpressionPrecedenceTest, AdditiveOperatorsShareLevel) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a - b + c");
  node_t *expr = parser_read_expression_additive(a, p);
  expect_top_operator(expr, "+");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_top_operator(left, "-");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* >> 与 << 同级，混用按左结合：(a << b) >> c */
TEST(ExpressionPrecedenceTest, ShiftOperatorsShareLevel) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a << b >> c");
  node_t *expr = parser_read_expression_shift(a, p);
  expect_top_operator(expr, ">>");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_top_operator(left, "<<");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* == 与 != 同级，混用按左结合：(a == b) != c */
TEST(ExpressionPrecedenceTest, EqualityOperatorsShareLevel) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a == b != c");
  node_t *expr = parser_read_expression_equality(a, p);
  expect_top_operator(expr, "!=");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_top_operator(left, "==");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 四个关系运算符同级，混用按左结合：(a < b) >= c */
TEST(ExpressionPrecedenceTest, RelationalOperatorsShareLevel) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a < b >= c");
  node_t *expr = parser_read_expression_relational(a, p);
  expect_top_operator(expr, ">=");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_top_operator(left, "<");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 最长匹配：>= 不能被拆成 > 后跟 =，<< 也不能被拆成两个 <。 */
TEST(ExpressionPrecedenceTest, LongestMatchWins) {
  const char *sources[] = {">=", "<=", ">>", "<<", nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    token_t *token = parser_peek_token(p);
    ASSERT_NE(token, nullptr) << "src=" << sources[i];
    const location_t *loc = token_get_location(token);
    EXPECT_EQ(loc->end.offset - loc->begin.offset, (ptrdiff_t)2)
        << "src=" << sources[i];
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* 三元优先级低于 ||：条件可以是整个逻辑或表达式。 */
TEST(ExpressionPrecedenceTest, TernaryConditionTakesWholeLor) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a || b ? c : d");
  node_t *expr = parser_read_expression_ternary(a, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_TERNARY);
  node_t *condition = node_get_child(expr, CLUX_EXPR_TERNARY_CONDITION);
  expect_top_operator(condition, "||");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 三元分支也接受逻辑或表达式。 */
TEST(ExpressionPrecedenceTest, TernaryBranchesTakeLor) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a ? b || c : d && e");
  node_t *expr = parser_read_expression_ternary(a, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_TERNARY);
  expect_top_operator(node_get_child(expr, CLUX_EXPR_TERNARY_THEN), "||");
  expect_top_operator(node_get_child(expr, CLUX_EXPR_TERNARY_ELSE), "&&");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 前缀一元表达式 =================
   ! ~ + - 复用二元节点形态：LEFT 为 NULL，SYMBOL 存运算符，RIGHT 存操作数。 */

static node_t *parse_unary(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_expression_unary(a, *p);
}

/* 断言一元节点的形状。 */
static void
expect_unary(node_t *expr, const char *op, node_kind_t operand_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_BINARY_CHILDREN_SIZE);
  /* 一元形态的关键特征：LEFT 缺失 */
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_kind(symbol), CLUX_NODE_SYMBOL);
  EXPECT_EQ(node_get_location(symbol)->end.offset -
                node_get_location(symbol)->begin.offset,
            (ptrdiff_t)strlen(op));
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_kind(operand), operand_kind);
}

TEST(ExpressionUnaryTest, AllOperators) {
  const char *ops[] = {"!", "~", "+", "-", nullptr};
  for (size_t i = 0; ops[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p;
    char src[16];
    snprintf(src, sizeof(src), "%sa", ops[i]);
    node_t *expr = parse_unary(a, &p, src);
    expect_unary(expr, ops[i], CLUX_NODE_LITERAL_IDENTIFIER);
    EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
    EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u) << "op=" << ops[i];
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

TEST(ExpressionUnaryTest, OperandCanBeNumeric) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_unary(a, &p, "-42");
  expect_unary(expr, "-", CLUX_NODE_LITERAL_NUMERIC);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionUnaryTest, OperandCanBeParenthesizedLater) {
  /* 括号尚未实现，用字符串/布尔等其他字面量验证操作数种类不受限。 */
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_unary(a, &p, "!true");
  expect_unary(expr, "!", CLUX_NODE_LITERAL_BOOLEAN);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 前缀运算符右绑定：!!a => !(!a) */
TEST(ExpressionUnaryTest, StackedOperatorsAreRightBinding) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_unary(a, &p, "!!a");
  ASSERT_NE(expr, nullptr);
  expect_unary(expr, "!", CLUX_NODE_EXPRESSION_BINARY);
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(inner, "!", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 混合叠加：-~+a => - (~ (+ a)) */
TEST(ExpressionUnaryTest, MixedStackedOperators) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_unary(a, &p, "-~+a");
  ASSERT_NE(expr, nullptr);
  expect_unary(expr, "-", CLUX_NODE_EXPRESSION_BINARY);
  node_t *second = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(second, "~", CLUX_NODE_EXPRESSION_BINARY);
  node_t *third = node_get_child(second, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(third, "+", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionUnaryTest, TriviaAfterOperator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_unary(a, &p, "! /* c */ a");
  expect_unary(expr, "!", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 无运算符时直接返回下一级结果（literal）。 */
TEST(ExpressionUnaryTest, FallsBackToLiteralWithoutOperator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_unary(a, &p, "a");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionUnaryTest, ReturnsNullOnEmptyStream) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_unary(a, &p, ""), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

/* 缺操作数：返回不完整节点（RIGHT 为 NULL），并上报错误。 */
TEST(ExpressionUnaryTest, MissingOperandReturnsPartialNodeAndError) {
  const char *ops[] = {"!", "~", "+", "-", nullptr};
  for (size_t i = 0; ops[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p;
    node_t *expr = parse_unary(a, &p, ops[i]);
    ASSERT_NE(expr, nullptr) << "op=" << ops[i];
    EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
    EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
    EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL), nullptr);
    EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);

    vec_t *errors = parser_get_error(p);
    ASSERT_EQ(vec_get_size(errors), 1u) << "op=" << ops[i];
    clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
    char expected[64];
    snprintf(
        expected, sizeof(expected), "expected expression after '%s'", ops[i]);
    EXPECT_STREQ(error_get_message(err), expected);
    EXPECT_STREQ(error_get_location(err)->filename, "test.clux");
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* 缺操作数且后面跟着无法解析的 token 时，错误位置指向该 token。 */
TEST(ExpressionUnaryTest, MissingOperandErrorPointsAtBadToken) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "! )";
  node_t *expr = parse_unary(a, &p, src);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 2);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 叠加中内层缺操作数：只报一条。 */
TEST(ExpressionUnaryTest, StackedMissingOperandReportsOnce) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_unary(a, &p, "!!");
  ASSERT_NE(expr, nullptr);
  expect_unary(expr, "!", CLUX_NODE_EXPRESSION_BINARY);
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(node_get_child(inner, CLUX_EXPR_BINARY_RIGHT), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 与二元层的关系 ---- */

/* 一元优先级高于所有二元层：-a * b => (-a) * b */
TEST(ExpressionUnaryTest, UnaryBindsTighterThanMultiplicative) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-a * b");
  node_t *expr = parser_read_expression_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_unary(left, "-", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 一元优先级高于 as/is：-a as b => (-a) as b */
TEST(ExpressionUnaryTest, UnaryBindsTighterThanAsIs) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-a as b");
  node_t *expr = parser_read_expression_as_is(a, p);
  expect_top_operator(expr, "as");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_unary(left, "-", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* as/is 右值可以是一元表达式：a as -b => a as (-b) */
TEST(ExpressionUnaryTest, AsIsRightOperandCanBeUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a as -b");
  node_t *expr = parser_read_expression_as_is(a, p);
  expect_top_operator(expr, "as");
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(right, "-", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 二元右操作数也可以是一元：a + -b => a + (-b) */
TEST(ExpressionUnaryTest, AdditiveRightOperandCanBeUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a + -b");
  node_t *expr = parser_read_expression_additive(a, p);
  expect_top_operator(expr, "+");
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(right, "-", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 子节点 parent 未自动挂载 ---- */

TEST(ExpressionUnaryTest, OperandParentIsNotAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_unary(a, &p, "!a");
  ASSERT_NE(expr, nullptr);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_parent(operand), nullptr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

/* 一元区间 = 运算符起点 .. 操作数终点，不含前后 trivia。 */
TEST(ExpressionUnaryTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "!a";
  node_t *expr = parse_unary(a, &p, src);
  expect_location_span(expr, src, "!a");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionUnaryTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  !  a  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expression_unary(a, p);
  expect_location_span(expr, "  !  a  ", "!  a");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionUnaryTest, StackedSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "!!a";
  node_t *expr = parse_unary(a, &p, src);
  expect_location_span(expr, src, "!!a");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_location_span(inner, src, "!a");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺操作数：区间退化为运算符自身。 */
TEST(ExpressionUnaryTest, SpanWhenOperandMissing) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "!";
  node_t *expr = parse_unary(a, &p, src);
  expect_location_span(expr, src, "!");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionUnaryTest, NoLeakOnUnary) {
  const char *sources[] = {"!a",
                           "~a",
                           "+a",
                           "-a",
                           "-42",
                           "!!a",
                           "-~+a",
                           "! /* c */ a",
                           "a",
                           "!",
                           "!!",
                           "-a * b",
                           "a as -b",
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

/* ================= 后缀一元表达式 =================
   .& .* .! .? 与一元层镜像：LEFT 存操作数，SYMBOL 存运算符，RIGHT 为 NULL。 */

static node_t *parse_postfix(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_expression_postfix(a, *p);
}

/* 断言后缀节点的形状。 */
static void
expect_postfix(node_t *expr, const char *op, node_kind_t operand_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_kind(operand), operand_kind);
  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_kind(symbol), CLUX_NODE_SYMBOL);
  EXPECT_EQ(node_get_location(symbol)->end.offset -
                node_get_location(symbol)->begin.offset,
            (ptrdiff_t)strlen(op));
  /* 后缀形态的关键特征：RIGHT 缺失 */
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
}

TEST(ExpressionPostfixTest, AllOperators) {
  const char *ops[] = {".&", ".*", ".!", ".?", nullptr};
  for (size_t i = 0; ops[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p;
    char src[16];
    snprintf(src, sizeof(src), "a%s", ops[i]);
    node_t *expr = parse_postfix(a, &p, src);
    expect_postfix(expr, ops[i], CLUX_NODE_LITERAL_IDENTIFIER);
    EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
    EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u) << "op=" << ops[i];
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

TEST(ExpressionPostfixTest, OperandCanBeNumeric) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "42.!");
  expect_postfix(expr, ".!", CLUX_NODE_LITERAL_NUMERIC);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 后缀左结合：p.*.& => (p.*).& */
TEST(ExpressionPostfixTest, LeftAssociativeChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "p.*.&");
  ASSERT_NE(expr, nullptr);
  expect_postfix(expr, ".&", CLUX_NODE_EXPRESSION_BINARY);
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_postfix(inner, ".*", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionPostfixTest, FourOperandChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.!.!.!.!");
  ASSERT_NE(expr, nullptr);
  node_t *cur = expr;
  int depth = 0;
  while (cur && node_get_kind(cur) == CLUX_NODE_EXPRESSION_BINARY) {
    depth++;
    cur = node_get_child(cur, CLUX_EXPR_BINARY_LEFT);
  }
  EXPECT_EQ(depth, 4);
  EXPECT_EQ(node_get_kind(cur), CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionPostfixTest, TriviaAfterOperator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.! /* c */ ");
  expect_postfix(expr, ".!", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 无后缀运算符时直接返回 literal。 */
TEST(ExpressionPostfixTest, FallsBackToLiteralWithoutOperator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionPostfixTest, FallbackDoesNotConsumeTrailingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a   ");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 点号后不是 identifier 时不构成成员访问，游标回退到接收者之后。 */
TEST(ExpressionPostfixTest, FallsBackWhenDotNotFollowedByIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.1");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  /* '.' 未被消费 */
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionPostfixTest, ReturnsNullOnNonLiteral) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_postfix(a, &p, ".!"), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionPostfixTest, ReturnsNullOnEmptyStream) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_postfix(a, &p, ""), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 与一元/二元层的关系 ---- */

/* 后缀优先级高于前缀：-p.* => -(p.*) */
TEST(ExpressionPostfixTest, PostfixBindsTighterThanPrefixUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-p.*");
  node_t *expr = parser_read_expression_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPRESSION_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_postfix(operand, ".*", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 后缀优先级高于所有二元层：a * b.! => a * (b.!) */
TEST(ExpressionPostfixTest, PostfixBindsTighterThanBinary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a * b.!");
  node_t *expr = parser_read_expression_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_postfix(right, ".!", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 后缀优先级高于 as/is：a.! as b => (a.!) as b */
TEST(ExpressionPostfixTest, PostfixBindsTighterThanAsIs) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a.! as b");
  node_t *expr = parser_read_expression_as_is(a, p);
  expect_top_operator(expr, "as");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_postfix(left, ".!", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 子节点 parent 未自动挂载 ---- */

TEST(ExpressionPostfixTest, OperandParentIsNotAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.!");
  ASSERT_NE(expr, nullptr);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_parent(operand), nullptr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

/* 后缀区间 = 操作数起点 .. 运算符终点，不含前后 trivia。 */
TEST(ExpressionPostfixTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a.!";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "a.!");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionPostfixTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  a  .!  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expression_postfix(a, p);
  expect_location_span(expr, "  a  .!  ", "a  .!");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 链式区间：外层覆盖整条链，内层覆盖被嵌套的那一段。 */
TEST(ExpressionPostfixTest, ChainSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a.*.&";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "a.*.&");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_location_span(inner, src, "a.*");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionPostfixTest, NoLeakOnPostfix) {
  const char *sources[] = {"a.&",
                           "a.*",
                           "a.!",
                           "a.?",
                           "42.!",
                           "p.*.&",
                           "a.!.!.!.!",
                           "a.! /* c */ ",
                           "a",
                           "a.1",
                           "a.b",
                           "a.b.c",
                           "-p.*",
                           "a * b.!",
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

/* ================= 成员访问 =================
   obj.field 复用二元节点：LEFT=接收者，SYMBOL='.'，RIGHT=字段名 identifier。 */

/* 断言成员访问节点的形状。 */
static void
expect_member(node_t *expr, node_kind_t receiver_kind, const char *field_src) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_BINARY_CHILDREN_SIZE);

  node_t *receiver = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(receiver, nullptr);
  EXPECT_EQ(node_get_kind(receiver), receiver_kind);

  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_kind(symbol), CLUX_NODE_SYMBOL);
  EXPECT_EQ(node_get_location(symbol)->end.offset -
                node_get_location(symbol)->begin.offset,
            1);

  node_t *field = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(node_get_kind(field), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(node_get_location(field)->end.offset -
                node_get_location(field)->begin.offset,
            (ptrdiff_t)strlen(field_src));
  EXPECT_EQ(strncmp(node_get_location(field)->begin.offset,
                    field_src,
                    strlen(field_src)),
            0);
}

TEST(ExpressionMemberTest, SimpleMember) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "obj.field");
  expect_member(expr, CLUX_NODE_LITERAL_IDENTIFIER, "field");
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 成员链左结合：a.b.c => (a.b).c */
TEST(ExpressionMemberTest, MemberChainIsLeftAssociative) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.b.c");
  ASSERT_NE(expr, nullptr);
  expect_member(expr, CLUX_NODE_EXPRESSION_BINARY, "c");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_member(inner, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionMemberTest, LongChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.b.c.d.e");
  ASSERT_NE(expr, nullptr);
  node_t *cur = expr;
  int depth = 0;
  while (cur && node_get_kind(cur) == CLUX_NODE_EXPRESSION_BINARY) {
    depth++;
    cur = node_get_child(cur, CLUX_EXPR_BINARY_LEFT);
  }
  EXPECT_EQ(depth, 4);
  EXPECT_EQ(node_get_kind(cur), CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 成员访问与后缀一元运算符可自由混用：a.b.* => (a.b).* */
TEST(ExpressionMemberTest, MixedWithPostfixOperator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.b.*");
  ASSERT_NE(expr, nullptr);
  expect_postfix(expr, ".*", CLUX_NODE_EXPRESSION_BINARY);
  node_t *receiver = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_member(receiver, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* a.*.b => (a.*).b */
TEST(ExpressionMemberTest, PostfixOperatorThenMember) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.*.b");
  ASSERT_NE(expr, nullptr);
  expect_member(expr, CLUX_NODE_EXPRESSION_BINARY, "b");
  node_t *receiver = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_postfix(receiver, ".*", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 成员访问允许点号两侧有 trivia。 */
TEST(ExpressionMemberTest, TriviaAroundDot) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a /* c */ . /* d */ b");
  expect_member(expr, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 关键字不能作字段名（字段恒为 identifier）。 */
TEST(ExpressionMemberTest, KeywordIsNotAField) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.var");
  ASSERT_NE(expr, nullptr);
  /* 不是成员访问，回退为接收者 */
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 数字不能作字段名。 */
TEST(ExpressionMemberTest, NumericIsNotAField) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.1");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 成员访问优先级高于所有二元层：a * b.c => a * (b.c) */
TEST(ExpressionMemberTest, MemberBindsTighterThanBinary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a * b.c");
  node_t *expr = parser_read_expression_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_member(right, CLUX_NODE_LITERAL_IDENTIFIER, "c");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 成员访问优先级高于前缀一元：-a.b => -(a.b) */
TEST(ExpressionMemberTest, MemberBindsTighterThanPrefixUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-a.b");
  node_t *expr = parser_read_expression_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPRESSION_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_member(operand, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

TEST(ExpressionMemberTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a.b";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "a.b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionMemberTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  a  .  b  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expression_postfix(a, p);
  expect_location_span(expr, "  a  .  b  ", "a  .  b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionMemberTest, ChainSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a.b.c";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "a.b.c");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_location_span(inner, src, "a.b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionMemberTest, ChildrenParentIsNotAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.b");
  ASSERT_NE(expr, nullptr);
  node_t *receiver = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(receiver, nullptr);
  EXPECT_EQ(node_get_parent(receiver), nullptr);
  node_t *field = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(node_get_parent(field), nullptr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 命名空间访问 =================
   Type::member / module::name 与成员访问结构相同，只差分隔符。 */

/* 断言命名空间访问节点的形状。 */
static void
expect_scope(node_t *expr, node_kind_t receiver_kind, const char *name_src) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_BINARY_CHILDREN_SIZE);

  node_t *receiver = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(receiver, nullptr);
  EXPECT_EQ(node_get_kind(receiver), receiver_kind);

  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_kind(symbol), CLUX_NODE_SYMBOL);
  EXPECT_EQ(node_get_location(symbol)->end.offset -
                node_get_location(symbol)->begin.offset,
            2);

  node_t *name = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(node_get_kind(name), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(node_get_location(name)->end.offset -
                node_get_location(name)->begin.offset,
            (ptrdiff_t)strlen(name_src));
  EXPECT_EQ(strncmp(node_get_location(name)->begin.offset,
                    name_src,
                    strlen(name_src)),
            0);
}

TEST(ExpressionScopeTest, SimpleScopeAccess) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "Type::member");
  expect_scope(expr, CLUX_NODE_LITERAL_IDENTIFIER, "member");
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 命名空间链左结合：a::b::c => (a::b)::c */
TEST(ExpressionScopeTest, ScopeChainIsLeftAssociative) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a::b::c");
  ASSERT_NE(expr, nullptr);
  expect_scope(expr, CLUX_NODE_EXPRESSION_BINARY, "c");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_scope(inner, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 与成员访问混用：a::b.c => (a::b).c */
TEST(ExpressionScopeTest, MixedWithMemberAccess) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a::b.c");
  ASSERT_NE(expr, nullptr);
  expect_member(expr, CLUX_NODE_EXPRESSION_BINARY, "c");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_scope(inner, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* a.b::c => (a.b)::c */
TEST(ExpressionScopeTest, MemberThenScope) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.b::c");
  ASSERT_NE(expr, nullptr);
  expect_scope(expr, CLUX_NODE_EXPRESSION_BINARY, "c");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_member(inner, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 与后缀一元运算符混用：a::b.* => (a::b).* */
TEST(ExpressionScopeTest, MixedWithPostfixOperator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a::b.*");
  ASSERT_NE(expr, nullptr);
  expect_postfix(expr, ".*", CLUX_NODE_EXPRESSION_BINARY);
  node_t *receiver = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_scope(receiver, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionScopeTest, TriviaAroundSeparator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a /* c */ :: /* d */ b");
  expect_scope(expr, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 分隔符后不是 identifier 时不构成访问，游标回退。 */
TEST(ExpressionScopeTest, FallsBackWhenSeparatorNotFollowedByIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a::1");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  /* '::' 未被消费 */
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionScopeTest, KeywordIsNotAScopeName) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a::var");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 命名空间访问优先级高于所有二元层：a * b::c => a * (b::c) */
TEST(ExpressionScopeTest, ScopeBindsTighterThanBinary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a * b::c");
  node_t *expr = parser_read_expression_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_scope(right, CLUX_NODE_LITERAL_IDENTIFIER, "c");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

TEST(ExpressionScopeTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a::b";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "a::b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionScopeTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  a  ::  b  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expression_postfix(a, p);
  expect_location_span(expr, "  a  ::  b  ", "a  ::  b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionScopeTest, ChainSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a::b::c";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "a::b::c");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_location_span(inner, src, "a::b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionScopeTest, ChildrenParentIsNotAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a::b");
  ASSERT_NE(expr, nullptr);
  node_t *receiver = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(receiver, nullptr);
  EXPECT_EQ(node_get_parent(receiver), nullptr);
  node_t *name = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(node_get_parent(name), nullptr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionScopeTest, NoLeakOnScope) {
  const char *sources[] = {"a::b",
                           "a::b::c",
                           "a::b.c",
                           "a.b::c",
                           "a::b.*",
                           "a::1",
                           "a::var",
                           "a * b::c",
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

/* ================= 下标 / 实例化 / 切片 =================
   下标与实例化语法同形（base[expr, ...]），语法阶段统一为
   CLUX_NODE_EXPRESSION_SUBSCRIPT；带冒号的切片为 CLUX_NODE_EXPRESSION_SLICE。 */

/* 断言下标/实例化节点：BASE 为接收者，其后是实参列表。 */
static void
expect_subscript(node_t *expr, node_kind_t base_kind, size_t arg_count) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_SUBSCRIPT);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_SUBSCRIPT_FIRST_ARG + arg_count);
  node_t *base = node_get_child(expr, CLUX_EXPR_SUBSCRIPT_BASE);
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(node_get_kind(base), base_kind);
  for (size_t i = 0; i < arg_count; i++) {
    EXPECT_NE(node_get_child(expr, CLUX_EXPR_SUBSCRIPT_FIRST_ARG + i), nullptr)
        << "arg=" << i;
  }
}

/* 断言切片节点：BASE 必存在，LO/HI 可为 NULL。 */
static void
expect_slice(node_t *expr, node_kind_t base_kind, bool has_lo, bool has_hi) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_SLICE);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_SLICE_CHILDREN_SIZE);
  node_t *base = node_get_child(expr, CLUX_EXPR_SLICE_BASE);
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(node_get_kind(base), base_kind);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_SLICE_LO) != nullptr, has_lo);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_SLICE_HI) != nullptr, has_hi);
}

/* ---- 下标 / 实例化 ---- */

TEST(ExpressionSubscriptTest, SingleArgument) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "arr[i]");
  expect_subscript(expr, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  node_t *arg = node_get_child(expr, CLUX_EXPR_SUBSCRIPT_FIRST_ARG);
  EXPECT_EQ(node_get_kind(arg), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionSubscriptTest, NumericIndex) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "arr[0]");
  expect_subscript(expr, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  node_t *arg = node_get_child(expr, CLUX_EXPR_SUBSCRIPT_FIRST_ARG);
  EXPECT_EQ(node_get_kind(arg), CLUX_NODE_LITERAL_NUMERIC);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 下标与实例化同形：多个逗号分隔实参。
   注意实例化的类型实参（如 i32）是关键字，类型读取器尚未实现，
   这里用 identifier 验证多实参形态。 */
TEST(ExpressionSubscriptTest, MultipleArguments) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "Vec[T, 4]");
  expect_subscript(expr, CLUX_NODE_LITERAL_IDENTIFIER, 2);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionSubscriptTest, ThreeArguments) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "T[a, b, c]");
  expect_subscript(expr, CLUX_NODE_LITERAL_IDENTIFIER, 3);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 实参可以是任意表达式。 */
TEST(ExpressionSubscriptTest, ExpressionArguments) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "arr[i + 1]");
  expect_subscript(expr, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  node_t *arg = node_get_child(expr, CLUX_EXPR_SUBSCRIPT_FIRST_ARG);
  EXPECT_EQ(node_get_kind(arg), CLUX_NODE_EXPRESSION_BINARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 链式下标左结合：a[i][j] => (a[i])[j] */
TEST(ExpressionSubscriptTest, ChainedSubscripts) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a[i][j]");
  ASSERT_NE(expr, nullptr);
  expect_subscript(expr, CLUX_NODE_EXPRESSION_SUBSCRIPT, 1);
  node_t *inner = node_get_child(expr, CLUX_EXPR_SUBSCRIPT_BASE);
  expect_subscript(inner, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 与成员访问混用：a.b[i] => (a.b)[i] */
TEST(ExpressionSubscriptTest, MemberThenSubscript) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.b[i]");
  ASSERT_NE(expr, nullptr);
  expect_subscript(expr, CLUX_NODE_EXPRESSION_BINARY, 1);
  node_t *base = node_get_child(expr, CLUX_EXPR_SUBSCRIPT_BASE);
  expect_member(base, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* a[i].b => (a[i]).b */
TEST(ExpressionSubscriptTest, SubscriptThenMember) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a[i].b");
  ASSERT_NE(expr, nullptr);
  expect_member(expr, CLUX_NODE_EXPRESSION_SUBSCRIPT, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionSubscriptTest, TriviaInsideBrackets) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a [ /* c */ i /* d */ ]");
  expect_subscript(expr, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 切片 ---- */

TEST(ExpressionSubscriptTest, SliceBothBounds) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "arr[1:3]");
  expect_slice(expr, CLUX_NODE_LITERAL_IDENTIFIER, true, true);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 省略下界：arr[:3] */
TEST(ExpressionSubscriptTest, SliceOpenLower) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "arr[:3]");
  expect_slice(expr, CLUX_NODE_LITERAL_IDENTIFIER, false, true);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 省略上界：arr[1:] */
TEST(ExpressionSubscriptTest, SliceOpenUpper) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "arr[1:]");
  expect_slice(expr, CLUX_NODE_LITERAL_IDENTIFIER, true, false);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 两端都省略：arr[:]（[N]T → []T 的唯一显式转换路径） */
TEST(ExpressionSubscriptTest, SliceBothOpen) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "arr[:]");
  expect_slice(expr, CLUX_NODE_LITERAL_IDENTIFIER, false, false);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 边界可以是表达式。 */
TEST(ExpressionSubscriptTest, SliceExpressionBounds) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "arr[i + 1:j * 2]");
  expect_slice(expr, CLUX_NODE_LITERAL_IDENTIFIER, true, true);
  node_t *lo = node_get_child(expr, CLUX_EXPR_SLICE_LO);
  EXPECT_EQ(node_get_kind(lo), CLUX_NODE_EXPRESSION_BINARY);
  node_t *hi = node_get_child(expr, CLUX_EXPR_SLICE_HI);
  EXPECT_EQ(node_get_kind(hi), CLUX_NODE_EXPRESSION_BINARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 切片链：a[1:][0] => (a[1:])[0] */
TEST(ExpressionSubscriptTest, SliceThenSubscript) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a[1:][0]");
  ASSERT_NE(expr, nullptr);
  expect_subscript(expr, CLUX_NODE_EXPRESSION_SLICE, 1);
  node_t *base = node_get_child(expr, CLUX_EXPR_SUBSCRIPT_BASE);
  expect_slice(base, CLUX_NODE_LITERAL_IDENTIFIER, true, false);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

/* 缺第一个实参：a[] */
TEST(ExpressionSubscriptTest, EmptySubscriptReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a[]");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_SUBSCRIPT);
  /* 只有 BASE，没有实参 */
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_SUBSCRIPT_FIRST_ARG);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "expected expression in subscript");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 逗号后缺实参：a[i,] */
TEST(ExpressionSubscriptTest, TrailingCommaReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a[i,]");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_SUBSCRIPT);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_SUBSCRIPT_FIRST_ARG + 1);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "expected expression after ','");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺右方括号：a[i */
TEST(ExpressionSubscriptTest, MissingCloseBracketReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a[i");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_SUBSCRIPT);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "expected ']' to close subscript");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 切片缺右方括号：a[1:2 */
TEST(ExpressionSubscriptTest, SliceMissingCloseBracketReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a[1:2");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_SLICE);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "expected ']' to close slice");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionSubscriptTest, ValidSubscriptProducesNoError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a[i]");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 优先级 ---- */

/* 下标优先级高于所有二元层：a * b[i] => a * (b[i]) */
TEST(ExpressionSubscriptTest, SubscriptBindsTighterThanBinary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a * b[i]");
  node_t *expr = parser_read_expression_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_subscript(right, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 下标优先级高于前缀一元：-a[i] => -(a[i]) */
TEST(ExpressionSubscriptTest, SubscriptBindsTighterThanPrefixUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-a[i]");
  node_t *expr = parser_read_expression_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPRESSION_SUBSCRIPT);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

TEST(ExpressionSubscriptTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a[i]";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "a[i]");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionSubscriptTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  a  [  i  ]  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expression_postfix(a, p);
  expect_location_span(expr, "  a  [  i  ]  ", "a  [  i  ]");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionSubscriptTest, SliceSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a[1:2]";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "a[1:2]");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionSubscriptTest, ChildrenParentIsNotAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a[i]");
  ASSERT_NE(expr, nullptr);
  node_t *base = node_get_child(expr, CLUX_EXPR_SUBSCRIPT_BASE);
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(node_get_parent(base), nullptr);
  node_t *arg = node_get_child(expr, CLUX_EXPR_SUBSCRIPT_FIRST_ARG);
  ASSERT_NE(arg, nullptr);
  EXPECT_EQ(node_get_parent(arg), nullptr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionSubscriptTest, NoLeakOnSubscript) {
  const char *sources[] = {
      "a[i]",    "a[0]",   "a[i + 1]", "Vec[T, 4]",       "T[a, b, c]",
      "a[i][j]", "a.b[i]", "a[i].b",   "a [ /* c */ i ]", "a[1:3]",
      "a[:3]",   "a[1:]",  "a[:]",     "a[i + 1:j * 2]",  "a[1:][0]",
      "a[]",     "a[i,]",  "a[i",      "a[1:2",           "a * b[i]",
      "",        nullptr};
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

/* ================= rest 表达式 =================
   ...expr?，不在优先级链上；孤立 `...` 无子节点。 */

static node_t *parse_rest(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_expression_rest(a, *p);
}

TEST(ExpressionRestTest, BareRestHasNoChildren) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_rest(a, &p, "...");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_REST);
  EXPECT_EQ(vec_get_size(node_get_children(expr)), 0u);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionRestTest, RestWithOperand) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_rest(a, &p, "...args");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_REST);
  ASSERT_EQ(vec_get_size(node_get_children(expr)), 1u);
  node_t *operand = node_get_child(expr, CLUX_EXPR_REST_EXPR);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_kind(operand), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 操作数可以是完整表达式。 */
TEST(ExpressionRestTest, OperandCanBeExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_rest(a, &p, "...a.b");
  ASSERT_NE(expr, nullptr);
  ASSERT_EQ(vec_get_size(node_get_children(expr)), 1u);
  node_t *operand = node_get_child(expr, CLUX_EXPR_REST_EXPR);
  EXPECT_EQ(node_get_kind(operand), CLUX_NODE_EXPRESSION_BINARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 孤立 `...` 后跟 trivia：游标停在 '...' 之后，不吞掉 trivia。 */
TEST(ExpressionRestTest, BareRestDoesNotConsumeTrailingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_rest(a, &p, "...   ");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(vec_get_size(node_get_children(expr)), 0u);
  /* token 序列：'...', whitespace */
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 孤立 `...` 后跟无法解析为表达式的 token：仍是无子节点的 rest。 */
TEST(ExpressionRestTest, BareRestBeforeNonExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_rest(a, &p, "...,");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(vec_get_size(node_get_children(expr)), 0u);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 不是 '...' 开头时返回 NULL，游标不动。 */
TEST(ExpressionRestTest, ReturnsNullWithoutRestToken) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_rest(a, &p, "a"), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionRestTest, ReturnsNullOnEmptyStream) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_rest(a, &p, ""), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

TEST(ExpressionRestTest, BareRestSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "...";
  node_t *expr = parse_rest(a, &p, src);
  expect_location_span(expr, src, "...");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionRestTest, RestWithOperandSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "...args";
  node_t *expr = parse_rest(a, &p, src);
  expect_location_span(expr, src, "...args");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionRestTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  ...  args  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expression_rest(a, p);
  expect_location_span(expr, "  ...  args  ", "...  args");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionRestTest, OperandParentIsNotAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_rest(a, &p, "...args");
  ASSERT_NE(expr, nullptr);
  node_t *operand = node_get_child(expr, CLUX_EXPR_REST_EXPR);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_parent(operand), nullptr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionRestTest, NoLeakOnRest) {
  const char *sources[] = {
      "...", "...args", "...a.b", "...   ", "...a + b", "a", "", nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expression_rest(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 函数调用 =================
   callee(arg, ...)：CALLEE 存下标 0，实参从下标 1 起，数量可变。
   每个实参都可以是 rest 表达式；孤立 `...` 语法合法，由 sema 报错。 */

/* 断言调用节点：CALLEE 为被调用表达式，其后是实参列表。 */
static void
expect_call(node_t *expr, node_kind_t callee_kind, size_t arg_count) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_CALL);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_CALL_FIRST_ARG + arg_count);
  node_t *callee = node_get_child(expr, CLUX_EXPR_CALL_CALLEE);
  ASSERT_NE(callee, nullptr);
  EXPECT_EQ(node_get_kind(callee), callee_kind);
  for (size_t i = 0; i < arg_count; i++) {
    EXPECT_NE(node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG + i), nullptr)
        << "arg=" << i;
  }
}

TEST(ExpressionCallTest, NoArguments) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f()");
  expect_call(expr, CLUX_NODE_LITERAL_IDENTIFIER, 0);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCallTest, SingleArgument) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(a)");
  expect_call(expr, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  node_t *arg = node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG);
  EXPECT_EQ(node_get_kind(arg), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCallTest, MultipleArguments) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(a, b, c)");
  expect_call(expr, CLUX_NODE_LITERAL_IDENTIFIER, 3);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 实参可以是任意表达式。 */
TEST(ExpressionCallTest, ExpressionArguments) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(a + 1, b * 2)");
  expect_call(expr, CLUX_NODE_LITERAL_IDENTIFIER, 2);
  EXPECT_EQ(node_get_kind(node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG)),
            CLUX_NODE_EXPRESSION_BINARY);
  EXPECT_EQ(node_get_kind(node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG + 1)),
            CLUX_NODE_EXPRESSION_BINARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* rest 实参：f(...args) */
TEST(ExpressionCallTest, RestArgument) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(...args)");
  expect_call(expr, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  node_t *arg = node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG);
  ASSERT_EQ(node_get_kind(arg), CLUX_NODE_EXPRESSION_REST);
  ASSERT_EQ(vec_get_size(node_get_children(arg)), 1u);
  EXPECT_EQ(node_get_kind(node_get_child(arg, CLUX_EXPR_REST_EXPR)),
            CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 每个实参都可能是 rest：a(b, ...c, d) */
TEST(ExpressionCallTest, RestAmongArguments) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a(b, ...c, d)");
  expect_call(expr, CLUX_NODE_LITERAL_IDENTIFIER, 3);
  EXPECT_EQ(node_get_kind(node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG)),
            CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(node_get_kind(node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG + 1)),
            CLUX_NODE_EXPRESSION_REST);
  EXPECT_EQ(node_get_kind(node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG + 2)),
            CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 孤立 `...` 作为实参：语法合法，无子节点，语义阶段报错。 */
TEST(ExpressionCallTest, BareRestArgument) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(...)");
  expect_call(expr, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  node_t *arg = node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG);
  ASSERT_EQ(node_get_kind(arg), CLUX_NODE_EXPRESSION_REST);
  EXPECT_EQ(vec_get_size(node_get_children(arg)), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCallTest, BareRestAmongArguments) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(a, ..., b)");
  expect_call(expr, CLUX_NODE_LITERAL_IDENTIFIER, 3);
  node_t *rest = node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG + 1);
  ASSERT_EQ(node_get_kind(rest), CLUX_NODE_EXPRESSION_REST);
  EXPECT_EQ(vec_get_size(node_get_children(rest)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 嵌套调用：f(g(x)) */
TEST(ExpressionCallTest, NestedCall) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(g(x))");
  expect_call(expr, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  node_t *inner = node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG);
  expect_call(inner, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 调用链左结合：f(a)(b) => (f(a))(b) */
TEST(ExpressionCallTest, CallChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(a)(b)");
  expect_call(expr, CLUX_NODE_EXPRESSION_CALL, 1);
  node_t *inner = node_get_child(expr, CLUX_EXPR_CALL_CALLEE);
  expect_call(inner, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCallTest, MemberCallee) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.b(c)");
  expect_call(expr, CLUX_NODE_EXPRESSION_BINARY, 1);
  node_t *callee = node_get_child(expr, CLUX_EXPR_CALL_CALLEE);
  expect_member(callee, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCallTest, SubscriptCallee) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a[i](j)");
  expect_call(expr, CLUX_NODE_EXPRESSION_SUBSCRIPT, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 调用后接成员：f(a).b => (f(a)).b */
TEST(ExpressionCallTest, CallThenMember) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(a).b");
  expect_member(expr, CLUX_NODE_EXPRESSION_CALL, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCallTest, TriviaInsideParens) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f ( /* c */ a /* d */ , b )");
  expect_call(expr, CLUX_NODE_LITERAL_IDENTIFIER, 2);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

/* 逗号后缺实参：f(a,) */
TEST(ExpressionCallTest, TrailingCommaReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(a,)");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_CALL);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_CALL_FIRST_ARG + 1);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "expected expression in call");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺右括号：f(a */
TEST(ExpressionCallTest, MissingCloseParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(a");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_CALL);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "expected ')' to close call");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 空实参列表未闭合：f( —— 缺实参与缺右括号各报一次。 */
TEST(ExpressionCallTest, UnclosedEmptyCallReportsBothErrors) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPRESSION_CALL);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 2u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression in call");
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 1)),
               "expected ')' to close call");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 优先级 ---- */

/* 调用优先级高于所有二元层：a * f(b) => a * (f(b)) */
TEST(ExpressionCallTest, CallBindsTighterThanBinary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a * f(b)");
  node_t *expr = parser_read_expression_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_call(right, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 调用优先级高于前缀一元：-f(x) => -(f(x)) */
TEST(ExpressionCallTest, CallBindsTighterThanPrefixUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-f(x)");
  node_t *expr = parser_read_expression_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPRESSION_CALL);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

/* 调用区间 = callee 起点 .. 右括号终点，不含前后 trivia。 */
TEST(ExpressionCallTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "f(a, b)";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "f(a, b)");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCallTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  f  (  a  )  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expression_postfix(a, p);
  expect_location_span(expr, "  f  (  a  )  ", "f  (  a  )");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCallTest, EmptyCallSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "f()";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "f()");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionCallTest, ChildrenParentIsNotAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(a)");
  ASSERT_NE(expr, nullptr);
  node_t *callee = node_get_child(expr, CLUX_EXPR_CALL_CALLEE);
  ASSERT_NE(callee, nullptr);
  EXPECT_EQ(node_get_parent(callee), nullptr);
  node_t *arg = node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG);
  ASSERT_NE(arg, nullptr);
  EXPECT_EQ(node_get_parent(arg), nullptr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionCallTest, NoLeakOnCall) {
  const char *sources[] = {"f()",        "f(a)",
                           "f(a, b, c)", "f(a + 1, b * 2)",
                           "f(...args)", "a(b, ...c, d)",
                           "f(...)",     "f(a, ..., b)",
                           "f(g(x))",    "f(a)(b)",
                           "a.b(c)",     "a[i](j)",
                           "f(a).b",     "f ( /* c */ a , b )",
                           "f(a,)",      "f(a",
                           "f(",         "a * f(b)",
                           "-f(x)",      "",
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
