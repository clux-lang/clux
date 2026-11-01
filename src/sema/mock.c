#include "sema/mock.h"

#include "parser/ast_func_def.h"
#include "parser/ast_string_lit.h"
#include "sema/struct_compose.h"
#include "sema/sema_cross.h"
#include "vm/type_func.h"

#include <string.h>

/* ---- 工具 ---- */

/* 目标名必须是字符串字面量：`mock("f", ...)`。运行期字符串无法在编译期解析
   目标（也就无法生成函数槽），所以明确拒绝。 */
static bool literal_name(ast_node_t *n, strslice_t *out) {
    if (!n || n->kind != AST_STRING_LIT) return false;
    *out = ((ast_string_lit_t *)n)->text;
    return out->ptr != NULL;
}

/* `name` 或 `alias::name` → 目标函数符号。`out_name` 回填裸名字（用于诊断）。 */
static sema_symbol_t *resolve_free(sema_t *sema, strslice_t spec,
                                   strslice_t *out_name, ast_node_t *loc,
                                   const char *what) {
    const char *sep = NULL;
    for (size_t i = 0; i + 1 < spec.len; i++) {
        if (spec.ptr[i] == ':' && spec.ptr[i + 1] == ':') {
            sep = spec.ptr + i;
            break;
        }
    }
    if (!sep) {
        *out_name = spec;
        sema_symbol_t *s = sema_lookup(sema->global_scope, spec);
        if (!s)
            diag_error(sema->diag, sema_loc(sema, loc),
                       "%s: unknown function '%.*s'", what, (int)spec.len,
                       spec.ptr);
        return s;
    }
    strslice_t alias = {spec.ptr, (size_t)(sep - spec.ptr)};
    strslice_t name = {sep + 2, spec.len - (size_t)(sep - spec.ptr) - 2};
    if (alias.len == 0 || name.len == 0) {
        diag_error(sema->diag, sema_loc(sema, loc),
                   "%s: '%.*s' is not a valid target name (expected `name` or "
                   "`module::name`)",
                   what, (int)spec.len, spec.ptr);
        return NULL;
    }
    sema_symbol_t *ms = sema_lookup(sema->global_scope, alias);
    if (!ms || ms->kind != SEMA_SYM_MODULE || !ms->module) {
        diag_error(sema->diag, sema_loc(sema, loc),
                   "%s: '%.*s' is not an imported module alias", what,
                   (int)alias.len, alias.ptr);
        return NULL;
    }
    sema_symbol_t *fs = sema_module_export_symbol(ms->module, name);
    if (!fs) {
        diag_error(sema->diag, sema_loc(sema, loc),
                   "%s: module '%.*s' has no exported function '%.*s'", what,
                   (int)alias.len, alias.ptr, (int)name.len, name.ptr);
        return NULL;
    }
    *out_name = name;
    return fs;
}

/* replacement 必须是同样签名的函数值。 */
static bool replacement_ok(sema_t *sema, const type_t *target,
                           value_t *repl, ast_node_t *loc, const char *what) {
    const type_t *rt = repl ? value_type(repl) : NULL;
    rt = rt ? type_strip_quals(rt) : NULL;
    if (!rt || type_kind_of(rt) != TYPE_KIND_FUNC) {
        diag_error(sema->diag, sema_loc(sema, loc),
                   "%s: replacement must be a function value", what);
        return false;
    }
    const type_t *tt = target ? type_strip_quals(target) : NULL;
    if (!tt || type_kind_of(tt) != TYPE_KIND_FUNC) return false;
    if (!type_equal(sema->vm, tt, rt)) {
        diag_error(sema->diag, sema_loc(sema, loc),
                   "%s: replacement signature must match the target "
                   "(parameters and return type)",
                   what);
        return false;
    }
    return true;
}

/* struct 方法查找：返回成员符号（SEMA_SYM_FUNC），并回填
   **方法定义所在** struct 的类型 id（C 后端按 `__method_<id>_<name>` 编码）。 */
static sema_symbol_t *find_method(sema_t *sema, ast_node_t *type_expr,
                                  strslice_t mname, ast_node_t *loc,
                                  const char **type_name_out,
                                  uint32_t *owner_type_id) {
    const type_t *t = resolve_type_expr(sema, type_expr);
    if (!t) return NULL;
    const type_t *core = type_strip_quals(t);
    if (!core || type_kind_of(core) != TYPE_KIND_STRUCT) {
        diag_error(sema->diag, sema_loc(sema, loc),
                   "mock_method: first argument must be a struct type");
        return NULL;
    }
    if (type_name_out) *type_name_out = core->name.ptr;
    const struct_type_t *owner = NULL;
    sema_symbol_t *ms =
        sema_struct_find_member(sema, core, mname, loc, &owner);
    if (owner_type_id) {
        const type_t *ot = owner ? (const type_t *)owner : core;
        const sema_type_t *st = sema_type_find(sema, ot);
        *owner_type_id = st ? st->id : 0;
    }
    return ms;
}

