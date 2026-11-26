#include "engine/slice.h"
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

typedef struct _clux_slice_meta_t {
  clux_type_t *base;
} clux_slice_meta_t;

static clux_slice_meta_t *slice_meta(clux_type_t *type) {
  return clux_type_get_meta(type);
}

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

static bool slice_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_SLICE:
    return true;
  default:
    return false;
  }
}

static bool
slice_read(clux_value_t *value, ptr_handle_t *out_ptr, size_t *out_len) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  if (!raw) {
    return false;
  }
  clux_slice_data_t *data = raw;
  *out_ptr = data->ptr;
  *out_len = data->len;
  return true;
}

/* 下标取项：按存储的 len 做边界检查，返回 base 类型的 view（与指向内存
   共享），写入由通用 assign 写穿 view 完成。 */
static clux_value_t *
slice_index(clux_context_t *ctx, clux_value_t *slice, clux_value_t *index) {
  clux_type_t *base = slice_meta(clux_value_get_type(slice))->base;
  if (is_error_value(index)) {
    return index;
  }
  if (clux_value_is_shadow(slice) || clux_value_is_shadow(index)) {
    return clux_context_create_shadow_value(ctx, base);
  }
  ptr_handle_t target = 0;
  size_t len = 0;
  if (!slice_read(slice, &target, &len) || !ptr_is_valid(target)) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot index an invalid slice");
  }
  int64_t idx = 0;
  clux_value_t *idx_err = clux_context_index_to_i64(ctx, index, &idx);
  if (idx_err) {
    return idx_err;
  }
  if (idx < 0 || (uint64_t)idx >= len) {
    return clux_context_create_error_message(ctx,
                                             NULL,
                                             "index %lld is out of bounds "
                                             "for slice of length %zu",
                                             (long long)idx,
                                             len);
  }
  size_t elem_size = clux_type_get_size(base);
  if (ptr_get_size(target) < ((size_t)idx + 1) * elem_size) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot index an invalid slice");
  }
  ptr_handle_t view = ptr_offset(target, (size_t)idx * elem_size);
  if (!view) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot index an invalid slice");
  }
  return clux_context_create_value(ctx, base, view, NULL, NULL);
}

/* 再切片 a[lo:hi]：lo/hi 为 NULL 表示开放端点（缺省 0 / 存储长度）。
   以存储的 {ptr, len} 为界偏移出新的胖指针（不拥有内存）。
   负数、越界、lo > hi 报 error；shadow 操作数产出切片类型的 shadow。 */
static clux_value_t *slice_slice(clux_context_t *ctx,
                                 clux_value_t *slice,
                                 clux_value_t *lo,
                                 clux_value_t *hi) {
  clux_type_t *base = slice_meta(clux_value_get_type(slice))->base;
  clux_type_t *slice_type = clux_context_get_slice_type(ctx, base);
  if (clux_value_is_shadow(slice) || (lo && clux_value_is_shadow(lo)) ||
      (hi && clux_value_is_shadow(hi))) {
    return clux_context_create_shadow_value(ctx, slice_type);
  }
  ptr_handle_t target = 0;
  size_t len = 0;
  if (!slice_read(slice, &target, &len) || !ptr_is_valid(target)) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot slice an invalid slice");
  }
  int64_t start = 0;
  int64_t end = (int64_t)len;
  if (lo) {
    clux_value_t *err = clux_context_index_to_i64(ctx, lo, &start);
    if (err) {
      return err;
    }
    if (start < 0) {
      return clux_context_create_error_message(
          ctx, NULL, "slice index must not be negative");
    }
    if ((uint64_t)start > len) {
      return clux_context_create_error_message(ctx,
                                               NULL,
                                               "slice index %lld is out of "
                                               "bounds for slice of length %zu",
                                               (long long)start,
                                               len);
    }
  }
  if (hi) {
    clux_value_t *err = clux_context_index_to_i64(ctx, hi, &end);
    if (err) {
      return err;
    }
    if (end < 0) {
      return clux_context_create_error_message(
          ctx, NULL, "slice index must not be negative");
    }
    if ((uint64_t)end > len) {
      return clux_context_create_error_message(ctx,
                                               NULL,
                                               "slice index %lld is out of "
                                               "bounds for slice of length %zu",
                                               (long long)end,
                                               len);
    }
  }
  if (start > end) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "slice start %lld exceeds end %lld",
        (long long)start,
        (long long)end);
  }
  size_t elem_size = clux_type_get_size(base);
  size_t new_len = (size_t)(end - start);
  if (new_len > 0 && ptr_get_size(target) < end * elem_size) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot slice an invalid slice");
  }
  ptr_handle_t new_ptr = target;
  if (start > 0 && (size_t)start < len) {
    new_ptr = ptr_offset(target, (size_t)start * elem_size);
    if (!new_ptr) {
      return clux_context_create_error_message(
          ctx, NULL, "cannot slice an invalid slice");
    }
  }
  return clux_context_create_slice(
      ctx, slice_type, new_ptr, new_len, NULL, NULL);
}

