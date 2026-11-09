#include <gtest/gtest.h>
#include "parser/expression.h"
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
  return parser_read_expr(a, *p);
}

/* 断言 assign 节点的三个子节点形状。 */
static void expect_assign(node_t *expr,
                          const char *id_src,
                          const char *symbol_src,
                          node_kind_t value_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_ASSIGN);
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
  node_t *expr = parser_read_expr(a, p);
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
    EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_ASSIGN) << "op=" << ops[i];
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_ASSIGN);
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

TEST(ExpressionAssignTest, ChildrenParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "x = 1");
  ASSERT_NE(expr, nullptr);
  node_t *id = node_get_child(expr, CLUX_EXPR_ASSIGN_ID);
  ASSERT_NE(id, nullptr);
  EXPECT_EQ(node_get_parent(id), expr);
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
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 三元表达式 ================= */

static node_t *parse_ternary(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_expr_ternary(a, *p);
}

/* 断言三元节点的三个子节点形状。 */
static void expect_ternary(node_t *expr,
                           node_kind_t condition_kind,
                           node_kind_t then_kind,
                           node_kind_t else_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TERNARY);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TERNARY);
  node_t *outer_else = node_get_child(expr, CLUX_EXPR_TERNARY_ELSE);
  ASSERT_NE(outer_else, nullptr);
  EXPECT_EQ(node_get_kind(outer_else), CLUX_NODE_EXPR_TERNARY);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TERNARY);
  node_t *then = node_get_child(expr, CLUX_EXPR_TERNARY_THEN);
  ASSERT_NE(then, nullptr);
  EXPECT_EQ(node_get_kind(then), CLUX_NODE_EXPR_TERNARY);
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

/* 单独的 '?' 现在是 optional 类型前缀，按“缺操作数”容错返回不完整节点。 */
TEST(ExpressionTernaryTest, ReturnsNullOnNonLiteral) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "?");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_ASSIGN);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_ASSIGN);
  node_t *value = node_get_child(expr, CLUX_EXPR_ASSIGN_VALUE);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(node_get_kind(value), CLUX_NODE_EXPR_TERNARY);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TERNARY);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TERNARY);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TERNARY);
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

/* ':' 后是 '?' 时，'?' 作为 optional 类型前缀进入值层，缺操作数报错，
   因此 else 分支是一个不完整的 optional 节点（三元本身不报错）。 */
TEST(ExpressionTernaryTest, MissingElseErrorPointsAtBadToken) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a ? b : ?";
  node_t *expr = parse_ternary(a, &p, src);
  ASSERT_NE(expr, nullptr);
  node_t *else_branch = node_get_child(expr, CLUX_EXPR_TERNARY_ELSE);
  ASSERT_NE(else_branch, nullptr);
  EXPECT_EQ(node_get_child(else_branch, CLUX_EXPR_BINARY_RIGHT), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  /* '?' 已被消费，流已到末尾：错误位置退化为 '?' 之后的零宽区间。 */
  EXPECT_EQ(error_get_location(err)->begin.offset, src + 9);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TERNARY);
  node_t *outer_else = node_get_child(expr, CLUX_EXPR_TERNARY_ELSE);
  ASSERT_NE(outer_else, nullptr);
  EXPECT_EQ(node_get_kind(outer_else), CLUX_NODE_EXPR_TERNARY);
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

TEST(ExpressionTernaryTest, ChildrenParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_ternary(a, &p, "a ? b : c");
  ASSERT_NE(expr, nullptr);
  node_t *condition = node_get_child(expr, CLUX_EXPR_TERNARY_CONDITION);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(node_get_parent(condition), expr);
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
    node_t *expr = parser_read_expr(a, p);
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
  node_t *expr = parser_read_expr(a, p);
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
  node_t *expr = parser_read_expr_ternary(a, p);
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

static const BinaryLevel kBinaryLevels[] = {
    {"Lor", parser_read_expr_lor, "||", "?", true},
    {"Land", parser_read_expr_land, "&&", "||", true},
    {"Or", parser_read_expr_or, "|", "&&", true},
    {"Xor", parser_read_expr_xor, "^", "|", true},
    {"And", parser_read_expr_and, "&", "^", true},
    {"Eq", parser_read_expr_equality, "==", "&", false},
    {"Ne", parser_read_expr_equality, "!=", "&", false},
    {"Extends", parser_read_expr_equality, "extends", "&", false},
    {"Ge", parser_read_expr_relational, ">=", "==", false},
    {"Le", parser_read_expr_relational, "<=", "==", false},
    {"Gt", parser_read_expr_relational, ">", "==", false},
    {"Lt", parser_read_expr_relational, "<", "==", false},
    {"Shl", parser_read_expr_shift, "<<", "<", false},
    {"Shr", parser_read_expr_shift, ">>", "<", false},
    {"Add", parser_read_expr_additive, "+", "<<", false},
    {"Sub", parser_read_expr_additive, "-", "<<", false},
    {"Mul", parser_read_expr_multiplicative, "*", "+", false},
    {"Div", parser_read_expr_multiplicative, "/", "+", false},
    {"Mod", parser_read_expr_multiplicative, "%", "+", false},
    {"As", parser_read_expr_as_is, "as", "*", false},
    {"Is", parser_read_expr_as_is, "is", "*", false},
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
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
  ASSERT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  if (GetParam().right_associative) {
    /* 左是裸标识符，右是嵌套二元节点 */
    node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(node_get_kind(left), CLUX_NODE_LITERAL_IDENTIFIER);
    node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(node_get_kind(right), CLUX_NODE_EXPR_BINARY);
    expect_binary(right,
                  GetParam().op,
                  CLUX_NODE_LITERAL_IDENTIFIER,
                  CLUX_NODE_LITERAL_IDENTIFIER);
  } else {
    /* 左是嵌套二元节点，右是裸标识符 */
    node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(node_get_kind(left), CLUX_NODE_EXPR_BINARY);
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
       注意不能用 parser_read_expr，那一层会把 `a ||= b` 当合法赋值。 */
    node_t *expr = parser_read_expr_lor(a, p);
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
   运算符，* 同时是指针类型前缀，单独出现会进入对应前缀层并按“缺操作数”
   容错，返回 RIGHT 为 NULL 的不完整节点并上报错误；以 '<' 开头的运算符
   会进入元组类型层，返回不完整元组节点并上报错误。 */
TEST_P(ExpressionBinaryTest, ReturnsNullOnNonLiteral) {
  parser_t *p;
  bool is_unary = strcmp(GetParam().op, "+") == 0 ||
                  strcmp(GetParam().op, "-") == 0 ||
                  strcmp(GetParam().op, "*") == 0;
  /* 只有裸 '<' 与组合出的 '<<' 会进入元组层；'<=' 是单个 token。 */
  bool is_tuple =
      strcmp(GetParam().op, "<") == 0 || strcmp(GetParam().op, "<<") == 0;
  if (!is_unary && !is_tuple) {
    EXPECT_EQ(parse(&p, GetParam().op), nullptr);
    EXPECT_EQ(parser_get_position(p), 0u);
    delete_parser(&p);
    return;
  }
  node_t *expr = parse(&p, GetParam().op);
  ASSERT_NE(expr, nullptr);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  if (is_tuple) {
    EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TUPLE);
    delete_node(&expr);
    delete_parser(&p);
    return;
  }
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  node_t *incomplete = expr;
  if (GetParam().right_associative) {
    node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(node_get_kind(right), CLUX_NODE_EXPR_BINARY);
    incomplete = right;
  } else {
    node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(node_get_kind(left), CLUX_NODE_EXPR_BINARY);
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
  node_t *expr = parser_read_expr(allocator_, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_ASSIGN);
  node_t *value = node_get_child(expr, CLUX_EXPR_ASSIGN_VALUE);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(node_get_kind(value), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
}

TEST_P(ExpressionBinaryTest, ChildrenParentIsAutoSet) {
  parser_t *p;
  char src[32];
  snprintf(src, sizeof(src), "a %s b", GetParam().op);
  node_t *expr = parse(&p, src);
  ASSERT_NE(expr, nullptr);
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(left, nullptr);
  EXPECT_EQ(node_get_parent(left), expr);
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

static const PrecedencePair kPrecedencePairs[] = {
    {"LorLand", parser_read_expr_lor, "||", "&&"},
    {"LandOr", parser_read_expr_land, "&&", "|"},
    {"OrXor", parser_read_expr_or, "|", "^"},
    {"XorAnd", parser_read_expr_xor, "^", "&"},
    {"AndEq", parser_read_expr_and, "&", "=="},
    {"EqRel", parser_read_expr_equality, "==", "<"},
    {"RelShift", parser_read_expr_relational, "<", "<<"},
    {"ShiftAdd", parser_read_expr_shift, "<<", "+"},
    {"AddMul", parser_read_expr_additive, "+", "*"},
    {"MulAs", parser_read_expr_multiplicative, "*", "as"},
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
  node_t *expr = parser_read_expr_lor(a, p);
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
    node_t *expr = parser_read_expr_as_is(a, p);
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
  node_t *expr = parser_read_expr_equality(a, p);
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
  node_t *expr = parser_read_expr_as_is(a, p);
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
  node_t *expr = parser_read_expr_multiplicative(a, p);
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
  node_t *expr = parser_read_expr_additive(a, p);
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
  node_t *expr = parser_read_expr_shift(a, p);
  expect_top_operator(expr, ">>");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_top_operator(left, "<<");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* `<<` / `>>` 由 parser 组合两个相邻的 `<` / `>` token。
   中间有空白或注释时不构成移位：a < < b 仍是关系层的一部分。 */
TEST(ExpressionPrecedenceTest, ShiftRequiresAdjacentTokens) {
  const char *sources[] = {"a < < b",
                           "a < /* c */ < b",
                           /* 相邻但不是同一个字符：<+ / >+ 不构成移位 */
                           "a <+ b",
                           "a >+ b",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr_shift(a, p);
    ASSERT_NE(expr, nullptr) << "src=" << sources[i];
    /* 没有移位：移位层原样返回加法的结果（裸标识符）。 */
    EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER)
        << "src=" << sources[i];
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* 组合出的移位运算符节点区间跨越两个字符。 */
TEST(ExpressionPrecedenceTest, ShiftSymbolSpanCoversBothChars) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a << b");
  node_t *expr = parser_read_expr_shift(a, p);
  expect_top_operator(expr, "<<");
  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  const location_t *loc = node_get_location(symbol);
  EXPECT_EQ(loc->end.offset - loc->begin.offset, (ptrdiff_t)2);
  EXPECT_EQ(strncmp(loc->begin.offset, "<<", 2), 0);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* == 与 != 同级，混用按左结合：(a == b) != c */
TEST(ExpressionPrecedenceTest, EqualityOperatorsShareLevel) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a == b != c");
  node_t *expr = parser_read_expr_equality(a, p);
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
  node_t *expr = parser_read_expr_relational(a, p);
  expect_top_operator(expr, ">=");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_top_operator(left, "<");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 最长匹配：>= 不能被拆成 > 后跟 =。
   `<<` / `>>` 不在词法表中，由 parser 组合，见 ShiftOperatorIsCombined。 */
TEST(ExpressionPrecedenceTest, LongestMatchWins) {
  const char *sources[] = {">=", "<=", nullptr};
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
  node_t *expr = parser_read_expr_ternary(a, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TERNARY);
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
  node_t *expr = parser_read_expr_ternary(a, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TERNARY);
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
  return parser_read_expr_unary(a, *p);
}

/* 断言一元节点的形状。 */
static void
expect_unary(node_t *expr, const char *op, node_kind_t operand_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
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
  expect_unary(expr, "!", CLUX_NODE_EXPR_BINARY);
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
  expect_unary(expr, "-", CLUX_NODE_EXPR_BINARY);
  node_t *second = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(second, "~", CLUX_NODE_EXPR_BINARY);
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
    EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
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
  expect_unary(expr, "!", CLUX_NODE_EXPR_BINARY);
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
  node_t *expr = parser_read_expr_multiplicative(a, p);
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
  node_t *expr = parser_read_expr_as_is(a, p);
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
  node_t *expr = parser_read_expr_as_is(a, p);
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
  node_t *expr = parser_read_expr_additive(a, p);
  expect_top_operator(expr, "+");
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(right, "-", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 子节点 parent 未自动挂载 ---- */

TEST(ExpressionUnaryTest, OperandParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_unary(a, &p, "!a");
  ASSERT_NE(expr, nullptr);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_parent(operand), expr);
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
  node_t *expr = parser_read_expr_unary(a, p);
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
    node_t *expr = parser_read_expr(a, p);
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
  return parser_read_expr_postfix(a, *p);
}

/* 断言后缀节点的形状。 */
static void
expect_postfix(node_t *expr, const char *op, node_kind_t operand_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
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

/* `.?` 由 parser 组合 '.' 与 '?' 两个相邻 token，节点区间跨越二者。 */
TEST(ExpressionPostfixTest, TryOperatorSymbolSpanCoversBothChars) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "a.?";
  node_t *expr = parse_postfix(a, &p, src);
  expect_postfix(expr, ".?", CLUX_NODE_LITERAL_IDENTIFIER);
  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  const location_t *loc = node_get_location(symbol);
  EXPECT_EQ(loc->begin.offset, src + 1);
  EXPECT_EQ(loc->end.offset, src + 3);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* '.' 与 '?' 之间有空白时不构成 `.?`：'.' 后不是 identifier，
   成员访问回退，游标停在 '.' 之前。 */
TEST(ExpressionPostfixTest, TryOperatorRequiresAdjacentTokens) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a. ?");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* `.?` 与构造不冲突：构造后的 `.?` 仍是后缀运算符。
   注意 `.T{1}.?` 中第一个 '.' 属于构造，第二个 '.' 与 '?' 组合。 */
TEST(ExpressionPostfixTest, TryOperatorAfterConstruct) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{1}.?");
  expect_postfix(expr, ".?", CLUX_NODE_EXPR_CONSTRUCT);
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
  expect_postfix(expr, ".&", CLUX_NODE_EXPR_BINARY);
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
  while (cur && node_get_kind(cur) == CLUX_NODE_EXPR_BINARY) {
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
  node_t *expr = parser_read_expr_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPR_BINARY);
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
  node_t *expr = parser_read_expr_multiplicative(a, p);
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
  node_t *expr = parser_read_expr_as_is(a, p);
  expect_top_operator(expr, "as");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_postfix(left, ".!", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 子节点 parent 未自动挂载 ---- */

TEST(ExpressionPostfixTest, OperandParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.!");
  ASSERT_NE(expr, nullptr);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_parent(operand), expr);
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
  node_t *expr = parser_read_expr_postfix(a, p);
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
    node_t *expr = parser_read_expr(a, p);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
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
  expect_member(expr, CLUX_NODE_EXPR_BINARY, "c");
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
  while (cur && node_get_kind(cur) == CLUX_NODE_EXPR_BINARY) {
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
  expect_postfix(expr, ".*", CLUX_NODE_EXPR_BINARY);
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
  expect_member(expr, CLUX_NODE_EXPR_BINARY, "b");
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
  node_t *expr = parser_read_expr_multiplicative(a, p);
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
  node_t *expr = parser_read_expr_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPR_BINARY);
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
  node_t *expr = parser_read_expr_postfix(a, p);
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

TEST(ExpressionMemberTest, ChildrenParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a.b");
  ASSERT_NE(expr, nullptr);
  node_t *receiver = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(receiver, nullptr);
  EXPECT_EQ(node_get_parent(receiver), expr);
  node_t *field = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(node_get_parent(field), expr);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
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
  expect_scope(expr, CLUX_NODE_EXPR_BINARY, "c");
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
  expect_member(expr, CLUX_NODE_EXPR_BINARY, "c");
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
  expect_scope(expr, CLUX_NODE_EXPR_BINARY, "c");
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
  expect_postfix(expr, ".*", CLUX_NODE_EXPR_BINARY);
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
  node_t *expr = parser_read_expr_multiplicative(a, p);
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
  node_t *expr = parser_read_expr_postfix(a, p);
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

TEST(ExpressionScopeTest, ChildrenParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a::b");
  ASSERT_NE(expr, nullptr);
  node_t *receiver = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(receiver, nullptr);
  EXPECT_EQ(node_get_parent(receiver), expr);
  node_t *name = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(node_get_parent(name), expr);
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
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 下标 / 实例化 / 切片 =================
   下标与实例化语法同形（base[expr, ...]），语法阶段统一为
   CLUX_NODE_EXPR_SUBSCRIPT；带冒号的切片为 CLUX_NODE_EXPR_SLICE。 */

/* 断言下标/实例化节点：BASE 为接收者，其后是实参列表。 */
static void
expect_subscript(node_t *expr, node_kind_t base_kind, size_t arg_count) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_SUBSCRIPT);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_SLICE);
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
   类型实参（如 i32）词法层是关键字，由 literal_type 读取。 */
TEST(ExpressionSubscriptTest, MultipleArguments) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "Vec[i32, 4]");
  expect_subscript(expr, CLUX_NODE_LITERAL_IDENTIFIER, 2);
  EXPECT_EQ(node_get_kind(node_get_child(expr, CLUX_EXPR_SUBSCRIPT_FIRST_ARG)),
            CLUX_NODE_LITERAL_TYPE);
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
  EXPECT_EQ(node_get_kind(arg), CLUX_NODE_EXPR_BINARY);
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
  expect_subscript(expr, CLUX_NODE_EXPR_SUBSCRIPT, 1);
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
  expect_subscript(expr, CLUX_NODE_EXPR_BINARY, 1);
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
  expect_member(expr, CLUX_NODE_EXPR_SUBSCRIPT, "b");
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
  EXPECT_EQ(node_get_kind(lo), CLUX_NODE_EXPR_BINARY);
  node_t *hi = node_get_child(expr, CLUX_EXPR_SLICE_HI);
  EXPECT_EQ(node_get_kind(hi), CLUX_NODE_EXPR_BINARY);
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
  expect_subscript(expr, CLUX_NODE_EXPR_SLICE, 1);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_SUBSCRIPT);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_SUBSCRIPT);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_SUBSCRIPT);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_SLICE);
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
  node_t *expr = parser_read_expr_multiplicative(a, p);
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
  node_t *expr = parser_read_expr_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPR_SUBSCRIPT);
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
  node_t *expr = parser_read_expr_postfix(a, p);
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

TEST(ExpressionSubscriptTest, ChildrenParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "a[i]");
  ASSERT_NE(expr, nullptr);
  node_t *base = node_get_child(expr, CLUX_EXPR_SUBSCRIPT_BASE);
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(node_get_parent(base), expr);
  node_t *arg = node_get_child(expr, CLUX_EXPR_SUBSCRIPT_FIRST_ARG);
  ASSERT_NE(arg, nullptr);
  EXPECT_EQ(node_get_parent(arg), expr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionSubscriptTest, NoLeakOnSubscript) {
  const char *sources[] = {
      "a[i]",    "a[0]",   "a[i + 1]", "Vec[i32, 4]",     "T[a, b, c]",
      "a[i][j]", "a.b[i]", "a[i].b",   "a [ /* c */ i ]", "a[1:3]",
      "a[:3]",   "a[1:]",  "a[:]",     "a[i + 1:j * 2]",  "a[1:][0]",
      "a[]",     "a[i,]",  "a[i",      "a[1:2",           "a * b[i]",
      "",        nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr(a, p);
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
  return parser_read_expr_rest(a, *p);
}

TEST(ExpressionRestTest, BareRestHasNoChildren) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_rest(a, &p, "...");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_REST);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_REST);
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
  EXPECT_EQ(node_get_kind(operand), CLUX_NODE_EXPR_BINARY);
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
  node_t *expr = parser_read_expr_rest(a, p);
  expect_location_span(expr, "  ...  args  ", "...  args");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionRestTest, OperandParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_rest(a, &p, "...args");
  ASSERT_NE(expr, nullptr);
  node_t *operand = node_get_child(expr, CLUX_EXPR_REST_EXPR);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_parent(operand), expr);
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
    node_t *expr = parser_read_expr_rest(a, p);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_CALL);
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
            CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(node_get_kind(node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG + 1)),
            CLUX_NODE_EXPR_BINARY);
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
  ASSERT_EQ(node_get_kind(arg), CLUX_NODE_EXPR_REST);
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
            CLUX_NODE_EXPR_REST);
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
  ASSERT_EQ(node_get_kind(arg), CLUX_NODE_EXPR_REST);
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
  ASSERT_EQ(node_get_kind(rest), CLUX_NODE_EXPR_REST);
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
  expect_call(expr, CLUX_NODE_EXPR_CALL, 1);
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
  expect_call(expr, CLUX_NODE_EXPR_BINARY, 1);
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
  expect_call(expr, CLUX_NODE_EXPR_SUBSCRIPT, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 调用后接成员：f(a).b => (f(a)).b */
TEST(ExpressionCallTest, CallThenMember) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(a).b");
  expect_member(expr, CLUX_NODE_EXPR_CALL, "b");
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_CALL);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_CALL);
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
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_CALL);
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
  node_t *expr = parser_read_expr_multiplicative(a, p);
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
  node_t *expr = parser_read_expr_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPR_CALL);
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
  node_t *expr = parser_read_expr_postfix(a, p);
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

