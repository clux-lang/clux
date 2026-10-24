#include "cbackend/c_lower_internal.h"

#include "sema/struct_compose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lower_method_t *find_method(lower_ctx_t *lc, uint32_t owner_id,
                             strslice_t name) {
    for (lower_method_t *m = lc->methods; m; m = m->next) {
        if (m->owner_id == owner_id && m->name.len == name.len &&
            memcmp(m->name.ptr, name.ptr, name.len) == 0)
            return m;
    }
    return NULL;
}

/* AST 表达式在生成的 C 里是否是可寻址左值。
 * 只覆盖会产生 C 左值的形态（变量、解引用、下标、左值的成员）；其余
 * （调用结果、构造/字面量等）视为右值，需要先物化成临时对象。 */
static bool ast_is_lvalue(const ast_node_t *e) {
    if (!e) return false;
    switch (e->kind) {
    case AST_IDENT:
    case AST_DEREF:
    case AST_INDEX:
        return true;
    case AST_MEMBER: {
        const ast_member_t *m = (const ast_member_t *)e;
        /* bitfield 字段是位读表达式，不是 C 左值。 */
        if (m->is_bitfield) return false;
        return ast_is_lvalue(m->object);
    }
    default:
        return false;
    }
}

static cir_expr_t *lower_call_arg(lower_ctx_t *lc, ast_node_t *a) {
    return lower_expr(lc, a);
}

/* ============================ bitfield ============================ */

static uint64_t bf_mask(size_t width) {
    return width >= 64 ? UINT64_MAX : ((UINT64_C(1) << width) - 1);
}

/* 位流写入：与 VM 的 bitfield_write 完全一致（小端字内、可跨字）。 */
static void bf_pack(uint64_t *words, uint32_t word_bits, uint32_t offset,
                    uint32_t width, uint64_t value) {
    value &= bf_mask(width);
    uint32_t idx = offset / word_bits;
    uint32_t sh = offset % word_bits;
    uint32_t lobits = width < word_bits - sh ? width : word_bits - sh;
    uint64_t lo_mask = bf_mask(lobits) << sh;
    words[idx] = (words[idx] & ~lo_mask) | ((value << sh) & lo_mask);
    if (sh + width > word_bits) {
        uint32_t rest = sh + width - word_bits;
        uint64_t hi_mask = bf_mask(rest);
        words[idx + 1] = (words[idx + 1] & ~hi_mask) |
                         ((value >> (word_bits - sh)) & hi_mask);
    }
}

const ast_struct_def_t *bitfield_def_of(lower_ctx_t *lc, const type_t *t) {
    (void)lc;
    if (!t) return NULL;
    t = type_strip_quals(t);
    if (type_kind_of(t) != TYPE_KIND_STRUCT) return NULL;
    const struct_type_t *st = (const struct_type_t *)t;
    return st->bitfield_ast ? (const ast_struct_def_t *)st->bitfield_ast : NULL;
}

const type_t *bitfield_value_type(lower_ctx_t *lc, uint32_t type_id) {
    const sema_type_t *st = sema_type_by_id(lc->sema, type_id);
    if (st && st->type) return st->type;
    return vm_type_load(lc->sema->vm, type_id);
}

/* bitfield 的隐藏存储字段（解析器固定叫 __bits；C 名加 clux_f_ 前缀）。 */
static const char *bitfield_storage_field(lower_ctx_t *lc, cir_type_t *ct) {
    if (ct && ct->fields && ct->fields->name) return ct->fields->name;
    return copy_arena(lc->arena, "clux_f___bits");
}

static cir_expr_t *bitfield_storage(lower_ctx_t *lc, cir_type_t *ct,
                                    cir_expr_t *object) {
    return cir_expr_member(lc->arena, object, bitfield_storage_field(lc, ct));
}

static cir_expr_t *lower_bitfield_read(lower_ctx_t *lc, ast_member_t *m) {
    const type_t *ot = infer_expr_type(lc, m->object);
    const type_t *vt = bitfield_value_type(lc, m->bit_value_type_id);
    if (!ot || !vt) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)m),
                   "c backend: cannot determine bitfield value type");
        lc->failed = true;
        return NULL;
    }
    cir_type_t *ct = map_type(lc, ot);
    cir_type_t *cvt = map_type(lc, vt);
    cir_expr_t *obj = lower_expr(lc, m->object);
    if (!ct || !cvt || !obj) return NULL;
    lc->unit->uses_bitfield = true;
    return cir_expr_bitfield_get(lc->arena,
                                 bitfield_storage(lc, ct, obj), m->bit_offset,
                                 m->bit_width, m->bit_word_bits, cvt);
}

cir_expr_t *lower_bitfield_assign(lower_ctx_t *lc, ast_member_t *m,
                                  const char *op, ast_node_t *value) {
    const type_t *ot = infer_expr_type(lc, m->object);
    const type_t *vt = bitfield_value_type(lc, m->bit_value_type_id);
    if (!ot || !vt) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)m),
                   "c backend: cannot determine bitfield value type");
        lc->failed = true;
        return NULL;
    }
    cir_type_t *ct = map_type(lc, ot);
    cir_type_t *cvt = map_type(lc, vt);
    cir_expr_t *obj = lower_expr(lc, m->object);
    if (!ct || !cvt || !obj) return NULL;
    bool simple = !op || strcmp(op, "=") == 0;
    cir_expr_t *val = NULL;
    if (simple) {
        val = lower_expr(lc, value);
    } else {
        /* 复合赋值：保留对象引用 → 读取 → value → 运算 → 写回。
           object 表达式在生成的 C 里出现两次（与常规字段复合赋值一致）。 */
        cir_expr_t *old = cir_expr_bitfield_get(
            lc->arena, bitfield_storage(lc, ct, obj), m->bit_offset,
            m->bit_width, m->bit_word_bits, cvt);
        char bop[8] = {0};
        size_t n = strlen(op);
        if (n >= 2 && op[n - 1] == '=') {
            if (n - 1 >= sizeof bop) n = sizeof bop;
            memcpy(bop, op, n - 1);
            bop[n - 1] = '\0';
        }
        cir_expr_t *rhs = lower_expr(lc, value);
        val = rhs ? cir_expr_binary(lc->arena, copy_arena(lc->arena, bop), old,
                                    rhs)
                  : NULL;
    }
    if (!val) return NULL;
    lc->unit->uses_bitfield = true;
    return cir_expr_bitfield_set(lc->arena, bitfield_storage(lc, ct, obj),
                                 m->bit_offset, m->bit_width, m->bit_word_bits,
                                 val, ct);
}

