/* opaque 类型：无类型载荷（FFI / 显式类型擦除边界，白皮书 §2.1）。
   布局等同裸指针：data 存 ptr_handle_t。转换规则：
   - 任何指针类型 → opaque 隐式放行（各指针类型自己的 safe_cast 负责）；
   - opaque → 任意指针必须显式 as（本 vtable 的 cast 槽位）；
   - opaque 与非指针类型之间没有可定义的转换，一律拒绝。 */
#include "engine/opaque.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static bool opaque_read(clux_value_t *value, ptr_handle_t *out) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  if (!raw) {
    return false;
  }
  *out = *(ptr_handle_t *)raw;
  return true;
}

static clux_value_t *opaque_cast_error(clux_context_t *ctx,
                                       clux_value_t *value,
                                       clux_type_t *target,
                                       const char *what) {
  const char *source_name = clux_type_get_name(clux_value_get_type(value));
  const char *target_name = clux_type_get_name(target);
  return clux_context_create_error_message(
      ctx,
      NULL,
      "%s '%s' to '%s'",
      what,
      source_name ? source_name : "<unknown>",
      target_name ? target_name : "<unknown>");
}

/* 隐式转换点（assign）：opaque 是类型擦除的终点类型，只接受
   opaque ↔ opaque 的限定归一；转出必须显式 as。非 opaque 源走到
   这里说明源类型没有自己的放行逻辑，直接拒绝。 */
static clux_value_t *opaque_safe_cast(clux_context_t *ctx,
                                      clux_value_t *value,
                                      clux_type_t *target) {
  if (is_error_value(value)) {
    return value;
  }
  clux_type_t *source = clux_value_get_type(value);
  if (clux_type_kind_unwrap(target) != CLUX_TYPE_OPAQUE ||
      clux_type_kind_unwrap(source) != CLUX_TYPE_OPAQUE) {
    return opaque_cast_error(ctx, value, target, "cannot safely cast");
  }
  if (source == target) {
    return value;
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, target);
  }
  ptr_handle_t handle = 0;
  if (!opaque_read(value, &handle)) {
    return opaque_cast_error(ctx, value, target, "cannot safely cast");
  }
  return clux_context_create_ptr(ctx, target, handle, NULL, NULL);
}

/* 显式转换（as）：任意指针类型合法——擦除的载荷重新获得类型；
   其余类型拒绝（opaque 没有可定义的非指针转换）。 */
static clux_value_t *
opaque_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
  if (is_error_value(value)) {
    return value;
  }
  clux_type_t *source = clux_value_get_type(value);
  clux_type_kind_t target_kind = clux_type_kind_unwrap(target);
  if (target_kind != CLUX_TYPE_PTR && target_kind != CLUX_TYPE_OPAQUE) {
    return opaque_cast_error(ctx, value, target, "cannot cast");
  }
  if (source == target) {
    return value;
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, target);
  }
  ptr_handle_t handle = 0;
  if (!opaque_read(value, &handle)) {
    return opaque_cast_error(ctx, value, target, "cannot cast");
  }
  return clux_context_create_ptr(ctx, target, handle, NULL, NULL);
}

static clux_value_t *opaque_compare(clux_context_t *ctx,
                                    clux_value_t *left,
                                    clux_value_t *right,
                                    clux_binary_op_t op) {
  if (is_error_value(left)) {
    return left;
  }
  if (is_error_value(right)) {
    return right;
  }
  if (clux_type_kind_unwrap(clux_value_get_type(right)) != CLUX_TYPE_OPAQUE) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "operator '%s' requires operands of the same type",
        op == CLUX_OP_EQ ? "==" : "!=");
  }
  clux_type_t *bool_type = clux_context_builtin_type(ctx, CLUX_TYPE_BOOL);
  if (clux_value_is_shadow(left) || clux_value_is_shadow(right)) {
    return clux_context_create_shadow_value(ctx, bool_type);
  }
  ptr_handle_t left_handle = 0;
  ptr_handle_t right_handle = 0;
  if (!opaque_read(left, &left_handle) || !opaque_read(right, &right_handle)) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot compare invalid opaque values");
  }
  bool equal = left_handle == right_handle;
  return clux_context_create_bool(
      ctx, op == CLUX_OP_EQ ? equal : !equal, NULL, NULL);
}

static clux_value_t *
opaque_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return opaque_compare(ctx, left, right, CLUX_OP_EQ);
}

static clux_value_t *
opaque_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return opaque_compare(ctx, left, right, CLUX_OP_NE);
}

static bool opaque_type_equal(clux_type_t *self, clux_type_t *other) {
  return clux_type_get_kind(self) == clux_type_get_kind(other);
}

static bool opaque_type_extends(clux_type_t *self, clux_type_t *base) {
  return clux_type_get_kind(self) == clux_type_get_kind(base);
}

static clux_vtable_t g_opaque_vtable = {
    .eq = opaque_eq,
    .ne = opaque_ne,
    .safe_cast = opaque_safe_cast,
    .cast = opaque_cast,
    .type_equal = opaque_type_equal,
    .type_extends = opaque_type_extends,
};

clux_vtable_t *get_clux_opaque_vtable(void) { return &g_opaque_vtable; }
