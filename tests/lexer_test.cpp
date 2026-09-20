#include <gtest/gtest.h>
#include "test_common.h"
#include <string>

extern "C" {
#include "core/allocator.h"
#include "core/stream.h"
#include "core/vec.h"
#include "parser/location.h"
#include "parser/lexer.h"
}

/* ---- Test allocator helpers ---- */

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr) { free(ptr); }

/* ---- Helpers ---- */

static istream_t *make_istream(allocator_t *a, const char *text) {
  stream_source_t src = stream_source_mem(a, text, strlen(text), false);
  return istream_open(a, src);
}

static lexer_t *
make_lexer(allocator_t *a, const char *text, const char *filename) {
  return lexer_create(a, make_istream(a, text), filename);
}

static token_t *take(allocator_t *a,
                     lexer_t *lx,
                     token_kind_t expected,
                     const char *expected_text) {
  (void)a;
  token_t *t = lexer_next(lx);
  EXPECT_NE(t, nullptr);
  if (!t) return nullptr;
  EXPECT_EQ(token_get_kind(t), expected);
  size_t len = 0;
  const char *text = token_get_text(t, &len);
  if (expected_text) {
    EXPECT_EQ(len, strlen(expected_text));
    EXPECT_EQ(memcmp(text, expected_text, len), 0);
  }
  return t;
}

/* Drain every remaining token (up to and incl. EOF) and free it. Used so
 * the allocator-leak check below stays clean. */
static void drain(allocator_t *a, lexer_t *lx) {
  for (;;) {
    token_t *t = lexer_next(lx);
    token_kind_t k = token_get_kind(t);
    token_free(a, &t);
    if (k == TOKEN_TYPE_EOF) break;
  }
}

/* Pull tokens until the first TOKEN_TYPE_ERROR, returning that token
 * (caller frees it). Every token before it is freed. Returns NULL if no
 * error token appears before EOF. */
static token_t *first_error(allocator_t *a, lexer_t *lx) {
  (void)a;
  for (;;) {
    token_t *t = lexer_next(lx);
    token_kind_t k = token_get_kind(t);
    if (k == TOKEN_TYPE_ERROR) return t;
    token_free(a, &t);
    if (k == TOKEN_TYPE_EOF) return nullptr;
  }
}

/* ==== Lexer create / close ==== */

TEST(Lexer, CreateNullSafe) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  EXPECT_EQ(lexer_create(NULL, NULL, "x"), nullptr);
  EXPECT_EQ(lexer_create(a, NULL, "x"), nullptr);
  lexer_close(nullptr);
  lexer_t *null_lexer = nullptr;
  lexer_close(&null_lexer); /* no-op */
  EXPECT_EQ(null_lexer, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, CreateClosesStreamOnClose) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "abc", "t.cx");
  ASSERT_NE(lx, nullptr);
  lexer_close(&lx);
  EXPECT_EQ(lx, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a); /* no leak: lexer owns the istream */
}

/* ==== EOF ==== */

TEST(Lexer, EmptyInputEof) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "", "t.cx");
  token_t *t = lexer_next(lx);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_EOF);
  const location_t *loc = token_get_location(t);
  EXPECT_EQ(loc->begin.offset, loc->end.offset); /* begin == end */
  EXPECT_EQ(loc->begin.line, loc->end.line);
  EXPECT_EQ(loc->begin.column, loc->end.column);
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, EofIdempotent) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "x", "t.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "x");
  token_free(a, &t);
  for (int i = 0; i < 5; i++) {
    t = lexer_next(lx);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_EOF);
    token_free(a, &t);
  }
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Keywords ==== */

TEST(Lexer, AllKeywords) {
  const char *kws[] = {
      "as",  "bool",  "break",    "const",   "continue", "else", "enum",
      "f32", "f64", "false", "for",      "func",    "i16",      "i32",  "i64",
      "i8",  "if",    "nil",     "return",  "str",     "true",  "u16",
      "u32", "u64",   "u8",      "undefined", "var",    "void",  "volatile",
      "while",
  };
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
    lexer_t *lx = make_lexer(a, kws[i], "kw.cx");
    token_t *t = take(a, lx, TOKEN_TYPE_KEYWORD, kws[i]);
    token_free(a, &t);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, KeywordVsIdentifier) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "if iffy if9", "t.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_KEYWORD, "if");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_WHITESPACE, " ");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "iffy");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_WHITESPACE, " ");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "if9");
  token_free(a, &t);
  t = lexer_next(lx);
  EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_EOF);
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Identifiers ==== */

