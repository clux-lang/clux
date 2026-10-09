#ifndef _H_CLUX_VM_TYPE_PTR_
#define _H_CLUX_VM_TYPE_PTR_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include "vm/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 指针类型 vtable（own/ref/fatal/share/weak 五种所有权修饰，kind 区分） */
extern const vtable_t VTABLE_PTR_OWN;
extern const vtable_t VTABLE_PTR_REF;
extern const vtable_t VTABLE_PTR_FATAL;
extern const vtable_t VTABLE_PTR_SHARE;
extern const vtable_t VTABLE_PTR_WEAK;

/* ================================================================ */
/* 指针类型（ptr_type_t，继承 type_t，m3-design §3/§12）             */
/* ================================================================ */

/**
 * ptr_type_t 构造对应统一两遍协议 PUSH_PTR / DEFINE_TYPE / LOAD_TYPE /
 * SET_TYPE（设 base_type）/ SEAL（与 func 签名类型同族）：
 *   - type_ptr_push 分配空 ptr_type（base_type=NULL，不入池）+ 压其 type
 *     value，返回该 type（外部只持有 type_t*）。
 *   - type_ptr_set_base 设被指向类型 T（SET_TYPE 运行期用）；密封后静默忽略。
 *   - type_ptr_seal 按 base_type 指针去重 intern（同一 base 的五种所有权是
 *     五个独立实例），标记 sealed。
 *
 * 指针类型的 size/align 恒定（指针宽），密封不依赖 base_type 布局——hoist
 * 中属引用依赖（emit_ref_type，LOAD_TYPE 拉回 base，不递归），与 func
 * 签名类型同族。
 */
typedef struct ptr_type_t ptr_type_t;  /* 定义见 vm/type.h */

/** PUSH_PTR：分配开放 ptr_type（base_type=NULL，不入池）+ 压其 type value。
 *  kind = 所有权修饰（TYPE_KIND_PTR_OWN/REF/FATAL/SHARE/WEAK，PUSH_PTR 指令
 *  u8 操作数）。 */
const type_t *type_ptr_push(vm_t *vm, type_kind_t kind);

/** SET_TYPE 运行期用：设被指向类型 T（密封后静默忽略） */
void type_ptr_set_base(vm_t *vm, const type_t *t, const type_t *base);

/** SEAL：按 base_type 去重 intern + 置 sealed。返回该 const type */
const type_t *type_ptr_seal(vm_t *vm, const type_t *t);

/** 一次性快捷（type_ptr_push + set_base + seal）：sema 解析 own/ref/fatal/
 *  share/weak *T 用。不向操作数栈压 type value（嵌套构造场景避免栈布局污染）。 */
const type_t *type_ptr_intern(vm_t *vm, type_kind_t kind, const type_t *base);

/** 取指针类型的被指向类型 T（非指针返回 NULL） */
static inline const type_t *ptr_type_base(const type_t *t) {
    return (t && (t->kind == TYPE_KIND_PTR_OWN ||
                  t->kind == TYPE_KIND_PTR_REF ||
                  t->kind == TYPE_KIND_PTR_FATAL ||
                  t->kind == TYPE_KIND_PTR_SHARE ||
                  t->kind == TYPE_KIND_PTR_WEAK))
               ? ((const ptr_type_t *)t)->base_type
               : NULL;
}

/* ================================================================ */
/* 指针值布局（m3-design §7/§8）                                     */
/* ================================================================ */

/**
 * 指针值（data 块布局）。五种指针的 value data 统一为 { 裸指针 }：
 *   - ptr：指向目标内存（new 堆块 / 栈上值 / 借用目标 / RC 控制块）
 *   - 所有权由 type kind 区分（m3-design §13.2：运行期零标志）：
 *     own *T    → 总拥有堆块，dispose 释放（new 产物接管后）
 *     ref *T    → 总借用，dispose 跳过（指向栈值 / 他人堆块）
 *     fatal *T  → ptr≠NULL 时拥有（将亡值），转移后 ptr=NULL（接管方拥有）
 *     share *T  → ptr 指向 rc_block_t，dispose 递减 strong 计数
 *     weak *T   → ptr 指向 rc_block_t，dispose 递减 weak 计数
 *
 * size = sizeof(ptr_value_t)（对齐到指针宽），type_ptr_seal 设定。
 */
