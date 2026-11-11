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
  node_t *node = parse_stmt(a, &p, "{ ) a; }");
  expect_block(node, 1);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected statement in block");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 块内写成 `{ { a; }; b; }` 时，块后的 ';' 是独立的空语句。 */
TEST(StmtBlockTest, SemicolonAfterNestedBlockIsEmptyStatement) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ { a; }; b; }");
  expect_block(node, 3);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_BLOCK);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_EMPTY);
  EXPECT_EQ(node_get_kind(node_get_child(node, 2)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 连续多个坏 token 各自报一次，不会死循环。 */
TEST(StmtBlockTest, MultipleUnparsableStatements) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ ) ) a; }");
  expect_block(node, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 2u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 空语句 =================
   语法：`;`（白皮书 §4.14）。叶子节点，无子节点。 */

TEST(StmtEmptyTest, SingleSemicolon) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = ";";
  node_t *node = parse_stmt(a, &p, src);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EMPTY);
  EXPECT_EQ(vec_get_size(node_get_children(node)), 0u);
  expect_location_span(node, src, src);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtEmptyTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ ; a; }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EMPTY);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtEmptyTest, ConsecutiveSemicolons) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ ; ; a; }");
  expect_block(node, 3);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EMPTY);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_EMPTY);
  EXPECT_EQ(node_get_kind(node_get_child(node, 2)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtEmptyTest, ReturnsNullOnNonEmpty) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_empty(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
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

/* ---- comptime func ---- */

TEST(StmtFuncTest, ComptimeFunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "comptime func f(): void {}");
  node_t *decl = expect_func_stmt(node, 0);
  ASSERT_NE(decl, nullptr);
  node_t *comptime = stmt_func_node_get_comptime(node);
  ASSERT_NE(comptime, nullptr);
  EXPECT_EQ(node_get_kind(comptime), CLUX_NODE_SYMBOL);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtFuncTest, DecoratedComptimeFunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] comptime func f(): void {}");
  node_t *decl = expect_func_stmt(node, 1);
  ASSERT_NE(decl, nullptr);
  EXPECT_NE(stmt_func_node_get_comptime(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间从 comptime 起点到函数体终点。 */
TEST(StmtFuncTest, ComptimeFuncSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "comptime func f(): void {}";
  node_t *node = parse_stmt(a, &p, src);
  expect_func_stmt(node, 0);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* comptime 后不是 func/cfunc 时不认领，交给其他 comptime 语句。 */
TEST(StmtFuncTest, DoesNotStealComptimeIf) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "comptime if (N) { }");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_func(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtFuncTest, DoesNotStealComptimeForeach) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p =
      create_parser(a, "test.clux", "comptime foreach (var x of seq) { }");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_func(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtFuncTest, DoesNotStealComptimeVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "comptime var x = 1;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_func(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 变量定义语句 =================
   语法：<decorator>* (var|using) name [: type]? = expr; */

/* 断言变量语句节点：KEYWORD / NAME / VALUE 必有，TYPE 按需。 */
static node_t *expect_var_stmt(node_t *node,
                               const char *keyword,
                               bool has_type,
                               size_t decorator_count) {
  if (!node) {
    ADD_FAILURE() << "expected variable statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_VAR);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_VAR_FIRST_DECORATOR + decorator_count);
  node_t *kw = node_get_child(node, CLUX_STMT_VAR_KEYWORD);
  if (!kw) {
    ADD_FAILURE() << "expected variable keyword";
    return node;
  }
  EXPECT_EQ(node_get_kind(kw), CLUX_NODE_SYMBOL);
  EXPECT_EQ(node_get_location(kw)->end.offset -
                node_get_location(kw)->begin.offset,
            (ptrdiff_t)strlen(keyword));
  EXPECT_NE(node_get_child(node, CLUX_STMT_VAR_NAME), nullptr);
  node_t *type = node_get_child(node, CLUX_STMT_VAR_TYPE);
  if (has_type) {
    EXPECT_NE(type, nullptr);
  } else {
    EXPECT_EQ(type, nullptr);
  }
  EXPECT_NE(node_get_child(node, CLUX_STMT_VAR_VALUE), nullptr);
  for (size_t i = 0; i < decorator_count; i++) {
    node_t *decorator = node_get_child(node, CLUX_STMT_VAR_FIRST_DECORATOR + i);
    EXPECT_NE(decorator, nullptr);
    if (decorator) {
      EXPECT_EQ(node_get_kind(decorator), CLUX_NODE_DECORATOR);
    }
  }
  return node;
}

TEST(StmtVarTest, WithoutType) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "var y = 2;");
  expect_var_stmt(node, "var", false, 0);
  node_t *name = node_get_child(node, CLUX_STMT_VAR_NAME);
  EXPECT_EQ(node_get_kind(name), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(node_get_kind(node_get_child(node, CLUX_STMT_VAR_VALUE)),
            CLUX_NODE_LITERAL_NUMERIC);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtVarTest, WithType) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "var x: i32 = 1;");
  expect_var_stmt(node, "var", true, 0);
  EXPECT_EQ(node_get_kind(node_get_child(node, CLUX_STMT_VAR_TYPE)),
            CLUX_NODE_LITERAL_TYPE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 类型只取后缀层：`var x: *i32 = p;` 的 `*i32` 是类型，`=` 不被吃掉。 */
TEST(StmtVarTest, PointerType) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "var x: *i32 = p;");
  expect_var_stmt(node, "var", true, 0);
  EXPECT_EQ(node_get_kind(node_get_child(node, CLUX_STMT_VAR_TYPE)),
            CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 初值走顶层入口：赋值、三元都允许。 */
TEST(StmtVarTest, ComplexInitializer) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "var x = a ? b : c;");
  expect_var_stmt(node, "var", false, 0);
  EXPECT_EQ(node_get_kind(node_get_child(node, CLUX_STMT_VAR_VALUE)),
            CLUX_NODE_EXPR_TERNARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtVarTest, UsingWithoutType) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "using a = expr;");
  expect_var_stmt(node, "using", false, 0);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtVarTest, UsingWithType) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "using b: T = expr;");
  expect_var_stmt(node, "using", true, 0);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* comptime var：comptime 记录在 COMPTIME 槽。 */
TEST(StmtVarTest, ComptimeVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "comptime var LIMIT: const u32 = 32;");
  expect_var_stmt(node, "var", true, 0);
  node_t *comptime = node_get_child(node, CLUX_STMT_VAR_COMPTIME);
  ASSERT_NE(comptime, nullptr);
  EXPECT_EQ(node_get_kind(comptime), CLUX_NODE_SYMBOL);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* comptime using 语法上可解析，合法性留给语义阶段检查。 */
TEST(StmtVarTest, ComptimeUsingParses) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "comptime using a = e;");
  expect_var_stmt(node, "using", false, 0);
  node_t *comptime = node_get_child(node, CLUX_STMT_VAR_COMPTIME);
  ASSERT_NE(comptime, nullptr);
  EXPECT_EQ(node_get_kind(comptime), CLUX_NODE_SYMBOL);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtVarTest, DecoratedComptimeVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] comptime var x = 1;");
  expect_var_stmt(node, "var", false, 1);
  EXPECT_NE(node_get_child(node, CLUX_STMT_VAR_COMPTIME), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* comptime var 的区间从 comptime 起点到 ';'。 */
TEST(StmtVarTest, ComptimeVarSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "comptime var x = 1;";
  node_t *node = parse_stmt(a, &p, src);
  expect_var_stmt(node, "var", false, 0);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* comptime 后接 if 时不认领为变量语句，游标不动。 */
TEST(StmtVarTest, DoesNotStealComptimeIf) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "comptime if (N) { }");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_var(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtVarTest, SingleDecorator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] var x = 1;");
  expect_var_stmt(node, "var", false, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtVarTest, MultipleDecoratorsKeepSourceOrder) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "[[trace]] [[metadata(\"k\", 1)]] using a = e;");
  expect_var_stmt(node, "using", false, 2);
  node_t *first = node_get_child(node, CLUX_STMT_VAR_FIRST_DECORATOR);
  node_t *second = node_get_child(node, CLUX_STMT_VAR_FIRST_DECORATOR + 1);
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

/* 装饰器内部含方括号时，分派仍能正确找到 var/using 关键字。 */
TEST(StmtVarTest, DecoratorWithSubscriptDispatchesVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[a[b]]] var x = 1;");
  expect_var_stmt(node, "var", false, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 装饰器引导函数语句时仍走函数语句。 */
TEST(StmtVarTest, DecoratorBeforeFuncStillDispatchesFunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] func f(): void {}");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_FUNC);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 块内可以混入变量语句。 */
TEST(StmtVarTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ var x = 1; using y = e; }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_VAR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_VAR);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖首个装饰器/关键字起点到 ';'。 */
TEST(StmtVarTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "[[trace]] var x: i32 = 1;";
  node_t *node = parse_stmt(a, &p, src);
  expect_var_stmt(node, "var", true, 1);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

TEST(StmtVarTest, MissingNameReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "var = 1;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_VAR);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected variable name in variable statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtVarTest, MissingTypeReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "var x: = 1;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected type after ':'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtVarTest, MissingEqualsReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "var x: i32 1;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '=' before initializer");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtVarTest, MissingInitializerReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "var x = ;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected initializer expression");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtVarTest, MissingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "var x = 1");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after variable statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 装饰器后既不是 var/using 也不是 func/cfunc。 */
TEST(StmtVarTest, DecoratorFollowedByNonDeclarationReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] a;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_FUNC);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 装饰器未闭合（EOF）：分派扫描安全退出，按函数语句报错。 */
TEST(StmtVarTest, UnclosedDecoratorAtEofReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace");
  ASSERT_NE(node, nullptr);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 只有装饰器、其后是 EOF：分派扫描安全退出。 */
TEST(StmtVarTest, DecoratorsThenEofReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]]");
  ASSERT_NE(node, nullptr);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 1u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 直接调用 var 入口且装饰器后不是 var/using：报错。 */
