#include "vm/value.h"
#include "vm/type_ownership.h"
#include "vm/vm.h"
#include "vm/type_error.h"
#include "vm/type_interrupt.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"   /* ptr_clear_owned_recursive（fatal 接管转换用） */
#include "vm/value_compound_assign.h" /* compound_assign_transfer */
#include "vm/value_owncopy.h"         /* value_own_copy_into */
#include "vm/value_deepcopy.h"        /* value_alloc_data_own */
#include "vm/str_pool.h"
#include "core/panic.h"

#include <string.h>
#include <stdio.h>

/* ---- value_t 结构体定义（仅此文件可见） ---- */

struct value_t {
    const type_t     *type;
    void             *data;
    bool              is_shadow;
    /* 运行期视图位：data 指向父值 data 块内部（字段/元素借用视图）。
       它**不是所有权状态**——只回答"dispose 时是否释放 data 块"。
       所有权的静态部分（own/ref/fatal）已经编码在 value->type 里。 */
    bool              borrowed;
    const scope_frame_t *frame; /* 借用：所属作用域的父子关系节点（&scope->frame） */
};

/* ---- 内部分配 class_t（value_t 堆分配用） ---- */

static class_t g_value_class = {
    .name       = "clux.vm.value",
    .size       = sizeof(value_t),
    .clone_fn   = NULL,
    .move_fn    = NULL,
    .dispose_fn = NULL,
};

/** 内部 class_t（value data 堆分配用） */
static class_t g_value_data_class = {
    .name       = "clux.vm.value_data",
    .size       = 1,  /* 实际大小由 allocator_new 的 count 参数控制 */
    .clone_fn   = NULL,
    .move_fn    = NULL,
    .dispose_fn = NULL,
};

value_t *value_alloc(allocator_t *alloc) {
    value_t *v = (value_t *)allocator_new(alloc, &g_value_class, 1);
    if (!v) panic("vm: out of memory allocating value");
    memset(v, 0, sizeof(value_t));
    return v;
}

void *value_alloc_data(allocator_t *alloc, const type_t *type) {
    if (!type || type->size == 0) return NULL;
    void *data = allocator_new(alloc, &g_value_data_class, type->size);
    if (!data) panic("vm: out of memory allocating value data");
    memset(data, 0, type->size);
    return data;
}

void *value_alloc_data_copy(allocator_t *alloc, const type_t *type, const void *src) {
    if (!type || type->size == 0) return NULL;
    void *data = allocator_new(alloc, &g_value_data_class, type->size);
    if (!data) panic("vm: out of memory allocating value data");
    memcpy(data, src, type->size);
    return data;
}

/* 按字节数分配原始内存（NEW 指令用）：清零 + value_data 类（与值数据块同
   生命周期规则；FREE / 作用域回收都用 allocator_free 按指针释放）。 */
void *value_alloc_raw(allocator_t *alloc, size_t size) {
    if (!alloc || size == 0) return NULL;
    void *data = allocator_new(alloc, &g_value_data_class, size);
    if (!data) panic("vm: out of memory allocating raw block");
    memset(data, 0, size);
    return data;
}

/* ---- 访问器 ---- */

const type_t *value_type(const value_t *v) {
    return v ? v->type : NULL;
}

void *value_data(const value_t *v) {
    return v ? v->data : NULL;
}

const scope_frame_t *value_frame(const value_t *v) {
    return v ? v->frame : NULL;
}

scope_t *value_scope(const value_t *v) {
    return (v && v->frame) ? scope_from_frame(v->frame) : NULL;
}

ownership_t value_type_ownership(const value_t *v) {
    return v ? type_ownership_of(v->type) : OWN_NONE;
}

void value_set_type_ownership(vm_t *vm, value_t *v, ownership_t own) {
    if (!v || !v->type) return;
    const type_t *core = type_strip_ownership(v->type);
    v->type = type_ownership_apply(vm, own, core);
}

bool value_is_void(const value_t *v) {
    return !v || v->type == NULL;
}

/* ---- 类型查询（type is value：kind 分类 + vtable 分派） ---- */

type_kind_t value_kind(const value_t *v) {
    return (v && v->type) ? v->type->kind : TYPE_KIND_VOID;
}

