#include "engine/cunion.h"
#include "core/allocator.h"
#include "core/vec.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct _clux_cunion_meta_t {
  vec_t *members;
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
  return meta ? vec_get_size(meta->members) : 0;
}

const clux_cunion_member_t *clux_cunion_get_member(clux_type_t *type,
                                                   size_t idx) {
  clux_cunion_meta_t *meta = type ? cunion_meta(type) : NULL;
  if (!meta || idx >= vec_get_size(meta->members)) {
    return NULL;
  }
  return vec_get(meta->members, idx);
}

const clux_cunion_member_t *clux_cunion_find_member(clux_type_t *type,
                                                    const char *name) {
  clux_cunion_meta_t *meta = type ? cunion_meta(type) : NULL;
  if (!meta || !name) {
    return NULL;
  }
  for (size_t idx = 0; idx < vec_get_size(meta->members); idx++) {
    clux_cunion_member_t *member = vec_get(meta->members, idx);
    if (strcmp(member->name, name) == 0) {
      return member;
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
  clux_type_t *bool_type = clux_context_builtin_type(ctx, CLUX_TYPE_BOOL);
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
                          const clux_cunion_member_t **members,
                          size_t count) {
  if (!self || vec_get_size(self->members) != count) {
    return false;
  }
  for (size_t idx = 0; idx < count; idx++) {
    const clux_cunion_member_t *src = vec_get_const(self->members, idx);
    const clux_cunion_member_t *dst = members[idx];
    if (strcmp(src->name, dst->name) != 0 ||
        !clux_type_equal(src->type, dst->type)) {
      return false;
    }
  }
  return true;
}

bool clux_cunion_members_equal(clux_type_t *self,
                               const clux_cunion_member_t **members,
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
  const clux_cunion_member_t **other_members =
      (const clux_cunion_member_t **)vec_get_data_const(other_meta->members);
  return members_equal(
      cunion_meta(self), other_members, vec_get_size(other_meta->members));
}

/* meta 析构：释放成员 vec 与各成员的自有资源（成员块与自拷的 name）。
   成员 type 是借用（归类型 owner），不在此释放；meta 本体由
   delete_clux_type 统一释放。 */
static void cunion_delete_meta(clux_type_t *self) {
  clux_cunion_meta_t *meta = cunion_meta(self);
  if (!meta || !meta->members) {
    return;
  }
  allocator_t *allocator = clux_type_get_allocator(self);
  for (size_t idx = 0; idx < vec_get_size(meta->members); idx++) {
    clux_cunion_member_t *member = vec_get(meta->members, idx);
    allocator_free(allocator, (void **)&member->name);
    allocator_free(allocator, (void **)&member);
  }
  delete_vec(&meta->members);
}

static clux_vtable_t g_cunion_vtable = {
    .eq = cunion_eq,
    .ne = cunion_ne,
    .field = cunion_field,
    .type_equal = cunion_type_equal,
    .delete_meta = cunion_delete_meta,
};

clux_vtable_t *get_clux_cunion_vtable(void) { return &g_cunion_vtable; }

/* 两阶段构造（与 tag union 同构，适配递归引用）：create 建空壳
   （size/align 为 0，未 seal），add_member 逐个填充成员（name 自拷，
   类型借用类型 owner），seal 冻结布局。 */
clux_type_t *create_clux_cunion_type(allocator_t *allocator, const char *name) {
  clux_type_t *type = create_clux_type(
      allocator, CLUX_TYPE_CUNION, name, 0, 0, get_clux_cunion_vtable());
  clux_cunion_meta_t *meta =
      clux_type_create_meta(type, sizeof(clux_cunion_meta_t));
  meta->members = create_vec(allocator);
  return type;
}

bool clux_cunion_type_add_member(clux_type_t *self,
                                 allocator_t *allocator,
                                 const char *name,
                                 clux_type_t *type) {
  if (clux_type_is_seal(self)) {
    return false;
  }
  clux_cunion_meta_t *meta = clux_type_get_meta(self);
  for (size_t idx = 0; idx < vec_get_size(meta->members); idx++) {
    clux_cunion_member_t *tmp = vec_get(meta->members, idx);
    if (strcmp(tmp->name, name) == 0) {
      return false;
    }
  }
  clux_cunion_member_t *mem = allocator_alloc(allocator, sizeof(*mem));
  char *name_copy = allocator_alloc(allocator, strlen(name) + 1);
  (void)strcpy(name_copy, name);
  mem->name = name_copy;
  mem->type = type;
  (void)vec_push(meta->members, mem);
  return true;
}

/* C ABI 布局定局：align = max(成员 align)（无 tag，下限 1），
   size = round_up(max(成员 size), align)，所有成员共享偏移 0。 */
void clux_cunion_type_seal(clux_type_t *self) {
  clux_cunion_meta_t *meta = clux_type_get_meta(self);
  size_t align = 1;
  size_t size = 0;
  for (size_t idx = 0; idx < vec_get_size(meta->members); idx++) {
    clux_cunion_member_t *member = vec_get(meta->members, idx);
    size_t member_align = clux_type_get_align(member->type);
    if (member_align > align) {
      align = member_align;
    }
    size_t member_size = clux_type_get_size(member->type);
    if (member_size > size) {
      size = member_size;
    }
  }
  size = (size + align - 1) / align * align;
  (void)clux_type_set_size(self, size);
  (void)clux_type_set_align(self, align);
  clux_type_seal(self);
}