TEST(StmtVarTest, DecoratorFollowedByNonVarReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[[trace]] a;");
  node_t *node = parser_read_stmt_var(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_VAR);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected 'var' or 'using' in variable statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtVarTest, ReturnsNullOnNonVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_var(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 类型别名语句 =================
   语法：<decorator>* type name = expr; */

/* 断言类型语句节点：NAME / VALUE 必有。 */
static node_t *expect_type_stmt(node_t *node, size_t decorator_count) {
  if (!node) {
    ADD_FAILURE() << "expected type statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_TYPE);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_TYPE_FIRST_DECORATOR + decorator_count);
  EXPECT_NE(node_get_child(node, CLUX_STMT_TYPE_NAME), nullptr);
  EXPECT_NE(node_get_child(node, CLUX_STMT_TYPE_VALUE), nullptr);
  for (size_t i = 0; i < decorator_count; i++) {
    node_t *decorator =
        node_get_child(node, CLUX_STMT_TYPE_FIRST_DECORATOR + i);
    EXPECT_NE(decorator, nullptr);
    if (decorator) {
      EXPECT_EQ(node_get_kind(decorator), CLUX_NODE_DECORATOR);
    }
  }
  return node;
}

TEST(StmtTypeTest, SimpleAlias) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "type UserId = u64;");
  expect_type_stmt(node, 0);
  node_t *name = node_get_child(node, CLUX_STMT_TYPE_NAME);
  EXPECT_EQ(node_get_kind(name), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(node_get_kind(node_get_child(node, CLUX_STMT_TYPE_VALUE)),
            CLUX_NODE_LITERAL_TYPE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 别名指向复合类型：切片、指针、函数类型。 */
TEST(StmtTypeTest, CompositeAlias) {
  const char *sources[] = {"type Buf = []u8;",
                           "type P = *i32;",
                           "type Handler = func(i32) -> void;",
                           "type Opt = ?i32;",
                           "type Tup = <i32, u8>;",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p;
    node_t *node = parse_stmt(a, &p, sources[i]);
    expect_type_stmt(node, 0);
    EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u) << "src=" << sources[i];
    delete_node(&node);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

TEST(StmtTypeTest, SingleDecorator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] type UserId = u64;");
  expect_type_stmt(node, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtTypeTest, MultipleDecoratorsKeepSourceOrder) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "[[trace]] [[metadata(\"k\", 1)]] type U = u64;");
  expect_type_stmt(node, 2);
  node_t *first = node_get_child(node, CLUX_STMT_TYPE_FIRST_DECORATOR);
  node_t *second = node_get_child(node, CLUX_STMT_TYPE_FIRST_DECORATOR + 1);
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

/* 泛型类型别名。 */
TEST(StmtTypeTest, GenericAlias) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "type Buffer[T] = []T;");
  expect_type_stmt(node, 0);
  node_t *generics = node_get_child(node, CLUX_STMT_TYPE_GENERICS);
  ASSERT_NE(generics, nullptr);
  EXPECT_EQ(node_get_kind(generics), CLUX_NODE_GENERIC_PARAMS);
  EXPECT_EQ(vec_get_size(node_get_children(generics)), 1u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 多参数、约束、默认值、参数包都走同一套泛型解析。 */
TEST(StmtTypeTest, GenericAliasForms) {
  const char *sources[] = {"type Pair[T, U] = <T, U>;",
                           "type B[N: comptime usize] = [N]u8;",
                           "type C[W extends IWriter[W]] = *W;",
                           "type D[T = i32] = []T;",
                           "type E[...Args] = <...Args>;",
                           "type F[] = i32;",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p;
    node_t *node = parse_stmt(a, &p, sources[i]);
    expect_type_stmt(node, 0);
    EXPECT_NE(node_get_child(node, CLUX_STMT_TYPE_GENERICS), nullptr)
        << "src=" << sources[i];
    EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u) << "src=" << sources[i];
    delete_node(&node);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* 无泛型时 GENERICS 槽为 NULL。 */
TEST(StmtTypeTest, NoGenericsSlotIsNull) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "type U = u64;");
  expect_type_stmt(node, 0);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_TYPE_GENERICS), nullptr);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 泛型 + 装饰器。 */
TEST(StmtTypeTest, GenericWithDecorator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] type Buffer[T] = []T;");
  expect_type_stmt(node, 1);
  EXPECT_NE(node_get_child(node, CLUX_STMT_TYPE_GENERICS), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 泛型参数错误路径。 */
TEST(StmtTypeTest, GenericErrorsReported) {
  const char *sources[] = {"type U[1] = i32;",
                           "type U[T() = i32;",
                           "type U[N:] = i32;",
                           "type U[W extends] = i32;",
                           "type U[T =] = i32;",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p;
    node_t *node = parse_stmt(a, &p, sources[i]);
    ASSERT_NE(node, nullptr);
    EXPECT_GE(vec_get_size(parser_get_error(p)), 1u) << "src=" << sources[i];
    delete_node(&node);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* 装饰器引导类型语句时正确分派。 */
TEST(StmtTypeTest, DecoratorDispatchesType) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[a[b]]] type U = u64;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_TYPE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 块内可以混入类型语句。 */
TEST(StmtTypeTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ var x = 1; type U = u64; }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_VAR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_TYPE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖首个装饰器/关键字起点到 ';'。 */
TEST(StmtTypeTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "[[trace]] type U = u64;";
  node_t *node = parse_stmt(a, &p, src);
  expect_type_stmt(node, 1);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

TEST(StmtTypeTest, MissingNameReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "type = u64;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_TYPE);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected type name in type statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtTypeTest, MissingEqualsReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "type U u64;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '=' after type name");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtTypeTest, MissingValueReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "type U = ;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected type expression");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtTypeTest, MissingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "type U = u64");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after type statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 直接调用 type 入口且装饰器后不是 type：报错。 */
TEST(StmtTypeTest, DecoratorFollowedByNonTypeReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[[trace]] a;");
  node_t *node = parser_read_stmt_type(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_TYPE);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected 'type' in type statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtTypeTest, ReturnsNullOnNonType) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_type(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 枚举语句 =================
   语法：<decorator>* <enum_decl>（白皮书 §3.6）。 */

static node_t *expect_enum_stmt(node_t *node, size_t decorator_count) {
  if (!node) {
    ADD_FAILURE() << "expected enum statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_ENUM);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_ENUM_FIRST_DECORATOR + decorator_count);
  node_t *enum_decl = node_get_child(node, CLUX_STMT_ENUM_ENUM_DECL);
  EXPECT_NE(enum_decl, nullptr);
  if (enum_decl) {
    EXPECT_EQ(node_get_kind(enum_decl), CLUX_NODE_EXPR_ENUM_DECL);
  }
  for (size_t i = 0; i < decorator_count; i++) {
    node_t *decorator =
        node_get_child(node, CLUX_STMT_ENUM_FIRST_DECORATOR + i);
    EXPECT_NE(decorator, nullptr);
    if (decorator) {
      EXPECT_EQ(node_get_kind(decorator), CLUX_NODE_DECORATOR);
    }
  }
  return enum_decl;
}

TEST(StmtEnumTest, WithoutDecorators) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "enum Color: u8 { Red = 1, Green = 2 }");
  expect_enum_stmt(node, 0);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtEnumTest, SingleDecorator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] enum E: u8 { A = 1 }");
  expect_enum_stmt(node, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtEnumTest, MultipleDecoratorsKeepSourceOrder) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(
      a,
      &p,
      "[[trace]] [[metadata(\"role\", \"handler\")]] enum E: u8 { A = 1 }");
  expect_enum_stmt(node, 2);
  node_t *first = node_get_child(node, CLUX_STMT_ENUM_FIRST_DECORATOR);
  node_t *second = node_get_child(node, CLUX_STMT_ENUM_FIRST_DECORATOR + 1);
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

TEST(StmtEnumTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; enum E: u8 { A = 1 } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_ENUM);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖首个装饰器起点到枚举声明终点。 */
TEST(StmtEnumTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "[[trace]] enum E: u8 { A = 1 }";
  node_t *node = parse_stmt(a, &p, src);
  expect_enum_stmt(node, 1);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 装饰器后缺枚举声明。 */
TEST(StmtEnumTest, MissingEnumDeclReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[[trace]] a;");
  node_t *node = parser_read_stmt_enum(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_ENUM);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_ENUM_ENUM_DECL), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected enum declaration in enum statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtEnumTest, ReturnsNullOnNonEnum) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_enum(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtEnumTest, ReturnsNullOnSingleBracket) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[trace] enum E: u8 { A = 1 }");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_enum(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= union 语句 =================
   语法：<decorator>* <union_decl>（白皮书 §3.7、§3.8）。
   tag union 与 cunion 共用同一节点，由声明内的 KEYWORD 槽区分。 */

static node_t *expect_union_stmt(node_t *node, size_t decorator_count) {
  if (!node) {
    ADD_FAILURE() << "expected union statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_UNION);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_UNION_FIRST_DECORATOR + decorator_count);
  node_t *union_decl = node_get_child(node, CLUX_STMT_UNION_UNION_DECL);
  EXPECT_NE(union_decl, nullptr);
  if (union_decl) {
    EXPECT_EQ(node_get_kind(union_decl), CLUX_NODE_EXPR_UNION);
  }
  for (size_t i = 0; i < decorator_count; i++) {
    node_t *decorator =
        node_get_child(node, CLUX_STMT_UNION_FIRST_DECORATOR + i);
    EXPECT_NE(decorator, nullptr);
    if (decorator) {
      EXPECT_EQ(node_get_kind(decorator), CLUX_NODE_DECORATOR);
    }
  }
  return union_decl;
}

TEST(StmtUnionTest, UnionWithoutDecorators) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "union Shape { circle: Circle; }");
  expect_union_stmt(node, 0);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtUnionTest, CunionWithoutDecorators) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "cunion Value { i: i32; f: f32; }");
  expect_union_stmt(node, 0);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtUnionTest, SingleDecorator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] union Shape { a: i32; }");
  expect_union_stmt(node, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtUnionTest, DecoratedCunion) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] cunion Value { i: i32; }");
  expect_union_stmt(node, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtUnionTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; union U { x: i32; } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_UNION);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖首个装饰器起点到 union 声明终点。 */
TEST(StmtUnionTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "[[trace]] union Shape { a: i32; }";
  node_t *node = parse_stmt(a, &p, src);
  expect_union_stmt(node, 1);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 装饰器后缺 union 声明。 */
TEST(StmtUnionTest, MissingUnionDeclReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[[trace]] a;");
  node_t *node = parser_read_stmt_union(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_UNION);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_UNION_UNION_DECL), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected union declaration in union statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtUnionTest, ReturnsNullOnNonUnion) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_union(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtUnionTest, ReturnsNullOnSingleBracket) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[trace] union U { a: i32; }");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_union(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 结构体语句 =================
   语法：<decorator>* <struct_decl>（白皮书 §3.5）。 */

static node_t *expect_struct_stmt(node_t *node, size_t decorator_count) {
  if (!node) {
    ADD_FAILURE() << "expected struct statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_STRUCT);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_STRUCT_FIRST_DECORATOR + decorator_count);
  node_t *struct_decl = node_get_child(node, CLUX_STMT_STRUCT_STRUCT_DECL);
  EXPECT_NE(struct_decl, nullptr);
  if (struct_decl) {
    EXPECT_EQ(node_get_kind(struct_decl), CLUX_NODE_EXPR_STRUCT_DECL);
  }
  for (size_t i = 0; i < decorator_count; i++) {
    node_t *decorator =
        node_get_child(node, CLUX_STMT_STRUCT_FIRST_DECORATOR + i);
    EXPECT_NE(decorator, nullptr);
    if (decorator) {
      EXPECT_EQ(node_get_kind(decorator), CLUX_NODE_DECORATOR);
    }
  }
  return struct_decl;
}

TEST(StmtStructTest, WithoutDecorators) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "struct User { name: str; }");
  expect_struct_stmt(node, 0);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtStructTest, SingleDecorator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] struct User { name: str; }");
  expect_struct_stmt(node, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtStructTest, MultipleDecoratorsKeepSourceOrder) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(
      a,
      &p,
      "[[trace]] [[metadata(\"role\", \"handler\")]] struct User { x: i32; }");
  expect_struct_stmt(node, 2);
  node_t *first = node_get_child(node, CLUX_STMT_STRUCT_FIRST_DECORATOR);
  node_t *second = node_get_child(node, CLUX_STMT_STRUCT_FIRST_DECORATOR + 1);
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

TEST(StmtStructTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; struct S { x: i32; } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_STRUCT);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖首个装饰器起点到结构体声明终点。 */
TEST(StmtStructTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "[[trace]] struct S { x: i32; }";
  node_t *node = parse_stmt(a, &p, src);
  expect_struct_stmt(node, 1);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 装饰器后缺结构体声明。 */
TEST(StmtStructTest, MissingStructDeclReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[[trace]] a;");
  node_t *node = parser_read_stmt_struct(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_STRUCT);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_STRUCT_STRUCT_DECL), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected struct declaration in struct statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtStructTest, ReturnsNullOnNonStruct) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_struct(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtStructTest, ReturnsNullOnSingleBracket) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[trace] struct S { x: i32; }");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_struct(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= bitfield 语句 =================
   语法：<decorator>* bitfield Name: Underlying { field: width; ... }
   （白皮书 §10）。 */

/* 断言 bitfield 语句节点：声明槽必存在，装饰器个数符合预期。 */
static node_t *expect_bitfield_stmt(node_t *node, size_t decorator_count) {
  if (!node) {
    ADD_FAILURE() << "expected bitfield statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_BITFIELD);
  EXPECT_EQ(stmt_bitfield_node_get_decorator_count(node), decorator_count);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_BITFIELD_FIRST_DECORATOR + decorator_count);
  node_t *bitfield_decl = stmt_bitfield_node_get_bitfield_decl(node);
  EXPECT_NE(bitfield_decl, nullptr);
  if (bitfield_decl) {
    EXPECT_EQ(node_get_kind(bitfield_decl), CLUX_NODE_EXPR_BITFIELD_DECL);
  }
  for (size_t i = 0; i < decorator_count; i++) {
    node_t *decorator =
        node_get_child(node, CLUX_STMT_BITFIELD_FIRST_DECORATOR + i);
    EXPECT_NE(decorator, nullptr);
    if (decorator) {
      EXPECT_EQ(node_get_kind(decorator), CLUX_NODE_DECORATOR);
    }
  }
  return bitfield_decl;
}

TEST(StmtBitfieldTest, WithoutDecorators) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "bitfield Flags: u32 { a: 3; }");
  expect_bitfield_stmt(node, 0);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtBitfieldTest, SingleDecorator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] bitfield Flags: u32 { a: 3; }");
  expect_bitfield_stmt(node, 1);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtBitfieldTest, MultipleDecoratorsKeepSourceOrder) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a,
                            &p,
                            "[[trace]] [[metadata(\"role\", \"flags\")]] "
                            "bitfield Flags: u8 { a: 1; }");
  expect_bitfield_stmt(node, 2);
  node_t *first = node_get_child(node, CLUX_STMT_BITFIELD_FIRST_DECORATOR);
  node_t *second = node_get_child(node, CLUX_STMT_BITFIELD_FIRST_DECORATOR + 1);
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

