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

/** 测试模式：编译文件及 import 模块，执行其中所有 test 块。 */
int driver_test_file(const char *path);

/** 测试模式：编译一组源码文件（项目模式），统一执行测试。 */
int driver_test_sources(const char *const *paths, size_t count);

/**
 * 编译流水线（加载 → 词法 → 语法 → 语义 → 编译字节码）后停止，**不执行**
 * 程序，仅将产物 `bytecode_t` 反汇编为字符格式汇编并写出到 `out_path`
 * （`.cxs` 文本）。
 *
 * 返回进程退出码：
 *   0  —— 编译成功并写出 asm
 *   1  —— 编译错误 / 文件无法打开 / asm 文件无法写出
 */
int driver_build_asm(const char *src_path, const char *out_path);

/**
 * 编译流水线（加载 → 词法 → 语法 → 语义 → 编译字节码）后停止，**不执行**
 * 程序，仅将产物 `bytecode_t` 序列化为二进制字节码并写出到 `out_path`
 * （`.cxb` = clux bytecode）。
 *
 * 返回进程退出码：
 *   0  —— 编译成功并写出 bin
 *   1  —— 编译错误 / 文件无法打开 / bin 文件无法写出
 */
int driver_build_bin(const char *src_path, const char *out_path);

/**
 * 逆向流程：将 `.cxs` 汇编文本汇编为字节码并直接执行（等价于
 * `build --emit-asm` 产物的反过程）。
 *
 * 复用 run 的执行阶段（注册函数 → 调用 main）。与 `driver_run_file`
 * 不同的是这里不经历 lex→parse→sema→compile，而是从已序列化的
 * 字符格式字节码重新加载。
 *
 * 返回进程退出码：
 *   0  —— 汇编 + 执行成功
 *   1  —— 汇编错误 / 文件无法打开 / 执行错误 / 无入口 `main`
 */
int driver_run_asm(const char *asm_path);

/**
 * 落盘格式互转（不执行、不经过源码前端）：
 *   `.cxs` 文本 --(汇编)--> `bytecode_t` --(序列化)--> `.cxb` 二进制
 *
 * 返回进程退出码：
 *   0  —— 成功
 *   1  —— 文件无法打开 / 汇编错误 / 写出失败
 */
int driver_asm_to_bin(const char *asm_path, const char *bin_path);

/**
 * 落盘格式互转（不执行、不经过源码前端）：
 *   `.cxb` 二进制 --(反序列化)--> `bytecode_t` --(反汇编)--> `.cxs` 文本
 *
 * 返回进程退出码：
 *   0  —— 成功
 *   1  —— 文件无法打开 / 反序列化失败 / 写出失败
 */
int driver_bin_to_asm(const char *bin_path, const char *asm_path);

/* ===========================================================================
 * 输入类型探测（内容嗅探，不依赖扩展名）
 * =========================================================================== */

typedef enum {
    DRIVER_INPUT_UNKNOWN = 0, /* 无法判定（空文件 / 不匹配任何特征） */
    DRIVER_INPUT_SOURCE,      /* clux 源码（.cx）：含 func/var/... 关键字 */
    DRIVER_INPUT_CXS,         /* 字节码汇编文本（.cxs）：指令行 / 标签 */
    DRIVER_INPUT_CXB,         /* 字节码二进制（.cxb）：含 "CXBC" magic */
} driver_input_kind_t;

/**
 * 按**文件内容**嗅探输入类型（扩展名不可靠，不作为判据）。
 *
 * 判定规则：
 *   1. 前 4 字节为 "CXBC" magic → DRIVER_INPUT_CXB（确定）
 *   2. 首个有效行（跳过空白 / `;` 注释 / `[.section]`）形如 `name:`
 *      标签定义，或首 token 命中 BCODE_ASM_TABLE 助记符 → DRIVER_INPUT_CXS
 *   3. 含 clux 关键字（func/var/if/while/return/...）→ DRIVER_INPUT_SOURCE
 *   4. 其余（含空文件、纯空白、不可识别内容）→ DRIVER_INPUT_UNKNOWN
 *
 * @param path 输入文件路径
 * @return 嗅探结果；文件无法打开返回 DRIVER_INPUT_UNKNOWN
 */
driver_input_kind_t driver_detect_input(const char *path);

/** 探测结果的稳定名称（用于诊断）："source" / "cxs" / "cxb" / "unknown" */
const char *driver_input_kind_name(driver_input_kind_t kind);

/**
 * 逆向流程：将 `.cxb` 二进制字节码反序列化并直接执行（等价于
 * `build --emit-bin` 产物的反过程）。
 *
 * 复用 run 的执行阶段（注册函数 → 调用 main）。与 `driver_run_file`
 * 不同的是这里不经历 lex→parse→sema→compile，而是从已序列化的
 * 二进制字节码重新加载。
 *
 * 返回进程退出码：
 *   0  —— 加载 + 执行成功
 *   1  —— 格式错误 / 文件无法打开 / 执行错误 / 无入口 `main`
 */
int driver_run_bin(const char *bin_path);

#ifdef __cplusplus
}
#endif
#endif