TEST(Lexer, Identifiers) {
  const char *idents[] = {"foo", "foo123", "_x", "x_y", "Foo", "a1_b2"};
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(idents) / sizeof(idents[0]); i++) {
    lexer_t *lx = make_lexer(a, idents[i], "id.cx");
    token_t *t = take(a, lx, TOKEN_TYPE_IDENTIFIER, idents[i]);
    token_free(a, &t);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, IdentifierStopsAtNonIdent) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  /* identifier stops at the '=' boundary; '=' is a symbol */
  lexer_t *lx = make_lexer(a, "abc=", "t.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "abc");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_SYMBOL, "=");
  token_free(a, &t);
  t = lexer_next(lx);
  EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_EOF);
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Unicode identifiers (spec 2.5: ID_Start / ID_Continue) ==== */

TEST(Lexer, UnicodeIdentifiers) {
  /* UTF-8 bytes written as escapes so the test does not depend on the
   * source file encoding: 变量 / café / _ü / a1_π / 中文 */
  const char *idents[] = {
      "\xe5\x8f\x98\xe9\x87\x8f",       /* 变量 */
      "caf\xc3\xa9",                    /* café */
      "_\xc3\xbc",                      /* _ü */
      "a1_\xcf\x80",                    /* a1_π */
      "\xe4\xb8\xad\xe6\x96\x87",       /* 中文 */
  };
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(idents) / sizeof(idents[0]); i++) {
    lexer_t *lx = make_lexer(a, idents[i], "id.cx");
    token_t *t = take(a, lx, TOKEN_TYPE_IDENTIFIER, idents[i]);
    token_free(a, &t);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, UnicodeIdentifierStopsAtSymbol) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "\xe5\x8f\x98\xe9\x87\x8f=1", "id.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "\xe5\x8f\x98\xe9\x87\x8f");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_SYMBOL, "=");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_NUMERIC, "1");
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, NonIdentifierUnicodeIsError) {
  struct {
    const char *src;
    const char *needle;
  } cases[] = {
      {"\xe2\x82\xac", "U+20AC"},     /* € (Sc) is not ID_Start */
      {"\xf0\x9f\x98\x80", "U+1F600"}, /* emoji (So) is not ID_Start */
      {"\xc2\xa9", "U+00A9"},          /* © (So) is not ID_Start */
  };
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    lexer_t *lx = make_lexer(a, cases[i].src, "id.cx");
    token_t *t = first_error(a, lx);
    ASSERT_NE(t, nullptr) << i;
    const char *msg = token_get_error_message(t);
    ASSERT_NE(msg, nullptr) << i;
    EXPECT_NE(strstr(msg, cases[i].needle), nullptr) << i;
    token_free(a, &t);
    drain(a, lx); /* the lexer simply resumes (here: straight to EOF) */
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Whitespace ==== */

TEST(Lexer, WhitespaceMergedIntoOneToken) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  const char *ws = " \t\n\r\n ";
  lexer_t *lx = make_lexer(a, ws, "ws.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_WHITESPACE, ws);
  token_free(a, &t);
  t = lexer_next(lx);
  EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_EOF);
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, WhitespaceLocation) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "ab\ncd", "ws.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "ab");
  token_free(a, &t);

  t = lexer_next(lx);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_WHITESPACE);
  const location_t *loc = token_get_location(t);
  EXPECT_EQ(loc->begin.offset, 2u);
  EXPECT_EQ(loc->begin.line, 1u);
  EXPECT_EQ(loc->begin.column, 3u);
  EXPECT_EQ(loc->end.offset, 3u);
  EXPECT_EQ(loc->end.line, 2u);
  EXPECT_EQ(loc->end.column, 1u);
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Locations ==== */

TEST(Lexer, LocationHalfOpenRange) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "ab\ncd", "loc.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "ab");
  const location_t *loc = token_get_location(t);
  EXPECT_EQ(loc->begin.offset, 0u);
  EXPECT_EQ(loc->begin.line, 1u);
  EXPECT_EQ(loc->begin.column, 1u);
  EXPECT_EQ(loc->end.offset, 2u); /* half-open [0, 2) */
  EXPECT_EQ(loc->end.line, 1u);
  EXPECT_EQ(loc->end.column, 3u);
  EXPECT_STREQ(loc->filename, "loc.cx");
  token_free(a, &t);

  t = lexer_next(lx); /* whitespace "\n" */
  token_free(a, &t);

  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "cd");
  loc = token_get_location(t);
  EXPECT_EQ(loc->begin.offset, 3u);
  EXPECT_EQ(loc->begin.line, 2u);
  EXPECT_EQ(loc->begin.column, 1u);
  EXPECT_EQ(loc->end.offset, 5u);
  EXPECT_EQ(loc->end.line, 2u);
  EXPECT_EQ(loc->end.column, 3u);
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Mixed stream ==== */

