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

void cir_unit_add_type(cir_unit_t *u, cir_type_t *t) {
    if (!u || !t) return;
    t->next = u->types;
    u->types = t;
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
