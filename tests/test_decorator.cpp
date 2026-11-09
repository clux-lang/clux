#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser/decorator.h"
#include "parser/node.h"
#include "parser/parser.h"
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
  if (ptr) g_free_count++;
  free(ptr);
}

static node_t *parse_decorator(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_decorator(a, *p);
}

/* 断言装饰器节点形状：只有一个子节点，且是表达式。 */
static node_t *expect_decorator(node_t *node) {
  if (!node) {
    ADD_FAILURE() << "expected decorator node, got NULL";
    return nullptr;
  }
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_DECORATOR);
  EXPECT_EQ(vec_get_size(node_get_children(node)),
            (size_t)CLUX_DECORATOR_CHILDREN_SIZE);
  node_t *expr = node_get_child(node, CLUX_DECORATOR_EXPR);
  EXPECT_NE(expr, nullptr);
  return expr;
}

/* ================= 基本形态 ================= */

TEST(DecoratorTest, Identifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_decorator(a, &p, "[[trace]]");
  node_t *expr = expect_decorator(node);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(DecoratorTest, Call) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_decorator(a, &p, "[[metadata(\"role\", \"handler\")]]");
  node_t *expr = expect_decorator(node);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_CALL);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 装饰器内的表达式走顶层入口：赋值、三元都允许。 */
TEST(DecoratorTest, ArbitraryExpression) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_decorator(a, &p, "[[a ? b : c]]");
  node_t *expr = expect_decorator(node);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_TERNARY);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 嵌套下标不会与闭合 `]]` 混淆。 */
TEST(DecoratorTest, NestedSubscript) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_decorator(a, &p, "[[a[b]]]");
  node_t *expr = expect_decorator(node);
  ASSERT_NE(expr, nullptr);
  EXPECT_EQ(node_get_kind(expr), CLUX_NODE_EXPR_SUBSCRIPT);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 闭合的两个 ']' 之间不允许空白：`]]` 是整体。 */
TEST(DecoratorTest, SeparatedCloseBracketsReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_decorator(a, &p, "[[trace] ]");
  ASSERT_NE(node, nullptr);
  expect_decorator(node);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ']]' to close decorator");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 起始两个 '[' 之间不允许空白（`[ [` 不是装饰器）。 */
TEST(DecoratorTest, SeparatedOpenBracketsIsNotDecorator) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[ [trace]]");
  node_t *node = parser_read_decorator(a, p);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(DecoratorTest, ReturnsNullOnNonBracket) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "trace");
  node_t *node = parser_read_decorator(a, p);
  EXPECT_EQ(node, nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(DecoratorTest, ReturnsNullOnSingleBracket) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "[trace]");
  node_t *node = parser_read_decorator(a, p);
  EXPECT_EQ(node, nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间覆盖 `[[` 起点到 `]]` 终点。 */
TEST(DecoratorTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "[[trace]]";
  node_t *node = parse_decorator(a, &p, src);
  expect_decorator(node);
  const location_t *loc = node_get_location(node);
  ASSERT_NE(loc, nullptr);
  EXPECT_EQ(loc->begin.offset, src);
  EXPECT_EQ(loc->end.offset, src + strlen(src));
  EXPECT_STREQ(loc->filename, "test.clux");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 错误路径 ================= */

TEST(DecoratorTest, MissingExpressionReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_decorator(a, &p, "[[]]");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_child(node, CLUX_DECORATOR_EXPR), nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected expression in decorator");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(DecoratorTest, MissingCloseReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_decorator(a, &p, "[[trace]");
  ASSERT_NE(node, nullptr);
  expect_decorator(node);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ']]' to close decorator");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(DecoratorTest, MissingBothCloseReportsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_decorator(a, &p, "[[trace");
  ASSERT_NE(node, nullptr);
  expect_decorator(node);
  vec_t *errors = parser_get_error(p);
  ASSERT_GE(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected ']]' to close decorator");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ================= 泄漏检查 ================= */

TEST(DecoratorTest, NoLeakOnDecorators) {
  const char *sources[] = {"[[trace]]",
                           "[[metadata(\"role\", \"handler\")]]",
                           "[[a ? b : c]]",
                           "[[a[b]]]",
                           "[[trace] ]",
                           "[[trace]  ]",
                           "[ [trace]]",
                           "trace",
                           "[trace]",
                           "[[]]",
                           "[[trace]",
                           "[[trace",
                           "[[",
                           "",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *node = parser_read_decorator(a, p);
    delete_node(&node);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}
