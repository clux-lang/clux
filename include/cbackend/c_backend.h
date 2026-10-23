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

#ifdef __cplusplus
}
#endif
#endif
