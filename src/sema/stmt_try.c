#include "sema/sema.h"
#include "core/arena.h"

#include "parser/ast_assign.h"
#include "parser/ast_block.h"
#include "parser/ast_call.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_ident.h"
#include "parser/ast_if.h"
#include "parser/ast_index.h"
#include "parser/ast_deref.h"
#include "parser/ast_member.h"
#include "parser/ast_return.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_var_def.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_for.h"
#include "parser/ast_switch.h"
#include "parser/ast_while.h"
#include "vm/type_ownership.h"

#include "sema_stmt_internal.h"
#include "sema_expr_internal.h"
#include <stdio.h>
#include <string.h>

static ast_node_t *member_call(sema_t *sema, ast_node_t *operand,
                               const char *name, uint32_t tb, uint32_t te) {
    ast_member_t *m = (ast_member_t *)ast_member_new(sema->arena, tb, te);
    ast_node_t *call = ast_call_new(sema->arena, tb, te);
    if (!m || !call) return NULL;
    m->object = operand;
    m->field = strslice_from_cstr(name);
    ((ast_call_t *)call)->callee = &m->base;
    return call;
}

bool sema_try_analyze(sema_t *sema, ast_node_t *operand, sema_scope_t *scope,
                      bool panic_mode, sema_try_info_t *out) {
    if (!sema || !operand || !out) return false;
    memset(out, 0, sizeof(*out));
    uint32_t tb = operand->tok_begin, te = operand->tok_end;

    out->ok_call = member_call(sema, operand, "ok", tb, te);
    if (!out->ok_call) return false;
    value_t *okv = sema_expr(sema, &out->ok_call, scope);
    if (is_poison(sema, okv) || !value_type(okv) ||
        type_kind_of(value_type(okv)) != TYPE_KIND_BOOL)
        return false;

    out->value_call = member_call(sema, operand, "value", tb, te);
    if (!out->value_call) return false;
    value_t *vv = sema_expr(sema, &out->value_call, scope);
    if (is_poison(sema, vv) || !value_type(vv) ||
        value_is_type(vv, TYPE_KIND_VOID))
        return false;
    out->value_type = value_type(vv);

    if (panic_mode) {
        out->panic_call = member_call(sema, operand, "panic", tb, te);
        if (!out->panic_call) return false;
        value_t *pv = sema_expr(sema, &out->panic_call, scope);
        if (is_poison(sema, pv) || !value_type(pv) ||
            type_kind_of(value_type(pv)) != TYPE_KIND_STR)
            return false;
        ast_node_t *pcallee = ast_ident_new(sema->arena, tb, te);
        ast_node_t *pcall = ast_call_new(sema->arena, tb, te);
        if (!pcallee || !pcall) return false;
        ((ast_ident_t *)pcallee)->name = strslice_from_cstr("panic");
        ((ast_call_t *)pcall)->callee = pcallee;
        ((ast_call_t *)pcall)->args = out->panic_call;
        ((ast_call_t *)pcall)->args_last = out->panic_call;
        value_t *pr = sema_expr(sema, &pcall, scope);
        if (value_is_error(sema->vm, pr)) return false;
        ast_node_t *st = ast_expr_stmt_new(sema->arena, tb, te);
        if (!st) return false;
        ((ast_expr_stmt_t *)st)->expr = pcall;
        st->is_generated = true;
        out->tail_stmt = st;
        return true;
    }

    out->error_call = member_call(sema, operand, "error", tb, te);
    if (!out->error_call) return false;
    value_t *ev = sema_expr(sema, &out->error_call, scope);
    if (is_poison(sema, ev) || !value_type(ev)) return false;
    const type_t *et = value_type(ev);
    ownership_t eo = type_ownership_of(et);
    if (eo == OWN_OWN || eo == OWN_FATAL || type_has_own_slots(et)) {
        diag_error(sema->diag, sema_loc(sema, out->error_call),
                   "error value from 'error()' must be copyable; own/fatal "
                   "errors are not supported by 'a.?'");
        return false;
    }

    const type_t *ret = sema->fn.return_type;
    const type_t *ret_core = type_strip_ownership(ret);
    if (!ret_core || type_kind_of(ret_core) != TYPE_KIND_STRUCT) {
        diag_error(sema->diag, sema_loc(sema, operand),
                   "'a.?' requires the current function to return a struct "
                   "with 'of_error(error)'");
        return false;
    }
    ast_node_t *tref = ast_ident_new(sema->arena, tb, te);
    ast_scope_ref_t *sr =
        (ast_scope_ref_t *)ast_scope_ref_new(sema->arena, tb, te);
    ast_node_t *ocall = ast_call_new(sema->arena, tb, te);
    if (!tref || !sr || !ocall) return false;
    ((ast_ident_t *)tref)->name = ret_core->name;
    sr->type_expr = tref;
    sr->variant = strslice_from_cstr("of_error");
    ((ast_call_t *)ocall)->callee = &sr->base;
    ((ast_call_t *)ocall)->args = out->error_call;
    ((ast_call_t *)ocall)->args_last = out->error_call;
    value_t *ov = sema_expr(sema, &ocall, scope);
    if (is_poison(sema, ov) || !value_type(ov)) return false;
    value_t *dst = value_make_shadow(sema->vm, ret);
    if (value_is_error(sema->vm, value_assign(sema->vm, dst, ov))) {
        diag_error(sema->diag, sema_loc(sema, ocall),
                   "'of_error' result is not assignable to the current return "
                   "type");
        return false;
    }
    ast_node_t *retn = ast_return_new(sema->arena, tb, te);
    if (!retn) return false;
    ((ast_return_t *)retn)->value = ocall;
    retn->is_generated = true;
    /* 错误返回路径同样要释放当前作用域已拥有的局部资源。 */
    retn->defers = collect_free_stmts(sema, scope, find_func_stop_sema(scope));
    out->tail_stmt = retn;
    return true;
}

