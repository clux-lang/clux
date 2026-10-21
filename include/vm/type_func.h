#ifndef _H_CLUX_VM_TYPE_FUNC_
#define _H_CLUX_VM_TYPE_FUNC_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/vtable.h"
#include "vm/type.h"

/** 函数类型 vtable */
extern const vtable_t VTABLE_FUNC;

/* ================================================================ */
/* 函数签名类型（func_type_t，继承 type_t）                            */
/* ================================================================ */

/**
 * func_type_t: 函数签名类型（type_t 的扩展）
 *
 * 首成员 base 必须为 type_t（向上转型安全）。sig 携带参数/返回值信息。
 * 由 vm 类型池（vm->sig_types）intern（func_type_seal 去重），vm 拥有生命周期。
 * vm->type_func 是无签名的 func 基类（sig 全零），也是 func_type_t 布局，
 * 向下转型安全。
 *
 * 构造对应字节码协议 PUSH_FUNC_TYPE / FUNC_TYPE_PARAM / FUNC_TYPE_RETURN /
 * FUNC_TYPE_VARARG / FUNC_TYPE_SEAL：
 *   - func_type_push 分配空 func_type 并加入 vm->sig_types 池，把其 type
 *     value 压入 vm->stack，返回该 type（外部只看到 type_t*，不感知
 *     func_type_t 子类）。
 *   - func_type_add_param 追加一个参数类型；func_type_set_return 设返回类型；
 *     func_type_set_variadic 标记可变参数；三者均在 seal 前调用，操作 push
 *     返回的 type。
 *   - func_type_seal 计算规范名、按签名去重 intern、标记 sealed。
 *
 * 注意：func type 与 function 不可混淆——func type 仅描述签名；函数对象
 * （可调用实体）由 func_new / bcode_function_new / BCODE_PUSH_FUNCTION 构造，
 * 以 func type 为其 value 的 type。
 */
typedef struct func_type_t {
    type_t      base;
    func_sig_t  sig;
    /* sealed 已提升到基类 type_t（见 type.h）；密封后不可再修改 sig */
} func_type_t;

/**
 * PUSH_FUNC_TYPE（对应字节码 PUSH_FUNC_TYPE）：
 *   分配空 func_type（sig 全零），把其 type value（type_as_value）压入
 *   vm->stack，返回该 type（const type_t*）。注意：此时尚未入池，仅密封
 *   （func_type_seal，vtable type_seal）后才加入 vm->sig_types 池（去重 intern）。
 * 返回的 type 处「未密封」状态，需经 add_param / set_return / set_variadic /
 * seal 收尾。外部永远只持有 type_t*，不感知 func_type_t 子类。
 */
const type_t *func_type_push(vm_t *vm);

/** 追加一个参数类型（对应字节码 FUNC_TYPE_PARAM）；push 后、seal 前调用。
 *  t 须为 func_type_push 返回的开放 func type，sealed 后静默忽略。 */
void func_type_add_param(vm_t *vm, const type_t *t, const type_t *param);

/** 设置返回类型（对应字节码 FUNC_TYPE_RETURN）；NULL = void；push 后、seal 前调用。
 *  t 须为 func_type_push 返回的开放 func type，sealed 后静默忽略。 */
void func_type_set_return(vm_t *vm, const type_t *t, const type_t *ret);

/** 标记可变参数（对应字节码 FUNC_TYPE_VARARG）；push 后、seal 前调用。
 *  t 须为 func_type_push 返回的开放 func type，sealed 后静默忽略。 */
void func_type_set_variadic(vm_t *vm, const type_t *t, bool variadic);

/** 密封：计算规范名（"func(...)..."）、按签名去重 intern、标记 sealed，
 *  返回该 const type（允许链式）。t 须为已设 sig 的开放 func type。 */
const type_t *func_type_seal(vm_t *vm, const type_t *t);

/** 一次性构造（func_type_push + add_param* + set_return + set_variadic + seal
 *  的快捷方式，不向 vm 栈压入 type value，供 C 侧构造 func type 用）。 */
const type_t *type_func_sig(vm_t *vm, const type_t *const *params,
                            size_t param_count, const type_t *return_type,
                            bool is_variadic);

/** 取 func type 的返回类型（非 func type 返回 NULL） */
static inline const type_t *func_type_return(const type_t *t) {
    t = type_strip_ownership(t);
    return (t && t->kind == TYPE_KIND_FUNC)
               ? ((const func_type_t *)t)->sig.return_type
               : NULL;
}

/** 取 func type 的参数个数（非 func type 返回 0） */
static inline size_t func_type_param_count(const type_t *t) {
    t = type_strip_ownership(t);
    return (t && t->kind == TYPE_KIND_FUNC)
               ? ((const func_type_t *)t)->sig.param_count
               : 0;
}

/** 取 func type 第 i 个参数类型（越界 / 非 func type 返回 NULL） */
static inline const type_t *func_type_param(const type_t *t, size_t i) {
    t = type_strip_ownership(t);
    if (!t || t->kind != TYPE_KIND_FUNC) return NULL;
    const func_type_t *ft = (const func_type_t *)t;
    return (i < ft->sig.param_count) ? ft->sig.params[i] : NULL;
}

/** func type 是否可变参数（非 func type 返回 false） */
static inline bool func_type_is_variadic(const type_t *t) {
    t = type_strip_ownership(t);
    return (t && t->kind == TYPE_KIND_FUNC)
               ? ((const func_type_t *)t)->sig.is_variadic
               : false;
}

/** func type 是否已密封（非 func type 返回 false；sealed 定义在基类 type_t） */
static inline bool func_type_is_sealed(const type_t *t) {
    t = type_strip_ownership(t);
    return type_is_sealed(t);
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_FUNC_ */
