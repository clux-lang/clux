/* M7 import：跨编译单元引用的 C lowering。
 *
 * 每个模块独立编译成 `.c/.h/.o`（模块前缀 `clux_<tag>_` 由其相对项目根目录
 * 的路径决定），导入方通过**同名符号**调用导出方的函数/全局变量：
 *
 *   import math from "./math";
 *   math::add(1, 2)   →   extern int32_t clux_<math_tag>_fn_add(void *, int32_t,
 *                                                                int32_t);
 *                         clux_<math_tag>_fn_add(NULL, 1, 2)
 *   math::PI          →   extern int32_t clux_<math_tag>_v_PI;
 *   .math::Point{...} →   类型经 sema 解析成共享 VM type_t，本地按布局生成
 *                         （跨 TU 只需布局一致，C 不要求 struct tag 相同）
 *
 * `export cfunc` 在导出方生成的是 **plain C ABI** 入口 `clux_<tag>_<name>`，
 * 导入方据此调用（str 参数/返回自动编组）。
 */

#include "cbackend/c_lower_internal.h"

#include "core/strmap.h"
#include "core/string.h"
#include "sema/sema_cross.h"
#include "sema/struct_compose.h"

#include <stdio.h>
#include <string.h>

/* `mod::` 的模块别名 → module_t（sema 的全局作用域里是 SEMA_SYM_MODULE）。 */
static module_t *import_module_of(lower_ctx_t *lc, ast_scope_ref_t *sr) {
    if (!sr || !sr->type_expr || sr->type_expr->kind != AST_IDENT) return NULL;
    sema_symbol_t *sym = sema_lookup(
        lc->sema->global_scope, ((ast_ident_t *)sr->type_expr)->name);
    if (!sym || sym->kind != SEMA_SYM_MODULE || !sym->module) return NULL;
    return sym->module;
}

/* 导出符号的**定义模块**：sema_exports 里借用的是定义模块 sema 的符号对象，
 * 按指针身份在 vm->module_order 里找回定义模块（reexport 时别名模块不是
 * 定义模块，C 符号前缀必须用定义模块的 tag）。 */
static module_t *symbol_owner_module(lower_ctx_t *lc, sema_symbol_t *es,
                                     module_t *fallback) {
    if (!es || !lc || !lc->sema || !lc->sema->vm) return fallback;
    vm_t *vm = lc->sema->vm;
    if (!vm->module_order) return fallback;
    size_t n = vec_len(vm->module_order);
    for (size_t i = 0; i < n; i++) {
        module_t *m = (module_t *)vec_get(vm->module_order, i);
        if (!m || !m->sema_exports) continue;
        const vec_t *keys = strmap_keys(m->sema_exports);
        size_t kn = keys ? vec_len((vec_t *)keys) : 0;
        for (size_t k = 0; k < kn; k++) {
            const char *key = (const char *)vec_get((vec_t *)keys, k);
            if (key && (sema_symbol_t *)strmap_get(m->sema_exports, key) == es)
                return m;
        }
    }
    return fallback;
}


static const char *dep_prefix(lower_ctx_t *lc, module_t *dep, char *buf,
                              size_t cap) {
    char tag[16];
    module_tag_for_path(dep->canonical ? dep->canonical : "", tag, sizeof tag);
    snprintf(buf, cap, "clux_%s_", tag);
    (void)lc;
    return buf;
}

static const char *import_sym(arena_t *arena, const char *prefix,
                              const char *kind, strslice_t name) {
    return module_symbol(arena, &(cir_unit_t){.sym_prefix = prefix}, kind, name);
}

/* 同一个跨模块符号只声明一次。 */
static void note_import_decl(lower_ctx_t *lc, const char *name,
                             const char *code) {
    for (lower_import_decl_t *d = lc->import_decls; d; d = d->next)
        if (d->name && strcmp(d->name, name) == 0) return;
    lower_import_decl_t *d = (lower_import_decl_t *)arena_calloc(
        lc->arena, 1, sizeof(*d), ALIGNOF(max_align_t));
    if (!d) {
        lc->failed = true;
        return;
    }
    d->name = name;
    d->next = lc->import_decls;
    lc->import_decls = d;
    cir_unit_add_extern_decl(lc->unit, code);
    /* 后端边界自检：跨模块符号由**其它编译单元**定义，这里不该报
       "has no C definition"。 */
    if (!cir_unit_has_extern_symbol(lc->unit, name))
        cir_unit_add_extern_symbol(lc->unit, name);
}