bool value_is_type(const value_t *v, type_kind_t kind) {
    if (!v || !v->type) return kind == TYPE_KIND_VOID;
    /* `own T` / `ref T` / `fatal T` 与 T 的结构/布局相同：按裸类型判定。 */
    return type_kind_of(v->type) == kind;
}

bool value_has_const(const value_t *v) {
    if (!v || !v->type) return false;
    return type_has_const(v->type);
}

/* ---- 构造器 ---- */

value_t *value_alloc_shell(allocator_t *alloc, const type_t *type, void *data) {
    value_t *v = value_alloc(alloc);
    v->type = type;
    v->data = data;
    v->borrowed = false;
    return v;
}

value_t *value_make(vm_t *vm, const type_t *type, void *data) {
    value_t *v = value_alloc_shell(vm->alloc, type, data);
    v->frame = vm->current_scope ? &vm->current_scope->frame : NULL;    scope_track(vm, vm->current_scope, v);
    return v;
}

/* 借用引用：data 指向父值 data 块内的业务内存偏移（C 语义 &arr[i]），
   borrowed=true，dispose 跳过 data 释放。value 是引擎内部内存对象，借用引用的
   data 指向的是业务数据而非引擎对象。
   借用值只匿名存活于表达式链中，绑定（DEFINE/STORE/RET/clone）时经
   value_clone materialize 成独立深拷贝。frame 指向当前求值作用域（借用
   引用匿名存活于当前表达式链，不随 data 归属父值）。 */
value_t *value_make_borrowed(vm_t *vm, const type_t *type, void *data) {
    value_t *v = value_alloc(vm->alloc);
    v->type = type;
    v->data = data;     /* 业务内存：父值 data 块内偏移 */
    v->borrowed = true;
    v->frame = vm->current_scope ? &vm->current_scope->frame : NULL;
    scope_track(vm, vm->current_scope, v);
    return v;
}

bool value_is_borrowed(const value_t *v) {
    return v && !v->is_shadow && v->borrowed;
}



/* ---- shadow value ---- */

value_t *value_make_shadow(vm_t *vm, const type_t *type) {
    value_t *v = value_alloc(vm->alloc);
    v->type = type;
    v->data = NULL;
    v->is_shadow = true;
    v->borrowed = false;
    v->frame = vm->current_scope ? &vm->current_scope->frame : NULL;
    scope_track(vm, vm->current_scope, v);
    return v;
}

bool value_is_shadow(const value_t *v) {
    return v && v->is_shadow;
}

/* ---- error 工具 ---- */

bool value_is_error(vm_t *vm, const value_t *v) {
    return v && vm && v->type == vm->type_error;
}

value_t *value_make_error(vm_t *vm, const char *message) {
    return value_make_error_loc(vm, message, NULL);
}

value_t *value_make_error_loc(vm_t *vm, const char *message, const char *location) {
    error_data_t ed;
    /* 字符串走 vm 字符串池 intern（data 全平凡：error_data_t 存池内指针） */
    ed.message  = message  ? vm_str_intern_cstr(vm, message)  : NULL;
    ed.location = location ? vm_str_intern_cstr(vm, location) : NULL;
    void *data = value_alloc_data_copy(vm->alloc, vm->type_error, &ed);
    return value_make(vm, vm->type_error, data);
}

/* ---- undefined 工具 ---- */

value_t *value_make_undefined(vm_t *vm) {
    return value_make(vm, vm->type_void, NULL);
}

bool value_is_undefined(vm_t *vm, const value_t *v) {
    return v && vm && v->type == vm->type_void;
}

/* ---- interrupt 工具 ---- */

bool value_is_interrupt(vm_t *vm, const value_t *v) {
    return v && vm && v->type == vm->type_interrupt;
}

value_t *value_make_interrupt(vm_t *vm, interrupt_kind_t kind) {
    interrupt_data_t id;
    id.kind = kind;
    void *data = value_alloc_data_copy(vm->alloc, vm->type_interrupt, &id);
    return value_make(vm, vm->type_interrupt, data);
}

interrupt_kind_t value_interrupt_kind(vm_t *vm, const value_t *v) {
    if (!value_is_interrupt(vm, v)) return INTERRUPT_RETURN;
    return ((interrupt_data_t *)value_data(v))->kind;
}

