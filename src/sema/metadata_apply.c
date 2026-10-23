#include "sema/metadata.h"
#include "sema/sema.h"
#include "sema/comptime.h"
#include "parser/ast_scope_ref.h"
#include "sema_expr_internal.h"
#include "vm/type_error.h"
#include "ctfe/ctfe.h"
#include "parser/ast_call.h"
#include "parser/ast_func_def.h"
#include "parser/ast_generic_param.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_type_def.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_union_def.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_program.h"
#include "parser/ast_struct_def.h"
#include "vm/scope.h"
#include "vm/type.h"
#include "vm/type_type.h"
#include "vm/value.h"

#include <string.h>
#include <stdio.h>

static bool meta_is_call(const ast_node_t *d, strslice_t *key,
                         ast_node_t **value) {
    if (!d || d->kind != AST_CALL) return false;
    const ast_call_t *c = (const ast_call_t *)d;
    if (!c->callee || c->callee->kind != AST_IDENT ||
        !strslice_eq(((const ast_ident_t *)c->callee)->name,
                     STRSLICE_LIT("metadata")))
        return false;
    if (!c->args || c->args->kind != AST_STRING_LIT || !c->args->next)
        return false;
    *key = ((const ast_string_lit_t *)c->args)->text;
    if (value) *value = c->args->next;
    return true;
}

static value_t *eval_const_expr(sema_t *sema, ast_node_t *expr,
                                sema_scope_t *scope) {
    ctfe_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.vm = sema->vm;
    ctx.sema = sema;
    ctx.sema_scope = scope;
    ctx.budget = 100000;
    ctx.max_depth = 128;
    return ctfe_eval(&ctx, expr);
}

static bool add_meta_decorator(sema_t *sema, const type_t *type,
                               sema_meta_kind_t kind, strslice_t owner,
                               ast_node_t *decorator, sema_scope_t *scope) {
    strslice_t key;
    ast_node_t *value = NULL;
    if (!meta_is_call(decorator, &key, &value) || !value) return false;
    sema_type_t *st = (sema_type_t *)sema_type_find(sema, type);
    if (!st) {
        diag_error(sema->diag, sema_loc(sema, decorator),
                   "metadata target type is not registered");
        return true;
    }
    value_t *v = eval_const_expr(sema, value, scope);
    if (!v || value_is_error(sema->vm, v)) {
        diag_error(sema->diag, sema_loc(sema, value),
                   "metadata value is not a compile-time constant");
        return true;
    }
    sema_ct_const_t ct;
    if (!sema_ct_encode(sema, v, &ct)) {
        char tn[64];
        sema_type_name(value_type(v), tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, value),
                   "metadata value of type %s cannot be stored at runtime", tn);
        return true;
    }
    if (!sema_meta_add(sema, st, kind, owner, key, &ct)) {
        diag_error(sema->diag, sema_loc(sema, decorator),
                   "duplicate metadata key '%.*s'", (int)key.len, key.ptr);
    }
    return true;
}

static bool is_type_decorator(const ast_node_t *d) {
    if (!d) return false;
    if (d->kind == AST_IDENT) return true;
    if (d->kind == AST_CALL) {
        const ast_call_t *c = (const ast_call_t *)d;
        return c->callee && c->callee->kind == AST_IDENT;
    }
    return false;
}

static value_t *make_type_value(vm_t *vm, const type_t *t) {
    const type_t **data = (const type_t **)value_alloc_data(vm->alloc, vm->type_type);
    if (!data) return NULL;
    *data = t;
    return value_make(vm, vm->type_type, data);
}

/* 执行类型装饰器：[[controller("url")]] => controller[Target]("url")。
   这是编译期调用，不生成运行期代码。 */
