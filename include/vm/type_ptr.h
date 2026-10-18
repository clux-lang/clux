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

/** 指针类型 vtable（所有权纯编译期，类型不区分 own/ref/fatal） */
extern const vtable_t VTABLE_PTR;
extern const vtable_t VTABLE_PTR_SHARE;  /* share *T：copy=+1，drop=-1 */
extern const vtable_t VTABLE_PTR_WEAK;   /* weak *T：copy=+1，drop=-1（弱计数） */

/* ================================================================ */
/* 指针类型（ptr_type_t，继承 type_t，m3-design §3）                 */
/* ================================================================ */

/**
 * ptr_type_t 构造对应统一两遍协议 PUSH_PTR / DEFINE_TYPE / LOAD_TYPE /
 * SET_TYPE（设 base_type）/ SEAL（与 func 签名类型同族）：
 *   - type_ptr_push 分配空 ptr_type（base_type=NULL，不入池）+ 压其 type
 *     value，返回该 type（外部只持有 type_t*）。
 *   - type_ptr_set_base 设被指向类型 T（SET_TYPE 运行期用）；密封后静默忽略。
 *   - type_ptr_seal 按 base_type 指针去重 intern（同一 base 的三种所有权是
 *     三个独立实例），标记 sealed。
 *
 * 指针类型的 size/align 恒定（指针宽），密封不依赖 base_type 布局——hoist
 * 中属引用依赖（emit_ref_type，LOAD_TYPE 拉回 base，不递归），与 func
 * 签名类型同族。
 */
typedef struct ptr_type_t ptr_type_t;  /* 定义见 vm/type.h */

/** PUSH_PTR：分配开放 ptr_type（base_type=NULL，不入池）+ 压其 type value。
 *  所有权纯编译期——类型不区分 own/ref/fatal，只有一种指针类型 *T。 */
const type_t *type_ptr_push(vm_t *vm, uint8_t rc);

/** SET_TYPE 运行期用：设被指向类型 T（密封后静默忽略） */
void type_ptr_set_base(vm_t *vm, const type_t *t, const type_t *base);

/** SEAL：按 base_type 去重 intern + 置 sealed。返回该 const type */
const type_t *type_ptr_seal(vm_t *vm, const type_t *t);

/** 一次性快捷（type_ptr_push + set_base + seal）：sema 解析 *T 用。
 *  不向操作数栈压 type value（嵌套构造场景避免栈布局污染）。
 *  所有权纯编译期——类型不区分 own/ref/fatal。 */
const type_t *type_ptr_intern(vm_t *vm, const type_t *base);

/** 取/建 RC 指针类型：rc = 1（share）/ 2（weak），0 等同 type_ptr_intern。 */
const type_t *type_ptr_intern_rc(vm_t *vm, const type_t *base, uint8_t rc);

/** 是否为 share / weak 指针类型（RC 形态）。 */
static inline bool type_is_share(const type_t *t) {
    return t && t->kind == TYPE_KIND_PTR && ((const ptr_type_t *)t)->rc == 1;
}
static inline bool type_is_weak(const type_t *t) {
    return t && t->kind == TYPE_KIND_PTR && ((const ptr_type_t *)t)->rc == 2;
}
/** RC 指针的共享形态：share/weak 都指向 RC 块内的 payload。 */
static inline bool type_is_rc_ptr(const type_t *t) {
    return type_is_share(t) || type_is_weak(t);
}

/* ---- RC 控制块（share/weak，m3-design §12）---- */

/** RC 块：头部（强/弱计数）紧跟 payload（T 的数据）；share/weak 的 ptr 字段
 *  指向 payload（于是 `p.*` 语义不变）。计数是 atomic（§12.5）。 */
typedef struct rc_block_t {
    /* 计数（atomic；原子操作由 type_ptr.c 用 __atomic_* 内建完成，
       这样头文件在 C / C++ 两种语言下都能编译）。 */
    unsigned      strong;    /* share 计数 */
    unsigned      weak;      /* weak 计数 + 1（strong 自占一个 weak 引用） */
    allocator_t  *alloc;
    const type_t *type;      /* payload 类型（析构递归用） */
    bool          destroyed; /* strong→0 后 payload 已析构 */
} rc_block_t;

/** payload 地址（头部之后按 type->align 对齐） */
static inline void *rc_block_payload(rc_block_t *rc) {
    size_t hdr = sizeof(rc_block_t);
    size_t align = (rc && rc->type && rc->type->align) ? rc->type->align : 1;
    size_t aligned = (hdr + align - 1) & ~(align - 1);
    return (uint8_t *)rc + aligned;
}

/** 由 payload 反查控制块（与 rc_block_payload 同一对齐公式） */
static inline rc_block_t *rc_block_of(const type_t *t, void *payload) {
    size_t hdr = sizeof(rc_block_t);
    size_t align = (t && t->align) ? t->align : 1;
    size_t aligned = (hdr + align - 1) & ~(align - 1);
    return (rc_block_t *)((uint8_t *)payload - aligned);
}

rc_block_t *rc_block_new(vm_t *vm, const type_t *payload_type);
void rc_block_strong_inc(rc_block_t *rc);
void rc_block_strong_dec(vm_t *vm, rc_block_t *rc);
void rc_block_weak_inc(rc_block_t *rc);
void rc_block_weak_dec(vm_t *vm, rc_block_t *rc);
/** 原子升级：strong>0 则 +1 并返回 payload，否则 NULL。 */
void *rc_block_upgrade(rc_block_t *rc);

/** `own/fatal *T` → `share *T`（fatal 创建通道，§12.1）：新建 RC 块（payload
 *  = 被指向值拷贝，接管其内的资源所有权），原堆块释放。 */
