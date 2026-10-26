#ifndef _H_CLUX_MODULE_PACKAGE_
#define _H_CLUX_MODULE_PACKAGE_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"
#include "core/arena.h"
#include "core/strslice.h"
#include <stdbool.h>
#include <stddef.h>

/* ===========================================================================
 * 包描述符（clux.mod 解析结果）
 *
 * clux.mod 是 JSON 文件，位于包根目录。结构：
 *   {
 *     "name": "myapp",
 *     "version": "0.1.0",
 *     "entry": "src/main.clux"
 *     // 或 "entry": { "pkg/sub": "src/sub.clux", ... }
 *     // "deps": { "name": "protocol:path@version", ... }  (可选)
 *   }
 *
 * entry 为 string 时是单入口包；为 object 时是多子包包（互斥）。
 * =========================================================================== */

/* ---- 依赖描述 ---- */

typedef struct {
    strslice_t name;       /* 借用：依赖名（如 "std"），arena 文本 */
    strslice_t spec;       /* 借用：完整依赖规格（如 "github:clux/std@0.1.0"） */
    strslice_t protocol;   /* 借用：协议（如 "github"、"file"） */
    strslice_t path;       /* 借用：协议内路径（如 "clux/std"） */
    strslice_t version;    /* 借用：版本/标签/分支/commit，可为空 */
} pkg_dep_t;

/* ---- 子包入口 ---- */

typedef struct {
    strslice_t name;       /* 借用：子包全名（如 "std/array"） */
    strslice_t path;       /* 借用：入口文件相对路径（如 "src/array.clux"） */
} pkg_entry_t;

/* ---- 包类型 ---- */

typedef enum {
    PKG_ENTRY_SINGLE,   /* entry 是 string */
    PKG_ENTRY_MULTI,    /* entry 是 Record<string, string> */
} pkg_entry_kind_t;

/* ---- 原生依赖（native 段） ---- */

/* 一个平台适用的 C 链接参数。字符串都借用 JSON arena，NUL 终止。
 * 只描述**已编译好的**静态/动态库；clux 不编译 C 源码、不处理头文件/宏。 */
typedef struct {
    const char **link;      /* `-l<name>` 或显式库文件 */
    size_t       link_count;
    const char **libdirs;   /* -L */
    size_t       libdir_count;
    const char **runtime;   /* 需要复制到可执行文件旁的 DLL/so */
    size_t       runtime_count;
} pkg_native_spec_t;

typedef struct {
    const char *platform;   /* "WIN64" / "LINUX" / "WIN64-X64" / "*" */
    pkg_native_spec_t spec;
} pkg_native_platform_t;

typedef struct {
    bool present;           /* clux.mod 是否有 native 段 */
    pkg_native_spec_t base;
    pkg_native_platform_t *platforms;
    size_t platform_count;
} pkg_native_t;

/* ---- 安装钩子（postinstall） ---- */

/* 一个平台适用的安装命令（在包根执行；由 `clux install --allow-scripts`
 * 显式触发，`clux build` 不会自动执行）。 */
typedef struct {
    const char *platform;   /* "WIN64" / "LINUX" / "WIN64-X64" / "*" */
    const char *command;    /* shell 命令或脚本路径 */
} pkg_postinstall_platform_t;

typedef struct {
    bool present;
    const char *command;    /* 基础命令（string 形式） */
    pkg_postinstall_platform_t *platforms;
    size_t platform_count;
} pkg_postinstall_t;

/* ---- 包描述符 ---- */

typedef struct {
    strslice_t      name;        /* 借用：包名（arena 文本） */
    strslice_t      version;     /* 借用：版本号（arena 文本） */
    pkg_entry_kind_t entry_kind;
    union {
        strslice_t   single;     /* 单入口文件路径 */
        struct {
            pkg_entry_t *entries;
            size_t       count;
        } multi;
    } entry;
    pkg_dep_t      *deps;        /* 依赖数组（可为 NULL） */
    size_t           dep_count;

    pkg_native_t     native;     /* 可选 native 段 */
    pkg_postinstall_t postinstall; /* 可选 postinstall 钩子 */

    /* 元信息 */
    char            root_dir[1024]; /* 包根目录绝对/相对路径（NUL 终止） */
} package_desc_t;

/* ---- 加载结果 ---- */

typedef enum {
    PKG_OK,               /* 成功 */
    PKG_ERR_NOT_FOUND,    /* clux.mod 文件不存在 */
    PKG_ERR_READ_FAILED,  /* 文件读取失败 */
    PKG_ERR_PARSE,        /* JSON 解析失败 */
    PKG_ERR_SCHEMA,       /* JSON 有效但不符合 clux.mod schema */
} pkg_result_t;

/* ---- API ---- */

/**
 * 从指定目录加载包描述符。
 *
 * @param alloc    分配器（用于文件读取缓冲等）
 * @param arena    arena（所有借用字符串、数组从这里分配）
 * @param dir      包根目录路径（指向 clux.mod 所在目录）
 * @param out_desc 输出描述符
 * @return PKG_OK 或错误码
 */
pkg_result_t package_load(allocator_t *alloc, arena_t *arena,
                          const char *dir, package_desc_t *out_desc);

/**
 * 从给定路径向上查找 clux.mod，找到后加载包描述符。
 *
 * @param alloc    分配器
 * @param arena    arena
 * @param start    起始查找路径（通常是当前工作目录或源文件路径）
 * @param out_dir  输出：找到的包根目录路径（NUL 终止，alloc 分配，调用方释放）
 * @param out_desc 输出描述符
 * @return PKG_OK 或错误码
 */
pkg_result_t package_find(allocator_t *alloc, arena_t *arena,
                          const char *start,
                          char **out_dir, package_desc_t *out_desc);

/**
 * 计算指定平台的有效 native 参数：base + 最具体的 platforms 条目覆盖。
 * 平台键按 "OS_TYPE-OS_ARCH" → "OS_TYPE" → "OS_ARCH" → "*" 的顺序匹配；
 * 平台条目里非空的数组覆盖 base 的对应数组。
 * 返回 NULL 表示没有 native 段；否则返回 out（内容有效）。
 */
const pkg_native_spec_t *pkg_native_effective(const pkg_native_t *native,
                                              const char *os_type,
                                              const char *os_arch,
                                              pkg_native_spec_t *out);

/**
 * 计算指定平台的有效 postinstall 命令（base 或最具体的 platforms 条目）。
 * 返回 NULL 表示没有可执行命令。
 */
const char *pkg_postinstall_effective(const pkg_postinstall_t *postinstall,
                                      const char *os_type,
                                      const char *os_arch);

/**
 * 获取错误码的可读名称（诊断用）。
 */
const char *pkg_result_name(pkg_result_t r);

/**
 * 获取 entry 的可读描述（诊断用）。
 */
const char *pkg_entry_kind_name(pkg_entry_kind_t k);

#ifdef __cplusplus
}
#endif
#endif
