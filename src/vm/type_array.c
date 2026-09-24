#include "vm/type_array.h"
#include "vm/type.h"
#include "vm/type_option.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
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
 * 数组 value 的 data 块是一段**连续业务内存**（C 数组语义）：
 *
 *   data = [elem_0][elem_1]...[elem_{N-1}]
 *          每块 elem_i 大小为 elem_type->size，对齐 elem_type->align，
 *          按固定偏移排列（data + i * elem_type->size）。
 *
 * 元素在块内是裸数据（无 value_t 头，非引擎对象）：
 *   - 标量元素：直接是数字字节（i32 等）
 *   - 嵌套数组元素：递归的连续子块（N * sizeof(子元素) 字节）
 *   - 字符串元素：string_t* 指针（深拷贝时复制 string_t 本体）
 *
 * 借用引用（is_own=false）的 data 直接指向块内偏移（业务内存）：
 *   arr[i]  →  data = arr.data + i * elem_type->size，type = elem_type
 *   m[1][0] →  内层借用 data = m.data + 1*sizeof([..]T)，再偏移 0
 * 读写直达业务内存，与 C 的 &arr[i] 语义一致（M2 设计 §2）。
 *
 * 生命周期（用户确认）：借用值只匿名存活于表达式链；绑定变量 / 返回值 /
 * 赋值（DEFINE / STORE / RET / clone）时经 value_clone materialize 成独立
 * 深拷贝（is_own=true），故无悬垂引用。借用值 dispose 跳过（data 指向父值
 * 内部，value_dispose 已处理）。块在构造后固定（M2 数组边界编译期常量，
 * 无运行时增长），借用存续期间父块不变，安全。
 */

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

/* ---- 初始化开放数组类型的公共 base 字段（运行时值存储 = 连续元素块） ---- */

static void array_type_init_base(array_type_t *at) {
    at->base.vtable = &VTABLE_ARRAY;
    at->base.name   = (strslice_t){ NULL, 0 };   /* 由 seal 填充 */
    at->base.size   = 0;      /* 未成形：密封时按 elem_size * length 计算 */
    at->base.align  = 0;
    at->base.kind   = TYPE_KIND_ARRAY;
    at->elem_type   = NULL;
    at->length      = SIZE_MAX;   /* 未成形：元素数量待定 */
    at->formed      = false;      /* 未定长成形 */
    /* sealed 由 memset 置 0；密封在 array_type_seal 中置位 */
    at->layout_size = 0;
    at->layout_align= 0;
}

/* ---- 构造：push_array / set_elem / set_count / seal ---- */

/* 分配开放 array_type（不入池，密封时才加入 vm->array_types），不压栈。
 * 供字节码路径（array_type_push）与值构造路径（type_array_intern）共用；
 * 压栈副作用仅由 array_type_push 承担，避免运行时/sema intern 污染操作数栈。 */
static array_type_t *array_type_create_open(vm_t *vm) {
    array_type_t *at = (array_type_t *)allocator_new_ex(
        vm->alloc, "array_type_t", sizeof(array_type_t), NULL, NULL, NULL, 1);
    if (!at) panic("vm: out of memory allocating array type");
    memset(at, 0, sizeof(array_type_t));

    array_type_init_base(at);  /* 未成形、未密封：等 set_elem / set_count / seal */
    return at;
}

const type_t *array_type_push(vm_t *vm) {
    if (!vm) return NULL;

    array_type_t *at = array_type_create_open(vm);

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

    /* 计算内存布局：数组值是连续元素块，size = elem_size * length，
       align 跟随元素类型（C 数组语义，M2 设计 §2） */
    at->layout_align = at->elem_type->align;
    at->layout_size  = (at->length == SIZE_MAX)
                          ? 0
                          : at->elem_type->size * at->length;
    at->base.size    = at->layout_size;    /* 运行时值存储 = 连续元素块 */
    at->base.align   = at->layout_align;
    at->base.sealed  = true;

    /* 重建类型名（含 length：固定长度 [elem; N]，未定长 [elem]） */
    char *name = array_type_name(vm->alloc, at->elem_type, at->length);
    if (!name) panic("vm: out of memory allocating array type name");
    at->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->array_types, vm->alloc, at);  /* 密封后入池（去重 intern） */
    return &at->base;
}

const type_t *type_array_intern(vm_t *vm, const type_t *elem_type, size_t count) {
    /* 一次性快捷：alloc（不压栈）+ set_elem + set_count + seal。
     * 与 array_type_push 的区别：不把 type value 压入操作数栈——本函数供
     * sema resolve_type_expr 与 value_make_array 等非字节码路径使用，
     * 压栈只会污染操作数栈（嵌套构造场景曾因此栈布局错位）。 */
    array_type_t *at = array_type_create_open(vm);
    const type_t *t = &at->base;
    array_type_set_elem(vm, t, elem_type);
    array_type_set_count(vm, t, count);
    return array_type_seal(vm, t);
}

