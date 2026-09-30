#include "vm/type_opaque.h"
#include "vm/type_ptr.h"
#include "vm/value.h"
#include "vm/vm.h"

#include <string.h>

/* ===========================================================================
 * opaque 类型（≈ C 的 void*，m3-design §8.4）
 *
 * opaque 是内建单例类型（vm->type_opaque，id 17），无开放构造阶段——与
 * error/interrupt 同族：vm_init_builtins 创建即 sealed，无 type_seal 槽位
 * （value_seal 对 sealed 类型直接原样返回）。
 *
 * C 内存映射：指针值 = 裸指针（size = sizeof(void*)，align = 指针宽）。
 *
 * 运算行为：
 *   - clone/assign：指针值平凡拷贝（裸指针数据）。
 *   - dispose：no-op（Step A 静默——new 堆块暂不释放）。
 *   - eq/ne：指针值字节比较（与指针 vtable 同款）。
 *   - implicit_cast：任意指针 → opaque（身份拷贝，由指针 vtable 提供——
 *     本 vtable 的 implicit_cast 只处理同类型身份拷贝）。
 *   - explicit_cast：opaque → 任意指针（as，由指针 vtable 提供）。
 *   - type_equal/type_extends：单例身份比较（opaque 是独立类型，不与任何
 *     指针类型鸭子相等）。
 * =========================================================================== */

static value_t *opaque_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    void *data = value_alloc_data(vm->alloc, t);
    memcpy(data, value_data(v), t->size);  /* 裸指针：平凡拷贝 */
    return value_make(vm, t, data);
}

static value_t *opaque_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    const type_t *t = value_type(dst);
    if (value_type(src) != t) {
        return value_make_error(vm,
            "opaque: assignment requires opaque type");
    }
    memcpy(value_data(dst), value_data(src), t->size);
    return dst;
}

static void opaque_dispose(vm_t *vm, value_t *v) {
    /* Step A 静默：new 堆块暂不释放。裸指针数据无额外资源。 */
    (void)vm; (void)v;
}

static value_t *opaque_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const type_t *t = value_type(a);
    if (value_type(b) != t) {
        return value_make_error(vm, "opaque: == requires opaque type");
    }
    bool eq = memcmp(value_data(a), value_data(b), t->size) == 0;
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    memcpy(data, &eq, sizeof(bool));
    return value_make(vm, vm->type_bool, data);
}

static value_t *opaque_ne(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const type_t *t = value_type(a);
    if (value_type(b) != t) {
        return value_make_error(vm, "opaque: != requires opaque type");
    }
    bool ne = memcmp(value_data(a), value_data(b), t->size) != 0;
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    memcpy(data, &ne, sizeof(bool));
    return value_make(vm, vm->type_bool, data);
}

static value_t *opaque_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    /* 仅同类型身份拷贝（opaque → opaque）；任意指针 → opaque 的隐式转换
       由指针 vtable 提供（ptr_implicit_cast 检测目标为 opaque）。 */
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    if (value_type(v) == target) {
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }
    return value_make_error(vm, "opaque: unsupported implicit cast");
}

static value_t *opaque_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    /* opaque → 任意指针显式（as，§8.4）。源是 opaque（vtable 分派按源
       类型）——对称于 ptr_implicit_cast 的指针→opaque 分支。opaque data
       存裸指针值，拷贝进指针值 target 槽（owns=false：从 opaque 恢复的
       指针是借用，无 owns 信息，堆块归原始持有者）。 */
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    if (target && (target->kind == TYPE_KIND_PTR_OWN ||
                   target->kind == TYPE_KIND_PTR_REF ||
                   target->kind == TYPE_KIND_PTR_FATAL)) {
        void *opq = *(void **)value_data(v);
        return ptr_make_value(vm, target, opq, false);
    }
    if (value_type(v) == target) {
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }
    return value_make_error(vm, "opaque: unsupported explicit cast");
}

static bool opaque_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    (void)vm;
    return a == b;  /* 单例身份比较 */
}

static bool opaque_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    (void)vm;
    return sub == sup;  /* 单例身份比较 */
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_OPAQUE = {
    .eq = opaque_eq, .ne = opaque_ne,
    .dispose = opaque_dispose,
    .clone = opaque_clone,
    .assign = opaque_assign,
    .implicit_cast = opaque_implicit_cast,
    .explicit_cast = opaque_explicit_cast,
    .type_equal = opaque_type_equal,
    .type_extends = opaque_type_extends,
    /* .type_seal = NULL：opaque 内建单例无开放构造阶段（已 sealed） */
};
