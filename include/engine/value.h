#ifndef _H_ENGINE_VALUE_
#define _H_ENGINE_VALUE_
#include "core/allocator.h"
#include "core/safe_alloc.h"
#include "engine/interface.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
clux_value_t *
create_clux_value(allocator_t *allocator, clux_type_t *type, ptr_handle_t data);
clux_value_t *create_clux_shadow_value(allocator_t *allocator,
                                       clux_type_t *type);
ptr_handle_t clux_value_get_data(clux_value_t *self);
clux_type_t *clux_value_get_type(clux_value_t *self);
void delete_clux_value(clux_value_t **self);
#ifdef __cplusplus
}
#endif
#endif
