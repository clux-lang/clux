#include "cbackend/c_lower_internal.h"

#include <stdio.h>
#include <string.h>

bool call_is_len(const ast_call_t *c) {
    if (!c || !c->callee) return false;
    if (c->callee->kind == AST_IDENT)
        return strslice_eq(((const ast_ident_t *)c->callee)->name,
                           STRSLICE_LIT("len"));
    if (c->callee->kind == AST_FUNC_REF) {
        const ast_func_ref_t *r = (const ast_func_ref_t *)c->callee;
        if (strslice_eq(r->name, STRSLICE_LIT("len"))) return true;
    }
    return false;
}

cir_expr_t *lower_len(lower_ctx_t *lc, ast_call_t *call) {
    if (!call->args || call->args->next) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: len() expects one argument");
        lc->failed = true;
        return NULL;
    }
    ast_node_t *arg = call->args;
    const type_t *ot = infer_expr_type(lc, arg);
    if (!ot) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, arg),
                   "c backend: cannot determine len() operand type");
        lc->failed = true;
        return NULL;
    }
    cir_expr_t *obj = lower_expr(lc, arg);
    cir_type_t *ct = map_type(lc, ot);
    return obj && ct ? cir_expr_len(lc->arena, obj, ct) : NULL;
}

cir_expr_t *lower_slice(lower_ctx_t *lc, ast_slice_t *sl) {
    const type_t *ot = infer_expr_type(lc, sl->object);
    if (!ot) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)sl),
                   "c backend: cannot determine slice base type");
        lc->failed = true;
        return NULL;
    }
    ot = type_strip_quals(ot);
    const type_t *st = NULL;
    if (type_kind_of(ot) == TYPE_KIND_ARRAY)
        st = type_slice_intern(lc->sema->vm,
                               ((const array_type_t *)ot)->elem_type);
    else if (type_kind_of(ot) == TYPE_KIND_SLICE)
        st = ot;
    else if (type_kind_of(ot) == TYPE_KIND_STR)
        st = type_slice_intern(lc->sema->vm,
                               type_const_intern(lc->sema->vm,
                                                 lc->sema->vm->type_u8));
    if (!st) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)sl),
                   "c backend: unsupported slice base type");
        lc->failed = true;
        return NULL;
    }
    cir_expr_t *obj = lower_expr(lc, sl->object);
    cir_expr_t *lo = sl->low ? lower_expr(lc, sl->low) : NULL;
    cir_expr_t *hi = sl->high ? lower_expr(lc, sl->high) : NULL;
    cir_type_t *ct = map_type(lc, st);
    cir_type_t *otc = map_type(lc, ot);
    if (!obj || !ct || !otc) return NULL;
    cir_expr_t *e = cir_expr_slice(lc->arena, ct, obj, lo, hi);
    if (e) e->src_type = otc;
    return e;
}

cir_expr_t *lower_make(lower_ctx_t *lc, ast_make_t *mk) {
    const type_t *et = sema_resolve_type_slot(lc->sema, &mk->elem_type);
    if (!et) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)mk),
                   "c backend: cannot resolve make() element type");
        lc->failed = true;
        return NULL;
    }
    const type_t *st = type_slice_intern(lc->sema->vm, et);
    cir_type_t *sct = map_type(lc, st);
    cir_type_t *ect = map_type(lc, et);
    cir_expr_t *len = lower_expr(lc, mk->length);
    if (!sct || !ect || !len) return NULL;
    lc->unit->uses_alloc = true;
    cir_expr_t *out = cir_expr_make(lc->arena, sct, ect, len, NULL);
    if (!out) return NULL;
    for (ast_node_t *it = mk->inits; it; it = it->next) {
        if (it->kind == AST_FILL) {
            ast_fill_t *fill = (ast_fill_t *)it;
            if (fill->count->kind != AST_INT_LIT) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, it),
                           "c backend: make() fill count must be constant");
                lc->failed = true;
                return NULL;
            }
            uint64_t n = ((ast_int_lit_t *)fill->count)->value;
            for (uint64_t i = 0; i < n; i++) {
                cir_expr_t *v = lower_expr(lc, fill->value);
                if (!v) return NULL;
                cir_expr_add_init(out, cir_init_new(lc->arena, NULL, v));
            }
        } else {
            cir_expr_t *v = lower_expr(lc, it);
            if (!v) return NULL;
            cir_expr_add_init(out, cir_init_new(lc->arena, NULL, v));
        }
    }
    return out;
}

static const char *capture_or_var(lower_ctx_t *lc, strslice_t name) {
    for (lower_capture_t *c = lc->captures; c; c = c->next)
        if (c->name.len == name.len &&
            memcmp(c->name.ptr, name.ptr, name.len) == 0)
            return c->expr;
    return module_symbol(lc->arena, lc->unit, "v_", name);
}