/* 构造器字段值：字段类型是 `?T` 时按 nil / 隐式 some 包装（与变量初始化、
   赋值、return 同一规则）。 */
static cir_expr_t *lower_ctor_value(lower_ctx_t *lc, const type_t *field_type,
                                    ast_node_t *val) {
    if (!val) return NULL;
    if (!field_type) return lower_expr(lc, val);
    const type_t *st = type_strip_quals(field_type);
    if (type_kind_of(st) == TYPE_KIND_OPTION) {
        cir_type_t *cot = map_type(lc, st);
        if (!cot) return NULL;
        if (val->kind == AST_NIL)
            return cir_expr_option(lc->arena, cot, false, NULL);
        const type_t *vt = infer_expr_type(lc, val);
        if (!vt || type_kind_of(type_strip_quals(vt)) != TYPE_KIND_OPTION) {
            cir_expr_t *v = lower_expr(lc, val);
            return v ? cir_expr_option(lc->arena, cot, true, v) : NULL;
        }
    }
    return lower_expr(lc, val);
}

/* 构造器字段的静态类型。
 * 注意：sema 会把 struct 构造的字段链重排为**类型表序**并去掉
 * AST_CONSTRUCT_FIELD 包装（链上是裸值节点），因此具名字段信息已经不在
 * AST 上——按**下标**取字段类型；若仍是具名字段节点（sema 之前的形态）
 * 则按名查。数组/其他 positional 构造取元素类型。 */
static const type_t *ctor_field_type(const type_t *t, const ast_node_t *field,
                                     size_t index) {
    if (!t) return NULL;
    const type_t *st = type_strip_quals(t);
    if (type_kind_of(st) == TYPE_KIND_STRUCT) {
        if (field->kind == AST_CONSTRUCT_FIELD) {
            int idx = struct_type_find_field(
                st, ((const ast_construct_field_t *)field)->name);
            if (idx >= 0) return struct_type_field(st, (size_t)idx)->type;
        }
        if (index < struct_type_field_count(st)) {
            const struct_field_t *sf = struct_type_field(st, index);
            return sf ? sf->type : NULL;
        }
        return NULL;
    }
    if (type_kind_of(st) == TYPE_KIND_ARRAY) return array_type_elem(st);
    if (type_kind_of(st) == TYPE_KIND_TUPLE) {
        const tuple_elem_t *te = tuple_type_elem(st, index);
        return te ? te->type : NULL;
    }
    return NULL;
}

static cir_expr_t *lower_bitfield_construct(lower_ctx_t *lc,
                                            ast_construct_t *c) {
    const type_t *t =
        c->type ? sema_resolve_type_slot(lc->sema, &c->type) : NULL;
    const ast_struct_def_t *bsd = bitfield_def_of(lc, t);
    if (!bsd) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)c),
                   "c backend: bitfield metadata missing");
        lc->failed = true;
        return NULL;
    }
    cir_type_t *ct = map_type(lc, t);
    if (!ct) return NULL;
    size_t i = 0;
    bool all_const = true;
    for (ast_node_t *f = c->bit_fields; f; f = f->next) {
        if (f->kind != AST_CONSTRUCT_FIELD ||
            !((ast_construct_field_t *)f)->value ||
            ((ast_construct_field_t *)f)->value->kind != AST_INT_LIT) {
            all_const = false;
            break;
        }
    }
    if (all_const) {
        /* 常量构造 → 直接写隐藏字的常量初始化器：可用于全局变量，
           不引入运行期辅助函数。 */
        size_t wc = bsd->bitfield_word_count;
        uint64_t *words = (uint64_t *)arena_calloc(
            lc->arena, wc ? wc : 1, sizeof(uint64_t), ALIGNOF(max_align_t));
        cir_type_t *arr = ct->fields ? ct->fields->type : NULL;
        if (!words || !arr) return NULL;
        i = 0;
        for (ast_node_t *f = c->bit_fields; f; f = f->next, i++) {
            if (i >= bsd->bitfield_field_count) break;
            const ast_bitfield_field_t *bf = &bsd->bitfield_fields[i];
            uint64_t raw =
                ((ast_int_lit_t *)((ast_construct_field_t *)f)->value)->value;
            bf_pack(words, bsd->bitfield_word_bits, bf->offset, bf->width, raw);
        }
        cir_expr_t *arrc = cir_expr_construct(lc->arena, arr, NULL);
        if (!arrc) return NULL;
        for (size_t k = 0; k < wc; k++) {
            uint64_t w = words[k] & bf_mask(bsd->bitfield_word_bits);
            cir_init_t *wi =
                cir_init_new(lc->arena, NULL,
                             cir_expr_int(lc->arena, (int64_t)w));
            if (!wi) return NULL;
            cir_expr_add_init(arrc, wi);
        }
        cir_expr_t *out = cir_expr_construct(lc->arena, ct, NULL);
        cir_init_t *hi = cir_init_new(lc->arena,
                                      bitfield_storage_field(lc, ct), arrc);
        if (!out || !hi) return NULL;
        cir_expr_add_init(out, hi);
        return out;
    }

    lc->unit->uses_bitfield = true;
    ct->needs_bf_make = true;
    cir_expr_t *out = cir_expr_bitfield_make(lc->arena, ct,
                                             bsd->bitfield_word_bits, NULL);
    if (!out) return NULL;
    i = 0;
    for (ast_node_t *f = c->bit_fields; f; f = f->next, i++) {
        if (f->kind != AST_CONSTRUCT_FIELD) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, f),
                       "c backend: malformed bitfield constructor");
            lc->failed = true;
            return NULL;
        }
        if (i >= bsd->bitfield_field_count) break;
        const ast_bitfield_field_t *bf = &bsd->bitfield_fields[i];
        cir_expr_t *v = lower_expr(lc, ((ast_construct_field_t *)f)->value);
        if (!v) return NULL;
        cir_init_t *init = cir_init_new(lc->arena, NULL, v);
        if (!init) return NULL;
        init->bit_offset = bf->offset;
        init->bit_width = bf->width;
        cir_expr_add_init(out, init);
    }
    return out;
}

/* ========================== end bitfield ========================== */

cir_expr_t *lower_call(lower_ctx_t *lc, ast_call_t *call);

