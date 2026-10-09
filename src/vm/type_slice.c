#include "vm/type_slice.h"
#include "vm/type_ptr.h"
#include "vm/type_array.h"
#include "vm/vm.h"
#include "vm/value.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/strslice.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * 切片类型（m4-design §1/§10）
 *
 * 切片 = 胖指针 { void *ptr; uint64_t len; }，与指针对齐但多一个 len 字段。
 * 三种所有权修饰对应三个独立 type kind（own/ref/fatal），运行期零标志
 * （同 M3 §13.2）。不存在 share/weak 切片。
 *
 * own []T   → 总拥有堆块，dispose 释放（make 产物接管后）
 * ref []T   → 总借用，dispose 跳过
 * fatal []T → ptr≠NULL 时拥有（将亡值），转移后 ptr=NULL（接管方拥有）
 * =========================================================================== */

/* ---- 内部工具 ---- */

/* 切片元素类型是否需要递归扫描（内嵌 own/share/weak 字段） */
bool slice_type_needs_scan(const type_t *t);
void slice_free_elems(vm_t *vm, const type_t *elem, void *ptr, uint64_t len);
void slice_clear_elems(vm_t *vm, const type_t *elem, void *ptr, uint64_t len);

bool slice_type_needs_scan(const type_t *t) {
    /* 委托 ptr_type_needs_scan——同一套递归扫描判定 */
    return ptr_type_needs_scan(t);
}

/* 释放切片元素的内嵌 own 字段（不释放堆块本身——调用方负责） */
void slice_free_elems(vm_t *vm, const type_t *elem, void *ptr, uint64_t len) {
    if (!vm || !elem || !ptr || len == 0) return;
    if (!slice_type_needs_scan(elem)) return;
    uint8_t *p = (uint8_t *)ptr;
    for (uint64_t i = 0; i < len; i++, p += elem->size)
        ptr_free_owned_recursive(vm, elem, p);
}

/* 清空切片元素的内嵌 own 指针（不释放——接管辅助） */
void slice_clear_elems(vm_t *vm, const type_t *elem, void *ptr, uint64_t len) {
    if (!vm || !elem || !ptr || len == 0) return;
    if (!slice_type_needs_scan(elem)) return;
    uint8_t *p = (uint8_t *)ptr;
    for (uint64_t i = 0; i < len; i++, p += elem->size)
        ptr_clear_owned_recursive(vm, elem, p);
}

/* ---- 释放堆块（按 kind 分派） ---- */

static void slice_free_heap(vm_t *vm, const value_t *v) {
    if (!v || value_is_shadow(v)) return;
    const type_t *t = value_type(v);
    if (!t) return;
    slice_value_t *sv = (slice_value_t *)value_data(v);
    if (!sv || !sv->ptr) return;
    switch (t->kind) {
    case TYPE_KIND_SLICE_OWN:
    case TYPE_KIND_SLICE_FATAL: {
        /* 递归释放元素内嵌 own 字段，再释放堆块 */
        const type_t *et = slice_type_elem(t);
        if (et) slice_free_elems(vm, et, sv->ptr, sv->len);
        void *heap = sv->ptr;
        sv->ptr = NULL;
        sv->len = 0;
        allocator_free(vm->alloc, &heap);
        break;
    }
    default:
        break;  /* ref：无堆块 */
    }
}

/* ---- 索引读取辅助 ---- */

static bool slice_read_index(vm_t *vm, value_t *index, size_t *out) {
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
    if (v > SIZE_MAX) return false;
    *out = (size_t)v;
    return true;
}

/* ---- 生命周期：clone / assign / dispose ---- */

static value_t *slice_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    const slice_value_t *sv = (const slice_value_t *)value_data(v);

    switch (t->kind) {
    case TYPE_KIND_SLICE_OWN: {
        /* 深拷贝：分配新堆块 + 逐元素克隆 */
        const type_t *et = slice_type_elem(t);
        if (!et) return value_make_error(vm, "slice: missing elem type");
        if (!sv->ptr) return slice_make_value(vm, t, NULL, 0);
        /* 分配 len * et->size 的堆块 */
        size_t total = (size_t)sv->len * et->size;
        void *new_heap = allocator_new_ex(vm->alloc, "slice_heap", total,
                                          NULL, NULL, NULL, 1);
        if (!new_heap) panic("vm: out of memory cloning slice");
        /* 逐元素克隆 */
        const uint8_t *sp = (const uint8_t *)sv->ptr;
        uint8_t *dp = (uint8_t *)new_heap;
        for (uint64_t i = 0; i < sv->len; i++, sp += et->size, dp += et->size) {
            void *se = ptr_clone_block(vm, et, sp);
            memcpy(dp, se, et->size);
            allocator_free(vm->alloc, (void **)&se);
        }
        return slice_make_value(vm, t, new_heap, sv->len);
    }
    case TYPE_KIND_SLICE_FATAL: {
        /* 转移语义：新值接管 (ptr, len)，源置空 */
        value_t *nv = slice_make_value(vm, t, sv->ptr, sv->len);
        ((slice_value_t *)value_data(v))->ptr = NULL;
        ((slice_value_t *)value_data(v))->len = 0;
        return nv;
    }
    default:
        /* ref：浅拷贝 ptr+len（借用） */
        return slice_make_value(vm, t, sv->ptr, sv->len);
    }
}