TEST(Lexer, MixedStream) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "func foo", "mix.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_KEYWORD, "func");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_WHITESPACE, " ");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "foo");
  token_free(a, &t);
  t = lexer_next(lx);
  EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_EOF);
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== token_is ==== */

TEST(Lexer, TokenIs) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "func", "t.cx");
  token_t *t = lexer_next(lx);
  ASSERT_NE(t, nullptr);
  EXPECT_TRUE(token_is(t, "func"));
  EXPECT_FALSE(token_is(t, "funcx"));
  EXPECT_FALSE(token_is(t, "fun"));
  EXPECT_FALSE(token_is(t, ""));
  EXPECT_FALSE(token_is(t, nullptr));
  EXPECT_FALSE(token_is(nullptr, "func"));
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Error token (lexing resumes; the message is on the token) ==== */

TEST(Lexer, UnrecognizedCharProducesErrorThenResumes) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "@x", "err.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_ERROR, nullptr);
  const char *msg = token_get_error_message(t);
  ASSERT_NE(msg, nullptr);
  EXPECT_NE(strstr(msg, "U+0040"), nullptr);
  token_free(a, &t);
  /* The lexer does NOT stop: it resumes after the bad character. */
  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "x");
  token_free(a, &t);
  t = lexer_next(lx);
  EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_EOF);
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, ErrorMessageAndLocation) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "ab\ncd @", "err.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "ab");
  token_free(a, &t);
  t = lexer_next(lx); /* "\n" */
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "cd");
  token_free(a, &t);
  t = lexer_next(lx); /* " " */
  token_free(a, &t);

  t = lexer_next(lx);
  EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_ERROR);
  location_t eloc = *token_get_location(t);
  const char *msg = token_get_error_message(t);
  ASSERT_NE(msg, nullptr);
  EXPECT_NE(strstr(msg, "U+0040"), nullptr);
  EXPECT_EQ(eloc.begin.line, 2u);
  EXPECT_EQ(eloc.begin.column, 4u); /* after "cd " on line 2 */
  token_free(a, &t);
  drain(a, lx);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Null safety ==== */

TEST(Lexer, NullSafety) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  EXPECT_EQ(lexer_next(nullptr), nullptr);
  EXPECT_EQ(token_get_kind(nullptr), TOKEN_TYPE_ERROR);
  EXPECT_EQ(token_get_location(nullptr), nullptr);

  lexer_t *lx = make_lexer(a, "abc", "t.cx");
  token_t *t = lexer_next(lx);
  ASSERT_NE(t, nullptr);

  size_t len = 123;
  EXPECT_EQ(token_get_text(nullptr, &len), nullptr);
  EXPECT_EQ(len, 0u);
  EXPECT_EQ(token_get_text(t, &len), token_get_text(t, &len));
  EXPECT_EQ(len, 3u); /* out_len may be NULL: still returns the slice */
  EXPECT_EQ(token_get_text(t, nullptr), token_get_text(t, &len));

  token_free(a, &t);
  token_free(a, nullptr); /* no-op */
  token_t *null_tok = nullptr;
  token_free(a, &null_tok); /* no-op */
  EXPECT_EQ(null_tok, nullptr);
  token_free(nullptr, &null_tok); /* no-op */
  EXPECT_EQ(create_token(nullptr, TOKEN_TYPE_EOF, location_t{}), nullptr);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== allocator_move on lexer ==== */

TEST(Lexer, MoveTransfersOwnership) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "func", "mv.cx");
  lexer_t *moved = (lexer_t *)allocator_move(a, (void **)&lx);
  ASSERT_NE(moved, nullptr);
  EXPECT_EQ(lx, nullptr); /* source nullified */

  token_t *t = take(a, moved, TOKEN_TYPE_KEYWORD, "func");
  token_free(a, &t);
  t = lexer_next(moved);
  EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_EOF);
  token_free(a, &t);
  lexer_close(&moved);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Numeric literals ==== */

