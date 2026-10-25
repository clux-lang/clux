#include "vm/vm.h"
#include "sema/sema.h"
#include "vm/type.h"
#include "vm/type_module.h"
#include "vm/type_array.h"
#include "vm/type_func.h"
#include "vm/type_cfunc.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/str_pool.h"
#include "vm/value.h"
#include "vm/function.h"
#include "vm/scope.h"
#include "core/panic.h"
#include "core/vec.h"

extern void vm_init_builtins(vm_t *vm);
extern void vm_register_len(vm_t *vm);
extern void vm_register_deep_clone(vm_t *vm);
extern void vm_register_rc_builtins(vm_t *vm);

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
        value_t *tv = value_make(vm, vm->type_type, data);
        value_t *stored = scope_define(vm, vm->global_scope, entries[i].name, tv);
        if (!stored || value_is_error(vm, stored)) {
            panic("vm: failed to register builtin type '%s'", entries[i].name);
        }
    }

    /* `usize`（C 的 size_t）：**独立的 type_t**（id 18，见 vm_init_builtins），
       不是 u64/u32 的别名——typeof/type_equal/FFI 描述符都按自己的身份走。
       这里只把它登记进 id 表与 global scope（`LOAD_TYPE 18` / 名字查找）。 */
    {
        const type_t *t = vm_type_usize(vm);
        vm_type_bind(vm, (uint32_t)t->id, t);
        void *data = value_alloc_data_copy(vm->alloc, vm->type_type, &t);
        value_t *tv = value_make(vm, vm->type_type, data);
        value_t *stored = scope_define(vm, vm->global_scope, "usize", tv);
        if (!stored || value_is_error(vm, stored))
            panic("vm: failed to register builtin type 'usize'");
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

    /* 函数 id 表（id → func_t*，索引即 id；元素不 owns，归 functions 池释放）。
       初始容量预留内建段，程序函数 id 从 64 起由编译器分配，BIND_FUNC 动态
       扩容登记。内建 id 分配计数器从 0 起（func_new 自动分配 < 程序段）。 */
    vm->functions_by_id = vec_new(alloc, /*owns_element=*/false);
    vm->next_builtin_func_id = 0;
    vm->next_program_func_id = FUNC_ID_PROGRAM_BASE;
    vm->module_order = vec_new(alloc, /*owns_element=*/false);
    vm->defined_type_ids_global = vec_new(alloc, /*owns_element=*/false);
    vm->sema_types_global = vec_new(alloc, /*owns_element=*/false);

    /* const/volatile 修饰类型池（type_const_intern / type_volatile_intern
       intern 用；元素由 vm_destroy 手动释放，vec 只持有指针数组） */
    vm->const_types = vec_new(alloc, /*owns_element=*/false);
    vm->volatile_types = vec_new(alloc, /*owns_element=*/false);
    vm->ownership_types = vec_new(alloc, /*owns_element=*/false);

    /* optional 修饰类型池（type_option_intern / type_option_seal intern 用；
       元素由 vm_destroy 手动释放，vec 只持有指针数组） */
    vm->option_types = vec_new(alloc, /*owns_element=*/false);

    /* 指针类型池（type_ptr_intern / type_ptr_seal intern 用；元素由
       vm_destroy 手动释放，vec 只持有指针数组） */
    vm->ptr_types = vec_new(alloc, /*owns_element=*/false);

    /* 枚举类型池（type_enum_intern / type_enum_seal intern 用；元素由
       vm_destroy 手动释放，vec 只持有指针数组） */
    vm->enum_types = vec_new(alloc, /*owns_element=*/false);

    /* 结构体类型池（type_struct_intern / type_struct_seal intern 用；元素由
       vm_destroy 手动释放，vec 只持有指针数组） */
    vm->struct_types = vec_new(alloc, /*owns_element=*/false);

    /* 元组类型池（type_tuple_intern / type_tuple_seal intern 用；元素由
       vm_destroy 手动释放，vec 只持有指针数组） */
    vm->tuple_types = vec_new(alloc, /*owns_element=*/false);

    /* tag union 类型池（type_union_intern / type_union_seal intern 用；元素由
       vm_destroy 手动释放，vec 只持有指针数组） */
    vm->union_types = vec_new(alloc, /*owns_element=*/false);

    /* C 语义 union 类型池（type_cunion_intern / type_cunion_seal intern 用；
       元素由 vm_destroy 手动释放，vec 只持有指针数组） */
    vm->cunion_types = vec_new(alloc, /*owns_element=*/false);

    /* 切片类型池（type_slice_intern / type_slice_seal intern 用；
       元素由 vm_destroy 手动释放，vec 只持有指针数组） */
    vm->slice_types = vec_new(alloc, /*owns_element=*/false);

    /* 类型 id 表（id → type_t*，索引即 id；元素不 owns，归各类型池释放）。
       初始容量预留内建段（0..16）；**程序类型 id 由 type_id_alloc 全 VM 全局
       分配**（见 vm.h 字段注释：多模块共享本表，id 不能按模块重复起算），
       DEFINE_TYPE <id> 声明登记（SEAL 密封后幂等重绑）动态扩容。 */
    vm->types_by_id = vec_new(alloc, /*owns_element=*/false);
    vm->type_id_next = TYPE_ID_PROGRAM_BASE;

    /* 字符串池（str 生命周期托管：字面量/拼接/错误消息统一 intern） */
    vm->strs = vec_new(alloc, /*owns_element=*/false);

    /* 模块表（M5：规范路径 → module_t*，vm 拥有 module 生命周期） */
    vm->modules = strmap_new(alloc, /*owns_value=*/false);

    /* 跨模块泛型类型实例去重表（mangling 名 → type_t*，借用类型池） */
    vm->generic_types = strmap_new(alloc, /*owns_value=*/false);

    /* 基本类型注册进 global scope（LOAD 指令按名查 type value） */
    vm_register_builtin_types(vm);

    /* error/interrupt 也登记进类型 id 表（id 15/16，内建段） */
    vm_type_bind(vm, 15, vm->type_error);
    vm_type_bind(vm, 16, vm->type_interrupt);

    /* opaque 登记进类型 id 表（id 17，内建段）+ global scope（type_lookup
       按名解析 "opaque"，sema AST_OPAQUE 分支用） */
    vm_type_bind(vm, 17, vm->type_opaque);
    {
        const type_t *t = vm->type_opaque;
        void *data = value_alloc_data_copy(vm->alloc, vm->type_type, &t);
        value_t *tv = value_make(vm, vm->type_type, data);
        value_t *stored = scope_define(vm, vm->global_scope, "opaque", tv);
        if (!stored || value_is_error(vm, stored)) {
            panic("vm: failed to register builtin type 'opaque'");
        }
    }

    /* M6：printf **不再是内建**——它是 libc 的函数，由 clux 源码经 FFI 声明后
       调用（`extern lib c from "*";` + `extern func c::printf(format: str, ...): i32;`），
       走 BCODE_EXTERN_CALL → libffi（定参/变参都精确）。此前那个"魔改版"自带
       格式解析与 `%v` 扩展，已整体删除（见 docs/m6-design.md §14.19）。 */

    /* len 内置函数（m4-design §5/§11.3：len(array/slice/str) -> usize） */
    vm_register_len(vm);
    vm_register_deep_clone(vm);
    vm_register_rc_builtins(vm);

    return vm;
}

