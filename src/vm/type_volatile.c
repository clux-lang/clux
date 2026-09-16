#include "vm/type.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/panic.h"
#include "core/string.h"

#include <string.h>
#include <stdalign.h>

/* ===========================================================================
 * volatile 修饰类型
 *
 * volatile_type_t 继承 type_t 持 sub 指针。运算行为代理到 sub（解包语义，
 * m2-design §10）：所有 vtable 槽位把操作数的 type 临时替换为 sub，调用
 * sub->vtable 对应槽位后恢复。volatile 是存取语义提示（禁止优化器重排/
 * 合并读写），不改变类型身份：volatile i32 != i32，但兼容性（extends）
 * 与 sub 一致（代理递归）。
 * =========================================================================== */

/* ---- 解包代理辅助 ---- */

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

#define V_BIN(op)                                                          \
    static value_t *v_##op(vm_t *vm, value_t *a, value_t *b) {             \
        const type_t *sub = type_qualifier_sub(value_type(a));             \
        if (!sub || !sub->vtable || !sub->vtable->op)                      \
            return value_make_error(vm, "volatile type: unsupported operation"); \
        return proxy_binary(vm, a, b, sub, sub->vtable->op);               \
    }

#define V_UN(op)                                                           \
    static value_t *v_##op(vm_t *vm, value_t *a) {                         \
        const type_t *sub = type_qualifier_sub(value_type(a));             \
        if (!sub || !sub->vtable || !sub->vtable->op)                      \
            return value_make_error(vm, "volatile type: unsupported operation"); \
        return proxy_unary(vm, a, sub, sub->vtable->op);                   \
    }

V_BIN(add)  V_BIN(sub)  V_BIN(mul)  V_BIN(div)  V_BIN(mod)
V_BIN(eq)   V_BIN(ne)   V_BIN(lt)   V_BIN(le)   V_BIN(gt)   V_BIN(ge)
V_BIN(band) V_BIN(bor)  V_BIN(bxor) V_BIN(shl)  V_BIN(shr)
V_BIN(assign)
V_UN(neg)   V_UN(bnot)  V_UN(lnot)

/* 类型转换：第三个参数是 const type_t*（非 value_t*），不能走 V_BIN 宏 */
static value_t *v_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    const type_t *sub = type_qualifier_sub(value_type(v));
    if (!sub || !sub->vtable || !sub->vtable->implicit_cast)
        return value_make_error(vm, "volatile type: unsupported implicit cast");
    /* volatile T → T：同底层表示（volatile 是存取提示），身份拷贝 */
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

static value_t *v_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    const type_t *sub = type_qualifier_sub(value_type(v));
    if (!sub || !sub->vtable || !sub->vtable->explicit_cast)
        return value_make_error(vm, "volatile type: unsupported explicit cast");
    const type_t *sv = value_swap_type(v, sub);
    value_t *r = sub->vtable->explicit_cast(vm, v, target);
    value_restore_type(v, sv);
    return r;
}

/* call：volatile 修饰的函数类型仍可调用（解包代理） */
static value_t *v_call(vm_t *vm, value_t *callee, value_t **args, size_t argc) {
    const type_t *sub = type_qualifier_sub(value_type(callee));
    if (!sub || !sub->vtable || !sub->vtable->call)
        return value_make_error(vm, "volatile type: value is not callable");
    const type_t *sc = value_swap_type(callee, sub);
    value_t *r = sub->vtable->call(vm, callee, args, argc);
    value_restore_type(callee, sc);
    return r;
}

/* dispose：解包后释放 sub 的 data 内资源（data 布局 = sub 布局） */
static void v_dispose(vm_t *vm, value_t *v) {
    const type_t *sub = type_qualifier_sub(value_type(v));
    if (!sub || !sub->vtable || !sub->vtable->dispose) return;
    const type_t *sv = value_swap_type(v, sub);
    sub->vtable->dispose(vm, v);
    value_restore_type(v, sv);
}

/* clone：保留 volatile 类型（拷贝 data，size 与 sub 相同） */
static value_t *v_clone(vm_t *vm, value_t *v) {
    void *data = value_alloc_data_copy(vm->alloc, value_type(v), value_data(v));
    return value_make(vm, value_type(v), data);
}

/* 鸭子类型判断：volatile 是独立类型（volatile i32 == i32 → false） */
static bool v_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    (void)vm;
    return a == b;
}

