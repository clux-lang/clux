#include "engine/ptr.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>
#include <stdio.h>
#include <string.h>

typedef struct _clux_ptr_meta_t {
  clux_type_t *base;
} clux_ptr_meta_t;

static clux_type_t *ptr_base_type(clux_type_t *type) {
  clux_ptr_meta_t *meta = clux_type_get_meta(type);
  return meta ? meta->base : NULL;
}

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static bool ptr_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_PTR:
    return true;
  default:
    return false;
  }
}

static bool ptr_read(clux_value_t *value, ptr_handle_t *out) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  if (!raw) {
    return false;
  }
  *out = *(ptr_handle_t *)raw;
  return true;
}

static clux_value_t *ptr_deref_get(clux_context_t *ctx, clux_value_t *ptr) {
  clux_type_t *type = clux_value_get_type(ptr);
  clux_type_t *base = ptr_base_type(type);
  if (!base) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        ctx,
        NULL,
        "pointer type '%s' has no base type",
        type_name ? type_name : "<unknown>");
  }
  if (clux_value_is_shadow(ptr)) {
    return clux_context_create_shadow_value(ctx, base);
  }
  ptr_handle_t target = 0;
  if (!ptr_read(ptr, &target) || !ptr_is_valid(target) ||
      ptr_get_size(target) < clux_type_get_size(base)) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        ctx,
        NULL,
        "cannot dereference a value of type '%s'",
        type_name ? type_name : "<unknown>");
  }
  size_t size = clux_type_get_size(base);
  ptr_handle_t data =
      safe_alloc(clux_context_get_safe_allocator(ctx), size, NULL);
  (void)memcpy(ptr_to_raw(data, 0), ptr_to_raw(target, 0), size);
  return clux_context_create_value(ctx, base, data, NULL, NULL);
}

static clux_value_t *
ptr_deref_set(clux_context_t *ctx, clux_value_t *ptr, clux_value_t *value) {
  clux_type_t *type = clux_value_get_type(ptr);
  clux_type_t *base = ptr_base_type(type);
  if (!base) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        ctx,
        NULL,
        "pointer type '%s' has no base type",
        type_name ? type_name : "<unknown>");
  }
  if (clux_value_is_shadow(ptr) || clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, base);
  }
  clux_type_t *value_type = clux_value_get_type(value);
  if (value_type != base) {
    const char *value_name = clux_type_get_name(value_type);
    const char *base_name = clux_type_get_name(base);
    return clux_context_create_error_message(
        ctx,
        NULL,
        "cannot assign '%s' through a pointer to '%s'",
        value_name ? value_name : "<unknown>",
        base_name ? base_name : "<unknown>");
  }
  ptr_handle_t target = 0;
  if (!ptr_read(ptr, &target) || !ptr_is_valid(target) ||
      ptr_get_size(target) < clux_type_get_size(base)) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        ctx,
        NULL,
        "cannot dereference a value of type '%s'",
        type_name ? type_name : "<unknown>");
  }
  size_t size = clux_type_get_size(base);
  (void)memcpy(
      ptr_to_raw(target, 0), ptr_to_raw(clux_value_get_data(value), 0), size);
  return value;
}

static clux_value_t *ptr_compare(clux_context_t *ctx,
                                 clux_value_t *left,
                                 clux_value_t *right,
                                 clux_binary_op_t op) {
  if (is_error_value(left)) {
    return left;
  }
  if (is_error_value(right)) {
    return right;
  }
  if (!ptr_kind(clux_type_get_kind(clux_value_get_type(right)))) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "operator '%s' requires operands of the same type",
        op == CLUX_OP_EQ ? "==" : "!=");
  }
  clux_type_t *bool_type = clux_context_get_bool_type(ctx);
  if (clux_value_is_shadow(left) || clux_value_is_shadow(right)) {
    return clux_context_create_shadow_value(ctx, bool_type);
  }
  ptr_handle_t left_handle = 0;
  ptr_handle_t right_handle = 0;
  if (!ptr_read(left, &left_handle) || !ptr_read(right, &right_handle)) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot compare invalid pointers");
  }
  bool equal = left_handle == right_handle;
  return clux_context_create_bool(
      ctx, op == CLUX_OP_EQ ? equal : !equal, NULL, NULL);
}

static clux_value_t *
ptr_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return ptr_compare(ctx, left, right, CLUX_OP_EQ);
}

static clux_value_t *
ptr_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return ptr_compare(ctx, left, right, CLUX_OP_NE);
}

static clux_vtable_t g_ptr_vtable = {
    .eq = ptr_eq,
    .ne = ptr_ne,
    .deref_get = ptr_deref_get,
    .deref_set = ptr_deref_set,
};

clux_vtable_t *get_clux_ptr_vtable(void) { return &g_ptr_vtable; }

clux_type_t *clux_type_get_ptr_type(clux_type_t *base) {
  clux_type_t *existing = clux_type_find_child(base, CLUX_TYPE_PTR);
  if (existing) {
    return existing;
  }
  const char *base_name = clux_type_get_name(base);
  base_name = base_name == NULL ? "<unknown>" : base_name;
  size_t len = strlen(base_name) + 2;
  char name[len];
  (void)snprintf(name, len, "*%s", base_name);
  allocator_t *allocator = clux_type_get_allocator(base);
  clux_type_t *type = create_clux_type(allocator,
                                       CLUX_TYPE_PTR,
                                       name,
                                       sizeof(ptr_handle_t),
                                       alignof(ptr_handle_t),
                                       get_clux_ptr_vtable());
  clux_ptr_meta_t *meta = clux_type_create_meta(type, sizeof(clux_ptr_meta_t));
  meta->base = base;
  clux_type_add_child(base, CLUX_TYPE_PTR, type);
  return type;
}
