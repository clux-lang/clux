#include "vm/type_cunion.h"
#include "vm/type.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/strslice.h"

#include <string.h>

/* ===========================================================================
 * C 语义 union 类型（cunion）
 *
 * cunion_type_t 继承 type_t 持 member 表 cunion_member_t { name, type }。
 * cunion 与 struct/union 同构语法（字段平铺 = member，构造强制单字段）。
 * cunion value 的 data 块布局（密封时计算）：
 *   [0 .. size)  所有 member 共享的联合体（offset 全为 0，无 tag）
 * size = max(member size)（C 对齐）、align = max(member align)、
 * 空 member 列表 -> size = 1（对齐 struct 空类型 C 语义）。
 *
 * 生命周期（clone/assign/dispose）：data 全平凡（str 成员归 vm 字符串池），
 *   整块 memcpy / 无需释放——完全按 C union 语义：不深拷贝 string_t、
 *   dispose 为 no-op（开发者自负安全）。
 *
 * 判等（eq/ne）：memcmp 字节比较（str 成员按字节比较，不递归字符串内容）。
 *
 * 严格类型：implicit_cast / explicit_cast 仅同 cunion 实例（身份拷贝）；
 * type_equal / type_extends 按指针（具名类型；M2 鸭子类型检查留待后续 Phase）。
 * =========================================================================== */

/* ---- 生命周期：clone / assign / dispose（全平凡） ---- */

static value_t *cunion_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    void *data = value_alloc_data(vm->alloc, t);
    memcpy(data, value_data(v), t->size);  /* data 全平凡：整块 memcpy */
    return value_make(vm, t, data);
}

static value_t *cunion_assign(vm_t *vm, value_t *dst, value_t *src) {
    /* safe_cast 语义（与 struct_assign 同款）：类型不同先向左值类型
       implicit_cast——同 cunion 实例身份拷贝通过；不兼容 -> cast 返回
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

static void cunion_dispose(vm_t *vm, value_t *v) {
    /* data 全平凡（str 成员归 vm 字符串池），无需释放——C union 语义：
       不深拷贝 string_t，dispose 为 no-op（开发者自负安全）。借用引用
       与 data 块本身由 value_dispose 统一处理。 */
    (void)vm; (void)v;
}

/* ---- 判等：memcmp 字节比较（C union 语义） ---- */

static value_t *cunion_eq(vm_t *vm, value_t *a, value_t *b) {
    /* safe_cast 语义（与 struct_eq 同款）：类型不同时尝试右值 implicit_cast
       到左值类型——同 cunion 实例身份拷贝通过；不兼容 -> cast error 传播。 */
    VTABLE_BINARY(vm, a, b, eq, "==");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);

    const type_t *t = value_type(a);
    bool r = (memcmp(value_data(a), value_data(b), t->size) == 0);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &r);
    return value_make(vm, vm->type_bool, data);
}

static value_t *cunion_ne(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, ne, "!=");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    value_t *eq = cunion_eq(vm, a, b);
    if (value_is_error(vm, eq)) return eq;
    bool r = !value_as(eq, bool);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &r);
    return value_make(vm, vm->type_bool, data);
}

/* ---- 类型转换：仅同 cunion 实例（身份拷贝） ---- */

static value_t *cunion_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (!target || target != value_type(v))
        return value_make_error(vm, "cunion: implicit cast only within same cunion");
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
    return value_make(vm, target, data);
}

static value_t *cunion_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    return cunion_implicit_cast(vm, v, target);
}

/* ---- 鸭子类型判断：cunion 是独立具名类型 ---- */

static bool cunion_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    (void)vm;
    return a == b;
}

static bool cunion_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    (void)vm;
    return sub == sup;
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_CUNION = {
    .eq = cunion_eq, .ne = cunion_ne,
    .dispose = cunion_dispose,
    .clone = cunion_clone,
    .assign = cunion_assign,
    .implicit_cast = cunion_implicit_cast,
    .explicit_cast = cunion_explicit_cast,
    .type_equal = cunion_type_equal,
    .type_extends = cunion_type_extends,
    .type_seal = type_cunion_seal, /* 开放构造路径（PUSH_CUNION -> SEAL）密封入口 */
};

/* ===========================================================================
 * 开放构造（PUSH_CUNION / DEFINE_FIELD / SEAL）
 * =========================================================================== */

/* dispose_fn：allocator_free 时自动释放 member 名 + member 表 + 显示名
   （open 对象分配时注册，seal 去重回收与 vm_destroy 都只需裸 allocator_free）。
   member 的 type 归 type 池，此处不释放。 */
