#include "engine/struct.h"
#include "core/allocator.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* meta 连体单块布局：meta 本体 | 字段表 | attribute 表 | method 名单 |
   subtype 表，delete_clux_type 释放 meta 本体即整块释放。 */
typedef struct _clux_struct_meta_t {
  clux_struct_field_t *fields;
  size_t count;
  clux_struct_attr_t *attrs;
  size_t attr_count;
  const char **methods;
  size_t method_count;
  clux_struct_subtype_t *subtypes;
  size_t subtype_count;
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
   vtable 的 eq 槽位，padding 不参与比较）。attribute/method/subtype 是
   类型命名空间成员，不属于实例数据，不参与实例比较。 */
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
  clux_type_t *bool_type = clux_context_builtin_type(ctx, CLUX_TYPE_BOOL);
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

size_t clux_struct_attr_count(clux_type_t *type) {
  clux_struct_meta_t *meta = type ? struct_meta(type) : NULL;
  return meta ? meta->attr_count : 0;
}

const clux_struct_attr_t *clux_struct_get_attr(clux_type_t *type, size_t idx) {
  clux_struct_meta_t *meta = type ? struct_meta(type) : NULL;
  if (!meta || idx >= meta->attr_count) {
    return NULL;
  }
  return &meta->attrs[idx];
}

const clux_struct_attr_t *clux_struct_find_attr(clux_type_t *type,
                                                const char *name) {
  clux_struct_meta_t *meta = type ? struct_meta(type) : NULL;
  if (!meta || !name) {
    return NULL;
  }
  for (size_t idx = 0; idx < meta->attr_count; idx++) {
    if (strcmp(meta->attrs[idx].name, name) == 0) {
      return &meta->attrs[idx];
    }
  }
  return NULL;
}

bool clux_struct_is_method(clux_type_t *type, const char *name) {
  clux_struct_meta_t *meta = type ? struct_meta(type) : NULL;
  if (!meta || !name) {
    return false;
  }
  for (size_t idx = 0; idx < meta->method_count; idx++) {
    if (strcmp(meta->methods[idx], name) == 0) {
      return true;
    }
  }
  return false;
}

size_t clux_struct_subtype_count(clux_type_t *type) {
  clux_struct_meta_t *meta = type ? struct_meta(type) : NULL;
  return meta ? meta->subtype_count : 0;
}

const clux_struct_subtype_t *clux_struct_get_subtype(clux_type_t *type,
                                                     size_t idx) {
  clux_struct_meta_t *meta = type ? struct_meta(type) : NULL;
  if (!meta || idx >= meta->subtype_count) {
    return NULL;
  }
  return &meta->subtypes[idx];
}

const clux_struct_subtype_t *clux_struct_find_subtype(clux_type_t *type,
                                                      const char *name) {
  clux_struct_meta_t *meta = type ? struct_meta(type) : NULL;
  if (!meta || !name) {
    return NULL;
  }
  for (size_t idx = 0; idx < meta->subtype_count; idx++) {
    if (strcmp(meta->subtypes[idx].name, name) == 0) {
      return &meta->subtypes[idx];
    }
  }
  return NULL;
}

/* 结构判等子例程：四张表按声明序逐项比较。attribute 参与判等的身份是
   （名, 值类型）——attr 值本体是运行期对象，值恒等不可行（同名同类型的
   声明即视为同一结构成员）。可选表 count 为 0 时只看 count（meta 内
   段指针恒指向连体块内位置，不以 NULL 判缺席）。 */
static bool fields_segment_equal(const clux_struct_field_t *self,
                                 const clux_struct_field_t *fields,
                                 size_t count) {
  if (!self || !fields || count == 0) {
    return false;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (strcmp(self[idx].name, fields[idx].name) != 0 ||
        !clux_type_equal(self[idx].type, fields[idx].type)) {
      return false;
    }
  }
  return true;
}

static bool attrs_segment_equal(const clux_struct_attr_t *self,
                                const clux_struct_attr_t *attrs,
                                size_t count) {
  if (count == 0) {
    return true;
  }
  if (!self || !attrs) {
    return false;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (strcmp(self[idx].name, attrs[idx].name) != 0 ||
        !clux_type_equal(clux_value_get_type(self[idx].value),
                         clux_value_get_type(attrs[idx].value))) {
      return false;
    }
  }
  return true;
}

static bool methods_segment_equal(const char **self,
                                  const char *const *methods,
                                  size_t count) {
  if (count == 0) {
    return true;
  }
  if (!self || !methods) {
    return false;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (strcmp(self[idx], methods[idx]) != 0) {
      return false;
    }
  }
  return true;
}

static bool subtypes_segment_equal(const clux_struct_subtype_t *self,
                                   const clux_struct_subtype_t *subtypes,
                                   size_t count) {
  if (count == 0) {
    return true;
  }
  if (!self || !subtypes) {
    return false;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (strcmp(self[idx].name, subtypes[idx].name) != 0 ||
        !clux_type_equal(self[idx].type, subtypes[idx].type)) {
      return false;
    }
  }
  return true;
}

static bool decls_segment_equal(const clux_struct_meta_t *self,
                                const clux_struct_decl_t *decl) {
  if (self->count != decl->field_count ||
      self->attr_count != decl->attr_count ||
      self->method_count != decl->method_count ||
      self->subtype_count != decl->subtype_count ||
      !fields_segment_equal(self->fields, decl->fields, decl->field_count)) {
    return false;
  }
  return attrs_segment_equal(self->attrs, decl->attrs, decl->attr_count) &&
         methods_segment_equal(
             self->methods, decl->methods, decl->method_count) &&
         subtypes_segment_equal(
             self->subtypes, decl->subtypes, decl->subtype_count);
}

bool clux_struct_decls_equal(clux_type_t *self,
                             const clux_struct_decl_t *decl) {
  if (!decl || !struct_kind(clux_type_get_kind(self))) {
    return false;
  }
  return decls_segment_equal(struct_meta(self), decl);
}

/* 类型相等核心：类型名不参与判等（白皮书：结构兼容的不同具名类型可以
   比较），四张声明表按声明序一一对应即相等。 */
static bool struct_type_equal(clux_type_t *self, clux_type_t *other) {
  if (!struct_kind(clux_type_get_kind(other))) {
    return false;
  }
  clux_struct_meta_t *self_meta = struct_meta(self);
  clux_struct_meta_t *other_meta = struct_meta(other);
  clux_struct_decl_t other_decl = {
      other_meta->fields,
      other_meta->count,
      other_meta->attrs,
      other_meta->attr_count,
      /* C 不允许 const char** 隐式转 const char*const*，显式转型消除 */
      (const char *const *)other_meta->methods,
      other_meta->method_count,
      other_meta->subtypes,
      other_meta->subtype_count,
  };
  return decls_segment_equal(self_meta, &other_decl);
}

/* 向下兼容（struct 指针隐式转换的白皮书依据）：base 的字段表是 self
   字段表的前缀（按声明序名+类型一致）即 self extends base——额外字段
   与 attribute/method/subtype 均不参与（只看字段兼容）。指针协变递归
   由此接通：*Derived extends *Base。 */
static bool struct_type_extends(clux_type_t *self, clux_type_t *base) {
  if (!struct_kind(clux_type_get_kind(base))) {
    return false;
  }
  clux_struct_meta_t *self_meta = struct_meta(self);
  clux_struct_meta_t *base_meta = struct_meta(base);
  if (!self_meta || !base_meta || base_meta->count > self_meta->count) {
    return false;
  }
  for (size_t idx = 0; idx < base_meta->count; idx++) {
    if (strcmp(self_meta->fields[idx].name, base_meta->fields[idx].name) != 0 ||
        !clux_type_equal(self_meta->fields[idx].type,
                         base_meta->fields[idx].type)) {
      return false;
    }
  }
  return true;
}

/* 鸭子类型互赋（值的隐式转换点）：两边字段表逐项一致（名+类型按声明序，
   即内存布局一致）即互相可赋值——类型名与 attribute/method/subtype 均
   不参与（不看声明身份，只看结构）。字段表必须完全一致而非前缀：值语义
   memcpy 按 target 平铺，target 多出的字段会越界读 source。 */
static clux_value_t *struct_safe_cast(clux_context_t *ctx,
                                      clux_value_t *value,
                                      clux_type_t *target) {
  clux_type_t *source = clux_value_get_type(value);
  if (clux_type_get_kind(source) == CLUX_TYPE_ERROR) {
    return value;
  }
  const char *source_name = clux_type_get_name(source);
  const char *target_name = clux_type_get_name(target);
  clux_struct_meta_t *source_meta = struct_meta(source);
  clux_struct_meta_t *target_meta =
      struct_kind(clux_type_kind_unwrap(target)) ? struct_meta(target) : NULL;
  bool duck =
      source_meta && target_meta && source_meta->count == target_meta->count;
  if (duck) {
    for (size_t idx = 0; idx < target_meta->count; idx++) {
      if (strcmp(source_meta->fields[idx].name,
                 target_meta->fields[idx].name) != 0 ||
          !clux_type_equal(source_meta->fields[idx].type,
                           target_meta->fields[idx].type)) {
        duck = false;
        break;
      }
    }
  }
  if (!duck) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "cannot safely cast '%s' to '%s'",
        source_name ? source_name : "<unknown>",
        target_name ? target_name : "<unknown>");
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, target);
  }
  /* 布局一致：共享源 data 建 target 类型的 view，assign 随后按 target
     size memcpy 穿透写入左值。 */
  return clux_context_create_value(
      ctx, target, clux_value_get_data(value), NULL, NULL);
}

