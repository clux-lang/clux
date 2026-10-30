#include "sema/sema_generic.h"
#include "sema/sema_interface.h"
#include "sema/sema_pattern.h"

#include "ctfe/ctfe.h"
#include "core/panic.h"
#include "core/string.h"
#include "desugar/desugar.h"
#include "parser/ast_array.h"
#include "parser/ast_block.h"
#include "parser/ast_call.h"
#include "parser/ast_return.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_const.h"
#include "parser/ast_construct.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_type_def.h"
#include "parser/ast_union_def.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_type.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_option.h"
#include "parser/ast_program.h"
#include "parser/ast_pack_type.h"
#include "parser/ast_ptr.h"
#include "parser/ast_scope_annot.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_tuple.h"
#include "parser/ast_var_def.h"
#include "parser/ast_volatile.h"
#include "parser/parser.h"
#include "parser/parse_utils.h"
#include "vm/module.h"
#include "vm/type_array.h"
#include "vm/type_cunion.h"
#include "vm/type_error.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "sema/metadata.h"
#include "sema/comptime.h"
#include "sema/struct_compose.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"

#include <stdio.h>
#include <string.h>

/* 内部：把 strslice 转成 NUL 终止缓冲（VM scope 注册用）。 */
static void generic_name_to_cstr(strslice_t s, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    size_t n = s.len < cap - 1 ? s.len : cap - 1;
    if (n > 0 && s.ptr) memcpy(buf, s.ptr, n);
    buf[n] = '\0';
}

static strslice_t arena_copy(arena_t *arena, const char *p, size_t n) {
    if (!arena || n == 0) return STRSLICE_EMPTY;
    char *dst = (char *)arena_alloc(arena, n, 1);
    memcpy(dst, p, n);
    return strslice_make(dst, n);
}

static void mangle_append_cstr(string_t *s, const char *cstr) {
    string_append_cstr(s, cstr);
}

static void mangle_append_uint(string_t *s, unsigned long long v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%llu", v);
    if (n > 0) string_append_bytes(s, buf, (size_t)n);
}

/* 类型实参的规范 mangling 编码。规则见 docs/m7-design.md §17：
 *   i32 / *i32 / [3]i32 / Vec$i32 / <i32,str> / func(i32)->f64 ...
 * 所有权修饰与指针 rc 都要编码（`own *i32` 与 `*i32` 是不同类型）。 */
void sema_generic_mangle_type(string_t *out, const type_t *t) {
    if (!out) return;
    if (!t) {
        mangle_append_cstr(out, "?");
        return;
    }
    switch (t->kind) {
    case TYPE_KIND_CONST:
        mangle_append_cstr(out, "const ");
        sema_generic_mangle_type(out, ((const const_type_t *)t)->sub);
        return;
    case TYPE_KIND_VOLATILE:
        mangle_append_cstr(out, "volatile ");
        sema_generic_mangle_type(out, ((const volatile_type_t *)t)->sub);
        return;
    case TYPE_KIND_OPTION:
        mangle_append_cstr(out, "?");
        sema_generic_mangle_type(out, type_option_inner(t));
        return;
    case TYPE_KIND_PTR: {
        mangle_append_cstr(out, "*");
        sema_generic_mangle_type(out, ptr_type_base(t));
        return;
    }
    case TYPE_KIND_SLICE:
        mangle_append_cstr(out, "[]");
        sema_generic_mangle_type(out, slice_type_elem(t));
        return;
    case TYPE_KIND_ARRAY: {
        mangle_append_cstr(out, "[");
        mangle_append_uint(out, (unsigned long long)array_type_len(t));
        mangle_append_cstr(out, "]");
        sema_generic_mangle_type(out, array_type_elem(t));
        return;
    }
    case TYPE_KIND_TUPLE: {
        mangle_append_cstr(out, "<");
        size_t n = tuple_type_elem_count(t);
        for (size_t i = 0; i < n; i++) {
            if (i > 0) mangle_append_cstr(out, ",");
            const tuple_elem_t *e = tuple_type_elem(t, i);
            sema_generic_mangle_type(out, e ? e->type : NULL);
        }
        mangle_append_cstr(out, ">");
        return;
    }
    case TYPE_KIND_FUNC: {
        size_t nc = func_type_capture_count(t);
        if (nc > 0) {
            mangle_append_cstr(out, "clos(");
            for (size_t ci = 0; ci < nc; ci++) {
                if (ci > 0) mangle_append_cstr(out, ",");
                sema_generic_mangle_type(out, func_type_capture_type(t, ci));
            }
            mangle_append_cstr(out, ")");
        }
        mangle_append_cstr(out, "func(");
        size_t n = func_type_param_count(t);
        for (size_t i = 0; i < n; i++) {
            if (i > 0) mangle_append_cstr(out, ",");
            sema_generic_mangle_type(out, func_type_param(t, i));
        }
        mangle_append_cstr(out, ")->");
        sema_generic_mangle_type(out, func_type_return(t));
        return;
    }
    case TYPE_KIND_VOID:
        mangle_append_cstr(out, "void");
        return;
    default:
        break;
    }

    /* 具名类型（内建标量 / enum / union / cunion / 命名 struct）：
       type->name 是规范显示名；匿名类型退化为 id 编码（仅求唯一）。 */
    if (t->name.ptr && t->name.len > 0) {
        string_append_bytes(out, t->name.ptr, t->name.len);
        return;
    }
    mangle_append_cstr(out, "#");
    mangle_append_uint(out, (unsigned long long)t->id);
}

const sema_generic_instance_t *sema_generic_find(sema_t *sema, strslice_t name) {
    if (!sema) return NULL;
    for (sema_generic_instance_t *e = sema->generics.instances; e; e = e->next) {
        if (strslice_eq(e->name, name)) return e;
    }
    return NULL;
}

void sema_generic_destroy(sema_t *sema) {
    if (!sema || !sema->vm) return;
    allocator_t *alloc = sema->vm->alloc;
    sema_generic_instance_t *e = sema->generics.instances;
    while (e) {
        sema_generic_instance_t *next = e->next;
        allocator_free(alloc, (void **)&e);
        e = next;
    }
    sema->generics.instances = NULL;
}

/* 解析 + 校验泛型参数表。P5 支持 comptime 值参数与参数包（包必须在最后）。 */
static bool check_generic_param_list(sema_t *sema,
                                     const ast_generic_params_t *list,
                                     const ast_node_t *def) {
    if (!list) return true;
    for (ast_node_t *p = list->first; p; p = p->next) {
        const ast_generic_param_t *gp = (const ast_generic_param_t *)p;
        if (gp->is_pack && p->next) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)def),
                       "generic parameter pack '...%.*s' must be the last "
                       "parameter",
                       (int)gp->name.len, gp->name.ptr);
            return false;
        }
    }
    return true;
}

static bool check_generic_params_supported(sema_t *sema,
                                           const ast_func_def_t *def) {
    return check_generic_param_list(sema, &def->generic, (ast_node_t *)def);
}

/* 泛型函数参数表里的参数包信息：fixed_count = 包之前的参数个数；
 * pack_gp = 包参数节点（无包 NULL）。 */
static size_t func_pack_info(const ast_func_def_t *def, ast_node_t **pack_gp) {
    size_t fixed = 0;
    if (pack_gp) *pack_gp = NULL;
    if (!def) return 0;
    for (ast_node_t *p = def->generic.first; p; p = p->next) {
        if (((ast_generic_param_t *)p)->is_pack) {
            if (pack_gp) *pack_gp = p;
            return fixed;
        }
        fixed++;
    }
    return fixed;
}

/* 函数**值参数**（非包）中位于 pack 之前的个数——即调用点额外实参的起点。 */
static size_t fn_fixed_value_param_count(const ast_func_def_t *def) {
    size_t n = 0;
    for (ast_node_t *p = def->params; p; p = p->next) {
        if (p->kind == AST_VAR_DEF &&
            ((ast_var_def_t *)p)->type_expr &&
            ((ast_var_def_t *)p)->type_expr->kind == AST_PACK_TYPE)
            break;
        n++;
    }
    return n;
}
/* 构造 `<T1,...,TN>` 类型表达式（元素为 AST_TYPE_REF）。 */
static ast_node_t *make_pack_tuple_type(sema_t *sema,
                                        const sema_generic_instance_t *inst,
                                        uint32_t tb, uint32_t te) {
    ast_node_t *t = ast_tuple_new(sema->arena, tb, te);
    if (!t) return NULL;
    ast_node_t *head = NULL, *last = NULL;
    for (size_t i = 0; i < inst->pack_count; i++) {
        const sema_generic_arg_t *a = &inst->args[inst->pack_offset + i];
        ast_node_t *ref = sema_gen_type_ref(sema, a->type);
        if (!ref) return NULL;
        ref->next = NULL;
        if (!head) head = ref;
        else last->next = ref;
        last = ref;
    }
    ((ast_tuple_t *)t)->elem_types = head;
    ((ast_tuple_t *)t)->elem_types_last = last;
    return t;
}

bool sema_generic_pack_is_param(sema_t *sema, strslice_t name) {
    const sema_generic_instance_t *inst = sema ? sema->generics.active_inst : NULL;
    return inst && inst->has_pack && !inst->pack_is_value &&
           inst->pack_param_name.ptr &&
           strslice_eq(inst->pack_param_name, name);
}

bool sema_generic_value_pack_is_param(sema_t *sema, strslice_t name) {
    const sema_generic_instance_t *inst = sema ? sema->generics.active_inst : NULL;
    return inst && inst->has_pack && inst->pack_is_value &&
           inst->pack_name.ptr && strslice_eq(inst->pack_name, name);
}

ast_node_t *sema_generic_value_pack_elem_expr(
    sema_t *sema, const sema_generic_instance_t *inst, size_t index,
    uint32_t tb, uint32_t te) {
    if (!sema || !inst || !inst->has_pack || !inst->pack_is_value ||
        index >= inst->pack_count)
        return NULL;
    const sema_generic_arg_t *a = &inst->args[inst->pack_offset + index];
    if (!a->is_value || !a->value.type) return NULL;
    const type_t *vt = a->value.type;
    ast_node_t *lit = NULL;
    if (type_kind_of(vt) == TYPE_KIND_STR) {
        lit = ast_string_lit_new(sema->arena, tb, te);
        if (!lit) return NULL;
        ((ast_string_lit_t *)lit)->text = a->value.s;
    } else {
        uint64_t v = 0;
        if (type_kind_of(vt) == TYPE_KIND_BOOL) {
            v = a->value.b ? 1 : 0;
        } else if (vt->name.len == 3 && vt->name.ptr &&
                   memcmp(vt->name.ptr, "u64", 3) == 0) {
            v = a->value.u;
        } else {
            switch (vt->size) {
            case 1: v = (uint8_t)a->value.i; break;
            case 2: v = (uint16_t)a->value.i; break;
            case 4: v = (uint32_t)a->value.i; break;
            default: v = (uint64_t)a->value.i; break;
            }
        }
        lit = ast_int_lit_new(sema->arena, tb, te);
        if (!lit) return NULL;
        ((ast_int_lit_t *)lit)->value = v;
        ((ast_int_lit_t *)lit)->type = vt->name;
    }
    lit->is_generated = true;
    return lit;
}

ast_node_t *sema_generic_value_pack_tuple_type_expr(
    sema_t *sema, const sema_generic_instance_t *inst, uint32_t tb,
    uint32_t te) {
    if (!sema || !inst || !inst->has_pack || !inst->pack_is_value) return NULL;
    ast_node_t *t = ast_tuple_new(sema->arena, tb, te);
    if (!t) return NULL;
    ast_node_t *head = NULL, *last = NULL;
    for (size_t i = 0; i < inst->pack_count; i++) {
        const sema_generic_arg_t *a = &inst->args[inst->pack_offset + i];
        if (!a->is_value || !a->value.type) return NULL;
        ast_node_t *ref = sema_gen_type_ref(sema, a->value.type);
        if (!ref) return NULL;
        ref->next = NULL;
        if (last) last->next = ref;
        else head = ref;
        last = ref;
    }
    ((ast_tuple_t *)t)->elem_types = head;
    ((ast_tuple_t *)t)->elem_types_last = last;
    t->is_generated = true;
    return t;
}

/* 取出内建 `[[noreturn]]` / `[[never]]` 标记（其余装饰器保留）。 */
static ast_node_t *take_noreturn_marker(ast_node_t *decs, bool *flag) {
    ast_node_t *head = NULL, *tail = NULL;
    for (ast_node_t *d = decs; d; d = d->next) {
        if (d->kind == AST_IDENT) {
            strslice_t n = ((ast_ident_t *)d)->name;
            if ((n.len == 8 && memcmp(n.ptr, "noreturn", 8) == 0) ||
                (n.len == 5 && memcmp(n.ptr, "never", 5) == 0)) {
                *flag = true;
                continue;
            }
        }
        d->next = NULL;
        if (tail) tail->next = d;
        else head = d;
        tail = d;
    }
    return head;
}

/* 泛型实例的装饰器：`[[d1]] [[d2]] func f[T]` 在**每个实例**上展开为
 *   包装函数（实例名） + 隐藏源函数（实例体）
 * 包装体：`return d1(d2(源))(实参...)`。源函数用同一 Args 实例化，
 * 因而装饰器参数/签名按具体类型生成。 */
static ast_node_t *generic_apply_decorators(sema_t *sema, ast_node_t *decs,
                                            ast_node_t *value, uint32_t tb,
                                            uint32_t te) {
    size_t n = 0;
    for (ast_node_t *d = decs; d; d = d->next) n++;
    if (n == 0) return value;
    ast_node_t **arr = (ast_node_t **)arena_calloc(
        sema->arena, n, sizeof(ast_node_t *), ALIGNOF(max_align_t));
    if (!arr) return value;
    size_t i = 0;
    for (ast_node_t *d = decs; d; d = d->next) arr[i++] = d;
    value->next = NULL;
    for (i = n; i > 0; i--) {
        ast_node_t *callee = arr[i - 1];
        callee->next = NULL;
        if (callee->kind == AST_CALL) {
            ast_call_t *c = (ast_call_t *)callee;
            value->next = NULL;
            if (c->args_last) c->args_last->next = value;
            else c->args = value;
            c->args_last = value;
            value = callee;
            continue;
        }
        ast_node_t *call = ast_call_new(sema->arena, tb, te);
        if (!call) return value;
        ((ast_call_t *)call)->callee = callee;
        ((ast_call_t *)call)->args = value;
        ((ast_call_t *)call)->args_last = value;
        value = call;
    }
    return value;
}

