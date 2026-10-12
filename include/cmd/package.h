#ifndef _H_CLUX_CMD_PACKAGE_
#define _H_CLUX_CMD_PACKAGE_
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/* ===========================================================================
 * CLI 包上下文辅助
 *
 * 从当前工作目录查找 clux.mod，解析入口路径。
 *   - 单 entry 项目：无需文件名参数，直接返回 entry 路径
 *   - 多 entry 项目：子包名作为参数，返回对应子包的入口路径
 *
 * 调用方获得一个相对于包根目录的入口文件绝对/相对路径，
 * 后续可直接传给 driver_run_file / format 等命令。
 * =========================================================================== */

/**
 * 从当前工作目录解析包入口。
 *
 * @param subdir     子包名（多 entry 时指定，单 entry 传 NULL）
 * @param out_path   输出：入口文件路径（相对于包根，NUL 终止，malloc 分配，
 *                   调用方 free）
 * @param out_root   输出：包根目录路径（NUL 终止，malloc 分配，调用方 free），
 *                   可为 NULL（不需要时）
 * @return 0 成功；非 0 失败（诊断已打印到 stderr）
 */
int cmd_resolve_entry(const char *subdir, char **out_path, char **out_root);

/**
 * 收集包内所有 .cx 源文件（递归遍历包根目录）。
 * 用于 `clux format`（全项目格式化）。
 *
 * @param root_dir   包根目录
 * @param out_paths  输出：文件路径数组（每个 malloc 分配）
 * @param out_count  输出：文件数量
 * @return 0 成功；非 0 失败
 */
int cmd_collect_sources(const char *root_dir, char ***out_paths,
                        size_t *out_count);

#ifdef __cplusplus
}
#endif
#endif
