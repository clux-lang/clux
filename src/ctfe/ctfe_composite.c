#include "ctfe/ctfe.h"

#include <stdint.h>
#include <string.h>

#include "parser/ast_addr.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_deref.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_slice.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_func_def.h"
#include "parser/ast_var_def.h"
#include "sema/magic.h"
#include "sema/sema.h"
#include "sema/struct_compose.h"
#include "vm/type_array.h"
#include "vm/type_cunion.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/ctfe_mem.h"
#include "vm/value.h"

/* ===========================================================================
 * CTFE 复合值支持
 *
 * CTFE 的值是**真实 data 块**（与运行期同一套 type_t 布局），所以复合值访问
 * 就是"按类型布局读写 data"。本文件提供与运行期字节码同语义的访问层：
 *
 *   - struct 字段 / tag union 成员（tag 校验）/ cunion 成员（offset 0）
 *   - 指针自动解引用（`p.x`）、显式解引用（`p.*`）、取地址（`x.&`）
 *   - optional：`.!` 解包、`nil`（比较 / 初始化 / 构造 / 赋值）
 *   - 切片：`a[lo:hi]` 与 `make(ptr, len)`（视图，不复制元素）
 *   - 实例方法调用 `recv.m(args)`（`*T` 接收者取接收者地址，值接收者传副本）
 *
 * 生命周期与运行期一致：复合值由所在 vm scope 拥有，data 块在 scope pop 时
 * 释放；本文件产出的值都经 value_make（自动 track 到当前 scope）。
 * =========================================================================== */

/* ---------------------------------------------------------------- 基础工具 */

static value_t *ctfe_bool(ctfe_ctx_t *ctx, bool b) {
    vm_t *vm = ctx->vm;
    void *d = value_alloc_data_copy(vm->alloc, vm->type_bool, &b);
    if (!d) return ctfe_err(ctx, "ctfe: out of memory making bool");
    return value_make(vm, vm->type_bool, d);
}

/* 读整数 value 为 int64（按宽度与符号；非整数返回 false）。 */
static bool ctfe_int_signed(vm_t *vm, const type_t *t) {
    if (t == vm->type_i8 || t == vm->type_i16 || t == vm->type_i32 ||
        t == vm->type_i64)
        return true;
    /* 用户别名 / const 包装：按规范名前缀判断（i8..i64 有符号，其余无符号） */
    return t && t->name.ptr && t->name.len > 0 && t->name.ptr[0] == 'i' &&
           t->name.ptr[1] >= '1' && t->name.ptr[1] <= '6';
}

static bool ctfe_read_int(ctfe_ctx_t *ctx, value_t *v, int64_t *out) {
    vm_t *vm = ctx->vm;
    if (!v || !value_data(v) || !value_type(v)) return false;
    const type_t *t = value_type(v);
    if (type_kind_of(t) != TYPE_KIND_INT) return false;
    const void *d = value_data(v);
    bool sgn = ctfe_int_signed(vm, t);
    switch (t->size) {
    case 1:
        *out = sgn ? (int64_t)*(const int8_t *)d : (int64_t)*(const uint8_t *)d;
        return true;
    case 2:
        *out = sgn ? (int64_t)*(const int16_t *)d : (int64_t)*(const uint16_t *)d;
        return true;
    case 4:
        *out = sgn ? (int64_t)*(const int32_t *)d : (int64_t)*(const uint32_t *)d;
        return true;
    case 8:
        *out = sgn ? *(const int64_t *)d : (int64_t)*(const uint64_t *)d;
        return true;
    default:
        (void)vm;
        return false;
    }
}

/* 从存储读一个值（按类型拷贝成独立 data 块）。 */
static value_t *ctfe_load(ctfe_ctx_t *ctx, const type_t *t, const void *addr) {
    vm_t *vm = ctx->vm;
    if (!t || !addr) return ctfe_err(ctx, "ctfe: cannot read composite slot");
    void *data = value_alloc_data_copy(vm->alloc, t, addr);
    if (!data) return ctfe_err(ctx, "ctfe: out of memory reading composite value");
    return value_make(vm, t, data);
}

