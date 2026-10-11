#ifndef _H_CLUX_VM_SCOPE_
#define _H_CLUX_VM_SCOPE_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/value.h"
#include "vm/scope_frame.h"
#include "core/strslice.h"
#include "core/strmap.h"
#include "core/vec.h"
#include <stdbool.h>

/**
 * scope_t: 词法作用域
 *
 * - owned 向量拥有所有 value_t*（命名的 + 匿名的），管理生命周期
 * - vars 是 name -> value_t* 的借用映射（owns_value=false），不拥有 value
 * - 变量查找沿 parent 链向上递归（当前 -> parent -> ... -> global）
 * - 作用域退出时通过 owned 销毁所有 value
 * - children 向量记录所有子作用域，用于 error 传播时砍掉整个子树
 * - defers 是**借用**向量（value_t*，不拥有值）：本作用域登记的全部 defer
 *   闭包值，按登记序存放；作用域退出时**逆序**执行（LIFO）。值本体的生命
 *   周期仍由 owned 统一管理（defer 栈只存指针），因此既不泄漏也不会 double
 *   free——执行完 defer 后 owned 照常 dispose 这些函数值（func value 的
 *   dispose 是 no-op，func_t 本体归 vm->functions）。
 * - frame 是与父子关系同步的轻量节点（frame.parent == parent->frame，
 *   根作用域 frame.parent == NULL）：value 借用 &scope->frame 标记所属
 *   作用域，所有权分析经 parent 链判定逃逸（见 scope_frame.h）
 *
 * 所有 value 必须由 scope 管理生命周期，严禁出现孤立 value。
 */
typedef struct scope_t {
    struct scope_t *parent;
    scope_frame_t   frame;      /* 父子关系节点（value 借用，与 parent 同步） */
    strmap_t       *vars;      /* name -> value_t* 借用映射（不拥有 value） */
    vec_t          *owned;     /* value_t* 所有 value（scope 拥有生命周期） */
    vec_t          *children;  /* scope_t* 子作用域（不拥有，子自行管理生命周期） */
    vec_t          *defers;    /* value_t* 借用（defer 闭包值，登记序；逆序执行） */
    allocator_t    *alloc;     /* 借用 vm 的 allocator */
} scope_t;

/** 创建新作用域，parent 可为 NULL（全局作用域） */
scope_t *scope_new(allocator_t *alloc, scope_t *parent);

/** 销毁作用域及其所有 value（先 dispose 再 free） */
void scope_destroy(vm_t *vm, scope_t **scope);

/**
 * 销毁作用域及其整棵子树（递归销毁所有 children）。
 * 用于 error 传播：不走 pop_scope，直接砍掉子树以避免触发 defer。
 * 子树销毁后从父作用域的 children 中移除自身。
 */
void scope_destroy_subtree(vm_t *vm, scope_t **scope);

/**
 * 将已创建的 value_t* 注册到 scope 的 owned 列表。
 * scope 拥有该 value 的生命周期，销毁时自动 dispose。
 * value_clone / value_make 等内部自动调用此函数，通常不需要手动调用。
 */
void scope_track(vm_t *vm, scope_t *scope, value_t *v);

/**
 * 定义变量: 将 name 和 v 绑定到当前作用域。
 * name 会被拷贝，value 会被 clone 进 scope。
 * 返回 scope 内存储的 value_t 指针（借用的）。
 * 已有同名变量 → error（重复定义）。
 */
value_t *scope_define(vm_t *vm, scope_t *scope, const char *name, value_t *v);


/**
 * **仅测试用**（src 已无调用者）：从 scope 的 owned 列表移除 v（不释放）。
 * 正常代码中 value 的生命周期完全由 scope 管理——不存在"脱离作用域"的
 * value（VM 所有权模型，2026-10 重构）。
 * 原用途：
 * 供 op_define/op_store 接管 fresh 临时值（new/construct/move/clone 产物）
 * 后调用——temp 已随 memcpy/绑定转移给目标，从 owned 移除防作用域退出
 * 双释放。v 不在 owned 列表时静默 no-op。
 */
void scope_untrack(vm_t *vm, scope_t *scope, value_t *v);

/**
 * 定义或替换变量（define-or-replace）：与 scope_define 不同，已有同名变量
 * 时先销毁旧值（从 owned 移除 + dispose + free）再 clone 新值绑定。
 * 用于闭包捕获槽位：函数提升区先以 undefined 占位（DEFINE 时无真实值），
 * 定义点 SET_CLOSURE 用真实捕获值替换——void 占位无 assign 槽，须走
 * scope 层替换。
 * 返回 scope 内存储的 value_t 指针（借用的），失败返回 NULL（无诊断，
 * 调用方负责 error）。
 */
value_t *scope_set(vm_t *vm, scope_t *scope, const char *name, value_t *v);

/** 查找变量（沿 parent 链递归），未找到返回 NULL */
value_t *scope_lookup(const scope_t *scope, strslice_t name);

/** 获取当前作用域的父作用域 */
static inline scope_t *scope_parent(const scope_t *scope) {
    return scope ? scope->parent : NULL;
}

/* ---- defer 栈（延迟执行块）---- */

/**
 * 登记一个 defer 闭包值到 scope 的 defer 栈（**借用**，不接管所有权）。
 * 值本体必须已由该 scope（或外层）的 owned 列表持有——正常路径是
 * BCODE_DEFER 弹出的、由 MAKE_FUNCTION 刚构造并 auto-track 进当前 scope
 * 的函数值。栈空时无操作。
 */
void scope_add_defer(vm_t *vm, scope_t *scope, value_t *v);

/**
 * **逆序**（LIFO）执行 scope 的 defer 栈：逐个 value_call(闭包, 无参)，
 * 全部执行完后清空栈。执行期间临时把 vm->current_scope 切到本 scope
 * （闭包体沿作用域链查找本 scope 的变量），返回前恢复。
 *
 * 调用时机：vm_pop_scope 在 scope_destroy（dispose owned + 释放 scope）
 * **之前**调用本函数——defer 体内因此能正常读到本作用域的一切（含
 * own/fatal 变量与它们指向的堆块）。defer 的返回值不传给任何消费点
 * （作用域正在销毁，没有接收方，与 `_ = expr` 丢弃语义一致）：唯一例外是
 * error 值（引擎级失败）会以 `defer: <消息>` 打到 stderr，避免 defer 体
 * 静默半途而废——不影响控制流与退出码。
 * 无 defer（绝大多数作用域）时零开销直接返回。
 */
void scope_run_defers(vm_t *vm, scope_t *scope);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_SCOPE_ */
