#include <gtest/gtest.h>
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/node.h"
#include "core/allocator.h"
#include <stdlib.h>

/* 表达式解析目前是占位实现（assign 返回 NULL），用例锁定当前契约，
   实现落地后这些会自然演进。 */

TEST(ExpressionTest, AssignReturnsNullForNow) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "1");
  EXPECT_EQ(parser_read_expression_assign(a, p, NULL), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTest, ExpressionDelegatesToAssign) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "1 + 2");
  EXPECT_EQ(parser_read_expression(a, p, NULL), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ExpressionTest, EmptyStreamReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  EXPECT_EQ(parser_read_expression_assign(a, p, NULL), nullptr);
  EXPECT_EQ(parser_read_expression(a, p, NULL), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}
