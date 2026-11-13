#ifndef _H_ENGINE_PTR_
#define _H_ENGINE_PTR_
#include "engine/interface.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 取指向 base 的指针类型（"*<base>"）：同 base 恒等返回同一实例，
   新类型注册为 base 的 child，随 base 递归释放。 */
clux_type_t *clux_type_get_ptr_type(clux_type_t *base);
clux_vtable_t *get_clux_ptr_vtable(void);
#ifdef __cplusplus
}
#endif
#endif
