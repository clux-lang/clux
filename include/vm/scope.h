#ifndef _H_CLUX_VM_SCOPE_
#define _H_CLUX_VM_SCOPE_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/value.h"
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
 *
 * 所有 value 必须由 scope 管理生命周期，严禁出现孤立 value。
 */
typedef struct scope_t {
    struct scope_t *parent;
    strmap_t       *vars;      /* name -> value_t* 借用映射（不拥有 value） */
    vec_t          *owned;     /* value_t* 所有 value（scope 拥有生命周期） */
    vec_t          *children;  /* scope_t* 子作用域（不拥有，子自行管理生命周期） */
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

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_SCOPE_ */
