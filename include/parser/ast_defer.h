#ifndef _H_CLUX_PARSER_AST_DEFER_
#define _H_CLUX_PARSER_AST_DEFER_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_func_def.h"
#include "parser/ast_node.h"
#include "parser/parser.h"

/**
 * defer 语句（延迟执行块）：
 *
 *   defer ['<a,b>] [|cap, (name: T = init)|] { body }
 *
 * 语义上是"在当前作用域里创建一个**带闭包的无参 void 函数值**，并把它压进
 * 该作用域的 defer 栈"：作用域退出（pop_scope）时在释放作用域内存**之前**
 * 把栈**逆序**弹出来逐个调用。
 *
 * 实现口径：**复用闭包节点**（ast_func_def_t）而不是另造一套捕获/函数体
 * 表示——
 *   - closure->captures / scope_annot_names / body 就是闭包那套字段
 *     （捕获可见性、'<a,b> 作用域标注、生命周期规则逐字沿用）；
 *   - closure->params 为 NULL（无参）、closure->return_expr 为 NULL
 *     （void，与"省略 :void"一致）；body 是 AST_BLOCK。
 *   - 该 closure 节点**不是** AST_FUNC_DEF 语句（不会被 sema/compiler 当
 *     局部函数提升/绑定名字）：它只经由 AST_DEFER 语句到达
 *     sema_check_func_literal 与 compile_expr（函数字面量同一条路径）。
 */
typedef struct {
    ast_node_t     base;
    ast_func_def_t *closure; /* 无参 void 闭包（AST_FUNC_DEF，见上） */
} ast_defer_t;

static inline ast_node_t *ast_defer_new(arena_t *arena,
                                        uint32_t tok_begin, uint32_t tok_end) {
    ast_defer_t *n = (ast_defer_t *)arena_calloc(
        arena, 1, sizeof(ast_defer_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_DEFER;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析 defer 语句（调用方已确认当前 token 是 defer 关键字）。 */
ast_node_t *parse_defer(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_DEFER_ */