/* 结构等价但 type_t 不同的复合值（匿名推断结构 vs 声明类型）：按布局
   重解释成目标类型（C 不允许 struct 间 cast）。 */
static cir_expr_t *lower_compat_value(lower_ctx_t *lc, const type_t *target,
                                      ast_node_t *expr) {
    cir_expr_t *v = lower_expr(lc, expr);
    if (!v || !target) return v;
    const type_t *vt = infer_expr_type(lc, expr);
    if (!vt) return v;
    const type_t *tt = type_strip_quals(target);
    const type_t *vs = type_strip_quals(vt);
    if (tt == vs) return v;
    type_kind_t kt = type_kind_of(tt), kv = type_kind_of(vs);
    if (kt != kv) return v;
    if (kt != TYPE_KIND_STRUCT && kt != TYPE_KIND_TUPLE &&
        kt != TYPE_KIND_CUNION)
        return v;
    cir_type_t *ct = map_type(lc, tt);
    cir_type_t *st = map_type(lc, vs);
    if (!ct || !st) return v;
    if (ct->c_name && st->c_name && strcmp(ct->c_name, st->c_name) == 0)
        return v;
    return cir_expr_bitcast(lc->arena, ct, st, v);
}

cir_expr_t *lower_call(lower_ctx_t *lc, ast_call_t *call) {
    strslice_t cname0 = STRSLICE_EMPTY;
    if (call->callee && call->callee->kind == AST_IDENT)
        cname0 = ((ast_ident_t *)call->callee)->name;
    else if (call->callee && call->callee->kind == AST_FUNC_REF) {
        ast_func_ref_t *ref0 = (ast_func_ref_t *)call->callee;
        cname0 = ref0->name;
        /* sema 改写的 FUNC_REF（内建/实例化调用）只有 fid：按 id 取名字，
           这样 `assert` 等内建仍能命中下面的特判。 */
        if (!cname0.ptr && ref0->fid) {
            sema_func_t *sf = sema_func_by_id(lc->sema, ref0->fid);
            if (sf && sf->def && sf->def->kind == AST_FUNC_DEF)
                cname0 = ((ast_func_def_t *)sf->def)->name;
        }
    }
    /* M6 FFI：extern cfunc 调用 → 直接调用真实 C 符号；data(s) → 取首元素指针；
       cfunc_ptr(f) → 生成 C trampoline。 */
    if (call->is_ffi_call) return lower_ffi_call(lc, call);
    if (call->is_data_call) return lower_data_call(lc, call);
    if (call_is_cfunc_ptr(call)) return lower_cfunc_ptr(lc, call);
    /* 跨模块泛型实例调用：sema 已把 callee 改写成实例 FUNC_REF（实例体在
       定义模块的 prog 里），按 fid 在**所有模块**里找回定义模块。 */
    if (call->callee && call->callee->kind == AST_FUNC_REF) {
        ast_func_ref_t *ref = (ast_func_ref_t *)call->callee;
        module_t *owner = NULL;
        sema_func_t *sf = ref->fid ? sema_func_by_id(lc->sema, ref->fid) : NULL;
        if (sf) {
            if (sf->def && sf->def->kind == AST_FUNC_DEF)
                owner = (module_t *)((ast_func_def_t *)sf->def)->owner_module;
        } else if (ref->fid) {
            sf = lower_find_func_module(lc, ref->fid, &owner);
        }
        if (sf && sf->def && sf->def->kind == AST_FUNC_DEF) {
            ast_func_def_t *df = (ast_func_def_t *)sf->def;
            if (!owner) owner = (module_t *)df->owner_module;
            if (owner && lc->sema->current_module &&
                owner != lc->sema->current_module) {
                cir_expr_t *r = lower_cross_module_func_call(lc, call, df, sf);
                if (r || lc->failed) return r;
            }
        }
    }
    /* test 上下文的 `assert(cond, text)`：sema 已把它改写为 (bool, str) 调用，
       这里映射到运行器生成的 clux_assert（失败即退出该用例子进程）。 */
    if ((lc->unit->test_runner || lc->unit->tests) &&
        strslice_eq(cname0, STRSLICE_LIT("assert"))) {
        lc->unit->uses_assert = true;
        cir_expr_t *expr = cir_expr_call(lc->arena, "clux_assert", NULL);
        if (!expr) return NULL;
        for (ast_node_t *a = call->args; a; a = a->next) {
            cir_expr_t *arg = lower_call_arg(lc, a);
            if (!arg) return NULL;
            cir_expr_append_arg(expr, arg);
        }
        return expr;
    }
    if (strslice_eq(cname0, STRSLICE_LIT("__deep_clone")) && call->args &&
        !call->args->next) {
        const type_t *ft = infer_expr_type(lc, call->args);
        if (ft && type_kind_of(type_strip_quals(ft)) == TYPE_KIND_FUNC) {
            cir_expr_t *obj = lower_expr(lc, call->args);
            if (!obj) return NULL;
            cir_expr_t *cl = cir_expr_func_clone(lc->arena, obj);
            if (cl) cl->type = map_type(lc, ft);
            return cl;
        }
    }
    if (call->callee && call->callee->kind == AST_SCOPE_REF) {
        ast_scope_ref_t *sr = (ast_scope_ref_t *)call->callee;
        /* 跨模块调用 `mod::fn(...)`。 */
        if (is_import_scope_ref(lc, sr)) return lower_import_call(lc, call, sr);
        uint32_t owner = call->member_type_id;
        if (!owner && sr->type_expr) {
            const type_t *t = sema_resolve_type_slot(lc->sema, &sr->type_expr);
            if (t) {
                const sema_type_t *st = sema_type_find(lc->sema, t);
                if (st) owner = st->id;
            }
        }
        lower_method_t *m = find_method(lc, owner, sr->variant);
        lower_method_t xm;
        if (!m && owner) m = lower_cross_module_method(lc, owner, sr->variant,
                                                       call->callee, &xm);
        if (!m) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                       "c backend: internal: method '%.*s' has no materialized "
                       "instance (sema should have instantiated the generic "
                       "type/method)",
                       (int)sr->variant.len, sr->variant.ptr);
            lc->failed = true;
            return NULL;
        }
        {
            cir_expr_t *out = cir_expr_call(lc->arena, m->cname, NULL);
            cir_expr_append_arg(out, cir_expr_ident(lc->arena, "NULL"));
            for (ast_node_t *a = call->args; a; a = a->next) {
                cir_expr_t *arg = lower_call_arg(lc, a);
                if (!arg) return NULL;
                cir_expr_append_arg(out, arg);
            }
            return out;
        }
    }
    if (call->is_member_call && call->callee &&
        call->callee->kind == AST_MEMBER) {
        ast_member_t *mem = (ast_member_t *)call->callee;
        lower_method_t *m =
            find_method(lc, call->member_type_id, mem->field);
        lower_method_t xm;
        if (!m && call->member_type_id)
            m = lower_cross_module_method(lc, call->member_type_id, mem->field,
                                          call->callee, &xm);
        if (!m) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                       "c backend: internal: method '%.*s' has no materialized "
                       "instance (sema should have instantiated the generic "
                       "type/method)",
                       (int)mem->field.len, mem->field.ptr);
            lc->failed = true;
            return NULL;
        }
        cir_expr_t *recv = lower_expr(lc, mem->object);
        if (!recv) return NULL;
        const type_t *rt = infer_expr_type(lc, mem->object);
        const type_t *self_t = NULL;
        ast_var_def_t *self_vd = (ast_var_def_t *)m->def->params;
        if (self_vd) {
            self_t = self_vd->resolved_type;
            if (!self_t && self_vd->type_expr)
                self_t = sema_resolve_type_slot(lc->sema, &self_vd->type_expr);
        }
        if (self_t && type_kind_of(type_strip_quals(self_t)) == TYPE_KIND_PTR &&
            (!rt || type_kind_of(type_strip_quals(rt)) != TYPE_KIND_PTR)) {
            /* ref *T 形参：右值接收者（如 `f().m()`）先物化成临时左值，
               否则 `&(call())` 在 C 里非法。 */
            if (!ast_is_lvalue(mem->object)) {
                cir_type_t *rct = rt ? map_type(lc, rt) : NULL;
                if (!rct) return NULL;
                recv = cir_expr_temp_lvalue(lc->arena, rct, recv);
            }
            recv = cir_expr_unary(lc->arena, "&", recv);
        }
        cir_expr_t *out = cir_expr_call(lc->arena, m->cname, NULL);
        cir_expr_append_arg(out, cir_expr_ident(lc->arena, "NULL"));
        cir_expr_append_arg(out, recv);
        for (ast_node_t *a = call->args; a; a = a->next) {
            cir_expr_t *arg = lower_call_arg(lc, a);
            if (!arg) return NULL;
            cir_expr_append_arg(out, arg);
        }
        return out;
    }
    /* 函数值调用：callee 是 func 类型变量/表达式。 */
    if (call->callee && call->callee->kind == AST_IDENT) {
        const type_t *ft = lookup_local(
            lc, ((ast_ident_t *)call->callee)->name);
        if (ft && type_kind_of(type_strip_quals(ft)) == TYPE_KIND_FUNC) {
            cir_expr_t *callee = lower_expr(lc, call->callee);
            if (!callee) return NULL;
            cir_expr_t *out = cir_expr_func_call(lc->arena, callee, NULL);
            for (ast_node_t *a = call->args; a; a = a->next) {
                cir_expr_t *arg = lower_call_arg(lc, a);
                if (!arg) return NULL;
                cir_expr_append_arg(out, arg);
            }
            return out;
        }
    }
    strslice_t name = STRSLICE_EMPTY;
    if (call->callee && call->callee->kind == AST_IDENT) {
        name = ((ast_ident_t *)call->callee)->name;
    } else if (call->callee && call->callee->kind == AST_FUNC_REF) {
        ast_func_ref_t *ref = (ast_func_ref_t *)call->callee;
        name = ref->name;
        if (!name.ptr || name.len == 0) {
            sema_func_t *sf = sema_func_by_id(lc->sema, ref->fid);
            if (sf && sf->def && sf->def->kind == AST_FUNC_DEF)
                name = ((ast_func_def_t *)sf->def)->name;
        }
    }
    if (!name.ptr) {
        /* 典型缺口：跨模块泛型调用 `mod::f[i32](...)`（实例由 sema 追加到
           定义模块的 prog，调用点形态是 AST_INDEX/泛型引用）。 */
        if (call->callee && call->callee->kind == AST_INDEX) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                       "c backend: cross-module generic calls are not "
                       "lowered yet (instantiate on the defining module side "
                       "or call a non-generic wrapper)");
        } else {
            diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                       "c backend: only direct function calls are supported");
        }
        lc->failed = true;
        return NULL;
    }
    const char *cname = module_symbol(lc->arena, lc->unit, "fn_", name);
    if (!cname) return NULL;
    cir_expr_t *expr = cir_expr_call(lc->arena, cname, NULL);
    if (!expr) return NULL;
    /* env-first ABI：直接调用传 NULL。 */
    cir_expr_append_arg(expr, cir_expr_ident(lc->arena, "NULL"));
    for (ast_node_t *a = call->args; a; a = a->next) {
        cir_expr_t *arg = lower_call_arg(lc, a);
        if (!arg) return NULL;
        cir_expr_append_arg(expr, arg);
    }
    return expr;
}

