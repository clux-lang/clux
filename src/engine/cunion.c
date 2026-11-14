#include "engine/cunion.h"
#include "core/allocator.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct _clux_cunion_meta_t {
  clux_cunion_member_t *members;
  size_t count;
} clux_cunion_meta_t;

static clux_cunion_meta_t *cunion_meta(clux_type_t *type) {
  return clux_type_get_meta(type);
}

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static bool cunion_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_CUNION:
    return true;
  default:
    return false;
  }
}

size_t clux_cunion_member_count(clux_type_t *type) {
  clux_cunion_meta_t *meta = type ? cunion_meta(type) : NULL;
  return meta ? meta->count : 0;
}

const clux_cunion_member_t *clux_cunion_get_member(clux_type_t *type,
                                                   size_t idx) {
  clux_cunion_meta_t *meta = type ? cunion_meta(type) : NULL;
  if (!meta || idx >= meta->count) {
    return NULL;
  }
  return &meta->members[idx];
}

const clux_cunion_member_t *clux_cunion_find_member(clux_type_t *type,
                                                    const char *name) {
  clux_cunion_meta_t *meta = type ? cunion_meta(type) : NULL;
  if (!meta || !name) {
    return NULL;
  }
  for (size_t idx = 0; idx < meta->count; idx++) {
    if (strcmp(meta->members[idx].name, name) == 0) {
      return &meta->members[idx];
    }
  }
  return NULL;
}

/* 成员读取（x.field）：返回成员类型的读写双向 view，与 union 内存共享
   偏移 0（所有成员重叠）。写入由通用 assign 写穿 view 完成，
   写入即重新解释同一段内存（C union 语义）。 */
static clux_value_t *
cunion_field(clux_context_t *ctx, clux_value_t *value, const char *name) {
  clux_type_t *type = clux_value_get_type(value);
  const clux_cunion_member_t *member = clux_cunion_find_member(type, name);
  if (!member) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(ctx,
                                             NULL,
                                             "cunion '%s' has no member '%s'",
                                             type_name ? type_name
                                                       : "<unknown>",
                                             name);
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, member->type);
  }
  ptr_handle_t view = clux_value_get_data(value);
  if (!view) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot access member '%s' of an invalid cunion", name);
  }
  return clux_context_create_value(ctx, member->type, view, NULL, NULL);
}

/* 按字节比较（白皮书 §5.5）：右操作数是任意 cunion 即可比，按左操作数
   类型的尺寸解释（"按左操作数类型解释"）。 */
static clux_value_t *cunion_compare(clux_context_t *ctx,
                                    clux_value_t *left,
                                    clux_value_t *right,
                                    clux_binary_op_t op) {
  if (is_error_value(left)) {
    return left;
  }
  if (is_error_value(right)) {
    return right;
  }
  if (!cunion_kind(clux_type_get_kind(clux_value_get_type(right)))) {
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
  void *left_raw = ptr_to_raw(clux_value_get_data(left), 0);
  void *right_raw = ptr_to_raw(clux_value_get_data(right), 0);
  if (!left_raw || !right_raw) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot compare invalid cunions");
  }
  bool equal = memcmp(left_raw,
                      right_raw,
                      clux_type_get_size(clux_value_get_type(left))) == 0;
  return clux_context_create_bool(
      ctx, op == CLUX_OP_EQ ? equal : !equal, NULL, NULL);
}

static clux_value_t *
cunion_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return cunion_compare(ctx, left, right, CLUX_OP_EQ);
}

static clux_value_t *
cunion_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return cunion_compare(ctx, left, right, CLUX_OP_NE);
}

/* 结构相等核心：成员数、成员名与成员类型按声明序一一对应即相等，
   类型名不参与判等（白皮书：结构兼容的不同具名类型可以比较）。 */
static bool members_equal(clux_cunion_meta_t *self,
                          const clux_cunion_member_t *members,
                          size_t count) {
  if (!self || self->count != count) {
    return false;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (strcmp(self->members[idx].name, members[idx].name) != 0 ||
        !clux_type_equal(self->members[idx].type, members[idx].type)) {
      return false;
    }
  }
  return true;
}

bool clux_cunion_members_equal(clux_type_t *self,
                               const clux_cunion_member_t *members,
                               size_t count) {
  if (!cunion_kind(clux_type_get_kind(self))) {
    return false;
  }
  return members_equal(cunion_meta(self), members, count);
}

static bool cunion_type_equal(clux_type_t *self, clux_type_t *other) {
  if (!cunion_kind(clux_type_get_kind(other))) {
    return false;
  }
  clux_cunion_meta_t *other_meta = cunion_meta(other);
  return members_equal(cunion_meta(self),
                       other_meta ? other_meta->members : NULL,
                       other_meta ? other_meta->count : 0);
}

static clux_vtable_t g_cunion_vtable = {
    .eq = cunion_eq,
    .ne = cunion_ne,
    .field = cunion_field,
    .type_equal = cunion_type_equal,
};

clux_vtable_t *get_clux_cunion_vtable(void) { return &g_cunion_vtable; }

static size_t cunion_layout_align(const clux_cunion_member_t *members,
                                  size_t count) {
  size_t align = 1;
  for (size_t idx = 0; idx < count; idx++) {
    size_t member_align = clux_type_get_align(members[idx].type);
    if (member_align > align) {
      align = member_align;
    }
  }
  return align;
}

clux_type_t *create_clux_cunion_type(allocator_t *allocator,
                                     const char *name,
                                     const clux_cunion_member_t *members,
                                     size_t count) {
  /* 对齐取成员最大值，尺寸取成员最大值后向上对齐（C ABI 布局）。 */
  size_t align = cunion_layout_align(members, count);
  size_t size = 0;
  for (size_t idx = 0; idx < count; idx++) {
    size_t member_size = clux_type_get_size(members[idx].type);
    if (member_size > size) {
      size = member_size;
    }
  }
  size = (size + align - 1) / align * align;
  clux_type_t *type = create_clux_type(
      allocator, CLUX_TYPE_CUNION, name, size, align, get_clux_cunion_vtable());
  /* meta 与成员表连体成单块分配：delete_clux_type 释放 meta 本体即整块释放 */
  clux_cunion_meta_t *meta = clux_type_create_meta(
      type, sizeof(clux_cunion_meta_t) + sizeof(clux_cunion_member_t) * count);
  meta->members =
      (clux_cunion_member_t *)((char *)meta + sizeof(clux_cunion_meta_t));
  (void)memcpy(meta->members, members, sizeof(clux_cunion_member_t) * count);
  meta->count = count;
  return type;
}
