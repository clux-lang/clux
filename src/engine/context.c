#include "engine/context.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/safe_alloc.h"
#include "core/vec.h"
#include "engine/integer.h"
#include "engine/interface.h"
#include "engine/scope.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdint.h>
#include <stdalign.h>
#include <string.h>
#include "engine/error.h"
struct _clux_context_t {
  allocator_t *allocator;
  safe_allocator_t *safe_allocator;
  vec_t *errors;
  clux_scope_t *global_scope;
  clux_scope_t *current_scope;
  struct {
    clux_type_t *type_error;
    clux_type_t *type_i8;
    clux_type_t *type_i16;
    clux_type_t *type_i32;
    clux_type_t *type_i64;
  } builtin_types;
};
clux_context_t *create_clux_context(allocator_t *allocator) {
  clux_context_t *ctx =
      allocator_alloc(allocator, sizeof(struct _clux_context_t));
  ctx->allocator = allocator;
  ctx->safe_allocator = create_safe_allocator(allocator);
  ctx->errors = create_vec(allocator);
  ctx->global_scope = create_clux_scope(allocator, NULL);
  ctx->current_scope = ctx->global_scope;
  ctx->builtin_types.type_error = create_clux_type(allocator,
                                                   CLUX_TYPE_ERROR,
                                                   "error",
                                                   sizeof(clux_error_t *),
                                                   alignof(clux_error_t *),
                                                   get_clux_error_vtable());
  ctx->builtin_types.type_i8 = create_clux_type(allocator,
                                                CLUX_TYPE_I8,
                                                "i8",
                                                sizeof(int8_t),
                                                alignof(int8_t),
                                                get_clux_i8_vtable());
  ctx->builtin_types.type_i16 = create_clux_type(allocator,
                                                 CLUX_TYPE_I16,
                                                 "i16",
                                                 sizeof(int16_t),
                                                 alignof(int16_t),
                                                 get_clux_i16_vtable());
  ctx->builtin_types.type_i32 = create_clux_type(allocator,
                                                 CLUX_TYPE_I32,
                                                 "i32",
                                                 sizeof(int32_t),
                                                 alignof(int32_t),
                                                 get_clux_i32_vtable());
  ctx->builtin_types.type_i64 = create_clux_type(allocator,
                                                 CLUX_TYPE_I64,
                                                 "i64",
                                                 sizeof(int64_t),
                                                 alignof(int64_t),
                                                 get_clux_i64_vtable());
  return ctx;
}
void delete_clux_context(clux_context_t **self) {
  if (!self || !*self) {
    return;
  }
  clux_context_t *ctx = *self;
  while (ctx->current_scope != ctx->global_scope) {
    clux_context_pop_scope(ctx);
  }
  delete_clux_scope(&ctx->global_scope);
  for (size_t idx = 0; idx < vec_get_size(ctx->errors); idx++) {
    clux_error_t *err = vec_get(ctx->errors, idx);
    delete_error(&err);
  }
  delete_vec(&ctx->errors);
  delete_clux_type(&ctx->builtin_types.type_i64);
  delete_clux_type(&ctx->builtin_types.type_i32);
  delete_clux_type(&ctx->builtin_types.type_i16);
  delete_clux_type(&ctx->builtin_types.type_i8);
  delete_clux_type(&ctx->builtin_types.type_error);
  delete_safe_allocator(&ctx->safe_allocator);
  allocator_free(ctx->allocator, (void **)self);
}
safe_allocator_t *clux_context_get_safe_allocator(clux_context_t *self) {
  return self->safe_allocator;
}
clux_value_t *clux_context_create_value(clux_context_t *self,
                                        clux_type_t *type,
                                        ptr_handle_t data,
                                        const char *name,
                                        location_t *loc) {
  clux_value_t *value = create_clux_value(self->allocator, type, data);
  clux_value_t *previous = clux_scope_define(self->current_scope, name, value);
  if (previous != NULL) {
    delete_clux_value(&value);
    clux_error_t *err = create_error(self->allocator,
                                     loc,
                                     "duplicate declaration '%s'",
                                     name ? name : "<anonymous>");
    return clux_context_create_error(self, err);
  }
  return value;
}
clux_value_t *clux_context_create_error(clux_context_t *self,
                                        clux_error_t *error) {
  (void)vec_push(self->errors, error);
  size_t size = clux_type_get_size(self->builtin_types.type_error);
  ptr_handle_t data =
      safe_alloc(self->safe_allocator, size, error_get_location(error));
  void *raw = ptr_to_raw(data, 0);
  (void)memcpy(raw, &error, size);
  return clux_context_create_value(
      self, self->builtin_types.type_error, data, NULL, NULL);
}
static clux_value_t *create_integer_value(clux_context_t *self,
                                          clux_type_t *type,
                                          const void *value,
                                          const char *name,
                                          location_t *loc) {
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  void *raw = ptr_to_raw(data, 0);
  (void)memcpy(raw, value, size);
  return clux_context_create_value(self, type, data, name, loc);
}
clux_value_t *clux_context_create_i8(clux_context_t *self,
                                     int8_t value,
                                     const char *name,
                                     location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_i8, &value, name, loc);
}
clux_value_t *clux_context_create_i16(clux_context_t *self,
                                      int16_t value,
                                      const char *name,
                                      location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_i16, &value, name, loc);
}
clux_value_t *clux_context_create_i32(clux_context_t *self,
                                      int32_t value,
                                      const char *name,
                                      location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_i32, &value, name, loc);
}
clux_value_t *clux_context_create_i64(clux_context_t *self,
                                      int64_t value,
                                      const char *name,
                                      location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_i64, &value, name, loc);
}
void clux_context_push_scope(clux_context_t *self) {
  clux_scope_t *scope = create_clux_scope(self->allocator, self->current_scope);
  self->current_scope = scope;
}
void clux_context_pop_scope(clux_context_t *self) {
  if (!self || self->current_scope == self->global_scope) {
    return;
  }
  clux_scope_t *parent = clux_scope_get_parent(self->current_scope);
  delete_clux_scope(&self->current_scope);
  self->current_scope = parent;
}
clux_value_t *clux_context_add(clux_context_t *self,
                               clux_value_t *left,
                               clux_value_t *right,
                               location_t *loc) {
  if (!left || !right) {
    clux_error_t *err = create_error(
        self->allocator, loc, "operator '+' requires two operands");
    return clux_context_create_error(self, err);
  }
  clux_type_t *type = clux_value_get_type(left);
  if (type != clux_value_get_type(right)) {
    clux_error_t *err =
        create_error(self->allocator,
                     loc,
                     "operator '+' requires operands of the same type");
    return clux_context_create_error(self, err);
  }
  clux_vtable_t *vt = clux_type_get_vtable(type);
  if (vt->add) {
    return vt->add(self, left, CLUX_OP_ADD, right);
  }
  const char *type_name = clux_type_get_name(type);
  clux_error_t *err = create_error(self->allocator,
                                   loc,
                                   "type '%s' does not support operator '+'",
                                   type_name ? type_name : "<unknown>");
  return clux_context_create_error(self, err);
}
clux_type_t *clux_context_get_i8_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_i8;
}
clux_type_t *clux_context_get_i16_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_i16;
}
clux_type_t *clux_context_get_i32_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_i32;
}
clux_type_t *clux_context_get_i64_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_i64;
}
clux_type_t *clux_context_get_error_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_error;
}