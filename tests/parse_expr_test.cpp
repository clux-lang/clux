/*
 * Description: parse_expr literal/primary/unary unit tests
 * Create: 2026-09-07
 */

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "core/allocator.h"
#include "core/arena.h"
#include "core/vec.h"
#include "core/stream.h"
#include "core/panic.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_node.h"
#include "parser/ast_kind.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_ident.h"
#include "parser/ast_unary.h"
#include "parser/ast_binary.h"
#include "parser/ast_array.h"
#include "parser/ast_construct.h"
#include "parser/ast_error.h"
#include "parser/ast_func_type.h"
}

#include "test_common.h"

namespace {

/* ---- Helper: build a token pool from source text ---- */

struct LexResult {
    vec_t       *tokens;
    lexer_t     *lexer;
    char        *source_buf;  /* heap-allocated copy, safe after move */
    size_t       source_len;
};

/**
 * Tokenize a source string into a token pool.
 * The source text is heap-allocated so its pointer remains stable
 * across LexResult moves (avoiding SSO issues with std::string).
 */
static LexResult lex_source(allocator_t *alloc, const char *src) {
    LexResult result;
    result.source_len = strlen(src);
    result.source_buf = (char *)malloc(result.source_len + 1);
    memcpy(result.source_buf, src, result.source_len + 1);

    stream_source_t mem_src = stream_source_mem(
        alloc, result.source_buf, result.source_len, false);
    istream_t *stream = istream_open(alloc, mem_src);

    lexer_t *lexer = lexer_create(alloc, stream, "test.clx");

    vec_t *pool = vec_new(alloc, true);  /* owns tokens */
    for (;;) {
        token_t *t = lexer_next(lexer);
        token_kind_t k = token_get_kind(t);
        vec_push(pool, alloc, t);
        if (k == TOKEN_TYPE_EOF || k == TOKEN_TYPE_ERROR) break;
    }

    result.tokens = pool;
    result.lexer = lexer;
    return result;
}

static void lex_result_destroy(allocator_t *alloc, LexResult &lr) {
    /* lexer_close closes the underlying istream too */
    lexer_close(&lr.lexer);
    vec_free(alloc, &lr.tokens);
    free(lr.source_buf);
    lr.source_buf = nullptr;
}

/* ---- Fixture ---- */

class ParseExprTest : public ::testing::Test {
protected:
    void SetUp() override {
        alloc_ = create_allocator(malloc, free);
        arena_ = arena_new_default(alloc_);
    }

    void TearDown() override {
        arena_destroy(alloc_, &arena_);
        EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc_);
    }

    /**
     * Convenience: create a parser from source text.
     * The LexResult is stored internally and cleaned up in TearDown
     * unless the caller takes ownership.
     */
    parser_t *make_parser(const char *src) {
        last_lex_ = lex_source(alloc_, src);
        return parser_create(alloc_, arena_, last_lex_.tokens);
    }

    void cleanup_parser(parser_t *p) {
        parser_destroy(&p);
        lex_result_destroy(alloc_, last_lex_);
    }

    allocator_t *alloc_ = nullptr;
    arena_t     *arena_ = nullptr;
    LexResult    last_lex_;
};

/* ================================================================ */
/* numeric_is_float                                                 */
/* ================================================================ */

/**
 * Scenario: Decimal number with dot is float
 * Expected: returns true
 */
TEST(NumericIsFloat, DecimalWithDot) {
    EXPECT_TRUE(numeric_is_float("3.14", 4));
}

/**
 * Scenario: Scientific notation with 'e' is float
 * Expected: returns true
 */
TEST(NumericIsFloat, ScientificNotationLowerE) {
    EXPECT_TRUE(numeric_is_float("1e10", 4));
}

/**
 * Scenario: Scientific notation with 'E' is float
 * Expected: returns true
 */
TEST(NumericIsFloat, ScientificNotationUpperE) {
    EXPECT_TRUE(numeric_is_float("1E5", 3));
}

/**
 * Scenario: Hex prefix (0x) is never float
 * Expected: returns false
 */
TEST(NumericIsFloat, HexPrefix) {
    EXPECT_FALSE(numeric_is_float("0xFF", 4));
}

/**
 * Scenario: Octal prefix (0o) is never float
 * Expected: returns false
 */
TEST(NumericIsFloat, OctalPrefix) {
    EXPECT_FALSE(numeric_is_float("0o77", 4));
}

/**
 * Scenario: Binary prefix (0b) is never float
 * Expected: returns false
 */
TEST(NumericIsFloat, BinaryPrefix) {
    EXPECT_FALSE(numeric_is_float("0b1010", 6));
}

/**
 * Scenario: Plain integer without dot or exponent
 * Expected: returns false
 */
TEST(NumericIsFloat, PlainInteger) {
    EXPECT_FALSE(numeric_is_float("42", 2));
}

/**
 * Scenario: Non-digit letters in text are not float indicators
 * Expected: returns false (no . or e/E found)
 */
TEST(NumericIsFloat, LetterINotFloat) {
    EXPECT_FALSE(numeric_is_float("3i", 2));
}

/**
 * Scenario: Non-digit letters in text are not float indicators
 * Expected: returns false
 */
TEST(NumericIsFloat, LetterUNotFloat) {
    EXPECT_FALSE(numeric_is_float("42u", 3));
}

/**
 * Scenario: Non-digit letters in text are not float indicators
 * Expected: returns false
 */
TEST(NumericIsFloat, LetterFNotFloat) {
    EXPECT_FALSE(numeric_is_float("1f", 2));
}

/**
 * Scenario: NULL text pointer returns false
 * Expected: returns false
 */
TEST(NumericIsFloat, NullTextReturnsFalse) {
    EXPECT_FALSE(numeric_is_float(NULL, 4));
}

/**
 * Scenario: Zero length returns false
 * Expected: returns false
 */
TEST(NumericIsFloat, ZeroLenReturnsFalse) {
    const char *text = "3.14";
    EXPECT_FALSE(numeric_is_float(text, 0));
}

