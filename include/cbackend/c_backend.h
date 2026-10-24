#ifndef _H_CLUX_CBACKEND_C_BACKEND_
#define _H_CLUX_CBACKEND_C_BACKEND_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 单文件 C 后端：lex → parse → desugar → sema → C IR → C 源文件 → host cc。
 * 当前只支持无 import、无 clux.mod 的单文件入口。
 */
int cbackend_build_file(const char *src_path, const char *out_path);

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
                         char ***names, size_t *count);
void cbackend_free_test_names(char **names, size_t count);

#ifdef __cplusplus
}
#endif
#endif
