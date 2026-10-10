#ifndef _H_CLUX_DRIVER_COMPILE_MODULE_
#define _H_CLUX_DRIVER_COMPILE_MODULE_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/vm.h"
#include "vm/module.h"
#include "core/allocator.h"
#include "core/arena.h"
#include "core/vec.h"
#include "diag/diagnostic.h"

/**
 * compile_module: 模块编译统一入口（M5 模块系统核心）
 *
 * 每个模块独立走完整流水线：lex → parse → sema → compile → 运行时注册段执行。
 * 产出 module_t（bc + global_scope + exports + canonical），注册到 vm->modules。
 *
 * sema 遇到 import 语句时递归调用 compile_module（同步递归），用 path_stack
 * 检测循环依赖。
 *
 * @param vm          共享 VM（全局表 types/functions/strs 跨模块统一）
 * @param alloc       分配器（与 vm 共享）
 * @param diag        诊断缓冲区（跨模块共享，错误统一收集）
 * @param arena       AST arena（借用，caller 拥有）
 * @param canonical   模块规范路径（作为 vm->modules 的 key）
 * @param path_stack  编译路径栈（vec_t<const char*>，循环依赖检测）
 * @return module_t*  成功返回 module（已注册到 vm->modules），失败返回 NULL（诊断已记录）
 */
module_t *compile_module(vm_t *vm, allocator_t *alloc, diag_buf_t *diag,
                         arena_t *arena, const char *canonical,
                         vec_t *path_stack);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_DRIVER_COMPILE_MODULE_ */
