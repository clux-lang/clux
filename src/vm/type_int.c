#include "vm/type_int.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/panic.h"

/* ================================================================ */
/* 类型分类辅助                                                      */
/* ================================================================ */

typedef enum {
    RANK_NONE = 0,
    RANK_BOOL = 1,
    RANK_I8   = 2,
    RANK_U8   = 3,
    RANK_I16  = 4,
    RANK_U16  = 5,
    RANK_I32  = 6,
    RANK_U32  = 7,
    RANK_I64  = 8,
    RANK_U64  = 9,
    RANK_F32  = 10,
    RANK_F64  = 11,
} type_rank_t;

static type_rank_t type_rank(const vm_t *vm, const type_t *t) {
    if (t == vm->type_bool) return RANK_BOOL;
    if (t == vm->type_i8)   return RANK_I8;
    if (t == vm->type_u8)   return RANK_U8;
    if (t == vm->type_i16)  return RANK_I16;
    if (t == vm->type_u16)  return RANK_U16;
    if (t == vm->type_i32)  return RANK_I32;
    if (t == vm->type_u32)  return RANK_U32;
    if (t == vm->type_i64)  return RANK_I64;
    if (t == vm->type_u64)  return RANK_U64;
    if (t == vm->type_f32)  return RANK_F32;
    if (t == vm->type_f64)  return RANK_F64;
    return RANK_NONE;
}

static bool is_signed_int(const vm_t *vm, const type_t *t) {
    return t == vm->type_i8 || t == vm->type_i16 ||
           t == vm->type_i32 || t == vm->type_i64;
}

static bool is_unsigned_int(const vm_t *vm, const type_t *t) {
    return t == vm->type_u8 || t == vm->type_u16 ||
           t == vm->type_u32 || t == vm->type_u64;
}

static bool is_int_type(const vm_t *vm, const type_t *t) {
    return is_signed_int(vm, t) || is_unsigned_int(vm, t);
}

static bool is_float_type(const vm_t *vm, const type_t *t) {
    return t == vm->type_f32 || t == vm->type_f64;
}

/* ================================================================ */
/* 按类型宽度读写                                                    */
/* ================================================================ */

/* 按宽度读取有符号整数，符号扩展到 int64_t */
static int64_t sint_read(const value_t *v) {
    switch (value_type(v)->size) {
        case 1: return (int64_t)*(const int8_t  *)value_data(v);
        case 2: return (int64_t)*(const int16_t *)value_data(v);
        case 4: return (int64_t)*(const int32_t *)value_data(v);
        default: return *(const int64_t *)value_data(v);
    }
}

/* 按宽度读取无符号整数，零扩展到 uint64_t */
static uint64_t uint_read(const value_t *v) {
    switch (value_type(v)->size) {
        case 1: return (uint64_t)*(const uint8_t  *)value_data(v);
        case 2: return (uint64_t)*(const uint16_t *)value_data(v);
        case 4: return (uint64_t)*(const uint32_t *)value_data(v);
        default: return *(const uint64_t *)value_data(v);
    }
}

/* 按类型符号读取，扩展到 int64_t（共享运算用） */
static int64_t int_read(const vm_t *vm, const value_t *v) {
    if (is_unsigned_int(vm, value_type(v)))
        return (int64_t)uint_read(v);
    return sint_read(v);
}

/* 按目标类型宽度存储（截断到 type->size 字节） */
static value_t *int_store(vm_t *vm, const type_t *type, uint64_t val) {
    void *data = value_alloc_data(vm->alloc, type);
    switch (type->size) {
        case 1: *(uint8_t  *)data = (uint8_t)val;  break;
        case 2: *(uint16_t *)data = (uint16_t)val; break;
        case 4: *(uint32_t *)data = (uint32_t)val; break;
        default: *(uint64_t *)data = val;          break;
    }
    return value_make(vm, type, data);
}

/* 按目标类型宽度存储 bool 结果 */
static value_t *bool_store(vm_t *vm, bool val) {
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    *(bool *)data = val;
    return value_make(vm, vm->type_bool, data);
}

/* ================================================================ */
/* clone：按宽度深拷贝 data                                           */
/* ================================================================ */