/* ---- 入口 ---- */

bool sema_mock_check_call(sema_t *sema, ast_call_t *call, sema_builtin_op_t op,
                          value_t *const *arg_shadows, size_t argc) {
    if (!sema || !call) return false;
    ast_node_t *loc = (ast_node_t *)call;
    const char *what = op == SEMA_BUILTIN_MOCK_METHOD ||
                               op == SEMA_BUILTIN_UNMOCK_METHOD
                           ? "mock_method"
                           : "mock";

    if (op == SEMA_BUILTIN_MOCK || op == SEMA_BUILTIN_UNMOCK) {
        const bool has_repl = op == SEMA_BUILTIN_MOCK;
        const size_t want = has_repl ? 2 : 1;
        if (argc != want) {
            diag_error(sema->diag, sema_loc(sema, loc),
                       has_repl
                           ? "mock: expects exactly 2 arguments "
                             "(name, replacement)"
                           : "unmock: expects exactly 1 argument (name)");
            return false;
        }
        strslice_t spec = STRSLICE_EMPTY;
        if (!literal_name(call->args, &spec)) {
            diag_error(sema->diag, sema_loc(sema, loc),
                       "%s: the target name must be a string literal "
                       "(it is resolved at compile time)",
                       what);
            return false;
        }
        strslice_t name = STRSLICE_EMPTY;
        sema_symbol_t *sym = resolve_free(sema, spec, &name, call->args, what);
        if (!sym) return false;
        if (sym->kind != SEMA_SYM_FUNC || !sym->ast ||
            sym->ast->kind != AST_FUNC_DEF) {
            diag_error(sema->diag, sema_loc(sema, call->args),
                       "%s: '%.*s' is not a function", what, (int)spec.len,
                       spec.ptr);
            return false;
        }
        ast_func_def_t *def = (ast_func_def_t *)sym->ast;
        if (sym->is_generic || def->generic.count > 0) {
            diag_error(sema->diag, sema_loc(sema, call->args),
                       "%s: generic function '%.*s' cannot be mocked", what,
                       (int)spec.len, spec.ptr);
            return false;
        }
        if (has_repl && !replacement_ok(sema, sym->type, arg_shadows[1],
                                        call->args->next, what))
            return false;
        call->mock_op = has_repl ? CLUX_MOCK_FREE : CLUX_UNMOCK_FREE;
        call->mock_fn_name = def->name;
        call->mock_owner_module =
            def->owner_module ? def->owner_module : sema->current_module;
        return true;
    }

    /* ---- 方法 mock ---- */
    const bool has_repl = op == SEMA_BUILTIN_MOCK_METHOD;
    const size_t want = has_repl ? 3 : 2;
    if (argc != want) {
        diag_error(sema->diag, sema_loc(sema, loc),
                   has_repl ? "mock_method: expects exactly 3 arguments "
                              "(type, method name, replacement)"
                            : "unmock_method: expects exactly 2 arguments "
                              "(type, method name)");
        return false;
    }
    ast_node_t *type_arg = call->args;
    ast_node_t *name_arg = type_arg ? type_arg->next : NULL;
    ast_node_t *repl_arg = name_arg ? name_arg->next : NULL;
    strslice_t mname = STRSLICE_EMPTY;
    if (!literal_name(name_arg, &mname)) {
        diag_error(sema->diag, sema_loc(sema, name_arg ? name_arg : loc),
                   "%s: the method name must be a string literal "
                   "(it is resolved at compile time)",
                   what);
        return false;
    }
    const char *tname = NULL;
    uint32_t owner_id = 0;
    sema_symbol_t *ms =
        find_method(sema, type_arg, mname, type_arg, &tname, &owner_id);
    if (!ms) return false;
    if (ms->kind != SEMA_SYM_FUNC || !ms->ast || ms->ast->kind != AST_FUNC_DEF) {
        diag_error(sema->diag, sema_loc(sema, type_arg),
                   "%s: struct '%s' has no method '%.*s'", what,
                   tname ? tname : "?", (int)mname.len, mname.ptr);
        return false;
    }
    ast_func_def_t *mdef = (ast_func_def_t *)ms->ast;
    if (mdef->generic.count > 0) {
        diag_error(sema->diag, sema_loc(sema, type_arg),
                   "%s: generic method '%.*s' cannot be mocked "
                   "(mock the concrete instance type instead)",
                   what, (int)mname.len, mname.ptr);
        return false;
    }
    if (has_repl && !replacement_ok(sema, ms->type, arg_shadows[2], repl_arg, what))
        return false;
    call->mock_op = has_repl ? CLUX_MOCK_METHOD : CLUX_UNMOCK_METHOD;
    call->mock_fn_name = mdef->name;
    call->mock_type_id = owner_id;
    call->mock_owner_module =
        mdef->owner_module ? mdef->owner_module : sema->current_module;
    return true;
}
