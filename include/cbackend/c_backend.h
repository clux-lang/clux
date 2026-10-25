#ifndef _H_CLUX_CBACKEND_C_BACKEND_
#define _H_CLUX_CBACKEND_C_BACKEND_
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/**
 * C 后端：lex → parse → desugar → sema → C IR → C 源文件 → host cc。
 * 当前只支持无 import、无 clux.mod 的单文件入口。
 *
 * 构建分两步：`.cx → .c → (host cc -c) → .o/.obj`，再用 host cc 链接成
 * 可执行文件。对象文件保留在产物旁边（多文件构建时可复用）。
 */

/* 宿主编译器/链接器选项（驱动层从 CLI 收集；数组在调用期间有效，
 * 元素为 argv 指针，不需要拷贝）。所有字段可省略（NULL/0 = 默认）。 */
typedef struct cbackend_toolchain {
    const char *compiler; /* C 编译器（NULL/空 = $CC，再回退平台默认） */
    const char *const *cflags;      /* 编译选项（逐条，原样传给编译器） */
    size_t cflag_count;
    const char *const *ldflags;     /* 链接选项（逐条，原样传给编译器驱动） */
    size_t ldflag_count;
    const char *const *include_dirs; /* -I 搜索目录 */
    size_t include_count;
    const char *const *lib_dirs;     /* -L 搜索目录 */
    size_t libdir_count;
    const char *const *libs;         /* -l 库名（不含 "-l" 前缀） */
    size_t lib_count;
} cbackend_toolchain_t;

/** 构建可执行文件：cx → c → o → exe。`tc` 可为 NULL（默认工具链）。 */
int cbackend_build_file_ex(const char *src_path, const char *out_path,
                           const cbackend_toolchain_t *tc);

/**
 * 通用构建入口（路径由调用方给全，便于把中间产物放到构建目录的镜像位置）：
 *   - `out_path != NULL`：链接成该可执行文件；
 *   - `out_path == NULL`：只编译到目标文件（`obj_path` 必填）；
 *   - `c_path`/`obj_path` 为 NULL 时按 `<out>.clux.c` / `<out>.o|.obj` 推导。
 */
int cbackend_build_ex(const char *src_path, const char *out_path,
                      const char *c_path, const char *obj_path,
                      const cbackend_toolchain_t *tc);

/** 只编译到目标文件（不链接）：cx → c → o。 */
int cbackend_emit_object(const char *src_path, const char *obj_path,
                         const cbackend_toolchain_t *tc);

/** 等价于 `cbackend_build_file_ex(src, out, NULL)`。 */
int cbackend_build_file(const char *src_path, const char *out_path);

/* 多文件（import）构建：每个模块独立生成 `.c/.h` 与对象，最后统一链接。
 * 模块前缀由各自的路径（相对项目根目录）决定，跨模块调用通过导出符号
 * `clux_<tag>_fn_<name>` / `clux_<tag>_v_<name>` 解析。 */
typedef struct cbackend_program {
    const char *build_dir;   /* 镜像根（依赖模块的 .c/.h/.o 放这里；NULL="build"） */
    const char *entry_c;     /* 入口 .c 路径（NULL = 镜像推导） */
    const char *entry_obj;   /* 入口目标文件（NULL = 镜像推导） */
    bool        object_only; /* 只编译到对象（不链接） */
    bool        emit_c_only; /* 只生成 C/头文件 */
    bool        keep_c;      /* 编译后保留生成的 .c（调试） */
    /* 测试模式：test 块改写成 `__clux_test_N` 函数，入口模块生成 runner
       main（跨模块合并用例表）；不要求 main。 */
    bool        tests;
    char     ***out_test_names; /* tests：返回 malloc 的用例显示名数组 */
    size_t     *out_test_count;
} cbackend_program_t;

int cbackend_build_program(const char *src_path, const char *out_path,
                           const cbackend_program_t *pgm,
                           const cbackend_toolchain_t *tc);
/**
 * 单文件 C 源生成：只生成一个 .c 文件，不调用 host cc。
 * 不解析 import / clux.mod；当前只支持单文件源码。
 */
int cbackend_generate_c(const char *src_path, const char *c_path);
/**
 * 测试运行器：把顶层 `test "Suite.Name" { ... }` 块改写成无参 void 函数，
 * 生成按 `argv[1]`（用例下标）选择并执行单个用例的 C 可执行文件。
 * 每个用例由调用方以子进程方式运行，用退出码判定通过/失败——这样单个用例
 * panic 不会影响其余用例。
 *
 * 成功时 `*names` / `*count` 返回 malloc 的显示名数组（调用方用
 * cbackend_free_test_names 释放）；没有 test 块时报错返回 1。
 */
int cbackend_build_tests(const char *src_path, const char *out_path,
                         char ***names, size_t *count, bool keep_c);
void cbackend_free_test_names(char **names, size_t count);

#ifdef __cplusplus
}
#endif
#endif
