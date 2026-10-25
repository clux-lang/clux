#ifndef _H_CLUX_DRIVER_DRIVER_
#define _H_CLUX_DRIVER_DRIVER_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"

/* token_t / vec_t are opaque at this boundary; only pointers cross it. */
typedef struct _token_t token_t;
typedef struct _vec_t vec_t;

/* ===========================================================================
 * clux 编译流水线编排层（Driver）
 *
 * 目前落地的阶段：
 *   ① 加载源文件  —— 读入 allocator 管理的内存缓冲
 *   ② 词法分析    —— Lexer 切分为 token 并汇入 token 池（vec<token_t*>）
 *   ③ 语法分析    —— Parser 将 token 流解析为 AST
 *   ④ 语义分析    —— Sema 构建作用域树 + shadow VM 类型检查（快速失败）
 *   ⑤ 输出 AST    —— 打印 AST 结构到 stdout（M1 阶段）
 *
 * 未来阶段（Bytecode Compiler / Bytecode VM）将在此框架内顺序插入。
 * =========================================================================== */

/**
 * 加载源码文件到内存（allocator 管理）。
 *
 * 以二进制模式（"rb"）整读文件。返回的 `out_data` 指向一块由 allocator
 * 分配、长度为 `out_len` 的缓冲，其生命周期由 `alloc` 管理——调用方**不**
 * 应手动释放它；只要 `alloc` 仍存活（且未被 delete），`out_data` 即可安全
 * 读取。
 *
 * 返回 0 成功；返回 -1 表示文件无法打开或读取失败（此时 `*out_data` 为 NULL）。
 */
int driver_load_source(allocator_t *alloc,
                       const char *path,
                       const char **out_data,
                       size_t *out_len);

/**
 * 加载源码并做词法分析，结果汇入 token 池。
 *
 * `out_pool` 接收一个新分配的 `vec_t*`（元素为 `token_t*`，owns_element=true，
 * 随 `vec_free` 自动释放每个 token）。池中按源码顺序保存所有 token，
 * 包括 `TOKEN_TYPE_WHITESPACE` / 注释，直至并包含 `TOKEN_TYPE_EOF`。
 * 词法错误以 `TOKEN_TYPE_ERROR` token 形式留在池中（不 fail-fast）。
 *
 * 返回 0 成功；返回 -1 表示文件无法打开（此时 `*out_pool` 为 NULL）。
 */
int driver_lex_file(allocator_t *alloc, const char *path, vec_t **out_pool);

/**
 * 流水线顶层入口：加载 → 词法分析 → 语法分析 → 语义分析 → 输出 AST。
 *
 * 词法错误直接快速失败（返回 1）。词法通过后进入语法分析；
 * 语法错误时输出诊断到 stderr（返回 1）。语法通过后进入语义分析；
 * 语义错误时输出诊断到 stderr（返回 1，不输出 AST）。
 *
 * 返回进程退出码：
 *   0  —— 成功（无词法/语法/语义错误、文件可读）
 *   1  —— 编译错误（文件无法打开 / 词法错误 / 语法错误 / 语义错误）
 */
int driver_run_file(const char *path);

/**
 * 同 `driver_run_file`，但把 main 的整数返回值经 `*exit_code` 带回
 * （void / 非整数 main 记为 0）。`clux run` 用它把退出码传回进程，
 * 与 `clux build` 生成的可执行文件保持一致（差分测试依赖这一点）。
 *
 * 返回：0 成功（此时 *exit_code 有效）；1 编译/运行错误。
 */
int driver_run_file_exit(const char *path, int *exit_code);


#ifdef __cplusplus
}
#endif
#endif
