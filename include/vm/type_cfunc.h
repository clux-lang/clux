#ifndef _H_CLUX_VM_TYPE_CFUNC_
#define _H_CLUX_VM_TYPE_CFUNC_

#ifdef __cplusplus
extern "C" {
#endif

#include "vm/type.h"
#include "vm/vtable.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * cfunc_type_t: **C 函数指针**类型（`cfunc(i32, i32) -> i32`）。
 *
 * 与 `func(...)`（TYPE_KIND_FUNC，clux 函数**值**，带 env/捕获）是**两个不同的
 * 类型**，彼此不可赋值、不可互相传参：
 *   - `func(...)` 的值是 `func_t*`（函数对象 + env），只在 clux 里调用；
 *   - `cfunc(...)` 的值是**裸代码指针**（无 env），只用于 FFI 边界
 *     （extern cfunc 的参数/返回值、extern var、与 opaque 互转）。
 *
 * 为什么必须分开：C 侧的函数指针没有捕获环境；把带 env 的 clux 闭包传给 C 会
 * 丢失 env（调用即错），所以类型系统必须挡住（用户要求"不互通"）。
 *
 * 内存布局：size/align = 指针宽（与 opaque 同形）。运算行为复用
 * VTABLE_OPAQUE（平凡拷贝、no-op dispose、指针相等）；类型身份独立。
 *
 * intern：按签名去重，与 func 类型共用一个池（`vm->sig_types`），按
 * kind 区分（func_sig_eq 相同但 kind 不同 → 不同类型）。
 */
typedef struct cfunc_type_t {
    type_t     base;
    func_sig_t sig;
} cfunc_type_t;

/** 类型是否为 cfunc（C 函数指针） */
static inline bool type_is_cfunc(const type_t *t) {
    return t && t->kind == TYPE_KIND_CFUNC;
}

/** 取 cfunc 类型的签名（非 cfunc 返回 NULL）。 */
const func_sig_t *cfunc_type_sig(const type_t *t);

/**
 * 按签名 intern 一个 cfunc 类型（去重；返回 vm 拥有的 `const type_t*`）。
 * `params` 为参数类型数组（借用，拷贝）；`ret` 为返回类型（NULL = void）。
 */
/** cfunc 类型的 vtable（指针语义 + type_seal 槽位，供运行期构造协议用） */
extern const vtable_t VTABLE_CFUNC;

/* 运行期构造协议（与 func 同构）：PUSH_CFUNC_TYPE 压开放类型 → 追加 → SEAL。 */
void cfunc_type_push(vm_t *vm);
void cfunc_type_add_param(vm_t *vm, const type_t *t, const type_t *param);
void cfunc_type_set_return(vm_t *vm, const type_t *t, const type_t *ret);
void cfunc_type_set_variadic(vm_t *vm, const type_t *t, bool variadic);
const type_t *cfunc_type_seal(vm_t *vm, const type_t *t);

const type_t *type_cfunc_sig(vm_t *vm, const type_t *const *params,
                             size_t param_count, const type_t *ret,
                             bool is_variadic);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_CFUNC_ */