/**
 * Scenario: Float with dot is float regardless of trailing characters
 * Expected: returns true
 */
TEST(NumericIsFloat, FloatWithTrailingChars) {
    /* dot found before any letters → still float */
    EXPECT_TRUE(numeric_is_float("3.14f", 5));
}

/**
 * Scenario: Hex with uppercase X prefix
 * Expected: returns false
 */
TEST(NumericIsFloat, HexPrefixUpperX) {
    EXPECT_FALSE(numeric_is_float("0XFF", 4));
}

/**
 * Scenario: Dot after non-digit letter is still found
 * Expected: returns true (dot found)
 */
TEST(NumericIsFloat, DotAfterLetterStillFloat) {
    /* "1i.2" — lexer won't produce this, but function still sees the dot */
    EXPECT_TRUE(numeric_is_float("1i.2", 4));
}

/**
 * Scenario: Leading zero without base prefix falls through to float check
 * Expected: returns false (plain decimal like "01")
 */
TEST(NumericIsFloat, LeadingZeroNoBasePrefix) {
    EXPECT_FALSE(numeric_is_float("01", 2));
}

/* ================================================================ */
/* parse_int_lit                                                    */
/* ================================================================ */

/**
 * Scenario: Parse decimal integer literal
 * Expected: returns AST_INT_LIT with parsed value 42, no suffix
 */
TEST_F(ParseExprTest, ParseIntLit_Decimal) {
    parser_t *p = make_parser("42");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_int_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_INT_LIT);

    auto *lit = (ast_int_lit_t *)node;
    EXPECT_EQ(lit->value, 42ULL);
    EXPECT_TRUE(strslice_is_empty(lit->type));

    cleanup_parser(p);
}

/**
 * Scenario: Parse hexadecimal integer literal
 * Expected: returns AST_INT_LIT with value 255 (0xFF), no suffix
 */
TEST_F(ParseExprTest, ParseIntLit_Hex) {
    parser_t *p = make_parser("0xFF");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_int_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_INT_LIT);

    auto *lit = (ast_int_lit_t *)node;
    EXPECT_EQ(lit->value, 0xFFULL);
    EXPECT_TRUE(strslice_is_empty(lit->type));

    cleanup_parser(p);
}

/**
 * Scenario: Parse octal integer literal
 * Expected: returns AST_INT_LIT with value 63 (0o77), no suffix
 */
TEST_F(ParseExprTest, ParseIntLit_Octal) {
    parser_t *p = make_parser("0o77");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_int_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_INT_LIT);

    auto *lit = (ast_int_lit_t *)node;
    EXPECT_EQ(lit->value, 63ULL);  /* 077 octal = 63 decimal */
    EXPECT_TRUE(strslice_is_empty(lit->type));

    cleanup_parser(p);
}

/**
 * Scenario: Parse binary integer literal
 * Expected: returns AST_INT_LIT with value 10 (0b1010), no suffix
 */
TEST_F(ParseExprTest, ParseIntLit_Binary) {
    parser_t *p = make_parser("0b1010");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_int_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_INT_LIT);

    auto *lit = (ast_int_lit_t *)node;
    EXPECT_EQ(lit->value, 10ULL);  /* 0b1010 = 10 decimal */
    EXPECT_TRUE(strslice_is_empty(lit->type));

    cleanup_parser(p);
}

/**
 * Scenario: Parse integer with type suffix
 * Expected: returns AST_INT_LIT with value 42 and type "u64"
 */
TEST_F(ParseExprTest, ParseIntLit_WithSuffix) {
    /* 数字和后缀之间无空格：lexer 产出 NUMERIC "42" + IDENTIFIER "u64" */
    parser_t *p = make_parser("42u64");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_int_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_INT_LIT);

    auto *lit = (ast_int_lit_t *)node;
    EXPECT_EQ(lit->value, 42ULL);
    EXPECT_EQ(lit->type.len, 3u);
    EXPECT_EQ(memcmp(lit->type.ptr, "u64", 3), 0);

    cleanup_parser(p);
}

/**
 * Scenario: Integer with invalid suffix (not a type suffix) is ignored
 * Expected: returns AST_INT_LIT with value 42, type empty
 */
TEST_F(ParseExprTest, ParseIntLit_InvalidSuffixIgnored) {
    parser_t *p = make_parser("42 hello");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_int_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_INT_LIT);

    auto *lit = (ast_int_lit_t *)node;
    EXPECT_EQ(lit->value, 42ULL);
    EXPECT_TRUE(strslice_is_empty(lit->type));

    cleanup_parser(p);
}

/**
 * Scenario: Float token does not match int lit
 * Expected: returns NULL and restores cursor
 */
TEST_F(ParseExprTest, ParseIntLit_FloatReturnsNull) {
    parser_t *p = make_parser("3.14");
    ASSERT_NE(p, nullptr);

    uint32_t pos_before = p->pos;
    ast_node_t *node = parse_int_lit(p);
    EXPECT_EQ(node, nullptr);
    EXPECT_EQ(p->pos, pos_before);

    cleanup_parser(p);
}

/**
 * Scenario: Non-numeric token does not match int lit
 * Expected: returns NULL and restores cursor
 */
TEST_F(ParseExprTest, ParseIntLit_NonNumericReturnsNull) {
    parser_t *p = make_parser("hello");
    ASSERT_NE(p, nullptr);

    /* skip whitespace to land on the identifier token */
    skip_trivia(p);

    uint32_t pos_before = p->pos;
    ast_node_t *node = parse_int_lit(p);
    EXPECT_EQ(node, nullptr);
    EXPECT_EQ(p->pos, pos_before);

    cleanup_parser(p);
}

/* ================================================================ */
/* parse_float_lit                                                  */
/* ================================================================ */

/**
 * Scenario: Parse decimal float literal
 * Expected: returns AST_FLOAT_LIT with value 3.14, no suffix
 */
