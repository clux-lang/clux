/*
 * Description: parse_assign_or_expr_stmt unit tests
 * Create: 2026-09-07
 */

#include <gtest/gtest.h>
#include <cstring>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "core/arena.h"
#include "core/vec.h"
#include "core/stream.h"
#include "core/panic.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "parser/parse_stmt.h"
#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_node.h"
#include "parser/ast_kind.h"
#include "parser/ast_ident.h"
#include "parser/ast_assign.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_var_def.h"
#include "parser/ast_block.h"
#include "parser/ast_if.h"
#include "parser/ast_while.h"
#include "parser/ast_for.h"
#include "parser/ast_return.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_binary.h"
#include "parser/ast_error.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ident.h"
#include "parser/ast_program.h"
}

#include "test_common.h"

namespace {

/* ---- Helper: build a token pool from source text ---- */

struct LexResult {
    vec_t       *tokens;
    lexer_t     *lexer;
    char        *source_buf;
    size_t       source_len;
};

static LexResult lex_source(allocator_t *alloc, const char *src) {
    LexResult result;
    result.source_len = strlen(src);
    result.source_buf = (char *)malloc(result.source_len + 1);
    memcpy(result.source_buf, src, result.source_len + 1);

    stream_source_t mem_src = stream_source_mem(
        alloc, result.source_buf, result.source_len, false);
    istream_t *stream = istream_open(alloc, mem_src);

    lexer_t *lexer = lexer_create(alloc, stream, "test.clx");

    vec_t *pool = vec_new(alloc, true);
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
    lexer_close(&lr.lexer);
    vec_free(alloc, &lr.tokens);
    free(lr.source_buf);
    lr.source_buf = nullptr;
}

/* ---- Fixture ---- */

class ParseStmtTest : public ::testing::Test {
protected:
    void SetUp() override {
        alloc_ = create_allocator(malloc, free);
        arena_ = arena_new_default(alloc_);
    }

    void TearDown() override {
        arena_destroy(alloc_, &arena_);
        EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc_);
    }

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

/* ---- 辅助：断言 token 文本 ---- */

static void expect_token_text(const token_t *tok, const char *expected) {
    ASSERT_NE(tok, nullptr);
    strslice_t s = token_strslice(tok);
    EXPECT_EQ(s.len, strlen(expected));
    EXPECT_EQ(memcmp(s.ptr, expected, s.len), 0);
}

/* ================================================================ */
/* Expression Statement: expr;                                       */
/* ================================================================ */

/**
 * Scenario: Simple function call as expression statement
 * Expected: AST_EXPR_STMT with expr being AST_CALL
 */
TEST_F(ParseStmtTest, ExprStmt_FunctionCall) {
    parser_t *p = make_parser("foo();");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_EXPR_STMT);

    auto *stmt = (ast_expr_stmt_t *)node;
    ASSERT_NE(stmt->expr, nullptr);
    EXPECT_EQ(stmt->expr->kind, AST_CALL);

    cleanup_parser(p);
}

/**
 * Scenario: Integer literal as expression statement
 * Expected: AST_EXPR_STMT with expr being AST_INT_LIT
 */
