#include "cbackend/c_ir.h"

#include <string.h>

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
