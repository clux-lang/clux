#ifndef _H_ENGINE_VOID_
#define _H_ENGINE_VOID_
#include "engine/interface.h"
#ifdef __cplusplus
extern "C" {
#endif

/* void 类型 vtable：无值无运算，仅按 kind 判等（context 单例）。 */
clux_vtable_t *get_clux_void_vtable(void);

#ifdef __cplusplus
}
#endif
#endif