/* ---- 类型运算 ---- */

/*
 * type value 的 == / extends 代理：type value 的 data 存指向 type_t 的
 * 指针。比较分派到类型自身的 vtable 槽位（type_equal / type_extends，
 * 鸭子类型判断）。NULL 槽位 → 默认指针比较（type.c 的默认入口处理）。
 * 返回 bool value（shadow 输入 → shadow 输出，与其余 vtable 一致）。
 */

const value_t *value_seal(vm_t *vm, value_t *src) {
    if (!vm || !src) return src;
    /* 仅 type value 可密封；非 type value 原样返回 */
    if (value_type(src) != vm->type_type) return src;

    const type_t *t = value_as(src, const type_t *);
    if (!t || !t->vtable || !t->vtable->type_seal)
        return src;  /* 无开放构造阶段（如内置基础类型已 sealed） */

    const type_t *sealed = t->vtable->type_seal(vm, t);
    if (!sealed) return src;  /* type_seal 失败（缺字段等），原样返回 */

    /* 统一兜底置位：密封后 sealed 标志必须为 true（各 type_seal 实现内部
     * 已置位，此处兜底防御，覆盖未来新类型遗漏；幂等无害） */
    if (!((type_t *)sealed)->sealed) ((type_t *)sealed)->sealed = true;

    if (sealed != t) {
        /* 去重：当前 t 已被 vtable 手工回收，重定向所有引用 t 的 type value
         * 到缓存 sealed（先更新自身，再扫描操作数栈以防多处引用）。 */
        *(const type_t **)value_data(src) = sealed;
        size_t sp = vec_len(vm->stack);
        for (size_t k = 0; k < sp; k++) {
            value_t *sv = (value_t *)vec_get(vm->stack, k);
            if (sv && value_type(sv) == vm->type_type &&
                value_as(sv, const type_t *) == t) {
                *(const type_t **)value_data(sv) = sealed;
            }
        }
    }
    return src;
}

value_t *value_type_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_error(vm, a)) return a;
    if (value_is_error(vm, b)) return b;
    if (value_type(a) != vm->type_type || value_type(b) != vm->type_type)
        return value_make_error(vm, "==: type value required");
    const type_t *ta = value_as(a, const type_t *);
    const type_t *tb = value_as(b, const type_t *);
    bool r = type_equal(vm, ta, tb);
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    *(bool *)data = r;
    return value_make(vm, vm->type_bool, data);
}

value_t *value_type_extends(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_error(vm, a)) return a;
    if (value_is_error(vm, b)) return b;
    if (value_type(a) != vm->type_type || value_type(b) != vm->type_type)
        return value_make_error(vm, "extends: type value required");
    const type_t *ta = value_as(a, const type_t *);
    const type_t *tb = value_as(b, const type_t *);
    bool r = type_extends(vm, ta, tb);
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    *(bool *)data = r;
    return value_make(vm, vm->type_bool, data);
}

/* extends 运算符入口：分派到 a 的 vtable（type value 的 VTABLE_TYPE.extends） */
value_t *value_extends(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_error(vm, a)) return a;
    if (value_is_error(vm, b)) return b;
    if (!a->type || !a->type->vtable || !a->type->vtable->extends)
        return value_make_error(vm, "type does not support operator 'extends'");
    return a->type->vtable->extends(vm, a, b);
}

/* ---- const/volatile 解包原语 ---- */

const type_t *value_swap_type(value_t *v, const type_t *new_type) {
    if (!v) return NULL;
    const type_t *old = v->type;
    v->type = new_type;
    return old;
}

void value_restore_type(value_t *v, const type_t *old_type) {
    if (v) v->type = old_type;
}

/* ---- 运算分派 ---- */

/*
 * value_xxx 薄封装：error 短路 + NULL vtable 检查 → 分派到 vtable
 * 类型协商、safe_cast 由各 vtable 函数通过 VTABLE_BINARY 自行处理
 *
 * 未初始化（TDZ）检查由 sema 确定性赋值分析在编译期完成，VM 值层不感知。
 */
#define DISPATCH(vm, a, b, op_name, op_sym)                                    \
    do {                                                                       \
        if (value_is_error((vm), (a))) return (a);                            \
        if (value_is_error((vm), (b))) return (b);                            \
        if (!(a)->type || !(a)->type->vtable || !(a)->type->vtable->op_name)  \
            return value_make_error(vm,                                        \
                "type does not support operator " op_sym);                     \
        return (a)->type->vtable->op_name(vm, a, b);                           \
    } while (0)

#define DISPATCH_UNARY(vm, a, op_name, op_sym)                                 \
    do {                                                                       \
        if (value_is_error((vm), (a))) return (a);                             \
        if (!(a)->type || !(a)->type->vtable || !(a)->type->vtable->op_name) { \
            return value_make_error(vm,                                        \
                "type does not support operator " op_sym);                     \
        }                                                                      \
        return (a)->type->vtable->op_name(vm, a);                              \
    } while (0)

value_t *value_add(vm_t *vm, value_t *a, value_t *b)  { DISPATCH(vm, a, b, add,  "+"); }
value_t *value_sub(vm_t *vm, value_t *a, value_t *b)  { DISPATCH(vm, a, b, sub,  "-"); }
value_t *value_mul(vm_t *vm, value_t *a, value_t *b)  { DISPATCH(vm, a, b, mul,  "*"); }
value_t *value_div(vm_t *vm, value_t *a, value_t *b)  { DISPATCH(vm, a, b, div,  "/"); }
value_t *value_mod(vm_t *vm, value_t *a, value_t *b)  { DISPATCH(vm, a, b, mod,  "%"); }
value_t *value_neg(vm_t *vm, value_t *a)              { DISPATCH_UNARY(vm, a, neg, "-"); }

value_t *value_eq(vm_t *vm, value_t *a, value_t *b)   { DISPATCH(vm, a, b, eq,   "=="); }
value_t *value_ne(vm_t *vm, value_t *a, value_t *b)   { DISPATCH(vm, a, b, ne,   "!="); }
value_t *value_lt(vm_t *vm, value_t *a, value_t *b)   { DISPATCH(vm, a, b, lt,   "<"); }
value_t *value_le(vm_t *vm, value_t *a, value_t *b)   { DISPATCH(vm, a, b, le,   "<="); }
value_t *value_gt(vm_t *vm, value_t *a, value_t *b)   { DISPATCH(vm, a, b, gt,   ">"); }
value_t *value_ge(vm_t *vm, value_t *a, value_t *b)   { DISPATCH(vm, a, b, ge,   ">="); }

value_t *value_band(vm_t *vm, value_t *a, value_t *b) { DISPATCH(vm, a, b, band, "&"); }
value_t *value_bor(vm_t *vm, value_t *a, value_t *b)  { DISPATCH(vm, a, b, bor,  "|"); }
value_t *value_bxor(vm_t *vm, value_t *a, value_t *b) { DISPATCH(vm, a, b, bxor, "^"); }
value_t *value_bnot(vm_t *vm, value_t *a)             { DISPATCH_UNARY(vm, a, bnot, "~"); }
value_t *value_shl(vm_t *vm, value_t *a, value_t *b)  { DISPATCH(vm, a, b, shl,  "<<"); }
value_t *value_shr(vm_t *vm, value_t *a, value_t *b)  { DISPATCH(vm, a, b, shr,  ">>"); }

value_t *value_lnot(vm_t *vm, value_t *a)             { DISPATCH_UNARY(vm, a, lnot, "!"); }

value_t *value_call(vm_t *vm, value_t *callee, value_t **args, size_t argc) {
    if (value_is_error(vm, callee)) return callee;
    /* 检查参数中是否有 error（shadow 分派前） */
    for (size_t i = 0; i < argc; i++) {
        if (value_is_error(vm, args[i])) return args[i];
    }
    if (!callee->type || !callee->type->vtable || !callee->type->vtable->call) {
        return value_make_error(vm, "value is not callable");
    }
    return callee->type->vtable->call(vm, callee, args, argc);
}

/* ---- 索引 / 容器运算 ---- */

