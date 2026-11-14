#include "engine/union.h"
#include "core/allocator.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct _clux_union_meta_t {
  clux_union_member_t *members;
  size_t count;
} clux_union_meta_t;

static clux_union_meta_t *union_meta(clux_type_t *type) {
  return clux_type_get_meta(type);
}

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static bool union_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_UNION:
    return true;
  default:
    return false;
  }
}

size_t clux_union_member_count(clux_type_t *type) {
  clux_union_meta_t *meta = type ? union_meta(type) : NULL;
  return meta ? meta->count : 0;
}

const clux_union_member_t *clux_union_get_member(clux_type_t *type,
                                                 size_t idx) {
  clux_union_meta_t *meta = type ? union_meta(type) : NULL;
  if (!meta || idx >= meta->count) {
    return NULL;
  }
  return &meta->members[idx];
}

const clux_union_member_t *clux_union_find_member(clux_type_t *type,
                                                  const char *name) {
  clux_union_meta_t *meta = type ? union_meta(type) : NULL;
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

size_t clux_union_payload_offset(clux_type_t *type) {
  /* tag 定宽 u32 占偏移 0，payload 从类型对齐处开始：对齐 =
     max(4, 成员最大对齐)，round_up(sizeof(u32), align) == align（align >= 4
     且为 2 的幂），故 payload 偏移恒等于类型对齐值。 */
  return clux_type_get_align(type);
}

static bool union_read_tag(clux_value_t *value, uint32_t *out_tag) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  if (!raw) {
    return false;
  }
  *out_tag = *(uint32_t *)raw;
  return true;
}

static size_t member_index(clux_type_t *type,
                           const clux_union_member_t *member) {
  clux_union_meta_t *meta = union_meta(type);
  return (size_t)(member - meta->members);
}

/* 成员读取（x.member）：tag 匹配该成员时返回成员类型的读写双向 view
   （与 payload 内存共享），tag 不匹配是运行期 error（先 is 判定再读取）。
   写入由通用 assign 写穿 view 完成。 */
static clux_value_t *
union_field(clux_context_t *ctx, clux_value_t *value, const char *name) {
  clux_type_t *type = clux_value_get_type(value);
  const clux_union_member_t *member = clux_union_find_member(type, name);
  if (!member) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(ctx,
                                             NULL,
                                             "union '%s' has no member '%s'",
                                             type_name ? type_name
                                                       : "<unknown>",
                                             name);
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, member->type);
  }
  uint32_t tag = 0;
  if (!union_read_tag(value, &tag)) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot access member '%s' of an invalid union", name);
  }
  if (tag != member_index(type, member)) {
    const clux_union_member_t *current = clux_union_get_member(type, tag);
    const char *current_name = current ? current->name : "<corrupt tag>";
    return clux_context_create_error_message(
        ctx,
        NULL,
        "union is not '%s': current member is '%s', test with 'is' first",
        name,
        current_name);
  }
  ptr_handle_t view =
      ptr_offset(clux_value_get_data(value), clux_union_payload_offset(type));
  if (!view) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot access member '%s' of an invalid union", name);
  }
  return clux_context_create_value(ctx, member->type, view, NULL, NULL);
}

/* is 判定（value is Member，白皮书 §5.7）：当前 tag 即该成员声明序号时
   为 true，结果为 bool。 */
static clux_value_t *
union_is_member(clux_context_t *ctx, clux_value_t *value, const char *name) {
  clux_type_t *type = clux_value_get_type(value);
  clux_type_t *bool_type = clux_context_get_bool_type(ctx);
  const clux_union_member_t *member = clux_union_find_member(type, name);
  if (!member) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(ctx,
                                             NULL,
                                             "union '%s' has no member '%s'",
                                             type_name ? type_name
                                                       : "<unknown>",
                                             name);
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, bool_type);
  }
  uint32_t tag = 0;
  if (!union_read_tag(value, &tag)) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot test an invalid union for membership");
  }
  return clux_context_create_bool(
      ctx, tag == member_index(type, member), NULL, NULL);
}

/* 同类型比较（白皮书 §5.5）：先比 tag，相同再按该成员的载荷递归比较
   （复用成员类型 vtable 的 eq 槽位，padding 与未激活成员不参与比较）。 */