/* ================================================================ */
/* vtable 实现                                                      */
/* ================================================================ */

/* ================================================================ */
/* 连续块元素操作（业务内存深拷贝 / 资源释放）                        */
/* ================================================================ */

/*
 * 块内元素是裸数据（无 value_t 头）。按元素类型递归处理：
 *   - 标量（int/float/bool 等无指针）：整段 memcpy
 *   - 字符串：块内存 string_t* 指针，深拷贝复制 string_t 本体
 *   - 嵌套数组：递归子块（子块内可能含字符串，须递归）
 *   - optional：ok tag 平凡拷贝 + value 字段按 inner 递归（?str/?数组）
 * raw 版本操作裸数据块；value 版本先从 value 取 data 再委托 raw。
 * 两个 raw 函数同时服务 option 的 clone/assign/dispose（type_option.c）。
 */

void value_blit_raw(vm_t *vm, void *dst, const void *src, const type_t *t);
void value_dispose_raw(vm_t *vm, void *raw, const type_t *t);

/* value → 块内偏移（dst 处写入 src value 的深拷贝） */
static void array_blit_value(vm_t *vm, void *dst, value_t *src, const type_t *t) {
    if (!dst || !src || !t) return;
    if (value_is_shadow(src)) {   /* shadow 仅类型计算，块内无数据可拷贝 */
        memset(dst, 0, t->size);
        return;
    }
    value_blit_raw(vm, dst, value_data(src), t);
}

void value_blit_raw(vm_t *vm, void *dst, const void *src, const type_t *t) {
    if (!dst || !src || !t) return;
    switch (t->kind) {
        case TYPE_KIND_ARRAY: {
            const type_t *et = array_type_elem(t);
            size_t es = et->size;
            size_t len = array_type_len(t);
            for (size_t i = 0; i < len; i++)
                value_blit_raw(vm, (uint8_t *)dst + i * es,
                               (const uint8_t *)src + i * es, et);
            break;
        }
        case TYPE_KIND_STRUCT: {
            /* 按字段偏移递归（字段类型可能含资源：str/数组/嵌套 struct/option） */
            size_t n = struct_type_field_count(t);
            for (size_t i = 0; i < n; i++) {
                const struct_field_t *f = struct_type_field(t, i);
                value_blit_raw(vm, (uint8_t *)dst + f->offset,
                               (const uint8_t *)src + f->offset, f->type);
            }
            break;
        }
        case TYPE_KIND_TUPLE: {
            /* 按元素偏移递归（元素类型可能含资源：str/数组/嵌套 tuple/option） */
            size_t n = tuple_type_elem_count(t);
            for (size_t i = 0; i < n; i++) {
                const tuple_elem_t *e = tuple_type_elem(t, i);
                value_blit_raw(vm, (uint8_t *)dst + e->offset,
                               (const uint8_t *)src + e->offset, e->type);
            }
            break;
        }
        case TYPE_KIND_UNION: {
            /* tag 平凡拷贝 + payload 按当前 tag 的 member 递归（读 src 的
               tag——dst 与 src 同类型实例，tag 相同；payload_struct NULL =
               纯 tag member 无 payload 区） */
            size_t ts = union_type_tag_size(t);
            memcpy(dst, src, ts);
            size_t idx = (size_t)union_read_tag_raw(src, ts);
            const union_member_t *m = union_type_member(t, idx);
            const type_t *pt = m ? m->payload_struct : NULL;
            if (pt)
                value_blit_raw(vm, (uint8_t *)dst + union_type_payload_offset(t),
                               (const uint8_t *)src + union_type_payload_offset(t), pt);
            break;
        }
        case TYPE_KIND_STR: {
            const string_t *s = *(const string_t *const *)src;
            string_t *copy = s ? string_from_string(vm->alloc, s) : NULL;
            if (s && !copy) panic("vm: out of memory copying array element string");
            *(string_t **)dst = copy;
            break;
        }
        case TYPE_KIND_OPTION: {
            /* ok tag 平凡拷贝 + value 字段按 inner 递归（?str/?数组深拷贝） */
            const type_t *it = type_option_inner(t);
            *(bool *)dst = *(const bool *)src;
            value_blit_raw(vm, (uint8_t *)dst + option_value_offset(t),
                           (const uint8_t *)src + option_value_offset(t), it);
            break;
        }
        default:
            memcpy(dst, src, t->size);
            break;
    }
}