cir_expr_t *lower_closure_literal(lower_ctx_t *lc, ast_func_def_t *fn) {
    lower_local_t *save_locals = lc->locals;
    lower_local_fn_t *save_local_fns = lc->local_fns;
    lower_capture_t *save_caps = lc->captures;
    unsigned seq = lc->fnlit_seq++;
    char nb[64];
    snprintf(nb, sizeof nb, "__closure_%u", seq);
    size_t n = strlen(nb);
    char *name = (char *)arena_alloc(lc->arena, n + 1, 1);
    if (!name) return NULL;
    memcpy(name, nb, n + 1);
    fn->name = strslice_from_bytes(name, n);

    char envtag[64];
    snprintf(envtag, sizeof envtag, "struct clux_env_%u", seq);
    cir_type_t *env = cir_type_new(lc->arena, CIR_TYPE_STRUCT,
                                   copy_arena(lc->arena, envtag));
    if (!env) return NULL;
    cir_unit_add_type(lc->unit, env);
    lc->unit->uses_alloc = true;

    cir_init_t *inits = NULL;
    for (ast_node_t *c = fn->captures; c; c = c->next) {
        ast_var_def_t *vd = (ast_var_def_t *)c;
        const type_t *t = vd->resolved_type;
        if (!t) t = lookup_local(lc, vd->name);
        if (!t && vd->type_expr)
            t = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        if (!t) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, c),
                       "c backend: cannot determine closure capture type");
            lc->failed = true;
            return NULL;
        }
        cir_type_t *ct = map_type(lc, t);
        if (!ct) return NULL;
        char fb[256];
        snprintf(fb, sizeof fb, "clux_f_%.*s", (int)vd->name.len,
                 vd->name.ptr ? vd->name.ptr : "");
        cir_field_t *f = cir_field_new(lc->arena,
                                       copy_arena(lc->arena, fb), ct);
        if (!f) return NULL;
        cir_type_add_field(env, f);
        const char *val = capture_or_var(lc, vd->name);
        cir_expr_t *ve = cir_expr_ident(lc->arena, val);
        cir_init_t *init = cir_init_new(lc->arena, f->name, ve);
        if (!init) return NULL;
        init->next = inits;
        inits = init;
    }
    cir_expr_t *env_new = cir_expr_new(lc->arena, env, NULL);
    env_new->closure_env = true;
    if (fn->env_dispose_fid) {
        sema_func_t *sf = sema_func_by_id(lc->sema, fn->env_dispose_fid);
        if (sf && sf->def && sf->def->kind == AST_FUNC_DEF) {
            ast_func_def_t *df = (ast_func_def_t *)sf->def;
            env_new->op =
                module_symbol(lc->arena, lc->unit, "fn_", df->name);
        }
    }
    if (!env_new->op && fn->capture_type_id) {
        for (sema_destroy_fn_t *d = lc->sema->destroy_fns; d; d = d->next) {
            if (d->type && d->type->id == fn->capture_type_id && d->fn &&
                d->fn->kind == AST_FUNC_DEF) {
                ast_func_def_t *df = (ast_func_def_t *)d->fn;
                env_new->op =
                    module_symbol(lc->arena, lc->unit, "fn_", df->name);
                break;
            }
        }
    }
    /* 为 clone(f) 生成 env clone 包装：clone 函数返回捕获结构体值，
       这里把它复制进新的闭包 env。 */
    const char *clone_fn = NULL;
    if (fn->env_clone_fid) {
        sema_func_t *sf = sema_func_by_id(lc->sema, fn->env_clone_fid);
        if (sf && sf->def && sf->def->kind == AST_FUNC_DEF) {
            ast_func_def_t *df = (ast_func_def_t *)sf->def;
            clone_fn = module_symbol(lc->arena, lc->unit, "fn_", df->name);
        }
    }
    if (!clone_fn && fn->capture_type_id) {
        for (sema_clone_fn_t *c = lc->sema->clone_fns; c; c = c->next) {
            if (c->kind == 0 && c->type && c->type->id == fn->capture_type_id &&
                c->fn && c->fn->kind == AST_FUNC_DEF) {
                ast_func_def_t *df = (ast_func_def_t *)c->fn;
                clone_fn =
                    module_symbol(lc->arena, lc->unit, "fn_", df->name);
                break;
            }
        }
    }
    if (clone_fn && fn->capture_type_id) {
        const sema_type_t *capst =
            sema_type_by_id(lc->sema, fn->capture_type_id);
        cir_type_t *capct = capst && capst->type
                                ? map_type(lc, capst->type)
                                : NULL;
        if (capct && capct->c_name) {
            char hn[64];
            snprintf(hn, sizeof hn, "clux_env_clone_%u", seq);
            const char *hname = copy_arena(lc->arena, hn);
            const char *disp = env_new->op ? env_new->op : "NULL";
            (void)disp;
            size_t need = strlen(hname) + strlen(capct->c_name) +
                          strlen(clone_fn) + 256;
            char *code = (char *)arena_alloc(lc->arena, need, 1);
            if (!code) return NULL;
            snprintf(code, need,
                     "static void *%s(void *env) {\n"
                     "    %s clux_tmp = %s(NULL, (%s *)env);\n"
                     "    return clux_new(sizeof(clux_tmp), &clux_tmp);\n"
                     "}\n",
                     hname, capct->c_name, clone_fn, capct->c_name);
            cir_unit_add_raw(lc->unit, code);
            env_new->text2 = hname;
        }
    }
    /* inits 取反（构造顺序） */
    for (cir_init_t *it = inits; it;) {
        cir_init_t *next = it->next;
        cir_expr_add_init(env_new, it);
        it = next;
    }

    lower_capture_t *caps = NULL;
    for (ast_node_t *c = fn->captures; c; c = c->next) {
        ast_var_def_t *vd = (ast_var_def_t *)c;
        lower_capture_t *cp = (lower_capture_t *)arena_calloc(
            lc->arena, 1, sizeof(*cp), ALIGNOF(max_align_t));
        if (!cp) return NULL;
        cp->name = vd->name;
        char eb[512];
        snprintf(eb, sizeof eb, "((%s *)env)->clux_f_%.*s", envtag,
                 (int)vd->name.len, vd->name.ptr ? vd->name.ptr : "");
        cp->expr = copy_arena(lc->arena, eb);
        cp->next = caps;
        caps = cp;
    }

    lc->locals = NULL;
    lc->local_decls = NULL;
    lc->local_fns = NULL;
    lc->captures = caps;
    lc->defers = NULL;
    lc->cleanup = NULL;
    cir_type_t *save_ret = lc->return_type;
    unsigned save_ret_seq = lc->ret_seq;
    lc->ret_seq = 0;
    cir_function_t *cf = lower_function(lc, fn, false);
    lc->locals = save_locals;
    lc->local_fns = save_local_fns;
    lc->captures = save_caps;
    lc->return_type = save_ret;
    lc->ret_seq = save_ret_seq;
    if (!cf) return NULL;
    cf->closure_env = env;
    cir_unit_add_function(lc->unit, cf);

    const type_t *ft = func_type_of_def(lc, fn);
    cir_type_t *ct = ft ? map_type(lc, ft) : NULL;
    if (!ct) return NULL;
    cir_expr_t *cl = cir_expr_closure(lc->arena, ct, cf->name, env_new);
    if (cl) {
        cl->text2 = env_new->text2;
        cl->op = env_new->op;
    }
    return cl;
}

