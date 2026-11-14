#include <gtest/gtest.h>
#include "parser/literal.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/parser.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/vec.h"
#include <stdlib.h>
#include <string.h>

/* ---- undefined ---- */

TEST(LiteralUndefinedTest, MatchesKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "undefined");
  node_t *node = parser_read_literal_undefined(a, p);
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
  EXPECT_EQ(parser_read_literal_undefined(a, p), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralUndefinedTest, RejectsIdentifierPrefix) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "undefinedx");
  EXPECT_EQ(parser_read_literal_undefined(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralUndefinedTest, EmptyStreamReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  EXPECT_EQ(parser_read_literal_undefined(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- string ---- */

TEST(LiteralStringTest, MatchesString) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"hi\"");
  node_t *node = parser_read_literal_string(a, p);
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
  node_t *node = parser_read_literal_string(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_STRING);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralStringTest, RejectsCharacter) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "'a'");
  EXPECT_EQ(parser_read_literal_string(a, p), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralStringTest, RejectsIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "x");
  EXPECT_EQ(parser_read_literal_string(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- character ---- */

TEST(LiteralCharacterTest, MatchesCharacter) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "'a'");
  node_t *node = parser_read_literal_character(a, p);
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
  node_t *node = parser_read_literal_character(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_CHARACTER);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralStringTest, AcceptsValidEscapes) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p =
      create_parser(a, "test.clux", "\"\\n\\t\\r\\\\\\'\\\"\\0\\x41\"");
  node_t *node = parser_read_literal_string(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralStringTest, RejectsInvalidEscape) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"\\q\"");
  node_t *node = parser_read_literal_string(a, p);
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "invalid escape sequence '\\q'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralStringTest, RejectsHexEscapeWithoutDigits) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"\\x\"");
  node_t *node = parser_read_literal_string(a, p);
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "expected hex digits after '\\x'");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralStringTest, AcceptsOneDigitHexEscape) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"\\x4\"");
  node_t *node = parser_read_literal_string(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralStringTest, RejectsRawNewline) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"a\nb\"");
  node_t *node = parser_read_literal_string(a, p);
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "string literal cannot span lines");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralCharacterTest, RejectsEmpty) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "''");
  node_t *node = parser_read_literal_character(a, p);
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "character literal cannot be empty");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralCharacterTest, RejectsMultipleCharacters) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "'ab'");
  node_t *node = parser_read_literal_character(a, p);
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "character literal must contain exactly one u8 character");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralCharacterTest, RejectsOutOfU8Range) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "'你'");
  node_t *node = parser_read_literal_character(a, p);
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "character literal value out of u8 range");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralCharacterTest, AcceptsHexEscape) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "'\\x41'");
  node_t *node = parser_read_literal_character(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralCharacterTest, RejectsString) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"a\"");
  EXPECT_EQ(parser_read_literal_character(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- boolean ---- */

TEST(LiteralBooleanTest, MatchesTrue) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "true");
  node_t *node = parser_read_literal_boolean(a, p);
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
  node_t *node = parser_read_literal_boolean(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_BOOLEAN);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralBooleanTest, RejectsOtherIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "truely");
  EXPECT_EQ(parser_read_literal_boolean(a, p), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralBooleanTest, RejectsKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "none");
  EXPECT_EQ(parser_read_literal_boolean(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- identifier ---- */

TEST(LiteralIdentifierTest, MatchesIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "foo");
  node_t *node = parser_read_literal_identifier(a, p);
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
  EXPECT_EQ(parser_read_literal_identifier(a, p), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralIdentifierTest, RejectsKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "var");
  EXPECT_EQ(parser_read_literal_identifier(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralIdentifierTest, RejectsNumeric) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "123");
  EXPECT_EQ(parser_read_literal_identifier(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- numeric ---- */

static node_t *read_numeric(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_literal_numeric(a, *p);
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

TEST(LiteralNumericTest, OctalAndBinaryLiterals) {
  allocator_t *a = create_allocator(malloc, free);
  const char *srcs[] = {"0o755", "0O17", "0b1010", "0B101", nullptr};
  for (size_t i = 0; srcs[i] != nullptr; i++) {
    parser_t *p;
    node_t *node = read_numeric(a, &p, srcs[i]);
    ASSERT_NE(node, nullptr) << "src=" << srcs[i];
    EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_NUMERIC);
    EXPECT_EQ(node_get_location(node)->end.offset,
              node_get_location(node)->begin.offset +
                  (ptrdiff_t)strlen(srcs[i]));
    delete_node(&node);
    delete_parser(&p);
  }
  delete_allocator(&a);
}

TEST(LiteralNumericTest, OctalAndBinarySuffix) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = read_numeric(a, &p, "0b1010u16");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_NUMERIC);
  EXPECT_EQ(parser_get_position(p), 2u);
  EXPECT_EQ(node_get_location(node)->end.offset,
            node_get_location(node)->begin.offset + 9);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, RejectsBasePrefixWithoutDigits) {
  const char *srcs[] = {"0x", "0o", "0b", nullptr};
  const char *messages[] = {"expected digits after '0x'",
                            "expected digits after '0o'",
                            "expected digits after '0b'",
                            nullptr};
  allocator_t *a = create_allocator(malloc, free);
  for (size_t i = 0; srcs[i] != nullptr; i++) {
    parser_t *p;
    node_t *node = read_numeric(a, &p, srcs[i]);
    ASSERT_NE(node, nullptr) << "src=" << srcs[i];
    vec_t *errors = parser_get_error(p);
    ASSERT_EQ(vec_get_size(errors), 1u) << "src=" << srcs[i];
    EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
                 messages[i]);
    delete_node(&node);
    delete_parser(&p);
  }
  delete_allocator(&a);
}

TEST(LiteralNumericTest, RejectsInvalidOctalDigit) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = read_numeric(a, &p, "0o8");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "invalid digit '8' in octal literal");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, RejectsInvalidBinaryDigit) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = read_numeric(a, &p, "0b102");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "invalid digit '2' in binary literal");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, RejectsIntegerSuffixOnFloat) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = read_numeric(a, &p, "1.5i32");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "floating literal requires 'f32' or 'f64' suffix");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, RejectsFloatSuffixOnInteger) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = read_numeric(a, &p, "1f32");
  ASSERT_NE(node, nullptr);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)),
               "integer literal cannot use floating suffix");
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, AcceptsMatchingSuffixes) {
  const char *srcs[] = {"1u8", "1.5f32", "0x1f32", "0o755i32", nullptr};
  allocator_t *a = create_allocator(malloc, free);
  for (size_t i = 0; srcs[i] != nullptr; i++) {
    parser_t *p;
    node_t *node = read_numeric(a, &p, srcs[i]);
    ASSERT_NE(node, nullptr) << "src=" << srcs[i];
    EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u) << "src=" << srcs[i];
    delete_node(&node);
    delete_parser(&p);
  }
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
  EXPECT_EQ(parser_read_literal_numeric(a, p), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, RejectsString) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"1\"");
  EXPECT_EQ(parser_read_literal_numeric(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, EmptyStreamReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  EXPECT_EQ(parser_read_literal_numeric(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralNumericTest, AttachesParent) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "42i32";
  parser_t *p = create_parser(a, "test.clux", src);
  location_t ploc = {
      .filename = "test.clux",
      .begin = {.offset = src, .column = 1, .line = 1},
      .end = {.offset = src, .column = 1, .line = 1},
  };
  node_t *parent = create_node(a, CLUX_NODE_PROGRAM);
  node_set_location(parent, &ploc);
  node_t *node = parser_read_literal_numeric(a, p);
  node_set_parent(node, parent);
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
  const char *sources[] = {"undefined",
                           "\"s\"",
                           "'c'",
                           "true",
                           "i32",
                           "opaque",
                           "123i32",
                           "foo",
                           nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *node = parser_read_literal_undefined(a, p);
    if (!node) node = parser_read_literal_string(a, p);
    if (!node) node = parser_read_literal_character(a, p);
    if (!node) node = parser_read_literal_boolean(a, p);
    if (!node) node = parser_read_literal_type(a, p);
    if (!node) node = parser_read_literal_numeric(a, p);
    if (!node) node = parser_read_literal_identifier(a, p);
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

/* ---- nil 已淘汰 ---- */

/* nil 不再是关键字：词法层产出普通标识符。 */
TEST(LiteralNoNilTest, NilIsNotKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "nil");
  token_t *tok = parser_peek_token(p);
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_IDENTIFIER);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 按标识符参与字面量分派，不再有专用 nil 字面量节点。 */
TEST(LiteralNoNilTest, NilParsesAsIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "nil");
  node_t *node = parser_read_literal(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_IDENTIFIER);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- parser_read_literal 聚合入口 ---- */

TEST(LiteralDispatchTest, DispatchesUndefined) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "undefined");
  node_t *node = parser_read_literal(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_UNDEFINED);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralDispatchTest, DispatchesBoolean) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "true");
  node_t *node = parser_read_literal(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_BOOLEAN);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralDispatchTest, DispatchesCharacter) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "'a'");
  node_t *node = parser_read_literal(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_CHARACTER);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralDispatchTest, DispatchesString) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"s\"");
  node_t *node = parser_read_literal(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_STRING);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralDispatchTest, DispatchesIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "foo");
  node_t *node = parser_read_literal(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_IDENTIFIER);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralDispatchTest, DispatchesNumeric) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "42i32");
  node_t *node = parser_read_literal(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_NUMERIC);
  EXPECT_EQ(parser_get_position(p), 2u); /* 数字 + 后缀 */
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralDispatchTest, ReturnsNullOnNonLiteral) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "var");
  EXPECT_EQ(parser_read_literal(a, p), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralDispatchTest, ReturnsNullOnEmptyStream) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  EXPECT_EQ(parser_read_literal(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralDispatchTest, AttachesParent) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "42";
  parser_t *p = create_parser(a, "test.clux", src);
  node_t *parent = create_node(a, CLUX_NODE_PROGRAM);
  node_t *node = parser_read_literal(a, p);
  node_set_parent(node, parent);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_parent(node), parent);
  delete_node(&node);
  delete_node(&parent);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 内建类型名 ---- */

