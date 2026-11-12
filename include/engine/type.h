#ifndef _H_ENGINE_TYPE_
#define _H_ENGINE_TYPE_
#include "core/allocator.h"
#include "engine/interface.h"
#ifdef __cplusplus
extern "C" {
#endif
clux_type_t *create_clux_type(allocator_t *allocator,
                              clux_type_kind_t kind,
                              const char *name,
                              size_t size,
                              size_t align,
                              clux_vtable_t /* borrow */ *vtable);
void delete_clux_type(clux_type_t **self);
clux_type_kind_t clux_type_get_kind(clux_type_t *self);
size_t clux_type_get_size(clux_type_t *self);
size_t clux_type_get_align(clux_type_t *self);
const char *clux_type_get_name(clux_type_t *self);
clux_vtable_t *clux_type_get_vtable(clux_type_t *self);
void *clux_type_create_meta(clux_type_t *self, size_t meta_size);
void *clux_type_get_meta(clux_type_t *self);
#ifdef __cplusplus
}
#endif
#endif
