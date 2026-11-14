#include "engine/struct.h"
#include "core/allocator.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct _clux_struct_meta_t {
  clux_struct_field_t *fields;
  size_t count;
} clux_struct_meta_t;

static clux_struct_meta_t *struct_meta(clux_type_t *type) {
  return clux_type_get_meta(type);
}

static bool struct_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_STRUCT:
    return true;
  default:
    return false;
  }
}

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

size_t clux_struct_field_count(clux_type_t *type) {
  clux_struct_meta_t *meta = type ? struct_meta(type) : NULL;
  return meta ? meta->count : 0;
}

const clux_struct_field_t *clux_struct_get_field(clux_type_t *type,
                                                 size_t idx) {
  clux_struct_meta_t *meta = type ? struct_meta(type) : NULL;
  if (!meta || idx >= meta->count) {
    return NULL;
  }
  return &meta->fields[idx];
}

const clux_struct_field_t *clux_struct_find_field(clux_type_t *type,
                                                  const char *name) {
  clux_struct_meta_t *meta = type ? struct_meta(type) : NULL;
  if (!meta || !name) {
    return NULL;
  }
  for (size_t idx = 0; idx < meta->count; idx++) {
    if (strcmp(meta->fields[idx].name, name) == 0) {
      return &meta->fields[idx];
    }
  }
  return NULL;
}

size_t clux_struct_field_offset(clux_type_t *type, size_t idx) {
  clux_struct_meta_t *meta = struct_meta(type);
  size_t offset = 0;
  for (size_t pos = 0; pos < idx && pos < meta->count; pos++) {
    size_t field_align = clux_type_get_align(meta->fields[pos].type);
    size_t field_size = clux_type_get_size(meta->fields[pos].type);
    offset =
        (offset + field_align - 1) / field_align * field_align + field_size;
  }
  size_t field_align = clux_type_get_align(meta->fields[idx].type);
  return (offset + field_align - 1) / field_align * field_align;
}

/* 字段读取（x.field）：返回字段类型的读写双向 view（与 Data 内存共享，
   全部字段同时有效，无 tag 检查）。写入由通用 assign 写穿 view 完成。 */
static clux_value_t *
struct_field(clux_context_t *ctx, clux_value_t *value, const char *name) {
  clux_type_t *type = clux_value_get_type(value);
  const clux_struct_field_t *field = clux_struct_find_field(type, name);
  if (!field) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(ctx,
                                             NULL,
                                             "struct '%s' has no field '%s'",
                                             type_name ? type_name
                                                       : "<unknown>",
                                             name);
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, field->type);
  }
  size_t idx = (size_t)(field - struct_meta(type)->fields);
  ptr_handle_t view = ptr_offset(clux_value_get_data(value),
                                 clux_struct_field_offset(type, idx));
  if (!view) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot access field '%s' of an invalid struct", name);
  }
  return clux_context_create_value(ctx, field->type, view, NULL, NULL);
}

/* 同类型比较（白皮书 §5.5）：按声明序逐字段递归比较（复用字段类型
   vtable 的 eq 槽位，padding 不参与比较）。 */
static clux_value_t *struct_compare(clux_context_t *ctx,
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
  if (!struct_kind(clux_type_get_kind(clux_value_get_type(right))) ||
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
  clux_struct_meta_t *meta = struct_meta(type);
  bool equal = true;
  for (size_t idx = 0; idx < meta->count; idx++) {
    clux_vtable_t *field_vt = clux_type_get_vtable(meta->fields[idx].type);
    if (!field_vt || !field_vt->eq) {
      return clux_context_create_error_message(
          ctx,
          NULL,
          "struct field '%s' of type '%s' does not support operator '=='",
          meta->fields[idx].name,
          clux_type_get_name(meta->fields[idx].type)
              ? clux_type_get_name(meta->fields[idx].type)
              : "<unknown>");
    }
    ptr_handle_t left_view = ptr_offset(clux_value_get_data(left),
                                        clux_struct_field_offset(type, idx));
    ptr_handle_t right_view = ptr_offset(clux_value_get_data(right),
                                         clux_struct_field_offset(type, idx));
    if (!left_view || !right_view) {
      return clux_context_create_error_message(
          ctx, NULL, "cannot compare invalid structs");
    }
    clux_value_t *left_field = clux_context_create_value(
        ctx, meta->fields[idx].type, left_view, NULL, NULL);
    clux_value_t *right_field = clux_context_create_value(
        ctx, meta->fields[idx].type, right_view, NULL, NULL);
    clux_value_t *result = field_vt->eq(ctx, left_field, right_field);
    if (is_error_value(result)) {
      return result;
    }
    void *raw = ptr_to_raw(clux_value_get_data(result), 0);
    if (!raw || !*(bool *)raw) {
      equal = false;
      break;
    }
  }
  return clux_context_create_bool(
      ctx, op == CLUX_OP_EQ ? equal : !equal, NULL, NULL);
}

static clux_value_t *
struct_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return struct_compare(ctx, left, right, CLUX_OP_EQ);
}

