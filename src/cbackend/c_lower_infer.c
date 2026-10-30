#include "cbackend/c_lower_internal.h"
#include "sema/struct_compose.h"

#include <stdio.h>
#include <string.h>

const type_t *infer_expr_type(lower_ctx_t *lc, ast_node_t *e) {
    if (!e) return NULL;
    vm_t *vm = lc->sema->vm;
    switch (e->kind) {
    case AST_INT_LIT: {
        ast_int_lit_t *n = (ast_int_lit_t *)e;
        if (strslice_eq(n->type, STRSLICE_LIT("i8"))) return vm->type_i8;
        if (strslice_eq(n->type, STRSLICE_LIT("i16"))) return vm->type_i16;
        if (strslice_eq(n->type, STRSLICE_LIT("i64"))) return vm->type_i64;
        if (strslice_eq(n->type, STRSLICE_LIT("u8"))) return vm->type_u8;
        if (strslice_eq(n->type, STRSLICE_LIT("u16"))) return vm->type_u16;
        if (strslice_eq(n->type, STRSLICE_LIT("u32"))) return vm->type_u32;
        if (strslice_eq(n->type, STRSLICE_LIT("u64"))) return vm->type_u64;
        if (strslice_eq(n->type, STRSLICE_LIT("usize"))) return vm->type_usize;
        return vm->type_i32;
    }
    case AST_BOOL_LIT:
        return vm->type_bool;
    case AST_FLOAT_LIT: {
        ast_float_lit_t *f = (ast_float_lit_t *)e;
        if (strslice_eq(f->type, STRSLICE_LIT("f32"))) return vm->type_f32;
        return vm->type_f64;
    }
    case AST_STRING_LIT:
        return vm->type_str;
    case AST_CHAR_LIT:
        return vm->type_u8;
    case AST_IDENT:
        return lookup_local(lc, ((ast_ident_t *)e)->name);
    case AST_SCOPE_REF: {
        /* `Color::Red`：sema 已把 type_expr 折叠成 AST_TYPE_REF，variant 的
           底层整数存在 value 里；类型就是该 enum 类型（C 侧用底层整数）。
           `lib::var`（extern var）：类型来自 extern var 登记表。 */
        ast_scope_ref_t *sr = (ast_scope_ref_t *)e;
        if (sr->is_ffi_var) return ffi_extern_var_type(lc, sr);
        if (sr->type_expr && sr->type_expr->kind == AST_TYPE_REF) {
            const type_t *t = resolve_type_expr(lc->sema, sr->type_expr);
            if (t && type_kind_of(t) == TYPE_KIND_ENUM) return t;
        }
        return NULL;
    }
    case AST_FUNC_REF:
        return func_type_of_def(lc, func_def_of_ref(lc, (ast_func_ref_t *)e));
    case AST_FUNC_DEF:
        /* 函数字面量/局部函数的值类型：有捕获 → 闭包类型（与
           lower_closure_literal 构造的 C 签名 struct 一致）。用
           func_type_of_def 会得到普通签名类型，把闭包字面量赋给推断变量时
           两个 sig struct 不兼容。 */
        return closure_type_of_def(lc, (ast_func_def_t *)e);
    case AST_TAKE:
        return infer_expr_type(lc, ((ast_take_t *)e)->operand);
    case AST_DEREF: {
        const type_t *pt = infer_expr_type(lc, ((ast_deref_t *)e)->operand);
        return pt && type_kind_of(pt) == TYPE_KIND_PTR ? ptr_type_base(pt) : NULL;
    }
    case AST_ADDR: {
        const type_t *bt = infer_expr_type(lc, ((ast_addr_t *)e)->operand);
        return bt ? type_ptr_intern(lc->sema->vm, bt) : NULL;
    }
    case AST_INDEX: {
        const type_t *bt = infer_expr_type(lc, ((ast_index_t *)e)->object);
        if (!bt) return NULL;
        bt = type_strip_quals(bt);
        if (type_kind_of(bt) == TYPE_KIND_ARRAY)
            return ((const array_type_t *)bt)->elem_type;
        if (type_kind_of(bt) == TYPE_KIND_SLICE)
            return slice_type_elem(bt);
        if (type_kind_of(bt) == TYPE_KIND_TUPLE) {
            int64_t ti = 0;
            ast_index_t *ix = (ast_index_t *)e;
            if (ix->indices && ix->indices->kind == AST_INT_LIT)
                ti = ((ast_int_lit_t *)ix->indices)->value;
            const tuple_elem_t *te = tuple_type_elem(bt, (size_t)ti);
            return te ? te->type : NULL;
        }
        return NULL;
    }
    case AST_MEMBER: {
        /* `__ctx.env.<cap>`：捕获字段的类型来自闭包捕获表。 */
        {
            ast_member_t *cm = (ast_member_t *)e;
            if (cm->object && cm->object->kind == AST_MEMBER) {
                ast_member_t *om = (ast_member_t *)cm->object;
                if (om->object && om->object->kind == AST_IDENT &&
                    strslice_eq(((ast_ident_t *)om->object)->name,
                                STRSLICE_LIT("__ctx")) &&
                    strslice_eq(om->field, STRSLICE_LIT("env"))) {
                    for (lower_capture_t *c = lc->captures; c; c = c->next)
                        if (c->name.len == cm->field.len &&
                            memcmp(c->name.ptr, cm->field.ptr,
                                   c->name.len) == 0)
                            return c->type;
                }
            }
        }
        ast_member_t *m = (ast_member_t *)e;
        if (m->is_bitfield) return bitfield_value_type(lc, m->bit_value_type_id);
        const type_t *ot = infer_expr_type(lc, m->object);
        if (!ot) return NULL;
        ot = type_strip_quals(ot);
        if (type_kind_of(ot) == TYPE_KIND_STRUCT) {
            size_t n = struct_type_field_count(ot);
            for (size_t i = 0; i < n; i++) {
                const struct_field_t *f = struct_type_field(ot, i);
                if (f && f->name.len == m->field.len &&
                    memcmp(f->name.ptr, m->field.ptr, f->name.len) == 0)
                    return f->type;
            }
        }
        if (type_kind_of(ot) == TYPE_KIND_UNION) {
            int mi = union_type_find_member(ot, m->field);
            const union_member_t *mem =
                mi >= 0 ? union_type_member(ot, (size_t)mi) : NULL;
            return mem ? mem->payload_type : NULL;
        }
        if (type_kind_of(ot) == TYPE_KIND_CUNION) {
            int mi = cunion_type_find_member(ot, m->field);
            const cunion_member_t *mem =
                mi >= 0 ? cunion_type_member(ot, (size_t)mi) : NULL;
            return mem ? mem->type : NULL;
        }
        return NULL;
    }
    case AST_SLICE: {
        ast_slice_t *s = (ast_slice_t *)e;
        const type_t *ot = infer_expr_type(lc, s->object);
        if (!ot) return NULL;
        ot = type_strip_quals(ot);
        if (type_kind_of(ot) == TYPE_KIND_ARRAY)
            return type_slice_intern(vm, ((const array_type_t *)ot)->elem_type);
        if (type_kind_of(ot) == TYPE_KIND_SLICE) return ot;
        if (type_kind_of(ot) == TYPE_KIND_STR)
            return type_slice_intern(vm, type_const_intern(vm, vm->type_u8));
        return NULL;    }
    case AST_MAKE: {
        ast_make_t *m = (ast_make_t *)e;
        const type_t *pt = infer_expr_type(lc, m->pointer);
        pt = pt ? type_strip_quals(pt) : NULL;
        if (!pt || type_kind_of(pt) != TYPE_KIND_PTR) return NULL;
        return type_slice_intern(vm, ptr_type_base(pt));
    }
    case AST_CONSTRUCT: {
        ast_construct_t *c = (ast_construct_t *)e;
        if (!c->type) return NULL;
        return sema_resolve_type_slot(lc->sema, &c->type);
    }
    case AST_TERNARY: {
        ast_ternary_t *t = (ast_ternary_t *)e;
        const type_t *tt = infer_expr_type(lc, t->then_branch);
        return tt ? tt : infer_expr_type(lc, t->else_branch);
    }
    case AST_UNWRAP: {
        const type_t *ot = infer_expr_type(lc, ((ast_unwrap_t *)e)->operand);
        return ot ? type_option_inner(ot) : NULL;
    }
    case AST_UNARY:
        return infer_expr_type(lc, ((ast_unary_t *)e)->operand);
    case AST_BINARY: {
        ast_binary_t *b = (ast_binary_t *)e;
        if (token_is(b->op, "as")) {
            const type_t *target = sema_resolve_type_slot(lc->sema, &b->rhs);
            if (target) return target;
        }
        if (token_is(b->op, "is")) return vm->type_bool;
        if (token_is(b->op, "==") || token_is(b->op, "!=") ||
            token_is(b->op, "<") || token_is(b->op, "<=") ||
            token_is(b->op, ">") || token_is(b->op, ">=") ||
            token_is(b->op, "&&") || token_is(b->op, "||"))
            return vm->type_bool;
        const type_t *lt = infer_expr_type(lc, b->lhs);
        return lt ? lt : infer_expr_type(lc, b->rhs);
    }
    case AST_CALL: {
        ast_call_t *c = (ast_call_t *)e;
        if (call_is_len(c)) return vm->type_usize;
        if (c->callee && c->callee->kind == AST_MEMBER) {
            ast_member_t *m = (ast_member_t *)c->callee;
            lower_method_t *meth =
                find_method(lc, c->member_type_id, m->field);
            if (meth && meth->def->return_expr)
                return sema_resolve_type_slot(lc->sema,
                                              &meth->def->return_expr);
        }
        if (c->callee && c->callee->kind == AST_SCOPE_REF) {
            ast_scope_ref_t *sr = (ast_scope_ref_t *)c->callee;
            const type_t *owner_t = lower_scope_ref_owner_type(lc, sr);
            uint32_t owner = c->member_type_id;
            if (!owner && owner_t) {
                const sema_type_t *st = sema_type_find(lc->sema, owner_t);
                if (st) owner = st->id;
            }
            lower_method_t *meth = find_method(lc, owner, sr->variant);
            if (!meth && owner_t) {
                const struct_type_t *fo = NULL;
                sema_symbol_t *ms = sema_struct_find_member(
                    lc->sema, owner_t, sr->variant, c->callee, &fo);
                if (ms && ms->type &&
                    type_kind_of(type_strip_quals(ms->type)) ==
                        TYPE_KIND_FUNC) {
                    const type_t *rt2 = func_type_return(ms->type);
                    if (rt2) return rt2;
                }
            }
            if (meth && meth->def->return_expr)
                return sema_resolve_type_slot(lc->sema,
                                              &meth->def->return_expr);
        }
        sema_func_t *sf = NULL;
        if (c->callee && c->callee->kind == AST_FUNC_REF)
            sf = sema_func_by_id(lc->sema, ((ast_func_ref_t *)c->callee)->fid);
        else if (c->callee && c->callee->kind == AST_IDENT) {
            /* 局部函数值变量（`var f = add; f(...)`）：返回签名返回值。 */
            const type_t *ft =
                lookup_local(lc, ((ast_ident_t *)c->callee)->name);
            if (ft && type_kind_of(type_strip_quals(ft)) == TYPE_KIND_FUNC)
                return func_type_return(ft);
            sema_symbol_t *sym = sema_lookup(
                lc->sema->global_scope, ((ast_ident_t *)c->callee)->name);
            if (sym && sym->ast && sym->ast->kind == AST_FUNC_DEF) {
                ast_func_def_t *fd = (ast_func_def_t *)sym->ast;
                if (fd->return_expr)
                    return sema_resolve_type_slot(lc->sema, &fd->return_expr);
            }
        }
        if (sf && sf->def && sf->def->kind == AST_FUNC_DEF) {
            ast_func_def_t *fd = (ast_func_def_t *)sf->def;
            if (fd->return_expr)
                return sema_resolve_type_slot(lc->sema, &fd->return_expr);
        }
        return NULL;
    }
    default:
        return NULL;
    }
}

