#ifndef _H_ENGINE_BITFIELD_
#define _H_ENGINE_BITFIELD_
#include "core/allocator.h"
#include "engine/interface.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 位段字段定义（白皮书 §10）：name 借用调用方的驻留字符串（context
   路径经 strs 驻留），类型不拷贝不释放；width 是编译期整数常量，
   合法范围 1..底层位宽（由创建路径校验）。bit_offset 由引擎按
   声明序从 LSB 起连续累计，不由调用方填写。 */
typedef struct _clux_bitfield_field_def_t {
  const char *name;
  size_t width;
} clux_bitfield_field_def_t;
/* meta 内的字段表条目：定义 + 引擎计算的位偏移（LSB 起连续排布）。 */
typedef struct _clux_bitfield_field_t {
  const char *name;
  size_t bit_offset;
  size_t width;
} clux_bitfield_field_t;
/* 创建具名 bitfield 类型（白皮书 §10）：一阶段构造（字段只是宽度
   常量，无类型引用）——布局即时定局：word_count = ceil(total_bits /
   word_bits)，size/align 按隐藏存储数组（word_count 个底层字）计算。
   字段表整体拷入 meta（连体单块），name 借用调用方驻留串。 */
clux_type_t *create_clux_bitfield_type(allocator_t *allocator,
                                       const char *name,
                                       clux_type_t *underlying,
                                       const clux_bitfield_field_def_t *fields,
                                       size_t count);
clux_vtable_t *get_clux_bitfield_vtable(void);
/* 字段表查询：按声明序。 */
size_t clux_bitfield_field_count(clux_type_t *type);
const clux_bitfield_field_t *clux_bitfield_get_field(clux_type_t *type,
                                                     size_t idx);
const clux_bitfield_field_t *clux_bitfield_find_field(clux_type_t *type,
                                                      const char *name);
/* 底层无符号类型（借用）。 */
clux_type_t *clux_bitfield_get_underlying(clux_type_t *type);
/* 位段提取/写入：小端位流（LSB 起连续），跨字由语言定义。raw 是
   bitfield 值的存储块（隐藏字数组），idx 是字段下标。写入按宽度
   掩码（高位置零后 RMW），提取返回零扩展的底层位型。 */
uint64_t clux_bitfield_extract(clux_type_t *type, void *raw, size_t idx);
void clux_bitfield_store(clux_type_t *type,
                         void *raw,
                         size_t idx,
                         uint64_t value);
/* 结构判等（底层类型 + 字段名与宽度按声明序）：把已有 bitfield 类型
   与原始字段定义表对比，供 context 创建路径做 find-or-dedup，
   类型名不参与判等（与 enum/union 一致）。 */
bool clux_bitfield_fields_equal(clux_type_t *self,
                                clux_type_t *underlying,
                                const clux_bitfield_field_def_t *fields,
                                size_t count);
#ifdef __cplusplus
}
#endif
#endif
