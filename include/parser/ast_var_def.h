#ifndef _H_CLUX_PARSER_AST_VAR_DEF_
#define _H_CLUX_PARSER_AST_VAR_DEF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stdint.h>

/* 前置声明：解析层不依赖 vm 层头文件，但闭包捕获槽的销毁序列生成器
   （sema 的 gen_destroy）需要捕获槽的**实际类型**（type 对象是唯一权威）。 */
struct type_t;

typedef struct {
    ast_node_t  base;
    strslice_t  name;        /* 变量名 */
    ast_node_t *type_expr;   /* 类型表达式（NULL = 推断；AST_IDENT/AST_CONST/AST_VOLATILE） */
    ast_node_t *init;        /* 初始化表达式（必须存在；AST_UNDEF = 未初始化声明） */
    bool        is_comptime; /* comptime var：右值必须编译期可计算，引用点折叠为常量 */

    /* sema 写入：该变量/捕获槽解析后的**实际类型**（可为 NULL）。
     * 目前唯一消费者是闭包捕获槽的销毁序列（stmt.c make_free_stmts）——
     * 捕获槽类型不在 AST 里（推断捕获无 type_expr），而销毁序列必须按 type
     * 对象的槽位所有权展开（own *T → FREE、含 own 字段的聚合 → 逐槽位）。 */
    const struct type_t *resolved_type;

    /* 声明类型的**所有权**（own/ref/fatal，由解析器在类型槽位折叠前从
     * AST_PTR / AST_SLICE_TYPE 的 ownership token 提取并缓存）。
     *
     * 为什么需要缓存：`sema_resolve_type_slot` 会把 `AST_PTR` 折叠成
     * `AST_TYPE_REF`，**丢失 ownership token**；此后 `sema_type_expr_ownership`
     * 只会返回 OWN_NONE。变量定义路径在 Pass 2a 折叠前就地取值，所以不受影响；
     * 但 **extern 声明（`extern cfunc` / `extern var`）的参数与类型槽位在
     * Pass 1 就被折叠**，到 Pass 1c 复审 ABI 约束时已无从提取
     * （`own *T` 因此漏过 C ABI 检查）。这里缓存一份，供那些路径读取。
     *
     * 编码与 `ownership_t`（`vm/ownership.h`）一致：0=OWN_NONE、1=OWN_OWN、
     * 2=OWN_REF、3=OWN_FATAL。用 `uint8_t` 而非 `ownership_t` 是为了不让
     * parser 层头文件依赖 vm 层头文件。 */
    uint8_t     ownership;

    /* ---- M6 FFI：`extern var <库>::<名>: T;`（动态库导出的**全局数据符号**）----
     * 与 `extern cfunc` 对称、与 `extern lib` 配套：名字是库里的数据符号，
     * 运行期由 dlsym/GetProcAddress 得到**变量地址**，读写都作用于库的那块内存
     * （没有本地副本）。因此：
     *   - `init` 恒为 NULL（**没有初始化器**，语义是声明而非定义）；
     *   - 是**左值**：`crt::_fmode = 0;` 走 EXTERN_STORE（区别于只读的常量）。
     * `lib_name` 是 `extern lib` 的别名（与 ast_func_def_t 的同名字段一致）。 */
    bool        is_extern_var;
    strslice_t  lib_name;
} ast_var_def_t;

static inline ast_node_t *ast_var_def_new(arena_t *arena,
                                          uint32_t tok_begin, uint32_t tok_end) {
    ast_var_def_t *n = (ast_var_def_t *)arena_calloc(
        arena, 1, sizeof(ast_var_def_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_VAR_DEF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析变量定义 var name[:type] = init; 初始化必须存在。 */
ast_node_t *parse_var_def(parser_t *p);

/* ---- 所有权编码（与 vm 的 ownership_t 数值一致，避免 parser 依赖 vm 头）---- */
enum {
    AST_OWN_NONE  = 0,
    AST_OWN_OWN   = 1,
    AST_OWN_REF   = 2,
    AST_OWN_FATAL = 3,
};

/**
 * 从类型表达式提取顶层所有权编码（`own *T` / `ref []T` …）。
 *
 * **必须在类型槽位折叠前调用**（`sema_resolve_type_slot` 会把 AST_PTR /
 * AST_SLICE_TYPE 折叠成 AST_TYPE_REF，丢失 ownership token）。所以本函数由
 * **解析器**在构造变量定义/参数时调用，把结果缓存进 `ast_var_def_t::ownership`。
 *
 * 实现见 `src/parser/parse_var_def.c`（那里包含 token/类型节点头）。
 */
uint8_t ast_type_expr_ownership_code(const ast_node_t *type_expr);


/** 解析 `extern var <lib>::<name>: <T>;`（调用方已消费 `extern`）。
 * M6 FFI：声明动态库导出的全局数据符号；无初始化器，符号由 dlsym 在运行期解析。
 * 别名来自 `extern lib <lib> from "...";`（路径 `"*"` = 当前进程自身）。 */
ast_node_t *parse_extern_var(parser_t *p, uint32_t tb);

#ifdef __cplusplus
}
#endif
#endif
