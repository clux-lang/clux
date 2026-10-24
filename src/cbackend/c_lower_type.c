#include "cbackend/c_lower_internal.h"

#include <stdio.h>
#include <string.h>

/* 模块内 C 类型/签名名前缀（`clux_<tag>_`）：多编译单元链接时同名类型
 * 不再冲突，且头文件里的类型名天然带模块身份。 */
static const char *type_prefix(lower_ctx_t *lc) {
    return (lc && lc->unit && lc->unit->sym_prefix) ? lc->unit->sym_prefix
                                                    : "clux_";
}

static bool same_sig(const cir_signature_t *s, cir_type_t *ret, size_t pc,
                     cir_type_t **pt) {
    if (!s || !s->ret || !ret) return false;
    if (strcmp(s->ret->c_name, ret->c_name) != 0) return false;
    if (s->param_count != pc) return false;
    for (size_t i = 0; i < pc; i++) {
        const char *a = s->params[i] ? s->params[i]->c_name : "void";
        const char *b = pt[i] ? pt[i]->c_name : "void";
        if (strcmp(a, b) != 0) return false;
    }
    return true;
}

cir_signature_t *signature_for(lower_ctx_t *lc, cir_type_t *ret,
                                      size_t pc, cir_type_t **pt) {
    for (cir_signature_t *s = lc->unit->signatures; s; s = s->next)
        if (same_sig(s, ret, pc, pt)) return s;
    char tag[64];
    snprintf(tag, sizeof tag, "%ssig_%u", type_prefix(lc), lc->sig_seq++);
    cir_signature_t *s = cir_signature_new(
        lc->arena, copy_arena(lc->arena, tag), ret, pc, pt);
    if (s) cir_unit_add_signature(lc->unit, s);
    return s;
}

