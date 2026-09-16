#include "vm/type.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/panic.h"
#include "core/string.h"

#include <string.h>
#include <stdalign.h>

/* ===========================================================================
 * const 修饰类型
 *
 * const_type_t 继承 type_t 持 sub 指针。运算行为代理到 sub（解包语义，
 * m2-design §10）：所有 vtable 槽位把操作数的 type 临时替换为 sub，调用
 * sub->vtable 对应槽位后恢复——base 槽位依赖 value_type() 指针比较判定
 * 类型，不能直接复用函数指针。const 赋值检查（不可变）是 sema 层职责
 * （flow_init 驱动的 TDZ 豁免），vm 层不拦截。
 * =========================================================================== */

/* ---- 解包代理辅助 ---- */

/* 临时把 a、b 的 type 换成 sub（若为 const/volatile 限定类型），调用 slot，
   恢复后返回结果。b 非限定类型时保持原样（sub vtable 的 VTABLE_BINARY
   协商处理剩余差异）。 */
typedef value_t *(*bin_slot_t)(vm_t *, value_t *, value_t *);
typedef value_t *(*un_slot_t)(vm_t *, value_t *);

static value_t *proxy_binary(vm_t *vm, value_t *a, value_t *b,
                             const type_t *sub, bin_slot_t slot) {
    const type_t *sa = value_swap_type(a, sub);
    const type_t *sb = NULL;
    if (type_qualifier_sub(value_type(b)))
        sb = value_swap_type(b, sub);
    value_t *r = slot(vm, a, b);
    if (sb) value_restore_type(b, sb);
    value_restore_type(a, sa);
    return r;
}

static value_t *proxy_unary(vm_t *vm, value_t *a, const type_t *sub,
                            un_slot_t slot) {
    const type_t *sa = value_swap_type(a, sub);
    value_t *r = slot(vm, a);
    value_restore_type(a, sa);
    return r;
}

/* ---- 二元/一元槽位（统一解包代理） ---- */

#define C_BIN(op)                                                          \
    static value_t *c_##op(vm_t *vm, value_t *a, value_t *b) {             \
        const type_t *sub = type_qualifier_sub(value_type(a));             \
        if (!sub || !sub->vtable || !sub->vtable->op)                      \
            return value_make_error(vm, "const type: unsupported operation"); \
        return proxy_binary(vm, a, b, sub, sub->vtable->op);               \
    }

#define C_UN(op)                                                           \
    static value_t *c_##op(vm_t *vm, value_t *a) {                         \
        const type_t *sub = type_qualifier_sub(value_type(a));             \
        if (!sub || !sub->vtable || !sub->vtable->op)                      \
            return value_make_error(vm, "const type: unsupported operation"); \
        return proxy_unary(vm, a, sub, sub->vtable->op);                   \
    }

C_BIN(add)  C_BIN(sub)  C_BIN(mul)  C_BIN(div)  C_BIN(mod)
C_BIN(eq)   C_BIN(ne)   C_BIN(lt)   C_BIN(le)   C_BIN(gt)   C_BIN(ge)
C_BIN(band) C_BIN(bor)  C_BIN(bxor) C_BIN(shl)  C_BIN(shr)
C_BIN(assign)
C_UN(neg)   C_UN(bnot)  C_UN(lnot)

/* 类型转换：第三个参数是 const type_t*（非 value_t*），不能走 C_BIN 宏 */
static value_t *c_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    const type_t *sub = type_qualifier_sub(value_type(v));
    if (!sub || !sub->vtable || !sub->vtable->implicit_cast)
        return value_make_error(vm, "const type: unsupported implicit cast");
    /* const T → T：同底层表示的复制语义（非拓宽，走身份拷贝） */
    if (target == sub) {
        if (value_is_shadow(v)) return value_make_shadow(vm, target);
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }
    const type_t *sv = value_swap_type(v, sub);
    value_t *r = sub->vtable->implicit_cast(vm, v, target);
    value_restore_type(v, sv);
    return r;
}

static value_t *c_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    const type_t *sub = type_qualifier_sub(value_type(v));
    if (!sub || !sub->vtable || !sub->vtable->explicit_cast)
        return value_make_error(vm, "const type: unsupported explicit cast");
    const type_t *sv = value_swap_type(v, sub);
    value_t *r = sub->vtable->explicit_cast(vm, v, target);
    value_restore_type(v, sv);
    return r;
}

/* call：const 修饰的函数类型仍可调用（解包代理） */
static value_t *c_call(vm_t *vm, value_t *callee, value_t **args, size_t argc) {
    const type_t *sub = type_qualifier_sub(value_type(callee));
    if (!sub || !sub->vtable || !sub->vtable->call)
        return value_make_error(vm, "const type: value is not callable");
    const type_t *sc = value_swap_type(callee, sub);
    value_t *r = sub->vtable->call(vm, callee, args, argc);
    value_restore_type(callee, sc);
    return r;
}

/* dispose：解包后释放 sub 的 data 内资源（data 布局 = sub 布局） */
static void c_dispose(vm_t *vm, value_t *v) {
    const type_t *sub = type_qualifier_sub(value_type(v));
    if (!sub || !sub->vtable || !sub->vtable->dispose) return;
    const type_t *sv = value_swap_type(v, sub);
    sub->vtable->dispose(vm, v);
    value_restore_type(v, sv);
}

/* clone：保留 const 类型（拷贝 data，size 与 sub 相同） */
static value_t *c_clone(vm_t *vm, value_t *v) {
    void *data = value_alloc_data_copy(vm->alloc, value_type(v), value_data(v));
    return value_make(vm, value_type(v), data);
}

