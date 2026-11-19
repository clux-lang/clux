#include "engine/enum.h"
#include "core/allocator.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct _clux_enum_meta_t {
  clux_type_t *underlying;
  clux_enum_variant_t *variants;
  size_t count;
} clux_enum_meta_t;

static clux_enum_meta_t *enum_meta(clux_type_t *type) {
  return clux_type_get_meta(type);
}

static bool enum_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_ENUM:
    return true;
  default:
    return false;
  }
}

size_t clux_enum_variant_count(clux_type_t *type) {
  clux_enum_meta_t *meta = type ? enum_meta(type) : NULL;
  return meta ? meta->count : 0;
}

const clux_enum_variant_t *clux_enum_get_variant(clux_type_t *type,
                                                 size_t idx) {
  clux_enum_meta_t *meta = type ? enum_meta(type) : NULL;
  if (!meta || idx >= meta->count) {
    return NULL;
  }
  return &meta->variants[idx];
}

const clux_enum_variant_t *clux_enum_find_variant(clux_type_t *type,
                                                  const char *name) {
  clux_enum_meta_t *meta = type ? enum_meta(type) : NULL;
  if (!meta || !name) {
    return NULL;
  }
  for (size_t idx = 0; idx < meta->count; idx++) {
    if (strcmp(meta->variants[idx].name, name) == 0) {
      return &meta->variants[idx];
    }
  }
  return NULL;
}

clux_type_t *clux_enum_get_underlying(clux_type_t *type) {
  clux_enum_meta_t *meta = type ? enum_meta(type) : NULL;
  return meta ? meta->underlying : NULL;
}

/* 结构相等核心：底层类型判等 + 成员名与成员值按声明序一一对应即相等，
   类型名不参与判等（与 union 一致：结构兼容的不同具名类型可以比较）。
   底层为 str 时值按字符串内容比较，否则按位型比较。 */
static bool variants_equal(clux_enum_meta_t *self,
                           clux_type_t *underlying,
                           const clux_enum_variant_t *variants,
                           size_t count) {
  if (!self || !clux_type_equal(self->underlying, underlying) ||
      self->count != count) {
    return false;
  }
  bool str_underlying =
      underlying && clux_type_kind_unwrap(underlying) == CLUX_TYPE_STR;
  for (size_t idx = 0; idx < count; idx++) {
    if (strcmp(self->variants[idx].name, variants[idx].name) != 0) {
      return false;
    }
    if (str_underlying) {
      if (!self->variants[idx].str || !variants[idx].str ||
          strcmp(self->variants[idx].str, variants[idx].str) != 0) {
        return false;
      }
    } else if (self->variants[idx].value != variants[idx].value) {
      return false;
    }
  }
  return true;
}

bool clux_enum_variants_equal(clux_type_t *self,
                              clux_type_t *underlying,
                              const clux_enum_variant_t *variants,
                              size_t count) {
  if (!enum_kind(clux_type_get_kind(self))) {
    return false;
  }
  return variants_equal(enum_meta(self), underlying, variants, count);
}

static bool enum_type_equal(clux_type_t *self, clux_type_t *other) {
  if (!enum_kind(clux_type_get_kind(other))) {
    return false;
  }
  clux_enum_meta_t *other_meta = enum_meta(other);
  return variants_equal(enum_meta(self),
                        other_meta ? other_meta->underlying : NULL,
                        other_meta ? other_meta->variants : NULL,
                        other_meta ? other_meta->count : 0);
}

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

/* 显式/安全 cast 合法性由底层类型决定（underlying 决定规则）：以底层类型
   的视图值委托底层 vtable 的 cast 通道执行，成功后按目标类型落地。
   目标是枚举时委托到目标枚举的底层并按目标枚举的布局重装位型；
   底层没有 cast 通道（如 str）时报与 context 默认一致的消息。 */