TEST(StmtBitfieldTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; bitfield F: u8 { x: 1; } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_BITFIELD);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖首个装饰器起点到位域声明终点。 */
TEST(StmtBitfieldTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "[[trace]] bitfield F: u8 { x: 1; }";
  node_t *node = parse_stmt(a, &p, src);
  expect_bitfield_stmt(node, 1);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 装饰器后缺位域声明。 */
TEST(StmtBitfieldTest, MissingBitfieldDeclReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[[trace]] a;");
  node_t *node = parser_read_stmt_bitfield(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_BITFIELD);
  EXPECT_EQ(stmt_bitfield_node_get_bitfield_decl(node), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected bitfield declaration in bitfield statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtBitfieldTest, ReturnsNullOnNonBitfield) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_bitfield(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtBitfieldTest, ReturnsNullOnSingleBracket) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[trace] bitfield F: u8 { }");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_bitfield(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= export 前缀 =================
   语法：<decorator>* export? [comptime]? <decl>（白皮书 §8.1）。
   export 位于装饰器之后、comptime 之前。 */

TEST(StmtExportTest, ExportFunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "export func add(a: i32, b: i32): i32 { a; }");
  node_t *decl = expect_func_stmt(node, 0);
  ASSERT_NE(decl, nullptr);
  EXPECT_NE(stmt_func_node_get_export(node), nullptr);
  EXPECT_EQ(stmt_func_node_get_comptime(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExportTest, DecoratedExportComptimeFunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "[[trace]] export comptime func f(): void {}");
  node_t *decl = expect_func_stmt(node, 1);
  ASSERT_NE(decl, nullptr);
  EXPECT_NE(stmt_func_node_get_export(node), nullptr);
  EXPECT_NE(stmt_func_node_get_comptime(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExportTest, ExportVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export var x: i32 = 1;");
  expect_var_stmt(node, "var", true, 0);
  EXPECT_NE(stmt_var_node_get_export(node), nullptr);
  EXPECT_EQ(stmt_var_node_get_comptime(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExportTest, DecoratedExportComptimeVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "[[trace]] export comptime var LIMIT: const u32 = 32;");
  expect_var_stmt(node, "var", true, 1);
  EXPECT_NE(stmt_var_node_get_export(node), nullptr);
  EXPECT_NE(stmt_var_node_get_comptime(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExportTest, ExportType) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export type UserId = u64;");
  expect_type_stmt(node, 0);
  EXPECT_NE(stmt_type_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExportTest, ExportEnum) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export enum E: u8 { A = 1 }");
  expect_enum_stmt(node, 0);
  EXPECT_NE(stmt_enum_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExportTest, ExportUnion) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export union U { a: i32; }");
  expect_union_stmt(node, 0);
  EXPECT_NE(stmt_union_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExportTest, ExportCunion) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export cunion U { a: i32; }");
  expect_union_stmt(node, 0);
  EXPECT_NE(stmt_union_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExportTest, ExportStruct) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export struct User { name: str; }");
  expect_struct_stmt(node, 0);
  EXPECT_NE(stmt_struct_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExportTest, ExportBitfield) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export bitfield Flags: u8 { a: 1; }");
  expect_bitfield_stmt(node, 0);
  EXPECT_NE(stmt_bitfield_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* export 位于装饰器之后。 */
TEST(StmtExportTest, DecoratedExportBitfield) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "[[trace]] export bitfield Flags: u8 { a: 1; }");
  expect_bitfield_stmt(node, 1);
  EXPECT_NE(stmt_bitfield_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间从 export 起点到声明终点。 */
TEST(StmtExportTest, ExportSpan) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "export func f(): void {}";
  node_t *node = parse_stmt(a, &p, src);
  expect_func_stmt(node, 0);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* export 后缺声明：由函数语句路径报错。 */
TEST(StmtExportTest, ExportWithoutDeclarationReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_FUNC);
  EXPECT_EQ(stmt_func_node_get_func_decl(node), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected function declaration in function statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= import 语句 =================
   语法：import <alias> from "<path>";（白皮书 §8.2）。 */

TEST(StmtImportTest, BasicImport) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "import math from \"math.cx\";");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_IMPORT);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_IMPORT_CHILDREN_SIZE);
  node_t *alias = stmt_import_node_get_alias(node);
  ASSERT_NE(alias, nullptr);
  EXPECT_EQ(node_get_kind(alias), CLUX_NODE_LITERAL_IDENTIFIER);
  node_t *path = stmt_import_node_get_path(node);
  ASSERT_NE(path, nullptr);
  EXPECT_EQ(node_get_kind(path), CLUX_NODE_LITERAL_STRING);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtImportTest, RelativePath) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "import app from \"app/main.cx\";");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_IMPORT);
  EXPECT_NE(stmt_import_node_get_path(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 import 关键字到结尾 ';'。 */
TEST(StmtImportTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "import math from \"math.cx\";";
  node_t *node = parse_stmt(a, &p, src);
  ASSERT_NE(node, nullptr);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtImportTest, MissingAliasReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "import from \"math.cx\";");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(stmt_import_node_get_alias(node), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected module alias after 'import'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtImportTest, MissingFromReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "import math \"math.cx\";");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected 'from' after module alias");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtImportTest, MissingPathReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "import math from ;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(stmt_import_node_get_path(node), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected module path string after 'from'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtImportTest, MissingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "import math from \"math.cx\"");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after import statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtImportTest, ReturnsNullOnNonImport) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_import(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 重新导出语句 =================
   语法：export * from "<path>";（白皮书 §8.4）。 */

TEST(StmtReexportTest, BasicReexport) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export * from \"other.cx\";");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_REEXPORT);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_REEXPORT_CHILDREN_SIZE);
  node_t *path = stmt_reexport_node_get_path(node);
  ASSERT_NE(path, nullptr);
  EXPECT_EQ(node_get_kind(path), CLUX_NODE_LITERAL_STRING);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 export 到结尾 ';'。 */
TEST(StmtReexportTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "export * from \"other.cx\";";
  node_t *node = parse_stmt(a, &p, src);
  ASSERT_NE(node, nullptr);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtReexportTest, MissingFromReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export * \"other.cx\";");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_REEXPORT);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected 'from' after 'export *'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtReexportTest, MissingPathReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export * from ;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(stmt_reexport_node_get_path(node), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected module path string after 'from'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtReexportTest, MissingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export * from \"other.cx\"");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after re-export statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* export 后不是 '*' 时不是重新导出，交给声明语句。 */
TEST(StmtReexportTest, ReturnsNullOnExportDecl) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "export func f(): void {}");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_reexport(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtReexportTest, ReturnsNullOnNonExport) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_reexport(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= extern lib 语句 =================
   语法：extern lib <alias> from "<path>";（白皮书 §8.8）。 */

TEST(StmtExternLibTest, BasicExternLib) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern lib crt from \"c\";");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EXTERN_LIB);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_EXTERN_LIB_CHILDREN_SIZE);
  node_t *alias = stmt_extern_lib_node_get_alias(node);
  ASSERT_NE(alias, nullptr);
  EXPECT_EQ(node_get_kind(alias), CLUX_NODE_LITERAL_IDENTIFIER);
  node_t *path = stmt_extern_lib_node_get_path(node);
  ASSERT_NE(path, nullptr);
  EXPECT_EQ(node_get_kind(path), CLUX_NODE_LITERAL_STRING);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* "*" 表示当前进程已链接的符号。 */
