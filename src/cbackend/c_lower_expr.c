#include "cbackend/c_lower_internal.h"

#include <stdio.h>
#include <string.h>

static lower_method_t *find_method(lower_ctx_t *lc, uint32_t owner_id,
                                   strslice_t name) {
    for (lower_method_t *m = lc->methods; m; m = m->next) {
        if (m->owner_id == owner_id && m->name.len == name.len &&
            memcmp(m->name.ptr, name.ptr, name.len) == 0)
            return m;
    }
    return NULL;
}

cir_expr_t *lower_call(lower_ctx_t *lc, ast_call_t *call) {
    if (call->is_member_call && call->callee &&
        call->callee->kind == AST_MEMBER) {
        ast_member_t *mem = (ast_member_t *)call->callee;
        lower_method_t *m =
            find_method(lc, call->member_type_id, mem->field);
        if (!m) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                       "c backend: method '%.*s' not found",
                       (int)mem->field.len, mem->field.ptr);
            lc->failed = true;
            return NULL;
        }
        cir_expr_t *recv = lower_expr(lc, mem->object);
        if (!recv) return NULL;
        const type_t *rt = infer_expr_type(lc, mem->object);
        const type_t *self_t = NULL;
        ast_var_def_t *self_vd = (ast_var_def_t *)m->def->params;
        if (self_vd) {
            self_t = self_vd->resolved_type;
            if (!self_t && self_vd->type_expr)
                self_t = sema_resolve_type_slot(lc->sema, &self_vd->type_expr);
        }
        if (self_t && type_kind_of(type_strip_quals(self_t)) == TYPE_KIND_PTR &&
            (!rt || type_kind_of(type_strip_quals(rt)) != TYPE_KIND_PTR))
            recv = cir_expr_unary(lc->arena, "&", recv);
        cir_expr_t *out = cir_expr_call(lc->arena, m->cname, NULL);
        cir_expr_append_arg(out, cir_expr_ident(lc->arena, "NULL"));
        cir_expr_append_arg(out, recv);
        for (ast_node_t *a = call->args; a; a = a->next) {
            cir_expr_t *arg = lower_expr(lc, a);
            if (!arg) return NULL;
            cir_expr_append_arg(out, arg);
        }
        return out;
    }
    /* 函数值调用：callee 是 func 类型变量/表达式。 */
    if (call->callee && call->callee->kind == AST_IDENT) {
        const type_t *ft = lookup_local(
            lc, ((ast_ident_t *)call->callee)->name);
        if (ft && type_kind_of(type_strip_quals(ft)) == TYPE_KIND_FUNC) {
            cir_expr_t *callee = lower_expr(lc, call->callee);
            if (!callee) return NULL;
            cir_expr_t *out = cir_expr_func_call(lc->arena, callee, NULL);
            for (ast_node_t *a = call->args; a; a = a->next) {
                cir_expr_t *arg = lower_expr(lc, a);
                if (!arg) return NULL;
                cir_expr_append_arg(out, arg);
            }
            return out;
        }
    }
    strslice_t name = STRSLICE_EMPTY;
    if (call->callee && call->callee->kind == AST_IDENT) {
        name = ((ast_ident_t *)call->callee)->name;
    } else if (call->callee && call->callee->kind == AST_FUNC_REF) {
        ast_func_ref_t *ref = (ast_func_ref_t *)call->callee;
        name = ref->name;
        if (!name.ptr || name.len == 0) {
            sema_func_t *sf = sema_func_by_id(lc->sema, ref->fid);
            if (sf && sf->def && sf->def->kind == AST_FUNC_DEF)
                name = ((ast_func_def_t *)sf->def)->name;
        }
    }
    if (!name.ptr) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: only direct function calls are supported");
        lc->failed = true;
        return NULL;
    }
    const char *cname = mangle(lc->arena, "clux_fn_", name);
    if (!cname) return NULL;
    cir_expr_t *expr = cir_expr_call(lc->arena, cname, NULL);
    if (!expr) return NULL;
    /* env-first ABI：直接调用传 NULL。 */
    cir_expr_append_arg(expr, cir_expr_ident(lc->arena, "NULL"));
    for (ast_node_t *a = call->args; a; a = a->next) {
        cir_expr_t *arg = lower_expr(lc, a);
        if (!arg) return NULL;
        cir_expr_append_arg(expr, arg);
    }
    return expr;
}