/* `extern <ret> <name>(void *env, <params>);`（clux env-first ABI）。
 * 跨模块调用（含泛型实例）共用。 */
void lower_import_func_extern(lower_ctx_t *lc, const char *cname,
                              const type_t *ft, bool env_first) {    const type_t *rt = func_type_return(ft);
    size_t pc = func_type_param_count(ft);
    cir_type_t *cret = map_type(lc, rt ? rt : lc->sema->vm->type_void);
    if (!cret) {
        lc->failed = true;
        return;
    }
    string_t *sb = string_new(lc->sema->vm->alloc);
    if (!sb) {
        lc->failed = true;
        return;
    }
    string_append_cstr(sb, "extern ");
    string_append_cstr(sb, cret->c_name);
    string_append_cstr(sb, " ");
    string_append_cstr(sb, cname);
    string_append_cstr(sb, "(");
    bool first = true;
    if (env_first) {
        string_append_cstr(sb, "void *env");
        first = false;
    }
    for (size_t i = 0; i < pc; i++) {
        const type_t *pt = func_type_param(ft, i);
        cir_type_t *ct = map_type(lc, pt);
        if (!ct) {
            lc->failed = true;
            string_free(&sb);
            return;
        }
        if (!first) string_append_cstr(sb, ", ");
        string_append_cstr(sb, ct->c_name);
        first = false;
    }
    if (first) string_append_cstr(sb, "void");
    string_append_cstr(sb, ");");
    const char *text = copy_arena(lc->arena, string_cstr(sb));
    string_free(&sb);
    if (!text) {
        lc->failed = true;
        return;
    }
    note_import_decl(lc, cname, text);
}

/* `mod::fn(...)`：跨单元函数调用。 */
cir_expr_t *lower_import_call(lower_ctx_t *lc, ast_call_t *call,
                              ast_scope_ref_t *sr) {
    module_t *dep = import_module_of(lc, sr);
    if (!dep) return NULL;
    sema_symbol_t *es = sema_module_export_symbol(dep, sr->variant);
    if (!es) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: module has no exported symbol '%.*s'",
                   (int)sr->variant.len, sr->variant.ptr);
        lc->failed = true;
        return NULL;
    }
    if (es->kind != SEMA_SYM_FUNC || !es->type) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: '%.*s' is not a callable export",
                   (int)sr->variant.len, sr->variant.ptr);
        lc->failed = true;
        return NULL;
    }
    const type_t *ft = type_strip_quals(es->type);
    if (type_kind_of(ft) != TYPE_KIND_FUNC) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: imported value is not a function");
        lc->failed = true;
        return NULL;
    }
    ast_func_def_t *def = (es->ast && es->ast->kind == AST_FUNC_DEF)
                              ? (ast_func_def_t *)es->ast
                              : NULL;
    bool is_cfunc = def && def->is_cfunc;
    module_t *owner = symbol_owner_module(lc, es, dep);
    char pbuf[64];
    const char *prefix = dep_prefix(lc, owner, pbuf, sizeof pbuf);
    const char *cname = import_sym(lc->arena, prefix, is_cfunc ? "" : "fn_",
                                   sr->variant);
    if (!cname) {
        lc->failed = true;
        return NULL;
    }
    lower_import_func_extern(lc, cname, ft, /*env_first=*/!is_cfunc);

    cir_expr_t *out = cir_expr_call(lc->arena, cname, NULL);
    if (!out) {
        lc->failed = true;
        return NULL;
    }
    if (!is_cfunc) cir_expr_append_arg(out, cir_expr_ident(lc->arena, "NULL"));
    size_t i = 0;
    for (ast_node_t *a = call->args; a; a = a->next, i++) {
        cir_expr_t *arg = lower_expr(lc, a);
        if (!arg) return NULL;
        /* 导出的 cfunc 是 C ABI：str 实参取 `.data`。 */
        if (is_cfunc && i < func_type_param_count(ft)) {
            const type_t *pt = type_strip_quals(func_type_param(ft, i));
            if (pt && pt == lc->sema->vm->type_str)
                arg = cir_expr_member(lc->arena, arg, "data");
        }
        cir_expr_append_arg(out, arg);
    }
    /* cfunc 返回 str：C 侧是 char*，包回 clux_str。 */
    if (is_cfunc) {
        const type_t *rt = type_strip_quals(func_type_return(ft));
        if (rt && rt == lc->sema->vm->type_str) {
            lc->unit->uses_str_from_c = true;
            cir_expr_t *wrap =
                cir_expr_call(lc->arena, "clux_str_from_c", NULL);
            if (!wrap) {
                lc->failed = true;
                return NULL;
            }
            cir_expr_append_arg(wrap, out);
            return wrap;
        }
    }
    return out;
}

