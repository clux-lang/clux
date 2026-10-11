#ifndef _H_CLUX_PARSER_AST_CALL_
#define _H_CLUX_PARSER_AST_CALL_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"

typedef struct {
    ast_node_t  base;
    ast_node_t *callee;      /* 被调表达式（可以是任意 expr，如 a.b） */
    ast_node_t *args;        /* 实参兄弟链 */
    ast_node_t *args_last;   /* O(1) 追加 */
    /* ---- M6 FFI：extern cfunc 调用的返回值类别（sema 填充，compiler 读取）----
     * callee 是 `<库>::<名>`（AST_ENUM_REF）时，sema 的 `::` 分支会解析出该
     * extern cfunc 的签名；这里只留一个**数值类别**，避免 type_t* 跨层传递：
     *   0 = void、1 = i32、2 = i64/u64、3 = 指针/opaque、4 = f32/f64。
     * `is_ffi_call` 由 sema 置位；compiler 据此改发 `BCODE_EXTERN_CALL`（见
     * docs/m6-design.md §14.7 步骤 ③），不再走 CALLEE + CALL。 */
    bool        is_ffi_call;
    uint8_t     ffi_ret_kind;
} ast_call_t;

static inline ast_node_t *ast_call_new(arena_t *arena,
                                       uint32_t tok_begin, uint32_t tok_end) {
    ast_call_t *n = (ast_call_t *)arena_calloc(
        arena, 1, sizeof(ast_call_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_CALL;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif
