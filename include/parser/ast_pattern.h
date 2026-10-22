#ifndef _H_CLUX_PARSER_AST_PATTERN_
#define _H_CLUX_PARSER_AST_PATTERN_

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* M7 `extends` 结构约束 PATTERN（docs/m7-design.md §27–§29）。
 *
 * 与类型表达式共享大部分语法外形，但有三个专用通配符：
 *   ?        任意单类型（含 void）
 *   <?>      任意元数元组
 *   ?*?      任意**非 RC**所有权指针（own/ref/fatal）；RC 必须显式
 *            share *? / weak *?
 *
 * 节点种类（AST_PAT_*）：
 *   WILD / OR / AND / PTR / SLICE / ARRAY / TUPLE / NAME / FUNC / QUAL
 * 全部只在泛型参数表的 `extends` 右侧出现，sema 匹配器消费。
 * ========================================================================= */

/* 指针/切片的“所有权位置”模式。 */
typedef enum {
    PAT_OWN_NONE = 0, /* 裸 *（无所有权修饰） */
    PAT_OWN_ANY,      /* ?（own/ref/fatal 任一；不含 RC） */
    PAT_OWN_OWN,
    PAT_OWN_REF,
    PAT_OWN_FATAL,
    PAT_OWN_SHARE,    /* share *?（RC 必须显式） */
    PAT_OWN_WEAK,     /* weak *? */
} ast_pat_own_t;

typedef struct {
    ast_node_t base; /* kind = AST_PAT_WILD */
} ast_pat_wild_t;

typedef struct {
    ast_node_t  base; /* kind = AST_PAT_OR / AST_PAT_AND */
    ast_node_t *lhs;
    ast_node_t *rhs;
} ast_pat_logic_t;

typedef struct {
    ast_node_t   base; /* kind = AST_PAT_PTR */
    ast_pat_own_t own;
    ast_node_t  *base_type; /* 被指向类型的 PATTERN */
} ast_pat_ptr_t;

typedef struct {
    ast_node_t   base; /* kind = AST_PAT_SLICE */
    ast_pat_own_t own;
    ast_node_t  *elem_type; /* 元素类型的 PATTERN */
} ast_pat_slice_t;

typedef struct {
    ast_node_t base;    /* kind = AST_PAT_ARRAY */
    ast_node_t *length; /* AST_INT_LIT 或 NULL（? 长度通配） */
    ast_node_t *elem_type;
} ast_pat_array_t;

typedef struct {
    ast_node_t  base;      /* kind = AST_PAT_TUPLE */
    ast_node_t *elem_types;
    ast_node_t *elem_types_last;
    bool        any_arity; /* <?> */
} ast_pat_tuple_t;

typedef struct {
    ast_node_t base;  /* kind = AST_PAT_NAME */
    strslice_t name;
    ast_node_t *args; /* 泛型实参 PATTERN 链（可 NULL） */
    ast_node_t *args_last;
} ast_pat_name_t;

typedef struct {
    ast_node_t base;       /* kind = AST_PAT_FUNC */
    ast_node_t *params;    /* PATTERN 链 */
    ast_node_t *params_last;
    ast_node_t *return_pat; /* PATTERN（NULL = void? 不允许，统一用 ?） */
} ast_pat_func_t;

typedef struct {
    ast_node_t base;    /* kind = AST_PAT_QUAL */
    bool        is_volatile;
    ast_node_t *sub;
} ast_pat_qual_t;

static inline ast_node_t *ast_pat_node_new(arena_t *arena, ast_kind_t kind,
                                           uint32_t tb, uint32_t te,
                                           size_t size) {
    ast_node_t *n = (ast_node_t *)arena_calloc(arena, 1, size,
                                               ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->kind = kind;
    n->tok_begin = tb;
    n->tok_end = te;
    return n;
}

#define AST_PAT_NEW(arena, kind, tb, te, type)                                 \
    ((ast_node_t *)ast_pat_node_new((arena), (kind), (tb), (te), sizeof(type)))

static inline ast_node_t *ast_pat_wild_new(arena_t *a, uint32_t tb, uint32_t te) {
    return AST_PAT_NEW(a, AST_PAT_WILD, tb, te, ast_pat_wild_t);
}
static inline ast_node_t *ast_pat_logic_new(arena_t *a, ast_kind_t kind,
                                            uint32_t tb, uint32_t te) {
    return AST_PAT_NEW(a, kind, tb, te, ast_pat_logic_t);
}
static inline ast_node_t *ast_pat_ptr_new(arena_t *a, uint32_t tb, uint32_t te) {
    return AST_PAT_NEW(a, AST_PAT_PTR, tb, te, ast_pat_ptr_t);
}
static inline ast_node_t *ast_pat_slice_new(arena_t *a, uint32_t tb, uint32_t te) {
    return AST_PAT_NEW(a, AST_PAT_SLICE, tb, te, ast_pat_slice_t);
}
static inline ast_node_t *ast_pat_array_new(arena_t *a, uint32_t tb, uint32_t te) {
    return AST_PAT_NEW(a, AST_PAT_ARRAY, tb, te, ast_pat_array_t);
}
static inline ast_node_t *ast_pat_tuple_new(arena_t *a, uint32_t tb, uint32_t te) {
    return AST_PAT_NEW(a, AST_PAT_TUPLE, tb, te, ast_pat_tuple_t);
}
static inline ast_node_t *ast_pat_name_new(arena_t *a, uint32_t tb, uint32_t te) {
    return AST_PAT_NEW(a, AST_PAT_NAME, tb, te, ast_pat_name_t);
}
static inline ast_node_t *ast_pat_func_new(arena_t *a, uint32_t tb, uint32_t te) {
    return AST_PAT_NEW(a, AST_PAT_FUNC, tb, te, ast_pat_func_t);
}
static inline ast_node_t *ast_pat_qual_new(arena_t *a, uint32_t tb, uint32_t te) {
    return AST_PAT_NEW(a, AST_PAT_QUAL, tb, te, ast_pat_qual_t);
}

/** 解析一个 `extends` PATTERN（`|` 最低、`&` 较高、括号分组）。 */
ast_node_t *parse_pattern(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif
