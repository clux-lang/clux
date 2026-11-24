#include "engine/bitfield.h"
#include "core/allocator.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* meta 连体单块布局：meta 本体 | 字段表。字段表在构造时一次定局
   （布局即时 seal，一阶段构造——位段字段只是宽度常量，无类型引用，
   不需要 union 的两阶段递归壳）。字段名借用 context 驻留串，
   delete_meta 为 NULL（释放 meta 本体即整块释放）。 */
typedef struct _clux_bitfield_meta_t {
  clux_type_t *underlying;
  clux_bitfield_field_t *fields;
  size_t count;
  size_t total_bits;
} clux_bitfield_meta_t;

static clux_bitfield_meta_t *bitfield_meta(clux_type_t *type) {
  return clux_type_get_meta(type);
}

static bool bitfield_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_BITFIELD:
    return true;
  default:
    return false;
  }
}

size_t clux_bitfield_field_count(clux_type_t *type) {
  clux_bitfield_meta_t *meta = type ? bitfield_meta(type) : NULL;
  return meta ? meta->count : 0;
}

const clux_bitfield_field_t *clux_bitfield_get_field(clux_type_t *type,
                                                     size_t idx) {
  clux_bitfield_meta_t *meta = type ? bitfield_meta(type) : NULL;
  if (!meta || idx >= meta->count) {
    return NULL;
  }
  return &meta->fields[idx];
}

const clux_bitfield_field_t *clux_bitfield_find_field(clux_type_t *type,
                                                      const char *name) {
  clux_bitfield_meta_t *meta = type ? bitfield_meta(type) : NULL;
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

clux_type_t *clux_bitfield_get_underlying(clux_type_t *type) {
  clux_bitfield_meta_t *meta = type ? bitfield_meta(type) : NULL;
  return meta ? meta->underlying : NULL;
}

/* 位流读：小端逐位（LSB 起连续排布，跨字由语言定义，无 C 位域 UB）。
   存储是底层字的数组——按字节流统一处理即可（底层字即主机端序
   整数，位 i 落在 byte i/8 的位 i%8，恰与小端位流一致）。 */
uint64_t clux_bitfield_extract(clux_type_t *type, void *raw, size_t idx) {
  clux_bitfield_meta_t *meta = bitfield_meta(type);
  if (!meta || !raw || idx >= meta->count) {
    return 0;
  }
  const uint8_t *bytes = (const uint8_t *)raw;
  size_t bit_offset = meta->fields[idx].bit_offset;
  size_t width = meta->fields[idx].width;
  uint64_t value = 0;
  for (size_t bit = 0; bit < width && bit < 64; bit++) {
    size_t pos = bit_offset + bit;
    if ((bytes[pos / 8] >> (pos % 8)) & 1u) {
      value |= ((uint64_t)1) << bit;
    }
  }
  return value;
}

/* 位流写：按宽度掩码（高位置零）后 RMW——先清字段区再置位，
   字段区之外的位不动（缺失字段补零语义由构造的零初始化存储保证，
   这里只负责写入时不清邻接字段）。 */
void clux_bitfield_store(clux_type_t *type,
                         void *raw,
                         size_t idx,
                         uint64_t value) {
  clux_bitfield_meta_t *meta = bitfield_meta(type);
  if (!meta || !raw || idx >= meta->count) {
    return;
  }
  uint8_t *bytes = (uint8_t *)raw;
  const clux_bitfield_field_t *field = &meta->fields[idx];
  for (size_t bit = 0; bit < field->width; bit++) {
    size_t pos = field->bit_offset + bit;
    uint8_t mask = (uint8_t)(1u << (pos % 8));
    if (bit < 64 && ((value >> bit) & 1u)) {
      bytes[pos / 8] |= mask;
    } else {
      bytes[pos / 8] &= (uint8_t)~mask;
    }
  }
}

/* 结构相等核心：底层类型判等 + 字段名与宽度按声明序一一对应即相等
   （bit_offset 是声明序的派生量，名+宽度一致则必然一致），类型名
   不参与判等（与 enum/union 一致：结构兼容的不同具名类型可比较）。 */
static bool fields_equal(clux_bitfield_meta_t *self,
                         clux_type_t *underlying,
                         const clux_bitfield_field_def_t *fields,
                         size_t count) {
  if (!self || !clux_type_equal(self->underlying, underlying) ||
      self->count != count) {
    return false;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (!fields[idx].name ||
        strcmp(self->fields[idx].name, fields[idx].name) != 0 ||
        self->fields[idx].width != fields[idx].width) {
      return false;
    }
  }
  return true;
}

bool clux_bitfield_fields_equal(clux_type_t *self,
                                clux_type_t *underlying,
                                const clux_bitfield_field_def_t *fields,
                                size_t count) {
  if (!bitfield_kind(clux_type_get_kind(self))) {
    return false;
  }
  return fields_equal(bitfield_meta(self), underlying, fields, count);
}

static bool bitfield_type_equal(clux_type_t *self, clux_type_t *other) {
  if (!bitfield_kind(clux_type_get_kind(other))) {
    return false;
  }
  clux_bitfield_meta_t *other_meta = bitfield_meta(other);
  /* other 的 meta 表是完整形态（含 bit_offset），按名+宽度对比即可：
   * 复用 fields_equal 需要定义表，这里直接按声明序逐项对比。 */
  if (!other_meta) {
    return false;
  }
  clux_bitfield_meta_t *meta = bitfield_meta(self);
  if (!clux_type_equal(meta->underlying, other_meta->underlying) ||
      meta->count != other_meta->count) {
    return false;
  }
  for (size_t idx = 0; idx < meta->count; idx++) {
    if (strcmp(meta->fields[idx].name, other_meta->fields[idx].name) != 0 ||
        meta->fields[idx].width != other_meta->fields[idx].width) {
      return false;
    }
  }
  return true;
}

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

/* 字段读取（x.field，白皮书 §10）：位段不是字节对齐的连续内存，
   无法产出现有协议的内存共享 view——读原语提取位段为底层无符号
   类型的独立值（拷贝语义，读取结果类型 = 底层无符号）。
   指针接收者（p.field）由 clux_context_field 的 PTR 自动 deref
   先行解引用到存储块，天然支持。字段写入走构造通道
   （clux_context_bitfield_set_field，按宽度掩码 RMW）；
   exec 的成员赋值通道接入后由其分派，本槽位只负责读。 */
static clux_value_t *
bitfield_field(clux_context_t *ctx, clux_value_t *value, const char *name) {
  clux_type_t *type = clux_value_get_type(value);
  const clux_bitfield_field_t *field = clux_bitfield_find_field(type, name);
  if (!field) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(ctx,
                                             NULL,
                                             "bitfield '%s' has no field '%s'",
                                             type_name ? type_name
                                                       : "<unknown>",
                                             name);
  }
  clux_type_t *underlying = clux_bitfield_get_underlying(type);
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(ctx, underlying);
  }
  size_t idx = (size_t)(field - bitfield_meta(type)->fields);
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  if (!raw) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot access field '%s' of an invalid bitfield", name);
  }
  uint64_t bits = clux_bitfield_extract(type, raw, idx);
  /* 底层类型的原始位型落地：底层是无符号整数，位型即零扩展值。 */
  size_t word_size = clux_type_get_size(underlying);
  ptr_handle_t data =
      safe_alloc(clux_context_get_safe_allocator(ctx), word_size, NULL);
  (void)memcpy(ptr_to_raw(data, 0), &bits, word_size);
  return clux_context_create_value(ctx, underlying, data, NULL, NULL);
}

/* 同类型比较（白皮书 §5.5）：存储按字节比较（隐藏字数组整体 memcmp，
   未写入字段天然为零，不存在 C 位域的 padding 不定值）。 */
static clux_value_t *bitfield_compare(clux_context_t *ctx,
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
  if (!bitfield_kind(clux_type_get_kind(clux_value_get_type(right))) ||
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
  size_t size = clux_type_get_size(type);
  void *left_raw = ptr_to_raw(clux_value_get_data(left), 0);
  void *right_raw = ptr_to_raw(clux_value_get_data(right), 0);
  if (!left_raw || !right_raw) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot compare invalid bitfields");
  }
  bool equal = memcmp(left_raw, right_raw, size) == 0;
  return clux_context_create_bool(
      ctx, op == CLUX_OP_EQ ? equal : !equal, NULL, NULL);
}

static clux_value_t *
bitfield_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return bitfield_compare(ctx, left, right, CLUX_OP_EQ);
}

static clux_value_t *
bitfield_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return bitfield_compare(ctx, left, right, CLUX_OP_NE);
}

static clux_vtable_t g_bitfield_vtable = {
    .eq = bitfield_eq,
    .ne = bitfield_ne,
    .field = bitfield_field,
    .type_equal = bitfield_type_equal,
};

clux_vtable_t *get_clux_bitfield_vtable(void) { return &g_bitfield_vtable; }

clux_type_t *create_clux_bitfield_type(allocator_t *allocator,
                                       const char *name,
                                       clux_type_t *underlying,
                                       const clux_bitfield_field_def_t *fields,
                                       size_t count) {
  /* 布局按隐藏存储数组：word_count = ceil(total_bits/word_bits) 个底层
     字（至少一字——空表已被创建路径拒绝，防御性兜底取 1）。 */
  size_t word_bits = clux_type_get_size(underlying) * 8;
  size_t total_bits = 0;
  for (size_t idx = 0; idx < count; idx++) {
    total_bits += fields[idx].width;
  }
  size_t word_count = total_bits ? (total_bits + word_bits - 1) / word_bits : 1;
  size_t word_size = clux_type_get_size(underlying);
  clux_type_t *type = create_clux_type(allocator,
                                       CLUX_TYPE_BITFIELD,
                                       name,
                                       word_count * word_size,
                                       word_size,
                                       get_clux_bitfield_vtable());
  /* meta 与字段表连体成单块分配：delete_clux_type 释放 meta 本体即整块
     释放（delete_meta 为 NULL）。bit_offset 按声明序从 LSB 连续累计。 */
  clux_bitfield_meta_t *meta = clux_type_create_meta(
      type,
      sizeof(clux_bitfield_meta_t) + sizeof(clux_bitfield_field_t) * count);
  meta->underlying = underlying;
  meta->fields =
      (clux_bitfield_field_t *)((char *)meta + sizeof(clux_bitfield_meta_t));
  size_t bit_offset = 0;
  for (size_t idx = 0; idx < count; idx++) {
    meta->fields[idx].name = fields[idx].name;
    meta->fields[idx].bit_offset = bit_offset;
    meta->fields[idx].width = fields[idx].width;
    bit_offset += fields[idx].width;
  }
  meta->count = count;
  meta->total_bits = total_bits;
  clux_type_seal(type); /* 布局即时定局：构造完成即完整 */
  return type;
}