cir_expr_t *lower_expr(lower_ctx_t *lc, ast_node_t *e) {
    if (!e || lc->failed) return NULL;
    switch (e->kind) {
    case AST_INT_LIT:
        return cir_expr_int(lc->arena, (int64_t)((ast_int_lit_t *)e)->value);
    case AST_FLOAT_LIT:
        return cir_expr_float(lc->arena, ((ast_float_lit_t *)e)->value);
    case AST_BOOL_LIT:
        return cir_expr_bool(lc->arena, ((ast_bool_lit_t *)e)->value);
    case AST_CHAR_LIT:
        return cir_expr_int(lc->arena, ((ast_char_lit_t *)e)->value);
    case AST_SCOPE_REF: {
        ast_scope_ref_t *sr = (ast_scope_ref_t *)e;
        /* extern var 读取：`td::td_i32` → 库里的数据符号（str 包成 clux_str）。 */
        if (sr->is_ffi_var) return lower_ffi_var_read(lc, sr);
        /* 跨模块全局变量读取：`math::PI`。 */
        if (is_import_scope_ref(lc, sr)) return lower_import_var_read(lc, sr);
        /* 枚举 variant `Color::Red`：sema 折叠后 type_expr 是 AST_TYPE_REF，
           `value` 是底层整数值。enum 在 C 侧就是底层整数 → 直接出字面量。 */
        const type_t *t = NULL;
        if (sr->type_expr && sr->type_expr->kind == AST_TYPE_REF)
            t = resolve_type_expr(lc->sema, sr->type_expr);
        if (t && type_kind_of(t) == TYPE_KIND_ENUM) {
            cir_type_t *ct = map_type(lc, t);
            if (!ct) return NULL;
            cir_expr_t *lit = cir_expr_int(lc->arena, sr->value);
            if (!lit) return NULL;
            /* 底层宽度/符号归一：`Small: u8 { B = 200u8 }` 的折叠值是
               int64（可能按有符号符号扩展成 -56）；C 里 enum 就是底层整数，
               统一转成底层 C 类型，比较/存储与 VM 的字节语义一致。 */
            cir_expr_t *cast = cir_expr_cast(lc->arena, ct, lit);
            if (!cast) return NULL;
            cast->type = ct;
            return cast;
        }
        diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                   "c backend: unsupported `::` reference '%.*s' (only enum "
                   "variants are supported; struct static attributes are not "
                   "implemented yet)",
                   (int)sr->variant.len, sr->variant.ptr);
        lc->failed = true;
        return NULL;
    }
    case AST_STRING_LIT: {
        ast_string_lit_t *s = (ast_string_lit_t *)e;
        if (!map_type(lc, lc->sema->vm->type_str)) return NULL;
        char *data = (char *)arena_alloc(lc->arena, s->text.len + 1, 1);
        if (!data) return NULL;
        if (s->text.len) memcpy(data, s->text.ptr, s->text.len);
        data[s->text.len] = '\0';
        return cir_expr_string(lc->arena, data, s->text.len);
    }
    case AST_IDENT: {
        strslice_t name = ((ast_ident_t *)e)->name;
        const char *lc_name = lookup_local_cname(lc, name);
        if (lc_name) return cir_expr_ident(lc->arena, lc_name);
        for (lower_capture_t *c = lc->captures; c; c = c->next)
            if (c->name.len == name.len &&
                memcmp(c->name.ptr, name.ptr, name.len) == 0)
                return cir_expr_ident(lc->arena, c->expr);
        const char *n = module_symbol(lc->arena, lc->unit, "v_", name);
        return n ? cir_expr_ident(lc->arena, n) : NULL;
    }
    case AST_UNARY: {
        ast_unary_t *u = (ast_unary_t *)e;
        const char *op = token_text(lc->arena, u->op);
        cir_expr_t *v = lower_expr(lc, u->operand);
        return op && v ? cir_expr_unary(lc->arena, op, v) : NULL;
    }
    case AST_BINARY: {
        ast_binary_t *b = (ast_binary_t *)e;
        if (token_is(b->op, "as")) {
            /* 显式类型转换：rhs 已被 sema 折叠为类型槽位（AST_TYPE_REF）。 */
            const type_t *target = sema_resolve_type_slot(lc->sema, &b->rhs);
            if (!target) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: 'as' target is not a type");
                lc->failed = true;
                return NULL;
            }
            cir_type_t *ct = map_type(lc, target);
            cir_expr_t *v = lower_expr(lc, b->lhs);
            return v && ct ? cir_expr_cast(lc->arena, ct, v) : NULL;
        }
        const type_t *lt = infer_expr_type(lc, b->lhs);
        const type_t *rt = infer_expr_type(lc, b->rhs);
        if (token_is(b->op, "is")) {
            const type_t *ut = infer_expr_type(lc, b->lhs);
            if (!ut || type_kind_of(type_strip_quals(ut)) != TYPE_KIND_UNION) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: 'is' requires a union operand");
                lc->failed = true;
                return NULL;
            }
            int tag = -1;
            if (b->rhs->kind == AST_IDENT)
                tag = union_type_find_member(
                    type_strip_quals(ut), ((ast_ident_t *)b->rhs)->name);
            else if (b->rhs->kind == AST_INT_LIT)
                tag = (int)((ast_int_lit_t *)b->rhs)->value;
            if (tag < 0) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: unknown union tag");
                lc->failed = true;
                return NULL;
            }
            cir_expr_t *obj = lower_expr(lc, b->lhs);
            if (!obj) return NULL;
            return cir_expr_binary(lc->arena, "==",
                                   cir_expr_union_tag(lc->arena, obj),
                                   cir_expr_int(lc->arena, tag));
        }
        if ((b->lhs->kind == AST_NIL || b->rhs->kind == AST_NIL) &&
            (token_is(b->op, "==") || token_is(b->op, "!="))) {
            ast_node_t *other = b->lhs->kind == AST_NIL ? b->rhs : b->lhs;
            const type_t *ot = infer_expr_type(lc, other);
            if (!ot || type_kind_of(type_strip_quals(ot)) != TYPE_KIND_OPTION) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: nil comparison requires optional operand");
                lc->failed = true;
                return NULL;
            }
            cir_expr_t *obj = lower_expr(lc, other);
            return obj ? cir_expr_opt_is_some(lc->arena, obj,
                                              token_is(b->op, "!="))
                       : NULL;
        }
        if (lt && rt && (token_is(b->op, "==") || token_is(b->op, "!=")) &&
            (type_kind_of(type_strip_quals(lt)) == TYPE_KIND_CUNION ||
             type_kind_of(type_strip_quals(lt)) == TYPE_KIND_TUPLE ||
             type_kind_of(type_strip_quals(lt)) == TYPE_KIND_SLICE ||
             type_kind_of(type_strip_quals(lt)) == TYPE_KIND_FUNC)) {
            /* 复合值的判等：C 不能直接 `==` 结构体。
               cunion/元组 → memcmp；slice → (data,len)；func → (fn,env)。 */
            bool neg = token_is(b->op, "!=");
            type_kind_t k = type_kind_of(type_strip_quals(lt));
            cir_expr_t *l = lower_expr(lc, b->lhs);
            cir_expr_t *r = lower_expr(lc, b->rhs);
            if (!l || !r) return NULL;
            cir_expr_t *eq = NULL;
            if (k == TYPE_KIND_CUNION || k == TYPE_KIND_TUPLE) {
                cir_type_t *ct = map_type(lc, lt);
                if (!ct) return NULL;
                lc->unit->uses_mem_eq = true;
                eq = cir_expr_mem_eq(lc->arena, ct, l, r);
            } else if (k == TYPE_KIND_SLICE) {
                eq = cir_expr_binary(
                    lc->arena, "&&",
                    cir_expr_binary(lc->arena, "==",
                                    cir_expr_member(lc->arena, l, "data"),
                                    cir_expr_member(lc->arena, r, "data")),
                    cir_expr_binary(lc->arena, "==",
                                    cir_expr_member(lc->arena, l, "len"),
                                    cir_expr_member(lc->arena, r, "len")));
            } else {
                eq = cir_expr_binary(
                    lc->arena, "&&",
                    cir_expr_binary(lc->arena, "==",
                                    cir_expr_member(lc->arena, l, "fn"),
                                    cir_expr_member(lc->arena, r, "fn")),
                    cir_expr_binary(lc->arena, "==",
                                    cir_expr_member(lc->arena, l, "env"),
                                    cir_expr_member(lc->arena, r, "env")));
            }
            if (!eq) return NULL;
            return neg ? cir_expr_unary(lc->arena, "!", eq) : eq;
        }
        if (lt && rt && type_strip_quals(lt)->kind == TYPE_KIND_STR &&
            type_strip_quals(rt)->kind == TYPE_KIND_STR &&
            (token_is(b->op, "==") || token_is(b->op, "!="))) {
            lc->unit->uses_str_cmp = true;
            cir_expr_t *l = lower_expr(lc, b->lhs);
            cir_expr_t *r = lower_expr(lc, b->rhs);
            if (!l || !r) return NULL;
            cir_expr_t *call = cir_expr_call(lc->arena, "clux_str_eq", NULL);
            cir_expr_append_arg(call, l);
            cir_expr_append_arg(call, r);
            if (token_is(b->op, "!="))
                call = cir_expr_unary(lc->arena, "!", call);
            return call;
        }
        const char *op = token_text(lc->arena, b->op);
        cir_expr_t *lhs = lower_expr(lc, b->lhs);
        cir_expr_t *rhs = lower_expr(lc, b->rhs);
        return op && lhs && rhs ? cir_expr_binary(lc->arena, op, lhs, rhs)
                                : NULL;
    }
    case AST_CALL:
        if (call_is_len((ast_call_t *)e)) return lower_len(lc, (ast_call_t *)e);
        return lower_call(lc, (ast_call_t *)e);
    case AST_IS_TAG: {
        ast_is_tag_t *it = (ast_is_tag_t *)e;
        cir_expr_t *obj = lower_expr(lc, it->object);
        if (!obj) return NULL;
        return cir_expr_binary(lc->arena, "==",
                               cir_expr_union_tag(lc->arena, obj),
                               cir_expr_int(lc->arena, it->tag));
    }
    case AST_OPT_IS_SOME: {
        ast_opt_is_some_t *os = (ast_opt_is_some_t *)e;
        cir_expr_t *obj = lower_expr(lc, os->object);
        return obj ? cir_expr_opt_is_some(lc->arena, obj, os->want_some)
                   : NULL;
    }
    case AST_UNWRAP: {
        ast_unwrap_t *uw = (ast_unwrap_t *)e;
        cir_expr_t *obj = lower_expr(lc, uw->operand);
        if (!obj) return NULL;
        lc->unit->uses_opt_unwrap = true;
        return cir_expr_unwrap(lc->arena, obj);
    }
    case AST_NIL:
        diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                   "c backend: nil requires an optional target type "
                   "(use .?T{nil})");
        lc->failed = true;
        return NULL;
    case AST_FUNC_REF: {
        ast_func_ref_t *ref = (ast_func_ref_t *)e;
        ast_func_def_t *fd = func_def_of_ref(lc, ref);
        strslice_t name = ref->name;
        if ((!name.ptr || name.len == 0) && fd) name = fd->name;
        if (!name.ptr) return NULL;
        const char *obj = module_symbol(lc->arena, lc->unit, "obj_", name);
        const type_t *ft = func_type_of_def(lc, fd);
        cir_type_t *ct = ft ? map_type(lc, ft) : NULL;
        return obj && ct ? cir_expr_func_obj(lc->arena, obj, ct) : NULL;
    }
    case AST_FUNC_DEF:
        return lower_func_literal(lc, (ast_func_def_t *)e);
    case AST_MOVE:
        return lower_expr(lc, ((ast_move_t *)e)->operand);
    case AST_TAKE:
        return lower_expr(lc, ((ast_take_t *)e)->operand);
    case AST_SLICE:
        return lower_slice(lc, (ast_slice_t *)e);
    case AST_MAKE:
        return lower_make(lc, (ast_make_t *)e);
    case AST_MEMBER: {
        ast_member_t *m = (ast_member_t *)e;
        if (m->is_bitfield) return lower_bitfield_read(lc, m);
        if (m->object && m->object->kind == AST_IDENT &&
            strslice_eq(((ast_ident_t *)m->object)->name,
                        STRSLICE_LIT("__ctx")) &&
            strslice_eq(m->field, STRSLICE_LIT("env")))
            return cir_expr_ident(
                lc->arena,
                lc->ctx_env_name ? lc->ctx_env_name : "clux_v___ctx_env");
        cir_expr_t *obj = lower_expr(lc, m->object);
        if (!obj) return NULL;
        const type_t *ot = infer_expr_type(lc, m->object);
        if (ot) ot = type_strip_quals(ot);
        if (ot && type_kind_of(ot) == TYPE_KIND_UNION) {
            int mi = union_type_find_member(ot, m->field);
            const union_member_t *mem =
                mi >= 0 ? union_type_member(ot, (size_t)mi) : NULL;
            if (!mem) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: unknown union member '%.*s'",
                           (int)m->field.len, m->field.ptr);
                lc->failed = true;
                return NULL;
            }
            char fb[256];
            snprintf(fb, sizeof fb, "clux_m_%.*s", (int)mem->name.len,
                     mem->name.ptr ? mem->name.ptr : "");
            cir_type_t *mct =
                mem->payload_type ? map_type(lc, mem->payload_type) : NULL;
            if (!mct) {
                lc->failed = true;
                return NULL;
            }
            lc->unit->uses_union_check = true;
            cir_expr_t *ua = cir_expr_union_at(lc->arena, map_type(lc, ot), mct,
                                               obj, (size_t)mi,
                                               copy_arena(lc->arena, fb));
            /* 左值接收者：必须是可写左值（`s.m = v`），用直接成员访问 + tag
               检查（复合字面量临时量只是读副本，写进去会丢失）。 */
            if (ua) ua->array_data = ast_is_lvalue(m->object);
            return ua;
        }
        if (ot && type_kind_of(ot) == TYPE_KIND_CUNION) {
            int mi = cunion_type_find_member(ot, m->field);
            const cunion_member_t *mem =
                mi >= 0 ? cunion_type_member(ot, (size_t)mi) : NULL;
            if (!mem) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: unknown cunion member '%.*s'",
                           (int)m->field.len, m->field.ptr);
                lc->failed = true;
                return NULL;
            }
            char fb[256];
            snprintf(fb, sizeof fb, "clux_m_%.*s", (int)mem->name.len,
                     mem->name.ptr ? mem->name.ptr : "");
            return cir_expr_union_member(lc->arena, obj,
                                         copy_arena(lc->arena, fb));
        }
        char fname[256];
        snprintf(fname, sizeof fname, "clux_f_%.*s", (int)m->field.len,
                 m->field.ptr ? m->field.ptr : "");
        return cir_expr_member(lc->arena, obj,
                               copy_arena(lc->arena, fname));
    }
    case AST_INDEX: {
        ast_index_t *ix = (ast_index_t *)e;
        const type_t *ot = infer_expr_type(lc, ix->object);
        const type_t *ost = ot ? type_strip_quals(ot) : NULL;
        /* 元组下标：编译期常量 → 成员 `clux_f_<i>`（读写都是左值）。 */
        if (ost && type_kind_of(ost) == TYPE_KIND_TUPLE) {
            int64_t ti = 0;
            if (ix->indices && ix->indices->kind == AST_INT_LIT)
                ti = ((ast_int_lit_t *)ix->indices)->value;
            else {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: tuple index must be a constant");
                lc->failed = true;
                return NULL;
            }
            cir_expr_t *obj = lower_expr(lc, ix->object);
            if (!obj) return NULL;
            char fname[32];
            snprintf(fname, sizeof fname, "clux_f_%lld", (long long)ti);
            return cir_expr_member(lc->arena, obj,
                                   copy_arena(lc->arena, fname));
        }
        cir_expr_t *obj = lower_expr(lc, ix->object);
        cir_expr_t *idx = lower_expr(lc, ix->indices);
        if (!obj || !idx) return NULL;
        /* 越界保护（与 VM 一致：OOB 视为 panic）：
           - 数组长度是类型常量，索引包成 clux_index_check(idx, N)；
           - 切片：接收者先落复合字面量临时量（只求值一次），
             再由 clux_slice_at 做越界检查并返回元素地址；
           - 指针下标是 C 语义，不检查。 */
        if (ost && type_kind_of(ost) == TYPE_KIND_ARRAY) {
            lc->unit->uses_index_check = true;
            cir_expr_t *chk =
                cir_expr_call(lc->arena, "clux_index_check", NULL);
            if (!chk) return NULL;
            cir_expr_append_arg(chk, idx);
            cir_expr_append_arg(chk, cir_expr_int(
                                         lc->arena,
                                         (int64_t)array_type_len(ost)));
            idx = chk;
        } else if (ost && type_kind_of(ost) == TYPE_KIND_SLICE) {
            /* 切片：接收者先落到复合字面量临时量（只求值一次），再由
               clux_slice_at 做越界检查并返回元素地址（左值，读写通用）。 */
            cir_type_t *sct = map_type(lc, ost);
            if (!sct) {
                lc->failed = true;
                return NULL;
            }
            lc->unit->uses_slice_check = true;
            cir_expr_t *at = cir_expr_slice_at(lc->arena, sct, obj, idx);
            if (!at) return NULL;
            return at;
        }
        return cir_expr_index(lc->arena, obj, idx, true);
    }
    case AST_DEREF: {
        ast_deref_t *d = (ast_deref_t *)e;
        cir_expr_t *v = lower_expr(lc, d->operand);
        return v ? cir_expr_unary(lc->arena, "*", v) : NULL;
    }
    case AST_ADDR: {
        ast_addr_t *a = (ast_addr_t *)e;
        cir_expr_t *v = lower_expr(lc, a->operand);
        return v ? cir_expr_unary(lc->arena, "&", v) : NULL;
    }
    case AST_CONSTRUCT: {
        ast_construct_t *c = (ast_construct_t *)e;
        if (c->is_bitfield) return lower_bitfield_construct(lc, c);
        if (!c->type) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                       "c backend: anonymous construction requires a type");
            lc->failed = true;
            return NULL;
        }
        const type_t *t = sema_resolve_type_slot(lc->sema, &c->type);
        if (t && type_kind_of(t) == TYPE_KIND_OPTION) {
            cir_type_t *ct = map_type(lc, t);
            if (!ct) return NULL;
            ast_node_t *fv = c->fields;
            if (fv && fv->kind == AST_CONSTRUCT_FIELD)
                fv = ((ast_construct_field_t *)fv)->value;
            if (!fv || fv->kind == AST_NIL)
                return cir_expr_option(lc->arena, ct, false, NULL);
            cir_expr_t *val = lower_expr(lc, fv);
            return val ? cir_expr_option(lc->arena, ct, true, val) : NULL;
        }
        if (t && type_kind_of(t) == TYPE_KIND_CUNION) {
            cir_type_t *ct = map_type(lc, t);
            const cunion_member_t *mem = cunion_type_member(t, c->member_tag);
            if (!ct || !mem) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: invalid cunion constructor");
                lc->failed = true;
                return NULL;
            }
            ast_node_t *fv = c->fields;
            if (fv && fv->kind == AST_CONSTRUCT_FIELD)
                fv = ((ast_construct_field_t *)fv)->value;
            cir_expr_t *val = lower_expr(lc, fv);
            if (!val) return NULL;
            char fb[256];
            snprintf(fb, sizeof fb, "clux_m_%.*s", (int)mem->name.len,
                     mem->name.ptr ? mem->name.ptr : "");
            return cir_expr_cunion_construct(lc->arena, ct,
                                             copy_arena(lc->arena, fb), val);
        }
        if (t && type_kind_of(t) == TYPE_KIND_UNION) {
            cir_type_t *ct = map_type(lc, t);
            const union_member_t *mem = union_type_member(t, c->member_tag);
            if (!ct || !mem) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                           "c backend: invalid union constructor");
                lc->failed = true;
                return NULL;
            }
            ast_node_t *fv = c->fields;
            if (fv && fv->kind == AST_CONSTRUCT_FIELD)
                fv = ((ast_construct_field_t *)fv)->value;
            cir_expr_t *val = lower_expr(lc, fv);
            if (!val) return NULL;
            char fb[256];
            snprintf(fb, sizeof fb, "clux_m_%.*s", (int)mem->name.len,
                     mem->name.ptr ? mem->name.ptr : "");
            return cir_expr_union_construct(lc->arena, ct, mem->tag,
                                            copy_arena(lc->arena, fb), val);
        }
        cir_type_t *ct = map_type(lc, t);
        if (!ct) return NULL;
        cir_expr_t *out = cir_expr_construct(lc->arena, ct, NULL);
        if (!out) return NULL;
        bool positional = (t && type_kind_of(t) != TYPE_KIND_STRUCT);
        size_t fi = 0;
        for (ast_node_t *f = c->fields; f; f = f->next, fi++) {
            const char *fname = NULL;
            ast_node_t *val = f;
            if (f->kind == AST_CONSTRUCT_FIELD) {
                ast_construct_field_t *cf = (ast_construct_field_t *)f;
                val = cf->value;
                if (!positional) {
                    char fb[256];
                    snprintf(fb, sizeof fb, "clux_f_%.*s", (int)cf->name.len,
                             cf->name.ptr ? cf->name.ptr : "");
                    fname = copy_arena(lc->arena, fb);
                }
            }
            const type_t *ft = ctor_field_type(t, f, fi);
            cir_expr_t *ve = lower_ctor_value(lc, ft, val);
            if (!ve) return NULL;
            cir_init_t *init = cir_init_new(lc->arena, fname, ve);
            if (!init) return NULL;
            cir_expr_add_init(out, init);
        }
        return out;
    }
    case AST_NEW: {
        ast_new_t *n = (ast_new_t *)e;
        if (!n->type) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                       "c backend: anonymous 'new' requires a type");
            lc->failed = true;
            return NULL;
        }
        const type_t *t = sema_resolve_type_slot(lc->sema, &n->type);
        cir_type_t *ct = map_type(lc, t);
        if (!ct) return NULL;
        lc->unit->uses_alloc = true;
        cir_expr_t *out = cir_expr_new(lc->arena, ct, NULL);
        if (!out) return NULL;
        bool positional = (t && type_kind_of(t) != TYPE_KIND_STRUCT);
        size_t fi = 0;
        for (ast_node_t *f = n->fields; f; f = f->next, fi++) {
            const char *fname = NULL;
            ast_node_t *val = f;
            if (f->kind == AST_CONSTRUCT_FIELD) {
                ast_construct_field_t *cf = (ast_construct_field_t *)f;
                val = cf->value;
                if (!positional) {
                    char fb[256];
                    snprintf(fb, sizeof fb, "clux_f_%.*s", (int)cf->name.len,
                             cf->name.ptr ? cf->name.ptr : "");
                    fname = copy_arena(lc->arena, fb);
                }
            }
            const type_t *ft = ctor_field_type(t, f, fi);
            cir_expr_t *ve = lower_ctor_value(lc, ft, val);
            if (!ve) return NULL;
            cir_init_t *init = cir_init_new(lc->arena, fname, ve);
            if (!init) return NULL;
            cir_expr_add_init(out, init);
        }
        return out;
    }
    case AST_TERNARY: {
        ast_ternary_t *t = (ast_ternary_t *)e;
        cir_expr_t *cond = lower_expr(lc, t->cond);
        cir_expr_t *then_e = lower_expr(lc, t->then_branch);
        cir_expr_t *else_e = lower_expr(lc, t->else_branch);
        return cond && then_e && else_e
                   ? cir_expr_ternary(lc->arena, cond, then_e, else_e)
                   : NULL;
    }
    case AST_ASSIGN: {
        ast_assign_t *a = (ast_assign_t *)e;
        const char *op = a->op ? token_text(lc->arena, a->op) : "=";
        /* `_ = expr`（discard 赋值）：只求值表达式。 */
        if (a->target && a->target->kind == AST_IDENT &&
            ((ast_ident_t *)a->target)->name.len == 1 &&
            ((ast_ident_t *)a->target)->name.ptr &&
            ((ast_ident_t *)a->target)->name.ptr[0] == '_')
            return lower_expr(lc, a->value);
        if (a->target && a->target->kind == AST_MEMBER &&
            ((ast_member_t *)a->target)->is_bitfield)
            return lower_bitfield_assign(lc, (ast_member_t *)a->target, op,
                                         a->value);
        /* extern var 写：C 符号是普通全局变量；str 目标取 `.data`。 */
        if (a->target && a->target->kind == AST_SCOPE_REF &&
            ((ast_scope_ref_t *)a->target)->is_ffi_var) {
            bool is_str = false;
            const char *cname = lower_ffi_var_target(
                lc, (ast_scope_ref_t *)a->target, &is_str);
            if (!cname) return NULL;
            cir_expr_t *lhs = cir_expr_ident(lc->arena, cname);
            cir_expr_t *value = lower_expr(lc, a->value);
            if (!lhs || !value) return NULL;
            if (is_str) {
                if (strcmp(op, "=") != 0) {
                    diag_error(lc->sema->diag,
                               sema_loc(lc->sema, (ast_node_t *)e),
                               "c backend: extern `str` variable does not "
                               "support compound assignment");
                    lc->failed = true;
                    return NULL;
                }
                value = cir_expr_member(lc->arena, value, "data");
            }
            return cir_expr_assign(lc->arena, lhs, op, value);
        }
        /* 跨模块全局变量写：`math::PI = v`。 */
        if (a->target && a->target->kind == AST_SCOPE_REF &&
            is_import_scope_ref(lc, (ast_scope_ref_t *)a->target)) {
            const char *cname = lower_import_var_target(
                lc, (ast_scope_ref_t *)a->target);
            if (!cname) return NULL;
            cir_expr_t *lhs = cir_expr_ident(lc->arena, cname);
            cir_expr_t *value = lower_expr(lc, a->value);
            if (!lhs || !value) return NULL;
            return cir_expr_assign(lc->arena, lhs, op, value);
        }
        cir_expr_t *target = lower_expr(lc, a->target);
        cir_expr_t *value = lower_expr(lc, a->value);
        return target && op && value
                   ? cir_expr_assign(lc->arena, target, op, value)
                   : NULL;
    }
    default:
        diag_error(lc->sema->diag, sema_loc(lc->sema, e),
                   "c backend: unsupported expression");
        lc->failed = true;
        return NULL;
    }
}

