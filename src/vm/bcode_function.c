#include "vm/bcode_function.h"
#include "vm/vm.h"
#include "vm/scope.h"
#include "vm/exec.h"
#include "vm/value.h"
#include "vm/type_interrupt.h"
#include "core/panic.h"
#include "core/vec.h"

#include <string.h>

static class_t g_bcode_function_class = {
    .name       = "clux.vm.bcode_function",
    .size       = sizeof(bcode_function_t),
    .clone_fn   = NULL,
    .move_fn    = NULL,
    .dispose_fn = NULL,
};

value_t *bcode_function_new(vm_t *vm, const type_t *sig_type,
                            uint32_t entry_pc,
                            scope_t *root_scope) {
    if (!vm || !vm->alloc || !sig_type) return NULL;
    bcode_function_t *fn = (bcode_function_t *)allocator_new(
        vm->alloc, &g_bcode_function_class, 1);
    if (!fn) panic("vm: out of memory allocating bcode_function");
    memset(fn, 0, sizeof(bcode_function_t));
    fn->base.cfunc      = bcode_call_cfunc;
    fn->base.root_scope = root_scope;
    /* id 默认 0（未登记）：由 BIND_FUNC <id> 运行期填充并登记 */
    /* 孤立闭包作用域（parent=NULL）：不挂在任何 scope 树下，不随定义点
       作用域销毁。clux 显式闭包捕获——调用期间由 func_vcall 临时接线
       closure_scope->parent = root_scope 使函数体可查看到模块变量，调用
       结束恢复 NULL。生命周期随函数对象（vm->functions）统一销毁。 */
    fn->base.closure_scope     = scope_new(vm->alloc, NULL);
    fn->base.owns_closure_scope = true;
    fn->entry_pc = entry_pc;

    /* 函数对象注册进 vm->functions（vm 统一释放，value 共享指针不 double free） */
    if (vm->functions) vec_push(vm->functions, vm->alloc, fn);

    /* 包装为 func value：data 存 bcode_function_t*，type 即签名类型。
       auto-track 到当前作用域（函数是一等值：既经统一 push_undefined + DEFINE
       注册命名，也可作为普通值参与 DEFINE——匿名函数表达式 var add = func(){}，
       track 壳由 scope 统一回收，DEFINE 系指令不手动释放原值） */
    void *data = value_alloc_data_copy(vm->alloc, sig_type, &fn);
    return value_make(vm, sig_type, data);
}

value_t *bcode_call_cfunc(vm_t *vm, func_t *self, size_t argc, value_t **args) {
    bcode_function_t *bfn = (bcode_function_t *)self;

    /* 1. 实参按序压操作数栈（a, b → 栈顶 b，函数体倒序 DEFINE 绑定） */
    for (size_t i = 0; i < argc; i++) exec_stack_push(vm, args[i]);

    /* 2. 保存现场，切到函数体入口驱动（bc 同一模块不变） */
    size_t saved_pc     = vm->pc;
    bool   saved_halted = vm->halted;
    vm->halted = false;
    value_t *r = exec_drive(vm, vm->bc, bfn->entry_pc);
    vm->pc     = saved_pc;
    vm->halted = saved_halted;

    /* 3. RET interrupt 哨兵：弹哨兵，栈顶即返回值（借用引用，func_vcall clone 回 caller） */
    if (value_is_interrupt(vm, r)) {
        exec_stack_pop(vm);
        return exec_stack_pop(vm);
    }
    /* error：弹掉 error 引用后传播（func_vcall 走 error 平衡路径） */
    if (value_is_error(vm, r)) {
        exec_stack_pop(vm);
        return r;
    }
    /* 函数体走完未 RET（编译错误）：无返回值 */
    return NULL;
}
