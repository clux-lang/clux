#include "cbackend/c_lower.h"

#include "parser/ast_assign.h"
#include "parser/ast_addr.h"
#include "parser/ast_binary.h"
#include "parser/ast_block.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_defer.h"
#include "parser/ast_deref.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_fill.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_for.h"
#include "parser/ast_for_index.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_if.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_is_tag.h"
#include "parser/ast_make.h"
#include "parser/ast_member.h"
#include "parser/ast_new.h"
#include "parser/ast_nil.h"
#include "parser/ast_opt_is_some.h"
#include "parser/ast_option.h"
#include "parser/ast_slice.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_union_def.h"
#include "parser/ast_free.h"
#include "parser/ast_return.h"
#include "parser/ast_unary.h"
#include "parser/ast_var_def.h"
#include "parser/ast_while.h"
#include "parser/lexer.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "vm/type_cunion.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_struct.h"
#include "vm/type_union.h"
#include "vm/vm.h"

#include <stdio.h>
#include <string.h>

typedef struct lower_defer {
    strslice_t name;
    ast_node_t *body;
    struct lower_defer *next;
} lower_defer_t;

typedef struct lower_local {
    strslice_t name;
    const type_t *type;
    struct lower_local *next;
} lower_local_t;

typedef struct lower_ctx {
    arena_t *arena;
    sema_t  *sema;
    cir_unit_t *unit;
    lower_defer_t *defers;
    lower_local_t *locals;
    cir_stmt_t *cleanup;
    cir_type_t *return_type;
    unsigned    ret_seq;
    bool     failed;
} lower_ctx_t;

static const char *copy_arena(arena_t *arena, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)arena_alloc(arena, n + 1, 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static const char *mangle(arena_t *arena, const char *prefix, strslice_t name) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s%.*s", prefix, (int)name.len,
                     name.ptr ? name.ptr : "");
    if (n <= 0 || (size_t)n >= sizeof buf) return NULL;
    char *p = (char *)arena_alloc(arena, (size_t)n + 1, 1);
    if (!p) return NULL;
    memcpy(p, buf, (size_t)n + 1);
    return p;
}

