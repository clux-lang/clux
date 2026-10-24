#include "cbackend/c_lower_internal.h"

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
    case AST_FUNC_REF:
        return func_type_of_def(lc, func_def_of_ref(lc, (ast_func_ref_t *)e));
    case AST_FUNC_DEF:
        return func_type_of_def(lc, (ast_func_def_t *)e);
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
        return NULL;
    }
    case AST_MEMBER: {
        ast_member_t *m = (ast_member_t *)e;
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
        const type_t *et = sema_resolve_type_slot(lc->sema, &m->elem_type);
        return et ? type_slice_intern(vm, et) : NULL;
    }
    case AST_CONSTRUCT: {
        ast_construct_t *c = (ast_construct_t *)e;
        if (!c->type) return NULL;
        return sema_resolve_type_slot(lc->sema, &c->type);
    }
    case AST_IS_TAG:
    case AST_OPT_IS_SOME:
        return vm->type_bool;
    case AST_UNWRAP: {
        const type_t *ot = infer_expr_type(lc, ((ast_unwrap_t *)e)->operand);
        return ot ? type_option_inner(ot) : NULL;
    }
    case AST_UNARY:
        return infer_expr_type(lc, ((ast_unary_t *)e)->operand);
    case AST_BINARY: {
        ast_binary_t *b = (ast_binary_t *)e;
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
        sema_func_t *sf = NULL;
        if (c->callee && c->callee->kind == AST_FUNC_REF)
            sf = sema_func_by_id(lc->sema, ((ast_func_ref_t *)c->callee)->fid);
        else if (c->callee && c->callee->kind == AST_IDENT) {
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

