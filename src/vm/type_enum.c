#include "vm/type_enum.h"
#include "vm/type.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/panic.h"
#include "core/strslice.h"

#include <string.h>

/* ===========================================================================
 * 枚举类型（enum）
 *
 * enum_type_t 继承 type_t 持 underlying（底层整型）与 variant 表。enum value
 * 的 data 是底层宽度的整数值块（size = underlying->size，align =
 * underlying->align），MAKE_ENUM 构造时按底层宽度截断。
 *
 * 严格类型分离（m2-design §3/§5）：
 *   - implicit_cast 仅同 enum 实例（v->type == target）身份拷贝；
 *     enum → 底层或反之 → error（无隐式转换）。
 *   - explicit_cast 仅 enum → 声明底层类型（按底层宽度截断拷贝）；
 *     target == 同 enum → clone；其他（含跳步 i8）→ error。
 *   - eq/ne 仅同 enum 实例按底层整数值比较；vs 底层/不同 enum → error。
 *   - type_equal / type_extends：enum 是独立具名类型（按指针），
 *     enum extends 底层 = false（严格分离）。
 * =========================================================================== */

/* ---- 按底层宽度读写整数值 ---- */

static int64_t enum_read_value(const value_t *v) {
    const type_t *u = enum_type_underlying(value_type(v));
    size_t sz = u ? u->size : value_type(v)->size;
    switch (sz) {
    case 1: return (int64_t)*(const int8_t  *)value_data(v);
    case 2: return (int64_t)*(const int16_t *)value_data(v);
    case 4: return (int64_t)*(const int32_t *)value_data(v);
    default: return *(const int64_t *)value_data(v);
    }
}

/* 按底层宽度截断存储 int64 值（低位截断，与 int_store 同语义） */
static void enum_store_value(vm_t *vm, void *data, const type_t *u, int64_t v) {
    switch (u->size) {
    case 1: *(int8_t  *)data = (int8_t)v;  break;
    case 2: *(int16_t *)data = (int16_t)v; break;
    case 4: *(int32_t *)data = (int32_t)v; break;
    default: *(int64_t *)data = v;         break;
    }
    (void)vm;
}

/* ---- 生命周期：clone / assign / dispose ---- */

static value_t *enum_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    void *data = value_alloc_data_copy(vm->alloc, value_type(v), value_data(v));
    return value_make(vm, value_type(v), data);
}

static value_t *enum_assign(vm_t *vm, value_t *dst, value_t *src) {
    /* 类型校验先于 shadow 短路（int_assign 同款顺序）：shadow 值也须通过
       类型检查——sema 编译期靠此拦截 enum 与异型（含底层）间的赋值。
       enum 无隐式转换路径（严格分离），任何类型不匹配都是错误。 */
    if (value_type(src) != value_type(dst)) {
        return value_make_error(vm,
            "assign: enum type mismatch on assignment");
    }
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    memcpy(value_data(dst), value_data(src), value_type(dst)->size);
    return dst;
}

static void enum_dispose(vm_t *vm, value_t *v) {
    /* 标量值块无资源（底层整数值）；借用引用由 value_dispose 统一拦截 */
    (void)vm; (void)v;
}

/* ---- 判等：仅同 enum 实例按底层整数值比较 ---- */

static value_t *enum_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_type(a) != value_type(b)) {
        return value_make_error(vm,
            "enum: compare only within same enum");
    }
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    bool r = (enum_read_value(a) == enum_read_value(b));
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &r);
    return value_make(vm, vm->type_bool, data);
}

static value_t *enum_ne(vm_t *vm, value_t *a, value_t *b) {
    if (value_type(a) != value_type(b)) {
        return value_make_error(vm,
            "enum: compare only within same enum");
    }
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    bool r = (enum_read_value(a) != enum_read_value(b));
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &r);
    return value_make(vm, vm->type_bool, data);
}

/* ---- 类型转换：严格分离 ---- */

static value_t *enum_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    /* 仅同 enum 实例：身份拷贝。类型校验先于 shadow 短路（sint 同款顺序）：
       shadow 值也须通过同 enum 检查——sema 编译期靠此拦截 enum→底层/异 enum
       的隐式转换。 */
    if (!target || target != value_type(v))
        return value_make_error(vm, "enum: implicit cast only within same enum");
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
    return value_make(vm, target, data);
}

