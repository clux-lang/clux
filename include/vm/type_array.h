#ifndef _H_CLUX_VM_TYPE_ARRAY_
#define _H_CLUX_VM_TYPE_ARRAY_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"

/** 数组类型 vtable */
extern const vtable_t VTABLE_ARRAY;

/**
 * 构造数组 value：将 count 个元素（全为 elem_type，或可被隐式转换到
 * elem_type）clone 进新建数组，类型 = type_array_intern(elem_type, count)。
 * 返回数组 value（auto-track 到 vm->current_scope）或 error value。
 */
value_t *value_make_array(vm_t *vm, const type_t *elem_type,
                          value_t **elems, size_t count);

/** 向数组末尾追加一个元素（类型检查 + clone 到当前作用域） */
void array_push(vm_t *vm, value_t *arr, value_t *elem);

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
 * 内存布局（密封时计算，运行时值仍由内部 vec 承载）：
 *   layout_size  = elem_type->size * length（length==SIZE_MAX 时为 0，动态数组无静态布局）
 *   layout_align = elem_type->align
 * base.size / base.align 始终为运行时值存储（array_data_t）的大小与对齐，
 * 与 layout_* 区分：layout_* 用于 sizeof / 结构体内存布局等编译期计算。
 * 按结构等价判定类型兼容（见 m2-design §11）。
 */
typedef struct array_type_t {
    type_t       base;
    const type_t *elem_type;  /* 元素类型（引用，不拥有）；由 set_elem 设定 */
    size_t        length;     /* 编译期元素数量；SIZE_MAX = 动态/未定长切片 */
    bool          formed;     /* 是否已定长成形（set_elem + set_count 后为真） */
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

/** 设置元素数量（对应字节码 define_bound N），标记 formed；仅一次有效，
 *  重复调用 / sealed 后静默忽略。t 须为 array_type_push 返回的开放数组类型。 */
void array_type_set_count(vm_t *vm, const type_t *t, size_t count);

/** 密封：计算内存布局（layout_size/layout_align），按 (elem_type,length) 去重
 *  intern，标记 sealed，返回该 const type（允许链式）。t 须为已设 elem 的开放
 * 数组类型（动态切片可省略 set_count，length 保持 SIZE_MAX）。 */
const type_t *array_type_seal(vm_t *vm, const type_t *t);

/** 一次性构造（push_array + set_elem + set_count + seal 的快捷方式） */
const type_t *type_array_intern(vm_t *vm, const type_t *elem_type, size_t count);

/** 取数组类型的元素类型（非数组类型返回 NULL） */
static inline const type_t *array_type_elem(const type_t *t) {
    return (t && t->kind == TYPE_KIND_ARRAY) ? ((const array_type_t *)t)->elem_type
                                             : NULL;
}

/** 取数组类型的编译期元素数量（动态/未定长返回 SIZE_MAX；非数组返回 0） */
static inline size_t array_type_len(const type_t *t) {
    return (t && t->kind == TYPE_KIND_ARRAY) ? ((const array_type_t *)t)->length : 0;
}

/** 数组类型是否已密封（非数组类型返回 false；sealed 定义在基类 type_t） */
static inline bool array_type_is_sealed(const type_t *t) {
    return type_is_sealed(t) && t->kind == TYPE_KIND_ARRAY;
}

/** 取数组类型的编译期内存布局总字节数（未密封/非数组返回 0） */
static inline size_t array_type_layout_size(const type_t *t) {
    return (t && t->kind == TYPE_KIND_ARRAY) ? ((const array_type_t *)t)->layout_size
                                             : 0;
}

/** 取数组类型的编译期对齐（未密封/非数组返回 0） */
static inline size_t array_type_layout_align(const type_t *t) {
    return (t && t->kind == TYPE_KIND_ARRAY) ? ((const array_type_t *)t)->layout_align
                                             : 0;
}

/* ---- 运行期数组 value 只读访问（供调试/格式化遍历，如 printf %v） ---- */

/** 前置声明：避免为仅消费指针的访问器引入整个 value.h */
typedef struct value_t value_t;

/** 返回数组 value 的元素个数（非数组 value 返回 0） */
size_t value_array_count(const value_t *v);

/** 返回第 i 个元素（只读借用，不 clone；元素由 scope 管理生命周期），越界返回 NULL */
const value_t *value_array_at(const value_t *v, size_t i);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_ARRAY_ */
