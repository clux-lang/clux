#include <gtest/gtest.h>
#include "parser/literal_nil.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/parser.h"
#include "core/allocator.h"
#include "core/vec.h"
#include <stdlib.h>

TEST(LiteralNilTest, MatchesNilKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "nil");
  node_t *node = parser_read_literal_nil(a, p, NULL);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_NIL);
  EXPECT_EQ(node_get_parent(node), nullptr);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNilTest, ConsumesToken) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "nil");
  EXPECT_EQ(parser_get_position(p), 0u);
  node_t *node = parser_read_literal_nil(a, p, NULL);
  ASSERT_NE(node, nullptr);
  /* 匹配成功后游标前进，重复调用不再匹配 */
  EXPECT_EQ(parser_get_position(p), 1u);
  node_t *again = parser_read_literal_nil(a, p, NULL);
  EXPECT_EQ(again, nullptr);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNilTest, RejectsOtherKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "none");
  node_t *node = parser_read_literal_nil(a, p, NULL);
  EXPECT_EQ(node, nullptr);
  /* 不匹配时不消费 token */
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNilTest, RejectsIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "nilx");
  node_t *node = parser_read_literal_nil(a, p, NULL);
  EXPECT_EQ(node, nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNilTest, RejectsString) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"nil\"");
  node_t *node = parser_read_literal_nil(a, p, NULL);
  EXPECT_EQ(node, nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNilTest, EmptyTokenStreamReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  node_t *node = parser_read_literal_nil(a, p, NULL);
  EXPECT_EQ(node, nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNilTest, LocationMatchesToken) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "  nil";
  parser_t *p = create_parser(a, "test.clux", src);
  /* 跳过前导空白 */
  parser_next(p);
  node_t *node = parser_read_literal_nil(a, p, NULL);
  ASSERT_NE(node, nullptr);
  const location_t *loc = node_get_location(node);
  EXPECT_EQ(loc->begin.offset, src + 2);
  EXPECT_EQ(loc->end.offset, src + 5);
  EXPECT_STREQ(loc->filename, "test.clux");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNilTest, AttachesParent) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "nil");
  location_t ploc = {
      .filename = "test.clux",
      .begin = {.offset = "nil", .line = 1, .column = 1},
      .end = {.offset = "nil", .line = 1, .column = 1},
  };
  node_t *parent = create_node(a, CLUX_NODE_PROGRAM, NULL, ploc);
  node_t *node = parser_read_literal_nil(a, p, parent);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_parent(node), parent);
  delete_node(&node);
  delete_node(&parent);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNilTest, NilIsKeywordToken) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "nil");
  token_t *tok = parser_peek_token(p);
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD);
  delete_parser(&p);
  delete_allocator(&a);
}
