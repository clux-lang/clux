#include "vm/function.h"
#include "vm/vm.h"
#include "vm/value.h"
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

    /* 包装为 func value：data 存 func_t*，type 即签名类型 */
    void *data = value_alloc_data_copy(vm->alloc, sig_type, &fn);
    return value_make_untracked(vm->alloc, sig_type, data);
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

/* CTFE 函数引用对象：轻量 func_t（cfunc=NULL、无 id/closure_scope），
   仅签名 + 名字，注册进 vm->functions 统一释放（func_destroy 见 owns_name
   处理：借用名字不释放）。见 function.h 注释。 */
func_t *func_new_program_ref(vm_t *vm, const type_t *sig_type, strslice_t name) {
    if (!vm || !vm->alloc || !sig_type) return NULL;
    func_t *fn = (func_t *)allocator_new(vm->alloc, &g_func_class, 1);
    if (!fn) panic("vm: out of memory allocating func ref");
    memset(fn, 0, sizeof(func_t));
    fn->type = sig_type;
    fn->name = name; /* 借用，owns_name=false */
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
    /* 签名信息归 vm 类型池所有，func_t 不释放 sig_type */
    allocator_free(alloc, (void **)pfn);
}