static bool invoke_type_decorator(sema_t *sema, const type_t *target,
                                  ast_node_t *decorator, sema_scope_t *scope) {
    strslice_t fname;
    ast_node_t *args = NULL;
    if (decorator->kind == AST_IDENT) {
        fname = ((ast_ident_t *)decorator)->name;
    } else if (decorator->kind == AST_CALL) {
        ast_call_t *call = (ast_call_t *)decorator;
        if (!call->callee || call->callee->kind != AST_IDENT) {
            diag_error(sema->diag, sema_loc(sema, decorator),
                       "type decorator must be a function name or call");
            return true;
        }
        fname = ((ast_ident_t *)call->callee)->name;
        args = call->args;
    } else {
        return true;
    }
    sema_symbol_t *sym = sema_lookup(scope, fname);
    if (!sym || sym->kind != SEMA_SYM_FUNC || !sym->ast ||
        sym->ast->kind != AST_FUNC_DEF) {
        diag_error(sema->diag, sema_loc(sema, decorator),
                   "unknown type decorator function '%.*s'",
                   (int)fname.len, fname.ptr);
        return true;
    }
    ast_func_def_t *fn = (ast_func_def_t *)sym->ast;
    if (!fn->is_comptime) {
        diag_error(sema->diag, sema_loc(sema, decorator),
                   "type decorator '%.*s' must be a comptime function",
                   (int)fname.len, fname.ptr);
        return true;
    }
    if (fn->generic.count != 1 || !fn->generic.first) {
        diag_error(sema->diag, sema_loc(sema, decorator),
                   "type decorator '%.*s' must have exactly one generic type "
                   "parameter",
                   (int)fname.len, fname.ptr);
        return true;
    }
    ast_generic_param_t *gp = (ast_generic_param_t *)fn->generic.first;
    if (gp->is_pack || gp->is_comptime) {
        diag_error(sema->diag, sema_loc(sema, decorator),
                   "type decorator generic parameter must be a type parameter");
        return true;
    }
    if (fn->return_expr) {
        /* 返回类型声明为 void 时 return_expr 为 NULL；非 NULL 仅用于诊断。 */
    }

    vm_t *vm = sema->vm;
    scope_t *saved = vm->current_scope;
    scope_t *type_scope = scope_new(vm->alloc, saved);
    if (!type_scope) return true;
    char nb[128];
    size_t n = gp->name.len < sizeof nb - 1 ? gp->name.len : sizeof nb - 1;
    memcpy(nb, gp->name.ptr, n);
    nb[n] = '\0';
    vm->current_scope = type_scope;
    value_t *tv = make_type_value(vm, target);
    if (!tv) {
        scope_destroy(vm, &type_scope);
        vm->current_scope = saved;
        return true;
    }
    value_t *stored = scope_define(vm, type_scope, nb, tv);
    if (!stored || value_is_error(vm, stored)) {
        scope_destroy(vm, &type_scope);
        vm->current_scope = saved;
        return true;
    }

    ctfe_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.vm = vm;
    ctx.sema = sema;
    ctx.sema_scope = scope;
    ctx.budget = 100000;
    ctx.max_depth = 128;
    value_t *r = ctfe_call_ast_fn(&ctx, fn, args);
    vm->current_scope = saved;
    scope_destroy(vm, &type_scope);
    if (!r || value_is_error(vm, r)) {
        const char *msg = NULL;
        if (r) {
            error_data_t *ed = (error_data_t *)value_data(r);
            msg = ed ? ed->message : NULL;
        }
        diag_error(sema->diag, sema_loc(sema, decorator),
                   "type decorator '%.*s' failed%s%s", (int)fname.len,
                   fname.ptr, msg ? ": " : "", msg ? msg : "");
    }
    return true;
}

void sema_metadata_apply_type(sema_t *sema, const type_t *type,
                              ast_node_t *decorators, sema_scope_t *scope) {
    for (ast_node_t *d = decorators; d; d = d->next) {
        if (meta_is_call(d, &(strslice_t){0}, NULL)) {
            add_meta_decorator(sema, type, SEMA_META_TYPE, STRSLICE_EMPTY, d,
                               scope);
        } else if (is_type_decorator(d)) {
            invoke_type_decorator(sema, type, d, scope);
        } else {
            diag_error(sema->diag, sema_loc(sema, d),
                       "unsupported type decorator");
        }
    }
}

void sema_metadata_apply_members(sema_t *sema, const type_t *type,
                                 ast_struct_def_t *sd, sema_scope_t *scope) {
    for (ast_node_t *f = sd->fields; f; f = f->next) {
        ast_struct_field_t *sf = (ast_struct_field_t *)f;
        for (ast_node_t *d = sf->metadata_decorators; d; d = d->next)
            add_meta_decorator(sema, type, SEMA_META_FIELD, sf->name, d, scope);
        for (ast_node_t *d = sf->decorators; d; d = d->next) {
            if (meta_is_call(d, &(strslice_t){0}, NULL))
                add_meta_decorator(sema, type, SEMA_META_FIELD, sf->name, d,
                                   scope);
            else
                diag_error(sema->diag, sema_loc(sema, d),
                           "field decorators only support metadata()");
        }
    }
    for (ast_node_t *m = sd->members; m; m = m->next) {
        if (m->kind != AST_FUNC_DEF) continue;
        ast_func_def_t *fn = (ast_func_def_t *)m;
        for (ast_node_t *d = fn->metadata_decorators; d; d = d->next)
            add_meta_decorator(sema, type, SEMA_META_METHOD, fn->name, d, scope);
    }
}

/* 编译期/运行期 get_metadata 共用的查找：返回条目。 */
const sema_meta_entry_t *sema_metadata_lookup(sema_t *sema,
                                              const type_t *type,
                                              sema_meta_kind_t kind,
                                              strslice_t owner,
                                              strslice_t key) {
    sema_type_t *st = (sema_type_t *)sema_type_find(sema, type);
    return sema_meta_find(st, kind, owner, key);
}