static ast_node_t *generic_decorated_forward_body(
    sema_t *sema, ast_func_def_t *fn, ast_node_t *decs, strslice_t src_name) {
    uint32_t tb = fn->base.tok_begin, te = fn->base.tok_end;
    ast_node_t *args = NULL, *args_last = NULL;
    for (ast_node_t *p = fn->params; p; p = p->next) {
        ast_var_def_t *pd = (ast_var_def_t *)p;
        ast_node_t *a = ast_ident_new(sema->arena, tb, te);
        if (!a) return NULL;
        ((ast_ident_t *)a)->name = pd->name;
        a->next = NULL;
        if (args_last) args_last->next = a;
        else args = a;
        args_last = a;
    }
    ast_node_t *src = ast_ident_new(sema->arena, tb, te);
    if (!src) return NULL;
    ((ast_ident_t *)src)->name = src_name;
    ast_node_t *callee =
        generic_apply_decorators(sema, decs, src, tb, te);
    ast_node_t *call = ast_call_new(sema->arena, tb, te);
    if (!callee || !call) return NULL;
    ((ast_call_t *)call)->callee = callee;
    ((ast_call_t *)call)->args = args;
    ((ast_call_t *)call)->args_last = args_last;

    ast_node_t *stmt;
    if (fn->return_expr) {
        stmt = ast_return_new(sema->arena, tb, te);
        if (stmt) ((ast_return_t *)stmt)->value = call;
    } else {
        stmt = ast_expr_stmt_new(sema->arena, tb, te);
        if (stmt) ((ast_expr_stmt_t *)stmt)->expr = call;
    }
    if (!stmt) return NULL;
    ast_node_t *blk = ast_block_new(sema->arena, tb, te);
    if (!blk) return NULL;
    ((ast_block_t *)blk)->stmts = stmt;
    ((ast_block_t *)blk)->stmts_last = stmt;
    return blk;
}

/* 参数包元素表达式（隐藏形参名）。 */
ast_node_t *sema_generic_pack_arg_expr(sema_t *sema,
                                       const sema_generic_instance_t *inst,
                                       size_t index, uint32_t tb, uint32_t te) {
    if (!sema || !inst || index >= inst->pack_count) return NULL;
    char nm[64];
    snprintf(nm, sizeof nm, "__pack_arg_%llu", (unsigned long long)index);
    ast_node_t *id = ast_ident_new(sema->arena, tb, te);
    if (!id) return NULL;
    ((ast_ident_t *)id)->name = arena_copy(sema->arena, nm, strlen(nm));
    id->is_generated = true;
    return id;
}

ast_node_t *sema_generic_pack_tuple_type_expr(
    sema_t *sema, const sema_generic_instance_t *inst, uint32_t tb,
    uint32_t te) {
    if (!sema || !inst || !inst->has_pack) return NULL;
    return make_pack_tuple_type(sema, inst, tb, te);
}

ast_node_t *sema_generic_pack_tuple_expr(sema_t *sema,
                                         const sema_generic_instance_t *inst,
                                         uint32_t tb, uint32_t te) {
    if (!sema || !inst || !inst->has_pack) return NULL;
    ast_node_t *tuple_ty = make_pack_tuple_type(sema, inst, tb, te);
    ast_node_t *ctor = ast_construct_new(sema->arena, tb, te);
    if (!tuple_ty || !ctor) return NULL;
    ((ast_construct_t *)ctor)->type = tuple_ty;
    ast_node_t *head = NULL, *last = NULL;
    for (size_t i = 0; i < inst->pack_count; i++) {
        ast_node_t *e = sema_generic_pack_arg_expr(sema, inst, i, tb, te);
        if (!e) return NULL;
        e->next = NULL;
        if (!head) head = e;
        else last->next = e;
        last = e;
    }
    ((ast_construct_t *)ctor)->fields = head;
    ((ast_construct_t *)ctor)->fields_last = last;
    return ctor;
}


/* P5b：把 `args: ...Args` 展开为
 *
 *     func f(<fixed params>, __pack_arg_0: T0, ..., __pack_arg_N-1: TN-1)
 *     {
 *         var args: <T0,...,TN-1> = .{ __pack_arg_0, ..., __pack_arg_N-1 };
 *         <原 body>
 *     }
 *
 * compiler/VM 只看到普通定长参数与普通 tuple 变量（设计 §30.2/§30.3）。
 */
static bool materialize_pack_params(sema_t *sema, ast_func_def_t *fn,
                                    sema_generic_instance_t *inst) {
    if (!inst->has_pack) return true;

    ast_node_t *pack_param = NULL;
    for (ast_node_t *p = fn->params; p; p = p->next) {
        if (p->kind == AST_VAR_DEF) {
            ast_var_def_t *vd = (ast_var_def_t *)p;
            if (vd->type_expr && vd->type_expr->kind == AST_PACK_TYPE) {
                pack_param = p;
                break;
            }
        }
    }
    if (!pack_param) return true; /* 包未用作可变参数 */
    if (pack_param->next) {
        diag_error(sema->diag, sema_loc(sema, pack_param),
                   "parameter pack '...%.*s' must be the last parameter",
                   (int)inst->pack_name.len, inst->pack_name.ptr);
        return false;
    }
    ast_pack_type_t *pt =
        (ast_pack_type_t *)((ast_var_def_t *)pack_param)->type_expr;
    if (!strslice_eq(pt->name, inst->pack_name)) {
        diag_error(sema->diag, sema_loc(sema, pack_param),
                   "parameter '...%.*s' does not name the generic parameter "
                   "pack",
                   (int)pt->name.len, pt->name.ptr);
        return false;
    }

    /* 重建固定参数链 + 合成参数。 */
    ast_node_t *head = NULL, *last = NULL;
    for (ast_node_t *p = fn->params; p && p != pack_param; p = p->next) {
        p->next = NULL;
        if (!head) head = p;
        else last->next = p;
        last = p;
    }

    uint32_t tb = pack_param->tok_begin, te = pack_param->tok_end;
    char nm[64];
    for (size_t i = 0; i < inst->pack_count; i++) {
        snprintf(nm, sizeof nm, "__pack_arg_%llu", (unsigned long long)i);
        ast_node_t *pv = ast_var_def_new(sema->arena, tb, te);
        if (!pv) return false;
        ast_var_def_t *vd = (ast_var_def_t *)pv;
        vd->name = arena_copy(sema->arena, nm, strlen(nm));
        const sema_generic_arg_t *a = &inst->args[inst->pack_offset + i];
        const type_t *ptype = a->type;
        vd->type_expr = sema_gen_type_ref(sema, ptype);
        if (!vd->type_expr) return false;
        pv->is_generated = true;
        pv->next = NULL;
        if (!head) head = pv;
        else last->next = pv;
        last = pv;
    }
    fn->params = head;
    fn->params_last = last;

    /* 变参形参名用于 AST 折叠（args[i]/len(args)/...args/裸 args）。 */
    inst->pack_param_name = ((ast_var_def_t *)pack_param)->name;

    /* 不再合成 `var args: <...> = ...` 局部 tuple：所有 args 使用点都在
       sema 中做 AST 变换（见 sema_generic_pack_*）。 */
    return true;
}

static const type_t *resolve_instance_sig(sema_t *sema, ast_func_def_t *fn) {
    size_t n = sema_count_siblings(fn->params);
    const type_t **ptypes = NULL;
    if (n > 0) {
        ptypes = (const type_t **)allocator_new_ex(
            sema->vm->alloc, "type_t*", sizeof(type_t *), NULL, NULL, NULL, n);
        if (!ptypes) panic("sema: out of memory resolving generic instance");
    }
    bool ok = true;
    size_t j = 0;
    for (ast_node_t *p = fn->params; p; p = p->next, j++) {
        ast_var_def_t *vd = (ast_var_def_t *)p;
        const type_t *t = sema_resolve_type_slot(sema, &vd->type_expr);
        if (!t) {
            diag_error(sema->diag, sema_loc(sema, p),
                       "generic instance '%.*s': unknown type in parameter '%.*s'",
                       (int)fn->name.len, fn->name.ptr,
                       (int)vd->name.len, vd->name.ptr);
            ok = false;
        }
        ptypes[j] = t;
    }
    const type_t *rt = NULL;
    if (fn->return_expr) {
        rt = sema_resolve_type_slot(sema, &fn->return_expr);
        if (!rt) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                       "generic instance '%.*s': unknown return type",
                       (int)fn->name.len, fn->name.ptr);
            ok = false;
        }
    }
    if (!ok) {
        if (ptypes) allocator_free(sema->vm->alloc, (void **)&ptypes);
        return NULL;
    }
    const type_t *sig = type_func_sig(sema->vm, ptypes, n, rt, false);
    const sema_type_t *st = sig ? sema_type_register(sema, sig) : NULL;
    if (st) fn->sig_id = st->id;
    if (ptypes) allocator_free(sema->vm->alloc, (void **)&ptypes);
    return sig;
}

static void push_instance_bindings(sema_t *sema,
                                   const sema_generic_instance_t *inst);

static value_t *ct_to_value(sema_t *sema, const sema_ct_const_t *c);

/* 类型参数绑定层：把 type_t 作为 type value、comptime 值作为真实 value
   定义到临时 VM scope。`T`/`N` 在签名/函数体里按普通名字解析。
   函数 walk 的 capture/param 层都压在这层之下，因此嵌套实例化自然看到
   外层绑定。 */
typedef struct {
    vec_t *types;
} types_ctx_t;

static types_ctx_t types_enter(sema_t *sema, struct module_t *owner) {
    types_ctx_t c = {sema->types};
    if (owner && owner->sema && owner->sema != sema && owner->sema->types)
        sema->types = owner->sema->types;
    return c;
}

static void types_leave(sema_t *sema, types_ctx_t c) {
    sema->types = c.types;
}

/* M7 跨模块归属：实例化/walk 期间切到定义模块的词法环境。 */
typedef struct {
    sema_scope_t     *global_scope;
    struct module_t  *current_module;
    vec_t            *tokens;
} owner_ctx_t;

static owner_ctx_t owner_enter(sema_t *sema, struct module_t *owner,
                               vec_t *tokens) {
    owner_ctx_t saved = {sema->global_scope, sema->current_module,
                         sema->tokens};
    if (owner) {
        sema->current_module = owner;
        if (owner->sema_global) sema->global_scope = owner->sema_global;
    }
    if (tokens) sema->tokens = tokens;
    return saved;
}

static void owner_leave(sema_t *sema, owner_ctx_t saved) {
    sema->global_scope = saved.global_scope;
    sema->current_module = saved.current_module;
    sema->tokens = saved.tokens;
}

/* 定义模块上下文中实例化的**本地**泛型：mangling 加模块前缀，避免与导入方
   同名泛型实例冲突。跨模块符号本身已有 alias$Name 前缀，不重复。 */