TEST(StmtExternLibTest, WildcardPath) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern lib self from \"*\";");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EXTERN_LIB);
  EXPECT_NE(stmt_extern_lib_node_get_path(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 extern 到结尾 ';'。 */
TEST(StmtExternLibTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "extern lib crt from \"c\";";
  node_t *node = parse_stmt(a, &p, src);
  ASSERT_NE(node, nullptr);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternLibTest, MissingAliasReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern lib from \"c\";");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(stmt_extern_lib_node_get_alias(node), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected library alias after 'extern lib'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternLibTest, MissingFromReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern lib crt \"c\";");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected 'from' after library alias");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternLibTest, MissingPathReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern lib crt from ;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(stmt_extern_lib_node_get_path(node), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected library path string after 'from'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternLibTest, MissingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern lib crt from \"c\"");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after extern lib statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* extern 后不是 lib 时不认领，留给其他 extern 形式。 */
TEST(StmtExternLibTest, ReturnsNullOnExternNonLib) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "extern cfunc f(): i32;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_extern_lib(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternLibTest, ReturnsNullOnNonExtern) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_extern_lib(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= extern cfunc / extern var 声明 =================
   语法：[export]? extern cfunc <lib>::<name>(params): ret;
         [export]? extern var <lib>::<name>: type;（白皮书 §8.9、§8.10）。 */

TEST(StmtExternDeclTest, BasicExternCfunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "extern cfunc libc::malloc(size: usize): opaque;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EXTERN_CFUNC);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_EXTERN_CFUNC_CHILDREN_SIZE);
  EXPECT_EQ(stmt_extern_cfunc_node_get_export(node), nullptr);
  EXPECT_NE(stmt_extern_cfunc_node_get_lib(node), nullptr);
  EXPECT_NE(stmt_extern_cfunc_node_get_name(node), nullptr);
  EXPECT_NE(stmt_extern_cfunc_node_get_params(node), nullptr);
  EXPECT_NE(stmt_extern_cfunc_node_get_return(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, BasicExternVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern var crt::errno: i32;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EXTERN_VAR);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_EXTERN_VAR_CHILDREN_SIZE);
  EXPECT_EQ(stmt_extern_var_node_get_export(node), nullptr);
  EXPECT_NE(stmt_extern_var_node_get_lib(node), nullptr);
  EXPECT_NE(stmt_extern_var_node_get_name(node), nullptr);
  EXPECT_NE(stmt_extern_var_node_get_type(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, ExportExternCfunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "export extern cfunc zlib::zlibVersion(): str;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EXTERN_CFUNC);
  EXPECT_NE(stmt_extern_cfunc_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, ExportExternVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export extern var zlib::zlib_errno: i32;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EXTERN_VAR);
  EXPECT_NE(stmt_extern_var_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 export/extern 起点到结尾 ';'。 */
TEST(StmtExternDeclTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "extern cfunc crt::puts(s: *const u8): i32;";
  node_t *node = parse_stmt(a, &p, src);
  ASSERT_NE(node, nullptr);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, MissingLibraryNameReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern cfunc ::puts(): i32;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected library name after 'extern cfunc'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, MissingScopeOperatorReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern cfunc crt puts(): i32;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '::' after library name");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, MissingSymbolNameReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern cfunc crt::(): i32;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected symbol name after '::'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, MissingParamsReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern cfunc crt::puts: i32;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '(' to start parameters");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, MissingReturnColonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern cfunc crt::puts()");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ':' before return type");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, MissingReturnTypeReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern cfunc crt::puts(): ;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected return type after ':'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, MissingCfuncSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern cfunc crt::puts(): i32");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after extern cfunc declaration");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, MissingVarColonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern var crt::errno i32;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ':' before variable type");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, MissingVarTypeReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern var crt::errno: ;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected variable type after ':'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, MissingVarSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern var crt::errno: i32");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after extern var declaration");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* extern 后不是 cfunc/var 时不认领。 */
TEST(StmtExternDeclTest, ReturnsNullOnExternLib) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "extern lib crt from \"c\";");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_extern_cfunc(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  node = parser_read_stmt_extern_var(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtExternDeclTest, ReturnsNullOnNonExtern) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_extern_cfunc(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  node = parser_read_stmt_extern_var(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= interface 语句 =================
   语法：<decorator>* [export]? interface Name[generics]? { ... }
   （白皮书 §3.10、§6.9）。 */

/* 断言 interface 语句节点：声明槽必存在，装饰器个数符合预期。 */
static node_t *expect_interface_stmt(node_t *node, size_t decorator_count) {
  if (!node) {
    ADD_FAILURE() << "expected interface statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_INTERFACE);
  EXPECT_EQ(stmt_interface_node_get_decorator_count(node), decorator_count);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_INTERFACE_FIRST_DECORATOR + decorator_count);
  node_t *decl = stmt_interface_node_get_interface_decl(node);
  EXPECT_NE(decl, nullptr);
  if (decl) {
    EXPECT_EQ(node_get_kind(decl), CLUX_NODE_EXPR_INTERFACE_DECL);
  }
  for (size_t i = 0; i < decorator_count; i++) {
    node_t *decorator =
        node_get_child(node, CLUX_STMT_INTERFACE_FIRST_DECORATOR + i);
    EXPECT_NE(decorator, nullptr);
    if (decorator) {
      EXPECT_EQ(node_get_kind(decorator), CLUX_NODE_DECORATOR);
    }
  }
  return decl;
}

TEST(StmtInterfaceTest, WithoutDecorators) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(
      a,
      &p,
      "interface IWriter[W] { func write(self: *W, data: []u8): void; }");
  expect_interface_stmt(node, 0);
  EXPECT_EQ(stmt_interface_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtInterfaceTest, ExportInterface) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(
      a, &p, "export interface Sized[T] { func size_of(self: *T): usize; }");
  expect_interface_stmt(node, 0);
  EXPECT_NE(stmt_interface_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtInterfaceTest, DecoratedExportInterface) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "[[trace]] export interface I { func f(): void; }");
  expect_interface_stmt(node, 1);
  EXPECT_NE(stmt_interface_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtInterfaceTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; interface I { func f(): void; } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_INTERFACE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖首个装饰器/export 起点到接口声明终点。 */
TEST(StmtInterfaceTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "export interface I { func f(): void; }";
  node_t *node = parse_stmt(a, &p, src);
  expect_interface_stmt(node, 0);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 装饰器/export 后缺接口声明。 */
TEST(StmtInterfaceTest, MissingInterfaceDeclReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[[trace]] a;");
  node_t *node = parser_read_stmt_interface(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_INTERFACE);
  EXPECT_EQ(stmt_interface_node_get_interface_decl(node), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected interface declaration in interface statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtInterfaceTest, ReturnsNullOnNonInterface) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_interface(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtInterfaceTest, ReturnsNullOnExportNonInterface) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "export func f(): void {}");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_interface(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= static_assert =================
   语法：static_assert(cond, "msg");（白皮书 §4.16）。
   SOURCE 为条件表达式源码片段，location 指向原文区间。 */

TEST(StmtStaticAssertTest, BasicStaticAssert) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(
      a,
      &p,
      "static_assert(sizeof(i64) == 8 as u64, \"expected 64-bit i64\");");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_STATIC_ASSERT);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_STATIC_ASSERT_CHILDREN_SIZE);
  node_t *condition = stmt_static_assert_node_get_condition(node);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(node_get_kind(condition), CLUX_NODE_EXPR_BINARY);
  node_t *message = stmt_static_assert_node_get_message(node);
  ASSERT_NE(message, nullptr);
  EXPECT_EQ(node_get_kind(message), CLUX_NODE_LITERAL_STRING);
  node_t *source = stmt_static_assert_node_get_source(node);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(node_get_kind(source), CLUX_NODE_SOURCE_TEXT);
  /* 源码片段区间与条件表达式完全一致。 */
  EXPECT_EQ(node_get_location(source)->begin.offset,
            node_get_location(condition)->begin.offset);
  EXPECT_EQ(node_get_location(source)->end.offset,
            node_get_location(condition)->end.offset);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 static_assert 到结尾 ';'。 */
TEST(StmtStaticAssertTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "static_assert(true, \"m\");";
  node_t *node = parse_stmt(a, &p, src);
  ASSERT_NE(node, nullptr);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtStaticAssertTest, MissingOpenParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "static_assert true, \"m\");");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '(' after 'static_assert'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtStaticAssertTest, MissingConditionReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "static_assert(, \"m\");");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected condition expression");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtStaticAssertTest, MissingCommaReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "static_assert(true \"m\");");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ',' after static_assert condition");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtStaticAssertTest, MissingMessageReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "static_assert(true, );");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected message string after ','");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtStaticAssertTest, MissingCloseParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "static_assert(true, \"m\";");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ')' to close static_assert");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtStaticAssertTest, MissingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "static_assert(true, \"m\")");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after static_assert statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtStaticAssertTest, ReturnsNullOnNonStaticAssert) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_static_assert(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= assert =================
   语法：assert(expr);（白皮书 §11.3）。
   assert 不是关键字，只有后接 '(' 时才按断言语句解析。 */

TEST(StmtAssertTest, BasicAssert) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "assert(x == 4);");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_ASSERT);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_ASSERT_CHILDREN_SIZE);
  node_t *condition = stmt_assert_node_get_condition(node);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(node_get_kind(condition), CLUX_NODE_EXPR_BINARY);
  node_t *source = stmt_assert_node_get_source(node);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(node_get_kind(source), CLUX_NODE_SOURCE_TEXT);
  EXPECT_EQ(node_get_location(source)->begin.offset,
            node_get_location(condition)->begin.offset);
  EXPECT_EQ(node_get_location(source)->end.offset,
            node_get_location(condition)->end.offset);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 assert 到结尾 ';'。 */
