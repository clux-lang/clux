#include "cbackend/c_lower_internal.h"

#include <stdio.h>
#include <string.h>

static bool is_defer_name(strslice_t n) {
    return n.ptr && n.len >= 8 && memcmp(n.ptr, "__defer_", 8) == 0;
}

static ast_node_t *find_defer_body(lower_ctx_t *lc, strslice_t name) {
    for (lower_defer_t *d = lc->defers; d; d = d->next)
        if (d->name.len == name.len &&
            memcmp(d->name.ptr, name.ptr, name.len) == 0)
            return d->body;
    return NULL;
}

void register_local(lower_ctx_t *lc, strslice_t name,
                           const type_t *type) {
    if (!name.ptr || !type) return;
    lower_local_t *l = (lower_local_t *)arena_calloc(
        lc->arena, 1, sizeof(*l), ALIGNOF(max_align_t));
    if (!l) return;
    l->name = name;
    l->type = type;
    l->next = lc->locals;
    lc->locals = l;
}

const type_t *lookup_local(lower_ctx_t *lc, strslice_t name) {
    for (lower_local_t *l = lc->locals; l; l = l->next)
        if (l->name.len == name.len &&
            memcmp(l->name.ptr, name.ptr, name.len) == 0)
            return l->type;
    return NULL;
}

