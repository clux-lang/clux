#include <gtest/gtest.h>
#include "parser/lexer.h"
#include "core/allocator.h"
#include "core/position.h"
#include "core/location.h"
#include "lexer_test_util.h"
#include <string.h>

// ============================ keyword ============================

TEST(LexerKeywordTest, VarIsKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "var";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD);
  EXPECT_EQ(p.offset, src + 3);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerKeywordTest, FuncIsKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "func";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerKeywordTest, IfElseAreKeywords) {
  allocator_t *a = create_allocator(malloc, free);
  {
    const char *src = "if";
    position_t p = lexer_test_pos(src);
    token_t *tok = lexer_read_identifier(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr);
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD);
    delete_token(&tok);
  }
  {
    const char *src = "else";
    position_t p = lexer_test_pos(src);
    token_t *tok = lexer_read_identifier(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr);
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD);
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerKeywordTest, TypeKeywords) {
  allocator_t *a = create_allocator(malloc, free);
  const char *kws[] = {"bool",
                       "i8",
                       "i16",
                       "i32",
                       "i64",
                       "u8",
                       "u16",
                       "u32",
                       "u64",
                       "isize",
                       "usize",
                       "f32",
                       "f64",
                       "str",
                       "void",
                       "opaque",
                       nullptr};
  for (int i = 0; kws[i] != nullptr; i++) {
    position_t p = lexer_test_pos(kws[i]);
    token_t *tok = lexer_read_identifier(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "kw=" << kws[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD) << "kw=" << kws[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerKeywordTest, StructUnionEnumAreKeywords) {
  allocator_t *a = create_allocator(malloc, free);
  const char *kws[] = {
      "struct", "union", "cunion", "enum", "implements", nullptr};
  for (int i = 0; kws[i] != nullptr; i++) {
    position_t p = lexer_test_pos(kws[i]);
    token_t *tok = lexer_read_identifier(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "kw=" << kws[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD) << "kw=" << kws[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerKeywordTest, OfIsKeyword) {
  allocator_t *a = create_allocator(malloc, free);
  position_t p = lexer_test_pos("of");
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerKeywordTest, ControlFlowKeywords) {
  allocator_t *a = create_allocator(malloc, free);
  const char *kws[] = {"for",
                       "foreach",
                       "while",
                       "do",
                       "break",
                       "continue",
                       "return",
                       "defer",
                       nullptr};
  for (int i = 0; kws[i] != nullptr; i++) {
    position_t p = lexer_test_pos(kws[i]);
    token_t *tok = lexer_read_identifier(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "kw=" << kws[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD) << "kw=" << kws[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerKeywordTest, ImportExportUsingAreKeywords) {
  allocator_t *a = create_allocator(malloc, free);
  const char *kws[] = {"import", "export", "using", nullptr};
  for (int i = 0; kws[i] != nullptr; i++) {
    position_t p = lexer_test_pos(kws[i]);
    token_t *tok = lexer_read_identifier(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "kw=" << kws[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD) << "kw=" << kws[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerKeywordTest, ConstVolatileAreKeywords) {
  allocator_t *a = create_allocator(malloc, free);
  const char *kws[] = {"const", "volatile", nullptr};
  for (int i = 0; kws[i] != nullptr; i++) {
    position_t p = lexer_test_pos(kws[i]);
    token_t *tok = lexer_read_identifier(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "kw=" << kws[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD) << "kw=" << kws[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerKeywordTest, UndefinedNoneAreKeywords) {
  allocator_t *a = create_allocator(malloc, free);
  const char *kws[] = {"undefined", "none", nullptr};
  for (int i = 0; kws[i] != nullptr; i++) {
    position_t p = lexer_test_pos(kws[i]);
    token_t *tok = lexer_read_identifier(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "kw=" << kws[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD) << "kw=" << kws[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerKeywordTest, TypeAndComptimeAreKeywords) {
  allocator_t *a = create_allocator(malloc, free);
  const char *kws[] = {"type", "comptime", nullptr};
  for (int i = 0; kws[i] != nullptr; i++) {
    position_t p = lexer_test_pos(kws[i]);
    token_t *tok = lexer_read_identifier(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "kw=" << kws[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD) << "kw=" << kws[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerKeywordTest, KeywordPrefixIsIdentifier) {
  /* "variable" 以 "var" 开头但不是 keyword */
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "variable";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_IDENTIFIER);
  EXPECT_EQ(p.offset, src + 8);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerKeywordTest, KeywordFollowedByCode) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "var x";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_identifier(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_KEYWORD);
  EXPECT_EQ(p.offset, src + 3); /* stops at space */
  delete_token(&tok);
  delete_allocator(&a);
}

// ============================ symbol ============================

TEST(LexerSymbolTest, Plus) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "+";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL);
  EXPECT_EQ(p.offset, src + 1);
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, "+");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, Minus) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "-";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, Arrow) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "->";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL);
  EXPECT_EQ(p.offset, src + 2);
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, "->");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, Ellipsis) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "...";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL);
  EXPECT_EQ(p.offset, src + 3);
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, "...");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, Equality) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "==";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL);
  EXPECT_EQ(p.offset, src + 2);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, NotEqual) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "!=";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL);
  EXPECT_EQ(p.offset, src + 2);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, CompoundAssignments) {
  allocator_t *a = create_allocator(malloc, free);
  const char *syms[] = {"+=",
                        "-=",
                        "*=",
                        "/=",
                        "%=",
                        "&=",
                        "|=",
                        "^=",
                        "~=",
                        ">>=",
                        "<<=",
                        "||=",
                        "&&=",
                        nullptr};
  for (int i = 0; syms[i] != nullptr; i++) {
    position_t p = lexer_test_pos(syms[i]);
    token_t *tok = lexer_read_symbol(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "sym=" << syms[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL) << "sym=" << syms[i];
    EXPECT_EQ(p.offset - syms[i], (ptrdiff_t)strlen(syms[i]))
        << "sym=" << syms[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

/* `<<` / `>>` 在词法层有歧义（与嵌套元组尖括号冲突），故意不合并：
   词法只产出单个 `<` / `>`，由 parser 在相邻位置组合。 */
TEST(LexerSymbolTest, ShiftOperatorsAreNotMerged) {
  allocator_t *a = create_allocator(malloc, free);
  const char *syms[] = {"<<", ">>", nullptr};
  for (int i = 0; syms[i] != nullptr; i++) {
    position_t p = lexer_test_pos(syms[i]);
    token_t *tok = lexer_read_symbol(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "sym=" << syms[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL) << "sym=" << syms[i];
    /* 只消费第一个字符 */
    EXPECT_EQ(p.offset - syms[i], (ptrdiff_t)1) << "sym=" << syms[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerSymbolTest, LogicalAnd) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "&&";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL);
  EXPECT_EQ(p.offset - src, (ptrdiff_t)2);
  delete_token(&tok);
  delete_allocator(&a);
}

/* `||` 在词法层有歧义（与空捕获列表 `||` 冲突），故意不合并：
   词法只产出单个 '|'，由 parser 在相邻位置组合。 */
TEST(LexerSymbolTest, LogicalOrIsNotMerged) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "||";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL);
  EXPECT_EQ(p.offset - src, (ptrdiff_t)1);
  delete_token(&tok);
  delete_allocator(&a);
}

/* `.?` 在词法层有歧义（与 optional 构造冲突），故意不合并：
   词法只产出 '.'，由 parser 组合。 */
TEST(LexerSymbolTest, TryOperatorIsNotMerged) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = ".?";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL);
  EXPECT_EQ(p.offset, src + 1);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, DotOperators) {
  allocator_t *a = create_allocator(malloc, free);
  const char *syms[] = {".!", ".", nullptr};
  for (int i = 0; syms[i] != nullptr; i++) {
    position_t p = lexer_test_pos(syms[i]);
    token_t *tok = lexer_read_symbol(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "sym=" << syms[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL) << "sym=" << syms[i];
    EXPECT_EQ(p.offset - syms[i], (ptrdiff_t)strlen(syms[i]))
        << "sym=" << syms[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerSymbolTest, Brackets) {
  allocator_t *a = create_allocator(malloc, free);
  const char *syms[] = {"(", ")", "[", "]", "{", "}", nullptr};
  for (int i = 0; syms[i] != nullptr; i++) {
    position_t p = lexer_test_pos(syms[i]);
    token_t *tok = lexer_read_symbol(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "sym=" << syms[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL) << "sym=" << syms[i];
    EXPECT_EQ(p.offset - syms[i], (ptrdiff_t)strlen(syms[i]))
        << "sym=" << syms[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

/* `[[` / `]]` 在词法层有歧义（与嵌套下标冲突），故意不合并：
   词法只产出单个 '[' / ']'，由 parser 在相邻位置组合。 */
TEST(LexerSymbolTest, DoubleBracketsAreNotMerged) {
  allocator_t *a = create_allocator(malloc, free);
  const char *syms[] = {"[[", "]]", nullptr};
  for (int i = 0; syms[i] != nullptr; i++) {
    position_t p = lexer_test_pos(syms[i]);
    token_t *tok = lexer_read_symbol(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "sym=" << syms[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL) << "sym=" << syms[i];
    EXPECT_EQ(p.offset - syms[i], (ptrdiff_t)1) << "sym=" << syms[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerSymbolTest, Punctuation) {
  allocator_t *a = create_allocator(malloc, free);
  const char *syms[] = {",", "?", ":", ";", "~", "^", "!", "=", nullptr};
  for (int i = 0; syms[i] != nullptr; i++) {
    position_t p = lexer_test_pos(syms[i]);
    token_t *tok = lexer_read_symbol(a, &p, "test.clux");
    ASSERT_NE(tok, nullptr) << "sym=" << syms[i];
    EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL) << "sym=" << syms[i];
    EXPECT_EQ(p.offset - syms[i], (ptrdiff_t)strlen(syms[i]))
        << "sym=" << syms[i];
    delete_token(&tok);
  }
  delete_allocator(&a);
}

TEST(LexerSymbolTest, LongestMatchPreferred) {
  /* ">>=" 应匹配为整体而非 ">>" + "=" */
  allocator_t *a = create_allocator(malloc, free);
  const char *src = ">>=";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL);
  EXPECT_EQ(p.offset, src + 3);
  char *text = lexer_test_text(tok, a);
  EXPECT_STREQ(text, ">>=");
  allocator_free(a, (void **)&text);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, EllipsisNotDot) {
  /* "..." 应匹配为整体而非 "." */
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "...";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL);
  EXPECT_EQ(p.offset, src + 3);
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, SymbolFollowedByCode) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "->x";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(token_get_kind(tok), CLUX_TOKEN_SYMBOL);
  EXPECT_EQ(p.offset, src + 2); /* stops at 'x' */
  delete_token(&tok);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, NonSymbolReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "abc";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, DigitReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "123";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, SpaceReturnsNull) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = " ";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  EXPECT_EQ(tok, nullptr);
  delete_allocator(&a);
}

TEST(LexerSymbolTest, ColumnUpdated) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "->x";
  position_t p = lexer_test_pos(src);
  token_t *tok = lexer_read_symbol(a, &p, "test.clux");
  ASSERT_NE(tok, nullptr);
  EXPECT_EQ(p.column, 3u); /* 1 + 2 chars */
  delete_token(&tok);
  delete_allocator(&a);
}