static strslice_t owner_local_prefix(sema_t *sema, module_t *owner,
                                     strslice_t name) {
    if (!owner || !owner->canonical || !sema->module_path) return STRSLICE_EMPTY;
    if (strcmp(owner->canonical, sema->module_path) == 0) return STRSLICE_EMPTY;
    uint32_t h = 2166136261u;
    for (const char *p = owner->canonical; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    char buf[320];
    int n = snprintf(buf, sizeof buf, "m%08x$%.*s", h, (int)name.len, name.ptr);
    if (n <= 0 || (size_t)n >= sizeof buf) return STRSLICE_EMPTY;
    return arena_copy(sema->arena, buf, (size_t)n);
}

static void define_instance_params(sema_t *sema,
                                   const sema_generic_instance_t *inst) {
    if (!inst) return;
    if (inst->parent) define_instance_params(sema, inst->parent);
    if (!inst->args) return;
    for (size_t ai = 0; ai < inst->param_count && ai < inst->arg_count; ai++) {
        if (!inst->param_names || !inst->param_names[ai].ptr) continue;
        const sema_generic_arg_t *a = &inst->args[ai];
        value_t *tv = a->is_value ? ct_to_value(sema, &a->value)
                                  : type_as_value(sema->vm, a->type);
        if (!tv) continue;
        char nm[256];
        generic_name_to_cstr(inst->param_names[ai], nm, sizeof nm);
        scope_define(sema->vm, sema->vm->current_scope, nm, tv);
    }

    /* P9：comptime 值参数包绑定为 `[M]<elem>` 编译期数组（供 CTFE 读取；
       表达式里的 `N[i]` 会折叠成字面量，不依赖运行时绑定）。 */
    if (inst->has_pack && inst->pack_is_value && inst->pack_count > 0 &&
        inst->args && inst->pack_offset < inst->arg_count) {
        const sema_generic_arg_t *first = &inst->args[inst->pack_offset];
        const type_t *et = first->value.type;
        if (et && first->is_value) {
            size_t n = inst->pack_count;
            const type_t *at = type_array_intern(sema->vm, et, n);
            if (at) {
                size_t esz = et->size;
                unsigned char *buf = (unsigned char *)allocator_new_ex(
                    sema->vm->alloc, "sema.packbuf", n * esz, NULL, NULL, NULL,
                    1);
                if (buf) {
                    memset(buf, 0, n * esz);
                    for (size_t k = 0; k < n; k++) {
                        const sema_ct_const_t *c =
                            &inst->args[inst->pack_offset + k].value;
                        uint64_t raw = 0;
                        if (et->kind == TYPE_KIND_BOOL)
                            raw = c->b ? 1 : 0;
                        else if (et->name.len == 3 && et->name.ptr &&
                                 memcmp(et->name.ptr, "u64", 3) == 0)
                            raw = c->u;
                        else
                            raw = (uint64_t)c->i;
                        memcpy(buf + k * esz, &raw, esz <= 8 ? esz : 8);
                    }
                    void *data = value_alloc_data_copy(sema->vm->alloc, at, buf);
                    allocator_free(sema->vm->alloc, (void **)&buf);
                    if (data) {
                        value_t *av = value_make(sema->vm, at, data);
                        if (av) {
                            char pn[256];
                            generic_name_to_cstr(inst->pack_name, pn,
                                                 sizeof pn);
                            scope_define(sema->vm,
                                         sema->vm->current_scope, pn, av);
                        }
                    }
                }
            }
        }
    }
}

static void push_instance_bindings(sema_t *sema,
                                   const sema_generic_instance_t *inst) {
    vm_push_scope(sema->vm);
    define_instance_params(sema, inst);
}

static strslice_t *copy_param_names(sema_t *sema,
                                    const ast_generic_params_t *list,
                                    size_t count) {
    if (!sema || !list || count == 0) return NULL;
    strslice_t *names = (strslice_t *)arena_alloc(
        sema->arena, count * sizeof(strslice_t), ALIGNOF(max_align_t));
    if (!names) return NULL;
    size_t i = 0;
    for (ast_node_t *p = list->first; p && i < count; p = p->next, i++)
        names[i] = ((ast_generic_param_t *)p)->name;
    return names;
}

/* ---- comptime 值实参（P5） ---- */

/* 原始位模式（无符号视角）。 */
static uint64_t ct_raw_u(const sema_ct_const_t *c) {
    if (!c || !c->type) return 0;
    if (c->type->kind == TYPE_KIND_BOOL) return c->b ? 1u : 0u;
    /* encode_int 的约定：u64 存 u，其余整数存 i（位模式）。 */
    if (c->type->name.len == 3 && c->type->name.ptr &&
        memcmp(c->type->name.ptr, "u64", 3) == 0)
        return c->u;
    uint64_t raw = 0;
    switch (c->type->size) {
    case 1: raw = (uint8_t)c->i; break;
    case 2: raw = (uint16_t)c->i; break;
    case 4: raw = (uint32_t)c->i; break;
    default: raw = (uint64_t)c->i; break;
    }
    return raw;
}

/* 把整型/布尔常量转换到声明类型（宽度截断；范围外按位截断，与 C 语义一致）。 */
static bool ct_convert_scalar(const sema_ct_const_t *in, const type_t *dst,
                              sema_ct_const_t *out) {
    if (!in || !dst || !out) return false;
    if (type_kind_of(dst) == TYPE_KIND_BOOL) {
        uint64_t raw = ct_raw_u(in);
        memset(out, 0, sizeof(*out));
        out->type = dst;
        out->b = raw != 0;
        return true;
    }
    if (type_kind_of(in->type) != TYPE_KIND_INT &&
        type_kind_of(in->type) != TYPE_KIND_ENUM)
        return false;
    if (type_kind_of(dst) != TYPE_KIND_INT) return false;
    uint64_t raw = ct_raw_u(in);
    memset(out, 0, sizeof(*out));
    out->type = dst;
    if (dst == in->type) {
        *out = *in;
        return true;
    }
    out->i = (int64_t)raw;
    if (dst->name.len == 3 && dst->name.ptr &&
        memcmp(dst->name.ptr, "u64", 3) == 0)
        out->u = raw;
    return true;
}

/* 编译期常量 → 真实 value（CTFE / 类型槽位求值需要带 data 的值）。 */
static value_t *ct_to_value(sema_t *sema, const sema_ct_const_t *c) {
    if (!sema || !c || !c->type) return NULL;
    vm_t *vm = sema->vm;
    const type_t *t = c->type;
    if (type_kind_of(t) == TYPE_KIND_BOOL) {
        bool b = c->b;
        void *data = value_alloc_data_copy(vm->alloc, t, &b);
        return data ? value_make(vm, t, data) : NULL;
    }
    if (type_kind_of(t) == TYPE_KIND_INT || type_kind_of(t) == TYPE_KIND_ENUM) {
        uint64_t raw = ct_raw_u(c);
        unsigned char buf[16];
        memset(buf, 0, sizeof buf);
        size_t sz = t->size <= sizeof buf ? t->size : sizeof buf;
        memcpy(buf, &raw, sz);
        void *data = value_alloc_data_copy(vm->alloc, t, buf);
        return data ? value_make(vm, t, data) : NULL;
    }
    return NULL; /* P5 值参数限定标量；字符串/复合后续再扩展 */
}

static void mangle_value(string_t *out, const sema_ct_const_t *c) {
    if (!c || !c->type) {
        string_append_cstr(out, "?");
        return;
    }
    if (type_kind_of(c->type) == TYPE_KIND_BOOL) {
        string_append_cstr(out, c->b ? "true" : "false");
        return;
    }
    char buf[32];
    if (type_kind_of(c->type) == TYPE_KIND_INT) {
        bool is_u64 = c->type->name.len == 3 && c->type->name.ptr &&
                      memcmp(c->type->name.ptr, "u64", 3) == 0;
        if (is_u64)
            snprintf(buf, sizeof buf, "%llu", (unsigned long long)c->u);
        else if (c->type->name.ptr && c->type->name.len > 0 &&
                 c->type->name.ptr[0] == 'u')
            snprintf(buf, sizeof buf, "%llu",
                     (unsigned long long)ct_raw_u(c));
        else
            snprintf(buf, sizeof buf, "%lld", (long long)c->i);
        string_append_cstr(out, buf);
        return;
    }
    snprintf(buf, sizeof buf, "%lld", (long long)c->i);
    string_append_cstr(out, buf);
}

static void mangle_arg(string_t *out, const sema_generic_arg_t *a) {
    if (!a) {
        string_append_cstr(out, "?");
        return;
    }
    if (a->is_value)
        mangle_value(out, &a->value);
    else
        sema_generic_mangle_type(out, a->type);
}

static sema_generic_arg_t *copy_args(sema_t *sema,
                                     const sema_generic_arg_t *args,
                                     size_t n) {
    if (!sema || n == 0) return NULL;
    sema_generic_arg_t *dst = (sema_generic_arg_t *)arena_alloc(
        sema->arena, n * sizeof(sema_generic_arg_t), ALIGNOF(max_align_t));
    if (!dst) return NULL;
    memcpy(dst, args, n * sizeof(sema_generic_arg_t));
    return dst;
}

/* 检查 `T extends PATTERN` 结构约束。只核对类型实参；值参数不允许约束
   （parser 已拒绝）。失败返回 false（诊断已记录）。 */
static bool check_generic_constraints(sema_t *sema,
                                      const ast_generic_params_t *list,
                                      const sema_generic_arg_t *args,
                                      size_t arg_count, location_t at) {
    if (!list) return true;

    /* 约束环境：所有已确定的类型参数（PATTERN 里 `IWriter[T]` 的 T 指向
       当前被检查的类型实参）。 */
    strslice_t names[SEMA_GENERIC_MAX_DEPTH];
    const type_t *types[SEMA_GENERIC_MAX_DEPTH];
    size_t en = 0;
    size_t j = 0;
    for (ast_node_t *p = list->first; p && j < arg_count;
         p = p->next, j++) {
        const ast_generic_param_t *gp = (const ast_generic_param_t *)p;
        if (gp->is_pack) break;
        if (args[j].is_value || !args[j].type) continue;
        if (en < SEMA_GENERIC_MAX_DEPTH) {
            names[en] = gp->name;
            types[en] = args[j].type;
            en++;
        }
    }

    strslice_t *saved_names = sema->pattern_env.names;
    const type_t **saved_types = sema->pattern_env.types;
    size_t saved_count = sema->pattern_env.count;
    sema->pattern_env.names = names;
    sema->pattern_env.types = types;
    sema->pattern_env.count = en;

    bool ok = true;
    size_t i = 0;
    for (ast_node_t *p = list->first; p && i < arg_count; p = p->next, i++) {
        const ast_generic_param_t *gp = (const ast_generic_param_t *)p;
        if (gp->is_pack) break;
        if (!gp->constraint || args[i].is_value) continue;
        if (!sema_pattern_match(sema, gp->constraint, args[i].type)) {
            char tn[96];
            sema_type_name(args[i].type, tn, sizeof tn);
            diag_error(sema->diag, at,
                       "type argument '%s' does not satisfy the constraint on "
                       "'%.*s' (declared with 'extends' in the generic "
                       "parameter list)",
                       tn, (int)gp->name.len, gp->name.ptr);
            ok = false;
            break;
        }
    }

    sema->pattern_env.names = saved_names;
    sema->pattern_env.types = saved_types;
    sema->pattern_env.count = saved_count;
    return ok;
}


static const type_t *instantiate_func_impl(
    sema_t *sema, sema_symbol_t *generic_sym, const sema_generic_arg_t *args,
    size_t arg_count, const ast_node_t *site,
    const sema_generic_instance_t **out, strslice_t name_prefix,
    sema_generic_instance_t *parent) {
    if (out) *out = NULL;
    if (!sema || !generic_sym || !generic_sym->ast ||
        generic_sym->ast->kind != AST_FUNC_DEF) {
        return NULL;
    }
    ast_func_def_t *def = (ast_func_def_t *)generic_sym->ast;
    if (def->generic.count == 0) return NULL;
    if (!check_generic_params_supported(sema, def)) return NULL;

    /* 跨模块泛型：定义所属模块的 token pool（tok 下标按它解释）。 */
    vec_t *def_tokens = generic_sym->generic_tokens
                            ? (vec_t *)generic_sym->generic_tokens
                            : (parent && parent->tokens
                                   ? (vec_t *)parent->tokens
                                   : sema->tokens);
    /* 实例归属：定义模块（跨模块符号/方法 owner）→ 父实例 → 当前上下文。 */
    module_t *owner_module =
        generic_sym->module
            ? generic_sym->module
            : (parent && parent->owner_module ? parent->owner_module
                                              : sema->current_module);
    /* owner 正在本 sema 内（module->sema 尚未挂上）：走本地实例路径。 */
    if (owner_module && !owner_module->sema) owner_module = NULL;

    location_t at = site ? sema_loc(sema, (ast_node_t *)site)
                         : sema_loc(sema, (ast_node_t *)def);

    ast_node_t *pack_gp = NULL;
    size_t fixed_count = func_pack_info(def, &pack_gp);
    bool has_pack = pack_gp != NULL;

    if (!has_pack) {
        if (arg_count != def->generic.count) {
            bool has_default = false;
            for (ast_node_t *p = def->generic.first; p; p = p->next) {
                if (((ast_generic_param_t *)p)->default_expr) has_default = true;
            }
            if (has_default && arg_count < def->generic.count) {
                diag_error(sema->diag, at,
                           "generic function '%.*s': omitting arguments with "
                           "defaults is not implemented yet (M7 P3)",
                           (int)def->name.len, def->name.ptr);
            } else {
                diag_error(sema->diag, at,
                           "generic function '%.*s' expects %zu generic "
                           "argument(s), got %zu",
                           (int)def->name.len, def->name.ptr,
                           def->generic.count, arg_count);
            }
            return NULL;
        }
    } else if (arg_count < fixed_count) {
        diag_error(sema->diag, at,
                   "generic function '%.*s' expects at least %zu generic "
                   "argument(s), got %zu",
                   (int)def->name.len, def->name.ptr, fixed_count, arg_count);
        return NULL;
    }
    {
        size_t i = 0;
        for (ast_node_t *p = def->generic.first; p && i < arg_count;
             p = p->next, i++) {
            const ast_generic_param_t *gp = (const ast_generic_param_t *)p;
            if (gp->is_pack) break; /* 其余都是 pack 元素 */
            if (!!gp->is_comptime != !!args[i].is_value) {
                diag_error(sema->diag, at,
                           "generic function '%.*s': argument %zu ('%.*s') must "
                           "be %s",
                           (int)def->name.len, def->name.ptr, i + 1,
                           (int)gp->name.len, gp->name.ptr,
                           gp->is_comptime ? "a comptime value" : "a type");
                return NULL;
            }
        }
        if (has_pack) {
            bool pack_value =
                ((const ast_generic_param_t *)pack_gp)->is_comptime;
            for (size_t k = fixed_count; k < arg_count; k++) {
                if (!!args[k].is_value != pack_value) {
                    if (pack_value)
                        diag_error(
                            sema->diag, at,
                            "comptime value parameter pack '...%.*s' only "
                            "accepts compile-time values",
                            (int)((ast_generic_param_t *)pack_gp)->name.len,
                            ((ast_generic_param_t *)pack_gp)->name.ptr);
                    else
                        diag_error(
                            sema->diag, at,
                            "parameter pack '%.*s' only accepts type arguments "
                            "in M7",
                            (int)((ast_generic_param_t *)pack_gp)->name.len,
                            ((ast_generic_param_t *)pack_gp)->name.ptr);
                    return NULL;
                }
            }
        }
    }

    if (sema->generics.depth >= sema->generics.max_depth) {
        diag_error(sema->diag, at,
                   "generic instantiation depth exceeds limit (%u) while "
                   "instantiating '%.*s'",
                   (unsigned)sema->generics.max_depth, (int)def->name.len,
                   def->name.ptr);
        return NULL;
    }
    if (!check_generic_constraints(sema, &def->generic, args, arg_count, at))
        return NULL;

    /* ---- mangling + 缓存 ---- */
    string_t *sb = string_new(sema->vm->alloc);
    if (!sb) panic("sema: out of memory building generic mangling");
    strslice_t base_name =
        name_prefix.len > 0 ? name_prefix
                            : owner_local_prefix(sema, owner_module, def->name);
    if (base_name.len == 0) base_name = def->name;
    string_append_bytes(sb, base_name.ptr, base_name.len);
    for (size_t i = 0; i < arg_count; i++) {
        string_append_char(sb, '$');
        mangle_arg(sb, &args[i]);
    }
    strslice_t iname =
        arena_copy(sema->arena, string_cstr(sb), string_len(sb));
    string_free(&sb);

    sema_generic_instance_t *hit = NULL;
    for (sema_generic_instance_t *e = sema->generics.instances; e; e = e->next) {
        if (strslice_eq(e->name, iname)) {
            hit = e;
            break;
        }
    }
    if (hit) {
        if (out) *out = hit;
        /* 在途实例的签名在 walk body 之前已解析完毕：递归实例化/递归调用
           直接复用签名（设计 §7「先查缓存」）。 */
        return hit->sig;
    }

    /* ---- 新实例：登记（先入表，环/递归能看到在途标记） ---- */
    sema_generic_instance_t *inst = (sema_generic_instance_t *)allocator_new_ex(
        sema->vm->alloc, "sema_generic_instance_t",
        sizeof(sema_generic_instance_t), NULL, NULL, NULL, 1);
    if (!inst) panic("sema: out of memory allocating generic instance");
    memset(inst, 0, sizeof(*inst));
    inst->name = iname;
    inst->kind = SEMA_GEN_FUNC;
    inst->def = (const ast_node_t *)def;
    inst->param_names = copy_param_names(sema, &def->generic, fixed_count);
    inst->param_count = fixed_count;
    inst->parent = parent;
    inst->tokens = def_tokens;
    inst->site = at;
    inst->caller = sema->current_instance;
    inst->owner_module = owner_module;
    inst->args = copy_args(sema, args, arg_count);
    inst->arg_count = arg_count;
    if (has_pack) {
        inst->has_pack = true;
        inst->pack_name = ((ast_generic_param_t *)pack_gp)->name;
        inst->pack_offset = fixed_count;
        inst->pack_count = arg_count - fixed_count;
        inst->pack_is_value =
            ((ast_generic_param_t *)pack_gp)->is_comptime;
    }
    inst->in_progress = true;
    inst->depth = sema->generics.depth + 1;
    inst->next = sema->generics.instances;
    sema->generics.instances = inst;
    if (out) *out = inst;

    sema->generics.depth++;

    /* ---- 类型参数绑定层（签名 + 作用域树解析用） ---- */
    push_instance_bindings(sema, inst);

    /* ---- 从原始 token 区间重新解析（天然的「干净 AST 副本」） ---- */
    parser_t *rp =
        parser_create(sema->vm->alloc, sema->arena, def_tokens);
    if (!rp) panic("sema: out of memory reparsing generic definition");
    rp->pos = def->base.tok_begin;
    rp->diag = sema->diag;
    ast_node_t *parsed = parse_func_def(rp);
    parser_destroy(&rp);
    if (!parsed || parsed->kind != AST_FUNC_DEF) {
        diag_error(sema->diag, at,
                   "internal: failed to materialize generic instance '%.*s'",
                   (int)def->name.len, def->name.ptr);
        vm_pop_scope(sema->vm);
        sema->generics.depth--;
        inst->in_progress = false;
        return NULL;
    }
    ast_func_def_t *fn = (ast_func_def_t *)parsed;
    /* 解析克隆不会复制装饰器链；模板上的装饰器仍要**每个实例**展开一次
       （fn->decorators 在本函数内被消费，def 的链保持不变）。 */
    if (!fn->decorators && ((const ast_func_def_t *)def)->decorators)
        fn->decorators = ((const ast_func_def_t *)def)->decorators;
    bool def_noreturn = ((const ast_func_def_t *)def)->is_noreturn;

    /* 装饰器在**每个实例**上展开：源函数保留实例体，实例名变成包装函数。
       源函数与实例共享参数/返回类型（同一 Args 实例），因此装饰器签名
       按具体类型生成。 */
    ast_node_t *decor_src = NULL;
    strslice_t decor_src_name = {0};
    if (fn->decorators) {
        ast_node_t *decs = fn->decorators;
        fn->decorators = NULL;
        char hb[512];
        int hn = snprintf(hb, sizeof hb, "__decor_src$%.*s",
                          (int)iname.len, iname.ptr);
        if (hn <= 0 || (size_t)hn >= sizeof hb) hn = 0;
        decor_src_name = arena_copy(sema->arena, hb, (size_t)hn);
        ast_func_def_t *src = (ast_func_def_t *)arena_calloc(
            sema->arena, 1, sizeof(ast_func_def_t), ALIGNOF(max_align_t));
        if (!src) panic("sema: out of memory allocating decorated source");
        static const ast_func_def_t zero_fn;
        *src = zero_fn;
        src->base = fn->base;
        src->base.next = NULL;
        src->base.is_generated = true;
        src->base.is_exported = false;
        src->name = decor_src_name;
        src->captures = fn->captures;
        src->scope_annot_names = fn->scope_annot_names;
        src->scope_annot_count = fn->scope_annot_count;
        src->params = fn->params;
        src->params_last = fn->params_last;
        src->return_expr = fn->return_expr;
        src->body = fn->body;
        src->is_comptime = false;
        src->owner_module = owner_module;
        decor_src = &src->base;

        fn->body = generic_decorated_forward_body(sema, fn, decs,
                                                  decor_src_name);
        if (!fn->body) {
            vm_pop_scope(sema->vm);
            sema->generics.depth--;
            inst->in_progress = false;
            return NULL;
        }
    }
    fn->generic = (ast_generic_params_t){0};
    fn->base.is_exported = false;
    fn->is_comptime = false;
    fn->is_noreturn = def_noreturn;
    fn->is_variadic = false;
    fn->fid = 0;
    fn->sig_id = 0;
    fn->capture_type_id = 0;
    fn->name = iname;
    fn->base.is_generated = true;
    fn->owner_module = owner_module;

    /* P5b：参数包展开（合成参数 + 入口 tuple 聚合）。 */
    if (!materialize_pack_params(sema, fn, inst)) {
        vm_pop_scope(sema->vm);
        sema->generics.depth--;
        inst->in_progress = false;
        return NULL;
    }
    /* 重新解析出来的 body 还没经过前脱糖（foreach 等）：单函数脱糖。 */
    desugar_t ds = desugar_make(sema->arena, sema->diag, def_tokens);
    desugar_func(&ds, fn);

    /* ---- 登记为普通函数（sema 队列 + prog->funcs） ---- */
    sema_func_t *sf = (sema_func_t *)allocator_new_ex(
        sema->vm->alloc, "sema_func_t", sizeof(sema_func_t), NULL, NULL, NULL, 1);
    if (!sf) panic("sema: out of memory allocating generic instance function");
    memset(sf, 0, sizeof(*sf));
    sf->def = parsed;
    sf->def_scope = (owner_module && owner_module->sema_global)
                        ? owner_module->sema_global
                        : sema->global_scope;
    sf->is_local = false;
    sf->is_walked = false;
    sf->generic_inst = inst; /* Pass 3b 队列循环据此重绑类型参数后 walk */
    sema_func_id_alloc(sema, fn);
    inst->fid = fn->fid;
    inst->instance = parsed;
    inst->func = sf;
    vec_t *dst_funcs = sema->funcs;
    ast_program_t *dst_prog = (sema->prog && sema->prog->kind == AST_PROGRAM)
                                  ? (ast_program_t *)sema->prog : NULL;
    if (owner_module && owner_module->sema) {
        if (owner_module->sema->funcs) dst_funcs = owner_module->sema->funcs;
        if (owner_module->sema->prog &&
            owner_module->sema->prog->kind == AST_PROGRAM)
            dst_prog = (ast_program_t *)owner_module->sema->prog;
    }
    vec_push(dst_funcs, sema->vm->alloc, sf);
    if (dst_prog) ast_append(&dst_prog->funcs, &dst_prog->funcs_last, NULL, parsed);

    /* ---- 签名 → 作用域树（函数体由全局 worklist walk） ---- */
    sema_generic_instance_t *saved_active = sema->generics.active_inst;
    sema->generics.active_inst = inst;
    owner_ctx_t owner_saved = owner_enter(sema, owner_module, def_tokens);
    types_ctx_t types_saved = types_enter(sema, owner_module);
    const type_t *sig = resolve_instance_sig(sema, fn);
    if (!sig) {
        types_leave(sema, types_saved);
        owner_leave(sema, owner_saved);
        sema->generics.active_inst = saved_active;
        vm_pop_scope(sema->vm);
        sema->generics.depth--;
        inst->in_progress = false;
        return NULL;
    }
    inst->sig = sig;
    sema_build_func_scope(sema, sf);

    /* 装饰器源函数：登记普通函数符号 + 进入 walk 队列；包装体的
       `__decor_src$...` 标识符据此解析。 */
    if (decor_src) {
        ((ast_func_def_t *)decor_src)->sig_id = fn->sig_id;
        sema_func_t *src_sf = (sema_func_t *)allocator_new_ex(
            sema->vm->alloc, "sema_func_t", sizeof(sema_func_t), NULL, NULL,
            NULL, 1);
        if (!src_sf) panic("sema: out of memory allocating decorator source");
        memset(src_sf, 0, sizeof(*src_sf));
        src_sf->def = decor_src;
        src_sf->def_scope = sf->def_scope;
        src_sf->is_local = false;
        src_sf->is_walked = false;
        src_sf->generic_inst = inst;
        sema_func_id_alloc(sema, (ast_func_def_t *)decor_src);
        sema_symbol_t src_sym = {
            .kind = SEMA_SYM_FUNC,
            .ast = decor_src,
            .fid = ((ast_func_def_t *)decor_src)->fid,
            .type = sig,
            .is_active = true,
        };
        sema_scope_define(sf->def_scope, decor_src_name, &src_sym);
        vec_push(dst_funcs, sema->vm->alloc, src_sf);
        if (dst_prog)
            ast_append(&dst_prog->funcs, &dst_prog->funcs_last, NULL,
                       decor_src);
        sema_build_func_scope(sema, src_sf);
    }
    types_leave(sema, types_saved);
    owner_leave(sema, owner_saved);
    sema->generics.active_inst = saved_active;

    vm_pop_scope(sema->vm);
    sema->generics.depth--;
    return sig;
}

const type_t *sema_generic_instantiate_func(
    sema_t *sema, sema_symbol_t *generic_sym, const sema_generic_arg_t *args,
    size_t arg_count, const ast_node_t *site,
    const sema_generic_instance_t **out) {
    return instantiate_func_impl(sema, generic_sym, args, arg_count, site, out,
                                 generic_sym->generic_prefix, NULL);
}

const sema_generic_instance_t *sema_generic_find_type_instance(
    sema_t *sema, const type_t *t) {
    if (!sema || !t) return NULL;
    const type_t *core = type_strip_quals(t);
    for (sema_generic_instance_t *e = sema->generics.instances; e;
         e = e->next) {
        if (e->kind == SEMA_GEN_FUNC || !e->type) continue;
        if (type_strip_quals(e->type) == core) return e;
    }
    return NULL;
}

static const type_t *instantiate_inferred_impl(
    sema_t *sema, sema_symbol_t *generic_sym,
    const sema_generic_arg_t *explicit_args, size_t explicit_count,
    value_t *const *arg_shadows, size_t argc, const ast_node_t *site,
    const sema_generic_instance_t **out, strslice_t name_prefix,
    sema_generic_instance_t *parent);

const type_t *sema_generic_instantiate_method_inferred(    sema_t *sema, sema_symbol_t *method_sym,
    const sema_generic_instance_t *owner, strslice_t owner_name,
    value_t *const *arg_shadows, size_t argc, const ast_node_t *site,
    const sema_generic_instance_t **out) {
    if (out) *out = NULL;
    if (!sema || !method_sym || !method_sym->ast ||
        method_sym->ast->kind != AST_FUNC_DEF)
        return NULL;
    ast_func_def_t *fn = (ast_func_def_t *)method_sym->ast;
    if (fn->generic.count == 0) return NULL;

    /* mangling 前缀：Owner$method（Owner 是实例 mangling 名/普通 struct 名）。 */
    string_t *sb = string_new(sema->vm->alloc);
    if (!sb) panic("sema: out of memory building method mangling");
    if (owner_name.len > 0) {
        string_append_bytes(sb, owner_name.ptr, owner_name.len);
        string_append_char(sb, '$');
    }
    string_append_bytes(sb, fn->name.ptr, fn->name.len);
    strslice_t prefix =
        arena_copy(sema->arena, string_cstr(sb), string_len(sb));
    string_free(&sb);

    if (owner) push_instance_bindings(sema, owner);
    owner_ctx_t owner_saved =
        owner_enter(sema, owner ? owner->owner_module : sema->current_module,
                    owner ? (vec_t *)owner->tokens : NULL);
    const type_t *sig = instantiate_inferred_impl(
        sema, method_sym, NULL, 0, arg_shadows, argc, site, out, prefix,
        (sema_generic_instance_t *)owner);
    owner_leave(sema, owner_saved);
    if (owner) vm_pop_scope(sema->vm);
    return sig;
}

const type_t *sema_generic_instantiate_method_explicit(
    sema_t *sema, sema_symbol_t *method_sym,
    const sema_generic_instance_t *owner, strslice_t owner_name,
    const sema_generic_arg_t *args, size_t arg_count,
    value_t *const *arg_shadows, size_t argc, const ast_node_t *site,
    const sema_generic_instance_t **out) {
    if (out) *out = NULL;
    if (!sema || !method_sym || !method_sym->ast ||
        method_sym->ast->kind != AST_FUNC_DEF)
        return NULL;
    ast_func_def_t *fn = (ast_func_def_t *)method_sym->ast;
    if (fn->generic.count == 0) return NULL;

    string_t *sb = string_new(sema->vm->alloc);
    if (!sb) panic("sema: out of memory building method mangling");
    if (owner_name.len > 0) {
        string_append_bytes(sb, owner_name.ptr, owner_name.len);
        string_append_char(sb, '$');
    }
    string_append_bytes(sb, fn->name.ptr, fn->name.len);
    strslice_t prefix =
        arena_copy(sema->arena, string_cstr(sb), string_len(sb));
    string_free(&sb);

    if (owner) push_instance_bindings(sema, owner);
    owner_ctx_t owner_saved =
        owner_enter(sema, owner ? owner->owner_module : sema->current_module,
                    owner ? (vec_t *)owner->tokens : NULL);
    const type_t *sig = instantiate_inferred_impl(
        sema, method_sym, args, arg_count, arg_shadows, argc, site, out, prefix,
        (sema_generic_instance_t *)owner);
    owner_leave(sema, owner_saved);
    if (owner) vm_pop_scope(sema->vm);
    return sig;
}

/* Pass 3b 队列循环：重绑类型参数后 walk 实例函数体。
 * 关键：实例化只做「签名 + 建树」并入队；函数体由外层 for 循环迭代处理。
 * 因此递归实例化（每次类型参数都变化）不会加深 C 调用栈——深度上限由
 * inst->depth（逻辑链深度）而不是栈深度约束。 */
bool sema_generic_drain_pending(sema_t *sema) {
    if (!sema || !sema->funcs) return false;
    bool progress = false;
    for (size_t i = 0; i < vec_len(sema->funcs); i++) {
        sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
        if (!sf || !sf->generic_inst || sf->is_walked) continue;
        if (!sf->scope) { /* 建树失败（已诊断）：标记避免死循环 */
            sf->is_walked = true;
            continue;
        }
        sema_generic_walk_instance(sema, sf);
        sf->is_walked = true;
        progress = true;
    }
    return progress;
}

void sema_generic_walk_instance(sema_t *sema, sema_func_t *sf) {
    if (!sema || !sf || !sf->generic_inst) return;
    if (!sf->scope) return; /* 建树失败（已诊断） */
    sema_generic_instance_t *inst = sf->generic_inst;
    push_instance_bindings(sema, inst);
    uint32_t saved_depth = sema->generics.depth;
    sema_generic_instance_t *saved_active = sema->generics.active_inst;
    sema_generic_instance_t *saved_cur = sema->current_instance;
    size_t diag_before = diag_count(sema->diag);
    sema->generics.depth = inst->depth;
    sema->generics.active_inst = inst;
    sema->current_instance = inst;
    owner_ctx_t owner_saved =
        owner_enter(sema, inst->owner_module, (vec_t *)inst->tokens);
    sema_walk_function(sema, sf);
    owner_leave(sema, owner_saved);
    sema->current_instance = saved_cur;
    sema->generics.active_inst = saved_active;
    sema->generics.depth = saved_depth;
    vm_pop_scope(sema->vm);
    inst->in_progress = false;
    sf->is_walked = true;

    /* §18：实例体诊断发生后补实例化链 note（定义点错误 + 调用点链）。 */
    if (diag_count(sema->diag) > diag_before) {
        for (sema_generic_instance_t *c = inst; c; c = c->caller) {
            if (!c->site.filename) continue;
            diag_note(sema->diag, c->site, "instantiated from '%.*s'",
                      (int)c->name.len, c->name.ptr);
        }
    }
}

bool sema_generic_rewrite_type_expr(sema_t *sema, ast_node_t **slot) {
    if (!sema || !slot || !*slot || (*slot)->kind != AST_INDEX) return false;
    ast_index_t *ix = (ast_index_t *)*slot;
    if (!ix->object || ix->object->kind != AST_IDENT) return false;
    strslice_t gname = ((ast_ident_t *)ix->object)->name;
    sema_symbol_t *gs =
        sema->global_scope ? sema_scope_find_local(sema->global_scope, gname)
                           : NULL;
    if (!gs || !gs->is_generic) return false;
    const type_t *t = resolve_type_expr(sema, *slot);
    if (!t) return true; /* 已诊断 */
    /* 只有 struct 有"按运行时类型名查成员/静态属性"的路径；enum/union/别名
       继续以 AST_INDEX 形式交给各自的解析分支（如 enum variant）。 */
    if (type_kind_of(t) != TYPE_KIND_STRUCT) return true;
    ast_node_t *id =
        ast_ident_new(sema->arena, (*slot)->tok_begin, (*slot)->tok_end);
    if (!id) return true;
    ((ast_ident_t *)id)->name = t->name; /* 实例的运行时类型名（mangling） */
    id->is_generated = true;
    id->next = (*slot)->next;
    *slot = id;
    return true;
}

/* ===========================================================================
 * P4：泛型类型实例化（struct / enum / union / type 别名）
 *
 * 与函数实例同一套机制：重新解析声明 → 类型参数绑定层 → 走普通类型流水线
 * （struct 复用 struct_compose 的开放对象/密封；成员方法手工注册）→ 合成
 * 声明追加 prog->funcs。compiler 仍然只看到普通声明。
 * =========================================================================== */

static bool decl_kind_of(const ast_node_t *def, uint8_t *kind) {
    if (!def || !kind) return false;
    switch (def->kind) {
    case AST_STRUCT_DEF: *kind = SEMA_GEN_STRUCT; return true;
    case AST_ENUM_DEF:   *kind = SEMA_GEN_ENUM;   return true;
    case AST_UNION_DEF:  *kind = SEMA_GEN_UNION;  return true;
    case AST_TYPE_DEF:   *kind = SEMA_GEN_ALIAS;  return true;
    default: return false;
    }
}

static const ast_generic_params_t *decl_generic(const ast_node_t *def) {
    if (!def) return NULL;
    switch (def->kind) {
    case AST_STRUCT_DEF: return &((const ast_struct_def_t *)def)->generic;
    case AST_ENUM_DEF:   return &((const ast_enum_def_t *)def)->generic;
    case AST_UNION_DEF:  return &((const ast_union_def_t *)def)->generic;
    case AST_TYPE_DEF:   return &((const ast_type_def_t *)def)->generic;
    default: return NULL;
    }
}

static strslice_t decl_name(const ast_node_t *def) {
    if (!def) return STRSLICE_EMPTY;
    switch (def->kind) {
    case AST_STRUCT_DEF: return ((const ast_struct_def_t *)def)->name;
    case AST_ENUM_DEF:   return ((const ast_enum_def_t *)def)->name;
    case AST_UNION_DEF:  return ((const ast_union_def_t *)def)->name;
    case AST_TYPE_DEF:   return ((const ast_type_def_t *)def)->name;
    default: return STRSLICE_EMPTY;
    }
}

static sema_symbol_t *ensure_type_symbol(sema_t *sema, strslice_t name,
                                         ast_node_t *def) {
    sema_symbol_t *sym = sema_scope_find_local(sema->global_scope, name);
    if (!sym) {
        sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = def};
        sym = sema_scope_define(sema->global_scope, name, &init);
    }
    if (sym) {
        sym->ast = def;      /* 实例声明（诊断/成员查找用） */
        sym->is_generic = false;
    }
    return sym;
}

