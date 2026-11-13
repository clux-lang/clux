#ifndef _H_ENGINE_SLICE_
#define _H_ENGINE_SLICE_
#include "core/safe_alloc.h"
#include "engine/interface.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 取元素为 base 的切片类型（"[]<base>"）：slice 是指针 + usize 的胖指针，
   不拥有指向的内存。同 base 恒等返回同一实例，新类型注册为 base 的
   child，随 base 递归释放。 */
/* slice 的 value data 布局：指针 + usize 长度（胖指针视图，不拥有内存） */
typedef struct _clux_slice_data_t {
  ptr_handle_t ptr;
  size_t len;
} clux_slice_data_t;
clux_type_t *clux_type_get_slice_type(clux_type_t *base);
clux_vtable_t *get_clux_slice_vtable(void);
#ifdef __cplusplus
}
#endif
#endif