static const char *token_text(arena_t *arena, const token_t *tok) {
    size_t n = 0;
    const char *s = token_get_text(tok, &n);
    if (!s) return NULL;
    char *p = (char *)arena_alloc(arena, n + 1, 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static const type_t *strip(sema_t *sema, const type_t *t) {
    (void)sema;
    return t ? type_strip_quals(t) : NULL;
}

static cir_type_t *map_type(lower_ctx_t *lc, const type_t *t) {
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

    cir_type_t *cached = cir_type_find(lc->unit, t, CIR_TYPE_STRUCT);
    if (cached) return cached;

    if (t->kind == TYPE_KIND_STRUCT) {
        char name[128];
        snprintf(name, sizeof name, "struct clux_s_%u", t->id);
        cir_type_t *ct = cir_type_new(lc->arena, CIR_TYPE_STRUCT,
                                      copy_arena(lc->arena, name));
        if (!ct) return NULL;
        ct->src = t;
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
        snprintf(name, sizeof name, "struct clux_a_%u", t->id);
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
        snprintf(name, sizeof name, "struct clux_sl_%u", t->id);
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
        snprintf(name, sizeof name, "struct clux_opt_%u", t->id);
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
        snprintf(name, sizeof name, "struct clux_cu_%u", t->id);
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
    if (t->kind == TYPE_KIND_UNION) {
        cached = cir_type_find(lc->unit, t, CIR_TYPE_UNION);
        if (cached) return cached;
        char name[128];
        snprintf(name, sizeof name, "struct clux_u_%u", t->id);
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

static cir_expr_t *lower_expr(lower_ctx_t *lc, ast_node_t *e);
static cir_expr_t *lower_len(lower_ctx_t *lc, ast_call_t *call);
static cir_expr_t *lower_slice(lower_ctx_t *lc, ast_slice_t *sl);
static cir_expr_t *lower_make(lower_ctx_t *lc, ast_make_t *mk);
static const type_t *lookup_local(lower_ctx_t *lc, strslice_t name);
static bool call_is_len(const ast_call_t *c);
static const type_t *infer_expr_type(lower_ctx_t *lc, ast_node_t *e);

static cir_expr_t *lower_call(lower_ctx_t *lc, ast_call_t *call) {
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
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: only direct function calls are supported");
        lc->failed = true;
        return NULL;
    }
    const char *cname = mangle(lc->arena, "clux_fn_", name);
    if (!cname) return NULL;
    cir_expr_t *expr = cir_expr_call(lc->arena, cname, NULL);
    if (!expr) return NULL;
    /* env-first ABI：直接调用传 NULL。 */
    cir_expr_append_arg(expr, cir_expr_ident(lc->arena, "NULL"));
    for (ast_node_t *a = call->args; a; a = a->next) {
        cir_expr_t *arg = lower_expr(lc, a);
        if (!arg) return NULL;
        cir_expr_append_arg(expr, arg);
    }
    return expr;
}

static cir_expr_t *lower_expr(lower_ctx_t *lc, ast_node_t *e) {
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
        const char *n = mangle(lc->arena, "clux_v_", ((ast_ident_t *)e)->name);
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
    case AST_SLICE:
        return lower_slice(lc, (ast_slice_t *)e);
    case AST_MAKE:
        return lower_make(lc, (ast_make_t *)e);
    case AST_MEMBER: {
        ast_member_t *m = (ast_member_t *)e;
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
            return cir_expr_union_member(lc->arena, obj,
                                         copy_arena(lc->arena, fb));
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
        cir_expr_t *obj = lower_expr(lc, ix->object);
        cir_expr_t *idx = lower_expr(lc, ix->indices);
        return obj && idx ? cir_expr_index(lc->arena, obj, idx, true) : NULL;
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
        for (ast_node_t *f = c->fields; f; f = f->next) {
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
            cir_expr_t *ve = lower_expr(lc, val);
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
        for (ast_node_t *f = n->fields; f; f = f->next) {
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
            cir_expr_t *ve = lower_expr(lc, val);
            if (!ve) return NULL;
            cir_init_t *init = cir_init_new(lc->arena, fname, ve);
            if (!init) return NULL;
            cir_expr_add_init(out, init);
        }
        return out;
    }
    case AST_ASSIGN: {
        ast_assign_t *a = (ast_assign_t *)e;
        cir_expr_t *target = lower_expr(lc, a->target);
        const char *op = a->op ? token_text(lc->arena, a->op) : "=";
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

static const type_t *infer_expr_type(lower_ctx_t *lc, ast_node_t *e) {
    if (!e) return NULL;
    vm_t *vm = lc->sema->vm;
    switch (e->kind) {
    case AST_INT_LIT: {
        ast_int_lit_t *n = (ast_int_lit_t *)e;
        if (strslice_eq(n->type, STRSLICE_LIT("i8"))) return vm->type_i8;
        if (strslice_eq(n->type, STRSLICE_LIT("i16"))) return vm->type_i16;
        if (strslice_eq(n->type, STRSLICE_LIT("i64"))) return vm->type_i64;
        if (strslice_eq(n->type, STRSLICE_LIT("u8"))) return vm->type_u8;
        if (strslice_eq(n->type, STRSLICE_LIT("u16"))) return vm->type_u16;
        if (strslice_eq(n->type, STRSLICE_LIT("u32"))) return vm->type_u32;
        if (strslice_eq(n->type, STRSLICE_LIT("u64"))) return vm->type_u64;
        if (strslice_eq(n->type, STRSLICE_LIT("usize"))) return vm->type_usize;
        return vm->type_i32;
    }
    case AST_BOOL_LIT:
        return vm->type_bool;
    case AST_FLOAT_LIT: {
        ast_float_lit_t *f = (ast_float_lit_t *)e;
        if (strslice_eq(f->type, STRSLICE_LIT("f32"))) return vm->type_f32;
        return vm->type_f64;
    }
    case AST_STRING_LIT:
        return vm->type_str;
    case AST_CHAR_LIT:
        return vm->type_u8;
    case AST_IDENT:
        return lookup_local(lc, ((ast_ident_t *)e)->name);
    case AST_DEREF: {
        const type_t *pt = infer_expr_type(lc, ((ast_deref_t *)e)->operand);
        return pt && type_kind_of(pt) == TYPE_KIND_PTR ? ptr_type_base(pt) : NULL;
    }
    case AST_ADDR: {
        const type_t *bt = infer_expr_type(lc, ((ast_addr_t *)e)->operand);
        return bt ? type_ptr_intern(lc->sema->vm, bt) : NULL;
    }
    case AST_INDEX: {
        const type_t *bt = infer_expr_type(lc, ((ast_index_t *)e)->object);
        if (!bt) return NULL;
        bt = type_strip_quals(bt);
        if (type_kind_of(bt) == TYPE_KIND_ARRAY)
            return ((const array_type_t *)bt)->elem_type;
        if (type_kind_of(bt) == TYPE_KIND_SLICE)
            return slice_type_elem(bt);
        return NULL;
    }
    case AST_MEMBER: {
        ast_member_t *m = (ast_member_t *)e;
        const type_t *ot = infer_expr_type(lc, m->object);
        if (!ot) return NULL;
        ot = type_strip_quals(ot);
        if (type_kind_of(ot) == TYPE_KIND_STRUCT) {
            size_t n = struct_type_field_count(ot);
            for (size_t i = 0; i < n; i++) {
                const struct_field_t *f = struct_type_field(ot, i);
                if (f && f->name.len == m->field.len &&
                    memcmp(f->name.ptr, m->field.ptr, f->name.len) == 0)
                    return f->type;
            }
        }
        if (type_kind_of(ot) == TYPE_KIND_UNION) {
            int mi = union_type_find_member(ot, m->field);
            const union_member_t *mem =
                mi >= 0 ? union_type_member(ot, (size_t)mi) : NULL;
            return mem ? mem->payload_type : NULL;
        }
        if (type_kind_of(ot) == TYPE_KIND_CUNION) {
            int mi = cunion_type_find_member(ot, m->field);
            const cunion_member_t *mem =
                mi >= 0 ? cunion_type_member(ot, (size_t)mi) : NULL;
            return mem ? mem->type : NULL;
        }
        return NULL;
    }
    case AST_SLICE: {
        ast_slice_t *s = (ast_slice_t *)e;
        const type_t *ot = infer_expr_type(lc, s->object);
        if (!ot) return NULL;
        ot = type_strip_quals(ot);
        if (type_kind_of(ot) == TYPE_KIND_ARRAY)
            return type_slice_intern(vm, ((const array_type_t *)ot)->elem_type);
        if (type_kind_of(ot) == TYPE_KIND_SLICE) return ot;
        if (type_kind_of(ot) == TYPE_KIND_STR)
            return type_slice_intern(vm, type_const_intern(vm, vm->type_u8));
        return NULL;    }
    case AST_MAKE: {
        ast_make_t *m = (ast_make_t *)e;
        const type_t *et = sema_resolve_type_slot(lc->sema, &m->elem_type);
        return et ? type_slice_intern(vm, et) : NULL;
    }
    case AST_CONSTRUCT: {
        ast_construct_t *c = (ast_construct_t *)e;
        if (!c->type) return NULL;
        return sema_resolve_type_slot(lc->sema, &c->type);
    }
    case AST_IS_TAG:
    case AST_OPT_IS_SOME:
        return vm->type_bool;
    case AST_UNWRAP: {
        const type_t *ot = infer_expr_type(lc, ((ast_unwrap_t *)e)->operand);
        return ot ? type_option_inner(ot) : NULL;
    }
    case AST_UNARY:
        return infer_expr_type(lc, ((ast_unary_t *)e)->operand);
    case AST_BINARY: {
        ast_binary_t *b = (ast_binary_t *)e;
        if (token_is(b->op, "is")) return vm->type_bool;
        if (token_is(b->op, "==") || token_is(b->op, "!=") ||
            token_is(b->op, "<") || token_is(b->op, "<=") ||
            token_is(b->op, ">") || token_is(b->op, ">=") ||
            token_is(b->op, "&&") || token_is(b->op, "||"))
            return vm->type_bool;
        const type_t *lt = infer_expr_type(lc, b->lhs);
        return lt ? lt : infer_expr_type(lc, b->rhs);
    }
    case AST_CALL: {
        ast_call_t *c = (ast_call_t *)e;
        if (call_is_len(c)) return vm->type_usize;
        sema_func_t *sf = NULL;
        if (c->callee && c->callee->kind == AST_FUNC_REF)
            sf = sema_func_by_id(lc->sema, ((ast_func_ref_t *)c->callee)->fid);
        else if (c->callee && c->callee->kind == AST_IDENT) {
            sema_symbol_t *sym = sema_lookup(
                lc->sema->global_scope, ((ast_ident_t *)c->callee)->name);
            if (sym && sym->ast && sym->ast->kind == AST_FUNC_DEF) {
                ast_func_def_t *fd = (ast_func_def_t *)sym->ast;
                if (fd->return_expr)
                    return sema_resolve_type_slot(lc->sema, &fd->return_expr);
            }
        }
        if (sf && sf->def && sf->def->kind == AST_FUNC_DEF) {
            ast_func_def_t *fd = (ast_func_def_t *)sf->def;
            if (fd->return_expr)
                return sema_resolve_type_slot(lc->sema, &fd->return_expr);
        }
        return NULL;
    }
    default:
        return NULL;
    }
}

static bool is_defer_name(strslice_t n) {
    return n.ptr && n.len >= 8 && memcmp(n.ptr, "__defer_", 8) == 0;
}

static ast_node_t *find_defer_body(lower_ctx_t *lc, strslice_t name) {
    for (lower_defer_t *d = lc->defers; d; d = d->next)
        if (d->name.len == name.len &&
            memcmp(d->name.ptr, name.ptr, name.len) == 0)
            return d->body;
    return NULL;
}

static void register_local(lower_ctx_t *lc, strslice_t name,
                           const type_t *type) {
    if (!name.ptr || !type) return;
    lower_local_t *l = (lower_local_t *)arena_calloc(
        lc->arena, 1, sizeof(*l), ALIGNOF(max_align_t));
    if (!l) return;
    l->name = name;
    l->type = type;
    l->next = lc->locals;
    lc->locals = l;
}

static const type_t *lookup_local(lower_ctx_t *lc, strslice_t name) {
    for (lower_local_t *l = lc->locals; l; l = l->next)
        if (l->name.len == name.len &&
            memcmp(l->name.ptr, name.ptr, name.len) == 0)
            return l->type;
    return NULL;
}

static bool call_is_len(const ast_call_t *c) {
    if (!c || !c->callee) return false;
    if (c->callee->kind == AST_IDENT)
        return strslice_eq(((const ast_ident_t *)c->callee)->name,
                           STRSLICE_LIT("len"));
    if (c->callee->kind == AST_FUNC_REF) {
        const ast_func_ref_t *r = (const ast_func_ref_t *)c->callee;
        if (strslice_eq(r->name, STRSLICE_LIT("len"))) return true;
    }
    return false;
}

static cir_expr_t *lower_len(lower_ctx_t *lc, ast_call_t *call) {
    if (!call->args || call->args->next) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: len() expects one argument");
        lc->failed = true;
        return NULL;
    }
    ast_node_t *arg = call->args;
    const type_t *ot = infer_expr_type(lc, arg);
    if (!ot) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, arg),
                   "c backend: cannot determine len() operand type");
        lc->failed = true;
        return NULL;
    }
    cir_expr_t *obj = lower_expr(lc, arg);
    cir_type_t *ct = map_type(lc, ot);
    return obj && ct ? cir_expr_len(lc->arena, obj, ct) : NULL;
}

static cir_expr_t *lower_slice(lower_ctx_t *lc, ast_slice_t *sl) {
    const type_t *ot = infer_expr_type(lc, sl->object);
    if (!ot) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)sl),
                   "c backend: cannot determine slice base type");
        lc->failed = true;
        return NULL;
    }
    ot = type_strip_quals(ot);
    const type_t *st = NULL;
    if (type_kind_of(ot) == TYPE_KIND_ARRAY)
        st = type_slice_intern(lc->sema->vm,
                               ((const array_type_t *)ot)->elem_type);
    else if (type_kind_of(ot) == TYPE_KIND_SLICE)
        st = ot;
    else if (type_kind_of(ot) == TYPE_KIND_STR)
        st = type_slice_intern(lc->sema->vm,
                               type_const_intern(lc->sema->vm,
                                                 lc->sema->vm->type_u8));
    if (!st) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)sl),
                   "c backend: unsupported slice base type");
        lc->failed = true;
        return NULL;
    }
    cir_expr_t *obj = lower_expr(lc, sl->object);
    cir_expr_t *lo = sl->low ? lower_expr(lc, sl->low) : NULL;
    cir_expr_t *hi = sl->high ? lower_expr(lc, sl->high) : NULL;
    cir_type_t *ct = map_type(lc, st);
    cir_type_t *otc = map_type(lc, ot);
    if (!obj || !ct || !otc) return NULL;
    cir_expr_t *e = cir_expr_slice(lc->arena, ct, obj, lo, hi);
    if (e) e->src_type = otc;
    return e;
}

