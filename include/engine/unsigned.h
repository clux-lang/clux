#ifndef _H_ENGINE_UNSIGNED_
#define _H_ENGINE_UNSIGNED_
#include "engine/interface.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
clux_vtable_t *get_clux_u8_vtable(void);
clux_vtable_t *get_clux_u16_vtable(void);
clux_vtable_t *get_clux_u32_vtable(void);
clux_vtable_t *get_clux_u64_vtable(void);
clux_vtable_t *get_clux_usize_vtable(void);
#ifdef __cplusplus
}
#endif
#endif
