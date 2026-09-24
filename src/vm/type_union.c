#include "vm/type_union.h"
#include "vm/type.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "vm/type_option.h" /* value_blit_raw / value_dispose_raw */
#include "vm/type_struct.h" /* type_struct_alloc_open / type_struct_add_field /
                               type_struct_seal（member payload struct） */
#include "core/panic.h"
#include "core/string.h"
#include "core/strslice.h"

#include <string.h>

/* ===========================================================================
 * tag union 类型（union）
 *
 * union_type_t 继承 type_t 持 member 表 union_member_t { name, tag,
 * payload_struct }。union value 的 data 块布局（密封时计算）：
 *   [0 .. tag_size)          tag 整数值（编号 0..member_count-1，宽度按
 *                            member_count 自适应：<=255→1, <=65535→2,
 *                            <=UINT32_MAX→4, 否则 8）
 *   [payload_offset .. size) 各 member payload 联合体（max member size）
 * 字段绝对偏移 = payload_offset + member 内字段偏移。
 *
 * 生命周期（clone/assign/dispose）按当前 tag 的 member payload 递归：
 *   先读 tag（union_read_tag）→ 定位 member → 其 payload_struct（NULL =
 *   纯 tag member，无 payload 区）→ 复用 value_blit_raw / value_dispose_raw
 *   （含 TYPE_KIND_UNION 分支：tag 平凡拷贝 + payload 按当前 tag 递归）。
 *   dispose 的 raw 版本同样先读 tag 才知道按哪个 member 释放。
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
    value_blit_raw(vm, data, value_data(v), t);
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
    value_dispose_raw(vm, value_data(dst), t);
    value_blit_raw(vm, value_data(dst), value_data(src), t);
    return dst;
}

static void union_dispose(vm_t *vm, value_t *v) {
    /* 借用引用不拥有 data（指向父值内部），跳过；由 value_dispose 统一拦截 */
    if (value_is_borrowed(v)) return;
    value_dispose_raw(vm, value_data(v), value_type(v));
}

/* ---- 判等：同 tag 按 payload 递归比较；不同 tag → false ---- */