/* `extern <type> <sym>;` + C 符号名。 */
static const char *import_var_symbol(lower_ctx_t *lc, ast_scope_ref_t *sr,
                                     const char **out_type_name) {
    module_t *dep = import_module_of(lc, sr);
    if (!dep) return NULL;
    sema_symbol_t *es = sema_module_export_symbol(dep, sr->variant);
    if (!es || es->kind != SEMA_SYM_VAR || !es->type) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)sr),
                   "c backend: '%.*s' is not an imported variable",
                   (int)sr->variant.len, sr->variant.ptr);
        lc->failed = true;
        return NULL;
    }
    char pbuf[64];
    module_t *owner = symbol_owner_module(lc, es, dep);
    const char *prefix = dep_prefix(lc, owner, pbuf, sizeof pbuf);
    const char *cname = import_sym(lc->arena, prefix, "v_", sr->variant);
    if (!cname) {
        lc->failed = true;
        return NULL;
    }
    cir_type_t *ct = map_type(lc, es->type);
    if (!ct) {
        lc->failed = true;
        return NULL;
    }
    string_t *sb = string_new(lc->sema->vm->alloc);
    if (!sb) {
        lc->failed = true;
        return NULL;
    }
    string_append_cstr(sb, "extern ");
    string_append_cstr(sb, ct->c_name);
    string_append_cstr(sb, " ");
    string_append_cstr(sb, cname);
    string_append_cstr(sb, ";");
    const char *text = copy_arena(lc->arena, string_cstr(sb));
    string_free(&sb);
    if (!text) {
        lc->failed = true;
        return NULL;
    }
    note_import_decl(lc, cname, text);
    if (out_type_name) *out_type_name = ct->c_name;
    return cname;
}

cir_expr_t *lower_import_var_read(lower_ctx_t *lc, ast_scope_ref_t *sr) {
    const char *cname = import_var_symbol(lc, sr, NULL);
    if (!cname) return NULL;
    return cir_expr_ident(lc->arena, cname);
}

const char *lower_import_var_target(lower_ctx_t *lc, ast_scope_ref_t *sr) {
    return import_var_symbol(lc, sr, NULL);
}

bool is_import_scope_ref(lower_ctx_t *lc, ast_scope_ref_t *sr) {
    return import_module_of(lc, sr) != NULL;
}

/* 按 fid 在**所有模块**的 sema->funcs 里找函数（跨模块泛型实例登记在
 * 定义模块的 funcs 队列里，导入方 sema->funcs 里没有）。 */
sema_func_t *lower_find_func_module(lower_ctx_t *lc, uint32_t fid,
                                    module_t **out_owner) {
    if (out_owner) *out_owner = NULL;
    if (!lc || !lc->sema || !lc->sema->vm || !fid) return NULL;
    vm_t *vm = lc->sema->vm;
    if (!vm->module_order) return NULL;
    size_t n = vec_len(vm->module_order);
    for (size_t i = 0; i < n; i++) {
        module_t *m = (module_t *)vec_get(vm->module_order, i);
        if (!m || !m->sema || !m->sema->funcs) continue;
        size_t fn = vec_len(m->sema->funcs);
        for (size_t k = 0; k < fn; k++) {
            sema_func_t *sf = (sema_func_t *)vec_get(m->sema->funcs, k);
            if (!sf || !sf->def || sf->def->kind != AST_FUNC_DEF) continue;
            if (((ast_func_def_t *)sf->def)->fid != fid) continue;
            if (out_owner) *out_owner = m;
            return sf;
        }
    }
    return NULL;
}

/* 方法实例若实际定义在导入模块（sema 把实例方法挂在 owner 模块），调用点
   必须用 owner 的符号前缀，而不是当前单元前缀。 */