/* 释放块内元素持有的资源（标量无资源；字符串释放 string_t；数组递归；
   optional 递归 value 字段——ok tag 无资源） */
void value_dispose_raw(vm_t *vm, void *raw, const type_t *t) {
    if (!raw || !t) return;
    switch (t->kind) {
        case TYPE_KIND_ARRAY: {
            const type_t *et = array_type_elem(t);
            size_t es = et->size;
            size_t len = array_type_len(t);
            for (size_t i = 0; i < len; i++)
                value_dispose_raw(vm, (uint8_t *)raw + i * es, et);
            break;
        }
        case TYPE_KIND_STRUCT: {
            size_t n = struct_type_field_count(t);
            for (size_t i = 0; i < n; i++) {
                const struct_field_t *f = struct_type_field(t, i);
                value_dispose_raw(vm, (uint8_t *)raw + f->offset, f->type);
            }
            break;
        }
        case TYPE_KIND_TUPLE: {
            size_t n = tuple_type_elem_count(t);
            for (size_t i = 0; i < n; i++) {
                const tuple_elem_t *e = tuple_type_elem(t, i);
                value_dispose_raw(vm, (uint8_t *)raw + e->offset, e->type);
            }
            break;
        }
        case TYPE_KIND_UNION: {
            /* 先读 data 首部 tag 才知道按哪个 member 释放 payload */
            size_t ts = union_type_tag_size(t);
            size_t idx = (size_t)union_read_tag_raw(raw, ts);
            const union_member_t *m = union_type_member(t, idx);
            const type_t *pt = m ? m->payload_struct : NULL;
            if (pt)
                value_dispose_raw(vm, (uint8_t *)raw + union_type_payload_offset(t), pt);
            break;
        }
        case TYPE_KIND_STR: {
            string_t **sp = (string_t **)raw;
            if (sp && *sp) string_free(sp);
            break;
        }
        case TYPE_KIND_OPTION: {
            const type_t *it = type_option_inner(t);
            value_dispose_raw(vm, (uint8_t *)raw + option_value_offset(t), it);
            break;
        }
        default:
            break;   /* 标量无资源 */
    }
}

/* ================================================================ */
/* vtable 实现                                                      */
/* ================================================================ */

/* ---- length: 返回 u64 元素个数 ---- */

static value_t *array_length(vm_t *vm, value_t *self) {
    if (value_is_shadow(self))
        return value_make_shadow(vm, vm->type_u64);
    /* 借用 self 的 type 也是数组类型（元素类型），length 从类型取 */
    size_t n = array_type_len(value_type(self));
    void *data = value_alloc_data(vm->alloc, vm->type_u64);
    *(uint64_t *)data = (uint64_t)n;
    return value_make(vm, vm->type_u64, data);
}

/* ---- get_index: self[index] -> 借用引用（data 指向块内业务内存） ---- */

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

    const type_t *t = value_type(self);
    const type_t *et = array_type_elem(t);
    size_t len = array_type_len(t);
    /* 越界检查：索引为负数（按无符号读入的大值，或真越界）一律拦下，
       返回硬错误（exec_drive 据此停机），并给出索引与长度便于定位。 */
    if (i >= len) {
        char buf[96];
        snprintf(buf, sizeof buf,
                 "array index %zu out of bounds (len=%zu)", i, len);
        return value_make_error(vm, buf);
    }

    /* 统一返回借用引用（标量与复合元素一致，C 左值语义）：
       data 直接指向块内业务内存偏移 arr.data + i * elem_size，
       读即直接解引用该内存，写经 INDEX_SET 直达原数组。 */
    return value_make_borrowed(vm, et,
                               (uint8_t *)value_data(self) + i * et->size);
}

/* ---- set_index: self[index] = val -> self（赋值走独立 INDEX_SET 指令，
 *     不经过 GET_INDEX 缓存，直接写块内偏移） ---- */

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

    const type_t *t = value_type(self);
    const type_t *et = array_type_elem(t);
    size_t len = array_type_len(t);
    /* 越界检查：见 array_get_index 同款说明 */
    if (i >= len) {
        char buf[96];
        snprintf(buf, sizeof buf,
                 "array index %zu out of bounds (len=%zu)", i, len);
        return value_make_error(vm, buf);
    }

    /* 类型检查：非元素类型尝试隐式转换 */
    value_t *v = val;
    if (value_type(val) != et) {
        v = value_implicit_cast(vm, val, et);
        if (value_is_error(vm, v)) return v;
    }

    /* 写块内偏移：先释放旧元素资源，再深拷贝新值到业务内存。
       self 是借用（多维链）时 data 已指向块内偏移，写直达原数组。 */
    uint8_t *slot = (uint8_t *)value_data(self) + i * et->size;
    value_dispose_raw(vm, slot, et);
    array_blit_value(vm, slot, v, et);
    return self;
}