static cir_expr_t *lower_make(lower_ctx_t *lc, ast_make_t *mk) {
    const type_t *et = sema_resolve_type_slot(lc->sema, &mk->elem_type);
    if (!et) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)mk),
                   "c backend: cannot resolve make() element type");
        lc->failed = true;
        return NULL;
    }
    const type_t *st = type_slice_intern(lc->sema->vm, et);
    cir_type_t *sct = map_type(lc, st);
    cir_type_t *ect = map_type(lc, et);
    cir_expr_t *len = lower_expr(lc, mk->length);
    if (!sct || !ect || !len) return NULL;
    lc->unit->uses_alloc = true;
    cir_expr_t *out = cir_expr_make(lc->arena, sct, ect, len, NULL);
    if (!out) return NULL;
    for (ast_node_t *it = mk->inits; it; it = it->next) {
        if (it->kind == AST_FILL) {
            ast_fill_t *fill = (ast_fill_t *)it;
            if (fill->count->kind != AST_INT_LIT) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, it),
                           "c backend: make() fill count must be constant");
                lc->failed = true;
                return NULL;
            }
            uint64_t n = ((ast_int_lit_t *)fill->count)->value;
            for (uint64_t i = 0; i < n; i++) {
                cir_expr_t *v = lower_expr(lc, fill->value);
                if (!v) return NULL;
                cir_expr_add_init(out, cir_init_new(lc->arena, NULL, v));
            }
        } else {
            cir_expr_t *v = lower_expr(lc, it);
            if (!v) return NULL;
            cir_expr_add_init(out, cir_init_new(lc->arena, NULL, v));
        }
    }
    return out;
}

