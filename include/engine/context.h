#ifndef _H_ENGINE_CONTEXT_
#define _H_ENGINE_CONTEXT_
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/safe_alloc.h"
#include "engine/interface.h"
#ifdef __cplusplus
extern "C" {
#endif
clux_context_t *create_clux_context(allocator_t *allocator);
void delete_clux_context(clux_context_t **self);
safe_allocator_t *clux_context_get_safe_allocator(clux_context_t *self);
clux_value_t *clux_context_create_value(clux_context_t *self,
                                        clux_type_t *type,
                                        ptr_handle_t data,
                                        const char *name,
                                        location_t *loc);
void clux_context_push_scope(clux_context_t *self);
void clux_context_pop_scope(clux_context_t *self);
clux_value_t *clux_context_create_error(clux_context_t *self,
                                        clux_error_t *error);
clux_value_t *clux_context_create_i32(clux_context_t *self,
                                      int32_t value,
                                      const char *name,
                                      location_t *loc);
clux_value_t *clux_context_add(clux_context_t *self,
                               clux_value_t *left,
                               clux_value_t *right,
                               location_t *loc);
clux_type_t *clux_context_get_i32_type(clux_context_t *ctx);
clux_type_t *clux_context_get_error_type(clux_context_t *ctx);
#ifdef __cplusplus
}
#endif
#endif
