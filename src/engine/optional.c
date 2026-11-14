#include "engine/optional.h"
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

typedef struct _clux_optional_meta_t {
  clux_type_t *base;
} clux_optional_meta_t;

static clux_optional_meta_t *optional_meta(clux_type_t *type) {
  return clux_type_get_meta(type);
}

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static bool optional_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_OPTIONAL:
    return true;
  default:
    return false;
  }
}

static bool optional_read(clux_value_t *value, bool *out_present) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  if (!raw) {
    return false;
  }
  *out_present = *(bool *)raw;
  return true;
}

/* 断言取值（#take）：some 返回 base 类型的 view（与 payload 内存共享），
   none 是运行期错误；写入由通用 assign 完成。 */
static clux_value_t *optional_take(clux_context_t *ctx,
                                   clux_value_t *optional) {
  clux_type_t *type = clux_value_get_type(optional);
  clux_type_t *base = optional_meta(type)->base;
  if (clux_value_is_shadow(optional)) {
    return clux_context_create_shadow_value(ctx, base);
  }
  bool present = false;
  ptr_handle_t data = clux_value_get_data(optional);
  if (!optional_read(optional, &present)) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot take a value from an invalid optional");
  }
  if (!present) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        ctx,
        NULL,
        "cannot take a value from a none optional of type '%s'",
        type_name ? type_name : "<unknown>");
  }
  ptr_handle_t view = ptr_offset(data, clux_optional_payload_offset(base));
  if (!view) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot take a value from an invalid optional");
  }
  return clux_context_create_value(ctx, base, view, NULL, NULL);
}

/* 判空查询（#is_none）：读首字节 present 标志，none 返回 true。 */
static clux_value_t *optional_is_none(clux_context_t *ctx,
                                      clux_value_t *optional) {
  clux_type_t *bool_type = clux_context_get_bool_type(ctx);
  if (clux_value_is_shadow(optional)) {
    return clux_context_create_shadow_value(ctx, bool_type);
  }
  bool present = false;
  if (!optional_read(optional, &present)) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot test an invalid optional for none");
  }
  return clux_context_create_bool(ctx, !present, NULL, NULL);
}

/* 同类型比较：present 相等且 some 时比 payload（padding 不参与比较）。 */
static clux_value_t *optional_compare(clux_context_t *ctx,
                                      clux_value_t *left,
                                      clux_value_t *right,
                                      clux_binary_op_t op) {
  if (is_error_value(left)) {
    return left;
  }
  if (is_error_value(right)) {
    return right;
  }
  clux_type_t *type = clux_value_get_type(left);
  clux_type_t *right_type = clux_value_get_type(right);
  clux_type_t *bool_type = clux_context_get_bool_type(ctx);
  bool right_is_optional = optional_kind(clux_type_get_kind(right_type)) &&
                           clux_type_equal(optional_meta(type)->base,
                                           optional_meta(right_type)->base);
  if (!right_is_optional) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "operator '%s' requires operands of the same type",
        op == CLUX_OP_EQ ? "==" : "!=");
  }
  if (clux_value_is_shadow(left) || clux_value_is_shadow(right)) {
    return clux_context_create_shadow_value(ctx, bool_type);
  }
  bool left_present = false;
  bool right_present = false;
  if (!optional_read(left, &left_present) ||
      !optional_read(right, &right_present)) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot compare invalid optionals");
  }
  bool equal = left_present == right_present;
  if (equal && left_present) {
    clux_type_t *base = optional_meta(type)->base;
    size_t offset = clux_optional_payload_offset(base);
    void *left_payload = ptr_to_raw(clux_value_get_data(left), offset);
    void *right_payload = ptr_to_raw(clux_value_get_data(right), offset);
    if (!left_payload || !right_payload) {
      return clux_context_create_error_message(
          ctx, NULL, "cannot compare invalid optionals");
    }
    equal = memcmp(left_payload, right_payload, clux_type_get_size(base)) == 0;
  }
  return clux_context_create_bool(
      ctx, op == CLUX_OP_EQ ? equal : !equal, NULL, NULL);
}

static clux_value_t *
optional_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return optional_compare(ctx, left, right, CLUX_OP_EQ);
}

static clux_value_t *
optional_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return optional_compare(ctx, left, right, CLUX_OP_NE);
}

static bool optional_type_equal(clux_type_t *self, clux_type_t *other) {
  if (clux_type_get_kind(other) != CLUX_TYPE_OPTIONAL) {
    return false;
  }
  clux_optional_meta_t *self_meta = optional_meta(self);
  clux_optional_meta_t *other_meta = optional_meta(other);
  if (!self_meta || !other_meta) {
    return false;
  }
  return clux_type_equal(self_meta->base, other_meta->base);
}

/* optional 协变：?T extends ?U 当且仅当 T extends U。 */
static bool optional_type_extends(clux_type_t *self, clux_type_t *base) {
  if (clux_type_get_kind(base) != CLUX_TYPE_OPTIONAL) {
    return false;
  }
  clux_optional_meta_t *self_meta = optional_meta(self);
  clux_optional_meta_t *base_meta = optional_meta(base);
  if (!self_meta || !base_meta) {
    return false;
  }
  return clux_type_extends(self_meta->base, base_meta->base);
}

static clux_vtable_t g_optional_vtable = {
    .eq = optional_eq,
    .ne = optional_ne,
    .is_none = optional_is_none,
    .take = optional_take,
    .type_equal = optional_type_equal,
    .type_extends = optional_type_extends,
};

clux_vtable_t *get_clux_optional_vtable(void) { return &g_optional_vtable; }

clux_type_t *clux_type_get_optional_base(clux_type_t *type) {
  if (!type || !optional_kind(clux_type_get_kind(type))) {
    return NULL;
  }
  clux_optional_meta_t *meta = optional_meta(type);
  return meta ? meta->base : NULL;
}

size_t clux_optional_payload_offset(clux_type_t *base) {
  /* present 标志占首字节，payload 从 base 对齐处开始。 */
  return clux_type_get_align(base) < 1 ? 1 : clux_type_get_align(base);
}

/* 桶去重判据：载荷 base 语义相等即视为同一 optional 类型，
   由 clux_type_equal 回答。 */
static bool optional_child_base_matches(clux_type_t *child, void *user_data) {
  clux_optional_meta_t *meta = clux_type_get_meta(child);
  return meta && clux_type_equal(meta->base, (clux_type_t *)user_data);
}

clux_type_t *clux_type_get_optional_type(clux_type_t *base) {
  clux_type_t *existing = clux_type_find_child(
      base, CLUX_TYPE_OPTIONAL, optional_child_base_matches, base);
  if (existing) {
    return existing;
  }
  const char *base_name = clux_type_get_name(base);
  base_name = base_name == NULL ? "<unknown>" : base_name;
  size_t len = strlen(base_name) + 2;
  char name[len];
  (void)snprintf(name, len, "?%s", base_name);
  allocator_t *allocator = clux_type_get_allocator(base);
  clux_type_t *type = create_clux_type(allocator,
                                       CLUX_TYPE_OPTIONAL,
                                       name,
                                       clux_optional_payload_offset(base) +
                                           clux_type_get_size(base),
                                       clux_type_get_align(base),
                                       get_clux_optional_vtable());
  clux_optional_meta_t *meta =
      clux_type_create_meta(type, sizeof(clux_optional_meta_t));
  meta->base = base;
  clux_type_add_child(base, CLUX_TYPE_OPTIONAL, type);
  return type;
}
