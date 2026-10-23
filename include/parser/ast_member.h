#ifndef _H_CLUX_PARSER_AST_MEMBER_
#define _H_CLUX_PARSER_AST_MEMBER_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"

typedef struct {
    ast_node_t  base;
    ast_node_t *object;     /* . 左边的表达式 */
    strslice_t  field;      /* . 右边的字段名 */
    bool        is_bitfield;
    uint32_t    bit_offset;
    uint32_t    bit_width;
    uint32_t    bit_word_bits;
    uint32_t    bit_value_type_id;
    uint32_t    bit_storage_type_id;
} ast_member_t;

static inline ast_node_t *ast_member_new(arena_t *arena,
                                         uint32_t tok_begin, uint32_t tok_end) {
    ast_member_t *n = (ast_member_t *)arena_calloc(
        arena, 1, sizeof(ast_member_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_MEMBER;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/**
 * 解析成员访问表达式（后缀 .field）。
 * 由 parse_postfix 内部调用，不直接对外。
 */
ast_node_t *parse_member(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif
