#ifndef _H_CLUX_PARSER_AST_CALL_
#define _H_CLUX_PARSER_AST_CALL_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include <stdint.h>

typedef struct {
    ast_node_t  base;
    ast_node_t *callee;      /* 被调表达式（可以是任意 expr，如 a.b） */
    ast_node_t *args;        /* 实参兄弟链 */
    ast_node_t *args_last;   /* O(1) 追加 */
    /* ---- M6 FFI：extern cfunc 调用的返回类型码 + 变参信息（sema 填、compiler 读）----
     * callee 是 `<库>::<名>`（AST_ENUM_REF）时，sema 的 `::` 分支解析出该
     * extern cfunc 的签名，把**返回类型码与变参信息**打包进这里（避免 type_t*
     * 跨层传递）：
     *   bits 0..7   = BCODE_FFI_RET_*（返回类型码）
     *   bit  8      = 变参（C 的 `...`）
     *   bits 16..31 = 固定参数个数
     * 编解码宏见 `vm/bcode.h` 的 BCODE_FFI_RET_PACK/UNPACK；compiler 把它原样
     * 写成 `BCODE_EXTERN_CALL` 的第三个操作数。
     * `is_ffi_call` 由 sema 置位；compiler 据此改发 `BCODE_EXTERN_CALL`（见
     * docs/m6-design.md §14.7 步骤 ③/§14.19），不再走 CALLEE + CALL。 */
    bool        is_ffi_call;
    uint32_t    ffi_ret_kind;
    /* 返回类型是**程序自定义类型**（struct/cunion/enum…）时，sema 在这里挂上
     * 声明的返回类型**表达式**；compiler 发 EXTERN_CALL 之前先
     * `compile_type_expr(ffi_ret_type_expr)` 压一个**类型值**（与 CONSTRUCT 同一
     * 机制），运行期据此读返回内存并造值（ret_kind = BCODE_FFI_RET_TYPED）。 */
    ast_node_t *ffi_ret_type_expr;
    /* `data(s)` —— 从序列取首元素指针（C ABI 适配）。它不是运行时函数调用：
     * 内置 cfunc 的实参会被 clone 进被调作用域，函数形态只能拿到副本地址
     * （slice 缓冲区副本 / 数组副本块，调用一结束即悬垂）。因此 sema 置本标志，
     * compiler 改发 `BCODE_DATA` 指令（就地读序列的存储身份，与 `x.&` 同理）。 */
    bool        is_data_call;
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