static cir_stmt_t *lower_block(lower_ctx_t *lc, ast_node_t *block,
                               bool *ok);

static cir_stmt_t *lower_stmt(lower_ctx_t *lc, ast_node_t *s, bool *ok);
static cir_stmt_t *lower_ast_list(lower_ctx_t *lc, ast_node_t *list, bool *ok);

static cir_stmt_t *lower_stmt(lower_ctx_t *lc, ast_node_t *s, bool *ok) {
    if (!s || lc->failed) return NULL;
    switch (s->kind) {
    case AST_FUNC_DEF: {
        ast_func_def_t *fn = (ast_func_def_t *)s;
        if (is_defer_name(fn->name)) {
            lower_defer_t *d = (lower_defer_t *)arena_calloc(
                lc->arena, 1, sizeof(*d), ALIGNOF(max_align_t));
            if (!d) return NULL;
            d->name = fn->name;
            d->body = fn->body;
            d->next = lc->defers;
            lc->defers = d;
            return NULL;
        }
        diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                   "c backend: local function definitions are not supported");
        *ok = false;
        return NULL;
    }
    case AST_VAR_DEF: {
        ast_var_def_t *vd = (ast_var_def_t *)s;
        const type_t *t = vd->resolved_type;
        if (!t && vd->type_expr) t = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        if (!t || type_kind_of(t) == TYPE_KIND_VOID)
            t = infer_expr_type(lc, vd->init);
        if (t) register_local(lc, vd->name, t);
        cir_type_t *ct = map_type(lc, t);
        if (!ct) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                       "c backend: cannot determine local variable type");
            *ok = false;
            return NULL;
        }
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_VAR);
        if (!st) return NULL;
        st->type = ct;
        st->name = mangle(lc->arena, "clux_v_", vd->name);
        if (vd->init && vd->init->kind != AST_UNDEF) {
            const type_t *it = infer_expr_type(lc, vd->init);
            if (vd->init->kind == AST_NIL && t &&
                type_kind_of(type_strip_quals(t)) == TYPE_KIND_OPTION) {
                st->expr = cir_expr_option(lc->arena, ct, false, NULL);
            } else if (t && type_kind_of(type_strip_quals(t)) == TYPE_KIND_OPTION &&
                       (!it || type_kind_of(type_strip_quals(it)) != TYPE_KIND_OPTION)) {
                cir_expr_t *v = lower_expr(lc, vd->init);
                st->expr = v ? cir_expr_option(lc->arena, ct, true, v) : NULL;
            } else {
                st->expr = lower_expr(lc, vd->init);
            }
        }
        return st;
    }
    case AST_ASSIGN: {
        ast_assign_t *a = (ast_assign_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_ASSIGN);
        if (!st) return NULL;
        st->lhs = lower_expr(lc, a->target);
        st->op = a->op ? token_text(lc->arena, a->op) : "=";
        const type_t *tt = infer_expr_type(lc, a->target);
        if (a->value && a->value->kind == AST_NIL && tt &&
            type_kind_of(type_strip_quals(tt)) == TYPE_KIND_OPTION) {
            cir_type_t *ot = map_type(lc, tt);
            st->expr = cir_expr_option(lc->arena, ot, false, NULL);
        } else if (tt && type_kind_of(type_strip_quals(tt)) == TYPE_KIND_OPTION) {
            const type_t *vt = infer_expr_type(lc, a->value);
            if (!vt || type_kind_of(type_strip_quals(vt)) != TYPE_KIND_OPTION) {
                cir_type_t *ot = map_type(lc, tt);
                cir_expr_t *v = lower_expr(lc, a->value);
                st->expr = v ? cir_expr_option(lc->arena, ot, true, v) : NULL;
            } else {
                st->expr = lower_expr(lc, a->value);
            }
        } else {
            st->expr = lower_expr(lc, a->value);
        }
        return st;
    }
    case AST_EXPR_STMT: {
        ast_expr_stmt_t *es = (ast_expr_stmt_t *)s;
        if (es->expr && es->expr->kind == AST_CALL) {
            ast_call_t *call = (ast_call_t *)es->expr;
            strslice_t name = STRSLICE_EMPTY;
            if (call->callee && call->callee->kind == AST_IDENT)
                name = ((ast_ident_t *)call->callee)->name;
            else if (call->callee && call->callee->kind == AST_FUNC_REF) {
                ast_func_ref_t *ref = (ast_func_ref_t *)call->callee;
                name = ref->name;
                if ((!name.ptr || name.len == 0) && lc->sema) {
                    sema_func_t *sf = sema_func_by_id(lc->sema, ref->fid);
                    if (sf && sf->def && sf->def->kind == AST_FUNC_DEF)
                        name = ((ast_func_def_t *)sf->def)->name;
                }
            }
            if (is_defer_name(name)) {
                ast_node_t *body = find_defer_body(lc, name);
                if (body) {
                    bool okd = true;
                    cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
                    if (!st) return NULL;
                    st->body = lower_block(lc, body, &okd);
                    if (!okd) *ok = false;
                    return st;
                }
            }
        }
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_EXPR);
        if (!st) return NULL;
        st->expr = lower_expr(lc, es->expr);
        return st;
    }
    case AST_RETURN: {
        ast_return_t *r = (ast_return_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_RETURN);
        if (!st) return NULL;
        if (r->value) {
            const type_t *rt = NULL;
            if (lc->return_type && lc->return_type->kind == CIR_TYPE_OPTION)
                rt = (const type_t *)lc->return_type->src;
            if (r->value->kind == AST_NIL && rt)
                st->expr = cir_expr_option(lc->arena, lc->return_type, false,
                                           NULL);
            else {
                const type_t *vt = infer_expr_type(lc, r->value);
                if (rt && (!vt || type_kind_of(type_strip_quals(vt)) !=
                                     TYPE_KIND_OPTION)) {
                    cir_expr_t *v = lower_expr(lc, r->value);
                    st->expr = v ? cir_expr_option(lc->arena,
                                                   lc->return_type, true, v)
                                 : NULL;
                } else {
                    st->expr = lower_expr(lc, r->value);
                }
            }
        }
        if (s->defers) {
            /* 先算 return 值到临时变量，再执行 defer/free，最后返回。 */
            cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
            if (!blk) return NULL;
            cir_stmt_t *tail = NULL;
            if (st->expr && lc->return_type &&
                lc->return_type->kind != CIR_TYPE_VOID) {
                char tmp[64];
                snprintf(tmp, sizeof tmp, "clux_ret_%u", lc->ret_seq++);
                cir_stmt_t *save = cir_stmt_new(lc->arena, CIR_STMT_VAR);
                if (!save) return NULL;
                save->type = lc->return_type;
                save->name = copy_arena(lc->arena, tmp);
                save->expr = st->expr;
                blk->body = save;
                tail = save;
                st->expr = cir_expr_ident(lc->arena, save->name);
            }
            bool okd = true;
            cir_stmt_t *clean = lower_ast_list(lc, s->defers, &okd);
            if (!okd) {
                *ok = false;
                return NULL;
            }
            if (clean) {
                if (tail) tail->next = clean;
                else blk->body = clean;
            }
            cir_stmt_t *last = blk->body;
            while (last && last->next) last = last->next;
            if (last) last->next = st;
            else blk->body = st;
            return blk;
        }
        return st;
    }
    case AST_BREAK: {
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_BREAK);
        if (!st) return NULL;
        if (s->defers) {
            cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
            if (!blk) return NULL;
            blk->body = lower_ast_list(lc, s->defers, ok);
            if (!*ok) return NULL;
            cir_stmt_t *t = blk->body;
            if (!t) blk->body = st;
            else {
                while (t->next) t = t->next;
                t->next = st;
            }
            return blk;
        }
        return st;
    }
    case AST_CONTINUE: {
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_CONTINUE);
        if (!st) return NULL;
        if (s->defers) {
            cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
            if (!blk) return NULL;
            blk->body = lower_ast_list(lc, s->defers, ok);
            if (!*ok) return NULL;
            cir_stmt_t *t = blk->body;
            if (!t) blk->body = st;
            else {
                while (t->next) t = t->next;
                t->next = st;
            }
            return blk;
        }
        return st;
    }
    case AST_BLOCK:
        return lower_block(lc, s, ok);
    case AST_IF: {
        ast_if_t *ifn = (ast_if_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_IF);
        if (!st) return NULL;
        st->expr = lower_expr(lc, ifn->cond);
        st->body = lower_block(lc, ifn->then_body, ok);
        if (ifn->else_body) {
            if (ifn->else_body->kind == AST_IF) {
                bool ok2 = true;
                cir_stmt_t *nested = lower_stmt(lc, ifn->else_body, &ok2);
                st->else_body = nested;
                if (!ok2) *ok = false;
            } else {
                st->else_body = lower_block(lc, ifn->else_body, ok);
            }
        }
        return st;
    }
    case AST_WHILE: {
        ast_while_t *w = (ast_while_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_WHILE);
        if (!st) return NULL;
        st->expr = lower_expr(lc, w->cond);
        st->body = lower_block(lc, w->body, ok);
        return st;
    }
    case AST_FOR: {
        ast_for_t *f = (ast_for_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_FOR);
        if (!st) return NULL;
        if (f->init) st->init = lower_stmt(lc, f->init, ok);
        if (f->cond) st->expr = lower_expr(lc, f->cond);
        if (f->update) st->update = lower_expr(lc, f->update);
        st->body = lower_block(lc, f->body, ok);
        return st;
    }
    case AST_DOWHILE: {
        ast_dowhile_t *d = (ast_dowhile_t *)s;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_DOWHILE);
        if (!st) return NULL;
        st->expr = lower_expr(lc, d->cond);
        st->body = lower_block(lc, d->body, ok);
        return st;
    }
    case AST_FOR_INDEX: {
        ast_for_index_t *fi = (ast_for_index_t *)s;
        const ast_node_t *src = fi->source;
        const ast_node_t *base = src;
        bool deref = false;
        if (src && src->kind == AST_DEREF) {
            deref = true;
            base = ((ast_deref_t *)src)->operand;
        }
        if (!base || base->kind != AST_IDENT) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                       "c backend: generated index loop source must be an array variable (%s)",
                       src ? ast_kind_name(src->kind) : "-");
            *ok = false;
            return NULL;
        }
        const char *vn = mangle(lc->arena, "clux_v_", fi->var);
        const char *raw = mangle(lc->arena, "clux_v_",
                                 ((ast_ident_t *)base)->name);
        if (!vn || !raw) return NULL;
        char srcbuf[512];
        snprintf(srcbuf, sizeof srcbuf, deref ? "(*%s)" : "%s", raw);
        const char *an = copy_arena(lc->arena, srcbuf);
        const type_t *stype = lookup_local(lc, ((ast_ident_t *)base)->name);
        if (stype && deref && type_kind_of(type_strip_quals(stype)) == TYPE_KIND_PTR)
            stype = ptr_type_base(stype);
        bool slice_src = stype && type_kind_of(type_strip_quals(stype)) == TYPE_KIND_SLICE;
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_FOR);
        if (!st) return NULL;
        cir_stmt_t *init = cir_stmt_new(lc->arena, CIR_STMT_VAR);
        if (!init) return NULL;
        init->type = cir_type_new(lc->arena, CIR_TYPE_USIZE, "size_t");
        init->name = vn;
        init->expr = cir_expr_int(lc->arena, 0);
        st->init = init;
        char lenbuf[512];
        if (slice_src)
            snprintf(lenbuf, sizeof lenbuf, "(%s).len", an);
        else
            snprintf(lenbuf, sizeof lenbuf,
                     "(sizeof((%s).data) / sizeof((%s).data[0]))", an, an);
        cir_expr_t *len = cir_expr_ident(lc->arena,
                                         copy_arena(lc->arena, lenbuf));
        st->expr = cir_expr_binary(lc->arena, "<",
                                   cir_expr_ident(lc->arena, vn), len);
        st->update = cir_expr_assign(
            lc->arena, cir_expr_ident(lc->arena, vn), "=",
            cir_expr_binary(lc->arena, "+", cir_expr_ident(lc->arena, vn),
                            cir_expr_int(lc->arena, 1)));
        st->body = lower_block(lc, fi->body, ok);
        return st;
    }
    case AST_DEFER: {
        ast_defer_t *d = (ast_defer_t *)s;
        ast_func_def_t *closure = (ast_func_def_t *)d->closure;
        bool okd = true;
        cir_stmt_t *body = closure ? lower_block(lc, closure->body, &okd) : NULL;
        if (!okd) {
            *ok = false;
            return NULL;
        }
        cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
        if (!blk) return NULL;
        blk->body = body;
        blk->next = lc->cleanup;
        lc->cleanup = blk;
        return NULL; /* defer 不产生当前位置语句 */
    }
    case AST_FREE: {
        ast_free_t *f = (ast_free_t *)s;
        lc->unit->uses_alloc = true;
        cir_stmt_t *head = NULL, *tail = NULL;
        for (ast_node_t *op = f->operand; op; op = op->next) {
            const type_t *ot = infer_expr_type(lc, op);
            cir_expr_t *ex = lower_expr(lc, op);
            if (!ex) {
                *ok = false;
                return head;
            }
            if (ot && type_kind_of(ot) == TYPE_KIND_SLICE)
                ex = cir_expr_slice_data(lc->arena, ex);
            cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_FREE);
            if (!st) return head;
            st->expr = ex;
            if (tail) tail->next = st;
            else head = st;
            tail = st;
        }
        return head;
    }
    case AST_EMPTY_STMT:
        return NULL;
    default:
        diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                   "c backend: unsupported statement (kind %d)",
                   (int)s->kind);
        *ok = false;
        return NULL;
    }
}

