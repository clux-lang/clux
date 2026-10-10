#ifndef _H_CLUX_PARSER_AST_NODE_
#define _H_CLUX_PARSER_AST_NODE_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/arena.h"
#include "parser/ast_kind.h"
#include <stdbool.h>
#include <stdint.h>

/* ================================================================ */
/* 公共头                                                             */
/* ================================================================ */

/**
 * 所有 AST 节点共享的公共头。
 *
 * 位置信息存储 token pool 下标（tok_begin / tok_end，共 8 字节），
 * 而非嵌入 location_t（~56 字节）。诊断时通过
 * vec_get(tokens, node->tok_begin)->location 取得完整源码位置。
 */
typedef struct ast_node {
    ast_kind_t       kind;        /* 节点种类 */
    uint32_t         tok_begin;   /* token pool 起始下标（inclusive） */
    uint32_t         tok_end;     /* token pool 结束下标（exclusive） */
    struct ast_node *parent;      /* 父节点（构建时填充） */
    struct ast_node *next;        /* 兄弟链（语句列表/参数/实参） */
    bool             is_exported;/* M5：export 前缀标记（全局声明进入模块导出表） */
} ast_node_t;

/* ================================================================ */
/* 工具函数                                                           */
/* ================================================================ */

/**
 * 追加 node 到 *head / *last 兄弟链尾，设置 node->parent。
 * 若 *head 为 NULL 则设为新节点；否则追加到 (*last)->next。
 */
void ast_append(ast_node_t **head, ast_node_t **last,
                ast_node_t *parent, ast_node_t *node);

/** 返回 kind 的人类可读名称。 */
const char *ast_kind_name(ast_kind_t kind);

/**
 * 创建无额外字段的基础节点（用于 AST_BREAK / AST_CONTINUE）。
 * 从 arena 分配，零初始化。
 */
static inline ast_node_t *ast_node_new(arena_t *arena, ast_kind_t kind,
                                       uint32_t tok_begin, uint32_t tok_end) {
    ast_node_t *n = (ast_node_t *)arena_calloc(
        arena, 1, sizeof(ast_node_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->kind      = kind;
    n->tok_begin = tok_begin;
    n->tok_end   = tok_end;
    return n;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_NODE_ */
