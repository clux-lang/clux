#include "vm/scope.h"
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/type_error.h"
#include "vm/type_ptr.h"
#include "core/strmap.h"
#include "core/panic.h"

#include <stdio.h>
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
    s->defers   = vec_new(alloc, /*owns_element=*/false);
    s->cleanup_names = vec_new(alloc, /*owns_element=*/false);
    if (!s->vars)     panic("vm: out of memory allocating scope vars");
    if (!s->owned)    panic("vm: out of memory allocating scope owned");
    if (!s->children) panic("vm: out of memory allocating scope children");
    if (!s->defers)   panic("vm: out of memory allocating scope defers");
    if (!s->cleanup_names) panic("vm: out of memory allocating scope cleanup names");

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

    /* 1. 释放所有 owned value 的 value_t 结构体与 data 块。
       **own 堆资源已由 sema 生成的 FREE <name> 指令在作用域退出点显式释放**，
       scope_destroy 不再递归释放堆资源——只释放 value_t 壳与 data 块本身。
       借用引用（OWN_REF）的 data 指向父值内部，跳过 data 释放。 */
    size_t owned_n = vec_len(scope->owned);
    for (size_t i = 0; i < owned_n; i++) {
        value_t *v = (value_t *)vec_get(scope->owned, i);
        if (v) {
            value_dispose(vm, v);  /* 释放 data 块（非借用） + 置 type=NULL */
            allocator_free(scope->alloc, (void **)&v);  /* 释放 value_t 结构体 */
        }
    }
    vec_free(scope->alloc, &scope->owned);

    /* 2. strmap_free 仅释放 key 副本（owns_value=false，不释放 value_t*） */
    strmap_free(scope->alloc, &scope->vars);

    /* 3. 释放 children 向量（不拥有元素，仅释放向量结构） */
    vec_free(scope->alloc, &scope->children);
    vec_free(scope->alloc, &scope->defers);
    if (scope->cleanup_names) {
        size_t cn = vec_len(scope->cleanup_names);
        for (size_t i = 0; i < cn; i++) {
            char *nm = (char *)vec_get(scope->cleanup_names, i);
            if (nm) allocator_free(scope->alloc, (void **)&nm);
        }
        vec_free(scope->alloc, &scope->cleanup_names);
    }

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

    /* 2. 测试模式：error 展开时 LIFO 执行本作用域已注册的 defer。
       正常路径的 defer 已由 compiler 在退出点显式调用；这里只处理异常。 */
    if (scope->defers) {
        size_t dn = vec_len(scope->defers);
        for (size_t i = dn; i > 0; i--) {
            value_t *fn = (value_t *)vec_get(scope->defers, i - 1);
            if (fn) (void)value_call(vm, fn, NULL, 0);
        }
        vec_free(scope->alloc, &scope->defers);
    }

    /* 3. 测试模式：异常展开时 FREE 已注册的 own 变量。正常路径的 FREE
       已由 compiler 在退出点执行；此处按变量名查当前 scope。 */
    if (scope->cleanup_names) {
        size_t cn = vec_len(scope->cleanup_names);
        for (size_t i = cn; i > 0; i--) {
            const char *nm = (const char *)vec_get(scope->cleanup_names, i - 1);
            if (!nm) continue;
            value_t *v = scope_lookup(scope, strslice_from_cstr(nm));
            if (!v || value_is_borrowed(v) || !value_data(v)) continue;
            const type_t *t = type_strip_quals(value_type(v));
            if (t) ptr_free_owned_recursive(vm, t, value_data(v));
        }
    }

    /* 4. 销毁自身（从父作用域的 children 中移除 + dispose vars + free） */
    scope_destroy(vm, pscope);
}

void scope_register_defer(vm_t *vm, scope_t *scope, value_t *fn) {
    (void)vm;
    if (!scope || !fn || !scope->defers) return;
    vec_push(scope->defers, scope->alloc, fn);
}

void scope_register_cleanup(vm_t *vm, scope_t *scope, strslice_t name) {
    if (!scope || !scope->cleanup_names || !name.ptr) return;
    char *copy = (char *)allocator_new_ex(scope->alloc, "scope.cleanup",
                                          name.len + 1, NULL, NULL, NULL, 1);
    if (!copy) return;
    memcpy(copy, name.ptr, name.len);
    copy[name.len] = '\0';
    vec_push(scope->cleanup_names, scope->alloc, copy);
}

void scope_track(vm_t *vm, scope_t *scope, value_t *v) {
    (void)vm;
    if (!scope || !v || !value_type(v)) return;
    vec_push(scope->owned, scope->alloc, v);
}

/* 定义变量：把 name 绑定到 v。
 *
 * **作用域唯一所有权**（VM 所有权模型，2026-10 重构）：
 *   - v 已由本作用域拥有（`v->frame == &scope->frame`）且不是借用视图
 *     → **直接绑定（采用/adopt）**，零拷贝。变量表只是 name→value 的指针别名；
 *     值本体仍在本作用域 owned 列表里，scope 退出统一释放。
 *   - 否则（借用视图：data 指向别的值内部；或值属于其他作用域）
 *     → 先 materialize 成独立深拷贝（拷贝产物登记进本作用域 owned）。
 *
 * 此前一律 clone，于是必须另开 `scope_define_owned` + `is_fresh` 标记 +
 * 调用方 `scope_untrack` 的"接管"双轨来避免重复深拷贝——那条轨道会让 value
 * 的归属在作用域之间游离（本次重构要消除的正是它）。 */
value_t *scope_define(vm_t *vm, scope_t *scope, const char *name, value_t *v) {
    if (!scope || !name) return NULL;

    /* 重复定义检查：当前作用域已有同名变量时报错 */
    if (strmap_get(scope->vars, name)) {
        return value_make_error(vm, "scope: duplicate variable definition");
    }

    value_t *stored;
    if (v && !value_is_borrowed(v) &&
        value_frame(v) == &scope->frame) {
        /* 本作用域所有的**非借用**值（只可能是表达式刚创建、尚未与任何变量
           绑定的临时值）：**直接采用**（零拷贝）。变量读取已改为压入借用视图，
           因此不会走到这里——无需"是否已绑定"的附加标记。 */
        stored = v;
    } else {
        /* 借用视图（`var b = a` 压入的是 a 的视图）或其他作用域的值：
           materialize 到本作用域（深拷贝）——否则 b 与 a 会别名。 */
        scope_t *saved = vm->current_scope;
        vm->current_scope = scope;
        stored = value_clone(vm, v);
        vm->current_scope = saved;
    }

    strmap_insert(scope->vars, vm->alloc, name, stored);
    return stored;
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

