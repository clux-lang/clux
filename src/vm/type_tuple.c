#include "vm/type_tuple.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "vm/type_option.h" /* value_blit_raw / value_dispose_raw */
#include "core/panic.h"
#include "core/string.h"
#include "core/strslice.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- 内部：读取 index value 的整数值（运行时 value，非字面整数；与
   array_read_index 同款——index 是整型 value，按宽度读立即数） ---- */

static bool tuple_read_index(vm_t *vm, value_t *index, size_t *out) {
    if (value_is_error(vm, index)) return false;
    const type_t *it = value_type(index);
    if (!it || it->kind != TYPE_KIND_INT) return false;
    uint64_t v;
    switch (it->size) {
        case 1: v = *(const uint8_t  *)value_data(index); break;
        case 2: v = *(const uint16_t *)value_data(index); break;
        case 4: v = *(const uint32_t *)value_data(index); break;
        default: v = *(const uint64_t *)value_data(index); break;
    }
    if (v > SIZE_MAX) return false;  /* 超出地址空间，视为越界 */
    *out = (size_t)v;
    return true;
}

/* ===========================================================================
 * 元组类型（tuple）
 *
 * tuple_type_t 继承 type_t 持元素表 tuple_elem_t { offset, type }（成员匿名，
 * 按位置访问）。tuple value 的 data 是连续内存块（size = type->size），元素
 * 按偏移 O(1) 读写。布局在密封时按 C 对齐规则一次性计算（与 struct 同款）：
 *   offset_0 = 0；offset_i = align_up(prev_end, elem_i.align)
 *   size = align_up(last_end, max_align)；align = max(元素 align)
 *
 * 生命周期（clone/assign/dispose）按元素递归（元素可能含资源：str/数组/
 * 嵌套 tuple/struct/option）——复用 value_blit_raw / value_dispose_raw（含
 * TYPE_KIND_TUPLE 分支，按元素偏移递归）。
 *
 * 严格类型：implicit_cast / explicit_cast 支持 Tuple↔Array 布局兼容互转
 * （m2-design §3：元组 ↔ 数组匿名互转，布局兼容时）与同 tuple 实例（身份
 * 拷贝）；eq/ne 同实例按元素递归比较；type_equal / type_extends 按元素表
 * 逐位比较（tuple_type_compatible）。
 * =========================================================================== */

/* ---- 生命周期：clone / assign / dispose（元素递归） ---- */

static value_t *tuple_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    void *data = value_alloc_data(vm->alloc, t);
    value_blit_raw(vm, data, value_data(v), t);
    return value_make(vm, t, data);
}

static value_t *tuple_assign(vm_t *vm, value_t *dst, value_t *src) {
    /* safe_cast 语义（与 struct_assign 同款）：类型不同先向左值类型
       implicit_cast——布局兼容的 tuple→tuple / tuple↔array 跨类型赋值经
       tuple_implicit_cast 转换通过；不兼容 → cast 返回 error，直接传播。 */
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

static void tuple_dispose(vm_t *vm, value_t *v) {
    /* 借用引用不拥有 data（指向父值内部），跳过；由 value_dispose 统一拦截 */
    if (value_is_borrowed(v)) return;
    value_dispose_raw(vm, value_data(v), value_type(v));
}

/* ---- 判等：同实例按元素递归比较（值相等，非指针） ---- */

/* 按类型读标量值并比较（int/float/bool/str 指针等标量元素；与 struct
   struct_field_equal 同款，仅遍历路径按元素表偏移） */
static bool tuple_elem_equal(vm_t *vm, const void *a, const void *b,
                             const type_t *et) {
    if (!et || et->kind == TYPE_KIND_STR) {
        /* str：指针相等（同一 string_t 实例）或内容相等 */
        const string_t *sa = *(const string_t *const *)a;
        const string_t *sb = *(const string_t *const *)b;
        if (sa == sb) return true;
        if (!sa || !sb) return false;
        return string_equals(sa, sb);
    }
    switch (et->kind) {
        case TYPE_KIND_INT:
        case TYPE_KIND_FLOAT:
        case TYPE_KIND_BOOL:
            /* 标量按字节比较（同类型 size 一致，无 padding） */
            return memcmp(a, b, et->size) == 0;
        case TYPE_KIND_ARRAY:
        case TYPE_KIND_OPTION:
        case TYPE_KIND_STRUCT:
        case TYPE_KIND_TUPLE: {
            /* 复合：递归比较。构造临时借用 value 分派 value_eq。 */
            value_t *va = value_make_borrowed(vm, et, (void *)a);
            value_t *vb = value_make_borrowed(vm, et, (void *)b);
            value_t *r = value_eq(vm, va, vb);
            if (value_is_error(vm, r)) return false;
            return value_as(r, bool);
        }
        default:
            return memcmp(a, b, et->size) == 0;
    }
}

static value_t *tuple_eq(vm_t *vm, value_t *a, value_t *b) {
    /* safe_cast 语义（与 struct_eq 同款）：类型不同时尝试右值 implicit_cast
       到左值类型——布局兼容的 tuple→tuple / tuple↔array 比较经
       tuple_implicit_cast 转换通过；不兼容 → cast error 传播。 */
    VTABLE_BINARY(vm, a, b, eq, "==");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);

    const type_t *t = value_type(a);
    size_t n = tuple_type_elem_count(t);
    bool r = true;
    for (size_t i = 0; i < n && r; i++) {
        const tuple_elem_t *e = tuple_type_elem(t, i);
        r = tuple_elem_equal(vm, (const uint8_t *)value_data(a) + e->offset,
                             (const uint8_t *)value_data(b) + e->offset,
                             e->type);
    }
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &r);
    return value_make(vm, vm->type_bool, data);
}