static value_t *slice_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_type(src) != value_type(dst)) {
        value_t *casted = value_implicit_cast(vm, src, value_type(dst));
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    /* 覆盖前释放旧值（own/fatal 释放堆块；ref 跳过） */
    slice_free_heap(vm, dst);
    const type_t *dt = value_type(dst);
    memcpy(value_data(dst), value_data(src), dt->size);
    return dst;
}

static void slice_dispose(vm_t *vm, value_t *v) {
    slice_free_heap(vm, v);
}

/* ---- eq/ne：胖指针比较（ptr 相同 && len 相同） ---- */

static value_t *slice_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    if (value_type(a) != value_type(b))
        return value_make_error(vm, "slice: == requires same slice type");
    const slice_value_t *sa = (const slice_value_t *)value_data(a);
    const slice_value_t *sb = (const slice_value_t *)value_data(b);
    bool eq = (sa->ptr == sb->ptr && sa->len == sb->len);
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    memcpy(data, &eq, sizeof(bool));
    return value_make(vm, vm->type_bool, data);
}

static value_t *slice_ne(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    if (value_type(a) != value_type(b))
        return value_make_error(vm, "slice: != requires same slice type");
    const slice_value_t *sa = (const slice_value_t *)value_data(a);
    const slice_value_t *sb = (const slice_value_t *)value_data(b);
    bool ne = (sa->ptr != sb->ptr || sa->len != sb->len);
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    memcpy(data, &ne, sizeof(bool));
    return value_make(vm, vm->type_bool, data);
}

/* ---- get_index / set_index / length ---- */

static value_t *slice_get_index(vm_t *vm, value_t *self, value_t *index) {
    if (value_is_error(vm, self)) return self;
    if (value_is_error(vm, index)) return index;

    if (value_is_shadow(self) || value_is_shadow(index)) {
        const type_t *et = slice_type_elem(value_type(self));
        return value_make_shadow(vm, et ? et : vm->type_void);
    }

    size_t i;
    if (!slice_read_index(vm, index, &i))
        return value_make_error(vm, "slice index must be an integer");

    const type_t *t = value_type(self);
    const type_t *et = slice_type_elem(t);
    const slice_value_t *sv = (const slice_value_t *)value_data(self);
    if (i >= sv->len) {
        char buf[96];
        snprintf(buf, sizeof buf,
                 "slice index %zu out of bounds (len=%llu)", i,
                 (unsigned long long)sv->len);
        return value_make_error(vm, buf);
    }

    /* 返回借用引用（data 指向堆块内偏移） */
    return value_make_borrowed(vm, et,
                               (uint8_t *)sv->ptr + i * et->size);
}

static value_t *slice_set_index(vm_t *vm, value_t *self, value_t *index,
                                 value_t *val) {
    if (value_is_error(vm, self)) return self;
    if (value_is_error(vm, index)) return index;
    if (value_is_error(vm, val)) return val;

    if (value_is_shadow(self) || value_is_shadow(index) || value_is_shadow(val))
        return self;

    size_t i;
    if (!slice_read_index(vm, index, &i))
        return value_make_error(vm, "slice index must be an integer");

    const type_t *t = value_type(self);
    const type_t *et = slice_type_elem(t);
    const slice_value_t *sv = (const slice_value_t *)value_data(self);
    if (i >= sv->len) {
        char buf[96];
        snprintf(buf, sizeof buf,
                 "slice index %zu out of bounds (len=%llu)", i,
                 (unsigned long long)sv->len);
        return value_make_error(vm, buf);
    }

    /* 类型检查：非元素类型尝试隐式转换 */
    value_t *v = val;
    if (value_type(val) != et) {
        v = value_implicit_cast(vm, val, et);
        if (value_is_error(vm, v)) return v;
    }

    /* 写堆块内偏移 */
    uint8_t *slot = (uint8_t *)sv->ptr + i * et->size;
    memcpy(slot, value_data(v), et->size);
    return self;
}

static value_t *slice_length(vm_t *vm, value_t *self) {
    if (value_is_shadow(self))
        return value_make_shadow(vm, vm->type_u64);
    const slice_value_t *sv = (const slice_value_t *)value_data(self);
    void *data = value_alloc_data(vm->alloc, vm->type_u64);
    *(uint64_t *)data = sv->len;
    return value_make(vm, vm->type_u64, data);
}