value_t *value_get_index(vm_t *vm, value_t *self, value_t *index) {
    if (value_is_error(vm, self)) return self;
    if (value_is_error(vm, index)) return index;
    if (!self->type || !self->type->vtable || !self->type->vtable->get_index) {
        return value_make_error(vm, "type does not support indexing");
    }
    return self->type->vtable->get_index(vm, self, index);
}

value_t *value_set_index(vm_t *vm, value_t *self, value_t *index, value_t *val) {
    if (value_is_error(vm, self)) return self;
    if (value_is_error(vm, index)) return index;
    if (value_is_error(vm, val)) return val;
    if (!self->type || !self->type->vtable || !self->type->vtable->set_index) {
        return value_make_error(vm, "type does not support indexing");
    }
    return self->type->vtable->set_index(vm, self, index, val);
}

value_t *value_length(vm_t *vm, value_t *self) {
    if (value_is_error(vm, self)) return self;
    if (!self->type || !self->type->vtable || !self->type->vtable->length) {
        return value_make_error(vm, "type does not support length");
    }
    return self->type->vtable->length(vm, self);
}

/* ---- 生命周期 ---- */

void value_dispose(vm_t *vm, value_t *v) {
    if (!v) return;
    if (v->type) {
        /* shadow 值无 data，跳过 dispose 和 data 释放 */
        if (!v->is_shadow) {
            /* 借用引用（OWN_REF）：data 指向父值内部，不拥有，跳过 dispose 与释放 */
            if (!v->borrowed) {
                if (v->type->vtable && v->type->vtable->dispose) {
                    v->type->vtable->dispose(vm, v);
                }
                /* 释放 data 块 */
                if (v->data) {
                    allocator_free(vm->alloc, (void **)&v->data);
                }
            }
        }
        v->type = NULL;
    }
}

/* fatal 变体 → 同布局非 fatal 类型的接管转换（见 value.h） */
value_t *value_take_fatal_into(vm_t *vm, value_t *v, const type_t *target,
                               bool (*cfg)(vm_t *, const type_t *,
                                           const type_t *)) {
    if (!vm || !v || !target || !cfg) return NULL;
    const type_t *src = v->type;
    if (!src || value_type_ownership(v) != OWN_FATAL) return NULL;
    if (src->kind != target->kind) return NULL;   /* 只认同 kind 的 fatal 变体 */
    if (src->size != target->size) return NULL;   /* 布局必须一致才能重贴标签 */
    if (!cfg(vm, target, src)) return NULL;       /* 各类型的布局兼容判定 */
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    void *data = value_alloc_data_copy(vm->alloc, target, v->data);
    /* 源清空其 own/fatal 指针：所有权随构造转移给新值（防双释放）。
       源的 data 块仍由作用域在退出时释放（裸字节块，无副作用）。 */
    ptr_clear_owned_recursive(vm, (type_t *)src, v->data);
    return value_make(vm, target, data);
}

/* 隐式转换/身份拷贝的 data 分配：持有资源才对象图克隆（见 vm/value_deepcopy.h）。
   所有权纯编译期——复合类型字段禁止携带指针/切片，只有指针/切片类型持有资源，
   但指针/切片的深拷贝由 CLONE 指令（ptr_clone_block）负责，不走此路径。
   此函数只处理复合类型/标量的浅拷贝。 */
void *value_alloc_data_own(vm_t *vm, const type_t *t, const void *src_data) {
    if (!vm || !t || !src_data) return NULL;
    return value_alloc_data_copy(vm->alloc, t, src_data);
}

/* 同布局重贴标签：把 v 的字节按 target 解释（**转移**语义）。见
   vm/value_deepcopy.h 的 value_relabel_into。 */
value_t *value_relabel_into(vm_t *vm, value_t *v, const type_t *target) {
    if (!vm || !v || !target || !value_type(v)) return NULL;
    const type_t *src_t = value_type(v);
    if (src_t == target) return v;
    /* 只对"逐字节同布局"成立（同 kind 且同大小），否则拒绝——把字节按错误布局
       解释会读出垃圾。 */
    if (src_t->kind != target->kind || src_t->size != target->size) return NULL;
    if (value_is_shadow(v)) {
        v->type = target;
        return v;
    }
    /* **就地**换 type_t：源已经是转移产物（value_move 刚分配好的独立块），
       直接改类型标记即可。此处**不能**再分配一份 data 再 memcpy——那会留下
       一块没人释放的旧块（实测 leak 报告），而且多一次无谓的 O(size) 拷贝。 */
    v->type = target;
    return v;
}