static value_t *int_clone(vm_t *vm, value_t *v) {
    void *data = value_alloc_data_copy(vm->alloc, value_type(v), value_data(v));
    return value_make(vm, value_type(v), data);
}

/* ================================================================ */
/* 赋值：向左值类型转换 + memcpy                                      */
/* ================================================================ */

static value_t *int_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_type(src) != value_type(dst)) {
        /* 向左值类型 implicit_cast */
        value_t *casted = value_implicit_cast(vm, src, value_type(dst));
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    /* shadow：只检查类型兼容性，不拷贝 data */
    if (value_is_shadow(dst) || value_is_shadow(src))
        return dst;
    memcpy(value_data(dst), value_data(src), value_type(dst)->size);
    return dst;
}

/* ================================================================ */
/* 共享算术/位运算（bit-level 相同，signed/unsigned 共用）            */
/* ================================================================ */

static value_t *int_add(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, add, "+");
    if (!is_int_type(vm, value_type(a)))
        return value_make_error(vm, "+: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    return int_store(vm, value_type(a), (uint64_t)(int_read(vm, a) + int_read(vm, b)));
}

static value_t *int_sub(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, sub, "-");
    if (!is_int_type(vm, value_type(a)))
        return value_make_error(vm, "-: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    return int_store(vm, value_type(a), (uint64_t)(int_read(vm, a) - int_read(vm, b)));
}

static value_t *int_mul(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, mul, "*");
    if (!is_int_type(vm, value_type(a)))
        return value_make_error(vm, "*: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    return int_store(vm, value_type(a), (uint64_t)(int_read(vm, a) * int_read(vm, b)));
}

static value_t *int_eq(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, eq, "==");
    if (!is_int_type(vm, value_type(a)))
        return value_make_error(vm, "==: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, int_read(vm, a) == int_read(vm, b));
}

static value_t *int_ne(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, ne, "!=");
    if (!is_int_type(vm, value_type(a)))
        return value_make_error(vm, "!=: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, int_read(vm, a) != int_read(vm, b));
}

static value_t *int_band(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, band, "&");
    if (!is_int_type(vm, value_type(a)))
        return value_make_error(vm, "&: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    return int_store(vm, value_type(a), (uint64_t)(int_read(vm, a) & int_read(vm, b)));
}

static value_t *int_bor(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, bor, "|");
    if (!is_int_type(vm, value_type(a)))
        return value_make_error(vm, "|: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    return int_store(vm, value_type(a), (uint64_t)(int_read(vm, a) | int_read(vm, b)));
}

static value_t *int_bxor(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, bxor, "^");
    if (!is_int_type(vm, value_type(a)))
        return value_make_error(vm, "^: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    return int_store(vm, value_type(a), (uint64_t)(int_read(vm, a) ^ int_read(vm, b)));
}

static value_t *int_bnot(vm_t *vm, value_t *a) {
    if (value_is_error(vm, a)) return a;
    if (!is_int_type(vm, value_type(a)))
        return value_make_error(vm, "~: type mismatch");
    if (value_is_shadow(a))
        return value_make_shadow(vm, value_type(a));
    return int_store(vm, value_type(a), (uint64_t)(~int_read(vm, a)));
}

static value_t *int_shl(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, shl, "<<");
    if (!is_int_type(vm, value_type(a)))
        return value_make_error(vm, "<<: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    return int_store(vm, value_type(a), (uint64_t)(int_read(vm, a) << int_read(vm, b)));
}

/* ================================================================ */
/* 有符号特有运算                                                    */
/* ================================================================ */

static value_t *sint_div(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, div, "/");
    if (!is_signed_int(vm, value_type(a)))
        return value_make_error(vm, "/: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    int64_t bv = sint_read(b);
    if (bv == 0) panic("division by zero");
    return int_store(vm, value_type(a), (uint64_t)(sint_read(a) / bv));
}

static value_t *sint_mod(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, mod, "%");
    if (!is_signed_int(vm, value_type(a)))
        return value_make_error(vm, "%: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    int64_t bv = sint_read(b);
    if (bv == 0) panic("modulo by zero");
    return int_store(vm, value_type(a), (uint64_t)(sint_read(a) % bv));
}

