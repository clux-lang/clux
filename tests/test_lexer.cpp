#include <gtest/gtest.h>
#include "parser/lexer.h"
#include "core/allocator.h"
#include "core/position.h"
#include "core/location.h"
#include <string.h>

static position_t make_pos(const char *src) {
  position_t p;
  p.filename = "test.clux";
  p.offset = src;
  p.line = 1;
  p.column = 1;
  return p;
}

// 辅助：用 location_get 提取 token 文本
static char *token_text(token_t *tok, allocator_t *a) {
  return location_get(&tok->loc, a);
}

TEST(LexerCharacterTest, SimpleAsciiChar) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'a'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  // 位置推进到末尾
  EXPECT_EQ(p.offset, src + 3);
  // 文本是 "'a'"
  char *text = token_text(tok, a);
  EXPECT_STREQ(text, "'a'");
  allocator_free(a, (void **)&text);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, NewlineEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\n'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  char *text = token_text(tok, a);
  EXPECT_STREQ(text, "'\\n'");
  allocator_free(a, (void **)&text);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, TabEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\t'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, CarriageReturnEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\r'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, NullEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\0'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, BackslashEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\\\'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, SingleQuoteEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\''";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, DoubleQuoteEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\\"'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, HexEscapeTwoDigits) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\x1b'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  char *text = token_text(tok, a);
  EXPECT_STREQ(text, "'\\x1b'");
  allocator_free(a, (void **)&text);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, HexEscapeOneDigit) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\xa'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, HexEscapeUpperCase) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\xFF'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, MultiByteUTF8Char) {
  allocator_t *a = create_allocator(malloc, free);
  /* '世' = 3 bytes UTF-8 */
  const char *src = "'\xE4\xB8\x96'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  EXPECT_EQ(p.offset, src + 5); // ' + 3 bytes + '
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, NotACharacterReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "abc";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  // 位置不变
  EXPECT_EQ(p.offset, src);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, EmptyCharLiteralIsError) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "''";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_ERROR);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, UnclosedCharLiteralIsError) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'a";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_ERROR);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, UnclosedAtEOF) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_ERROR);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, InvalidEscapeIsError) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\z'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_ERROR);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, HexEscapeNoDigitsIsError) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\\x'";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_ERROR);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, CharFollowedByMoreCode) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'x' + 1";
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  EXPECT_EQ(p.offset, src + 3); // 停在 'x' 之后的空格
  char *text = token_text(tok, a);
  EXPECT_STREQ(text, "'x'");
  allocator_free(a, (void **)&text);
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}

TEST(LexerCharacterTest, LocationFieldsCorrect) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "'\n'";
  /* 注意：字符内容是换行符，但这不是转义，是字面量换行 */
  position_t p = make_pos(src);
  token_t *tok = lexer_read_character(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(tok->kind, CLUX_TOKEN_CHARACTER);
  // begin 在 src[0] ('\''), end 越过闭合 '
  EXPECT_EQ(tok->loc.begin.offset, src);
  EXPECT_EQ(tok->loc.end.offset, src + 3);
  EXPECT_STREQ(tok->loc.filename, "test.clux");
  allocator_free(a, (void **)&tok);
  delete_allocator(&a);
}
