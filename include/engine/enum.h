#ifndef _H_ENGINE_ENUM_
#define _H_ENGINE_ENUM_
#include "core/allocator.h"
#include "engine/interface.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 枚举成员（白皮书 §3.6）：底层为整数时 value 是编译期整数常量按
   底层类型宽度截断存储的原始位型（无符号读取，解释归底层类型的
   kind）；底层为 str 时 str 是成员的字符串常量（context 路径经 strs
   驻留），value 不用（恒 0）。全表成员值形态与底层类型一致。 */
typedef struct _clux_enum_variant_t {
  const char *name; /* 借用调用方的驻留字符串，类型不拷贝不释放 */
  uint64_t value;
  const char *str; /* 底层为 str 时非 NULL，否则 NULL */
} clux_enum_variant_t;
/* 创建具名 enum 类型（白皮书 §3.6）：布局（size/align）与底层类型
   （整数或 str）一致，值表示即底层的原始位型。variant 表整体拷入
   meta，variant name 与 str 常量由调用方保证生命周期（context 路径
   经 strs 驻留）。 */
clux_type_t *create_clux_enum_type(allocator_t *allocator,
                                   const char *name,
                                   clux_type_t *underlying,
                                   const clux_enum_variant_t *variants,
                                   size_t count);
clux_vtable_t *get_clux_enum_vtable(void);
/* variant 表查询：按声明序。 */
size_t clux_enum_variant_count(clux_type_t *type);
const clux_enum_variant_t *clux_enum_get_variant(clux_type_t *type, size_t idx);
const clux_enum_variant_t *clux_enum_find_variant(clux_type_t *type,
                                                  const char *name);
/* 底层整数类型（借用）。 */
clux_type_t *clux_enum_get_underlying(clux_type_t *type);
/* 结构判等（底层类型 + 成员名 + 成员值按声明序）：把已有 enum 类型与
   原始成员表对比，供 context 创建路径做 find-or-dedup，类型名不参与
   判等（与 union 一致：结构兼容的不同具名类型可以比较）。 */
bool clux_enum_variants_equal(clux_type_t *self,
                              clux_type_t *underlying,
                              const clux_enum_variant_t *variants,
                              size_t count);
#ifdef __cplusplus
}
#endif
#endif