TEST(Lexer, IntegerLiterals) {
  const char *cases[] = {
      "0", "42", "12345", "0xFF", "0Xff", "0o77", "0O17", "0b1010", "0B1"};
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    lexer_t *lx = make_lexer(a, cases[i], "num.cx");
    token_t *t = take(a, lx, TOKEN_TYPE_NUMERIC, cases[i]);
    token_free(a, &t);
    t = lexer_next(lx);
    EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_EOF);
    token_free(a, &t);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, IntegerLiteralsWithSuffix) {
  /* 后缀是独立 token：1i8 → NUMERIC "1" + IDENTIFIER "i8" */
  struct { const char *src; const char *num; const char *suf; } cases[] = {
      {"1i8", "1", "i8"}, {"100i32", "100", "i32"}, {"42u64", "42", "u64"},
      {"0xFFu8", "0xFF", "u8"}, {"0b1010u16", "0b1010", "u16"},
      {"7i16", "7", "i16"}, {"9u32", "9", "u32"}};
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    lexer_t *lx = make_lexer(a, cases[i].src, "num.cx");
    token_t *t = take(a, lx, TOKEN_TYPE_NUMERIC, cases[i].num);
    token_free(a, &t);
    t = take(a, lx, TOKEN_TYPE_KEYWORD, cases[i].suf);
    token_free(a, &t);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, FloatLiterals) {
  /* 纯浮点数（无后缀） */
  const char *cases[] = {"3.14", "1.0e10", "1e10", "0.5", "1e-3", "2E+5", "0.0"};
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    lexer_t *lx = make_lexer(a, cases[i], "num.cx");
    token_t *t = take(a, lx, TOKEN_TYPE_NUMERIC, cases[i]);
    token_free(a, &t);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, FloatLiteralsWithSuffix) {
  /* 浮点数 + 后缀：3.14f32 → NUMERIC "3.14" + IDENTIFIER "f32" */
  struct { const char *src; const char *num; const char *suf; } cases[] = {
      {"3.14f32", "3.14", "f32"}, {"1.0e10f64", "1.0e10", "f64"}};
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    lexer_t *lx = make_lexer(a, cases[i].src, "num.cx");
    token_t *t = take(a, lx, TOKEN_TYPE_NUMERIC, cases[i].num);
    token_free(a, &t);
    t = take(a, lx, TOKEN_TYPE_KEYWORD, cases[i].suf);
    token_free(a, &t);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, NumericSuffixBindsToLiteral) {
  /* 后缀紧贴数字，无空格：42i8 → NUMERIC + IDENTIFIER */
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "42i8", "num.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_NUMERIC, "42");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_KEYWORD, "i8");
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, HexELetterIsDigitNotExponent) {
  /* In hex literals 'e' is a digit; 0x1e+5 splits into 0x1e and + and 5. */
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "0x1e+5", "num.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_NUMERIC, "0x1e");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_SYMBOL, "+");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_NUMERIC, "5");
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, NumericErrorsAreErrorTokens) {
  struct {
    const char *src;
    const char *needle;
  } cases[] = {
      {"0x", "no digits after prefix"},
      {"0o", "no digits after prefix"},
      {"0b2", "no digits after prefix"}, /* '2' is not binary */
      {"1.", "expected digit after decimal point"},
      {"1.a", "expected digit after decimal point"},
      {"1e", "expected digit in exponent"},
      {"1e+", "expected digit in exponent"},
      {"1.5e", "expected digit in exponent"},
  };
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    lexer_t *lx = make_lexer(a, cases[i].src, "num.cx");
    token_t *t = first_error(a, lx);
    ASSERT_NE(t, nullptr) << cases[i].src;
    const char *msg = token_get_error_message(t);
    ASSERT_NE(msg, nullptr) << cases[i].src;
    EXPECT_NE(strstr(msg, cases[i].needle), nullptr) << cases[i].src;
    token_free(a, &t);
    drain(a, lx);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Character literals ==== */

TEST(Lexer, CharLiterals) {
  const char *cases[] = {"'a'",    "'\\n'",   "'\\''", "'\\\\'", "'\\x41'",
                         "' '",    "'\\0'",   "'\\t'", "'\\r'",  "'\\\"'",
                         "'\\x7'", "'\\xFF'", "'\\x3'"};
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    lexer_t *lx = make_lexer(a, cases[i], "chr.cx");
    token_t *t = take(a, lx, TOKEN_TYPE_CHARACTER, cases[i]);
    token_free(a, &t);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, CharErrorsAreErrorTokens) {
  struct {
    const char *src;
    const char *needle;
  } cases[] = {
      {"'a", "unterminated character literal"},
      {"'\\", "unterminated character literal"},
      {"'\n'", "unterminated character literal"},
      {"''", "empty character literal"},
      {"'ab'", "exactly one character"},
      {"'\\01'", "exactly one character"},
      {"'\\q'", "invalid escape sequence"},
      {"'\\u0041'", "invalid escape sequence"},
      {"'\\x'", "invalid hex escape"},
      {"'\\xZ'", "invalid hex escape"},
      {"'\xe4\xb8\xad'", "out of range for u8"},
  };
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    lexer_t *lx = make_lexer(a, cases[i].src, "chr.cx");
    token_t *t = first_error(a, lx);
    ASSERT_NE(t, nullptr) << cases[i].src;
    const char *msg = token_get_error_message(t);
    ASSERT_NE(msg, nullptr) << cases[i].src;
    EXPECT_NE(strstr(msg, cases[i].needle), nullptr) << cases[i].src;
    token_free(a, &t);
    drain(a, lx);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== String literals ==== */

TEST(Lexer, StringLiteralsRawText) {
  /* The token text keeps the quotes and escapes verbatim; decoding is
   * the Parser's job. */
  const char *cases[] = {
      "\"hello\"", "\"\"", "\"a\\nb\"", "\"say \\\"hi\\\"\"", "\"\\x41\\0\""};
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    lexer_t *lx = make_lexer(a, cases[i], "str.cx");
    token_t *t = take(a, lx, TOKEN_TYPE_STRING, cases[i]);
    token_free(a, &t);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, StringErrorsAreErrorTokens) {
  struct {
    const char *src;
    const char *needle;
  } cases[] = {
      {"\"abc", "unterminated string literal"},
      {"\"ab\\", "unterminated string literal"},
      {"\"ab\ncd\"", "unterminated string literal"},
      {"\"\\q\"", "invalid escape sequence"},
      {"\"\\x\"", "invalid hex escape"},
      {"\"\\xz\"", "invalid hex escape"},
      {"\"a\\u0041\"", "invalid escape sequence"},
  };
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    lexer_t *lx = make_lexer(a, cases[i].src, "str.cx");
    token_t *t = first_error(a, lx);
    ASSERT_NE(t, nullptr) << cases[i].src;
    const char *msg = token_get_error_message(t);
    ASSERT_NE(msg, nullptr) << cases[i].src;
    EXPECT_NE(strstr(msg, cases[i].needle), nullptr) << cases[i].src;
    token_free(a, &t);
    drain(a, lx);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Comments ==== */

TEST(Lexer, LineComment) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "// hello\nx", "cmt.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_COMMENT, "// hello");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_WHITESPACE, "\n");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "x");
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, LineCommentToEof) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "// no newline", "cmt.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_COMMENT, "// no newline");
  token_free(a, &t);
  t = lexer_next(lx);
  EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_EOF);
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, BlockComment) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "/* a */x", "cmt.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_MULTILINE_COMMENT, "/* a */");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "x");
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, BlockCommentNotNested) {
  // 块注释不嵌套（2026-09-20 定稿，与 C 一致）：内层第一个 */ 即结束，
  // 剩余部分按普通 token 继续词法分析。
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "/* a /* b */c*/x", "cmt.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_MULTILINE_COMMENT, "/* a /* b */");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "c");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_SYMBOL, "*");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_SYMBOL, "/");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "x");
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, UnterminatedBlockCommentIsError) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "/* abc", "cmt.cx");
  token_t *t = first_error(a, lx);
  ASSERT_NE(t, nullptr);
  const char *msg = token_get_error_message(t);
  ASSERT_NE(msg, nullptr);
  EXPECT_NE(strstr(msg, "unterminated block comment"), nullptr);
  token_free(a, &t);
  drain(a, lx);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Symbols ==== */