TEST_F(ParseExprTest, ParseFloatLit_DecimalDot) {
    parser_t *p = make_parser("3.14");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_float_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_FLOAT_LIT);

    auto *lit = (ast_float_lit_t *)node;
    EXPECT_DOUBLE_EQ(lit->value, 3.14);
    EXPECT_TRUE(strslice_is_empty(lit->type));

    cleanup_parser(p);
}

/**
 * Scenario: Parse float in scientific notation
 * Expected: returns AST_FLOAT_LIT with value 1e10, no suffix
 */
TEST_F(ParseExprTest, ParseFloatLit_Scientific) {
    parser_t *p = make_parser("1e10");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_float_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_FLOAT_LIT);

    auto *lit = (ast_float_lit_t *)node;
    EXPECT_DOUBLE_EQ(lit->value, 1e10);
    EXPECT_TRUE(strslice_is_empty(lit->type));

    cleanup_parser(p);
}

/**
 * Scenario: Parse float with type suffix
 * Expected: returns AST_FLOAT_LIT with value 3.14 and type "f32"
 */
TEST_F(ParseExprTest, ParseFloatLit_WithSuffix) {
    /* 3.14f32 → NUMERIC "3.14" + KEYWORD "f32" */
    parser_t *p = make_parser("3.14f32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_float_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_FLOAT_LIT);

    auto *lit = (ast_float_lit_t *)node;
    EXPECT_DOUBLE_EQ(lit->value, 3.14);
    EXPECT_EQ(lit->type.len, 3u);
    EXPECT_EQ(memcmp(lit->type.ptr, "f32", 3), 0);

    cleanup_parser(p);
}

/**
 * Scenario: Integer-number with float suffix is a float literal
 * Expected: 1f32 → value 1.0, type "f32"
 */
TEST_F(ParseExprTest, ParseFloatLit_IntNumWithFloatSuffix) {
    parser_t *p = make_parser("1f32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_float_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_FLOAT_LIT);

    auto *lit = (ast_float_lit_t *)node;
    EXPECT_DOUBLE_EQ(lit->value, 1.0);
    EXPECT_EQ(lit->type.len, 3u);
    EXPECT_EQ(memcmp(lit->type.ptr, "f32", 3), 0);

    cleanup_parser(p);
}

/**
 * Scenario: Integer token does not match float lit
 * Expected: returns NULL and restores cursor
 */
TEST_F(ParseExprTest, ParseFloatLit_IntegerReturnsNull) {
    parser_t *p = make_parser("42");
    ASSERT_NE(p, nullptr);

    uint32_t pos_before = p->pos;
    ast_node_t *node = parse_float_lit(p);
    EXPECT_EQ(node, nullptr);
    EXPECT_EQ(p->pos, pos_before);

    cleanup_parser(p);
}

/**
 * Scenario: Non-numeric token does not match float lit
 * Expected: returns NULL and restores cursor
 */
TEST_F(ParseExprTest, ParseFloatLit_NonNumericReturnsNull) {
    parser_t *p = make_parser("hello");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    uint32_t pos_before = p->pos;
    ast_node_t *node = parse_float_lit(p);
    EXPECT_EQ(node, nullptr);
    EXPECT_EQ(p->pos, pos_before);

    cleanup_parser(p);
}

/* ================================================================ */
/* parse_bool_lit                                                   */
/* ================================================================ */

/**
 * Scenario: Parse "true" keyword as bool literal
 * Expected: returns AST_BOOL_LIT with value == true
 */
TEST_F(ParseExprTest, ParseBoolLit_True) {
    parser_t *p = make_parser("true");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_bool_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_BOOL_LIT);

    auto *lit = (ast_bool_lit_t *)node;
    EXPECT_TRUE(lit->value);

    cleanup_parser(p);
}

/**
 * Scenario: Parse "false" keyword as bool literal
 * Expected: returns AST_BOOL_LIT with value == false
 */
TEST_F(ParseExprTest, ParseBoolLit_False) {
    parser_t *p = make_parser("false");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_bool_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_BOOL_LIT);

    auto *lit = (ast_bool_lit_t *)node;
    EXPECT_FALSE(lit->value);

    cleanup_parser(p);
}

/**
 * Scenario: Identifier token is not a bool keyword
 * Expected: returns NULL and restores cursor
 */
TEST_F(ParseExprTest, ParseBoolLit_NonKeywordReturnsNull) {
    parser_t *p = make_parser("hello");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    uint32_t pos_before = p->pos;
    ast_node_t *node = parse_bool_lit(p);
    EXPECT_EQ(node, nullptr);
    EXPECT_EQ(p->pos, pos_before);

    cleanup_parser(p);
}

/**
 * Scenario: Integer token is not a bool keyword
 * Expected: returns NULL and restores cursor
 */
TEST_F(ParseExprTest, ParseBoolLit_NumericReturnsNull) {
    parser_t *p = make_parser("42");
    ASSERT_NE(p, nullptr);

    uint32_t pos_before = p->pos;
    ast_node_t *node = parse_bool_lit(p);
    EXPECT_EQ(node, nullptr);
    EXPECT_EQ(p->pos, pos_before);

    cleanup_parser(p);
}

/* ================================================================ */
/* parse_string_lit                                                 */
/* ================================================================ */

/**
 * Scenario: Parse string literal token
 * Expected: returns AST_STRING_LIT with resolved text (no quotes, no escapes)
 */
TEST_F(ParseExprTest, ParseStringLit_Basic) {
    parser_t *p = make_parser("\"hello\"");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_string_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_STRING_LIT);

    auto *lit = (ast_string_lit_t *)node;
    /* text is escape-resolved, quotes stripped */
    EXPECT_EQ(lit->text.len, 5u);
    EXPECT_EQ(memcmp(lit->text.ptr, "hello", 5), 0);

    cleanup_parser(p);
}

/**
 * Scenario: Non-string token does not match string lit
 * Expected: returns NULL and restores cursor
 */
