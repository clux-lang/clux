#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser/expression.h"
#include "parser/node.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include <stdlib.h>
#include <string.h>

/* ---- 辅助 ---- */

static size_t g_alloc_count;
static size_t g_free_count;

static void *counting_alloc(size_t size) {
  g_alloc_count++;
  return malloc(size);
}

static void counting_free(void *ptr) {
  g_free_count++;
  free(ptr);
}

static node_t *parse_stmt(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  parser_skip_trivia(*p);
  return parser_read_stmt(a, *p);
}

/* 断言块节点：children 数量与区间。 */
static void expect_block(node_t *node, size_t stmt_count) {
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_BLOCK);
  EXPECT_EQ(vec_get_size(node_get_children(node)), stmt_count);
}

/* 断言表达式语句节点：只有一个子节点，且是表达式。
   返回子表达式；节点形状不对时返回 NULL（ASSERT_* 不能用于非 void 函数）。 */
static node_t *expect_expr_stmt(node_t *node) {
  if (!node) {
    ADD_FAILURE() << "expected expression statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(vec_get_size(node_get_children(node)), 1u);
  node_t *expr = node_get_child(node, 0);
  EXPECT_NE(expr, nullptr);
  return expr;
}

/* 断言节点区间等于 src 中的 expected 子串（不含前后 trivia）。 */
static void
expect_location_span(node_t *node, const char *src, const char *expected) {
  const location_t *loc = node_get_location(node);
  ASSERT_NE(loc, nullptr);
  const char *at = strstr(src, expected);
  ASSERT_NE(at, nullptr) << "expected not found in src";
  EXPECT_EQ(loc->begin.offset, at)
      << "begin: got '" << loc->begin.offset << "'";
  EXPECT_EQ(loc->end.offset, at + strlen(expected))
      << "end: got '" << loc->end.offset << "'";
  EXPECT_STREQ(loc->filename, "test.clux");
}

/* ================= 块语句 ================= */

TEST(StmtBlockTest, EmptyBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{}");
  expect_block(node, 0);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtBlockTest, SingleStatement) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; }");
  expect_block(node, 1);
  expect_expr_stmt(node_get_child(node, 0));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtBlockTest, MultipleStatements) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; b; c; }");
  expect_block(node, 3);
  for (size_t i = 0; i < 3; i++) {
    expect_expr_stmt(node_get_child(node, i));
  }
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtBlockTest, NestedBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ { a; } b; }");
  expect_block(node, 2);
  expect_block(node_get_child(node, 0), 1);
  expect_expr_stmt(node_get_child(node, 1));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 语句之间的空白与注释都被跳过。 */
TEST(StmtBlockTest, TriviaBetweenStatements) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ /* c */ a; // x\n b; }");
  expect_block(node, 2);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间 = '{' 起点 .. '}' 终点，不含前后 trivia。 */
TEST(StmtBlockTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "  { a; }  ";
  node_t *node = parse_stmt(a, &p, src);
  expect_block(node, 1);
  expect_location_span(node, src, "{ a; }");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtBlockTest, EmptyBlockSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "{}";
  node_t *node = parse_stmt(a, &p, src);
  expect_location_span(node, src, "{}");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 非 '{' 开头：不是块语句，返回 NULL 且游标不动。 */
TEST(StmtBlockTest, ReturnsNullOnNonBlock) {
  const char *sources[] = {"a", "}", "", nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    EXPECT_EQ(parser_read_stmt_block(a, p), nullptr) << "src=" << sources[i];
    EXPECT_EQ(parser_get_position(p), 0u) << "src=" << sources[i];
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* 块语句不携带 ';'：区间止于 '}'，'}' 之后的 ';' 不被块消费。 */
TEST(StmtBlockTest, BlockDoesNotConsumeTrailingSemicolon) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "{ a; };";
  node_t *node = parse_stmt(a, &p, src);
  expect_block(node, 1);
  expect_location_span(node, src, "{ a; }");
  /* ';' 留在流中：空语句尚未实现，这里只断言块没有吞掉它。 */
  token_t *next = parser_peek_token(p);
  ASSERT_NE(next, nullptr);
  EXPECT_TRUE(token_is(next, CLUX_TOKEN_SYMBOL, ";"));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 块作为块内语句时同样不携带 ';'：{ { a; } b; } 中两个语句相邻即可。 */
TEST(StmtBlockTest, NestedBlockNeedsNoSemicolon) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ { a; } b; }");
  expect_block(node, 2);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 块语句错误路径 ---- */

/* 未闭合：{ a; */
TEST(StmtBlockTest, UnclosedBlockReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a;");
  expect_block(node, 1);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '}' to close block");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 空块未闭合：{ */
TEST(StmtBlockTest, UnclosedEmptyBlockReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{");
  expect_block(node, 0);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '}' to close block");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 无法解析的语句：跳过该 token 后继续，块仍能正常闭合。 */
TEST(StmtBlockTest, UnparsableStatementIsSkipped) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ ; a; }");
  expect_block(node, 1);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected statement in block");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 块内写成 `{ a; };` 时，块后的 ';' 是孤立分号：空语句尚未实现，
   按无法解析的语句报错，块本身仍正常闭合。 */
TEST(StmtBlockTest, SemicolonAfterNestedBlockIsUnparsableForNow) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ { a; }; b; }");
  expect_block(node, 2);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected statement in block");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 连续多个坏 token 各自报一次，不会死循环。 */
TEST(StmtBlockTest, MultipleUnparsableStatements) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ ; ; a; }");
  expect_block(node, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 2u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 表达式语句 ================= */

TEST(StmtExprTest, SingleExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "a;");
  node_t *expr = expect_expr_stmt(node);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExprTest, AssignmentStatement) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "x = 1;");
  node_t *expr = expect_expr_stmt(node);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_ASSIGN);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExprTest, CallStatement) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "f(a);");
  node_t *expr = expect_expr_stmt(node);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_CALL);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 语句前的 trivia 不属于区间，';' 属于区间。 */