static value_t *sint_neg(vm_t *vm, value_t *a) {
    if (value_is_error(vm, a)) return a;
    if (!is_signed_int(vm, value_type(a)))
        return value_make_error(vm, "-: type mismatch");
    if (value_is_shadow(a))
        return value_make_shadow(vm, value_type(a));
    return int_store(vm, value_type(a), (uint64_t)(-sint_read(a)));
}

static value_t *sint_shr(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, shr, ">>");
    if (!is_signed_int(vm, value_type(a)))
        return value_make_error(vm, ">>: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    return int_store(vm, value_type(a), (uint64_t)(sint_read(a) >> sint_read(b)));
}

static value_t *sint_lt(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, lt, "<");
    if (!is_signed_int(vm, value_type(a)))
        return value_make_error(vm, "<: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, sint_read(a) < sint_read(b));
}

static value_t *sint_le(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, le, "<=");
    if (!is_signed_int(vm, value_type(a)))
        return value_make_error(vm, "<=: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, sint_read(a) <= sint_read(b));
}

static value_t *sint_gt(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, gt, ">");
    if (!is_signed_int(vm, value_type(a)))
        return value_make_error(vm, ">: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, sint_read(a) > sint_read(b));
}

static value_t *sint_ge(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, ge, ">=");
    if (!is_signed_int(vm, value_type(a)))
        return value_make_error(vm, ">=: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, sint_read(a) >= sint_read(b));
}

/* ================================================================ */
/* 无符号特有运算                                                    */
/* ================================================================ */

static value_t *uint_div(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, div, "/");
    if (!is_unsigned_int(vm, value_type(a)))
        return value_make_error(vm, "/: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    uint64_t bv = uint_read(b);
    if (bv == 0) panic("division by zero");
    return int_store(vm, value_type(a), uint_read(a) / bv);
}

static value_t *uint_mod(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, mod, "%");
    if (!is_unsigned_int(vm, value_type(a)))
        return value_make_error(vm, "%: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    uint64_t bv = uint_read(b);
    if (bv == 0) panic("modulo by zero");
    return int_store(vm, value_type(a), uint_read(a) % bv);
}

static value_t *uint_shr(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, shr, ">>");
    if (!is_unsigned_int(vm, value_type(a)))
        return value_make_error(vm, ">>: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, value_type(a));
    return int_store(vm, value_type(a), uint_read(a) >> uint_read(b));
}

static value_t *uint_lt(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, lt, "<");
    if (!is_unsigned_int(vm, value_type(a)))
        return value_make_error(vm, "<: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, uint_read(a) < uint_read(b));
}

static value_t *uint_le(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, le, "<=");
    if (!is_unsigned_int(vm, value_type(a)))
        return value_make_error(vm, "<=: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, uint_read(a) <= uint_read(b));
}

static value_t *uint_gt(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, gt, ">");
    if (!is_unsigned_int(vm, value_type(a)))
        return value_make_error(vm, ">: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, uint_read(a) > uint_read(b));
}

static value_t *uint_ge(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, ge, ">=");
    if (!is_unsigned_int(vm, value_type(a)))
        return value_make_error(vm, ">=: type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    return bool_store(vm, uint_read(a) >= uint_read(b));
}

/* ================================================================ */
/* 隐式转换                                                          */
/* ================================================================ */

static value_t *sint_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    /* 加限定符身份转换（i32 → volatile i32 / const i32）：限定符不改变
       底层表示，非拓宽非缩窄。仅在标量层处理（指针 const 语义不同，
       指针 vtable 不调用）。 */
    if (type_is_const(target) || type_is_volatile(target)) {
        value_t *q = value_implicit_qualify(vm, v, target);
        if (!value_is_error(vm, q)) return q;
    }

    type_rank_t sr = type_rank(vm, value_type(v));
    type_rank_t tr = type_rank(vm, target);

    if (tr <= sr)
        return value_make_error(vm, "implicit cast: not a widening conversion");

    /* shadow：只检查类型兼容性，返回 shadow */
    if (value_is_shadow(v))
        return value_make_shadow(vm, target);

    int64_t sv = sint_read(v);

    if (is_signed_int(vm, target)) {
        return int_store(vm, target, (uint64_t)sv);
    }

    return value_make_error(vm, "implicit cast: incompatible target type");
}

