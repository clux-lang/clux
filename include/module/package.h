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
