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
                            scope_t *root_scope,
                            bytecode_t *bc) {
    if (!vm || !vm->alloc || !sig_type) return NULL;
    bcode_function_t *fn = (bcode_function_t *)allocator_new(
        vm->alloc, &g_bcode_function_class, 1);
    if (!fn) panic("vm: out of memory allocating bcode_function");
    memset(fn, 0, sizeof(bcode_function_t));
    fn->base.cfunc      = bcode_call_cfunc;
    fn->base.root_scope = root_scope;
    fn->base.type       = sig_type; /* 签名类型：LOAD_FUNCTION 从 func_t 取签名包装 value */
    /* id 默认 0（未登记）：由 BIND_FUNC <id> 运行期填充并登记 */
    /* 孤立闭包作用域（parent=NULL）：不挂在任何 scope 树下，不随定义点
       作用域销毁。clux 显式闭包捕获——调用期间由 func_vcall 临时接线
       closure_scope->parent = root_scope 使函数体可查看到模块变量，调用
       结束恢复 NULL。生命周期随函数对象（vm->functions）统一销毁。 */
    fn->base.closure_scope     = scope_new(vm->alloc, NULL);
    fn->base.owns_closure_scope = true;
    fn->entry_pc = entry_pc;
    fn->bc       = bc;

    /* 函数对象注册进 vm->functions（vm 统一释放，value 共享指针不 double free） */
    if (vm->functions) vec_push(vm->functions, vm->alloc, fn);

    /* 包装为 func value：data 存 bcode_function_t*，type 即签名类型。
       auto-track 到当前作用域（函数是一等值：既经统一 push_undefined + DEFINE
       注册命名，也可作为普通值参与 DEFINE——匿名函数表达式 var add = func(){}，
       track 壳由 scope 统一回收，DEFINE 系指令不手动释放原值） */
    void *data = value_alloc_data_copy(vm->alloc, sig_type, &fn);
    return value_make(vm, sig_type, data);
}

/* 按基底实例化新函数实例：共享 entry_pc/cfunc/type/id/name，closure_scope
   独立（复制基底捕获槽名 + undefined 占位，定义点 SET_CLOSURE 逐个替换）。
   名字借用基底（owns_name=false）——基底 owns_name=true（SET_FUNC_NAME 堆
   拷贝）由 func_destroy 随基底释放，新实例共享 strslice 指针不释放。 */
func_t *func_instantiate_raw(vm_t *vm, func_t *base) {
    if (!vm || !base || !base->type) return NULL;
    bcode_function_t *fn = (bcode_function_t *)allocator_new(
        vm->alloc, &g_bcode_function_class, 1);
    if (!fn) panic("vm: out of memory instantiating bcode_function");
    memset(fn, 0, sizeof(bcode_function_t));
    fn->base.cfunc       = base->cfunc;
    fn->base.root_scope  = base->root_scope;
    fn->base.type        = base->type;
    fn->base.id          = base->id;   /* 共享基底 id（不重复登记 functions_by_id） */
    fn->base.name        = base->name; /* 借用基底名字 */
    fn->base.capture_type = base->capture_type;
    fn->base.env_dispose_fid = base->env_dispose_fid;
    fn->base.env_clone_fid   = base->env_clone_fid;
    fn->base.ct_literal      = base->ct_literal;
    if (base->capture_type && base->capture_type->base.size > 0) {
        void *env = allocator_new_ex(vm->alloc, "closure_env",
                                     base->capture_type->base.size, NULL, NULL, NULL,
                                     1);
        if (!env) panic("vm: out of memory allocating closure env");
        memset(env, 0, base->capture_type->base.size);
        fn->base.env = env;
    } /* 捕获聚合类型 */
    fn->base.closure_scope     = scope_new(vm->alloc, NULL);
    fn->base.owns_closure_scope = true;
    fn->entry_pc = (base->cfunc == bcode_call_cfunc)
                       ? ((bcode_function_t *)base)->entry_pc : 0;
    fn->bc       = (base->cfunc == bcode_call_cfunc)
                       ? ((bcode_function_t *)base)->bc : NULL;

    /* 复制基底捕获槽名 + undefined 占位（strmap_keys 只读 key 向量）。
       定义点 SET_CLOSURE 用真实捕获值替换（scope_set）——提升后定义点前
       调用 → 捕获槽 undefined（TDZ 语义，与 hoist 区占位一致）。 */
    /* 捕获不再放 scope：env 块（按 capture_type 分配）才是捕获存储，
       closure_scope 只作为调用期作用域链的挂点（parent = root_scope）。 */

    if (vm->functions) vec_push(vm->functions, vm->alloc, fn);
    return (func_t *)fn;
}
value_t *func_instantiate(vm_t *vm, func_t *base) {
    func_t *fn = func_instantiate_raw(vm, base);
    if (!fn) return NULL;
    /* 运行期值类型仍用**签名类型**（func_type_t，sig 与闭包类型共享布局）：
       闭包类型是 sema 的类型身份，运行期不需要它来读写 env（capture_type
       已经挂在函数对象上）。 */
    void *data = value_alloc_data_copy(vm->alloc, base->type, &fn);
    return value_make(vm, base->type, data);
}

value_t *bcode_call_cfunc(vm_t *vm, func_t *self, size_t argc, value_t **args) {
    bcode_function_t *bfn = (bcode_function_t *)self;

    /* 1. 实参按序压操作数栈（a, b → 栈顶 b，函数体倒序 DEFINE 绑定） */
    for (size_t i = 0; i < argc; i++) exec_stack_push(vm, args[i]);

    /* 2. 保存现场，切到函数体入口驱动。
       M5：用函数所属模块的 bc（bfn->bc），而非 vm->bc——跨模块调用时
       函数体在被导入模块的字节码中，必须切换到正确的 bcode。 */
    size_t      saved_pc     = vm->pc;
    bool        saved_halted = vm->halted;
    bytecode_t *saved_bc     = vm->bc;
    vm->halted = false;
    vm->bc     = bfn->bc;
    value_t *r = exec_drive(vm, bfn->bc, bfn->entry_pc);
    vm->pc     = saved_pc;
    vm->halted = saved_halted;
    vm->bc     = saved_bc;

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