static clux_value_t *slice_compare(clux_context_t *ctx,
                                   clux_value_t *left,
                                   clux_value_t *right,
                                   clux_binary_op_t op) {
  if (is_error_value(left)) {
    return left;
  }
  if (is_error_value(right)) {
    return right;
  }
  if (!slice_kind(clux_type_get_kind(clux_value_get_type(right)))) {
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
  ptr_handle_t left_ptr = 0;
  ptr_handle_t right_ptr = 0;
  size_t left_len = 0;
  size_t right_len = 0;
  if (!slice_read(left, &left_ptr, &left_len) ||
      !slice_read(right, &right_ptr, &right_len)) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot compare invalid slices");
  }
  bool equal = left_ptr == right_ptr && left_len == right_len;
  return clux_context_create_bool(
      ctx, op == CLUX_OP_EQ ? equal : !equal, NULL, NULL);
}

static clux_value_t *
slice_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return slice_compare(ctx, left, right, CLUX_OP_EQ);
}

static clux_value_t *
slice_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return slice_compare(ctx, left, right, CLUX_OP_NE);
}

static bool slice_type_equal(clux_type_t *self, clux_type_t *other) {
  if (clux_type_get_kind(other) != CLUX_TYPE_SLICE) {
    return false;
  }
  clux_slice_meta_t *self_meta = clux_type_get_meta(self);
  clux_slice_meta_t *other_meta = clux_type_get_meta(other);
  if (!self_meta || !other_meta) {
    return false;
  }
  return clux_type_equal(self_meta->base, other_meta->base);
}

/* 切片协变：[]T extends []U 当且仅当 T extends U。 */
static bool slice_type_extends(clux_type_t *self, clux_type_t *base) {
  if (clux_type_get_kind(base) != CLUX_TYPE_SLICE) {
    return false;
  }
  clux_slice_meta_t *self_meta = clux_type_get_meta(self);
  clux_slice_meta_t *base_meta = clux_type_get_meta(base);
  if (!self_meta || !base_meta) {
    return false;
  }
  return clux_type_extends(self_meta->base, base_meta->base);
}

static clux_vtable_t g_slice_vtable = {
    .eq = slice_eq,
    .ne = slice_ne,
    .index = slice_index,
    .slice = slice_slice,
    .type_equal = slice_type_equal,
    .type_extends = slice_type_extends,
};

clux_vtable_t *get_clux_slice_vtable(void) { return &g_slice_vtable; }

/* 桶去重判据：base 语义相等即视为同一 slice 类型（跨实例、递归复合均命中），
   由 clux_type_equal 回答。 */
static bool slice_child_base_matches(clux_type_t *child, void *user_data) {
  clux_slice_meta_t *meta = clux_type_get_meta(child);
  return meta && clux_type_equal(meta->base, (clux_type_t *)user_data);
}

clux_type_t *clux_slice_get_base(clux_type_t *type) {
  if (!type || clux_type_get_kind(type) != CLUX_TYPE_SLICE) {
    return NULL;
  }
  clux_slice_meta_t *meta = slice_meta(type);
  return meta ? meta->base : NULL;
}

clux_type_t *clux_type_get_slice_type(clux_type_t *base) {
  clux_type_t *existing = clux_type_find_child(
      base, CLUX_TYPE_SLICE, slice_child_base_matches, base);
  if (existing) {
    return existing;
  }
  const char *base_name = clux_type_get_name(base);
  base_name = base_name == NULL ? "<unknown>" : base_name;
  size_t len = strlen(base_name) + 3;
  char name[len];
  (void)snprintf(name, len, "[]%s", base_name);
  allocator_t *allocator = clux_type_get_allocator(base);
  clux_type_t *type = create_clux_type(allocator,
                                       CLUX_TYPE_SLICE,
                                       name,
                                       sizeof(clux_slice_data_t),
                                       alignof(clux_slice_data_t),
                                       get_clux_slice_vtable());
  clux_slice_meta_t *meta =
      clux_type_create_meta(type, sizeof(clux_slice_meta_t));
  meta->base = base;
  clux_type_add_child(base, CLUX_TYPE_SLICE, type);
  clux_type_seal(type); /* 布局即时定局：构造完成即完整 */
  return type;
}
