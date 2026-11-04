#include <gtest/gtest.h>
#include "parser/literal.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/parser.h"
#include "core/allocator.h"
#include "core/vec.h"
#include <stdlib.h>
#include <string.h>

/* ---- undefined ---- */

TEST(LiteralUndefinedTest, MatchesKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "undefined");
  node_t *node = parser_read_literal_undefined(a, p, NULL);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_UNDEFINED);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralUndefinedTest, RejectsOtherKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "none");
  EXPECT_EQ(parser_read_literal_undefined(a, p, NULL), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralUndefinedTest, RejectsIdentifierPrefix) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "undefinedx");
  EXPECT_EQ(parser_read_literal_undefined(a, p, NULL), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralUndefinedTest, EmptyStreamReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  EXPECT_EQ(parser_read_literal_undefined(a, p, NULL), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- string ---- */

TEST(LiteralStringTest, MatchesString) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"hi\"");
  node_t *node = parser_read_literal_string(a, p, NULL);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_STRING);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralStringTest, MatchesEmptyString) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"\"");
  node_t *node = parser_read_literal_string(a, p, NULL);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_STRING);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralStringTest, RejectsCharacter) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "'a'");
  EXPECT_EQ(parser_read_literal_string(a, p, NULL), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralStringTest, RejectsIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "x");
  EXPECT_EQ(parser_read_literal_string(a, p, NULL), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- character ---- */

TEST(LiteralCharacterTest, MatchesCharacter) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "'a'");
  node_t *node = parser_read_literal_character(a, p, NULL);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_CHARACTER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralCharacterTest, MatchesEscapeCharacter) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "'\\n'");
  node_t *node = parser_read_literal_character(a, p, NULL);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_CHARACTER);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralCharacterTest, RejectsString) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"a\"");
  EXPECT_EQ(parser_read_literal_character(a, p, NULL), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- boolean ---- */

TEST(LiteralBooleanTest, MatchesTrue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "true");
  node_t *node = parser_read_literal_boolean(a, p, NULL);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_BOOLEAN);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralBooleanTest, MatchesFalse) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "false");
  node_t *node = parser_read_literal_boolean(a, p, NULL);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_BOOLEAN);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralBooleanTest, RejectsOtherIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "truely");
  EXPECT_EQ(parser_read_literal_boolean(a, p, NULL), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralBooleanTest, RejectsKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "nil");
  EXPECT_EQ(parser_read_literal_boolean(a, p, NULL), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- identifier ---- */

TEST(LiteralIdentifierTest, MatchesIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "foo");
  node_t *node = parser_read_literal_identifier(a, p, NULL);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralIdentifierTest, RejectsTrueKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "true");
  /* true 现在是关键字，不再是标识符 */
  EXPECT_EQ(parser_read_literal_identifier(a, p, NULL), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralIdentifierTest, RejectsKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "var");
  EXPECT_EQ(parser_read_literal_identifier(a, p, NULL), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralIdentifierTest, RejectsNumeric) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "123");
  EXPECT_EQ(parser_read_literal_identifier(a, p, NULL), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- numeric ---- */

static node_t *read_numeric(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_literal_numeric(a, *p, NULL);
}

TEST(LiteralNumericTest, MatchesPlainInteger) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = read_numeric(a, &p, "123");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_NUMERIC);
  EXPECT_EQ(parser_get_position(p), 1u);
  /* 无后缀时 location 只覆盖数字 */
  EXPECT_EQ(node_get_location(node)->end.offset,
            node_get_location(node)->begin.offset + 3);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, AppliesEverySuffix) {
  const char *suffixes[] = {"i8",
                            "i16",
                            "i32",
                            "i64",
                            "isize",
                            "u8",
                            "u16",
                            "u32",
                            "u64",
                            "usize",
                            "f32",
                            "f64",
                            nullptr};
  for (size_t i = 0; suffixes[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    char src[32];
    snprintf(src, sizeof(src), "123%s", suffixes[i]);
    parser_t *p;
    node_t *node = read_numeric(a, &p, src);
    ASSERT_NE(node, nullptr) << "suffix=" << suffixes[i];
    EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_NUMERIC);
    EXPECT_EQ(parser_get_position(p), 2u) << "suffix=" << suffixes[i];
    /* location 覆盖数字 + 后缀 */
    EXPECT_EQ(node_get_location(node)->end.offset,
              node_get_location(node)->begin.offset + strlen(src))
        << "suffix=" << suffixes[i];
    delete_node(&node);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

TEST(LiteralNumericTest, SuffixAfterSpaceIsNotConsumed) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = read_numeric(a, &p, "123 i32");
  ASSERT_NE(node, nullptr);
  /* 空白隔开 → 后缀不属于该数字 */
  EXPECT_EQ(parser_get_position(p), 1u);
  EXPECT_EQ(node_get_location(node)->end.offset,
            node_get_location(node)->begin.offset + 3);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, SuffixAfterCommentIsNotConsumed) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = read_numeric(a, &p, "123/*c*/i32");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, NonNumericSuffixIsNotConsumed) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  /* "123abc"：abc 是标识符，不是类型后缀 */
  node_t *node = read_numeric(a, &p, "123abc");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, RejectsIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "abc");
  EXPECT_EQ(parser_read_literal_numeric(a, p, NULL), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, RejectsString) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"1\"");
  EXPECT_EQ(parser_read_literal_numeric(a, p, NULL), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, EmptyStreamReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  EXPECT_EQ(parser_read_literal_numeric(a, p, NULL), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, AttachesParent) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "42i32";
  parser_t *p = create_parser(a, "test.clux", src);
  location_t ploc = {
      .filename = "test.clux",
      .begin = {.offset = src, .line = 1, .column = 1},
      .end = {.offset = src, .line = 1, .column = 1},
  };
  node_t *parent = create_node(a, CLUX_NODE_PROGRAM, NULL, ploc);
  node_t *node = parser_read_literal_numeric(a, p, parent);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_parent(node), parent);
  delete_node(&node);
  delete_node(&parent);
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

TEST(LiteralTest, NoLeakAcrossAllReaders) {
  const char *sources[] = {
      "undefined", "\"s\"", "'c'", "true", "123i32", "foo", nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *node = parser_read_literal_undefined(a, p, NULL);
    if (!node) node = parser_read_literal_string(a, p, NULL);
    if (!node) node = parser_read_literal_character(a, p, NULL);
    if (!node) node = parser_read_literal_boolean(a, p, NULL);
    if (!node) node = parser_read_literal_numeric(a, p, NULL);
    if (!node) node = parser_read_literal_identifier(a, p, NULL);
    EXPECT_NE(node, nullptr) << "src=" << sources[i];
    delete_node(&node);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}

TEST(LiteralNumericTest, OtherKeywordSuffixIsNotConsumed) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  /* "123var"：var 是关键字但不是类型后缀 */
  node_t *node = read_numeric(a, &p, "123var");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(parser_get_position(p), 1u);
  EXPECT_EQ(node_get_location(node)->end.offset,
            node_get_location(node)->begin.offset + 3);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralBooleanTest, TrueAndFalseAreKeywords) {
  allocator_t *a = create_allocator(malloc, free);
  const char *sources[] = {"true", "false", nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    token_t *tok = parser_peek_token(p);
    ASSERT_NE(tok, nullptr) << sources[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD) << sources[i];
    delete_parser(&p);
  }
  delete_allocator(&a);
}

/* ---- nil ---- */

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
