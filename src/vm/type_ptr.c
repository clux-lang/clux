#include "vm/type_ptr.h"
#include "vm/type.h"
#include "vm/type_opaque.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/panic.h"

#include <string.h>
#include <stdalign.h>

/* ===========================================================================
 * 指针类型（own *T / ref *T / fatal *T，m3-design §3）
 *
 * ptr_type_t 继承 type_t 持 base_type 指针。C 内存映射：指针值 = 裸指针
 * （size = sizeof(void*)，align = 指针宽），无 RC 控制块——运行期零成本，
 * 逃逸检查全编译期（Step A 静默，Step B 叠加）。
 *
 * 三种所有权修饰（own/ref/fatal）是同一结构、独立 intern 实例（kind 区分
 * 所有权，own *i32 != ref *i32）。同一 base_type 的三种指针各自去重。
 *
 * 运算行为（m3-design §3/§8.4）：
 *   - eq/ne：指针值字节比较（裸指针数据）。
 *   - clone：复制指针值（裸指针数据平凡拷贝）。
 *   - assign：指针值覆盖（类型须相同）。
 *   - dispose：no-op（Step A 静默——new 堆块暂不释放，所有权销毁规则
 *     Step B 叠加）。
 *   - implicit_cast：own → ref 身份拷贝（借用的表达，§3.2）；任意指针 →
 *     opaque 隐式（§8.4）。
 *   - explicit_cast：opaque → 任意指针（as，§8.4）。
 *   - type_equal / type_extends：按 base 递归（三种所有权修饰是独立类型）。
 * =========================================================================== */

/* ---- 生命周期：clone（复制指针值）/ assign / dispose（no-op，Step A） ---- */

static value_t *ptr_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    void *data = value_alloc_data(vm->alloc, t);
    memcpy(data, value_data(v), t->size);  /* 指针值 = 裸指针：平凡拷贝 */
    return value_make(vm, t, data);
}

static value_t *ptr_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    const type_t *t = value_type(dst);
    if (value_type(src) != t) {
        return value_make_error(vm,
            "ptr: assignment requires matching pointer type");
    }
    memcpy(value_data(dst), value_data(src), t->size);  /* 指针值覆盖 */
    return dst;
}

static void ptr_dispose(vm_t *vm, value_t *v) {
    /* Step A 静默：new 堆块暂不释放（所有权销毁规则 Step B 叠加）。
       指针值本身是裸指针数据，无额外资源。 */
    (void)vm; (void)v;
}

/* ---- eq/ne：指针值字节比较 ---- */

static value_t *ptr_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const type_t *ta = value_type(a);
    const type_t *tb = value_type(b);
    if (ta->kind != tb->kind || ptr_type_base(ta) != ptr_type_base(tb)) {
        return value_make_error(vm,
            "ptr: == requires same pointer type");
    }
    void *pa = value_data(a);
    void *pb = value_data(b);
    bool eq = memcmp(pa, pb, ta->size) == 0;
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    memcpy(data, &eq, sizeof(bool));
    return value_make(vm, vm->type_bool, data);
}

static value_t *ptr_ne(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const type_t *ta = value_type(a);
    const type_t *tb = value_type(b);
    if (ta->kind != tb->kind || ptr_type_base(ta) != ptr_type_base(tb)) {
        return value_make_error(vm,
            "ptr: != requires same pointer type");
    }
    void *pa = value_data(a);
    void *pb = value_data(b);
    bool ne = memcmp(pa, pb, ta->size) != 0;
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    memcpy(data, &ne, sizeof(bool));
    return value_make(vm, vm->type_bool, data);
}

/* ---- 类型转换 ---- */

/* 目标是否为 opaque 类型（vtable 是 VTABLE_OPAQUE） */
static bool is_opaque_type(const type_t *t) {
    return t && t->vtable == &VTABLE_OPAQUE;
}

static value_t *ptr_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    const type_t *src = value_type(v);

    /* 任意指针 → opaque 隐式（m3-design §8.4）：身份拷贝指针值 */
    if (is_opaque_type(target)) {
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }

    /* own → ref 身份拷贝（借用的表达，§3.2）：同 base 的 own → ref */
    if (src->kind == TYPE_KIND_PTR_OWN && target &&
        target->kind == TYPE_KIND_PTR_REF &&
        ptr_type_base(src) == ptr_type_base(target)) {
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }

    /* 同类型身份拷贝（ref → ref 等，clone 语义） */
    if (src == target) {
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }

    return value_make_error(vm, "ptr: unsupported implicit cast for pointer");
}

