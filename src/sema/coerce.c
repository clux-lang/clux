#include "sema/coerce.h"

#include "parser/ast_binary.h"
#include "parser/ast_construct.h"
#include "parser/ast_option.h"
#include "sema/comptime.h"
#include "sema/magic.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"

#include <string.h>

/* `?P` 包装：把 slot 改写成 `.?P{<原表达式>}` 再求值。 */
static value_t *wrap_optional(sema_t *sema, ast_node_t **slot,
                              const type_t *inner, sema_scope_t *scope) {
    ast_node_t *arg = *slot;
    ast_node_t *tref = sema_ct_type_ref(sema, inner, arg);
    ast_node_t *optn =
        tref ? ast_option_new(sema->arena, arg->tok_begin, arg->tok_end) : NULL;
    ast_node_t *ctor =
        optn ? ast_construct_new(sema->arena, arg->tok_begin, arg->tok_end)
             : NULL;
    if (!optn || !ctor) return NULL;
    ((ast_option_t *)optn)->sub = tref;
    ((ast_construct_t *)ctor)->type = optn;
    ((ast_construct_t *)ctor)->fields = arg;
    ((ast_construct_t *)ctor)->fields_last = arg;
    ctor->is_generated = true;
    ctor->next = arg->next;
    arg->next = NULL;
    *slot = ctor;
    return sema_expr(sema, slot, scope);
}

/* 显式 cast：把 slot 改写成 `<原表达式> as want`。 */
static value_t *insert_cast(sema_t *sema, ast_node_t **slot, const type_t *want,
                            sema_scope_t *scope) {
    const token_t *tok = arena_token_symbol(sema->arena, "as", NULL);
    ast_node_t *arg = *slot;
    ast_node_t *tref = sema_gen_type_ref(sema, want);
    if (!tok || !tref) return NULL;
    ast_node_t *bin = ast_binary_new(sema->arena, arg->tok_begin, arg->tok_end);
    if (!bin) return NULL;
    ((ast_binary_t *)bin)->op = tok;
    ((ast_binary_t *)bin)->lhs = arg;
    ((ast_binary_t *)bin)->rhs = tref;
    bin->is_generated = true;
    bin->next = arg->next;
    arg->next = NULL;
    *slot = bin;
    return sema_expr(sema, slot, scope);
}

value_t *sema_coerce(sema_t *sema, ast_node_t **slot, value_t *v,
                     const type_t *want, sema_scope_t *scope) {
    if (!sema || !slot || !*slot || !v || !want) return v;
    if (value_is_error(sema->vm, v)) return v;
    const type_t *w = type_strip_quals(want);
    const type_t *at = value_type(v);
    at = at ? type_strip_quals(at) : NULL;
    if (!w || !at) return v;
    if (w == at) {
        /* 同类型通常无需转换；但带 `__init__` / `__init_copy__` 的 struct
           仍要走初始化派发（同类型"位置"要按 move-only 规则检查/移动）。 */
        bool magic_init = false;
        if (type_kind_of(w) == TYPE_KIND_STRUCT) {
            magic_init = sema_magic_member(sema, w, "__init__") ||
                         sema_magic_member(sema, w, "__init_copy__");
        }
        if (!magic_init) return v;
    }

    /* 1) `?P` 目标：nil 或 P 值都包成 `.?P{...}`。 */
    if (type_kind_of(w) == TYPE_KIND_OPTION) {
        if (type_kind_of(at) == TYPE_KIND_OPTION) return v; /* 已是 ?P */
        const type_t *inner = type_option_inner(w);
        if (!inner) return v;
        if ((*slot)->kind == AST_NIL) {
            value_t *nv = wrap_optional(sema, slot, inner, scope);
            return nv ? nv : v;
        }
        value_t *probe = value_make_shadow(sema->vm, at);
        if (!probe) return v;
        if (value_is_error(sema->vm,
                           value_implicit_cast(sema->vm, probe, inner)))
            return v; /* 不能提升：交给调用方诊断 */
        value_t *nv = wrap_optional(sema, slot, inner, scope);
        return nv ? nv : v;
    }

    /* 2) 结构体指针之间的隐式转换（派生 → 基类）需要显式 cast。 */
    if (type_is_ptr(w) && type_is_ptr(at)) {
        const type_t *wb = type_strip_quals(ptr_type_base(w));
        const type_t *ab = type_strip_quals(ptr_type_base(at));
        if (wb && ab && wb != ab && type_kind_of(wb) == TYPE_KIND_STRUCT &&
            type_kind_of(ab) == TYPE_KIND_STRUCT) {
            value_t *cv = insert_cast(sema, slot, w, scope);
            return cv ? cv : v;
        }
        return v;
    }

    /* 3) 单参 `__init__` 的隐式构造（`.T{v}` / `__init_copy__`）。 */
    {
        bool converted = false;
        value_t *nv =
            sema_magic_init_convert(sema, slot, at, want, scope, &converted);
        if (converted) return nv;
    }
    return v;
}