static bool union_payload_equal(vm_t *vm, const void *a, const void *b,
                                const type_t *pt) {
    if (!pt) return true; /* 纯 tag member：无 payload，恒等 */
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
        /* 同 tag：payload 按该 member 的 payload_struct 递归比较 */
        if (ta < union_type_member_count(t)) {
            const union_member_t *m = union_type_member(t, (size_t)ta);
            r = union_payload_equal(vm,
                (const uint8_t *)value_data(a) + po,
                (const uint8_t *)value_data(b) + po,
                m ? m->payload_struct : NULL);
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
   member 的 payload_struct 密封后归 struct_types 池（其字段表/字段名由
   struct dispose 统一释放），此处不释放避免 double free。 */
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

    nm[n].name           = (strslice_t){ nbuf, name.len };
    nm[n].tag            = (uint32_t)n;
    nm[n].payload_struct = NULL;
    ut->members = nm;
    ut->member_count = n + 1;
    return &nm[n];
}

/* UNION_MEMBER 运行期用：追加 member（名拷贝到 vm 堆，tag = 追加序；
   payload 字段表经 type_union_add_field 追加到其开放 payload struct）。
   密封后静默忽略。 */
void type_union_add_member(vm_t *vm, const type_t *t, strslice_t name) {
    if (!vm || !t || t->kind != TYPE_KIND_UNION || type_is_sealed(t)) return;
    union_type_t *ut = (union_type_t *)t;
    union_grow_member(vm, ut, name);
}

/* 内部：构建 member 的开放 payload struct（type_struct_alloc_open 分配 + 追加
   字段用）。返回开放 struct type（未入池，未密封）。 */
static const type_t *union_open_payload_struct(vm_t *vm, union_type_t *ut) {
    if (ut->member_count == 0) return NULL;
    union_member_t *m = &ut->members[ut->member_count - 1];
    if (m->payload_struct) return m->payload_struct;
    const type_t *ps = type_struct_alloc_open(vm); /* 不压栈 */
    m->payload_struct = ps;
    return ps;
}

/* DEFINE_FIELD 运行期用（union 分支）：向当前（最后追加）member 的开放
   payload struct 追加字段（名拷贝到 vm 堆）。密封后静默忽略。 */
void type_union_add_field(vm_t *vm, const type_t *t, strslice_t name,
                          const type_t *ftype) {
    if (!vm || !t || t->kind != TYPE_KIND_UNION || type_is_sealed(t)) return;
    union_type_t *ut = (union_type_t *)t;
    if (ut->member_count == 0) return; /* 无 member：无 payload struct */
    const type_t *ps = union_open_payload_struct(vm, ut);
    if (!ps) return;
    type_struct_add_field(vm, ps, name, ftype);
}

/* ---- 密封期：构建各 member 的 payload struct（按字段表唯一构建） ---- */

/* member payload struct：开放阶段追加字段的开放 struct（type_struct_alloc_open
   分配），密封时直接 type_struct_seal——内部按 (字段名+类型+顺序) 去重
   intern（字段表一致的 member 共享同一 payload struct）。返回密封后的 struct
   type（归 struct_types 池）或 NULL。 */
static const type_t *union_seal_payload_struct(vm_t *vm, union_type_t *ut,
                                               size_t mi) {
    union_member_t *m = &ut->members[mi];
    if (!m->payload_struct) return NULL; /* 纯 tag member */
    const struct_type_t *open = (const struct_type_t *)m->payload_struct;

    /* 防御：字段类型必须已解析（NULL 会产生 size=0 的 struct） */
    for (size_t i = 0; i < open->field_count; i++) {
        if (!open->fields[i].type) return NULL;
    }

    /* 直接密封开放 payload struct：命中已 intern 的类型时开放对象被回收，
       未命中则就地密封（字段表保留，无需二次拷贝） */
    return type_struct_seal(vm, &open->base);
}

/* 按 (member 表内容) 判断两 union 是否等价（去重 intern 用）：
   member 名 + 顺序一致，且 payload 字段表结构一致（field_count + 字段名 +
   类型指针）。 */
static bool union_member_same(const union_member_t *a, const union_member_t *b) {
    if (a->tag != b->tag) return false;
    if (a->name.len != b->name.len) return false;
    if (a->name.ptr && b->name.ptr &&
        memcmp(a->name.ptr, b->name.ptr, a->name.len) != 0) return false;
    if ((a->name.ptr == NULL) != (b->name.ptr == NULL)) return false;
    /* payload struct 字段表结构比较（类型指针引用；seal 后不可变） */
    const struct_type_t *pa = (const struct_type_t *)a->payload_struct;
    const struct_type_t *pb = (const struct_type_t *)b->payload_struct;
    if ((pa == NULL) != (pb == NULL)) return false;
    if (!pa) return true; /* 两纯 tag member */
    if (pa->field_count != pb->field_count) return false;
    for (size_t i = 0; i < pa->field_count; i++) {
        const struct_field_t *fa = &pa->fields[i];
        const struct_field_t *fb = &pb->fields[i];
        if (fa->type != fb->type) return false;
        if (fa->name.len != fb->name.len) return false;
        if (fa->name.ptr && fb->name.ptr &&
            memcmp(fa->name.ptr, fb->name.ptr, fa->name.len) != 0) return false;
        if ((fa->name.ptr == NULL) != (fb->name.ptr == NULL)) return false;
    }
    return true;
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

/* 计算 member payload 联合体的最大 size / 对齐（纯 tag member 无 payload） */
static void union_payload_extent(vm_t *vm, const union_type_t *ut,
                                 size_t *max_size, size_t *max_align) {
    (void)vm;
    size_t ms = 0, ma = 1;
    for (size_t i = 0; i < ut->member_count; i++) {
        const type_t *ps = ut->members[i].payload_struct;
        if (!ps) continue;
        if (ps->size > ms) ms = ps->size;
        if (ps->align > ma) ma = ps->align;
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

/* SEAL：构建 member payload struct + 拷贝 member 表 + 布局（tag 宽度 +
   payload 偏移）+ 按 (member 表内容) 去重 intern + 置 sealed。返回密封后的
   const type（可能 != self）。 */
const type_t *type_union_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_UNION) return NULL;
    if (type_is_sealed(t)) return t; /* 幂等 */

    union_type_t *ut = (union_type_t *)t;

    /* member 列表不能为空（parser/sema 已校验；防御分支） */
    if (ut->member_count == 0) return NULL;

    /* 先构建各 member 的 payload struct（密封；字段类型依赖后序已由
       compiler/sema 保证先密封）——布局计算需要 payload size/align */
    for (size_t i = 0; i < ut->member_count; i++) {
        union_member_t *m = &ut->members[i];
        if (!m->payload_struct) continue;
        const type_t *sealed = union_seal_payload_struct(vm, ut, i);
        if (!sealed) return NULL;
        m->payload_struct = sealed;
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
    if (ps == 0) ut->base.align = 1; /* 纯 tag member union：无 payload 区 */
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

    /* 拷贝 member 表（vm 拥有；名深拷贝；payload 字段表按成员逐个复制） */
    union_member_t *copy = (union_member_t *)allocator_new_ex(
        vm->alloc, "union_member_t", sizeof(union_member_t), NULL, NULL,
        NULL, count);
    if (!copy) panic("vm: out of memory interning union type");
    for (size_t i = 0; i < count; i++) {
        char *nbuf = union_name_copy(vm->alloc, members[i].name);
        if (!nbuf) panic("vm: out of memory interning union member name");
        copy[i].name           = (strslice_t){ nbuf, members[i].name.len };
        copy[i].tag            = members[i].tag;
        copy[i].payload_struct = members[i].payload_struct;
    }
    ut->members = copy;
    ut->member_count = count;

    const type_t *t = type_union_seal(vm, &ut->base);
    if (!t) allocator_free(vm->alloc, (void **)&ut); /* 密封失败（如字段类型
                                                       未解析）：回收开放对象 */
    return t;
}

/* ===========================================================================
 * member / 字段查找
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

void union_type_find_field(const type_t *t, strslice_t name,
                           int *member_idx, int *field_idx) {
    *member_idx = -1;
    *field_idx  = -1;
    if (!t || t->kind != TYPE_KIND_UNION) return;
    const union_type_t *ut = (const union_type_t *)t;
    for (size_t i = 0; i < ut->member_count; i++) {
        const union_member_t *m = &ut->members[i];
        if (!m->payload_struct) continue;
        const struct_type_t *ps = (const struct_type_t *)m->payload_struct;
        for (size_t j = 0; j < ps->field_count; j++) {
            const struct_field_t *f = &ps->fields[j];
            if (f->name.len == name.len && f->name.ptr &&
                memcmp(f->name.ptr, name.ptr, name.len) == 0) {
                *member_idx = (int)i;
                *field_idx  = (int)j;
                return;
            }
        }
    }
}