cir_stmt_t *lower_stmt(lower_ctx_t *lc, ast_node_t *s, bool *ok) {
    if (!s || lc->failed) return NULL;
    switch (s->kind) {
    case AST_FUNC_DEF: {
        ast_func_def_t *fn = (ast_func_def_t *)s;
        if (is_defer_name(fn->name)) {
            lower_defer_t *d = (lower_defer_t *)arena_calloc(
                lc->arena, 1, sizeof(*d), ALIGNOF(max_align_t));
            if (!d) return NULL;
            d->name = fn->name;
            d->body = fn->body;
            d->next = lc->defers;
            lc->defers = d;
            return NULL;
        }
        diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                   "c backend: local function definitions are not supported");
        *ok = false;
        return NULL;
    }
    case AST_VAR_DEF: {
        ast_var_def_t *vd = (ast_var_def_t *)s;
        const type_t *t = vd->resolved_type;
        if (!t && vd->type_expr) t = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        if (!t || type_kind_of(t) == TYPE_KIND_VOID)
            t = infer_expr_type(lc, vd->init);
        if (t) register_local(lc, vd->name, t);
        cir_type_t *ct = map_type(lc, t);
        if (!ct) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                       "c backend: cannot determine local variable type");
            *ok = false;
            return NULL;
        }
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_VAR);
        if (!st) return NULL;
        st->type = ct;
        st->name = mangle(lc->arena, "clux_v_", vd->name);
        if (vd->init && vd->init->kind != AST_UNDEF) {
            const type_t *it = infer_expr_type(lc, vd->init);
            if (vd->init->kind == AST_NIL && t &&
                type_kind_of(type_strip_quals(t)) == TYPE_KIND_OPTION) {
                st->expr = cir_expr_option(lc->arena, ct, false, NULL);
            } else if (t && type_kind_of(type_strip_quals(t)) == TYPE_KIND_OPTION &&
                       (!it || type_kind_of(type_strip_quals(it)) != TYPE_KIND_OPTION)) {
                cir_expr_t *v = lower_expr(lc, vd->init);
                st->expr = v ? cir_expr_option(lc->arena, ct, true, v) : NULL;
            } else {
                st->expr = lower_expr(lc, vd->init);
            }
        }
        return st;
    }
    case AST_ASSIGN: {
        ast_assign_t *a = (ast_assign_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_ASSIGN);
        if (!st) return NULL;
        st->lhs = lower_expr(lc, a->target);
        st->op = a->op ? token_text(lc->arena, a->op) : "=";
        const type_t *tt = infer_expr_type(lc, a->target);
        if (a->value && a->value->kind == AST_NIL && tt &&
            type_kind_of(type_strip_quals(tt)) == TYPE_KIND_OPTION) {
            cir_type_t *ot = map_type(lc, tt);
            st->expr = cir_expr_option(lc->arena, ot, false, NULL);
        } else if (tt && type_kind_of(type_strip_quals(tt)) == TYPE_KIND_OPTION) {
            const type_t *vt = infer_expr_type(lc, a->value);
            if (!vt || type_kind_of(type_strip_quals(vt)) != TYPE_KIND_OPTION) {
                cir_type_t *ot = map_type(lc, tt);
                cir_expr_t *v = lower_expr(lc, a->value);
                st->expr = v ? cir_expr_option(lc->arena, ot, true, v) : NULL;
            } else {
                st->expr = lower_expr(lc, a->value);
            }
        } else {
            st->expr = lower_expr(lc, a->value);
        }
        return st;
    }
    case AST_EXPR_STMT: {
        ast_expr_stmt_t *es = (ast_expr_stmt_t *)s;
        if (es->expr && es->expr->kind == AST_CALL) {
            ast_call_t *call = (ast_call_t *)es->expr;
            strslice_t name = STRSLICE_EMPTY;
            if (call->callee && call->callee->kind == AST_IDENT)
                name = ((ast_ident_t *)call->callee)->name;
            else if (call->callee && call->callee->kind == AST_FUNC_REF) {
                ast_func_ref_t *ref = (ast_func_ref_t *)call->callee;
                name = ref->name;
                if ((!name.ptr || name.len == 0) && lc->sema) {
                    sema_func_t *sf = sema_func_by_id(lc->sema, ref->fid);
                    if (sf && sf->def && sf->def->kind == AST_FUNC_DEF)
                        name = ((ast_func_def_t *)sf->def)->name;
                }
            }
            if (is_defer_name(name)) {
                ast_node_t *body = find_defer_body(lc, name);
                if (body) {
                    bool okd = true;
                    cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
                    if (!st) return NULL;
                    st->body = lower_block(lc, body, &okd);
                    if (!okd) *ok = false;
                    return st;
                }
            }
        }
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_EXPR);
        if (!st) return NULL;
        st->expr = lower_expr(lc, es->expr);
        return st;
    }
    case AST_RETURN: {
        ast_return_t *r = (ast_return_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_RETURN);
        if (!st) return NULL;
        if (r->value) {
            const type_t *rt = NULL;
            if (lc->return_type && lc->return_type->kind == CIR_TYPE_OPTION)
                rt = (const type_t *)lc->return_type->src;
            if (r->value->kind == AST_NIL && rt)
                st->expr = cir_expr_option(lc->arena, lc->return_type, false,
                                           NULL);
            else {
                const type_t *vt = infer_expr_type(lc, r->value);
                if (rt && (!vt || type_kind_of(type_strip_quals(vt)) !=
                                     TYPE_KIND_OPTION)) {
                    cir_expr_t *v = lower_expr(lc, r->value);
                    st->expr = v ? cir_expr_option(lc->arena,
                                                   lc->return_type, true, v)
                                 : NULL;
                } else {
                    st->expr = lower_expr(lc, r->value);
                }
            }
        }
        if (s->defers) {
            /* 先算 return 值到临时变量，再执行 defer/free，最后返回。 */
            cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
            if (!blk) return NULL;
            cir_stmt_t *tail = NULL;
            if (st->expr && lc->return_type &&
                lc->return_type->kind != CIR_TYPE_VOID) {
                char tmp[64];
                snprintf(tmp, sizeof tmp, "clux_ret_%u", lc->ret_seq++);
                cir_stmt_t *save = cir_stmt_new(lc->arena, CIR_STMT_VAR);
                if (!save) return NULL;
                save->type = lc->return_type;
                save->name = copy_arena(lc->arena, tmp);
                save->expr = st->expr;
                blk->body = save;
                tail = save;
                st->expr = cir_expr_ident(lc->arena, save->name);
            }
            bool okd = true;
            cir_stmt_t *clean = lower_ast_list(lc, s->defers, &okd);
            if (!okd) {
                *ok = false;
                return NULL;
            }
            if (clean) {
                if (tail) tail->next = clean;
                else blk->body = clean;
            }
            cir_stmt_t *last = blk->body;
            while (last && last->next) last = last->next;
            if (last) last->next = st;
            else blk->body = st;
            return blk;
        }
        return st;
    }
    case AST_BREAK: {
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_BREAK);
        if (!st) return NULL;
        if (s->defers) {
            cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
            if (!blk) return NULL;
            blk->body = lower_ast_list(lc, s->defers, ok);
            if (!*ok) return NULL;
            cir_stmt_t *t = blk->body;
            if (!t) blk->body = st;
            else {
                while (t->next) t = t->next;
                t->next = st;
            }
            return blk;
        }
        return st;
    }
    case AST_CONTINUE: {
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_CONTINUE);
        if (!st) return NULL;
        if (s->defers) {
            cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
            if (!blk) return NULL;
            blk->body = lower_ast_list(lc, s->defers, ok);
            if (!*ok) return NULL;
            cir_stmt_t *t = blk->body;
            if (!t) blk->body = st;
            else {
                while (t->next) t = t->next;
                t->next = st;
            }
            return blk;
        }
        return st;
    }
    case AST_BLOCK:
        return lower_block(lc, s, ok);
    case AST_IF: {
        ast_if_t *ifn = (ast_if_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_IF);
        if (!st) return NULL;
        st->expr = lower_expr(lc, ifn->cond);
        st->body = lower_block(lc, ifn->then_body, ok);
        if (ifn->else_body) {
            if (ifn->else_body->kind == AST_IF) {
                bool ok2 = true;
                cir_stmt_t *nested = lower_stmt(lc, ifn->else_body, &ok2);
                st->else_body = nested;
                if (!ok2) *ok = false;
            } else {
                st->else_body = lower_block(lc, ifn->else_body, ok);
            }
        }
        return st;
    }
    case AST_WHILE: {
        ast_while_t *w = (ast_while_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_WHILE);
        if (!st) return NULL;
        st->expr = lower_expr(lc, w->cond);
        st->body = lower_block(lc, w->body, ok);
        return st;
    }
    case AST_FOR: {
        ast_for_t *f = (ast_for_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_FOR);
        if (!st) return NULL;
        if (f->init) st->init = lower_stmt(lc, f->init, ok);
        if (f->cond) st->expr = lower_expr(lc, f->cond);
        if (f->update) st->update = lower_expr(lc, f->update);
        st->body = lower_block(lc, f->body, ok);
        return st;
    }
    case AST_DOWHILE: {
        ast_dowhile_t *d = (ast_dowhile_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_DOWHILE);
        if (!st) return NULL;
        st->expr = lower_expr(lc, d->cond);
        st->body = lower_block(lc, d->body, ok);
        return st;
    }
    case AST_FOR_INDEX: {
        ast_for_index_t *fi = (ast_for_index_t *)s;
        const ast_node_t *src = fi->source;
        const ast_node_t *base = src;
        bool deref = false;
        if (src && src->kind == AST_DEREF) {
            deref = true;
            base = ((ast_deref_t *)src)->operand;
        }
        if (!base || base->kind != AST_IDENT) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                       "c backend: generated index loop source must be an array variable (%s)",
                       src ? ast_kind_name(src->kind) : "-");
            *ok = false;
            return NULL;
        }
        const char *vn = mangle(lc->arena, "clux_v_", fi->var);
        const char *raw = mangle(lc->arena, "clux_v_",
                                 ((ast_ident_t *)base)->name);
        if (!vn || !raw) return NULL;
        char srcbuf[512];
        snprintf(srcbuf, sizeof srcbuf, deref ? "(*%s)" : "%s", raw);
        const char *an = copy_arena(lc->arena, srcbuf);
        const type_t *stype = lookup_local(lc, ((ast_ident_t *)base)->name);
        if (stype && deref && type_kind_of(type_strip_quals(stype)) == TYPE_KIND_PTR)
            stype = ptr_type_base(stype);
        bool slice_src = stype && type_kind_of(type_strip_quals(stype)) == TYPE_KIND_SLICE;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_FOR);
        if (!st) return NULL;
        cir_stmt_t *init = cir_stmt_new(lc->arena, CIR_STMT_VAR);
        if (!init) return NULL;
        init->type = cir_type_new(lc->arena, CIR_TYPE_USIZE, "size_t");
        init->name = vn;
        init->expr = cir_expr_int(lc->arena, 0);
        st->init = init;
        char lenbuf[512];
        if (slice_src)
            snprintf(lenbuf, sizeof lenbuf, "(%s).len", an);
        else
            snprintf(lenbuf, sizeof lenbuf,
                     "(sizeof((%s).data) / sizeof((%s).data[0]))", an, an);
        cir_expr_t *len = cir_expr_ident(lc->arena,
                                         copy_arena(lc->arena, lenbuf));
        st->expr = cir_expr_binary(lc->arena, "<",
                                   cir_expr_ident(lc->arena, vn), len);
        st->update = cir_expr_assign(
            lc->arena, cir_expr_ident(lc->arena, vn), "=",
            cir_expr_binary(lc->arena, "+", cir_expr_ident(lc->arena, vn),
                            cir_expr_int(lc->arena, 1)));
        st->body = lower_block(lc, fi->body, ok);
        return st;
    }
    case AST_DEFER: {
        ast_defer_t *d = (ast_defer_t *)s;
        ast_func_def_t *closure = (ast_func_def_t *)d->closure;
        bool okd = true;
        cir_stmt_t *body = closure ? lower_block(lc, closure->body, &okd) : NULL;
        if (!okd) {
            *ok = false;
            return NULL;
        }
        cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
        if (!blk) return NULL;
        blk->body = body;
        blk->next = lc->cleanup;
        lc->cleanup = blk;
        return NULL; /* defer 不产生当前位置语句 */
    }
    case AST_FREE: {
        ast_free_t *f = (ast_free_t *)s;
        lc->unit->uses_alloc = true;
        cir_stmt_t *head = NULL, *tail = NULL;
        for (ast_node_t *op = f->operand; op; op = op->next) {
            const type_t *ot = infer_expr_type(lc, op);
            cir_expr_t *ex = lower_expr(lc, op);
            if (!ex) {
                *ok = false;
                return head;
            }
            if (ot && type_kind_of(ot) == TYPE_KIND_SLICE)
                ex = cir_expr_slice_data(lc->arena, ex);
            else if (ot && type_kind_of(ot) == TYPE_KIND_FUNC)
                ex = cir_expr_closure_release(lc->arena, ex);
            cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_FREE);
            if (!st) return head;
            st->expr = ex;
            if (tail) tail->next = st;
            else head = st;
            tail = st;
        }
        return head;
    }
    case AST_EMPTY_STMT:
        return NULL;
    default:
        diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                   "c backend: unsupported statement (kind %d)",
                   (int)s->kind);
        *ok = false;
        return NULL;
    }
}

cir_stmt_t *lower_ast_list(lower_ctx_t *lc, ast_node_t *list, bool *ok) {
    cir_stmt_t *head = NULL, *tail = NULL;
    for (ast_node_t *s = list; s; s = s->next) {
        cir_stmt_t *st = lower_stmt(lc, s, ok);
        if (!*ok) return head;
        if (!st) continue;
        if (tail) tail->next = st;
        else head = st;
        tail = st;
    }
    return head;
}

cir_stmt_t *lower_block(lower_ctx_t *lc, ast_node_t *block, bool *ok) {    if (!block || block->kind != AST_BLOCK) {
        *ok = false;
        return NULL;
    }
    cir_stmt_t *head = NULL, *tail = NULL;
    for (ast_node_t *s = ((ast_block_t *)block)->stmts; s; s = s->next) {
        cir_stmt_t *st = lower_stmt(lc, s, ok);
        if (!*ok) return head;
        if (!st) continue;
        if (tail) tail->next = st;
        else head = st;
        tail = st;
    }
    return head;
}
