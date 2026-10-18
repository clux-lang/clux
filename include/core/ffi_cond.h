#ifndef _H_CLUX_CORE_FFI_COND_
#define _H_CLUX_CORE_FFI_COND_
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/* ===========================================================================
 * FFI 声明的**条件后缀**：`extern lib <别名> from "<库名>" assert(<条件>);`
 *
 * 第二阶段（按场景选库）：不同平台/发行版里，同一个逻辑库（例如 C 运行库）
 * 叫法不同（`msvcrt` / `c` / `System`）。与其把库名写死，不如写**多条**声明、
 * 用条件后缀声明各自的适用范围：
 *
 *     extern lib crt from "msvcrt" assert(OS_TYPE == "WIN64" || OS_TYPE == "WIN32");
 *     extern lib crt from "c"      assert(OS_TYPE == "LINUX");
 *
 * **求值时机 = 前脱糖（desugar，parser → sema 之间）**：条件为假的那条
 * `extern lib` **整条被裁剪**——不加载、不登记别名、不校验符号。因此多条
 * 同别名声明只要条件互斥就不冲突；sema/driver/容器（.cxb/.cxs）只会见到
 * 存活的那一条。
 *
 * 条件语言（故意极小，**不是**通用表达式）：
 *   cond   := or
 *   or     := and ( '||' and )*
 *   and    := cmp ( '&&' cmp )*
 *   cmp    := '(' cond ')' | KEY ( '==' | '!=' ) "字符串"
 *
 * `&&` 优先级高于 `||`（与 C 一致）；括号可改变结合。`KEY` 是**注入的键**
 * （不是变量，程序别处看不见它），值恒为字符串：
 *   - 内建：`OS_TYPE`（`WIN64` / `WIN32` / `LINUX` / `MACOS` / `BSD`）、
 *           `OS_ARCH`（`X64` / `X86` / `ARM64` / `ARM`）；
 *   - 命令行 `-D KEY=VALUE`（可重复；同名覆盖内建）。
 * 未定义的键在求值时报错——拼写错误若静默变成 false，会让整条声明被无声裁剪。
 *
 * 键集合是**进程级全局静态表**（见下），不随编译单元传递：desugar / 解析器 /
 * 测试都直接读写同一张表；`ffi_defines_reset()` 把它恢复成"只有内建键"。
 * =========================================================================== */

/** 键集合容量与字符串长度上限（静态表，无堆分配）。 */
#define FFI_DEFINES_MAX 64
#define FFI_DEFINES_KEY_MAX 64
#define FFI_DEFINES_VALUE_MAX 256

/**
 * 写入内建键（`OS_TYPE` / `OS_ARCH`，按当前编译目标）。幂等：已有同名键覆盖。
 * 首次调用 `ffi_defines_add/get` 时也会自动初始化内建键，所以通常不必显式调。
 */
void ffi_defines_add_builtins(void);

/** 清空全表并重新写入内建键（测试 / 重新注入时用）。 */
void ffi_defines_reset(void);

/**
 * 添加/覆盖一个键值。`key` 必须形如标识符、`value` 任意字符串（可为空）。
 * 返回 0 成功；非 0 失败（非法键 / 超长 / 表满）。
 */
int ffi_defines_add(const char *key, const char *value);

/** 解析 `KEY=VALUE` 并添加（供命令行 `-D` 使用）。返回 0 成功。 */
int ffi_defines_add_kv(const char *kv);

/** 查值；不存在返回 NULL。 */
const char *ffi_defines_get(const char *key);

/** 条目数（诊断用：列出已知键）。 */
size_t ffi_defines_count(void);

/** 第 i 条的键（i < count；否则 NULL）。 */
const char *ffi_defines_key_at(size_t i);

/**
 * 只做**语法**校验（不求值、不检查键是否存在）。供解析器在拿到条件文本时
 * 立刻报错，位置更精确。成功返回 true；失败把原因写进 err 并返回 false。
 */
bool ffi_cond_validate(const char *text, char *err, size_t ecap);

/**
 * 按当前全局键表求值。`defs` 为 NULL 的语义见 ffi_cond_validate；求值总是用
 * 全局表。
 *
 * 成功返回 true 并写 *out；失败（语法错 / 未知键）返回 false 并写 err。
 */
bool ffi_cond_eval(const char *text, bool *out, char *err, size_t ecap);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_CORE_FFI_COND_ */
