/*
 * File: parse_extern_lib_test.cpp
 * Description: `extern lib <别名> from "<库名>";` 的解析与迁移诊断。
 *
 * 背景（M6 FFI 表示形式优化）：动态库声明原本是
 * `extern lib "<路径>" as <别名>;`——**完整文件名**写死在源码里，无法跨平台。
 * 现在改为**通用库名**（generic name，CMake 的 find_library 风格）：
 *
 *     extern lib libc from "c";     // 执行器按平台找 c.dll / libc.so / c.so
 *     extern lib libc from "*";     // 当前进程自身（不变）
 *
 * 旧形式已删除，解析器对它会给出**迁移提示**（而不是笼统的语法错误）。
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "core/arena.h"
#include "core/vec.h"
#include "core/stream.h"
#include "diag/diagnostic.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "parser/ast_node.h"
#include "parser/ast_kind.h"
#include "parser/ast_program.h"
#include "parser/ast_extern_lib.h"
}

#include "test_common.h"

namespace {

struct LexResult {
    vec_t   *tokens;
    lexer_t *lexer;
    char    *source_buf;
    size_t   source_len;
};

static LexResult lex_source(allocator_t *alloc, const char *src) {
    LexResult result{};
    result.source_len = strlen(src);
    result.source_buf = (char *)malloc(result.source_len + 1);
    memcpy(result.source_buf, src, result.source_len + 1);

    stream_source_t mem_src =
        stream_source_mem(alloc, result.source_buf, result.source_len, false);
    istream_t *stream = istream_open(alloc, mem_src);
    result.lexer = lexer_create(alloc, stream, "test.clx");

    result.tokens = vec_new(alloc, true);
    for (;;) {
        token_t *t = lexer_next(result.lexer);
        token_kind_t k = token_get_kind(t);
        vec_push(result.tokens, alloc, t);
        if (k == TOKEN_TYPE_EOF || k == TOKEN_TYPE_ERROR) break;
    }
    return result;
}

static void lex_result_destroy(allocator_t *alloc, LexResult &lr) {
    lexer_close(&lr.lexer);
    vec_free(alloc, &lr.tokens);
    free(lr.source_buf);
    lr.source_buf = nullptr;
}

class ParseExternLibTest : public ::testing::Test {
protected:
    void SetUp() override {
        alloc_ = create_allocator(malloc, free);
        arena_ = arena_new_default(alloc_);
        diag_  = diag_buf_new(alloc_);
    }
    void TearDown() override {
        if (lex_.tokens) lex_result_destroy(alloc_, lex_);
        if (diag_) diag_buf_destroy(&diag_);
        arena_destroy(alloc_, &arena_);
        EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc_);
    }

    /** lex → parse，返回程序 AST（诊断进 diag_）。lex_ 保持存活。 */
    ast_node_t *parse(const char *src) {
        lex_ = lex_source(alloc_, src);
        parser_t *parser = parser_create(alloc_, arena_, lex_.tokens);
        parser->diag = diag_;
        ast_node_t *ast = parser_parse(parser);
        parser_destroy(&parser);
        return ast;
    }

    /** 取程序里第一个 AST_EXTERN_LIB（没有则 nullptr）。 */
    static ast_extern_lib_t *first_extern_lib(ast_node_t *ast) {
        if (!ast || ast->kind != AST_PROGRAM) return nullptr;
        for (ast_node_t *n = ((ast_program_t *)ast)->funcs; n; n = n->next) {
            if (n->kind == AST_EXTERN_LIB) return (ast_extern_lib_t *)n;
        }
        return nullptr;
    }

    /** 诊断里是否有一条包含 needle。 */
    bool diag_contains(const char *needle) const {
        const diagnostic_t *items = diag_items(diag_);
        for (size_t i = 0; i < diag_count(diag_); i++) {
            if (items[i].message && strstr(items[i].message, needle)) return true;
        }
        return false;
    }

    allocator_t *alloc_ = nullptr;
    arena_t     *arena_ = nullptr;
    diag_buf_t  *diag_  = nullptr;
    LexResult    lex_{};
};

} // namespace

TEST_F(ParseExternLibTest, ParsesAliasThenName) {
    ast_node_t *ast = parse(
        "extern lib libc from \"c\";\n"
        "func main(): i32 { return 0; }\n");
    ASSERT_FALSE(diag_has_error(diag_));
    ast_extern_lib_t *el = first_extern_lib(ast);
    ASSERT_NE(el, nullptr);

    EXPECT_EQ(el->alias.len, 4u);
    EXPECT_EQ(memcmp(el->alias.ptr, "libc", 4), 0);
    /* `name` 是通用库名（去引号后原样保留） */
    EXPECT_EQ(el->name.len, 1u);
    EXPECT_EQ(memcmp(el->name.ptr, "c", 1), 0);
}

TEST_F(ParseExternLibTest, AcceptsProcessAliasAndExplicitPath) {
    ast_node_t *ast = parse(
        "extern lib libc from \"*\";\n"
        "extern lib x from \"/usr/lib/libfoo.so\";\n"
        "func main(): i32 { return 0; }\n");
    ASSERT_FALSE(diag_has_error(diag_));

    int seen = 0;
    for (ast_node_t *n = ((ast_program_t *)ast)->funcs; n; n = n->next) {
        if (n->kind != AST_EXTERN_LIB) continue;
        ast_extern_lib_t *el = (ast_extern_lib_t *)n;
        if (el->alias.len == 4 && memcmp(el->alias.ptr, "libc", 4) == 0) {
            EXPECT_EQ(el->name.len, 1u);
            EXPECT_EQ(memcmp(el->name.ptr, "*", 1), 0);
            seen++;
        } else if (el->alias.len == 1 && memcmp(el->alias.ptr, "x", 1) == 0) {
            /* 显式路径原样保留：加载器先按 as-is 尝试 */
            EXPECT_EQ(el->name.len, std::strlen("/usr/lib/libfoo.so"));
            seen++;
        }
    }
    EXPECT_EQ(seen, 2);
}

TEST_F(ParseExternLibTest, OldPathAsAliasFormRejectedWithMigrationHint) {
    parse(
        "extern lib \"c\" as libc;\n"
        "func main(): i32 { return 0; }\n");
    EXPECT_TRUE(diag_has_error(diag_));
    EXPECT_TRUE(diag_contains("no longer supported"));
    EXPECT_TRUE(diag_contains("extern lib <alias> from"));
}

TEST_F(ParseExternLibTest, MissingFromKeywordRejected) {
    parse(
        "extern lib libc \"c\";\n"
        "func main(): i32 { return 0; }\n");
    EXPECT_TRUE(diag_has_error(diag_));
    EXPECT_TRUE(diag_contains("from"));
}

TEST_F(ParseExternLibTest, EmptyNameRejected) {
    parse(
        "extern lib libc from \"\";\n"
        "func main(): i32 { return 0; }\n");
    EXPECT_TRUE(diag_has_error(diag_));
    EXPECT_TRUE(diag_contains("empty library name"));
}

TEST_F(ParseExternLibTest, NonIdentifierAliasRejected) {
    parse(
        "extern lib 42 from \"c\";\n"
        "func main(): i32 { return 0; }\n");
    EXPECT_TRUE(diag_has_error(diag_));
    EXPECT_TRUE(diag_contains("alias"));
}
