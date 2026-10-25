#ifndef _H_CLUX_CMD_BUILD_
#define _H_CLUX_CMD_BUILD_
#ifdef __cplusplus
extern "C" {
#endif

#include "cmd/cmd.h"

int cmd_build(const cmd_args_t *args);

/** 供 `clux run` 复用构建参数：以 `src` 为入口构建可执行文件，成功时
 *  `*out_exe` 返回 malloc 的 exe 路径（调用方 free）。 */
int cmd_build_for_run(const cmd_args_t *args, const char *src,
                      char **out_exe);

#ifdef __cplusplus
}
#endif
#endif