static clux_value_t *union_compare(clux_context_t *ctx,
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
  if (!union_kind(clux_type_get_kind(clux_value_get_type(right))) ||
      !clux_type_equal(type, clux_value_get_type(right))) {
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
  uint32_t left_tag = 0;
  uint32_t right_tag = 0;
  if (!union_read_tag(left, &left_tag) || !union_read_tag(right, &right_tag)) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot compare invalid unions");
  }
  bool equal = left_tag == right_tag;
  if (equal) {
    const clux_union_member_t *member = clux_union_get_member(type, left_tag);
    if (!member) {
      return clux_context_create_error_message(
          ctx, NULL, "cannot compare invalid unions");
    }
    clux_vtable_t *member_vt = clux_type_get_vtable(member->type);
    if (!member_vt || !member_vt->eq) {
      return clux_context_create_error_message(
          ctx,
          NULL,
          "union member '%s' of type '%s' does not support operator '=='",
          member->name,
          clux_type_get_name(member->type) ? clux_type_get_name(member->type)
                                           : "<unknown>");
    }
    size_t offset = clux_union_payload_offset(type);
    ptr_handle_t left_view = ptr_offset(clux_value_get_data(left), offset);
    ptr_handle_t right_view = ptr_offset(clux_value_get_data(right), offset);
    if (!left_view || !right_view) {
      return clux_context_create_error_message(
          ctx, NULL, "cannot compare invalid unions");
    }
    clux_value_t *left_payload =
        clux_context_create_value(ctx, member->type, left_view, NULL, NULL);
    clux_value_t *right_payload =
        clux_context_create_value(ctx, member->type, right_view, NULL, NULL);
    clux_value_t *result = member_vt->eq(ctx, left_payload, right_payload);
    if (is_error_value(result)) {
      return result;
    }
    void *raw = ptr_to_raw(clux_value_get_data(result), 0);
    equal = raw ? *(bool *)raw : false;
  }
  return clux_context_create_bool(
      ctx, op == CLUX_OP_EQ ? equal : !equal, NULL, NULL);
}

static clux_value_t *
union_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return union_compare(ctx, left, right, CLUX_OP_EQ);
}

static clux_value_t *
union_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return union_compare(ctx, left, right, CLUX_OP_NE);
}

/* 结构相等核心：成员数、成员名与成员类型按声明序一一对应即相等，
   类型名不参与判等（白皮书：结构兼容的不同具名类型可以比较）。 */
static bool members_equal(clux_union_meta_t *self,
                          const clux_union_member_t *members,
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

bool clux_union_members_equal(clux_type_t *self,
                              const clux_union_member_t *members,
                              size_t count) {
  if (!union_kind(clux_type_get_kind(self))) {
    return false;
  }
  return members_equal(union_meta(self), members, count);
}

static bool union_type_equal(clux_type_t *self, clux_type_t *other) {
  if (!union_kind(clux_type_get_kind(other))) {
    return false;
  }
  clux_union_meta_t *other_meta = union_meta(other);
  return members_equal(union_meta(self),
                       other_meta ? other_meta->members : NULL,
                       other_meta ? other_meta->count : 0);
}

/* 激活成员（set_member）：切换 tag 并清零 payload，返回原值。
   field 的 tag 检查保证读取只会命中已激活的成员。 */
clux_value_t *clux_union_set_member(clux_context_t *ctx,
                                    clux_value_t *value,
                                    const char *name) {
  clux_type_t *type = clux_value_get_type(value);
  const clux_union_member_t *member = clux_union_find_member(type, name);
  if (!member) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(ctx,
                                             NULL,
                                             "union '%s' has no member '%s'",
                                             type_name ? type_name
                                                       : "<unknown>",
                                             name);
  }
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  if (!raw) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot activate a member of an invalid union");
  }
  *(uint32_t *)raw = (uint32_t)member_index(type, member);
  void *payload =
      ptr_to_raw(clux_value_get_data(value), clux_union_payload_offset(type));
  if (payload) {
    memset(
        payload, 0, clux_type_get_size(type) - clux_union_payload_offset(type));
  }
  return value;
}

static clux_vtable_t g_union_vtable = {
    .eq = union_eq,
    .ne = union_ne,
    .field = union_field,
    .is_member = union_is_member,
    .type_equal = union_type_equal,
};

clux_vtable_t *get_clux_union_vtable(void) { return &g_union_vtable; }

static size_t union_layout_align(const clux_union_member_t *members,
                                 size_t count) {
  size_t align = sizeof(uint32_t); /* tag u32 定宽，对齐下限 4 */
  for (size_t idx = 0; idx < count; idx++) {
    size_t member_align = clux_type_get_align(members[idx].type);
    if (member_align > align) {
      align = member_align;
    }
  }
  return align;
}

clux_type_t *create_clux_union_type(allocator_t *allocator,
                                    const char *name,
                                    const clux_union_member_t *members,
                                    size_t count) {
  /* tag u32 在偏移 0，payload 从对齐处开始：
     align = max(sizeof(u32), max(成员 align))，
     size = round_up(align + max(成员 size), align)。 */
  size_t align = union_layout_align(members, count);
  size_t payload = 0;
  for (size_t idx = 0; idx < count; idx++) {
    size_t member_size = clux_type_get_size(members[idx].type);
    if (member_size > payload) {
      payload = member_size;
    }
  }
  size_t size = (align + payload + align - 1) / align * align;
  clux_type_t *type = create_clux_type(
      allocator, CLUX_TYPE_UNION, name, size, align, get_clux_union_vtable());
  /* meta 与成员表连体成单块分配：delete_clux_type 释放 meta 本体即整块释放 */
  clux_union_meta_t *meta = clux_type_create_meta(
      type, sizeof(clux_union_meta_t) + sizeof(clux_union_member_t) * count);
  meta->members =
      (clux_union_member_t *)((char *)meta + sizeof(clux_union_meta_t));
  (void)memcpy(meta->members, members, sizeof(clux_union_member_t) * count);
  meta->count = count;
  return type;
}