/* ---- 类型转换 ---- */

static value_t *slice_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    const type_t *src = value_type(v);
    const slice_value_t *sv = (const slice_value_t *)value_data(v);

    /* own → ref 借用（同 elem 的 own → ref，§3.2）：复制 ptr+len，不释放 */
    if (src->kind == TYPE_KIND_SLICE_OWN && target &&
        target->kind == TYPE_KIND_SLICE_REF &&
        slice_type_elem(src) == slice_type_elem(target)) {
        return slice_make_value(vm, target, sv->ptr, sv->len);
    }

    /* fatal → own 接管（§3.3）：新值接管 (ptr,len)，源置空防双释放 */
    if (src->kind == TYPE_KIND_SLICE_FATAL && target &&
        target->kind == TYPE_KIND_SLICE_OWN &&
        slice_type_elem(src) == slice_type_elem(target)) {
        value_t *nv = slice_make_value(vm, target, sv->ptr, sv->len);
        ((slice_value_t *)value_data(v))->ptr = NULL;
        ((slice_value_t *)value_data(v))->len = 0;
        return nv;
    }

    /* ref → ref（copy = 浅拷贝 ptr+len） */
    if (src->kind == TYPE_KIND_SLICE_REF && target &&
        target->kind == TYPE_KIND_SLICE_REF &&
        slice_type_elem(src) == slice_type_elem(target)) {
        return slice_make_value(vm, target, sv->ptr, sv->len);
    }

    /* 同类型身份拷贝 */
    if (src == target) {
        return slice_make_value(vm, target, sv->ptr, sv->len);
    }

    return value_make_error(vm, "slice: unsupported implicit cast for slice");
}

static value_t *slice_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (value_is_shadow(v)) return value_make_shadow(vm, target);

    /* 同类型身份拷贝 */
    if (value_type(v) == target) {
        const slice_value_t *sv = (const slice_value_t *)value_data(v);
        return slice_make_value(vm, target, sv->ptr, sv->len);
    }

    return value_make_error(vm, "slice: unsupported explicit cast for slice");
}

/* ---- 鸭子类型判断 ---- */

static bool slice_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    if (a == b) return true;
    if (!b) return false;
    if (a->kind != b->kind) return false;
    if (a->kind != TYPE_KIND_SLICE_OWN && a->kind != TYPE_KIND_SLICE_REF &&
        a->kind != TYPE_KIND_SLICE_FATAL) {
        return false;
    }
    return type_equal(vm, slice_type_elem(a), slice_type_elem(b));
}

static bool slice_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    if (sub == sup) return true;
    if (!sup) return false;
    if (sub->kind != TYPE_KIND_SLICE_OWN && sub->kind != TYPE_KIND_SLICE_REF &&
        sub->kind != TYPE_KIND_SLICE_FATAL) {
        return false;
    }
    if (sub->kind != sup->kind) return false;
    return type_extends(vm, slice_type_elem(sub), slice_type_elem(sup));
}

/* ---- 类型名： "own []T" / "ref []T" / "fatal []T" ---- */

static char *slice_name(allocator_t *alloc, type_kind_t kind,
                        const type_t *elem) {
    const char *owner;
    switch (kind) {
    case TYPE_KIND_SLICE_OWN:   owner = "own ";   break;
    case TYPE_KIND_SLICE_REF:   owner = "ref ";   break;
    case TYPE_KIND_SLICE_FATAL: owner = "fatal "; break;
    default:                    owner = "";        break;
    }
    size_t ol = strlen(owner);
    size_t bl = elem && elem->name.ptr ? elem->name.len : 0;
    /* "own " + "[]" + elem_name + '\0' */
    char *buf = allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL, NULL,
                                 ol + 2 + bl + 1);
    if (!buf) return NULL;
    memcpy(buf, owner, ol);
    buf[ol] = '[';
    buf[ol + 1] = ']';
    if (bl) memcpy(buf + ol + 2, elem->name.ptr, bl);
    buf[ol + 2 + bl] = '\0';
    return buf;
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_SLICE_OWN = {
    .eq = slice_eq, .ne = slice_ne,
    .dispose = slice_dispose,
    .clone = slice_clone,
    .assign = slice_assign,
    .implicit_cast = slice_implicit_cast,
    .explicit_cast = slice_explicit_cast,
    .get_index = slice_get_index,
    .set_index = slice_set_index,
    .length = slice_length,
    .type_equal = slice_type_equal,
    .type_extends = slice_type_extends,
    .type_seal = type_slice_seal,
};

