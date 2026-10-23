#include "cbackend/c_lower.h"

#include "parser/ast_assign.h"
#include "parser/ast_binary.h"
#include "parser/ast_block.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_if.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_return.h"
#include "parser/ast_unary.h"
#include "parser/ast_var_def.h"
#include "parser/ast_while.h"
#include "parser/lexer.h"
#include "vm/type.h"
#include "vm/vm.h"

#include <stdio.h>
#include <string.h>

typedef struct lower_ctx {
    arena_t *arena;
    sema_t  *sema;
    bool     failed;
} lower_ctx_t;

static const char *copy_arena(arena_t *arena, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)arena_alloc(arena, n + 1, 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static const char *mangle(arena_t *arena, const char *prefix, strslice_t name) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s%.*s", prefix, (int)name.len,
                     name.ptr ? name.ptr : "");
    if (n <= 0 || (size_t)n >= sizeof buf) return NULL;
    char *p = (char *)arena_alloc(arena, (size_t)n + 1, 1);
    if (!p) return NULL;
    memcpy(p, buf, (size_t)n + 1);
    return p;
}

static const char *token_text(arena_t *arena, const token_t *tok) {
    size_t n = 0;
    const char *s = token_get_text(tok, &n);
    if (!s) return NULL;
    char *p = (char *)arena_alloc(arena, n + 1, 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static const type_t *strip(sema_t *sema, const type_t *t) {
    (void)sema;
    return t ? type_strip_quals(t) : NULL;
}

static cir_type_t *map_type(lower_ctx_t *lc, const type_t *t) {
    t = strip(lc->sema, t);
    if (!t) return cir_type_new(lc->arena, CIR_TYPE_VOID, "void");
    vm_t *vm = lc->sema->vm;
    if (t == vm->type_i32) return cir_type_new(lc->arena, CIR_TYPE_I32, "int32_t");
    if (t == vm->type_u32) return cir_type_new(lc->arena, CIR_TYPE_I32, "uint32_t");
    if (t == vm->type_usize) return cir_type_new(lc->arena, CIR_TYPE_USIZE, "size_t");
    if (t == vm->type_bool) return cir_type_new(lc->arena, CIR_TYPE_BOOL, "bool");
    if (t == vm->type_void) return cir_type_new(lc->arena, CIR_TYPE_VOID, "void");
    lc->failed = true;
    return NULL;
}

static cir_expr_t *lower_expr(lower_ctx_t *lc, ast_node_t *e);

static cir_expr_t *lower_call(lower_ctx_t *lc, ast_call_t *call) {
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

static cir_expr_t *lower_expr(lower_ctx_t *lc, ast_node_t *e) {
    if (!e || lc->failed) return NULL;
    switch (e->kind) {
    case AST_INT_LIT:
        return cir_expr_int(lc->arena, (int64_t)((ast_int_lit_t *)e)->value);
    case AST_BOOL_LIT:
        return cir_expr_bool(lc->arena, ((ast_bool_lit_t *)e)->value);
    case AST_IDENT: {
        const char *n = mangle(lc->arena, "clux_v_", ((ast_ident_t *)e)->name);
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
        const char *op = token_text(lc->arena, b->op);
        cir_expr_t *lhs = lower_expr(lc, b->lhs);
        cir_expr_t *rhs = lower_expr(lc, b->rhs);
        return op && lhs && rhs ? cir_expr_binary(lc->arena, op, lhs, rhs)
                                : NULL;
    }
    case AST_CALL:
        return lower_call(lc, (ast_call_t *)e);
    default:
        diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                   "c backend: unsupported expression");
        lc->failed = true;
        return NULL;
    }
}

static const type_t *infer_expr_type(lower_ctx_t *lc, ast_node_t *e) {
    if (!e) return NULL;
    vm_t *vm = lc->sema->vm;
    switch (e->kind) {
    case AST_INT_LIT: {
        ast_int_lit_t *n = (ast_int_lit_t *)e;
        if (strslice_eq(n->type, STRSLICE_LIT("i8"))) return vm->type_i8;
        if (strslice_eq(n->type, STRSLICE_LIT("i16"))) return vm->type_i16;
        if (strslice_eq(n->type, STRSLICE_LIT("i64"))) return vm->type_i64;
        if (strslice_eq(n->type, STRSLICE_LIT("u8"))) return vm->type_u8;
        if (strslice_eq(n->type, STRSLICE_LIT("u16"))) return vm->type_u16;
        if (strslice_eq(n->type, STRSLICE_LIT("u32"))) return vm->type_u32;
        if (strslice_eq(n->type, STRSLICE_LIT("u64"))) return vm->type_u64;
        if (strslice_eq(n->type, STRSLICE_LIT("usize"))) return vm->type_usize;
        return vm->type_i32;
    }
    case AST_BOOL_LIT:
        return vm->type_bool;
    case AST_UNARY:
        return infer_expr_type(lc, ((ast_unary_t *)e)->operand);
    case AST_BINARY: {
        ast_binary_t *b = (ast_binary_t *)e;
        if (token_is(b->op, "==") || token_is(b->op, "!=") ||
            token_is(b->op, "<") || token_is(b->op, "<=") ||
            token_is(b->op, ">") || token_is(b->op, ">=") ||
            token_is(b->op, "&&") || token_is(b->op, "||"))
            return vm->type_bool;
        const type_t *lt = infer_expr_type(lc, b->lhs);
        return lt ? lt : infer_expr_type(lc, b->rhs);
    }
    default:
        return NULL;
    }
}

static cir_stmt_t *lower_block(lower_ctx_t *lc, ast_node_t *block,
                               bool *ok);

static cir_stmt_t *lower_stmt(lower_ctx_t *lc, ast_node_t *s, bool *ok) {
    if (!s || lc->failed) return NULL;
    switch (s->kind) {
    case AST_VAR_DEF: {
        ast_var_def_t *vd = (ast_var_def_t *)s;
        const type_t *t = vd->resolved_type;
        if (!t && vd->type_expr) t = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        if (!t || type_kind_of(t) == TYPE_KIND_VOID)
            t = infer_expr_type(lc, vd->init);
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
        if (vd->init && vd->init->kind != AST_UNDEF)
            st->expr = lower_expr(lc, vd->init);
        return st;
    }
    case AST_ASSIGN: {
        ast_assign_t *a = (ast_assign_t *)s;
        if (!a->target || a->target->kind != AST_IDENT) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                       "c backend: only simple identifier assignment is supported");
            *ok = false;
            return NULL;
        }
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_ASSIGN);
        if (!st) return NULL;
        st->name = mangle(lc->arena, "clux_v_",
                          ((ast_ident_t *)a->target)->name);
        st->op = a->op ? token_text(lc->arena, a->op) : "=";
        st->expr = lower_expr(lc, a->value);
        return st;
    }
    case AST_EXPR_STMT: {
        ast_expr_stmt_t *es = (ast_expr_stmt_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_EXPR);
        if (!st) return NULL;
        st->expr = lower_expr(lc, es->expr);
        return st;
    }
    case AST_RETURN: {
        ast_return_t *r = (ast_return_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_RETURN);
        if (!st) return NULL;
        if (r->value) st->expr = lower_expr(lc, r->value);
        return st;
    }
    case AST_BREAK: {
        return cir_stmt_new(lc->arena, CIR_STMT_BREAK);
    }
    case AST_CONTINUE: {
        return cir_stmt_new(lc->arena, CIR_STMT_CONTINUE);
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
    case AST_EMPTY_STMT:
        return NULL;
    default:
        diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                   "c backend: unsupported statement");
        *ok = false;
        return NULL;
    }
}

