#ifndef _H_CLUX_VM_TYPE_STRUCT_
#define _H_CLUX_VM_TYPE_STRUCT_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include "vm/scope.h"
#include "core/strmap.h"
#include "core/vec.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ================================================================ */
/* 结构体类型（struct_type_t，继承 type_t）                           */
/* ================================================================ */

/**
 * struct_type_t 构造对应统一两遍协议 PUSH_STRUCT / DEFINE_TYPE /
 * LOAD_TYPE / DEFINE_FIELD×N / SEAL（与 const/volatile/option/enum 同族）：
 *   - type_struct_push 分配空 struct_type（fields=NULL，不入池）+ 压其 type
 *     value，返回该 type（外部只持有 type_t*）。
 *   - type_struct_add_field 追加字段（DEFINE_FIELD 运行期用）：名从 strtable
 *     拷贝（vm 拥有），类型引用记录；密封后静默忽略。
 *   - type_struct_seal 拷贝字段表 + 按 C 对齐规则计算布局（offset/size/
 *     align）+ 按 (字段名+类型+顺序) 去重 intern + 置 sealed。
 *
 * 内存布局（密封时计算，C 对齐规则）：
 *   offset_0 = 0；offset_i = align_up(prev_end, field_i.align)
 *   size = align_up(last_end, max_align)；align = max(字段 align)
 * struct value 的 data 是连续内存块（size = type->size），字段按偏移 O(1)
 * 读写。字段类型是布局依赖（须先密封），构造期依赖后序保证。
 */

/** 取结构体类型的字段数量（非 struct 返回 0） */
static inline size_t struct_type_field_count(const type_t *t) {
    return (t && t->kind == TYPE_KIND_STRUCT) ? ((const struct_type_t *)t)->field_count
                                              : 0;
}

/** 取第 i 个字段（越界/非 struct 返回 NULL） */
static inline const struct_field_t *struct_type_field(const type_t *t, size_t i) {
    if (!t || t->kind != TYPE_KIND_STRUCT) return NULL;
    const struct_type_t *st = (const struct_type_t *)t;
    return (i < st->field_count) ? &st->fields[i] : NULL;
}

/**
 * 按名查字段（sema 构造校验 / 后续成员访问用）：命中返回下标，未找到返回 -1。
 */
int struct_type_find_field(const type_t *t, strslice_t name);

/** PUSH_STRUCT：分配空 struct_type（fields=NULL，不入池）+ 压其 type value */
const type_t *type_struct_push(vm_t *vm);

/** 分配开放 struct type（fields=NULL，不入池，**不压栈**）——union member
 * payload 内部构造用（type_union_add_field 向开放 payload struct 追加字段）。
 * 不向操作数栈压 type value；密封由 type_struct_seal 完成。 */
const type_t *type_struct_alloc_open(vm_t *vm);

/** DEFINE_FIELD 运行期用：追加字段（开放 struct；槽位所有权已编码在 ftype 上）。 */
void type_struct_add_field(vm_t *vm, const type_t *t, strslice_t name,
                           const type_t *ftype);

/** SEAL：拷贝字段表 + C 对齐布局 + 按 (字段名+类型+顺序) 去重 intern + 置 sealed。
 *
 *  **seal 是类型生命周期的分界线**（不变量，见下方 type_struct_* 写入 API）：
 *   - seal **之前**（开放类型）：字段表、嵌入表与**身份标记**（unique）**可编辑**
 *     ——结构体组合（`...Base;`，sema）与未来的装饰器（在 sema 建出 type 后改类型
 *     属性）都在这个窗口里完成；
 *   - seal **之后**：类型不可变——本文件的写入 API 一律拒绝 sealed 类型
 *     （返回 false，类型一字不改），类型/布局/元数据都只能被读取。
 *  去重规则：**标记为 unique 的类型（具名 struct 定义）跳过 intern 去重**，独立
 *  占一个 type_t 对象（见 type_struct_mark_unique）；其余按字段表去重。
 *  `type_struct_seal` 本身幂等（已 sealed 直接返回自己）。 */
const type_t *type_struct_seal(vm_t *vm, const type_t *t);

