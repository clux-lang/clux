#include <gtest/gtest.h>
#include "parser/lexer.h"
#include "core/allocator.h"
#include "core/position.h"
#include "core/location.h"
#include "lexer_test_util.h"
#include <string.h>

// 辅助：用 location_get 提取 token 文本

TEST(LexerCharacterTest, SimpleAsciiChar) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'a'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  // 位置推进到末尾
  EXPECT_EQ(p.offset, src + 3);
  // 文本是 "'a'"
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, "'a'");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, NewlineEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\n'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, "'\\n'");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, TabEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\t'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, CarriageReturnEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\r'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, NullEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\0'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, BackslashEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\\\'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, SingleQuoteEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\''";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, DoubleQuoteEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\\"'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, HexEscapeTwoDigits) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\x1b'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, "'\\x1b'");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, HexEscapeOneDigit) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\xa'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, HexEscapeUpperCase) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\xFF'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, MultiByteUTF8Char) {
  allocator_t *a = create_allocator(malloc, free);
  /* '世' = 3 bytes UTF-8 */
  const char *src = "'\xE4\xB8\x96'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  EXPECT_EQ(p.offset, src + 5); // ' + 3 bytes + '
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, NotACharacterReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "abc";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  // 位置不变
  EXPECT_EQ(p.offset, src);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, EmptyCharLiteralIsError) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "''";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_ERROR);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, UnclosedCharLiteralIsError) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'a";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_ERROR);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, UnclosedAtEOF) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_ERROR);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, InvalidEscapeIsError) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\z'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_ERROR);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, HexEscapeNoDigitsIsError) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\x'";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_ERROR);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, CharFollowedByMoreCode) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'x' + 1";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  EXPECT_EQ(p.offset, src + 3); // 停在 'x' 之后的空格
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, "'x'");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, LocationFieldsCorrect) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\n'";
  /* 注意：字符内容是换行符，但这不是转义，是字面量换行 */
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_CHARACTER);
  // begin 在 src[0] ('\''), end 越过闭合 '
  EXPECT_EQ(token_get_location(tok)->begin.offset, src);
  EXPECT_EQ(token_get_location(tok)->end.offset, src + 3);
  EXPECT_STREQ(token_get_location(tok)->filename, "test.clux");
  delete_token(&tok);
  delete_allocator(&a);
}

/* ============================ token 基础类 ============================ */

static location_t make_token_loc(const char *src, size_t b, size_t e) {
  location_t loc;
  loc.filename = "test.clux";
  loc.begin.offset = src + b;
  loc.begin.line = 1;
  loc.begin.column = (uint32_t)(b + 1);
  loc.end.offset = src + e;
  loc.end.line = 1;
  loc.end.column = (uint32_t)(e + 1);
  return loc;
}

TEST(TokenTest, CreateAndGetters) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "abc";
  location_t loc = make_token_loc(src, 0, 3);
  token_t *tok = create_token(a, CLUX_TOKEN_IDENTIFIER, loc);
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_IDENTIFIER);
  const location_t *got = token_get_location(tok);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->begin.offset, src);
  EXPECT_EQ(got->end.offset, src + 3);
  EXPECT_STREQ(got->filename, "test.clux");
  delete_token(&tok);
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(TokenTest, CreateAllKinds) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "x";
  location_t loc = make_token_loc(src, 0, 1);
  token_kind_t kinds[] = {
      CLUX_TOKEN_ERROR,
      CLUX_TOKEN_CHARACTER,
      CLUX_TOKEN_STRING,
      CLUX_TOKEN_NUMERIC,
      CLUX_TOKEN_IDENTIFIER,
      CLUX_TOKEN_SYMBOL,
      CLUX_TOKEN_KEYWORD,
      CLUX_TOKEN_WHITESPACE,
      CLUX_TOKEN_COMMENT,
      CLUX_TOKEN_MULTILINE_COMMENT,
  };
  for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    token_t *tok = create_token(a, kinds[i], loc);
    EXPECT_EQ(token_get_kind(tok), kinds[i]);
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(TokenTest, DeleteNullIsNoop) {
  token_t *tok = nullptr;
  delete_token(&tok);
  EXPECT_EQ(tok, nullptr);
  delete_token(nullptr);
}

TEST(TokenTest, NoLeakOnDelete) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "x";
  location_t loc = make_token_loc(src, 0, 1);
  token_t *tok = create_token(a, CLUX_TOKEN_SYMBOL, loc);
  delete_token(&tok);
  delete_allocator(&a);
  SUCCEED();
}