TEST_F(ParseExprTest, ParseStringLit_NonStringReturnsNull) {
    parser_t *p = make_parser("42");
    ASSERT_NE(p, nullptr);

    uint32_t pos_before = p->pos;
    ast_node_t *node = parse_string_lit(p);
    EXPECT_EQ(node, nullptr);
    EXPECT_EQ(p->pos, pos_before);

    cleanup_parser(p);
}

/* ================================================================ */
/* parse_char_lit                                                   */
/* ================================================================ */

/**
 * Scenario: Parse character literal token
 * Expected: returns AST_CHAR_LIT with value 'a' (97)
 */
TEST_F(ParseExprTest, ParseCharLit_Basic) {
    parser_t *p = make_parser("'a'");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_char_lit(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_CHAR_LIT);

    auto *lit = (ast_char_lit_t *)node;
    EXPECT_EQ(lit->value, (uint32_t)'a');

    cleanup_parser(p);
}

/**
 * Scenario: Non-character token does not match char lit
 * Expected: returns NULL and restores cursor
 */
TEST_F(ParseExprTest, ParseCharLit_NonCharReturnsNull) {
    parser_t *p = make_parser("42");
    ASSERT_NE(p, nullptr);

    uint32_t pos_before = p->pos;
    ast_node_t *node = parse_char_lit(p);
    EXPECT_EQ(node, nullptr);
    EXPECT_EQ(p->pos, pos_before);

    cleanup_parser(p);
}

/* ================================================================ */
/* parse_ident                                                      */
/* ================================================================ */

/**
 * Scenario: Parse identifier token
 * Expected: returns AST_IDENT with correct name
 */
TEST_F(ParseExprTest, ParseIdent_Basic) {
    parser_t *p = make_parser("foo");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_ident(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_IDENT);

    auto *ident = (ast_ident_t *)node;
    EXPECT_EQ(ident->name.len, 3u);
    EXPECT_EQ(memcmp(ident->name.ptr, "foo", 3), 0);

    cleanup_parser(p);
}

/**
 * Scenario: Non-identifier token does not match ident
 * Expected: returns NULL and restores cursor
 */
TEST_F(ParseExprTest, ParseIdent_NonIdentReturnsNull) {
    parser_t *p = make_parser("42");
    ASSERT_NE(p, nullptr);

    uint32_t pos_before = p->pos;
    ast_node_t *node = parse_ident(p);
    EXPECT_EQ(node, nullptr);
    EXPECT_EQ(p->pos, pos_before);

    cleanup_parser(p);
}

/* ================================================================ */
/* parse_primary                                                    */
/* ================================================================ */

/**
 * Scenario: Grouped expression with parentheses
 * Expected: returns the inner expression (int lit 42)
 */
TEST_F(ParseExprTest, ParsePrimary_GroupedExpr) {
    parser_t *p = make_parser("(42)");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_primary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_INT_LIT);

    auto *lit = (ast_int_lit_t *)node;
    EXPECT_EQ(lit->value, 42ULL);

    cleanup_parser(p);
}

/**
 * Scenario: Grouped expression missing closing paren
 * Expected: returns AST_ERROR
 */
TEST_F(ParseExprTest, ParsePrimary_GroupedMissingCloseParen) {
    parser_t *p = make_parser("(42");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_primary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: Empty parentheses yield AST_ERROR (inner expr is NULL)
 * Expected: returns AST_ERROR
 */
TEST_F(ParseExprTest, ParsePrimary_EmptyParensReturnsError) {
    parser_t *p = make_parser("()");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_primary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: No matching primary expression (bare symbol)
 * Expected: returns NULL
 */
TEST_F(ParseExprTest, ParsePrimary_NoMatchReturnsNull) {
    /* A bare '+' symbol does not start any primary */
    parser_t *p = make_parser("+");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_primary(p);
    EXPECT_EQ(node, nullptr);

    cleanup_parser(p);
}

/**
 * Scenario: String literal matches in primary
 * Expected: returns AST_STRING_LIT
 */
TEST_F(ParseExprTest, ParsePrimary_StringLit) {
    parser_t *p = make_parser("\"hello\"");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_primary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_STRING_LIT);

    auto *lit = (ast_string_lit_t *)node;
    EXPECT_EQ(lit->text.len, 5u);
    EXPECT_EQ(memcmp(lit->text.ptr, "hello", 5), 0);

    cleanup_parser(p);
}

/**
 * Scenario: Bool literal true matches in primary (before ident check)
 * Expected: returns AST_BOOL_LIT, not AST_IDENT
 */
TEST_F(ParseExprTest, ParsePrimary_BoolTrueBeforeIdent) {
    parser_t *p = make_parser("true");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_primary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_BOOL_LIT);

    auto *lit = (ast_bool_lit_t *)node;
    EXPECT_TRUE(lit->value);

    cleanup_parser(p);
}

/* ================================================================ */
/* parse_unary                                                      */
/* ================================================================ */

/**
 * 辅助：断言 op token 文本与 expected 匹配（用于 binary / unary 的 op）
 */
static void expect_op_text(const token_t *op, const char *expected) {
    ASSERT_NE(op, nullptr);
    strslice_t s = token_strslice(op);
    EXPECT_EQ(s.len, strlen(expected));
    EXPECT_EQ(memcmp(s.ptr, expected, s.len), 0);
}

/**
 * Scenario: Logical NOT prefix operator
 * Expected: returns AST_UNARY with op='!' and identifier operand
 */
TEST_F(ParseExprTest, ParseUnary_Bang) {
    parser_t *p = make_parser("!x");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_unary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_UNARY);

    auto *unary = (ast_unary_t *)node;
    expect_op_text(unary->op, "!");
    ASSERT_NE(unary->operand, nullptr);
    EXPECT_EQ(unary->operand->kind, AST_IDENT);

    cleanup_parser(p);
}

/**
 * Scenario: Bitwise NOT prefix operator
 * Expected: returns AST_UNARY with op='~'
 */
