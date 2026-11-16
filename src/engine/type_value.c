#include "engine/type_value.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* type value 封装：值描述一个类型，data 存被描述类型的指针。
   只提供 eq 与 value_extends 两个能力，均代理类型层：
   eq 按语义判等（clux_type_equal，类型名与 attr/method/subtype 不参与，
   *T 递归比较 base），extends 按派生关系（clux_type_extends，
   *Derived 鸭子/协变递归成立）。 */
static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static clux_type_t *type_value_read(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(clux_type_t **)raw : NULL;
}

static clux_value_t *
type_value_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  if (is_error_value(left)) {
    return left;
  }
  if (is_error_value(right)) {
    return right;
  }
  if (clux_type_kind_unwrap(clux_value_get_type(right)) != CLUX_TYPE_TYPE) {
    return clux_context_create_error_message(
        ctx, NULL, "operator '==' requires operands of the same type");
  }
  clux_type_t *bool_type = clux_context_builtin_type(ctx, CLUX_TYPE_BOOL);
  if (clux_value_is_shadow(left) || clux_value_is_shadow(right)) {
    return clux_context_create_shadow_value(ctx, bool_type);
  }
  bool equal = clux_type_equal(type_value_read(left), type_value_read(right));
  return clux_context_create_bool(ctx, equal, NULL, NULL);
}

static bool type_value_extends(clux_value_t *self, clux_value_t *base) {
  clux_type_t *self_type = type_value_read(self);
  clux_type_t *base_type = type_value_read(base);
  if (!self_type || !base_type) {
    return false;
  }
  return clux_type_extends(self_type, base_type);
}

static clux_vtable_t g_type_value_vtable = {
    .eq = type_value_eq,
    .value_extends = type_value_extends,
};

clux_vtable_t *get_clux_type_value_vtable(void) { return &g_type_value_vtable; }