void sema_metadata_apply_all(sema_t *sema, ast_program_t *prog) {
    if (!sema || !prog || !sema->global_scope) return;
    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind == AST_STRUCT_DEF) {
            ast_struct_def_t *sd = (ast_struct_def_t *)n;
            sema_symbol_t *sym =
                sema_scope_find_local(sema->global_scope, sd->name);
            if (!sym || !sym->type) continue;
            sema_metadata_apply_members(sema, sym->type, sd,
                                        sema->global_scope);
            sema_metadata_apply_type(sema, sym->type, sd->decorators,
                                     sema->global_scope);
        } else if (n->kind == AST_TYPE_DEF) {
            ast_type_def_t *td = (ast_type_def_t *)n;
            sema_symbol_t *sym =
                sema_scope_find_local(sema->global_scope, td->name);
            if (!sym || !sym->type) continue;
            sema_metadata_apply_type(sema, sym->type, td->decorators,
                                     sema->global_scope);
        } else if (n->kind == AST_ENUM_DEF) {
            ast_enum_def_t *ed = (ast_enum_def_t *)n;
            sema_symbol_t *sym =
                sema_scope_find_local(sema->global_scope, ed->name);
            if (!sym || !sym->type) continue;
            sema_metadata_apply_type(sema, sym->type, ed->decorators,
                                     sema->global_scope);
        } else if (n->kind == AST_UNION_DEF) {
            ast_union_def_t *ud = (ast_union_def_t *)n;
            sema_symbol_t *sym =
                sema_scope_find_local(sema->global_scope, ud->name);
            if (!sym || !sym->type) continue;
            sema_metadata_apply_type(sema, sym->type, ud->decorators,
                                     sema->global_scope);
        } else if (n->kind == AST_CUNION_DEF) {
            ast_cunion_def_t *cd = (ast_cunion_def_t *)n;
            sema_symbol_t *sym =
                sema_scope_find_local(sema->global_scope, cd->name);
            if (!sym || !sym->type) continue;
            sema_metadata_apply_type(sema, sym->type, cd->decorators,
                                     sema->global_scope);
        }
    }
}

bool sema_metadata_try_get(sema_t *sema, ast_node_t **node,
                           sema_scope_t *scope, value_t **out) {    if (!node || !*node || (*node)->kind != AST_CALL) return false;
    ast_call_t *call = (ast_call_t *)*node;
    if (!call->callee || call->callee->kind != AST_INDEX) return false;
    ast_index_t *ix = (ast_index_t *)call->callee;
    if (!ix->object || ix->object->kind != AST_IDENT || !ix->indices)
        return false;
    strslice_t fname = ((ast_ident_t *)ix->object)->name;
    sema_meta_kind_t kind;
    size_t argc;
    if (strslice_eq(fname, STRSLICE_LIT("get_metadata"))) {
        kind = SEMA_META_TYPE;
        argc = 1;
    } else if (strslice_eq(fname, STRSLICE_LIT("get_field_metadata"))) {
        kind = SEMA_META_FIELD;
        argc = 2;
    } else if (strslice_eq(fname, STRSLICE_LIT("get_method_metadata"))) {
        kind = SEMA_META_METHOD;
        argc = 2;
    } else {
        return false;
    }
    size_t n = 0;
    for (ast_node_t *a = call->args; a; a = a->next) n++;
    if (n != argc) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "%.*s expects %zu string argument(s)", (int)fname.len,
                   fname.ptr, argc);
        *out = val_void(sema);
        return true;
    }
    value_t *tv = sema_expr(sema, &ix->indices, scope);
    if (!tv || value_is_error(sema->vm, tv) ||
        !value_is_type_value(tv)) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "%.*s requires a concrete type argument", (int)fname.len,
                   fname.ptr);
        *out = val_void(sema);
        return true;
    }
    const type_t *target = value_as(tv, const type_t *);
    strslice_t owner = STRSLICE_EMPTY, key = STRSLICE_EMPTY;
    ast_node_t *a = call->args;
    if (kind != SEMA_META_TYPE) {
        if (!a || a->kind != AST_STRING_LIT) {
            diag_error(sema->diag, sema_loc(sema, a ? a : *node),
                       "%.*s field/method name must be a string literal",
                       (int)fname.len, fname.ptr);
            *out = val_void(sema);
            return true;
        }
        owner = ((ast_string_lit_t *)a)->text;
        a = a->next;
    }
    if (!a || a->kind != AST_STRING_LIT) {
        diag_error(sema->diag, sema_loc(sema, a ? a : *node),
                   "%.*s key must be a string literal", (int)fname.len,
                   fname.ptr);
        *out = val_void(sema);
        return true;
    }
    key = ((ast_string_lit_t *)a)->text;
    const sema_meta_entry_t *e = sema_metadata_lookup(sema, target, kind, owner, key);
    if (!e) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "metadata '%.*s' not found on %.*s", (int)key.len, key.ptr,
                   (int)target->name.len, target->name.ptr);
        *out = val_void(sema);
        return true;
    }
    ast_node_t *lit = sema_ct_lit(sema, *node, &e->value);
    if (!lit) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "metadata '%.*s' cannot be folded", (int)key.len, key.ptr);
        *out = val_void(sema);
        return true;
    }
    lit->next = (*node)->next;
    *node = lit;
    *out = value_make_shadow(sema->vm, e->value.type);
    return true;
}
