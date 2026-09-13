#include "vm/type_array.h"
#include "vm/type.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "vm/scope.h"
#include "core/vec.h"
#include "core/panic.h"
#include "core/string.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>
#include <stdio.h>

/* ================================================================ */
/* 数组 value 内部表示                                              */
/* ================================================================ */

/*
 * 数组 value 的 data 块持有一个元素 vec。vec 不拥有元素（owns_element=false），
 * 元素 value 由 scope 持有生命周期（value_make / value_clone 自动 track）。
 * 数组 clone 时逐元素 clone 到新 scope；数组 dispose 仅释放 vec 结构本身，
 * 元素 data 由 scope 统一释放，避免重复释放。
 */
typedef struct {
    vec_t *elems;  /* value_t*，owns_element = false */
} array_data_t;

/* ---- 内部：读取 index value 的整数值（运行时 value，非字面整数） ---- */

static bool array_read_index(vm_t *vm, value_t *index, size_t *out) {
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

/* ---- 类型名： [elem_type] / [elem_type; N] ---- */

static char *array_type_name(allocator_t *alloc, const type_t *elem, size_t len) {
    string_t *s = string_new(alloc);
    if (!s) panic("vm: out of memory building array type name");
    string_append_cstr(s, "[");
    if (elem && elem->name.ptr)
        string_append_bytes(s, elem->name.ptr, elem->name.len);
    else
        string_append_cstr(s, "?");
    if (len != SIZE_MAX) {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "; %zu", len);
        string_append_bytes(s, buf, (size_t)n);
    }
    string_append_cstr(s, "]");
    char *out = (char *)allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL,
                                          NULL, string_len(s) + 1);
    if (out) {
        memcpy(out, string_data(s), string_len(s));
        out[string_len(s)] = '\0';
    }
    string_free(&s);
    return out;
}

/* ================================================================ */
/* 数组类型池（按 elem_type + length 去重 intern）                   */
/* ================================================================ */

/* ---- 初始化开放数组类型的公共 base 字段（运行时值存储 = array_data_t） ---- */

static void array_type_init_base(array_type_t *at) {
    at->base.vtable = &VTABLE_ARRAY;
    at->base.name   = (strslice_t){ NULL, 0 };   /* 由 seal 填充 */
    at->base.size   = sizeof(array_data_t);      /* 运行时值存储大小（vec 指针） */
    at->base.align  = alignof(array_data_t);
    at->base.kind   = TYPE_KIND_ARRAY;
    at->elem_type   = NULL;
    at->length      = SIZE_MAX;   /* 未成形：元素数量待定 */
    at->formed      = false;      /* 未定长成形 */
    /* sealed 由 memset 置 0；密封在 array_type_seal 中置位 */
    at->layout_size = 0;
    at->layout_align= 0;
}

/* ---- 构造：push_array / set_elem / set_count / seal ---- */

const type_t *array_type_push(vm_t *vm) {
    if (!vm) return NULL;

    /* 分配空 array_type（不入池，密封时才加入 vm->array_types） */
    array_type_t *at = (array_type_t *)allocator_new_ex(
        vm->alloc, "array_type_t", sizeof(array_type_t), NULL, NULL, NULL, 1);
    if (!at) panic("vm: out of memory allocating array type");
    memset(at, 0, sizeof(array_type_t));

    array_type_init_base(at);  /* 未成形、未密封：等 set_elem / set_count / seal */

    /* 把该 type 对应的 type value 压入操作数栈（对应字节码 push_array） */
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &at->base));

    return &at->base;  /* 外部只持有 type_t*，不感知 array_type_t 子类 */
}

void array_type_set_elem(vm_t *vm, const type_t *t, const type_t *elem_type) {
    (void)vm;
    if (!t || t->kind != TYPE_KIND_ARRAY) return;
    array_type_t *at = (array_type_t *)t;
    if (type_is_sealed(t) || at->formed) return;  /* 密封/已成形后不可再设 */
    at->elem_type = elem_type;
}

void array_type_set_count(vm_t *vm, const type_t *t, size_t count) {
    (void)vm;
    if (!t || t->kind != TYPE_KIND_ARRAY) return;
    array_type_t *at = (array_type_t *)t;
    if (type_is_sealed(t)) return;
    if (at->formed) return;            /* 重复 set_count 是错误，静默忽略 */
    if (!at->elem_type) return;        /* 未 set_elem 即 set_count 是错误 */
    at->length = count;
    at->formed = true;
}

