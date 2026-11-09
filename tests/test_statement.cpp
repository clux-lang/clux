#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser/decorator.h"
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

/* ================= return ================= */

/* 断言 return 节点：无返回值时 VALUE 为 NULL。 */
static void expect_return_stmt(node_t *node, bool has_value) {
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_RETURN);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_RETURN_CHILDREN_SIZE);
  node_t *value = node_get_child(node, CLUX_STMT_RETURN_VALUE);
  if (has_value) {
    EXPECT_NE(value, nullptr);
  } else {
    EXPECT_EQ(value, nullptr);
  }
}

TEST(StmtReturnTest, EmptyReturn) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "return;");
  expect_return_stmt(node, false);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtReturnTest, ReturnExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "return x + 1;");
  expect_return_stmt(node, true);
  node_t *value = node_get_child(node, CLUX_STMT_RETURN_VALUE);
  EXPECT_EQ(node_get_kind(value), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 返回表达式走顶层入口：赋值、三元等整条链都允许。 */
TEST(StmtReturnTest, ReturnAssignmentExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "return x = 1;");
  expect_return_stmt(node, true);
  EXPECT_EQ(node_get_kind(node_get_child(node, CLUX_STMT_RETURN_VALUE)),
            CLUX_NODE_EXPR_ASSIGN);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtReturnTest, SpanWithoutValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "return;";
  node_t *node = parse_stmt(a, &p, src);
  expect_return_stmt(node, false);
  expect_location_span(node, src, "return;");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtReturnTest, SpanWithValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "return a + b;";
  node_t *node = parse_stmt(a, &p, src);
  expect_return_stmt(node, true);
  expect_location_span(node, src, "return a + b;");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtReturnTest, ReturnsNullOnNonReturn) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "break;");
  node_t *node = parser_read_stmt_return(a, p);
  EXPECT_EQ(node, nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtReturnTest, MissingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "return x");
  expect_return_stmt(node, true);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after return statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtReturnTest, MissingExpressionReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "return }");
  expect_return_stmt(node, false);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression after 'return'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= break / continue ================= */

TEST(StmtJumpTest, Break) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "break;";
  node_t *node = parse_stmt(a, &p, src);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_BREAK);
  EXPECT_EQ(vec_get_size(node_get_children(node)), 0u);
  expect_location_span(node, src, "break;");
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtJumpTest, Continue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "continue;";
  node_t *node = parse_stmt(a, &p, src);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_CONTINUE);
  EXPECT_EQ(vec_get_size(node_get_children(node)), 0u);
  expect_location_span(node, src, "continue;");
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtJumpTest, BreakReturnsNullOnNonBreak) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "continue;");
  node_t *node = parser_read_stmt_break(a, p);
  EXPECT_EQ(node, nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtJumpTest, ContinueReturnsNullOnNonContinue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "break;");
  node_t *node = parser_read_stmt_continue(a, p);
  EXPECT_EQ(node, nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtJumpTest, BreakMissingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "break");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_BREAK);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after 'break'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtJumpTest, ContinueMissingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "continue");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_CONTINUE);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after 'continue'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 分派 ================= */

TEST(StmtDispatchTest, DispatchesReturn) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "return;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_RETURN);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesBreak) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "break;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_BREAK);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesContinue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "continue;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_CONTINUE);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 块内混合各类语句。 */
TEST(StmtDispatchTest, MixedStatementsInBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; break; continue; return 1; }");
  expect_block(node, 4);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_BREAK);
  EXPECT_EQ(node_get_kind(node_get_child(node, 2)), CLUX_NODE_STMT_CONTINUE);
  EXPECT_EQ(node_get_kind(node_get_child(node, 3)), CLUX_NODE_STMT_RETURN);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 函数语句 =================
   语法：<decorator>* <func_decl>。本阶段不做名字检查。 */