static value_t *tuple_ne(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, ne, "!=");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    value_t *eq = tuple_eq(vm, a, b);
    if (value_is_error(vm, eq)) return eq;
    bool r = !value_as(eq, bool);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &r);
    return value_make(vm, vm->type_bool, data);
}

/* ---- 类型转换：Tuple↔Array 布局兼容互转（m2-design §3） + 同 tuple 身份 ---- */

/* 布局兼容判断：Tuple ↔ Array（元素逐个 type_equal + 数量一致 ⟹ size/align
   由元素表唯一决定相同）。非 tuple/array 组合返回 false。 */
bool tuple_array_layout_compatible(vm_t *vm, const type_t *t,
                                   const type_t *other) {
    if (!t || !other) return false;
    if (t->kind == TYPE_KIND_TUPLE && other->kind == TYPE_KIND_ARRAY) {
        size_t n = tuple_type_elem_count(t);
        if (n != array_type_len(other)) return false;
        for (size_t i = 0; i < n; i++) {
            const tuple_elem_t *e = tuple_type_elem(t, i);
            if (!type_equal(vm, e->type, array_type_elem(other))) return false;
        }
        return true;
    }
    if (t->kind == TYPE_KIND_ARRAY && other->kind == TYPE_KIND_TUPLE) {
        size_t n = tuple_type_elem_count(other);
        if (n != array_type_len(t)) return false;
        for (size_t i = 0; i < n; i++) {
            const tuple_elem_t *e = tuple_type_elem(other, i);
            if (!type_equal(vm, array_type_elem(t), e->type)) return false;
        }
        return true;
    }
    return false;
}

static value_t *tuple_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (!target) return value_make_error(vm, "tuple: missing cast target");
    const type_t *src = value_type(v);
    /* 同 tuple 实例（身份拷贝） */
    if (target->kind == TYPE_KIND_TUPLE && target == src) {
        if (value_is_shadow(v)) return value_make_shadow(vm, target);
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }
    /* 布局兼容的 tuple→tuple（不同 intern 实例，元素一致） */
    if (target->kind == TYPE_KIND_TUPLE && src->kind == TYPE_KIND_TUPLE &&
        tuple_type_compatible(vm, target, src)) {
        if (value_is_shadow(v)) return value_make_shadow(vm, target);
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }
    /* 布局兼容的 tuple↔array 互转 */
    if (tuple_array_layout_compatible(vm, target, src) ||
        tuple_array_layout_compatible(vm, src, target)) {
        if (value_is_shadow(v)) return value_make_shadow(vm, target);
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }
    return value_make_error(vm, "tuple: implicit cast only within layout-compatible "
                               "tuple/array");
}

static value_t *tuple_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    return tuple_implicit_cast(vm, v, target);
}

/* ---- 下标访问：t[i] 借用引用 / t[i] = v（运行期越界检查，同数组语义） ---- */