value_t *ptr_make_share_from_fatal(vm_t *vm, value_t *fatal_val);
/** `share *T` → `weak *T`（弱计数 +1）。 */
value_t *ptr_make_weak_from_share(vm_t *vm, value_t *share_val);

/** 取指针类型的被指向类型 T（非指针返回 NULL） */
static inline const type_t *ptr_type_base(const type_t *t) {
    return type_is_ptr(t) ? ((const ptr_type_t *)t)->base_type : NULL;
}

/* ================================================================ */
/* 指针值布局（m3-design §7/§8）                                     */
/* ================================================================ */

/**
 * 指针值（data 块布局）。三种指针的 value data 统一为 { 裸指针 }：
 *   - ptr：指向目标内存（new 堆块 / 栈上值 / 借用目标）
 *   - 所有权由 type_t.ownership 区分（m3-design §13.2：运行期零标志）：
 *     own *T    → 总拥有堆块，dispose 释放（new 产物接管后）
 *     ref *T    → 总借用，dispose 跳过（指向栈值 / 他人堆块）
 *     fatal *T  → ptr≠NULL 时拥有（将亡值），转移后 ptr=NULL（接管方拥有）
 *
 * size = sizeof(ptr_value_t)（对齐到指针宽），type_ptr_seal 设定。
 */
typedef struct ptr_value_t {
    void *ptr;
} ptr_value_t;

/** 读指针值目标地址（非指针 value 返回 NULL）。 */
static inline void *ptr_value_target(const value_t *v) {
    if (!v) return NULL;
    const type_t *t = value_type(v);
    if (!type_is_ptr(t))
        return NULL;
    return ((const ptr_value_t *)value_data(v))->ptr;
}

/** 构造指针值：目标地址（三种指针统一，所有权由 type_t.ownership 区分，
 *  m3-design §13.2 运行期零标志） */
value_t *ptr_make_value(vm_t *vm, const type_t *pt, void *target);

/* ================================================================ */
/* 递归销毁（m3-design §3.1/§13：own 销毁沿字段递归）                */
/* ================================================================ */

/**
 * 递归释放 data 块内所有 **own** 指针指向的堆内存（m3-design §3.1：
 * `own *Struct` 且 Struct 含 own 字段时，销毁递归传导）。
 *
 * 纯静态分派（type + 裸 data 块，不依赖 value_t 包装）：
 *   - own *T：ptr≠NULL → 先递归销毁堆块内容（T 为含 own 字段的复合类型时
 *     内嵌堆块一并释放），再释放本堆块，置 ptr=NULL（防双释放）
 *   - **fatal *T：不释放**（所有权体系不变量——fatal 必被接受方接管或被
 *     `_ = ` 丢弃，堆块归接管方/dispose 路径；销毁路径碰它就是 double-free）
 *   - ref *T：借用，跳过（指向栈值或他人堆块，不拥有）
 *   - struct/tuple：按字段/元素偏移递归（**与 ownership 无关**，故 fatal 复合
 *     类型与其 base 走同一条路径）
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
/** 目标类型是否含需递归扫描的 own 字段（own 指针 + 含 own 的
 *  复合类型 struct/tuple/array/option/union/slice_own）。**fatal 不在此列**：
 *  将亡值必被接受方接管（或 `_ = ` 丢弃），销毁路径永不碰它指向的堆块。
 *
 *  供 ptr_free_owned_recursive / ptr_clear_owned_recursive 判定递归入口，
 *  切片类型系统（type_slice.c）通过 slice_type_needs_scan 委托此函数。 */
bool ptr_type_needs_scan(const type_t *t);

void ptr_free_owned_recursive(vm_t *vm, const type_t *t, void *data);

/**
 * 按**槽位所有权**销毁一个槽位里的旧值（就地覆盖前调用）：
 *   - 叶子（ptr / slice）：仅 `own == OWN_OWN` 时释放该堆块；
 *   - 复合（struct/tuple/union/…）：内部 own 槽位归本值所有 → 递归展开
 *     （内层 ref 槽位一律跳过）。
 *
 * 与 sema 的 gen_destroy 同一规则（type 对象是唯一权威）。供 FIELD_SET 等
 * "覆盖旧值"的运行期路径使用——sema 侧的赋值点销毁序列尚未实现（见
 * docs/ownership-refactor.md §7），但这些路径的旧值不能直接丢弃，否则
 * 旧值里的 own 堆块再也找不到 → 泄漏。
 */
void ptr_free_slot_value(vm_t *vm, const type_t *slot_type, ownership_t own,
                         void *data);

/**
 * 只释放**这一个**指针/切片的堆块，**不递归**被指向值/元素。
 *
 * 供 `FREE` 指令使用：A 方案下销毁序列完全由 sema 按 AST 生成（被指向值的
 * own 槽位由 `FREE v.f` 逐条释放），运行期**不再**按类型递归扫描。旧的递归版
 * 会沿被指向类型无差别递归**所有**指针字段（含 `ref` 槽位）——聚合一旦能携带
 * ref 指针，就会把借用目标的栈地址也 free 掉（实测 0xC0000005）。
 */
void ptr_free_block_only(vm_t *vm, const type_t *t, void *data);

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

/** 把数据块里的**每一个 share/weak 槽位**计数 +1（拷贝进新槽位 = 引用 +1）。
 *  构造 / 字段写入 / 闭包捕获 / 数组元素等 memcpy 之后调用；own 指针不参与
 *  （那是转移，源会被清空）。 */
void ptr_rc_inc_recursive(vm_t *vm, const type_t *t, void *data);

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

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_PTR_ */
