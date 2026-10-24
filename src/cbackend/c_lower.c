#include "cbackend/c_lower_internal.h"

#include <stdio.h>
#include <string.h>

static ast_func_def_t *find_main(ast_program_t *prog) {
    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_FUNC_DEF) continue;
        ast_func_def_t *fn = (ast_func_def_t *)n;
        if (strslice_eq(fn->name, STRSLICE_LIT("main"))) return fn;
    }
    return NULL;
}

/* 测试运行器：test 块已被改写成 `__clux_test_<n>` 无参 void 函数。 */
static size_t count_test_funcs(ast_program_t *prog) {
    size_t n = 0;
    for (ast_node_t *f = prog->funcs; f; f = f->next) {
        if (f->kind != AST_FUNC_DEF) continue;
        strslice_t name = ((ast_func_def_t *)f)->name;
        if (name.ptr && name.len >= 13 &&
            memcmp(name.ptr, "__clux_test_", 12) == 0)
            n++;
    }
    return n;
}

static void mark_cleanup_returns(cir_stmt_t *s) {
    for (; s; s = s->next) {
        if (s->kind == CIR_STMT_RETURN) s->cleanup_return = true;
        if (s->body) mark_cleanup_returns(s->body);
        if (s->else_body) mark_cleanup_returns(s->else_body);
        if (s->init) mark_cleanup_returns(s->init);
    }
}

cir_function_t *lower_function(lower_ctx_t *lc, ast_func_def_t *fn,
                                      bool is_entry) {
    lc->locals = NULL;
    if (fn->generic.count > 0 || fn->is_cfunc || fn->is_extern_cfunc) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)fn),
                   "c backend: unsupported function kind");
        lc->failed = true;
        return NULL;
    }
    const type_t *ret = lc->sema->vm->type_void;
    if (fn->return_expr)
        ret = sema_resolve_type_slot(lc->sema, &fn->return_expr);
    else if (fn->sig_id) {
        const sema_type_t *st = sema_type_by_id(lc->sema, fn->sig_id);
        if (st && st->type &&
            type_kind_of(st->type) == TYPE_KIND_FUNC) {
            const type_t *fr = func_type_return(st->type);
            if (fr) ret = fr;
        }
    }
    cir_type_t *cret = map_type(lc, ret);
    if (!cret) return NULL;

    size_t pc = 0;
    for (ast_node_t *p = fn->params; p; p = p->next) pc++;
    cir_param_t *params = NULL;
    cir_type_t **ptypes = NULL;
    if (pc) {
        params = (cir_param_t *)arena_calloc(
            lc->arena, pc, sizeof(*params), ALIGNOF(max_align_t));
        ptypes = (cir_type_t **)arena_calloc(
            lc->arena, pc, sizeof(*ptypes), ALIGNOF(max_align_t));
        if (!params || !ptypes) return NULL;
    }
    size_t i = 0;
    for (ast_node_t *p = fn->params; p; p = p->next, i++) {
        ast_var_def_t *vd = (ast_var_def_t *)p;
        const type_t *pt = vd->resolved_type;
        if (!pt && vd->type_expr)
            pt = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        ptypes[i] = map_type(lc, pt);
        if (!ptypes[i]) return NULL;
        params[i].type = ptypes[i];
        params[i].name = mangle(lc->arena, "clux_v_", vd->name);
        register_local(lc, vd->name, pt);
    }

    bool ok = true;
    lc->cleanup = NULL;
    lc->defers = NULL;
    lc->return_type = cret;
    lc->ret_seq = 0;
    cir_stmt_t *body = lower_block(lc, fn->body, &ok);
    if (!ok) return NULL;
    cir_stmt_t *cleanup = lc->cleanup;
    lc->cleanup = NULL;
    if (cleanup) mark_cleanup_returns(body);

    const char *cname = mangle(lc->arena, "clux_fn_", fn->name);
    const char *obj = mangle(lc->arena, "clux_obj_", fn->name);
    if (!cname || !obj) return NULL;
    cir_signature_t *sig = signature_for(lc, cret, pc, ptypes);
    if (!sig) return NULL;
    int line = sema_loc(lc->sema, (ast_node_t *)fn).begin.line;
    cir_function_t *out = cir_function_new(lc->arena, cname, obj, sig, pc, params,
                                           body, line, is_entry);
    if (out) out->cleanup = cleanup;
    return out;
}

