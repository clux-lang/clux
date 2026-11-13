#include "engine/qualifier.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct _clux_qualifier_meta_t {
  clux_type_t *base;
} clux_qualifier_meta_t;

static bool qualifier_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_CONST:
  case CLUX_TYPE_VOLATILE:
    return true;
  default:
    return false;
  }
}

static const char *qualifier_label(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_CONST:
    return "const";
  case CLUX_TYPE_VOLATILE:
    return "volatile";
  default:
    return "<unknown>";
  }
}

clux_type_t *clux_type_get_qualified_base(clux_type_t *type) {
  if (!type || !qualifier_kind(clux_type_get_kind(type))) {
    return NULL;
  }
  clux_qualifier_meta_t *meta = clux_type_get_meta(type);
  return meta ? meta->base : NULL;
}

clux_type_t *clux_type_unwrap(clux_type_t *type) {
  clux_type_t *base = clux_type_get_qualified_base(type);
  while (base) {
    type = base;
    base = clux_type_get_qualified_base(type);
  }
  return type;
}

clux_type_kind_t clux_type_kind_unwrap(clux_type_t *type) {
  clux_type_t *unwrapped = clux_type_unwrap(type);
  return unwrapped ? clux_type_get_kind(unwrapped) : CLUX_TYPE_ERROR;
}

bool clux_type_is_const(clux_type_t *type) {
  while (type && qualifier_kind(clux_type_get_kind(type))) {
    if (clux_type_get_kind(type) == CLUX_TYPE_CONST) {
      return true;
    }
    type = clux_type_get_qualified_base(type);
  }
  return false;
}

static bool qualifier_type_equal(clux_type_t *self, clux_type_t *other) {
  if (clux_type_get_kind(self) != clux_type_get_kind(other)) {
    return false;
  }
  clux_qualifier_meta_t *self_meta = clux_type_get_meta(self);
  clux_qualifier_meta_t *other_meta = clux_type_get_meta(other);
  if (!self_meta || !other_meta) {
    return false;
  }
  return clux_type_equal(self_meta->base, other_meta->base);
}

/* 限定是收紧：加限定 extends 原类型成立（const i32 extends i32），
   反向不成立（i32 不 extends const i32）；同限定符时递归比较 bases。 */
static bool qualifier_type_extends(clux_type_t *self, clux_type_t *base) {
  clux_qualifier_meta_t *self_meta = clux_type_get_meta(self);
  if (!self_meta) {
    return false;
  }
  if (clux_type_get_kind(base) == clux_type_get_kind(self)) {
    clux_qualifier_meta_t *base_meta = clux_type_get_meta(base);
    return base_meta && clux_type_extends(self_meta->base, base_meta->base);
  }
  return clux_type_extends(self_meta->base, base);
}

/* ---- 静态委托：基于左值创建 base 类型的 view 后走标准入口 ---- */

/* ptr_offset(data,0) 生成内存共享的借用 view；data 为 0（shadow）时
   ptr_offset 返回 0，view 仍是 shadow。view 注册进当前 scope，销毁时
   safe_free 对 view 句柄是无害 no-op，不会双重释放根句柄。 */
static clux_value_t *qualified_view(clux_context_t *ctx, clux_value_t *value) {
  clux_type_t *base = clux_type_get_qualified_base(clux_value_get_type(value));
  ptr_handle_t data = clux_value_get_data(value);
  return clux_context_create_value(
      ctx, base, data ? ptr_offset(data, 0) : 0, NULL, NULL);
}

static clux_value_t *qualified_binary(clux_context_t *ctx,
                                      clux_binary_op_t op,
                                      clux_value_t *left,
                                      clux_value_t *right) {
  return clux_context_binary(ctx, op, qualified_view(ctx, left), right, NULL);
}

static clux_value_t *qualified_prefix(clux_context_t *ctx,
                                      clux_prefix_op_t op,
                                      clux_value_t *value) {
  return clux_context_prefix(ctx, op, qualified_view(ctx, value), NULL);
}