/* 断言函数语句节点：FUNC_DECL 槽存在，装饰器个数符合预期。 */
static node_t *expect_func_stmt(node_t *node, size_t decorator_count) {
  if (!node) {
    ADD_FAILURE() << "expected function statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_FUNC);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_FUNC_FIRST_DECORATOR + decorator_count);
  node_t *func_decl = node_get_child(node, CLUX_STMT_FUNC_FUNC_DECL);
  EXPECT_NE(func_decl, nullptr);
  if (func_decl) {
    EXPECT_EQ(node_get_kind(func_decl), CLUX_NODE_EXPR_FUNC_DECL);
  }
  for (size_t i = 0; i < decorator_count; i++) {
    node_t *decorator =
        node_get_child(node, CLUX_STMT_FUNC_FIRST_DECORATOR + i);
    EXPECT_NE(decorator, nullptr);
    if (decorator) {
      EXPECT_EQ(node_get_kind(decorator), CLUX_NODE_DECORATOR);
    }
  }
  return func_decl;
}

TEST(StmtFuncTest, WithoutDecorators) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "func f(): void {}");
  expect_func_stmt(node, 0);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtFuncTest, SingleDecorator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] func f(): void {}");
  expect_func_stmt(node, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtFuncTest, MultipleDecoratorsKeepSourceOrder) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(
      a, &p, "[[trace]] [[metadata(\"role\", \"handler\")]] func f(): void {}");
  expect_func_stmt(node, 2);
  /* 第一个装饰器的表达式是标识符 trace，第二个是调用。 */
  node_t *first = node_get_child(node, CLUX_STMT_FUNC_FIRST_DECORATOR);
  node_t *second = node_get_child(node, CLUX_STMT_FUNC_FIRST_DECORATOR + 1);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(node_get_kind(node_get_child(first, CLUX_DECORATOR_EXPR)),
            CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(node_get_kind(node_get_child(second, CLUX_DECORATOR_EXPR)),
            CLUX_NODE_EXPR_CALL);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* cfunc 同样构成函数语句。 */
TEST(StmtFuncTest, Cfunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "cfunc callback(x: i32): i32 { a; }");
  expect_func_stmt(node, 0);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 匿名函数声明也走函数语句。 */
TEST(StmtFuncTest, Anonymous) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "func(x: i32): i32 { a; }");
  expect_func_stmt(node, 0);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 带捕获与泛型。 */
TEST(StmtFuncTest, CapturesAndGenerics) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "[[trace]] func |(o: i32 = 1)| map[T](x: T): T { a; }");
  expect_func_stmt(node, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 块内可以混入函数语句。 */
TEST(StmtFuncTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; func f(): void {} }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_FUNC);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖首个装饰器起点到函数声明终点。 */
TEST(StmtFuncTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "[[trace]] func f(): void {}";
  node_t *node = parse_stmt(a, &p, src);
  expect_func_stmt(node, 1);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 装饰器后缺函数声明。 */
TEST(StmtFuncTest, MissingFuncDeclReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] a;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_FUNC);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_FUNC_FUNC_DECL), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected function declaration in function statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 非装饰器、非 func/cfunc 起始时返回 NULL 且游标不动。 */
TEST(StmtFuncTest, ReturnsNullOnNonFunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_func(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtFuncTest, ReturnsNullOnSingleBracket) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[trace] func f(): void {}");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_func(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtTest, NoLeakOnStatements) {
  const char *sources[] = {
      "{}",
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
      "return;",
      "return x;",
      "return a + b;",
      "return x",
      "return }",
      "break;",
      "continue;",
      "break",
      "continue",
      "{ a; break; continue; return 1; }",
      "func f(): void {}",
      "[[trace]] func f(): void {}",
      "[[trace]] [[metadata(\"role\", \"handler\")]] func f(): void {}",
      "cfunc callback(x: i32): i32 { a; }",
      "func(x: i32): i32 { a; }",
      "[[trace]] func |(o: i32 = 1)| map[T](x: T): T { a; }",
      "{ a; func f(): void {} }",
      "[[trace]] a;",
      "[trace] func f(): void {}",
      "[[trace]] func f(): void",
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