/* 写入存储（必要时按目标类型隐式转换；复合值按字节覆盖，与运行期一致）。 */
static value_t *ctfe_store(ctfe_ctx_t *ctx, const type_t *t, void *addr,
                           value_t *src) {
    vm_t *vm = ctx->vm;
    if (!t || !addr) return ctfe_err(ctx, "ctfe: cannot write composite slot");
    if (!src) return ctfe_err(ctx, "ctfe: missing value on the right-hand side");
    if (value_is_error(vm, src)) return src;
    if (value_is_shadow(src))
        return ctfe_err(ctx, "ctfe: value is not a compile-time constant");
    if (value_type(src) != t) {
        value_t *casted = value_implicit_cast(vm, src, t);
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    memcpy(addr, value_data(src), t->size);
    return value_make_undefined(vm);
}

/* --------------------------------------------------------------- nil / as */

value_t *ctfe_none_value(ctfe_ctx_t *ctx, const type_t *opt_type) {
    vm_t *vm = ctx->vm;
    if (!opt_type || type_kind_of(opt_type) != TYPE_KIND_OPTION)
        return ctfe_err(ctx, "ctfe: 'nil' requires an optional type");
    void *data = value_alloc_data(vm->alloc, opt_type);
    if (!data) return ctfe_err(ctx, "ctfe: out of memory making nil optional");
    memset(data, 0, opt_type->size); /* ok = false 即 none */
    return value_make(vm, opt_type, data);
}

value_t *ctfe_eval_as(ctfe_ctx_t *ctx, ast_node_t *e, const type_t *want) {
    vm_t *vm = ctx->vm;
    if (!e) return ctfe_err(ctx, "ctfe: missing expression");
    const type_t *w = want ? type_strip_quals(want) : NULL;
    if (e->kind == AST_NIL) {
        if (!w || type_kind_of(w) != TYPE_KIND_OPTION)
            return ctfe_err(ctx, "ctfe: 'nil' requires an optional target type");
        return ctfe_none_value(ctx, w);
    }
    value_t *v = ctfe_eval(ctx, e);
    if (value_is_error(vm, v)) return v;
    if (!w) return v;
    if (value_is_shadow(v))
        return ctfe_err(ctx, "ctfe: value is not a compile-time constant");
    if (value_type(v) == want || value_type(v) == w) return v;
    value_t *casted = value_implicit_cast(vm, v, w);
    if (value_is_error(vm, casted)) {
        /* 可赋值性已由 sema 校验；CTFE 不做更严格的二次判定——闭包类型带
           捕获槽位身份、匿名构造类型等"结构等价但 type_t 不同"的场景在这里
           会误报 "does not support implicit cast"。按原值继续。 */
        return v;
    }
    return casted;
}

value_t *ctfe_nil_compare(ctfe_ctx_t *ctx, ast_binary_t *b, bool lhs_is_nil) {
    vm_t *vm = ctx->vm;
    ast_node_t *other = lhs_is_nil ? b->rhs : b->lhs;
    value_t *v = ctfe_eval(ctx, other);
    if (value_is_error(vm, v)) return v;
    const type_t *t = value_type(v);
    t = t ? type_strip_quals(t) : NULL;
    if (!t || type_kind_of(t) != TYPE_KIND_OPTION)
        return ctfe_err(ctx, "ctfe: comparison with nil requires an optional value");
    bool some = option_ok(value_data(v));
    bool eq = !some; /* nil == none */
    if (token_is(b->op, "!=")) eq = !eq;
    return ctfe_bool(ctx, eq);
}

value_t *ctfe_is_member(ctfe_ctx_t *ctx, ast_binary_t *b) {
    vm_t *vm = ctx->vm;
    value_t *v = ctfe_eval(ctx, b->lhs);
    if (value_is_error(vm, v)) return v;
    const type_t *t = value_type(v);
    t = t ? type_strip_quals(t) : NULL;
    if (!t || type_kind_of(t) != TYPE_KIND_UNION)
        return ctfe_err(ctx, "ctfe: 'is' requires a tag union operand");
    /* sema 可能已把右操作数（member 名）折叠为 tag 整数字面量；两种形态都接受。 */
    uint64_t want = 0;
    bool have = false;
    if (b->rhs && b->rhs->kind == AST_INT_LIT) {
        want = ((ast_int_lit_t *)b->rhs)->value;
        have = true;
    } else if (b->rhs && b->rhs->kind == AST_IDENT) {
        strslice_t name = ((ast_ident_t *)b->rhs)->name;
        int mi = union_type_find_member(t, name);
        if (mi < 0) return ctfe_err(ctx, "ctfe: 'is': union has no such member");
        const union_member_t *m = union_type_member(t, (size_t)mi);
        if (!m) return ctfe_err(ctx, "ctfe: 'is': missing union member");
        want = (uint64_t)m->tag;
        have = true;
    }
    if (!have)
        return ctfe_err(ctx, "ctfe: 'is' requires a member name on the right");
    return ctfe_bool(ctx, union_read_tag(v) == want);
}

/* ------------------------------------------------------------- 左值定位 */

typedef struct {
    void         *addr;  /* 存储地址（编译期值的 data 块内部） */
    const type_t *type;  /* 该位置的静态类型 */
    void         *owner; /* 所属存储块基址（逃逸指针归属登记；NULL = 未知） */
} ctfe_lval_t;

/* 归属登记用的块基址：未显式携带时退回当前地址（保守：以该位置为块身份）。 */
static void *lv_owner(const ctfe_lval_t *lv) {
    return lv->owner ? lv->owner : lv->addr;
}

/* 解引用前校验目标地址：逃逸到已释放存储块的指针 → 诊断（NULL = 通过）。
   每个"从指针值得到存储地址"的位置都必须调用。 */
static const char *uaf_check(void *target) {
    return ctfe_mem_check(target);
}

/* 在 base(type t @ addr) 上定位成员 `name`：struct 字段 / union 成员（tag
 * 校验）/ cunion 成员；指针类型先自动解引用一层（与运行期 `p.field` 一致）。
 * 返回 0 = 错误（*err 已置），1 = 成功。 */
static int ctfe_field_slot(ctfe_ctx_t *ctx, const type_t *t, void *addr,
                           void *owner, strslice_t name, ctfe_lval_t *out,
                           value_t **err) {
    if (!t || !addr) {
        *err = ctfe_err(ctx, "ctfe: cannot access member of an empty value");
        return 0;
    }
    t = type_strip_quals(t);
    if (type_is_ptr(t)) {
        void *target = *(void **)addr;
        if (!target) {
            *err = ctfe_errf(ctx, "ctfe: null pointer dereference (member '%.*s')",
                             (int)name.len, name.ptr);
            return 0;
        }
        const char *uaf = uaf_check(target);
        if (uaf) {
            *err = ctfe_err(ctx, uaf);
            return 0;
        }
        t = type_strip_quals(ptr_type_base(t));
        addr = target;
        owner = ctfe_mem_owner_of(target);
        if (!owner) owner = target;
    }
    if (!t) {
        *err = ctfe_err(ctx, "ctfe: pointer has no pointee type");
        return 0;
    }
    if (type_kind_of(t) == TYPE_KIND_STRUCT) {
        int fi = struct_type_find_field(t, name);
        if (fi < 0) {
            *err = ctfe_errf(ctx, "ctfe: struct '%.*s' has no field '%.*s'",
                             (int)t->name.len, t->name.ptr, (int)name.len,
                             name.ptr);
            return 0;
        }
        const struct_field_t *f = struct_type_field(t, (size_t)fi);
        if (!f || !f->type) {
            *err = ctfe_err(ctx, "ctfe: struct field has no type");
            return 0;
        }
        out->type = f->type;
        out->addr = (uint8_t *)addr + f->offset;
        out->owner = owner ? owner : addr;
        return 1;
    }
    if (type_kind_of(t) == TYPE_KIND_UNION) {
        int mi = union_type_find_member(t, name);
        if (mi < 0) {
            *err = ctfe_errf(ctx, "ctfe: union '%.*s' has no member '%.*s'",
                             (int)t->name.len, t->name.ptr, (int)name.len,
                             name.ptr);
            return 0;
        }
        const union_member_t *m = union_type_member(t, (size_t)mi);
        if (!m || !m->payload_type) {
            *err = ctfe_err(ctx, "ctfe: union member has no payload type");
            return 0;
        }
        uint64_t tag = union_read_tag_raw(addr, union_type_tag_size(t));
        if (tag != (uint64_t)m->tag) {
            *err = ctfe_errf(ctx,
                             "ctfe: union member '%.*s' is not active "
                             "(tag=%llu, expected=%u)",
                             (int)name.len, name.ptr,
                             (unsigned long long)tag, (unsigned)m->tag);
            return 0;
        }
        out->type = m->payload_type;
        out->addr = (uint8_t *)addr + union_type_payload_offset(t);
        out->owner = owner ? owner : addr;
        return 1;
    }
    if (type_kind_of(t) == TYPE_KIND_CUNION) {
        int mi = cunion_type_find_member(t, name);
        if (mi < 0) {
            *err = ctfe_errf(ctx, "ctfe: cunion '%.*s' has no member '%.*s'",
                             (int)t->name.len, t->name.ptr, (int)name.len,
                             name.ptr);
            return 0;
        }
        const cunion_member_t *m = cunion_type_member(t, (size_t)mi);
        if (!m || !m->type) {
            *err = ctfe_err(ctx, "ctfe: cunion member has no type");
            return 0;
        }
        out->type = m->type;
        out->addr = addr; /* 所有成员共享 offset 0 */
        out->owner = owner ? owner : addr;
        return 1;
    }
    *err = ctfe_errf(ctx, "ctfe: cannot access member '%.*s' of value of type %s",
                     (int)name.len, name.ptr,
                     t->name.ptr ? t->name.ptr : "<anonymous>");
    return 0;
}

/* 1 = 左值（out 有效）；0 = 不是左值（无诊断）；-1 = 错误（*err）。 */
static int ctfe_lval(ctfe_ctx_t *ctx, ast_node_t *e, ctfe_lval_t *out,
                     value_t **err) {
    vm_t *vm = ctx->vm;
    if (!e) return 0;
    switch (e->kind) {
    case AST_IDENT: {
        ast_ident_t *id = (ast_ident_t *)e;
        value_t *v = scope_lookup(vm->current_scope, id->name);
        if (!v) {
            *err = ctfe_errf(ctx, "ctfe: undefined variable '%.*s'",
                             (int)id->name.len, id->name.ptr);
            return -1;
        }
        if (value_is_shadow(v)) {
            *err = ctfe_errf(ctx,
                             "ctfe: variable '%.*s' is not a compile-time constant",
                             (int)id->name.len, id->name.ptr);
            return -1;
        }
        out->addr = value_data(v);
        out->type = value_type(v);
        out->owner = value_data(v);
        return out->addr && out->type ? 1 : 0;
    }
    case AST_MEMBER: {
        ast_member_t *m = (ast_member_t *)e;
        ctfe_lval_t base;
        value_t *berr = NULL;
        int rc = ctfe_lval(ctx, m->object, &base, &berr);
        if (rc < 0) {
            *err = berr;
            return -1;
        }
        if (rc == 0) return 0; /* 接收者是右值：由调用方按值读取 */
        if (!ctfe_field_slot(ctx, base.type, base.addr, lv_owner(&base), m->field,
                             out, err))
            return -1;
        return 1;
    }
    case AST_INDEX: {
        ast_index_t *ix = (ast_index_t *)e;
        ctfe_lval_t base;
        value_t *berr = NULL;
        int rc = ctfe_lval(ctx, ix->object, &base, &berr);
        if (rc < 0) {
            *err = berr;
            return -1;
        }
        if (rc == 0) return 0;
        value_t *iv = ctfe_eval(ctx, ix->indices);
        if (value_is_error(vm, iv)) {
            *err = iv;
            return -1;
        }
        int64_t idx = 0;
        if (!ctfe_read_int(ctx, iv, &idx)) {
            *err = ctfe_err(ctx, "ctfe: index must be an integer");
            return -1;
        }
        const type_t *t = type_strip_quals(base.type);
        if (!t) return 0;
        if (type_kind_of(t) == TYPE_KIND_ARRAY) {
            const type_t *et = array_type_elem(t);
            size_t n = array_type_len(t);
            if (idx < 0 || (uint64_t)idx >= n) {
                *err = ctfe_errf(ctx, "ctfe: array index %lld out of bounds (len=%zu)",
                                 (long long)idx, n);
                return -1;
            }
            out->type = et;
            out->addr = (uint8_t *)base.addr + (size_t)idx * et->size;
            out->owner = lv_owner(&base);
            return 1;
        }
        if (type_kind_of(t) == TYPE_KIND_TUPLE) {
            if (idx < 0 || (size_t)idx >= tuple_type_elem_count(t)) {
                *err = ctfe_errf(ctx, "ctfe: tuple index %lld out of bounds",
                                 (long long)idx);
                return -1;
            }
            const tuple_elem_t *te = tuple_type_elem(t, (size_t)idx);
            if (!te || !te->type) {
                *err = ctfe_err(ctx, "ctfe: tuple element has no type");
                return -1;
            }
            out->type = te->type;
            out->addr = (uint8_t *)base.addr + te->offset;
            out->owner = lv_owner(&base);
            return 1;
        }
        if (type_is_slice(t)) {
            const type_t *et = slice_type_elem(t);
            uint64_t n = ((const slice_value_t *)base.addr)->len;
            void *ptr = ((const slice_value_t *)base.addr)->ptr;
            if (idx < 0 || (uint64_t)idx >= n) {
                *err = ctfe_errf(ctx, "ctfe: slice index %lld out of bounds (len=%llu)",
                                 (long long)idx, (unsigned long long)n);
                return -1;
            }
            if (!ptr) {
                *err = ctfe_err(ctx, "ctfe: null slice");
                return -1;
            }
            const char *uaf = uaf_check(ptr);
            if (uaf) {
                *err = ctfe_err(ctx, uaf);
                return -1;
            }
            out->type = et;
            out->addr = (uint8_t *)ptr + (size_t)idx * et->size;
            out->owner = ctfe_mem_owner_of(ptr);
            if (!out->owner) out->owner = ptr;
            return 1;
        }
        return 0;
    }
    case AST_DEREF: {
        ast_deref_t *d = (ast_deref_t *)e;
        value_t *pv = ctfe_eval(ctx, d->operand);
        if (value_is_error(vm, pv)) {
            *err = pv;
            return -1;
        }
        const type_t *pt = value_type(pv);
        if (!pt || !type_is_ptr(pt)) {
            *err = ctfe_err(ctx, "ctfe: dereference requires a pointer");
            return -1;
        }
        void *target = ptr_value_target(pv);
        if (!target) {
            *err = ctfe_err(ctx, "ctfe: null pointer dereference");
            return -1;
        }
        const char *uaf = uaf_check(target);
        if (uaf) {
            *err = ctfe_err(ctx, uaf);
            return -1;
        }
        out->type = ptr_type_base(pt);
        out->addr = target;
        out->owner = ctfe_mem_owner_of(target);
        if (!out->owner) out->owner = target;
        return 1;
    }
    case AST_UNWRAP: {
        ast_unwrap_t *u = (ast_unwrap_t *)e;
        ctfe_lval_t base;
        value_t *berr = NULL;
        int rc = ctfe_lval(ctx, u->operand, &base, &berr);
        if (rc < 0) {
            *err = berr;
            return -1;
        }
        if (rc == 0) return 0;
        const type_t *t = type_strip_quals(base.type);
        if (!t || type_kind_of(t) != TYPE_KIND_OPTION) {
            *err = ctfe_err(ctx, "ctfe: '.:' unwrap requires an optional value");
            return -1;
        }
        if (!option_ok(base.addr)) {
            *err = ctfe_err(ctx, "ctfe: unwrap of a nil optional value");
            return -1;
        }
        out->type = type_option_inner(t);
        out->addr = option_value(base.addr, t);
        out->owner = lv_owner(&base);
        return 1;
    }
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ 成员读 */

value_t *ctfe_member_get(ctfe_ctx_t *ctx, ast_member_t *m) {
    vm_t *vm = ctx->vm;
    ctfe_lval_t lv;
    value_t *err = NULL;
    int rc = ctfe_lval(ctx, (ast_node_t *)m, &lv, &err);
    if (rc < 0) return err;
    if (rc == 0) {
        /* 右值接收者（调用结果 / 构造字面量）：按值在 data 块上读字段。 */
        value_t *ov = ctfe_eval(ctx, m->object);
        if (value_is_error(vm, ov)) return ov;
        if (value_is_shadow(ov))
            return ctfe_err(ctx, "ctfe: receiver is not a compile-time value");
        value_t *ferr = NULL;
        if (!ctfe_field_slot(ctx, value_type(ov), value_data(ov), value_data(ov),
                             m->field, &lv, &ferr))
            return ferr;
    }
    return ctfe_load(ctx, lv.type, lv.addr);
}

/* ------------------------------------------------------------------ 赋值 */

value_t *ctfe_assign_lvalue(ctfe_ctx_t *ctx, ast_assign_t *as) {
    vm_t *vm = ctx->vm;
    ctfe_lval_t lv;
    value_t *err = NULL;
    int rc = ctfe_lval(ctx, as->target, &lv, &err);
    if (rc < 0) return err;
    if (rc == 0)
        return ctfe_err(ctx, "ctfe: assignment target is not an addressable "
                             "compile-time value");
    if (!lv.type) return ctfe_err(ctx, "ctfe: assignment target has no type");

    value_t *src = ctfe_eval_as(ctx, as->value, lv.type);
    if (value_is_error(vm, src)) return src;
    if (token_is(as->op, "=")) return ctfe_store(ctx, lv.type, lv.addr, src);

    value_t *old = ctfe_load(ctx, lv.type, lv.addr);
    if (value_is_error(vm, old)) return old;
    value_t *r = NULL;
    if (token_is(as->op, "+="))      r = value_add(vm, old, src);
    else if (token_is(as->op, "-=")) r = value_sub(vm, old, src);
    else if (token_is(as->op, "*=")) r = value_mul(vm, old, src);
    else if (token_is(as->op, "/=")) r = value_div(vm, old, src);
    else if (token_is(as->op, "%=")) r = value_mod(vm, old, src);
    else return ctfe_err(ctx, "ctfe: unsupported assignment operator");
    if (value_is_error(vm, r)) return r;
    return ctfe_store(ctx, lv.type, lv.addr, r);
}

/* 下标左值赋值（数组 / 元组 / 切片 / 指针）：与成员赋值同一条存储路径。 */
value_t *ctfe_index_assign(ctfe_ctx_t *ctx, ast_assign_t *as) {
    return ctfe_assign_lvalue(ctx, as);
}

/* ------------------------------------------------------ unwrap / deref / & */

value_t *ctfe_unwrap_opt(ctfe_ctx_t *ctx, ast_unwrap_t *u) {
    vm_t *vm = ctx->vm;
    ctfe_lval_t lv;
    value_t *err = NULL;
    int rc = ctfe_lval(ctx, (ast_node_t *)u, &lv, &err);
    if (rc < 0) return err;
    if (rc == 0) {
        /* 右值 optional：直接按值判断并读载荷。 */
        value_t *v = ctfe_eval(ctx, u->operand);
        if (value_is_error(vm, v)) return v;
        const type_t *t = value_type(v);
        t = t ? type_strip_quals(t) : NULL;
        if (!t || type_kind_of(t) != TYPE_KIND_OPTION)
            return ctfe_err(ctx, "ctfe: '.:' unwrap requires an optional value");
        if (!option_ok(value_data(v)))
            return ctfe_err(ctx, "ctfe: unwrap of a nil optional value");
        return ctfe_load(ctx, type_option_inner(t), option_value(value_data(v), t));
    }
    return ctfe_load(ctx, lv.type, lv.addr);
}

value_t *ctfe_deref(ctfe_ctx_t *ctx, ast_node_t *operand) {
    vm_t *vm = ctx->vm;
    ast_node_t *probe = operand;
    (void)probe;
    value_t *pv = ctfe_eval(ctx, operand);
    if (value_is_error(vm, pv)) return pv;
    const type_t *pt = value_type(pv);
    if (!pt || !type_is_ptr(pt))
        return ctfe_err(ctx, "ctfe: dereference requires a pointer");
    void *target = ptr_value_target(pv);
    if (!target) return ctfe_err(ctx, "ctfe: null pointer dereference");
    const char *uaf = uaf_check(target);
    if (uaf) return ctfe_err(ctx, uaf);
    return ctfe_load(ctx, ptr_type_base(pt), target);
}

value_t *ctfe_addr_of(ctfe_ctx_t *ctx, ast_node_t *addr_node) {
    vm_t *vm = ctx->vm;
    if (!addr_node || addr_node->kind != AST_ADDR) return NULL;
    ast_addr_t *n = (ast_addr_t *)addr_node;
    ctfe_lval_t lv;
    value_t *err = NULL;
    int rc = ctfe_lval(ctx, n->operand, &lv, &err);
    if (rc < 0) return err;
    if (rc == 0) {
        /* 右值物化：**仅**编译器生成的取址（魔法方法派发 `b.&`）——求值成
           受跟踪的副本再取地址（与 C 后端 `&(T){v}` 复合字面量同语义）。
           用户写的 `x.&` 在 sema 已经要求可寻址。 */
        if (!addr_node->is_generated)
            return ctfe_err(ctx,
                            "ctfe: cannot take the address of a non-addressable "
                            "compile-time value");
        value_t *v = ctfe_eval(ctx, n->operand);
        if (value_is_error(vm, v)) return v;
        const type_t *t = value_type(v);
        if (!t) return ctfe_err(ctx, "ctfe: address operand has no type");
        value_t *copy = ctfe_load(ctx, t, value_data(v));
        if (value_is_error(vm, copy)) return copy;
        const type_t *pt = type_ptr_intern(vm, t);
        if (!pt) return ctfe_err(ctx, "ctfe: cannot intern pointer type");
        if (ctx->sema) sema_type_register(ctx->sema, pt);
        /* 物化副本的地址同样可能逃逸：登记归属（副本随作用域销毁后失效）。 */
        ctfe_mem_note_addr(value_data(copy), value_data(copy));
        return ptr_make_value(vm, pt, value_data(copy));
    }
    const type_t *pt = type_ptr_intern(vm, lv.type);
    if (!pt) return ctfe_err(ctx, "ctfe: cannot intern pointer type");
    if (ctx->sema) sema_type_register(ctx->sema, pt);
    /* 登记取址结果的归属存储块：块被 value_dispose 释放后，任何仍指向
       该地址的指针再被解引用都会被诊断（逃逸指针 use-after-free）。 */
    ctfe_mem_note_addr(lv.addr, lv_owner(&lv));
    return ptr_make_value(vm, pt, lv.addr);
}

/* ------------------------------------------------------------------ 切片 */

value_t *ctfe_slice_expr(ctfe_ctx_t *ctx, ast_slice_t *sl) {
    vm_t *vm = ctx->vm;
    value_t *obj = ctfe_eval(ctx, sl->object);
    if (value_is_error(vm, obj)) return obj;
    const type_t *t = value_type(obj);
    t = t ? type_strip_quals(t) : NULL;
    if (!t) return ctfe_err(ctx, "ctfe: slice base has no type");

    const type_t *elem = NULL;
    void *base = NULL;
    void *owner = NULL; /* 切片基址的归属存储块（str 字面量不参与登记） */
    uint64_t n = 0;
    if (type_kind_of(t) == TYPE_KIND_ARRAY) {
        elem = array_type_elem(t);
        base = value_data(obj);
        owner = base;
        n = (uint64_t)array_type_len(t);
    } else if (type_is_slice(t)) {
        slice_value_t sv = slice_view(obj);
        elem = slice_type_elem(t);
        base = sv.ptr;
        /* 再切片是基于**已有切片**的视图：先校验它的基址块还活着
           （`lo > 0` 时新基址是内部地址，精确命中查不到，必须查原基址）。 */
        const char *uaf = uaf_check(sv.ptr);
        if (uaf) return ctfe_err(ctx, uaf);
        owner = ctfe_mem_owner_of(sv.ptr);
        if (!owner) owner = sv.ptr;
        n = sv.len;
    } else if (type_kind_of(t) == TYPE_KIND_STR) {
        const char *s = *(const char *const *)value_data(obj);
        elem = type_const_intern(vm, vm->type_u8);
        base = (void *)s;
        n = s ? (uint64_t)strlen(s) : 0;
    } else {
        return ctfe_err(ctx, "ctfe: unsupported slice base type");
    }
    if (!elem) return ctfe_err(ctx, "ctfe: slice base has no element type");

    int64_t lo = 0;
    int64_t hi = (int64_t)n;
    if (sl->low) {
        value_t *v = ctfe_eval(ctx, sl->low);
        if (value_is_error(vm, v)) return v;
        if (!ctfe_read_int(ctx, v, &lo))
            return ctfe_err(ctx, "ctfe: slice lower bound must be an integer");
    }
    if (sl->high) {
        value_t *v = ctfe_eval(ctx, sl->high);
        if (value_is_error(vm, v)) return v;
        if (!ctfe_read_int(ctx, v, &hi))
            return ctfe_err(ctx, "ctfe: slice upper bound must be an integer");
    }
    if (lo < 0 || hi < lo || (uint64_t)hi > n)
        return ctfe_errf(ctx,
                         "ctfe: slice [%lld:%lld] out of bounds (len=%llu)",
                         (long long)lo, (long long)hi, (unsigned long long)n);
    const type_t *st = type_slice_intern(vm, elem);
    if (!st) return ctfe_err(ctx, "ctfe: cannot intern slice type");
    void *slice_base = (uint8_t *)base + (size_t)lo * elem->size;
    if (lo < hi && owner) {
        const char *uaf = uaf_check(slice_base);
        if (uaf) return ctfe_err(ctx, uaf);
        /* 数组：owner 就是数组值自己的 data 块；切片：owner 继承自更早的
           取址。存活状态由分配/释放钩子维护，这里只记录归属。 */
        ctfe_mem_note_addr(slice_base, owner);
    }
    return slice_make_value(vm, st, slice_base, (uint64_t)(hi - lo));
}

value_t *ctfe_make_slice(ctfe_ctx_t *ctx, ast_make_t *mk) {
    vm_t *vm = ctx->vm;
    value_t *pv = ctfe_eval(ctx, mk->pointer);
    if (value_is_error(vm, pv)) return pv;
    const type_t *pt = value_type(pv);
    pt = pt ? type_strip_quals(pt) : NULL;
    if (!pt || !type_is_ptr(pt))
        return ctfe_err(ctx, "ctfe: make() requires a pointer first argument");
    const type_t *elem = ptr_type_base(pt);
    value_t *lv = ctfe_eval(ctx, mk->length);
    if (value_is_error(vm, lv)) return lv;
    int64_t len = 0;
    if (!ctfe_read_int(ctx, lv, &len) || len < 0)
        return ctfe_err(ctx, "ctfe: make() length must be a non-negative integer");
    const type_t *st = type_slice_intern(vm, elem);
    if (!st) return ctfe_err(ctx, "ctfe: cannot intern slice type");
    void *base = ptr_value_target(pv);
    if (base) {
        const char *uaf = uaf_check(base);
        if (uaf) return ctfe_err(ctx, uaf);
        /* 归属来自指针（可能继承自更早的取址）；存活由分配/释放钩子判定。 */
        ctfe_mem_note_addr(base, ctfe_mem_owner_of(base));
    }
    return slice_make_value(vm, st, base, (uint64_t)len);
}

/* --------------------------------------------------------------- 静态方法 */

/* `Type::method(args)`：无 self 的静态方法。返回 NULL = 不是静态方法
   （例如枚举 variant 引用、跨模块限定名），调用方回退。 */
value_t *ctfe_static_call(ctfe_ctx_t *ctx, ast_call_t *call) {
    vm_t *vm = ctx->vm;
    if (!ctx->sema || !call || !call->callee ||
        call->callee->kind != AST_SCOPE_REF)
        return NULL;
    ast_scope_ref_t *sr = (ast_scope_ref_t *)call->callee;
    if (!sr->type_expr) return NULL;

    const type_t *T = NULL;
    strslice_t tname = STRSLICE_EMPTY;
    if (sr->type_expr->kind == AST_IDENT)
        tname = ((ast_ident_t *)sr->type_expr)->name;
    else if (sr->type_expr->kind == AST_TYPE_REF)
        tname = ((ast_type_ref_t *)sr->type_expr)->name;
    if (tname.ptr && ctx->sema->global_scope) {
        sema_symbol_t *tsym =
            ctx->sema_scope ? sema_lookup(ctx->sema_scope, tname) : NULL;
        if (!tsym) tsym = sema_lookup(ctx->sema->global_scope, tname);
        if (tsym && tsym->kind == SEMA_SYM_TYPE && tsym->type) T = tsym->type;
    }
    if (!T) {
        /* 类型表达式（如 `Box[i32]::make(...)`）：按表达式求值取类型值。 */
        value_t *tv = ctfe_eval(ctx, sr->type_expr);
        if (value_is_error(vm, tv)) return tv;
        if (value_is_type(tv, TYPE_KIND_TYPE)) T = value_as(tv, const type_t *);
    }
    if (!T) return NULL;
    const type_t *core = type_strip_quals(T);
    if (!core || type_kind_of(core) != TYPE_KIND_STRUCT) return NULL;

    sema_symbol_t *ms = sema_struct_find_member(ctx->sema, core, sr->variant,
                                                call->callee, NULL);
    if (!ms || ms->kind != SEMA_SYM_FUNC || !ms->ast) return NULL;
    ast_func_def_t *def = (ast_func_def_t *)ms->ast;
    if (def->generic.count > 0)
        return ctfe_errf(ctx,
                         "ctfe: generic static method '%.*s' is not supported "
                         "in compile-time code",
                         (int)sr->variant.len, sr->variant.ptr);

    size_t argc = ctfe_count_siblings(call->args);
    value_t *argv[argc > 0 ? argc : 1];
    size_t i = 0;
    for (ast_node_t *a = call->args; a; a = a->next, i++) {
        argv[i] = ctfe_eval(ctx, a);
        if (value_is_error(vm, argv[i])) return argv[i];
    }
    return ctfe_call_ast_fn_vals(ctx, def, argv, argc);
}

/* ------------------------------------------------------------- 内建 data/take */

value_t *ctfe_builtin_data(ctfe_ctx_t *ctx, ast_node_t *arg) {
    vm_t *vm = ctx->vm;
    if (!arg) return ctfe_err(ctx, "ctfe: data() expects one argument");
    if (arg->next) return ctfe_err(ctx, "ctfe: data() expects one argument");
    value_t *v = ctfe_eval(ctx, arg);
    if (value_is_error(vm, v)) return v;
    if (value_is_shadow(v))
        return ctfe_err(ctx, "ctfe: data() requires a compile-time value");
    const type_t *t = value_type(v);
    t = t ? type_strip_quals(t) : NULL;
    if (!t) return ctfe_err(ctx, "ctfe: data() argument has no type");
    const type_t *elem = NULL;
    void *ptr = NULL;
    if (type_kind_of(t) == TYPE_KIND_ARRAY) {
        elem = array_type_elem(t);
        ptr = value_data(v); /* 数组值的 data 块 = 元素序列首地址 */
    } else if (type_is_slice(t)) {
        slice_value_t sv = slice_view(v);
        elem = slice_type_elem(t);
        ptr = sv.ptr;
    } else if (type_kind_of(t) == TYPE_KIND_STR) {
        const char *s = *(const char *const *)value_data(v);
        elem = vm->type_u8;
        ptr = (void *)s;
    }
    if (!elem)
        return ctfe_err(ctx, "ctfe: data() argument must be array/slice/str");
    const type_t *pt = type_ptr_intern(vm, elem);
    if (!pt) return ctfe_err(ctx, "ctfe: cannot intern pointer type");
    if (ctx->sema) sema_type_register(ctx->sema, pt);
    /* str 的字符缓冲不归 value data 块管理（字符串池/独立分配），不登记。 */
    if (ptr && type_kind_of(t) != TYPE_KIND_STR) {
        /* 切片取 data：归属继承自切片基址；数组取 data：ptr 就是数组值自己的
           data 块。存活状态由分配/释放钩子维护。 */
        ctfe_mem_note_addr(ptr, ctfe_mem_owner_of(ptr));
    }
    return ptr_make_value(vm, pt, ptr);
}

value_t *ctfe_builtin_take(ctfe_ctx_t *ctx, ast_node_t *arg) {
    vm_t *vm = ctx->vm;
    if (!arg || arg->next)
        return ctfe_err(ctx, "ctfe: take() expects one argument");
    ctfe_lval_t lv;
    value_t *err = NULL;
    int rc = ctfe_lval(ctx, arg, &lv, &err);
    if (rc < 0) return err;
    if (rc == 0)
        return ctfe_err(ctx,
                        "ctfe: take() requires an addressable optional value");
    const type_t *t = lv.type ? type_strip_quals(lv.type) : NULL;
    if (!t || type_kind_of(t) != TYPE_KIND_OPTION)
        return ctfe_err(ctx, "ctfe: take() argument must be an optional value");
    if (!option_ok(lv.addr))
        return ctfe_err(ctx, "ctfe: take() of a nil optional (runtime panic)");
    value_t *payload =
        ctfe_load(ctx, type_option_inner(t), option_value(lv.addr, t));
    if (value_is_error(vm, payload)) return payload;
    memset(lv.addr, 0, t->size); /* 源置 none（载荷已被拷走） */
    return payload;
}

value_t *ctfe_builtin_panic(ctfe_ctx_t *ctx, ast_node_t *arg) {
    vm_t *vm = ctx->vm;
    value_t *m = arg ? ctfe_eval(ctx, arg) : NULL;
    if (m && value_is_error(vm, m)) return m;
    const char *s = NULL;
    if (m && value_type(m) == vm->type_str && value_data(m))
        s = *(const char *const *)value_data(m);
    return ctfe_errf(ctx, "ctfe: compile-time panic: %s", s ? s : "");
}

/* `x.?` / `x.!`（sema 生成的 AST_TRY）：
 *   { var __try_N = <setup.init>;
 *     __try_N.ok() ? __try_N.value()
 *                  : <tail_stmt> }        // .?：return Ret::of_error(err)
 *                                          // .!：panic(__try_N.panic())
 * 失败路径执行 tail 后会留下 ctrl == RETURN，外层语句/块据此提前结束。 */
value_t *ctfe_try_expr(ctfe_ctx_t *ctx, ast_try_t *tr) {
    vm_t *vm = ctx->vm;
    if (!tr) return ctfe_err(ctx, "ctfe: malformed try expression");
    if (tr->setup) {
        value_t *r = ctfe_eval_stmt(ctx, tr->setup);
        if (value_is_error(vm, r)) return r;
    }
    if (!tr->ok_call)
        return ctfe_err(ctx, "ctfe: malformed try expression (missing ok())");
    value_t *okv = ctfe_eval(ctx, tr->ok_call);
    if (value_is_error(vm, okv)) return okv;
    if (!value_type(okv) || type_kind_of(value_type(okv)) != TYPE_KIND_BOOL)
        return ctfe_err(ctx, "ctfe: 'ok()' must return bool");
    if (ctfe_read_bool(vm, okv)) {
        if (!tr->value_call)
            return ctfe_err(ctx, "ctfe: malformed try expression (missing value())");
        return ctfe_eval(ctx, tr->value_call);
    }

    /* 失败路径。`.!`（panic_mode）：编译期直接报错，并尽量带上 panic 消息。 */
    if (tr->panic_mode) {
        ast_node_t *pcall = tr->tail_stmt;
        if (pcall && pcall->kind == AST_EXPR_STMT)
            pcall = ((ast_expr_stmt_t *)pcall)->expr;
        const char *msg = "";
        if (pcall && pcall->kind == AST_CALL && ((ast_call_t *)pcall)->args) {
            value_t *mv = ctfe_eval(ctx, ((ast_call_t *)pcall)->args);
            if (!value_is_error(vm, mv) && value_type(mv) == vm->type_str &&
                value_data(mv))
                msg = *(const char *const *)value_data(mv);
        }
        return ctfe_errf(ctx, "ctfe: '.!' on an error result (panic: %s)",
                         msg ? msg : "");
    }
    if (tr->tail_stmt) {
        value_t *r = ctfe_eval_stmt(ctx, tr->tail_stmt); /* return of_error(...) */
        if (value_is_error(vm, r)) return r;
    }
    return value_make_undefined(vm);
}

/* --------------------------------------------------------------- 方法调用 */

value_t *ctfe_method_call(ctfe_ctx_t *ctx, ast_call_t *call) {
    vm_t *vm = ctx->vm;
    if (!ctx->sema || !call || !call->callee || call->callee->kind != AST_MEMBER)
        return NULL;
    ast_member_t *m = (ast_member_t *)call->callee;

    /* 接收者：左值取存储地址（供 `*T` self 原地修改），右值按值求值。 */
    ctfe_lval_t lv;
    value_t *lerr = NULL;
    int lrc = ctfe_lval(ctx, m->object, &lv, &lerr);
    value_t *recv = NULL;
    void *self_addr = NULL; /* `*T` self 收到的地址：接收者对象本身 */
    if (lrc < 0) return lerr;
    if (lrc > 0) {
        recv = ctfe_load(ctx, lv.type, lv.addr);
        self_addr = lv.addr;
    } else {
        recv = ctfe_eval(ctx, m->object);
    }
    if (!recv || value_is_error(vm, recv)) return recv ? recv : NULL;

    const type_t *rt = value_type(recv);
    rt = rt ? type_strip_quals(rt) : NULL;
    if (!rt) return NULL;
    const type_t *st = rt;
    if (type_is_ptr(st)) {
        /* 接收者已经是指针：`*T` self 要的是**被指向对象**的地址（pointee），
           不能把指针变量自己的槽位地址传进去（那会变成 **T）。 */
        self_addr = ptr_value_target(recv);
        const char *uaf = uaf_check(self_addr);
        if (uaf) return ctfe_err(ctx, uaf);
        st = type_strip_quals(ptr_type_base(st));
    }
    if (!st || type_kind_of(st) != TYPE_KIND_STRUCT) return NULL;

    sema_symbol_t *ms =
        sema_struct_find_member(ctx->sema, st, m->field, call->callee, NULL);
    if (!ms || ms->kind != SEMA_SYM_FUNC || !ms->ast) return NULL;
    ast_func_def_t *def = (ast_func_def_t *)ms->ast;
    if (def->generic.count > 0)
        return ctfe_errf(ctx,
                         "ctfe: generic method '%.*s' is not supported in "
                         "compile-time code",
                         (int)m->field.len, m->field.ptr);
    if (!def->params || def->params->kind != AST_VAR_DEF)
        return ctfe_err(ctx, "ctfe: method has no self parameter");
    ast_var_def_t *self_vd = (ast_var_def_t *)def->params;
    const type_t *self_t = NULL;
    if (self_vd->type_expr) {
        value_t *tv = ctfe_eval(ctx, self_vd->type_expr);
        if (!value_is_error(vm, tv) && value_is_type(tv, TYPE_KIND_TYPE))
            self_t = value_as(tv, const type_t *);
    }
    if (!self_t)
        return ctfe_err(ctx, "ctfe: cannot resolve the method's self type");

    size_t argc = ctfe_count_siblings(call->args);
    value_t *argv[argc + 1];
    if (type_is_ptr(type_strip_quals(self_t))) {
        void *addr = self_addr;
        if (!addr) {
            /* 右值接收者：物化一个追踪的副本，取它的地址
               （与运行期"右值接收者先落临时左值"一致）。 */
            value_t *tmp = ctfe_load(ctx, rt, value_data(recv));
            if (value_is_error(vm, tmp)) return tmp;
            addr = value_data(tmp);
        }
        argv[0] = ptr_make_value(vm, self_t, addr);
        if (!argv[0]) return ctfe_err(ctx, "ctfe: out of memory making receiver");
    } else {
        argv[0] = recv;
    }
    size_t i = 1;
    for (ast_node_t *a = call->args; a; a = a->next, i++) {
        argv[i] = ctfe_eval(ctx, a);
        if (value_is_error(vm, argv[i])) return argv[i];
    }
    return ctfe_call_ast_fn_vals(ctx, def, argv, argc + 1);
}
