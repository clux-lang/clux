#include "core/error.h"
#include "engine/interface.h"
static clux_vtable_t g_error_vtable = {
    .add = NULL,
};
clux_vtable_t *get_clux_error_vtable(void) { return &g_error_vtable; }