#ifndef _H_CLUX_PARSER_AST_SCOPE_REF_
#define _H_CLUX_PARSER_AST_SCOPE_REF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stdint.h>

/* 作用域限定引用节点：lhs::name
 *
 * 通用 `::` 限定访问节点，承载五种语义（sema 按上下文分派）：
 *   - 枚举 variant：Color::Red（type_expr 折叠为 AST_TYPE_REF，value 存底层整数）
 *   - 模块成员：mod::func（type_expr 是 AST_IDENT 解析到 SEMA_SYM_MODULE）
 *   - FFI 符号：libc::printf / libc::var（is_ffi_var / is_ffi_call 标记）
 *   - struct 静态成员：T::x / T::f（is_struct_attr 标记）
 *   - 跨模块 struct 成员：mod::Type::method（type_expr 是嵌套 AST_SCOPE_REF）
 *
 * 链式访问 a::b::c 由 parser 产出嵌套 AST_SCOPE_REF（lhs 也可以是
 * AST_SCOPE_REF），sema 递归处理。
 */
typedef struct {
    ast_node_t  base;        /* kind = AST_SCOPE_REF */
    ast_node_t *type_expr;   /* 左侧表达式（AST_IDENT / AST_TYPE_REF / 嵌套 AST_SCOPE_REF） */
    strslice_t  variant;     /* `::` 右侧的名字 */
    int64_t     value;       /* sema 折叠后的底层整数值 */

    /* ---- M6 FFI：`<库别名>::<数据符号>`（extern var 的引用）----
     * 与 ast_call_t.is_ffi_call/ffi_ret_kind 同一契约：sema 在 :: 分支识别出
     * "别名是 extern lib 且限定名符号是 extern var"时置这两个字段，compiler
     * 据此发 `EXTERN_LOAD`（读取）/ `EXTERN_STORE`（赋值目标），**不再**走枚举
     * variant 路径。ffi_var_kind 是 BCODE_FFI_DATA_* 宽度/类别码（见 vm/bcode.h）。
     * 置位时 type_expr 保持 AST_IDENT（sema 不把它折叠成类型）。 */
    bool        is_ffi_var;
    uint32_t    ffi_var_kind;
    /* 数据符号的类型是程序自定义类型（struct/cunion/enum）时，sema 在这里挂上
     * 声明的**类型表达式**；compiler 在读/写之前先 `compile_type_expr` 压一个
     * 类型值（ffi_var_kind = BCODE_FFI_DATA_TYPED），运行期按该类型的 size 读写。 */
    ast_node_t *ffi_var_type_expr;

    /* ---- struct 静态属性（attribute）引用 ----
     * `T::x`（读取）/ `T::x = v`（写入）：sema 在 :: 分支识别出 "T 是 struct
     * 类型且 x 是其静态属性"时置 is_struct_attr + struct_attr_type_id，compiler
     * 据此发 `LOAD_ATTR <type_id> <name>`（读取）/ `STORE_ATTR <type_id> <name>`
     * （赋值目标），**不再**走枚举 variant / FFI 路径。
     * type_expr 被 sema 折叠为 AST_TYPE_REF（类型引用）。 */
    bool        is_struct_attr;
    uint32_t    struct_attr_type_id; /* 属性所在的 struct 类型的 type_id */
} ast_scope_ref_t;

static inline ast_node_t *ast_scope_ref_new(arena_t *arena,
                                           uint32_t tok_begin, uint32_t tok_end) {
    ast_scope_ref_t *n = (ast_scope_ref_t *)arena_calloc(
        arena, 1, sizeof(ast_scope_ref_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_SCOPE_REF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_SCOPE_REF_ */
