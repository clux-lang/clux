#include "vm/scope.h"
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/type_ptr.h"
#include "vm/type_error.h"
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
    if (!s->vars)     panic("vm: out of memory allocating scope vars");
    if (!s->owned)    panic("vm: out of memory allocating scope owned");
    if (!s->children) panic("vm: out of memory allocating scope children");
    if (!s->defers)   panic("vm: out of memory allocating scope defers");

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

    /* 3b. 释放 defers 向量（**借用**指针，不拥有值——值随 owned 释放，
        与 owned 中同一批函数值的 dispose 互不干涉，无 double free）。
        注意：defer 的**执行**不在这里，而在 vm_pop_scope（正常作用域退出
        路径）→ scope_run_defers，先跑完 defer 再调用本函数销毁；error
        展开走 scope_destroy_subtree（不触发 defer，见 include/vm/scope.h）。 */
    vec_free(scope->alloc, &scope->defers);

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

/* ================================================================ */
/* defer 栈（延迟执行块）                                             */
/* ================================================================ */

/* BCODE_DEFER：登记（借用）一个 defer 闭包值到 scope 的 defer 栈。
 *
 * 所有权：**不接管**——值由 value_make 在 MAKE_FUNCTION 时已 auto-track 进
 * 同一个 scope 的 owned 列表；这里只把指针按登记序追加。scope 退出时
 * scope_run_defers 逆序执行，随后 scope_destroy 照常 dispose owned 里的
 * 那批函数值（func value 的 dispose 是 no-op，func_t 归 vm->functions），
 * 因此既不泄漏也不会 double free。 */
void scope_add_defer(vm_t *vm, scope_t *scope, value_t *v) {
    (void)vm;
    if (!scope || !v || !scope->defers) return;
    vec_push(scope->defers, scope->alloc, v);
}

/* 逆序执行 defer 栈：LIFO（后登记的先执行）。
 *
 * 每次调用的语义与"调用一个无参 void 闭包"完全一致——走唯一入口
 * value_call → func_vcall：闭包自己的 closure_scope（捕获值）+ 匿名参数层，
 * 函数体在独立的 scope 链上执行。因此 defer 体内：
 *   - 读捕获变量（clone 值，与闭包同语义）；
 *   - 读**本作用域**尚未释放的一切（own/fatal 变量及其堆块：本函数在
 *     scope_destroy 的 dispose 之前被调用）；
 *   - 调用任意函数 / 读写字段。
 *
 * 返回值一律丢弃（与 `_ = expr` 一致）：defer 的 error 值不改变作用域退出
 * 流程，也不向调用方传播（作用域正在销毁，无处可传）。闭包体内部的
 * `return` 只从该闭包返回（它就是函数体），不影响外层函数。
 *
 * 执行期间把 vm->current_scope 临时切到本 scope：闭包体沿作用域链查找时
 * 能看到本作用域的名字（func_vcall 会再把 current_scope 换成闭包自己的
 * closure_scope，闭包体查找链 = closure_scope → 本 scope(临时 parent 接线
 * 到函数的 root_scope)…，这里切换的是"调用方现场"，语义与在作用域内直接
 * 调用该闭包一致）。 */
void scope_run_defers(vm_t *vm, scope_t *scope) {
    if (!vm || !scope || !scope->defers) return;
    if (vec_is_empty(scope->defers)) return; /* 绝大多数作用域：零开销 */

    scope_t *saved_current = vm->current_scope;
    vm->current_scope = scope;

    /* 边执行边弹栈（不预先快照长度）：执行期间 current_scope 已是闭包自己的
       作用域，defer 体内再登记 defer 只会登记到它自己的作用域——本 scope 的
       栈不会被展开中追加。即便真被追加（防御），此处也照样按 LIFO 执行。 */
    while (!vec_is_empty(scope->defers)) {
        value_t *d = (value_t *)vec_pop(scope->defers);
        if (!d) continue;
        value_t *r = value_call(vm, d, NULL, 0);
        /* defer 的返回值（含 error/panic 值）**不传给任何消费点**：作用域正在
           销毁，没有接收方（与 `_ = expr` 丢弃语义一致）。唯一的例外是"打出来
           让人看见"——error 值在这个 VM 里代表引擎级失败（越界/未定义变量/
           unwrap none/FFI 失败…），静默丢弃会让 defer 体悄悄半途而废，因此把
           error 文本打到 stderr（**不改控制流、不改退出码**，与析构函数
           "记日志继续" 同一姿态）。 */
        if (value_is_error(vm, r)) {
            error_data_t *ed = (error_data_t *)value_data(r);
            fprintf(stderr, "defer: %s\n",
                    ed && ed->message ? ed->message : "runtime error");
        }
    }

    vm->current_scope = saved_current;
}
