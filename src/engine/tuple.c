#include "engine/tuple.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct _clux_tuple_meta_t {
  size_t count;
  clux_type_t **elems; /* meta 连体块：紧随 meta 本体 */
} clux_tuple_meta_t;

static clux_tuple_meta_t *tuple_meta(clux_type_t *type) {
  return clux_type_get_meta(type);
}

static bool tuple_kind(clux_type_kind_t kind) {
  return kind == CLUX_TYPE_TUPLE;
}

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

/* 按位置取元素：返回元素类型的 view（与元组内联块内存共享），越界报
   error，写入由通用 assign 写穿 view 完成。 */
static clux_value_t *
tuple_index(clux_context_t *ctx, clux_value_t *tuple, clux_value_t *index) {
  if (is_error_value(index)) {
    return index;
  }
  clux_tuple_meta_t *meta = tuple_meta(clux_value_get_type(tuple));
  int64_t idx = 0;
  clux_value_t *idx_err = clux_context_index_to_i64(ctx, index, &idx);
  if (idx_err) {
    return idx_err;
  }
  if (idx < 0 || !meta || (uint64_t)idx >= meta->count) {
    return clux_context_create_error_message(ctx,
                                             NULL,
                                             "index %lld is out of bounds "
                                             "for tuple of length %zu",
                                             (long long)idx,
                                             meta ? meta->count : 0);
  }
  clux_type_t *elem = meta->elems[idx];
  if (clux_value_is_shadow(tuple) || clux_value_is_shadow(index)) {
    return clux_context_create_shadow_value(ctx, elem);
  }
  ptr_handle_t view = ptr_offset(
      clux_value_get_data(tuple),
      clux_tuple_elem_offset(clux_value_get_type(tuple), (size_t)idx));
  if (!view) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot index an invalid tuple");
  }
  return clux_context_create_value(ctx, elem, view, NULL, NULL);
}

/* 判等：元组按字节比较（白皮书 §5.5）；结构一致的异构平铺要求两侧
   元素类型序列相同（type_equal），shadow 产出 bool shadow。 */
static clux_value_t *tuple_compare(clux_context_t *ctx,
                                   clux_value_t *left,
                                   clux_value_t *right,
                                   clux_binary_op_t op) {
  if (is_error_value(left)) {
    return left;
  }
  if (is_error_value(right)) {
    return right;
  }
  if (!tuple_kind(clux_type_get_kind(clux_value_get_type(right))) ||
      !clux_type_equal(clux_value_get_type(left), clux_value_get_type(right))) {
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
  size_t size = clux_type_get_size(clux_value_get_type(left));
  void *left_raw = ptr_to_raw(clux_value_get_data(left), 0);
  void *right_raw = ptr_to_raw(clux_value_get_data(right), 0);
  if (!left_raw || !right_raw) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot compare invalid tuples");
  }
  bool equal = memcmp(left_raw, right_raw, size) == 0;
  return clux_context_create_bool(
      ctx, op == CLUX_OP_EQ ? equal : !equal, NULL, NULL);
}

static clux_value_t *
tuple_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return tuple_compare(ctx, left, right, CLUX_OP_EQ);
}

static clux_value_t *
tuple_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return tuple_compare(ctx, left, right, CLUX_OP_NE);
}

static bool tuple_type_equal(clux_type_t *self, clux_type_t *other) {
  if (!tuple_kind(clux_type_get_kind(other))) {
    return false;
  }
  clux_tuple_meta_t *self_meta = tuple_meta(self);
  clux_tuple_meta_t *other_meta = tuple_meta(other);
  if (!self_meta || !other_meta || self_meta->count != other_meta->count) {
    return false;
  }
  for (size_t idx = 0; idx < self_meta->count; idx++) {
    if (!clux_type_equal(self_meta->elems[idx], other_meta->elems[idx])) {
      return false;
    }
  }
  return true;
}

static clux_vtable_t g_tuple_vtable = {
    .eq = tuple_eq,
    .ne = tuple_ne,
    .index = tuple_index,
    .type_equal = tuple_type_equal,
};

clux_vtable_t *get_clux_tuple_vtable(void) { return &g_tuple_vtable; }

size_t clux_tuple_elem_count(clux_type_t *type) {
  clux_tuple_meta_t *meta = type ? tuple_meta(type) : NULL;
  return meta ? meta->count : 0;
}

clux_type_t *clux_tuple_get_elem(clux_type_t *type, size_t idx) {
  clux_tuple_meta_t *meta = type ? tuple_meta(type) : NULL;
  if (!meta || idx >= meta->count) {
    return NULL;
  }
  return meta->elems[idx];
}

size_t clux_tuple_elem_offset(clux_type_t *type, size_t idx) {
  clux_tuple_meta_t *meta = tuple_meta(type);
  size_t offset = 0;
  for (size_t pos = 0; meta && pos < idx && pos < meta->count; pos++) {
    size_t elem_align = clux_type_get_align(meta->elems[pos]);
    size_t elem_size = clux_type_get_size(meta->elems[pos]);
    offset = (offset + elem_align - 1) / elem_align * elem_align + elem_size;
  }
  if (!meta || idx >= meta->count) {
    return offset;
  }
  size_t elem_align = clux_type_get_align(meta->elems[idx]);
  return (offset + elem_align - 1) / elem_align * elem_align;
}

bool clux_tuple_signature_equal(clux_type_t *self,
                                clux_type_t **elems,
                                size_t count) {
  if (!tuple_kind(clux_type_get_kind(self))) {
    return false;
  }
  clux_tuple_meta_t *meta = tuple_meta(self);
  if (!meta || meta->count != count) {
    return false;
  }
  if (count == 0) {
    return true;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (!elems || !clux_type_equal(meta->elems[idx], elems[idx])) {
      return false;
    }
  }
  return true;
}

/* 布局：元素按声明序、各自按自身对齐平铺；align = max(元素对齐)，
   size = 平铺总大小向上取整到 align；空元组 size 0、align 1。 */
static void tuple_layout(clux_type_t **elems,
                         size_t count,
                         size_t *out_size,
                         size_t *out_align) {
  size_t size = 0;
  size_t align = 1;
  for (size_t idx = 0; idx < count; idx++) {
    size_t elem_align = clux_type_get_align(elems[idx]);
    size_t elem_size = clux_type_get_size(elems[idx]);
    size = (size + elem_align - 1) / elem_align * elem_align + elem_size;
    if (elem_align > align) {
      align = elem_align;
    }
  }
  *out_size = (size + align - 1) / align * align;
  *out_align = align;
}

clux_type_t *create_clux_tuple_type(allocator_t *allocator,
                                    const char *name,
                                    clux_type_t **elems,
                                    size_t count) {
  size_t size = 0;
  size_t align = 1;
  if (elems && count > 0) {
    tuple_layout(elems, count, &size, &align);
  }
  clux_type_t *type = create_clux_type(
      allocator, CLUX_TYPE_TUPLE, name, size, align, get_clux_tuple_vtable());
  /* meta 与元素类型表连体成单块分配：delete_clux_type 释放 meta
     本体即整块释放 */
  clux_tuple_meta_t *meta = clux_type_create_meta(
      type, sizeof(clux_tuple_meta_t) + sizeof(clux_type_t *) * count);
  meta->elems = (clux_type_t **)((char *)meta + sizeof(clux_tuple_meta_t));
  if (elems && count > 0) {
    (void)memcpy(meta->elems, elems, sizeof(clux_type_t *) * count);
  }
  meta->count = count;
  return type;
}
