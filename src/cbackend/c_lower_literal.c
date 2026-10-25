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
    if (type_kind_of(type_strip_quals(ot)) == TYPE_KIND_TUPLE) {
        /* 元组长度是编译期常量，C 里没有 `.data/len` 字段。 */
        return cir_expr_int(lc->arena,
                            (int64_t)tuple_type_elem_count(type_strip_quals(ot)));
    }
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
    { const char *ln = lookup_local_cname(lc, name); if (ln) return ln; }
    return module_symbol(lc->arena, lc->unit, "v_", name);
}

/* 函数定义的值类型：有捕获 → 闭包类型（捕获槽位进入类型身份）；
   无捕获 → 普通签名类型。捕获槽位类型解析顺序与 lower_closure_literal
   一致（resolved_type → 局部符号 → type_expr）。 */
const type_t *closure_type_of_def(lower_ctx_t *lc, ast_func_def_t *fn) {
    const type_t *plain = func_type_of_def(lc, fn);
    if (!plain || !fn) return plain;
    size_t nc = 0;
    for (ast_node_t *c = fn->captures; c; c = c->next) nc++;
    if (nc == 0) return plain;
    const type_t **capts = (const type_t **)arena_calloc(
        lc->arena, nc, sizeof(*capts), ALIGNOF(max_align_t));
    if (!capts) return plain;
    size_t k = 0;
    for (ast_node_t *c = fn->captures; c; c = c->next, k++) {
        ast_var_def_t *vd = (ast_var_def_t *)c;
        const type_t *t = vd->resolved_type;
        if (!t) t = lookup_local(lc, vd->name);
        if (!t && vd->type_expr)
            t = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        if (!t) return plain;   /* 类型未知：退回签名类型（已有诊断） */
        capts[k] = t;
    }
    size_t np = func_type_param_count(plain);
    const type_t *params[np > 0 ? np : 1];
    for (size_t i = 0; i < np; i++) params[i] = func_type_param(plain, i);
    const type_t *cty = type_func_closure(
        lc->sema->vm, np > 0 ? params : NULL, np,
        func_type_return(plain), func_type_is_variadic(plain), capts, nc);
    return cty ? cty : plain;
}

cir_expr_t *lower_closure_literal(lower_ctx_t *lc, ast_func_def_t *fn) {
    lower_local_t *save_locals = lc->locals;
    lower_local_fn_t *save_local_fns = lc->local_fns;
    lower_capture_t *save_caps = lc->captures;
    strslice_t orig = fn->name;
    unsigned seq = lc->fnlit_seq++;
    char nb[64];
    snprintf(nb, sizeof nb, "__closure_%u", seq);
    size_t n = strlen(nb);
    char *name = (char *)arena_alloc(lc->arena, n + 1, 1);
    if (!name) return NULL;
    memcpy(name, nb, n + 1);
    fn->name = strslice_from_bytes(name, n);

    /* 捕获槽位（声明序）：类型数组供闭包类型；inits 供内联 env 构造。 */
    size_t nc = 0;
    for (ast_node_t *c = fn->captures; c; c = c->next) nc++;
    const type_t **capts = NULL;
    if (nc > 0) {
        capts = (const type_t **)arena_calloc(
            lc->arena, nc, sizeof(*capts), ALIGNOF(max_align_t));
        if (!capts) return NULL;
    }
    cir_init_t *inits = NULL;
    size_t k = 0;
    for (ast_node_t *c = fn->captures; c; c = c->next, k++) {
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
        capts[k] = t;
        cir_expr_t *ve = NULL;
        if (vd->init && vd->init->kind != AST_UNDEF) {
            ve = lower_expr(lc, vd->init);
        } else {
            lower_local_fn_t *clf = find_local_fn(lc, vd->name);
            if (clf && clf->is_closure && clf->var_name) {
                ve = cir_expr_ident(lc->arena, clf->var_name);
            } else if (clf) {
                cir_type_t *fct = map_type(lc, clf->type);
                ve = fct ? cir_expr_func_obj(lc->arena, clf->obj, fct) : NULL;
            } else {
                const char *val = capture_or_var(lc, vd->name);
                ve = val ? cir_expr_ident(lc->arena, val) : NULL;
            }
        }
        if (!ve) return NULL;
        char fb[64];
        snprintf(fb, sizeof fb, "clux_f_%zu", k);
        cir_init_t *init = cir_init_new(lc->arena,
                                        copy_arena(lc->arena, fb), ve);
        if (!init) return NULL;
        init->next = inits;
        inits = init;
    }

    /* 闭包类型（捕获槽位进入类型身份）→ C 签名 struct 带内联 env。 */
    const type_t *plain = func_type_of_def(lc, fn);
    cir_type_t *ct = NULL;
    cir_type_t *env = NULL;
    if (plain) {
        size_t np = func_type_param_count(plain);
        const type_t *params[np > 0 ? np : 1];
        for (size_t i = 0; i < np; i++) params[i] = func_type_param(plain, i);
        const type_t *cty = type_func_closure(
            lc->sema->vm, np > 0 ? params : NULL, np,
            func_type_return(plain), false, capts, nc);
        ct = cty ? map_type(lc, cty) : NULL;
        if (ct && ct->sig) env = ct->sig->env_type;
    }
    if (!ct || !env) return NULL;

    cir_init_t *rev_inits = NULL;
    for (cir_init_t *it = inits; it;) {
        cir_init_t *next = it->next;
        it->next = rev_inits;
        rev_inits = it;
        it = next;
    }
    cir_expr_t *env_c = cir_expr_construct(lc->arena, env, rev_inits);
    if (!env_c) return NULL;

    /* body 捕获访问：名字 → 位置索引（C env 字段 clux_f_<idx>）。 */
    lower_capture_t *caps = NULL;
    k = 0;
    for (ast_node_t *c = fn->captures; c; c = c->next, k++) {
        ast_var_def_t *vd = (ast_var_def_t *)c;
        lower_capture_t *cp = (lower_capture_t *)arena_calloc(
            lc->arena, 1, sizeof(*cp), ALIGNOF(max_align_t));
        if (!cp) return NULL;
        cp->name = vd->name;
        cp->type = capts[k];
        cp->index = k;
        cp->next = caps;
        caps = cp;
    }
    /* caps 反转为声明序（查索引按名字，顺序不敏感；保持稳定） */
    lower_capture_t *rev = NULL;
    while (caps) {
        lower_capture_t *n2 = caps->next;
        caps->next = rev;
        rev = caps;
        caps = n2;
    }
    caps = rev;

    strslice_t save_self = lc->cur_self_name;
    const char *save_fc = lc->cur_fn_cname;
    const char *save_ep = lc->cur_env_param;
    lc->cur_self_name = orig;
    lc->cur_fn_cname = module_symbol(lc->arena, lc->unit, "fn_", fn->name);
    lc->cur_env_param = "env";

    lc->locals = NULL;
    lc->local_decls = NULL;
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
    lc->cur_self_name = save_self;
    lc->cur_fn_cname = save_fc;
    lc->cur_env_param = save_ep;
    lc->return_type = save_ret;
    lc->ret_seq = save_ret_seq;
    if (!cf) return NULL;
    cf->closure_env = env;
    cir_unit_add_function(lc->unit, cf);

    cir_expr_t *cl = cir_expr_closure(lc->arena, ct, cf->name, env_c);
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