static value_t *tuple_get_index(vm_t *vm, value_t *self, value_t *index) {
    if (value_is_error(vm, self)) return self;
    if (value_is_error(vm, index)) return index;

    /* shadow：只返回元素类型 shadow（不操作实际数据） */
    if (value_is_shadow(self) || value_is_shadow(index)) {
        const type_t *t = value_type(self);
        size_t n = tuple_type_elem_count(t);
        /* shadow index 无值可取——取首个元素类型（sema 编译期只做类型协商，
           运行时索引值不可知；首个元素类型与整体类型形状一致） */
        const tuple_elem_t *e = tuple_type_elem(t, 0);
        return value_make_shadow(vm, e ? e->type : vm->type_void);
    }

    size_t i;
    if (!tuple_read_index(vm, index, &i))
        return value_make_error(vm, "tuple index must be an integer");

    const type_t *t = value_type(self);
    size_t n = tuple_type_elem_count(t);
    /* 越界检查：返回硬错误（exec_drive 据此停机），并给出索引与长度便于定位 */
    if (i >= n) {
        char buf[96];
        snprintf(buf, sizeof buf,
                 "tuple index %zu out of bounds (len=%zu)", i, n);
        return value_make_error(vm, buf);
    }

    /* 统一返回借用引用（标量与复合元素一致，C 左值语义）：
       data 直接指向块内业务内存偏移 data + elem.offset，
       读即直接解引用该内存，写经 INDEX_SET 直达原 tuple。 */
    const tuple_elem_t *e = tuple_type_elem(t, i);
    return value_make_borrowed(vm, e->type,
                               (uint8_t *)value_data(self) + e->offset);
}

static value_t *tuple_set_index(vm_t *vm, value_t *self, value_t *index,
                                 value_t *val) {
    if (value_is_error(vm, self)) return self;
    if (value_is_error(vm, index)) return index;
    if (value_is_error(vm, val)) return val;

    /* shadow：仅类型检查，不操作实际数据 */
    if (value_is_shadow(self) || value_is_shadow(index) || value_is_shadow(val))
        return self;

    size_t i;
    if (!tuple_read_index(vm, index, &i))
        return value_make_error(vm, "tuple index must be an integer");

    const type_t *t = value_type(self);
    size_t n = tuple_type_elem_count(t);
    if (i >= n) {
        char buf[96];
        snprintf(buf, sizeof buf,
                 "tuple index %zu out of bounds (len=%zu)", i, n);
        return value_make_error(vm, buf);
    }

    const tuple_elem_t *e = tuple_type_elem(t, i);

    /* 类型检查：非元素类型尝试隐式转换 */
    value_t *v = val;
    if (value_type(val) != e->type) {
        v = value_implicit_cast(vm, val, e->type);
        if (value_is_error(vm, v)) return v;
    }

    /* 写块内偏移：先释放旧元素资源，再深拷贝新值到业务内存。
       self 是借用（多维链）时 data 已指向块内偏移，写直达原 tuple。 */
    uint8_t *slot = (uint8_t *)value_data(self) + e->offset;
    value_dispose_raw(vm, slot, e->type);
    value_blit_raw(vm, slot, value_data(v), e->type);
    return self;
}

/* ---- 长度查询：返回 u64 元素个数 ---- */

static value_t *tuple_length(vm_t *vm, value_t *self) {
    if (value_is_shadow(self))
        return value_make_shadow(vm, vm->type_u64);
    size_t n = tuple_type_elem_count(value_type(self));
    void *data = value_alloc_data(vm->alloc, vm->type_u64);
    *(uint64_t *)data = (uint64_t)n;
    return value_make(vm, vm->type_u64, data);
}

/* ---- 鸭子类型判断：tuple 按元素表逐位（类型+顺序） ---- */

static bool tuple_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    return tuple_type_compatible(vm, a, b);
}

static bool tuple_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    return tuple_type_compatible(vm, sub, sup);
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_TUPLE = {
    .eq = tuple_eq, .ne = tuple_ne,
    .dispose = tuple_dispose,
    .clone = tuple_clone,
    .assign = tuple_assign,
    .implicit_cast = tuple_implicit_cast,
    .explicit_cast = tuple_explicit_cast,
    .get_index = tuple_get_index,
    .set_index = tuple_set_index,
    .length = tuple_length,
    .type_equal = tuple_type_equal,
    .type_extends = tuple_type_extends,
    .type_seal = type_tuple_seal, /* 开放构造路径（PUSH_TUPLE → SEAL）密封入口 */
};

/* ===========================================================================
 * 开放构造（PUSH_TUPLE / APPEND_ELEM / SEAL）
 * =========================================================================== */

/* dispose_fn：allocator_free 时自动释放元素表 + 显示名
   （open 对象分配时注册，seal 去重回收与 vm_destroy 都只需裸 allocator_free） */
static void tuple_type_dispose(void *self, allocator_t *allocator) {
    tuple_type_t *tt = (tuple_type_t *)self;
    if (tt->elems) {
        allocator_free(allocator, (void **)&tt->elems);
    }
    if (tt->base.name.ptr) {
        char *np = (char *)tt->base.name.ptr;
        allocator_free(allocator, (void **)&np);
    }
}