/* ===========================================================================
 * 结构体组合（`...Base;`）——seal **之前**（开放窗口）的字段/嵌入表编辑
 *
 * 用户规格（隐含规则）："在 seal 之前可以复写编辑类型的 attribute"——即类型
 * 在 seal 前是可编辑的（attribute/元数据可改写），seal 后不可变。因此结构体
 * 组合（以及未来装饰器对类型的修改）都必须在**目标类型 seal 之前**完成：
 *   1. sema 走既有开放构造路径建出 Test 的**开放** struct_type（type_struct_alloc_open）；
 *   2. 逐自有字段 type_struct_add_field；
 *   3. 把每个 `...Base`（**已 sealed**）的字段表按位置
 *      type_struct_insert_embed_fields 插进来（虚拟字段：只贡献字段表与 find_field，
 *      成员方法/静态属性不复制）；
 *   4. type_struct_set_embeds 登记嵌入表（成员继承查找用）；
 *   5. type_struct_mark_unique 标记"具名类型、身份独立"（跳过去重 intern）；
 *   6. 只 seal **一次**（type_struct_seal）——seal 时的深拷贝让两表定型。
 *
 * 由此保证：不变量"seal 后类型不可变"在整个仓库成立，组合不需要（也不允许）
 * 事后改写已 sealed 的类型；`type_t*` 身份由"同一个开放对象就地 seal"保持。
 * =========================================================================== */

/** 一个 `...Base` 嵌入的字段表插入描述（seal 前插入用）：
 *  `fields[0..field_count)` 是 Base **已 sealed** 类型此刻的字段表（名字被深
 *  拷贝、字段类型引用记录）；`layer` = 该 `...Base` 出现在 Test **自有**字段
 *  的第几个之前（0 = 所有自有字段之前，== 自有字段数 = 最后）——与
 *  ast_struct_embed_t::field_index 同义，插入位置按 layer 计算。 */
typedef struct struct_embed_fields_t {
    const struct_field_t *fields;
    size_t                field_count;
    size_t                layer;
} struct_embed_fields_t;

/** 把嵌入的字段表按 layer 插入开放 struct 类型的字段表（seal 前）。
 *  `embeds` 按 `...` 声明序传入（同一 layer 的多个嵌入保持声明序）。
 *  **sealed 类型拒绝**（返回 false，不改动任何东西）——组合必须走 seal 前窗口。
 *  字段名深拷贝（vm 拥有，与 add_field 一致），字段类型是引用（不拥有）。
 *  失败（sealed / 参数非法）返回 false。 */
bool type_struct_insert_embed_fields(vm_t *vm, const type_t *t,
                                     const struct_embed_fields_t *embeds,
                                     size_t embed_count);

/** 登记结构体组合的嵌入表（`...Base` 的类型指针，按声明序；**seal 前**设置）。
 *  成员名字查找完全依赖它（字段表是虚拟的：只贡献字段，不贡献成员）。
 *  **sealed 类型拒绝**（返回 false）。表由 vm 拥有（深拷贝指针数组）。 */
bool type_struct_set_embeds(vm_t *vm, const type_t *t,
                            const type_t *const *embeds, size_t embed_count);

/** 追加单个嵌入类型到开放 struct 的 embeds 表（seal 前可编辑，逐个追加）。
 *  **sealed 类型拒绝**（返回 false）。 */
bool type_struct_add_embed(vm_t *vm, const type_t *t, const type_t *embed);

/** 结构体组合的嵌入表（`...Base` 登记的被嵌入类型，按 `...` 出现顺序）。
 *  无嵌入（或非 struct）→ *count = 0。指针生命周期同类型对象（vm 拥有）。 */
const type_t *const *type_struct_embeds(const type_t *t, size_t *count);

/** 把开放 struct 类型标记为**独立身份**（命名 struct 定义用，**seal 之前**调用）。
 *
 *  置位后 seal **跳过去重 intern**：该类型不与类型池里字段表相同的既有 struct
 *  合并，独占一个 type_t 对象。理由是命名的 `struct A` / `struct B` 即使字段表
 *  逐位相同也是两个不同具名类型（独立成员命名空间 `A::f` / `B::f`、独立嵌入表）；
 *  合并会让显示名互相覆盖、并让结构体组合的嵌入表退化成 [X, X]（歧义判定失效）。
 *  返回 false = 参数非法 / 类型已 seal（不变量：类型属性只能在 seal 前编辑）。 */
bool type_struct_mark_unique(vm_t *vm, const type_t *t);

/** 该 struct 类型是否被标记为独立身份（非 struct / 未标记 → false）。 */
bool type_struct_is_unique(const type_t *t);

/** 一次性快捷（push + 全量字段 + seal）：sema 构造 struct 类型用。
 * 不向操作数栈压 type value。 */
const type_t *type_struct_intern(vm_t *vm, const struct_field_t *fields,
                                 size_t count);



