#include "vm/type_array.h"
#include "vm/type.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/value.h"
#include "vm/value_compound_assign.h" /* compound_assign_transfer（赋值共享实现） */
#include "vm/value_deepcopy.h"        /* value_alloc_data_own（转换期深拷贝） */
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
 * 借用引用（ownership=OWN_REF）的 data 直接指向块内偏移（业务内存）：
 *   arr[i]  →  data = arr.data + i * elem_type->size，type = elem_type
 *   m[1][0] →  内层借用 data = m.data + 1*sizeof([..]T)，再偏移 0
 * 读写直达业务内存，与 C 的 &arr[i] 语义一致（M2 设计 §2）。
 *
 * 生命周期（用户确认）：借用值只匿名存活于表达式链；绑定变量 / 返回值 /
 * 赋值（DEFINE / STORE / RET / clone）时经 value_clone materialize 成独立
 * 深拷贝（ownership=OWN_OWN），故无悬垂引用。借用值 dispose 跳过（data 指向父值
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

/* SET_FIELD_OWNERSHIP 运行期用（array 分支）：设元素槽位的所有权（seal 前）。 */
void array_type_set_elem_ownership(vm_t *vm, const type_t *t, ownership_t own) {
    (void)vm;
    if (!t || t->kind != TYPE_KIND_ARRAY) return;
    array_type_t *at = (array_type_t *)t;
    if (type_is_sealed(t)) return;
    at->elem_ownership = own;
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

    /* 去重 intern（按 elem_type + length + 元素槽位所有权）：命中已有 sealed
       类型则复用并手工回收本开放类型。`[N]own *T` 与 `[N]ref *T` 是不同类型
       （销毁语义不同），所有权必须进键。 */
    size_t n = vec_len(vm->array_types);
    for (size_t i = 0; i < n; i++) {
        const array_type_t *other = (const array_type_t *)vec_get(vm->array_types, i);
        if (other && other != at && type_is_sealed(&other->base) &&
            other->elem_type == at->elem_type && other->length == at->length &&
            other->elem_ownership == at->elem_ownership) {
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

    /* 重建类型名（含 length：固定长度 [elem; N]，未定长 [elem]）。所有权
       已迁移到值层，不再有 fatal 变体前缀。 */
    char *base_name = array_type_name(vm->alloc, at->elem_type, at->length);
    if (!base_name) panic("vm: out of memory allocating array type name");
    at->base.name = (strslice_t){ base_name, strlen(base_name) };

    vec_push(vm->array_types, vm->alloc, at);  /* 密封后入池（去重 intern） */
    return &at->base;
}

const type_t *type_array_intern(vm_t *vm, const type_t *elem_type, size_t count) {
    return type_array_intern_own(vm, elem_type, count, OWN_NONE);
}

/* 带**元素槽位所有权**的一次性构造（`[N]own *T` 等，见 type_array.h）。 */
const type_t *type_array_intern_own(vm_t *vm, const type_t *elem_type,
                                    size_t count, ownership_t elem_ownership) {
    /* 一次性快捷：alloc（不压栈）+ set_elem + set_count + seal。
     * 与 array_type_push 的区别：不把 type value 压入操作数栈——本函数供
     * sema resolve_type_expr 与 value_make_array 等非字节码路径使用，
     * 压栈只会污染操作数栈（嵌套构造场景曾因此栈布局错位）。 */
    array_type_t *at = array_type_create_open(vm);
    const type_t *t = &at->base;
    array_type_set_elem(vm, t, elem_type);
    array_type_set_elem_ownership(vm, t, elem_ownership);
    array_type_set_count(vm, t, count);
    return array_type_seal(vm, t);
}

/* 兼容包装：旧签名（参数曾表示"数组类型本身的 ownership"，已废弃）——
   现在等价于元素槽位所有权。 */
const type_t *type_array_intern_with_ownership(vm_t *vm, const type_t *elem_type,
                                               size_t count,
                                               ownership_t ownership) {
    return type_array_intern_own(vm, elem_type, count, ownership);
}

/* ================================================================ */
/* vtable 实现                                                      */
/* ================================================================ */

/* ================================================================ */
/* vtable 实现                                                      */
/* ================================================================ */

/* ---- length: 返回 usize 元素个数（C 的 size_t） ---- */

static value_t *array_length(vm_t *vm, value_t *self) {
    /* 长度 = usize（C 的 size_t，独立类型，宽度随架构）：u64 在 32 位平台
       上宽度是错的。 */
    const type_t *ut = vm_type_usize(vm);
    if (value_is_shadow(self))
        return value_make_shadow(vm, ut);
    /* 借用 self 的 type 也是数组类型（元素类型），length 从类型取 */
    size_t n = array_type_len(value_type(self));
    void *data = value_alloc_data(vm->alloc, ut);
    if (ut->size == 4) *(uint32_t *)data = (uint32_t)n;
    else               *(uint64_t *)data = (uint64_t)n;
    return value_make(vm, ut, data);
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

    /* 写块内偏移：data 全平凡（元素为 memcpy 可拷贝字节块），直接覆盖。
       self 是借用（多维链）时 data 已指向块内偏移，写直达原数组。 */
    uint8_t *slot = (uint8_t *)value_data(self) + i * et->size;
    memcpy(slot, value_data(v), et->size);
    return self;
}

/* ---- dispose: 释放块内元素资源（data 块本身由 value_dispose 释放） ---- */

static void array_dispose(vm_t *vm, value_t *v) {
    /* data 全平凡：元素为裸字节块（字符串归 vm 字符串池），无需释放。
       借用引用与 data 块本身由 value_dispose 统一处理。 */
    (void)vm; (void)v;
}

/* ---- clone: 分配新块深拷贝全部元素；借用 → materialize（同路径） ---- */

static value_t *array_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));

    /* 借用/普通数组的 type 均为数组类型；借用 v 的 data 指向块内偏移，
       按其类型深拷贝该块即 materialize（独立 ownership=OWN_OWN 拷贝）。
       **持有资源则对象图克隆**（value_alloc_data_own）：隐式拷贝路径同样承载
       含 own 元素的数组（闭包捕获、scope_define 的借用 materialize），浅拷贝
       会让副本与源共享内嵌堆块 → 双释放。 */
    const type_t *t = value_type(v);
    void *block = value_alloc_data_own(vm, t, value_data(v));
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

    /* 共享实现：释放左值旧值 → memcpy → 清空源（详见 value_compound_assign.h） */
    return compound_assign_transfer(vm, dst, src);
}