/* 物化 struct 实例的开放类型对象（与 struct_compose.c 的
 * materialize_struct_type 同构）：字段类型表达式里的递归 `Node[T]` 需要
 * 在字段解析**之前**就能拿到这个对象。 */
static const type_t *materialize_instance_struct(sema_t *sema,
                                                 ast_struct_def_t *sd,
                                                 sema_symbol_t *sym) {
    vm_t *vm = sema->vm;
    const type_t *t = type_struct_alloc_open(vm);
    if (!t) return NULL;
    type_struct_mark_unique(vm, t); /* 命名实例：独立身份 */
    if (!vm->struct_types) vm->struct_types = vec_new(vm->alloc, false);
    vec_push(vm->struct_types, vm->alloc, (void *)t);
    const sema_type_t *st = sema_type_register(sema, t);
    if (!st) return NULL;
    sd->type_id = st->id;
    if (!type_set_name(vm, t, sd->name)) return NULL;
    value_t *tv = type_as_value(vm, t);
    if (!tv) return NULL;
    char nb[256];
    generic_name_to_cstr(sd->name, nb, sizeof nb);
    scope_define(vm, vm->current_scope, nb, tv);
    sym->type = t; /* 开放对象；is_active 由 compose 在 seal 后置位 */
    return t;
}

