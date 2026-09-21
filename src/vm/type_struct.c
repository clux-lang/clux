#include "vm/type_struct.h"
#include "vm/type.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "vm/type_option.h" /* value_blit_raw / value_dispose_raw */
#include "core/panic.h"
#include "core/string.h"
#include "core/strslice.h"

#include <string.h>

/* ===========================================================================
 * 结构体类型（struct）
 *
 * struct_type_t 继承 type_t 持字段表 field_t { name, offset, type }。struct
 * value 的 data 是连续内存块（size = type->size），字段按偏移 O(1) 读写。
 * 布局在密封时按 C 对齐规则一次性计算：
 *   offset_0 = 0；offset_i = align_up(prev_end, field_i.align)
 *   size = align_up(last_end, max_align)；align = max(字段 align)
 *
 * 生命周期（clone/assign/dispose）按字段递归（字段可能含资源：str/数组/
 * 嵌套 struct/option）——复用 value_blit_raw / value_dispose_raw（含
 * TYPE_KIND_STRUCT 分支，按字段偏移递归）。
 *
 * 严格类型：implicit_cast / explicit_cast 仅同 struct 实例（身份拷贝）；
 * eq/ne 同实例按字段递归比较；type_equal / type_extends 按指针（具名类型，
 * M2 鸭子类型检查留待后续 Phase）。
 * =========================================================================== */

/* ---- 生命周期：clone / assign / dispose（字段递归） ---- */

static value_t *struct_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    void *data = value_alloc_data(vm->alloc, t);
    value_blit_raw(vm, data, value_data(v), t);
    return value_make(vm, t, data);
}

static value_t *struct_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_type(src) != value_type(dst)) {
        return value_make_error(vm,
            "assign: struct type mismatch on assignment");
    }
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    const type_t *t = value_type(dst);
    value_dispose_raw(vm, value_data(dst), t);
    value_blit_raw(vm, value_data(dst), value_data(src), t);
    return dst;
}

static void struct_dispose(vm_t *vm, value_t *v) {
    /* 借用引用不拥有 data（指向父值内部），跳过；由 value_dispose 统一拦截 */
    if (value_is_borrowed(v)) return;
    value_dispose_raw(vm, value_data(v), value_type(v));
}

/* ---- 判等：同实例按字段递归比较（值相等，非指针） ---- */

/* 按类型读标量值并比较（int/float/bool/str 指针等标量字段） */
static bool struct_field_equal(vm_t *vm, const void *a, const void *b,
                               const type_t *ft) {
    if (!ft || ft->kind == TYPE_KIND_STR) {
        /* str：指针相等（同一 string_t 实例）或内容相等 */
        const string_t *sa = *(const string_t *const *)a;
        const string_t *sb = *(const string_t *const *)b;
        if (sa == sb) return true;
        if (!sa || !sb) return false;
        return string_equals(sa, sb);
    }
    switch (ft->kind) {
        case TYPE_KIND_INT:
        case TYPE_KIND_FLOAT:
        case TYPE_KIND_BOOL:
            /* 标量按字节比较（同类型 size 一致，无 padding） */
            return memcmp(a, b, ft->size) == 0;
        case TYPE_KIND_ARRAY:
        case TYPE_KIND_OPTION:
        case TYPE_KIND_STRUCT: {
            /* 复合：递归比较。构造临时借用 value 分派 value_eq。 */
            value_t *va = value_make_borrowed(vm, ft, (void *)a);
            value_t *vb = value_make_borrowed(vm, ft, (void *)b);
            value_t *r = value_eq(vm, va, vb);
            if (value_is_error(vm, r)) return false;
            return value_as(r, bool);
        }
        default:
            return memcmp(a, b, ft->size) == 0;
    }
}

static value_t *struct_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_type(a) != value_type(b)) {
        return value_make_error(vm,
            "struct: compare only within same struct");
    }
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);

    const type_t *t = value_type(a);
    size_t n = struct_type_field_count(t);
    bool r = true;
    for (size_t i = 0; i < n && r; i++) {
        const struct_field_t *f = struct_type_field(t, i);
        r = struct_field_equal(vm, (const uint8_t *)value_data(a) + f->offset,
                               (const uint8_t *)value_data(b) + f->offset,
                               f->type);
    }
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &r);
    return value_make(vm, vm->type_bool, data);
}

static value_t *struct_ne(vm_t *vm, value_t *a, value_t *b) {
    if (value_type(a) != value_type(b)) {
        return value_make_error(vm,
            "struct: compare only within same struct");
    }
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    value_t *eq = struct_eq(vm, a, b);
    if (value_is_error(vm, eq)) return eq;
    bool r = !value_as(eq, bool);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &r);
    return value_make(vm, vm->type_bool, data);
}

