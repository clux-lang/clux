#ifndef _H_ENGINE_ERROR_
#define _H_ENGINE_ERROR_
#include "engine/interface.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
clux_vtable_t *get_clux_error_vtable(void);
bool clux_value_is_error(clux_value_t *value);
#ifdef __cplusplus
}
#endif
#endif