/* ---- 类型转换：array↔tuple 布局兼容互转（m2-design §3）+ 同 array 身份 ---- */

/* fatal 变体的布局兼容判定（供 value_take_fatal_into 回调）：
   同 kind（都是数组）且元素类型相同 ⟹ 长度与 size 相同（fatal 变体按同一
   elem+len 构造），故只需比元素类型。 */
static bool array_fatal_layout_ok(vm_t *vm, const type_t *target,
                                  const type_t *src) {
    (void)vm;
    return array_type_elem(target) == array_type_elem(src);
}

static value_t *array_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (!target) return value_make_error(vm, "array: missing cast target");
    /* fatal 变体 → 同布局的非 fatal 数组：接管转换（见 value_take_fatal_into） */
    value_t *taken = value_take_fatal_into(vm, v, target,
                                           array_fatal_layout_ok);
    if (taken) return taken;
    const type_t *src = value_type(v);
    /* 同 array 实例（身份拷贝；持有资源时对象图克隆，防与源共享内嵌 own） */
    if (target->kind == TYPE_KIND_ARRAY && target == src) {
        if (value_is_shadow(v)) return value_make_shadow(vm, target);
        void *data = value_alloc_data_own(vm, target, value_data(v));
        return value_make(vm, target, data);
    }
    /* 布局兼容的 array↔tuple 互转（双向） */
    if (tuple_array_layout_compatible(vm, target, src) ||
        tuple_array_layout_compatible(vm, src, target)) {
        if (value_is_shadow(v)) return value_make_shadow(vm, target);
        void *data = value_alloc_data_own(vm, target, value_data(v));
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
    return value_make_array_own(vm, elem_type, elems, count, OWN_NONE);
}

/* 带元素槽位所有权的数组值构造：类型用 (elem, count, elem_own) 去重，
   与声明处/hoist 构造的数组类型一致（`[N]own *T` 的元素按 own 销毁）。 */
value_t *value_make_array_own(vm_t *vm, const type_t *elem_type,
                              value_t **elems, size_t count,
                              ownership_t elem_ownership) {
    if (!vm || !elem_type) return NULL;

    const type_t *at = type_array_intern_own(vm, elem_type, count, elem_ownership);
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
        /* 类型检查：非元素类型尝试隐式转换；同类型直接用（身份，不 clone——
           value_implicit_cast 同类型短路 value_clone 浅拷贝，会与元素共享
           内嵌 own 堆块）。 */
        if (value_type(e) != elem_type) {
            value_t *casted = value_implicit_cast(vm, e, elem_type);
            if (value_is_error(vm, casted)) return casted;
            e = casted;
        }
        /* 平凡拷贝元素数据到块内偏移（data 全平凡：整段 memcpy） */
        memcpy((uint8_t *)block + i * es, value_data(e), es);
        /* 元素已复制进 block：所有权随 memcpy 转移——清空源对象图内所有
           own/fatal 指针（递归置 ptr=NULL，覆盖复合元素内嵌 own 指针：
           Cell 元素内部 own *i32 字段，不止直接 own/fatal 指针元素）。
           防作用域退出时源与 block 双释放；堆块归外层递归销毁；ref
           借用不受影响。 */
        ptr_clear_owned_recursive(vm, elem_type, value_data(e));
    }
    return value_make(vm, at, block);
}

/* ================================================================ */
/* 运行期数组 value 只读访问（供 value_dump 等调试遍历使用）            */
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