TEST(StmtAssertTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "assert(x == 4);";
  node_t *node = parse_stmt(a, &p, src);
  ASSERT_NE(node, nullptr);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtAssertTest, MissingConditionReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "assert();");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected condition expression");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtAssertTest, MissingCloseParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "assert(x;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ')' to close assert");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtAssertTest, MissingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "assert(x)");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after assert statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* assert 后不是 '(' 时不是断言语句，交回表达式语句。 */
TEST(StmtAssertTest, ReturnsNullWithoutParen) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "assert;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_assert(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtAssertTest, ReturnsNullOnNonAssert) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "foo(x);");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_assert(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= test 语句 =================
   语法：test "Suite.Name" { body }（白皮书 §3.14、§11.1）。
   test 不是关键字，只有后接字符串字面量时才按测试块解析。 */

/* 断言 test 节点：NAME 为字符串字面量，BODY 为块。 */
static node_t *expect_test_stmt(node_t *node) {
  if (!node) {
    ADD_FAILURE() << "expected test statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_TEST);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_TEST_CHILDREN_SIZE);
  node_t *name = stmt_test_node_get_name(node);
  EXPECT_NE(name, nullptr);
  if (name) {
    EXPECT_EQ(node_get_kind(name), CLUX_NODE_LITERAL_STRING);
  }
  node_t *body = stmt_test_node_get_body(node);
  EXPECT_NE(body, nullptr);
  if (body) {
    EXPECT_EQ(node_get_kind(body), CLUX_NODE_STMT_BLOCK);
  }
  return body;
}

TEST(StmtTestTest, BasicTestBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "test \"Suite.Case\" { assert(1 + 1 == 2); }");
  node_t *body = expect_test_stmt(node);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(stmt_block_node_get_stmt_count(body), 1u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtTestTest, EmptyBody) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "test \"Suite.Empty\" {}");
  node_t *body = expect_test_stmt(node);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(stmt_block_node_get_stmt_count(body), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 test 标识符到块终点。 */
TEST(StmtTestTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "test \"Suite.Case\" { assert(x); }";
  node_t *node = parse_stmt(a, &p, src);
  expect_test_stmt(node);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* test 后不是字符串字面量时不是测试块，交回表达式语句。 */
TEST(StmtTestTest, ReturnsNullWithoutString) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "test;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_test(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtTestTest, ReturnsNullOnNonTest) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "foo \"x\" {}");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_test(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* test 后缺块。 */
TEST(StmtTestTest, MissingBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "test \"Suite.Case\"");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_TEST);
  EXPECT_EQ(stmt_test_node_get_body(node), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' to start test body");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* test 作为普通标识符时仍走表达式语句。 */
TEST(StmtTestTest, TestIdentifierStaysExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "test;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= defer 语句 =================
   语法：defer |captures|? { body }（白皮书 §4.12）。
   捕获项复用 FUNC_PARAM：NAME 必有，DEFAULT 可缺失。 */

/* 断言 defer 节点：BODY 必存在；captures 是否存在的期望由调用方检查。 */
static node_t *expect_defer_stmt(node_t *node) {
  if (!node) {
    ADD_FAILURE() << "expected defer statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_DEFER);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_DEFER_CHILDREN_SIZE);
  node_t *body = node_get_child(node, CLUX_STMT_DEFER_BODY);
  EXPECT_NE(body, nullptr);
  if (body) {
    EXPECT_EQ(node_get_kind(body), CLUX_NODE_STMT_BLOCK);
  }
  return body;
}

/* 断言捕获列表里第 idx 个捕获项是 name，默认值是否存在。 */
static void
expect_defer_capture(node_t *captures, size_t idx, bool has_default) {
  ASSERT_NE(captures, nullptr);
  EXPECT_EQ(node_get_kind(captures), CLUX_NODE_FUNC_CAPTURES);
  ASSERT_EQ(vec_get_size(node_get_children(captures)), 1u);
  node_t *params = node_get_child(captures, 0);
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(node_get_kind(params), CLUX_NODE_FUNC_PARAMS);
  node_t *param = func_params_node_get_param(params, idx);
  ASSERT_NE(param, nullptr);
  EXPECT_NE(func_param_node_get_name(param), nullptr);
  EXPECT_EQ(func_param_node_get_default(param) != nullptr, has_default);
}

TEST(StmtDeferTest, WithoutCaptures) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "defer { a; }");
  expect_defer_stmt(node);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_DEFER_CAPTURES), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDeferTest, EmptyBody) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "defer {}");
  node_t *body = expect_defer_stmt(node);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(stmt_block_node_get_stmt_count(body), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDeferTest, BareCapture) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "defer |captured| { a; }");
  expect_defer_stmt(node);
  expect_defer_capture(
      node_get_child(node, CLUX_STMT_DEFER_CAPTURES), 0, false);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 裸列表形式的默认值停在闭合 '|' 之前。 */
TEST(StmtDeferTest, BareCaptureWithDefault) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "defer |pa = a.&| { a; }");
  expect_defer_stmt(node);
  expect_defer_capture(node_get_child(node, CLUX_STMT_DEFER_CAPTURES), 0, true);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDeferTest, ParenthesizedCapture) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "defer |(h = handle)| { a; }");
  expect_defer_stmt(node);
  expect_defer_capture(node_get_child(node, CLUX_STMT_DEFER_CAPTURES), 0, true);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDeferTest, MultipleCaptures) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "defer |a, b = 1| { }");
  expect_defer_stmt(node);
  node_t *captures = node_get_child(node, CLUX_STMT_DEFER_CAPTURES);
  ASSERT_NE(captures, nullptr);
  node_t *params = node_get_child(captures, 0);
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(func_params_node_get_param_count(params), 2u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 空捕获列表 `||`：不产生 FUNC_PARAMS 子节点。 */
TEST(StmtDeferTest, EmptyCaptureList) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "defer || { }");
  expect_defer_stmt(node);
  node_t *captures = node_get_child(node, CLUX_STMT_DEFER_CAPTURES);
  ASSERT_NE(captures, nullptr);
  EXPECT_EQ(node_get_kind(captures), CLUX_NODE_FUNC_CAPTURES);
  EXPECT_EQ(vec_get_size(node_get_children(captures)), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDeferTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; defer { b; } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_DEFER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 defer 关键字到块终点。 */
TEST(StmtDeferTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "defer |x| { a; }";
  node_t *node = parse_stmt(a, &p, src);
  expect_defer_stmt(node);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

TEST(StmtDeferTest, MissingBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "defer");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_DEFER);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_DEFER_BODY), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' to start defer body");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDeferTest, MissingCaptureCloseReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "defer |a { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '|' to close defer captures");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDeferTest, MissingParenCloseReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "defer |(a { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ')' to close defer captures");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDeferTest, InvalidCaptureNameReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "defer |1| { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected capture name in defer");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDeferTest, ReturnsNullOnNonDefer) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_defer(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= if 语句 =================
   语法：if (cond) { then } [else { else } | else if ...]（白皮书 §4.4）。 */