static clux_value_t *
struct_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return struct_compare(ctx, left, right, CLUX_OP_NE);
}

/* 结构相等核心：字段数、字段名与字段类型按声明序一一对应即相等，
   类型名不参与判等（白皮书：结构兼容的不同具名类型可以比较）。 */
static bool fields_equal(clux_struct_meta_t *self,
                         const clux_struct_field_t *fields,
                         size_t count) {
  if (!self || self->count != count) {
    return false;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (strcmp(self->fields[idx].name, fields[idx].name) != 0 ||
        !clux_type_equal(self->fields[idx].type, fields[idx].type)) {
      return false;
    }
  }
  return true;
}

bool clux_struct_fields_equal(clux_type_t *self,
                              const clux_struct_field_t *fields,
                              size_t count) {
  if (!struct_kind(clux_type_get_kind(self))) {
    return false;
  }
  return fields_equal(struct_meta(self), fields, count);
}

static bool struct_type_equal(clux_type_t *self, clux_type_t *other) {
  if (!struct_kind(clux_type_get_kind(other))) {
    return false;
  }
  clux_struct_meta_t *other_meta = struct_meta(other);
  return fields_equal(struct_meta(self),
                      other_meta ? other_meta->fields : NULL,
                      other_meta ? other_meta->count : 0);
}

static clux_vtable_t g_struct_vtable = {
    .eq = struct_eq,
    .ne = struct_ne,
    .field = struct_field,
    .type_equal = struct_type_equal,
};

clux_vtable_t *get_clux_struct_vtable(void) { return &g_struct_vtable; }

clux_type_t *create_clux_struct_type(allocator_t *allocator,
                                     const char *name,
                                     const clux_struct_field_t *fields,
                                     size_t count) {
  /* 字段按声明序顺序平铺（各自按自身对齐取偏移）：
     align = max(字段 align)，size = round_up(平铺末尾, align)。 */
  size_t align = 1;
  for (size_t idx = 0; idx < count; idx++) {
    size_t field_align = clux_type_get_align(fields[idx].type);
    if (field_align > align) {
      align = field_align;
    }
  }
  size_t end = 0;
  for (size_t idx = 0; idx < count; idx++) {
    size_t field_align = clux_type_get_align(fields[idx].type);
    size_t field_size = clux_type_get_size(fields[idx].type);
    end = (end + field_align - 1) / field_align * field_align + field_size;
  }
  size_t size = (end + align - 1) / align * align;
  clux_type_t *type = create_clux_type(
      allocator, CLUX_TYPE_STRUCT, name, size, align, get_clux_struct_vtable());
  /* meta 与字段表连体成单块分配：delete_clux_type 释放 meta 本体即整块释放 */
  clux_struct_meta_t *meta = clux_type_create_meta(
      type, sizeof(clux_struct_meta_t) + sizeof(clux_struct_field_t) * count);
  meta->fields =
      (clux_struct_field_t *)((char *)meta + sizeof(clux_struct_meta_t));
  (void)memcpy(meta->fields, fields, sizeof(clux_struct_field_t) * count);
  meta->count = count;
  return type;
}