/* 兼容性代理：volatile 不改变类型身份，extends 语义与 sub 一致（递归） */
static bool v_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    if (sub == sup) return true;
    const type_t *s = type_qualifier_sub(sub);
    if (!s) return false;
    return type_extends(vm, s, sup);
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_VOLATILE = {
    .add = v_add, .sub = v_sub, .mul = v_mul, .div = v_div, .mod = v_mod,
    .neg = v_neg,
    .eq = v_eq, .ne = v_ne, .lt = v_lt, .le = v_le, .gt = v_gt, .ge = v_ge,
    .band = v_band, .bor = v_bor, .bxor = v_bxor, .bnot = v_bnot,
    .shl = v_shl, .shr = v_shr,
    .lnot = v_lnot,
    .call = v_call,
    .extends = NULL, /* volatile 类型值自身不可作 type value 运算（值层无 type 值） */
    .dispose = v_dispose,
    .clone = v_clone,
    .assign = v_assign,
    .implicit_cast = v_implicit_cast,
    .explicit_cast = v_explicit_cast,
    .type_equal = v_type_equal,
    .type_extends = v_type_extends,
    .type_seal = type_volatile_seal, /* 开放构造路径（PUSH_VOLATILE → SEAL）密封入口 */
};

/* ===========================================================================
 * 开放构造（PUSH_VOLATILE / SET_TYPE / SEAL，与 array/func 统一的两遍构造）
 * =========================================================================== */

/* 构造名 "volatile <sub 名>"（堆分配，vm 拥有） */
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

/* 分配开放 volatile_type（sub=NULL，不入池，不压栈）。供字节码路径
 * （type_volatile_push）与 intern 快捷（type_volatile_intern）共用。 */
static volatile_type_t *volatile_type_create_open(vm_t *vm) {
    volatile_type_t *vt = (volatile_type_t *)allocator_new_ex(
        vm->alloc, "volatile_type_t", sizeof(volatile_type_t), NULL, NULL, NULL, 1);
    if (!vt) panic("vm: out of memory allocating volatile type");
    memset(vt, 0, sizeof(volatile_type_t));
    vt->base.vtable = &VTABLE_VOLATILE;
    vt->base.kind   = TYPE_KIND_VOLATILE;
    /* name/size/align 由 seal 按 sub 填充；sub 由 SET_TYPE 设定；sealed 置 0 */
    return vt;
}

/* PUSH_VOLATILE：分配开放 volatile_type（sub=NULL，不入池）+ 压其 type value。
 * 返回开放 type（未密封），经 SET_TYPE 设 sub → SEAL 密封收尾。 */
const type_t *type_volatile_push(vm_t *vm) {
    if (!vm) return NULL;
    volatile_type_t *vt = volatile_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &vt->base));
    return &vt->base; /* 外部只持有 type_t*，不感知 volatile_type_t 子类 */
}

/* SEAL：计算内存布局（size/align 拷贝 sub——volatile 值存储与 sub 相同），
 * 按 sub 指针去重 intern，标记 sealed，返回该 volatile type（允许链式）。
 * t 须为已设 sub 的开放 volatile 类型（type_volatile_push 产物）。 */
const type_t *type_volatile_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_VOLATILE) return NULL;
    if (type_is_sealed(t)) return t;  /* 已密封直接返回（幂等） */

    volatile_type_t *vt = (volatile_type_t *)t;

    /* 必须已设 sub（SET_TYPE） */
    if (!vt->sub) return NULL;

    if (!vm->volatile_types)
        vm->volatile_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern（按 sub 指针）：命中已有 sealed 类型则复用并手工回收本开放类型 */
    size_t n = vec_len(vm->volatile_types);
    for (size_t i = 0; i < n; i++) {
        const volatile_type_t *other =
            (const volatile_type_t *)vec_get(vm->volatile_types, i);
        if (other && other != vt && type_is_sealed(&other->base) &&
            other->sub == vt->sub) {
            /* 本开放类型与已有 sealed 类型重复：复用 other。
             * 操作数栈中引用本开放类型 vt 的 type value 由 value_seal 负责
             * 重定向到 other（避免悬空）；此处仅手工回收 vt。 */
            if (vt->base.name.ptr) {
                char *np = (char *)vt->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&vt);
            return &other->base;
        }
    }

    /* 计算内存布局：volatile 值存储与 sub 相同（size/align 拷贝，无额外布局），
       置 sealed 标志（构造完成后不可再修改 sub） */
    vt->base.size   = vt->sub->size;
    vt->base.align  = vt->sub->align;
    vt->base.sealed = true;

    /* 重建类型名 "volatile <sub 名>"（sub 密封后名字已定） */
    char *name = qual_name(vm->alloc, "volatile", vt->sub);
    if (!name) panic("vm: out of memory allocating volatile type name");
    vt->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->volatile_types, vm->alloc, vt);  /* 密封后入池（去重 intern） */
    return &vt->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_volatile_intern(vm_t *vm, const type_t *sub) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。
     * 与 type_volatile_push 的区别：不向操作数栈压入 type value（嵌套构造
     * 场景避免栈布局污染）。去重 intern 由 type_volatile_seal 完成。 */
    if (!vm || !sub) return NULL;
    volatile_type_t *vt = volatile_type_create_open(vm);
    vt->sub = sub;
    return type_volatile_seal(vm, &vt->base);
}