/* 断言 if 节点：CONDITION 与 THEN 必存在。 */
static node_t *expect_if_stmt(node_t *node) {
  if (!node) {
    ADD_FAILURE() << "expected if statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_IF);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_IF_CHILDREN_SIZE);
  EXPECT_NE(node_get_child(node, CLUX_STMT_IF_CONDITION), nullptr);
  node_t *then_branch = node_get_child(node, CLUX_STMT_IF_THEN);
  EXPECT_NE(then_branch, nullptr);
  if (then_branch) {
    EXPECT_EQ(node_get_kind(then_branch), CLUX_NODE_STMT_BLOCK);
  }
  return then_branch;
}

TEST(StmtIfTest, BasicIf) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "if (x) { a; }");
  node_t *then_branch = expect_if_stmt(node);
  ASSERT_NE(then_branch, nullptr);
  EXPECT_EQ(stmt_block_node_get_stmt_count(then_branch), 1u);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_IF_ELSE), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtIfTest, IfElse) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "if (x) { a; } else { b; }");
  expect_if_stmt(node);
  node_t *else_branch = node_get_child(node, CLUX_STMT_IF_ELSE);
  ASSERT_NE(else_branch, nullptr);
  EXPECT_EQ(node_get_kind(else_branch), CLUX_NODE_STMT_BLOCK);
  EXPECT_EQ(stmt_block_node_get_stmt_count(else_branch), 1u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* else if 递归为嵌套 if 语句。 */
TEST(StmtIfTest, ElseIfChain) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "if (a) { } else if (b) { } else { c; }");
  expect_if_stmt(node);
  node_t *else_branch = node_get_child(node, CLUX_STMT_IF_ELSE);
  ASSERT_NE(else_branch, nullptr);
  EXPECT_EQ(node_get_kind(else_branch), CLUX_NODE_STMT_IF);
  node_t *nested_else = node_get_child(else_branch, CLUX_STMT_IF_ELSE);
  ASSERT_NE(nested_else, nullptr);
  EXPECT_EQ(node_get_kind(nested_else), CLUX_NODE_STMT_BLOCK);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtIfTest, EmptyBodies) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "if (x) {} else {}");
  node_t *then_branch = expect_if_stmt(node);
  ASSERT_NE(then_branch, nullptr);
  EXPECT_EQ(stmt_block_node_get_stmt_count(then_branch), 0u);
  node_t *else_branch = node_get_child(node, CLUX_STMT_IF_ELSE);
  ASSERT_NE(else_branch, nullptr);
  EXPECT_EQ(stmt_block_node_get_stmt_count(else_branch), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 条件可以是完整表达式。 */
TEST(StmtIfTest, ConditionExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "if (a + b > 0) { }");
  expect_if_stmt(node);
  node_t *condition = node_get_child(node, CLUX_STMT_IF_CONDITION);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(node_get_kind(condition), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtIfTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; if (x) { b; } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_IF);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 if 关键字到 then 分支终点。 */
TEST(StmtIfTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "if (x) { a; }";
  node_t *node = parse_stmt(a, &p, src);
  expect_if_stmt(node);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 有 else 时区间延伸到 else 分支终点。 */
TEST(StmtIfTest, SpanWithElse) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "if (x) { a; } else { b; }";
  node_t *node = parse_stmt(a, &p, src);
  expect_if_stmt(node);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

TEST(StmtIfTest, MissingOpenParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "if x) { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '(' after 'if'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtIfTest, MissingConditionReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "if () { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected condition expression");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtIfTest, MissingCloseParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "if (x { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ')' after if condition");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtIfTest, MissingThenBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "if (x)");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_IF_THEN), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' to start if body");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtIfTest, InvalidElseReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "if (x) { } else a;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_IF_ELSE), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' or 'if' after 'else'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtIfTest, ReturnsNullOnNonIf) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_if(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= while 语句 =================
   语法：while (cond) { body }（白皮书 §4.6）。 */

/* 断言 while 节点：CONDITION 与 BODY 必存在。 */
static node_t *expect_while_stmt(node_t *node) {
  if (!node) {
    ADD_FAILURE() << "expected while statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_WHILE);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_WHILE_CHILDREN_SIZE);
  EXPECT_NE(node_get_child(node, CLUX_STMT_WHILE_CONDITION), nullptr);
  node_t *body = node_get_child(node, CLUX_STMT_WHILE_BODY);
  EXPECT_NE(body, nullptr);
  if (body) {
    EXPECT_EQ(node_get_kind(body), CLUX_NODE_STMT_BLOCK);
  }
  return body;
}

TEST(StmtWhileTest, BasicWhile) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "while (x) { a; }");
  node_t *body = expect_while_stmt(node);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(stmt_block_node_get_stmt_count(body), 1u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtWhileTest, EmptyBody) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "while (x) {}");
  node_t *body = expect_while_stmt(node);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(stmt_block_node_get_stmt_count(body), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 条件可以是完整表达式。 */
TEST(StmtWhileTest, ConditionExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "while (i < n && ok) { i = i + 1; }");
  expect_while_stmt(node);
  node_t *condition = node_get_child(node, CLUX_STMT_WHILE_CONDITION);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(node_get_kind(condition), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtWhileTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; while (x) { b; } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_WHILE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 while 关键字到循环体终点。 */
TEST(StmtWhileTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "while (x) { a; }";
  node_t *node = parse_stmt(a, &p, src);
  expect_while_stmt(node);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

TEST(StmtWhileTest, MissingOpenParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "while x) { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '(' after 'while'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtWhileTest, MissingConditionReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "while () { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected condition expression");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtWhileTest, MissingCloseParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "while (x { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ')' after while condition");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtWhileTest, MissingBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "while (x)");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_WHILE_BODY), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' to start while body");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtWhileTest, ReturnsNullOnNonWhile) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_while(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= for 语句 =================
   语法：for (init; cond; update) { body }（白皮书 §4.8）。
   初始化可以是变量语句或表达式，三段均可省略。 */

/* 断言 for 节点：BODY 必存在，其余槽按需检查。 */
static node_t *expect_for_stmt(node_t *node) {
  if (!node) {
    ADD_FAILURE() << "expected for statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_FOR);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_FOR_CHILDREN_SIZE);
  node_t *body = node_get_child(node, CLUX_STMT_FOR_BODY);
  EXPECT_NE(body, nullptr);
  if (body) {
    EXPECT_EQ(node_get_kind(body), CLUX_NODE_STMT_BLOCK);
  }
  return body;
}

TEST(StmtForTest, BasicFor) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "for (var i = 0; i < 10; i += 1) { a; }");
  node_t *body = expect_for_stmt(node);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(stmt_block_node_get_stmt_count(body), 1u);
  node_t *init = node_get_child(node, CLUX_STMT_FOR_INIT);
  ASSERT_NE(init, nullptr);
  EXPECT_EQ(node_get_kind(init), CLUX_NODE_STMT_VAR);
  node_t *condition = node_get_child(node, CLUX_STMT_FOR_CONDITION);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(node_get_kind(condition), CLUX_NODE_EXPR_BINARY);
  node_t *update = node_get_child(node, CLUX_STMT_FOR_UPDATE);
  ASSERT_NE(update, nullptr);
  EXPECT_EQ(node_get_kind(update), CLUX_NODE_EXPR_ASSIGN);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 初始化也可以是表达式。 */
TEST(StmtForTest, ExpressionInit) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "for (i = 0; i < 10; i += 1) { }");
  expect_for_stmt(node);
  node_t *init = node_get_child(node, CLUX_STMT_FOR_INIT);
  ASSERT_NE(init, nullptr);
  EXPECT_EQ(node_get_kind(init), CLUX_NODE_EXPR_ASSIGN);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForTest, EmptyInit) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "for (; i < 10; i += 1) { }");
  expect_for_stmt(node);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_FOR_INIT), nullptr);
  EXPECT_NE(node_get_child(node, CLUX_STMT_FOR_CONDITION), nullptr);
  EXPECT_NE(node_get_child(node, CLUX_STMT_FOR_UPDATE), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForTest, EmptyCondition) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "for (var i = 0; ; i += 1) { }");
  expect_for_stmt(node);
  EXPECT_NE(node_get_child(node, CLUX_STMT_FOR_INIT), nullptr);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_FOR_CONDITION), nullptr);
  EXPECT_NE(node_get_child(node, CLUX_STMT_FOR_UPDATE), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForTest, EmptyUpdate) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "for (var i = 0; i < 10; ) { }");
  expect_for_stmt(node);
  EXPECT_NE(node_get_child(node, CLUX_STMT_FOR_INIT), nullptr);
  EXPECT_NE(node_get_child(node, CLUX_STMT_FOR_CONDITION), nullptr);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_FOR_UPDATE), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForTest, AllPartsEmpty) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "for (;;) { }");
  expect_for_stmt(node);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_FOR_INIT), nullptr);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_FOR_CONDITION), nullptr);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_FOR_UPDATE), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "{ a; for (var i = 0; i < 1; i += 1) { b; } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_FOR);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 for 关键字到循环体终点。 */
