#include "cbackend/c_ir.h"

#include <string.h>

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