static value_t *ptr_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (value_is_shadow(v)) return value_make_shadow(vm, target);

    /* opaque → 任意指针显式（as，§8.4）：身份拷贝指针值 */
    if (value_type(v)->vtable == &VTABLE_OPAQUE &&
        target && (target->kind == TYPE_KIND_PTR_OWN ||
                   target->kind == TYPE_KIND_PTR_REF ||
                   target->kind == TYPE_KIND_PTR_FATAL)) {
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }

    /* 同类型身份拷贝 */
    if (value_type(v) == target) {
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }

    return value_make_error(vm, "ptr: unsupported explicit cast for pointer");
}

/* ---- 鸭子类型判断：按 base 递归（三种所有权修饰是独立类型） ---- */

static bool ptr_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    if (a == b) return true;
    if (!b) return false;
    if (a->kind != b->kind) return false;
    if (a->kind != TYPE_KIND_PTR_OWN && a->kind != TYPE_KIND_PTR_REF &&
        a->kind != TYPE_KIND_PTR_FATAL) {
        return false;
    }
    return type_equal(vm, ptr_type_base(a), ptr_type_base(b));
}

static bool ptr_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    if (sub == sup) return true;
    if (!sup) return false;
    if (sub->kind != TYPE_KIND_PTR_OWN && sub->kind != TYPE_KIND_PTR_REF &&
        sub->kind != TYPE_KIND_PTR_FATAL) {
        return false;
    }
    /* 指针按 base 递归：同所有权修饰下 base 兼容即指针兼容。
       （跨所有权修饰的兼容由 sema 层控制——own → ref 的借用在转换层
       处理，类型值层面的鸭子判断不混用所有权。） */
    if (sub->kind != sup->kind) return false;
    return type_extends(vm, ptr_type_base(sub), ptr_type_base(sup));
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_PTR_OWN = {
    .eq = ptr_eq, .ne = ptr_ne,
    .dispose = ptr_dispose,
    .clone = ptr_clone,
    .assign = ptr_assign,
    .implicit_cast = ptr_implicit_cast,
    .explicit_cast = ptr_explicit_cast,
    .type_equal = ptr_type_equal,
    .type_extends = ptr_type_extends,
    .type_seal = type_ptr_seal, /* 开放构造路径（PUSH_PTR → SEAL）密封入口 */
};

const vtable_t VTABLE_PTR_REF = {
    .eq = ptr_eq, .ne = ptr_ne,
    .dispose = ptr_dispose,
    .clone = ptr_clone,
    .assign = ptr_assign,
    .implicit_cast = ptr_implicit_cast,
    .explicit_cast = ptr_explicit_cast,
    .type_equal = ptr_type_equal,
    .type_extends = ptr_type_extends,
    .type_seal = type_ptr_seal,
};

const vtable_t VTABLE_PTR_FATAL = {
    .eq = ptr_eq, .ne = ptr_ne,
    .dispose = ptr_dispose,
    .clone = ptr_clone,
    .assign = ptr_assign,
    .implicit_cast = ptr_implicit_cast,
    .explicit_cast = ptr_explicit_cast,
    .type_equal = ptr_type_equal,
    .type_extends = ptr_type_extends,
    .type_seal = type_ptr_seal,
};

/* ===========================================================================
 * 开放构造（PUSH_PTR / SET_TYPE / SEAL，与 const/volatile 统一的两遍构造）
 * =========================================================================== */

/* 构造名 "own *T" / "ref *T" / "fatal *T"（堆分配，vm 拥有） */
static char *ptr_name(allocator_t *alloc, type_kind_t kind, const type_t *base) {
    const char *owner;
    switch (kind) {
    case TYPE_KIND_PTR_OWN:   owner = "own ";   break;
    case TYPE_KIND_PTR_REF:   owner = "ref ";   break;
    case TYPE_KIND_PTR_FATAL: owner = "fatal "; break;
    default:                  owner = "ptr ";   break;
    }
    size_t ol = strlen(owner);
    size_t bl = base && base->name.ptr ? base->name.len : 0;
    char *buf = allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL, NULL,
                                 ol + bl + 2);
    if (!buf) return NULL;
    memcpy(buf, owner, ol);
    if (bl) memcpy(buf + ol, base->name.ptr, bl);
    buf[ol + bl] = '\0';
    return buf;
}

/* 分配开放 ptr_type（base_type=NULL，不入池，不压栈）。供字节码路径
 * （type_ptr_push）与 intern 快捷（type_ptr_intern）共用。 */
