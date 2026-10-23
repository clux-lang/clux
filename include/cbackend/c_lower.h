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
 * 第一阶段只接受一个无参 main，返回 i32 或 void。
 * 失败返回 NULL，诊断写入 sema->diag。
 */
cir_unit_t *cbackend_lower(arena_t *arena, sema_t *sema,
                           ast_program_t *prog, const char *source_path);

#ifdef __cplusplus
}
#endif
#endif