static value_t *uint_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    /* 加限定符身份转换（u32 → volatile u32 / const u32），同 sint */
    if (type_is_const(target) || type_is_volatile(target)) {
        value_t *q = value_implicit_qualify(vm, v, target);
        if (!value_is_error(vm, q)) return q;
    }

    type_rank_t sr = type_rank(vm, value_type(v));
    type_rank_t tr = type_rank(vm, target);

    if (tr <= sr)
        return value_make_error(vm, "implicit cast: not a widening conversion");

    if (is_signed_int(vm, target))
        return value_make_error(vm, "implicit cast: cannot implicitly cast unsigned to signed");

    /* shadow：只检查类型兼容性，返回 shadow */
    if (value_is_shadow(v))
        return value_make_shadow(vm, target);

    uint64_t uv = uint_read(v);

    if (is_unsigned_int(vm, target)) {
        return int_store(vm, target, uv);
    }

    return value_make_error(vm, "implicit cast: incompatible target type");
}

/* ================================================================ */
/* 显式转换                                                          */
/* ================================================================ */

static value_t *sint_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    /* shadow：只检查类型兼容性，返回 shadow */
    if (value_is_shadow(v)) {
        if (is_int_type(vm, target) || is_float_type(vm, target) || target == vm->type_bool)
            return value_make_shadow(vm, target);
        return value_make_error(vm, "explicit cast: incompatible target type");
    }

    int64_t sv = sint_read(v);

    if (is_int_type(vm, target)) {
        return int_store(vm, target, (uint64_t)sv);
    }

    if (is_float_type(vm, target)) {
        void *data = value_alloc_data(vm->alloc, target);
        if (target == vm->type_f32)
            *(float *)data = (float)sv;
        else
            *(double *)data = (double)sv;
        return value_make(vm, target, data);
    }

    if (target == vm->type_bool) {
        return bool_store(vm, sv != 0);
    }

    return value_make_error(vm, "explicit cast: incompatible target type");
}

static value_t *uint_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    /* shadow：只检查类型兼容性，返回 shadow */
    if (value_is_shadow(v)) {
        if (is_int_type(vm, target) || is_float_type(vm, target) || target == vm->type_bool)
            return value_make_shadow(vm, target);
        return value_make_error(vm, "explicit cast: incompatible target type");
    }

    uint64_t uv = uint_read(v);

    if (is_int_type(vm, target)) {
        return int_store(vm, target, uv);
    }

    if (is_float_type(vm, target)) {
        void *data = value_alloc_data(vm->alloc, target);
        if (target == vm->type_f32)
            *(float *)data = (float)uv;
        else
            *(double *)data = (double)uv;
        return value_make(vm, target, data);
    }

    if (target == vm->type_bool) {
        return bool_store(vm, uv != 0);
    }

    return value_make_error(vm, "explicit cast: incompatible target type");
}

/* ================================================================ */
/* vtable 定义                                                       */
/* ================================================================ */

const vtable_t VTABLE_INT_SIGNED = {
    .add = int_add,  .sub = int_sub,
    .mul = int_mul,  .div = sint_div,
    .mod = sint_mod,  .neg = sint_neg,
    .eq  = int_eq,   .ne  = int_ne,
    .lt  = sint_lt,   .le  = sint_le,
    .gt  = sint_gt,   .ge  = sint_ge,
    .band = int_band, .bor = int_bor,
    .bxor = int_bxor, .bnot = int_bnot,
    .shl = int_shl,   .shr = sint_shr,
    .clone = int_clone,
    .assign = int_assign,
    .implicit_cast = sint_implicit_cast,
    .explicit_cast = sint_explicit_cast,
};

const vtable_t VTABLE_INT_UNSIGNED = {
    .add = int_add,  .sub = int_sub,
    .mul = int_mul,  .div = uint_div,
    .mod = uint_mod,  .neg = NULL,
    .eq  = int_eq,   .ne  = int_ne,
    .lt  = uint_lt,   .le  = uint_le,
    .gt  = uint_gt,   .ge  = uint_ge,
    .band = int_band, .bor = int_bor,
    .bxor = int_bxor, .bnot = int_bnot,
    .shl = int_shl,   .shr = uint_shr,
    .clone = int_clone,
    .assign = int_assign,
    .implicit_cast = uint_implicit_cast,
    .explicit_cast = uint_explicit_cast,
};
