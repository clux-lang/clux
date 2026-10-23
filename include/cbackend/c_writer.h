#ifndef _H_CLUX_CBACKEND_C_WRITER_
#define _H_CLUX_CBACKEND_C_WRITER_

#include "cbackend/c_ir.h"
#include "core/allocator.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int cir_write_file(const cir_unit_t *unit, const char *path,
                   allocator_t *alloc);

#ifdef __cplusplus
}
#endif
#endif
