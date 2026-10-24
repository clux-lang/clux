#include "cbackend/c_ir.h"

#include <string.h>

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
