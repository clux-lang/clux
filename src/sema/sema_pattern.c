#include "sema/sema_pattern.h"

#include "parser/ast_int_lit.h"
#include "parser/ast_pattern.h"
#include "sema/sema_cross.h"
#include "sema/sema_generic.h"
#include "sema/symbol.h"
#include "vm/type_array.h"
#include "vm/type_enum.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_ownership.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"

#include <string.h>

/* ===========================================================================
 * M7 `extends` PATTERN 匹配（设计 §27–§29）
 *
 * 只做结构对位，不捕获、不产生副作用。限定符/所有权/RC 都按**类型身份**
 * 严格匹配（§29.1）；struct 名称按字段表相等（§29.2）；enum/union 名义匹配
 * （§29.6）；函数签名严格同构（§29.5）。
 * ========================================================================= */

static uint64_t pat_ct_raw(const sema_ct_const_t *c) {
    if (!c || !c->type) return 0;
    if (c->type->name.len == 3 && c->type->name.ptr &&
        memcmp(c->type->name.ptr, "u64", 3) == 0)
        return c->u;
    switch (c->type->size) {
    case 1: return (uint8_t)c->i;
    case 2: return (uint16_t)c->i;
    case 4: return (uint32_t)c->i;
    default: return (uint64_t)c->i;
    }
}

static const type_t *pattern_name_type(sema_t *sema, strslice_t name) {    if (!sema || !name.ptr) return NULL;
    const type_t *t = type_lookup(sema->vm, name);
    if (t) return t;
    if (sema->global_scope) {
        sema_symbol_t *sym = sema_scope_find_local(sema->global_scope, name);
        if (sym && sym->kind == SEMA_SYM_TYPE) return sym->type;
    }
    return NULL;
}

static bool match_own(ast_pat_own_t own, const type_t *t, bool is_slice) {
    type_kind_t core_kind = type_kind_of(t); /* 穿透所有权 */
    if (is_slice) {
        if (core_kind != TYPE_KIND_SLICE) return false;
    } else {
        if (core_kind != TYPE_KIND_PTR) return false;
    }
    ownership_t o = type_ownership_of(t);
    switch (own) {
    case PAT_OWN_ANY:
        /* 任意非 RC 所有权指针：own / ref / fatal（§27 用户补充） */
        return o == OWN_OWN || o == OWN_REF || o == OWN_FATAL;
    case PAT_OWN_NONE:
        return o == OWN_NONE;
    case PAT_OWN_OWN:
        return o == OWN_OWN;
    case PAT_OWN_REF:
        return o == OWN_REF;
    case PAT_OWN_FATAL:
        return o == OWN_FATAL;
    case PAT_OWN_SHARE:
    case PAT_OWN_WEAK:
        return false;
    }
    return false;
}

static bool match_name(sema_t *sema, ast_pat_name_t *pn, const type_t *t);

static bool match_tuple(sema_t *sema, ast_pat_tuple_t *tp, const type_t *t) {
    const type_t *core = type_strip_ownership(t);
    if (!core || core->kind != TYPE_KIND_TUPLE) return false;
    size_t n = tuple_type_elem_count(core);
    if (tp->any_arity) return true;
    size_t np = 0;
    for (ast_node_t *e = tp->elem_types; e; e = e->next) np++;
    if (np != n) return false;
    size_t i = 0;
    for (ast_node_t *e = tp->elem_types; e; e = e->next, i++) {
        const tuple_elem_t *te = tuple_type_elem(core, i);
        if (!te || !sema_pattern_match(sema, e, te->type)) return false;
    }
    return true;
}

static bool match_func(sema_t *sema, ast_pat_func_t *fp, const type_t *t) {
    const type_t *core = type_strip_ownership(t);
    if (!core) return false;
    if (core->kind != TYPE_KIND_FUNC && core->kind != TYPE_KIND_CFUNC)
        return false;
    size_t np = 0;
    for (ast_node_t *p = fp->params; p; p = p->next) np++;
    if (np != func_type_param_count(core)) return false;
    size_t i = 0;
    for (ast_node_t *p = fp->params; p; p = p->next, i++) {
        if (!sema_pattern_match(sema, p, func_type_param(core, i))) return false;
    }
    return sema_pattern_match(sema, fp->return_pat, func_type_return(core));
}

/* PATTERN 的泛型实例 `Vec[?]`：actual 必须是同一泛型定义的实例，且类型实参
 * 逐位匹配（类型实参用 PATTERN 递归；值实参只支持字面量/通配）。 */
static bool match_generic_instance(sema_t *sema, ast_pat_name_t *pn,
                                   sema_symbol_t *gsym, const type_t *t) {
    const type_t *core = type_strip_quals(t);
    const sema_generic_instance_t *gi = NULL;
    for (sema_generic_instance_t *e = sema->generics.instances; e;
         e = e->next) {
        if (e->kind == SEMA_GEN_FUNC || e->def != gsym->ast || !e->type)
            continue;
        if (type_strip_quals(e->type) == core) {
            gi = e;
            break;
        }
    }
    if (!gi) return false;
    size_t np = 0;
    for (ast_node_t *a = pn->args; a; a = a->next) np++;
    if (np != gi->arg_count) return false;
    size_t i = 0;
    for (ast_node_t *a = pn->args; a; a = a->next, i++) {
        const sema_generic_arg_t *ga = &gi->args[i];
        if (ga->is_value) {
            if (a->kind == AST_PAT_WILD) continue;
            if (a->kind != AST_INT_LIT) return false;
            if (((ast_int_lit_t *)a)->value != pat_ct_raw(&ga->value)) return false;
        } else {
            if (!sema_pattern_match(sema, a, ga->type)) return false;
        }
    }
    return true;
}

