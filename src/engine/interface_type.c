/* interface 类型（白皮书 §3.10、§6.9）：纯编译期结构约束——成员是方法
   签名（名 + func 类型），满足是结构的（提供全部成员名+签名一致即满足，
   不要求显式 implements），不产生运行期对象/虚表。无值、无运算——
   vtable 只提供结构类型判等，其余槽位留空由 context 兜底报不支持。
   永不 seal（与 void 同族）：is_seal 是完整性判据，create_value 的 seal
   门禁由此从根上拒绝 interface 作值类型（变量创建/类型标注等值位置）。 */
#include "engine/interface_type.h"
#include "core/allocator.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct _clux_interface_meta_t {
  clux_interface_member_t *members;
  size_t count;
} clux_interface_meta_t;

static clux_interface_meta_t *interface_meta(clux_type_t *type) {
  return clux_type_get_meta(type);
}

static bool interface_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_INTERFACE:
    return true;
  default:
    return false;
  }
}

size_t clux_interface_member_count(clux_type_t *type) {
  clux_interface_meta_t *meta = type ? interface_meta(type) : NULL;
  return meta ? meta->count : 0;
}

const clux_interface_member_t *clux_interface_get_member(clux_type_t *type,
                                                         size_t idx) {
  clux_interface_meta_t *meta = type ? interface_meta(type) : NULL;
  if (!meta || idx >= meta->count) {
    return NULL;
  }
  return &meta->members[idx];
}

const clux_interface_member_t *clux_interface_find_member(clux_type_t *type,
                                                          const char *name) {
  clux_interface_meta_t *meta = type ? interface_meta(type) : NULL;
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

/* 结构相等核心：成员名与签名（func 类型判等）按声明序一一对应即相等，
   类型名不参与判等（与 enum/union 一致：结构兼容的不同具名类型可以
   比较）。 */
static bool members_equal(clux_interface_meta_t *self,
                          const clux_interface_member_t *members,
                          size_t count) {
  if (!self || self->count != count) {
    return false;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (strcmp(self->members[idx].name, members[idx].name) != 0 ||
        !clux_type_equal(self->members[idx].func, members[idx].func)) {
      return false;
    }
  }
  return true;
}

bool clux_interface_members_equal(clux_type_t *self,
                                  const clux_interface_member_t *members,
                                  size_t count) {
  if (!interface_kind(clux_type_get_kind(self))) {
    return false;
  }
  return members_equal(interface_meta(self), members, count);
}

static bool interface_type_equal(clux_type_t *self, clux_type_t *other) {
  if (!interface_kind(clux_type_get_kind(other))) {
    return false;
  }
  clux_interface_meta_t *other_meta = interface_meta(other);
  return members_equal(interface_meta(self),
                       other_meta ? other_meta->members : NULL,
                       other_meta ? other_meta->count : 0);
}

static clux_vtable_t g_interface_vtable = {
    .type_equal = interface_type_equal,
};

clux_vtable_t *get_clux_interface_vtable(void) { return &g_interface_vtable; }

clux_type_t *create_clux_interface_type(allocator_t *allocator,
                                        const char *name,
                                        const clux_interface_member_t *members,
                                        size_t count) {
  /* 布局无意义（无运行期对象）：size 0、align 1，与 void 一致。 */
  clux_type_t *type = create_clux_type(
      allocator, CLUX_TYPE_INTERFACE, name, 0, 1, get_clux_interface_vtable());
  /* meta 与成员表连体成单块分配：delete_clux_type 释放 meta 本体即整块
     释放；成员名与签名类型均借用（context 路径经 strs 驻留/类型表托管），
     无自有堆资源（delete_meta 留空）。 */
  clux_interface_meta_t *meta = clux_type_create_meta(
      type,
      sizeof(clux_interface_meta_t) + sizeof(clux_interface_member_t) * count);
  meta->members =
      (clux_interface_member_t *)((char *)meta + sizeof(clux_interface_meta_t));
  if (count > 0) {
    (void)memcpy(
        meta->members, members, sizeof(clux_interface_member_t) * count);
  }
  meta->count = count;
  /* 永不 seal（void 先例）：interface 是天然不完整的约束类型，值创建的
     seal 门禁由此拒绝其出现在值位置。 */
  return type;
}
