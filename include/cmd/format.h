#ifndef _H_CMD_FORMAT_
#define _H_CMD_FORMAT_
#include "core/allocator.h"
#include "core/cmdline.h"
#ifdef __cplusplus
extern "C" {
#endif

void cmd_format_register(cmdline_t *cmdline, allocator_t *allocator);
#ifdef __cplusplus
}
#endif
#endif