/* 注册实例的成员（方法/静态属性）。与 pass1d_struct_members 同构，但：
 *  - 类型参数绑定层在线（签名/字段类型解析可解析 T）；
 *  - 方法函数记录 generic_inst = 类型实例 → Pass 3b walk 前重绑 T；
 *  - 方法签名立即解析（Pass 2 可能已经跑完）。 */
static void instantiate_struct_members(sema_t *sema, ast_struct_def_t *sd,
                                       sema_symbol_t *sym,
                                       sema_generic_instance_t *inst) {
    if (!sd->members) {
        if (sd->implements && sym->type)
            (void)sema_check_struct_implements(sema, sym->type, sd->implements,
                                               (ast_node_t *)sd);
        return;
    }
    sema_scope_t *mscope = sema_scope_new(sema->vm->alloc, SEMA_SCOPE_GLOBAL,
                                          sema->global_scope);
    if (!mscope) panic("sema: out of memory creating generic member scope");
    sema_scope_add_child(sema->global_scope, mscope);
    sym->member_scope = mscope;

    sema_t *osema = (inst->owner_module && inst->owner_module->sema)
                        ? inst->owner_module->sema
                        : sema;
    sema_scope_t *gsemascope =
        (inst->owner_module && inst->owner_module->sema_global)
            ? inst->owner_module->sema_global
            : sema->global_scope;
    ast_program_t *owner_prog =
        (osema->prog && osema->prog->kind == AST_PROGRAM)
            ? (ast_program_t *)osema->prog
            : NULL;

    for (ast_node_t *m = sd->members; m; m = m->next) {
        if (m->kind == AST_FUNC_DEF) {
            ast_func_def_t *fn = (ast_func_def_t *)m;
            fn->owner_module = inst->owner_module;
            bool noreturn_marker = false;
            fn->decorators =
                take_noreturn_marker(fn->decorators, &noreturn_marker);
            if (noreturn_marker) fn->is_noreturn = true;
            if (fn->generic.count > 0) {
                /* P4b：泛型方法只登记符号，调用点实例化。 */
                sema_symbol_t ginit = {.kind = SEMA_SYM_FUNC,
                                       .ast = m,
                                       .is_generic = true,
                                       .is_noreturn = fn->is_noreturn,
                                       .is_active = true};
                if (!sema_scope_define(mscope, fn->name, &ginit)) {
                    diag_error(sema->diag, sema_loc(sema, m),
                               "struct '%.*s': duplicate method '%.*s'",
                               (int)sd->name.len, sd->name.ptr,
                               (int)fn->name.len, fn->name.ptr);
                }
                continue;
            }
            sema_symbol_t init = {.kind = SEMA_SYM_FUNC};
            ast_node_t *member_src = NULL;
            strslice_t member_src_name = {0};
            if (fn->decorators) {
                ast_node_t *decs = fn->decorators;
                fn->decorators = NULL;
                char hb[512];
                int hn = snprintf(hb, sizeof hb, "__decor_src$%.*s$%.*s",
                                  (int)sd->name.len, sd->name.ptr,
                                  (int)fn->name.len, fn->name.ptr);
                if (hn <= 0 || (size_t)hn >= sizeof hb) hn = 0;
                member_src_name =
                    arena_copy(sema->arena, hb, (size_t)hn);
                ast_func_def_t *src = (ast_func_def_t *)arena_calloc(
                    sema->arena, 1, sizeof(ast_func_def_t),
                    ALIGNOF(max_align_t));
                if (!src)
                    panic("sema: out of memory allocating decorated method");
                static const ast_func_def_t zero_method;
                *src = zero_method;
                src->base = fn->base;
                src->base.next = NULL;
                src->base.is_generated = true;
                src->name = member_src_name;
                src->params = fn->params;
                src->params_last = fn->params_last;
                src->return_expr = fn->return_expr;
                src->body = fn->body;
                src->owner_module = inst->owner_module;
                member_src = &src->base;
                fn->body = generic_decorated_forward_body(
                    sema, fn, decs, member_src_name);
                if (!fn->body) {
                    diag_error(sema->diag, sema_loc(sema, m),
                               "cannot build decorated method wrapper");
                    continue;
                }
            }
            sema_symbol_t *msym = sema_scope_define(mscope, fn->name, &init);
            if (!msym) {
                diag_error(sema->diag, sema_loc(sema, m),
                           "struct '%.*s': duplicate method '%.*s'",
                           (int)sd->name.len, sd->name.ptr,
                           (int)fn->name.len, fn->name.ptr);
                continue;
            }
            msym->is_active = true;
            msym->ast = m;
            msym->is_noreturn = fn->is_noreturn;
            sema_func_t *sf = (sema_func_t *)allocator_new_ex(
                sema->vm->alloc, "sema_func_t", sizeof(sema_func_t), NULL, NULL,
                NULL, 1);
            if (!sf) panic("sema: out of memory allocating generic method");
            memset(sf, 0, sizeof(*sf));
            sf->def = m;
            sf->def_scope = mscope;
            sf->generic_inst = inst; /* Pass 3b walk 前重绑类型参数 */
            sema_func_id_alloc(sema, fn);
            msym->fid = fn->fid;
            const type_t *sig = resolve_instance_sig(sema, fn);
            if (sig) msym->type = sig;
            vec_push(sema->funcs, sema->vm->alloc, sf);
            sema_build_func_scope(sema, sf);
            if (member_src && sig) {
                ((ast_func_def_t *)member_src)->sig_id = fn->sig_id;
                sema_func_t *src_sf = (sema_func_t *)allocator_new_ex(
                    sema->vm->alloc, "sema_func_t", sizeof(sema_func_t), NULL,
                    NULL, NULL, 1);
                if (!src_sf)
                    panic("sema: out of memory allocating decorated method "
                          "source");
                memset(src_sf, 0, sizeof(*src_sf));
                src_sf->def = member_src;
                src_sf->def_scope = gsemascope;
                src_sf->generic_inst = inst;
                sema_func_id_alloc(sema, (ast_func_def_t *)member_src);
                sema_symbol_t src_sym = {
                    .kind = SEMA_SYM_FUNC,
                    .ast = member_src,
                    .fid = ((ast_func_def_t *)member_src)->fid,
                    .type = sig,
                    .is_active = true,
                };
                sema_scope_define(gsemascope, member_src_name, &src_sym);
                vec_push(osema->funcs, sema->vm->alloc, src_sf);
                if (owner_prog)
                    ast_append(&owner_prog->funcs, &owner_prog->funcs_last,
                               NULL, member_src);
                sema_build_func_scope(sema, src_sf);
            }
        } else if (m->kind == AST_VAR_DEF) {
            ast_var_def_t *vd = (ast_var_def_t *)m;
            sema_symbol_t init = {.kind = SEMA_SYM_VAR,
                                  .is_comptime = vd->is_comptime};
            sema_symbol_t *msym = sema_scope_define(mscope, vd->name, &init);
            if (!msym) {
                diag_error(sema->diag, sema_loc(sema, m),
                           "struct '%.*s': duplicate attribute '%.*s'",
                           (int)sd->name.len, sd->name.ptr,
                           (int)vd->name.len, vd->name.ptr);
                continue;
            }
            if (vd->is_comptime) {
                sema_eval_comptime_var(sema, vd, mscope);
            } else {
                sema_eval_global_var(sema, vd, mscope);
            }
        }
    }
    if (sd->implements && sym->type) {
        (void)sema_check_struct_implements(sema, sym->type, sd->implements,
                                           (ast_node_t *)sd);
    }
}

static const type_t *build_struct_instance(sema_t *sema, ast_struct_def_t *sd,
                                           sema_generic_instance_t *inst) {
    sema_symbol_t *sym = ensure_type_symbol(sema, sd->name, (ast_node_t *)sd);
    if (!sym) return NULL;
    const type_t *open = materialize_instance_struct(sema, sd, sym);
    if (!open) return NULL;
    inst->type = open;
    sema_compose_result_t r =
        sema_struct_compose_def(sema, sd, sema->global_scope);
    if (r != SEMA_COMPOSE_OK) {
        if (r == SEMA_COMPOSE_DEFERRED) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)sd),
                       "generic type instance '%.*s' could not be completed "
                       "(a field inlines an unsealed struct by value)",
                       (int)sd->name.len, sd->name.ptr);
        }
        return NULL;
    }
    inst->type = sym->type;
    /* 泛型实例的装饰器在实例化点执行：字段/方法 metadata 与类型装饰器
       都绑定到具体实例类型。 */
    sema_metadata_apply_members(sema, sym->type, sd, sema->global_scope);
    sema_metadata_apply_type(sema, sym->type, sd->decorators,
                             sema->global_scope);
    instantiate_struct_members(sema, sd, sym, inst);
    return inst->type;
}

static const type_t *build_enum_instance(sema_t *sema, ast_enum_def_t *ed,
                                         sema_generic_instance_t *inst) {
    sema_symbol_t *sym = ensure_type_symbol(sema, ed->name, (ast_node_t *)ed);
    if (!sym) return NULL;
    if (!sema_eval_enum_def(sema, ed, sema->global_scope)) return NULL;
    inst->type = sym->type;
    return inst->type;
}

static const type_t *build_union_instance(sema_t *sema, ast_union_def_t *ud,
                                          sema_generic_instance_t *inst) {
    sema_symbol_t *sym = ensure_type_symbol(sema, ud->name, (ast_node_t *)ud);
    if (!sym) return NULL;
    vm_t *vm = sema->vm;

    /* 与 struct 同构：先分配开放 union 并登记，字段里的 `own *U[T]`
       才能拿到同一个 type_t；最后再 seal。 */
    const type_t *open = type_union_open(vm);
    if (!open) return NULL;
    type_union_mark_unique(vm, open);
    const sema_type_t *st = sema_type_register(sema, open);
    if (!st) return NULL;
    ud->type_id = st->id;
    if (!type_set_name(vm, open, ud->name)) return NULL;
    value_t *tv = type_as_value(vm, open);
    if (!tv) return NULL;
    char nb[256];
    if (ud->name.len >= sizeof nb) return NULL;
    memcpy(nb, ud->name.ptr, ud->name.len);
    nb[ud->name.len] = '\0';
    scope_define(vm, vm->current_scope, nb, tv);
    sym->type = open;
    inst->type = open;

    bool ok = true;
    for (ast_node_t *fn = ud->fields; fn; fn = fn->next) {
        ast_struct_field_t *sf = (ast_struct_field_t *)fn;
        bool dup = false;
        for (ast_node_t *prev = ud->fields; prev && prev != fn;
             prev = prev->next) {
            ast_struct_field_t *pf = (ast_struct_field_t *)prev;
            if (pf->name.len == sf->name.len && pf->name.ptr &&
                memcmp(pf->name.ptr, sf->name.ptr, sf->name.len) == 0) {
                diag_error(sema->diag, sema_loc(sema, fn),
                           "union '%.*s': duplicate member name '%.*s'",
                           (int)ud->name.len, ud->name.ptr,
                           (int)sf->name.len, sf->name.ptr);
                dup = true;
                break;
            }
        }
        if (dup) {
            ok = false;
            continue;
        }
        const type_t *ft = sema_resolve_type_slot(sema, &sf->type);
        if (!ft) {
            diag_error(sema->diag, sema_loc(sema, sf->type),
                       "union '%.*s': unknown field type for '%.*s'",
                       (int)ud->name.len, ud->name.ptr, (int)sf->name.len,
                       sf->name.ptr);
            ok = false;
            continue;
        }
        if (type_strip_quals(ft) == open) {
            diag_error(sema->diag, sema_loc(sema, sf->type),
                       "union '%.*s' contains itself by value in '%.*s'; use "
                       "own/ref pointer or slice indirection",
                       (int)ud->name.len, ud->name.ptr, (int)sf->name.len,
                       sf->name.ptr);
            ok = false;
            continue;
        }
        type_union_add_member(vm, open, sf->name);
        type_union_set_member_type(vm, open, ft);
    }
    if (!ok) return NULL;
    const type_t *sealed = type_union_seal(vm, open);
    if (!sealed) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)ud),
                   "generic union instance '%.*s' could not be completed",
                   (int)ud->name.len, ud->name.ptr);
        return NULL;
    }
    inst->type = sealed;
    sym->type = sealed;
    sym->is_active = true;
    sym->flow_init = true;
    return sealed;
}