/* ---- dispose: 释放块内元素资源（data 块本身由 value_dispose 释放） ---- */

static void array_dispose(vm_t *vm, value_t *v) {
    /* 借用引用不拥有 data（指向父数组内部），跳过；由 value_dispose 统一拦截 */
    if (value_is_borrowed(v)) return;
    value_dispose_raw(vm, value_data(v), value_type(v));
}

/* ---- clone: 分配新块深拷贝全部元素；借用 → materialize（同路径） ---- */

static value_t *array_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));

    /* 借用/普通数组的 type 均为数组类型；借用 v 的 data 指向块内偏移，
       按其类型深拷贝该块即 materialize（独立 is_own=true 拷贝）。 */
    const type_t *t = value_type(v);
    void *block = value_alloc_data(vm->alloc, t);
    value_blit_raw(vm, block, value_data(v), t);
    return value_make(vm, t, block);
}

/* ---- assign: 释放 dst 块内资源，深拷贝 src 块（src 可为借用） ---- */

static value_t *array_assign(vm_t *vm, value_t *dst, value_t *src) {
    /* safe_cast 语义（与 struct_assign / tuple_assign 同款）：类型不同先向
       左值类型 implicit_cast——布局兼容的 array↔tuple 跨类型赋值经
       array_implicit_cast 转换通过；不兼容 → cast 返回 error，直接传播。 */
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

/* ---- 类型转换：array↔tuple 布局兼容互转（m2-design §3）+ 同 array 身份 ---- */

static value_t *array_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (!target) return value_make_error(vm, "array: missing cast target");
    const type_t *src = value_type(v);
    /* 同 array 实例（身份拷贝） */
    if (target->kind == TYPE_KIND_ARRAY && target == src) {
        if (value_is_shadow(v)) return value_make_shadow(vm, target);
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }
    /* 布局兼容的 array↔tuple 互转（双向） */
    if (tuple_array_layout_compatible(vm, target, src) ||
        tuple_array_layout_compatible(vm, src, target)) {
        if (value_is_shadow(v)) return value_make_shadow(vm, target);
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }
    return value_make_error(vm, "array: implicit cast only within layout-compatible "
                               "array/tuple");
}

static value_t *array_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    return array_implicit_cast(vm, v, target);
}

const vtable_t VTABLE_ARRAY = {
    .get_index = array_get_index,
    .set_index = array_set_index,
    .length    = array_length,
    .dispose   = array_dispose,
    .clone     = array_clone,
    .assign    = array_assign,
    .implicit_cast = array_implicit_cast,
    .explicit_cast = array_explicit_cast,
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

    /* 分配连续元素块（at->size = count * elem_type->size，seal 时已计算） */
    void *block = value_alloc_data(vm->alloc, at);
    const size_t es = elem_type->size;

    for (size_t i = 0; i < count; i++) {
        value_t *e = elems[i];
        if (value_is_error(vm, e)) return e;
        /* 自动 0 填充占位（undefined）：跳过 blit——分配块已由
           value_alloc_data 清零，缺失元素保持类型零值（数值 0 /
           bool false / func NULL=nil / str NULL）。 */
        if (value_is_undefined(vm, e)) continue;
        /* 类型检查：非元素类型尝试隐式转换 */
        if (value_type(e) != elem_type) {
            value_t *casted = value_implicit_cast(vm, e, elem_type);
            if (value_is_error(vm, casted)) return casted;
            e = casted;
        }
        /* 深拷贝元素数据到块内偏移（标量 memcpy；嵌套数组/字符串递归） */
        array_blit_value(vm, (uint8_t *)block + i * es, e, elem_type);
    }
    return value_make(vm, at, block);
}

/* ================================================================ */
/* 运行期数组 value 只读访问（供调试/格式化遍历，如 printf %v）        */
/* ================================================================ */

size_t value_array_count(const value_t *v) {
    if (!v || value_kind(v) != TYPE_KIND_ARRAY) return 0;
    /* 借用 v 的 type 也是数组类型，count 从类型取 */
    return array_type_len(value_type(v));
}

value_t *value_array_at(vm_t *vm, const value_t *v, size_t i) {
    if (!v || value_kind(v) != TYPE_KIND_ARRAY) return NULL;
    const type_t *t = value_type(v);
    const type_t *et = array_type_elem(t);
    size_t len = array_type_len(t);
    if (i >= len) return NULL;
    /* 返回借用引用（data 指向块内偏移，业务内存），供递归遍历 */
    return value_make_borrowed(vm, et,
                               (uint8_t *)value_data(v) + i * et->size);
}