typedef struct ptr_value_t {
    void *ptr;
} ptr_value_t;

/** 读指针值目标地址（非指针 value 返回 NULL）。
 *  share/weak 返回 RC 块内 payload 地址（rc_block_payload）。 */
static inline void *ptr_value_target(const value_t *v) {
    if (!v) return NULL;
    const type_t *t = value_type(v);
    if (!t || (t->kind != TYPE_KIND_PTR_OWN &&
               t->kind != TYPE_KIND_PTR_REF &&
               t->kind != TYPE_KIND_PTR_FATAL &&
               t->kind != TYPE_KIND_PTR_SHARE &&
               t->kind != TYPE_KIND_PTR_WEAK))
        return NULL;
    return ((const ptr_value_t *)value_data(v))->ptr;
}

/** 构造指针值：目标地址（五种指针统一，所有权由 type kind 区分，
 *  m3-design §13.2 运行期零标志） */
value_t *ptr_make_value(vm_t *vm, const type_t *pt, void *target);

/* ================================================================ */
/* 递归销毁（m3-design §3.1/§13：own 销毁沿字段递归）                */
/* ================================================================ */

/**
 * 递归释放 data 块内所有 own 指针指向的堆内存（m3-design §3.1：
 * `own *Struct` 且 Struct 含 own 字段时，销毁递归传导）。
 *
 * 纯静态分派（type + 裸 data 块，不依赖 value_t 包装）：
 *   - own *T：ptr≠NULL → 先递归销毁堆块内容（T 为含 own 字段的复合类型时
 *     内嵌堆块一并释放），再释放本堆块，置 ptr=NULL（防双释放）
 *   - fatal *T：ptr≠NULL → 同 own（将亡值未接管时释放；接管路径已 ptr=NULL）
 *   - ref *T：借用，跳过（指向栈值或他人堆块，不拥有）
 *   - struct/tuple：按字段/元素偏移递归
 *   - array：按 elem_type 逐元素递归（连续块，步长 elem_type->size）
 *   - option：ok 时按 inner 递归 value 字段
 *   - union：按当前 tag 的 member payload 类型递归
 *   - cunion：跳过（无 tag，无法确定激活 member，与 clone/dispose 的
 *     "开发者自负"语义一致）
 *   - 标量/字符串/其他：无操作
 *
 * 只释放 own 指向的堆块，不释放 data 块本身（data 块归 value 生命周期
 * 管理——scope_destroy 先调用本函数再 value_dispose）。
 */
void ptr_free_owned_recursive(vm_t *vm, const type_t *t, void *data);

/**
 * 递归清空 data 块内所有 own/fatal 指针的 ptr（**不释放**，m3-design
 * §3.3 接管辅助）。与 ptr_free_owned_recursive 同遍历（own/fatal 指针 +
 * struct/tuple/array/option/union 按字段/元素/tag 递归），但只置 ptr=NULL：
 *
 * 供 op_new/op_construct/value_make_array 在成员值 memcpy 进目标块后调用——
 * 指针值已随 memcpy 复制进接管方，所有权随拷贝转移，清空源对象图防作用域
 * 退出时与接管方双释放。遍历覆盖复合类型内嵌 own 指针（own *Struct 的
 * Struct 字段里的 own *T），不止顶层直接指针。
 */
void ptr_clear_owned_recursive(vm_t *vm, const type_t *t, void *data);

/**
 * 深拷贝 type 类型的数据块（对象图克隆，m3-design §7"深拷贝整个对象图"）：
 *   - own/fatal 指针且 ptr≠NULL：分配新堆块 + 递归克隆被指向类型（内嵌
 *     own 字段一并深拷贝），新指针指向独立新堆块——两个实例各自独占完整
 *     对象图，递归销毁互不干扰（无共享指针 = 无双释放）
 *   - 借用（ref / ptr=NULL）：复制指针值，不深拷贝被指物
 *   - struct/tuple/array/option/union：按字段/元素/tag 递归克隆
 *   - cunion：整块 memcpy（无 tag，无法确定激活 member，开发者自负）
 *   - 标量/字符串：整块 memcpy
 *
 * 返回新分配的 dst 块（调用方 memcpy 进目标或直接持有）。struct/tuple/
 * array 的 vtable clone 走此路径保证 own 字段深拷贝（浅拷贝 + 递归销毁
 * 会双释放共享堆块）。
 */