TEST(StmtExprTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "  a + b ;  ";
  node_t *node = parse_stmt(a, &p, src);
  expect_expr_stmt(node);
  expect_location_span(node, src, "a + b ;");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 非表达式开头：返回 NULL 且游标不动。 */
TEST(StmtExprTest, ReturnsNullOnNonExpression) {
  const char *sources[] = {";", "}", "", nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    EXPECT_EQ(parser_read_stmt_expr(a, p), nullptr) << "src=" << sources[i];
    EXPECT_EQ(parser_get_position(p), 0u) << "src=" << sources[i];
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* ---- 表达式语句错误路径 ---- */

/* 缺分号：a —— 仍返回语句节点，区间止于表达式末尾。 */
TEST(StmtExprTest, MissingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "a");
  node_t *expr = expect_expr_stmt(node);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after expression statement");
  const location_t *loc = node_get_location(node);
  EXPECT_EQ(loc->begin.offset, node_get_location(expr)->begin.offset);
  EXPECT_EQ(loc->end.offset, node_get_location(expr)->end.offset);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 缺分号时不消费后续 token：跳过 trivia 后游标停在下一条语句的首 token。 */
TEST(StmtExprTest, MissingSemicolonDoesNotConsumeNextToken) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "a b");
  expect_expr_stmt(node);
  token_t *next = parser_peek_token(p);
  ASSERT_NE(next, nullptr);
  EXPECT_EQ(token_get_kind(next), CLUX_TOKEN_IDENTIFIER);
  EXPECT_TRUE(token_is(next, CLUX_TOKEN_IDENTIFIER, "b"));
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 分派 ================= */

TEST(StmtDispatchTest, DispatchesBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; }");
  expect_block(node, 1);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "a;");
  expect_expr_stmt(node);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, ReturnsNullOnEmptyStream) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  EXPECT_EQ(parse_stmt(a, &p, ""), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 连续解析多条语句：每次调用只消费一条。 */
TEST(StmtDispatchTest, ParsesStatementsSequentially) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a; { b; } c;");
  parser_skip_trivia(p);
  node_t *first = parser_read_stmt(a, p);
  expect_expr_stmt(first);
  parser_skip_trivia(p);
  node_t *second = parser_read_stmt(a, p);
  expect_block(second, 1);
  parser_skip_trivia(p);
  node_t *third = parser_read_stmt(a, p);
  expect_expr_stmt(third);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&first);
  delete_node(&second);
  delete_node(&third);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 泄漏检查 ================= */

TEST(StmtTest, NoLeakOnStatements) {
  const char *sources[] = {"{}",
                           "{ a; }",
                           "{ a; b; c; }",
                           "{ { a; } b; }",
                           "{ /* c */ a; // x\n b; }",
                           "a;",
                           "x = 1;",
                           "f(a);",
                           "{ a;",
                           "{",
                           "{ ; a; }",
                           "{ ; ; a; }",
                           "a",
                           "a b",
                           ";",
                           "}",
                           "",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    parser_skip_trivia(p);
    node_t *node = parser_read_stmt(a, p);
    delete_node(&node);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}
