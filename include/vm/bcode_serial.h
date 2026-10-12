#ifndef _H_CLUX_VM_BCODE_SERIAL_
#define _H_CLUX_VM_BCODE_SERIAL_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"
#include "vm/bcode.h"

#include <stddef.h>
#include <stdint.h>

/**
 * 字节码二进制序列化 / 反序列化（`.cxb` = clux bytecode）。
 *
 * 与 `.cxs`（文本汇编，见 bcode_asm.h）互为同一 `bytecode_t` 的两种落盘形态：
 *   `.cxb` —— 二进制直存，体积小、加载快（无需解析），用于分发/缓存；
 *   `.cxs` —— 可读文本，用于调试/手写/回归比对。
 *
 * 文件格式（全部小端）：
 *   [magic "CXBC" : 4 字节]
 *   [format_version : u32]                    当前 1
 *   [str_count : u32]                          字符串表条目数
 *   重复 str_count 次：[str_len : u32][bytes : str_len 字节]
 *   [code_len : u64]                           字节码流长度（字节）
 *   [code : code_len 字节]                     原样字节码
 *
 * 字符串表无 NUL 终止（长度前缀），code 段原样落盘。加载方按 str_count /
 * code_len 严格校验越界与截断，任何不一致视为损坏并失败（不 panic）。
 */

/* 当前二进制格式版本号。
 *
 * v2：新增 BCODE_SET_OWNERSHIP（fatal 复合类型将亡值变体），它插在指令枚举
 * 中段，使该指令之后的全部 opcode 编号整体后移——v1 的 .cxb 落盘字节用新
 * 表解释会张冠李戴（静默错误执行）。因此必须在读版本号时直接拒绝旧文件。 */
#define BCODE_SERIAL_VERSION 2u

/* 魔数："CXBC"（clux bytecode），小端写入。 */
#define BCODE_SERIAL_MAGIC 0x43425843u /* 'C''X''B''C' little-endian */

/**
 * 将字节码模块序列化为二进制并写入 `out_path`。
 *
 * @return 0 成功；-1 失败（bc/out_path 为 NULL 或文件无法写入）
 */
int bcode_serial(const bytecode_t *bc, const char *out_path);

/**
 * 序列化到内存：返回由 `alloc` 分配的缓冲（长度经 `out_len` 取回）。
 *
 * `alloc` 必须存活至缓冲使用完毕；调用方用完须 `allocator_free(alloc, &buf)`
 * 释放（allocator_new_ex 的分配不随 delete_allocator 自动回收）。
 * 失败（bc/alloc 为 NULL）返回 NULL。
 */
uint8_t *bcode_serial_mem(allocator_t *alloc, const bytecode_t *bc,
                          size_t *out_len);

/**
 * 从内存缓冲反序列化为 `bytecode_t`（含 strtable 与 code 流）。
 *
 * 严格校验魔数、版本、各段长度与边界；`data`/`len` 生命周期只需覆盖
 * 本调用（内容被深拷贝进新模块）。失败时 `*out_bc` 置 NULL 并打印
 * 诊断到 stderr。
 *
 * @return 0 成功；非 0 失败（参数非法 / 格式不匹配 / 数据损坏 / 越界）
 */
int bcode_deserial(allocator_t *alloc, const uint8_t *data, size_t len,
                   bytecode_t **out_bc);

/**
 * 从 `.cxb` 文件读取并反序列化（内部 fopen/fread 整读）。
 * 语义同 bcode_deserial，仅输入来源为文件。
 *
 * @return 0 成功；非 0 失败（文件无法打开 / 读取不足 / 格式错误）
 */
int bcode_deserial_from_file(allocator_t *alloc, const char *path,
                             bytecode_t **out_bc);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_BCODE_SERIAL_ */