TEST(Lexer, SingleSymbols) {
  const char *syms[] = {"+", "-", "*", "%", "<", ">", "=", "!", "&", "|",  "^",
                        "~", "(", ")", "{", "}", "[", "]", ";", ",", ":", "."};
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
    lexer_t *lx = make_lexer(a, syms[i], "sym.cx");
    token_t *t = take(a, lx, TOKEN_TYPE_SYMBOL, syms[i]);
    token_free(a, &t);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, TwoCharSymbolsMaximalMunch) {
  const char *syms[] = {"<<",
                        ">>",
                        "<=",
                        ">=",
                        "==",
                        "!=",
                        "&&",
                        "||",
                        "->",
                        "+=",
                        "-=",
                        "*=",
                        "/=",
                        "%="};
  allocator_t *a = create_allocator(test_alloc, test_free);
  for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
    lexer_t *lx = make_lexer(a, syms[i], "sym.cx");
    token_t *t = take(a, lx, TOKEN_TYPE_SYMBOL, syms[i]);
    token_free(a, &t);
    lexer_close(&lx);
  }
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, MaximalMunchSplitsLongerRuns) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "<<<", "sym.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_SYMBOL, "<<");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_SYMBOL, "<");
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, SymbolMixedWithTokens) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "a<b", "sym.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "a");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_SYMBOL, "<");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "b");
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

