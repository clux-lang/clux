#include "cbackend/c_ir.h"

#include <string.h>

cir_unit_t *cir_unit_new(arena_t *arena, const char *source_path) {
    cir_unit_t *u = (cir_unit_t *)arena_calloc(
        arena, 1, sizeof(*u), ALIGNOF(max_align_t));
    if (!u) return NULL;
    u->source_path = source_path;
    return u;
}

void cir_unit_add_include(cir_unit_t *u, arena_t *arena, const char *include) {
    if (!u || !include) return;
    size_t n = u->include_count + 1;
    const char **arr = (const char **)arena_alloc(
        arena, n * sizeof(*arr), ALIGNOF(max_align_t));
    if (!arr) return;
    if (u->include_count)
        memcpy(arr, u->includes, u->include_count * sizeof(*arr));
    arr[n - 1] = include;
    u->includes = arr;
    u->include_count = n;
}

cir_type_t *cir_type_new(arena_t *arena, cir_type_kind_t kind,
                         const char *c_name) {
    cir_type_t *t = (cir_type_t *)arena_calloc(
        arena, 1, sizeof(*t), ALIGNOF(max_align_t));
    if (!t) return NULL;
    t->kind = kind;
    t->c_name = c_name;
    return t;
}

cir_expr_t *cir_expr_int(arena_t *arena, int64_t value) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_INT;
    e->int_value = value;
    return e;
}

cir_expr_t *cir_expr_ident(arena_t *arena, const char *name) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_IDENT;
    e->text = name;
    return e;
}

cir_expr_t *cir_expr_unary(arena_t *arena, const char *op, cir_expr_t *v) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_UNARY;
    e->op = op;
    e->lhs = v;
    return e;
}

cir_expr_t *cir_expr_binary(arena_t *arena, const char *op, cir_expr_t *lhs,
                            cir_expr_t *rhs) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_BINARY;
    e->op = op;
    e->lhs = lhs;
    e->rhs = rhs;
    return e;
}

cir_signature_t *cir_signature_new(arena_t *arena, const char *tag,
                                   cir_type_t *ret, size_t param_count,
                                   cir_type_t **params) {
    cir_signature_t *s = (cir_signature_t *)arena_calloc(
        arena, 1, sizeof(*s), ALIGNOF(max_align_t));
    if (!s) return NULL;
    s->tag = tag;
    s->ret = ret;
    s->param_count = param_count;
    s->params = params;
    return s;
}

cir_function_t *cir_function_new(arena_t *arena, const char *name,
                                 const char *object, cir_signature_t *sig,
                                 size_t param_count, cir_param_t *params,
                                 cir_expr_t *return_expr, int source_line,
                                 bool is_entry) {
    cir_function_t *f = (cir_function_t *)arena_calloc(
        arena, 1, sizeof(*f), ALIGNOF(max_align_t));
    if (!f) return NULL;
    f->name = name;
    f->object = object;
    f->sig = sig;
    f->param_count = param_count;
    f->params = params;
    f->return_expr = return_expr;
    f->source_line = source_line;
    f->is_entry = is_entry;
    return f;
}

void cir_unit_add_signature(cir_unit_t *u, cir_signature_t *sig) {
    if (!u || !sig) return;
    sig->next = u->signatures;
    u->signatures = sig;
}

void cir_unit_add_function(cir_unit_t *u, cir_function_t *fn) {
    if (!u || !fn) return;
    fn->next = u->functions;
    u->functions = fn;
}
