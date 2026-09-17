#ifndef _H_CLUX_VM_BCODE_FUNCTION_
#define _H_CLUX_VM_BCODE_FUNCTION_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/function.h"
#include "vm/value.h"
#include "vm/type.h"
#include "core/allocator.h"
#include <stdint.h>

typedef struct vm_t    vm_t;
typedef struct scope_t scope_t;

/**
 * bcode_function_t: 字节码函数对象（func_t 的子类）
 *
 * 由 BCODE_PUSH_FUNCTION 执行时构造（非编译期预建表）：
 * - base.cfunc = bcode_call_cfunc：驱动字节码体执行的引擎回调
 * - base.closure_scope：定义时从 root_scope 创建的孤立作用域
 *   （clux 显式闭包捕获：不自动捕获定义点块作用域变量，只持有
 *   显式捕获的变量；parent=root_scope 使函数体可查看到模块变量）
 * - base.root_scope：定义时的模块作用域
 * - base.id：默认 0；BIND_FUNC <id> 运行期填充（编译器分配的全局唯一 id，
 *   程序段 >= FUNC_ID_PROGRAM_BASE）并登记进 vm->functions_by_id
 * - entry_pc：函数体字节码入口（绝对偏移，函数体内倒序 DEFINE 绑参）
 *
 * 名字由 SET_FUNC_NAME 写入（调试/显示用）：scope 注册用 DEFINE 的
 * strtable 名。函数对象本体注册进 vm->functions（vm 统一释放生命周期）。
 */
typedef struct bcode_function_t {
    func_t   base;
    uint32_t entry_pc;
} bcode_function_t;

/**
 * 创建字节码函数对象并包装为 func value。
 *
 * - sig_type: 签名类型（CREATE_FUNC_TYPE 产物），不可为 NULL
 * - entry_pc: 函数体字节码入口（绝对偏移）
 * - root_scope: 定义时的模块作用域；closure_scope 由内部创建
 *   （scope_new(alloc, root_scope)，孤立于定义点 current_scope）
 *   id 默认 0，由后续 BIND_FUNC <id> 填充
 * - 返回 value_t*：data 存 bcode_function_t*，type 即 sig_type；
 *   untracked（调用方管理）：value_dispose(vm, v) 释放 data 块与 value_t，
 *   函数对象本体归 vm->functions，由 vm_destroy 统一释放。
 */
value_t *bcode_function_new(vm_t *vm, const type_t *sig_type,
                            uint32_t entry_pc,
                            scope_t *root_scope);

/** 字节码函数执行回调（cfunc_t）：驱动函数体字节码，捕获 RET interrupt 哨兵 */
value_t *bcode_call_cfunc(vm_t *vm, func_t *self, size_t argc, value_t **args);

/**
 * 按基底函数实例化新函数实例（函数定义点每次求值调用）。
 *
 * 函数定义（字面量/局部函数）每次求值都应生成独立实例：共享基底的
 * entry_pc/cfunc/type/id/name，但 closure_scope 独立（复制基底捕获槽名 +
 * undefined 占位，定义点 SET_CLOSURE 绑定各自捕获值）——循环内
 * `fns[i] = func|(val=items[i])|...` 三个槽位指向三个独立对象，
 * 捕获互不干扰（对标 LOAD_FUNCTION 的"同一对象"引用语义）。
 *
 * - base: 基底函数（hoist 区 PUSH_FUNCTION 构造，functions_by_id 登记）
 * - 返回 value_t*：data 存新 bcode_function_t*，type 即签名类型；
 *   untracked（调用方管理）：value_dispose(vm, v) 释放 data 块与 value_t，
 *   函数实例本体注册进 vm->functions，由 vm_destroy 统一释放。
 *   名字借用基底（owns_name=false），不重复拥有。
 */
value_t *func_instantiate(vm_t *vm, func_t *base);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_BCODE_FUNCTION_ */
