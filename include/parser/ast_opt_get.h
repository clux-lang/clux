#ifndef _H_CLUX_PARSER_AST_OPT_GET_
#define _H_CLUX_PARSER_AST_OPT_GET_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"

/**
 * 窄化 SOME 读取节点（sema 生成，parser 不产生）。
 *
 * 路径窄化（docs m2-design §12.4）：if (x != nil) 的 SOME 分支内，x 已被
 * 判定为非 none——此时 x 的读取应当退化为内部类型 T（?T → T）。sema 在
 * 识别窄化后把 SOME 分支内的 AST_IDENT(x) 重写为 AST_OPT_GET（携带符号名），
 * compiler 发 `PUSH <name>` + `BCODE_OPT_GET`（无操作数）。
 *
 * 运行时真相：OPT_GET 弹 ?T 值 → 借用返回 value 字段的借用引用
 * （value_make_borrowed，data 指向 option 值块内偏移，零拷贝），与
 * is_own 借用字段同构——SOME 分支内 ok 恒为 true，读取安全。
 *
 * name：被窄化符号的名字（strtable 键，与 AST_IDENT 一致）。
 */
typedef struct {
    ast_node_t  base;
    strslice_t  name;        /* 被窄化 ?T 符号名 */
} ast_opt_get_t;

static inline ast_node_t *ast_opt_get_new(arena_t *arena,
                                          uint32_t tok_begin, uint32_t tok_end) {
    ast_opt_get_t *n = (ast_opt_get_t *)arena_calloc(
        arena, 1, sizeof(ast_opt_get_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_OPT_GET;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_OPT_GET_ */