/**
 * 结构兼容判断（鸭子类型，m2-design §2）：两 struct 类型字段名 + 字段类型
 * （type_equal，支持嵌套复合）+ 字段顺序完全一致即兼容——布局（offset/size/
 * align）由字段表唯一决定，字段兼容 ⟹ 布局相同。
 *
 * 用于跨具名类型的赋值/判等/隐式转换（struct A 与 struct B 字段一致时可
 * 互赋值）与匿名构造校验（`.{...}` 推断的匿名类型 vs 目标类型）。非 struct
 * 输入返回 false。
 */
bool struct_type_compatible(vm_t *vm, const type_t *a, const type_t *b);

/** 结构体**指针向上转换**的字段兼容判定：target（小/基）的字段是 source（大/派生）
 *  字段表的**前缀**（名字 + 类型逐一相等）→ 同一地址按 target 解释安全（C 布局自左
 *  向右，前缀字段的 offset 与类型一致）。仅用于指针转换；值转换仍要求类型完全相同
 *  （struct_type_compatible），否则会静默丢掉尾部字段。 */
bool struct_type_prefix_compatible(vm_t *vm, const type_t *target,
                                   const type_t *source);

/* ================================================================
 * 运行时成员管理（静态变量 + 方法）
 *
 * struct_type 在运行时持有自己的成员生命周期（非语法糖）。三字段协作
 * （见 type.h 的 struct_type_t 注释）：
 *   - member_scope: 借用作用域（挂在 module global_scope 下），管 value 生命周期
 *   - attributes: name → value_t* 借用映射（静态变量 + 方法值）
 *   - methods: 方法名白名单（char*，供 a.b() 解析区分方法与属性）
 *
 * 成员注册发生在 sema 类型 seal 之后、函数体遍历之前。member_scope 由调用方
 * 创建（挂在 module global_scope 下），注册到 struct_type 上（借用）。attributes
 * 和 methods 由 struct_type 拥有（struct_type_dispose 释放）。
 * ================================================================ */

/**
 * 初始化 struct_type 的运行时成员容器。创建 attributes（name→value_t* 借用映射）
 * 和 methods（char* owned 名字白名单）。member_scope 由调用方单独设置
 * （type_struct_set_member_scope）。
 * 幂等：已初始化的直接返回 true。非 struct 返回 false。 */
bool type_struct_members_init(vm_t *vm, const type_t *t);

/**
 * 设置 struct_type 的成员作用域（借用，生命周期由 module global_scope 子树管理）。
 * 调用方负责创建 scope 并挂到 module global_scope 下。NULL 清除借用。
 * 非 struct 拒绝（返回 false）。允许在 seal 后调用——运行时成员注册发生在
 * 类型 seal 之后。 */
bool type_struct_set_member_scope(vm_t *vm, const type_t *t, scope_t *scope);

/** 取 struct_type 的成员作用域（非 struct / 无成员 → NULL）。 */
scope_t *type_struct_member_scope(const type_t *t);

/**
 * 注册一个静态属性到 struct_type 的 attributes 表。
 * name 被 NUL 终止拷贝（attributes map 拥有 key），value 是借用（生命周期归
 * member_scope）。已有同名属性 → 返回 false（不覆盖，调用方负责报诊断）。
 * comptime var 成员的 const 约束由 sema 层在赋值时拦截，此处不区分。
 * 非 struct / 未初始化成员容器 → 返回 false。 */
bool type_struct_add_attribute(vm_t *vm, const type_t *t,
                               const char *name, value_t *value);

/**
 * 从 attributes 表按名查值（借用指针）。未找到 / 非 struct → NULL。
 * 不沿嵌入链查找——嵌入继承由上层（sema/VM）逐层调用本函数完成。 */
value_t *type_struct_get_attribute(const type_t *t, const char *name);

/**
 * 注册一个方法名到 methods 白名单。name 被 NUL 终止拷贝（methods vec 拥有）。
 * 方法的 func value 仍存于 attributes（通过 type_struct_add_attribute 注册），
 * methods 只记录"这个名字是方法"。已有同名 → 返回 false。
 * 非 struct / 未初始化成员容器 → 返回 false。 */
bool type_struct_add_method(vm_t *vm, const type_t *t, const char *name);

/**
 * 查询名字是否为方法（在 methods 白名单中）。非 struct / 无方法 → false。
 * 不沿嵌入链查找——嵌入继承由上层逐层调用本函数完成。 */
bool type_struct_is_method(const type_t *t, const char *name);

/** 取方法名白名单（char* 元素的 vec）。非 struct / 无方法 → NULL。 */
vec_t *type_struct_methods(const type_t *t);

/** 取 attributes 表（name→value_t* 借用映射）。非 struct / 无属性 → NULL。 */
strmap_t *type_struct_attributes(const type_t *t);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_STRUCT_ */
