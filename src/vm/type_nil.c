#include "vm/type_nil.h"
#include "vm/value.h"
#include "vm/vm.h"

#include <string.h>

/* ---- bool 值构造（与其他 vtable 同模式） ---- */

static value_t *bool_store(vm_t *vm, bool val) {
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    *(bool *)data = val;
    return value_make(vm, vm->type_bool, data);
}

/* ---- clone：平凡拷贝 func_t* 宽度零块 ---- */

static value_t *nil_clone(vm_t *vm, value_t *v) {
    void *data = value_alloc_data_copy(vm->alloc, value_type(v), value_data(v));
    return value_make(vm, value_type(v), data);
}

/* ---- assign：同类型 memcpy（nil 赋值 nil，恒等） ---- */

static value_t *nil_assign(vm_t *vm, value_t *dst, value_t *src) {
    (void)vm;
    if (value_type(src) != value_type(dst))
        return value_make_error(vm, "assign: nil type mismatch");
    if (value_is_shadow(dst) || value_is_shadow(src))
        return dst;
    memcpy(value_data(dst), value_data(src), value_type(dst)->size);
    return dst;
}

/* ---- eq/ne：nil == nil 恒真；nil == func 比较函数指针是否为 NULL ---- */

static value_t *nil_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_error(vm, a)) return a;
    if (value_is_error(vm, b)) return b;
    bool b_nil  = (value_type(b) == vm->type_nil);
    bool b_func = (value_type(b)->kind == TYPE_KIND_FUNC);
    if (!b_nil && !b_func)
        return value_make_error(vm, "==: cannot compare nil with this type");
    if (b_nil) return bool_store(vm, true); /* nil == nil */
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    /* nil == func：func 指针是否为 NULL（nil 的 0 初始化语义） */
    const func_t *fn = *(const func_t **)value_data(b);
    return bool_store(vm, fn == NULL);
}

static value_t *nil_ne(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_error(vm, a)) return a;
    if (value_is_error(vm, b)) return b;
    bool b_nil  = (value_type(b) == vm->type_nil);
    bool b_func = (value_type(b)->kind == TYPE_KIND_FUNC);
    if (!b_nil && !b_func)
        return value_make_error(vm, "!=: cannot compare nil with this type");
    if (b_nil) return bool_store(vm, false);
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const func_t *fn = *(const func_t **)value_data(b);
    return bool_store(vm, fn != NULL);
}

/* ---- 类型转换 ---- */

/* nil → 任意 func 类型：身份复制（data 即 func_t* 宽度的 NULL 指针）。
   隐式与显式都支持——函数 0 初始化（var f: func(...) = nil / f = nil）。 */
static value_t *nil_to_func(vm_t *vm, value_t *v, const type_t *target) {
    if (target->kind != TYPE_KIND_FUNC)
        return value_make_error(vm, "cast: nil can only convert to func or u64");
    if (value_is_shadow(v))
        return value_make_shadow(vm, target);
    void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
    return value_make(vm, target, data);
}

static value_t *nil_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    return nil_to_func(vm, v, target);
}

/* nil → u64(0)：显式转换（用户要求"显式的转换为 u64(0)"） */
static value_t *nil_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (target == vm->type_u64) {
        if (value_is_shadow(v))
            return value_make_shadow(vm, vm->type_u64);
        void *data = value_alloc_data(vm->alloc, vm->type_u64);
        *(uint64_t *)data = 0;
        return value_make(vm, vm->type_u64, data);
    }
    if (target->kind == TYPE_KIND_FUNC)
        return nil_to_func(vm, v, target);
    return value_make_error(vm, "cast: nil can only convert to u64 or func");
}

const vtable_t VTABLE_NIL = {
    .eq      = nil_eq,
    .ne      = nil_ne,
    .clone   = nil_clone,
    .assign  = nil_assign,
    .implicit_cast = nil_implicit_cast,
    .explicit_cast = nil_explicit_cast,
};
