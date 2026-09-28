#include "vm/type_union.h"
#include "vm/type.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/strslice.h"

#include <string.h>

/* ===========================================================================
 * tag union 类型（union）
 *
 * union_type_t 继承 type_t 持 member 表 union_member_t { name, tag,
 * payload_type }。union 与 struct 同构语法（字段平铺 = member，构造强制
 * 单字段）。union value 的 data 块布局（密封时计算）：
 *   [0 .. tag_size)          tag 整数值（编号 0..member_count-1，宽度按
 *                            member_count 自适应：<=255→1, <=65535→2,
 *                            <=UINT32_MAX→4, 否则 8）
 *   [payload_offset .. size) 各 member payload 联合体（max member size）
 * member payload 偏移 = payload_offset（单字段，无内部偏移）。
 *
 * 生命周期（clone/assign/dispose）：data 全平凡（tag + payload 为 memcpy
 *   可拷贝的裸字节块，字符串归 vm 字符串池），整块 memcpy / 无需释放。
 *
 * 严格类型：implicit_cast / explicit_cast 仅同 union 实例（身份拷贝）；
 * eq/ne 按 tag + payload 递归比较（不同 tag → false，同 tag → payload
 * 递归比较）；type_equal / type_extends 按指针（具名类型；M2 鸭子类型
 * 检查留待后续 Phase）。
 * =========================================================================== */

/* ---- 内部：按 tag_size 宽度读写 tag 整数值 ---- */

uint64_t union_read_tag(const value_t *v) {
    if (!v || value_kind(v) != TYPE_KIND_UNION) return 0;
    const type_t *t = value_type(v);
    return union_read_tag_raw(value_data(v), union_type_tag_size(t));
}

/* ---- 生命周期：clone / assign / dispose（按当前 tag 的 member 递归） ---- */

static value_t *union_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    void *data = value_alloc_data(vm->alloc, t);
    memcpy(data, value_data(v), t->size);  /* data 全平凡：整块 memcpy */
    return value_make(vm, t, data);
}

static value_t *union_assign(vm_t *vm, value_t *dst, value_t *src) {
    /* safe_cast 语义（与 struct_assign 同款）：类型不同先向左值类型
       implicit_cast——同 union 实例身份拷贝通过；不兼容 → cast 返回
       error，直接传播。 */
    if (value_type(src) != value_type(dst)) {
        value_t *casted = value_implicit_cast(vm, src, value_type(dst));
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    const type_t *t = value_type(dst);
    memcpy(value_data(dst), value_data(src), t->size);  /* 平凡覆盖 */
    return dst;
}

static void union_dispose(vm_t *vm, value_t *v) {
    /* data 全平凡（tag + payload 为裸字节块，字符串归池），无需释放。
       借用引用与 data 块本身由 value_dispose 统一处理。 */
    (void)vm; (void)v;
}

/* ---- 判等：同 tag 按 payload 递归比较；不同 tag → false ---- */

static bool union_payload_equal(vm_t *vm, const void *a, const void *b,
                                const type_t *pt) {
    value_t *va = value_make_borrowed(vm, pt, (void *)a);
    value_t *vb = value_make_borrowed(vm, pt, (void *)b);
    value_t *r = value_eq(vm, va, vb);
    if (value_is_error(vm, r)) return false;
    return value_as(r, bool);
}

static value_t *union_eq(vm_t *vm, value_t *a, value_t *b) {
    /* safe_cast 语义（与 struct_eq 同款）：类型不同时尝试右值 implicit_cast
       到左值类型——同 union 实例身份拷贝通过；不兼容 → cast error 传播。 */
    VTABLE_BINARY(vm, a, b, eq, "==");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);

    const type_t *t = value_type(a);
    size_t po = union_type_payload_offset(t);
    uint64_t ta = union_read_tag(a);
    uint64_t tb = union_read_tag(b);
    bool r = (ta == tb);
    if (r) {
        /* 同 tag：payload 按该 member 的 payload_type 递归比较 */
        if (ta < union_type_member_count(t)) {
            const union_member_t *m = union_type_member(t, (size_t)ta);
            r = union_payload_equal(vm,
                (const uint8_t *)value_data(a) + po,
                (const uint8_t *)value_data(b) + po,
                m ? m->payload_type : NULL);
        }
    }
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &r);
    return value_make(vm, vm->type_bool, data);
}