TEST(ExpressionCallTest, ChildrenParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f(a)");
  ASSERT_NE(expr, nullptr);
  node_t *callee = node_get_child(expr, CLUX_EXPR_CALL_CALLEE);
  ASSERT_NE(callee, nullptr);
  EXPECT_EQ(node_get_parent(callee), expr);
  node_t *arg = node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG);
  ASSERT_NE(arg, nullptr);
  EXPECT_EQ(node_get_parent(arg), expr);
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
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 构造表达式 =================
   .type?{ elem, ... }：TYPE 存下标 0（匿名构造为 NULL），元素从下标 1 起。
   语法阶段类型就是普通表达式，按后缀层解析，与成员/调用/命名空间访问同级；
   元素一律是表达式或 rest 表达式。 */

/* 断言构造节点：type_kind 为 CLUX_NODE_SYMBOL 之外的任意节点类型；
   has_type 为 false 时 TYPE 必须为 NULL。 */
static void expect_construct(node_t *expr,
                             bool has_type,
                             node_kind_t type_kind,
                             size_t elem_count) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_CONSTRUCT);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_CONSTRUCT_FIRST_ELEM + elem_count);
  node_t *type = node_get_child(expr, CLUX_EXPR_CONSTRUCT_TYPE);
  if (has_type) {
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(node_get_kind(type), type_kind);
  } else {
    EXPECT_EQ(type, nullptr);
  }
  for (size_t i = 0; i < elem_count; i++) {
    EXPECT_NE(node_get_child(expr, CLUX_EXPR_CONSTRUCT_FIRST_ELEM + i), nullptr)
        << "elem=" << i;
  }
}

TEST(ExpressionConstructTest, EmptyNamed) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{}");
  expect_construct(expr, true, CLUX_NODE_LITERAL_IDENTIFIER, 0);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionConstructTest, EmptyAnonymous) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".{}");
  expect_construct(expr, false, CLUX_NODE_LITERAL_IDENTIFIER, 0);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionConstructTest, SingleElement) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{1}");
  expect_construct(expr, true, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  EXPECT_EQ(node_get_kind(node_get_child(expr, CLUX_EXPR_CONSTRUCT_FIRST_ELEM)),
            CLUX_NODE_LITERAL_NUMERIC);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionConstructTest, MultipleElements) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{1, \"a\", true}");
  expect_construct(expr, true, CLUX_NODE_LITERAL_IDENTIFIER, 3);
  EXPECT_EQ(node_get_kind(node_get_child(expr, CLUX_EXPR_CONSTRUCT_FIRST_ELEM)),
            CLUX_NODE_LITERAL_NUMERIC);
  EXPECT_EQ(
      node_get_kind(node_get_child(expr, CLUX_EXPR_CONSTRUCT_FIRST_ELEM + 1)),
      CLUX_NODE_LITERAL_STRING);
  EXPECT_EQ(
      node_get_kind(node_get_child(expr, CLUX_EXPR_CONSTRUCT_FIRST_ELEM + 2)),
      CLUX_NODE_LITERAL_BOOLEAN);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 元素可以是任意表达式。 */
TEST(ExpressionConstructTest, ExpressionElements) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{a + 1, f(b)}");
  expect_construct(expr, true, CLUX_NODE_LITERAL_IDENTIFIER, 2);
  EXPECT_EQ(node_get_kind(node_get_child(expr, CLUX_EXPR_CONSTRUCT_FIRST_ELEM)),
            CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(
      node_get_kind(node_get_child(expr, CLUX_EXPR_CONSTRUCT_FIRST_ELEM + 1)),
      CLUX_NODE_EXPR_CALL);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 类型槽是普通表达式：成员访问 / 调用 / 下标 / 命名空间访问均可。 */
TEST(ExpressionConstructTest, TypeCanBeMemberAccess) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".mod.T{1}");
  expect_construct(expr, true, CLUX_NODE_EXPR_BINARY, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionConstructTest, TypeCanBeCall) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".aaa(b){1}");
  expect_construct(expr, true, CLUX_NODE_EXPR_CALL, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionConstructTest, TypeCanBeSubscript) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".Box[T]{1}");
  expect_construct(expr, true, CLUX_NODE_EXPR_SUBSCRIPT, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 类型槽可以是内建类型名（literal_type）。 */
TEST(ExpressionConstructTest, TypeCanBeBuiltinTypeName) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".i32{5}");
  expect_construct(expr, true, CLUX_NODE_LITERAL_TYPE, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 类型槽不限于标识符：字面量在语法阶段同样合法。 */
TEST(ExpressionConstructTest, TypeCanBeNumericLiteral) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".123{}");
  expect_construct(expr, true, CLUX_NODE_LITERAL_NUMERIC, 0);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* rest 元素：.T{...base, 2} */
