#include "sema/sema_interface.h"

#include "core/panic.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_interface_def.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_struct_embed.h"
#include "parser/ast_var_def.h"
#include "parser/parser.h"
#include "sema/sema_cross.h"
#include "sema/struct_compose.h"
#include "sema/symbol.h"
#include "vm/type_func.h"
#include "vm/type_struct.h"

#include <string.h>

/* ===========================================================================
 * M7 搂31b锛歩nterface 瀹炵幇鏍￠獙
 *
 * interface 鏄函缂栬瘧鏈熷疄浣擄細鏈枃浠跺彧鍋?struct 鏄惁婊¤冻绛惧悕闆嗗悎"鐨勬鏌ワ紝
 * 骞舵妸 (struct type, interface def, 绫诲瀷瀹炲弬) 璁板叆 sema->impls锛屼緵
 * `T extends I[...]` 绾︽潫鍖归厤浣跨敤銆備笉浜х敓 vtable / type_t銆? * ========================================================================= */

typedef struct {
    const ast_node_t *def;
    sema_symbol_t    *sym;
    const type_t    **args;
    size_t            arg_count;
} iface_ref_t;

static sema_symbol_t *iface_symbol_for_base(sema_t *sema, ast_node_t *base,
                                            strslice_t *out_name) {
    *out_name = STRSLICE_EMPTY;
    if (base && base->kind == AST_IDENT) {
        *out_name = ((ast_ident_t *)base)->name;
        return sema_scope_find_local(sema->global_scope, *out_name);
    }
    if (base && base->kind == AST_SCOPE_REF) {
        ast_scope_ref_t *sr = (ast_scope_ref_t *)base;
        if (!sr->type_expr || sr->type_expr->kind != AST_IDENT) return NULL;
        sema_symbol_t *modsym = sema_scope_find_local(
            sema->global_scope, ((ast_ident_t *)sr->type_expr)->name);
        if (!modsym || modsym->kind != SEMA_SYM_MODULE || !modsym->module)
            return NULL;
        *out_name = sr->variant;
        sema_symbol_t *sym =
            sema_module_export_symbol(modsym->module, sr->variant);
        if (sym && !sym->module) sym->module = modsym->module;
        return sym;
    }
    return NULL;
}

static bool iface_resolve(sema_t *sema, ast_node_t *expr, iface_ref_t *out) {
    memset(out, 0, sizeof(*out));
    if (!expr) return false;

    ast_node_t *base = expr;
    ast_node_t *indices = NULL;
    if (expr->kind == AST_INDEX) {
        ast_index_t *ix = (ast_index_t *)expr;
        base = ix->object;
        indices = ix->indices;
    }
    strslice_t name = STRSLICE_EMPTY;
    sema_symbol_t *sym = iface_symbol_for_base(sema, base, &name);
    if (!sym || sym->kind != SEMA_SYM_INTERFACE || !sym->ast) {
        if (name.ptr)
            diag_error(sema->diag, sema_loc(sema, expr),
                       "'%.*s' is not an interface", (int)name.len, name.ptr);
        else
            diag_error(sema->diag, sema_loc(sema, expr),
                       "'implements' requires an interface name (optionally "
                       "with type arguments)");
        return false;
    }
    const ast_interface_def_t *idef = (const ast_interface_def_t *)sym->ast;
    size_t ntp = idef->generic.count;
    size_t n = sema_count_siblings(indices);
    if (n != ntp) {
        diag_error(sema->diag, sema_loc(sema, expr),
                   "interface '%.*s' expects %zu type argument(s), got %zu",
                   (int)name.len, name.ptr, ntp, n);
        return false;
    }
    /* interface 娉涘瀷鍙傛暟鍙敮鎸佺被鍨嬪弬鏁帮紙鍊煎弬鏁扮暀缁欏悗缁級銆?*/
    for (ast_node_t *p = idef->generic.first; p; p = p->next) {
        if (((ast_generic_param_t *)p)->is_comptime ||
            ((ast_generic_param_t *)p)->is_pack) {
            diag_error(sema->diag, sema_loc(sema, expr),
                       "interface '%.*s' only supports plain type parameters "
                       "in M7",
                       (int)name.len, name.ptr);
            return false;
        }
    }

    const type_t **args = NULL;
    if (n > 0) {
        args = (const type_t **)arena_alloc(
            sema->arena, n * sizeof(type_t *), ALIGNOF(max_align_t));
        if (!args) return false;
        size_t i = 0;
        for (ast_node_t *a = indices; a; a = a->next, i++) {
            const type_t *t = resolve_type_expr(sema, a);
            if (!t) return false;
            args[i] = t;
        }
    }
    out->def = sym->ast;
    out->sym = sym;
    out->args = args;
    out->arg_count = n;
    return true;
}