cir_unit_t *cbackend_lower(arena_t *arena, sema_t *sema, ast_program_t *prog,
                           const char *source_path, bool test_mode) {
    if (!arena || !sema || !prog || !source_path) return NULL;
    ast_func_def_t *main_fn = find_main(prog);
    if (!main_fn && !test_mode) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)prog),
                   "c backend: no 'main' function");
        return NULL;
    }
    size_t param_count = 0;
    if (main_fn) {
        for (ast_node_t *p = main_fn->params; p; p = p->next) param_count++;
        if (param_count != 0) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)main_fn),
                       "c backend: 'main' must not take parameters");
            return NULL;
        }
        if (main_fn->return_expr) {
            const type_t *rt =
                sema_resolve_type_slot(sema, &main_fn->return_expr);
            if (rt != sema->vm->type_i32 && rt != sema->vm->type_void) {
                diag_error(sema->diag, sema_loc(sema, (ast_node_t *)main_fn),
                           "c backend: 'main' must return i32 or void");
                return NULL;
            }
        }
    }

    lower_ctx_t lc = {.arena = arena, .sema = sema, .failed = false};
    cir_unit_t *u = cir_unit_new(arena, source_path);
    if (!u) return NULL;
    lc.unit = u;
    /* 测试运行器标记必须在 lowering 之前就位：lowering 里 assert 的映射
       依赖它。 */
    if (test_mode) {
        u->test_runner = true;
        u->test_count = count_test_funcs(prog);
    }
    cir_unit_add_include(u, arena, "stdint.h");
    cir_unit_add_include(u, arena, "stddef.h");
    cir_unit_add_include(u, arena, "stdbool.h");

    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_VAR_DEF) continue;
        ast_var_def_t *vd = (ast_var_def_t *)n;
        if (vd->is_comptime || vd->is_extern_var) continue;
        const type_t *t = vd->resolved_type;
        if (!t && vd->type_expr)
            t = sema_resolve_type_slot(sema, &vd->type_expr);
        if (!t || type_kind_of(t) == TYPE_KIND_VOID)
            t = infer_expr_type(&lc, vd->init);
        if (t) register_global(&lc, vd->name, t);
        cir_type_t *ct = map_type(&lc, t);
        if (!ct) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)vd),
                       "c backend: cannot determine global variable type");
            return NULL;
        }
        cir_global_t *g = (cir_global_t *)arena_calloc(
            arena, 1, sizeof(*g), ALIGNOF(max_align_t));
        if (!g) return NULL;
        g->type = ct;
        g->name = mangle(arena, "clux_v_", vd->name);
        if (vd->init && vd->init->kind != AST_UNDEF)
            g->init = lower_expr(&lc, vd->init);
        cir_unit_add_global(u, g);
    }

    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_STRUCT_DEF) continue;
        ast_struct_def_t *sd = (ast_struct_def_t *)n;
        /* 泛型 struct 的方法体与类型参数绑定，sema 不会把实例化后的方法体
           放进 prog->funcs（只有函数实例会被重新解析成独立 AST）。因此 C
           后端暂时不 lowering 模板方法体；调用点会报“方法未找到”。 */
        if (sd->generic.count > 0) continue;
        for (ast_node_t *mem = sd->members; mem; mem = mem->next) {
            if (mem->kind != AST_FUNC_DEF) continue;
            ast_func_def_t *m = (ast_func_def_t *)mem;
            if (m->generic.count > 0 || m->is_comptime) continue;
            strslice_t orig = m->name;
            char mb[256];
            snprintf(mb, sizeof mb, "__method_%u_%.*s", sd->type_id,
                     (int)orig.len, orig.ptr ? orig.ptr : "");
            size_t mn = strlen(mb);
            char *cname = (char *)arena_alloc(arena, mn + 1, 1);
            if (!cname) return NULL;
            memcpy(cname, mb, mn + 1);
            m->name = strslice_from_bytes(cname, mn);
            cir_function_t *cf = lower_function(&lc, m, false);
            if (!cf || lc.failed) return NULL;
            cir_unit_add_function(u, cf);
            lower_method_t *lm = (lower_method_t *)arena_calloc(
                arena, 1, sizeof(*lm), ALIGNOF(max_align_t));
            if (!lm) return NULL;
            lm->name = orig;
            lm->owner_id = sd->type_id;
            lm->def = m;
            lm->cname = cf->name;
            lm->next = lc.methods;
            lc.methods = lm;
        }
    }

    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_FUNC_DEF) continue;
        ast_func_def_t *fn = (ast_func_def_t *)n;
        if (strslice_eq(fn->name, STRSLICE_LIT("main"))) continue;
        if (fn->generic.count > 0 || fn->is_cfunc || fn->is_extern_cfunc)
            continue;
        cir_function_t *cf = lower_function(&lc, fn, false);
        if (!cf || lc.failed) return NULL;
        cir_unit_add_function(u, cf);
    }
    cir_function_t *cm = main_fn ? lower_function(&lc, main_fn, true) : NULL;
    if (main_fn) {
        if (!cm || lc.failed) return NULL;
        cir_unit_add_function(u, cm);
    }
    return u;
}