/* ---- 类型转换：仅同 struct 实例（严格，鸭子留待后续） ---- */

static value_t *struct_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (!target || target != value_type(v))
        return value_make_error(vm, "struct: implicit cast only within same struct");
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
    return value_make(vm, target, data);
}

static value_t *struct_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (!target || target != value_type(v))
        return value_make_error(vm, "struct: explicit cast only within same struct");
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
    return value_make(vm, target, data);
}

/* ---- 鸭子类型判断：struct 是独立具名类型（按指针） ---- */

static bool struct_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    (void)vm;
    return a == b;
}

static bool struct_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    (void)vm;
    return sub == sup;
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_STRUCT = {
    .eq = struct_eq, .ne = struct_ne,
    .dispose = struct_dispose,
    .clone = struct_clone,
    .assign = struct_assign,
    .implicit_cast = struct_implicit_cast,
    .explicit_cast = struct_explicit_cast,
    .type_equal = struct_type_equal,
    .type_extends = struct_type_extends,
    .type_seal = type_struct_seal, /* 开放构造路径（PUSH_STRUCT → SEAL）密封入口 */
};

/* ===========================================================================
 * 开放构造（PUSH_STRUCT / DEFINE_FIELD / SEAL）
 * =========================================================================== */

/* dispose_fn：allocator_free 时自动释放字段名 + 字段表 + 显示名
   （open 对象分配时注册，seal 去重回收与 vm_destroy 都只需裸 allocator_free） */
static void struct_type_dispose(void *self, allocator_t *allocator) {
    struct_type_t *st = (struct_type_t *)self;
    if (st->fields) {
        for (size_t i = 0; i < st->field_count; i++) {
            if (st->fields[i].name.ptr) {
                char *np = (char *)st->fields[i].name.ptr;
                allocator_free(allocator, (void **)&np);
            }
        }
        allocator_free(allocator, (void **)&st->fields);
    }
    if (st->base.name.ptr) {
        char *np = (char *)st->base.name.ptr;
        allocator_free(allocator, (void **)&np);
    }
}

static struct_type_t *struct_type_create_open(vm_t *vm) {
    struct_type_t *st = (struct_type_t *)allocator_new_ex(
        vm->alloc, "struct_type_t", sizeof(struct_type_t), NULL, NULL,
        struct_type_dispose, 1);
    if (!st) panic("vm: out of memory allocating struct type");
    memset(st, 0, sizeof(struct_type_t));
    st->base.vtable = &VTABLE_STRUCT;
    st->base.kind   = TYPE_KIND_STRUCT;
    return st;
}

/* PUSH_STRUCT：分配空 struct_type（fields=NULL，不入池）+ 压其 type value */
const type_t *type_struct_push(vm_t *vm) {
    if (!vm) return NULL;
    struct_type_t *st = struct_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &st->base));
    return &st->base;
}

/* DEFINE_FIELD 运行期用：追加字段（名拷贝到 vm 堆；密封后静默忽略）。
 * 开放阶段动态扩容（field_count 递增）。 */
void type_struct_add_field(vm_t *vm, const type_t *t, strslice_t name,
                           const type_t *ftype) {
    if (!vm || !t || t->kind != TYPE_KIND_STRUCT || type_is_sealed(t)) return;
    struct_type_t *st = (struct_type_t *)t;

    /* 名拷贝（vm 拥有；seal 时整表再拷贝一次，开放期追加名在 seal 后释放） */
    char *nbuf = (char *)allocator_new_ex(vm->alloc, "char", sizeof(char),
                                          NULL, NULL, NULL, name.len + 1);
    if (!nbuf) panic("vm: out of memory adding struct field");
    memcpy(nbuf, name.ptr, name.len);
    nbuf[name.len] = '\0';

    size_t n = st->field_count;
    struct_field_t *nf = (struct_field_t *)allocator_new_ex(
        vm->alloc, "struct_field_t", sizeof(struct_field_t), NULL, NULL, NULL,
        n + 1);
    if (!nf) panic("vm: out of memory adding struct field");
    if (n > 0) memcpy(nf, st->fields, n * sizeof(struct_field_t));
    if (st->fields) allocator_free(vm->alloc, (void **)&st->fields);

    nf[n].name   = (strslice_t){ nbuf, name.len };
    nf[n].offset = 0;  /* 布局在 seal 时统一计算 */
    nf[n].type   = ftype;
    st->fields = nf;
    st->field_count = n + 1;
}

