#ifndef _H_CLUX_PARSER_AST_FILL_
#define _H_CLUX_PARSER_AST_FILL_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"

/**
 * 值包节点：<v,N>（尖括号前导的"值 × 重复次数"批量初始化）。
 *
 * 仅存在于 CONSTRUCT 的匿名构造字段链中：.[N]T{<v,N>} 表示 N 个 v 铺满
 * 构造目标（数组）——与普通表达式位置的 <...>（元组类型，M2 预留）天然
 * 区别：parser 只在构造器字段链上下文识别 <v,N>，其他位置 '[' / '(' 之后
 * 的 < 归中缀比较或元组，不在此解析。
 *
 * v：填充值表达式；count：重复次数（编译期常量，sema 求值）。语义由
 * sema/compiler 消费：sema 展开类型检查（N 个元素全部为 v 类型），
 * compiler 展开为 N 份 v 的值（或运行时循环填充，见编译策略）。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *value;      /* 填充值表达式 v */
    ast_node_t *count;      /* 重复次数表达式 N（编译期常量） */
} ast_fill_t;

static inline ast_node_t *ast_fill_new(arena_t *arena,
                                       uint32_t tok_begin, uint32_t tok_end) {
    ast_fill_t *n = (ast_fill_t *)arena_calloc(
        arena, 1, sizeof(ast_fill_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_FILL;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_FILL_ */