static value_t *enum_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    /* 仅 enum → 声明底层类型（截断拷贝）；同 enum → clone */
    if (value_is_shadow(v)) {
        if (target && (target == value_type(v) ||
                       target == enum_type_underlying(value_type(v))))
            return value_make_shadow(vm, target);
        return value_make_error(vm,
            "enum: explicit cast only to declared underlying type");
    }
    if (target && target == value_type(v)) {
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }
    if (target && target == enum_type_underlying(value_type(v))) {
        void *data = value_alloc_data(vm->alloc, target);
        enum_store_value(vm, data, target, enum_read_value(v));
        return value_make(vm, target, data);
    }
    return value_make_error(vm, "enum: explicit cast only to declared underlying type");
}

/* ---- 鸭子类型判断：enum 是独立具名类型 ---- */

static bool enum_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    (void)vm;
    return a == b;
}

static bool enum_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    (void)vm;
    /* 严格分离：enum 仅等于自身；enum → 底层 false */
    return sub == sup;
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_ENUM = {
    .eq = enum_eq, .ne = enum_ne,
    .dispose = enum_dispose,
    .clone = enum_clone,
    .assign = enum_assign,
    .implicit_cast = enum_implicit_cast,
    .explicit_cast = enum_explicit_cast,
    .type_equal = enum_type_equal,
    .type_extends = enum_type_extends,
    .type_seal = type_enum_seal, /* 开放构造路径（PUSH_ENUM → SEAL）密封入口 */
};

/* ===========================================================================
 * 开放构造（PUSH_ENUM / SET_TYPE / ENUM_VARIANT / SEAL）
 * =========================================================================== */

/* 构造名 "<enum 名>"：开放阶段用占位名（SET_TYPE_NAME 由编译器在 hoist
 * 定义段发，覆盖为真实 enum 名；sema intern 快捷直接设真实名）。 */
static char *enum_name_copy(allocator_t *alloc, strslice_t name) {
    char *buf = (char *)allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL,
                                         NULL, name.len + 1);
    if (!buf) return NULL;
    memcpy(buf, name.ptr, name.len);
    buf[name.len] = '\0';
    return buf;
}

static enum_type_t *enum_type_create_open(vm_t *vm) {
    enum_type_t *et = (enum_type_t *)allocator_new_ex(
        vm->alloc, "enum_type_t", sizeof(enum_type_t), NULL, NULL, NULL, 1);
    if (!et) panic("vm: out of memory allocating enum type");
    memset(et, 0, sizeof(enum_type_t));
    et->base.vtable = &VTABLE_ENUM;
    et->base.kind   = TYPE_KIND_ENUM;
    return et;
}

/* PUSH_ENUM：分配开放 enum_type（underlying=NULL，不入池）+ 压其 type value */
const type_t *type_enum_push(vm_t *vm) {
    if (!vm) return NULL;
    enum_type_t *et = enum_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &et->base));
    return &et->base;
}

/* SET_TYPE 运行期用：设底层整型（密封后静默忽略） */
void type_enum_set_underlying(vm_t *vm, const type_t *t, const type_t *underlying) {
    (void)vm;
    if (!t || t->kind != TYPE_KIND_ENUM || type_is_sealed(t)) return;
    if (underlying && underlying->kind != TYPE_KIND_INT) return; /* 防御 */
    ((enum_type_t *)t)->underlying = underlying;
}

/* ENUM_VARIANT 运行期用：追加 variant（名拷贝到 vm 堆，值按底层宽度截断）。
 * 开放阶段动态扩容（variant_count 递增）；密封后静默忽略。 */
void type_enum_add_variant(vm_t *vm, const type_t *t, strslice_t name, int64_t value) {
    if (!vm || !t || t->kind != TYPE_KIND_ENUM || type_is_sealed(t)) return;
    enum_type_t *et = (enum_type_t *)t;

    /* 名拷贝（vm 拥有；seal 时整表再拷贝一次，开放期追加名在 seal 后释放） */
    char *nbuf = enum_name_copy(vm->alloc, name);
    if (!nbuf) panic("vm: out of memory adding enum variant");

    size_t n = et->variant_count;
    enum_variant_t *nv = (enum_variant_t *)allocator_new_ex(
        vm->alloc, "enum_variant_t", sizeof(enum_variant_t), NULL, NULL, NULL,
        n + 1);
    if (!nv) panic("vm: out of memory adding enum variant");
    if (n > 0) memcpy(nv, et->variants, n * sizeof(enum_variant_t));
    if (et->variants) allocator_free(vm->alloc, (void **)&et->variants);

    /* 底层未设（SET_TYPE 前）时按 i64 存；密封时按 underlying->size 截断 */
    nv[n].name  = (strslice_t){ nbuf, name.len };
    nv[n].value = value;
    et->variants = nv;
    et->variant_count = n + 1;
}

