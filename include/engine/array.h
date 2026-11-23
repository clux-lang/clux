#ifndef _H_ENGINE_ARRAY_
#define _H_ENGINE_ARRAY_
#include "engine/interface.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 取元素为 base、长度为 len 的定长数组类型（"[len]<base>"）：data 是
   N 个元素的连续内联块，非指针。同 (base, len) 恒等返回同一实例，
   新类型注册为 base 的 child，随 base 递归释放。 */
clux_type_t *clux_type_get_array_type(clux_type_t *base, size_t len);
/* 读取定长数组的长度（构造/穷尽校验用）：非 array 类型返回 0。 */
size_t clux_array_get_length(clux_type_t *type);
/* 读取定长数组的元素类型（tuple 内存兼容吸收用）：非 array 类型返回
   NULL。 */
clux_type_t *clux_array_get_base(clux_type_t *type);
clux_vtable_t *get_clux_array_vtable(void);
#ifdef __cplusplus
}
#endif
#endif
