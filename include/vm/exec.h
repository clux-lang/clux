#ifndef _H_CLUX_VM_EXEC_
#define _H_CLUX_VM_EXEC_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/vm.h"
#include "vm/bcode.h"

/**
 * 字节码执行器
 *
 * 执行器上下文直接复用 vm_t（见 vm.h 执行器状态区）：操作数栈（stack）、
 * 当前模块（bc）、指令指针（pc）、停止标志（halted）都是 vm 的字段，
 * 不另建 exec_t。结果由 exec_run 直接返回（生命周期挂在 current_scope
 * 下，调用方不手动释放），无需 result 字段。
 *
 * 主循环（exec_run）只移动 opcode 部分：读 u32 opcode → pc += 4 → 按
 * opcode 分派回调。操作数由各指令回调自己消费、自己推进 *pc。
 *
 * 统一出口只处理 error（引擎级硬错误 → 停止）；interrupt 哨兵（RET）
 * 由 bcode_call_cfunc 函数执行子循环捕获，主循环不消费。
 *
 * 未初始化（TDZ）检查由 sema 确定性赋值分析在编译期完成；运行时
 * var x:T = undefined 只存零值占位，VM 值层不感知未初始化状态。
 */

/** 指令回调签名：消费操作数推进 *pc，返回结果 value（或 NULL 不压栈）。
 *  返回值会被压入操作数栈（借用引用，不转移所有权）。 */
typedef value_t *(*bcode_handler_t)(vm_t *vm, bytecode_t *bc, size_t *pc);

/**
 * 驱动循环：从 start_pc 执行指令直到 HALT（返回 NULL）/ error（返回 error）
 * / interrupt（返回 interrupt）。返回值（非 NULL）已压入操作数栈。
 * 主循环（exec_run）与函数执行子循环（bcode_call_cfunc）共用；
 * 子循环调用时保存/恢复 vm->pc 与 vm->halted。
 */
value_t *exec_drive(vm_t *vm, bytecode_t *bc, size_t start_pc);

/**
 * 执行字节码模块直到 HALT 或 error。
 * 执行前清空操作数栈；结束后返回 error（若出错）或 NULL（正常 HALT）。
 * 返回的 error 生命周期挂在 current_scope 下，调用方不手动释放。
 *
 * clux 无顶层语句：exec_run 只执行类型提升区 + 函数注册段（类型构造 +
 * 函数值构造 + DEFINE_FUNCTION），不执行任何函数体。入口函数（main）由
 * 调用方在 exec_run 之后 scope_lookup + value_call 显式触发。
 */
value_t *exec_run(vm_t *vm, bytecode_t *bc);

/** 执行器调试：将操作数栈顶引用压入 offset 深度的借用引用（PUSH_VALUE 用） */
value_t *exec_stack_peek(const vm_t *vm, size_t offset);

/** 弹出一个操作数栈引用（不释放，归 scope） */
value_t *exec_stack_pop(vm_t *vm);

/** 压入一个借用引用 */
void exec_stack_push(vm_t *vm, value_t *v);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_EXEC_ */
