#include "core/ffi_abi.h"

#include "vm/vm.h"
#include "vm/type_struct.h"
#include "vm/type_cunion.h"
#include "vm/type_ptr.h"

#include <stdio.h>
#include <string.h>

/* ---- §2 C ABI 类型约束（m6-design §2.1/§2.2）----
 *
 * `extern cfunc` 的参数/返回类型必须是 C 可表达的类型；clux 专有的类型（option、
 * tag union、切片、元组、闭包、类型值）没有 C 对应，必须在编译期拒绝。
 * 返回 NULL 表示"可用于 C ABI"，否则返回拒绝原因。
 *
 * 只依赖类型表，不依赖 libffi：C 后端直接生成 C 原型/调用，不需要描述符。 */
const char *ffi_c_abi_reject_reason(const type_t *t) {
    if (!t) return "missing type";
    if (t->kind == TYPE_KIND_ARRAY)
        return "array has no by-value C representation: arrays decay to pointers "
               "in C (and C cannot return arrays) — declare the parameter as "
               "'ref *T' / 'ref opaque' and pass data(a) / a.& instead";
    switch (t->kind) {
    case TYPE_KIND_INT:
    case TYPE_KIND_FLOAT:
    case TYPE_KIND_BOOL:
    case TYPE_KIND_STR:
    case TYPE_KIND_OPAQUE:
    case TYPE_KIND_ENUM:
    case TYPE_KIND_STRUCT:
    case TYPE_KIND_CUNION:
    case TYPE_KIND_VOID:
    case TYPE_KIND_PTR:
    case TYPE_KIND_CFUNC:
        return NULL;
    case TYPE_KIND_OPTION:
        return "option has no C representation";
    case TYPE_KIND_UNION:
        return "tag union has no C representation (use cunion for C unions)";
    case TYPE_KIND_SLICE:
        return "slice is a fat pointer (not C ABI)";
    case TYPE_KIND_TUPLE:
        return "tuple has no C representation";
    case TYPE_KIND_FUNC:
        return "closures/functions cannot cross the C ABI";
    case TYPE_KIND_TYPE:
        return "type values cannot cross the C ABI";
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE:
        return "const/volatile wrapper cannot cross the C ABI";
    default:
        return "type is not C ABI compatible";
    }
}

/* 聚合类型字段的递归校验：不再构造 libffi 描述符，只递归类型约束。 */
static bool check_aggregate_fields(const type_t *t, char *err, size_t ecap) {
    if (!t) return true;
    if (t->kind == TYPE_KIND_STRUCT) {
        size_t n = struct_type_field_count(t);
        for (size_t i = 0; i < n; i++) {
            const struct_field_t *f = struct_type_field(t, i);
            if (!f || !f->type) continue;
            const char *why = ffi_c_abi_reject_reason(f->type);
            if (why) {
                if (err && ecap)
                    snprintf(err, ecap, "field '%.*s': %s",
                             (int)f->name.len, f->name.ptr ? f->name.ptr : "?",
                             why);
                return false;
            }
            if (!check_aggregate_fields(f->type, err, ecap)) return false;
        }
        return true;
    }
    if (t->kind == TYPE_KIND_CUNION) {
        size_t n = cunion_type_member_count(t);
        for (size_t i = 0; i < n; i++) {
            const cunion_member_t *m = cunion_type_member(t, i);
            if (!m || !m->type) continue;
            const char *why = ffi_c_abi_reject_reason(m->type);
            if (why) {
                if (err && ecap)
                    snprintf(err, ecap, "member '%.*s': %s",
                             (int)m->name.len, m->name.ptr ? m->name.ptr : "?",
                             why);
                return false;
            }
            if (!check_aggregate_fields(m->type, err, ecap)) return false;
        }
        return true;
    }
    return true;
}

bool ffi_c_abi_check_aggregate(const type_t *t, const struct vm_t *vm, char *err,
                               size_t ecap) {
    (void)vm;
    return check_aggregate_fields(t, err, ecap);
}