static clux_vtable_t g_struct_vtable = {
    .eq = struct_eq,
    .ne = struct_ne,
    .field = struct_field,
    .safe_cast = struct_safe_cast,
    .type_equal = struct_type_equal,
    .type_extends = struct_type_extends,
};

clux_vtable_t *get_clux_struct_vtable(void) { return &g_struct_vtable; }

clux_type_t *create_clux_struct_type(allocator_t *allocator,
                                     const char *name,
                                     const clux_struct_decl_t *decl) {
  /* 字段按声明序顺序平铺（各自按自身对齐取偏移）：
     align = max(字段 align)，size = round_up(平铺末尾, align)。
     attribute/method/subtype 是类型命名空间成员，不参与 Data 布局。 */
  size_t align = 1;
  for (size_t idx = 0; idx < decl->field_count; idx++) {
    size_t field_align = clux_type_get_align(decl->fields[idx].type);
    if (field_align > align) {
      align = field_align;
    }
  }
  size_t end = 0;
  for (size_t idx = 0; idx < decl->field_count; idx++) {
    size_t field_align = clux_type_get_align(decl->fields[idx].type);
    size_t field_size = clux_type_get_size(decl->fields[idx].type);
    end = (end + field_align - 1) / field_align * field_align + field_size;
  }
  size_t size = (end + align - 1) / align * align;
  clux_type_t *type = create_clux_type(
      allocator, CLUX_TYPE_STRUCT, name, size, align, get_clux_struct_vtable());
  /* meta 与四张表连体成单块分配：delete_clux_type 释放 meta 本体即整块释放 */
  size_t bytes = sizeof(clux_struct_meta_t) +
                 sizeof(clux_struct_field_t) * decl->field_count +
                 sizeof(clux_struct_attr_t) * decl->attr_count +
                 sizeof(const char *) * decl->method_count +
                 sizeof(clux_struct_subtype_t) * decl->subtype_count;
  clux_struct_meta_t *meta = clux_type_create_meta(type, bytes);
  char *cursor = (char *)meta + sizeof(clux_struct_meta_t);
  meta->fields = (clux_struct_field_t *)cursor;
  cursor += sizeof(clux_struct_field_t) * decl->field_count;
  (void)memcpy(meta->fields,
               decl->fields,
               sizeof(clux_struct_field_t) * decl->field_count);
  meta->count = decl->field_count;
  meta->attrs = (clux_struct_attr_t *)cursor;
  cursor += sizeof(clux_struct_attr_t) * decl->attr_count;
  if (decl->attr_count > 0) {
    (void)memcpy(meta->attrs,
                 decl->attrs,
                 sizeof(clux_struct_attr_t) * decl->attr_count);
  }
  meta->attr_count = decl->attr_count;
  meta->methods = (const char **)cursor;
  cursor += sizeof(const char *) * decl->method_count;
  if (decl->method_count > 0) {
    (void)memcpy(meta->methods,
                 decl->methods,
                 sizeof(const char *) * decl->method_count);
  }
  meta->method_count = decl->method_count;
  meta->subtypes = (clux_struct_subtype_t *)cursor;
  if (decl->subtype_count > 0) {
    (void)memcpy(meta->subtypes,
                 decl->subtypes,
                 sizeof(clux_struct_subtype_t) * decl->subtype_count);
  }
  meta->subtype_count = decl->subtype_count;
  clux_type_seal(type); /* 布局即时定局：构造完成即完整 */
  return type;
}
