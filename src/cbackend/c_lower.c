#include "cbackend/c_lower_internal.h"
#include "cbackend/c_writer.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <stdint.h>
#else
#include <stdint.h>
#include <unistd.h>
#endif

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
    lc->local_decls = NULL;
    if (fn->generic.count > 0) {
        /* 架构契约：泛型在 sema 内实例化完毕，后端只该看到具体函数。
           走到这里说明 sema 漏了实例化（内部错误）。 */
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)fn),
                   "c backend: internal: generic function '%.*s' reached "
                   "lowering (sema must materialize instances)",
                   (int)fn->name.len, fn->name.ptr);
        lc->failed = true;
        return NULL;
    }
    if (fn->is_extern_cfunc) {
        /* extern cfunc 只有声明：原型由 lower_extern_decls 输出，调用点直接
           调用 C 符号；走到这里说明 sema/AST 形态异常。 */
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)fn),
                   "c backend: internal: extern cfunc '%.*s' reached function "
                   "lowering",
                   (int)fn->name.len, fn->name.ptr);
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
        params[i].name = module_symbol(lc->arena, lc->unit, "v_", vd->name);
        register_local(lc, vd->name, pt);
    }

    bool ok = true;
    lc->cleanup = NULL;
    lc->defers = NULL;
    lc->return_type = cret;
    lc->return_sema_type = ret;
    lc->ret_seq = 0;
    cir_stmt_t *body = lower_block(lc, fn->body, &ok);
    if (!ok) return NULL;
    cir_stmt_t *cleanup = lc->cleanup;
    lc->cleanup = NULL;
    if (cleanup) mark_cleanup_returns(body);

    const char *cname = module_symbol(lc->arena, lc->unit, "fn_", fn->name);
    const char *obj = module_symbol(lc->arena, lc->unit, "obj_", fn->name);
    if (!cname || !obj) return NULL;
    cir_signature_t *sig = signature_for(lc, cret, pc, ptypes);
    if (!sig) return NULL;
    int line = sema_loc(lc->sema, (ast_node_t *)fn).begin.line;
    cir_function_t *out = cir_function_new(lc->arena, cname, obj, sig, pc, params,
                                           body, line, is_entry);
    if (out) {
        out->cleanup = cleanup;
        /* `export func`：非 static，并在模块头文件里声明。
           `export cfunc` 走 C ABI 包装（lower_exported_cfunc），内部
           env-first 函数保持 static。
           泛型实例（名字含 `$`）跨模块可见：不加 static（唯一 mangling 名，
           不会与其他模块冲突），但不进头文件。 */
        out->is_exported = ((ast_node_t *)fn)->is_exported && !is_entry &&
                           !fn->is_cfunc;
        bool generic_instance = false;
        for (size_t k = 0; k + 1 <= fn->name.len; k++) {
            if (fn->name.ptr && fn->name.ptr[k] == '$') {
                generic_instance = true;
                break;
            }
        }
        /* 结构体方法（sema 生成名 `__method_<typeId>_<name>`，全局唯一）：
           跨模块调用时也要能链接到定义模块的实现。
           测试函数（`__clux_test_N`）：多文件测试时由入口模块的 runner
           跨单元调用。 */
        bool method = fn->name.len > 9 && fn->name.ptr &&
                      memcmp(fn->name.ptr, "__method_", 9) == 0;
        bool test_fn = fn->name.len > 12 && fn->name.ptr &&
                       memcmp(fn->name.ptr, "__clux_test_", 12) == 0;
        out->is_public = out->is_exported || generic_instance || method || test_fn;
    }
    return out;
}

/* ---- 调用目标自检 -------------------------------------------------------
 * 后端只应看到 sema 已具体化的函数：泛型实例（函数/类型实例）都由 sema
 * 从定义 token 重新解析成干净 AST 后追加到 prog->funcs，模板本身不生成代码。
 * 如果某次调用指向一个既不在本 unit、也不是已知运行期辅助函数的名字，
 * 说明 sema 漏掉了实例化（或用了不支持的 builtin）——在这里报内部错误，
 * 而不是让 host cc 报 "undeclared function"。 */

static bool unit_has_function(const cir_unit_t *u, const char *name) {
    for (const cir_function_t *f = u->functions; f; f = f->next)
        if (f->name && strcmp(f->name, name) == 0) return true;
    return false;
}

