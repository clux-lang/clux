#ifndef _H_CLUX_SEMA_INTERNAL_
#define _H_CLUX_SEMA_INTERNAL_
/* 内部头：sema.c 拆分后的跨文件声明（sema 私有）。 */
#include "sema/sema.h"

#ifdef __cplusplus
extern "C" {
#endif


void pass0_imports(sema_t *sema, ast_program_t *prog);
void pass1_names(sema_t *sema, ast_program_t *prog);
void pass1b_types(sema_t *sema, ast_program_t *prog, bool speculative);
void pass1c_extern_abi(sema_t *sema, ast_program_t *prog);
void pass1d_struct_members(sema_t *sema, ast_program_t *prog);
void pass2_types(sema_t *sema, bool speculative);
void pass_globals(sema_t *sema, ast_program_t *prog, bool speculative);
/* 只求值全局 comptime var：支撑"comptime var → 装饰器 → 普通全局"的顺序。 */
void pass_comptime_vars(sema_t *sema, ast_program_t *prog, bool speculative);
size_t sema_speculative_round(sema_t *sema, ast_program_t *prog);

/* 守卫：被污染的值（错误已报告）→ 直接返回 void shadow。 */
#define EX_GUARD(sema, v)                                                     \
  do {                                                                        \
    if (is_poison((sema), (v))) return val_void((sema));                       \
  } while (0)

#ifdef __cplusplus
}
#endif
#endif