/* ---- 数据符号（extern var）的类型 ↔ 宽度/类别码 ---- */
bool ffi_data_kind_of(const type_t *t, const struct vm_t *vm, uint32_t *out_kind) {
    if (!t || !vm || !out_kind) return false;
    const vm_t *v = (const vm_t *)vm;
    if (t == v->type_i8)  { *out_kind = CLUX_FFI_DATA_I8;  return true; }
    if (t == v->type_i16) { *out_kind = CLUX_FFI_DATA_I16; return true; }
    if (t == v->type_i32) { *out_kind = CLUX_FFI_DATA_I32; return true; }
    if (t == v->type_i64) { *out_kind = CLUX_FFI_DATA_I64; return true; }
    if (t == v->type_u8)  { *out_kind = CLUX_FFI_DATA_U8;  return true; }
    if (t == v->type_u16) { *out_kind = CLUX_FFI_DATA_U16; return true; }
    if (t == v->type_u32) { *out_kind = CLUX_FFI_DATA_U32; return true; }
    if (t == v->type_u64) { *out_kind = CLUX_FFI_DATA_U64; return true; }
    if (t == v->type_usize) { *out_kind = CLUX_FFI_DATA_USIZE; return true; }
    if (t == v->type_f32) { *out_kind = CLUX_FFI_DATA_F32; return true; }
    if (t == v->type_f64) { *out_kind = CLUX_FFI_DATA_F64; return true; }
    if (t == v->type_bool) { *out_kind = CLUX_FFI_DATA_BOOL; return true; }
    if (t == v->type_opaque) { *out_kind = CLUX_FFI_DATA_PTR; return true; }
    if (t == v->type_str) { *out_kind = CLUX_FFI_DATA_STR; return true; }
    return false;
}

const type_t *ffi_data_type_of(const struct vm_t *vm, uint32_t kind,
                               size_t *out_size) {
    if (!vm) return NULL;
    const vm_t *v = (const vm_t *)vm;
    switch (kind) {
    case CLUX_FFI_DATA_I8:  if (out_size) *out_size = 1; return v->type_i8;
    case CLUX_FFI_DATA_I16: if (out_size) *out_size = 2; return v->type_i16;
    case CLUX_FFI_DATA_I32: if (out_size) *out_size = 4; return v->type_i32;
    case CLUX_FFI_DATA_I64: if (out_size) *out_size = 8; return v->type_i64;
    case CLUX_FFI_DATA_U8:  if (out_size) *out_size = 1; return v->type_u8;
    case CLUX_FFI_DATA_U16: if (out_size) *out_size = 2; return v->type_u16;
    case CLUX_FFI_DATA_U32: if (out_size) *out_size = 4; return v->type_u32;
    case CLUX_FFI_DATA_U64: if (out_size) *out_size = 8; return v->type_u64;
    case CLUX_FFI_DATA_USIZE:
        if (out_size) *out_size = sizeof(void *);
        return v->type_usize;
    case CLUX_FFI_DATA_F32: if (out_size) *out_size = 4; return v->type_f32;
    case CLUX_FFI_DATA_F64: if (out_size) *out_size = 8; return v->type_f64;
    case CLUX_FFI_DATA_BOOL:
        if (out_size) *out_size = 1;
        return v->type_bool;
    case CLUX_FFI_DATA_PTR:
        if (out_size) *out_size = sizeof(void *);
        return v->type_opaque;
    case CLUX_FFI_DATA_STR:
        if (out_size) *out_size = sizeof(char *);
        return v->type_str;
    default:
        return NULL;
    }
}

bool ffi_data_kind_needs_type(const type_t *t) {
    if (!t) return false;
    switch (t->kind) {
    case TYPE_KIND_STRUCT:
    case TYPE_KIND_CUNION:
    case TYPE_KIND_ENUM:
        return true;
    default:
        return false;
    }
}