/* 聚合块写入：持有资源的字段深拷贝（见 vm/value_owncopy.h）。
   所有权纯编译期——复合类型字段禁止携带指针/切片，ft 永远不含 own 资源，
   直接 memcpy 即可。保留 ptr_clone_block 路径供未来扩展。 */
void *value_own_copy_into(vm_t *vm, void *dst, const value_t *src,
                          const type_t *ft) {
    uint8_t *p = (uint8_t *)dst;
    if (!dst || !src || !ft || !value_data(src)) return p;
    memcpy(p, value_data(src), ft->size);
    /* RC 系（share/weak）是**真实类型**：拷贝进新槽位 = 引用计数 +1。
       （own 指针是转移语义，源会被清空，不在此列。） */
    ptr_rc_inc_recursive(vm, ft, p);
    return p;
}

/* 复合类型就地赋值的共享实现（见 vm/value_compound_assign.h） */
value_t *compound_assign_transfer(vm_t *vm, value_t *dst, value_t *src) {
    if (!vm || !dst || !src) return dst;
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    const type_t *t = value_type(dst);
    if (!t || !value_data(dst) || !value_data(src)) return dst;
    /* 1. 释放左值旧值拥有的堆块：整块 memcpy 会丢掉旧字段的裸指针，不先释放
          就再也找不到那些块 → 泄漏。 */
    ptr_free_owned_recursive(vm, t, value_data(dst));
    /* 2. 整块 memcpy（data 全平凡：字段即裸字节） */
    memcpy(value_data(dst), value_data(src), t->size);
    /* 3. 清空源的 own/fatal 指针：所有权随 memcpy 转移进 dst；不清空则源与
          dst 共享同一堆块 → 两侧析构各释放一次 = double-free。 */
    ptr_clear_owned_recursive(vm, (type_t *)value_type(src), value_data(src));
    return dst;
}

value_t *value_clone(vm_t *vm, value_t *v) {
    if (!v || !v->type) return v;

    /* shadow 值 clone 返回新的 shadow（不分配 data） */
    if (v->is_shadow) {
        return value_make_shadow(vm, v->type);
    }

    /* 通过 vtable clone，NULL clone 槽 = 不支持 */
    if (v->type->vtable && v->type->vtable->clone) {
        return v->type->vtable->clone(vm, v);
    }
    return value_make_error(vm, "type does not support clone");
}

value_t *value_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_is_error(vm, src)) return src;
    if (!dst || !dst->type) return value_make_error(vm, "assign: cannot assign to void");
    if (value_is_error(vm, dst)) return value_make_error(vm, "assign: cannot assign to error");
    if (!dst->type->vtable || !dst->type->vtable->assign) {
        return value_make_error(vm, "assign: type does not support assignment");
    }
    return dst->type->vtable->assign(vm, dst, src);
}

/* ---- 类型转换 ---- */

value_t *value_implicit_qualify(vm_t *vm, value_t *v, const type_t *target) {
    if (value_is_error(vm, v)) return v;
    if (!v || !v->type || !target) {
        return value_make_error(vm, "cannot implicitly qualify void");
    }
    /* 限定符 sub 链剥到源类型（如 i32 → volatile i32、
       i32 → const volatile i32），且源无限定符（源本身带限定符时由
       type_const.c/type_volatile.c 的 implicit_cast 处理脱限定符/再限定） */
    const type_t *q = target;
    while (q && (type_is_const(q) || type_is_volatile(q)))
        q = type_qualifier_sub(q);
    if (!q || q != v->type)
        return value_make_error(vm, "implicit qualify: target is not a qualifier of source");
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
    return value_make(vm, target, data);
}

/* 所有权修饰之间的隐式转换（布局相同，纯重贴标签）：
 *   fatal T → own T   接管将亡值（"消费"）
 *   fatal T → ref T   以借用视图消费（调用方立即失去所有权）
 *   own T   → ref T   借出（源仍拥有）
 *   own/fatal T → T   只读视图（旧模型的等价落点）
 * 反向（ref → own 等）不允许：那会凭空产生所有权。
 * 运行期类型已经剥离所有权，所以本分支只在编译期 shadow 路径生效。 */
