#ifndef _H_CLUX_VM_TYPE_ARRAY_
#define _H_CLUX_VM_TYPE_ARRAY_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include "vm/type_ownership.h"

/** 数组类型 vtable */
extern const vtable_t VTABLE_ARRAY;

/**
 * 构造数组 value：将 count 个元素（全为 elem_type，或可被隐式转换到
 * elem_type）深拷贝进新建连续元素块，类型 = type_array_intern(elem_type,
 * count)。返回数组 value（auto-track 到 vm->current_scope）或 error value。
 */
value_t *value_make_array(vm_t *vm, const type_t *elem_type,
                          value_t **elems, size_t count);

/** 带**元素槽位所有权**的数组值构造：构造出的数组类型与 (elem, count,
 *  elem_own) 去重一致（`[N]own *T` 的元素按 own 销毁）。 */
value_t *value_make_array_own(vm_t *vm, const type_t *elem_type,
                              value_t **elems, size_t count,
                              ownership_t elem_ownership);

/* ================================================================ */
/* 数组类型（array_type_t，继承 type_t）                              */
/* ================================================================ */

/**
 * array_type_t: 数组类型（type_t 的扩展）
 *
 * 首成员 base 必须为 type_t（向上转型安全）。elem_type 为元素类型，
 * length 为编译期元素数量（SIZE_MAX 表示动态/未定长切片）。
 *
 * 构造对应 M2 字节码协议 push_array / load <elem> / define_bound N / seal：
 *   - array_type_push 分配空 array_type 并加入 vm->array_types 池，把其
 *     type value 压入 vm->stack，返回该 type（外部只看到 type_t*，不感知
 *     array_type_t 子类）。
 *   - array_type_set_elem 设元素（基本）类型；array_type_set_count 设元素
 *     数量并标记 formed；二者均在 seal 前调用，操作对象为 push 返回的 type。
 *   - array_type_seal 计算内存布局（layout_size/layout_align），标记 sealed，
 *     并按 (elem_type, length) 去重（命中已有 sealed 类型则复用并释放本开放类型）。
 *
 * 内存布局（密封时计算）：数组 value 的 data 是**连续元素块**（C 数组语义，
 * M2 设计 §2），元素按固定偏移排列：
 *   layout_size = base.size  = elem_type->size * length（定长）
 *   layout_align = base.align = elem_type->align
 * 借用引用 arr[i] 的 data = arr.data + i * elem_type->size（业务内存）。
 * length==SIZE_MAX（动态）无静态布局，size=0。
 * 按结构等价判定类型兼容（见 m2-design §11）。
 */
typedef struct array_type_t {
    type_t       base;
    const type_t *elem_type;  /* 元素类型（引用，不拥有）；由 set_elem 设定 */
    size_t        length;     /* 编译期元素数量；SIZE_MAX = 动态/未定长切片 */
    bool          formed;     /* 是否已定长成形（set_elem + set_count 后为真） */
    /* 该 **元素槽位** 的所有权（合法性说明同 struct_field_t::ownership）：
       `[N]own *T` 的元素是 own 叶子；销毁序列 `for i in 0..N` 要据此决定是否
       FREE `a[i]`。参与去重 intern（own 元素与 ref 元素是不同类型）。 */
    ownership_t   elem_ownership;
    /* sealed 已提升到基类 type_t（见 type.h）；密封后不可再修改 elem/length */
    size_t        layout_size;  /* 密封后：元素总字节数 = elem_type->size*length */
    size_t        layout_align; /* 密封后：对齐 = elem_type->align */
} array_type_t;

/**
 * push_array（对应字节码 push_array）：
 *   分配空 array_type，把其 type value（type_as_value）压入 vm->stack，返回该
 *   type（const type_t*）。注意：此时尚未入池，仅密封（array_type_seal，vtable
 *   type_seal）后才加入 vm->array_types 池（去重 intern）。
 * 返回的 type 处「未成形、未密封」状态，需经 set_elem / set_count / seal 收尾。
 * 外部永远只持有 type_t*，不感知 array_type_t 子类。
 */
const type_t *array_type_push(vm_t *vm);

/** 设置元素（基本）类型（对应字节码 load <elem>）；在 set_count / seal 前调用。
 *  t 须为 array_type_push 返回的开放数组类型，重复调用 / sealed 后静默忽略。 */
void array_type_set_elem(vm_t *vm, const type_t *t, const type_t *elem_type);

