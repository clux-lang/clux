#ifndef _H_CLUX_CBACKEND_C_LOWER_
#define _H_CLUX_CBACKEND_C_LOWER_

#include "cbackend/c_ir.h"
#include "core/arena.h"
#include "sema/sema.h"
#include "parser/ast_program.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * sema 后的 clux 程序 → C IR。
 * 普通构建要求一个无参 main，返回 i32 或 void。
 * `test_mode`：测试运行器，顶层 test 块已被改写成无参 void 函数，
 * 允许没有 main（运行器入口由 writer 生成）。
 * 失败返回 NULL，诊断写入 sema->diag。
 */
cir_unit_t *cbackend_lower(arena_t *arena, sema_t *sema,
                           ast_program_t *prog, const char *source_path,
                           bool test_mode);

#ifdef __cplusplus
}
#endif
#endif