static bool ownership_cast_ok(ownership_t from, ownership_t to) {
    if (from == to) return true;
    if (from == OWN_FATAL)
        return to == OWN_OWN || to == OWN_REF || to == OWN_NONE;
    if (from == OWN_OWN) return to == OWN_REF || to == OWN_NONE;
    if (from == OWN_REF) return to == OWN_NONE;
    /* 裸类型 → 所有权修饰：运行期构造/写入带修饰槽位时必须允许（所有权是
       编译期契约，sema 已校验来源；这里只做同布局重贴标签）。 */
    if (from == OWN_NONE) return true;
    return false;
}

value_t *value_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (value_is_error(vm, v)) return v;
    if (!v || !v->type) {
        return value_make_error(vm, "cannot implicitly cast void");
    }
    if (v->type != target) {
        ownership_t from = type_ownership_of(v->type);
        ownership_t to = type_ownership_of(target);
        /* 裸类型相同（布局等价）且所有权方向允许：纯重贴标签。
           即使两侧都是 OWN（例如槽位类型与表达式类型来自不同 intern 实例），
           也直接落到目标类型——所有权是编译期契约，运行期只认布局。 */
        if (type_strip_ownership(v->type) == type_strip_ownership(target) &&
            ownership_cast_ok(from, to)) {
            if (value_is_shadow(v)) return value_make_shadow(vm, target);
            /* 运行期：两者裸类型相同，重贴标签即可（所有权是编译期概念）。 */
            v->type = target;
            return v;
        }
    }
    if (v->type == target) {
        /* 类型相同：把值 materialize 到当前作用域。**必须用
           value_alloc_data_own**（持有资源则对象图克隆）——此前的 value_clone
           对 array/tuple/union/option 是**浅拷贝**，会让副本与源共享内嵌 own
           堆块 → 两侧析构各释放一次 = double-free（实测 access violation）。
           注意不能只依赖各类型的 vtable implicit_cast：本分支在派发**之前**
           短路返回，各族自己那条"身份拷贝"分支根本不会被执行到。 */
        if (value_is_shadow(v)) return value_make_shadow(vm, target);
        if (!value_data(v)) return v;
        void *data = value_alloc_data_own(vm, target, value_data(v));
        if (!data) return value_make_error(vm, "cast: out of memory");
        return value_make(vm, target, data);
    }
    /* T → ?T 隐式提升（some，D1）：目标为 option 且源不是 option 时提升。
       内层类型完全相同是最常见形态；不同时由 value_lift_option 先按内层做一次
       隐式转换（例：`ref *Node → ?ref *const Node`：提升的同时加限定符）。
       在公共入口特判——所有标量 vtable 无需感知 ?T（value_lift_option 深拷贝
       T 值到 value 字段 + 置 ok=true）。 */
    if (target && target->kind == TYPE_KIND_OPTION &&
        v->type->kind != TYPE_KIND_OPTION) {
        return value_lift_option(vm, v, target);
    }
    if (v->type->vtable && v->type->vtable->implicit_cast) {
        return v->type->vtable->implicit_cast(vm, v, target);
    }
    return value_make_error(vm, "type does not support implicit cast");
}

value_t *value_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (value_is_error(vm, v)) return v;
    if (!v || !v->type) {
        return value_make_error(vm, "cannot explicitly cast void");
    }
    if (v->type == target) {
        return value_clone(vm, v);
    }
    if (v->type->vtable && v->type->vtable->explicit_cast) {
        return v->type->vtable->explicit_cast(vm, v, target);
    }
    return value_make_error(vm, "type does not support explicit cast");
}

/* 所有权转移 = 纯 memcpy；堆释放由 sema 生成的 FREE 负责。 */
value_t *value_move(vm_t *vm, value_t *v) {
    if (!v) return v;
    if (value_is_shadow(v)) return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    if (!t || !value_data(v)) return v;
    void *data = value_alloc_data(vm->alloc, t);
    if (!data) return value_make_error(vm, "out of memory moving value");
    memcpy(data, value_data(v), t->size);
    return value_make(vm, t, data);
}
