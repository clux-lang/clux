#include "vm/vm.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "vm/type_func.h"
#include "vm/value.h"
#include "vm/function.h"
#include "core/panic.h"
#include "core/vec.h"

extern void vm_init_builtins(vm_t *vm);
extern void vm_register_printf(vm_t *vm);

static class_t g_vm_class = {
    .name       = "clux.vm",
    .size       = sizeof(vm_t),
    .clone_fn   = NULL,
    .move_fn    = NULL,
    .dispose_fn = NULL,
};

/* ---- 基本类型以 type value 注册进 global scope（LOAD "i32" 按名查） ---- */

#include <stddef.h> /* offsetof */

typedef struct {
    const char *name;
    size_t      slot_off; /* vm_t 中对应 type_t* 字段的偏移（编译期常量） */
    uint32_t    id;       /* 类型全局唯一 id（固定，LOAD_TYPE 内建段） */
} builtin_type_entry_t;

static void vm_register_builtin_types(vm_t *vm) {
    static const builtin_type_entry_t entries[] = {
        { "i8",   offsetof(vm_t, type_i8),    0 },
        { "i16",  offsetof(vm_t, type_i16),   1 },
        { "i32",  offsetof(vm_t, type_i32),   2 },
        { "i64",  offsetof(vm_t, type_i64),   3 },
        { "u8",   offsetof(vm_t, type_u8),    4 },
        { "u16",  offsetof(vm_t, type_u16),   5 },
        { "u32",  offsetof(vm_t, type_u32),   6 },
        { "u64",  offsetof(vm_t, type_u64),   7 },
        { "f32",  offsetof(vm_t, type_f32),   8 },
        { "f64",  offsetof(vm_t, type_f64),   9 },
        { "bool", offsetof(vm_t, type_bool), 10 },
        { "str",  offsetof(vm_t, type_str),  11 },
        { "void", offsetof(vm_t, type_void), 12 },
        { "type", offsetof(vm_t, type_type), 13 },
        { "func", offsetof(vm_t, type_func), 14 },
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        const type_t *t = *(type_t **)((char *)vm + entries[i].slot_off);

        /* 登记进类型 id 表（LOAD_TYPE 内建段直接查表，无需运行期构造） */
        vm_type_bind(vm, entries[i].id, t);

        void *data = value_alloc_data_copy(vm->alloc, vm->type_type, &t);
        value_t *tv = value_make_untracked(vm->alloc, vm->type_type, data);
        value_t *stored = scope_define(vm, vm->global_scope, entries[i].name, tv);
        if (!stored || value_is_error(vm, stored)) {
            panic("vm: failed to register builtin type '%s'", entries[i].name);
        }
        value_dispose(vm, tv);
        allocator_free(vm->alloc, (void **)&tv);
    }
}

vm_t *vm_new(allocator_t *alloc) {
    if (!alloc) return NULL;

    vm_t *vm = (vm_t *)allocator_new(alloc, &g_vm_class, 1);
    if (!vm) panic("vm: out of memory allocating vm");

    vm->alloc = alloc;

    vm_init_builtins(vm);

    /* global_scope -> root_scope(module) -> current_scope */
    vm->global_scope  = scope_new(alloc, NULL);
    vm->root_scope    = scope_new(alloc, vm->global_scope);
    vm->current_scope = vm->root_scope;

    /* 执行器操作数栈：借用引用，不拥有 value */
    vm->stack = vec_new(alloc, /*owns_element=*/false);

    /* 函数对象池：func_t*，不 owns 元素，vm_destroy 手动释放
       （必须先于 func_new 使用——内置函数注册依赖池存在） */
    vm->functions = vec_new(alloc, /*owns_element=*/false);

    /* const/volatile 修饰类型池（type_const_intern / type_volatile_intern
       intern 用；元素由 vm_destroy 手动释放，vec 只持有指针数组） */
    vm->const_types = vec_new(alloc, /*owns_element=*/false);
    vm->volatile_types = vec_new(alloc, /*owns_element=*/false);

    /* 类型 id 表（id → type_t*，索引即 id；元素不 owns，归各类型池释放）。
       初始容量预留内建段（0..16），程序类型 id 从 64 起由编译器分配，
       BIND_TYPE 动态扩容登记。 */
    vm->types_by_id = vec_new(alloc, /*owns_element=*/false);

    /* 基本类型注册进 global scope（LOAD 指令按名查 type value） */
    vm_register_builtin_types(vm);

    /* error/interrupt 也登记进类型 id 表（id 15/16，内建段） */
    vm_type_bind(vm, 15, vm->type_error);
    vm_type_bind(vm, 16, vm->type_interrupt);

    /* printf 内置函数（临时注册，M1 硬编码绑定 C printf） */
    vm_register_printf(vm);

    return vm;
}

