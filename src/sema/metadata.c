#include "sema/metadata.h"
#include "sema/sema.h"
#include "core/arena.h"
#include "vm/vm.h"

#include <string.h>

static strslice_t meta_copy(sema_t *sema, strslice_t s) {
    if (!s.ptr || s.len == 0) return STRSLICE_EMPTY;
    char *buf = (char *)arena_alloc(sema->arena, s.len + 1, 1);
    if (!buf) return STRSLICE_EMPTY;
    memcpy(buf, s.ptr, s.len);
    buf[s.len] = '\0';
    return strslice_from_bytes(buf, s.len);
}

static sema_meta_entry_t *meta_head(sema_type_t *st, sema_meta_kind_t kind) {
    if (!st) return NULL;
    if (kind == SEMA_META_TYPE) return st->type_meta;
    if (kind == SEMA_META_FIELD) return st->field_meta;
    return st->method_meta;
}

static void meta_set_head(sema_type_t *st, sema_meta_kind_t kind,
                          sema_meta_entry_t *head) {
    if (kind == SEMA_META_TYPE) st->type_meta = head;
    else if (kind == SEMA_META_FIELD) st->field_meta = head;
    else st->method_meta = head;
}

const sema_meta_entry_t *sema_meta_find(const sema_type_t *st,
                                         sema_meta_kind_t kind,
                                         strslice_t owner, strslice_t key) {
    if (!st) return NULL;
    const sema_meta_entry_t *e = kind == SEMA_META_TYPE
                                     ? st->type_meta
                                     : kind == SEMA_META_FIELD ? st->field_meta
                                                               : st->method_meta;
    for (; e; e = e->next) {
        if (strslice_eq(e->key, key) &&
            (kind == SEMA_META_TYPE || strslice_eq(e->owner, owner)))
            return e;
    }
    return NULL;
}

sema_meta_entry_t *sema_meta_add(sema_t *sema, sema_type_t *st,
                                  sema_meta_kind_t kind, strslice_t owner,
                                  strslice_t key, const sema_ct_const_t *value) {
    if (!sema || !st || !value || !key.ptr) return NULL;
    if (sema_meta_find(st, kind, owner, key)) return NULL;
    sema_meta_entry_t *e = (sema_meta_entry_t *)arena_calloc(
        sema->arena, 1, sizeof(*e), ALIGNOF(max_align_t));
    if (!e) return NULL;
    e->key = meta_copy(sema, key);
    e->owner = kind == SEMA_META_TYPE ? STRSLICE_EMPTY : meta_copy(sema, owner);
    e->value = *value;
    e->next = meta_head(st, kind);
    meta_set_head(st, kind, e);
    return e;
}
