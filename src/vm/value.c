#include "vm/value.h"
#include "vm/vm.h"
#include "vm/type_error.h"
#include "vm/type_interrupt.h"
#include "core/panic.h"
#include "core/string.h"

#include <string.h>

/* ---- value_t 结构体定义（仅此文件可见） ---- */

struct value_t {
    const type_t *type;
    void        *data;
    bool         is_shadow;
    bool         is_own;   /* true=拥有 data（dispose 释放）；false=借用引用（data 指向父值内部，跳过释放） */
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

/* ---- 访问器 ---- */

const type_t *value_type(const value_t *v) {
    return v ? v->type : NULL;
}

void *value_data(const value_t *v) {
    return v ? v->data : NULL;
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
    return v->type->kind == kind;
}

bool value_has_const(const value_t *v) {
    if (!v || !v->type) return false;
    return type_has_const(v->type);
}

/* ---- 构造器 ---- */

value_t *value_make_untracked(allocator_t *alloc, const type_t *type, void *data) {
    value_t *v = value_alloc(alloc);
    v->type = type;
    v->data = data;
    v->is_own = true;   /* 默认拥有 data */
    return v;
}

value_t *value_make(vm_t *vm, const type_t *type, void *data) {
    value_t *v = value_make_untracked(vm->alloc, type, data);
    scope_track(vm, vm->current_scope, v);
    return v;
}

/* 借用引用：data 指向父值 data 块内的业务内存偏移（C 语义 &arr[i]），
   is_own=false，dispose 跳过 data 释放。value 是引擎内部内存对象（含 type/
   is_own 等元数据），借用引用的 data 指向的是业务数据而非引擎对象。
   借用值只匿名存活于表达式链中，绑定（DEFINE/STORE/RET/clone）时经
   value_clone materialize 成独立深拷贝。 */
value_t *value_make_borrowed(vm_t *vm, const type_t *type, void *data) {
    value_t *v = value_alloc(vm->alloc);
    v->type = type;
    v->data = data;     /* 业务内存：父值 data 块内偏移 */
    v->is_own = false;
    scope_track(vm, vm->current_scope, v);
    return v;
}

bool value_is_borrowed(const value_t *v) {
    return v && !v->is_shadow && !v->is_own;
}

/* ---- shadow value ---- */

value_t *value_make_shadow(vm_t *vm, const type_t *type) {
    value_t *v = value_alloc(vm->alloc);
    v->type = type;
    v->data = NULL;
    v->is_shadow = true;
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
    ed.message  = message  ? string_from_cstr(vm->alloc, message)  : NULL;
    ed.location = location ? string_from_cstr(vm->alloc, location) : NULL;
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
            /* 借用引用：data 指向父值内部，不拥有，跳过 dispose 与释放 */
            if (v->is_own) {
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

value_t *value_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (value_is_error(vm, v)) return v;
    if (!v || !v->type) {
        return value_make_error(vm, "cannot implicitly cast void");
    }
    if (v->type == target) {
        /* 类型相同，clone 到当前 scope */
        return value_clone(vm, v);
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