static void push_iface_bindings(sema_t *sema, const iface_ref_t *ref) {
    vm_push_scope(sema->vm);
    const ast_interface_def_t *idef = (const ast_interface_def_t *)ref->def;
    size_t i = 0;
    for (ast_node_t *p = idef->generic.first; p && i < ref->arg_count;
         p = p->next, i++) {
        ast_generic_param_t *gp = (ast_generic_param_t *)p;
        value_t *tv = type_as_value(sema->vm, ref->args[i]);
        if (!tv) continue;
        char nb[256];
        size_t len = gp->name.len < sizeof nb - 1 ? gp->name.len : sizeof nb - 1;
        if (len > 0 && gp->name.ptr) memcpy(nb, gp->name.ptr, len);
        nb[len] = '\0';
        scope_define(sema->vm, sema->vm->current_scope, nb, tv);
    }
}

/* 鎺ュ彛鎴愬憳绛惧悕锛堝湪缁戝畾灞備笅瑙ｆ瀽锛夈€?*/
static const type_t *iface_member_sig(sema_t *sema, ast_func_def_t *fn) {
    size_t n = sema_count_siblings(fn->params);
    const type_t **ptypes = NULL;
    if (n > 0) {
        ptypes = (const type_t **)arena_alloc(
            sema->arena, n * sizeof(type_t *), ALIGNOF(max_align_t));
        if (!ptypes) return NULL;
    }
    size_t i = 0;
    for (ast_node_t *p = fn->params; p; p = p->next, i++) {
        ast_var_def_t *vd = (ast_var_def_t *)p;
        const type_t *t = sema_resolve_type_slot(sema, &vd->type_expr);
        if (!t) return NULL;
        ptypes[i] = t;
    }
    const type_t *rt = fn->return_expr
                           ? sema_resolve_type_slot(sema, &fn->return_expr)
                           : sema->vm->type_void;
    if (fn->return_expr && !rt) return NULL;
    /* 与普通函数签名一致：void 返回用内建 type_void（不是 NULL）。 */
    return type_func_sig(sema->vm, ptypes, n, rt, false);
}

static void add_impl(sema_t *sema, const type_t *st, const ast_node_t *def,
                     const type_t *const *args, size_t n) {
    for (sema_impl_t *it = sema->impls; it; it = it->next) {
        if (it->type == st && it->iface_def == def) return;
    }
    sema_impl_t *im = (sema_impl_t *)allocator_new_ex(
        sema->vm->alloc, "sema_impl_t", sizeof(sema_impl_t), NULL, NULL, NULL, 1);
    if (!im) return;
    memset(im, 0, sizeof(*im));
    im->type = st;
    im->iface_def = def;
    im->arg_count = n;
    if (n > 0) {
        const type_t **copy = (const type_t **)arena_alloc(
            sema->arena, n * sizeof(type_t *), ALIGNOF(max_align_t));
        if (copy) {
            memcpy(copy, args, n * sizeof(type_t *));
            im->args = copy;
        } else {
            im->arg_count = 0;
        }
    }
    im->next = sema->impls;
    sema->impls = im;
}

static bool check_one_interface(sema_t *sema, const type_t *st,
                                ast_node_t *iface_expr, const ast_node_t *site,
                                unsigned depth, bool record);