cir_type_t *map_type(lower_ctx_t *lc, const type_t *t) {
    t = strip(lc->sema, t);
    if (!t) return cir_type_new(lc->arena, CIR_TYPE_VOID, "void");
    vm_t *vm = lc->sema->vm;
    if (t == vm->type_i32) return cir_type_new(lc->arena, CIR_TYPE_I32, "int32_t");
    if (t == vm->type_u32) return cir_type_new(lc->arena, CIR_TYPE_I32, "uint32_t");
    if (t == vm->type_usize) return cir_type_new(lc->arena, CIR_TYPE_USIZE, "size_t");
    if (t == vm->type_bool) return cir_type_new(lc->arena, CIR_TYPE_BOOL, "bool");
    if (t == vm->type_f32) return cir_type_new(lc->arena, CIR_TYPE_F32, "float");
    if (t == vm->type_f64) return cir_type_new(lc->arena, CIR_TYPE_F64, "double");
    if (t == vm->type_void) return cir_type_new(lc->arena, CIR_TYPE_VOID, "void");
    if (t == vm->type_u8) return cir_type_new(lc->arena, CIR_TYPE_I32, "uint8_t");
    if (t == vm->type_i8) return cir_type_new(lc->arena, CIR_TYPE_I32, "int8_t");
    if (t == vm->type_u16) return cir_type_new(lc->arena, CIR_TYPE_I32, "uint16_t");
    if (t == vm->type_i16) return cir_type_new(lc->arena, CIR_TYPE_I32, "int16_t");
    if (t == vm->type_u64) return cir_type_new(lc->arena, CIR_TYPE_I32, "uint64_t");
    if (t == vm->type_i64) return cir_type_new(lc->arena, CIR_TYPE_I32, "int64_t");
    if (t == vm->type_str) {
        cir_type_t *cached = cir_type_find(lc->unit, t, CIR_TYPE_STR);
        if (cached) return cached;
        cir_type_t *ct = cir_type_new(lc->arena, CIR_TYPE_STR,
                                      "struct clux_str");
        if (!ct) return NULL;
        ct->src = t;
        cir_unit_add_type(lc->unit, ct);
        return ct;
    }
    /* enum：C 侧就是底层整数（无独立 C enum 类型；variant 由 sema 折叠成
       整数字面量）。 */
    if (type_kind_of(t) == TYPE_KIND_ENUM) {
        const type_t *u = enum_type_underlying(t);
        return u ? map_type(lc, u) : NULL;
    }
    /* `cfunc(...)`：C 函数指针（typedef 由 FFI 层按签名生成/去重）。 */
    if (type_kind_of(t) == TYPE_KIND_CFUNC) {
        cir_type_t *cached = cir_type_find(lc->unit, t, CIR_TYPE_PTR);
        if (cached) return cached;
        bool ok = true;
        const char *tag = ffi_c_func_pointer_type(lc, t, &ok);
        if (!ok || !tag) return NULL;
        cir_type_t *ct = cir_type_new(lc->arena, CIR_TYPE_PTR, tag);
        if (!ct) return NULL;
        ct->src = t;
        cir_unit_add_type(lc->unit, ct);
        return ct;
    }
    /* `opaque`：C 里是 `void *`（FFI 指针/回调实参、`extern cfunc` 的裸指针
       都用它；`data(s)` / `a.&` 也产生它）。 */
    if (t == vm->type_opaque) {
        cir_type_t *cached = cir_type_find(lc->unit, t, CIR_TYPE_PTR);
        if (cached) return cached;
        cir_type_t *ct = cir_type_new(lc->arena, CIR_TYPE_PTR, "void *");
        if (!ct) return NULL;
        ct->src = t;
        cir_unit_add_type(lc->unit, ct);
        return ct;
    }

    cir_type_t *cached = cir_type_find(lc->unit, t, CIR_TYPE_STRUCT);
    if (cached) return cached;

    if (t->kind == TYPE_KIND_STRUCT) {
        char name[128];
        snprintf(name, sizeof name, "struct %ss_%u", type_prefix(lc), t->id);
        cir_type_t *ct = cir_type_new(lc->arena, CIR_TYPE_STRUCT,
                                      copy_arena(lc->arena, name));
        if (!ct) return NULL;
        ct->src = t;
        const struct_type_t *bst = (const struct_type_t *)t;
        if (bst->bitfield_ast) {
            const ast_struct_def_t *bsd =
                (const ast_struct_def_t *)bst->bitfield_ast;
            ct->is_bitfield = true;
            ct->bit_word_bits = bsd->bitfield_word_bits;
        }
        cir_unit_add_type(lc->unit, ct);
        size_t n = struct_type_field_count(t);
        for (size_t i = 0; i < n; i++) {
            const struct_field_t *sf = struct_type_field(t, i);
            if (!sf || !sf->type) continue;
            cir_type_t *ft = map_type(lc, sf->type);
            if (!ft) return NULL;
            char fn[256];
            snprintf(fn, sizeof fn, "clux_f_%.*s", (int)sf->name.len,
                     sf->name.ptr ? sf->name.ptr : "");
            cir_field_t *f = cir_field_new(lc->arena,
                                           copy_arena(lc->arena, fn), ft);
            if (!f) return NULL;
            cir_type_add_field(ct, f);
        }
        return ct;
    }
    if (t->kind == TYPE_KIND_ARRAY) {
        cached = cir_type_find(lc->unit, t, CIR_TYPE_ARRAY);
        if (cached) return cached;
        const array_type_t *at = (const array_type_t *)t;
        char name[128];
        snprintf(name, sizeof name, "struct %sa_%u", type_prefix(lc), t->id);
        cir_type_t *ct = cir_type_new(lc->arena, CIR_TYPE_ARRAY,
                                      copy_arena(lc->arena, name));
        if (!ct) return NULL;
        ct->src = t;
        ct->array_len = at->length;
        cir_unit_add_type(lc->unit, ct);
        ct->elem = map_type(lc, at->elem_type);
        return ct->elem ? ct : NULL;
    }
    if (t->kind == TYPE_KIND_PTR) {
        cached = cir_type_find(lc->unit, t, CIR_TYPE_PTR);
        if (cached) return cached;
        const type_t *elem = ptr_type_base(t);
        cir_type_t *et = map_type(lc, elem);
        if (!et) return NULL;
        char name[512];
        snprintf(name, sizeof name, "%s *", et->c_name);
        cir_type_t *ct = cir_type_new(lc->arena, CIR_TYPE_PTR,
                                      copy_arena(lc->arena, name));
        if (!ct) return NULL;
        ct->src = t;
        ct->elem = et;
        cir_unit_add_type(lc->unit, ct);
        return ct;
    }
    if (t->kind == TYPE_KIND_SLICE) {
        cached = cir_type_find(lc->unit, t, CIR_TYPE_SLICE);
        if (cached) return cached;
        char name[128];
        snprintf(name, sizeof name, "struct %ssl_%u", type_prefix(lc), t->id);
        cir_type_t *ct = cir_type_new(lc->arena, CIR_TYPE_SLICE,
                                      copy_arena(lc->arena, name));
        if (!ct) return NULL;
        ct->src = t;
        cir_unit_add_type(lc->unit, ct);
        ct->elem = map_type(lc, slice_type_elem(t));
        return ct->elem ? ct : NULL;
    }
    if (t->kind == TYPE_KIND_OPTION) {
        cached = cir_type_find(lc->unit, t, CIR_TYPE_OPTION);
        if (cached) return cached;
        char name[128];
        snprintf(name, sizeof name, "struct %sopt_%u", type_prefix(lc), t->id);
        cir_type_t *ct = cir_type_new(lc->arena, CIR_TYPE_OPTION,
                                      copy_arena(lc->arena, name));
        if (!ct) return NULL;
        ct->src = t;
        cir_unit_add_type(lc->unit, ct);
        ct->elem = map_type(lc, type_option_inner(t));
        return ct->elem ? ct : NULL;
    }
    if (t->kind == TYPE_KIND_CUNION) {
        cached = cir_type_find(lc->unit, t, CIR_TYPE_CUNION);
        if (cached) return cached;
        char name[128];
        snprintf(name, sizeof name, "struct %scu_%u", type_prefix(lc), t->id);
        cir_type_t *ct = cir_type_new(lc->arena, CIR_TYPE_CUNION,
                                      copy_arena(lc->arena, name));
        if (!ct) return NULL;
        ct->src = t;
        cir_unit_add_type(lc->unit, ct);
        const cunion_type_t *ut = (const cunion_type_t *)t;
        for (size_t i = 0; i < ut->member_count; i++) {
            const cunion_member_t *m = &ut->members[i];
            if (!m->type) continue;
            cir_type_t *mt = map_type(lc, m->type);
            if (!mt) return NULL;
            char fn[256];
            snprintf(fn, sizeof fn, "clux_m_%.*s", (int)m->name.len,
                     m->name.ptr ? m->name.ptr : "");
            cir_field_t *f = cir_field_new(lc->arena,
                                           copy_arena(lc->arena, fn), mt);
            if (!f) return NULL;
            cir_type_add_field(ct, f);
        }
        return ct;
    }
    if (t->kind == TYPE_KIND_FUNC) {
        cached = cir_type_find(lc->unit, t, CIR_TYPE_FUNC);
        if (cached) return cached;
        size_t pc = func_type_param_count(t);
        cir_type_t **pt = NULL;
        if (pc) {
            pt = (cir_type_t **)arena_calloc(lc->arena, pc, sizeof(*pt),
                                             ALIGNOF(max_align_t));
            if (!pt) return NULL;
        }
        for (size_t i = 0; i < pc; i++) {
            pt[i] = map_type(lc, func_type_param(t, i));
            if (!pt[i]) return NULL;
        }
        cir_type_t *ret = map_type(lc, func_type_return(t));
        if (!ret) return NULL;
        cir_signature_t *sig = signature_for(lc, ret, pc, pt);
        if (!sig) return NULL;
        char fname[256];
        snprintf(fname, sizeof fname, "struct %s", sig->tag);
        cir_type_t *ct = cir_type_new(lc->arena, CIR_TYPE_FUNC,
                                      copy_arena(lc->arena, fname));
        if (!ct) return NULL;
        ct->src = t;
        ct->sig = sig;
        cir_unit_add_type(lc->unit, ct);
        return ct;
    }
    if (t->kind == TYPE_KIND_UNION) {
        cached = cir_type_find(lc->unit, t, CIR_TYPE_UNION);
        if (cached) return cached;
        char name[128];
        snprintf(name, sizeof name, "struct %su_%u", type_prefix(lc), t->id);
        cir_type_t *ct = cir_type_new(lc->arena, CIR_TYPE_UNION,
                                      copy_arena(lc->arena, name));
        if (!ct) return NULL;
        ct->src = t;
        cir_unit_add_type(lc->unit, ct);
        const union_type_t *ut = (const union_type_t *)t;
        for (size_t i = 0; i < ut->member_count; i++) {
            const union_member_t *m = &ut->members[i];
            if (!m->payload_type) continue;
            cir_type_t *mt = map_type(lc, m->payload_type);
            if (!mt) return NULL;
            char fn[256];
            snprintf(fn, sizeof fn, "clux_m_%.*s", (int)m->name.len,
                     m->name.ptr ? m->name.ptr : "");
            cir_field_t *f = cir_field_new(lc->arena,
                                           copy_arena(lc->arena, fn), mt);
            if (!f) return NULL;
            f->tag = m->tag;
            cir_type_add_field(ct, f);
        }
        return ct;
    }
    lc->failed = true;
    return NULL;
}