TEST(Lexer, DotIsASymbol) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "a.b", "sym.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "a");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_SYMBOL, ".");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "b");
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== CRLF line endings ==== */

TEST(Lexer, LineCommentStopsAtCarriageReturn) {
  /* In a CRLF source the comment must not swallow the '\r'. */
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "// hi\r\nx", "cmt.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_COMMENT, "// hi");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_WHITESPACE, "\r\n");
  token_free(a, &t);
  t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "x");
  token_free(a, &t);
  lexer_close(&lx);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== UTF-8 BOM ==== */

TEST(Lexer, Utf8BomIsSkipped) {
  allocator_t *a = create_allocator(test_alloc, test_free);
  lexer_t *lx = make_lexer(a, "\xEF\xBB\xBF" "abc", "bom.cx");
  token_t *t = take(a, lx, TOKEN_TYPE_IDENTIFIER, "abc");
  const location_t *loc = token_get_location(t);
  EXPECT_EQ(loc->begin.offset, 3u); /* the BOM belongs to no token */
  EXPECT_EQ(loc->begin.line, 1u);
  token_free(a, &t);
  t = lexer_next(lx);
  EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_EOF);
  token_free(a, &t);
  lexer_close(&lx);

  lexer_t *only = make_lexer(a, "\xEF\xBB\xBF", "bom.cx"); /* BOM-only file */
  t = lexer_next(only);
  EXPECT_EQ(token_get_kind(t), TOKEN_TYPE_EOF);
  token_free(a, &t);
  lexer_close(&only);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* ==== Token pool (the parser-side abstraction over lexer_next) ==== */

/**
 * The lexer only exposes lexer_next; the pipeline assembles a plain
 * vec<token*> on top of it. The Parser then walks that vec by index and
 * may move forwards or backwards freely — no re-lexing, no lookahead
 * buffer inside the lexer. This test shows the whole shape.
 */
TEST(Lexer, TokenPoolSupportsBacktracking) {
  allocator_t *a = create_allocator(test_alloc, test_free);

  /* Build the pool exactly as the pipeline would. */
  lexer_t *lx = make_lexer(a, "func foo", "pool.cx");
  vec_t *pool = vec_new(a, true); /* owns_element: frees the tokens */
  for (;;) {
    token_t *t = lexer_next(lx);
    token_kind_t k = token_get_kind(t);
    vec_push(pool, a, t);
    if (k == TOKEN_TYPE_EOF) break;
  }
  lexer_close(&lx); /* tokens live on in the pool */

  /* pool = [func, " ", foo, EOF] */
  EXPECT_EQ(vec_len(pool), 4u);
  token_t *k0 = (token_t *)vec_get(pool, 0);
  EXPECT_EQ(token_get_kind(k0), TOKEN_TYPE_KEYWORD);
  EXPECT_TRUE(token_is(k0, "func"));

  /* Forward walk. */
  token_t *f = (token_t *)vec_get(pool, 2);
  EXPECT_TRUE(token_is(f, "foo"));

  /* Backward walk — the Parser can revisit an earlier token at will. */
  token_t *b = (token_t *)vec_get(pool, 0);
  EXPECT_TRUE(token_is(b, "func"));

  /* EOF sentinel is the last element. */
  token_t *eof = (token_t *)vec_get(pool, vec_len(pool) - 1);
  EXPECT_EQ(token_get_kind(eof), TOKEN_TYPE_EOF);

  /* A single vec_free releases every token (no manual token_free). */
  vec_free(a, &pool);
  EXPECT_EQ(pool, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}
