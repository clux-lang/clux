#include "vm/function.h"
#include "vm/type_ptr.h"
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/ffi_lib.h"   /* M6 Phase 5：函数对象持有的 C 回调闭包 */
#include "core/panic.h"
#include "core/vec.h"

#include <string.h>

static class_t g_func_class = {
    .name       = "clux.vm.func",
    .size       = sizeof(func_t),
    .clone_fn   = NULL,
    .move_fn    = NULL,
    .dispose_fn = NULL,
};

/* ---- 生命周期 ---- */

value_t *func_new(vm_t *vm,
                  cfunc_t cfunc,
                  scope_t *closure_scope,
                  scope_t *root_scope,
                  const type_t *sig_type,
                  strslice_t name) {
    if (!vm || !vm->alloc || !sig_type) return NULL;
    func_t *fn = (func_t *)allocator_new(vm->alloc, &g_func_class, 1);
    if (!fn) panic("vm: out of memory allocating func");
    memset(fn, 0, sizeof(func_t));
    fn->cfunc         = cfunc;
    fn->closure_scope = closure_scope;
    fn->root_scope    = root_scope;
    fn->type          = sig_type;
    fn->name          = name;

    /* 内建函数 id：从 0 起递增（内建段 < FUNC_ID_PROGRAM_BASE）。注册时机
       保证唯一——func_new 仅在 vm 初始化阶段（vm_register_*）调用，此时
       next_builtin_func_id 尚未接触程序函数 id 段。 */
    fn->id = vm->next_builtin_func_id++;
    if (fn->id >= FUNC_ID_PROGRAM_BASE)
        panic("vm: builtin function id overflow");

    /* 函数对象注册进 vm->functions（vm 统一释放，value 共享指针不 double free） */
    if (vm->functions) vec_push(vm->functions, vm->alloc, fn);

    /* 登记进 id 表（内建段 0..N-1 立即可查） */
    vm_func_bind(vm, fn->id, fn);

    /* 包装为 func value：data 存 func_t*，type 即签名类型。
       **登记进 global scope**（内建函数值随 VM 存活，不属于任何调用帧）——
       引擎对象同样不得孤立于 scope：作用域退出/VM 销毁统一释放。
       （此前 value_alloc_shell + 调用方手工 dispose/free 是唯一例外，
       本次所有权重构已消除。） */
    void *data = value_alloc_data_copy(vm->alloc, sig_type, &fn);
    scope_t *saved_scope = vm->current_scope;
    if (vm->global_scope) vm->current_scope = vm->global_scope;
    value_t *fv = value_make(vm, sig_type, data);
    vm->current_scope = saved_scope;
    return fv;
}

/* ---- 函数 id 表（id → func_t*，索引即 id） ---- */

/* BIND_FUNC 登记：扩容至 id+1 后写入。重复登记幂等（同一实例多 id 别名）。
   空洞槽位（内建段与程序段之间 1..63）以 NULL 填充——vec_push 拒绝 NULL
   值（no-op），故用 vec_resize 扩展长度填充。 */
void vm_func_bind(vm_t *vm, uint32_t id, func_t *fn) {
    if (!vm || !fn) return;
    if (vec_len(vm->functions_by_id) <= id) {
        vec_resize(vm->functions_by_id, vm->alloc, (size_t)id + 1);
    }
    vec_set(vm->functions_by_id, id, fn);
}

/* 按 id 查函数：id 越界或未登记返回 NULL。 */
func_t *vm_func_load(vm_t *vm, uint32_t id) {
    if (!vm || id >= vec_len(vm->functions_by_id)) return NULL;
    return (func_t *)vec_get(vm->functions_by_id, id);
}

/* CTFE 函数引用对象：轻量 func_t（cfunc=NULL、不登记 functions_by_id），
   仅签名 + 名字 + id，注册进 vm->functions 统一释放（func_destroy 见
   owns_name 处理：借用名字不释放）。见 function.h 注释。 */
func_t *func_new_program_ref(vm_t *vm, const type_t *sig_type,
                             strslice_t name, uint32_t id) {
    if (!vm || !vm->alloc || !sig_type) return NULL;
    func_t *fn = (func_t *)allocator_new(vm->alloc, &g_func_class, 1);
    if (!fn) panic("vm: out of memory allocating func ref");
    memset(fn, 0, sizeof(func_t));
    fn->type = sig_type;
    fn->name = name; /* 借用，owns_name=false */
    fn->id   = id;   /* sema 分配的 fid / 内建 id，编码折叠用 */
    if (vm->functions) vec_push(vm->functions, vm->alloc, fn);
    return fn;
}

void func_destroy(allocator_t *alloc, func_t **pfn) {
    if (!pfn || !*pfn) return;
    func_t *fn = *pfn;
    /* SET_FUNC_NAME 堆拷贝的名字（程序函数）随对象释放 */
    if (fn->owns_name && fn->name.ptr) {
        char *np = (char *)fn->name.ptr;
        allocator_free(alloc, (void **)&np);
        fn->name = (strslice_t){ NULL, 0 };
    }
    /* M6 FFI Phase 5：`cfunc_ptr(f)` 惰性创建的 C 回调闭包随函数对象释放。
       libffi 的 trampoline 不在本 allocator 记账内，**必须**显式销毁，否则
       静默泄漏可执行内存（见 docs/m6-design.md §14.14）。ctx 由本 allocator
       分配，同样在此释放。 */
    if (fn->ffi_closure) {
        clux_ffi_closure_destroy(alloc, fn->ffi_closure);
        fn->ffi_closure = NULL;
    }
    if (fn->ffi_closure_ctx) {
        allocator_free(alloc, &fn->ffi_closure_ctx);
    }
    fn->ffi_closure_code = NULL;
    /* 闭包 env 块（捕获结构体实例）：先按 capture_type 的槽位所有权清理残留
       （sema 生成的 `FREE f.<cap>` 已把 own 捕获槽置 NULL，这里只兜底），
       再释放块本体。 */
    if (fn->env) {
        /* env 块的**内嵌 own 槽位**由 sema 生成的 FREE 负责；这里只兜底释放
           env 块本体（函数对象销毁时其 data 已不可达）。内联 env 属于函数
           对象分配块，随下面的 allocator_free(pfn) 一起释放。 */
        if (!fn->env_inline) allocator_free(alloc, &fn->env);
        else fn->env = NULL;
    }
    /* 签名信息归 vm 类型池所有，func_t 不释放 sig_type */
    allocator_free(alloc, (void **)pfn);
}

value_t *func_value_ref(vm_t *vm, func_t *fn) {
    if (!vm || !fn || !fn->type) return NULL;
    void *data = value_alloc_data_copy(vm->alloc, fn->type, &fn);
    if (!data) return NULL;
    return value_make(vm, fn->type, data);
}
