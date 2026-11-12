#ifndef _H_CMD_INSTALL_
#define _H_CMD_INSTALL_
#include "core/allocator.h"
#include "core/cmdline.h"
#ifdef __cplusplus
extern "C" {
#endif

void cmd_install_register(cmdline_t *cmdline, allocator_t *allocator);
#ifdef __cplusplus
}
#endif
#endif