static ptr_type_t *ptr_type_create_open(vm_t *vm, type_kind_t kind) {
    ptr_type_t *pt = (ptr_type_t *)allocator_new_ex(
        vm->alloc, "ptr_type_t", sizeof(ptr_type_t), NULL, NULL, NULL, 1);
    if (!pt) panic("vm: out of memory allocating pointer type");
    memset(pt, 0, sizeof(ptr_type_t));
    switch (kind) {
    case TYPE_KIND_PTR_OWN:   pt->base.vtable = &VTABLE_PTR_OWN;   break;
    case TYPE_KIND_PTR_REF:   pt->base.vtable = &VTABLE_PTR_REF;   break;
    case TYPE_KIND_PTR_FATAL: pt->base.vtable = &VTABLE_PTR_FATAL; break;
    default:
        panic("vm: invalid pointer ownership kind");
        break;
    }
    pt->base.kind = kind;
    /* name/size/align 由 seal 按 base 填充；base_type 由 SET_TYPE 设定；sealed 置 0 */
    return pt;
}

/* PUSH_PTR：分配开放 ptr_type（base_type=NULL，不入池）+ 压其 type value。
 * 返回开放 type（未密封），经 SET_TYPE 设 base_type → SEAL 密封收尾。
 * kind = 所有权修饰（TYPE_KIND_PTR_OWN/REF/FATAL）。 */
const type_t *type_ptr_push(vm_t *vm, type_kind_t kind) {
    if (!vm) return NULL;
    ptr_type_t *pt = ptr_type_create_open(vm, kind);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &pt->base));
    return &pt->base; /* 外部只持有 type_t*，不感知 ptr_type_t 子类 */
}

/* SET_TYPE 运行期用：设被指向类型 T（密封后静默忽略） */
void type_ptr_set_base(vm_t *vm, const type_t *t, const type_t *base) {
    (void)vm;
    if (!t || type_is_sealed(t)) return;
    if (t->kind != TYPE_KIND_PTR_OWN && t->kind != TYPE_KIND_PTR_REF &&
        t->kind != TYPE_KIND_PTR_FATAL) {
        return;
    }
    ((ptr_type_t *)t)->base_type = base;
}

/* SEAL：按 base_type 去重 intern（同一 base 的三种所有权是三个独立实例），
 * 标记 sealed，返回该 const type（允许链式）。t 须为已设 base_type 的开放
 * ptr 类型（type_ptr_push 产物）。 */
const type_t *type_ptr_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t) return NULL;
    if (t->kind != TYPE_KIND_PTR_OWN && t->kind != TYPE_KIND_PTR_REF &&
        t->kind != TYPE_KIND_PTR_FATAL) {
        return NULL;
    }
    if (type_is_sealed(t)) return t;  /* 已密封直接返回（幂等） */

    ptr_type_t *pt = (ptr_type_t *)t;

    /* 必须已设 base_type（SET_TYPE） */
    if (!pt->base_type) return NULL;

    if (!vm->ptr_types) vm->ptr_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern（按 base_type + 所有权 kind）：命中已有 sealed 类型则复用
     * 并手工回收本开放类型 */
    size_t n = vec_len(vm->ptr_types);
    for (size_t i = 0; i < n; i++) {
        const ptr_type_t *other = (const ptr_type_t *)vec_get(vm->ptr_types, i);
        if (other && other != pt && type_is_sealed(&other->base) &&
            other->base.kind == pt->base.kind &&
            other->base_type == pt->base_type) {
            /* 本开放类型与已有 sealed 类型重复：复用 other。
             * 操作数栈中引用本开放类型 pt 的 type value 由 value_seal 负责
             * 重定向到 other（避免悬空）；此处仅手工回收 pt。 */
            if (pt->base.name.ptr) {
                char *np = (char *)pt->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&pt);
            return &other->base;
        }
    }

    /* 指针类型 size/align 恒定（指针宽），不依赖 base_type 布局 */
    pt->base.size   = sizeof(void *);
    pt->base.align  = alignof(void *);
    pt->base.sealed = true;

    /* 重建类型名 "own *T" / "ref *T" / "fatal *T" */
    char *name = ptr_name(vm->alloc, pt->base.kind, pt->base_type);
    if (!name) panic("vm: out of memory allocating pointer type name");
    pt->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->ptr_types, vm->alloc, pt);  /* 密封后入池（去重 intern） */
    return &pt->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_ptr_intern(vm_t *vm, type_kind_t kind, const type_t *base) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。
     * 与 type_ptr_push 的区别：不向操作数栈压入 type value（嵌套构造
     * 场景避免栈布局污染）。去重 intern 由 type_ptr_seal 完成。 */
    if (!vm || !base) return NULL;
    if (kind != TYPE_KIND_PTR_OWN && kind != TYPE_KIND_PTR_REF &&
        kind != TYPE_KIND_PTR_FATAL) {
        return NULL;
    }
    ptr_type_t *pt = ptr_type_create_open(vm, kind);
    pt->base_type = base;
    return type_ptr_seal(vm, &pt->base);
}