static value_t *union_ne(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, ne, "!=");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    value_t *eq = union_eq(vm, a, b);
    if (value_is_error(vm, eq)) return eq;
    bool r = !value_as(eq, bool);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &r);
    return value_make(vm, vm->type_bool, data);
}

/* ---- 类型转换：仅同 union 实例（身份拷贝） ---- */

static value_t *union_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (!target || target != value_type(v))
        return value_make_error(vm, "union: implicit cast only within same union");
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
    return value_make(vm, target, data);
}

static value_t *union_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    return union_implicit_cast(vm, v, target);
}

/* ---- 鸭子类型判断：union 是独立具名类型 ---- */

static bool union_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    (void)vm;
    return a == b;
}

static bool union_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    (void)vm;
    return sub == sup;
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_UNION = {
    .eq = union_eq, .ne = union_ne,
    .dispose = union_dispose,
    .clone = union_clone,
    .assign = union_assign,
    .implicit_cast = union_implicit_cast,
    .explicit_cast = union_explicit_cast,
    .type_equal = union_type_equal,
    .type_extends = union_type_extends,
    .type_seal = type_union_seal, /* 开放构造路径（PUSH_UNION → SEAL）密封入口 */
};

/* ===========================================================================
 * 开放构造（PUSH_UNION / UNION_MEMBER / DEFINE_FIELD / SEAL）
 * =========================================================================== */

/* dispose_fn：allocator_free 时自动释放 member 名 + member 表 + 显示名
   （open 对象分配时注册，seal 去重回收与 vm_destroy 都只需裸 allocator_free）。
   member 的 payload_type 归 type 池，此处不释放。 */
static void union_type_dispose(void *self, allocator_t *allocator) {
    union_type_t *ut = (union_type_t *)self;
    if (ut->members) {
        for (size_t i = 0; i < ut->member_count; i++) {
            union_member_t *m = &ut->members[i];
            if (m->name.ptr) {
                char *np = (char *)m->name.ptr;
                allocator_free(allocator, (void **)&np);
            }
        }
        allocator_free(allocator, (void **)&ut->members);
    }
    if (ut->base.name.ptr) {
        char *np = (char *)ut->base.name.ptr;
        allocator_free(allocator, (void **)&np);
    }
}

static union_type_t *union_type_create_open(vm_t *vm) {
    union_type_t *ut = (union_type_t *)allocator_new_ex(
        vm->alloc, "union_type_t", sizeof(union_type_t), NULL, NULL,
        union_type_dispose, 1);
    if (!ut) panic("vm: out of memory allocating union type");
    memset(ut, 0, sizeof(union_type_t));
    ut->base.vtable = &VTABLE_UNION;
    ut->base.kind   = TYPE_KIND_UNION;
    return ut;
}

/* PUSH_UNION：分配空 union_type（members=NULL，不入池）+ 压其 type value */
const type_t *type_union_push(vm_t *vm) {
    if (!vm) return NULL;
    union_type_t *ut = union_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &ut->base));
    return &ut->base;
}

/* 内部：member 名拷贝（vm 拥有；seal 时整表再拷贝一次，开放期追加名在
   seal 后释放） */
static char *union_name_copy(allocator_t *alloc, strslice_t name) {
    char *buf = (char *)allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL,
                                         NULL, name.len + 1);
    if (!buf) return NULL;
    memcpy(buf, name.ptr, name.len);
    buf[name.len] = '\0';
    return buf;
}

