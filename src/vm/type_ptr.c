#include "vm/type_ptr.h"
#include "vm/type.h"
#include "vm/type_opaque.h"
#include "vm/type_option.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_array.h"
#include "vm/type_slice.h"
#include "vm/type_union.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/panic.h"

#include <string.h>
#include <stdalign.h>

/* ---- 前向声明：slice_free_elems / slice_clear_elems 在 type_slice.c 中
   定义，切片元素级递归销毁/清空由下方 ptr_free_owned_recursive /
   ptr_clear_owned_recursive 的 SLICE_OWN 分支调用。 ---- */
void slice_free_elems(vm_t *vm, const type_t *elem, void *ptr, uint64_t len);
void slice_clear_elems(vm_t *vm, const type_t *elem, void *ptr, uint64_t len);

/* ===========================================================================
 * 指针类型（*T，m3-design §3）
 *
 * ptr_type_t 继承 type_t 持 base_type 指针。指针值 data 块布局 =
 * ptr_value_t { void *ptr; }（见 type_ptr.h）。
 *
 * 所有权纯编译期——类型不区分 own/ref/fatal，只有一种指针类型 *T。
 * 运行期零标志：clone/dispose/cast 不按所有权分派，堆释放由 sema 生成
 * FREE 指令负责。
 *
 * 生命周期（运行期简化）：
 *   - clone：复制指针值（浅拷贝——堆块所有权由编译期 ownership 语义控制，
 *     运行期不做深拷贝/转移分派）
 *   - dispose：no-op（堆释放由 FREE 指令负责）
 *   - eq/ne：比较 ptr 字段
 *   - implicit_cast：身份拷贝 / 指针向上转换 / → opaque 隐式
 *   - explicit_cast：opaque → 任意指针（as，§8.4）
 *   - type_equal / type_extends：按 base 递归
 * =========================================================================== */

/* ===========================================================================
 * 递归销毁（m3-design §3.1/§13）：own 销毁沿字段递归。
 *
 * 纯静态分派（type + 裸 data 块，不依赖 value_t 包装）——供 scope_destroy
 * 在 value_dispose 前扫描 owned 值调用（销毁变量前先销毁变量内部所有 own
 * 指向的堆内存，含结构体字段内嵌的 own 指针）。
 *
 * 双释放防线：ref 总借用跳过；own/fatal ptr=NULL（已转移）跳过；递归
 * 销毁后堆块随即释放并置 ptr=NULL，不重复扫描（每个 ptr_value_t 只被
 * 消费一次）。
 * =========================================================================== */

/* 目标是否含需递归销毁的 own 指针（指针 + 切片）。
   复合类型（struct/tuple/array/option/union）禁止携带指针/切片字段，
   因此不再需要递归扫描——它们只含平凡标量或嵌套复合类型。
   所有权纯编译期——运行期不再按 ownership 区分，只要类型是指针/切片
   就可能有堆块需要释放（调用方——FREE/DISPOSE——保证只对需要的值调用）。 */
bool ptr_type_needs_scan(const type_t *t) {
    if (!t) return false;
    switch (t->kind) {
    case TYPE_KIND_PTR:
    case TYPE_KIND_SLICE:
        return true;
    default:
        return false;
    }
}

void ptr_free_owned_recursive(vm_t *vm, const type_t *t, void *data) {
    if (!vm || !t || !data) return;
    switch (t->kind) {
    case TYPE_KIND_PTR: {
        /* 指针：ptr≠NULL → 先递归销毁堆块内容（T 含 own 字段时内嵌堆块
           一并释放），再释放本堆块，置 ptr=NULL 防双释放。
           所有权纯编译期——调用方（FREE 指令）保证只对 own 变量调用。 */
        ptr_value_t *pv = (ptr_value_t *)data;
        if (!pv->ptr) return;
        const type_t *base = ptr_type_base(t);
        if (base && ptr_type_needs_scan(base))
            ptr_free_owned_recursive(vm, base, pv->ptr);
        void *heap = pv->ptr;
        pv->ptr = NULL;
        allocator_free(vm->alloc, &heap);
        return;
    }
    case TYPE_KIND_SLICE: {
        /* []T：拥有堆块 → 先递归销毁元素内嵌 own 字段，再释放堆块，
           置 ptr=NULL 防双释放。 */
        slice_value_t *sv = (slice_value_t *)data;
        if (!sv->ptr) return;
        const type_t *et = slice_type_elem(t);
        if (et) slice_free_elems(vm, et, sv->ptr, sv->len);
        void *heap = sv->ptr;
        sv->ptr = NULL;
        sv->len = 0;
        allocator_free(vm->alloc, &heap);
        return;
    }
    default:
        /* 标量/字符串/cunion/opaque 等：无 own 堆块可销毁 */
        return;
    }
}

