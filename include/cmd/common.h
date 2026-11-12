#ifndef _H_CMD_COMMON_
#define _H_CMD_COMMON_
#include "core/cmdline.h"
#ifdef __cplusplus
extern "C" {
#endif

#define CMD_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/* 子命令共用的位置参数读取与未实现提示。 */
const char *cmd_first_positional(const cmdline_options_t *options);
int cmd_not_implemented(const char *command, const char *input);
#ifdef __cplusplus
}
#endif
#endif