/* 按 (underlying, variant 表内容) 判断两枚举是否等价（去重 intern 用） */
static bool enum_same(const enum_type_t *a, const enum_type_t *b) {
    if (a->underlying != b->underlying) return false;
    if (a->variant_count != b->variant_count) return false;
    for (size_t i = 0; i < a->variant_count; i++) {
        const enum_variant_t *va = &a->variants[i];
        const enum_variant_t *vb = &b->variants[i];
        if (va->value != vb->value) return false;
        if (va->name.len != vb->name.len) return false;
        if (va->name.ptr && vb->name.ptr &&
            memcmp(va->name.ptr, vb->name.ptr, va->name.len) != 0) return false;
        if ((va->name.ptr == NULL) != (vb->name.ptr == NULL)) return false;
    }
    return true;
}

/* 释放 enum_type_t 持有的资源（variant 表 + 名），不释放结构体本身 */
static void enum_type_free_resources(vm_t *vm, enum_type_t *et) {
    if (et->variants) {
        for (size_t i = 0; i < et->variant_count; i++) {
            if (et->variants[i].name.ptr) {
                char *np = (char *)et->variants[i].name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
        }
        allocator_free(vm->alloc, (void **)&et->variants);
    }
    if (et->base.name.ptr) {
        char *np = (char *)et->base.name.ptr;
        allocator_free(vm->alloc, (void **)&np);
    }
}

/* SEAL：拷贝 variant 表 + 布局（size/align = underlying）+ 按 (underlying,
 * variants) 去重 intern + 置 sealed。返回密封后的 const type（可能 != self）。 */
const type_t *type_enum_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_ENUM) return NULL;
    if (type_is_sealed(t)) return t; /* 幂等 */

    enum_type_t *et = (enum_type_t *)t;

    /* 必须已设 underlying（SET_TYPE） */
    if (!et->underlying) return NULL;

    if (!vm->enum_types) vm->enum_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern：命中已有 sealed 类型则复用并手工回收本开放类型 */
    size_t n = vec_len(vm->enum_types);
    for (size_t i = 0; i < n; i++) {
        const enum_type_t *other = (const enum_type_t *)vec_get(vm->enum_types, i);
        if (other && other != et && type_is_sealed(&other->base) &&
            enum_same(other, et)) {
            enum_type_free_resources(vm, et);
            allocator_free(vm->alloc, (void **)&et);
            return &other->base;
        }
    }

    /* 布局 = 底层布局 */
    et->base.size  = et->underlying->size;
    et->base.align = et->underlying->align;
    et->base.sealed = true;

    vec_push(vm->enum_types, vm->alloc, et); /* 密封后入池（去重 intern） */
    return &et->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_enum_intern(vm_t *vm, const type_t *underlying,
                               const enum_variant_t *variants, size_t count) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。不向操作数栈
     * 压入 type value。去重 intern 由 type_enum_seal 完成。 */
    if (!vm || !underlying || underlying->kind != TYPE_KIND_INT) return NULL;
    enum_type_t *et = enum_type_create_open(vm);
    et->underlying = underlying;

    /* 拷贝 variant 表（vm 拥有；名深拷贝） */
    if (count > 0) {
        enum_variant_t *copy = (enum_variant_t *)allocator_new_ex(
            vm->alloc, "enum_variant_t", sizeof(enum_variant_t), NULL, NULL,
            NULL, count);
        if (!copy) panic("vm: out of memory interning enum type");
        for (size_t i = 0; i < count; i++) {
            char *nbuf = enum_name_copy(vm->alloc, variants[i].name);
            if (!nbuf) panic("vm: out of memory interning enum variant name");
            copy[i].name  = (strslice_t){ nbuf, variants[i].name.len };
            copy[i].value = variants[i].value;
        }
        et->variants = copy;
        et->variant_count = count;
    }
    return type_enum_seal(vm, &et->base);
}

/* ===========================================================================
 * variant 查找
 * =========================================================================== */

int enum_type_find_variant(const type_t *t, strslice_t name) {
    if (!t || t->kind != TYPE_KIND_ENUM) return -1;
    const enum_type_t *et = (const enum_type_t *)t;
    for (size_t i = 0; i < et->variant_count; i++) {
        const enum_variant_t *v = &et->variants[i];
        if (v->name.len == name.len && v->name.ptr &&
            memcmp(v->name.ptr, name.ptr, name.len) == 0)
            return (int)i;
    }
    return -1;
}