static bool try_place_expr(const ast_node_t *n) {
    return n && (n->kind == AST_IDENT || n->kind == AST_MEMBER ||
                 n->kind == AST_INDEX || n->kind == AST_DEREF ||
                 n->kind == AST_SCOPE_REF);
}

bool sema_try_probe(sema_t *sema, ast_node_t *unwrap, sema_scope_t *scope,
                    sema_try_info_t *out) {
    if (!sema || !unwrap || unwrap->kind != AST_UNWRAP || !out) return false;
    ast_unwrap_t *u = (ast_unwrap_t *)unwrap;
    if (!u->operand) return false;
    value_t *ov = sema_expr(sema, &u->operand, scope);
    if (is_poison(sema, ov) || !value_type(ov) ||
        type_kind_of(value_type(ov)) != TYPE_KIND_STRUCT)
        return false;
    const type_t *ot = value_type(ov);
    bool owning = type_ownership_of(ot) == OWN_OWN ||
                  type_ownership_of(ot) == OWN_FATAL ||
                  type_has_own_slots(ot);

    /* own result 的 place 左值：直接对它 take，不复制临时。
       （value() 会 take_opt(self.val)，消费槽位。） */
    if (owning && try_place_expr(u->operand)) {
        bool panic_mode = u->op && token_is(u->op, "!");
        if (!sema_try_analyze(sema, u->operand, scope, panic_mode, out))
            return false;
        out->setup = NULL;
        return true;
    }

    /* 左值表达式只求值一次：var __try_N: R = <operand>; */
    char nb[64], cn[64];
    uint32_t seq = sema->try_seq++;
    snprintf(nb, sizeof nb, "__try_%llu", (unsigned long long)seq);
    snprintf(cn, sizeof cn, "__try_%llu", (unsigned long long)seq);
    ast_var_def_t *tv = (ast_var_def_t *)ast_var_def_new(
        sema->arena, u->operand->tok_begin, u->operand->tok_end);
    if (!tv) return false;
    size_t nlen = strlen(nb);
    char *ncopy = (char *)arena_alloc(sema->arena, nlen, 1);
    if (!ncopy) return false;
    memcpy(ncopy, nb, nlen);
    tv->name = (strslice_t){ncopy, nlen};
    tv->type_expr = sema_gen_type_ref(sema, ot);
    tv->init = u->operand;
    tv->resolved_type = ot;
    tv->base.is_generated = true;
    if (!tv->type_expr) return false;

    sema_symbol_t tinit = {.kind = SEMA_SYM_VAR, .type = ot,
                           .is_active = true, .flow_init = true,
                           .declared = true};
    if (!sema_scope_define(scope, tv->name, &tinit)) return false;
    value_t *shadow = value_make_shadow(sema->vm, ot);
    if (shadow) scope_define(sema->vm, sema->vm->current_scope, cn, shadow);

    ast_node_t *tid = ast_ident_new(sema->arena, tv->base.tok_begin,
                                    tv->base.tok_end);
    if (!tid) return false;
    ((ast_ident_t *)tid)->name = tv->name;
    bool panic_mode = u->op && token_is(u->op, "!");
    if (!sema_try_analyze(sema, tid, scope, panic_mode, out)) return false;
    out->setup = &tv->base;
    return true;
}

ast_node_t *sema_try_make_if(sema_t *sema, const sema_try_info_t *info,
                             ast_node_t *target, ast_node_t *operand,
                             uint32_t tb, uint32_t te) {
    if (!sema || !info || !info->ok_call || !info->tail_stmt) return NULL;
    ast_node_t *iff = ast_if_new(sema->arena, tb, te);
    ast_node_t *thenb = ast_block_new(sema->arena, tb, te);
    ast_node_t *elseb = ast_block_new(sema->arena, tb, te);
    ast_node_t *as = ast_assign_new(sema->arena, tb, te);
    if (!iff || !thenb || !elseb || !as) return NULL;
    ((ast_assign_t *)as)->target = target;
    ((ast_assign_t *)as)->value = info->value_call;
    location_t loc = {0};
    ((ast_assign_t *)as)->op =
        (token_t *)arena_token_symbol(sema->arena, "=", &loc);
    as->is_generated = true;
    ((ast_block_t *)thenb)->stmts = as;
    ((ast_block_t *)thenb)->stmts_last = as;
    ((ast_block_t *)elseb)->stmts = info->tail_stmt;
    ((ast_block_t *)elseb)->stmts_last = info->tail_stmt;
    ((ast_if_t *)iff)->cond = info->ok_call;
    ((ast_if_t *)iff)->then_body = thenb;
    ((ast_if_t *)iff)->else_body = elseb;
    iff->is_generated = true;
    (void)operand;
    return iff;
}