const type_t *array_type_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_ARRAY) return NULL;
    if (type_is_sealed(t)) return t;  /* 已密封直接返回（幂等） */

    array_type_t *at = (array_type_t *)t;

    /* 必须已设元素类型（动态切片可省略 set_count，length 保持 SIZE_MAX） */
    if (!at->elem_type) return NULL;

    if (!vm->array_types) vm->array_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern（按 elem_type + length）：命中已有 sealed 类型则复用并手工回收本开放类型 */
    size_t n = vec_len(vm->array_types);
    for (size_t i = 0; i < n; i++) {
        const array_type_t *other = (const array_type_t *)vec_get(vm->array_types, i);
        if (other && other != at && type_is_sealed(&other->base) &&
            other->elem_type == at->elem_type && other->length == at->length) {
            /* 本开放类型与已有 sealed 类型重复：复用 other。
             * 操作数栈中引用本开放类型 at 的 type value 由 value_seal 负责
             * 重定向到 other（避免悬空）；此处仅手工回收 at。 */
            if (at->base.name.ptr) {
                char *np = (char *)at->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&at);
            return &other->base;
        }
    }

    /* 计算内存布局（运行时值仍由内部 vec 承载，故 layout_* 仅编译期语义） */
    at->layout_align = at->elem_type->align;
    at->layout_size  = (at->length == SIZE_MAX)
                          ? 0
                          : at->elem_type->size * at->length;
    at->base.sealed  = true;

    /* 重建类型名（含 length：固定长度 [elem; N]，未定长 [elem]） */
    char *name = array_type_name(vm->alloc, at->elem_type, at->length);
    if (!name) panic("vm: out of memory allocating array type name");
    at->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->array_types, vm->alloc, at);  /* 密封后入池（去重 intern） */
    return &at->base;
}

const type_t *type_array_intern(vm_t *vm, const type_t *elem_type, size_t count) {
    /* 一次性快捷：push_array + set_elem + set_count + seal */
    const type_t *t = array_type_push(vm);
    if (!t) return NULL;
    array_type_set_elem(vm, t, elem_type);
    array_type_set_count(vm, t, count);
    return array_type_seal(vm, t);
}

/* ================================================================ */
/* vtable 实现                                                      */
/* ================================================================ */

/* ---- length: 返回 u64 元素个数 ---- */

static value_t *array_length(vm_t *vm, value_t *self) {
    if (value_is_shadow(self))
        return value_make_shadow(vm, vm->type_u64);
    array_data_t *d = (array_data_t *)value_data(self);
    size_t n = (d && d->elems) ? vec_len(d->elems) : 0;
    void *data = value_alloc_data(vm->alloc, vm->type_u64);
    *(uint64_t *)data = (uint64_t)n;
    return value_make(vm, vm->type_u64, data);
}

/* ---- get_index: self[index] -> 元素副本 ---- */

static value_t *array_get_index(vm_t *vm, value_t *self, value_t *index) {
    if (value_is_error(vm, self)) return self;
    if (value_is_error(vm, index)) return index;

    /* shadow：只返回元素类型 shadow（不操作实际数据） */
    if (value_is_shadow(self) || value_is_shadow(index)) {
        const type_t *et = array_type_elem(value_type(self));
        return value_make_shadow(vm, et ? et : vm->type_void);
    }

    size_t i;
    if (!array_read_index(vm, index, &i))
        return value_make_error(vm, "array index must be an integer");

    array_data_t *d = (array_data_t *)value_data(self);
    size_t len = (d && d->elems) ? vec_len(d->elems) : 0;
    /* 越界检查：索引为负数（按无符号读入的大值，或真越界）一律拦下，
       返回硬错误（exec_drive 据此停机），并给出索引与长度便于定位。 */
    if (i >= len) {
        char buf[96];
        snprintf(buf, sizeof buf,
                 "array index %zu out of bounds (len=%zu)", i, len);
        return value_make_error(vm, buf);
    }

    value_t *elem = (value_t *)vec_get(d->elems, i);
    /* 只读访问：返回元素副本（clone 到当前作用域） */
    return value_clone(vm, elem);
}

/* ---- set_index: self[index] = val -> self ---- */

static value_t *array_set_index(vm_t *vm, value_t *self, value_t *index,
                                 value_t *val) {
    if (value_is_error(vm, self)) return self;
    if (value_is_error(vm, index)) return index;
    if (value_is_error(vm, val)) return val;

    /* shadow：仅类型检查，不操作实际数据 */
    if (value_is_shadow(self) || value_is_shadow(index) || value_is_shadow(val))
        return self;

    size_t i;
    if (!array_read_index(vm, index, &i))
        return value_make_error(vm, "array index must be an integer");

    array_data_t *d = (array_data_t *)value_data(self);
    size_t len = (d && d->elems) ? vec_len(d->elems) : 0;
    /* 越界检查：见 array_get_index 同款说明 */
    if (i >= len) {
        char buf[96];
        snprintf(buf, sizeof buf,
                 "array index %zu out of bounds (len=%zu)", i, len);
        return value_make_error(vm, buf);
    }

    /* 类型检查：非元素类型尝试隐式转换 */
    const type_t *et = array_type_elem(value_type(self));
    if (et && value_type(val) != et) {
        value_t *casted = value_implicit_cast(vm, val, et);
        if (value_is_error(vm, casted)) return casted;
        val = casted;
    }

    value_t *old = (value_t *)vec_get(d->elems, i);
    value_t *cloned = value_clone(vm, val);  /* 元素归 scope 持有 */
    vec_set(d->elems, i, cloned);
    if (old) value_dispose(vm, old);  /* 释放被替换元素的 data（struct 由 scope 释放） */
    return self;
}

