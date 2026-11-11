#include <gtest/gtest.h>
#include "parser/lexer.h"
#include "core/allocator.h"
#include "core/position.h"
#include "core/location.h"
#include "lexer_test_util.h"

// ============================ string ============================

TEST(LexerStringTest, SimpleString) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\"hello\"";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_STRING);
  EXPECT_EQ(p.offset, src + 7);
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, "\"hello\"");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerStringTest, EmptyString) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\"\"";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_STRING);
  EXPECT_EQ(p.offset, src + 2);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerStringTest, StringWithNewlineEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\"a\\nb\"";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_STRING);
  EXPECT_EQ(p.offset, src + 6);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerStringTest, StringWithHexEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\"\\x41\"";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_STRING);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerStringTest, StringWithEscapedQuote) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\"say \\\"hi\\\"\"";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_STRING);
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, "\"say \\\"hi\\\"\"");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerStringTest, StringWithBackslashEscape) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\"a\\\\b\"";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_STRING);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerStringTest, StringWithUTF8) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\"\xE4\xB8\x96\xE7\x95\x8C\""; /* "世界" */
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_STRING);
  EXPECT_EQ(p.offset, src + 8); /* " + 6 bytes + " */
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerStringTest, UnclosedStringIsError) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\"hello";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_ERROR);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerStringTest, UnclosedStringWithEscapeIsError) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\"hello\\";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_ERROR);
  delete_token(&tok);
  delete_allocator(&a);
}

/* 无效转义属于内容错误：lexer 只产出 STRING，由 parser 报错 */
TEST(LexerStringTest, InvalidEscapeInStringIsString) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\"\\z\"";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_STRING);
  EXPECT_EQ(p.offset, src + 4);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerStringTest, NotAStringReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "abc";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(LexerStringTest, StringFollowedByCode) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\"x\" + 1";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_STRING);
  EXPECT_EQ(p.offset, src + 3);
  delete_token(&tok);
  delete_allocator(&a);
}

// ============================ identifier ============================

