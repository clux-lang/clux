#include "cbackend/c_lower.h"

#include "parser/ast_binary.h"
#include "parser/ast_block.h"
#include "parser/ast_func_def.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_return.h"
#include "parser/ast_unary.h"
#include "parser/lexer.h"
#include "vm/type.h"
#include "vm/vm.h"

#include <stdio.h>
#include <string.h>

static const char *arena_str(arena_t *arena, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)arena_alloc(arena, n + 1, 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static cir_type_t *map_type(arena_t *arena, sema_t *sema, const type_t *t) {
    if (!t) return cir_type_new(arena, CIR_TYPE_VOID, "void");
    vm_t *vm = sema->vm;
    if (t == vm->type_i32) return cir_type_new(arena, CIR_TYPE_I32, "int32_t");
    if (t == vm->type_void) return cir_type_new(arena, CIR_TYPE_VOID, "void");
    if (t == vm->type_usize) return cir_type_new(arena, CIR_TYPE_USIZE, "size_t");
    if (t == vm->type_bool) return cir_type_new(arena, CIR_TYPE_BOOL, "bool");
    if (t == vm->type_u32) return cir_type_new(arena, CIR_TYPE_I32, "uint32_t");
    return NULL;
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

static cir_expr_t *lower_expr(arena_t *arena, sema_t *sema, ast_node_t *e) {
    if (!e) return NULL;
    switch (e->kind) {
    case AST_INT_LIT:
        return cir_expr_int(arena, (int64_t)((ast_int_lit_t *)e)->value);
    case AST_UNARY: {
        ast_unary_t *u = (ast_unary_t *)e;
        const char *op = token_text(arena, u->op);
        cir_expr_t *v = lower_expr(arena, sema, u->operand);
        if (!op || !v) return NULL;
        return cir_expr_unary(arena, op, v);
    }
    case AST_BINARY: {
        ast_binary_t *b = (ast_binary_t *)e;
        const char *op = token_text(arena, b->op);
        cir_expr_t *lhs = lower_expr(arena, sema, b->lhs);
        cir_expr_t *rhs = lower_expr(arena, sema, b->rhs);
        if (!op || !lhs || !rhs) return NULL;
        return cir_expr_binary(arena, op, lhs, rhs);
    }
    default:
        diag_error(sema->diag, sema_loc(sema, e),
                   "c backend: unsupported main return expression");
        return NULL;
    }
}

static ast_func_def_t *find_main(ast_program_t *prog) {
    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_FUNC_DEF) continue;
        ast_func_def_t *fn = (ast_func_def_t *)n;
        if (strslice_eq(fn->name, STRSLICE_LIT("main"))) return fn;
    }
    return NULL;
}

cir_unit_t *cbackend_lower(arena_t *arena, sema_t *sema, ast_program_t *prog,
                           const char *source_path) {
    if (!arena || !sema || !prog || !source_path) return NULL;
    ast_func_def_t *fn = find_main(prog);
    if (!fn) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)prog),
                   "c backend: no 'main' function");
        return NULL;
    }
    if (fn->generic.count > 0 || fn->is_cfunc || fn->is_extern_cfunc) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                   "c backend: 'main' must be a non-generic clux function");
        return NULL;
    }
    size_t param_count = 0;
    for (ast_node_t *p = fn->params; p; p = p->next) param_count++;
    if (param_count != 0) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                   "c backend: 'main' must not take parameters");
        return NULL;
    }

    const type_t *ret = sema->vm->type_void;
    if (fn->return_expr) {
        ret = sema_resolve_type_slot(sema, &fn->return_expr);
        if (!ret) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                       "c backend: cannot resolve 'main' return type");
            return NULL;
        }
    }
    if (ret != sema->vm->type_void && ret != sema->vm->type_i32) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                   "c backend: 'main' must return i32 or void");
        return NULL;
    }
    cir_type_t *cret = map_type(arena, sema, ret);
    if (!cret) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                   "c backend: unsupported 'main' return type");
        return NULL;
    }

    cir_expr_t *ret_expr = NULL;
    bool has_return = false;
    if (fn->body && fn->body->kind == AST_BLOCK) {
        for (ast_node_t *s = ((ast_block_t *)fn->body)->stmts; s; s = s->next) {
            if (s->kind != AST_RETURN) continue;
            has_return = true;
            ast_return_t *r = (ast_return_t *)s;
            if (r->value) {
                ret_expr = lower_expr(arena, sema, r->value);
                if (!ret_expr) return NULL;
            }
            break;
        }
    }
    if (!has_return && ret != sema->vm->type_void) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                   "c backend: non-void 'main' has no return");
        return NULL;
    }

    const char *sig_tag = "clux_fn_i32_void";
    if (ret == sema->vm->type_void) sig_tag = "clux_fn_void_void";
    cir_signature_t *sig =
        cir_signature_new(arena, sig_tag, cret, 0, NULL);
    if (!sig) return NULL;

    const char *cname = arena_str(arena, "clux_fn_main");
    const char *obj = arena_str(arena, "clux_obj_main");
    if (!cname || !obj) return NULL;
    int line = sema_loc(sema, (ast_node_t *)fn).begin.line;
    cir_function_t *cf = cir_function_new(arena, cname, obj, sig, 0, NULL,
                                          ret_expr, line, true);
    if (!cf) return NULL;

    cir_unit_t *u = cir_unit_new(arena, source_path);
    if (!u) return NULL;
    cir_unit_add_include(u, arena, "stdint.h");
    cir_unit_add_include(u, arena, "stddef.h");
    cir_unit_add_include(u, arena, "stdbool.h");
    cir_unit_add_signature(u, sig);
    cir_unit_add_function(u, cf);
    return u;
}