static cir_stmt_t *lower_ast_list(lower_ctx_t *lc, ast_node_t *list, bool *ok) {
    cir_stmt_t *head = NULL, *tail = NULL;
    for (ast_node_t *s = list; s; s = s->next) {
        cir_stmt_t *st = lower_stmt(lc, s, ok);
        if (!*ok) return head;
        if (!st) continue;
        if (tail) tail->next = st;
        else head = st;
        tail = st;
    }
    return head;
}

static cir_stmt_t *lower_block(lower_ctx_t *lc, ast_node_t *block, bool *ok) {    if (!block || block->kind != AST_BLOCK) {
        *ok = false;
        return NULL;
    }
    cir_stmt_t *head = NULL, *tail = NULL;
    for (ast_node_t *s = ((ast_block_t *)block)->stmts; s; s = s->next) {
        cir_stmt_t *st = lower_stmt(lc, s, ok);
        if (!*ok) return head;
        if (!st) continue;
        if (tail) tail->next = st;
        else head = st;
        tail = st;
    }
    return head;
}

static ast_func_def_t *find_main(ast_program_t *prog) {
    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_FUNC_DEF) continue;
        ast_func_def_t *fn = (ast_func_def_t *)n;
        if (strslice_eq(fn->name, STRSLICE_LIT("main"))) return fn;
    }
    return NULL;
}