TEST(LexerIdentifierTest, SimpleIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "foo";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_IDENTIFIER);
  EXPECT_EQ(p.offset, src + 3);
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, "foo");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerIdentifierTest, IdentifierWithDigits) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "foo123";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_IDENTIFIER);
  EXPECT_EQ(p.offset, src + 6);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerIdentifierTest, IdentifierWithUnderscore) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "_my_var";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_IDENTIFIER);
  EXPECT_EQ(p.offset, src + 7);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerIdentifierTest, SingleUnderscore) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "_";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_IDENTIFIER);
  EXPECT_EQ(p.offset, src + 1);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerIdentifierTest, UnicodeIdentifier) {
  allocator_t *a = create_allocator(malloc, free);
  /* "变量" = U+53D8 U+91CF */
  const char *src = "\xE5\x8F\x98\xE9\x87\x8F";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_IDENTIFIER);
  EXPECT_EQ(p.offset, src + 6);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerIdentifierTest, IdentifierStopsAtNonIDChar) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "foo+bar";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_IDENTIFIER);
  EXPECT_EQ(p.offset, src + 3); /* stops at '+' */
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerIdentifierTest, DigitStartReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "123abc";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(LexerIdentifierTest, SymbolStartReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "+foo";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

// ============================ whitespace ============================

TEST(LexerWhitespaceTest, SingleSpace) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = " x";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_whitespace(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_WHITESPACE);
  EXPECT_EQ(p.offset, src + 1);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerWhitespaceTest, MultipleSpaces) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "   x";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_whitespace(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_WHITESPACE);
  EXPECT_EQ(p.offset, src + 3);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerWhitespaceTest, TabWhitespace) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\tx";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_whitespace(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_WHITESPACE);
  EXPECT_EQ(p.offset, src + 1);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerWhitespaceTest, NewlineWhitespace) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\nx";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_whitespace(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_WHITESPACE);
  EXPECT_EQ(p.offset, src + 1);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerWhitespaceTest, MixedWhitespace) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = " \t\n x";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_whitespace(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_WHITESPACE);
  EXPECT_EQ(p.offset, src + 4);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerWhitespaceTest, NonWhitespaceReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "abc";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_whitespace(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(LexerWhitespaceTest, UnicodeWhitespace) {
  allocator_t *a = create_allocator(malloc, free);
  /* U+00A0 (non-break space) is Unicode whitespace but u_isWhitespace
     excludes it; U+2003 EM SPACE is included */
  const char *src = "\xE2\x80\x83x"; /* EM SPACE + x */
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_whitespace(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_WHITESPACE);
  EXPECT_EQ(p.offset, src + 3);
  delete_token(&tok);
  delete_allocator(&a);
}

// ============================ comment (single-line) ============================

TEST(LexerCommentTest, SimpleComment) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "// hello";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_comment(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_COMMENT);
  EXPECT_EQ(p.offset, src + 8);
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, "// hello");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCommentTest, CommentStopsAtNewline) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "// hi\nx";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_comment(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_COMMENT);
  EXPECT_EQ(p.offset, src + 5); /* 不含 \n */
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCommentTest, CommentAtEOF) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "// eof";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_comment(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_COMMENT);
  EXPECT_EQ(p.offset, src + 6);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCommentTest, EmptyComment) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "//";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_comment(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_COMMENT);
  EXPECT_EQ(p.offset, src + 2);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCommentTest, NotACommentReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "/ not a comment";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_comment(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

// ============================ multiline comment ============================

TEST(LexerMultilineCommentTest, SimpleMultiline) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "/* hello */";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_multiline_comment(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_MULTILINE_COMMENT);
  EXPECT_EQ(p.offset, src + 11);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerMultilineCommentTest, MultilineSpansLines) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "/* line1\nline2 */";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_multiline_comment(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_MULTILINE_COMMENT);
  EXPECT_EQ(p.offset, src + 17);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerMultilineCommentTest, EmptyMultiline) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "/**/";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_multiline_comment(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_MULTILINE_COMMENT);
  EXPECT_EQ(p.offset, src + 4);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerMultilineCommentTest, MultilineWithStars) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "/* a * b * c */";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_multiline_comment(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_MULTILINE_COMMENT);
  EXPECT_EQ(p.offset, src + 15);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerMultilineCommentTest, UnclosedMultilineIsError) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "/* unclosed";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_multiline_comment(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_ERROR);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerMultilineCommentTest, UnclosedMultilineAtEOFAfterStar) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "/* unclosed*";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_multiline_comment(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_ERROR);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerMultilineCommentTest, NotMultilineReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "/ not";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_multiline_comment(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

// ============================ numeric ============================

TEST(LexerNumericTest, DecimalInteger) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "42";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 2);
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, "42");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, Zero) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "0";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 1);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, HexUppercase) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "0xDEAD";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 6);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, HexLowercase) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "0xdead";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 6);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, HexMixedCase) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "0xDeAdBeEf";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 10);
  delete_token(&tok);
  delete_allocator(&a);
}

/* 0x 后无数字属于内容错误：lexer 只产出 NUMERIC，由 parser 报错 */
TEST(LexerNumericTest, HexNoDigitsIsNumeric) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "0x ";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 2);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, OctalPrefixed) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "0o755";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 5);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, OctalPrefixedUppercase) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "0O17";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 4);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, BinaryPrefixed) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "0b1010";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 6);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, BinaryPrefixedUppercase) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "0B101";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 5);
  delete_token(&tok);
  delete_allocator(&a);
}

