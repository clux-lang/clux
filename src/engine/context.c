#include "engine/context.h"
#include "engine/array.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/rbtree.h"
#include "core/safe_alloc.h"
#include "core/vec.h"
#include "engine/integer.h"
#include "engine/interface.h"
#include "engine/scope.h"
#include "engine/type.h"
#include "engine/unsigned.h"
#include "engine/value.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdalign.h>
#include <stdio.h>
#include <string.h>
#include "engine/bool.h"
#include "engine/error.h"
#include "engine/floating.h"
#include "engine/ptr.h"
#include "engine/qualifier.h"
#include "engine/slice.h"
#include "engine/str.h"

struct _clux_context_t {
  allocator_t *allocator;
  safe_allocator_t *safe_allocator;
  vec_t *errors;
  rbtree_t *strs;
  clux_scope_t *global_scope;
  clux_scope_t *current_scope;
  struct {
    clux_type_t *type_error;
    clux_type_t *type_i8;
    clux_type_t *type_i16;
    clux_type_t *type_i32;
    clux_type_t *type_i64;
    clux_type_t *type_u8;
    clux_type_t *type_u16;
    clux_type_t *type_u32;
    clux_type_t *type_u64;
    clux_type_t *type_isize;
    clux_type_t *type_usize;
    clux_type_t *type_f32;
    clux_type_t *type_f64;
    clux_type_t *type_bool;
    clux_type_t *type_str;
  } builtin_types;
};
clux_context_t *create_clux_context(allocator_t *allocator) {
  clux_context_t *ctx =
      allocator_alloc(allocator, sizeof(struct _clux_context_t));
  ctx->allocator = allocator;
  ctx->safe_allocator = create_safe_allocator(allocator);
  ctx->errors = create_vec(allocator);
  ctx->strs = create_rbtree(allocator);
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
  ctx->builtin_types.type_u8 = create_clux_type(allocator,
                                                CLUX_TYPE_U8,
                                                "u8",
                                                sizeof(uint8_t),
                                                alignof(uint8_t),
                                                get_clux_u8_vtable());
  ctx->builtin_types.type_u16 = create_clux_type(allocator,
                                                 CLUX_TYPE_U16,
                                                 "u16",
                                                 sizeof(uint16_t),
                                                 alignof(uint16_t),
                                                 get_clux_u16_vtable());
  ctx->builtin_types.type_u32 = create_clux_type(allocator,
                                                 CLUX_TYPE_U32,
                                                 "u32",
                                                 sizeof(uint32_t),
                                                 alignof(uint32_t),
                                                 get_clux_u32_vtable());
  ctx->builtin_types.type_u64 = create_clux_type(allocator,
                                                 CLUX_TYPE_U64,
                                                 "u64",
                                                 sizeof(uint64_t),
                                                 alignof(uint64_t),
                                                 get_clux_u64_vtable());
  ctx->builtin_types.type_isize = create_clux_type(allocator,
                                                   CLUX_TYPE_ISIZE,
                                                   "isize",
                                                   sizeof(intptr_t),
                                                   alignof(intptr_t),
                                                   get_clux_isize_vtable());
  ctx->builtin_types.type_usize = create_clux_type(allocator,
                                                   CLUX_TYPE_USIZE,
                                                   "usize",
                                                   sizeof(size_t),
                                                   alignof(size_t),
                                                   get_clux_usize_vtable());
  ctx->builtin_types.type_f32 = create_clux_type(allocator,
                                                 CLUX_TYPE_F32,
                                                 "f32",
                                                 sizeof(float),
                                                 alignof(float),
                                                 get_clux_f32_vtable());
  ctx->builtin_types.type_f64 = create_clux_type(allocator,
                                                 CLUX_TYPE_F64,
                                                 "f64",
                                                 sizeof(double),
                                                 alignof(double),
                                                 get_clux_f64_vtable());
  ctx->builtin_types.type_bool = create_clux_type(allocator,
                                                  CLUX_TYPE_BOOL,
                                                  "bool",
                                                  sizeof(bool),
                                                  alignof(bool),
                                                  get_clux_bool_vtable());
  ctx->builtin_types.type_str = create_clux_type(allocator,
                                                 CLUX_TYPE_STR,
                                                 "str",
                                                 sizeof(char *),
                                                 alignof(char *),
                                                 get_clux_str_vtable());
  return ctx;
}
static void strs_dispose(uintptr_t key, char *str, allocator_t *allocator) {
  (void)key;
  allocator_free(allocator, (void **)&str);
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
  delete_clux_type(&ctx->builtin_types.type_str);
  delete_clux_type(&ctx->builtin_types.type_bool);
  delete_clux_type(&ctx->builtin_types.type_f64);
  delete_clux_type(&ctx->builtin_types.type_f32);
  delete_clux_type(&ctx->builtin_types.type_u64);
  delete_clux_type(&ctx->builtin_types.type_usize);
  delete_clux_type(&ctx->builtin_types.type_isize);
  delete_clux_type(&ctx->builtin_types.type_u32);
  delete_clux_type(&ctx->builtin_types.type_u16);
  delete_clux_type(&ctx->builtin_types.type_u8);
  delete_clux_type(&ctx->builtin_types.type_i64);
  delete_clux_type(&ctx->builtin_types.type_i32);
  delete_clux_type(&ctx->builtin_types.type_i16);
  delete_clux_type(&ctx->builtin_types.type_i8);
  delete_clux_type(&ctx->builtin_types.type_error);
  delete_vec(&ctx->errors);
  rbtree_foreach(ctx->strs, (rbtree_callback_t)strs_dispose, ctx->allocator);
  delete_rbtree(&ctx->strs);
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
clux_value_t *clux_context_create_shadow_value(clux_context_t *self,
                                               clux_type_t *type) {
  return clux_context_create_value(self, type, 0, NULL, NULL);
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
clux_value_t *clux_context_create_u8(clux_context_t *self,
                                     uint8_t value,
                                     const char *name,
                                     location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_u8, &value, name, loc);
}
clux_value_t *clux_context_create_u16(clux_context_t *self,
                                      uint16_t value,
                                      const char *name,
                                      location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_u16, &value, name, loc);
}
clux_value_t *clux_context_create_u32(clux_context_t *self,
                                      uint32_t value,
                                      const char *name,
                                      location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_u32, &value, name, loc);
}
clux_value_t *clux_context_create_u64(clux_context_t *self,
                                      uint64_t value,
                                      const char *name,
                                      location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_u64, &value, name, loc);
}
clux_value_t *clux_context_create_isize(clux_context_t *self,
                                        intptr_t value,
                                        const char *name,
                                        location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_isize, &value, name, loc);
}
clux_value_t *clux_context_create_usize(clux_context_t *self,
                                        size_t value,
                                        const char *name,
                                        location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_usize, &value, name, loc);
}
clux_value_t *clux_context_create_f32(clux_context_t *self,
                                      float value,
                                      const char *name,
                                      location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_f32, &value, name, loc);
}
clux_value_t *clux_context_create_f64(clux_context_t *self,
                                      double value,
                                      const char *name,
                                      location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_f64, &value, name, loc);
}
clux_value_t *clux_context_create_bool(clux_context_t *self,
                                       bool value,
                                       const char *name,
                                       location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_bool, &value, name, loc);
}
char *clux_context_alloc_str(clux_context_t *self, size_t len) {
  char *str = allocator_alloc(self->allocator, len + 1);
  str[len] = '\0';
  (void)rbtree_insert(self->strs, (uintptr_t)str, str);
  return str;
}
clux_value_t *clux_context_create_str(clux_context_t *self,
                                      const char *value,
                                      const char *name,
                                      location_t *loc) {
  size_t len = value ? strlen(value) : 0;
  char *str = clux_context_alloc_str(self, len);
  if (value) {
    (void)memcpy(str, value, len);
  }
  return clux_context_create_str_value(self, str, name, loc);
}

