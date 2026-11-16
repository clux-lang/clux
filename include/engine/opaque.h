#ifndef _H_ENGINE_OPAQUE_
#define _H_ENGINE_OPAQUE_
#include "engine/interface.h"
#ifdef __cplusplus
extern "C" {
#endif

/* opaque 类型 vtable（context 单例）：无类型载荷，布局等同裸指针。
   任何指针 → opaque 走各指针类型 safe_cast 的隐式放行；
   opaque → 任意指针走本 vtable 的 cast（显式 as）。 */
clux_vtable_t *get_clux_opaque_vtable(void);

#ifdef __cplusplus
}
#endif
#endif
