#ifndef _H_CLUX_VM_FUNCTION_
#define _H_CLUX_VM_FUNCTION_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/value.h"
#include "core/strslice.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct vm_t    vm_t;
typedef struct scope_t scope_t;
typedef struct func_t  func_t;

/* ---- C 函数签名 ---- */

/**
 * 引擎侧函数原型：所有 clux 函数最终以 C 函数形式执行。
 * - vm: 虚拟机上下文
 * - self: 指向 func_t 自身
 * - argc: 实参数量
 * - args: 实参数组（已 clone 到当前作用域）
 * 返回值: 结果 value（调用方负责 clone 到自己的作用域）
 */
typedef value_t *(*cfunc_t)(vm_t *vm, func_t *self, size_t argc, value_t **args);

/* ---- func_t: 函数对象 ---- */

/**
 * func_t: 函数对象（VM 引擎侧）
 *
 * - cfunc: C 函数指针，引擎通过它执行逻辑
 * - closure_scope: 定义时的词法环境（闭包捕获）
 * - root_scope: 定义时的模块作用域（全局根的子作用域）
 * - id: 函数全局唯一 id（内建函数由 vm 自动分配 < FUNC_ID_PROGRAM_BASE；
 *   程序函数由编译器分配 >= FUNC_ID_PROGRAM_BASE，PUSH_FUNCTION 立即数
 *   写入，BIND_FUNC 登记进 vm->functions_by_id）
 * - name: 函数名（调试/显示用）。内建函数为静态字面量（owns_name=false）；
 *   程序函数经 SET_FUNC_NAME 拷贝到 vm 堆（owns_name=true，func_destroy 释放）
 *
 * 签名不存于 func_t：签名类型（func_type_t）由 func_new 传入并成为
 * func value 的 type，调用点经 value_type() 取回（见 func_new）。
 * sema 侧不构造 func value：符号表只记录函数定义 AST 节点（sym->ast），
 * 签名类型存于 sym->type。
 *
 * 引擎视角的参数没有名字，cfunc 通过 args[i] 按位置访问。
 * 参数名绑定是语言层（AST interpreter）的职责。
 */
struct func_t {
    cfunc_t         cfunc;
    scope_t        *closure_scope;
    scope_t        *root_scope;
    uint32_t        id;
    strslice_t      name;
    bool            owns_closure_scope; /* true：closure_scope 由函数对象创建（bcode_function），
                                           随 vm->functions 释放；false：调用方传入（不拥有） */
    bool            owns_name;          /* true：name 为 SET_FUNC_NAME 堆拷贝，func_destroy 释放 */
};

/* ---- 函数 id 分段（function.c / compile_func.c 共用） ---- */
/* 内建函数固定 id 0..(FUNC_ID_BUILTIN_COUNT-1)（vm 自动分配：func_new 内
   部从 0 起递增，printf=0）；程序函数 id 由编译器分配，从 FUNC_ID_PROGRAM_BASE
   起（预留扩展空隙，见 vm.h 注释）。 */
#define FUNC_ID_BUILTIN_COUNT 1u
#define FUNC_ID_PROGRAM_BASE  64u

/* ---- 函数 id 表（id → func_t*，索引即 id） ---- */

/**
 * 登记函数到 id 表（vm->functions_by_id，索引即 id）。BIND_FUNC <id> 运行期
 * 用；幂等——同一函数重复登记（多 id 别名）无害。
 * Panics on out-of-memory.
 */
void vm_func_bind(vm_t *vm, uint32_t id, func_t *fn);

/**
 * 按 id 查函数：id 越界或未登记返回 NULL。调用方无需手动释放（func_t 归
 * vm->functions 池）。
 */
func_t *vm_func_load(vm_t *vm, uint32_t id);

/* ---- 生命周期 ---- */

/**
 * 创建函数对象并包装为 func value。
 *
 * - vm: 虚拟机上下文（alloc 取自 vm->alloc；func_t 注册进 vm->functions，
 *   由 vm_destroy 统一释放生命周期）
 * - sig_type: 签名类型（func_type_t，由 type_func_sig 注册 intern）。
 *   成为 func value 的 type：调用点经 value_type() 取回签名，
 *   func_shadow_call 据此做参数数量/隐式转换校验。不可为 NULL。
 * - 返回 value_t*：data 存指向 func_t 的指针，type 即 sig_type。
 *   untracked（调用方管理生命周期）：value_dispose(vm, v) 释放 data 块
 *   与 value_t 结构体；func_t 本体归 vm->functions，由 vm_destroy 释放。
 */
value_t *func_new(vm_t *vm,
                  cfunc_t cfunc,
                  scope_t *closure_scope,
                  scope_t *root_scope,
                  const type_t *sig_type,
                  strslice_t name);

/**
 * 销毁函数对象（释放 func_t 自身；签名归 vm 类型池所有）。
 * 仅供 vm_destroy 遍历 vm->functions 时调用，普通代码不应直接使用
 * （函数对象生命周期由 vm 统一管理）。
 */
void func_destroy(allocator_t *alloc, func_t **fn);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_FUNCTION_ */