/* 鸭子类型判断：const 是独立类型（const i32 == i32 → false） */
static bool c_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    (void)vm;
    return a == b;
}

static bool c_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    if (sub == sup) return true;
    /* const 类型可兼容到其 sub（复制语义：const 值可赋给非 const 变量） */
    const type_t *b = type_qualifier_sub(sub);
    if (!b) return false;
    return type_extends(vm, b, sup);
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_CONST = {
    .add = c_add, .sub = c_sub, .mul = c_mul, .div = c_div, .mod = c_mod,
    .neg = c_neg,
    .eq = c_eq, .ne = c_ne, .lt = c_lt, .le = c_le, .gt = c_gt, .ge = c_ge,
    .band = c_band, .bor = c_bor, .bxor = c_bxor, .bnot = c_bnot,
    .shl = c_shl, .shr = c_shr,
    .lnot = c_lnot,
    .call = c_call,
    .extends = NULL, /* const 类型值自身不可作 type value 运算（值层无 type 值） */
    .dispose = c_dispose,
    .clone = c_clone,
    .assign = c_assign,
    .implicit_cast = c_implicit_cast,
    .explicit_cast = c_explicit_cast,
    .type_equal = c_type_equal,
    .type_extends = c_type_extends,
    .type_seal = type_const_seal, /* 开放构造路径（PUSH_CONST → SEAL）密封入口 */
};

/* ===========================================================================
 * 开放构造（PUSH_CONST / SET_TYPE / SEAL，与 array/func 统一的两遍构造）
 * =========================================================================== */

/* 构造名 "const <sub 名>"（堆分配，vm 拥有） */
static char *qual_name(allocator_t *alloc, const char *prefix,
                       const type_t *sub) {
    size_t pl = strlen(prefix);
    size_t sl = sub && sub->name.ptr ? sub->name.len : 0;
    char *buf = allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL, NULL,
                                 pl + 1 + sl + 1);
    if (!buf) return NULL;
    memcpy(buf, prefix, pl);
    buf[pl] = ' ';
    if (sl) memcpy(buf + pl + 1, sub->name.ptr, sl);
    buf[pl + 1 + sl] = '\0';
    return buf;
}

/* 分配开放 const_type（sub=NULL，不入池，不压栈）。供字节码路径
 * （type_const_push）与 intern 快捷（type_const_intern）共用。 */
static const_type_t *const_type_create_open(vm_t *vm) {
    const_type_t *ct = (const_type_t *)allocator_new_ex(
        vm->alloc, "const_type_t", sizeof(const_type_t), NULL, NULL, NULL, 1);
    if (!ct) panic("vm: out of memory allocating const type");
    memset(ct, 0, sizeof(const_type_t));
    ct->base.vtable = &VTABLE_CONST;
    ct->base.kind   = TYPE_KIND_CONST;
    /* name/size/align 由 seal 按 sub 填充；sub 由 SET_TYPE 设定；sealed 置 0 */
    return ct;
}

/* PUSH_CONST：分配开放 const_type（sub=NULL，不入池）+ 压其 type value。
 * 返回开放 type（未密封），经 SET_TYPE 设 sub → SEAL 密封收尾。 */
const type_t *type_const_push(vm_t *vm) {
    if (!vm) return NULL;
    const_type_t *ct = const_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &ct->base));
    return &ct->base; /* 外部只持有 type_t*，不感知 const_type_t 子类 */
}

/* SEAL：计算内存布局（size/align 拷贝 sub——const 值存储与 sub 相同），
 * 按 sub 指针去重 intern，标记 sealed，返回该 const type（允许链式）。
 * t 须为已设 sub 的开放 const 类型（type_const_push 产物）。 */
const type_t *type_const_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_CONST) return NULL;
    if (type_is_sealed(t)) return t;  /* 已密封直接返回（幂等） */

    const_type_t *ct = (const_type_t *)t;

    /* 必须已设 sub（SET_TYPE） */
    if (!ct->sub) return NULL;

    if (!vm->const_types) vm->const_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern（按 sub 指针）：命中已有 sealed 类型则复用并手工回收本开放类型 */
    size_t n = vec_len(vm->const_types);
    for (size_t i = 0; i < n; i++) {
        const const_type_t *other = (const const_type_t *)vec_get(vm->const_types, i);
        if (other && other != ct && type_is_sealed(&other->base) &&
            other->sub == ct->sub) {
            /* 本开放类型与已有 sealed 类型重复：复用 other。
             * 操作数栈中引用本开放类型 ct 的 type value 由 value_seal 负责
             * 重定向到 other（避免悬空）；此处仅手工回收 ct。 */
            if (ct->base.name.ptr) {
                char *np = (char *)ct->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&ct);
            return &other->base;
        }
    }

    /* 计算内存布局：const 值存储与 sub 相同（size/align 拷贝，无额外布局），
       置 sealed 标志（构造完成后不可再修改 sub） */
    ct->base.size   = ct->sub->size;
    ct->base.align  = ct->sub->align;
    ct->base.sealed = true;

    /* 重建类型名 "const <sub 名>"（sub 密封后名字已定） */
    char *name = qual_name(vm->alloc, "const", ct->sub);
    if (!name) panic("vm: out of memory allocating const type name");
    ct->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->const_types, vm->alloc, ct);  /* 密封后入池（去重 intern） */
    return &ct->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_const_intern(vm_t *vm, const type_t *sub) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。
     * 与 type_const_push 的区别：不向操作数栈压入 type value（嵌套构造
     * 场景避免栈布局污染）。去重 intern 由 type_const_seal 完成。 */
    if (!vm || !sub) return NULL;
    const_type_t *ct = const_type_create_open(vm);
    ct->sub = sub;
    return type_const_seal(vm, &ct->base);
}