static bool check_one_interface(sema_t *sema, const type_t *st,
                                ast_node_t *iface_expr, const ast_node_t *site,
                                unsigned depth, bool record) {
    if (depth > 32) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)site),
                   "interface member copy chain is too deep (cycle?)");
        return false;
    }
    iface_ref_t ref;
    if (!iface_resolve(sema, iface_expr, &ref)) return false;

    push_iface_bindings(sema, &ref);

    /* 閲嶆柊瑙ｆ瀽鎺ュ彛瀹氫箟锛氭垚鍛樼被鍨嬫Ы浣嶄細鍦ㄨВ鏋愪腑鎶樺彔锛岀粷涓嶈兘姹℃煋鍘?AST
       锛堝悓涓€ interface 鍙兘琚涓?struct 瀹炵幇锛夈€?*/
    vec_t *saved_tokens = sema->tokens;
    if (ref.sym->module && ref.sym->module->src_tokens)
        sema->tokens = (vec_t *)ref.sym->module->src_tokens;
    parser_t *rp = parser_create(sema->vm->alloc, sema->arena, sema->tokens);
    if (!rp) panic("sema: out of memory reparsing interface");
    rp->pos = ref.def->tok_begin;
    rp->diag = sema->diag;
    ast_node_t *parsed = parse_interface_def(rp);
    parser_destroy(&rp);
    sema->tokens = saved_tokens;
    if (!parsed || parsed->kind != AST_INTERFACE_DEF) {
        diag_error(sema->diag, sema_loc(sema, iface_expr),
                   "internal: failed to materialize interface");
        vm_pop_scope(sema->vm);
        return false;
    }
    ast_interface_def_t *idef = (ast_interface_def_t *)parsed;

    bool ok = true;

    /* `...Base;` 成员复制：Base 的成员也是本接口要求的一部分。只支持
       非泛型接口的复制（泛型嵌入需显式实参语法，留待 P6c）。 */
    for (ast_node_t *e = idef->embeds; e; e = e->next) {
        ast_struct_embed_t *emb = (ast_struct_embed_t *)e;
        ast_node_t *base_expr = emb->type_expr;
        if (!base_expr) {
            base_expr = ast_ident_new(sema->arena, e->tok_begin, e->tok_end);
            if (!base_expr) { ok = false; continue; }
            ((ast_ident_t *)base_expr)->name = emb->name;
        }
        if (!check_one_interface(sema, st, base_expr, site, depth + 1,
                                 /*record=*/false))
            ok = false;
    }
    for (ast_node_t *m = idef->members; m; m = m->next) {
        if (m->kind == AST_VAR_DEF) {
            ast_var_def_t *vd = (ast_var_def_t *)m;
            const type_t *want = sema_resolve_type_slot(sema, &vd->type_expr);
            if (!want) {
                ok = false;
                continue;
            }
            int fi = struct_type_find_field(st, vd->name);
            if (fi < 0) {
                diag_error(sema->diag, sema_loc(sema, (ast_node_t *)site),
                           "struct '%.*s' does not implement interface '%.*s': "
                           "missing attribute '%.*s'",
                           (int)st->name.len, st->name.ptr,
                           (int)((ast_interface_def_t *)ref.def)->name.len,
                           ((ast_interface_def_t *)ref.def)->name.ptr,
                           (int)vd->name.len, vd->name.ptr);
                ok = false;
                continue;
            }
            const struct_field_t *f = struct_type_field(st, (size_t)fi);
            if (!f || !type_equal(sema->vm, f->type, want)) {
                diag_error(sema->diag, sema_loc(sema, (ast_node_t *)site),
                           "struct '%.*s': attribute '%.*s' does not match the "
                           "interface signature",
                           (int)st->name.len, st->name.ptr,
                           (int)vd->name.len, vd->name.ptr);
                ok = false;
            }
        } else if (m->kind == AST_FUNC_DEF) {
            ast_func_def_t *fn = (ast_func_def_t *)m;
            const type_t *want = iface_member_sig(sema, fn);
            if (!want) {
                ok = false;
                continue;
            }
            sema_symbol_t *ms = sema_struct_find_member(sema, st, fn->name,
                                                        NULL, NULL);
            if (!ms || ms->kind != SEMA_SYM_FUNC || !ms->type) {
                diag_error(sema->diag, sema_loc(sema, (ast_node_t *)site),
                           "struct '%.*s' does not implement interface '%.*s': "
                           "missing method '%.*s'",
                           (int)st->name.len, st->name.ptr,
                           (int)((ast_interface_def_t *)ref.def)->name.len,
                           ((ast_interface_def_t *)ref.def)->name.ptr,
                           (int)fn->name.len, fn->name.ptr);
                ok = false;
                continue;
            }
            if (!type_equal(sema->vm, ms->type, want)) {
                diag_error(sema->diag, sema_loc(sema, (ast_node_t *)site),
                           "struct '%.*s': method '%.*s' does not match the "
                           "interface signature",
                           (int)st->name.len, st->name.ptr,
                           (int)fn->name.len, fn->name.ptr);
                ok = false;
            }
        }
    }

    vm_pop_scope(sema->vm);
    if (ok && record) add_impl(sema, st, ref.def, ref.args, ref.arg_count);
    return ok;
}

bool sema_check_struct_implements(sema_t *sema, const type_t *struct_type,
                                  ast_node_t *implements,
                                  const ast_node_t *site) {
    if (!sema || !struct_type || !implements) return true;
    bool ok = true;
    for (ast_node_t *im = implements; im; im = im->next)
        if (!check_one_interface(sema, struct_type, im, site, 0,
                                 /*record=*/true))
            ok = false;
    return ok;
}

void sema_check_implements_pass(sema_t *sema, ast_program_t *prog) {
    if (!sema || !prog) return;
    for (ast_node_t *d = prog->funcs; d; d = d->next) {
        if (d->kind != AST_STRUCT_DEF) continue;
        ast_struct_def_t *sd = (ast_struct_def_t *)d;
        if (!sd->implements) continue;
        if (sd->generic.count > 0) continue; /* 瀹炰緥鍖栨椂閫愬疄渚嬫鏌?*/
        sema_symbol_t *sym =
            sema_scope_find_local(sema->global_scope, sd->name);
        if (!sym || !sym->is_active || !sym->type) continue;
        (void)sema_check_struct_implements(sema, sym->type, sd->implements, d);
    }
}

bool sema_type_implements(sema_t *sema, const type_t *t,
                          const ast_node_t *iface_def,
                          const type_t *const *args, size_t arg_count) {
    if (!sema || !t || !iface_def) return false;
    const type_t *core = type_strip_quals(t);
    for (sema_impl_t *it = sema->impls; it; it = it->next) {
        if (it->type != core || it->iface_def != iface_def) continue;
        if (it->arg_count != arg_count) continue;
        bool match = true;
        for (size_t i = 0; i < arg_count; i++) {
            if (!type_equal(sema->vm, it->args[i], args[i])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