TEST_F(ParseExprTest, ParseUnary_Tilde) {
    parser_t *p = make_parser("~x");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_unary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_UNARY);

    auto *unary = (ast_unary_t *)node;
    expect_op_text(unary->op, "~");
    ASSERT_NE(unary->operand, nullptr);
    EXPECT_EQ(unary->operand->kind, AST_IDENT);

    cleanup_parser(p);
}

/**
 * Scenario: Negation prefix operator
 * Expected: returns AST_UNARY with op='-'
 */
TEST_F(ParseExprTest, ParseUnary_Minus) {
    parser_t *p = make_parser("-x");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_unary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_UNARY);

    auto *unary = (ast_unary_t *)node;
    expect_op_text(unary->op, "-");
    ASSERT_NE(unary->operand, nullptr);
    EXPECT_EQ(unary->operand->kind, AST_IDENT);

    cleanup_parser(p);
}

/**
 * Scenario: Double logical NOT (recursive unary)
 * Expected: returns nested AST_UNARY, outer op='!' inner op='!'
 */
TEST_F(ParseExprTest, ParseUnary_DoubleBang) {
    parser_t *p = make_parser("!!x");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_unary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_UNARY);

    auto *outer = (ast_unary_t *)node;
    expect_op_text(outer->op, "!");

    ASSERT_NE(outer->operand, nullptr);
    EXPECT_EQ(outer->operand->kind, AST_UNARY);

    auto *inner = (ast_unary_t *)outer->operand;
    expect_op_text(inner->op, "!");
    ASSERT_NE(inner->operand, nullptr);
    EXPECT_EQ(inner->operand->kind, AST_IDENT);

    cleanup_parser(p);
}

/**
 * Scenario: No prefix operator falls through to parse_primary
 * Expected: returns AST_INT_LIT (primary matched)
 */
TEST_F(ParseExprTest, ParseUnary_NoPrefixFallsToPrimary) {
    parser_t *p = make_parser("42");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_unary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_INT_LIT);

    auto *lit = (ast_int_lit_t *)node;
    EXPECT_EQ(lit->value, 42ULL);

    cleanup_parser(p);
}

/**
 * Scenario: Prefix operator with no operand (at EOF) returns AST_ERROR
 * Expected: returns AST_ERROR node
 */
TEST_F(ParseExprTest, ParseUnary_MissingOperandReturnsError) {
    parser_t *p = make_parser("!");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_unary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: Double prefix where inner has no operand propagates AST_ERROR
 * Expected: returns AST_ERROR (not NULL) — error propagation path
 */
TEST_F(ParseExprTest, ParseUnary_DoubleBangMissingOperand) {
    parser_t *p = make_parser("!!");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_unary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: Negation of integer literal
 * Expected: returns AST_UNARY with op='-', operand is AST_INT_LIT
 */
TEST_F(ParseExprTest, ParseUnary_MinusIntLit) {
    parser_t *p = make_parser("-42");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_unary(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_UNARY);

    auto *unary = (ast_unary_t *)node;
    expect_op_text(unary->op, "-");
    ASSERT_NE(unary->operand, nullptr);
    EXPECT_EQ(unary->operand->kind, AST_INT_LIT);

    cleanup_parser(p);
}

/* ================================================================ */
/* 位运算 / 逻辑运算 / 移位 运算符测试                                */
/* ================================================================ */

/**
 * Scenario: a || b — 逻辑或（绑定力 1/2）
 * Expected: AST_BINARY with op="||", lhs=ident("a"), rhs=ident("b")
 */
TEST_F(ParseExprTest, Binary_LogicalOr) {
    parser_t *p = make_parser("a || b");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "||");
    ASSERT_NE(bin->lhs, nullptr);
    EXPECT_EQ(bin->lhs->kind, AST_IDENT);
    ASSERT_NE(bin->rhs, nullptr);
    EXPECT_EQ(bin->rhs->kind, AST_IDENT);

    cleanup_parser(p);
}

/**
 * Scenario: a && b — 逻辑与（绑定力 3/4）
 * Expected: AST_BINARY with op="&&"
 */
TEST_F(ParseExprTest, Binary_LogicalAnd) {
    parser_t *p = make_parser("a && b");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "&&");

    cleanup_parser(p);
}

/**
 * Scenario: a | b — 位或（绑定力 5/6）
 * Expected: AST_BINARY with op="|"
 */
TEST_F(ParseExprTest, Binary_BitwiseOr) {
    parser_t *p = make_parser("a | b");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "|");

    cleanup_parser(p);
}

/**
 * Scenario: a ^ b — 位异或（绑定力 7/8）
 * Expected: AST_BINARY with op="^"
 */
TEST_F(ParseExprTest, Binary_BitwiseXor) {
    parser_t *p = make_parser("a ^ b");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "^");

    cleanup_parser(p);
}

/**
 * Scenario: a & b — 位与（绑定力 9/10）
 * Expected: AST_BINARY with op="&"
 */
TEST_F(ParseExprTest, Binary_BitwiseAnd) {
    parser_t *p = make_parser("a & b");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "&");

    cleanup_parser(p);
}

/**
 * Scenario: a == b — 等于（绑定力 11/12）
 * Expected: AST_BINARY with op="=="
 */
TEST_F(ParseExprTest, Binary_Equal) {
    parser_t *p = make_parser("a == b");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "==");

    cleanup_parser(p);
}

/**
 * Scenario: a != b — 不等于（绑定力 11/12）
 * Expected: AST_BINARY with op="!="
 */
TEST_F(ParseExprTest, Binary_NotEqual) {
    parser_t *p = make_parser("a != b");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "!=");

    cleanup_parser(p);
}

/**
 * Scenario: a << b — 左移（绑定力 15/16）
 * Expected: AST_BINARY with op="<<"
 */
TEST_F(ParseExprTest, Binary_ShiftLeft) {
    parser_t *p = make_parser("a << b");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "<<");

    cleanup_parser(p);
}