static const type_t *build_alias_instance(sema_t *sema, ast_type_def_t *td,
                                          sema_generic_instance_t *inst) {
    /* 先走 CTFE：泛型别名 RHS 里可能出现 `T extends PATTERN ? A : B`
       这类类型计算，只有 CTFE 能折叠（resolve_type_expr 只认静态构造）。 */
    ctfe_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.vm = sema->vm;
    ctx.sema = sema;
    ctx.sema_scope = sema->global_scope;
    ctx.budget = 100000;
    ctx.max_depth = 128;
    value_t *sh = ctfe_eval(&ctx, td->expr);
    if (sh && !value_is_error(sema->vm, sh) &&
        value_is_type(sh, TYPE_KIND_TYPE)) {
        const type_t *t = value_as(sh, const type_t *);
        inst->type = t;
        return t;
    }
    /* 包展开等 CTFE 不覆盖的形态回退到类型解析器。 */
    const type_t *t = resolve_type_expr(sema, td->expr);
    if (!t) return NULL;
    inst->type = t;
    return t;
}

const type_t *sema_generic_instantiate_type(
    sema_t *sema, sema_symbol_t *generic_sym, const sema_generic_arg_t *args,
    size_t arg_count, const ast_node_t *site,
    const sema_generic_instance_t **out) {
    if (out) *out = NULL;
    if (!sema || !generic_sym || !generic_sym->ast) return NULL;
    ast_node_t *def = generic_sym->ast;
    uint8_t kind = 0;
    if (!decl_kind_of(def, &kind)) return NULL;
    const ast_generic_params_t *glist = decl_generic(def);
    vec_t *def_tokens = generic_sym->generic_tokens
                            ? (vec_t *)generic_sym->generic_tokens
                            : sema->tokens;
    size_t ntp = glist ? glist->count : 0;
    if (ntp == 0) return NULL;
    if (!check_generic_param_list(sema, glist, def)) return NULL;
    bool has_pack = false;
    size_t fixed_ntp = ntp;
    ast_generic_param_t *pack_gp = NULL;
    for (ast_node_t *pg = glist->first; pg; pg = pg->next) {
        ast_generic_param_t *gp = (ast_generic_param_t *)pg;
        if (gp->is_pack) {
            has_pack = true;
            pack_gp = gp;
            fixed_ntp--;
        }
    }
    if (has_pack && kind != SEMA_GEN_ALIAS && kind != SEMA_GEN_STRUCT) {
        diag_error(sema->diag, sema_loc(sema, def),
                   "only type aliases and structs may have generic parameter "
                   "packs (enum/union instances need fixed tag shape)");
        return NULL;
    }

    location_t at = site ? sema_loc(sema, (ast_node_t *)site)
                         : sema_loc(sema, def);
    strslice_t base = generic_sym->generic_prefix.len
                          ? generic_sym->generic_prefix
                          : decl_name(def);
    if (!generic_sym->generic_prefix.len) {
        module_t *owner_mod = generic_sym->module
                                  ? generic_sym->module
                                  : sema->current_module;
        strslice_t op = owner_local_prefix(sema, owner_mod, base);
        if (op.len) base = op;
    }

    if (has_pack ? (arg_count < fixed_ntp) : (arg_count != ntp)) {
        diag_error(sema->diag, at,
                   has_pack
                       ? "generic type '%.*s' expects at least %zu generic "
                         "argument(s), got %zu"
                       : "generic type '%.*s' expects %zu generic argument(s), "
                         "got %zu",
                   (int)base.len, base.ptr, has_pack ? fixed_ntp : ntp,
                   arg_count);
        return NULL;
    }
    {
        size_t i = 0;
        for (ast_node_t *p = glist->first; p && i < fixed_ntp;
             p = p->next, i++) {
            const ast_generic_param_t *gp = (const ast_generic_param_t *)p;
            if (!!gp->is_comptime != !!args[i].is_value) {
                diag_error(sema->diag, at,
                           "generic type '%.*s': argument %zu ('%.*s') must be %s",
                           (int)base.len, base.ptr, i + 1,
                           (int)gp->name.len, gp->name.ptr,
                           gp->is_comptime ? "a comptime value" : "a type");
                return NULL;
            }
        }
        /* 参数包元素：当前只支持类型包（`<...Args>` 展开）。 */
        if (has_pack) {
            for (size_t i = fixed_ntp; i < arg_count; i++) {
                if (args[i].is_value) {
                    diag_error(sema->diag, at,
                               "comptime value parameter packs are not "
                               "implemented yet (M7 P8)");
                    return NULL;
                }
            }
        }
    }
    if (sema->generics.depth >= sema->generics.max_depth) {
        diag_error(sema->diag, at,
                   "generic instantiation depth exceeds limit (%u) while "
                   "instantiating '%.*s'",
                   (unsigned)sema->generics.max_depth, (int)base.len,
                   base.ptr);
        return NULL;
    }
    if (!check_generic_constraints(sema, glist, args, arg_count, at))
        return NULL;

    /* ---- mangling + 缓存 ---- */
    string_t *sb = string_new(sema->vm->alloc);
    if (!sb) panic("sema: out of memory building generic mangling");
    string_append_bytes(sb, base.ptr, base.len);
    for (size_t i = 0; i < arg_count; i++) {
        string_append_char(sb, '$');
        mangle_arg(sb, &args[i]);
    }
    strslice_t iname =
        arena_copy(sema->arena, string_cstr(sb), string_len(sb));
    string_free(&sb);

    /* 跨模块全局去重：类型实例是 VM 级共享实体（同一个 mangling 名必须
       对应同一个 type_t，否则会出现同名但字段表不同的两个类型）。 */
    if (iname.len > 0) {
        char gkey[512];
        if (iname.len < sizeof gkey) {
            memcpy(gkey, iname.ptr, iname.len);
            gkey[iname.len] = '\0';
            if (sema->vm->generic_types) {
                const type_t *known =
                    (const type_t *)strmap_get(sema->vm->generic_types, gkey);
                if (known) return known;
            }
        }
    }

    for (sema_generic_instance_t *e = sema->generics.instances; e; e = e->next) {
        if (!strslice_eq(e->name, iname)) continue;
        if (e->kind != kind) {
            diag_error(sema->diag, at,
                       "internal: generic mangling collision '%.*s'",
                       (int)iname.len, iname.ptr);
            return NULL;
        }
        if (out) *out = e;
        if (e->type) return e->type;
        diag_error(sema->diag, at,
                   "recursive generic type '%.*s' is not supported yet "
                   "(only struct instances may reference themselves through "
                   "pointers/slices)",
                   (int)base.len, base.ptr);
        return NULL;
    }

    /* 结构去重：同名缓存之外，按「泛型定义 + 实参身份」再查一次。不同
       mangling 前缀（方法/嵌套实例的 generic_prefix 不同）会生成不同名字，
       但语义上是同一个实例类型——必须复用同一个 type_t，否则 C 侧会出现
       两个布局相同却类型不同的 struct（`s_65` vs `s_77` 不兼容赋值）。 */
    for (sema_generic_instance_t *e = sema->generics.instances; e;
         e = e->next) {
        if (e->kind != kind || e->def != def ||
            e->arg_count != arg_count || !e->type)
            continue;
        bool same = true;
        for (size_t k = 0; k < arg_count; k++) {
            if (args[k].is_value != e->args[k].is_value) {
                same = false;
                break;
            }
            if (!args[k].is_value) {
                const type_t *x =
                    args[k].type ? type_strip_quals(args[k].type) : NULL;
                const type_t *y = e->args[k].type
                                      ? type_strip_quals(e->args[k].type)
                                      : NULL;
                if (x != y) {
                    same = false;
                    break;
                }
            } else if (memcmp(&args[k].value, &e->args[k].value,
                              sizeof args[k].value) != 0) {
                same = false;
                break;
            }
        }
        if (same) {
            if (out) *out = e;
            return e->type;
        }
    }

    sema_generic_instance_t *inst = (sema_generic_instance_t *)allocator_new_ex(
        sema->vm->alloc, "sema_generic_instance_t",
        sizeof(sema_generic_instance_t), NULL, NULL, NULL, 1);
    if (!inst) panic("sema: out of memory allocating generic type instance");
    memset(inst, 0, sizeof(*inst));
    inst->name = iname;
    inst->kind = kind;
    inst->def = def;
    inst->param_names = copy_param_names(sema, glist, fixed_ntp);
    inst->param_count = fixed_ntp;
    inst->args = copy_args(sema, args, arg_count);
    inst->arg_count = arg_count;
    if (has_pack) {
        inst->has_pack = true;
        inst->pack_name = pack_gp->name;
        inst->pack_offset = fixed_ntp;
        inst->pack_count = arg_count - fixed_ntp;
    }
    inst->in_progress = true;
    inst->depth = sema->generics.depth + 1;
    inst->tokens = def_tokens;
    inst->site = at;
    inst->caller = sema->current_instance;
    inst->owner_module =
        generic_sym->module ? generic_sym->module : sema->current_module;
    if (inst->owner_module && !inst->owner_module->sema)
        inst->owner_module = NULL;
    inst->next = sema->generics.instances;
    sema->generics.instances = inst;
    if (out) *out = inst;

    sema->generics.depth++;
    push_instance_bindings(sema, inst);

    /* ---- 从定义点 token 区间重新解析（干净 AST 副本） ---- */
    parser_t *rp = parser_create(sema->vm->alloc, sema->arena, def_tokens);
    if (!rp) panic("sema: out of memory reparsing generic type definition");
    rp->pos = def->tok_begin;
    rp->diag = sema->diag;
    ast_node_t *parsed = NULL;
    switch (def->kind) {
    case AST_STRUCT_DEF: parsed = parse_struct_def(rp); break;
    case AST_ENUM_DEF:   parsed = parse_enum_def(rp);   break;
    case AST_UNION_DEF:  parsed = parse_union_def(rp);  break;
    case AST_TYPE_DEF:   parsed = parse_type_def(rp);   break;
    default: break;
    }
    parser_destroy(&rp);
    if (!parsed || parsed->kind != def->kind) {
        diag_error(sema->diag, at,
                   "internal: failed to materialize generic type instance '%.*s'",
                   (int)base.len, base.ptr);
        vm_pop_scope(sema->vm);
        sema->generics.depth--;
        inst->in_progress = false;
        return NULL;
    }
    parsed->is_exported = false;
    parsed->is_generated = true;

    const type_t *result = NULL;
    vec_t *saved_tokens = sema->tokens;
    if (def_tokens) sema->tokens = def_tokens; /* 定义模块诊断位置 */
    owner_ctx_t owner_saved = owner_enter(sema, inst->owner_module, NULL);
    types_ctx_t types_saved = types_enter(sema, inst->owner_module);
    switch (kind) {
    case SEMA_GEN_STRUCT: {
        ast_struct_def_t *sd = (ast_struct_def_t *)parsed;
        const ast_struct_def_t *orig = (const ast_struct_def_t *)def;
        sd->decorators = orig->decorators;
        sd->metadata_decorators = orig->metadata_decorators;
        sd->generic = (ast_generic_params_t){0};
        sd->name = iname;
        inst->instance = parsed;
        /* 字段类型可能写 `<...Args>`：build 期间需要 active_inst 展开。 */
        sema_generic_instance_t *saved_active = sema->generics.active_inst;
        sema->generics.active_inst = inst;
        result = build_struct_instance(sema, sd, inst);
        sema->generics.active_inst = saved_active;
        break;
    }
    case SEMA_GEN_ENUM: {
        ast_enum_def_t *ed = (ast_enum_def_t *)parsed;
        ed->generic = (ast_generic_params_t){0};
        ed->name = iname;
        inst->instance = parsed;
        result = build_enum_instance(sema, ed, inst);
        break;
    }
    case SEMA_GEN_UNION: {
        ast_union_def_t *ud = (ast_union_def_t *)parsed;
        ud->generic = (ast_generic_params_t){0};
        ud->name = iname;
        inst->instance = parsed;
        result = build_union_instance(sema, ud, inst);
        break;
    }
    case SEMA_GEN_ALIAS: {
        ast_type_def_t *td = (ast_type_def_t *)parsed;
        td->generic = (ast_generic_params_t){0};
        td->name = iname;
        /* 别名右值可能是 `<...Args>`：需要 active_inst 做包展开。 */
        sema_generic_instance_t *saved_active = sema->generics.active_inst;
        sema->generics.active_inst = inst;
        result = build_alias_instance(sema, td, inst);
        sema->generics.active_inst = saved_active;
        break;
    }
    default: break;
    }
    types_leave(sema, types_saved);
    owner_leave(sema, owner_saved);
    sema->tokens = saved_tokens;

    /* 定义模块作用域里登记的实例类型符号，导入方 sema 也要能看到
       （find_global / 成员查找按实例 mangling 名查当前 global_scope）。
       方法 member_scope 仍挂在定义模块作用域下（生命周期随 owner）。 */
    if (result && inst->owner_module && sema->global_scope &&
        inst->owner_module->sema_global) {
        sema_symbol_t *src = sema_scope_find_local(
            inst->owner_module->sema_global, iname);
        if (src && !sema_scope_find_local(sema->global_scope, iname)) {
            sema_symbol_t init = *src;
            sema_scope_define(sema->global_scope, iname, &init);
        }
    }

    if (result && kind != SEMA_GEN_ALIAS) {
        ast_program_t *prog = NULL;
        if (inst->owner_module && inst->owner_module->sema &&
            inst->owner_module->sema->prog &&
            inst->owner_module->sema->prog->kind == AST_PROGRAM)
            prog = (ast_program_t *)inst->owner_module->sema->prog;
        else if (sema->prog && sema->prog->kind == AST_PROGRAM)
            prog = (ast_program_t *)sema->prog;
        if (prog) ast_append(&prog->funcs, &prog->funcs_last, NULL, parsed);
    }

    vm_pop_scope(sema->vm);
    sema->generics.depth--;
    inst->in_progress = false;
    /* VM 级按名登记：跨模块的同一 mangling 名必须复用同一个 type_t。 */
    if (result && sema->vm->generic_types) {
        char gkey[512];
        if (iname.len > 0 && iname.len < sizeof gkey) {
            memcpy(gkey, iname.ptr, iname.len);
            gkey[iname.len] = '\0';
            if (!strmap_get(sema->vm->generic_types, gkey))
                strmap_insert(sema->vm->generic_types, sema->vm->alloc, gkey,
                              (void *)result);
        }
    }
    return result;
}