static void mark_cleanup_returns(cir_stmt_t *s) {
    for (; s; s = s->next) {
        if (s->kind == CIR_STMT_RETURN) s->cleanup_return = true;
        if (s->body) mark_cleanup_returns(s->body);
        if (s->else_body) mark_cleanup_returns(s->else_body);
        if (s->init) mark_cleanup_returns(s->init);
    }
}

static cir_function_t *lower_function(lower_ctx_t *lc, ast_func_def_t *fn,
                                      bool is_entry) {
    lc->locals = NULL;
    if (fn->generic.count > 0 || fn->is_cfunc || fn->is_extern_cfunc) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)fn),
                   "c backend: unsupported function kind");
        lc->failed = true;
        return NULL;
    }
    const type_t *ret = lc->sema->vm->type_void;
    if (fn->return_expr)
        ret = sema_resolve_type_slot(lc->sema, &fn->return_expr);
    cir_type_t *cret = map_type(lc, ret);
    if (!cret) return NULL;

    size_t pc = 0;
    for (ast_node_t *p = fn->params; p; p = p->next) pc++;
    cir_param_t *params = NULL;
    cir_type_t **ptypes = NULL;
    if (pc) {
        params = (cir_param_t *)arena_calloc(
            lc->arena, pc, sizeof(*params), ALIGNOF(max_align_t));
        ptypes = (cir_type_t **)arena_calloc(
            lc->arena, pc, sizeof(*ptypes), ALIGNOF(max_align_t));
        if (!params || !ptypes) return NULL;
    }
    size_t i = 0;
    for (ast_node_t *p = fn->params; p; p = p->next, i++) {
        ast_var_def_t *vd = (ast_var_def_t *)p;
        const type_t *pt = vd->resolved_type;
        if (!pt && vd->type_expr)
            pt = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        ptypes[i] = map_type(lc, pt);
        if (!ptypes[i]) return NULL;
        params[i].type = ptypes[i];
        params[i].name = mangle(lc->arena, "clux_v_", vd->name);
        register_local(lc, vd->name, pt);
    }

    bool ok = true;
    lc->cleanup = NULL;
    lc->defers = NULL;
    lc->return_type = cret;
    lc->ret_seq = 0;
    cir_stmt_t *body = lower_block(lc, fn->body, &ok);
    if (!ok) return NULL;
    cir_stmt_t *cleanup = lc->cleanup;
    lc->cleanup = NULL;
    if (cleanup) mark_cleanup_returns(body);

    const char *cname = mangle(lc->arena, "clux_fn_", fn->name);
    const char *obj = mangle(lc->arena, "clux_obj_", fn->name);
    const char *tag = mangle(lc->arena, "clux_sig_", fn->name);
    if (!cname || !obj || !tag) return NULL;
    cir_signature_t *sig =
        cir_signature_new(lc->arena, tag, cret, pc, ptypes);
    if (!sig) return NULL;
    int line = sema_loc(lc->sema, (ast_node_t *)fn).begin.line;
    cir_function_t *out = cir_function_new(lc->arena, cname, obj, sig, pc, params,
                                           body, line, is_entry);
    if (out) out->cleanup = cleanup;
    return out;
}