/* 0o / 0b 后无数字同样只产出 NUMERIC，内容错误留给 parser。 */
TEST(LexerNumericTest, BinaryAndOctalNoDigitsAreNumeric) {
  allocator_t *a = create_allocator(malloc, free);
  {
    const char *src = "0b ";
    position_t p = lexer_test_pos(src);
    token_t *tok = lexer_read_numeric(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr);
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
    EXPECT_EQ(p.offset, src + 2);
    delete_token(&tok);
  }
  {
    const char *src = "0o ";
    position_t p = lexer_test_pos(src);
    token_t *tok = lexer_read_numeric(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr);
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
    EXPECT_EQ(p.offset, src + 2);
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerNumericTest, OctalNumber) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "0777";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 4);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, FloatSimple) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "3.14";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 4);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, FloatWithExponent) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "1.5e10";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 6);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, FloatWithSignedExponent) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "2.5e-3";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 6);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, FloatUppercaseExponent) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "1E5";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 3);
  delete_token(&tok);
  delete_allocator(&a);
}

/* clux 禁止 `.123` 简写：点号开头不是数字字面量。 */
TEST(LexerNumericTest, FloatStartsWithDotIsRejected) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = ".5";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  /* 游标不动，'.' 留给后续 token */
  EXPECT_EQ(p.offset, src);
  delete_allocator(&a);
}

TEST(LexerNumericTest, IntegerWithExponent) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "42e5";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 4);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, ExponentNoDigitsRollsBack) {
  allocator_t *a = create_allocator(malloc, free);
  /* "123e+abc" — e 后有 + 但无数字，回退不消费 e+ */
  const char *src = "123e+x";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 3); /* 只消费 "123" */
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericTest, DotAloneReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = ".foo";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(LexerNumericTest, NonNumericReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "abc";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(LexerNumericTest, NumericFollowedByCode) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "42 + x";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 2);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerStringTest, UnclosedAfterBackslashAtEOF) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "\"abc\\";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_string(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_ERROR);
  delete_token(&tok);
  delete_allocator(&a);
}

/* ---- 覆盖率补强：EOF 边界与短路分支 ---- */

TEST(LexerIdentifierCoverageTest, EmptyInputReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(LexerWhitespaceCoverageTest, EmptyInputReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_whitespace(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(LexerWhitespaceCoverageTest, WhitespaceAtEOFStops) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "   ";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_whitespace(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_WHITESPACE);
  EXPECT_EQ(p.offset, src + 3);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerCommentCoverageTest, SingleSlashIsNotComment) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "/";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_comment(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(LexerMultilineCommentCoverageTest, SingleSlashIsNotComment) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "/";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_multiline_comment(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(LexerNumericCoverageTest, ZeroNotFollowedByOctalOrHex) {
  allocator_t *a = create_allocator(malloc, free);
  /* "0" 单独，后面是 '8'（不是八进制/十六进制） */
  const char *src = "08";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  /* 0 走十进制分支，消费 "08" */
  EXPECT_EQ(p.offset, src + 2);
  delete_token(&tok);
  delete_allocator(&a);
}

/* clux 禁止 `123.` 简写：点号后无数字时不算小数部分，只消费整数部分。 */
TEST(LexerNumericCoverageTest, FloatDotNoFractionStopsAtDot) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "1.";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  /* 只消费 "1"，'.' 留给后续 token */
  EXPECT_EQ(p.offset, src + 1);
  delete_token(&tok);
  delete_allocator(&a);
}

/* 点号后紧跟数字才属于小数部分，因此 `42.!` 切分为 `42` + `.!`，
   后缀解包运算符得以紧跟数字字面量。 */
TEST(LexerNumericCoverageTest, NumericStopsBeforePostfixOperator) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "42.!";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 2);
  delete_token(&tok);
  /* 紧随其后的是 .! 后缀运算符 */
  token_t *postfix = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(postfix, nullptr);
  EXPECT_EQ(p.offset, src + 4);
  delete_token(&postfix);
  delete_allocator(&a);
}

TEST(LexerNumericCoverageTest, HexZeroDigitsAtEOF) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "0x";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  /* 内容错误留给 parser，lexer 仍产出 NUMERIC */
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_NUMERIC);
  EXPECT_EQ(p.offset, src + 2);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerNumericCoverageTest, DotAtEOFReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = ".";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_numeric(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}