const vtable_t VTABLE_SLICE_REF = {
    .eq = slice_eq, .ne = slice_ne,
    .dispose = slice_dispose,
    .clone = slice_clone,
    .assign = slice_assign,
    .implicit_cast = slice_implicit_cast,
    .explicit_cast = slice_explicit_cast,
    .get_index = slice_get_index,
    .set_index = slice_set_index,
    .length = slice_length,
    .type_equal = slice_type_equal,
    .type_extends = slice_type_extends,
    .type_seal = type_slice_seal,
};

const vtable_t VTABLE_SLICE_FATAL = {
    .eq = slice_eq, .ne = slice_ne,
    .dispose = slice_dispose,
    .clone = slice_clone,
    .assign = slice_assign,
    .implicit_cast = slice_implicit_cast,
    .explicit_cast = slice_explicit_cast,
    .get_index = slice_get_index,
    .set_index = slice_set_index,
    .length = slice_length,
    .type_equal = slice_type_equal,
    .type_extends = slice_type_extends,
    .type_seal = type_slice_seal,
};

/* ===========================================================================
 * 两遍构造协议
 * =========================================================================== */

static slice_type_t *slice_type_create_open(vm_t *vm, type_kind_t kind) {
    slice_type_t *st = (slice_type_t *)allocator_new_ex(
        vm->alloc, "slice_type_t", sizeof(slice_type_t), NULL, NULL, NULL, 1);
    if (!st) panic("vm: out of memory allocating slice type");
    memset(st, 0, sizeof(slice_type_t));
    switch (kind) {
    case TYPE_KIND_SLICE_OWN:   st->base.vtable = &VTABLE_SLICE_OWN;   break;
    case TYPE_KIND_SLICE_REF:   st->base.vtable = &VTABLE_SLICE_REF;   break;
    case TYPE_KIND_SLICE_FATAL: st->base.vtable = &VTABLE_SLICE_FATAL; break;
    default:
        panic("vm: invalid slice ownership kind");
        break;
    }
    st->base.kind = kind;
    /* name/size/align 由 seal 按 elem 填充；elem_type 由 SET_TYPE 设定 */
    return st;
}

const type_t *type_slice_push(vm_t *vm, type_kind_t kind) {
    if (!vm) return NULL;
    slice_type_t *st = slice_type_create_open(vm, kind);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &st->base));
    return &st->base;
}

void type_slice_set_elem(vm_t *vm, const type_t *t, const type_t *elem) {
    (void)vm;
    if (!t || type_is_sealed(t)) return;
    if (t->kind != TYPE_KIND_SLICE_OWN && t->kind != TYPE_KIND_SLICE_REF &&
        t->kind != TYPE_KIND_SLICE_FATAL) {
        return;
    }
    ((slice_type_t *)t)->elem_type = elem;
}

const type_t *type_slice_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t) return NULL;
    if (t->kind != TYPE_KIND_SLICE_OWN && t->kind != TYPE_KIND_SLICE_REF &&
        t->kind != TYPE_KIND_SLICE_FATAL) {
        return NULL;
    }
    if (type_is_sealed(t)) return t;

    slice_type_t *st = (slice_type_t *)t;
    if (!st->elem_type) return NULL;

    if (!vm->slice_types) vm->slice_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* dedup intern by (kind, elem_type) */
    size_t n = vec_len(vm->slice_types);
    for (size_t i = 0; i < n; i++) {
        const slice_type_t *other = (const slice_type_t *)vec_get(vm->slice_types, i);
        if (other && other != st && type_is_sealed(&other->base) &&
            other->base.kind == st->base.kind &&
            other->elem_type == st->elem_type) {
            if (st->base.name.ptr) {
                char *np = (char *)st->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&st);
            return &other->base;
        }
    }

    st->base.size   = sizeof(slice_value_t);
    st->base.align  = _Alignof(slice_value_t);
    st->base.sealed = true;

    char *name = slice_name(vm->alloc, st->base.kind, st->elem_type);
    if (!name) panic("vm: out of memory allocating slice type name");
    st->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->slice_types, vm->alloc, st);
    return &st->base;
}

const type_t *type_slice_intern(vm_t *vm, type_kind_t kind,
                                const type_t *elem) {
    if (!vm || !elem) return NULL;
    if (kind != TYPE_KIND_SLICE_OWN && kind != TYPE_KIND_SLICE_REF &&
        kind != TYPE_KIND_SLICE_FATAL) {
        return NULL;
    }
    slice_type_t *st = slice_type_create_open(vm, kind);
    st->elem_type = elem;
    return type_slice_seal(vm, &st->base);
}

/* ---- 构造切片 value ---- */

value_t *slice_make_value(vm_t *vm, const type_t *slice_type,
                          void *ptr, uint64_t len) {
    slice_value_t sv = { ptr, len };
    void *data = value_alloc_data_copy(vm->alloc, slice_type, &sv);
    return value_make(vm, slice_type, data);
}