/**
 * Scenario: a >> b — 右移（绑定力 15/16）
 * Expected: AST_BINARY with op=">>"
 */
TEST_F(ParseExprTest, Binary_ShiftRight) {
    parser_t *p = make_parser("a >> b");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, ">>");

    cleanup_parser(p);
}

/**
 * Scenario: 绑定力验证 — a || b && c 应解析为 a || (b && c)
 * 因为 || 绑定力(1/2) < && 绑定力(3/4)
 * Expected: 顶层是 ||，右侧是 &&
 */
TEST_F(ParseExprTest, Binary_Precedence_OrAnd) {
    parser_t *p = make_parser("a || b && c");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "||");
    ASSERT_NE(bin->lhs, nullptr);
    EXPECT_EQ(bin->lhs->kind, AST_IDENT);  /* a */
    ASSERT_NE(bin->rhs, nullptr);
    EXPECT_EQ(bin->rhs->kind, AST_BINARY); /* (b && c) */

    auto *rhs = (ast_binary_t *)bin->rhs;
    expect_op_text(rhs->op, "&&");

    cleanup_parser(p);
}

/**
 * Scenario: 绑定力验证 — a | b ^ c 应解析为 a | (b ^ c)
 * 因为 | 绑定力(5/6) < ^ 绑定力(7/8)
 * Expected: 顶层是 |，右侧是 ^
 */
TEST_F(ParseExprTest, Binary_Precedence_BitOrXor) {
    parser_t *p = make_parser("a | b ^ c");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "|");
    ASSERT_NE(bin->rhs, nullptr);
    EXPECT_EQ(bin->rhs->kind, AST_BINARY);

    auto *rhs = (ast_binary_t *)bin->rhs;
    expect_op_text(rhs->op, "^");

    cleanup_parser(p);
}

/**
 * Scenario: 绑定力验证 — a & b << c 应解析为 a & (b << c)
 * 因为 & 绑定力(9/10) < << 绑定力(15/16)
 * Expected: 顶层是 &，右侧是 <<
 */
TEST_F(ParseExprTest, Binary_Precedence_BitAndShift) {
    parser_t *p = make_parser("a & b << c");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "&");
    ASSERT_NE(bin->rhs, nullptr);
    EXPECT_EQ(bin->rhs->kind, AST_BINARY);

    auto *rhs = (ast_binary_t *)bin->rhs;
    expect_op_text(rhs->op, "<<");

    cleanup_parser(p);
}

/**
 * Scenario: 绑定力验证 — a + b * c 应解析为 a + (b * c)
 * 因为 + 绑定力(17/18) < * 绑定力(19/20)
 * Expected: 顶层是 +，右侧是 *
 */
TEST_F(ParseExprTest, Binary_Precedence_AddMul) {
    parser_t *p = make_parser("a + b * c");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "+");
    ASSERT_NE(bin->rhs, nullptr);
    EXPECT_EQ(bin->rhs->kind, AST_BINARY);

    auto *rhs = (ast_binary_t *)bin->rhs;
    expect_op_text(rhs->op, "*");

    cleanup_parser(p);
}

/**
 * Scenario: 左结合验证 — a - b - c 应解析为 (a - b) - c
 * Expected: 顶层是 -，lhs 是 (a - b)
 */
TEST_F(ParseExprTest, Binary_LeftAssociativity_Sub) {
    parser_t *p = make_parser("a - b - c");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "-");
    ASSERT_NE(bin->lhs, nullptr);
    EXPECT_EQ(bin->lhs->kind, AST_BINARY); /* (a - b) */

    auto *lhs = (ast_binary_t *)bin->lhs;
    expect_op_text(lhs->op, "-");

    cleanup_parser(p);
}

/**
 * Scenario: 混合运算 — a + b == c && d | e
 * 绑定力：+(17) < ==(11? no, ==11 > +17? no)
 * 等一下，==(11) < +(17)，所以 + 先绑定
 * 完整解析：((a + b) == c) && (d | e)
 * Expected: 顶层 &&，lhs 是 ==，rhs 是 |
 */
TEST_F(ParseExprTest, Binary_MixedPrecedence) {
    parser_t *p = make_parser("a + b == c && d | e");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "&&");
    ASSERT_NE(bin->lhs, nullptr);
    EXPECT_EQ(bin->lhs->kind, AST_BINARY); /* (a + b) == c */
    ASSERT_NE(bin->rhs, nullptr);
    EXPECT_EQ(bin->rhs->kind, AST_BINARY); /* d | e */

    /* lhs: == */
    auto *lhs = (ast_binary_t *)bin->lhs;
    expect_op_text(lhs->op, "==");

    /* rhs: | */
    auto *rhs = (ast_binary_t *)bin->rhs;
    expect_op_text(rhs->op, "|");

    cleanup_parser(p);
}

/**
 * Scenario: 前缀一元与位运算 — !a & b 应解析为 (!a) & b
 * 前缀绑定力 23 > 位与绑定力 9/10
 * Expected: 顶层 &，lhs 是 AST_UNARY
 */
TEST_F(ParseExprTest, Binary_PrefixWithBitAnd) {
    parser_t *p = make_parser("!a & b");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BINARY);

    auto *bin = (ast_binary_t *)node;
    expect_op_text(bin->op, "&");
    ASSERT_NE(bin->lhs, nullptr);
    EXPECT_EQ(bin->lhs->kind, AST_UNARY);

    auto *unary = (ast_unary_t *)bin->lhs;
    expect_op_text(unary->op, "!");

    cleanup_parser(p);
}

/* ================================================================ */
/* 数组类型表达式 [N]T                                              */
/* ================================================================ */

/**
 * Scenario: 一维数组类型 [1]i32
 * Expected: AST_ARRAY，length=AST_INT_LIT(1)，base_type=AST_IDENT(i32)
 */