/* 所有权转移清空（接管辅助）：递归把 data 块内所有指针置 ptr=NULL
   （**不释放**——指针值已随 memcpy 复制进接管方堆块，所有权随拷贝转移）。
   供 op_new/op_construct 在成员值 memcpy 进目标块后调用，清空源对象图内
   所有指针，防作用域退出时与接管方双释放。 */
void ptr_clear_owned_recursive(vm_t *vm, const type_t *t, void *data) {
    if (!vm || !t || !data) return;
    switch (t->kind) {
    case TYPE_KIND_PTR: {
        ptr_value_t *pv = (ptr_value_t *)data;
        if (!pv->ptr) return;
        const type_t *base = ptr_type_base(t);
        if (base && ptr_type_needs_scan(base))
            ptr_clear_owned_recursive(vm, base, pv->ptr);
        pv->ptr = NULL;  /* 只清指针：堆块已归接管方，不释放 */
        return;
    }
    case TYPE_KIND_SLICE: {
        /* []T：堆块已随 memcpy 复制进接管方，清源 ptr=NULL 防作用域
           退出时与接管方双释放。递归清空元素内嵌指针（堆块归接管方，
           不释放）。 */
        slice_value_t *sv = (slice_value_t *)data;
        if (!sv->ptr) return;
        const type_t *et = slice_type_elem(t);
        if (et) slice_clear_elems(vm, et, sv->ptr, sv->len);
        sv->ptr = NULL;
        sv->len = 0;
        return;
    }
    default:
        /* 标量/字符串/ref *T/cunion/opaque 等：无 own 指针可清 */
        return;
    }
}

/* ---- 生命周期：clone / assign / dispose ---- */

/* 深拷贝 type 类型的数据块（对象图克隆）：
   所有权纯编译期——运行期 clone 不按 ownership 分派。
   - 指针且 ptr≠NULL：分配新堆块 + 递归克隆被指向类型，新指针指向独立新堆块
   - ptr=NULL：复制指针值
   - 切片且 ptr≠NULL：分配新堆块 + 逐元素递归克隆
   - 切片 ptr=NULL：胖指针值拷贝
   - struct/tuple/array/option/union：按字段/元素/tag 递归克隆
   - cunion：整块 memcpy
   - 标量/字符串：整块 memcpy
   注意：clone_block 只做深拷贝——是否深拷贝指针由调用方（value_clone /
   CLONE 指令）决定，不在本函数内按 ownership 分派。 */
void *ptr_clone_block(vm_t *vm, const type_t *type, const void *src) {
    void *dst = value_alloc_data(vm->alloc, type);
    switch (type->kind) {
    case TYPE_KIND_PTR: {
        const ptr_value_t *sp = (const ptr_value_t *)src;
        if (sp->ptr) {
            const type_t *base = ptr_type_base(type);
            if (!base) {
                memcpy(dst, src, type->size);
                break;
            }
            void *heap = ptr_clone_block(vm, base, sp->ptr);
            memcpy(dst, &heap, sizeof(heap));
        } else {
            memcpy(dst, src, type->size);
        }
        break;
    }
    case TYPE_KIND_SLICE: {
        const slice_value_t *ss = (const slice_value_t *)src;
        if (!ss->ptr) {
            memcpy(dst, src, type->size);
            break;
        }
        const type_t *et = slice_type_elem(type);
        if (!et) {
            memcpy(dst, src, type->size);
            break;
        }
        size_t total = (size_t)ss->len * et->size;
        void *new_heap = allocator_new_ex(vm->alloc, "slice_heap", total,
                                          NULL, NULL, NULL, 1);
        if (!new_heap) panic("vm: out of memory cloning slice block");
        const uint8_t *sp = (const uint8_t *)ss->ptr;
        uint8_t *dp = (uint8_t *)new_heap;
        for (uint64_t i = 0; i < ss->len; i++, sp += et->size, dp += et->size) {
            void *se = ptr_clone_block(vm, et, sp);
            memcpy(dp, se, et->size);
            allocator_free(vm->alloc, (void **)&se);
        }
        slice_value_t new_sv = { new_heap, ss->len };
        memcpy(dst, &new_sv, sizeof(slice_value_t));
        break;
    }
    default:
        memcpy(dst, src, type->size);
        break;
    }
    return dst;
}