static tuple_type_t *tuple_type_create_open(vm_t *vm) {
    tuple_type_t *tt = (tuple_type_t *)allocator_new_ex(
        vm->alloc, "tuple_type_t", sizeof(tuple_type_t), NULL, NULL,
        tuple_type_dispose, 1);
    if (!tt) panic("vm: out of memory allocating tuple type");
    memset(tt, 0, sizeof(tuple_type_t));
    tt->base.vtable = &VTABLE_TUPLE;
    tt->base.kind   = TYPE_KIND_TUPLE;
    return tt;
}

/* PUSH_TUPLE：分配空 tuple_type（elems=NULL，不入池）+ 压其 type value */
const type_t *type_tuple_push(vm_t *vm) {
    if (!vm) return NULL;
    tuple_type_t *tt = tuple_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &tt->base));
    return &tt->base;
}

/* APPEND_ELEM 运行期用：追加元素（类型引用记录，元素匿名；密封后静默忽略）。
 * 开放阶段动态扩容（elem_count 递增）。 */
void type_tuple_add_elem(vm_t *vm, const type_t *t, const type_t *etype) {
    if (!vm || !t || t->kind != TYPE_KIND_TUPLE || type_is_sealed(t)) return;
    tuple_type_t *tt = (tuple_type_t *)t;

    size_t n = tt->elem_count;
    tuple_elem_t *ne = (tuple_elem_t *)allocator_new_ex(
        vm->alloc, "tuple_elem_t", sizeof(tuple_elem_t), NULL, NULL, NULL,
        n + 1);
    if (!ne) panic("vm: out of memory adding tuple element");
    if (n > 0) memcpy(ne, tt->elems, n * sizeof(tuple_elem_t));
    if (tt->elems) allocator_free(vm->alloc, (void **)&tt->elems);

    ne[n].offset = 0;  /* 布局在 seal 时统一计算 */
    ne[n].type   = etype;
    tt->elems = ne;
    tt->elem_count = n + 1;
}

/* 按（元素类型 + 顺序）判断两元组是否等价（去重 intern 用） */
static bool tuple_same(const tuple_type_t *a, const tuple_type_t *b) {
    if (a->elem_count != b->elem_count) return false;
    for (size_t i = 0; i < a->elem_count; i++) {
        if (a->elems[i].type != b->elems[i].type) return false;
    }
    return true;
}

/* 元组兼容（鸭子类型，m2-design §3）：元素类型（type_equal，嵌套复合类型
 * 递归）+ 元素顺序完全一致即兼容。布局（offset/size/align）由元素表唯一
 * 决定（C 对齐规则），元素兼容 ⟹ size/align 相同——无需单独比较。 */
bool tuple_type_compatible(vm_t *vm, const type_t *a, const type_t *b) {
    if (!a || !b || a->kind != TYPE_KIND_TUPLE ||
        b->kind != TYPE_KIND_TUPLE)
        return false;
    if (a == b) return true;
    const tuple_type_t *ta = (const tuple_type_t *)a;
    const tuple_type_t *tb = (const tuple_type_t *)b;
    if (ta->elem_count != tb->elem_count) return false;
    for (size_t i = 0; i < ta->elem_count; i++) {
        if (!type_equal(vm, ta->elems[i].type, tb->elems[i].type)) return false;
    }
    return true;
}

/* 类型名 "<T1, T2>"（构造见文件尾） */
static char *tuple_type_name(allocator_t *alloc, const tuple_type_t *tt);

/* C 对齐规则向上取整 */
static size_t tuple_align_up(size_t v, size_t a) {
    if (a <= 1) return v;
    return (v + a - 1) / a * a;
}

/* SEAL：拷贝元素表 + C 对齐布局 + 按（元素类型+顺序）去重 intern + 置
 * sealed。返回密封后的 const type（可能 != self）。 */
