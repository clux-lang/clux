#ifndef _H_ENGINE_SCOPE_
#define _H_ENGINE_SCOPE_
#include "core/allocator.h"
#include "engine/interface.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
clux_scope_t *create_clux_scope(allocator_t *allocator, clux_scope_t *parent);
void delete_clux_scope(clux_scope_t **self);
clux_value_t *
clux_scope_define(clux_scope_t *self, const char *name, clux_value_t *value);
clux_value_t *clux_scope_get(clux_scope_t *self, const char *name);
clux_scope_t *clux_scope_get_parent(clux_scope_t *self);
#ifdef __cplusplus
}
#endif
#endif