cir_expr_t *lower_func_literal(lower_ctx_t *lc, ast_func_def_t *fn) {
    if (!fn) return NULL;
    if (fn->captures || fn->capture_type_id != 0)
        return lower_closure_literal(lc, fn);
    char nb[64];
    snprintf(nb, sizeof nb, "__fnlit_%u", lc->fnlit_seq++);
    size_t n = strlen(nb);
    char *name = (char *)arena_alloc(lc->arena, n + 1, 1);
    if (!name) return NULL;
    memcpy(name, nb, n + 1);
    fn->name = strslice_from_bytes(name, n);

    const type_t *ft = func_type_of_def(lc, fn);
    if (!ft) return NULL;

    lower_local_t *save_locals = lc->locals;
    lower_defer_t *save_defers = lc->defers;
    cir_stmt_t *save_cleanup = lc->cleanup;
    cir_type_t *save_ret = lc->return_type;
    unsigned save_ret_seq = lc->ret_seq;
    lc->locals = NULL;
    lc->local_decls = NULL;
    lc->local_fns = NULL;
    lc->defers = NULL;
    lc->cleanup = NULL;
    lc->ret_seq = 0;

    cir_function_t *cf = lower_function(lc, fn, false);
    lc->locals = save_locals;
    lc->defers = save_defers;
    lc->cleanup = save_cleanup;
    lc->return_type = save_ret;
    lc->ret_seq = save_ret_seq;
    if (!cf) return NULL;
    cir_unit_add_function(lc->unit, cf);

    cir_type_t *ct = map_type(lc, ft);
    return ct ? cir_expr_func_obj(lc->arena, cf->object, ct) : NULL;
}