const type_t *type_tuple_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_TUPLE) return NULL;
    if (type_is_sealed(t)) return t; /* 幂等 */

    tuple_type_t *tt = (tuple_type_t *)t;

    /* 防御：元素类型必须已解析（NULL 会产生 size=0 的 tuple，后续
       value_alloc_data 返回 NULL 崩溃）。非法输入拒绝密封。 */
    for (size_t i = 0; i < tt->elem_count; i++) {
        if (!tt->elems[i].type) return NULL;
    }

    if (!vm->tuple_types) vm->tuple_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern：命中已有 sealed 类型则复用并手工回收本开放类型
       （allocator_free 自动调 dispose_fn 释放元素表） */
    size_t n = vec_len(vm->tuple_types);
    for (size_t i = 0; i < n; i++) {
        const tuple_type_t *other = (const tuple_type_t *)vec_get(vm->tuple_types, i);
        if (other && other != tt && type_is_sealed(&other->base) &&
            tuple_same(other, tt)) {
            allocator_free(vm->alloc, (void **)&tt);
            return &other->base;
        }
    }

    /* 计算 C 对齐布局：offset_0 = 0；offset_i = align_up(prev_end, align_i)；
       size = align_up(last_end, max_align)；align = max(元素 align)。
       空 tuple：C 语义 size=1（保证 value_alloc_data 能分配 data 块）。 */
    size_t off = 0, max_align = 1;
    for (size_t i = 0; i < tt->elem_count; i++) {
        tuple_elem_t *e = &tt->elems[i];
        size_t ea = e->type && e->type->align > 0 ? e->type->align : 1;
        off = tuple_align_up(off, ea);
        e->offset = off;
        off += e->type ? e->type->size : 0;
        if (ea > max_align) max_align = ea;
    }
    tt->base.size  = tuple_align_up(off, max_align);
    if (tt->elem_count == 0) tt->base.size = 1; /* 空 tuple：C 语义 1 字节 */
    tt->base.align = max_align;
    tt->base.sealed = true;

    /* 重建类型名 "<T1, T2>" */
    char *name = tuple_type_name(vm->alloc, tt);
    if (name) tt->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->tuple_types, vm->alloc, tt); /* 密封后入池（去重 intern） */
    return &tt->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_tuple_intern(vm_t *vm, const tuple_elem_t *elems,
                                size_t count) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。不向操作数栈
     * 压入 type value。去重 intern 由 type_tuple_seal 完成。 */
    if (!vm) return NULL;
    tuple_type_t *tt = tuple_type_create_open(vm);

    /* 拷贝元素表（vm 拥有） */
    if (count > 0) {
        tuple_elem_t *copy = (tuple_elem_t *)allocator_new_ex(
            vm->alloc, "tuple_elem_t", sizeof(tuple_elem_t), NULL, NULL,
            NULL, count);
        if (!copy) panic("vm: out of memory interning tuple type");
        for (size_t i = 0; i < count; i++) {
            copy[i].offset = 0; /* seal 统一计算 */
            copy[i].type   = elems[i].type;
        }
        tt->elems = copy;
        tt->elem_count = count;
    }
    const type_t *t = type_tuple_seal(vm, &tt->base);
    if (!t) allocator_free(vm->alloc, (void **)&tt); /* 密封失败（如元素类型
                                                       未解析）：回收开放对象 */
    return t;
}

/* ===========================================================================
 * 类型名 "<T1, T2>"
 * =========================================================================== */

static char *tuple_type_name(allocator_t *alloc, const tuple_type_t *tt) {
    string_t *s = string_new(alloc);
    if (!s) return NULL;
    string_append_char(s, '<');
    for (size_t i = 0; i < tt->elem_count; i++) {
        if (i) string_append_cstr(s, ", ");
        const type_t *et = tt->elems[i].type;
        if (et && et->name.ptr)
            string_append_bytes(s, et->name.ptr, et->name.len);
        else
            string_append_cstr(s, "?");
    }
    string_append_char(s, '>');
    char *out = (char *)allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL,
                                          NULL, string_len(s) + 1);
    if (out) {
        memcpy(out, string_data(s), string_len(s));
        out[string_len(s)] = '\0';
    }
    string_free(&s);
    return out;
}

/* ===========================================================================
 * 运行期 tuple value 只读访问（供调试/格式化遍历，如 printf %v）
 * =========================================================================== */

size_t value_tuple_count(const value_t *v) {
    if (!v || value_kind(v) != TYPE_KIND_TUPLE) return 0;
    /* 借用 v 的 type 也是 tuple 类型，count 从类型取 */
    return tuple_type_elem_count(value_type(v));
}

value_t *value_tuple_at(vm_t *vm, const value_t *v, size_t i) {
    if (!v || value_kind(v) != TYPE_KIND_TUPLE) return NULL;
    const type_t *t = value_type(v);
    size_t n = tuple_type_elem_count(t);
    if (i >= n) return NULL;
    const tuple_elem_t *e = tuple_type_elem(t, i);
    /* 返回借用引用（data 指向块内偏移，业务内存），供递归遍历 */
    return value_make_borrowed(vm, e->type,
                               (uint8_t *)value_data(v) + e->offset);
}
