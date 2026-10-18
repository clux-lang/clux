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
 * 所有权纯编译期——类型不区分 own/ref/fatal，只有一种切片类型 []T。
 * 运行期零标志：clone/dispose/cast 不按所有权分派，堆释放由 sema 生成
 * FREE 指令负责。
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
    /* 所有权纯编译期——clone 统一为浅拷贝胖指针（ptr+len）。
       深拷贝由 CLONE 指令（op_clone）显式调用 ptr_clone_block 完成。 */
    return slice_make_value(vm, t, sv->ptr, sv->len);
}

static value_t *slice_assign(vm_t *vm, value_t *dst, value_t *src) {
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

static void slice_dispose(vm_t *vm, value_t *v) {
    /* 所有权纯编译期——dispose 不释放堆块（由 FREE 指令负责）。 */
    (void)vm; (void)v;
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

    /* 所有权纯编译期——类型不区分 own/ref/fatal，同 elem 即同类型。
       ownership 降级/接管由 sema 在编译期控制。 */
    if (src == target ||
        (target && slice_type_elem(src) == slice_type_elem(target))) {
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

/* ---- 鸭子类型判断（所有权纯编译期，类型不区分 ownership） ---- */

static bool slice_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    if (a == b) return true;
    if (!b) return false;
    if (!type_is_slice(a) || !type_is_slice(b)) return false;
    /* 元素槽位所有权是类型描述的一部分：`[]own *T` 与 `[]ref *T` 是不同类型
       （销毁语义不同），不能互相赋值。 */
    if (slice_type_elem_ownership(a) != slice_type_elem_ownership(b))
        return false;
    return type_equal(vm, slice_type_elem(a), slice_type_elem(b));
}

static bool slice_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    if (sub == sup) return true;
    if (!sup) return false;
    if (!type_is_slice(sub)) return false;
    return type_extends(vm, slice_type_elem(sub), slice_type_elem(sup));
}

/* ---- 类型名： "[]T"（所有权纯编译期，类型名不携带 ownership 前缀） ---- */

static char *slice_name(allocator_t *alloc, const type_t *elem) {
    size_t bl = elem && elem->name.ptr ? elem->name.len : 0;
    /* "[]" + elem_name + '\0' */
    char *buf = allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL, NULL,
                                 2 + bl + 1);
    if (!buf) return NULL;
    buf[0] = '[';
    buf[1] = ']';
    if (bl) memcpy(buf + 2, elem->name.ptr, bl);
    buf[2 + bl] = '\0';
    return buf;
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_SLICE = {
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

static slice_type_t *slice_type_create_open(vm_t *vm) {
    slice_type_t *st = (slice_type_t *)allocator_new_ex(
        vm->alloc, "slice_type_t", sizeof(slice_type_t), NULL, NULL, NULL, 1);
    if (!st) panic("vm: out of memory allocating slice type");
    memset(st, 0, sizeof(slice_type_t));
    st->base.vtable = &VTABLE_SLICE;
    st->base.kind = TYPE_KIND_SLICE;
    /* name/size/align 由 seal 按 elem 填充；elem_type 由 SET_TYPE 设定 */
    return st;
}

const type_t *type_slice_push(vm_t *vm) {
    if (!vm) return NULL;
    slice_type_t *st = slice_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &st->base));
    return &st->base;
}

void type_slice_set_elem(vm_t *vm, const type_t *t, const type_t *elem) {
    (void)vm;
    if (!t || type_is_sealed(t)) return;
    if (!type_is_slice(t)) return;
    ((slice_type_t *)t)->elem_type = elem;
}

/* SET_FIELD_OWNERSHIP 运行期用（slice 分支）：设元素槽位的所有权（seal 前）。 */
void type_slice_set_elem_ownership(vm_t *vm, const type_t *t, ownership_t own) {
    (void)vm;
    if (!t || type_is_sealed(t)) return;
    if (!type_is_slice(t)) return;
    ((slice_type_t *)t)->elem_ownership = own;
}

const type_t *type_slice_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t) return NULL;
    if (!type_is_slice(t)) return NULL;
    if (type_is_sealed(t)) return t;

    slice_type_t *st = (slice_type_t *)t;
    if (!st->elem_type) return NULL;

    if (!vm->slice_types) vm->slice_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* dedup intern by (elem_type + 元素槽位所有权) */
    size_t n = vec_len(vm->slice_types);
    for (size_t i = 0; i < n; i++) {
        const slice_type_t *other = (const slice_type_t *)vec_get(vm->slice_types, i);
        if (other && other != st && type_is_sealed(&other->base) &&
            other->elem_type == st->elem_type &&
            other->elem_ownership == st->elem_ownership) {
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

    char *name = slice_name(vm->alloc, st->elem_type);
    if (!name) panic("vm: out of memory allocating slice type name");
    st->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->slice_types, vm->alloc, st);
    return &st->base;
}

const type_t *type_slice_intern(vm_t *vm, const type_t *elem) {
    return type_slice_intern_own(vm, elem, OWN_NONE);
}

/* 带元素槽位所有权的一次性构造（`own []own *T` 等）。 */
const type_t *type_slice_intern_own(vm_t *vm, const type_t *elem,
                                    ownership_t elem_ownership) {
    if (!vm || !elem) return NULL;
    slice_type_t *st = slice_type_create_open(vm);
    st->elem_type = elem;
    st->elem_ownership = elem_ownership;
    return type_slice_seal(vm, &st->base);
}

/* ---- 构造切片 value ---- */

value_t *slice_make_value(vm_t *vm, const type_t *slice_type,
                          void *ptr, uint64_t len) {
    slice_value_t sv = { ptr, len };
    void *data = value_alloc_data_copy(vm->alloc, slice_type, &sv);
    return value_make(vm, slice_type, data);
}