static const char *kTypeNames[] = {"bool",
                                   "str",
                                   "void",
                                   "opaque",
                                   "i8",
                                   "i16",
                                   "i32",
                                   "i64",
                                   "u8",
                                   "u16",
                                   "u32",
                                   "u64",
                                   "f32",
                                   "f64",
                                   "isize",
                                   "usize",
                                   nullptr};

TEST(LiteralTypeTest, MatchesEveryBuiltinTypeName) {
  for (size_t i = 0; kTypeNames[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p = create_parser(a, "test.clux", kTypeNames[i]);
    node_t *node = parser_read_literal_type(a, p);
    ASSERT_NE(node, nullptr) << "name=" << kTypeNames[i];
    EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_TYPE)
        << "name=" << kTypeNames[i];
    EXPECT_EQ(parser_get_position(p), 1u) << "name=" << kTypeNames[i];
    /* location 精确覆盖类型名，不含前后 trivia */
    const location_t *loc = node_get_location(node);
    EXPECT_EQ(loc->end.offset - loc->begin.offset,
              (ptrdiff_t)strlen(kTypeNames[i]))
        << "name=" << kTypeNames[i];
    EXPECT_EQ(strncmp(loc->begin.offset, kTypeNames[i], strlen(kTypeNames[i])),
              0)
        << "name=" << kTypeNames[i];
    EXPECT_STREQ(loc->filename, "test.clux");
    delete_node(&node);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* 类型名是关键字，不是 identifier。 */
TEST(LiteralTypeTest, TypeNamesAreKeywords) {
  for (size_t i = 0; kTypeNames[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p = create_parser(a, "test.clux", kTypeNames[i]);
    token_t *tok = parser_peek_token(p);
    ASSERT_NE(tok, nullptr) << "name=" << kTypeNames[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD)
        << "name=" << kTypeNames[i];
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* opaque 此前遗漏在词法关键字表外，这里显式锁定。 */
TEST(LiteralTypeTest, OpaqueIsKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "opaque");
  token_t *tok = parser_peek_token(p);
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralTypeTest, RejectsOtherKeyword) {
  const char *sources[] = {"var", "func", "struct", "none", "true", nullptr};
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    EXPECT_EQ(parser_read_literal_type(a, p), nullptr) << "src=" << sources[i];
    EXPECT_EQ(parser_get_position(p), 0u) << "src=" << sources[i];
    delete_parser(&p);
    delete_allocator(&a);
  }
}

TEST(LiteralTypeTest, RejectsIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "User");
  EXPECT_EQ(parser_read_literal_type(a, p), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 类型名后跟标识符字符时词法层已切成一个 identifier，不是类型名。 */
TEST(LiteralTypeTest, RejectsTypeNamePrefix) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "i32x");
  EXPECT_EQ(parser_read_literal_type(a, p), nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralTypeTest, RejectsNumeric) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "123");
  EXPECT_EQ(parser_read_literal_type(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralTypeTest, RejectsString) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"i32\"");
  EXPECT_EQ(parser_read_literal_type(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralTypeTest, EmptyStreamReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  EXPECT_EQ(parser_read_literal_type(a, p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralTypeTest, AttachesParent) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "i32");
  node_t *parent = create_node(a, CLUX_NODE_PROGRAM);
  node_t *node = parser_read_literal_type(a, p);
  ASSERT_NE(node, nullptr);
  node_set_parent(node, parent);
  EXPECT_EQ(node_get_parent(node), parent);
  delete_node(&node);
  delete_node(&parent);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 聚合入口：类型名优先于 identifier（后者本来也匹配不到关键字）。 */
TEST(LiteralDispatchTest, DispatchesTypeName) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "i32");
  node_t *node = parser_read_literal(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_TYPE);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(LiteralDispatchTest, DispatchesOpaque) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "opaque");
  node_t *node = parser_read_literal(a, p);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_LITERAL_TYPE);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}
