#ifndef _H_CLUX_PARSER_AST_ERROR_
#define _H_CLUX_PARSER_AST_ERROR_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/vec.h"
#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/lexer.h"
#include "diag/diagnostic.h"
#include <stdarg.h>
#include <stdio.h>

typedef struct {
    ast_node_t  base;
    strslice_t  message;     /* 错误描述信息（arena 分配，NUL 终止） */
} ast_error_t;

/**
 * 创建错误节点（printf 风格格式化消息）。
 * 消息文本会被复制到 arena 上以保证生命周期。
 * 示例：ast_error_new(diag, tokens, arena, tb, te, "expected '%s' but got '%s'", ")", "]");
 *
 * 若传入非 NULL 的 diag / tokens，错误同时被记录进共享诊断缓冲区
 * （位置由 tokens[tok_begin] 解析），交由 driver 在流水线出口统一打印。
 * 二者为 NULL 时（如单测）退化为仅构造节点。
 */
static inline ast_node_t *ast_error_newv(diag_buf_t *diag, vec_t *tokens,
                                         arena_t *arena,
                                         uint32_t tok_begin, uint32_t tok_end,
                                         const char *fmt, va_list ap)
#if defined(__GNUC__)
    __attribute__((format(printf, 6, 0)))
#endif
;

/**
 * va_list 版本，供内部使用。
 */
static inline ast_node_t *ast_error_new(diag_buf_t *diag, vec_t *tokens,
                                        arena_t *arena,
                                        uint32_t tok_begin, uint32_t tok_end,
                                        const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 6, 7)))
#endif
;

/* ---- inline 实现 ---- */

static inline ast_node_t *ast_error_newv(diag_buf_t *diag, vec_t *tokens,
                                         arena_t *arena,
                                         uint32_t tok_begin, uint32_t tok_end,
                                         const char *fmt, va_list ap) {
    ast_error_t *n = (ast_error_t *)arena_calloc(
        arena, 1, sizeof(ast_error_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_ERROR;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;

    if (fmt) {
        /* 先计算所需长度 */
        va_list ap2;
        va_copy(ap2, ap);
        int len = vsnprintf(NULL, 0, fmt, ap2);
        va_end(ap2);

        if (len > 0) {
            char *buf = (char *)arena_alloc(arena, (size_t)len + 1, 1);
            if (buf) {
                vsnprintf(buf, (size_t)len + 1, fmt, ap);
                n->message.ptr = buf;
                n->message.len = (size_t)len;
            }
        }
    }

    /* 携带 diag 时，把同一错误记录进共享诊断缓冲区（由 driver 出口统一打印）。
       diag / tokens 为 NULL 时（如单测）仅构造节点，不影响解析结果。 */
    if (diag && n->message.ptr && tokens && tok_begin < vec_len(tokens)) {
        const token_t *tok = (const token_t *)vec_get(tokens, tok_begin);
        const location_t *loc = token_get_location(tok);
        location_t l = {0};
        if (loc) l = *loc;
        diag_error(diag, l, "%s", n->message.ptr);
    }

    return &n->base;
}

static inline ast_node_t *ast_error_new(diag_buf_t *diag, vec_t *tokens,
                                        arena_t *arena,
                                        uint32_t tok_begin, uint32_t tok_end,
                                        const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ast_node_t *result = ast_error_newv(diag, tokens, arena, tok_begin, tok_end, fmt, ap);
    va_end(ap);
    return result;
}

#ifdef __cplusplus
}
#endif
#endif
