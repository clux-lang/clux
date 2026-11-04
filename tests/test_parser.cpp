#include <gtest/gtest.h>
#include "parser/parser.h"
#include "parser/lexer.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/vec.h"
#include <stdlib.h>
#include <string.h>

/* ---- 生命周期 ---- */

TEST(ParserTest, CreateAndDelete) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "var x = 1;");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  EXPECT_STREQ(parser_get_filename(p), "test.clux");
  delete_parser(&p);
  EXPECT_EQ(p, nullptr);
  delete_allocator(&a);
}

TEST(ParserTest, DeleteNullIsNoop) {
  parser_t *p = nullptr;
  delete_parser(&p);
  EXPECT_EQ(p, nullptr);
  delete_parser(nullptr);
}

TEST(ParserTest, EmptySourceProducesNoTokens) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  EXPECT_EQ(vec_get_size(parser_get_tokens(p)), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 词法集成：各类 token 都能被切出 ---- */

TEST(ParserTest, LexesSimpleDeclaration) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "var x = 1;");
  vec_t *tokens = parser_get_tokens(p);
  /* var, ' ', x, ' ', =, ' ', 1, ; */
  EXPECT_EQ(vec_get_size(tokens), 8u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, LexesStringAndComment) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "\"hi\" // note");
  vec_t *tokens = parser_get_tokens(p);
  /* string, ' ', comment */
  EXPECT_EQ(vec_get_size(tokens), 3u);
  EXPECT_EQ(token_get_kind((token_t *)vec_get(tokens, 0)), CLUX_TOKEN_STRING);
  EXPECT_EQ(token_get_kind((token_t *)vec_get(tokens, 2)), CLUX_TOKEN_COMMENT);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, LexesMultilineComment) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "/* a\nb */x");
  vec_t *tokens = parser_get_tokens(p);
  EXPECT_EQ(vec_get_size(tokens), 2u);
  EXPECT_EQ(token_get_kind((token_t *)vec_get(tokens, 0)),
            CLUX_TOKEN_MULTILINE_COMMENT);
  EXPECT_EQ(token_get_kind((token_t *)vec_get(tokens, 1)),
            CLUX_TOKEN_IDENTIFIER);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, LexesKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "return");
  vec_t *tokens = parser_get_tokens(p);
  ASSERT_EQ(vec_get_size(tokens), 1u);
  EXPECT_EQ(token_get_kind((token_t *)vec_get(tokens, 0)), CLUX_TOKEN_KEYWORD);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 词法错误：TODO 补全的路径 ---- */

TEST(ParserTest, UnexpectedCharacterRecordsError) {
  allocator_t *a = create_allocator(malloc, free);
  /* '@' 不被任何 reader 接受 → 词法中断，记录错误 */
  parser_t *p = create_parser(a, "test.clux", "@");
  EXPECT_EQ(vec_get_size(parser_get_tokens(p)), 0u);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err), "unexpected character '@'");
  const location_t *loc = error_get_location(err);
  EXPECT_STREQ(loc->filename, "test.clux");
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, ErrorLocationPointsAtBadCharacter) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "var @";
  parser_t *p = create_parser(a, "test.clux", src);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  const location_t *loc = error_get_location(err);
  EXPECT_EQ(loc->begin.offset, src + 4);
  EXPECT_EQ(loc->end.offset, src + 4);
  /* 前面的 token 仍然保留 */
  EXPECT_GT(vec_get_size(parser_get_tokens(p)), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, UnterminatedStringRecordsError) {
  allocator_t *a = create_allocator(malloc, free);
  /* 未闭合字符串 → lexer 产出 ERROR → parser 转成 error 并停止扫描 */
  parser_t *p = create_parser(a, "test.clux", "\"unclosed");
  EXPECT_EQ(vec_get_size(parser_get_tokens(p)), 0u);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err),
               "unterminated string or character literal");
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, UnterminatedCharacterRecordsError) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "'a");
  EXPECT_EQ(vec_get_size(parser_get_tokens(p)), 0u);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  clux_error_t *err = (clux_error_t *)vec_get(errors, 0);
  EXPECT_STREQ(error_get_message(err),
               "unterminated string or character literal");
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, InvalidUtf8RecordsError) {
  allocator_t *a = create_allocator(malloc, free);
  /* 孤立续字节 0x80 是非法 UTF-8，不属于任何 reader 的起始字符 */
  parser_t *p = create_parser(a, "test.clux", "\x80");
  EXPECT_EQ(vec_get_size(parser_get_tokens(p)), 0u);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 1u);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- token 游标 ---- */