void vm_destroy(vm_t **pvm) {
    if (!pvm || !*pvm) return;
    vm_t *vm = *pvm;

    /* 销毁作用域链：root_scope 是 global 的子作用域 */
    /* 先销毁 root_scope 以下的所有作用域（current_scope 可能在 root 之下） */
    /* 逐层 pop 直到 root_scope，再销毁 root_scope，再销毁 global_scope */
    while (vm->current_scope && vm->current_scope != vm->root_scope) {
        vm_pop_scope(vm);
    }
    scope_destroy(vm, &vm->root_scope);
    scope_destroy(vm, &vm->global_scope);

    vm->current_scope = NULL;

    /* 执行器操作数栈（借用引用，不拥有，仅释放向量结构） */
    vec_free(vm->alloc, &vm->stack);

    /* 函数对象池：统一释放 func_t 及其自建的孤立 closure_scope
       （签名归 sig_types 池，不在此释放） */
    if (vm->functions) {
        size_t n = vec_len(vm->functions);
        for (size_t i = 0; i < n; i++) {
            func_t *fn = (func_t *)vec_get(vm->functions, i);
            if (!fn) continue;
            if (fn->owns_closure_scope && fn->closure_scope) {
                scope_destroy(vm, &fn->closure_scope);
            }
            func_destroy(vm->alloc, &fn);
        }
        vec_free(vm->alloc, &vm->functions);
    }

    /* 函数签名类型池：单遍释放（M1 签名只引用内置静态类型，无相互依赖） */
    if (vm->sig_types) {
        size_t n = vec_len(vm->sig_types);
        for (size_t i = 0; i < n; i++) {
            func_type_t *ft = (func_type_t *)vec_get(vm->sig_types, i);
            if (!ft) continue;
            if (ft->sig.params) allocator_free(vm->alloc, (void **)&ft->sig.params);
            if (ft->base.name.ptr) {
                char *np = (char *)ft->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&ft);
        }
        vec_free(vm->alloc, &vm->sig_types);
    }

    /* const 修饰类型池：释放 name + 结构体（sub 归底层类型，不在此释放） */
    if (vm->const_types) {
        size_t n = vec_len(vm->const_types);
        for (size_t i = 0; i < n; i++) {
            const_type_t *ct = (const_type_t *)vec_get(vm->const_types, i);
            if (!ct) continue;
            if (ct->base.name.ptr) {
                char *np = (char *)ct->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&ct);
        }
        vec_free(vm->alloc, &vm->const_types);
    }

    /* volatile 修饰类型池：同上 */
    if (vm->volatile_types) {
        size_t n = vec_len(vm->volatile_types);
        for (size_t i = 0; i < n; i++) {
            volatile_type_t *vt = (volatile_type_t *)vec_get(vm->volatile_types, i);
            if (!vt) continue;
            if (vt->base.name.ptr) {
                char *np = (char *)vt->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&vt);
        }
        vec_free(vm->alloc, &vm->volatile_types);
    }

    /* 数组类型池：释放 name + 结构体（elem_type 归底层类型，不在此释放） */
    if (vm->array_types) {
        size_t n = vec_len(vm->array_types);
        for (size_t i = 0; i < n; i++) {
            array_type_t *at = (array_type_t *)vec_get(vm->array_types, i);
            if (!at) continue;
            if (at->base.name.ptr) {
                char *np = (char *)at->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&at);
        }
        vec_free(vm->alloc, &vm->array_types);
    }

    /* 类型 id 表：元素归各类型池，仅释放向量结构 */
    vec_free(vm->alloc, &vm->types_by_id);

    allocator_free(vm->alloc, (void **)pvm);
}

void vm_push_scope(vm_t *vm) {
    if (!vm) return;
    scope_t *child = scope_new(vm->alloc, vm->current_scope);
    vm->current_scope = child;
}

void vm_pop_scope(vm_t *vm) {
    if (!vm || !vm->current_scope) return;
    if (vm->current_scope == vm->global_scope) {
        panic("vm: cannot pop the global scope");
    }
    scope_t *parent = scope_parent(vm->current_scope);
    scope_destroy(vm, &vm->current_scope);
    vm->current_scope = parent;
}
