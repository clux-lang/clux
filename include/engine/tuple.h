#ifndef _H_ENGINE_TUPLE_
#define _H_ENGINE_TUPLE_
#include "core/allocator.h"
#include "engine/interface.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 创建元组类型（白皮书 §2.6）：元素按声明序、各自按自身对齐平铺的
   值类型；元素按位置访问（index），判等按字节比较（§5.5）。
   空元组合法（size 0、align 1）。类型判等 = 元素类型序列按声明序
   结构一致。 */
clux_type_t *create_clux_tuple_type(allocator_t *allocator,
                                    const char *name,
                                    clux_type_t **elems,
                                    size_t count);
clux_vtable_t *get_clux_tuple_vtable(void);
/* 元素类型序列（连续数组，随类型 meta 释放）。 */
size_t clux_tuple_elem_count(clux_type_t *type);
/* 越界返回 NULL。 */
clux_type_t *clux_tuple_get_elem(clux_type_t *type, size_t idx);
/* 元素在 data 内的绝对偏移（按声明序平铺计算）。 */
size_t clux_tuple_elem_offset(clux_type_t *type, size_t idx);
/* 结构判等：元素类型序列按声明序一一对应即相等（供 context 去重）。 */
bool clux_tuple_signature_equal(clux_type_t *self,
                                clux_type_t **elems,
                                size_t count);
#ifdef __cplusplus
}
#endif
#endif
