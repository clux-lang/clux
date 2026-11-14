#ifndef _H_ENGINE_STRUCT_
#define _H_ENGINE_STRUCT_
#include "core/allocator.h"
#include "engine/interface.h"
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* struct 字段：按声明序顺序布局（C 布局），所有字段同时有效
   （与 tag union 的单成员激活不同）。 */
typedef struct _clux_struct_field_t {
  const char *name; /* 借用调用方的驻留字符串，类型不拷贝不释放 */
  clux_type_t *type;
} clux_struct_field_t;
/* 创建具名 struct 类型（白皮书 §3-declarations）：字段按声明序顺序平铺
   （各自按自身对齐取偏移，size 向类型对齐取整，align = max(字段对齐)，
   空表非法）。字段表整体拷入 meta，字段 name 由调用方保证生命周期
   （context 路径经 strs 驻留）。 */
clux_type_t *create_clux_struct_type(allocator_t *allocator,
                                     const char *name,
                                     const clux_struct_field_t *fields,
                                     size_t count);
clux_vtable_t *get_clux_struct_vtable(void);
/* 字段表查询：按声明序。 */
size_t clux_struct_field_count(clux_type_t *type);
const clux_struct_field_t *clux_struct_get_field(clux_type_t *type, size_t idx);
const clux_struct_field_t *clux_struct_find_field(clux_type_t *type,
                                                  const char *name);
/* 字段在 Data 内的偏移（偏移 0 起，按声明序平铺）。 */
size_t clux_struct_field_offset(clux_type_t *type, size_t idx);
/* 结构判等（字段名+类型按声明序）：把已有 struct 类型与原始字段表对比，
   供 context 创建路径做 find-or-dedup，无需先构造临时类型实例。 */
bool clux_struct_fields_equal(clux_type_t *self,
                              const clux_struct_field_t *fields,
                              size_t count);
#ifdef __cplusplus
}
#endif
#endif