void vm_destroy(vm_t **pvm) {
    if (!pvm || !*pvm) return;
    vm_t *vm = *pvm;

    /* 模块表（M5）：必须在作用域链之前销毁。
       module 的 global_scope 以 vm->global_scope 为 parent（内建类型可见），
       scope_destroy → scope_remove_from_parent 会访问 parent->children 向量。
       若先销毁 vm->global_scope，module scope 的 remove_from_parent 将访问
       已释放的 global_scope 内存（use-after-free）。
       scope_destroy 释放 value 壳 + data 指针拷贝，不释放 func_t 本体
       （func_t 归 functions 池统一释放，无 double-free）。
       closure_scope->parent 在非调用期为 NULL，销毁顺序无关。 */
    if (vm->modules) {
        const vec_t *keys = strmap_keys(vm->modules);
        size_t n = vec_len(keys);
        for (size_t i = 0; i < n; i++) {
            const char *key = (const char *)vec_get(keys, i);
            module_t *mod = (module_t *)strmap_get(vm->modules, key);
            if (mod) module_destroy(vm, &mod);
        }
        strmap_free(vm->alloc, &vm->modules);
    }
    if (vm->generic_types) strmap_free(vm->alloc, &vm->generic_types);
    if (vm->module_order) vec_free(vm->alloc, &vm->module_order);
    if (vm->defined_type_ids_global) vec_free(vm->alloc, &vm->defined_type_ids_global);
    if (vm->sema_types_global) {
        size_t stn = vec_len(vm->sema_types_global);
        for (size_t sti = 0; sti < stn; sti++) {
            sema_type_t *st = (sema_type_t *)vec_get(vm->sema_types_global, sti);
            allocator_free(vm->alloc, (void **)&st);
        }
        vec_free(vm->alloc, &vm->sema_types_global);
    }

    /* 销毁作用域链：root_scope 是 global 的子作用域 */
    /* 先销毁 root_scope 以下的所有作用域（current_scope 可能在 root 之下） */
    /* 逐层 pop 直到 root_scope，再销毁 root_scope，再销毁 global_scope */
    while (vm->current_scope && vm->current_scope != vm->root_scope) {
        vm_pop_scope(vm);
    }
    scope_destroy(vm, &vm->root_scope);
    scope_destroy(vm, &vm->global_scope);

    vm->current_scope = NULL;

    /* 字符串池：释放全部 intern 的字符串块（value 侧只存池引用，不触碰） */
    vm_str_pool_destroy(vm);

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

    /* 函数 id 表：元素归 functions 池，仅释放向量结构 */
    vec_free(vm->alloc, &vm->functions_by_id);

    /* 函数签名类型池：单遍释放（M1 签名只引用内置静态类型，无相互依赖） */
    if (vm->sig_types) {
        size_t n = vec_len(vm->sig_types);
        for (size_t i = 0; i < n; i++) {
            type_t *tb = (type_t *)vec_get(vm->sig_types, i);
            if (!tb) continue;
            /* sig_types 同时存放 func 与 cfunc 两种布局：cfunc_type_t 没有
               capture_types 字段，按 func_type_t 释放会读到垃圾并 free 非法
               指针（cfunc + str 源码构建后进程清理崩溃）。 */
            if (tb->kind == TYPE_KIND_CFUNC) {
                cfunc_type_t *ct = (cfunc_type_t *)tb;
                if (ct->sig.params)
                    allocator_free(vm->alloc, (void **)&ct->sig.params);
                if (ct->base.name.ptr) {
                    char *np = (char *)ct->base.name.ptr;
                    allocator_free(vm->alloc, (void **)&np);
                }
                allocator_free(vm->alloc, (void **)&ct);
                continue;
            }
            func_type_t *ft = (func_type_t *)tb;
            if (ft->sig.params) allocator_free(vm->alloc, (void **)&ft->sig.params);
            if (ft->capture_types)
                allocator_free(vm->alloc, (void **)&ft->capture_types);
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
    if (vm->ownership_types) {
        size_t n = vec_len(vm->ownership_types);
        for (size_t i = 0; i < n; i++) {
            ownership_type_t *ot =
                (ownership_type_t *)vec_get(vm->ownership_types, i);
            if (!ot) continue;
            if (ot->base.name.ptr) {
                char *np = (char *)ot->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&ot);
        }
        vec_free(vm->alloc, &vm->ownership_types);
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

    /* optional 修饰类型池：释放 name + 结构体（inner 归底层类型，不在此释放） */
    if (vm->option_types) {
        size_t n = vec_len(vm->option_types);
        for (size_t i = 0; i < n; i++) {
            option_type_t *ot = (option_type_t *)vec_get(vm->option_types, i);
            if (!ot) continue;
            if (ot->base.name.ptr) {
                char *np = (char *)ot->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&ot);
        }
        vec_free(vm->alloc, &vm->option_types);
    }

    /* 指针类型池：释放 name + 结构体（base_type 归底层类型，不在此释放） */
    if (vm->ptr_types) {
        size_t n = vec_len(vm->ptr_types);
        for (size_t i = 0; i < n; i++) {
            ptr_type_t *pt = (ptr_type_t *)vec_get(vm->ptr_types, i);
            if (!pt) continue;
            if (pt->base.name.ptr) {
                char *np = (char *)pt->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&pt);
        }
        vec_free(vm->alloc, &vm->ptr_types);
    }

    /* 枚举类型池：释放 variant 表（含名）+ 结构体（underlying 归底层类型，
       不在此释放） */
    if (vm->enum_types) {
        size_t n = vec_len(vm->enum_types);
        for (size_t i = 0; i < n; i++) {
            enum_type_t *et = (enum_type_t *)vec_get(vm->enum_types, i);
            if (!et) continue;
            if (et->variants) {
                for (size_t j = 0; j < et->variant_count; j++) {
                    if (et->variants[j].name.ptr) {
                        char *np = (char *)et->variants[j].name.ptr;
                        allocator_free(vm->alloc, (void **)&np);
                    }
                }
                allocator_free(vm->alloc, (void **)&et->variants);
            }
            if (et->base.name.ptr) {
                char *np = (char *)et->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&et);
        }
        vec_free(vm->alloc, &vm->enum_types);
    }

    /* 结构体类型池：dispose_fn 自动释放字段表（含字段名）+ 显示名，仅需裸释放
       （字段类型归各自类型池，不在此释放） */
    if (vm->struct_types) {
        size_t n = vec_len(vm->struct_types);
        for (size_t i = 0; i < n; i++) {
            struct_type_t *st = (struct_type_t *)vec_get(vm->struct_types, i);
            if (!st) continue;
            allocator_free(vm->alloc, (void **)&st);
        }
        vec_free(vm->alloc, &vm->struct_types);
    }

    /* 元组类型池：dispose_fn 自动释放元素表 + 显示名，仅需裸释放（元素类型
       归各自类型池，不在此释放） */
    if (vm->tuple_types) {
        size_t n = vec_len(vm->tuple_types);
        for (size_t i = 0; i < n; i++) {
            tuple_type_t *tt = (tuple_type_t *)vec_get(vm->tuple_types, i);
            if (!tt) continue;
            allocator_free(vm->alloc, (void **)&tt);
        }
        vec_free(vm->alloc, &vm->tuple_types);
    }

    /* tag union 类型池：dispose_fn 自动释放 member 表 + 显示名，仅需裸释放
       （member payload_struct 归 struct_types 池，已在上方释放） */
    if (vm->union_types) {
        size_t n = vec_len(vm->union_types);
        for (size_t i = 0; i < n; i++) {
            union_type_t *ut = (union_type_t *)vec_get(vm->union_types, i);
            if (!ut) continue;
            allocator_free(vm->alloc, (void **)&ut);
        }
        vec_free(vm->alloc, &vm->union_types);
    }

    /* C 语义 union 类型池：dispose_fn 自动释放 member 表 + 显示名，仅需裸释放
       （member 类型归各自类型池，不在此释放） */
    if (vm->cunion_types) {
        size_t n = vec_len(vm->cunion_types);
        for (size_t i = 0; i < n; i++) {
            cunion_type_t *ct = (cunion_type_t *)vec_get(vm->cunion_types, i);
            if (!ct) continue;
            allocator_free(vm->alloc, (void **)&ct);
        }
        vec_free(vm->alloc, &vm->cunion_types);
    }

    /* 切片类型池：释放 name + 结构体（elem_type 归底层类型，不在此释放） */
    if (vm->slice_types) {
        size_t n = vec_len(vm->slice_types);
        for (size_t i = 0; i < n; i++) {
            slice_type_t *st = (slice_type_t *)vec_get(vm->slice_types, i);
            if (!st) continue;
            if (st->base.name.ptr) {
                char *np = (char *)st->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&st);
        }
        vec_free(vm->alloc, &vm->slice_types);
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
    /* 作用域退出 = 纯销毁（dispose owned + 释放 scope）。
       defer 已在编译期由 sema 改写为作用域退出点的显式 call 序列，
       运行期零隐式执行。error 值向上传播走 scope_destroy_subtree。 */
    scope_destroy(vm, &vm->current_scope);
    vm->current_scope = parent;
}

/* ---- 模块表（M5：规范路径 → module_t*） ---- */

void vm_module_bind(vm_t *vm, const char *path, module_t *mod) {
    if (!vm || !path || !mod) return;
    /* 幂等：同一路径重复登记，旧 module 由调用方释放（strmap_insert 返回旧值） */
    void *old = strmap_insert(vm->modules, vm->alloc, path, mod);
    (void)old; /* 调用方应避免重复注册；旧值不释放（可能导致泄漏，但不应发生） */
}

void vm_module_unbind(vm_t *vm, const char *path) {
    if (!vm || !path || !vm->modules) return;
    strmap_remove(vm->modules, path);
}

module_t *vm_module_lookup(vm_t *vm, const char *path) {
    if (!vm || !path || !vm->modules) return NULL;
    return (module_t *)strmap_get(vm->modules, path);
}

value_t *vm_import(vm_t *vm, const char *path) {
    if (!vm || !path) return NULL;
    module_t *mod = vm_module_lookup(vm, path);
    if (!mod) return NULL;
    /* 构造模块值：data = module_t*（借用，不拥有） */
    void *data = value_alloc_data_copy(vm->alloc, vm->type_module, &mod);
    return value_make(vm, vm->type_module, data);
}