/* 按 (字段名 + 类型 + 顺序) 判断两结构体是否等价（去重 intern 用） */
static bool struct_same(const struct_type_t *a, const struct_type_t *b) {
    if (a->field_count != b->field_count) return false;
    for (size_t i = 0; i < a->field_count; i++) {
        const struct_field_t *fa = &a->fields[i];
        const struct_field_t *fb = &b->fields[i];
        if (fa->type != fb->type) return false;
        if (fa->name.len != fb->name.len) return false;
        if (fa->name.ptr && fb->name.ptr &&
            memcmp(fa->name.ptr, fb->name.ptr, fa->name.len) != 0) return false;
        if ((fa->name.ptr == NULL) != (fb->name.ptr == NULL)) return false;
    }
    return true;
}

/* C 对齐规则向上取整 */
static size_t align_up(size_t v, size_t a) {
    if (a <= 1) return v;
    return (v + a - 1) / a * a;
}

/* SEAL：拷贝字段表 + C 对齐布局 + 按 (字段名+类型+顺序) 去重 intern + 置
 * sealed。返回密封后的 const type（可能 != self）。 */
const type_t *type_struct_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_STRUCT) return NULL;
    if (type_is_sealed(t)) return t; /* 幂等 */

    struct_type_t *st = (struct_type_t *)t;

    if (!vm->struct_types) vm->struct_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern：命中已有 sealed 类型则复用并手工回收本开放类型
       （allocator_free 自动调 dispose_fn 释放字段表） */
    size_t n = vec_len(vm->struct_types);
    for (size_t i = 0; i < n; i++) {
        const struct_type_t *other = (const struct_type_t *)vec_get(vm->struct_types, i);
        if (other && other != st && type_is_sealed(&other->base) &&
            struct_same(other, st)) {
            allocator_free(vm->alloc, (void **)&st);
            return &other->base;
        }
    }

    /* 计算 C 对齐布局：offset_0 = 0；offset_i = align_up(prev_end, align_i)；
       size = align_up(last_end, max_align)；align = max(字段 align) */
    size_t off = 0, max_align = 1;
    for (size_t i = 0; i < st->field_count; i++) {
        struct_field_t *f = &st->fields[i];
        size_t fa = f->type && f->type->align > 0 ? f->type->align : 1;
        off = align_up(off, fa);
        f->offset = off;
        off += f->type ? f->type->size : 0;
        if (fa > max_align) max_align = fa;
    }
    st->base.size  = align_up(off, max_align);
    st->base.align = max_align;
    st->base.sealed = true;

    vec_push(vm->struct_types, vm->alloc, st); /* 密封后入池（去重 intern） */
    return &st->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_struct_intern(vm_t *vm, const struct_field_t *fields,
                                 size_t count) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。不向操作数栈
     * 压入 type value。去重 intern 由 type_struct_seal 完成。 */
    if (!vm) return NULL;
    struct_type_t *st = struct_type_create_open(vm);

    /* 拷贝字段表（vm 拥有；名深拷贝） */
    if (count > 0) {
        struct_field_t *copy = (struct_field_t *)allocator_new_ex(
            vm->alloc, "struct_field_t", sizeof(struct_field_t), NULL, NULL,
            NULL, count);
        if (!copy) panic("vm: out of memory interning struct type");
        for (size_t i = 0; i < count; i++) {
            char *nbuf = (char *)allocator_new_ex(vm->alloc, "char",
                                                  sizeof(char), NULL, NULL,
                                                  NULL, fields[i].name.len + 1);
            if (!nbuf) panic("vm: out of memory interning struct field name");
            memcpy(nbuf, fields[i].name.ptr, fields[i].name.len);
            nbuf[fields[i].name.len] = '\0';
            copy[i].name   = (strslice_t){ nbuf, fields[i].name.len };
            copy[i].offset = 0; /* seal 统一计算 */
            copy[i].type   = fields[i].type;
        }
        st->fields = copy;
        st->field_count = count;
    }
    return type_struct_seal(vm, &st->base);
}

/* ===========================================================================
 * 字段查找
 * =========================================================================== */

int struct_type_find_field(const type_t *t, strslice_t name) {
    if (!t || t->kind != TYPE_KIND_STRUCT) return -1;
    const struct_type_t *st = (const struct_type_t *)t;
    for (size_t i = 0; i < st->field_count; i++) {
        const struct_field_t *f = &st->fields[i];
        if (f->name.len == name.len && f->name.ptr &&
            memcmp(f->name.ptr, name.ptr, name.len) == 0)
            return (int)i;
    }
    return -1;
}
