#ifndef _H_CLUX_VM_VTABLE_
#define _H_CLUX_VM_VTABLE_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include <stdbool.h>
#include <stddef.h>

/* ---- Forward declarations ---- */

typedef struct vm_t    vm_t;
typedef struct type_t  type_t;
typedef struct value_t value_t;

/**
 * vtable_t: 类型行为虚表
 *
 * 所有运算通过虚表分派: a + b => a.type->vtable->add(vm, a, b)
 * 不支持的运算置 NULL，调用时触发运行时 panic。
 *
 * 所有 value 参数均为 value_t* 指针（堆分配，scope 持有生命周期）。
 */
typedef struct vtable_t {
    /* ---- 算术运算 ---- */
    value_t *(*add)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*sub)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*mul)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*div)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*mod)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*neg)(vm_t *vm, value_t *a);

    /* ---- 比较运算 ---- */
    value_t *(*eq)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*ne)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*lt)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*le)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*gt)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*ge)(vm_t *vm, value_t *a, value_t *b);

    /* ---- 位运算 ---- */
    value_t *(*band)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*bor)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*bxor)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*bnot)(vm_t *vm, value_t *a);
    value_t *(*shl)(vm_t *vm, value_t *a, value_t *b);
    value_t *(*shr)(vm_t *vm, value_t *a, value_t *b);

    /* ---- 逻辑运算 ---- */
    value_t *(*lnot)(vm_t *vm, value_t *a);

    /* ---- 调用（函数类型） ---- */
    value_t *(*call)(vm_t *vm, value_t *callee, value_t **args, size_t argc);

    /* ---- 索引 / 容器运算 ---- */
    /* 下标读取 self[index]：index 为运行时 value（整数），返回元素副本或 error */
    value_t *(*get_index)(vm_t *vm, value_t *self, value_t *index);
    /* 下标写入 self[index] = val：返回 self 或 error */
    value_t *(*set_index)(vm_t *vm, value_t *self, value_t *index, value_t *val);
    /* 长度查询：返回表示元素个数的整数 value（i64） */
    value_t *(*length)(vm_t *vm, value_t *self);

    /* ---- 类型运算（鸭子类型，type value 的 extends 运算符） ---- */
    value_t *(*extends)(vm_t *vm, value_t *a, value_t *b);

    /* ---- 生命周期 ---- */
    void    (*dispose)(vm_t *vm, value_t *v);
    value_t *(*clone)(vm_t *vm, value_t *v);
    value_t *(*assign)(vm_t *vm, value_t *dst, value_t *src);

    /* ---- 类型转换 ---- */
    value_t *(*implicit_cast)(vm_t *vm, value_t *v, const type_t *target);
    value_t *(*explicit_cast)(vm_t *vm, value_t *v, const type_t *target);

    /* ---- 类型运算（鸭子类型判断，type value 的 == / extends 代理） ---- */
    /* 由 type value（VTABLE_TYPE）的 eq/extends 运算分派到两侧类型自身：
       a->vtable->type_equal(vm, a, b)。NULL 槽位 = 默认指针比较（单例）。
       复合类型（struct/array/tuple，M2）按成员结构递归判断。 */
    bool    (*type_equal)(vm_t *vm, const type_t *a, const type_t *b);
    bool    (*type_extends)(vm_t *vm, const type_t *sub, const type_t *sup);

    /* ---- 类型密封（SEAL 命令归一：type value 构造收尾） ---- */
    /* 由 value_seal(vm, type_value) 代理调用：type_value->data 即 const type_t*，
       分派 data->vtable->type_seal。回调负责：计算规范名 + 标记 sealed +
       按类型池去重 intern（命中已有密封实现则手工回收当前 type 并返回缓存）+ 入池。
       NULL 槽位 = 该类型无开放构造阶段（如内置基础类型已 sealed），value_seal
       直接原样返回。返回密封后的 const type（可能 != self，表示复用缓存）。 */
    const type_t *(*type_seal)(vm_t *vm, const type_t *self);
} vtable_t;

/** 全零 vtable（所有函数指针为 NULL） */
extern const vtable_t VTABLE_ZERO;

/* ---- const / volatile 修饰类型 vtable（解包代理到 sub，见 type_const.c） ---- */

extern const vtable_t VTABLE_CONST;
extern const vtable_t VTABLE_VOLATILE;
extern const vtable_t VTABLE_OPTION;
extern const vtable_t VTABLE_ENUM;

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_VTABLE_ */
