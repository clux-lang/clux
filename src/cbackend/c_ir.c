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

cir_type_t *cir_type_find(const cir_unit_t *u, const void *src,
                          cir_type_kind_t kind) {
    if (!u || !src) return NULL;
    for (cir_type_t *t = u->types; t; t = t->next)
        if (t->src == src && t->kind == kind) return t;
    return NULL;
}

cir_field_t *cir_field_new(arena_t *arena, const char *name,
                           cir_type_t *type) {
    cir_field_t *f = (cir_field_t *)arena_calloc(
        arena, 1, sizeof(*f), ALIGNOF(max_align_t));
    if (!f) return NULL;
    f->name = name;
    f->type = type;
    return f;
}

void cir_type_add_field(cir_type_t *t, cir_field_t *f) {
    if (!t || !f) return;
    f->next = NULL;
    if (!t->fields) {
        t->fields = f;
        return;
    }
    cir_field_t *p = t->fields;
    while (p->next) p = p->next;
    p->next = f;
}

void cir_unit_add_type(cir_unit_t *u, cir_type_t *t) {
    if (!u || !t) return;
    t->next = u->types;
    u->types = t;
}

cir_expr_t *cir_expr_int(arena_t *arena, int64_t value) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_INT;
    e->int_value = value;
    return e;
}

cir_expr_t *cir_expr_bool(arena_t *arena, bool value) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_BOOL;
    e->bool_value = value;
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

cir_expr_t *cir_expr_call(arena_t *arena, const char *callee,
                          cir_expr_t *args) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_CALL;
    e->text = callee;
    e->args = args;
    return e;
}

cir_expr_t *cir_expr_assign(arena_t *arena, cir_expr_t *target,
                            const char *op, cir_expr_t *value) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_ASSIGN;
    e->lhs = target;
    e->op = op;
    e->rhs = value;
    return e;
}

cir_expr_t *cir_expr_member(arena_t *arena, cir_expr_t *object,
                            const char *field) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_MEMBER;
    e->lhs = object;
    e->text = field;
    return e;
}

cir_expr_t *cir_expr_index(arena_t *arena, cir_expr_t *object,
                           cir_expr_t *index, bool array_data) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_INDEX;
    e->lhs = object;
    e->rhs = index;
    e->array_data = array_data;
    return e;
}

cir_init_t *cir_init_new(arena_t *arena, const char *name,
                         cir_expr_t *value) {
    cir_init_t *i = (cir_init_t *)arena_calloc(
        arena, 1, sizeof(*i), ALIGNOF(max_align_t));
    if (!i) return NULL;
    i->name = name;
    i->value = value;
    return i;
}

void cir_expr_add_init(cir_expr_t *construct, cir_init_t *init) {
    if (!construct || !init) return;
    init->next = NULL;
    if (!construct->inits) {
        construct->inits = init;
        return;
    }
    cir_init_t *p = construct->inits;
    while (p->next) p = p->next;
    p->next = init;
}

cir_expr_t *cir_expr_construct(arena_t *arena, cir_type_t *type,
                               cir_init_t *inits) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_CONSTRUCT;
    e->type = type;
    e->inits = inits;
    return e;
}

cir_expr_t *cir_expr_new(arena_t *arena, cir_type_t *type,
                         cir_init_t *inits) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_NEW;
    e->type = type;
    e->inits = inits;
    return e;
}

cir_expr_t *cir_expr_slice(arena_t *arena, cir_type_t *type,
                           cir_expr_t *object, cir_expr_t *low,
                           cir_expr_t *high) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_SLICE;
    e->type = type;
    e->lhs = object;
    e->rhs = low;
    e->high = high;
    return e;
}

cir_expr_t *cir_expr_len(arena_t *arena, cir_expr_t *object,
                         cir_type_t *object_type) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_LEN;
    e->lhs = object;
    e->type = object_type;
    return e;
}

cir_expr_t *cir_expr_make(arena_t *arena, cir_type_t *slice_type,
                          cir_type_t *elem_type, cir_expr_t *length,
                          cir_init_t *inits) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_MAKE;
    e->type = slice_type;
    e->elem = elem_type;
    e->lhs = length;
    e->inits = inits;
    return e;
}

cir_expr_t *cir_expr_slice_data(arena_t *arena, cir_expr_t *object) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_SLICE_DATA;
    e->lhs = object;
    return e;
}

cir_expr_t *cir_expr_string(arena_t *arena, const char *data, size_t len) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_STRING;
    e->text = data;
    e->text_len = len;
    return e;
}

void cir_expr_append_arg(cir_expr_t *call, cir_expr_t *arg) {
    if (!call || !arg) return;
    arg->next = NULL;
    if (!call->args) {
        call->args = arg;
        return;
    }
    cir_expr_t *t = call->args;
    while (t->next) t = t->next;
    t->next = arg;
}

cir_stmt_t *cir_stmt_new(arena_t *arena, cir_stmt_kind_t kind) {
    cir_stmt_t *s = (cir_stmt_t *)arena_calloc(
        arena, 1, sizeof(*s), ALIGNOF(max_align_t));
    if (!s) return NULL;
    s->kind = kind;
    return s;
}

void cir_stmt_append(cir_stmt_t *block, cir_stmt_t *stmt) {
    if (!block || !stmt) return;
    stmt->next = NULL;
    if (!block->body) {
        block->body = stmt;
        return;
    }
    cir_stmt_t *t = block->body;
    while (t->next) t = t->next;
    t->next = stmt;
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
                                 cir_stmt_t *body, int source_line,
                                 bool is_entry) {
    cir_function_t *f = (cir_function_t *)arena_calloc(
        arena, 1, sizeof(*f), ALIGNOF(max_align_t));
    if (!f) return NULL;
    f->name = name;
    f->object = object;
    f->sig = sig;
    f->param_count = param_count;
    f->params = params;
    f->body = body;
    f->source_line = source_line;    f->is_entry = is_entry;
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

void cir_unit_add_global(cir_unit_t *u, cir_global_t *g) {
    if (!u || !g) return;
    g->next = u->globals;
    u->globals = g;
}