TEST_F(ParseExprTest, Array_SingleDimension) {
    parser_t *p = make_parser("[1]i32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ARRAY);

    auto *arr = (ast_array_t *)node;
    ASSERT_NE(arr->length, nullptr);
    EXPECT_EQ(arr->length->kind, AST_INT_LIT);
    EXPECT_EQ(((ast_int_lit_t *)arr->length)->value, 1ULL);
    ASSERT_NE(arr->base_type, nullptr);
    EXPECT_EQ(arr->base_type->kind, AST_IDENT);
    EXPECT_EQ(((ast_ident_t *)arr->base_type)->name.len, 3u);
    EXPECT_EQ(memcmp(((ast_ident_t *)arr->base_type)->name.ptr, "i32", 3), 0);

    cleanup_parser(p);
}

/**
 * Scenario: 多维数组类型 [2][3]i32（嵌套 AST_ARRAY）
 * Expected: 外层 length=2，base_type 是 AST_ARRAY([3]i32)
 */
TEST_F(ParseExprTest, Array_MultiDimension) {
    parser_t *p = make_parser("[2][3]i32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ARRAY);

    auto *outer = (ast_array_t *)node;
    ASSERT_NE(outer->length, nullptr);
    EXPECT_EQ(outer->length->kind, AST_INT_LIT);
    EXPECT_EQ(((ast_int_lit_t *)outer->length)->value, 2ULL);

    ASSERT_NE(outer->base_type, nullptr);
    ASSERT_EQ(outer->base_type->kind, AST_ARRAY);

    auto *inner = (ast_array_t *)outer->base_type;
    ASSERT_NE(inner->length, nullptr);
    EXPECT_EQ(inner->length->kind, AST_INT_LIT);
    EXPECT_EQ(((ast_int_lit_t *)inner->length)->value, 3ULL);
    ASSERT_NE(inner->base_type, nullptr);
    EXPECT_EQ(inner->base_type->kind, AST_IDENT);
    EXPECT_EQ(((ast_ident_t *)inner->base_type)->name.len, 3u);
    EXPECT_EQ(memcmp(((ast_ident_t *)inner->base_type)->name.ptr, "i32", 3), 0);

    cleanup_parser(p);
}

/**
 * Scenario: 数组类型长度可用标识符 [N]i32（长度在 sema 阶段求值）
 * Expected: AST_ARRAY，length=AST_IDENT(N)
 */
TEST_F(ParseExprTest, Array_LengthIsIdentifier) {
    parser_t *p = make_parser("[N]i32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ARRAY);

    auto *arr = (ast_array_t *)node;
    ASSERT_NE(arr->length, nullptr);
    EXPECT_EQ(arr->length->kind, AST_IDENT);
    EXPECT_EQ(((ast_ident_t *)arr->length)->name.len, 1u);
    EXPECT_EQ(memcmp(((ast_ident_t *)arr->length)->name.ptr, "N", 1), 0);
    ASSERT_NE(arr->base_type, nullptr);
    EXPECT_EQ(arr->base_type->kind, AST_IDENT);

    cleanup_parser(p);
}

/**
 * Scenario: 数组类型内部允许空格与注释容错 [ 1 ] （含注释） i32
 * Expected: 仍解析为 AST_ARRAY（skip_trivia 天然容错）
 */
TEST_F(ParseExprTest, Array_TriviaTolerant) {
    parser_t *p = make_parser("[ 1 ] /* len */ i32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ARRAY);

    auto *arr = (ast_array_t *)node;
    ASSERT_NE(arr->length, nullptr);
    EXPECT_EQ(arr->length->kind, AST_INT_LIT);
    ASSERT_NE(arr->base_type, nullptr);
    EXPECT_EQ(arr->base_type->kind, AST_IDENT);

    cleanup_parser(p);
}

/**
 * Scenario: 数组类型缺少长度 [i32 → AST_ERROR
 * Expected: 返回 AST_ERROR 节点
 */
TEST_F(ParseExprTest, Array_MissingLengthReturnsError) {
    parser_t *p = make_parser("[i32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: 数组类型缺少 ']' [1 i32 → AST_ERROR
 * Expected: 返回 AST_ERROR 节点
 */
TEST_F(ParseExprTest, Array_MissingCloseBracketReturnsError) {
    parser_t *p = make_parser("[1 i32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: 数组类型缺少基础类型 [1] → AST_ERROR
 * Expected: 返回 AST_ERROR 节点
 */
TEST_F(ParseExprTest, Array_MissingBaseTypeReturnsError) {
    parser_t *p = make_parser("[1]");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/* ================================================================ */
/* 类型字面量构造 .<type> { ... }（'.' 前导）                       */
/* ================================================================ */

/**
 * Scenario: 基础构造 .i32{123}
 * Expected: AST_CONSTRUCT，type=AST_IDENT(i32)，fields=单 int_lit(123)
 */
TEST_F(ParseExprTest, Construct_Basic) {
    parser_t *p = make_parser(".i32{123}");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_CONSTRUCT);

    auto *c = (ast_construct_t *)node;
    ASSERT_NE(c->type, nullptr);
    EXPECT_EQ(c->type->kind, AST_IDENT);
    EXPECT_EQ(((ast_ident_t *)c->type)->name.len, 3u);
    EXPECT_EQ(memcmp(((ast_ident_t *)c->type)->name.ptr, "i32", 3), 0);
    ASSERT_NE(c->fields, nullptr);
    EXPECT_EQ(c->fields->kind, AST_INT_LIT);
    EXPECT_EQ(((ast_int_lit_t *)c->fields)->value, 123ULL);
    EXPECT_EQ(c->fields->next, nullptr);  /* 单字段 */

    cleanup_parser(p);
}

/**
 * Scenario: 数组构造 .[1]i32{123}
 * Expected: AST_CONSTRUCT，type=AST_ARRAY([1]i32)，fields=单 int_lit(123)
 */
TEST_F(ParseExprTest, Construct_ArrayType) {
    parser_t *p = make_parser(".[1]i32{123}");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_CONSTRUCT);

    auto *c = (ast_construct_t *)node;
    ASSERT_NE(c->type, nullptr);
    ASSERT_EQ(c->type->kind, AST_ARRAY);
    ASSERT_NE(c->fields, nullptr);
    EXPECT_EQ(c->fields->kind, AST_INT_LIT);
    EXPECT_EQ(((ast_int_lit_t *)c->fields)->value, 123ULL);

    cleanup_parser(p);
}

/**
 * Scenario: 多字段构造 .i32{1, 2, 3}
 * Expected: AST_CONSTRUCT，fields 链表含 3 个 int_lit
 */
TEST_F(ParseExprTest, Construct_MultipleFields) {
    parser_t *p = make_parser(".i32{1, 2, 3}");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_CONSTRUCT);

    auto *c = (ast_construct_t *)node;
    ASSERT_NE(c->fields, nullptr);
    int count = 0;
    for (ast_node_t *f = c->fields; f; f = f->next) {
        EXPECT_EQ(f->kind, AST_INT_LIT);
        count++;
    }
    EXPECT_EQ(count, 3);
    EXPECT_EQ(c->fields_last->kind, AST_INT_LIT);

    cleanup_parser(p);
}

/**
 * Scenario: 空构造 .i32{} → AST_CONSTRUCT，fields 为 NULL
 */
TEST_F(ParseExprTest, Construct_EmptyFields) {
    parser_t *p = make_parser(".i32{}");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_CONSTRUCT);

    auto *c = (ast_construct_t *)node;
    EXPECT_EQ(c->fields, nullptr);
    EXPECT_EQ(c->fields_last, nullptr);

    cleanup_parser(p);
}

/**
 * Scenario: 构造缺少类型 . {123} → AST_ERROR（'.' 后必须是类型）
 */
TEST_F(ParseExprTest, Construct_MissingTypeReturnsError) {
    parser_t *p = make_parser(".{123}");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: 构造缺少 '{' .i32 123 → AST_ERROR
 */
TEST_F(ParseExprTest, Construct_MissingBraceReturnsError) {
    parser_t *p = make_parser(".i32 123");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: 构造缺少 '}' 闭合 .i32{123 → AST_ERROR
 */
TEST_F(ParseExprTest, Construct_MissingCloseBraceReturnsError) {
    parser_t *p = make_parser(".i32{123");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/* ================================================================ */
/* 函数签名类型 func(...)->ret（M2 函数类型）                        */
/* ================================================================ */

static void expect_ident_text(ast_node_t *n, const char *expected) {
    ASSERT_NE(n, nullptr);
    ASSERT_EQ(n->kind, AST_IDENT);
    ast_ident_t *id = (ast_ident_t *)n;
    EXPECT_EQ(id->name.len, strlen(expected));
    EXPECT_EQ(memcmp(id->name.ptr, expected, id->name.len), 0);
}

/**
 * Scenario: func(i32,i32)->i32 基本签名
 * Expected: AST_FUNC_TYPE，params=[i32,i32]，return_type=i32
 */
TEST_F(ParseExprTest, FuncType_Basic) {
    parser_t *p = make_parser("func(i32,i32)->i32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_FUNC_TYPE);

    auto *ft = (ast_func_type_t *)node;
    ASSERT_NE(ft->params, nullptr);
    expect_ident_text(ft->params, "i32");
    ASSERT_NE(ft->params->next, nullptr);
    expect_ident_text(ft->params->next, "i32");
    EXPECT_EQ(ft->params->next->next, nullptr); /* 两参数 */
    expect_ident_text(ft->return_type, "i32");

    cleanup_parser(p);
}

/**
 * Scenario: func(i32) 无 '->' 返回类型 → 不允许隐式 void 返回值
 * Expected: AST_ERROR（统一生成式下 func(...) 后无 '->' 即匿名字面量）
 */
TEST_F(ParseExprTest, FuncType_NoReturnTypeFails) {
    parser_t *p = make_parser("func(i32)");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: func() 零参数 + 返回类型
 * Expected: AST_FUNC_TYPE，params=NULL，return_type=i32
 */
TEST_F(ParseExprTest, FuncType_ZeroParams) {
    parser_t *p = make_parser("func()->i32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_FUNC_TYPE);

    auto *ft = (ast_func_type_t *)node;
    EXPECT_EQ(ft->params, nullptr);
    expect_ident_text(ft->return_type, "i32");

    cleanup_parser(p);
}

/**
 * Scenario: 参数含复合类型 func([4]i32)->func(i32)->i32
 * Expected: AST_FUNC_TYPE，param[0]=AST_ARRAY([4]i32)，return=AST_FUNC_TYPE
 */
TEST_F(ParseExprTest, FuncType_CompoundParams) {
    parser_t *p = make_parser("func([4]i32)->func(i32)->i32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_expr(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_FUNC_TYPE);

    auto *ft = (ast_func_type_t *)node;
    ASSERT_NE(ft->params, nullptr);
    ASSERT_EQ(ft->params->kind, AST_ARRAY);
    ASSERT_NE(ft->return_type, nullptr);
    EXPECT_EQ(ft->return_type->kind, AST_FUNC_TYPE);

    auto *rt = (ast_func_type_t *)ft->return_type;
    expect_ident_text(rt->params, "i32");
    expect_ident_text(rt->return_type, "i32");

    cleanup_parser(p);
}

/**
 * Scenario: func 关键字分支优先于 AST_IDENT 兜底
 * Expected: parse_primary 对 "func()->void" 返回 AST_FUNC_TYPE 而非 AST_IDENT
 */
TEST_F(ParseExprTest, FuncType_FuncKeywordNotIdent) {
    parser_t *p = make_parser("func()->void");
    ASSERT_NE(p, nullptr);

    skip_trivia(p);

    ast_node_t *node = parse_primary(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_FUNC_TYPE);

    cleanup_parser(p);
}

} // namespace