bool sema_generic_parse_explicit_args(sema_t *sema, sema_symbol_t *generic_sym,
                                      ast_node_t *indices, size_t *out_count,
                                      sema_generic_arg_t **out_args) {
    if (out_count) *out_count = 0;
    if (out_args) *out_args = NULL;
    if (!sema || !generic_sym || !generic_sym->ast || !out_args) return false;

    const ast_generic_params_t *list = NULL;
    switch (generic_sym->ast->kind) {
    case AST_FUNC_DEF:   list = &((ast_func_def_t *)generic_sym->ast)->generic; break;
    case AST_STRUCT_DEF: list = &((ast_struct_def_t *)generic_sym->ast)->generic; break;
    case AST_ENUM_DEF:   list = &((ast_enum_def_t *)generic_sym->ast)->generic; break;
    case AST_UNION_DEF:  list = &((ast_union_def_t *)generic_sym->ast)->generic; break;
    case AST_TYPE_DEF:   list = &((ast_type_def_t *)generic_sym->ast)->generic; break;
    default: return false;
    }
    if (!list || list->count == 0) return false;

    size_t n = sema_count_siblings(indices);
    if (n == 0) return true; /* 调用方按"没有显式实参"处理（推导/默认值） */

    bool has_pack = false;
    for (ast_node_t *p = list->first; p; p = p->next)
        if (((ast_generic_param_t *)p)->is_pack) has_pack = true;

    sema_generic_arg_t *args = (sema_generic_arg_t *)allocator_new_ex(
        sema->vm->alloc, "sema_generic_arg_t", sizeof(sema_generic_arg_t), NULL,
        NULL, NULL, n);
    if (!args) panic("sema: out of memory allocating generic arguments");
    memset(args, 0, n * sizeof(*args));

    ast_node_t *gp_node = list->first;
    ast_node_t *a = indices;
    size_t i = 0;
    bool ok = true;
    for (; a && gp_node && i < n; a = a->next, gp_node = gp_node->next, i++) {
        const ast_generic_param_t *gp = (const ast_generic_param_t *)gp_node;
        if (gp->is_pack) {
            /* 参数包：剩余所有显式实参都是包元素。 */
            if (!gp->is_comptime) {
                for (; a && i < n; a = a->next) {
                    const type_t *t = resolve_type_expr(sema, a);
                    if (!t) {
                        ok = false;
                        break;
                    }
                    args[i].is_value = false;
                    args[i].type = t;
                    i++;
                }
                break;
            }
            /* comptime 值参数包：逐个 CTFE + 编码 + 转换。 */
            const type_t *vt =
                gp->type_expr ? resolve_type_expr(sema, gp->type_expr) : NULL;
            if (!vt) {
                diag_error(
                    sema->diag,
                    sema_loc(sema, (ast_node_t *)generic_sym->ast),
                    "comptime value parameter pack '...%.*s' has an unknown type",
                    (int)gp->name.len, gp->name.ptr);
                ok = false;
                break;
            }
            if (type_kind_of(vt) != TYPE_KIND_INT &&
                type_kind_of(vt) != TYPE_KIND_BOOL) {
                diag_error(
                    sema->diag,
                    sema_loc(sema, (ast_node_t *)generic_sym->ast),
                    "comptime value parameter pack '...%.*s': only "
                    "integer/bool scalar types are supported in M7",
                    (int)gp->name.len, gp->name.ptr);
                ok = false;
                break;
            }
            for (; a && i < n; a = a->next, i++) {
                ctfe_ctx_t ctx;
                memset(&ctx, 0, sizeof ctx);
                ctx.vm = sema->vm;
                ctx.sema = sema;
                ctx.budget = 100000;
                ctx.max_depth = 128;
                value_t *v = ctfe_eval(&ctx, a);
                if (!v || value_is_error(sema->vm, v)) {
                    diag_error(sema->diag, sema_loc(sema, a),
                               "comptime value for pack '...%.*s' is not a "
                               "compile-time constant",
                               (int)gp->name.len, gp->name.ptr);
                    ok = false;
                    break;
                }
                sema_ct_const_t ct;
                sema_ct_const_t converted;
                if (!sema_ct_encode(sema, v, &ct) ||
                    !ct_convert_scalar(&ct, vt, &converted)) {
                    diag_error(sema->diag, sema_loc(sema, a),
                               "comptime value for pack '...%.*s' cannot be "
                               "converted to its declared type",
                               (int)gp->name.len, gp->name.ptr);
                    ok = false;
                    break;
                }
                args[i].is_value = true;
                args[i].type = vt;
                args[i].value = converted;
            }
            break;
        }
        if (!gp->is_comptime) {
            const type_t *t = resolve_type_expr(sema, a);
            if (!t) {
                ok = false;
                break;
            }
            args[i].is_value = false;
            args[i].type = t;
            continue;
        }

        /* comptime 值实参：CTFE 求值 + 编码 + 转换到声明类型。 */
        const type_t *vt =
            gp->type_expr ? resolve_type_expr(sema, gp->type_expr) : NULL;
        if (!vt) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)generic_sym->ast),
                       "comptime value parameter '%.*s' has an unknown type",
                       (int)gp->name.len, gp->name.ptr);
            ok = false;
            break;
        }
        if (type_kind_of(vt) != TYPE_KIND_INT &&
            type_kind_of(vt) != TYPE_KIND_BOOL) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)generic_sym->ast),
                       "comptime value parameter '%.*s': only integer/bool "
                       "scalar types are supported in M7",
                       (int)gp->name.len, gp->name.ptr);
            ok = false;
            break;
        }
        ctfe_ctx_t ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.vm = sema->vm;
        ctx.sema = sema;
        ctx.budget = 100000;
        ctx.max_depth = 128;
        value_t *v = ctfe_eval(&ctx, a);
        if (!v || value_is_error(sema->vm, v)) {
            const char *msg = NULL;
            if (v) {
                error_data_t *ed = (error_data_t *)value_data(v);
                msg = ed && ed->message ? ed->message : NULL;
            }
            diag_error(sema->diag, sema_loc(sema, a),
                       "comptime value argument for '%.*s' is not a "
                       "compile-time constant%s%s",
                       (int)gp->name.len, gp->name.ptr, msg ? ": " : "",
                       msg ? msg : "");
            ok = false;
            break;
        }
        sema_ct_const_t ct;
        if (value_is_type(v, TYPE_KIND_TYPE)) {
            diag_error(sema->diag, sema_loc(sema, a),
                       "comptime value parameter '%.*s' expects a value, got a "
                       "type (write the type for a type parameter instead)",
                       (int)gp->name.len, gp->name.ptr);
            ok = false;
            break;
        }
        if (!sema_ct_encode(sema, v, &ct)) {
            diag_error(sema->diag, sema_loc(sema, a),
                       "comptime value argument for '%.*s' has an unsupported "
                       "type",
                       (int)gp->name.len, gp->name.ptr);
            ok = false;
            break;
        }
        sema_ct_const_t converted;
        if (!ct_convert_scalar(&ct, vt, &converted)) {
            diag_error(sema->diag, sema_loc(sema, a),
                       "comptime value argument for '%.*s' cannot be converted "
                       "to its declared type",
                       (int)gp->name.len, gp->name.ptr);
            ok = false;
            break;
        }
        args[i].is_value = true;
        args[i].type = vt;
        args[i].value = converted;
    }

    if (!ok) {
        allocator_free(sema->vm->alloc, (void **)&args);
        return false;
    }
    if (out_count) *out_count = has_pack ? i : n;
    *out_args = args;
    return true;
}

/* ===========================================================================
 * P3：调用点类型实参推导
 *
 * 形参类型表达式是一棵「类型模式」：T 是洞，其余构造与实参类型逐节点对齐。
 * 匹配只填洞，不做最终类型校验——真正的兼容性检查仍由 value_call 在实例
 * 签名上完成（错误消息与普通函数一致）。
 *
 * 所有权按**不对称**规则参与匹配（用户 2026-10-22 补充）：
 *   fatal 实参 → own 形参 ✓（将亡值被接收为 own）
 *   own/fatal 实参 → ref 形参 ✓（借用）
 *   own 实参 ⇏ fatal 形参（fatal 形参不存在，仅作说明）
 * 推导出的 T 不含形参侧的所有权修饰（`own *T` 对 `fatal *i32` 推出 T=i32）。
 * =========================================================================== */

typedef struct {
    const ast_func_def_t *def;
    const type_t        **types;
    bool                 *set;
    sema_ct_const_t     *values;   /* 值参数绑定（is_value 时有效） */
    bool                 *is_value;/* 该参数是 comptime 值参数 */
    size_t                n;
    bool                  failed;
} infer_t;

static int infer_param_index(const ast_func_def_t *def, strslice_t name) {
    int i = 0;
    for (ast_node_t *p = def->generic.first; p; p = p->next, i++) {
        if (strslice_eq(((ast_generic_param_t *)p)->name, name)) return i;
    }
    return -1;
}

static void infer_bind(sema_t *sema, infer_t *b, size_t idx,
                       const type_t *actual, const ast_node_t *site);

/* 绑定 comptime 值参数；raw 是整型位模式，type 为声明类型。 */
static void infer_bind_value(sema_t *sema, infer_t *b, size_t idx,
                             uint64_t raw, const ast_node_t *site) {
    if (!b || b->failed || idx >= b->n) return;
    ast_node_t *pn = b->def->generic.first;
    for (size_t k = 0; k < idx && pn; k++) pn = pn->next;
    const ast_generic_param_t *gp = (const ast_generic_param_t *)pn;
    const type_t *vt = NULL;
    if (gp && gp->type_expr) vt = resolve_type_expr(sema, gp->type_expr);
    if (!vt) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)site),
                   "cannot resolve the declared type of comptime parameter "
                   "'%.*s'",
                   gp ? (int)gp->name.len : 0, gp ? gp->name.ptr : NULL);
        b->failed = true;
        return;
    }
    sema_ct_const_t ct;
    memset(&ct, 0, sizeof ct);
    ct.type = vt;
    ct.i = (int64_t)raw;
    if (vt->name.len == 3 && vt->name.ptr &&
        memcmp(vt->name.ptr, "u64", 3) == 0)
        ct.u = raw;
    if (b->set[idx]) {
        if (ct_raw_u(&b->values[idx]) != raw) {
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)site),
                       "inconsistent value for comptime parameter '%.*s': "
                       "inferred both %llu and %llu",
                       gp ? (int)gp->name.len : 0, gp ? gp->name.ptr : NULL,
                       (unsigned long long)ct_raw_u(&b->values[idx]),
                       (unsigned long long)raw);
            b->failed = true;
        }
        return;
    }
    b->values[idx] = ct;
    b->is_value[idx] = true;
    b->set[idx] = true;
}

static void infer_bind(sema_t *sema, infer_t *b, size_t idx,
                       const type_t *actual, const ast_node_t *site) {
    if (!b || b->failed || idx >= b->n || !actual) return;
    ast_node_t *pn0 = b->def->generic.first;
    for (size_t k = 0; k < idx && pn0; k++) pn0 = pn0->next;
    if (pn0 && ((const ast_generic_param_t *)pn0)->is_comptime) return;
    if (b->set[idx]) {
        if (!type_equal(sema->vm, b->types[idx], actual)) {
            char a[64], c[64];
            sema_type_name(b->types[idx], a, sizeof a);
            sema_type_name(actual, c, sizeof c);
            ast_node_t *pn = b->def->generic.first;
            for (size_t k = 0; k < idx && pn; k++) pn = pn->next;
            strslice_t nm = pn ? ((ast_generic_param_t *)pn)->name
                               : STRSLICE_EMPTY;
            diag_error(sema->diag, sema_loc(sema, (ast_node_t *)site),
                       "inconsistent type argument for '%.*s': inferred both "
                       "'%s' and '%s'",
                       (int)nm.len, nm.ptr, a, c);
            b->failed = true;
        }
        return;
    }
    b->types[idx] = actual;
    b->set[idx] = true;
}

static void infer_pattern(sema_t *sema, infer_t *b, ast_node_t *pat,
                          const type_t *actual, const ast_node_t *site);

static void infer_pattern_ptr_slice(sema_t *sema, infer_t *b, ast_node_t *pat,
                                    const type_t *actual,
                                    const ast_node_t *site) {
    if (!pat || !actual || !b || b->failed) return;
    ast_node_t *base = NULL;
    if (pat->kind == AST_PTR) {
        ast_ptr_t *p = (ast_ptr_t *)pat;
        const type_t *core = actual;
        if (!core || core->kind != TYPE_KIND_PTR) return;
        base = p->base_type;
        infer_pattern(sema, b, base, ptr_type_base(actual), site);
        return;
    }
    if (pat->kind == AST_SLICE_TYPE) {
        ast_slice_type_t *s = (ast_slice_type_t *)pat;
        const type_t *core = actual;
        if (!core || core->kind != TYPE_KIND_SLICE) return;
        infer_pattern(sema, b, s->elem_type, slice_type_elem(actual), site);
        return;
    }
}