clux_value_t *clux_context_create_ptr(clux_context_t *self,
                                      clux_type_t *type,
                                      ptr_handle_t value,
                                      const char *name,
                                      location_t *loc) {
  ptr_handle_t data =
      safe_alloc(self->safe_allocator, sizeof(ptr_handle_t), loc);
  ptr_handle_t *raw = ptr_to_raw(data, 0);
  *raw = value;
  return clux_context_create_value(self, type, data, name, loc);
}
clux_type_t *clux_context_get_ptr_type(clux_context_t *self,
                                       clux_type_t *base) {
  (void)self;
  return base ? clux_type_get_ptr_type(base) : NULL;
}
clux_type_t *clux_context_get_slice_type(clux_context_t *self,
                                         clux_type_t *base) {
  (void)self;
  return base ? clux_type_get_slice_type(base) : NULL;
}
clux_value_t *clux_context_create_slice(clux_context_t *self,
                                        clux_type_t *type,
                                        ptr_handle_t ptr,
                                        size_t len,
                                        const char *name,
                                        location_t *loc) {
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  void *raw = ptr_to_raw(data, 0);
  (void)memcpy(raw, &(clux_slice_data_t){ptr, len}, size);
  return clux_context_create_value(self, type, data, name, loc);
}
clux_value_t *clux_context_slice_len(clux_context_t *self,
                                     clux_value_t *slice,
                                     location_t *loc) {
  if (!slice) {
    return clux_context_create_error_message(
        self, loc, "slice length requires a slice");
  }
  clux_type_t *type = clux_value_get_type(slice);
  if (clux_type_get_kind(type) == CLUX_TYPE_ERROR) {
    return slice;
  }
  if (clux_type_get_kind(type) != CLUX_TYPE_SLICE) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(self,
                                             loc,
                                             "type '%s' is not a slice",
                                             type_name ? type_name
                                                       : "<unknown>");
  }
  if (clux_value_is_shadow(slice)) {
    return clux_context_create_shadow_value(self,
                                            self->builtin_types.type_usize);
  }
  ptr_handle_t data = clux_value_get_data(slice);
  clux_slice_data_t *raw = ptr_to_raw(data, 0);
  if (!raw) {
    return clux_context_create_error_message(
        self, loc, "cannot read length of an invalid slice");
  }
  return clux_context_create_usize(self, raw->len, NULL, loc);
}
clux_type_t *clux_context_get_array_type(clux_context_t *self,
                                         clux_type_t *base,
                                         size_t len) {
  (void)self;
  return base ? clux_type_get_array_type(base, len) : NULL;
}
clux_value_t *clux_context_create_array(clux_context_t *self,
                                        clux_type_t *type,
                                        const char *name,
                                        location_t *loc) {
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  memset(ptr_to_raw(data, 0), 0, size);
  return clux_context_create_value(self, type, data, name, loc);
}
clux_value_t *
clux_context_deref(clux_context_t *self, clux_value_t *ptr, location_t *loc) {
  if (!ptr) {
    return clux_context_create_error_message(
        self, loc, "deref requires a pointer");
  }
  if (clux_type_get_kind(clux_value_get_type(ptr)) == CLUX_TYPE_ERROR) {
    return ptr;
  }
  clux_type_t *type = clux_value_get_type(ptr);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  if (vt && vt->deref) {
    return vt->deref(self, ptr);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' does not support operator '*'",
      type_name ? type_name : "<unknown>");
}
clux_value_t *clux_context_index(clux_context_t *self,
                                 clux_value_t *aggregate,
                                 clux_value_t *index,
                                 location_t *loc) {
  if (!aggregate || !index) {
    return clux_context_create_error_message(
        self, loc, "index requires an aggregate and an index");
  }
  if (clux_type_get_kind(clux_value_get_type(aggregate)) == CLUX_TYPE_ERROR) {
    return aggregate;
  }
  if (clux_type_get_kind(clux_value_get_type(index)) == CLUX_TYPE_ERROR) {
    return index;
  }
  clux_type_t *type = clux_value_get_type(aggregate);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  if (vt && vt->index) {
    return vt->index(self, aggregate, index);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' does not support indexing",
      type_name ? type_name : "<unknown>");
}
clux_value_t *clux_context_index_to_i64(clux_context_t *self,
                                        clux_value_t *index,
                                        int64_t *out) {
  clux_type_kind_t kind = clux_type_get_kind(clux_value_get_type(index));
  void *raw = ptr_to_raw(clux_value_get_data(index), 0);
  if (!raw) {
    return clux_context_create_error_message(
        self, NULL, "cannot read an invalid index");
  }
  int64_t value = 0;
  switch (kind) {
  case CLUX_TYPE_I8:
    value = *(int8_t *)raw;
    break;
  case CLUX_TYPE_I16:
    value = *(int16_t *)raw;
    break;
  case CLUX_TYPE_I32:
    value = *(int32_t *)raw;
    break;
  case CLUX_TYPE_I64:
    value = *(int64_t *)raw;
    break;
  case CLUX_TYPE_ISIZE:
    value = *(intptr_t *)raw;
    break;
  case CLUX_TYPE_U8:
    value = *(uint8_t *)raw;
    break;
  case CLUX_TYPE_U16:
    value = *(uint16_t *)raw;
    break;
  case CLUX_TYPE_U32:
    value = *(uint32_t *)raw;
    break;
  case CLUX_TYPE_U64:
    value = (int64_t)*(uint64_t *)raw;
    break;
  case CLUX_TYPE_USIZE:
    value = (int64_t)*(size_t *)raw;
    break;
  default: {
    const char *type_name = clux_type_get_name(clux_value_get_type(index));
    return clux_context_create_error_message(
        self,
        NULL,
        "index must be an integer, got '%s'",
        type_name ? type_name : "<unknown>");
  }
  }
  *out = value;
  return NULL;
}
clux_value_t *
clux_context_ref(clux_context_t *self, clux_value_t *value, location_t *loc) {
  if (!value) {
    return clux_context_create_error_message(self, loc, "ref requires a value");
  }
  clux_type_t *type = clux_value_get_type(value);
  if (clux_type_get_kind(type) == CLUX_TYPE_ERROR) {
    return value;
  }
  clux_type_t *ptr_type = clux_context_get_ptr_type(self, type);
  if (!ptr_type) {
    return clux_context_create_error_message(
        self, loc, "ref requires a typed value");
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(self, ptr_type);
  }
  return clux_context_create_ptr(
      self, ptr_type, clux_value_get_data(value), NULL, loc);
}
clux_value_t *clux_context_create_str_value(clux_context_t *self,
                                            char *str,
                                            const char *name,
                                            location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_str, &str, name, loc);
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
clux_value_t *clux_context_create_error_message(clux_context_t *self,
                                                location_t *loc,
                                                const char *fmt,
                                                ...) {
  va_list args;
  va_start(args, fmt);
  size_t len = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  char *msg = allocator_alloc(self->allocator, len + 1);
  va_start(args, fmt);
  (void)vsnprintf(msg, len + 1, fmt, args);
  va_end(args);
  clux_error_t *err = create_error(self->allocator, loc, "%s", msg);
  allocator_free(self->allocator, (void **)&msg);
  return clux_context_create_error(self, err);
}
static const char *binary_op_name(clux_binary_op_t op) {
  switch (op) {
  case CLUX_OP_ADD:
    return "+";
  case CLUX_OP_SUB:
    return "-";
  case CLUX_OP_MUL:
    return "*";
  case CLUX_OP_DIV:
    return "/";
  case CLUX_OP_MOD:
    return "%";
  case CLUX_OP_AND:
    return "&";
  case CLUX_OP_OR:
    return "|";
  case CLUX_OP_XOR:
    return "^";
  case CLUX_OP_SHL:
    return "<<";
  case CLUX_OP_SHR:
    return ">>";
  case CLUX_OP_EQ:
    return "==";
  case CLUX_OP_NE:
    return "!=";
  case CLUX_OP_LT:
    return "<";
  case CLUX_OP_LE:
    return "<=";
  case CLUX_OP_GT:
    return ">";
  case CLUX_OP_GE:
    return ">=";
  }
  return "<unknown>";
}
static const char *prefix_op_name(clux_prefix_op_t op) {
  switch (op) {
  case CLUX_OP_PLUS:
    return "+";
  case CLUX_OP_NEGATIVE:
    return "-";
  case CLUX_OP_NOT:
    return "~";
  case CLUX_OP_LNOT:
    return "!";
  }
  return "<unknown>";
}
static clux_vt_binary_fn_t binary_op_fn(clux_vtable_t *vt,
                                        clux_binary_op_t op) {
  switch (op) {
  case CLUX_OP_ADD:
    return vt->add;
  case CLUX_OP_SUB:
    return vt->sub;
  case CLUX_OP_MUL:
    return vt->mul;
  case CLUX_OP_DIV:
    return vt->div;
  case CLUX_OP_MOD:
    return vt->mod;
  case CLUX_OP_AND:
    return vt->bit_and;
  case CLUX_OP_OR:
    return vt->bit_or;
  case CLUX_OP_XOR:
    return vt->bit_xor;
  case CLUX_OP_SHL:
    return vt->shl;
  case CLUX_OP_SHR:
    return vt->shr;
  case CLUX_OP_EQ:
    return vt->eq;
  case CLUX_OP_NE:
    return vt->ne;
  case CLUX_OP_LT:
    return vt->lt;
  case CLUX_OP_LE:
    return vt->le;
  case CLUX_OP_GT:
    return vt->gt;
  case CLUX_OP_GE:
    return vt->ge;
  }
  return NULL;
}
static clux_vt_prefix_fn_t prefix_op_fn(clux_vtable_t *vt,
                                        clux_prefix_op_t op) {
  switch (op) {
  case CLUX_OP_PLUS:
    return vt->plus;
  case CLUX_OP_NEGATIVE:
    return vt->negative;
  case CLUX_OP_NOT:
    return vt->bit_not;
  case CLUX_OP_LNOT:
    return vt->logic_not;
  }
  return NULL;
}
clux_value_t *clux_context_safe_cast(clux_context_t *self,
                                     clux_value_t *value,
                                     clux_type_t *target,
                                     location_t *loc) {
  if (!value || !target) {
    return clux_context_create_error_message(
        self, loc, "safe cast requires a value and a target type");
  }
  clux_type_t *source = clux_value_get_type(value);
  clux_vtable_t *vt = clux_type_get_vtable(source);
  if (vt && vt->safe_cast) {
    return vt->safe_cast(self, value, target);
  }
  const char *source_name = clux_type_get_name(source);
  const char *target_name = clux_type_get_name(target);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' cannot be safely cast to '%s'",
      source_name ? source_name : "<unknown>",
      target_name ? target_name : "<unknown>");
}
clux_value_t *clux_context_cast(clux_context_t *self,
                                clux_value_t *value,
                                clux_type_t *target,
                                location_t *loc) {
  if (!value || !target) {
    return clux_context_create_error_message(
        self, loc, "cast requires a value and a target type");
  }
  clux_type_t *source = clux_value_get_type(value);
  clux_vtable_t *vt = clux_type_get_vtable(source);
  if (vt && vt->cast) {
    return vt->cast(self, value, target);
  }
  const char *source_name = clux_type_get_name(source);
  const char *target_name = clux_type_get_name(target);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' cannot be cast to '%s'",
      source_name ? source_name : "<unknown>",
      target_name ? target_name : "<unknown>");
}
clux_value_t *clux_context_binary(clux_context_t *self,
                                  clux_binary_op_t op,
                                  clux_value_t *left,
                                  clux_value_t *right,
                                  location_t *loc) {
  const char *op_name = binary_op_name(op);
  if (!left || !right) {
    return clux_context_create_error_message(
        self, loc, "operator '%s' requires two operands", op_name);
  }
  clux_type_t *type = clux_value_get_type(left);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  clux_vt_binary_fn_t fn = vt ? binary_op_fn(vt, op) : NULL;
  if (fn) {
    return fn(self, left, right);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' does not support operator "
      "'%s'",
      type_name ? type_name : "<unknown>",
      op_name);
}
clux_value_t *clux_context_prefix(clux_context_t *self,
                                  clux_prefix_op_t op,
                                  clux_value_t *value,
                                  location_t *loc) {
  const char *op_name = prefix_op_name(op);
  if (!value) {
    return clux_context_create_error_message(
        self, loc, "operator '%s' requires one operand", op_name);
  }
  clux_type_t *type = clux_value_get_type(value);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  clux_vt_prefix_fn_t fn = vt ? prefix_op_fn(vt, op) : NULL;
  if (fn) {
    return fn(self, value);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' does not support operator "
      "'%s'",
      type_name ? type_name : "<unknown>",
      op_name);
}
clux_value_t *clux_context_add(clux_context_t *self,
                               clux_value_t *left,
                               clux_value_t *right,
                               location_t *loc) {
  return clux_context_binary(self, CLUX_OP_ADD, left, right, loc);
}
clux_value_t *clux_context_assign(clux_context_t *self,
                                  clux_value_t *target,
                                  clux_value_t *value,
                                  location_t *loc) {
  if (!target || !value) {
    return clux_context_create_error_message(
        self, loc, "assignment requires a target and a value");
  }
  clux_type_t *target_type = clux_value_get_type(target);
  if (clux_type_get_kind(target_type) == CLUX_TYPE_ERROR) {
    return target;
  }
  if (clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR) {
    return value;
  }
  /* const 是静态性质：目标限定链含 const 即拒绝写入（volatile 放行） */
  if (clux_type_is_const(target_type)) {
    const char *target_name = clux_type_get_name(target_type);
    return clux_context_create_error_message(
        self,
        loc,
        "cannot assign to a value of type '%s'",
        target_name ? target_name : "<unknown>");
  }
  /* 同类型无需转换：复合类型（slice/ptr 等）没有 safe_cast，恒等直接 memcpy */
  clux_value_t *casted = value;
  if (!clux_type_equal(target_type, clux_value_get_type(value))) {
    casted = clux_context_safe_cast(self, value, target_type, loc);
  }
  clux_type_t *casted_type = clux_value_get_type(casted);
  if (clux_type_get_kind(casted_type) == CLUX_TYPE_ERROR) {
    return casted;
  }
  if (clux_value_is_shadow(target) || clux_value_is_shadow(casted)) {
    return target;
  }
  /* value 的 data 是平凡位图：右值 safe_cast 成左值类型后整体 memcpy 穿透 */
  size_t size = clux_type_get_size(target_type);
  void *dst = ptr_to_raw(clux_value_get_data(target), 0);
  void *src = ptr_to_raw(clux_value_get_data(casted), 0);
  if (dst && src) {
    (void)memcpy(dst, src, size);
  }
  return target;
}
clux_value_t *
clux_context_clone(clux_context_t *self, clux_value_t *value, location_t *loc) {
  if (!value) {
    return clux_context_create_error_message(
        self, loc, "clone requires a value");
  }
  clux_type_t *type = clux_value_get_type(value);
  if (clux_type_get_kind(type) == CLUX_TYPE_ERROR) {
    return value;
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(self, type);
  }
  /* value 的 data 是平凡位图：新分配同尺寸块整体拷贝即可 */
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  void *dst = ptr_to_raw(data, 0);
  void *src = ptr_to_raw(clux_value_get_data(value), 0);
  if (dst && src) {
    (void)memcpy(dst, src, size);
  }
  return clux_context_create_value(self, type, data, NULL, NULL);
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
clux_type_t *clux_context_get_u8_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_u8;
}
clux_type_t *clux_context_get_u16_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_u16;
}
clux_type_t *clux_context_get_u32_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_u32;
}
clux_type_t *clux_context_get_u64_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_u64;
}
clux_type_t *clux_context_get_isize_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_isize;
}
clux_type_t *clux_context_get_usize_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_usize;
}
clux_type_t *clux_context_get_f32_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_f32;
}
clux_type_t *clux_context_get_f64_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_f64;
}
clux_type_t *clux_context_get_bool_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_bool;
}
clux_type_t *clux_context_get_str_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_str;
}
clux_type_t *clux_context_get_error_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_error;
}