TEST(StmtForTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "for (var i = 0; i < 1; i += 1) { a; }";
  node_t *node = parse_stmt(a, &p, src);
  expect_for_stmt(node);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

TEST(StmtForTest, MissingOpenParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "for var i = 0; i < 1; i += 1) { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '(' after 'for'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForTest, MissingInitializerSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "for (1 2; i < 1; i += 1) { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after for initializer");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForTest, MissingConditionSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "for (var i = 0; i < 1 i += 1) { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after for condition");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForTest, MissingCloseParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "for (var i = 0; i < 1; i += 1 { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ')' after for update");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForTest, MissingBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "for (var i = 0; i < 1; i += 1)");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_FOR_BODY), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' to start for body");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForTest, ReturnsNullOnNonFor) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_for(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= foreach 语句 =================
   语法：foreach ([var] name [: type]? of iterable) { body }（白皮书 §4.9）。 */

/* 断言 foreach 节点：NAME / ITERABLE / BODY 必存在。 */
static node_t *expect_foreach_stmt(node_t *node) {
  if (!node) {
    ADD_FAILURE() << "expected foreach statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_FOREACH);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_FOREACH_CHILDREN_SIZE);
  EXPECT_NE(node_get_child(node, CLUX_STMT_FOREACH_NAME), nullptr);
  EXPECT_NE(node_get_child(node, CLUX_STMT_FOREACH_ITERABLE), nullptr);
  node_t *body = node_get_child(node, CLUX_STMT_FOREACH_BODY);
  EXPECT_NE(body, nullptr);
  if (body) {
    EXPECT_EQ(node_get_kind(body), CLUX_NODE_STMT_BLOCK);
  }
  return body;
}

TEST(StmtForeachTest, BasicForeach) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "foreach (var v of a) { b; }");
  node_t *body = expect_foreach_stmt(node);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(stmt_block_node_get_stmt_count(body), 1u);
  node_t *keyword = node_get_child(node, CLUX_STMT_FOREACH_KEYWORD);
  ASSERT_NE(keyword, nullptr);
  EXPECT_EQ(node_get_kind(keyword), CLUX_NODE_SYMBOL);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_FOREACH_TYPE), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 省略 var 表示只读迭代。 */
TEST(StmtForeachTest, WithoutVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "foreach (v of a) { }");
  expect_foreach_stmt(node);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_FOREACH_KEYWORD), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForeachTest, TypedVariable) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "foreach (var item: Item of items) { }");
  expect_foreach_stmt(node);
  node_t *type = node_get_child(node, CLUX_STMT_FOREACH_TYPE);
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(node_get_kind(type), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 被迭代对象可以是表达式。 */
TEST(StmtForeachTest, SliceIterable) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "foreach (var v of a[1:3]) { }");
  expect_foreach_stmt(node);
  node_t *iterable = node_get_child(node, CLUX_STMT_FOREACH_ITERABLE);
  ASSERT_NE(iterable, nullptr);
  EXPECT_EQ(node_get_kind(iterable), CLUX_NODE_EXPR_SLICE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForeachTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; foreach (var v of xs) { b; } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_FOREACH);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 foreach 关键字到循环体终点。 */
TEST(StmtForeachTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "foreach (var v of a) { b; }";
  node_t *node = parse_stmt(a, &p, src);
  expect_foreach_stmt(node);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

TEST(StmtForeachTest, MissingOpenParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "foreach var v of a) { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '(' after 'foreach'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForeachTest, MissingNameReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "foreach (var of a) { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected iteration variable name in foreach");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForeachTest, MissingOfReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "foreach (var v a) { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected 'of' in foreach");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForeachTest, MissingIterableReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "foreach (var v of ) { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected iterable expression");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForeachTest, MissingCloseParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "foreach (var v of a { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ')' after foreach iterable");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForeachTest, MissingBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "foreach (var v of a)");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_FOREACH_BODY), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' to start foreach body");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtForeachTest, ReturnsNullOnNonForeach) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_foreach(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= do-while 语句 =================
   语法：do { body } while (cond);（白皮书 §4.7）。 */

/* 断言 do-while 节点：BODY 与 CONDITION 必存在。 */
static node_t *expect_do_while_stmt(node_t *node) {
  if (!node) {
    ADD_FAILURE() << "expected do-while statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_DO_WHILE);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_DO_WHILE_CHILDREN_SIZE);
  node_t *body = node_get_child(node, CLUX_STMT_DO_WHILE_BODY);
  EXPECT_NE(body, nullptr);
  if (body) {
    EXPECT_EQ(node_get_kind(body), CLUX_NODE_STMT_BLOCK);
  }
  EXPECT_NE(node_get_child(node, CLUX_STMT_DO_WHILE_CONDITION), nullptr);
  return body;
}

TEST(StmtDoWhileTest, BasicDoWhile) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "do { a; } while (x);");
  node_t *body = expect_do_while_stmt(node);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(stmt_block_node_get_stmt_count(body), 1u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDoWhileTest, EmptyBody) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "do {} while (x);");
  node_t *body = expect_do_while_stmt(node);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(stmt_block_node_get_stmt_count(body), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 条件可以是完整表达式。 */
TEST(StmtDoWhileTest, ConditionExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "do { } while (i < n && ok);");
  expect_do_while_stmt(node);
  node_t *condition = node_get_child(node, CLUX_STMT_DO_WHILE_CONDITION);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(node_get_kind(condition), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDoWhileTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; do { b; } while (x); }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_DO_WHILE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 do 关键字到末尾 ';'。 */
TEST(StmtDoWhileTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "do { a; } while (x);";
  node_t *node = parse_stmt(a, &p, src);
  expect_do_while_stmt(node);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

TEST(StmtDoWhileTest, MissingBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "do while (x);");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_DO_WHILE_BODY), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' to start do body");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDoWhileTest, MissingWhileReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "do { } (x);");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected 'while' after do body");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDoWhileTest, MissingOpenParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "do { } while x;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '(' after 'while'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDoWhileTest, MissingConditionReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "do { } while ();");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected condition expression");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDoWhileTest, MissingCloseParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "do { } while (x;");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ')' after do-while condition");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDoWhileTest, MissingSemicolonReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "do { } while (x)");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ';' after do-while statement");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDoWhileTest, ReturnsNullOnNonDoWhile) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_do_while(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= switch 语句 =================
   语法：switch (value) { branch... }（白皮书 §4.5）。
   分支写作 (pattern, ...) -> { ... } 或 default -> { ... }。 */

/* 断言 switch 节点：VALUE 与 BODY 必存在。 */
static node_t *expect_switch_stmt(node_t *node) {
  if (!node) {
    ADD_FAILURE() << "expected switch statement, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_SWITCH);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_SWITCH_CHILDREN_SIZE);
  EXPECT_NE(node_get_child(node, CLUX_STMT_SWITCH_VALUE), nullptr);
  node_t *body = node_get_child(node, CLUX_STMT_SWITCH_BODY);
  EXPECT_NE(body, nullptr);
  if (body) {
    EXPECT_EQ(node_get_kind(body), CLUX_NODE_SWITCH_BRANCHES);
  }
  return body;
}

TEST(StmtSwitchTest, BasicSwitch) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(
      a, &p, "switch (v) { (1) -> { a; } (2, 3) -> { b; } default -> { c; } }");
  node_t *branches = expect_switch_stmt(node);
  ASSERT_NE(branches, nullptr);
  ASSERT_EQ(switch_branches_node_get_branch_count(branches), 3u);

  node_t *first = switch_branches_node_get_branch(branches, 0);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(node_get_kind(first), CLUX_NODE_SWITCH_BRANCH);
  node_t *first_patterns = switch_branch_node_get_patterns(first);
  ASSERT_NE(first_patterns, nullptr);
  EXPECT_EQ(switch_patterns_node_get_pattern_count(first_patterns), 1u);
  EXPECT_NE(switch_branch_node_get_body(first), nullptr);

  node_t *second = switch_branches_node_get_branch(branches, 1);
  ASSERT_NE(second, nullptr);
  node_t *second_patterns = switch_branch_node_get_patterns(second);
  ASSERT_NE(second_patterns, nullptr);
  EXPECT_EQ(switch_patterns_node_get_pattern_count(second_patterns), 2u);

  node_t *default_branch = switch_branches_node_get_branch(branches, 2);
  ASSERT_NE(default_branch, nullptr);
  EXPECT_EQ(switch_branch_node_get_patterns(default_branch), nullptr);
  EXPECT_NE(switch_branch_node_get_body(default_branch), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, EmptyBody) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch (v) {}");
  node_t *branches = expect_switch_stmt(node);
  ASSERT_NE(branches, nullptr);
  EXPECT_EQ(switch_branches_node_get_branch_count(branches), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, SingleDefault) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch (v) { default -> { a; } }");
  node_t *branches = expect_switch_stmt(node);
  ASSERT_NE(branches, nullptr);
  ASSERT_EQ(switch_branches_node_get_branch_count(branches), 1u);
  node_t *branch = switch_branches_node_get_branch(branches, 0);
  EXPECT_EQ(switch_branch_node_get_patterns(branch), nullptr);
  EXPECT_NE(switch_branch_node_get_body(branch), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 匹配值可以是完整表达式。 */
TEST(StmtSwitchTest, ExpressionValue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch (a + b) { (1) -> { } }");
  expect_switch_stmt(node);
  node_t *value = node_get_child(node, CLUX_STMT_SWITCH_VALUE);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(node_get_kind(value), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; switch (v) { default -> { } } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_SWITCH);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 switch 关键字到分支列表终点。 */
TEST(StmtSwitchTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "switch (v) { (1) -> { a; } }";
  node_t *node = parse_stmt(a, &p, src);
  expect_switch_stmt(node);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误路径 ---- */

TEST(StmtSwitchTest, MissingOpenParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch v) { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '(' after 'switch'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, MissingValueReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch () { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected switch value expression");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, MissingCloseParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch (v { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ')' after switch value");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, MissingBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch (v)");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_child(node, CLUX_STMT_SWITCH_BODY), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' to start switch body");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, MissingArrowReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch (v) { (1) { } }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '->' after switch branch");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, MissingDefaultArrowReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch (v) { default { } }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '->' after switch branch");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, MissingBranchBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch (v) { (1) -> }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '{' to start switch branch body");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, InvalidBranchStartReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch (v) { 1 -> { } }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected switch branch");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, EmptyPatternListReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch (v) { () -> { } }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected pattern in switch branch");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, UnclosedBodyReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch (v) { (1) -> { }");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '}' to close switch body");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtSwitchTest, ReturnsNullOnNonSwitch) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_switch(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= comptime if 语句 =================
   语法：comptime if (cond) { ... }（白皮书 §4.15）。
   槽布局与普通 if 相同，else if 链保持 comptime 语义。 */

TEST(StmtComptimeIfTest, BasicComptimeIf) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "comptime if (N == 3) { a; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_COMPTIME_IF);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_IF_CHILDREN_SIZE);
  node_t *condition = stmt_if_node_get_condition(node);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(node_get_kind(condition), CLUX_NODE_EXPR_BINARY);
  node_t *then_branch = stmt_if_node_get_then(node);
  ASSERT_NE(then_branch, nullptr);
  EXPECT_EQ(node_get_kind(then_branch), CLUX_NODE_STMT_BLOCK);
  EXPECT_EQ(stmt_if_node_get_else(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtComptimeIfTest, WithElse) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "comptime if (N) { } else { a; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_COMPTIME_IF);
  node_t *else_branch = stmt_if_node_get_else(node);
  ASSERT_NE(else_branch, nullptr);
  EXPECT_EQ(node_get_kind(else_branch), CLUX_NODE_STMT_BLOCK);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* else if 链中的嵌套节点也必须是 comptime if。 */
TEST(StmtComptimeIfTest, ElseIfChainStaysComptime) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "comptime if (a) { } else if (b) { } else { c; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_COMPTIME_IF);
  node_t *else_branch = stmt_if_node_get_else(node);
  ASSERT_NE(else_branch, nullptr);
  EXPECT_EQ(node_get_kind(else_branch), CLUX_NODE_STMT_COMPTIME_IF);
  node_t *nested_else = stmt_if_node_get_else(else_branch);
  ASSERT_NE(nested_else, nullptr);
  EXPECT_EQ(node_get_kind(nested_else), CLUX_NODE_STMT_BLOCK);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtComptimeIfTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "{ a; comptime if (N) { b; } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 0)), CLUX_NODE_STMT_EXPR);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)), CLUX_NODE_STMT_COMPTIME_IF);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 comptime 关键字到分支终点。 */
TEST(StmtComptimeIfTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "comptime if (N) { a; } else { b; }";
  node_t *node = parse_stmt(a, &p, src);
  ASSERT_NE(node, nullptr);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* comptime 后缺 if。 */
TEST(StmtComptimeIfTest, MissingIfReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "comptime");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_COMPTIME_IF);
  EXPECT_EQ(stmt_if_node_get_condition(node), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected 'if' after 'comptime'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtComptimeIfTest, ReturnsNullOnNonComptime) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a;");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_comptime_if(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= comptime foreach 语句 =================
   语法：comptime foreach (...) { ... }（白皮书 §4.15）。
   槽布局与普通 foreach 相同，在编译期展开。 */

TEST(StmtComptimeForeachTest, BasicComptimeForeach) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "comptime foreach (var x of seq) { a; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_COMPTIME_FOREACH);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_STMT_FOREACH_CHILDREN_SIZE);
  EXPECT_NE(stmt_foreach_node_get_name(node), nullptr);
  EXPECT_NE(stmt_foreach_node_get_iterable(node), nullptr);
  node_t *body = stmt_foreach_node_get_body(node);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(node_get_kind(body), CLUX_NODE_STMT_BLOCK);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtComptimeForeachTest, InsideBlock) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "{ a; comptime foreach (var x of seq) { b; } }");
  expect_block(node, 2);
  EXPECT_EQ(node_get_kind(node_get_child(node, 1)),
            CLUX_NODE_STMT_COMPTIME_FOREACH);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 comptime 关键字到循环体终点。 */
TEST(StmtComptimeForeachTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "comptime foreach (var x of seq) { a; }";
  node_t *node = parse_stmt(a, &p, src);
  ASSERT_NE(node, nullptr);
  expect_location_span(node, src, src);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* comptime 后接 foreach 才由本函数认领；comptime if 必须返回 NULL
   且游标不动，交给 comptime if 语句。 */
TEST(StmtComptimeForeachTest, DoesNotStealComptimeIf) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "comptime if (N) { }");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_comptime_foreach(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtComptimeForeachTest, ReturnsNullOnNonComptime) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "foreach (var x of seq) { }");
  size_t before = parser_get_position(p);
  node_t *node = parser_read_stmt_comptime_foreach(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), before);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 头部错误走与普通 foreach 相同的容错路径。 */
TEST(StmtComptimeForeachTest, MissingOpenParenReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "comptime foreach var x of seq) { }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_COMPTIME_FOREACH);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected '(' after 'foreach'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 语句分派：enum / union / cunion / struct / defer / if / comptime if /
   while / for / foreach / do-while / switch 直接进入对应语句节点。 */
TEST(StmtDispatchTest, DispatchesEnum) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "enum E: u8 { A = 1 }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_ENUM);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesUnion) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "union U { a: i32; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_UNION);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesCunion) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "cunion U { a: i32; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_UNION);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesStruct) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "struct S { x: i32; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_STRUCT);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesImport) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "import math from \"math.cx\";");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_IMPORT);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesReexport) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export * from \"other.cx\";");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_REEXPORT);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesExternLib) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern lib crt from \"c\";");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EXTERN_LIB);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesExternCfunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "extern cfunc crt::puts(s: *const u8): i32;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EXTERN_CFUNC);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesExternVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "extern var crt::errno: i32;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EXTERN_VAR);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesExportExternCfunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_stmt(a, &p, "export extern cfunc zlib::zlibVersion(): str;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EXTERN_CFUNC);
  EXPECT_NE(stmt_extern_cfunc_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesInterface) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "interface I { func f(): void; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_INTERFACE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesExportInterface) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export interface I { func f(): void; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_INTERFACE);
  EXPECT_NE(stmt_interface_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesStaticAssert) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "static_assert(true, \"m\");");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_STATIC_ASSERT);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesAssert) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "assert(x == 4);");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_ASSERT);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesExportFunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export func f(): void {}");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_FUNC);
  EXPECT_NE(stmt_func_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesExportStruct) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export struct S { x: i32; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_STRUCT);
  EXPECT_NE(stmt_struct_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesExportComptimeVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export comptime var x = 1;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_VAR);
  EXPECT_NE(stmt_var_node_get_export(node), nullptr);
  EXPECT_NE(stmt_var_node_get_comptime(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesDecoratedExportEnum) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] export enum E: u8 { A = 1 }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_ENUM);
  EXPECT_NE(stmt_enum_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesBitfield) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "bitfield F: u8 { x: 1; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_BITFIELD);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesDecoratedBitfield) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] bitfield F: u8 { x: 1; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_BITFIELD);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesExportBitfield) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "export bitfield F: u8 { x: 1; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_BITFIELD);
  EXPECT_NE(stmt_bitfield_node_get_export(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesTest) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "test \"Suite.Case\" { assert(x); }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_TEST);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesDefer) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "defer { a; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_DEFER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesComptimeFunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "comptime func f(): void {}");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_FUNC);
  EXPECT_NE(stmt_func_node_get_comptime(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesDecoratedComptimeFunc) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] comptime func f(): void {}");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_FUNC);
  EXPECT_NE(stmt_func_node_get_comptime(node), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesComptimeVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "comptime var x = 1;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_VAR);
  EXPECT_NE(node_get_child(node, CLUX_STMT_VAR_COMPTIME), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesDecoratedComptimeVar) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] comptime var x = 1;");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_VAR);
  EXPECT_NE(node_get_child(node, CLUX_STMT_VAR_COMPTIME), nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesComptimeIf) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "comptime if (N) { a; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_COMPTIME_IF);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesEmpty) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, ";");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_EMPTY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesSwitch) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "switch (v) { default -> { } }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_SWITCH);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesDoWhile) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "do { a; } while (x);");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_DO_WHILE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesForeach) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "foreach (var v of a) { b; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_FOREACH);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesComptimeForeach) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "comptime foreach (var x of seq) { b; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_COMPTIME_FOREACH);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesFor) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "for (var i = 0; i < 1; i += 1) { a; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_FOR);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesWhile) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "while (x) { a; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_WHILE);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesIf) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "if (x) { a; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_IF);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesDecoratedStruct) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] struct S { x: i32; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_STRUCT);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(StmtDispatchTest, DispatchesDecoratedEnumAndUnion) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_stmt(a, &p, "[[trace]] enum E: u8 { A = 1 }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_ENUM);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);

  a = create_allocator(malloc, free);
  node = parse_stmt(a, &p, "[[trace]] union U { a: i32; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_STMT_UNION);
  delete_node(&node);
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
      "comptime func f(): void {}",
      "[[trace]] comptime func f(): void {}",
      "export func f(): void {}",
      "[[trace]] export comptime func f(): void {}",
      "export var x: i32 = 1;",
      "[[trace]] export comptime var LIMIT: const u32 = 32;",
      "export type UserId = u64;",
      "export enum E: u8 { A = 1 }",
      "export union U { a: i32; }",
      "export cunion U { a: i32; }",
      "export struct S { x: i32; }",
      "export",
      "import math from \"math.cx\";",
      "import app from \"app/main.cx\";",
      "import from \"math.cx\";",
      "import math \"math.cx\";",
      "import math from ;",
      "import math from \"math.cx\"",
      "export * from \"other.cx\";",
      "export * \"other.cx\";",
      "export * from ;",
      "export * from \"other.cx\"",
      "extern lib crt from \"c\";",
      "extern lib self from \"*\";",
      "extern lib from \"c\";",
      "extern lib crt \"c\";",
      "extern lib crt from ;",
      "extern lib crt from \"c\"",
      "extern cfunc crt::puts(s: *const u8): i32;",
      "extern var crt::errno: i32;",
      "export extern cfunc zlib::zlibVersion(): str;",
      "export extern var zlib::zlib_errno: i32;",
      "extern cfunc ::puts(): i32;",
      "extern cfunc crt puts(): i32;",
      "extern cfunc crt::(): i32;",
      "extern cfunc crt::puts: i32;",
      "extern cfunc crt::puts()",
      "extern cfunc crt::puts(): ;",
      "extern cfunc crt::puts(): i32",
      "extern var crt::errno i32;",
      "extern var crt::errno: ;",
      "extern var crt::errno: i32",
      "interface IWriter[W] { func write(self: *W, data: []u8): void; }",
      "export interface Sized[T] { func size_of(self: *T): usize; }",
      "[[trace]] export interface I { func f(): void; }",
      "{ a; interface I { func f(): void; } }",
      "interface I { func f(): void }",
      "interface I { x: i32; }",
      "interface I",
      "[[trace]] interface",
      "static_assert(true, \"m\");",
      "static_assert(sizeof(i64) == 8 as u64, \"expected 64-bit i64\");",
      "static_assert true, \"m\");",
      "static_assert(, \"m\");",
      "static_assert(true \"m\");",
      "static_assert(true, );",
      "static_assert(true, \"m\";",
      "static_assert(true, \"m\")",
      "assert(x == 4);",
      "assert();",
      "assert(x;",
      "assert(x)",
      "assert;",
      "{ a; func f(): void {} }",
      "[[trace]] a;",
      "[trace] func f(): void {}",
      "[[trace]] func f(): void",
      "enum E: u8 { A = 1 }",
      "[[trace]] enum E: u8 { A = 1 }",
      "[[trace]] [[metadata(\"role\", \"handler\")]] enum E: u8 { A = 1 }",
      "{ a; enum E: u8 { A = 1 } }",
      "enum E: u8 { A = }",
      "enum E: u8 { A = 1",
      "[[trace]] enum",
      "union U { a: i32; }",
      "cunion U { a: i32; }",
      "[[trace]] union U { a: i32; }",
      "[[trace]] cunion U { a: i32; }",
      "{ a; union U { a: i32; } }",
      "union U { a: i32 }",
      "union U",
      "[[trace]] union",
      "struct S { x: i32; }",
      "[[trace]] struct S { x: i32; }",
      "[[trace]] [[metadata(\"role\", \"handler\")]] struct S { x: i32; }",
      "{ a; struct S { x: i32; } }",
      "struct S { var total: i32 = 0; x: i32; func f(self: *S): void {} "
      "...Base; }",
      "struct S { [[metadata(\"unit\", \"px\")]] x: i32; }",
      "struct S { x: i32 }",
      "struct S { 1: i32; }",
      "struct S",
      "[[trace]] struct",
      "bitfield F: u8 { x: 1; }",
      "[[trace]] bitfield F: u8 { x: 1; }",
      "[[trace]] [[metadata(\"role\", \"flags\")]] bitfield F: u8 { x: 1; }",
      "{ a; bitfield F: u8 { x: 1; } }",
      "bitfield F: u8 { x: 1 }",
      "bitfield F: u8 { 1: 1; }",
      "bitfield F: u8",
      "[[trace]] bitfield",
      "test \"Suite.Case\" { assert(x); }",
      "test \"Suite.Empty\" {}",
      "test \"Suite.Case\"",
      "test;",
      "defer { a; }",
      "defer {}",
      "defer |captured| { a; }",
      "defer |pa = a.&| { a; }",
      "defer |(h = handle)| { a; }",
      "defer |a, b = 1| { }",
      "defer || { }",
      "{ a; defer { b; } }",
      "defer",
      "defer |a { }",
      "defer |(a { }",
      "defer |1| { }",
      "if (x) { a; }",
      "if (x) { a; } else { b; }",
      "if (a) { } else if (b) { } else { c; }",
      "if (x) {} else {}",
      "if (a + b > 0) { }",
      "{ a; if (x) { b; } }",
      "if x) { }",
      "if () { }",
      "if (x { }",
      "if (x)",
      "if (x) { } else a;",
      "while (x) { a; }",
      "while (x) {}",
      "while (i < n && ok) { i = i + 1; }",
      "{ a; while (x) { b; } }",
      "while x) { }",
      "while () { }",
      "while (x { }",
      "while (x)",
      "for (var i = 0; i < 10; i += 1) { a; }",
      "for (i = 0; i < 10; i += 1) { }",
      "for (; i < 10; i += 1) { }",
      "for (var i = 0; ; i += 1) { }",
      "for (var i = 0; i < 10; ) { }",
      "for (;;) { }",
      "{ a; for (var i = 0; i < 1; i += 1) { b; } }",
      "for var i = 0; i < 1; i += 1) { }",
      "for (1 2; i < 1; i += 1) { }",
      "for (var i = 0; i < 1 i += 1) { }",
      "for (var i = 0; i < 1; i += 1 { }",
      "for (var i = 0; i < 1; i += 1)",
      "foreach (var v of a) { b; }",
      "foreach (v of a) { }",
      "foreach (var item: Item of items) { }",
      "foreach (var v of a[1:3]) { }",
      "{ a; foreach (var v of xs) { b; } }",
      "foreach var v of a) { }",
      "foreach (var of a) { }",
      "foreach (var v a) { }",
      "foreach (var v of ) { }",
      "foreach (var v of a { }",
      "foreach (var v of a)",
      "do { a; } while (x);",
      "do {} while (x);",
      "do { } while (i < n && ok);",
      "{ a; do { b; } while (x); }",
      "do while (x);",
      "do { } (x);",
      "do { } while x;",
      "do { } while ();",
      "do { } while (x;",
      "do { } while (x)",
      "switch (v) { (1) -> { a; } (2, 3) -> { b; } default -> { c; } }",
      "switch (v) {}",
      "switch (v) { default -> { a; } }",
      "switch (a + b) { (1) -> { } }",
      "{ a; switch (v) { default -> { } } }",
      "switch v) { }",
      "switch () { }",
      "switch (v { }",
      "switch (v)",
      "switch (v) { (1) { } }",
      "switch (v) { default { } }",
      "switch (v) { (1) -> }",
      "switch (v) { 1 -> { } }",
      "switch (v) { () -> { } }",
      "switch (v) { (1) -> { }",
      ";",
      "{ ; a; }",
      "{ ; ; a; }",
      "{ { a; }; b; }",
      "comptime if (N == 3) { a; }",
      "comptime if (N) { } else { a; }",
      "comptime if (a) { } else if (b) { } else { c; }",
      "{ a; comptime if (N) { b; } }",
      "comptime",
      "comptime foreach (var x of seq) { a; }",
      "{ a; comptime foreach (var x of seq) { b; } }",
      "comptime foreach var x of seq) { }",
      "comptime var x = 1;",
      "comptime using a = e;",
      "[[trace]] comptime var x = 1;",
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