static void infer_pattern(sema_t *sema, infer_t *b, ast_node_t *pat,
                          const type_t *actual, const ast_node_t *site) {
    if (!pat || !actual || !b || b->failed) return;
    switch (pat->kind) {
    case AST_IDENT: {
        strslice_t nm = ((ast_ident_t *)pat)->name;
        int idx = infer_param_index(b->def, nm);
        if (idx >= 0) {
            infer_bind(sema, b, (size_t)idx, actual, site);
            return;
        }
        /* 具体类型名：只做一致性参考，不产生绑定（最终校验在 value_call）。 */
        (void)resolve_type_expr(sema, pat);
        return;
    }
    case AST_SCOPE_ANNOT:
        infer_pattern(sema, b, ((ast_scope_annot_t *)pat)->sub, actual, site);
        return;
    case AST_CONST:
        infer_pattern(sema, b, ((ast_const_t *)pat)->sub,
                      type_strip_quals(actual), site);
        return;
    case AST_VOLATILE:
        infer_pattern(sema, b, ((ast_volatile_t *)pat)->sub,
                      type_strip_quals(actual), site);
        return;
    case AST_OPTION: {
        const type_t *inner =
            type_is_option(actual) ? type_option_inner(actual) : actual;
        infer_pattern(sema, b, ((ast_option_t *)pat)->sub, inner, site);
        return;
    }
    case AST_PTR:
    case AST_SLICE_TYPE:
        infer_pattern_ptr_slice(sema, b, pat, actual, site);
        return;
    case AST_ARRAY: {
        ast_array_t *a = (ast_array_t *)pat;
        const type_t *core = actual;
        if (!core || core->kind != TYPE_KIND_ARRAY) return;
        uint64_t alen = (uint64_t)array_type_len(actual);
        if (a->length && a->length->kind == AST_IDENT) {
            int li = infer_param_index(b->def,
                                       ((ast_ident_t *)a->length)->name);
            if (li >= 0) {
                ast_node_t *pn = b->def->generic.first;
                for (int k = 0; k < li && pn; k++) pn = pn->next;
                const ast_generic_param_t *gp = (const ast_generic_param_t *)pn;
                if (gp && gp->is_comptime)
                    infer_bind_value(sema, b, (size_t)li, alen, site);
            }
        } else if (a->length && a->length->kind == AST_INT_LIT) {
            if (((ast_int_lit_t *)a->length)->value != alen) return;
        }
        infer_pattern(sema, b, a->base_type, array_type_elem(actual), site);
        return;
    }
    case AST_TUPLE: {
        ast_tuple_t *t = (ast_tuple_t *)pat;
        const type_t *core = actual;
        if (!core || core->kind != TYPE_KIND_TUPLE) return;
        size_t np = sema_count_siblings(t->elem_types);
        if (np != tuple_type_elem_count(actual)) return;
        size_t i = 0;
        for (ast_node_t *e = t->elem_types; e && i < np; e = e->next, i++) {
            const tuple_elem_t *te = tuple_type_elem(actual, i);
            infer_pattern(sema, b, e, te ? te->type : NULL, site);
        }
        return;
    }
    case AST_FUNC_TYPE: {
        ast_func_type_t *f = (ast_func_type_t *)pat;
        const type_t *core = actual;
        if (!core || (core->kind != TYPE_KIND_FUNC &&
                      core->kind != TYPE_KIND_CFUNC))
            return;
        size_t np = sema_count_siblings(f->params);
        if (np != func_type_param_count(actual)) return;
        size_t i = 0;
        for (ast_node_t *p = f->params; p && i < np; p = p->next, i++)
            infer_pattern(sema, b, p, func_type_param(actual, i), site);
        infer_pattern(sema, b, f->return_type, func_type_return(actual), site);
        return;
    }
    case AST_INDEX: {
        /* 形参是泛型类型实例 `Box[T]`：实参必须已是同一泛型定义的实例。
           从实例表按 (def, type) 反查其类型实参，与 PATTERN 的实参逐位匹配。 */
        ast_index_t *ix = (ast_index_t *)pat;
        if (!ix->object || ix->object->kind != AST_IDENT) return;
        strslice_t gname = ((ast_ident_t *)ix->object)->name;
        sema_symbol_t *gs =
            sema->global_scope ? sema_scope_find_local(sema->global_scope, gname)
                               : NULL;
        if (!gs || !gs->is_generic) return;
        const type_t *core = type_strip_quals(actual);
        const sema_generic_instance_t *gi = NULL;
        for (sema_generic_instance_t *e = sema->generics.instances; e;
             e = e->next) {
            if (e->kind == SEMA_GEN_FUNC) continue;
            if (e->def != gs->ast || !e->type) continue;
            if (type_strip_quals(e->type) == core) {
                gi = e;
                break;
            }
        }
        if (!gi) return;
        size_t pi = 0;
        for (ast_node_t *a = ix->indices; a && pi < gi->arg_count;
             a = a->next, pi++) {
            const sema_generic_arg_t *ga = &gi->args[pi];
            if (!ga->is_value) {
                infer_pattern(sema, b, a, ga->type, site);
                continue;
            }
            uint64_t raw = ct_raw_u(&ga->value);
            if (a->kind == AST_IDENT) {
                int li = infer_param_index(b->def, ((ast_ident_t *)a)->name);
                if (li < 0) continue;
                ast_node_t *pn = b->def->generic.first;
                for (int k = 0; k < li && pn; k++) pn = pn->next;
                const ast_generic_param_t *gp = (const ast_generic_param_t *)pn;
                if (gp && gp->is_comptime)
                    infer_bind_value(sema, b, (size_t)li, raw, site);
            } else if (a->kind == AST_INT_LIT) {
                if (((ast_int_lit_t *)a)->value != raw) return;
            }
        }
        return;
    }
    default:
        (void)resolve_type_expr(sema, pat);
        return;
    }
}

static const type_t *instantiate_inferred_impl(
    sema_t *sema, sema_symbol_t *generic_sym,
    const sema_generic_arg_t *explicit_args, size_t explicit_count,
    value_t *const *arg_shadows, size_t argc, const ast_node_t *site,
    const sema_generic_instance_t **out, strslice_t name_prefix,
    sema_generic_instance_t *parent) {
    if (out) *out = NULL;
    if (!sema || !generic_sym || !generic_sym->ast ||
        generic_sym->ast->kind != AST_FUNC_DEF)
        return NULL;
    ast_func_def_t *def = (ast_func_def_t *)generic_sym->ast;
    size_t n = def->generic.count;
    if (n == 0) return NULL;
    ast_node_t *pack_gp = NULL;
    size_t fixed_n = func_pack_info(def, &pack_gp);
    bool has_pack = pack_gp != NULL;
    size_t nf = has_pack ? fixed_n : n; /* 固定参数个数（不含包） */
    if (!has_pack && explicit_count > nf) {
        diag_error(sema->diag,
                   site ? sema_loc(sema, (ast_node_t *)site)
                        : sema_loc(sema, (ast_node_t *)def),
                   "generic function '%.*s' expects %zu generic argument(s), "
                   "got %zu",
                   (int)def->name.len, def->name.ptr, nf, explicit_count);
        return NULL;
    }
    size_t fixed_vparams = fn_fixed_value_param_count(def);
    size_t extra_args = argc > fixed_vparams ? argc - fixed_vparams : 0;
    size_t exp_pack = explicit_count > nf ? explicit_count - nf : 0;
    size_t pack_n = exp_pack > extra_args ? exp_pack : extra_args;
    if (has_pack && exp_pack > 0 && exp_pack != extra_args) {
        diag_error(sema->diag,
                   site ? sema_loc(sema, (ast_node_t *)site)
                        : sema_loc(sema, (ast_node_t *)def),
                   "parameter pack '...%.*s' explicitly has %zu type "
                   "argument(s), but %zu variadic argument(s) are passed",
                   (int)((ast_generic_param_t *)pack_gp)->name.len,
                   ((ast_generic_param_t *)pack_gp)->name.ptr, exp_pack,
                   extra_args);
        return NULL;
    }

    const type_t *types_buf[SEMA_GENERIC_MAX_DEPTH];
    bool set_buf[SEMA_GENERIC_MAX_DEPTH];
    bool is_value_buf[SEMA_GENERIC_MAX_DEPTH];
    sema_ct_const_t values_buf[SEMA_GENERIC_MAX_DEPTH];
    sema_generic_arg_t pack_args[SEMA_GENERIC_MAX_DEPTH];
    if (nf > SEMA_GENERIC_MAX_DEPTH || pack_n > SEMA_GENERIC_MAX_DEPTH) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)def),
                   "too many generic parameters (%zu)", nf + pack_n);
        return NULL;
    }
    memset(pack_args, 0, sizeof pack_args);
    memset(types_buf, 0, sizeof types_buf);
    memset(set_buf, 0, sizeof set_buf);
    memset(is_value_buf, 0, sizeof is_value_buf);
    memset(values_buf, 0, sizeof values_buf);

    infer_t b;
    b.def = def;
    b.types = types_buf;
    b.set = set_buf;
    b.values = values_buf;
    b.is_value = is_value_buf;
    b.n = nf;
    b.failed = false;

    /* 显式实参先占位：前 nf 个是固定参数；其余是包元素。 */
    for (size_t i = 0; i < explicit_count && explicit_args; i++) {
        if (i < nf) {
            if (explicit_args[i].is_value) {
                if (explicit_args[i].type)
                    infer_bind_value(sema, &b, i,
                                     ct_raw_u(&explicit_args[i].value), site);
            } else if (explicit_args[i].type) {
                infer_bind(sema, &b, i, explicit_args[i].type, site);
            }
        } else {
            pack_args[i - nf] = explicit_args[i];
        }
    }

    /* 形参类型模式 × 实参类型（固定参数）。 */
    size_t pi = 0;
    for (ast_node_t *p = def->params; p && pi < fixed_vparams && pi < argc;
         p = p->next, pi++) {
        value_t *av = arg_shadows ? arg_shadows[pi] : NULL;
        if (!av || value_is_error(sema->vm, av) ||
            value_is_type(av, TYPE_KIND_VOID))
            continue;
        ast_var_def_t *vd = (ast_var_def_t *)p;
        infer_pattern(sema, &b, vd->type_expr, value_type(av), site);
    }
    if (b.failed) return NULL;

    /* 参数包元素：多余实参的类型即包元素类型。 */
    for (size_t k = 0; k < extra_args && k < pack_n; k++) {
        value_t *av = arg_shadows ? arg_shadows[fixed_vparams + k] : NULL;
        const type_t *at =
            (av && !value_is_error(sema->vm, av) &&
             !value_is_type(av, TYPE_KIND_VOID))
                ? value_type(av)
                : NULL;
        if (!at) return NULL;
        if (k < exp_pack) {
            if (pack_args[k].is_value) {
                diag_error(sema->diag, sema_loc(sema, (ast_node_t *)site),
                           "parameter pack only accepts type arguments in M7");
                return NULL;
            }
            if (pack_args[k].type &&
                !type_equal(sema->vm, pack_args[k].type, at)) {
                char a[64], c[64];
                sema_type_name(pack_args[k].type, a, sizeof a);
                sema_type_name(at, c, sizeof c);
                diag_error(sema->diag, sema_loc(sema, (ast_node_t *)site),
                           "parameter pack argument %zu: explicitly '%s', but "
                           "the actual argument has type '%s'",
                           k + 1, a, c);
                return NULL;
            }
        }
        pack_args[k].is_value = false;
        pack_args[k].type = at;
    }


    /* 默认值补齐（定义点求值；可引用更早的参数，见设计 §3）。
       类型参数求值为类型；comptime 值参数走 CTFE + 转换为声明类型。 */
    bool need_default = false;
    for (size_t i = 0; i < nf; i++)
        if (!set_buf[i]) need_default = true;
    if (need_default) {
        vm_push_scope(sema->vm);
        int idx = 0;
        for (ast_node_t *p = def->generic.first; p && idx < (int)nf;
             p = p->next, idx++) {
            if (!set_buf[idx]) continue;
            char nm[256];
            generic_name_to_cstr(((ast_generic_param_t *)p)->name, nm, sizeof nm);
            value_t *tv = is_value_buf[idx] ? ct_to_value(sema, &values_buf[idx])
                                            : type_as_value(sema->vm, types_buf[idx]);
            if (tv) scope_define(sema->vm, sema->vm->current_scope, nm, tv);
        }
        idx = 0;
        for (ast_node_t *p = def->generic.first; p && idx < (int)nf;
             p = p->next, idx++) {
            if (set_buf[idx]) continue;
            ast_generic_param_t *gp = (ast_generic_param_t *)p;
            if (!gp->default_expr) continue;
            char nm[256];
            generic_name_to_cstr(gp->name, nm, sizeof nm);
            if (gp->is_comptime) {
                const type_t *vt = gp->type_expr
                                       ? resolve_type_expr(sema, gp->type_expr)
                                       : NULL;
                ctfe_ctx_t ctx;
                memset(&ctx, 0, sizeof ctx);
                ctx.vm = sema->vm;
                ctx.sema = sema;
                ctx.budget = 100000;
                ctx.max_depth = 128;
                value_t *v = ctfe_eval(&ctx, gp->default_expr);
                sema_ct_const_t ct, converted;
                if (!v || value_is_error(sema->vm, v) || !vt ||
                    !sema_ct_encode(sema, v, &ct) ||
                    !ct_convert_scalar(&ct, vt, &converted))
                    continue;
                values_buf[idx] = converted;
                is_value_buf[idx] = true;
                set_buf[idx] = true;
                value_t *tv = ct_to_value(sema, &converted);
                if (tv)
                    scope_define(sema->vm, sema->vm->current_scope, nm, tv);
            } else {
                const type_t *dt = resolve_type_expr(sema, gp->default_expr);
                if (!dt) continue;
                types_buf[idx] = dt;
                set_buf[idx] = true;
                value_t *tv = type_as_value(sema->vm, dt);
                if (tv)
                    scope_define(sema->vm, sema->vm->current_scope, nm, tv);
            }
        }
        vm_pop_scope(sema->vm);
    }

    /* 仍未确定的固定参数：报推导失败（设计 §19）。 */
    for (size_t i = 0; i < nf; i++) {
        if (set_buf[i]) continue;
        ast_node_t *p = def->generic.first;
        for (size_t k = 0; k < i && p; k++) p = p->next;
        ast_generic_param_t *gp = (ast_generic_param_t *)p;
        diag_error(sema->diag,
                   site ? sema_loc(sema, (ast_node_t *)site)
                        : sema_loc(sema, (ast_node_t *)def),
                   "cannot deduce %s argument '%.*s' for generic function "
                   "'%.*s' (add explicit arguments, e.g. '%.*s[...]')",
                   (gp && gp->is_comptime) ? "value" : "type",
                   gp ? (int)gp->name.len : 0, gp ? gp->name.ptr : NULL,
                   (int)def->name.len, def->name.ptr, (int)def->name.len,
                   def->name.ptr);
        return NULL;
    }

    sema_generic_arg_t args[SEMA_GENERIC_MAX_DEPTH];
    memset(args, 0, sizeof args);
    for (size_t i = 0; i < nf; i++) {
        args[i].is_value = is_value_buf[i];
        if (is_value_buf[i]) {
            args[i].type = values_buf[i].type;
            args[i].value = values_buf[i];
        } else {
            args[i].type = types_buf[i];
        }
    }
    for (size_t k = 0; k < pack_n; k++) args[nf + k] = pack_args[k];
    return instantiate_func_impl(sema, generic_sym, args, nf + pack_n, site,
                                 out, name_prefix, parent);
}

const type_t *sema_generic_instantiate_inferred(
    sema_t *sema, sema_symbol_t *generic_sym,
    const sema_generic_arg_t *explicit_args, size_t explicit_count,
    value_t *const *arg_shadows, size_t argc, const ast_node_t *site,
    const sema_generic_instance_t **out) {
    return instantiate_inferred_impl(sema, generic_sym, explicit_args,
                                     explicit_count, arg_shadows, argc, site,
                                     out, generic_sym->generic_prefix, NULL);
}