/* ---- dispose: 仅释放 vec 结构（元素归 scope 管理） ---- */

static void array_dispose(vm_t *vm, value_t *v) {
    array_data_t *d = (array_data_t *)value_data(v);
    if (d && d->elems) {
        vec_free(vm->alloc, &d->elems);  /* 不 owns 元素，仅释放 vec 结构 */
    }
}

/* ---- clone: 逐元素 clone（元素 track 到当前作用域） ---- */

static value_t *array_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));

    array_data_t *src = (array_data_t *)value_data(v);
    array_data_t *d = (array_data_t *)value_alloc_data(vm->alloc, value_type(v));
    d->elems = vec_new(vm->alloc, /*owns_element=*/false);
    if (!d->elems) panic("vm: out of memory cloning array");

    size_t n = src->elems ? vec_len(src->elems) : 0;
    for (size_t i = 0; i < n; i++) {
        value_t *e = (value_t *)vec_get(src->elems, i);
        vec_push(d->elems, vm->alloc, value_clone(vm, e));
    }
    return value_make(vm, value_type(v), d);
}

/* ---- assign: 释放旧元素 vec，clone 新元素 ---- */

static value_t *array_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;

    array_dispose(vm, dst);  /* 释放 dst 的元素 vec */
    array_data_t *d = (array_data_t *)value_data(dst);
    array_data_t *s = (array_data_t *)value_data(src);
    d->elems = vec_new(vm->alloc, /*owns_element=*/false);
    if (!d->elems) panic("vm: out of memory assigning array");

    size_t n = s->elems ? vec_len(s->elems) : 0;
    for (size_t i = 0; i < n; i++) {
        value_t *e = (value_t *)vec_get(s->elems, i);
        vec_push(d->elems, vm->alloc, value_clone(vm, e));
    }
    return dst;
}

const vtable_t VTABLE_ARRAY = {
    .get_index = array_get_index,
    .set_index = array_set_index,
    .length    = array_length,
    .dispose   = array_dispose,
    .clone     = array_clone,
    .assign    = array_assign,
    .type_seal = array_type_seal,
};

/* ================================================================ */
/* 数组 value 构造                                                  */
/* ================================================================ */

value_t *value_make_array(vm_t *vm, const type_t *elem_type,
                          value_t **elems, size_t count) {
    if (!vm || !elem_type) return NULL;

    const type_t *at = type_array_intern(vm, elem_type, count);
    if (!at) return value_make_error(vm, "array: failed to intern array type");

    array_data_t *d = (array_data_t *)value_alloc_data(vm->alloc, at);
    d->elems = vec_new(vm->alloc, /*owns_element=*/false);
    if (!d->elems) panic("vm: out of memory creating array");

    for (size_t i = 0; i < count; i++) {
        value_t *e = elems[i];
        if (value_is_error(vm, e)) {
            vec_free(vm->alloc, &d->elems);
            return e;
        }
        /* 类型检查：非元素类型尝试隐式转换 */
        if (value_type(e) != elem_type) {
            value_t *casted = value_implicit_cast(vm, e, elem_type);
            if (value_is_error(vm, casted)) {
                vec_free(vm->alloc, &d->elems);
                return casted;
            }
            e = casted;
        }
        vec_push(d->elems, vm->alloc, value_clone(vm, e));
    }
    return value_make(vm, at, d);
}

void array_push(vm_t *vm, value_t *arr, value_t *elem) {
    if (!vm || !arr || !elem) return;
    if (value_is_error(vm, elem)) return;
    if (value_is_shadow(arr) || value_is_shadow(elem)) return;

    array_data_t *d = (array_data_t *)value_data(arr);
    if (!d) return;
    if (!d->elems) {
        d->elems = vec_new(vm->alloc, /*owns_element=*/false);
        if (!d->elems) panic("vm: out of memory growing array");
    }
    const type_t *et = array_type_elem(value_type(arr));
    value_t *e = elem;
    if (et && value_type(e) != et) {
        value_t *casted = value_implicit_cast(vm, e, et);
        if (value_is_error(vm, casted)) return;  /* 类型错误静默跳过 */
        e = casted;
    }
    vec_push(d->elems, vm->alloc, value_clone(vm, e));
}

/* ================================================================ */
/* 运行期数组 value 只读访问（供调试/格式化遍历，如 printf %v）        */
/* ================================================================ */

size_t value_array_count(const value_t *v) {
    if (!v || value_kind(v) != TYPE_KIND_ARRAY) return 0;
    array_data_t *d = (array_data_t *)value_data(v);
    return (d && d->elems) ? vec_len(d->elems) : 0;
}

const value_t *value_array_at(const value_t *v, size_t i) {
    if (!v || value_kind(v) != TYPE_KIND_ARRAY) return NULL;
    array_data_t *d = (array_data_t *)value_data(v);
    if (!d || !d->elems) return NULL;
    size_t n = vec_len(d->elems);
    if (i >= n) return NULL;
    return (const value_t *)vec_get(d->elems, i);
}