#define QUALIFY_BINARY_FN(NAME, OP)                                            \
  static clux_value_t *NAME(                                                   \
      clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {          \
    return qualified_binary(ctx, OP, left, right);                             \
  }

#define QUALIFY_PREFIX_FN(NAME, OP)                                            \
  static clux_value_t *NAME(clux_context_t *ctx, clux_value_t *value) {        \
    return qualified_prefix(ctx, OP, value);                                   \
  }

QUALIFY_BINARY_FN(qual_add, CLUX_OP_ADD)
QUALIFY_BINARY_FN(qual_sub, CLUX_OP_SUB)
QUALIFY_BINARY_FN(qual_mul, CLUX_OP_MUL)
QUALIFY_BINARY_FN(qual_div, CLUX_OP_DIV)
QUALIFY_BINARY_FN(qual_mod, CLUX_OP_MOD)
QUALIFY_BINARY_FN(qual_shl, CLUX_OP_SHL)
QUALIFY_BINARY_FN(qual_shr, CLUX_OP_SHR)
QUALIFY_BINARY_FN(qual_bit_and, CLUX_OP_AND)
QUALIFY_BINARY_FN(qual_bit_or, CLUX_OP_OR)
QUALIFY_BINARY_FN(qual_bit_xor, CLUX_OP_XOR)
QUALIFY_BINARY_FN(qual_eq, CLUX_OP_EQ)
QUALIFY_BINARY_FN(qual_ne, CLUX_OP_NE)
QUALIFY_BINARY_FN(qual_lt, CLUX_OP_LT)
QUALIFY_BINARY_FN(qual_le, CLUX_OP_LE)
QUALIFY_BINARY_FN(qual_gt, CLUX_OP_GT)
QUALIFY_BINARY_FN(qual_ge, CLUX_OP_GE)

QUALIFY_PREFIX_FN(qual_plus, CLUX_OP_PLUS)
QUALIFY_PREFIX_FN(qual_negative, CLUX_OP_NEGATIVE)
QUALIFY_PREFIX_FN(qual_bit_not, CLUX_OP_NOT)
QUALIFY_PREFIX_FN(qual_logic_not, CLUX_OP_LNOT)

static clux_value_t *
qual_safe_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
  return clux_context_safe_cast(ctx, qualified_view(ctx, value), target, NULL);
}

static clux_value_t *
qual_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
  return clux_context_cast(ctx, qualified_view(ctx, value), target, NULL);
}

static clux_value_t *qual_deref_get(clux_context_t *ctx, clux_value_t *ptr) {
  return clux_context_deref_get(ctx, qualified_view(ctx, ptr), NULL);
}

static clux_value_t *
qual_deref_set(clux_context_t *ctx, clux_value_t *ptr, clux_value_t *value) {
  return clux_context_deref_set(ctx, qualified_view(ctx, ptr), value, NULL);
}

static clux_vtable_t g_qualifier_vtable = {
    .add = qual_add,
    .sub = qual_sub,
    .mul = qual_mul,
    .div = qual_div,
    .mod = qual_mod,
    .shl = qual_shl,
    .shr = qual_shr,
    .bit_and = qual_bit_and,
    .bit_or = qual_bit_or,
    .bit_xor = qual_bit_xor,
    .eq = qual_eq,
    .ne = qual_ne,
    .lt = qual_lt,
    .le = qual_le,
    .gt = qual_gt,
    .ge = qual_ge,
    .plus = qual_plus,
    .negative = qual_negative,
    .bit_not = qual_bit_not,
    .logic_not = qual_logic_not,
    .safe_cast = qual_safe_cast,
    .cast = qual_cast,
    .deref_get = qual_deref_get,
    .deref_set = qual_deref_set,
    .type_equal = qualifier_type_equal,
    .type_extends = qualifier_type_extends,
};

/* 桶去重判据：被限定 base 语义相等即同一限定类型。 */
static bool qualifier_child_base_matches(clux_type_t *child, void *user_data) {
  clux_qualifier_meta_t *meta = clux_type_get_meta(child);
  return meta && clux_type_equal(meta->base, (clux_type_t *)user_data);
}

clux_type_t *clux_type_get_qualified_type(clux_type_t *base,
                                          clux_type_kind_t qualifier) {
  if (!base || !qualifier_kind(qualifier)) {
    return NULL;
  }
  clux_type_t *existing =
      clux_type_find_child(base, qualifier, qualifier_child_base_matches, base);
  if (existing) {
    return existing;
  }
  const char *base_name = clux_type_get_name(base);
  base_name = base_name == NULL ? "<unknown>" : base_name;
  const char *label = qualifier_label(qualifier);
  size_t len = strlen(label) + strlen(base_name) + 2;
  char name[len];
  (void)snprintf(name, len, "%s %s", label, base_name);
  allocator_t *allocator = clux_type_get_allocator(base);
  clux_type_t *type = create_clux_type(allocator,
                                       qualifier,
                                       name,
                                       clux_type_get_size(base),
                                       clux_type_get_align(base),
                                       &g_qualifier_vtable);
  clux_qualifier_meta_t *meta =
      clux_type_create_meta(type, sizeof(clux_qualifier_meta_t));
  meta->base = base;
  clux_type_add_child(base, qualifier, type);
  return type;
}