static bool call_target_known(const cir_unit_t *u, const char *name) {
    static const char *const helpers[] = {
        "clux_new",   "clux_make",       "clux_str_eq", "clux_bf_get",
        "clux_bf_set", "clux_assert",    "clux_panic_none",
        "clux_str_from_c", "clux_index_check", "clux_slice_check",
        /* 运行期 ABI（自带原型，见 writer 的 runtime prelude） */
        "malloc", "calloc", "realloc", "free", "memcpy", "memset", "memcmp",
        "strlen", "abort", "printf", "fflush", "strncmp", "clux_panic",
        "_Exit", "clux_rc_new", "clux_rc_retain", "clux_rc_release",
        "clux_rc_weak_retain", "clux_rc_weak_release", "clux_rc_upgrade", NULL};
    if (!name) return true;
    for (size_t i = 0; helpers[i]; i++)
        if (strcmp(name, helpers[i]) == 0) return true;
    if (strncmp(name, "clux_bf_make_", 13) == 0) return true;
    /* extern cfunc：符号由外部库定义（原型已在本单元里声明）。 */
    if (cir_unit_has_extern_symbol(u, name)) return true;
    return unit_has_function(u, name);
}

static const char *missing_call_in_expr(const cir_unit_t *u,
                                        const cir_expr_t *e);
static const char *missing_call_in_list(const cir_unit_t *u,
                                        const cir_expr_t *e) {
    for (; e; e = e->next) {
        const char *m = missing_call_in_expr(u, e);
        if (m) return m;
    }
    return NULL;
}

static const char *missing_call_in_expr(const cir_unit_t *u,
                                        const cir_expr_t *e) {
    if (!e) return NULL;
    if (e->kind == CIR_EXPR_CALL && !call_target_known(u, e->text))
        return e->text;
    const char *m = NULL;
    if ((m = missing_call_in_expr(u, e->lhs))) return m;
    if ((m = missing_call_in_expr(u, e->rhs))) return m;
    if ((m = missing_call_in_expr(u, e->high))) return m;
    if ((m = missing_call_in_list(u, e->args))) return m;
    for (const cir_init_t *i = e->inits; i; i = i->next) {
        if ((m = missing_call_in_expr(u, i->value))) return m;
    }
    return NULL;
}

static const char *missing_call_in_stmt(const cir_unit_t *u,
                                        const cir_stmt_t *s) {
    for (; s; s = s->next) {
        const char *m = NULL;
        if ((m = missing_call_in_expr(u, s->expr))) return m;
        if ((m = missing_call_in_expr(u, s->lhs))) return m;
        if ((m = missing_call_in_expr(u, s->update))) return m;
        if ((m = missing_call_in_stmt(u, s->init))) return m;
        if ((m = missing_call_in_stmt(u, s->body))) return m;
        if ((m = missing_call_in_stmt(u, s->else_body))) return m;
    }
    return NULL;
}

static const char *find_missing_call(const cir_unit_t *u) {
    for (const cir_function_t *f = u->functions; f; f = f->next) {
        const char *m = missing_call_in_stmt(u, f->body);
        if (m) return m;
        m = missing_call_in_stmt(u, f->cleanup);
        if (m) return m;
    }
    for (const cir_global_t *g = u->globals; g; g = g->next) {
        const char *m = missing_call_in_expr(u, g->init);
        if (m) return m;
    }
    return NULL;
}