/* clone：所有权纯编译期——运行期 clone 统一为浅拷贝指针值。
   深拷贝由 CLONE 指令（op_clone）显式调用 ptr_clone_block 完成，
   vtable->clone 只做浅拷贝（指针值复制）。 */
static value_t *ptr_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);
    return ptr_make_value(vm, t, pv->ptr);
}

static value_t *ptr_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_type(src) != value_type(dst)) {
        value_t *casted = value_implicit_cast(vm, src, value_type(dst));
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    /* 所有权纯编译期——assign 不释放旧堆块（由 sema 在赋值点生成 FREE）。 */
    const type_t *dt = value_type(dst);
    memcpy(value_data(dst), value_data(src), dt->size);
    return dst;
}

static void ptr_dispose(vm_t *vm, value_t *v) {
    /* 所有权纯编译期——dispose 不释放堆块（由 FREE 指令负责）。 */
    (void)vm; (void)v;
}

/* ---- eq/ne：指针目标地址比较 ---- */

static value_t *ptr_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const type_t *ta = value_type(a);
    const type_t *tb = value_type(b);
    if (ta != tb || ptr_type_base(ta) != ptr_type_base(tb)) {
        return value_make_error(vm,
            "ptr: == requires same pointer type");
    }
    const ptr_value_t *pa = (const ptr_value_t *)value_data(a);
    const ptr_value_t *pb = (const ptr_value_t *)value_data(b);
    bool eq = pa->ptr == pb->ptr;
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    memcpy(data, &eq, sizeof(bool));
    return value_make(vm, vm->type_bool, data);
}

static value_t *ptr_ne(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const type_t *ta = value_type(a);
    const type_t *tb = value_type(b);
    if (ta != tb || ptr_type_base(ta) != ptr_type_base(tb)) {
        return value_make_error(vm,
            "ptr: != requires same pointer type");
    }
    const ptr_value_t *pa = (const ptr_value_t *)value_data(a);
    const ptr_value_t *pb = (const ptr_value_t *)value_data(b);
    bool ne = pa->ptr != pb->ptr;
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    memcpy(data, &ne, sizeof(bool));
    return value_make(vm, vm->type_bool, data);
}

/* ---- 类型转换 ---- */

/* 目标是否为 opaque 类型（vtable 是 VTABLE_OPAQUE） */
static bool is_opaque_type(const type_t *t) {
    return t && t->vtable == &VTABLE_OPAQUE;
}

/* target_base 是否等于 src_base、或只是它外层加了 const/volatile（可嵌套）。
 * C 语义：**限定符只能加不能去**——`*T → *const T` 永远安全，
 * 反向（去 const）必须显式 `as`。 */
static bool ptr_upcast_keeps_quals(const type_t *src_base,
                                   const type_t *target_base) {
    if (type_has_const(src_base) && !type_has_const(target_base)) return false;
    return true;
}

static bool ptr_base_adds_quals(const type_t *src_base, const type_t *target_base) {
    if (src_base == target_base) return true;
    const type_t *t = target_base;
    while (t && (t->kind == TYPE_KIND_CONST || t->kind == TYPE_KIND_VOLATILE)) {
        const type_t *sub = (t->kind == TYPE_KIND_CONST)
                                ? ((const const_type_t *)t)->sub
                                : ((const volatile_type_t *)t)->sub;
        if (!sub) break;
        if (sub == src_base) return true;
        t = sub;
    }
    return false;
}

/* 隐式转换的**类型层判据**（不接触 data）：判断 `src → target` 是否允许。
 * 所有权纯编译期——类型不区分 own/ref/fatal，转换判据只看 base 类型兼容性。
 * ownership 降级（own→ref）和接管（fatal→own）不再在此处理——它们是同类型
 * 间的值层操作，sema 在编译期控制。 */