static cir_stmt_t *lower_block(lower_ctx_t *lc, ast_node_t *block, bool *ok) {
    if (!block || block->kind != AST_BLOCK) {
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

static ast_func_def_t *find_main(ast_program_t *prog) {
    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_FUNC_DEF) continue;
        ast_func_def_t *fn = (ast_func_def_t *)n;
        if (strslice_eq(fn->name, STRSLICE_LIT("main"))) return fn;
    }
    return NULL;
}

static cir_function_t *lower_function(lower_ctx_t *lc, ast_func_def_t *fn,
                                      bool is_entry) {
    if (fn->generic.count > 0 || fn->is_cfunc || fn->is_extern_cfunc) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)fn),
                   "c backend: unsupported function kind");
        lc->failed = true;
        return NULL;
    }
    const type_t *ret = lc->sema->vm->type_void;
    if (fn->return_expr)
        ret = sema_resolve_type_slot(lc->sema, &fn->return_expr);
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
    }

    bool ok = true;
    cir_stmt_t *body = lower_block(lc, fn->body, &ok);
    if (!ok) return NULL;

    const char *cname = mangle(lc->arena, "clux_fn_", fn->name);
    const char *obj = mangle(lc->arena, "clux_obj_", fn->name);
    const char *tag = mangle(lc->arena, "clux_sig_", fn->name);
    if (!cname || !obj || !tag) return NULL;
    cir_signature_t *sig =
        cir_signature_new(lc->arena, tag, cret, pc, ptypes);
    if (!sig) return NULL;
    int line = sema_loc(lc->sema, (ast_node_t *)fn).begin.line;
    return cir_function_new(lc->arena, cname, obj, sig, pc, params, body, line,
                            is_entry);
}

cir_unit_t *cbackend_lower(arena_t *arena, sema_t *sema, ast_program_t *prog,
                           const char *source_path) {
    if (!arena || !sema || !prog || !source_path) return NULL;
    ast_func_def_t *main_fn = find_main(prog);
    if (!main_fn) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)prog),
                   "c backend: no 'main' function");
        return NULL;
    }
    size_t param_count = 0;
    for (ast_node_t *p = main_fn->params; p; p = p->next) param_count++;
    if (param_count != 0) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)main_fn),
                   "c backend: 'main' must not take parameters");
        return NULL;
    }
    if (main_fn->return_expr) {
        const type_t *rt = sema_resolve_type_slot(sema, &main_fn->return_expr);
        if (rt != sema->vm->type_i32 && rt != sema->vm->type_void) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)main_fn),
                       "c backend: 'main' must return i32 or void");
            return NULL;
        }
    }

    lower_ctx_t lc = {.arena = arena, .sema = sema, .failed = false};
    cir_unit_t *u = cir_unit_new(arena, source_path);
    if (!u) return NULL;
    cir_unit_add_include(u, arena, "stdint.h");
    cir_unit_add_include(u, arena, "stddef.h");
    cir_unit_add_include(u, arena, "stdbool.h");

    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_FUNC_DEF) continue;
        ast_func_def_t *fn = (ast_func_def_t *)n;
        if (strslice_eq(fn->name, STRSLICE_LIT("main"))) continue;
        if (fn->generic.count > 0 || fn->is_cfunc || fn->is_extern_cfunc)
            continue;
        cir_function_t *cf = lower_function(&lc, fn, false);
        if (!cf || lc.failed) return NULL;
        cir_unit_add_signature(u, cf->sig);
        cir_unit_add_function(u, cf);
    }
    cir_function_t *cm = lower_function(&lc, main_fn, true);
    if (!cm || lc.failed) return NULL;
    cir_unit_add_signature(u, cm->sig);
    cir_unit_add_function(u, cm);
    return u;
}