TEST_F(ParseStmtTest, ExprStmt_IntLiteral) {
    parser_t *p = make_parser("42;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_EXPR_STMT);

    auto *stmt = (ast_expr_stmt_t *)node;
    ASSERT_NE(stmt->expr, nullptr);
    EXPECT_EQ(stmt->expr->kind, AST_INT_LIT);

    cleanup_parser(p);
}

/**
 * Scenario: Binary expression as expression statement
 * Expected: AST_EXPR_STMT with expr being AST_BINARY
 */
TEST_F(ParseStmtTest, ExprStmt_BinaryExpr) {
    parser_t *p = make_parser("a + b;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_EXPR_STMT);

    auto *stmt = (ast_expr_stmt_t *)node;
    ASSERT_NE(stmt->expr, nullptr);
    EXPECT_EQ(stmt->expr->kind, AST_BINARY);

    cleanup_parser(p);
}

/**
 * Scenario: Missing semicolon after expression
 * Expected: AST_ERROR
 */
TEST_F(ParseStmtTest, ExprStmt_MissingSemicolon) {
    parser_t *p = make_parser("42");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/* ================================================================ */
/* Assignment: name = expr; / name += expr; etc.                     */
/* ================================================================ */

/**
 * Scenario: Simple assignment x = 10;
 * Expected: AST_ASSIGN with name="x", op="=", value is AST_INT_LIT
 */
TEST_F(ParseStmtTest, Assign_Simple) {
    parser_t *p = make_parser("x = 10;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ASSIGN);

    auto *assign = (ast_assign_t *)node;
    ASSERT_EQ(assign->target->kind, AST_IDENT);
    auto *lid = (ast_ident_t *)assign->target;
    EXPECT_EQ(lid->name.len, 1u);
    EXPECT_EQ(lid->name.ptr[0], 'x');
    expect_token_text(assign->op, "=");
    ASSERT_NE(assign->value, nullptr);
    EXPECT_EQ(assign->value->kind, AST_INT_LIT);

    cleanup_parser(p);
}

/**
 * Scenario: Compound assignment y += 5;
 * Expected: AST_ASSIGN with op="+="
 */
TEST_F(ParseStmtTest, Assign_CompoundAdd) {
    parser_t *p = make_parser("y += 5;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ASSIGN);

    auto *assign = (ast_assign_t *)node;
    expect_token_text(assign->op, "+=");
    ASSERT_NE(assign->value, nullptr);
    EXPECT_EQ(assign->value->kind, AST_INT_LIT);

    cleanup_parser(p);
}

/**
 * Scenario: Compound subtraction assignment z -= 3;
 * Expected: AST_ASSIGN with op="-="
 */
TEST_F(ParseStmtTest, Assign_CompoundSub) {
    parser_t *p = make_parser("z -= 3;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ASSIGN);

    auto *assign = (ast_assign_t *)node;
    expect_token_text(assign->op, "-=");

    cleanup_parser(p);
}

/**
 * Scenario: Compound multiplication assignment m *= 2;
 * Expected: AST_ASSIGN with op="*="
 */
TEST_F(ParseStmtTest, Assign_CompoundMul) {
    parser_t *p = make_parser("m *= 2;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ASSIGN);

    auto *assign = (ast_assign_t *)node;
    expect_token_text(assign->op, "*=");

    cleanup_parser(p);
}

/**
 * Scenario: Compound division assignment d /= 4;
 * Expected: AST_ASSIGN with op="/="
 */
TEST_F(ParseStmtTest, Assign_CompoundDiv) {
    parser_t *p = make_parser("d /= 4;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ASSIGN);

    auto *assign = (ast_assign_t *)node;
    expect_token_text(assign->op, "/=");

    cleanup_parser(p);
}

/**
 * Scenario: Compound modulo assignment r %= 2;
 * Expected: AST_ASSIGN with op="%="
 */
TEST_F(ParseStmtTest, Assign_CompoundMod) {
    parser_t *p = make_parser("r %= 2;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ASSIGN);

    auto *assign = (ast_assign_t *)node;
    expect_token_text(assign->op, "%=");

    cleanup_parser(p);
}

/**
 * Scenario: Assignment with expression RHS: result = a + b * c;
 * Expected: AST_ASSIGN, value is AST_BINARY
 */
TEST_F(ParseStmtTest, Assign_ExprRHS) {
    parser_t *p = make_parser("result = a + b * c;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ASSIGN);

    auto *assign = (ast_assign_t *)node;
    ASSERT_NE(assign->value, nullptr);
    EXPECT_EQ(assign->value->kind, AST_BINARY);

    cleanup_parser(p);
}

/**
 * Scenario: Assignment missing semicolon: x = 10
 * Expected: AST_ERROR
 */
TEST_F(ParseStmtTest, Assign_MissingSemicolon) {
    parser_t *p = make_parser("x = 10");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: Assignment with missing RHS: x = ;
 * Expected: AST_ERROR
 */
TEST_F(ParseStmtTest, Assign_MissingRHS) {
    parser_t *p = make_parser("x = ;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/* ================================================================ */
/* Underscore assignment: _ = expr; (discard semantics in Sema)       */
/* ================================================================ */

/**
 * Scenario: _ = foo(); is parsed as AST_ASSIGN with name="_"
 * Expected: AST_ASSIGN, name="_", op="=", value is AST_CALL
 */
TEST_F(ParseStmtTest, UnderscoreAssign_FunctionCall) {
    parser_t *p = make_parser("_ = foo();");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ASSIGN);

    auto *assign = (ast_assign_t *)node;
    ASSERT_EQ(assign->target->kind, AST_IDENT);
    EXPECT_EQ(((ast_ident_t *)assign->target)->name.len, 1u);
    EXPECT_EQ(((ast_ident_t *)assign->target)->name.ptr[0], '_');
    expect_token_text(assign->op, "=");
    ASSERT_NE(assign->value, nullptr);
    EXPECT_EQ(assign->value->kind, AST_CALL);

    cleanup_parser(p);
}

/**
 * Scenario: _ = 42; is parsed as AST_ASSIGN with name="_"
 * Expected: AST_ASSIGN, name="_", value is AST_INT_LIT(42)
 */
TEST_F(ParseStmtTest, UnderscoreAssign_IntLiteral) {
    parser_t *p = make_parser("_ = 42;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ASSIGN);

    auto *assign = (ast_assign_t *)node;
    ASSERT_EQ(assign->target->kind, AST_IDENT);
    EXPECT_EQ(((ast_ident_t *)assign->target)->name.len, 1u);
    EXPECT_EQ(((ast_ident_t *)assign->target)->name.ptr[0], '_');
    ASSERT_NE(assign->value, nullptr);
    EXPECT_EQ(assign->value->kind, AST_INT_LIT);
    EXPECT_EQ(((ast_int_lit_t *)assign->value)->value, 42ULL);

    cleanup_parser(p);
}

/**
 * Scenario: _ = a + b; is parsed as AST_ASSIGN with name="_"
 * Expected: AST_ASSIGN, name="_", value is AST_BINARY
 */
TEST_F(ParseStmtTest, UnderscoreAssign_BinaryExpr) {
    parser_t *p = make_parser("_ = a + b;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ASSIGN);

    auto *assign = (ast_assign_t *)node;
    ASSERT_EQ(assign->target->kind, AST_IDENT);
    EXPECT_EQ(((ast_ident_t *)assign->target)->name.len, 1u);
    EXPECT_EQ(((ast_ident_t *)assign->target)->name.ptr[0], '_');
    ASSERT_NE(assign->value, nullptr);
    EXPECT_EQ(assign->value->kind, AST_BINARY);

    cleanup_parser(p);
}

/**
 * Scenario: _ = 42 missing semicolon → AST_ERROR
 */
TEST_F(ParseStmtTest, UnderscoreAssign_MissingSemicolon) {
    parser_t *p = make_parser("_ = 42");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: _ = ; missing RHS → AST_ERROR
 */
TEST_F(ParseStmtTest, UnderscoreAssign_MissingRHS) {
    parser_t *p = make_parser("_ = ;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: _ += 1; is AST_ASSIGN with name="_", op="+="
 * Compound assignment with underscore is also a normal AST_ASSIGN
 */
TEST_F(ParseStmtTest, UnderscoreAssign_CompoundAssign) {
    parser_t *p = make_parser("_ += 1;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_assign_or_expr_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_ASSIGN);

    auto *assign = (ast_assign_t *)node;
    ASSERT_EQ(assign->target->kind, AST_IDENT);
    EXPECT_EQ(((ast_ident_t *)assign->target)->name.len, 1u);
    EXPECT_EQ(((ast_ident_t *)assign->target)->name.ptr[0], '_');
    expect_token_text(assign->op, "+=");

    cleanup_parser(p);
}

/* ================================================================ */
/* Var Definition: var name[:type] = init;  (init required)          */
/* ================================================================ */

/**
 * Scenario: var without initializer: var x;
 * Expected: AST_ERROR (initializer is required)
 */
TEST_F(ParseStmtTest, VarDef_MissingInitializer) {
    parser_t *p = make_parser("var x;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_var_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: var with type but no initializer: var count:i32;
 * Expected: AST_ERROR (initializer is required)
 */
TEST_F(ParseStmtTest, VarDef_WithTypeNoInit) {
    parser_t *p = make_parser("var count:i32;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_var_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: var with init: var x = 42;
 * Expected: AST_VAR_DEF, name="x", no type, init is AST_INT_LIT(42)
 */
TEST_F(ParseStmtTest, VarDef_WithInit) {
    parser_t *p = make_parser("var x = 42;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_var_def(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_VAR_DEF);

    auto *vd = (ast_var_def_t *)node;
    EXPECT_EQ(vd->type_expr, nullptr); /* 无类型标注 = 推断 */
    ASSERT_NE(vd->init, nullptr);
    EXPECT_EQ(vd->init->kind, AST_INT_LIT);
    EXPECT_EQ(((ast_int_lit_t *)vd->init)->value, 42ULL);

    cleanup_parser(p);
}

/**
 * Scenario: var with type and init: var pi:f64 = 3.14;
 * Expected: AST_VAR_DEF, type_expr=AST_IDENT("f64"), init is AST_FLOAT_LIT
 */
TEST_F(ParseStmtTest, VarDef_WithTypeAndInit) {
    parser_t *p = make_parser("var pi:f64 = 3.14;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_var_def(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_VAR_DEF);

    auto *vd = (ast_var_def_t *)node;
    ASSERT_NE(vd->type_expr, nullptr);
    EXPECT_EQ(vd->type_expr->kind, AST_IDENT);
    auto *tn = (ast_ident_t *)vd->type_expr;
    EXPECT_TRUE(strslice_eq(tn->name, strslice_from_cstr("f64")));
    ASSERT_NE(vd->init, nullptr);
    EXPECT_EQ(vd->init->kind, AST_FLOAT_LIT);

    cleanup_parser(p);
}

/**
 * Scenario: var with complex init expression: var result = a + b * c;
 * Expected: AST_VAR_DEF, init is AST_BINARY
 */
TEST_F(ParseStmtTest, VarDef_ComplexInit) {
    parser_t *p = make_parser("var result = a + b * c;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_var_def(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_VAR_DEF);

    auto *vd = (ast_var_def_t *)node;
    ASSERT_NE(vd->init, nullptr);
    EXPECT_EQ(vd->init->kind, AST_BINARY);

    cleanup_parser(p);
}

/**
 * Scenario: Missing variable name: var ;
 * Expected: AST_ERROR
 */
TEST_F(ParseStmtTest, VarDef_MissingName) {
    parser_t *p = make_parser("var ;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_var_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: Missing semicolon: var x = 1
 * Expected: AST_ERROR
 */
TEST_F(ParseStmtTest, VarDef_MissingSemicolon) {
    parser_t *p = make_parser("var x = 1");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_var_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: Type annotation without type name: var x:;
 * Expected: AST_ERROR
 */
TEST_F(ParseStmtTest, VarDef_MissingTypeName) {
    parser_t *p = make_parser("var x:;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_var_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: Init without expression: var x = ;
 * Expected: AST_ERROR
 */
TEST_F(ParseStmtTest, VarDef_MissingInitExpr) {
    parser_t *p = make_parser("var x = ;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_var_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: Non-var input returns NULL (mismatch)
 */
TEST_F(ParseStmtTest, VarDef_Mismatch) {
    parser_t *p = make_parser("x = 1;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_var_def(p);
    EXPECT_EQ(node, nullptr);

    cleanup_parser(p);
}

/* ================================================================ */
/* Block: { stmt; stmt; ... }                                        */
/* ================================================================ */

/**
 * Scenario: Empty block
 * Expected: AST_BLOCK with no stmts
 */
TEST_F(ParseStmtTest, Block_Empty) {
    parser_t *p = make_parser("{}");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_block(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BLOCK);

    auto *blk = (ast_block_t *)node;
    EXPECT_EQ(blk->stmts, nullptr);

    cleanup_parser(p);
}

/**
 * Scenario: Block with one statement
 * Expected: AST_BLOCK with one stmt
 */
TEST_F(ParseStmtTest, Block_OneStmt) {
    parser_t *p = make_parser("{ var x = 1; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_block(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BLOCK);

    auto *blk = (ast_block_t *)node;
    ASSERT_NE(blk->stmts, nullptr);
    EXPECT_EQ(blk->stmts->kind, AST_VAR_DEF);
    EXPECT_EQ(blk->stmts->next, nullptr);

    cleanup_parser(p);
}

/**
 * Scenario: Block with multiple statements
 * Expected: AST_BLOCK with stmts linked via next
 */
TEST_F(ParseStmtTest, Block_MultipleStmts) {
    parser_t *p = make_parser("{ var x = 1; x + 2; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_block(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BLOCK);

    auto *blk = (ast_block_t *)node;
    ASSERT_NE(blk->stmts, nullptr);
    EXPECT_EQ(blk->stmts->kind, AST_VAR_DEF);
    ASSERT_NE(blk->stmts->next, nullptr);
    EXPECT_EQ(blk->stmts->next->kind, AST_EXPR_STMT);

    cleanup_parser(p);
}

/**
 * Scenario: Missing closing brace
 * Expected: AST_ERROR
 */
TEST_F(ParseStmtTest, Block_MissingCloseBrace) {
    parser_t *p = make_parser("{ var x = 1; ");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_block(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/**
 * Scenario: Bare block as standalone statement via parse_stmt
 * Expected: AST_BLOCK dispatched by parse_stmt
 */
TEST_F(ParseStmtTest, Stmt_BareBlock) {
    parser_t *p = make_parser("{ var x = 1; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BLOCK);

    auto *blk = (ast_block_t *)node;
    ASSERT_NE(blk->stmts, nullptr);
    EXPECT_EQ(blk->stmts->kind, AST_VAR_DEF);

    cleanup_parser(p);
}

/**
 * Scenario: Nested bare blocks
 * Expected: outer AST_BLOCK containing inner AST_BLOCK
 */
TEST_F(ParseStmtTest, Stmt_NestedBareBlock) {
    parser_t *p = make_parser("{ { var x = 1; } }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_stmt(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_BLOCK);

    auto *blk = (ast_block_t *)node;
    ASSERT_NE(blk->stmts, nullptr);
    EXPECT_EQ(blk->stmts->kind, AST_BLOCK);

    cleanup_parser(p);
}

/* ================================================================ */
/* Return: return [expr];                                            */
/* ================================================================ */

/**
 * Scenario: return with expression
 * Expected: AST_RETURN with value
 */
TEST_F(ParseStmtTest, Return_WithExpr) {
    parser_t *p = make_parser("return 42;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_return(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_RETURN);

    auto *ret = (ast_return_t *)node;
    ASSERT_NE(ret->value, nullptr);
    EXPECT_EQ(ret->value->kind, AST_INT_LIT);

    cleanup_parser(p);
}

/**
 * Scenario: return without expression
 * Expected: AST_RETURN with value = NULL
 */
TEST_F(ParseStmtTest, Return_Void) {
    parser_t *p = make_parser("return;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_return(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_RETURN);

    auto *ret = (ast_return_t *)node;
    EXPECT_EQ(ret->value, nullptr);

    cleanup_parser(p);
}

/**
 * Scenario: return missing semicolon
 * Expected: AST_ERROR
 */
TEST_F(ParseStmtTest, Return_MissingSemicolon) {
    parser_t *p = make_parser("return 42");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_return(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/* ================================================================ */
/* Break / Continue                                                  */
/* ================================================================ */

TEST_F(ParseStmtTest, Break) {
    parser_t *p = make_parser("break;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_break(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_BREAK);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, Continue) {
    parser_t *p = make_parser("continue;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_continue(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_CONTINUE);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, Break_MissingSemicolon) {
    parser_t *p = make_parser("break");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_break(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/* ================================================================ */
/* If: if cond { then } [else { else }]                              */
/* ================================================================ */

/**
 * Scenario: Simple if with block
 * Expected: AST_IF with cond and then_body, no else_body
 */
TEST_F(ParseStmtTest, If_Simple) {
    parser_t *p = make_parser("if (x) { var y = 1; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_if(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_IF);

    auto *if_node = (ast_if_t *)node;
    ASSERT_NE(if_node->cond, nullptr);
    EXPECT_EQ(if_node->cond->kind, AST_IDENT);
    ASSERT_NE(if_node->then_body, nullptr);
    EXPECT_EQ(if_node->then_body->kind, AST_BLOCK);
    EXPECT_EQ(if_node->else_body, nullptr);

    cleanup_parser(p);
}

/**
 * Scenario: if-else
 * Expected: AST_IF with else_body
 */
TEST_F(ParseStmtTest, If_WithElse) {
    parser_t *p = make_parser("if (x) { var y = 1; } else { var z = 2; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_if(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_IF);

    auto *if_node = (ast_if_t *)node;
    ASSERT_NE(if_node->else_body, nullptr);
    EXPECT_EQ(if_node->else_body->kind, AST_BLOCK);

    cleanup_parser(p);
}

/**
 * Scenario: else-if chain
 * Expected: else_body is another AST_IF
 */
TEST_F(ParseStmtTest, If_ElseIfChain) {
    parser_t *p = make_parser("if (a) { var x = 1; } else if (b) { var y = 2; } else { var z = 3; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_if(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_IF);

    auto *if_node = (ast_if_t *)node;
    ASSERT_NE(if_node->else_body, nullptr);
    EXPECT_EQ(if_node->else_body->kind, AST_IF);

    auto *else_if = (ast_if_t *)if_node->else_body;
    ASSERT_NE(else_if->else_body, nullptr);
    EXPECT_EQ(else_if->else_body->kind, AST_BLOCK);

    cleanup_parser(p);
}

/**
 * Scenario: if missing block
 * Expected: AST_ERROR
 */
TEST_F(ParseStmtTest, If_MissingBlock) {
    parser_t *p = make_parser("if (x) var y = 1;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_if(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/* ================================================================ */
/* While: while cond { body }                                        */
/* ================================================================ */

TEST_F(ParseStmtTest, While_Simple) {
    parser_t *p = make_parser("while (x) { var y = 1; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_while(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_WHILE);

    auto *wh = (ast_while_t *)node;
    ASSERT_NE(wh->cond, nullptr);
    EXPECT_EQ(wh->cond->kind, AST_IDENT);
    ASSERT_NE(wh->body, nullptr);
    EXPECT_EQ(wh->body->kind, AST_BLOCK);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, While_MissingBlock) {
    parser_t *p = make_parser("while (x) var y = 1;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_while(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/* ================================================================ */
/* For: for (init; cond; update) { body }                            */
/* ================================================================ */

TEST_F(ParseStmtTest, For_WithVarInit) {
    parser_t *p = make_parser("for (var i:i32 = 0; i < 10; i + 1) { var x = i; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_for(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_FOR);

    auto *fr = (ast_for_t *)node;
    ASSERT_NE(fr->init, nullptr);
    EXPECT_EQ(fr->init->kind, AST_VAR_DEF);
    ASSERT_NE(fr->cond, nullptr);
    ASSERT_NE(fr->update, nullptr);
    ASSERT_NE(fr->body, nullptr);
    EXPECT_EQ(fr->body->kind, AST_BLOCK);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, For_EmptyInit) {
    parser_t *p = make_parser("for (; x < 10; x + 1) { var y = 1; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_for(p);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->kind, AST_FOR);

    auto *fr = (ast_for_t *)node;
    EXPECT_EQ(fr->init, nullptr);
    ASSERT_NE(fr->cond, nullptr);
    ASSERT_NE(fr->update, nullptr);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, For_MissingParens) {
    parser_t *p = make_parser("for var i:i32 = 0; i < 10; i + 1 { }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_for(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/* ================================================================ */
/* parse_stmt: dispatcher                                            */
/* ================================================================ */

/**
 * Scenario: parse_stmt dispatches to var_def
 */
TEST_F(ParseStmtTest, Stmt_DispatchVar) {
    parser_t *p = make_parser("var x = 1;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_stmt(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_VAR_DEF);

    cleanup_parser(p);
}

/**
 * Scenario: parse_stmt dispatches to if
 */
TEST_F(ParseStmtTest, Stmt_DispatchIf) {
    parser_t *p = make_parser("if (x) { }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_stmt(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_IF);

    cleanup_parser(p);
}

/**
 * Scenario: parse_stmt dispatches to return
 */
TEST_F(ParseStmtTest, Stmt_DispatchReturn) {
    parser_t *p = make_parser("return 42;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_stmt(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_RETURN);

    cleanup_parser(p);
}

/**
 * Scenario: parse_stmt dispatches to break
 */
TEST_F(ParseStmtTest, Stmt_DispatchBreak) {
    parser_t *p = make_parser("break;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_stmt(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_BREAK);

    cleanup_parser(p);
}

/**
 * Scenario: parse_stmt falls through to assign_or_expr_stmt
 */
TEST_F(ParseStmtTest, Stmt_FallThroughExpr) {
    parser_t *p = make_parser("foo();");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_stmt(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_EXPR_STMT);

    cleanup_parser(p);
}

/* ================================================================ */
/* parse_func_def 测试                                                */
/* ================================================================ */

TEST_F(ParseStmtTest, FuncDefSimple) {
    parser_t *p = make_parser("func main():i32 { return 0; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_func_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_FUNC_DEF);

    auto *fn = (ast_func_def_t *)node;
    EXPECT_TRUE(strslice_eq(fn->name, strslice_from_cstr("main")));
    ASSERT_NE(fn->return_expr, nullptr);
    EXPECT_EQ(fn->return_expr->kind, AST_IDENT);
    EXPECT_TRUE(strslice_eq(((ast_ident_t *)fn->return_expr)->name,
                            strslice_from_cstr("i32")));
    EXPECT_EQ(fn->params, nullptr);
    ASSERT_NE(fn->body, nullptr);
    EXPECT_EQ(fn->body->kind, AST_BLOCK);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, FuncDefWithParams) {
    parser_t *p = make_parser("func add(a:i32, b:i32):i32 { return a; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_func_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_FUNC_DEF);

    auto *fn = (ast_func_def_t *)node;
    EXPECT_TRUE(strslice_eq(fn->name, strslice_from_cstr("add")));
    ASSERT_NE(fn->return_expr, nullptr);
    EXPECT_EQ(fn->return_expr->kind, AST_IDENT);
    EXPECT_TRUE(strslice_eq(((ast_ident_t *)fn->return_expr)->name,
                            strslice_from_cstr("i32")));

    /* 两个参数 */
    ASSERT_NE(fn->params, nullptr);
    auto *pa = (ast_var_def_t *)fn->params;
    EXPECT_TRUE(strslice_eq(pa->name, strslice_from_cstr("a")));
    ASSERT_NE(pa->type_expr, nullptr);
    EXPECT_EQ(pa->type_expr->kind, AST_IDENT);
    EXPECT_TRUE(strslice_eq(((ast_ident_t *)pa->type_expr)->name,
                            strslice_from_cstr("i32")));

    ASSERT_NE(pa->base.next, nullptr);
    auto *pb = (ast_var_def_t *)pa->base.next;
    EXPECT_TRUE(strslice_eq(pb->name, strslice_from_cstr("b")));
    ASSERT_NE(pb->type_expr, nullptr);
    EXPECT_EQ(pb->type_expr->kind, AST_IDENT);
    EXPECT_TRUE(strslice_eq(((ast_ident_t *)pb->type_expr)->name,
                            strslice_from_cstr("i32")));
    EXPECT_EQ(pb->base.next, nullptr);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, FuncDefVoidReturn) {
    parser_t *p = make_parser("func greet(name:str): void { return; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_func_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_FUNC_DEF);

    auto *fn = (ast_func_def_t *)node;
    /* 显式 : void → return_expr 解析为 AST_IDENT("void") */
    ASSERT_NE(fn->return_expr, nullptr);
    EXPECT_EQ(fn->return_expr->kind, AST_IDENT);
    EXPECT_TRUE(strslice_eq(((ast_ident_t *)fn->return_expr)->name,
                            strslice_from_cstr("void")));
    ASSERT_NE(fn->params, nullptr);

    auto *param = (ast_var_def_t *)fn->params;
    EXPECT_TRUE(strslice_eq(param->name, strslice_from_cstr("name")));
    ASSERT_NE(param->type_expr, nullptr);
    EXPECT_EQ(param->type_expr->kind, AST_IDENT);
    EXPECT_TRUE(strslice_eq(((ast_ident_t *)param->type_expr)->name,
                            strslice_from_cstr("str")));

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, FuncDefNoParams) {
    parser_t *p = make_parser("func foo():u64 { return 1; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_func_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_FUNC_DEF);

    auto *fn = (ast_func_def_t *)node;
    EXPECT_TRUE(strslice_eq(fn->name, strslice_from_cstr("foo")));
    ASSERT_NE(fn->return_expr, nullptr);
    EXPECT_EQ(fn->return_expr->kind, AST_IDENT);
    EXPECT_TRUE(strslice_eq(((ast_ident_t *)fn->return_expr)->name,
                            strslice_from_cstr("u64")));
    EXPECT_EQ(fn->params, nullptr);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, FuncDefMissingName) {
    parser_t *p = make_parser("func ():i32 { }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_func_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, FuncDefMissingParen) {
    parser_t *p = make_parser("func foo:i32 { }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_func_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, FuncDefMissingBody) {
    parser_t *p = make_parser("func foo():i32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_func_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, FuncDefParamMissingType) {
    parser_t *p = make_parser("func foo(a):i32 { }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_func_def(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, FuncDefNotFunc) {
    parser_t *p = make_parser("var x = 1;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_func_def(p);
    EXPECT_EQ(node, nullptr);

    cleanup_parser(p);
}

/* ================================================================ */
/* parse_program 测试                                                 */
/* ================================================================ */

TEST_F(ParseStmtTest, ProgramEmpty) {
    parser_t *p = make_parser("");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_program(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_PROGRAM);

    auto *prog = (ast_program_t *)node;
    EXPECT_EQ(prog->funcs, nullptr);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, ProgramSingleFunc) {
    parser_t *p = make_parser("func main():i32 { return 0; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_program(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_PROGRAM);

    auto *prog = (ast_program_t *)node;
    ASSERT_NE(prog->funcs, nullptr);
    EXPECT_EQ(prog->funcs->kind, AST_FUNC_DEF);
    EXPECT_EQ(prog->funcs->next, nullptr);

    auto *fn = (ast_func_def_t *)prog->funcs;
    EXPECT_TRUE(strslice_eq(fn->name, strslice_from_cstr("main")));

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, ProgramMultipleFuncs) {
    parser_t *p = make_parser(
        "func add(a:i32, b:i32):i32 { return a; }\n"
        "func main():i32 { return 0; }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_program(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_PROGRAM);

    auto *prog = (ast_program_t *)node;
    ASSERT_NE(prog->funcs, nullptr);

    auto *fn1 = (ast_func_def_t *)prog->funcs;
    EXPECT_TRUE(strslice_eq(fn1->name, strslice_from_cstr("add")));

    ASSERT_NE(fn1->base.next, nullptr);
    auto *fn2 = (ast_func_def_t *)fn1->base.next;
    EXPECT_TRUE(strslice_eq(fn2->name, strslice_from_cstr("main")));
    EXPECT_EQ(fn2->base.next, nullptr);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, ProgramNonFuncAtTopLevel) {
    parser_t *p = make_parser("var x = 1;");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_program(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, ProgramFuncErrorPropagation) {
    parser_t *p = make_parser("func foo():i32");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parse_program(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_ERROR);

    cleanup_parser(p);
}

/* ================================================================ */
/* parser_parse 公开 API 测试                                         */
/* ================================================================ */

TEST_F(ParseStmtTest, ParserParseSimple) {
    parser_t *p = make_parser("func main():void { }");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parser_parse(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_PROGRAM);

    cleanup_parser(p);
}

TEST_F(ParseStmtTest, ParserParseEmpty) {
    parser_t *p = make_parser("");
    ASSERT_NE(p, nullptr);

    ast_node_t *node = parser_parse(p);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->kind, AST_PROGRAM);

    cleanup_parser(p);
}

} // namespace