/* interface 约束 `T extends I[...]`：actual 必须已实现同一 interface 实例，
 * PATTERN 的实参逐位匹配实现记录的类型实参（值实参只支持字面量）。 */
static bool match_interface(sema_t *sema, ast_pat_name_t *pn,
                            sema_symbol_t *iface_sym, const type_t *t) {
    const type_t *core = type_strip_quals(t);
    size_t np = 0;
    for (ast_node_t *a = pn->args; a; a = a->next) np++;
    for (sema_impl_t *im = sema->impls; im; im = im->next) {
        if (im->type != core || im->iface_def != iface_sym->ast) continue;
        if (im->arg_count != np) continue;
        size_t i = 0;
        bool ok = true;
        for (ast_node_t *a = pn->args; a && i < np; a = a->next, i++) {
            if (!sema_pattern_match(sema, a, im->args[i])) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

static bool match_name(sema_t *sema, ast_pat_name_t *pn, const type_t *t) {
    if (!pn || !t) return false;
    /* 约束环境：PATTERN 中引用被约束参数名（`IWriter[T]` 的 T）时，
       T 代表当前正在检查的类型实参。 */
    for (size_t i = 0; i < sema->pattern_env.count; i++) {
        if (strslice_eq(sema->pattern_env.names[i], pn->name))
            return type_equal(sema->vm, sema->pattern_env.types[i],
                              type_strip_quals(t));
    }
    sema_symbol_t *gsym = NULL;
    if (pn->qualifier.ptr && pn->qualifier.len) {
        sema_symbol_t *modsym =
            sema_scope_find_local(sema->global_scope, pn->qualifier);
        if (modsym && modsym->kind == SEMA_SYM_MODULE && modsym->module)
            gsym = sema_module_export_symbol(modsym->module, pn->name);
    } else if (sema->global_scope) {
        gsym = sema_scope_find_local(sema->global_scope, pn->name);
    }
    if (gsym && gsym->kind == SEMA_SYM_INTERFACE)
        return match_interface(sema, pn, gsym, t);
    if (pn->args) {
        if (!gsym || !gsym->is_generic) return false;
        return match_generic_instance(sema, pn, gsym, t);
    }
    /* 泛型名（无实参模式）：只要求是同一泛型定义的实例。 */
    if (gsym && gsym->is_generic) {
        const type_t *core = type_strip_quals(t);
        for (sema_generic_instance_t *e = sema->generics.instances; e;
             e = e->next) {
            if (e->kind != SEMA_GEN_FUNC && e->def == gsym->ast &&
                e->type && type_strip_quals(e->type) == core)
                return true;
        }
        return false;
    }
    const type_t *pt = pattern_name_type(sema, pn->name);
    if (!pt) return false;
    /* struct 按字段表相等（§29.2 鸭子类型）；enum/union/标量按类型身份。 */
    if (type_kind_of(pt) == TYPE_KIND_STRUCT &&
        type_kind_of(t) == TYPE_KIND_STRUCT)
        return struct_type_compatible(sema->vm, type_strip_quals(t),
                                      type_strip_quals(pt));
    return type_strip_quals(t) == pt;
}

bool sema_pattern_match(sema_t *sema, ast_node_t *pat, const type_t *t) {
    if (!sema || !pat || !t) return false;
    switch (pat->kind) {
    case AST_PAT_WILD:
        return true; /* ? 匹配任意类型（含 void，§29.3） */
    case AST_PAT_OR:
        return sema_pattern_match(sema, ((ast_pat_logic_t *)pat)->lhs, t) ||
               sema_pattern_match(sema, ((ast_pat_logic_t *)pat)->rhs, t);
    case AST_PAT_AND:
        return sema_pattern_match(sema, ((ast_pat_logic_t *)pat)->lhs, t) &&
               sema_pattern_match(sema, ((ast_pat_logic_t *)pat)->rhs, t);
    case AST_PAT_PTR: {
        ast_pat_ptr_t *p = (ast_pat_ptr_t *)pat;
        if (!match_own(p->own, t, false)) return false;
        return sema_pattern_match(sema, p->base_type, ptr_type_base(t));
    }
    case AST_PAT_SLICE: {
        ast_pat_slice_t *s = (ast_pat_slice_t *)pat;
        if (!match_own(s->own, t, true)) return false;
        return sema_pattern_match(sema, s->elem_type, slice_type_elem(t));
    }
    case AST_PAT_ARRAY: {
        ast_pat_array_t *a = (ast_pat_array_t *)pat;
        const type_t *core = type_strip_ownership(t);
        if (!core || core->kind != TYPE_KIND_ARRAY) return false;
        if (a->length) {
            if (a->length->kind != AST_INT_LIT) return false;
            if (((ast_int_lit_t *)a->length)->value != array_type_len(core))
                return false;
        }
        return sema_pattern_match(sema, a->elem_type, array_type_elem(core));
    }
    case AST_PAT_TUPLE:
        return match_tuple(sema, (ast_pat_tuple_t *)pat, t);
    case AST_PAT_NAME:
        return match_name(sema, (ast_pat_name_t *)pat, t);
    case AST_PAT_FUNC:
        return match_func(sema, (ast_pat_func_t *)pat, t);
    case AST_PAT_QUAL: {
        ast_pat_qual_t *q = (ast_pat_qual_t *)pat;
        if (q->is_volatile) {
            if (t->kind != TYPE_KIND_VOLATILE) return false;
            return sema_pattern_match(sema, q->sub,
                                      ((const volatile_type_t *)t)->sub);
        }
        if (t->kind != TYPE_KIND_CONST) return false;
        return sema_pattern_match(sema, q->sub, ((const const_type_t *)t)->sub);
    }
    default:
        return false;
    }
}