void *ptr_clone_block(vm_t *vm, const type_t *type, const void *src);

/* ================================================================ */
/* RC 控制块（share/weak，m3-design §12）                            */
/* ================================================================ */

/**
 * rc_block_t：RC 控制块（share/weak 的 ptr 指向此结构）。
 *
 * 布局：头部（计数 + 元信息）紧跟 payload（T 的实际数据）。
 *   - strong：share 引用计数（atomic）。strong→0 时析构 payload（递归
 *     释放内嵌 own/share 字段），但保留控制块（weak 仍观察）。
 *   - weak：weak 引用计数 + 1（strong 自身占一个 weak 引用，strong→0
 *     时递减）。weak→0 时释放控制块 + payload 内存。
 *   - alloc：分配器引用（释放控制块用，内嵌 data 便于 C 后端生成）。
 *   - type：payload 类型引用（析构递归用）。
 *   - destroyed：strong→0 析构后置 true（防御重复析构）。
 *
 * 线程安全（m3-design §12.5）：strong/weak 皆为 atomic 变量，
 * upgrade 用 CAS 原子检查 strong>0 并递增。
 */
#include <stdatomic.h>

typedef struct rc_block_t {
    atomic_uint  strong;       /* share 引用计数 */
    atomic_uint  weak;         /* weak 引用计数 + 1（strong 占一个 weak） */
    allocator_t *alloc;        /* 分配器（释放控制块用） */
    const type_t *type;        /* payload 类型（析构递归用） */
    bool         destroyed;    /* strong→0 析构后置 true */
    /* payload 紧跟其后（rc_block_payload 获取地址） */
} rc_block_t;

/** RC 块 payload 地址（紧跟头部之后，按 type->align 对齐） */
static inline void *rc_block_payload(rc_block_t *rc) {
    size_t hdr = sizeof(rc_block_t);
    size_t align = rc->type ? rc->type->align : 1;
    size_t aligned = (hdr + align - 1) & ~(align - 1);
    return (uint8_t *)rc + aligned;
}

/** 分配 RC 块 + payload（按 type->size 布局），初始化 strong=1, weak=1。
 *  payload 由调用方填充（memcpy 成员值）。 */
rc_block_t *rc_block_new(allocator_t *alloc, const type_t *payload_type);

/** share 引用计数 +1（atomic fetch_add）。 */
void rc_block_strong_inc(rc_block_t *rc);

/** share 引用计数 -1。strong→0 时析构 payload（递归释放内嵌 own/share
 *  字段）+ weak--（strong 释放其占有的 weak 引用）。weak→0 时释放控制块。 */
void rc_block_strong_dec(vm_t *vm, rc_block_t *rc);

/** weak 引用计数 +1（atomic fetch_add）。 */
void rc_block_weak_inc(rc_block_t *rc);

/** weak 引用计数 -1。weak→0 时释放控制块 + payload 内存。 */
void rc_block_weak_dec(vm_t *vm, rc_block_t *rc);

/** upgrade：原子检查 strong>0 并递增。成功返回 payload 地址（调用方包装
 *  为 share value），失败返回 NULL（strong 已归零，对象已析构）。 */
void *rc_block_upgrade(rc_block_t *rc);

/** weak 从 share 派生：递增 weak 计数，返回 share 同指向的 weak value。 */
value_t *ptr_make_weak_from_share(vm_t *vm, value_t *share_val);

/** share 创建：从 fatal 值接管堆块 → 分配 RC 块（payload = 堆块内容拷贝），
 *  返回 share *T value（m3-design §12.1：fatal → share 创建通道）。 */
value_t *ptr_make_share_from_fatal(vm_t *vm, value_t *fatal_val);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_PTR_ */