const type_t *func_type_of_def(lower_ctx_t *lc, ast_func_def_t *fn) {
    if (!fn) return NULL;
    size_t pc = 0;
    for (ast_node_t *p = fn->params; p; p = p->next) pc++;
    const type_t **pt = NULL;
    if (pc) {
        pt = (const type_t **)arena_calloc(lc->arena, pc, sizeof(*pt),
                                           ALIGNOF(max_align_t));
        if (!pt) return NULL;
    }
    size_t i = 0;
    for (ast_node_t *p = fn->params; p; p = p->next, i++) {
        ast_var_def_t *vd = (ast_var_def_t *)p;
        const type_t *t = vd->resolved_type;
        if (!t && vd->type_expr)
            t = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        if (!t) return NULL;
        pt[i] = t;
    }
    const type_t *ret = lc->sema->vm->type_void;
    if (fn->return_expr)
        ret = sema_resolve_type_slot(lc->sema, &fn->return_expr);
    return type_func_sig(lc->sema->vm, pt, pc, ret, false);
}

ast_func_def_t *func_def_of_ref(lower_ctx_t *lc, ast_func_ref_t *ref) {
    sema_func_t *sf = sema_func_by_id(lc->sema, ref->fid);
    if (sf && sf->def && sf->def->kind == AST_FUNC_DEF)
        return (ast_func_def_t *)sf->def;
    return NULL;
}