cir_unit_t *cbackend_lower(arena_t *arena, sema_t *sema, ast_program_t *prog,
                           const char *source_path, bool test_mode,
                           bool allow_no_main) {
    if (!arena || !sema || !prog || !source_path) return NULL;
    ast_func_def_t *main_fn = find_main(prog);
    if (!main_fn && !test_mode && !allow_no_main) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)prog),
                   "c backend: no 'main' function");
        return NULL;
    }
    size_t param_count = 0;
    const type_t *mp0 = NULL, *mp1 = NULL;
    if (main_fn) {
        for (ast_node_t *p = main_fn->params; p; p = p->next) {
            ast_var_def_t *vd = (ast_var_def_t *)p;
            const type_t *pt = vd->resolved_type;
            if (!pt && vd->type_expr)
                pt = sema_resolve_type_slot(sema, &vd->type_expr);
            if (param_count == 0) mp0 = pt;
            else if (param_count == 1) mp1 = pt;
            param_count++;
        }
        /* main 的 argv 参数统一暴露为 `ref []str`（argv[0] 是程序名）。 */
        bool ok_sig = false;
        if (param_count == 0) {
            ok_sig = true;
        } else if (param_count == 1) {
            const type_t *p0 = mp0 ? type_strip_quals(mp0) : NULL;
            if (p0 && type_kind_of(p0) == TYPE_KIND_INT &&
                p0 == sema->vm->type_i32)
                ok_sig = true; /* main(argc: i32) */
            else if (p0 && type_kind_of(p0) == TYPE_KIND_SLICE) {
                const type_t *el =
                    type_strip_quals(slice_type_elem(p0));
                ok_sig = el && el == sema->vm->type_str; /* main(argv) */
            }
        } else if (param_count == 2) {
            const type_t *p0 = mp0 ? type_strip_quals(mp0) : NULL;
            const type_t *p1 = mp1 ? type_strip_quals(mp1) : NULL;
            if (p0 == sema->vm->type_i32 && p1 &&
                type_kind_of(p1) == TYPE_KIND_SLICE) {
                const type_t *el = type_strip_quals(slice_type_elem(p1));
                ok_sig = el && el == sema->vm->type_str; /* main(argc, argv) */
            }
        }
        if (!ok_sig) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)main_fn),
                       "c backend: 'main' parameters must be (), (argc: i32), "
                       "(argv: ref []str), or (argc: i32, argv: ref []str)");
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
    /* 模块指纹：**源文件相对项目根目录（cwd）的路径**——相对路径本身就是
       它在依赖图里的身份，同一路径在任何机器/任何构建目录下都得到同一个
       短 hash（多编译单元链接时用 `clux_<hash>_` 前缀区分符号）。 */
    {
        char tag[16];
        module_tag_for_path(source_path, tag, sizeof tag);
        cir_unit_set_module(u, tag);
    }
    /* 测试运行器标记必须在 lowering 之前就位：lowering 里 assert 的映射
       依赖它。 */
    if (test_mode) {
        u->tests = true;
        u->test_runner = true;
        u->test_count = count_test_funcs(prog);
        /* 用例表：函数符号（`<prefix>fn___clux_test_N`）。显示名与跨模块
           合并由多文件构建层填充/替换。 */
        size_t cap = u->test_count ? u->test_count : 1;
        u->test_cases = (cir_test_case_t *)arena_calloc(
            arena, cap, sizeof(cir_test_case_t), ALIGNOF(max_align_t));
        if (!u->test_cases) return NULL;
        u->test_case_count = 0;
        for (ast_node_t *f = prog->funcs; f; f = f->next) {
            if (f->kind != AST_FUNC_DEF) continue;
            strslice_t name = ((ast_func_def_t *)f)->name;
            if (!name.ptr || name.len < 13 ||
                memcmp(name.ptr, "__clux_test_", 12) != 0)
                continue;
            u->test_cases[u->test_case_count].symbol =
                module_symbol(arena, u, "fn_", name);
            u->test_case_count++;
        }
    }
    /* 导出类型名（`export struct/union/cunion/enum/type`）：C 形态无条件进
       头文件。enum 渲染成底层整数，不产生 CIR 类型，也就不需要登记。 */
    {
        size_t cap = 0;
        for (ast_node_t *n = prog->funcs; n; n = n->next)
            if (n->is_exported) cap++;
        if (cap) {
            lc.exported_types = (strslice_t *)arena_calloc(
                arena, cap, sizeof(strslice_t), ALIGNOF(max_align_t));
            if (!lc.exported_types) return NULL;
            for (ast_node_t *n = prog->funcs; n; n = n->next) {
                if (!n->is_exported) continue;
                strslice_t name = STRSLICE_EMPTY;
                switch (n->kind) {
                case AST_STRUCT_DEF: {
                    ast_struct_def_t *sd = (ast_struct_def_t *)n;
                    if (sd->generic.count > 0) continue; /* 模板无具体 C 形态 */
                    name = sd->name;
                    break;
                }
                case AST_UNION_DEF: {
                    ast_union_def_t *ud = (ast_union_def_t *)n;
                    if (ud->generic.count > 0) continue;
                    name = ud->name;
                    break;
                }
                case AST_CUNION_DEF: {
                    ast_cunion_def_t *cd = (ast_cunion_def_t *)n;
                    name = cd->name;
                    break;
                }
                case AST_ENUM_DEF:
                    name = ((ast_enum_def_t *)n)->name;
                    break;
                case AST_TYPE_DEF: {
                    ast_type_def_t *td = (ast_type_def_t *)n;
                    if (td->generic.count > 0) continue;
                    name = td->name;
                    break;
                }
                default:
                    continue;
                }
                lc.exported_types[lc.exported_type_count++] = name;
            }
        }
    }
    cir_unit_add_include(u, arena, "stdint.h");
    cir_unit_add_include(u, arena, "stddef.h");
    cir_unit_add_include(u, arena, "stdbool.h");

    /* extern lib → 链接参数；extern cfunc → C 原型 + 调用点映射。
       必须在 lowering 之前：调用点要按 `<lib>::<name>` 查表。 */
    lower_extern_decls(&lc, prog);
    if (lc.failed) return NULL;

    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_VAR_DEF) continue;
        ast_var_def_t *vd = (ast_var_def_t *)n;
        if (vd->is_comptime || vd->is_extern_var) continue;
        const type_t *t = vd->resolved_type;
        if (!t && vd->type_expr)
            t = sema_resolve_type_slot(sema, &vd->type_expr);
        if (!t || type_kind_of(t) == TYPE_KIND_VOID)
            t = infer_expr_type(&lc, vd->init);
        t = decl_value_type(&lc, t, vd->init);
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
        g->name = module_symbol(arena, u, "v_", vd->name);
        g->is_exported = ((ast_node_t *)vd)->is_exported;
        if (vd->init && vd->init->kind != AST_UNDEF)
            g->init = lower_expr(&lc, vd->init);
        g->needs_init = g->init && !cir_expr_is_constant(g->init);
        cir_unit_add_global(u, g);
    }

    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_STRUCT_DEF) continue;
        ast_struct_def_t *sd = (ast_struct_def_t *)n;
        /* 模板 struct 不是实例，不生成代码（与 VM compiler 的
           `generic.count > 0` 跳过一致）。sema 会把每个用到的类型实例重新
           解析成**具体** struct（名字含 `$`，generic.count == 0）并追加到
           prog->funcs，实例方法就在其中。 */
        if (sd->generic.count > 0) continue;
        for (ast_node_t *mem = sd->members; mem; mem = mem->next) {
            if (mem->kind != AST_FUNC_DEF) continue;
            ast_func_def_t *m = (ast_func_def_t *)mem;
            /* 泛型方法模板（`func map[U]`）：调用点由 sema 实例化，模板不生成
               代码。 */
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
        /* 泛型函数模板不生成代码；用到的实例已由 sema 重新解析成具体函数，
           在下面的遍历里按普通函数 lowering。extern cfunc 只有声明（原型由
           lower_extern_decls 生成），没有可 lowering 的函数体。 */
        if (fn->generic.count > 0 || fn->is_extern_cfunc) continue;
        cir_function_t *cf = lower_function(&lc, fn, false);
        if (!cf || lc.failed) return NULL;
        cir_unit_add_function(u, cf);
    }
    cir_function_t *cm = main_fn ? lower_function(&lc, main_fn, true) : NULL;
    if (main_fn) {
        if (!cm || lc.failed) return NULL;
        cir_unit_add_function(u, cm);
    }
    /* `export cfunc`：生成 C ABI 包装（内部函数保持 env-first/static）。 */
    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_FUNC_DEF || !n->is_exported) continue;
        ast_func_def_t *fn = (ast_func_def_t *)n;
        if (fn->is_cfunc && !fn->is_extern_cfunc)
            lower_exported_cfunc(&lc, fn);
    }
    if (lc.failed) return NULL;

    /* 泛型必须在 sema 结束前实例化完毕：这里是后端边界上的最后一道自检。 */
    const char *missing = find_missing_call(u);
    if (missing) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)prog),
                   "c backend: call to '%s' has no C definition "
                   "(unsupported builtin, or a generic instance that sema did "
                   "not materialize)",
                   missing);
        return NULL;
    }

    /* ---- 头文件类型集合 ----
     * 导出类型（无条件）+ 导出函数/全局的签名类型；递归展开依赖在
     * cw_mark_header_type 里完成，保证 .h 里的类型按拓扑序（依赖先）输出。
     * 导出类型即使源码里没有用到也要生成 C 形态（头文件是公开契约）。 */
    for (size_t i = 0; i < lc.exported_type_count; i++) {
        sema_symbol_t *ts = sema_lookup(sema->global_scope,
                                        lc.exported_types[i]);
        if (ts && ts->kind == SEMA_SYM_TYPE && ts->type) {
            cir_type_t *ct = map_type(&lc, ts->type);
            if (ct) cw_mark_header_type(ct);
        }
    }
    for (cir_type_t *t = u->types; t; t = t->next) {
        if (!t->src) continue;
        const type_t *ct = (const type_t *)t->src;
        for (size_t i = 0; i < lc.exported_type_count; i++) {
            if (strslice_eq(ct->name, lc.exported_types[i])) {
                cw_mark_header_type(t);
                break;
            }
        }
    }
    for (cir_function_t *f = u->functions; f; f = f->next) {
        if (!f->is_exported || !f->sig) continue;
        if (f->sig->ret) cw_mark_header_type(f->sig->ret);
        for (size_t i = 0; i < f->sig->param_count; i++)
            cw_mark_header_type(f->sig->params[i]);
    }
    for (cir_global_t *g = u->globals; g; g = g->next)
        if (g->is_exported && g->type) cw_mark_header_type(g->type);
    for (cir_type_ref_t *r = u->header_types; r; r = r->next)
        cw_mark_header_type(r->type);

    return u;
}
