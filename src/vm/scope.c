#include "vm/scope.h"
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/type_ptr.h"
#include "core/strmap.h"
#include "core/panic.h"

#include <string.h>

static class_t g_scope_class = {
    .name       = "clux.vm.scope",
    .size       = sizeof(scope_t),
    .clone_fn   = NULL,
    .move_fn    = NULL,
    .dispose_fn = NULL,
};

scope_t *scope_new(allocator_t *alloc, scope_t *parent) {
    scope_t *s = (scope_t *)allocator_new(alloc, &g_scope_class, 1);
    if (!s) panic("vm: out of memory allocating scope");

    s->parent   = parent;
    s->frame.parent = parent ? &parent->frame : NULL; /* 与 parent 同步 */
    s->alloc    = alloc;
    s->vars     = strmap_new(alloc, /*owns_value=*/false);
    s->owned    = vec_new(alloc, /*owns_element=*/false);
    s->children = vec_new(alloc, /*owns_element=*/false);
    if (!s->vars)     panic("vm: out of memory allocating scope vars");
    if (!s->owned)    panic("vm: out of memory allocating scope owned");
    if (!s->children) panic("vm: out of memory allocating scope children");

    /* 注册到父作用域的 children */
    if (parent && parent->children) {
        vec_push(parent->children, alloc, s);
    }

    return s;
}

/* ---- 内部：从父作用域的 children 中移除自身 ---- */

static void scope_remove_from_parent(scope_t *scope) {
    if (!scope || !scope->parent || !scope->parent->children) return;
    size_t n = vec_len(scope->parent->children);
    for (size_t i = 0; i < n; i++) {
        if (vec_get(scope->parent->children, i) == scope) {
            vec_swap_remove(scope->parent->children, i);
            break;
        }
    }
}

void scope_destroy(vm_t *vm, scope_t **pscope) {
    if (!pscope || !*pscope) return;
    scope_t *scope = *pscope;

    /* 0. 从父作用域的 children 中移除自身 */
    scope_remove_from_parent(scope);

    /* 1. dispose + free 所有 owned value（scope 唯一的生命周期管理入口）。
       销毁前先递归释放值内部所有 own 指针指向的堆内存（m3-design §3.1：
       own 销毁沿字段递归——own *Struct 且 Struct 含 own 字段时递归传导；
       含结构体字段内嵌 own 指针的场景由 ptr_free_owned_recursive 统一
       扫描，value_dispose 只负责释放值自身的 data 块）。
       仅拥有 data 的值（非借用）递归销毁——借用引用的 data 指向父值内部，
       由父值销毁统一处理，跳过防 use-after-free（父值先于借用创建，先销毁）。 */
    size_t owned_n = vec_len(scope->owned);
    for (size_t i = 0; i < owned_n; i++) {
        value_t *v = (value_t *)vec_get(scope->owned, i);
        if (v) {
            if (!value_is_borrowed(v))
                ptr_free_owned_recursive(vm, value_type(v), value_data(v));
            value_dispose(vm, v);  /* 释放 data */
            allocator_free(scope->alloc, (void **)&v);  /* 释放 value_t 结构体 */
        }
    }
    vec_free(scope->alloc, &scope->owned);

    /* 2. strmap_free 仅释放 key 副本（owns_value=false，不释放 value_t*） */
    strmap_free(scope->alloc, &scope->vars);

    /* 3. 释放 children 向量（不拥有元素，仅释放向量结构） */
    vec_free(scope->alloc, &scope->children);

    /* 4. 释放 scope 结构体 */
    allocator_free(scope->alloc, (void **)pscope);
}

void scope_destroy_subtree(vm_t *vm, scope_t **pscope) {
    if (!pscope || !*pscope) return;
    scope_t *scope = *pscope;

    /* 1. 递归销毁所有子作用域（子作用域会从本 scope 的 children 中移除自身） */
    while (!vec_is_empty(scope->children)) {
        scope_t *child = (scope_t *)vec_last(scope->children);
        scope_destroy_subtree(vm, &child);
    }

    /* 2. 销毁自身（从父作用域的 children 中移除 + dispose vars + free） */
    scope_destroy(vm, pscope);
}

