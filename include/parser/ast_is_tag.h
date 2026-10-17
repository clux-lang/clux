#ifndef _H_CLUX_PARSER_AST_IS_TAG_
#define _H_CLUX_PARSER_AST_IS_TAG_

#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include <stdint.h>

/**
 * AST_IS_TAG：`<object> is <member>` 的**直接形态**（由 sema 生成）。
 *
 * 为什么不复用 `AST_BINARY`：`is` 运算符在 AST 里靠 `token_t *op` 识别，
 * 而 `token_t` 是 lexer 内部类型，sema 无法为生成的节点合成一个 "is" token。
 * 本节点把编译期就已知的 **member tag 值**直接带上，compiler 据此发
 * `IS_TAG <tag>`，零额外解析。
 *
 * 用途：own tag union 的销毁序列需要按 tag 条件释放
 * （`if (v is m) { FREE v.m }`），而销毁序列由 sema 在作用域退出点生成。
 * 不出现在用户源码里（用户仍写 `v is m`，走 AST_BINARY）。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *object;  /* 求值得 union 值的表达式（销毁场景：变量引用） */
    uint32_t    tag;     /* member 的 tag 编号（编译期确定） */
} ast_is_tag_t;

static inline ast_node_t *ast_is_tag_new(arena_t *arena,
                                         uint32_t tok_begin,
                                         uint32_t tok_end) {
    ast_is_tag_t *n = (ast_is_tag_t *)arena_calloc(
        arena, 1, sizeof(ast_is_tag_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_IS_TAG;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif

#endif /* _H_CLUX_PARSER_AST_IS_TAG_ */
