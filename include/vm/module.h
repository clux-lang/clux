#ifndef _H_CLUX_VM_MODULE_
#define _H_CLUX_VM_MODULE_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"
#include "core/strmap.h"
#include <stdbool.h>

typedef struct vm_t       vm_t;
typedef struct scope_t    scope_t;
typedef struct bytecode_t bytecode_t;

/**
 * module_t: 编译期模块对象（M5 模块系统）
 *
 * 每个模块独立走完整流水线（lex → parse → sema → compile），产出三部分：
 *
 * - bc: 该模块的字节码（注册段在 PC=0，函数体在后）。自包含。
 * - global_scope: 模块全局作用域。运行期 DEFINE 把函数/var 绑定到这里。
 *   func_t.root_scope 指向所属 module 的 global_scope，跨模块调用时切换。
 * - exports: 导出表（name → value_t* 借用映射，指向 global_scope 中的符号）。
 *   GET_MEMBER 从这里取成员。非导出符号不在此表中，模块私有。
 * - canonical: 规范路径，作为 vm->modules 的 key（owned，vm_destroy 释放）。
 *
 * module 对象归 vm 拥有，vm_destroy 统一释放。sema 用完即销毁，产物转移给 module。
 */
typedef struct module_t {
    bytecode_t *bc;             /* 模块字节码（owned） */
    scope_t    *global_scope;   /* 模块全局作用域（owned） */
    strmap_t   *exports;        /* name → value_t* 借用映射（owned map，借用 value） */
    char       *canonical;      /* 规范路径（owned，NUL 终止） */
} module_t;

/**
 * 创建 module 对象。canonical 路径字符串被拷贝（module 拥有副本）。
 * bc 和 global_scope 的所有权转移给 module。
 * exports 由调用方填充（sema 构建导出表后传入）。
 */
module_t *module_new(allocator_t *alloc, bytecode_t *bc,
                     scope_t *global_scope, strmap_t *exports,
                     const char *canonical);

/**
 * 销毁 module 对象：释放 bc、global_scope、exports map、canonical 字符串。
 */
void module_destroy(vm_t *vm, module_t **mod);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_MODULE_ */
