#ifndef _H_CLUX_CMD_PATH_
#define _H_CLUX_CMD_PATH_

#ifdef __cplusplus
extern "C" {
#endif

#include "cmd/cmd.h"

/**
 * 由输入路径推导输出路径：与输入同目录、去原扩展名后加 `ext`。
 *
 * 例：cmd_derive_out_path("a/b/fib.cxs", ".cxb") → "a/b/fib.cxb"
 *
 * 返回新分配的字符串（调用方 free）；内存不足返回 NULL。
 */
char *cmd_derive_out_path(const char *src, const char *ext);

/**
 * 输出路径解析：`args` 中 `key` 选项的 value 优先（如 --output=PATH），
 * 否则用 `cmd_derive_out_path(in_path, ext)` 推导。
 *
 * 成功时 `*out_path` 指向结果（可能是 args 中的字符串，或新分配）；
 * `*out_owned` 为需要 free 的指针（无则为 NULL）。
 *
 * 返回 0 成功；1 失败（内存不足，已打印诊断）。
 */
int cmd_resolve_output(const cmd_args_t *args, const char *key,
                       const char *in_path, const char *ext,
                       const char **out_path, char **out_owned);

/**
 * 解析单横线 `-o PATH` / `-o=PATH`（唯一允许的短选项）。
 *
 * `cmd_args_parse` 只把 `--` 前缀识别为选项，故 `-o` 会落入位置参数，
 * 需由 handler 显式扫描。本函数扫描位置参数并返回 `-o` 的取值，同时
 * 通过 `out_pos` / `out_posn` 交回**过滤掉 `-o` 及其取值后**的位置参数
 * 序列，供 handler 按语义取用（如 pos[0]=子操作、pos[1]=输入…）。
 *
 * @param args      解析结果
 * @param opt_out   `-o` 的值；未出现为 NULL，出现但缺值为缺值标记（见返回）
 * @param out_pos   输出：过滤后的位置参数数组（调用方提供，容量足够）
 * @param out_posn  输出：过滤后的位置参数个数
 * @param out_cap   `out_pos` 容量
 * @return 0 正常；1 出现 `-o` 但缺值
 */
int cmd_take_short_output(const cmd_args_t *args,
                          const char **opt_out,
                          const char **out_pos, size_t *out_posn,
                          size_t out_cap);

/**
 * 从位置参数序列里剥离 `-D KEY=VALUE` / `-DKEY=VALUE`（可重复；CMake 风格）。
 *
 * 与 `cmd_take_short_output` 同因：`cmd_args_parse` 只认 `--`，单横线的 `-D`
 * 会落进位置参数，必须由 handler 显式扫描。剥离后的其余位置参数按序写入
 * `out_pos`（容量 pos_cap），`-D` 的 `KEY=VALUE` 写入 `out_kv`（容量 kv_cap）。
 *
 * @return 0 正常；1 出现 `-D` 但缺值；2 `KEY=VALUE` 条目超出 kv_cap
 */
int cmd_take_defines(const char *const *in, size_t n, const char **out_pos,
                     size_t *out_posn, size_t pos_cap, const char **out_kv,
                     size_t *out_kvn, size_t kv_cap);

/**
 * 把收集到的 `KEY=VALUE` 条目写进**全局 FFI 键表**（core/ffi_cond.h）——
 * `extern lib ... assert(...)` 的求值就查这张表。非法条目打印错误并返回 1。
 */
int cmd_apply_defines(const char *const *kv, size_t n);

#ifdef __cplusplus
}
#endif
#endif
