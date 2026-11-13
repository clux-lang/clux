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

/* 解引用（*p）：返回 base 类型的 view（与目标内存共享），
   写入由通用 assign 完成。 */
static clux_value_t *ptr_deref(clux_context_t *ctx, clux_value_t *ptr) {
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
  ptr_handle_t view = ptr_offset(target, 0);
  if (!view) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        ctx,
        NULL,
        "cannot dereference a value of type '%s'",
        type_name ? type_name : "<unknown>");
  }
  return clux_context_create_value(ctx, base, view, NULL, NULL);
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

static bool ptr_type_equal(clux_type_t *self, clux_type_t *other) {
  if (clux_type_get_kind(other) != CLUX_TYPE_PTR) {
    return false;
  }
  clux_ptr_meta_t *self_meta = clux_type_get_meta(self);
  clux_ptr_meta_t *other_meta = clux_type_get_meta(other);
  if (!self_meta || !other_meta) {
    return false;
  }
  return clux_type_equal(self_meta->base, other_meta->base);
}

/* 指针协变：*T extends *U 当且仅当 T extends U。 */
static bool ptr_type_extends(clux_type_t *self, clux_type_t *base) {
  if (clux_type_get_kind(base) != CLUX_TYPE_PTR) {
    return false;
  }
  clux_ptr_meta_t *self_meta = clux_type_get_meta(self);
  clux_ptr_meta_t *base_meta = clux_type_get_meta(base);
  if (!self_meta || !base_meta) {
    return false;
  }
  return clux_type_extends(self_meta->base, base_meta->base);
}

static clux_vtable_t g_ptr_vtable = {
    .eq = ptr_eq,
    .ne = ptr_ne,
    .deref = ptr_deref,
    .type_equal = ptr_type_equal,
    .type_extends = ptr_type_extends,
};

clux_vtable_t *get_clux_ptr_vtable(void) { return &g_ptr_vtable; }

/* 桶去重判据：base 语义相等即视为同一 ptr 类型（跨实例、递归复合均命中），
   由 clux_type_equal 回答。 */
static bool ptr_child_base_matches(clux_type_t *child, void *user_data) {
  clux_ptr_meta_t *meta = clux_type_get_meta(child);
  return meta && clux_type_equal(meta->base, (clux_type_t *)user_data);
}

clux_type_t *clux_type_get_ptr_type(clux_type_t *base) {
  clux_type_t *existing =
      clux_type_find_child(base, CLUX_TYPE_PTR, ptr_child_base_matches, base);
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
