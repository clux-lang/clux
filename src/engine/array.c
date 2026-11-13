#include "engine/array.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct _clux_array_meta_t {
  clux_type_t *base;
  size_t len;
} clux_array_meta_t;

static clux_array_meta_t *array_meta(clux_type_t *type) {
  return clux_type_get_meta(type);
}

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static bool array_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_ARRAY:
    return true;
  default:
    return false;
  }
}

/* 下标取项：返回 base 类型的 view（与数组内联块内存共享），越界报 error，
   写入由通用 assign 写穿 view 完成。 */
static clux_value_t *
array_index(clux_context_t *ctx, clux_value_t *array, clux_value_t *index) {
  clux_type_t *base = array_meta(clux_value_get_type(array))->base;
  if (is_error_value(index)) {
    return index;
  }
  if (clux_value_is_shadow(array) || clux_value_is_shadow(index)) {
    return clux_context_create_shadow_value(ctx, base);
  }
  clux_array_meta_t *meta = array_meta(clux_value_get_type(array));
  int64_t idx = 0;
  clux_value_t *idx_err = clux_context_index_to_i64(ctx, index, &idx);
  if (idx_err) {
    return idx_err;
  }
  if (idx < 0 || (uint64_t)idx >= meta->len) {
    return clux_context_create_error_message(ctx,
                                             NULL,
                                             "index %lld is out of bounds "
                                             "for array of length %zu",
                                             (long long)idx,
                                             meta->len);
  }
  size_t elem_size = clux_type_get_size(base);
  ptr_handle_t view =
      ptr_offset(clux_value_get_data(array), (size_t)idx * elem_size);
  if (!view) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot index an invalid array");
  }
  return clux_context_create_value(ctx, base, view, NULL, NULL);
}

static clux_value_t *array_compare(clux_context_t *ctx,
                                   clux_value_t *left,
                                   clux_value_t *right,
                                   clux_binary_op_t op) {
  if (is_error_value(left)) {
    return left;
  }
  if (is_error_value(right)) {
    return right;
  }
  if (!array_kind(clux_type_get_kind(clux_value_get_type(right)))) {
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
  clux_array_meta_t *left_meta = array_meta(clux_value_get_type(left));
  clux_array_meta_t *right_meta = array_meta(clux_value_get_type(right));
  void *left_raw = ptr_to_raw(clux_value_get_data(left), 0);
  void *right_raw = ptr_to_raw(clux_value_get_data(right), 0);
  if (!left_raw || !right_raw) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot compare invalid arrays");
  }
  bool equal =
      left_meta->len == right_meta->len &&
      memcmp(left_raw,
             right_raw,
             left_meta->len * clux_type_get_size(left_meta->base)) == 0;
  return clux_context_create_bool(
      ctx, op == CLUX_OP_EQ ? equal : !equal, NULL, NULL);
}

static clux_value_t *
array_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return array_compare(ctx, left, right, CLUX_OP_EQ);
}

static clux_value_t *
array_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return array_compare(ctx, left, right, CLUX_OP_NE);
}

static bool array_type_equal(clux_type_t *self, clux_type_t *other) {
  if (clux_type_get_kind(other) != CLUX_TYPE_ARRAY) {
    return false;
  }
  clux_array_meta_t *self_meta = array_meta(self);
  clux_array_meta_t *other_meta = array_meta(other);
  if (!self_meta || !other_meta) {
    return false;
  }
  return self_meta->len == other_meta->len &&
         clux_type_equal(self_meta->base, other_meta->base);
}

/* 数组协变：[N]T extends [N]U 当且仅当 T extends U（长度必须相同）。 */
static bool array_type_extends(clux_type_t *self, clux_type_t *base) {
  if (clux_type_get_kind(base) != CLUX_TYPE_ARRAY) {
    return false;
  }
  clux_array_meta_t *self_meta = array_meta(self);
  clux_array_meta_t *base_meta = array_meta(base);
  if (!self_meta || !base_meta) {
    return false;
  }
  return self_meta->len == base_meta->len &&
         clux_type_extends(self_meta->base, base_meta->base);
}

static clux_vtable_t g_array_vtable = {
    .eq = array_eq,
    .ne = array_ne,
    .index = array_index,
    .type_equal = array_type_equal,
    .type_extends = array_type_extends,
};

clux_vtable_t *get_clux_array_vtable(void) { return &g_array_vtable; }

/* 桶去重判据：base 语义相等且长度相同即视为同一数组类型 */
typedef struct _clux_array_key_t {
  clux_type_t *base;
  size_t len;
} clux_array_key_t;

static bool array_child_base_matches(clux_type_t *child, void *user_data) {
  clux_array_key_t *key = user_data;
  clux_array_meta_t *meta = clux_type_get_meta(child);
  return meta && meta->len == key->len &&
         clux_type_equal(meta->base, key->base);
}

clux_type_t *clux_type_get_array_type(clux_type_t *base, size_t len) {
  clux_array_key_t key = {base, len};
  clux_type_t *existing = clux_type_find_child(
      base, CLUX_TYPE_ARRAY, array_child_base_matches, &key);
  if (existing) {
    return existing;
  }
  const char *base_name = clux_type_get_name(base);
  base_name = base_name == NULL ? "<unknown>" : base_name;
  char name[16 + strlen(base_name) + 3];
  (void)snprintf(name, sizeof(name), "[%zu]%s", len, base_name);
  allocator_t *allocator = clux_type_get_allocator(base);
  clux_type_t *type = create_clux_type(allocator,
                                       CLUX_TYPE_ARRAY,
                                       name,
                                       len * clux_type_get_size(base),
                                       clux_type_get_align(base),
                                       get_clux_array_vtable());
  clux_array_meta_t *meta =
      clux_type_create_meta(type, sizeof(clux_array_meta_t));
  meta->base = base;
  meta->len = len;
  clux_type_add_child(base, CLUX_TYPE_ARRAY, type);
  return type;
}