const char *lower_method_owner_cname(lower_ctx_t *lc, ast_func_def_t *def,
                                     const type_t *ft) {
    if (!lc || !def || !ft) return NULL;
    module_t *owner = (module_t *)def->owner_module;
    if (!owner) return NULL;
    char pb[64];
    const char *prefix = dep_prefix(lc, owner, pb, sizeof pb);
    if (!prefix || (lc->unit->sym_prefix &&
                    strcmp(prefix, lc->unit->sym_prefix) == 0))
        return NULL;
    const char *cname = import_sym(lc->arena, prefix, "fn_", def->name);
    if (!cname) {
        lc->failed = true;
        return NULL;
    }
    lower_import_func_extern(lc, cname, ft, /*env_first=*/true);
    return lc->failed ? NULL : cname;
}

/* 跨模块结构体方法调用：方法函数体在定义模块的编译单元里（方法名由 sema
 * 生成为 `__method_<typeId>_<name>`，天然唯一），这里按定义模块的符号生成
 * extern 声明并补一个 `lower_method_t`，调用点沿用普通方法调用 lowering
 * （NULL env + self 实参）。 */
lower_method_t *lower_cross_module_method(lower_ctx_t *lc, uint32_t type_id,
                                          strslice_t name, ast_node_t *site,
                                          lower_method_t *out) {
    if (!out || !type_id) return NULL;
    const sema_type_t *stt = sema_type_by_id(lc->sema, type_id);
    if (!stt || !stt->type) return NULL;
    sema_symbol_t *ms =
        sema_struct_find_member(lc->sema, stt->type, name, site, NULL);
    if (!ms || ms->kind != SEMA_SYM_FUNC || !ms->ast ||
        ms->ast->kind != AST_FUNC_DEF)
        return NULL;
    ast_func_def_t *def = (ast_func_def_t *)ms->ast;
    module_t *owner = NULL;
    (void)lower_find_func_module(lc, def->fid, &owner);
    if (!owner) return NULL;
    const type_t *ft = ms->type;
    if (!ft || type_kind_of(type_strip_quals(ft)) != TYPE_KIND_FUNC) return NULL;
    char pbuf[64];
    const char *prefix = dep_prefix(lc, owner, pbuf, sizeof pbuf);
    const char *cname = import_sym(lc->arena, prefix, "fn_", def->name);
    if (!cname) {
        lc->failed = true;
        return NULL;
    }
    lower_import_func_extern(lc, cname, ft, /*env_first=*/true);
    if (lc->failed) return NULL;
    memset(out, 0, sizeof(*out));
    out->name = name;
    out->owner_id = type_id;
    out->def = def;
    out->cname = cname;
    return out;
}

/* 跨模块**泛型实例**调用：sema 把调用点的 callee 改写成实例的 FUNC_REF
 * （fid 在导入方 sema 分配，实例体在定义模块的 prog 里）。按 fid 取回
 * sema_func_t → 定义模块（def->owner_module）与实例名，生成
 * `clux_<ownerTag>_fn_<instance>` 的 extern 声明并调用。 */
cir_expr_t *lower_cross_module_func_call(lower_ctx_t *lc, ast_call_t *call,
                                         ast_func_def_t *def, sema_func_t *sf) {
    module_t *owner = (module_t *)def->owner_module;
    const type_t *ft = (sf && sf->generic_inst) ? sf->generic_inst->sig : NULL;
    if (!owner) return NULL;
    if (!ft || type_kind_of(ft) != TYPE_KIND_FUNC) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: cannot resolve cross-module generic instance "
                   "'%.*s' signature",
                   (int)def->name.len, def->name.ptr);
        lc->failed = true;
        return NULL;
    }
    char pbuf[64];
    const char *prefix = dep_prefix(lc, owner, pbuf, sizeof pbuf);
    const char *cname = import_sym(lc->arena, prefix, "fn_", def->name);
    if (!cname) {
        lc->failed = true;
        return NULL;
    }
    lower_import_func_extern(lc, cname, ft, /*env_first=*/true);
    if (lc->failed) return NULL;
    cir_expr_t *out = cir_expr_call(lc->arena, cname, NULL);
    if (!out) {
        lc->failed = true;
        return NULL;
    }
    cir_expr_append_arg(out, cir_expr_ident(lc->arena, "NULL"));
    for (ast_node_t *a = call->args; a; a = a->next) {
        cir_expr_t *arg = lower_expr(lc, a);
        if (!arg) return NULL;
        cir_expr_append_arg(out, arg);
    }
    return out;
}