TEST(ExpressionConstructTest, RestElement) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{...base, 2}");
  expect_construct(expr, true, CLUX_NODE_LITERAL_IDENTIFIER, 2);
  node_t *rest = node_get_child(expr, CLUX_EXPR_CONSTRUCT_FIRST_ELEM);
  ASSERT_EQ(node_get_kind(rest), CLUX_NODE_EXPR_REST);
  ASSERT_EQ(vec_get_size(node_get_children(rest)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 孤立 `...` 元素：语法合法、无子节点，语义阶段报错。 */
TEST(ExpressionConstructTest, BareRestElement) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{...}");
  expect_construct(expr, true, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  node_t *rest = node_get_child(expr, CLUX_EXPR_CONSTRUCT_FIRST_ELEM);
  ASSERT_EQ(node_get_kind(rest), CLUX_NODE_EXPR_REST);
  EXPECT_EQ(vec_get_size(node_get_children(rest)), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 嵌套构造：.T{.U{1}} */
TEST(ExpressionConstructTest, NestedConstruct) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{.U{1}}");
  expect_construct(expr, true, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  node_t *inner = node_get_child(expr, CLUX_EXPR_CONSTRUCT_FIRST_ELEM);
  expect_construct(inner, true, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 构造作为后缀链起点，可继续成员/下标/调用。 */
TEST(ExpressionConstructTest, ConstructThenMember) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{1}.field");
  expect_member(expr, CLUX_NODE_EXPR_CONSTRUCT, "field");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionConstructTest, ConstructThenSubscript) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{1}[0]");
  expect_subscript(expr, CLUX_NODE_EXPR_CONSTRUCT, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionConstructTest, ConstructThenCall) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{1}(x)");
  expect_call(expr, CLUX_NODE_EXPR_CONSTRUCT, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionConstructTest, TriviaInsideBraces) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T { /* c */ 1 /* d */ , 2 }");
  expect_construct(expr, true, CLUX_NODE_LITERAL_IDENTIFIER, 2);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 回退：'.' 后既不是 '{' 也不是可构造表达式时不消费 '.' ---- */

TEST(ExpressionConstructTest, FallsBackOnBareDot) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_postfix(a, &p, "."), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

/* `.a` 没有花括号：不是构造，游标回到 '.' 之前。 */
TEST(ExpressionConstructTest, FallsBackWithoutBrace) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_postfix(a, &p, ".a"), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

/* 元素缺失：.T{,} —— 缺元素与缺右花括号各报一次（与调用 f( 一致）。 */
TEST(ExpressionConstructTest, MissingElementReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{,}");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_CONSTRUCT);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_CONSTRUCT_FIRST_ELEM);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 2u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression in construct");
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 1)),
               "expected '}' to close construct");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 逗号后缺元素：.T{1,} */
TEST(ExpressionConstructTest, TrailingCommaReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{1,}");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_CONSTRUCT_FIRST_ELEM + 1);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression in construct");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺右花括号：.T{1 */
TEST(ExpressionConstructTest, MissingCloseBraceReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{1");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_CONSTRUCT);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '}' to close construct");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 优先级 ---- */

/* 构造优先级高于所有二元层：a + .T{1} => a + (.T{1}) */
TEST(ExpressionConstructTest, ConstructBindsTighterThanBinary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a + .T{1}");
  node_t *expr = parser_read_expr_additive(a, p);
  expect_top_operator(expr, "+");
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_construct(right, true, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 构造优先级高于前缀一元：-.T{1} => -(.T{1}) */
TEST(ExpressionConstructTest, ConstructBindsTighterThanPrefixUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-.T{1}");
  node_t *expr = parser_read_expr_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPR_CONSTRUCT);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

/* 构造区间 = '.' 起点 .. 右花括号终点，不含前后 trivia。 */
TEST(ExpressionConstructTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = ".T{1, 2}";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, ".T{1, 2}");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionConstructTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  .  T  {  1  }  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expr_postfix(a, p);
  expect_location_span(expr, "  .  T  {  1  }  ", ".  T  {  1  }");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionConstructTest, EmptyConstructSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = ".{}";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, ".{}");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionConstructTest, ChildrenParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, ".T{1}");
  ASSERT_NE(expr, nullptr);
  node_t *type = node_get_child(expr, CLUX_EXPR_CONSTRUCT_TYPE);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(node_get_parent(type), expr);
  node_t *elem = node_get_child(expr, CLUX_EXPR_CONSTRUCT_FIRST_ELEM);
  ASSERT_NE(elem, nullptr);
  EXPECT_EQ(node_get_parent(elem), expr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionConstructTest, NoLeakOnConstruct) {
  const char *sources[] = {".{}",
                           ".T{}",
                           ".T{1}",
                           ".T{1, \"a\", true}",
                           ".T{a + 1, f(b)}",
                           ".mod.T{1}",
                           ".aaa(b){1}",
                           ".Box[T]{1}",
                           ".i32{5}",
                           ".123{}",
                           ".T{...base, 2}",
                           ".T{...}",
                           ".T{.U{1}}",
                           ".T{1}.field",
                           ".T{1}[0]",
                           ".T{1}(x)",
                           ".T { /* c */ 1 , 2 }",
                           ".",
                           ".a",
                           ".T{,}",
                           ".T{1,}",
                           ".T{1",
                           "a + .T{1}",
                           "-.T{1}",
                           "",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 值表达式层 =================
   与 literal 平级的构造统一从 parser_read_expr_value 进入。
   目前内部只有 literal 一个代理，后续陆续加入同级的运算符。 */

TEST(ExpressionValueTest, DispatchesEveryLiteralKind) {
  struct Case {
    const char *src;
    node_kind_t kind;
  };
  const Case cases[] = {{"nil", CLUX_NODE_LITERAL_NIL},
                        {"undefined", CLUX_NODE_LITERAL_UNDEFINED},
                        {"true", CLUX_NODE_LITERAL_BOOLEAN},
                        {"i32", CLUX_NODE_LITERAL_TYPE},
                        {"'a'", CLUX_NODE_LITERAL_CHARACTER},
                        {"\"s\"", CLUX_NODE_LITERAL_STRING},
                        {"foo", CLUX_NODE_LITERAL_IDENTIFIER},
                        {"42", CLUX_NODE_LITERAL_NUMERIC}};
  for (const Case &c : cases) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p = create_parser(a, "test.clux", c.src);
    node_t *expr = parser_read_expr_value(a, p);
    ASSERT_NE(expr, nullptr) << "src=" << c.src;
    EXPECT_EQ(node_get_kind(expr), c.kind) << "src=" << c.src;
    EXPECT_EQ(parser_get_position(p), 1u) << "src=" << c.src;
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* 构造与 literal 同级，同样从值层进入。 */
TEST(ExpressionValueTest, DispatchesConstruct) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", ".T{1}");
  node_t *expr = parser_read_expr_value(a, p);
  expect_construct(expr, true, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionValueTest, DispatchesAnonymousConstruct) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", ".{}");
  node_t *expr = parser_read_expr_value(a, p);
  expect_construct(expr, false, CLUX_NODE_LITERAL_IDENTIFIER, 0);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 值层不消费构造之后的后缀：.T{1}.field 的值层结果只是构造节点。 */
TEST(ExpressionValueTest, DoesNotConsumeSuffixAfterConstruct) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", ".T{1}.field");
  node_t *expr = parser_read_expr_value(a, p);
  expect_construct(expr, true, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  EXPECT_EQ(token_get_kind(parser_peek_token(p)), CLUX_TOKEN_SYMBOL);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionValueTest, ReturnsNullOnNonValue) {
  const char *sources[] = {"var", ".", "!", nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    EXPECT_EQ(parser_read_expr_value(a, p), nullptr) << "src=" << sources[i];
    EXPECT_EQ(parser_get_position(p), 0u) << "src=" << sources[i];
    delete_parser(&p);
    delete_allocator(&a);
  }
}

TEST(ExpressionValueTest, ReturnsNullOnEmptyStream) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  EXPECT_EQ(parser_read_expr_value(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 值层不消费后缀：a.b 的值层结果只是裸标识符。 */
TEST(ExpressionValueTest, DoesNotConsumeSuffix) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a.b");
  node_t *expr = parser_read_expr_value(a, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionValueTest, NoLeakOnValue) {
  const char *sources[] = {"nil",
                           "undefined",
                           "true",
                           "i32",
                           "'a'",
                           "\"s\"",
                           "foo",
                           "42i64",
                           ".T{1}",
                           ".{}",
                           ".T{...base, 2}",
                           ".T{1}.field",
                           ".T{",
                           "var",
                           "",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr_value(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 括号表达式 =================
   ( expr )：只有一个子节点，用于改变结合/优先级。
   括号内走顶层入口，语法层不校验是否为合法值。 */

static void expect_group(node_t *expr, node_kind_t inner_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_GROUP);
  EXPECT_EQ(vec_get_size(node_get_children(expr)), 1u);
  node_t *inner = node_get_child(expr, CLUX_EXPR_GROUP_EXPR);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(node_get_kind(inner), inner_kind);
}

TEST(ExpressionGroupTest, WrapsIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "(a)");
  expect_group(expr, CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 括号内是整条表达式链，不只是值层。 */
TEST(ExpressionGroupTest, WrapsBinaryExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "(a + b)");
  expect_group(expr, CLUX_NODE_EXPR_BINARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 语法层允许顶层入口作为子语句：赋值在括号内合法。 */
TEST(ExpressionGroupTest, AllowsAssignmentInside) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "(x = 1)");
  expect_group(expr, CLUX_NODE_EXPR_ASSIGN);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionGroupTest, AllowsTernaryInside) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "(a ? b : c)");
  expect_group(expr, CLUX_NODE_EXPR_TERNARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 嵌套括号：(a) 的括号内还可以是括号。 */
TEST(ExpressionGroupTest, NestedGroups) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "((a))");
  expect_group(expr, CLUX_NODE_EXPR_GROUP);
  node_t *inner = node_get_child(expr, CLUX_EXPR_GROUP_EXPR);
  expect_group(inner, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 括号改变优先级：a * (b + c) 的右操作数是括号节点。 */
TEST(ExpressionGroupTest, ChangesPrecedence) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a * (b + c)");
  node_t *expr = parser_read_expr_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_group(right, CLUX_NODE_EXPR_BINARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 括号是后缀链的起点：(a).b / (a)[i] / (a)(x) */
TEST(ExpressionGroupTest, GroupThenMember) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "(a).b");
  expect_member(expr, CLUX_NODE_EXPR_GROUP, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionGroupTest, GroupThenSubscript) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "(a)[i]");
  expect_subscript(expr, CLUX_NODE_EXPR_GROUP, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionGroupTest, GroupThenCall) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "(a)(x)");
  expect_call(expr, CLUX_NODE_EXPR_GROUP, 1);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 调用实参里的括号：f((a)) 的实参是括号节点。 */
TEST(ExpressionGroupTest, GroupInsideCallArgument) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "f((a))");
  expect_call(expr, CLUX_NODE_LITERAL_IDENTIFIER, 1);
  expect_group(node_get_child(expr, CLUX_EXPR_CALL_FIRST_ARG),
               CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 括号前后 trivia 不属于表达式区间。 */
TEST(ExpressionGroupTest, TriviaInsideParens) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "( /* c */ a /* d */ )");
  expect_group(expr, CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

TEST(ExpressionGroupTest, EmptyParensReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "()");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_GROUP);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_GROUP_EXPR), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression in parentheses");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionGroupTest, MissingCloseParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "(a");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_GROUP);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ')' to close parentheses");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 空括号未闭合：( —— 缺表达式与缺右括号各报一次。 */
TEST(ExpressionGroupTest, UnclosedEmptyParensReportsBothErrors) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "(");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_GROUP);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 2u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression in parentheses");
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 1)),
               "expected ')' to close parentheses");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

/* 区间 = '(' 起点 .. ')' 终点，不含前后 trivia。 */
TEST(ExpressionGroupTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "(a + b)";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "(a + b)");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionGroupTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  (  a  )  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expr_postfix(a, p);
  expect_location_span(expr, "  (  a  )  ", "(  a  )");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionGroupTest, NestedSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "((a))";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "((a))");
  node_t *inner = node_get_child(expr, CLUX_EXPR_GROUP_EXPR);
  expect_location_span(inner, src, "(a)");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionGroupTest, ChildParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "(a)");
  ASSERT_NE(expr, nullptr);
  node_t *inner = node_get_child(expr, CLUX_EXPR_GROUP_EXPR);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(node_get_parent(inner), expr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionGroupTest, NoLeakOnGroup) {
  const char *sources[] = {"(a)",
                           "(a + b)",
                           "(x = 1)",
                           "(a ? b : c)",
                           "((a))",
                           "a * (b + c)",
                           "(a).b",
                           "(a)[i]",
                           "(a)(x)",
                           "f((a))",
                           "( /* c */ a )",
                           "()",
                           "(a",
                           "(",
                           "",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 指针类型前缀 =================
   *<postfix>：复用一元节点形态，LEFT 为 NULL，SYMBOL 存 '*'，RIGHT 存操作数。
   操作数只取后缀层，因此 *a.b 解析为 *(a.b)，*a * b 解析为 (*a) * b。 */

TEST(ExpressionPointerTest, WrapsIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*a");
  expect_unary(expr, "*", CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 操作数取后缀层：*a.b => *(a.b) */
TEST(ExpressionPointerTest, OperandTakesPostfixChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*a.b");
  expect_unary(expr, "*", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_member(operand, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 递归：**T 解析为 *(*T) */
TEST(ExpressionPointerTest, NestedPointers) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "**T");
  expect_unary(expr, "*", CLUX_NODE_EXPR_BINARY);
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(inner, "*", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 类型名作为操作数：*i32 */
TEST(ExpressionPointerTest, WrapsBuiltinTypeName) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*i32");
  expect_unary(expr, "*", CLUX_NODE_LITERAL_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 操作数可以是括号表达式。 */
TEST(ExpressionPointerTest, WrapsGroup) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*(a + b)");
  expect_unary(expr, "*", CLUX_NODE_EXPR_GROUP);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 与乘法不冲突：*a * b => (*a) * b */
TEST(ExpressionPointerTest, DoesNotSwallowMultiplication) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "*a * b");
  node_t *expr = parser_read_expr_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_unary(left, "*", CLUX_NODE_LITERAL_IDENTIFIER);
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(node_get_kind(right), CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* a * *b：右操作数是指针前缀。 */
TEST(ExpressionPointerTest, PointerAsRightOperand) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a * *b");
  node_t *expr = parser_read_expr_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *right = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(right, "*", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 指针优先级高于前缀一元：-*a => -(*a) */
TEST(ExpressionPointerTest, BindsTighterThanPrefixUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-*a");
  node_t *expr = parser_read_expr_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(operand, "*", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 后缀链的起点：*a.b.c => *(a.b.c) */
TEST(ExpressionPointerTest, OperandTakesWholeSuffixChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*a.b.c");
  expect_unary(expr, "*", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_member(operand, CLUX_NODE_EXPR_BINARY, "c");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

/* 缺操作数：单独的 '*' —— 返回不完整节点并上报错误。 */
TEST(ExpressionPointerTest, MissingOperandReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression after '*'");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺操作数时游标回到 '*' 之后，不吞掉后续 trivia。 */
TEST(ExpressionPointerTest, MissingOperandDoesNotConsumeTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*   ");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

/* 区间 = '*' 起点 .. 操作数终点，不含前后 trivia。 */
TEST(ExpressionPointerTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "*a.b";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "*a.b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionPointerTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  *  a  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expr_postfix(a, p);
  expect_location_span(expr, "  *  a  ", "*  a");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionPointerTest, NestedSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "**T";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "**T");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_location_span(inner, src, "*T");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionPointerTest, OperandParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*a");
  ASSERT_NE(expr, nullptr);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_parent(operand), expr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionPointerTest, NoLeakOnPointer) {
  const char *sources[] = {"*a",
                           "*a.b",
                           "**T",
                           "*i32",
                           "*(a + b)",
                           "*a * b",
                           "a * *b",
                           "-*a",
                           "*a.b.c",
                           "*",
                           "*   ",
                           "",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 类型修饰前缀 =================
   const / volatile <postfix>：复用一元节点形态，LEFT 为 NULL，
   SYMBOL 存修饰关键字，RIGHT 存操作数。 */

static void
expect_qualifier(node_t *expr, const char *op, node_kind_t operand_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_BINARY_CHILDREN_SIZE);
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

TEST(ExpressionQualifierTest, ConstWrapsIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "const T");
  expect_qualifier(expr, "const", CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionQualifierTest, VolatileWrapsIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "volatile T");
  expect_qualifier(expr, "volatile", CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionQualifierTest, WrapsBuiltinTypeName) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "const i32");
  expect_qualifier(expr, "const", CLUX_NODE_LITERAL_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 操作数取后缀层：const a.b => const (a.b) */
TEST(ExpressionQualifierTest, OperandTakesPostfixChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "const a.b");
  expect_qualifier(expr, "const", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_member(operand, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 与指针组合：const *T => const (*T) */
TEST(ExpressionQualifierTest, WrapsPointer) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "const *T");
  expect_qualifier(expr, "const", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(operand, "*", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 指针套修饰：*const T => *(const T) */
TEST(ExpressionQualifierTest, PointerWrapsQualifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*const T");
  expect_unary(expr, "*", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_qualifier(operand, "const", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 叠加：const volatile T => const (volatile T) */
TEST(ExpressionQualifierTest, StackedQualifiers) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "const volatile T");
  expect_qualifier(expr, "const", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_qualifier(operand, "volatile", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 修饰不吞乘法：const T * x => (const T) * x */
TEST(ExpressionQualifierTest, DoesNotSwallowMultiplication) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "const T * x");
  node_t *expr = parser_read_expr_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_qualifier(left, "const", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 修饰优先级高于前缀一元：-const T => -(const T) */
TEST(ExpressionQualifierTest, BindsTighterThanPrefixUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-const T");
  node_t *expr = parser_read_expr_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_qualifier(operand, "const", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 修饰可包裹括号：const (a + b) */
TEST(ExpressionQualifierTest, WrapsGroup) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "const (a + b)");
  expect_qualifier(expr, "const", CLUX_NODE_EXPR_GROUP);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

/* 缺操作数：单独的 const —— 返回不完整节点并上报错误。 */
TEST(ExpressionQualifierTest, MissingOperandReportsError) {
  const char *ops[] = {"const", "volatile", nullptr};
  for (size_t i = 0; ops[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p;
    node_t *expr = parse_postfix(a, &p, ops[i]);
    ASSERT_NE(expr, nullptr) << "op=" << ops[i];
    EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
    EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
    EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL), nullptr);
    EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
    vec_t *errors = parser_get_error(p);
    ASSERT_EQ(vec_get_size(errors), 1u) << "op=" << ops[i];
    EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
                 "expected expression after qualifier");
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* 缺操作数时游标回到关键字之后，不吞掉后续 trivia。 */
TEST(ExpressionQualifierTest, MissingOperandDoesNotConsumeTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "const   ");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

/* 区间 = 关键字起点 .. 操作数终点，不含前后 trivia。 */
TEST(ExpressionQualifierTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "const a.b";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "const a.b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionQualifierTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  const   T  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expr_postfix(a, p);
  expect_location_span(expr, "  const   T  ", "const   T");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionQualifierTest, StackedSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "const volatile T";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "const volatile T");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_location_span(inner, src, "volatile T");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionQualifierTest, OperandParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "const T");
  ASSERT_NE(expr, nullptr);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_parent(operand), expr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionQualifierTest, NoLeakOnQualifier) {
  const char *sources[] = {"const T",
                           "volatile T",
                           "const i32",
                           "const a.b",
                           "const *T",
                           "*const T",
                           "const volatile T",
                           "const T * x",
                           "-const T",
                           "const (a + b)",
                           "const",
                           "volatile",
                           "const   ",
                           "",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 切片类型前缀 =================
   []<postfix>：复用一元节点形态，LEFT 为 NULL，SYMBOL 存 '[]'，
   RIGHT 存操作数。 */

static void expect_slice_type(node_t *expr, node_kind_t operand_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_BINARY_CHILDREN_SIZE);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_kind(symbol), CLUX_NODE_SYMBOL);
  EXPECT_EQ(node_get_location(symbol)->end.offset -
                node_get_location(symbol)->begin.offset,
            (ptrdiff_t)2);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_kind(operand), operand_kind);
}

TEST(ExpressionSliceTypeTest, WrapsIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[]T");
  expect_slice_type(expr, CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionSliceTypeTest, WrapsBuiltinTypeName) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[]u8");
  expect_slice_type(expr, CLUX_NODE_LITERAL_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 操作数取后缀层：[]a.b => [](a.b) */
TEST(ExpressionSliceTypeTest, OperandTakesPostfixChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[]a.b");
  expect_slice_type(expr, CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_member(operand, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 嵌套切片：[][]T => []([]T) */
TEST(ExpressionSliceTypeTest, NestedSlices) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[][]T");
  expect_slice_type(expr, CLUX_NODE_EXPR_BINARY);
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_slice_type(inner, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 与指针组合：[]*T => [](*T) */
TEST(ExpressionSliceTypeTest, WrapsPointer) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[]*T");
  expect_slice_type(expr, CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(operand, "*", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 指针套切片：*[]T => *([]T) */
TEST(ExpressionSliceTypeTest, PointerWrapsSlice) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*[]T");
  expect_unary(expr, "*", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_slice_type(operand, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 修饰套切片：[]const u8 => [](const u8) */
TEST(ExpressionSliceTypeTest, WrapsQualifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[]const u8");
  expect_slice_type(expr, CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_qualifier(operand, "const", CLUX_NODE_LITERAL_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 切片不吞乘法：[]T * x => ([]T) * x */
TEST(ExpressionSliceTypeTest, DoesNotSwallowMultiplication) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[]T * x");
  node_t *expr = parser_read_expr_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_slice_type(left, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 切片优先级高于前缀一元：-[]T => -([]T) */
TEST(ExpressionSliceTypeTest, BindsTighterThanPrefixUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-[]T");
  node_t *expr = parser_read_expr_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_slice_type(operand, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* `[` 后不是 `]` 时不构成切片类型，交给数组类型层处理：
   结果不是切片形态（LEFT 非 NULL，存的是长度）。 */
TEST(ExpressionSliceTypeTest, FallsBackOnNonEmptyBrackets) {
  const char *sources[] = {"[T]", "[4]i32", nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr_value(a, p);
    ASSERT_NE(expr, nullptr) << "src=" << sources[i];
    EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr)
        << "src=" << sources[i];
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* ---- 错误路径 ---- */

/* 缺操作数：单独的 [] —— 返回不完整节点并上报错误。 */
TEST(ExpressionSliceTypeTest, MissingOperandReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[]");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression after '[]'");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺操作数时游标回到 ']' 之后，不吞掉后续 trivia。 */
TEST(ExpressionSliceTypeTest, MissingOperandDoesNotConsumeTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[]   ");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(parser_get_position(p), 2u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

/* 区间 = '[' 起点 .. 操作数终点，不含前后 trivia。 */
TEST(ExpressionSliceTypeTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "[]a.b";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "[]a.b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionSliceTypeTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  []   T  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expr_postfix(a, p);
  expect_location_span(expr, "  []   T  ", "[]   T");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionSliceTypeTest, NestedSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "[][]T";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "[][]T");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_location_span(inner, src, "[]T");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionSliceTypeTest, OperandParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[]T");
  ASSERT_NE(expr, nullptr);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_parent(operand), expr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionSliceTypeTest, NoLeakOnSliceType) {
  const char *sources[] = {"[]T",
                           "[]u8",
                           "[]a.b",
                           "[][]T",
                           "[]*T",
                           "*[]T",
                           "[]const u8",
                           "[]T * x",
                           "-[]T",
                           "[]",
                           "[]   ",
                           "[T]",
                           "[",
                           "",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 数组类型前缀 =================
   [<expr>]<postfix>：LEFT 存长度（任意表达式），SYMBOL 存 '[]'，
   RIGHT 存元素。切片 []T 的 LEFT 为 NULL，两者靠 LEFT 是否缺失区分。 */

static void expect_array_type(node_t *expr,
                              node_kind_t length_kind,
                              node_kind_t elem_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_t *length = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(length, nullptr);
  EXPECT_EQ(node_get_kind(length), length_kind);
  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_kind(symbol), CLUX_NODE_SYMBOL);
  node_t *elem = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(elem, nullptr);
  EXPECT_EQ(node_get_kind(elem), elem_kind);
}

TEST(ExpressionArrayTypeTest, NumericLength) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[4]i32");
  expect_array_type(expr, CLUX_NODE_LITERAL_NUMERIC, CLUX_NODE_LITERAL_TYPE);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 长度是任意表达式：走顶层入口，二元运算合法。 */
TEST(ExpressionArrayTypeTest, LengthCanBeExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[N + 1]T");
  expect_array_type(expr, CLUX_NODE_EXPR_BINARY, CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 长度可以是赋值这类顶层表达式。 */
TEST(ExpressionArrayTypeTest, LengthCanBeAssignment) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[n = 4]T");
  expect_array_type(expr, CLUX_NODE_EXPR_ASSIGN, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 长度可以是调用。 */
TEST(ExpressionArrayTypeTest, LengthCanBeCall) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[len(x)]T");
  expect_array_type(expr, CLUX_NODE_EXPR_CALL, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 元素取后缀层：[4]a.b => [4](a.b) */
TEST(ExpressionArrayTypeTest, ElementTakesPostfixChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[4]a.b");
  expect_array_type(expr, CLUX_NODE_LITERAL_NUMERIC, CLUX_NODE_EXPR_BINARY);
  node_t *elem = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_member(elem, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 嵌套数组：[2][3]i32 => [2]([3]i32) */
TEST(ExpressionArrayTypeTest, NestedArrays) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[2][3]i32");
  expect_array_type(expr, CLUX_NODE_LITERAL_NUMERIC, CLUX_NODE_EXPR_BINARY);
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_array_type(inner, CLUX_NODE_LITERAL_NUMERIC, CLUX_NODE_LITERAL_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 数组套切片：[4][]u8 => [4]([]u8) */
TEST(ExpressionArrayTypeTest, ArrayOfSlice) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[4][]u8");
  expect_array_type(expr, CLUX_NODE_LITERAL_NUMERIC, CLUX_NODE_EXPR_BINARY);
  node_t *elem = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  EXPECT_EQ(node_get_child(elem, CLUX_EXPR_BINARY_LEFT), nullptr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 切片套数组：[] [4]u8 => []([4]u8) */
TEST(ExpressionArrayTypeTest, SliceOfArray) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[][4]u8");
  expect_slice_type(expr, CLUX_NODE_EXPR_BINARY);
  node_t *elem = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_array_type(elem, CLUX_NODE_LITERAL_NUMERIC, CLUX_NODE_LITERAL_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 与指针组合：[4]*T => [4](*T) */
TEST(ExpressionArrayTypeTest, ArrayOfPointer) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[4]*T");
  expect_array_type(expr, CLUX_NODE_LITERAL_NUMERIC, CLUX_NODE_EXPR_BINARY);
  node_t *elem = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(elem, "*", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 指针套数组：*[4]T => *([4]T) */
TEST(ExpressionArrayTypeTest, PointerToArray) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*[4]T");
  expect_unary(expr, "*", CLUX_NODE_EXPR_BINARY);
  node_t *elem = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_array_type(
      elem, CLUX_NODE_LITERAL_NUMERIC, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 数组套修饰：[4]const u8 => [4](const u8) */
TEST(ExpressionArrayTypeTest, ArrayOfQualified) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[4]const u8");
  expect_array_type(expr, CLUX_NODE_LITERAL_NUMERIC, CLUX_NODE_EXPR_BINARY);
  node_t *elem = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_qualifier(elem, "const", CLUX_NODE_LITERAL_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 数组类型不吞乘法：[4]T * x => ([4]T) * x */
TEST(ExpressionArrayTypeTest, DoesNotSwallowMultiplication) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[4]T * x");
  node_t *expr = parser_read_expr_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_array_type(
      left, CLUX_NODE_LITERAL_NUMERIC, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 数组类型优先级高于前缀一元：-[4]T => -([4]T) */
TEST(ExpressionArrayTypeTest, BindsTighterThanPrefixUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-[4]T");
  node_t *expr = parser_read_expr_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_array_type(
      operand, CLUX_NODE_LITERAL_NUMERIC, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* [] 仍是切片类型，不被数组层吞掉。 */
TEST(ExpressionArrayTypeTest, EmptyBracketsStaySlice) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[]T");
  expect_slice_type(expr, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

/* 缺长度：[ ]T => 长度缺失，仍返回不完整节点。 */
TEST(ExpressionArrayTypeTest, MissingLengthReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[=]T");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression in array length");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺右方括号：[4T */
TEST(ExpressionArrayTypeTest, MissingCloseBracketReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[4T");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ']' to close array type");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺元素：[4] */
TEST(ExpressionArrayTypeTest, MissingElementReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[4]");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression after array type");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺元素时游标回到 ']' 之后，不吞掉后续 trivia。 */
TEST(ExpressionArrayTypeTest, MissingElementDoesNotConsumeTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[4]   ");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(parser_get_position(p), 3u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

/* 区间 = '[' 起点 .. 元素终点，不含前后 trivia。 */
TEST(ExpressionArrayTypeTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "[4]a.b";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "[4]a.b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionArrayTypeTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  [ 4 ]   T  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expr_postfix(a, p);
  expect_location_span(expr, "  [ 4 ]   T  ", "[ 4 ]   T");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionArrayTypeTest, NestedSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "[2][3]i32";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "[2][3]i32");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_location_span(inner, src, "[3]i32");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionArrayTypeTest, ChildrenParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "[4]T");
  ASSERT_NE(expr, nullptr);
  node_t *length = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(length, nullptr);
  EXPECT_EQ(node_get_parent(length), expr);
  node_t *elem = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(elem, nullptr);
  EXPECT_EQ(node_get_parent(elem), expr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionArrayTypeTest, NoLeakOnArrayType) {
  const char *sources[] = {"[4]i32", "[N + 1]T",  "[n = 4]T",    "[len(x)]T",
                           "[4]a.b", "[2][3]i32", "[4][]u8",     "[][4]u8",
                           "[4]*T",  "*[4]T",     "[4]const u8", "[4]T * x",
                           "-[4]T",  "[]T",       "[=]T",        "[4T",
                           "[4]",    "[4]   ",    "[",           "",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 元组类型 =================
   <T, U, ...>：元素从下标 0 起，数量可变，可空，可含 rest 表达式。 */

static void expect_tuple(node_t *expr, size_t elem_count) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TUPLE);
  EXPECT_EQ(vec_get_size(node_get_children(expr)), elem_count);
  for (size_t i = 0; i < elem_count; i++) {
    EXPECT_NE(node_get_child(expr, i), nullptr) << "elem=" << i;
  }
}

TEST(ExpressionTupleTypeTest, EmptyTuple) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<>");
  expect_tuple(expr, 0);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTupleTypeTest, SingleElement) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<i32>");
  expect_tuple(expr, 1);
  EXPECT_EQ(node_get_kind(node_get_child(expr, 0)), CLUX_NODE_LITERAL_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTupleTypeTest, MultipleElements) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<i32, str, bool>");
  expect_tuple(expr, 3);
  EXPECT_EQ(node_get_kind(node_get_child(expr, 0)), CLUX_NODE_LITERAL_TYPE);
  EXPECT_EQ(node_get_kind(node_get_child(expr, 1)), CLUX_NODE_LITERAL_TYPE);
  EXPECT_EQ(node_get_kind(node_get_child(expr, 2)), CLUX_NODE_LITERAL_TYPE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 元素取后缀层：<a.b> => <(a.b)> */
TEST(ExpressionTupleTypeTest, ElementsTakePostfixChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<a.b, c.d>");
  expect_tuple(expr, 2);
  EXPECT_EQ(node_get_kind(node_get_child(expr, 0)), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(node_get_kind(node_get_child(expr, 1)), CLUX_NODE_EXPR_BINARY);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* rest 元素：<...Ts> */
TEST(ExpressionTupleTypeTest, RestElement) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<...Ts>");
  expect_tuple(expr, 1);
  node_t *rest = node_get_child(expr, 0);
  ASSERT_EQ(node_get_kind(rest), CLUX_NODE_EXPR_REST);
  ASSERT_EQ(vec_get_size(node_get_children(rest)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* rest 与普通元素混用：<i32, ...Ts, bool> */
TEST(ExpressionTupleTypeTest, RestAmongElements) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<i32, ...Ts, bool>");
  expect_tuple(expr, 3);
  EXPECT_EQ(node_get_kind(node_get_child(expr, 0)), CLUX_NODE_LITERAL_TYPE);
  EXPECT_EQ(node_get_kind(node_get_child(expr, 1)), CLUX_NODE_EXPR_REST);
  EXPECT_EQ(node_get_kind(node_get_child(expr, 2)), CLUX_NODE_LITERAL_TYPE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 孤立 `...` 元素：语法合法、无子节点，语义阶段报错。 */
TEST(ExpressionTupleTypeTest, BareRestElement) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<...>");
  expect_tuple(expr, 1);
  node_t *rest = node_get_child(expr, 0);
  ASSERT_EQ(node_get_kind(rest), CLUX_NODE_EXPR_REST);
  EXPECT_EQ(vec_get_size(node_get_children(rest)), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 嵌套元组：<<a, b>, c> —— 逐层各消费一个 '>'。 */
TEST(ExpressionTupleTypeTest, NestedTuple) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<<a, b>, c>");
  expect_tuple(expr, 2);
  node_t *inner = node_get_child(expr, 0);
  expect_tuple(inner, 2);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 元素可以是其它类型前缀：<*T, []u8, [4]i32> */
TEST(ExpressionTupleTypeTest, ElementsCanBeTypePrefixes) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<*T, []u8, [4]i32>");
  expect_tuple(expr, 3);
  node_t *ptr = node_get_child(expr, 0);
  expect_unary(ptr, "*", CLUX_NODE_LITERAL_IDENTIFIER);
  node_t *slice = node_get_child(expr, 1);
  expect_slice_type(slice, CLUX_NODE_LITERAL_TYPE);
  node_t *array = node_get_child(expr, 2);
  expect_array_type(array, CLUX_NODE_LITERAL_NUMERIC, CLUX_NODE_LITERAL_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 元组不吞乘法：<a, b> * x => (<a, b>) * x */
TEST(ExpressionTupleTypeTest, DoesNotSwallowMultiplication) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "<a, b> * x");
  node_t *expr = parser_read_expr_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_tuple(left, 2);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 元组优先级高于前缀一元：-<a> => -(<a>) */
TEST(ExpressionTupleTypeTest, BindsTighterThanPrefixUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-<a>");
  node_t *expr = parser_read_expr_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPR_TUPLE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 元组作为其它类型前缀的操作数：*<a, b> => *(<a, b>) */
TEST(ExpressionTupleTypeTest, TupleAsPointerOperand) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*<a, b>");
  expect_unary(expr, "*", CLUX_NODE_EXPR_TUPLE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 元组前后 trivia 不属于表达式区间。 */
TEST(ExpressionTupleTypeTest, TriviaInsideAngles) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "< /* c */ a /* d */ , b >");
  expect_tuple(expr, 2);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

/* 元素缺失：<,> —— 缺元素与缺右尖括号各报一次。 */
TEST(ExpressionTupleTypeTest, MissingElementReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<,>");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TUPLE);
  EXPECT_EQ(vec_get_size(node_get_children(expr)), 0u);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 2u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression in tuple type");
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 1)),
               "expected '>' to close tuple type");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 逗号后缺元素：<a,> */
TEST(ExpressionTupleTypeTest, TrailingCommaReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<a,>");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(vec_get_size(node_get_children(expr)), 1u);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression in tuple type");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺右尖括号：<a */
TEST(ExpressionTupleTypeTest, MissingCloseReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<a");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TUPLE);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '>' to close tuple type");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

/* 区间 = '<' 起点 .. '>' 终点，不含前后 trivia。 */
TEST(ExpressionTupleTypeTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "<a, b>";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "<a, b>");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTupleTypeTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  < a >  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expr_postfix(a, p);
  expect_location_span(expr, "  < a >  ", "< a >");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTupleTypeTest, EmptyTupleSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "<>";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "<>");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTupleTypeTest, NestedSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "<<a, b>, c>";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "<<a, b>, c>");
  node_t *inner = node_get_child(expr, 0);
  expect_location_span(inner, src, "<a, b>");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionTupleTypeTest, ChildrenParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "<a, b>");
  ASSERT_NE(expr, nullptr);
  for (size_t i = 0; i < 2; i++) {
    node_t *elem = node_get_child(expr, i);
    ASSERT_NE(elem, nullptr);
    EXPECT_EQ(node_get_parent(elem), expr) << "elem=" << i;
  }
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionTupleTypeTest, NoLeakOnTupleType) {
  const char *sources[] = {"<>",
                           "<i32>",
                           "<i32, str, bool>",
                           "<a.b, c.d>",
                           "<...Ts>",
                           "<i32, ...Ts, bool>",
                           "<...>",
                           "<<a, b>, c>",
                           "<*T, []u8, [4]i32>",
                           "<a, b> * x",
                           "-<a>",
                           "*<a, b>",
                           "< /* c */ a , b >",
                           "<,>",
                           "<a,>",
                           "<a",
                           "<",
                           "",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= optional 类型前缀 =================
   ?<postfix>：复用一元节点形态，LEFT 为 NULL，SYMBOL 存 '?'，RIGHT 存操作数。 */

static void expect_optional_type(node_t *expr, node_kind_t operand_kind) {
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_BINARY_CHILDREN_SIZE);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_kind(symbol), CLUX_NODE_SYMBOL);
  EXPECT_EQ(node_get_location(symbol)->end.offset -
                node_get_location(symbol)->begin.offset,
            (ptrdiff_t)1);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_kind(operand), operand_kind);
}

TEST(ExpressionOptionalTypeTest, WrapsIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "?T");
  expect_optional_type(expr, CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionOptionalTypeTest, WrapsBuiltinTypeName) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "?i32");
  expect_optional_type(expr, CLUX_NODE_LITERAL_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 操作数取后缀层：?a.b => ?(a.b) */
TEST(ExpressionOptionalTypeTest, OperandTakesPostfixChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "?a.b");
  expect_optional_type(expr, CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_member(operand, CLUX_NODE_LITERAL_IDENTIFIER, "b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 叠加：??T => ?(?T) */
TEST(ExpressionOptionalTypeTest, NestedOptionals) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "??T");
  expect_optional_type(expr, CLUX_NODE_EXPR_BINARY);
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_optional_type(inner, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 与指针组合：?*T => ?(*T) */
TEST(ExpressionOptionalTypeTest, WrapsPointer) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "?*T");
  expect_optional_type(expr, CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_unary(operand, "*", CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 指针套 optional：*?T => *(?T) */
TEST(ExpressionOptionalTypeTest, PointerWrapsOptional) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "*?T");
  expect_unary(expr, "*", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_optional_type(operand, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 与切片组合：?[]u8 => ?([]u8) */
TEST(ExpressionOptionalTypeTest, WrapsSlice) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "?[]u8");
  expect_optional_type(expr, CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_slice_type(operand, CLUX_NODE_LITERAL_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 与修饰组合：?const u8 => ?(const u8) */
TEST(ExpressionOptionalTypeTest, WrapsQualifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "?const u8");
  expect_optional_type(expr, CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_qualifier(operand, "const", CLUX_NODE_LITERAL_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* optional 不吞乘法：?T * x => (?T) * x */
TEST(ExpressionOptionalTypeTest, DoesNotSwallowMultiplication) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "?T * x");
  node_t *expr = parser_read_expr_multiplicative(a, p);
  expect_top_operator(expr, "*");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  expect_optional_type(left, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* optional 优先级高于前缀一元：-?T => -(?T) */
TEST(ExpressionOptionalTypeTest, BindsTighterThanPrefixUnary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "-?T");
  node_t *expr = parser_read_expr_unary(a, p);
  expect_unary(expr, "-", CLUX_NODE_EXPR_BINARY);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_optional_type(operand, CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 与三元不冲突：a ? b : c 中的 '?' 在左操作数之后，由三元层消费。 */
TEST(ExpressionOptionalTypeTest, DoesNotConflictWithTernary) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a ? b : c");
  node_t *expr = parser_read_expr_ternary(a, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TERNARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 三元分支里可以是 optional 类型：a ? ?T : ?U */
TEST(ExpressionOptionalTypeTest, OptionalInsideTernaryBranch) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a ? ?T : ?U");
  node_t *expr = parser_read_expr_ternary(a, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TERNARY);
  node_t *then_branch = node_get_child(expr, CLUX_EXPR_TERNARY_THEN);
  expect_optional_type(then_branch, CLUX_NODE_LITERAL_IDENTIFIER);
  node_t *else_branch = node_get_child(expr, CLUX_EXPR_TERNARY_ELSE);
  expect_optional_type(else_branch, CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

/* 缺操作数：单独的 '?' */
TEST(ExpressionOptionalTypeTest, MissingOperandReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "?");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_LEFT), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression after '?'");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺操作数时游标回到 '?' 之后，不吞掉后续 trivia。 */
TEST(ExpressionOptionalTypeTest, MissingOperandDoesNotConsumeTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "?   ");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- location ---- */

/* 区间 = '?' 起点 .. 操作数终点，不含前后 trivia。 */
TEST(ExpressionOptionalTypeTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "?a.b";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "?a.b");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionOptionalTypeTest, SpanWithoutSurroundingTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  ?   T  ");
  parser_skip_trivia(p);
  node_t *expr = parser_read_expr_postfix(a, p);
  expect_location_span(expr, "  ?   T  ", "?   T");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionOptionalTypeTest, NestedSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "??T";
  node_t *expr = parse_postfix(a, &p, src);
  expect_location_span(expr, src, "??T");
  node_t *inner = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  expect_location_span(inner, src, "?T");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parent ---- */

TEST(ExpressionOptionalTypeTest, OperandParentIsAutoSet) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "?T");
  ASSERT_NE(expr, nullptr);
  node_t *operand = node_get_child(expr, CLUX_EXPR_BINARY_RIGHT);
  ASSERT_NE(operand, nullptr);
  EXPECT_EQ(node_get_parent(operand), expr);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 泄漏检查 ---- */

TEST(ExpressionOptionalTypeTest, NoLeakOnOptionalType) {
  const char *sources[] = {"?T",
                           "?i32",
                           "?a.b",
                           "??T",
                           "?*T",
                           "*?T",
                           "?[]u8",
                           "?const u8",
                           "?T * x",
                           "-?T",
                           "a ? b : c",
                           "a ? ?T : ?U",
                           "?",
                           "?   ",
                           "",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 逻辑或的相邻组合 =================
   `||` 不在词法表中（与空捕获列表冲突），由 parser 组合两个相邻的 '|'。 */

/* `a || b` 解析为逻辑或，运算符节点区间跨越两个 '|'。 */
TEST(ExpressionLorTest, CombinesAdjacentPipes) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "a || b");
  expect_top_operator(expr, "||");
  node_t *symbol = node_get_child(expr, CLUX_EXPR_BINARY_SYMBOL);
  ASSERT_NE(symbol, nullptr);
  EXPECT_EQ(node_get_location(symbol)->end.offset -
                node_get_location(symbol)->begin.offset,
            (ptrdiff_t)2);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* `a | | b`（中间有空白）不是逻辑或：两个 '|' 各自是位或，无法解析右值。 */
TEST(ExpressionLorTest, SeparatedPipesAreNotLor) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "a | | b");
  ASSERT_NE(expr, nullptr);
  /* 顶层是位或，不是逻辑或。 */
  expect_top_operator(expr, "|");
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* `a ||| b`：前两个 '|' 组合成逻辑或，右操作数从第三个 '|' 起解析失败，
   因此顶层是逻辑或且 RIGHT 缺失，同时报错。 */
TEST(ExpressionLorTest, TriplePipeReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "a ||| b");
  ASSERT_NE(expr, nullptr);
  expect_top_operator(expr, "||");
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_BINARY_RIGHT), nullptr);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 位或层不能吃掉 `||` 的第一个 '|'。 */
TEST(ExpressionLorTest, BitOrDoesNotConsumeLor) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_expr(a, &p, "a | b || c");
  ASSERT_NE(expr, nullptr);
  expect_top_operator(expr, "||");
  node_t *left = node_get_child(expr, CLUX_EXPR_BINARY_LEFT);
  ASSERT_NE(left, nullptr);
  expect_top_operator(left, "|");
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 函数类型 =================
   func |captures|(params) -> ret（白皮书 §2.7）。
   不是前导可判定的：只有形参列表之后的 '->' 才确认是函数类型，
   否则回退游标、返回 NULL，把 func 留给函数定义。 */

/* 断言函数类型节点形状：形参列表与返回类型必存在。 */
static node_t *expect_func_type(node_t *expr) {
  if (!expr) {
    ADD_FAILURE() << "expected function type node, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_FUNC_TYPE);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_FUNC_TYPE_CHILDREN_SIZE);
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_PARAMS);
  EXPECT_NE(params, nullptr);
  if (params) {
    EXPECT_EQ(node_get_kind(params), CLUX_NODE_FUNC_PARAMS);
  }
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_FUNC_TYPE_RETURN), nullptr);
  return expr;
}

TEST(ExpressionFuncTypeTest, BasicType) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func(i32, str) -> bool");
  expect_func_type(expr);
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_PARAMS);
  EXPECT_EQ(vec_get_size(node_get_children(params)), 2u);
  EXPECT_EQ(node_get_kind(node_get_child(params, 0)), CLUX_NODE_LITERAL_TYPE);
  EXPECT_EQ(node_get_kind(node_get_child(params, 1)), CLUX_NODE_LITERAL_TYPE);
  EXPECT_EQ(node_get_kind(node_get_child(expr, CLUX_EXPR_FUNC_TYPE_RETURN)),
            CLUX_NODE_LITERAL_TYPE);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_FUNC_TYPE_CAPTURES), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionFuncTypeTest, CfuncType) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cfunc(i32) -> i32");
  expect_func_type(expr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionFuncTypeTest, NoParameters) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func() -> void");
  expect_func_type(expr);
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_PARAMS);
  EXPECT_EQ(vec_get_size(node_get_children(params)), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionFuncTypeTest, Captures) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func |a: i32, b: str|(i32) -> i32");
  expect_func_type(expr);
  node_t *captures = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_CAPTURES);
  ASSERT_NE(captures, nullptr);
  EXPECT_EQ(node_get_kind(captures), CLUX_NODE_FUNC_CAPTURES);
  EXPECT_EQ(vec_get_size(node_get_children(captures)), 2u);
  for (size_t i = 0; i < 2; i++) {
    node_t *cap = node_get_child(captures, i);
    ASSERT_NE(cap, nullptr);
    EXPECT_EQ(node_get_kind(cap), CLUX_NODE_FUNC_CAPTURE);
    EXPECT_NE(node_get_child(cap, CLUX_FUNC_CAPTURE_NAME), nullptr);
    EXPECT_NE(node_get_child(cap, CLUX_FUNC_CAPTURE_TYPE), nullptr);
  }
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 空捕获列表写作 `||`（单个 token）。 */
TEST(ExpressionFuncTypeTest, EmptyCaptures) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func ||(i32) -> i32");
  expect_func_type(expr);
  node_t *captures = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_CAPTURES);
  ASSERT_NE(captures, nullptr);
  EXPECT_EQ(node_get_kind(captures), CLUX_NODE_FUNC_CAPTURES);
  EXPECT_EQ(vec_get_size(node_get_children(captures)), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 返回类型本身可以是函数类型。 */
TEST(ExpressionFuncTypeTest, NestedReturnType) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func() -> func() -> i32");
  expect_func_type(expr);
  node_t *ret = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_RETURN);
  ASSERT_NE(ret, nullptr);
  EXPECT_EQ(node_get_kind(ret), CLUX_NODE_EXPR_FUNC_TYPE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 形参类型可以是复合类型（指针、切片等）。 */
TEST(ExpressionFuncTypeTest, CompositeParameterTypes) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func(*u8, []i32) -> bool");
  expect_func_type(expr);
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_PARAMS);
  EXPECT_EQ(vec_get_size(node_get_children(params)), 2u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* C FFI 变参：孤立 `...` 作为最后一个形参。 */
TEST(ExpressionFuncTypeTest, VariadicParameter) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cfunc(i32, ...) -> i32");
  expect_func_type(expr);
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_PARAMS);
  ASSERT_EQ(vec_get_size(node_get_children(params)), 2u);
  EXPECT_EQ(node_get_kind(node_get_child(params, 0)), CLUX_NODE_LITERAL_TYPE);
  node_t *rest = node_get_child(params, 1);
  ASSERT_NE(rest, nullptr);
  EXPECT_EQ(node_get_kind(rest), CLUX_NODE_EXPR_REST);
  /* 孤立 `...` 没有操作数。 */
  EXPECT_EQ(vec_get_size(node_get_children(rest)), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 参数包形参 `...Args`。 */
TEST(ExpressionFuncTypeTest, ParameterPack) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func(...Args) -> void");
  expect_func_type(expr);
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_PARAMS);
  ASSERT_EQ(vec_get_size(node_get_children(params)), 1u);
  node_t *rest = node_get_child(params, 0);
  ASSERT_NE(rest, nullptr);
  EXPECT_EQ(node_get_kind(rest), CLUX_NODE_EXPR_REST);
  ASSERT_EQ(vec_get_size(node_get_children(rest)), 1u);
  EXPECT_EQ(node_get_kind(node_get_child(rest, CLUX_EXPR_REST_EXPR)),
            CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 变参只能出现在末尾；`..., i32` 语法上仍按列表解析，语义阶段再校验。 */
TEST(ExpressionFuncTypeTest, VariadicNotLastIsParsed) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cfunc(..., i32) -> i32");
  expect_func_type(expr);
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_PARAMS);
  EXPECT_EQ(vec_get_size(node_get_children(params)), 2u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 只有变参。 */
TEST(ExpressionFuncTypeTest, OnlyVariadicParameter) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cfunc(...) -> void");
  expect_func_type(expr);
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_PARAMS);
  ASSERT_EQ(vec_get_size(node_get_children(params)), 1u);
  EXPECT_EQ(node_get_kind(node_get_child(params, 0)), CLUX_NODE_EXPR_REST);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 没有 '->' 时不是函数类型：回退到函数声明，按声明规则报错。 */
TEST(ExpressionFuncTypeTest, NotFuncTypeFallsBackToDecl) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "func(x: i32)");
  node_t *expr = parser_read_expr(a, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_FUNC_DECL);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 捕获列表未闭合：不是函数类型，回退到函数声明后报错。 */
TEST(ExpressionFuncTypeTest, UnclosedCapturesFallsBackToDecl) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "func |a: i32(i32) -> i32");
  node_t *expr = parser_read_expr(a, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_FUNC_DECL);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 捕获列表之后缺形参列表：不是函数类型，回退到函数声明后报错。 */
TEST(ExpressionFuncTypeTest, MissingParamsAfterCapturesFallsBackToDecl) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "func |a: i32| -> i32");
  node_t *expr = parser_read_expr(a, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_FUNC_DECL);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 捕获项缺名字。 */
TEST(ExpressionFuncTypeTest, MissingCaptureNameReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func |: i32|(i32) -> i32");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected capture in captures");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 形参里的嵌套括号与方括号计入深度，不会提前闭合。 */
TEST(ExpressionFuncTypeTest, NestedBracketsInParams) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func([4]i32, func() -> i32) -> bool");
  expect_func_type(expr);
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_PARAMS);
  EXPECT_EQ(vec_get_size(node_get_children(params)), 2u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 捕获列表里的嵌套括号同样计入深度。 */
TEST(ExpressionFuncTypeTest, NestedBracketsInCaptures) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func |f: func() -> i32|(i32) -> i32");
  expect_func_type(expr);
  node_t *captures = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_CAPTURES);
  ASSERT_NE(captures, nullptr);
  EXPECT_EQ(vec_get_size(node_get_children(captures)), 1u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 花括号内的 '|' 不应提前闭合捕获列表：struct 等内联类型可能带块，
   块内的 attribute 初始化可能出现 '|'。 */
TEST(ExpressionFuncTypeTest, BraceProtectsPipeInCaptures) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func |a: {|}|(i32) -> i32");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_FUNC_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 花括号内的 ')' 同样不应提前闭合形参列表。 */
TEST(ExpressionFuncTypeTest, BraceProtectsParenInParams) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func({)}) -> i32");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_FUNC_TYPE);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 未闭合的形参列表：不是函数类型，回退到函数声明后报错。 */
TEST(ExpressionFuncTypeTest, UnclosedParamsFallsBackToDecl) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "func(i32 -> i32");
  node_t *expr = parser_read_expr(a, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_FUNC_DECL);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 多余的闭合符号不会让深度回绕；不是函数类型，回退到函数声明后报错。 */
TEST(ExpressionFuncTypeTest, ExtraCloseParenFallsBackToDecl) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "func()) -> i32");
  node_t *expr = parser_read_expr(a, p);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_FUNC_DECL);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 捕获项缺 ':'。 */
TEST(ExpressionFuncTypeTest, MissingCaptureColonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func |a|(i32) -> i32");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ':' after capture name");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 捕获项缺类型。 */
TEST(ExpressionFuncTypeTest, MissingCaptureTypeReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func |a:|(i32) -> i32");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected capture type");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 捕获项之间缺分隔符。 */
TEST(ExpressionFuncTypeTest, MissingCaptureSeparatorReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func |a: i32 b: str|(i32) -> i32");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ',' or '|' in captures");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 形参缺类型。 */
TEST(ExpressionFuncTypeTest, MissingParameterTypeReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func(,) -> i32");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected parameter type in function");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 形参之间缺分隔符。 */
TEST(ExpressionFuncTypeTest, MissingParameterSeparatorReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func(i32 str) -> i32");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ',' or ')' in parameters");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* '->' 之后缺返回类型。 */
TEST(ExpressionFuncTypeTest, MissingReturnTypeReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func() ->");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected return type after '->'");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 func 关键字到返回类型终点。 */
TEST(ExpressionFuncTypeTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "func(i32) -> bool";
  node_t *expr = parse_postfix(a, &p, src);
  expect_func_type(expr);
  const location_t *loc = node_get_location(expr);
  EXPECT_EQ(loc->begin.offset, src);
  EXPECT_EQ(loc->end.offset, src + strlen(src));
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 捕获项区间 = 名字起点 .. 类型终点。 */
TEST(ExpressionFuncTypeTest, CaptureSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "func |a: i32|(i32) -> i32";
  node_t *expr = parse_postfix(a, &p, src);
  node_t *captures = node_get_child(expr, CLUX_EXPR_FUNC_TYPE_CAPTURES);
  ASSERT_NE(captures, nullptr);
  node_t *cap = node_get_child(captures, 0);
  ASSERT_NE(cap, nullptr);
  const location_t *loc = node_get_location(cap);
  EXPECT_EQ(loc->begin.offset, src + strlen("func |"));
  EXPECT_EQ(loc->end.offset, src + strlen("func |a: i32"));
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionFuncTypeTest, NoLeakOnFuncType) {
  const char *sources[] = {"func(i32, str) -> bool",
                           "cfunc(i32) -> i32",
                           "func() -> void",
                           "func |a: i32|(i32) -> i32",
                           "func |a: i32, b: str|(i32) -> i32",
                           "func ||(i32) -> i32",
                           "func() -> func() -> i32",
                           "func(*u8, []i32) -> bool",
                           "func(x: i32)",
                           "func",
                           "func(",
                           "func()",
                           "func() ->",
                           "func |a|(i32) -> i32",
                           "func |a:|(i32) -> i32",
                           "func |a: i32 b: str|(i32) -> i32",
                           "func |a: i32(i32) -> i32",
                           "func |a: i32| -> i32",
                           "func |: i32|(i32) -> i32",
                           "func([4]i32, func() -> i32) -> bool",
                           "func |f: func() -> i32|(i32) -> i32",
                           "func(i32 -> i32",
                           "func()) -> i32",
                           "func |a: {|}|(i32) -> i32",
                           "func({)}) -> i32",
                           "cfunc(i32, ...) -> i32",
                           "func(...Args) -> void",
                           "cfunc(..., i32) -> i32",
                           "cfunc(...) -> void",
                           "func(,) -> i32",
                           "func(i32 str) -> i32",
                           "func(i32",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= 函数声明 / 函数定义 / 闭包 =================
   func_decl 是 func_type 的后退方案：func_type 已确认不是类型时，
   首 token 为 func/cfunc 的构造必然是声明，任何不匹配都是硬错误。
   语法：func |captures| name[generics](params): ret {body} */

/* 断言函数声明节点形状：关键字、形参列表、返回类型必存在。 */
static node_t *expect_func_decl(node_t *expr, const char *keyword) {
  if (!expr) {
    ADD_FAILURE() << "expected function declaration node, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_FUNC_DECL);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_FUNC_DECL_CHILDREN_SIZE);
  node_t *kw = node_get_child(expr, CLUX_EXPR_FUNC_DECL_KEYWORD);
  if (!kw) {
    ADD_FAILURE() << "expected function keyword node";
    return expr;
  }
  EXPECT_EQ(node_get_kind(kw), CLUX_NODE_SYMBOL);
  EXPECT_EQ(node_get_location(kw)->end.offset -
                node_get_location(kw)->begin.offset,
            (ptrdiff_t)strlen(keyword));
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_DECL_PARAMS);
  if (!params) {
    ADD_FAILURE() << "expected function parameter list";
    return expr;
  }
  EXPECT_EQ(node_get_kind(params), CLUX_NODE_FUNC_PARAMS);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_FUNC_DECL_RETURN), nullptr);
  return expr;
}

/* 断言形参列表里第 idx 个形参是 name: type。 */
static void expect_named_param(node_t *params, size_t idx) {
  node_t *param = node_get_child(params, idx);
  ASSERT_NE(param, nullptr);
  EXPECT_EQ(node_get_kind(param), CLUX_NODE_FUNC_PARAM);
  EXPECT_NE(node_get_child(param, CLUX_FUNC_PARAM_NAME), nullptr);
  EXPECT_NE(node_get_child(param, CLUX_FUNC_PARAM_TYPE), nullptr);
  EXPECT_EQ(node_get_child(param, CLUX_FUNC_PARAM_DEFAULT), nullptr);
}

TEST(ExpressionFuncDeclTest, NamedDefinition) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "func foo(a: i32): bool { a; }";
  node_t *expr = parse_postfix(a, &p, src);
  expect_func_decl(expr, "func");
  node_t *name = node_get_child(expr, CLUX_EXPR_FUNC_DECL_NAME);
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(node_get_kind(name), CLUX_NODE_LITERAL_IDENTIFIER);
  expect_named_param(node_get_child(expr, CLUX_EXPR_FUNC_DECL_PARAMS), 0);
  node_t *body = node_get_child(expr, CLUX_EXPR_FUNC_DECL_BODY);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(node_get_kind(body), CLUX_NODE_STMT_BLOCK);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_FUNC_DECL_CAPTURES), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_FUNC_DECL_GENERICS), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 匿名函数值：没有名字。 */
TEST(ExpressionFuncDeclTest, AnonymousDefinition) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func(x: i32): i32 { a; }");
  expect_func_decl(expr, "func");
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_FUNC_DECL_NAME), nullptr);
  expect_named_param(node_get_child(expr, CLUX_EXPR_FUNC_DECL_PARAMS), 0);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_FUNC_DECL_BODY), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionFuncDeclTest, NoParameters) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func f(): void {}");
  expect_func_decl(expr, "func");
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_DECL_PARAMS);
  EXPECT_EQ(vec_get_size(node_get_children(params)), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionFuncDeclTest, MultipleParameters) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func f(a: i32, b: str): bool {}");
  expect_func_decl(expr, "func");
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_DECL_PARAMS);
  ASSERT_EQ(vec_get_size(node_get_children(params)), 2u);
  expect_named_param(params, 0);
  expect_named_param(params, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 函数参数没有默认值：`=` 不被形参层消费，落到外层报错。 */
TEST(ExpressionFuncDeclTest, ParameterDefaultValueIsRejected) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func f(a: i32 = 10): void {}");
  ASSERT_NE(expr, nullptr);
  node_t *param =
      node_get_child(node_get_child(expr, CLUX_EXPR_FUNC_DECL_PARAMS), 0);
  ASSERT_NE(param, nullptr);
  EXPECT_NE(node_get_child(param, CLUX_FUNC_PARAM_NAME), nullptr);
  EXPECT_NE(node_get_child(param, CLUX_FUNC_PARAM_TYPE), nullptr);
  EXPECT_EQ(node_get_child(param, CLUX_FUNC_PARAM_DEFAULT), nullptr);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 闭包的捕获列表允许默认值。 */
TEST(ExpressionFuncDeclTest, CaptureDefaultValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr =
      parse_postfix(a, &p, "func |(offset: i32 = 10)| f(): i32 { a; }");
  expect_func_decl(expr, "func");
  node_t *captures = node_get_child(expr, CLUX_EXPR_FUNC_DECL_CAPTURES);
  ASSERT_NE(captures, nullptr);
  EXPECT_EQ(node_get_kind(captures), CLUX_NODE_FUNC_CAPTURES);
  /* 捕获列表的唯一子节点是与形参列表同构的 FUNC_PARAMS。 */
  ASSERT_EQ(vec_get_size(node_get_children(captures)), 1u);
  node_t *params = node_get_child(captures, 0);
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(node_get_kind(params), CLUX_NODE_FUNC_PARAMS);
  node_t *cap = node_get_child(params, 0);
  ASSERT_NE(cap, nullptr);
  EXPECT_EQ(node_get_kind(cap), CLUX_NODE_FUNC_PARAM);
  EXPECT_NE(node_get_child(cap, CLUX_FUNC_PARAM_NAME), nullptr);
  node_t *def = node_get_child(cap, CLUX_FUNC_PARAM_DEFAULT);
  ASSERT_NE(def, nullptr);
  EXPECT_EQ(node_get_kind(def), CLUX_NODE_LITERAL_NUMERIC);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 空捕获列表 `||`。 */
TEST(ExpressionFuncDeclTest, EmptyCaptures) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func || f(): void {}");
  expect_func_decl(expr, "func");
  node_t *captures = node_get_child(expr, CLUX_EXPR_FUNC_DECL_CAPTURES);
  ASSERT_NE(captures, nullptr);
  EXPECT_EQ(vec_get_size(node_get_children(captures)), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 泛型函数。 */
TEST(ExpressionFuncDeclTest, GenericFunction) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func id[T](x: T): T { a; }");
  expect_func_decl(expr, "func");
  node_t *generics = node_get_child(expr, CLUX_EXPR_FUNC_DECL_GENERICS);
  ASSERT_NE(generics, nullptr);
  EXPECT_EQ(node_get_kind(generics), CLUX_NODE_GENERIC_PARAMS);
  EXPECT_EQ(vec_get_size(node_get_children(generics)), 1u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 捕获 + 名字 + 泛型 + 形参 + 返回类型 + 函数体。 */
TEST(ExpressionFuncDeclTest, FullForm) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(
      a, &p, "func |(o: i32 = 1)| map[T](x: T, y: i32): T { a; }");
  expect_func_decl(expr, "func");
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_FUNC_DECL_CAPTURES), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_FUNC_DECL_NAME), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_FUNC_DECL_GENERICS), nullptr);
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_DECL_PARAMS);
  EXPECT_EQ(vec_get_size(node_get_children(params)), 2u);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_FUNC_DECL_BODY), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 参数包形参 `args: ...Args`。 */
TEST(ExpressionFuncDeclTest, ParameterPack) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr =
      parse_postfix(a, &p, "func first[...Args](args: ...Args): void { a; }");
  expect_func_decl(expr, "func");
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_DECL_PARAMS);
  ASSERT_EQ(vec_get_size(node_get_children(params)), 1u);
  node_t *param = node_get_child(params, 0);
  ASSERT_NE(param, nullptr);
  EXPECT_EQ(node_get_kind(param), CLUX_NODE_FUNC_PARAM);
  node_t *type = node_get_child(param, CLUX_FUNC_PARAM_TYPE);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(node_get_kind(type), CLUX_NODE_EXPR_REST);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* cfunc 在表达式上下文同样是定义，一样带函数体。 */
TEST(ExpressionFuncDeclTest, CfuncDefinition) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cfunc callback(x: i32): i32 { a; }");
  expect_func_decl(expr, "cfunc");
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_FUNC_DECL_CAPTURES), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_FUNC_DECL_NAME), nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_FUNC_DECL_GENERICS), nullptr);
  node_t *body = node_get_child(expr, CLUX_EXPR_FUNC_DECL_BODY);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(node_get_kind(body), CLUX_NODE_STMT_BLOCK);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* cfunc 缺函数体也按声明规则报错：语法层不区分 cfunc 的两种形态。 */
TEST(ExpressionFuncDeclTest, CfuncWithoutBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cfunc callback(x: i32): i32");
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_FUNC_DECL_BODY), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' to start function body");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* cfunc 带捕获与泛型：语法层照常解析，限制留给语义检查。 */
TEST(ExpressionFuncDeclTest, CfuncWithCapturesAndGenericsIsParsed) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cfunc |(o: i32)| f[T](x: T): T { a; }");
  expect_func_decl(expr, "cfunc");
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_FUNC_DECL_CAPTURES), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_FUNC_DECL_GENERICS), nullptr);
  EXPECT_NE(node_get_child(expr, CLUX_EXPR_FUNC_DECL_BODY), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 泛型：编译期值参数、约束、默认值、多参数、参数包。 */
TEST(ExpressionFuncDeclTest, GenericParameterForms) {
  const char *sources[] = {"func f[T, U](x: T): T { a; }",
                           "func f[N: comptime usize](x: i32): i32 { a; }",
                           "func f[W extends IWriter[W]](w: *W): void { a; }",
                           "func f[T = i32](): u64 { a; }",
                           "func first[...Args](args: ...Args): void { a; }",
                           "func f[](): void {}",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p;
    node_t *expr = parse_postfix(a, &p, sources[i]);
    expect_func_decl(expr, "func");
    EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u) << "src=" << sources[i];
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* 无名字的 rest 形参 `...`（C FFI 变参）。 */
TEST(ExpressionFuncDeclTest, BareRestParameter) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cfunc f(a: i32, ...): i32 { a; }");
  expect_func_decl(expr, "cfunc");
  node_t *params = node_get_child(expr, CLUX_EXPR_FUNC_DECL_PARAMS);
  ASSERT_EQ(vec_get_size(node_get_children(params)), 2u);
  EXPECT_EQ(node_get_kind(node_get_child(params, 1)), CLUX_NODE_EXPR_REST);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

/* 形参缺名字。 */
TEST(ExpressionFuncDeclTest, MissingParameterNameReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func f(: i32): void {}");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected parameter name in function");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 形参名后缺 ':'。 */
TEST(ExpressionFuncDeclTest, MissingParameterColonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func f(a i32): void {}");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ':' after parameter name");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 形参缺类型。 */
TEST(ExpressionFuncDeclTest, MissingParameterTypeReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func f(a:): void {}");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected parameter type");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 形参之间缺分隔符。 */
TEST(ExpressionFuncDeclTest, MissingParameterSeparatorReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func f(a: i32 b: str): void {}");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ',' or ')' in parameters");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺形参列表。 */
TEST(ExpressionFuncDeclTest, MissingParamsReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func f: void {}");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '(' to start parameters");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺返回类型引导 ':'。 */
TEST(ExpressionFuncDeclTest, MissingReturnColonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func f() void {}");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ':' before return type");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺返回类型。 */
TEST(ExpressionFuncDeclTest, MissingReturnTypeReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func f(): {}");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected return type after ':'");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺函数体。 */
TEST(ExpressionFuncDeclTest, MissingBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func f(): void");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' to start function body");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 捕获列表缺 '('。 */
TEST(ExpressionFuncDeclTest, MissingCaptureParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func |a: i32| f(): void {}");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '(' after '|' in captures");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 捕获列表缺默认值。 */
TEST(ExpressionFuncDeclTest, MissingCaptureDefaultReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func |(o: i32 =)| f(): i32 { a; }");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected capture default value");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 分隔符错误后直到 EOF 都没有闭合 ')'：容错循环安全退出。 */
TEST(ExpressionFuncDeclTest, SeparatorErrorAtEofReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func f(a: i32 b");
  ASSERT_NE(expr, nullptr);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 泛型参数错误路径。 */
TEST(ExpressionFuncDeclTest, GenericErrorsReported) {
  const char *sources[] = {"func f[1](): void {}",
                           "func f[T(): void {}",
                           "func f[N:](): void {}",
                           "func f[W extends](): void {}",
                           "func f[T =](): void {}",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p;
    node_t *expr = parse_postfix(a, &p, sources[i]);
    ASSERT_NE(expr, nullptr);
    EXPECT_GE(vec_get_size(parser_get_error(p)), 1u) << "src=" << sources[i];
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* 捕获列表缺闭合 '|'。 */
TEST(ExpressionFuncDeclTest, UnclosedCapturesReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "func |(o: i32) f(): void {}");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '|' to close captures");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 func 关键字到函数体终点。 */
TEST(ExpressionFuncDeclTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "func f(): void {}";
  node_t *expr = parse_postfix(a, &p, src);
  expect_func_decl(expr, "func");
  const location_t *loc = node_get_location(expr);
  EXPECT_EQ(loc->begin.offset, src);
  EXPECT_EQ(loc->end.offset, src + strlen(src));
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 匿名函数没有名字，区间止于函数体。 */
TEST(ExpressionFuncDeclTest, AnonymousSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "func(x: i32): i32 { a; }";
  node_t *expr = parse_postfix(a, &p, src);
  expect_func_decl(expr, "func");
  const location_t *loc = node_get_location(expr);
  EXPECT_EQ(loc->begin.offset, src);
  EXPECT_EQ(loc->end.offset, src + strlen(src));
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionFuncDeclTest, NoLeakOnFuncDecl) {
  const char *sources[] = {"func foo(a: i32): bool { a; }",
                           "func(x: i32): i32 { a; }",
                           "func f(): void {}",
                           "func f(a: i32, b: str): bool {}",
                           "func f(a: i32 = 10): void {}",
                           "func |(offset: i32 = 10)| f(): i32 { a; }",
                           "func || f(): void {}",
                           "func id[T](x: T): T { a; }",
                           "func |(o: i32 = 1)| map[T](x: T, y: i32): T { a; }",
                           "func first[...Args](args: ...Args): void { a; }",
                           "cfunc callback(x: i32): i32 { a; }",
                           "cfunc callback(x: i32): i32",
                           "cfunc |(o: i32)| f[T](x: T): T { a; }",
                           "func f(: i32): void {}",
                           "func f(a i32): void {}",
                           "func f(a:): void {}",
                           "func f(a: i32 b: str): void {}",
                           "func f: void {}",
                           "func f() void {}",
                           "func f(): {}",
                           "func f(): void",
                           "func |a: i32| f(): void {}",
                           "func |(o: i32 =)| f(): i32 { a; }",
                           "func",
                           "func f",
                           "cfunc f(a: i32, ...): i32 { a; }",
                           "func f[T, U](x: T): T { a; }",
                           "func f[N: comptime usize](x: i32): i32 { a; }",
                           "func f[W extends IWriter[W]](w: *W): void { a; }",
                           "func f[T = i32](): u64 { a; }",
                           "func f[](): void {}",
                           "func f[1](): void {}",
                           "func f[T(): void {}",
                           "func f[N:](): void {}",
                           "func f[W extends](): void {}",
                           "func f[T =](): void {}",
                           "func |(o: i32) f(): void {}",
                           "func f(a: i32 b",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

/* ================= C union =================
   cunion Name[generics] { field; ... }（白皮书 §3.8）。
   字段节点 CLUX_NODE_FIELD 由 struct / tag union / cunion 共用，
   因此不带具体类型前缀。 */

/* 断言 cunion 节点形状：名字必存在，字段列表必存在。 */
static node_t *expect_cunion(node_t *expr, const char *name) {
  if (!expr) {
    ADD_FAILURE() << "expected cunion node, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_CUNION);
  EXPECT_EQ(vec_get_size(node_get_children(expr)),
            (size_t)CLUX_EXPR_CUNION_CHILDREN_SIZE);
  node_t *name_node = node_get_child(expr, CLUX_EXPR_CUNION_NAME);
  if (!name_node) {
    ADD_FAILURE() << "expected cunion name";
    return expr;
  }
  EXPECT_EQ(node_get_kind(name_node), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(node_get_location(name_node)->end.offset -
                node_get_location(name_node)->begin.offset,
            (ptrdiff_t)strlen(name));
  node_t *body = node_get_child(expr, CLUX_EXPR_CUNION_BODY);
  if (!body) {
    ADD_FAILURE() << "expected cunion body";
    return expr;
  }
  EXPECT_EQ(node_get_kind(body), CLUX_NODE_FIELDS);
  return expr;
}

/* 断言字段列表里第 idx 个字段是 name: type。 */
static void expect_field(node_t *fields, size_t idx) {
  node_t *field = node_get_child(fields, idx);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(node_get_kind(field), CLUX_NODE_FIELD);
  EXPECT_NE(node_get_child(field, CLUX_FIELD_NAME), nullptr);
  EXPECT_NE(node_get_child(field, CLUX_FIELD_TYPE), nullptr);
}

TEST(ExpressionCunionTest, BasicDefinition) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion Value { i: i32; f: f32; }");
  expect_cunion(expr, "Value");
  node_t *fields = node_get_child(expr, CLUX_EXPR_CUNION_BODY);
  ASSERT_EQ(vec_get_size(node_get_children(fields)), 2u);
  expect_field(fields, 0);
  expect_field(fields, 1);
  EXPECT_EQ(node_get_child(expr, CLUX_EXPR_CUNION_GENERICS), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCunionTest, EmptyBody) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion Empty {}");
  expect_cunion(expr, "Empty");
  node_t *fields = node_get_child(expr, CLUX_EXPR_CUNION_BODY);
  EXPECT_EQ(vec_get_size(node_get_children(fields)), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 空字段 `;` 不允许：';' 必须严格跟随字段。 */
TEST(ExpressionCunionTest, EmptySemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { ; i: i32; }");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected field in type body");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 最后一个字段后必须写 ';'，不允许省略。 */
TEST(ExpressionCunionTest, LastFieldWithoutSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { i: i32 }");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after field");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 末尾多余的 ';' 同样不允许。 */
TEST(ExpressionCunionTest, TrailingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { i: i32;; }");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected field in type body");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 泛型 cunion。 */
TEST(ExpressionCunionTest, GenericDefinition) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V[T] { value: T; }");
  expect_cunion(expr, "V");
  node_t *generics = node_get_child(expr, CLUX_EXPR_CUNION_GENERICS);
  ASSERT_NE(generics, nullptr);
  EXPECT_EQ(node_get_kind(generics), CLUX_NODE_GENERIC_PARAMS);
  EXPECT_EQ(vec_get_size(node_get_children(generics)), 1u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 字段类型可以是复合类型。 */
TEST(ExpressionCunionTest, CompositeFieldTypes) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { p: *u8; s: []u8; }");
  expect_cunion(expr, "V");
  node_t *fields = node_get_child(expr, CLUX_EXPR_CUNION_BODY);
  ASSERT_EQ(vec_get_size(node_get_children(fields)), 2u);
  expect_field(fields, 0);
  expect_field(fields, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

TEST(ExpressionCunionTest, MissingNameReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion { i: i32; }");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected name after 'cunion'");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCunionTest, MissingBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' to start cunion body");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCunionTest, MissingFieldColonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { i i32; }");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ':' after field name");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCunionTest, MissingFieldTypeReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { i:; }");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected field type");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCunionTest, MissingFieldSeparatorReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { i: i32 f: f32; }");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after field");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCunionTest, UnclosedBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { i: i32;");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '}' to close fields");
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 非法字段起始（数字）：报错后跳到 ';' 继续。 */
TEST(ExpressionCunionTest, InvalidFieldStartReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { 1: i32; j: i32; }");
  ASSERT_NE(expr, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected field in type body");
  /* 跳过后仍解析出第二个字段。 */
  node_t *fields = node_get_child(expr, CLUX_EXPR_CUNION_BODY);
  EXPECT_EQ(vec_get_size(node_get_children(fields)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 非法字段起始直接到 EOF：容错循环安全退出。 */
TEST(ExpressionCunionTest, InvalidFieldStartAtEofReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { 1");
  ASSERT_NE(expr, nullptr);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 字段分隔符错误直接到 EOF：容错循环安全退出。 */
TEST(ExpressionCunionTest, MissingFieldSeparatorAtEofReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { i: i32 f");
  ASSERT_NE(expr, nullptr);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 非法字段起始后直接遇到 '}'：容错跳到 '}' 并正常闭合。 */
TEST(ExpressionCunionTest, InvalidFieldStartBeforeClose) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { 1 }");
  ASSERT_NE(expr, nullptr);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  node_t *fields = node_get_child(expr, CLUX_EXPR_CUNION_BODY);
  EXPECT_EQ(vec_get_size(node_get_children(fields)), 0u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 字段分隔符错误后直接遇到 '}'：容错跳到 '}' 并正常闭合。 */
TEST(ExpressionCunionTest, MissingFieldSeparatorBeforeClose) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *expr = parse_postfix(a, &p, "cunion V { i: i32 f }");
  ASSERT_NE(expr, nullptr);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  node_t *fields = node_get_child(expr, CLUX_EXPR_CUNION_BODY);
  EXPECT_EQ(vec_get_size(node_get_children(fields)), 1u);
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 cunion 关键字到字段列表终点。 */
TEST(ExpressionCunionTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "cunion V { i: i32; }";
  node_t *expr = parse_postfix(a, &p, src);
  expect_cunion(expr, "V");
  const location_t *loc = node_get_location(expr);
  EXPECT_EQ(loc->begin.offset, src);
  EXPECT_EQ(loc->end.offset, src + strlen(src));
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 字段区间 = 名字起点 .. 类型终点。 */
TEST(ExpressionCunionTest, FieldSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "cunion V { i: i32; }";
  node_t *expr = parse_postfix(a, &p, src);
  node_t *fields = node_get_child(expr, CLUX_EXPR_CUNION_BODY);
  ASSERT_NE(fields, nullptr);
  node_t *field = node_get_child(fields, 0);
  ASSERT_NE(field, nullptr);
  const location_t *loc = node_get_location(field);
  EXPECT_EQ(loc->begin.offset, src + strlen("cunion V { "));
  EXPECT_EQ(loc->end.offset, src + strlen("cunion V { i: i32"));
  delete_node(&expr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionCunionTest, NoLeakOnCunion) {
  const char *sources[] = {"cunion Value { i: i32; f: f32; }",
                           "cunion Empty {}",
                           "cunion V { ; i: i32; }",
                           "cunion V { i: i32 }",
                           "cunion V { i: i32;; }",
                           "cunion V { i: i32 }",
                           "cunion V[T] { value: T; }",
                           "cunion V { p: *u8; s: []u8; }",
                           "cunion { i: i32; }",
                           "cunion V",
                           "cunion V { i i32; }",
                           "cunion V { i:; }",
                           "cunion V { i: i32 f: f32; }",
                           "cunion V { i: i32;",
                           "cunion V { 1: i32; j: i32; }",
                           "cunion",
                           "cunion V[",
                           "cunion V { 1",
                           "cunion V { i: i32 f",
                           "cunion V { 1 }",
                           "cunion V { i: i32 f }",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *expr = parser_read_expr(a, p);
    delete_node(&expr);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}