static bool ptr_implicit_cast_ok(vm_t *vm, const type_t *src,
                                 const type_t *target) {
    if (!src || !target) return false;
    if (src == target) return true; /* 同类型身份拷贝 */
    if (is_opaque_type(target)) return true; /* 任意指针 → opaque */

    /* 只给 pointee 加限定：身份拷贝 */
    if (ptr_base_adds_quals(ptr_type_base(src), ptr_type_base(target)))
        return true;

    /* 结构体指针向上转换（大 → 小，前缀字段兼容） */
    if (ptr_upcast_keeps_quals(ptr_type_base(src), ptr_type_base(target)) &&
        struct_type_prefix_compatible(vm,
            type_strip_quals(ptr_type_base(target)),
            type_strip_quals(ptr_type_base(src))))
        return true;

    return false;
}

static value_t *ptr_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    const type_t *src = value_type(v);

    if (value_is_shadow(v)) {
        if (!ptr_implicit_cast_ok(vm, src, target))
            return value_make_error(vm, "ptr: unsupported implicit cast for pointer");
        return value_make_shadow(vm, target);
    }

    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);

    /* 任意指针 → opaque 隐式（m3-design §8.4）：身份拷贝指针目标地址。 */
    if (is_opaque_type(target)) {
        void *data = value_alloc_data_copy(vm->alloc, target, &pv->ptr);
        return value_make(vm, target, data);
    }

    /* 目标只是给 pointee 加了 const/volatile：身份拷贝 */
    if (target && ptr_base_adds_quals(ptr_type_base(src), ptr_type_base(target))) {
        return ptr_make_value(vm, target, pv->ptr);
    }

    /* 结构体指针向上自动转换（大 → 小，前缀字段兼容） */
    if (target &&
        ptr_upcast_keeps_quals(ptr_type_base(src), ptr_type_base(target)) &&
        struct_type_prefix_compatible(vm,
            type_strip_quals(ptr_type_base(target)),
            type_strip_quals(ptr_type_base(src)))) {
        return ptr_make_value(vm, target, pv->ptr);
    }

    /* 同类型身份拷贝 */
    if (src == target) {
        return ptr_make_value(vm, target, pv->ptr);
    }

    return value_make_error(vm, "ptr: unsupported implicit cast for pointer");
}

static value_t *ptr_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (value_is_shadow(v)) return value_make_shadow(vm, target);

    /* 同类型身份拷贝 */
    if (value_type(v) == target) {
        const ptr_value_t *pv = (const ptr_value_t *)value_data(v);
        return ptr_make_value(vm, target, pv->ptr);
    }

    return value_make_error(vm, "ptr: unsupported explicit cast for pointer");
}

/* ---- 鸭子类型判断：按 base 递归（所有权纯编译期，类型不区分 ownership） ---- */

static bool ptr_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    if (a == b) return true;
    if (!b) return false;
    if (!type_is_ptr(a) || !type_is_ptr(b)) return false;
    return type_equal(vm, ptr_type_base(a), ptr_type_base(b));
}

static bool ptr_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    if (sub == sup) return true;
    if (!sup) return false;
    if (!type_is_ptr(sub)) return false;
    /* 结构体指针向上转换：Small 的字段是 Big 的前缀（C 公共起始序列）。 */
    if (ptr_upcast_keeps_quals(ptr_type_base(sub), ptr_type_base(sup)) &&
        struct_type_prefix_compatible(vm,
            type_strip_quals(ptr_type_base(sup)),
            type_strip_quals(ptr_type_base(sub))))
        return true;
    return type_extends(vm, ptr_type_base(sub), ptr_type_base(sup));
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_PTR = {
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

/* 构造名 "*T"（所有权纯编译期，类型名不携带 ownership 前缀） */
static char *ptr_name(allocator_t *alloc, const type_t *base) {
    size_t ol = 1;
    size_t bl = base && base->name.ptr ? base->name.len : 0;
    char *buf = allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL, NULL,
                                 ol + bl + 1);
    if (!buf) return NULL;
    buf[0] = '*';
    if (bl) memcpy(buf + ol, base->name.ptr, bl);
    buf[ol + bl] = '\0';
    return buf;
}

/* 分配开放 ptr_type（base_type=NULL，不入池，不压栈）。供字节码路径
 * （type_ptr_push）与 intern 快捷（type_ptr_intern）共用。
 * 所有权纯编译期——只有一种指针类型 *T。 */
