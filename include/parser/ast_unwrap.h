#ifndef _H_CLUX_PARSER_AST_UNWRAP_
#define _H_CLUX_PARSER_AST_UNWRAP_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/**
 * optional 解包节点（后缀 .! assert / .? try）。
 *
 * .! （内部名 assert）：a.! 解包 ?T → T，none 时运行期 panic。
 *    用户范式：if (a != nil) { var v = a.!; ... }——先判空再解包，
 *    解包失败（未判空）是编程错误，panic 终止。
 * .? （内部名 try）：a.? 仅词法预留（parser 组合识别 '.'+'?'，不做双字符
 *    token——与构造器 .?T{...} 前导冲突，见 lexer 注释），语义未实现
 *    ——sema 遇 .? 报 "not implemented"。
 *
 * 运行时真相：compiler 发 `compile_expr(operand)` + `BCODE_UNWRAP`——
 * UNWRAP 弹 ?T 值 → 借用返回 value 字段的借用引用（value_make_borrowed，
 * data 指向 option 值块内偏移，零拷贝）；ok=false（none）→ panic 错误值。
 *
 * op：解包运算符 token（".!" 或 ".?"）。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *operand;    /* . 左边的表达式 */
    token_t    *op;         /* 解包运算符 token（.! / .?） */
} ast_unwrap_t;

static inline ast_node_t *ast_unwrap_new(arena_t *arena,
                                         uint32_t tok_begin, uint32_t tok_end) {
    ast_unwrap_t *n = (ast_unwrap_t *)arena_calloc(
        arena, 1, sizeof(ast_unwrap_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_UNWRAP;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_UNWRAP_ */
