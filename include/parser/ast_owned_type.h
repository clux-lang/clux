#ifndef _H_CLUX_PARSER_AST_OWNED_TYPE_
#define _H_CLUX_PARSER_AST_OWNED_TYPE_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/lexer.h"
#include <stdbool.h>

/* 所有权限定的**非指针**类型（`fatal T`，m3-design §3.3）。
 *
 * 与 ast_ptr_t / ast_slice_type_t 的分工：
 *   - 指针：own|ref|fatal|share|weak *T（ast_ptr_t）
 *   - 切片：own|ref|fatal []T（ast_slice_type_t）
 *   - 其余类型：`fatal T`（本节点）——T 是 struct/tuple/array/option/union
 *     这类**可能内嵌 own 字段**的复合类型。语义是"该值的将亡值变体"：由
 *     move()/clone() 或返回 fatal T 的函数产出，必须被接收方接管（存进
 *     变量 / 作为参数被消费 / 继续返回）。
 *
 * 为什么需要独立节点而不是复用 AST_PTR：指针/切片的所有权是**类型的一部分**
 * （own *T 与 ref *T 是两种类型，各自有 intern 实例与构造操作数），而复合
 * 类型的 ownership 恒为 OWN_NONE，只有 fatal 一个例外——解析层必须把
 * "fatal 后跟 `*`/`[]`"（指针/切片）与"fatal 后跟复合类型名"区分开。
 *
 * ownership 为修饰关键字 token（目前只有 "fatal" 合法）；base_type 为被限定
 * 的类型表达式。 */
typedef struct {
    ast_node_t  base;
    token_t    *ownership;  /* 所有权修饰关键字 token（fatal） */
    ast_node_t *base_type;  /* 被限定的类型表达式 T */
} ast_owned_type_t;

static inline ast_node_t *ast_owned_type_new(arena_t *arena,
                                             uint32_t tok_begin,
                                             uint32_t tok_end) {
    ast_owned_type_t *n = (ast_owned_type_t *)arena_calloc(
        arena, 1, sizeof(ast_owned_type_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_OWNED_TYPE;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_OWNED_TYPE_ */
