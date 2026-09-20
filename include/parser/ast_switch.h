#ifndef _H_CLUX_PARSER_AST_SWITCH_
#define _H_CLUX_PARSER_AST_SWITCH_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/**
 * switch 语句（docs m2-design §4：if 语法糖，语义内联在 sema/compiler）。
 *
 * 语法：switch(cond) { (a, b)->{...} (c)->{...} default->{...} }
 *   - 条件带括号，可以是运行时表达式（不限制常量）
 *   - 分支 (模式列表)->{块}：逗号分隔的模式列表 = || 链（惰性求值）
 *   - default 兜底；无 fallthrough
 *   - 无独立 switch 语义：sema 每模式 value_eq 校验 + 全分支 flow meet，
 *     compiler 发 cond 求值一次 + 每模式 EQ/JNZ 短路跳转（等价于文档
 *     desugar `var __switch_N = cond; if (...) {} else if ...`）
 */

/** 单个分支：模式列表 + 分支体 */
typedef struct {
    ast_node_t  base;        /* kind = AST_SWITCH_CASE */
    ast_node_t *patterns;    /* 模式表达式兄弟链（逗号分隔 = || 链） */
    ast_node_t *body;        /* AST_BLOCK */
} ast_switch_case_t;

static inline ast_node_t *ast_switch_case_new(arena_t *arena,
                                              uint32_t tok_begin,
                                              uint32_t tok_end) {
    ast_switch_case_t *n = (ast_switch_case_t *)arena_calloc(
        arena, 1, sizeof(ast_switch_case_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_SWITCH_CASE;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** switch 语句节点 */
typedef struct {
    ast_node_t  base;            /* kind = AST_SWITCH */
    ast_node_t *cond;            /* 条件表达式 */
    ast_node_t *cases;           /* AST_SWITCH_CASE 兄弟链（声明序） */
    ast_node_t *default_body;    /* AST_BLOCK 或 NULL（default 兜底） */
} ast_switch_t;

static inline ast_node_t *ast_switch_new(arena_t *arena,
                                         uint32_t tok_begin, uint32_t tok_end) {
    ast_switch_t *n = (ast_switch_t *)arena_calloc(
        arena, 1, sizeof(ast_switch_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_SWITCH;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析 switch 语句。不匹配返回 NULL，错误返回 AST_ERROR。 */
ast_node_t *parse_switch(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_SWITCH_ */