cir_unit_t *cbackend_lower(arena_t *arena, sema_t *sema, ast_program_t *prog,
                           const char *source_path) {
    if (!arena || !sema || !prog || !source_path) return NULL;
    ast_func_def_t *main_fn = find_main(prog);
    if (!main_fn) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)prog),
                   "c backend: no 'main' function");
        return NULL;
    }
    size_t param_count = 0;
    for (ast_node_t *p = main_fn->params; p; p = p->next) param_count++;
    if (param_count != 0) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)main_fn),
                   "c backend: 'main' must not take parameters");
        return NULL;
    }
    if (main_fn->return_expr) {
        const type_t *rt = sema_resolve_type_slot(sema, &main_fn->return_expr);
        if (rt != sema->vm->type_i32 && rt != sema->vm->type_void) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)main_fn),
                       "c backend: 'main' must return i32 or void");
            return NULL;
        }
    }

    lower_ctx_t lc = {.arena = arena, .sema = sema, .failed = false};
    cir_unit_t *u = cir_unit_new(arena, source_path);
    if (!u) return NULL;
    lc.unit = u;
    cir_unit_add_include(u, arena, "stdint.h");
    cir_unit_add_include(u, arena, "stddef.h");
    cir_unit_add_include(u, arena, "stdbool.h");

    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_VAR_DEF) continue;
        ast_var_def_t *vd = (ast_var_def_t *)n;
        if (vd->is_comptime || vd->is_extern_var) continue;
        const type_t *t = vd->resolved_type;
        if (!t && vd->type_expr)
            t = sema_resolve_type_slot(sema, &vd->type_expr);
        if (!t || type_kind_of(t) == TYPE_KIND_VOID)
            t = infer_expr_type(&lc, vd->init);
        cir_type_t *ct = map_type(&lc, t);
        if (!ct) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)vd),
                       "c backend: cannot determine global variable type");
            return NULL;
        }
        cir_global_t *g = (cir_global_t *)arena_calloc(
            arena, 1, sizeof(*g), ALIGNOF(max_align_t));
        if (!g) return NULL;
        g->type = ct;
        g->name = mangle(arena, "clux_v_", vd->name);
        if (vd->init && vd->init->kind != AST_UNDEF)
            g->init = lower_expr(&lc, vd->init);
        cir_unit_add_global(u, g);
    }

    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_FUNC_DEF) continue;
        ast_func_def_t *fn = (ast_func_def_t *)n;
        if (strslice_eq(fn->name, STRSLICE_LIT("main"))) continue;
        if (fn->generic.count > 0 || fn->is_cfunc || fn->is_extern_cfunc)
            continue;
        cir_function_t *cf = lower_function(&lc, fn, false);
        if (!cf || lc.failed) return NULL;
        cir_unit_add_signature(u, cf->sig);
        cir_unit_add_function(u, cf);
    }
    cir_function_t *cm = lower_function(&lc, main_fn, true);
    if (!cm || lc.failed) return NULL;
    cir_unit_add_signature(u, cm->sig);
    cir_unit_add_function(u, cm);
    return u;
}