/** SET_FIELD_OWNERSHIP 运行期用（array 分支）：设元素槽位的所有权（seal 前）。 */
void array_type_set_elem_ownership(vm_t *vm, const type_t *t, ownership_t own);

/** 设置元素数量（对应字节码 define_bound N），标记 formed；仅一次有效，
 *  重复调用 / sealed 后静默忽略。t 须为 array_type_push 返回的开放数组类型。 */
void array_type_set_count(vm_t *vm, const type_t *t, size_t count);

/** 密封：计算内存布局（layout_size/layout_align），按 (elem_type,length) 去重
 *  intern，标记 sealed，返回该 const type（允许链式）。t 须为已设 elem 的开放
 * 数组类型（动态切片可省略 set_count，length 保持 SIZE_MAX）。 */
const type_t *array_type_seal(vm_t *vm, const type_t *t);

/** 一次性构造（push_array + set_elem + set_count + seal 的快捷方式） */
const type_t *type_array_intern(vm_t *vm, const type_t *elem_type, size_t count);

/** 带**元素槽位所有权**的一次性构造：`[N]own *T` / `[N]ref *T` 等。
 *  所有权在 seal 之前写入并参与去重 intern。 */
const type_t *type_array_intern_own(vm_t *vm, const type_t *elem_type,
                                    size_t count, ownership_t elem_ownership);

/** 带 ownership 的一次性构造：与 type_array_intern 同，但数组类型的
 *  ownership 在 seal **之前**设为 ownership。用于 `fatal [N]T`——含 own
 *  元素（如 [N]Box）的定长数组跨函数边界的将亡值变体。ownership 恒为
 *  OWN_NONE，唯一合法取值是 OWN_FATAL（去重按 (elem,length,ownership) 区分）。 */
const type_t *type_array_intern_with_ownership(vm_t *vm, const type_t *elem_type,
                                               size_t count,
                                               ownership_t ownership);

/** 取数组类型的元素类型（非数组类型返回 NULL） */
static inline const type_t *array_type_elem(const type_t *t) {
    t = type_strip_ownership(t);
    return (t && t->kind == TYPE_KIND_ARRAY) ? ((const array_type_t *)t)->elem_type
                                             : NULL;
}

/** 取数组类型元素槽位的所有权（非数组类型返回 OWN_NONE） */
static inline ownership_t array_type_elem_ownership(const type_t *t) {
  return type_kind_of(t) == TYPE_KIND_ARRAY ? type_ownership_of(array_type_elem(t)) : OWN_NONE;
}

/** 取数组类型的编译期元素数量（动态/未定长返回 SIZE_MAX；非数组返回 0） */
static inline size_t array_type_len(const type_t *t) {
    t = type_strip_ownership(t);
    return (t && t->kind == TYPE_KIND_ARRAY) ? ((const array_type_t *)t)->length : 0;
}

/** 数组类型是否已密封（非数组类型返回 false；sealed 定义在基类 type_t） */
static inline bool array_type_is_sealed(const type_t *t) {
    t = type_strip_ownership(t);
    return type_is_sealed(t) && t->kind == TYPE_KIND_ARRAY;
}

/** 取数组类型的编译期内存布局总字节数（未密封/非数组返回 0） */
static inline size_t array_type_layout_size(const type_t *t) {
    t = type_strip_ownership(t);
    return (t && t->kind == TYPE_KIND_ARRAY) ? ((const array_type_t *)t)->layout_size
                                             : 0;
}

/** 取数组类型的编译期对齐（未密封/非数组返回 0） */
static inline size_t array_type_layout_align(const type_t *t) {
    t = type_strip_ownership(t);
    return (t && t->kind == TYPE_KIND_ARRAY) ? ((const array_type_t *)t)->layout_align
                                             : 0;
}

/* ---- 运行期数组 value 只读访问（供 value_dump 等调试遍历） ---- */

/** 前置声明：避免为仅消费指针的访问器引入整个 value.h */
typedef struct value_t value_t;

/** 返回数组 value 的元素个数（借用/普通一致，从类型取；非数组 value 返回 0） */
size_t value_array_count(const value_t *v);

/**
 * 返回第 i 个元素的借用引用（data 指向块内偏移业务内存，type = 元素类型），
 * 越界返回 NULL。供 value_dump 等调试遍历递归使用；借用值 auto-track
 * 到 vm->current_scope。
 */
value_t *value_array_at(vm_t *vm, const value_t *v, size_t i);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_ARRAY_ */