static ptr_type_t *ptr_type_create_open(vm_t *vm) {
    ptr_type_t *pt = (ptr_type_t *)allocator_new_ex(
        vm->alloc, "ptr_type_t", sizeof(ptr_type_t), NULL, NULL, NULL, 1);
    if (!pt) panic("vm: out of memory allocating pointer type");
    memset(pt, 0, sizeof(ptr_type_t));
    pt->base.vtable = &VTABLE_PTR;
    pt->base.kind = TYPE_KIND_PTR;
    /* name/size/align 由 seal 按 base 填充；base_type 由 SET_TYPE 设定；sealed 置 0 */
    return pt;
}

/* PUSH_PTR：分配开放 ptr_type（base_type=NULL，不入池）+ 压其 type value。
 * 返回开放 type（未密封），经 SET_TYPE 设 base_type → SEAL 密封收尾。
 * 所有权纯编译期——只有一种指针类型 *T。 */
const type_t *type_ptr_push(vm_t *vm) {
    if (!vm) return NULL;
    ptr_type_t *pt = ptr_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &pt->base));
    return &pt->base; /* 外部只持有 type_t*，不感知 ptr_type_t 子类 */
}

/* SET_TYPE 运行期用：设被指向类型 T（密封后静默忽略） */
void type_ptr_set_base(vm_t *vm, const type_t *t, const type_t *base) {
    (void)vm;
    if (!t || type_is_sealed(t)) return;
    if (!type_is_ptr(t)) return;
    ((ptr_type_t *)t)->base_type = base;
}

/* SEAL：按 base_type 去重 intern（所有权纯编译期——只有一种指针类型 *T，
 * 同一 base 只有一个实例），标记 sealed，返回该 const type（允许链式）。
 * t 须为已设 base_type 的开放 ptr 类型（type_ptr_push 产物）。 */
const type_t *type_ptr_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t) return NULL;
    if (!type_is_ptr(t)) return NULL;
    if (type_is_sealed(t)) return t;  /* 已密封直接返回（幂等） */

    ptr_type_t *pt = (ptr_type_t *)t;

    /* 必须已设 base_type（SET_TYPE） */
    if (!pt->base_type) return NULL;

    if (!vm->ptr_types) vm->ptr_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern（按 base_type）：命中已有 sealed 类型则复用
     * 并手工回收本开放类型 */
    size_t n = vec_len(vm->ptr_types);
    for (size_t i = 0; i < n; i++) {
        const ptr_type_t *other = (const ptr_type_t *)vec_get(vm->ptr_types, i);
        if (other && other != pt && type_is_sealed(&other->base) &&
            other->base_type == pt->base_type) {
            if (pt->base.name.ptr) {
                char *np = (char *)pt->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&pt);
            return &other->base;
        }
    }

    /* 指针值 size/align 恒定（ptr_value_t = 裸指针，对齐指针宽），
       不依赖 base_type 布局 */
    pt->base.size   = sizeof(ptr_value_t);
    pt->base.align  = alignof(ptr_value_t);
    pt->base.sealed = true;

    /* 重建类型名 "*T" */
    char *name = ptr_name(vm->alloc, pt->base_type);
    if (!name) panic("vm: out of memory allocating pointer type name");
    pt->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->ptr_types, vm->alloc, pt);  /* 密封后入池（去重 intern） */
    return &pt->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_ptr_intern(vm_t *vm, const type_t *base) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。
     * 与 type_ptr_push 的区别：不向操作数栈压入 type value（嵌套构造
     * 场景避免栈布局污染）。去重 intern 由 type_ptr_seal 完成。
     * 所有权纯编译期——只有一种指针类型 *T。 */
    if (!vm || !base) return NULL;
    ptr_type_t *pt = ptr_type_create_open(vm);
    pt->base_type = base;
    return type_ptr_seal(vm, &pt->base);
}

/* 构造指针值：目标地址（所有权纯编译期，只有一种指针类型 *T）。
 * data 块 = ptr_value_t（值布局，见 type_ptr.h）。 */
value_t *ptr_make_value(vm_t *vm, const type_t *pt, void *target) {
    ptr_value_t pv = { target };
    void *data = value_alloc_data_copy(vm->alloc, pt, &pv);
    return value_make(vm, pt, data);
}
