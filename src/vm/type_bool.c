#include "vm/type_bool.h"
#include "vm/value.h"
#include "vm/vm.h"

static value_t *bool_store(vm_t *vm, bool val) {
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    *(bool *)data = val;
    return value_make(vm, vm->type_bool, data);
}

static value_t *bool_eq(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, eq, "==");
    if (value_type(a) != vm->type_bool || value_type(b) != vm->type_bool)
        return value_make_error(vm, "==: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, value_as(a, bool) == value_as(b, bool));
}

static value_t *bool_ne(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, ne, "!=");
    if (value_type(a) != vm->type_bool || value_type(b) != vm->type_bool)
        return value_make_error(vm, "!=: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, value_as(a, bool) != value_as(b, bool));
}

static value_t *bool_lnot(vm_t *vm, value_t *a) {
    if (value_is_error(vm, a)) return a;
    if (value_type(a) != vm->type_bool)
        return value_make_error(vm, "!: type mismatch");
    if (value_is_shadow(a))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, !value_as(a, bool));
}

/* ---- clone：深拷贝 data ---- */

static value_t *bool_clone(vm_t *vm, value_t *v) {
    void *data = value_alloc_data_copy(vm->alloc, value_type(v), value_data(v));
    return value_make(vm, value_type(v), data);
}

/* ---- 赋值：类型必须匹配，memcpy ---- */

static value_t *bool_assign(vm_t *vm, value_t *dst, value_t *src) {
    (void)vm;
    if (value_type(src) != value_type(dst))
        return value_make_error(vm, "assign: bool type mismatch");
    /* shadow：只检查类型兼容性，不拷贝 data */
    if (value_is_shadow(dst) || value_is_shadow(src))
        return dst;
    *(bool *)value_data(dst) = *(const bool *)value_data(src);
    return dst;
}

/* ---- 显式转换：bool → int/float ---- */

static value_t *bool_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    /* shadow：只检查类型兼容性，返回 shadow */
    if (value_is_shadow(v)) {
        if (target == vm->type_i8 || target == vm->type_i16 ||
            target == vm->type_i32 || target == vm->type_i64 ||
            target == vm->type_u8 || target == vm->type_u16 ||
            target == vm->type_u32 || target == vm->type_u64 ||
            target == vm->type_usize ||
            target == vm->type_f32 || target == vm->type_f64)
            return value_make_shadow(vm, target);
        return value_make_error(vm, "explicit cast: incompatible target type");
    }

    bool bv = value_as(v, bool);

    if (target == vm->type_i8 || target == vm->type_i16 ||
        target == vm->type_i32 || target == vm->type_i64) {
        int64_t iv = bv ? 1 : 0;
        void *data = value_alloc_data_copy(vm->alloc, target, &iv);
        return value_make(vm, target, data);
    }

    if (target == vm->type_u8 || target == vm->type_u16 ||
        target == vm->type_u32 || target == vm->type_u64 ||
        target == vm->type_usize) {
        uint64_t uv = bv ? 1 : 0;
        void *data = value_alloc_data_copy(vm->alloc, target, &uv);
        return value_make(vm, target, data);
    }

    if (target == vm->type_f32 || target == vm->type_f64) {
        void *data = value_alloc_data(vm->alloc, target);
        if (target == vm->type_f32)
            *(float *)data = bv ? 1.0f : 0.0f;
        else
            *(double *)data = bv ? 1.0 : 0.0;
        return value_make(vm, target, data);
    }

    return value_make_error(vm, "explicit cast: incompatible target type");
}

const vtable_t VTABLE_BOOL = {
    .eq = bool_eq, .ne = bool_ne,
    .lnot = bool_lnot,
    .clone = bool_clone,
    .assign = bool_assign,
    .explicit_cast = bool_explicit_cast,
};