static void cunion_type_dispose(void *self, allocator_t *allocator) {
    cunion_type_t *ct = (cunion_type_t *)self;
    if (ct->members) {
        for (size_t i = 0; i < ct->member_count; i++) {
            cunion_member_t *m = &ct->members[i];
            if (m->name.ptr) {
                char *np = (char *)m->name.ptr;
                allocator_free(allocator, (void **)&np);
            }
        }
        allocator_free(allocator, (void **)&ct->members);
    }
    if (ct->base.name.ptr) {
        char *np = (char *)ct->base.name.ptr;
        allocator_free(allocator, (void **)&np);
    }
}

static cunion_type_t *cunion_type_create_open(vm_t *vm) {
    cunion_type_t *ct = (cunion_type_t *)allocator_new_ex(
        vm->alloc, "cunion_type_t", sizeof(cunion_type_t), NULL, NULL,
        cunion_type_dispose, 1);
    if (!ct) panic("vm: out of memory allocating cunion type");
    memset(ct, 0, sizeof(cunion_type_t));
    ct->base.vtable = &VTABLE_CUNION;
    ct->base.kind   = TYPE_KIND_CUNION;
    return ct;
}

/* PUSH_CUNION：分配空 cunion_type（members=NULL，不入池）+ 压其 type value */
const type_t *type_cunion_push(vm_t *vm) {
    if (!vm) return NULL;
    cunion_type_t *ct = cunion_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &ct->base));
    return &ct->base;
}

/* 内部：member 名拷贝（vm 拥有；seal 时整表再拷贝一次，开放期追加名在
   seal 后释放） */
static char *cunion_name_copy(allocator_t *alloc, strslice_t name) {
    char *buf = (char *)allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL,
                                         NULL, name.len + 1);
    if (!buf) return NULL;
    memcpy(buf, name.ptr, name.len);
    buf[name.len] = 0;
    return buf;
}

/* 内部：member 追加（动态扩容；名拷贝到 vm 堆） */
static cunion_member_t *cunion_grow_member(vm_t *vm, cunion_type_t *ct,
                                           strslice_t name) {
    char *nbuf = cunion_name_copy(vm->alloc, name);
    if (!nbuf) panic("vm: out of memory adding cunion member");

    size_t n = ct->member_count;
    cunion_member_t *nm = (cunion_member_t *)allocator_new_ex(
        vm->alloc, "cunion_member_t", sizeof(cunion_member_t), NULL, NULL, NULL,
        n + 1);
    if (!nm) panic("vm: out of memory adding cunion member");
    if (n > 0) memcpy(nm, ct->members, n * sizeof(cunion_member_t));
    if (ct->members) allocator_free(vm->alloc, (void **)&ct->members);

    nm[n].name = (strslice_t){ nbuf, name.len };
    nm[n].type = NULL;
    ct->members = nm;
    ct->member_count = n + 1;
    return &nm[n];
}

/* DEFINE_FIELD 运行期用（cunion 分支）：追加 member（名拷贝到 vm 堆；
   member 类型经 type_cunion_set_member_type 写入）。密封后静默忽略。 */
void type_cunion_add_member(vm_t *vm, const type_t *t, strslice_t name) {
    if (!vm || !t || t->kind != TYPE_KIND_CUNION || type_is_sealed(t)) return;
    cunion_type_t *ct = (cunion_type_t *)t;
    cunion_grow_member(vm, ct, name);
}

/* DEFINE_FIELD 运行期用（cunion 分支）：设置最后追加的 member 的类型。
   密封后静默忽略。 */
void type_cunion_set_member_type(vm_t *vm, const type_t *t, const type_t *ftype) {
    if (!vm || !t || t->kind != TYPE_KIND_CUNION || type_is_sealed(t)) return;
    cunion_type_t *ct = (cunion_type_t *)t;
    if (ct->member_count == 0) return; /* 无 member：无处写入 */
    cunion_member_t *m = &ct->members[ct->member_count - 1];
    m->type = ftype;
}

/* ---- 密封期：布局计算 + 去重 intern ---- */

/* 按 (member 表内容) 判断两 cunion 是否等价（去重 intern 用）：
   member 名 + 顺序一致，且类型指针一致。 */
static bool cunion_member_same(const cunion_member_t *a, const cunion_member_t *b) {
    if (a->name.len != b->name.len) return false;
    if (a->name.ptr && b->name.ptr &&
        memcmp(a->name.ptr, b->name.ptr, a->name.len) != 0) return false;
    if ((a->name.ptr == NULL) != (b->name.ptr == NULL)) return false;
    return a->type == b->type;
}

static bool cunion_same(const cunion_type_t *a, const cunion_type_t *b) {
    if (a->member_count != b->member_count) return false;
    for (size_t i = 0; i < a->member_count; i++) {
        if (!cunion_member_same(&a->members[i], &b->members[i])) return false;
    }
    return true;
}

/* C 对齐规则向上取整 */
static size_t cunion_align_up(size_t v, size_t a) {
    if (a <= 1) return v;
    return (v + a - 1) / a * a;
}