/* 内部：member 追加（动态扩容；名拷贝到 vm 堆，tag = 追加序） */
static union_member_t *union_grow_member(vm_t *vm, union_type_t *ut,
                                         strslice_t name) {
    char *nbuf = union_name_copy(vm->alloc, name);
    if (!nbuf) panic("vm: out of memory adding union member");

    size_t n = ut->member_count;
    union_member_t *nm = (union_member_t *)allocator_new_ex(
        vm->alloc, "union_member_t", sizeof(union_member_t), NULL, NULL, NULL,
        n + 1);
    if (!nm) panic("vm: out of memory adding union member");
    if (n > 0) memcpy(nm, ut->members, n * sizeof(union_member_t));
    if (ut->members) allocator_free(vm->alloc, (void **)&ut->members);

    nm[n].name         = (strslice_t){ nbuf, name.len };
    nm[n].tag          = (uint32_t)n;
    nm[n].payload_type = NULL;
    ut->members = nm;
    ut->member_count = n + 1;
    return &nm[n];
}

/* UNION_MEMBER 运行期用：追加 member（名拷贝到 vm 堆，tag = 追加序；
   member 类型经 type_union_set_member_type 写入）。密封后静默忽略。 */
void type_union_add_member(vm_t *vm, const type_t *t, strslice_t name) {
    if (!vm || !t || t->kind != TYPE_KIND_UNION || type_is_sealed(t)) return;
    union_type_t *ut = (union_type_t *)t;
    union_grow_member(vm, ut, name);
}

/* DEFINE_FIELD 运行期用（union 分支）：设置最后追加的 member 的 payload
   类型。密封后静默忽略。 */
void type_union_set_member_type(vm_t *vm, const type_t *t, const type_t *ftype) {
    if (!vm || !t || t->kind != TYPE_KIND_UNION || type_is_sealed(t)) return;
    union_type_t *ut = (union_type_t *)t;
    if (ut->member_count == 0) return; /* 无 member：无处写入 */
    union_member_t *m = &ut->members[ut->member_count - 1];
    m->payload_type = ftype;
}

/* ---- 密封期：布局计算 + 去重 intern ---- */

/* 按 (member 表内容) 判断两 union 是否等价（去重 intern 用）：
   member 名 + 顺序一致，且 payload 类型指针一致。 */
static bool union_member_same(const union_member_t *a, const union_member_t *b) {
    if (a->tag != b->tag) return false;
    if (a->name.len != b->name.len) return false;
    if (a->name.ptr && b->name.ptr &&
        memcmp(a->name.ptr, b->name.ptr, a->name.len) != 0) return false;
    if ((a->name.ptr == NULL) != (b->name.ptr == NULL)) return false;
    return a->payload_type == b->payload_type;
}

static bool union_same(const union_type_t *a, const union_type_t *b) {
    if (a->member_count != b->member_count) return false;
    for (size_t i = 0; i < a->member_count; i++) {
        if (!union_member_same(&a->members[i], &b->members[i])) return false;
    }
    return true;
}

/* C 对齐规则向上取整 */
static size_t union_align_up(size_t v, size_t a) {
    if (a <= 1) return v;
    return (v + a - 1) / a * a;
}

/* 计算 member payload 联合体的最大 size / 对齐 */
static void union_payload_extent(vm_t *vm, const union_type_t *ut,
                                 size_t *max_size, size_t *max_align) {
    (void)vm;
    size_t ms = 0, ma = 1;
    for (size_t i = 0; i < ut->member_count; i++) {
        const type_t *pt = ut->members[i].payload_type;
        if (!pt) continue;
        if (pt->size > ms) ms = pt->size;
        if (pt->align > ma) ma = pt->align;
    }
    *max_size = ms;
    *max_align = ma;
}

/* 按 member_count 自适应 tag 宽度：<=255→1，<=65535→2，<=UINT32_MAX→4，否则 8 */
static size_t union_tag_width(size_t member_count) {
    if (member_count <= UINT8_MAX) return 1;
    if (member_count <= UINT16_MAX) return 2;
    if (member_count <= UINT32_MAX) return 4;
    return 8;
}