TEST(ParserTest, NextAdvancesPosition) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a b");
  /* "a", " ", "b" → 3 tokens */
  EXPECT_EQ(parser_get_position(p), 0u);
  parser_next(p);
  EXPECT_EQ(parser_get_position(p), 1u);
  parser_next(p);
  EXPECT_EQ(parser_get_position(p), 2u);
  parser_next(p);
  EXPECT_EQ(parser_get_position(p), 3u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, NextAtEndIsNoop) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a");
  parser_next(p);
  EXPECT_EQ(parser_get_position(p), 1u);
  /* 已在末尾，再前进不越界 */
  parser_next(p);
  EXPECT_EQ(parser_get_position(p), 1u);
  parser_next(p);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, NextOnEmptyTokenStreamIsNoop) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  parser_next(p);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, PeekTokenDoesNotAdvance) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "ab");
  token_t *peeked = parser_peek_token(p);
  ASSERT_NE(peeked, nullptr);
  EXPECT_EQ(parser_get_position(p), 0u);
  /* peek 两次结果一致 */
  EXPECT_EQ(parser_peek_token(p), peeked);
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, PeekAtEndReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  EXPECT_EQ(parser_peek_token(p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, SetPositionWithinRange) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a b");
  EXPECT_TRUE(parser_set_position(p, 1));
  EXPECT_EQ(parser_get_position(p), 1u);
  EXPECT_TRUE(parser_set_position(p, 3));
  EXPECT_EQ(parser_get_position(p), 3u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, SetPositionOutOfRangeFails) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "ab");
  EXPECT_FALSE(parser_set_position(p, 4));
  EXPECT_EQ(parser_get_position(p), 0u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, SetPositionBackwardsAllowsReread) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "a b");
  token_t *first = parser_peek_token(p);
  parser_next(p);
  parser_next(p);
  EXPECT_TRUE(parser_set_position(p, 0));
  EXPECT_EQ(parser_peek_token(p), first);
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 错误列表 ---- */

TEST(ParserTest, PushErrorAppends) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  location_t loc = {
      .filename = "test.clux",
      .begin = {.offset = "", .line = 1, .column = 1},
      .end = {.offset = "", .line = 1, .column = 1},
  };
  clux_error_t *e1 = create_error(a, &loc, "first");
  clux_error_t *e2 = create_error(a, &loc, "second");
  parser_push_error(p, e1);
  parser_push_error(p, e2);
  vec_t *errors = parser_get_error(p);
  ASSERT_EQ(vec_get_size(errors), 2u);
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 0)), "first");
  EXPECT_STREQ(error_get_message((clux_error_t *)vec_get(errors, 1)), "second");
  delete_parser(&p);
  delete_allocator(&a);
}

/* ---- 所有权：delete_parser 释放所有 token 与 error ---- */

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

TEST(ParserTest, NoLeakOnDelete) {
  g_alloc_count = 0;
  g_free_count = 0;
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  parser_t *p = create_parser(a, "test.clux", "var x = 1; // c\n\"s\"");
  delete_parser(&p);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}

TEST(ParserTest, NoLeakWithLexerError) {
  g_alloc_count = 0;
  g_free_count = 0;
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  parser_t *p = create_parser(a, "test.clux", "var @");
  delete_parser(&p);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}

/* ---- parser_skip_trivia ---- */

TEST(ParserTest, SkipTriviaSkipsWhitespace) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "   x");
  parser_skip_trivia(p);
  token_t *tok = parser_peek_token(p);
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_IDENTIFIER);
  EXPECT_TRUE(token_is(tok, CLUX_TOKEN_IDENTIFIER, "x"));
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, SkipTriviaSkipsSingleLineComment) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "// note\nx");
  parser_skip_trivia(p);
  token_t *tok = parser_peek_token(p);
  ASSERT_NE(tok, nullptr);
  EXPECT_TRUE(token_is(tok, CLUX_TOKEN_IDENTIFIER, "x"));
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, SkipTriviaSkipsMultilineComment) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "/* a\nb */x");
  parser_skip_trivia(p);
  token_t *tok = parser_peek_token(p);
  ASSERT_NE(tok, nullptr);
  EXPECT_TRUE(token_is(tok, CLUX_TOKEN_IDENTIFIER, "x"));
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, SkipTriviaSkipsMixedTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", " \t// c\n /* m */  \nx");
  parser_skip_trivia(p);
  token_t *tok = parser_peek_token(p);
  ASSERT_NE(tok, nullptr);
  EXPECT_TRUE(token_is(tok, CLUX_TOKEN_IDENTIFIER, "x"));
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, SkipTriviaStopsAtNonTrivia) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  x  y");
  parser_skip_trivia(p);
  EXPECT_EQ(parser_get_position(p), 1u); /* 只跳过第一个空白 */
  parser_next(p);
  parser_skip_trivia(p);
  token_t *tok = parser_peek_token(p);
  ASSERT_NE(tok, nullptr);
  EXPECT_TRUE(token_is(tok, CLUX_TOKEN_IDENTIFIER, "y"));
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, SkipTriviaOnOnlyTriviaReachesEnd) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "  // c\n /* m */ ");
  parser_skip_trivia(p);
  EXPECT_EQ(parser_peek_token(p), nullptr);
  EXPECT_EQ(parser_get_position(p), vec_get_size(parser_get_tokens(p)));
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, SkipTriviaOnEmptyStreamIsNoop) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "");
  parser_skip_trivia(p);
  EXPECT_EQ(parser_get_position(p), 0u);
  EXPECT_EQ(parser_peek_token(p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, SkipTriviaAtEndIsNoop) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "x");
  parser_next(p);
  EXPECT_EQ(parser_get_position(p), 1u);
  parser_skip_trivia(p);
  EXPECT_EQ(parser_get_position(p), 1u);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ParserTest, SkipTriviaDoesNotSkipTokens) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p = create_parser(a, "test.clux", "x");
  parser_skip_trivia(p);
  /* 有效 token 不被跳过 */
  EXPECT_EQ(parser_get_position(p), 0u);
  EXPECT_NE(parser_peek_token(p), nullptr);
  delete_parser(&p);
  delete_allocator(&a);
}