/* 计算 member 联合体的最大 size / 对齐（C union：offset 全 0，共享内存） */
static void cunion_member_extent(vm_t *vm, const cunion_type_t *ct,
                                 size_t *max_size, size_t *max_align) {
    (void)vm;
    size_t ms = 0, ma = 1;
    for (size_t i = 0; i < ct->member_count; i++) {
        const type_t *mt = ct->members[i].type;
        if (!mt) continue;
        if (mt->size > ms) ms = mt->size;
        if (mt->align > ma) ma = mt->align;
    }
    *max_size = ms;
    *max_align = ma;
}

/* SEAL：拷贝 member 表 + 布局（size = max(member size) 对齐）+ 按 (member 表
   内容) 去重 intern + 置 sealed。返回密封后的 const type（可能 != self）。 */
const type_t *type_cunion_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_CUNION) return NULL;
    if (type_is_sealed(t)) return t; /* 幂等 */

    cunion_type_t *ct = (cunion_type_t *)t;

    /* 防御：每个 member 的类型必须已解析（NULL 会产生 size=0 的联合体区）。
       依赖后序由 compiler/sema 保证先密封。 */
    for (size_t i = 0; i < ct->member_count; i++) {
        if (!ct->members[i].type) return NULL;
    }

    if (!vm->cunion_types)
        vm->cunion_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern：命中已有 sealed 类型则复用并手工回收本开放类型
       （allocator_free 自动调 dispose_fn 释放 member 表） */
    size_t n = vec_len(vm->cunion_types);
    for (size_t i = 0; i < n; i++) {
        const cunion_type_t *other = (const cunion_type_t *)vec_get(vm->cunion_types, i);
        if (other && other != ct && type_is_sealed(&other->base) &&
            cunion_same(other, ct)) {
            allocator_free(vm->alloc, (void **)&ct);
            return &other->base;
        }
    }

    /* 计算布局：size = max(member size)（C 对齐），align = max(member align)；
       所有 member offset 全为 0（C union 语义，共享内存，无 tag）。 */
    size_t ms, ma;
    cunion_member_extent(vm, ct, &ms, &ma);
    ct->base.size  = cunion_align_up(ms, ma);
    ct->base.align = ma;
    if (ms == 0) ct->base.align = 1; /* 防御：所有 member 类型 size=0 */
    if (ct->base.size == 0) ct->base.size = 1; /* 防御：保证 data 块可分配
                                                  （对齐 struct 空类型 C 语义） */
    ct->base.sealed = true;

    vec_push(vm->cunion_types, vm->alloc, ct); /* 密封后入池（去重 intern） */
    return &ct->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_cunion_intern(vm_t *vm, const cunion_member_t *members,
                                 size_t count) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。不向操作数栈
     * 压入 type value。去重 intern 由 type_cunion_seal 完成。
     * 空 member 列表（count==0）合法：seal 置 size=1 占位（对齐 struct 空
     * 类型 C 语义），sema 空 cunion 定义走此路径。 */
    if (!vm) return NULL;
    cunion_type_t *ct = cunion_type_create_open(vm);

    /* 拷贝 member 表（vm 拥有；名深拷贝；类型按成员逐个复制）。
       空 member 列表（count==0）跳过分配（对齐 struct 空类型）。 */
    if (count > 0) {
        cunion_member_t *copy = (cunion_member_t *)allocator_new_ex(
            vm->alloc, "cunion_member_t", sizeof(cunion_member_t), NULL, NULL,
            NULL, count);
        if (!copy) panic("vm: out of memory interning cunion type");
        for (size_t i = 0; i < count; i++) {
            char *nbuf = cunion_name_copy(vm->alloc, members[i].name);
            if (!nbuf) panic("vm: out of memory interning cunion member name");
            copy[i].name = (strslice_t){ nbuf, members[i].name.len };
            copy[i].type = members[i].type;
        }
        ct->members = copy;
        ct->member_count = count;
    }

    const type_t *t = type_cunion_seal(vm, &ct->base);
    if (!t) allocator_free(vm->alloc, (void **)&ct); /* 密封失败（如字段类型
                                                       未解析）：回收开放对象 */
    return t;
}

/* ===========================================================================
 * member 查找
 * =========================================================================== */

int cunion_type_find_member(const type_t *t, strslice_t name) {
    if (!t || t->kind != TYPE_KIND_CUNION) return -1;
    const cunion_type_t *ct = (const cunion_type_t *)t;
    for (size_t i = 0; i < ct->member_count; i++) {
        const cunion_member_t *m = &ct->members[i];
        if (m->name.len == name.len && m->name.ptr &&
            memcmp(m->name.ptr, name.ptr, name.len) == 0)
            return (int)i;
    }
    return -1;
}