/* SEAL：拷贝 member 表 + 布局（tag 宽度 + payload 偏移）+ 按 (member 表
   内容) 去重 intern + 置 sealed。返回密封后的 const type（可能 != self）。 */
const type_t *type_union_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_UNION) return NULL;
    if (type_is_sealed(t)) return t; /* 幂等 */

    union_type_t *ut = (union_type_t *)t;

    /* member 列表不能为空（parser/sema 已校验；防御分支） */
    if (ut->member_count == 0) return NULL;

    /* 防御：每个 member 的 payload 类型必须已解析（NULL 会产生 size=0 的
       payload 区）。依赖后序由 compiler/sema 保证先密封。 */
    for (size_t i = 0; i < ut->member_count; i++) {
        if (!ut->members[i].payload_type) return NULL;
    }

    if (!vm->union_types) vm->union_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern：命中已有 sealed 类型则复用并手工回收本开放类型
       （allocator_free 自动调 dispose_fn 释放 member 表） */
    size_t n = vec_len(vm->union_types);
    for (size_t i = 0; i < n; i++) {
        const union_type_t *other = (const union_type_t *)vec_get(vm->union_types, i);
        if (other && other != ut && type_is_sealed(&other->base) &&
            union_same(other, ut)) {
            allocator_free(vm->alloc, (void **)&ut);
            return &other->base;
        }
    }

    /* 计算布局：tag 宽度自适应 + payload 联合体对齐偏移 */
    size_t tag_sz = union_tag_width(ut->member_count);
    size_t ps, pa;
    union_payload_extent(vm, ut, &ps, &pa);
    size_t po = union_align_up(tag_sz, pa);
    ut->tag_size       = tag_sz;
    ut->payload_offset = po;
    ut->base.size      = po + ps;
    ut->base.align     = pa;
    if (ps == 0) ut->base.align = 1; /* 防御：所有 member 类型 size=0 */
    if (ut->base.size == 0) ut->base.size = 1; /* 防御：保证 data 块可分配 */
    ut->base.sealed = true;

    vec_push(vm->union_types, vm->alloc, ut); /* 密封后入池（去重 intern） */
    return &ut->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_union_intern(vm_t *vm, const union_member_t *members,
                                size_t count) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。不向操作数栈
     * 压入 type value。去重 intern 由 type_union_seal 完成。 */
    if (!vm || !members || count == 0) return NULL;
    union_type_t *ut = union_type_create_open(vm);

    /* 拷贝 member 表（vm 拥有；名深拷贝；payload 类型按成员逐个复制） */
    union_member_t *copy = (union_member_t *)allocator_new_ex(
        vm->alloc, "union_member_t", sizeof(union_member_t), NULL, NULL,
        NULL, count);
    if (!copy) panic("vm: out of memory interning union type");
    for (size_t i = 0; i < count; i++) {
        char *nbuf = union_name_copy(vm->alloc, members[i].name);
        if (!nbuf) panic("vm: out of memory interning union member name");
        copy[i].name         = (strslice_t){ nbuf, members[i].name.len };
        copy[i].tag          = members[i].tag;
        copy[i].payload_type = members[i].payload_type;
    }
    ut->members = copy;
    ut->member_count = count;

    const type_t *t = type_union_seal(vm, &ut->base);
    if (!t) allocator_free(vm->alloc, (void **)&ut); /* 密封失败（如字段类型
                                                       未解析）：回收开放对象 */
    return t;
}

/* ===========================================================================
 * member 查找
 * =========================================================================== */

int union_type_find_member(const type_t *t, strslice_t name) {
    if (!t || t->kind != TYPE_KIND_UNION) return -1;
    const union_type_t *ut = (const union_type_t *)t;
    for (size_t i = 0; i < ut->member_count; i++) {
        const union_member_t *m = &ut->members[i];
        if (m->name.len == name.len && m->name.ptr &&
            memcmp(m->name.ptr, name.ptr, name.len) == 0)
            return (int)i;
    }
    return -1;
}