static clux_value_t *enum_cast_impl(clux_context_t *ctx,
                                    clux_value_t *value,
                                    clux_type_t *target,
                                    bool safe) {
  clux_type_t *source = clux_value_get_type(value);
  if (clux_type_get_kind(source) == CLUX_TYPE_ERROR) {
    return value;
  }
  if (source == target) {
    return value;
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, target);
  }
  clux_type_t *underlying = clux_enum_get_underlying(source);
  clux_type_t *target_underlying = enum_kind(clux_type_kind_unwrap(target))
                                       ? clux_enum_get_underlying(target)
                                       : NULL;
  clux_type_t *effective = target_underlying ? target_underlying : target;
  clux_vtable_t *vt = underlying ? clux_type_get_vtable(underlying) : NULL;
  bool has_slot = vt && (safe ? vt->safe_cast != NULL : vt->cast != NULL);
  if (!has_slot) {
    const char *source_name = clux_type_get_name(source);
    const char *target_name = clux_type_get_name(target);
    return clux_context_create_error_message(
        ctx,
        NULL,
        safe ? "cannot safely cast '%s' to '%s'" : "cannot cast '%s' to '%s'",
        source_name ? source_name : "<unknown>",
        target_name ? target_name : "<unknown>");
  }
  /* 底层视图：复制枚举值的位型为底层类型的值，交底层 cast 规则裁决。 */
  safe_allocator_t *sa = clux_context_get_safe_allocator(ctx);
  size_t size = clux_type_get_size(source);
  ptr_handle_t data = safe_alloc(sa, size, NULL);
  (void)memcpy(ptr_to_raw(data, 0), clux_value_get_raw(value, 0), size);
  clux_value_t *view =
      create_clux_value(clux_context_get_allocator(ctx), underlying, data);
  clux_value_t *result = safe ? vt->safe_cast(ctx, view, effective)
                              : vt->cast(ctx, view, effective);
  clux_value_t *out = result;
  if (result == view) {
    /* 恒等委托（底层→同底层）：视图未落地作用域，复制位型重新注册。 */
    ptr_handle_t copy = safe_alloc(sa, size, NULL);
    (void)memcpy(ptr_to_raw(copy, 0), ptr_to_raw(data, 0), size);
    out = clux_context_create_value(ctx, effective, copy, NULL, NULL);
  }
  delete_clux_value(&view);
  if (is_error_value(out) || !target_underlying) {
    return out;
  }
  /* 目标是枚举：把底层 cast 结果的位型按目标枚举布局落地。 */
  size_t target_size = clux_type_get_size(target);
  ptr_handle_t bits = safe_alloc(sa, target_size, NULL);
  (void)memcpy(ptr_to_raw(bits, 0), clux_value_get_raw(out, 0), target_size);
  return clux_context_create_value(ctx, target, bits, NULL, NULL);
}

static clux_value_t *
enum_safe_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
  return enum_cast_impl(ctx, value, target, true);
}

static clux_value_t *
enum_cast(clux_context_t *ctx, clux_value_t *value, clux_type_t *target) {
  return enum_cast_impl(ctx, value, target, false);
}

static clux_vtable_t g_enum_vtable = {
    .safe_cast = enum_safe_cast,
    .cast = enum_cast,
    .type_equal = enum_type_equal,
};

clux_vtable_t *get_clux_enum_vtable(void) { return &g_enum_vtable; }

clux_type_t *create_clux_enum_type(allocator_t *allocator,
                                   const char *name,
                                   clux_type_t *underlying,
                                   const clux_enum_variant_t *variants,
                                   size_t count) {
  /* 布局与底层整数类型一致：值表示即底层整数的原始位型。 */
  size_t size = clux_type_get_size(underlying);
  size_t align = clux_type_get_align(underlying);
  clux_type_t *type = create_clux_type(
      allocator, CLUX_TYPE_ENUM, name, size, align, get_clux_enum_vtable());
  /* meta 与成员表连体成单块分配：delete_clux_type 释放 meta 本体即整块释放 */
  clux_enum_meta_t *meta = clux_type_create_meta(
      type, sizeof(clux_enum_meta_t) + sizeof(clux_enum_variant_t) * count);
  meta->underlying = underlying;
  meta->variants =
      (clux_enum_variant_t *)((char *)meta + sizeof(clux_enum_meta_t));
  (void)memcpy(meta->variants, variants, sizeof(clux_enum_variant_t) * count);
  meta->count = count;
  return type;
}
