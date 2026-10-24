#include "cbackend/c_ir.h"

#include <string.h>

cir_expr_t *cir_expr_int(arena_t *arena, int64_t value) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_INT;
    e->int_value = value;
    return e;
}

cir_expr_t *cir_expr_float(arena_t *arena, double value) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_FLOAT;
    e->float_value = value;
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

cir_expr_t *cir_expr_option(arena_t *arena, cir_type_t *type, bool some,
                            cir_expr_t *value) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_OPTION;
    e->type = type;
    e->bool_value = some;
    e->lhs = value;
    return e;
}

cir_expr_t *cir_expr_opt_is_some(arena_t *arena, cir_expr_t *object,
                                 bool want_some) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_OPT_IS_SOME;
    e->lhs = object;
    e->bool_value = want_some;
    return e;
}

cir_expr_t *cir_expr_unwrap(arena_t *arena, cir_expr_t *object) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_UNWRAP;
    e->lhs = object;
    return e;
}

cir_expr_t *cir_expr_union_construct(arena_t *arena, cir_type_t *type,
                                     size_t tag, const char *field,
                                     cir_expr_t *value) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_UNION_CONSTRUCT;
    e->type = type;
    e->tag = tag;
    e->text = field;
    e->lhs = value;
    return e;
}

cir_expr_t *cir_expr_union_member(arena_t *arena, cir_expr_t *object,
                                  const char *field) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_UNION_MEMBER;
    e->lhs = object;
    e->text = field;
    return e;
}

cir_expr_t *cir_expr_union_tag(arena_t *arena, cir_expr_t *object) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_UNION_TAG;
    e->lhs = object;
    return e;
}

cir_expr_t *cir_expr_cunion_construct(arena_t *arena, cir_type_t *type,
                                      const char *field,
                                      cir_expr_t *value) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_CUNION_CONSTRUCT;
    e->type = type;
    e->text = field;
    e->lhs = value;
    return e;
}

cir_expr_t *cir_expr_func_obj(arena_t *arena, const char *object,
                              cir_type_t *type) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_FUNC_OBJ;
    e->text = object;
    e->type = type;
    return e;
}

cir_expr_t *cir_expr_func_call(arena_t *arena, cir_expr_t *callee,
                               cir_expr_t *args) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_FUNC_CALL;
    e->lhs = callee;
    e->args = args;
    return e;
}

cir_expr_t *cir_expr_closure(arena_t *arena, cir_type_t *type,
                             const char *fn_name, cir_expr_t *env) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_CLOSURE;
    e->type = type;
    e->text = fn_name;
    e->lhs = env;
    return e;
}

cir_expr_t *cir_expr_closure_release(arena_t *arena, cir_expr_t *object) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_CLOSURE_RELEASE;
    e->lhs = object;
    return e;
}

cir_expr_t *cir_expr_func_clone(arena_t *arena, cir_expr_t *object) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_FUNC_CLONE;
    e->lhs = object;
    return e;
}

cir_expr_t *cir_expr_bitfield_get(arena_t *arena, cir_expr_t *object,
                                  size_t offset, size_t width,
                                  size_t word_bits, cir_type_t *value_type) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_BITFIELD_GET;
    e->lhs = object;
    e->type = value_type;
    e->bit_offset = offset;
    e->bit_width = width;
    e->bit_word_bits = word_bits;
    return e;
}

cir_expr_t *cir_expr_bitfield_set(arena_t *arena, cir_expr_t *object,
                                  size_t offset, size_t width,
                                  size_t word_bits, cir_expr_t *value,
                                  cir_type_t *object_type) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_BITFIELD_SET;
    e->lhs = object;
    e->rhs = value;
    e->type = object_type;
    e->bit_offset = offset;
    e->bit_width = width;
    e->bit_word_bits = word_bits;
    return e;
}

cir_expr_t *cir_expr_bitfield_make(arena_t *arena, cir_type_t *type,
                                   size_t word_bits, cir_init_t *inits) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_BITFIELD_MAKE;
    e->type = type;
    e->bit_word_bits = word_bits;
    e->inits = inits;
    return e;
}

cir_expr_t *cir_expr_cast(arena_t *arena, cir_type_t *type,
                          cir_expr_t *value) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_CAST;
    e->type = type;
    e->lhs = value;
    return e;
}

cir_expr_t *cir_expr_ternary(arena_t *arena, cir_expr_t *cond,
                             cir_expr_t *then_e, cir_expr_t *else_e) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_TERNARY;
    e->lhs = cond;
    e->rhs = then_e;
    e->high = else_e;
    return e;
}

cir_expr_t *cir_expr_mem_eq(arena_t *arena, cir_type_t *type, cir_expr_t *lhs,
                            cir_expr_t *rhs) {
    cir_expr_t *e = (cir_expr_t *)arena_calloc(
        arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->kind = CIR_EXPR_MEM_EQ;
    e->type = type;
    e->lhs = lhs;
    e->rhs = rhs;
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
