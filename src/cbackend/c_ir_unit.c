#include "cbackend/c_ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *arena_dup(arena_t *arena, const char *s) {
    if (!arena) return NULL;
    size_t n = s ? strlen(s) : 0;
    char *p = (char *)arena_alloc(arena, n + 1, ALIGNOF(max_align_t));
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

cir_unit_t *cir_unit_new(arena_t *arena, const char *source_path) {
    cir_unit_t *u = (cir_unit_t *)arena_calloc(
        arena, 1, sizeof(*u), ALIGNOF(max_align_t));
    if (!u) return NULL;
    u->arena = arena;
    u->source_path = source_path;
    return u;
}

void cir_unit_set_module(cir_unit_t *u, const char *tag) {
    if (!u || !u->arena) return;
    u->module_tag = arena_dup(u->arena, tag ? tag : "");
    char buf[128];
    snprintf(buf, sizeof buf, "clux_%s_", u->module_tag);
    u->sym_prefix = arena_dup(u->arena, buf);
}

void cir_unit_add_header_type(cir_unit_t *u, cir_type_t *t) {
    if (!u || !t || !u->arena) return;
    cir_type_ref_t *r = (cir_type_ref_t *)arena_calloc(
        u->arena, 1, sizeof(*r), ALIGNOF(max_align_t));
    if (!r) return;
    r->type = t;
    r->next = u->header_types;
    u->header_types = r;
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

void cir_unit_add_raw(cir_unit_t *u, const char *code) {
    if (!u || !code || !u->arena) return;
    cir_raw_t *r = (cir_raw_t *)arena_calloc(u->arena, 1, sizeof(*r),
                                             ALIGNOF(max_align_t));
    if (!r) return;
    r->code = code;
    r->next = u->raw_helpers;
    u->raw_helpers = r;
}

void cir_unit_add_extern_typedef(cir_unit_t *u, const char *code) {
    if (!u || !code || !u->arena) return;
    cir_raw_t *r = (cir_raw_t *)arena_calloc(u->arena, 1, sizeof(*r),
                                             ALIGNOF(max_align_t));
    if (!r) return;
    r->code = code;
    if (u->extern_typedefs_last) u->extern_typedefs_last->next = r;
    else u->extern_typedefs = r;
    u->extern_typedefs_last = r;
}

void cir_unit_add_extern_decl(cir_unit_t *u, const char *code) {
    if (!u || !code || !u->arena) return;
    cir_raw_t *r = (cir_raw_t *)arena_calloc(u->arena, 1, sizeof(*r),
                                             ALIGNOF(max_align_t));
    if (!r) return;
    r->code = code;
    if (u->extern_decls_last) u->extern_decls_last->next = r;
    else u->extern_decls = r;
    u->extern_decls_last = r;
}

void cir_unit_add_extern_decl_pub(cir_unit_t *u, const char *code) {
    if (!u || !code || !u->arena) return;
    cir_raw_t *r = (cir_raw_t *)arena_calloc(u->arena, 1, sizeof(*r),
                                             ALIGNOF(max_align_t));
    if (!r) return;
    r->code = code;
    if (u->extern_decls_pub_last) u->extern_decls_pub_last->next = r;
    else u->extern_decls_pub = r;
    u->extern_decls_pub_last = r;
}

void cir_unit_add_link_arg(cir_unit_t *u, const char *arg) {
    if (!u || !arg || !u->arena) return;
    cir_link_arg_t *l = (cir_link_arg_t *)arena_calloc(
        u->arena, 1, sizeof(*l), ALIGNOF(max_align_t));
    if (!l) return;
    l->arg = arg;
    if (u->link_args_last) u->link_args_last->next = l;
    else u->link_args = l;
    u->link_args_last = l;
}

void cir_unit_add_extern_symbol(cir_unit_t *u, const char *name) {
    if (!u || !name || !u->arena) return;
    size_t n = u->extern_symbol_count + 1;
    const char **arr = (const char **)arena_alloc(
        u->arena, n * sizeof(*arr), ALIGNOF(max_align_t));
    if (!arr) return;
    if (u->extern_symbol_count)
        memcpy(arr, u->extern_symbols, u->extern_symbol_count * sizeof(*arr));
    arr[n - 1] = name;
    u->extern_symbols = arr;
    u->extern_symbol_count = n;
}

bool cir_unit_has_extern_symbol(const cir_unit_t *u, const char *name) {
    if (!u || !name) return false;
    for (size_t i = 0; i < u->extern_symbol_count; i++)
        if (u->extern_symbols[i] && strcmp(u->extern_symbols[i], name) == 0)
            return true;
    return false;
}