void scope_track(vm_t *vm, scope_t *scope, value_t *v) {
    (void)vm;
    if (!scope || !v || !value_type(v)) return;
    vec_push(scope->owned, scope->alloc, v);
}

value_t *scope_define(vm_t *vm, scope_t *scope, const char *name, value_t *v) {
    if (!scope || !name) return NULL;

    /* 重复定义检查：当前作用域已有同名变量时报错 */
    if (strmap_get(scope->vars, name)) {
        return value_make_error(vm, "scope: duplicate variable definition");
    }

    /* 临时切换 current_scope 到目标 scope，clone 后 value 自动注册到 owned */
    scope_t *saved = vm->current_scope;
    vm->current_scope = scope;
    value_t *cloned = value_clone(vm, v);
    vm->current_scope = saved;

    strmap_insert(scope->vars, vm->alloc, name, cloned);

    return cloned;
}

/* 所有权接管定义（m3-design §7 含 own 复合类型不可 copy，须 move/clone）：
   v 是 fresh 临时值（new/construct/move/clone 产物，auto-track 在
   vm->current_scope 的 owned 列表）——DEFINE 直接接管：绑定 v 本体到变量名，
   跳过 value_clone 浅拷贝（浅拷贝会让变量与 temp 共享内嵌 own 堆块，作用域
   退出双释放）。v **保持**在 owned 列表——变量生命周期 = 作用域生命周期，
   scope_destroy 遍历 owned 统一销毁（递归释放堆块 + 释放 data/value 壳），
   与 scope_define 的 clone 产物同生命周期路径。曾误从 owned 移除导致接管值
   的 value/data/堆块全部泄漏。R4 专用：调用方（op_define）已保证 v 非借用
   引用。 */
value_t *scope_define_owned(vm_t *vm, scope_t *scope, const char *name,
                            value_t *v) {
    if (!scope || !name) return NULL;

    /* 重复定义检查：当前作用域已有同名变量时报错 */
    if (strmap_get(scope->vars, name)) {
        return value_make_error(vm, "scope: duplicate variable definition");
    }

    /* 防御（调用方 op_define 已保证）：v 非借用引用（借用 data 指向父值
       内部，接管会误伤父值）。 */
    if (value_is_borrowed(v)) return value_make_error(vm,
        "scope: cannot take ownership of a borrowed reference");

    /* v 的 frame 指向其原所属作用域——随 scope 销毁统一处理
       （scope_destroy 遍历 owned 递归销毁），无需改 frame。 */

    strmap_insert(scope->vars, vm->alloc, name, v);
    return v;
}

void scope_untrack(vm_t *vm, scope_t *scope, value_t *v) {
    (void)vm;
    if (!scope || !v || !scope->owned) return;
    size_t n = vec_len(scope->owned);
    for (size_t i = 0; i < n; i++) {
        if (vec_get(scope->owned, i) == v) {
            vec_swap_remove(scope->owned, i);
            break;
        }
    }
}

/* 从 owned 列表中移除并销毁 value（scope_set 替换旧值用） */
static void scope_remove_owned(vm_t *vm, scope_t *scope, value_t *v) {
    size_t n = vec_len(scope->owned);
    for (size_t i = 0; i < n; i++) {
        if (vec_get(scope->owned, i) == v) {
            vec_swap_remove(scope->owned, i);
            break;
        }
    }
    value_dispose(vm, v);
    allocator_free(scope->alloc, (void **)&v);
}

value_t *scope_set(vm_t *vm, scope_t *scope, const char *name, value_t *v) {
    if (!scope || !name) return NULL;

    value_t *old = (value_t *)strmap_get(scope->vars, name);
    if (old) {
        /* 已有：移除 vars 映射 + 从 owned 移除旧值销毁，再走 define 绑定新值 */
        strmap_remove(scope->vars, name);
        scope_remove_owned(vm, scope, old);
    }
    return scope_define(vm, scope, name, v);
}

value_t *scope_lookup(const scope_t *scope, strslice_t name) {
    for (const scope_t *s = scope; s; s = s->parent) {
        char buf[256];
        if (name.len < sizeof(buf)) {
            memcpy(buf, name.ptr, name.len);
            buf[name.len] = '\0';
            value_t *v = (value_t *)strmap_get(s->vars, buf);
            if (v) return v;
        }
    }
    return NULL;
}