cir_expr_t *lower_expr(lower_ctx_t *lc, ast_node_t *e) {
    if (!e || lc->failed) return NULL;
    switch (e->kind) {
    case AST_INT_LIT:
        return cir_expr_int(lc->arena, (int64_t)((ast_int_lit_t *)e)->value);
    case AST_FLOAT_LIT:
        return cir_expr_float(lc->arena, ((ast_float_lit_t *)e)->value);
    case AST_BOOL_LIT:
        return cir_expr_bool(lc->arena, ((ast_bool_lit_t *)e)->value);
    case AST_CHAR_LIT:
        return cir_expr_int(lc->arena, ((ast_char_lit_t *)e)->value);
    case AST_STRING_LIT: {
        ast_string_lit_t *s = (ast_string_lit_t *)e;
        if (!map_type(lc, lc->sema->vm->type_str)) return NULL;
        char *data = (char *)arena_alloc(lc->arena, s->text.len + 1, 1);
        if (!data) return NULL;
        if (s->text.len) memcpy(data, s->text.ptr, s->text.len);
        data[s->text.len] = '\0';
        return cir_expr_string(lc->arena, data, s->text.len);
    }
    case AST_IDENT: {
        strslice_t name = ((ast_ident_t *)e)->name;
        if (!lookup_local(lc, name)) {
            for (lower_capture_t *c = lc->captures; c; c = c->next)
                if (c->name.len == name.len &&
                    memcmp(c->name.ptr, name.ptr, name.len) == 0)
                    return cir_expr_ident(lc->arena, c->expr);
        }
        const char *n = mangle(lc->arena, "clux_v_", name);
        return n ? cir_expr_ident(lc->arena, n) : NULL;
    }
    case AST_UNARY: {
        ast_unary_t *u = (ast_unary_t *)e;
        const char *op = token_text(lc->arena, u->op);
        cir_expr_t *v = lower_expr(lc, u->operand);
        return op && v ? cir_expr_unary(lc->arena, op, v) : NULL;
    }
    case AST_BINARY: {
        ast_binary_t *b = (ast_binary_t *)e;
        const type_t *lt = infer_expr_type(lc, b->lhs);
        const type_t *rt = infer_expr_type(lc, b->rhs);
        if (token_is(b->op, "is")) {
            const type_t *ut = infer_expr_type(lc, b->lhs);
            if (!ut || type_kind_of(type_strip_quals(ut)) != TYPE_KIND_UNION) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: 'is' requires a union operand");
                lc->failed = true;
                return NULL;
            }
            int tag = -1;
            if (b->rhs->kind == AST_IDENT)
                tag = union_type_find_member(
                    type_strip_quals(ut), ((ast_ident_t *)b->rhs)->name);
            else if (b->rhs->kind == AST_INT_LIT)
                tag = (int)((ast_int_lit_t *)b->rhs)->value;
            if (tag < 0) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: unknown union tag");
                lc->failed = true;
                return NULL;
            }
            cir_expr_t *obj = lower_expr(lc, b->lhs);
            if (!obj) return NULL;
            return cir_expr_binary(lc->arena, "==",
                                   cir_expr_union_tag(lc->arena, obj),
                                   cir_expr_int(lc->arena, tag));
        }
        if ((b->lhs->kind == AST_NIL || b->rhs->kind == AST_NIL) &&
            (token_is(b->op, "==") || token_is(b->op, "!="))) {
            ast_node_t *other = b->lhs->kind == AST_NIL ? b->rhs : b->lhs;
            const type_t *ot = infer_expr_type(lc, other);
            if (!ot || type_kind_of(type_strip_quals(ot)) != TYPE_KIND_OPTION) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: nil comparison requires optional operand");
                lc->failed = true;
                return NULL;
            }
            cir_expr_t *obj = lower_expr(lc, other);
            return obj ? cir_expr_opt_is_some(lc->arena, obj,
                                              token_is(b->op, "!="))
                       : NULL;
        }
        if (lt && rt && type_strip_quals(lt)->kind == TYPE_KIND_STR &&
            type_strip_quals(rt)->kind == TYPE_KIND_STR &&
            (token_is(b->op, "==") || token_is(b->op, "!="))) {
            lc->unit->uses_str_cmp = true;
            cir_expr_t *l = lower_expr(lc, b->lhs);
            cir_expr_t *r = lower_expr(lc, b->rhs);
            if (!l || !r) return NULL;
            cir_expr_t *call = cir_expr_call(lc->arena, "clux_str_eq", NULL);
            cir_expr_append_arg(call, l);
            cir_expr_append_arg(call, r);
            if (token_is(b->op, "!="))
                call = cir_expr_unary(lc->arena, "!", call);
            return call;
        }
        const char *op = token_text(lc->arena, b->op);
        cir_expr_t *lhs = lower_expr(lc, b->lhs);
        cir_expr_t *rhs = lower_expr(lc, b->rhs);
        return op && lhs && rhs ? cir_expr_binary(lc->arena, op, lhs, rhs)
                                : NULL;
    }
    case AST_CALL:
        if (call_is_len((ast_call_t *)e)) return lower_len(lc, (ast_call_t *)e);
        return lower_call(lc, (ast_call_t *)e);
    case AST_IS_TAG: {
        ast_is_tag_t *it = (ast_is_tag_t *)e;
        cir_expr_t *obj = lower_expr(lc, it->object);
        if (!obj) return NULL;
        return cir_expr_binary(lc->arena, "==",
                               cir_expr_union_tag(lc->arena, obj),
                               cir_expr_int(lc->arena, it->tag));
    }
    case AST_OPT_IS_SOME: {
        ast_opt_is_some_t *os = (ast_opt_is_some_t *)e;
        cir_expr_t *obj = lower_expr(lc, os->object);
        return obj ? cir_expr_opt_is_some(lc->arena, obj, os->want_some)
                   : NULL;
    }
    case AST_UNWRAP: {
        ast_unwrap_t *uw = (ast_unwrap_t *)e;
        cir_expr_t *obj = lower_expr(lc, uw->operand);
        if (!obj) return NULL;
        lc->unit->uses_opt_unwrap = true;
        return cir_expr_unwrap(lc->arena, obj);
    }
    case AST_NIL:
        diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                   "c backend: nil requires an optional target type "
                   "(use .?T{nil})");
        lc->failed = true;
        return NULL;
    case AST_FUNC_REF: {
        ast_func_ref_t *ref = (ast_func_ref_t *)e;
        ast_func_def_t *fd = func_def_of_ref(lc, ref);
        strslice_t name = ref->name;
        if ((!name.ptr || name.len == 0) && fd) name = fd->name;
        if (!name.ptr) return NULL;
        const char *obj = mangle(lc->arena, "clux_obj_", name);
        const type_t *ft = func_type_of_def(lc, fd);
        cir_type_t *ct = ft ? map_type(lc, ft) : NULL;
        return obj && ct ? cir_expr_func_obj(lc->arena, obj, ct) : NULL;
    }
    case AST_FUNC_DEF:
        return lower_func_literal(lc, (ast_func_def_t *)e);
    case AST_SLICE:
        return lower_slice(lc, (ast_slice_t *)e);
    case AST_MAKE:
        return lower_make(lc, (ast_make_t *)e);
    case AST_MEMBER: {
        ast_member_t *m = (ast_member_t *)e;
        if (m->object && m->object->kind == AST_IDENT &&
            strslice_eq(((ast_ident_t *)m->object)->name,
                        STRSLICE_LIT("__ctx")) &&
            strslice_eq(m->field, STRSLICE_LIT("env")))
            return cir_expr_ident(lc->arena, "clux_v___ctx_env");
        cir_expr_t *obj = lower_expr(lc, m->object);
        if (!obj) return NULL;
        const type_t *ot = infer_expr_type(lc, m->object);
        if (ot) ot = type_strip_quals(ot);
        if (ot && type_kind_of(ot) == TYPE_KIND_UNION) {
            int mi = union_type_find_member(ot, m->field);
            const union_member_t *mem =
                mi >= 0 ? union_type_member(ot, (size_t)mi) : NULL;
            if (!mem) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: unknown union member '%.*s'",
                           (int)m->field.len, m->field.ptr);
                lc->failed = true;
                return NULL;
            }
            char fb[256];
            snprintf(fb, sizeof fb, "clux_m_%.*s", (int)mem->name.len,
                     mem->name.ptr ? mem->name.ptr : "");
            return cir_expr_union_member(lc->arena, obj,
                                         copy_arena(lc->arena, fb));
        }
        if (ot && type_kind_of(ot) == TYPE_KIND_CUNION) {
            int mi = cunion_type_find_member(ot, m->field);
            const cunion_member_t *mem =
                mi >= 0 ? cunion_type_member(ot, (size_t)mi) : NULL;
            if (!mem) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: unknown cunion member '%.*s'",
                           (int)m->field.len, m->field.ptr);
                lc->failed = true;
                return NULL;
            }
            char fb[256];
            snprintf(fb, sizeof fb, "clux_m_%.*s", (int)mem->name.len,
                     mem->name.ptr ? mem->name.ptr : "");
            return cir_expr_union_member(lc->arena, obj,
                                         copy_arena(lc->arena, fb));
        }
        char fname[256];
        snprintf(fname, sizeof fname, "clux_f_%.*s", (int)m->field.len,
                 m->field.ptr ? m->field.ptr : "");
        return cir_expr_member(lc->arena, obj,
                               copy_arena(lc->arena, fname));
    }
    case AST_INDEX: {
        ast_index_t *ix = (ast_index_t *)e;
        cir_expr_t *obj = lower_expr(lc, ix->object);
        cir_expr_t *idx = lower_expr(lc, ix->indices);
        return obj && idx ? cir_expr_index(lc->arena, obj, idx, true) : NULL;
    }
    case AST_DEREF: {
        ast_deref_t *d = (ast_deref_t *)e;
        cir_expr_t *v = lower_expr(lc, d->operand);
        return v ? cir_expr_unary(lc->arena, "*", v) : NULL;
    }
    case AST_ADDR: {
        ast_addr_t *a = (ast_addr_t *)e;
        cir_expr_t *v = lower_expr(lc, a->operand);
        return v ? cir_expr_unary(lc->arena, "&", v) : NULL;
    }
    case AST_CONSTRUCT: {
        ast_construct_t *c = (ast_construct_t *)e;
        if (!c->type) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                       "c backend: anonymous construction requires a type");
            lc->failed = true;
            return NULL;
        }
        const type_t *t = sema_resolve_type_slot(lc->sema, &c->type);
        if (t && type_kind_of(t) == TYPE_KIND_OPTION) {
            cir_type_t *ct = map_type(lc, t);
            if (!ct) return NULL;
            ast_node_t *fv = c->fields;
            if (fv && fv->kind == AST_CONSTRUCT_FIELD)
                fv = ((ast_construct_field_t *)fv)->value;
            if (!fv || fv->kind == AST_NIL)
                return cir_expr_option(lc->arena, ct, false, NULL);
            cir_expr_t *val = lower_expr(lc, fv);
            return val ? cir_expr_option(lc->arena, ct, true, val) : NULL;
        }
        if (t && type_kind_of(t) == TYPE_KIND_CUNION) {
            cir_type_t *ct = map_type(lc, t);
            const cunion_member_t *mem = cunion_type_member(t, c->member_tag);
            if (!ct || !mem) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: invalid cunion constructor");
                lc->failed = true;
                return NULL;
            }
            ast_node_t *fv = c->fields;
            if (fv && fv->kind == AST_CONSTRUCT_FIELD)
                fv = ((ast_construct_field_t *)fv)->value;
            cir_expr_t *val = lower_expr(lc, fv);
            if (!val) return NULL;
            char fb[256];
            snprintf(fb, sizeof fb, "clux_m_%.*s", (int)mem->name.len,
                     mem->name.ptr ? mem->name.ptr : "");
            return cir_expr_cunion_construct(lc->arena, ct,
                                             copy_arena(lc->arena, fb), val);
        }
        if (t && type_kind_of(t) == TYPE_KIND_UNION) {
            cir_type_t *ct = map_type(lc, t);
            const union_member_t *mem = union_type_member(t, c->member_tag);
            if (!ct || !mem) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: invalid union constructor");
                lc->failed = true;
                return NULL;
            }
            ast_node_t *fv = c->fields;
            if (fv && fv->kind == AST_CONSTRUCT_FIELD)
                fv = ((ast_construct_field_t *)fv)->value;
            cir_expr_t *val = lower_expr(lc, fv);
            if (!val) return NULL;
            char fb[256];
            snprintf(fb, sizeof fb, "clux_m_%.*s", (int)mem->name.len,
                     mem->name.ptr ? mem->name.ptr : "");
            return cir_expr_union_construct(lc->arena, ct, mem->tag,
                                            copy_arena(lc->arena, fb), val);
        }
        cir_type_t *ct = map_type(lc, t);
        if (!ct) return NULL;
        cir_expr_t *out = cir_expr_construct(lc->arena, ct, NULL);
        if (!out) return NULL;
        bool positional = (t && type_kind_of(t) != TYPE_KIND_STRUCT);
        for (ast_node_t *f = c->fields; f; f = f->next) {
            const char *fname = NULL;
            ast_node_t *val = f;
            if (f->kind == AST_CONSTRUCT_FIELD) {
                ast_construct_field_t *cf = (ast_construct_field_t *)f;
                val = cf->value;
                if (!positional) {
                    char fb[256];
                    snprintf(fb, sizeof fb, "clux_f_%.*s", (int)cf->name.len,
                             cf->name.ptr ? cf->name.ptr : "");
                    fname = copy_arena(lc->arena, fb);
                }
            }
            cir_expr_t *ve = lower_expr(lc, val);
            if (!ve) return NULL;
            cir_init_t *init = cir_init_new(lc->arena, fname, ve);
            if (!init) return NULL;
            cir_expr_add_init(out, init);
        }
        return out;
    }
    case AST_NEW: {
        ast_new_t *n = (ast_new_t *)e;
        if (!n->type) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                       "c backend: anonymous 'new' requires a type");
            lc->failed = true;
            return NULL;
        }
        const type_t *t = sema_resolve_type_slot(lc->sema, &n->type);
        cir_type_t *ct = map_type(lc, t);
        if (!ct) return NULL;
        lc->unit->uses_alloc = true;
        cir_expr_t *out = cir_expr_new(lc->arena, ct, NULL);
        if (!out) return NULL;
        bool positional = (t && type_kind_of(t) != TYPE_KIND_STRUCT);
        for (ast_node_t *f = n->fields; f; f = f->next) {
            const char *fname = NULL;
            ast_node_t *val = f;
            if (f->kind == AST_CONSTRUCT_FIELD) {
                ast_construct_field_t *cf = (ast_construct_field_t *)f;
                val = cf->value;
                if (!positional) {
                    char fb[256];
                    snprintf(fb, sizeof fb, "clux_f_%.*s", (int)cf->name.len,
                             cf->name.ptr ? cf->name.ptr : "");
                    fname = copy_arena(lc->arena, fb);
                }
            }
            cir_expr_t *ve = lower_expr(lc, val);
            if (!ve) return NULL;
            cir_init_t *init = cir_init_new(lc->arena, fname, ve);
            if (!init) return NULL;
            cir_expr_add_init(out, init);
        }
        return out;
    }
    case AST_ASSIGN: {
        ast_assign_t *a = (ast_assign_t *)e;
        cir_expr_t *target = lower_expr(lc, a->target);
        const char *op = a->op ? token_text(lc->arena, a->op) : "=";
        cir_expr_t *value = lower_expr(lc, a->value);
        return target && op && value
                   ? cir_expr_assign(lc->arena, target, op, value)
                   : NULL;
    }
    default:
        diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                   "c backend: unsupported expression");
        lc->failed = true;
        return NULL;
    }
}

