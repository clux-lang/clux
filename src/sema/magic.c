#include "sema/magic.h"

#include "parser/ast_call.h"
#include "parser/ast_member.h"
#include "parser/ast_program.h"
#include "parser/ast_struct_def.h"
#include "parser/lexer.h"
#include "sema/struct_compose.h"
#include "parser/ast_func_def.h"
#include "parser/ast_var_def.h"
#include "vm/type_func.h"
#include "vm/type_ptr.h"
#include "vm/type_struct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 魔法表（顺序无关；`nparams` 含 self）。返回类型约束见表项注释；
   `rhs_ptr`：二元运算符的右操作数按指针传递（零拷贝，调用点生成 `b.&`）。 */
static const sema_magic_entry_t k_magic[] = {
    {"__add__", SEMA_MAGIC_ADD, 2, false, false, false, true, false, false},
    {"__sub__", SEMA_MAGIC_SUB, 2, false, false, false, true, false, false},
    {"__mul__", SEMA_MAGIC_MUL, 2, false, false, false, true, false, false},
    {"__div__", SEMA_MAGIC_DIV, 2, false, false, false, true, false, false},
    {"__mod__", SEMA_MAGIC_MOD, 2, false, false, false, true, false, false},
    {"__and__", SEMA_MAGIC_AND, 2, false, false, false, true, false, false},
    {"__or__", SEMA_MAGIC_OR, 2, false, false, false, true, false, false},
    {"__xor__", SEMA_MAGIC_XOR, 2, false, false, false, true, false, false},
    {"__shl__", SEMA_MAGIC_SHL, 2, false, false, false, true, false, false},
    {"__shr__", SEMA_MAGIC_SHR, 2, false, false, false, true, false, false},
    {"__neg__", SEMA_MAGIC_NEG, 1, false, false, false, false, false, false},
    {"__not__", SEMA_MAGIC_NOT, 1, true, false, false, false, false, false},
    {"__bnot__", SEMA_MAGIC_BNOT, 1, false, false, false, false, false, false},
    {"__eq__", SEMA_MAGIC_EQ, 2, true, false, false, true, false, false},
    {"__lt__", SEMA_MAGIC_LT, 2, true, false, false, true, false, false},
    {"__le__", SEMA_MAGIC_LE, 2, true, false, false, true, false, false},
    {"__gt__", SEMA_MAGIC_GT, 2, true, false, false, true, false, false},
    {"__ge__", SEMA_MAGIC_GE, 2, true, false, false, true, false, false},
    {"__index__", SEMA_MAGIC_INDEX, 2, false, false, true, false, false, false},
    {"__len__", SEMA_MAGIC_LEN, 1, false, true, false, false, false, false},
    {"__deref__", SEMA_MAGIC_DEREF, 1, false, false, true, false, false, false},
    {"__dispose__", SEMA_MAGIC_DISPOSE, 1, false, false, false, false, true, false},
    {"__assign__", SEMA_MAGIC_ASSIGN, 2, false, false, false, true, true, false},
    {"__init__", SEMA_MAGIC_INIT, 0, false, false, false, false, true, true},
};

#define MAGIC_COUNT (sizeof(k_magic) / sizeof(k_magic[0]))

/* CLI 覆盖（进程级；-1 = 未设置）。sema_create 时经 resolve() 取走。 */
static int g_deref_depth_override = -1;

void sema_magic_set_deref_depth_override(int depth) {
    g_deref_depth_override = depth >= 0 ? depth : -1;
}

uint32_t sema_magic_deref_depth_resolve(void) {
    if (g_deref_depth_override >= 0 &&
        g_deref_depth_override < 32)
        return (uint32_t)g_deref_depth_override;
    const char *env = getenv("CLUX_MAGIC_DEREF_DEPTH");
    if (env && *env) {
        long v = strtol(env, NULL, 10);
        if (v >= 0 && v < 32) return (uint32_t)v;
    }
    return SEMA_MAGIC_DEREF_MAX_DEPTH_DEFAULT;
}

const sema_magic_entry_t *sema_magic_by_name(strslice_t name) {
    for (size_t i = 0; i < MAGIC_COUNT; i++) {
        size_t n = strlen(k_magic[i].name);
        if (name.len == n && name.ptr && memcmp(name.ptr, k_magic[i].name, n) == 0)
            return &k_magic[i];
    }
    return NULL;
}

const char *sema_magic_name(sema_magic_t id) {
    for (size_t i = 0; i < MAGIC_COUNT; i++)
        if (k_magic[i].id == id) return k_magic[i].name;
    return NULL;
}

bool sema_magic_is_reserved_name(strslice_t name) {
    return name.ptr && name.len >= 2 && name.ptr[0] == '_' && name.ptr[1] == '_';
}

const struct type_t *sema_magic_receiver_target(const struct type_t *self_type) {
    if (!self_type) return NULL;
    const type_t *t = type_strip_quals(self_type);
    if (!t || !type_is_ptr(t)) return NULL;
    return type_strip_quals(ptr_type_base(t));
}

bool sema_magic_is_derefable(sema_t *sema, const type_t *t) {
    if (!sema || !t) return false;
    const type_t *c = type_strip_quals(t);
    if (!c) return false;
    if (type_is_ptr(c)) return true;
    if (type_kind_of(c) != TYPE_KIND_STRUCT) return false;
    return sema_magic_member(sema, c, "__deref__") != NULL;
}

sema_symbol_t *sema_magic_member(sema_t *sema, const type_t *t,
                                 const char *name) {
    if (!sema || !t || !name) return NULL;
    const type_t *c = type_strip_quals(t);
    if (!c || type_kind_of(c) != TYPE_KIND_STRUCT) return NULL;
    return sema_struct_find_member(sema, c, strslice_from_cstr(name), NULL, NULL);
}

bool sema_magic_check_method(sema_t *sema, const type_t *owner, ast_node_t *def,
                             const type_t *const *params, size_t nparams,
                             const type_t *ret) {
    if (!sema || !owner || !def) return true;
    ast_func_def_t *fn = (ast_func_def_t *)def;
    (void)params;
    (void)nparams;
    (void)ret;

    /* 接收者形态不在这里判断：实例调用时 `a.m(...)` 传的是 `a.&`（见
       expr_call.c 的接收者可赋值性校验），`self` 若是值形参会由普通隐式转换
       规则自然拒绝。这里只校验魔法方法自身的契约。 */
    if (!sema_magic_is_reserved_name(fn->name)) return true;

    const sema_magic_entry_t *me = sema_magic_by_name(fn->name);
    if (!me) {
        diag_error(sema->diag, sema_loc(sema, def),
                   "unknown magic method '%.*s'", (int)fn->name.len,
                   fn->name.ptr);
        return true;
    }

    /* 魔法方法可以是泛型：`func __eq__[U](self: *const T, rhs: *const U)`
       —— 派发点会像普通泛型方法调用一样按实参推断并实例化（clux 没有
       重载，泛型 + comptime if 就是重载的表达方式）。泛型声明在这里不做
       具体签名的校验（形参还是类型参数），实例化后的调用由普通调用规则
       检查。 */

    /* 接收者形态不在这里判断：方法就是类型命名空间里的函数，`a.m()` 是
       `typeof(a)::m(a.&)` 的语法糖（见 docs-backup/magic-methods-design.md）。
       `self` 若是值形参，调用点把 `a.&`（*T）传给值形参时会由普通隐式转换
       规则自然拒绝；`self` 指向基类（嵌入继承转发）也是合法的。这里只校验
       魔法方法自身的契约（名字/参数个数/返回类型）。 */
    if (nparams == 0 || !params || !params[0]) {
        diag_error(sema->diag, sema_loc(sema, def),
                   "magic method '%.*s' must take a receiver argument",
                   (int)fn->name.len, fn->name.ptr);
        return true;
    }

    if (!me->any_arity && nparams != me->nparams) {
        if (me->nparams == 1)
            diag_error(sema->diag, sema_loc(sema, def),
                       "magic method '%s' takes no arguments besides self",
                       me->name);
        else
            diag_error(sema->diag, sema_loc(sema, def),
                       "magic method '%s' takes exactly one argument besides "
                       "self",
                       me->name);
        return true;
    }

    /* 二元运算符的右操作数按指针传（零拷贝）：`a op b` ⇒ `<op>(a.&, b.&)`。 */
    if (me->rhs_ptr) {
        const type_t *rt = params[1];
        if (!rt || !type_is_ptr(type_strip_quals(rt))) {
            char tn[64];
            sema_type_name(rt, tn, sizeof tn);
            diag_error(sema->diag, sema_loc(sema, def),
                       "magic method '%s' must take its right-hand operand by "
                       "pointer ('*const T'), got '%s'",
                       me->name, tn);
            return true;
        }
    }

    if (me->ret_bool && (!ret || type_kind_of(ret) != TYPE_KIND_BOOL)) {
        char tn[64];
        sema_type_name(ret, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, def),
                   "magic method '%s' must return bool (got '%s')", me->name,
                   tn);
        return true;
    }
    if (me->ret_usize &&
        (!ret ||
         ret != (sema->vm ? (const type_t *)vm_type_usize(sema->vm) : NULL))) {
        char tn[64];
        sema_type_name(ret, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, def),
                   "magic method '%s' must return usize (got '%s')", me->name,
                   tn);
        return true;
    }
    if (me->ret_deref && ret && !sema_magic_is_derefable(sema, ret)) {
        char tn[64];
        sema_type_name(ret, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, def),
                   "magic method '%s' must return a pointer or a type with "
                   "'__deref__' (got '%s')",
                   me->name, tn);
        return true;
    }
    if (me->ret_void && ret && type_kind_of(ret) != TYPE_KIND_VOID) {
        char tn[64];
        sema_type_name(ret, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, def),
                   "magic method '%s' must return void (got '%s')", me->name,
                   tn);
        return true;
    }
    return true;
}

ast_node_t *sema_magic_make_call(sema_t *sema, ast_node_t *recv,
                                 const char *name, ast_node_t *args,
                                 uint32_t tok_begin, uint32_t tok_end) {
    if (!sema || !recv || !name) return NULL;
    ast_member_t *m = (ast_member_t *)ast_member_new(sema->arena, tok_begin,
                                                     tok_end);
    ast_node_t *call = ast_call_new(sema->arena, tok_begin, tok_end);
    if (!m || !call) return NULL;
    m->object = recv;
    m->field = strslice_from_cstr(name);
    m->base.next = NULL;
    ast_call_t *c = (ast_call_t *)call;
    c->callee = &m->base;
    c->args = args;
    ast_node_t *last = args;
    while (last && last->next) last = last->next;
    c->args_last = last;
    call->is_generated = true;
    call->next = NULL;
    return call;
}

const char *sema_magic_binop_name(const token_t *op) {
    if (!op) return NULL;
    if (token_is(op, "+")) return "__add__";
    if (token_is(op, "-")) return "__sub__";
    if (token_is(op, "*")) return "__mul__";
    if (token_is(op, "/")) return "__div__";
    if (token_is(op, "%")) return "__mod__";
    if (token_is(op, "&")) return "__and__";
    if (token_is(op, "|")) return "__or__";
    if (token_is(op, "^")) return "__xor__";
    if (token_is(op, "<<")) return "__shl__";
    if (token_is(op, ">>")) return "__shr__";
    if (token_is(op, "==")) return "__eq__";
    if (token_is(op, "!=")) return "__eq__";
    if (token_is(op, "<")) return "__lt__";
    if (token_is(op, "<=")) return "__le__";
    if (token_is(op, ">")) return "__gt__";
    if (token_is(op, ">=")) return "__ge__";
    return NULL;
}

const char *sema_magic_unop_name(const token_t *op) {
    if (!op) return NULL;
    if (token_is(op, "-")) return "__neg__";
    if (token_is(op, "!")) return "__not__";
    if (token_is(op, "~")) return "__bnot__";
    return NULL;
}

/* 沿 `__deref__` 链展开接收者，直到 member 存在。
 *
 * 关键点：环/深度检测用**局部** visited 栈——之前用 sema 级全局栈时，
 * `w.x = w.x + 2` 里目标展开还没退出就求值右值，右值的 `w.x` 被误判成环
 * （W -> W）。现在每次回退独立走自己的链，互不影响。 */
int sema_magic_deref_rewrite(sema_t *sema, ast_node_t **out_expr,
                             const type_t *recv_t, ast_node_t *recv_expr,
                             const char *member, uint32_t tok_begin,
                             uint32_t tok_end) {
    if (!sema || !out_expr || !recv_expr || !member) return 0;
    uint32_t limit = sema->magic_deref_max_depth;
    if (limit == 0) {
        diag_error(sema->diag, sema_loc(sema, recv_expr),
                   "'__deref__' expansion is disabled "
                   "(CLUX_MAGIC_DEREF_DEPTH=0)");
        return -1;
    }
    const type_t *visited[32];
    uint32_t nv = 0;
    const type_t *t = recv_t;
    ast_node_t *expr = recv_expr;
    strslice_t mname = strslice_from_cstr(member);

    for (uint32_t step = 0; step < limit; step++) {
        const type_t *core = t ? type_strip_quals(t) : NULL;
        const type_t *st = core;
        if (st && type_is_ptr(st)) st = type_strip_quals(ptr_type_base(st));
        if (!st || type_kind_of(st) != TYPE_KIND_STRUCT) return 0;
        /* 成员已存在（字段或方法）→ 展开完成。 */
        if (struct_type_find_field(st, mname) >= 0) {
            *out_expr = expr;
            return 1;
        }
        if (sema_struct_find_member(sema, st, mname, NULL, NULL)) {
            *out_expr = expr;
            return 1;
        }
        sema_symbol_t *dm = sema_magic_member(sema, st, "__deref__");
        if (!dm || !dm->type || type_kind_of(dm->type) != TYPE_KIND_FUNC)
            return 0;
        const type_t *R = func_type_return(dm->type);
        if (!R) return 0;
        for (uint32_t i = 0; i < nv; i++) {
            if (visited[i] == st) {
                char chain[256];
                size_t off = 0;
                for (uint32_t k = 0; k < nv && off < sizeof chain; k++) {
                    int n = snprintf(chain + off, sizeof chain - off, "%.*s -> ",
                                     (int)(visited[k]->name.ptr
                                               ? visited[k]->name.len : 0),
                                     visited[k]->name.ptr ? visited[k]->name.ptr
                                                          : "?");
                    if (n > 0) off += (size_t)n;
                }
                if (off < sizeof chain)
                    snprintf(chain + off, sizeof chain - off, "%.*s",
                             (int)st->name.len, st->name.ptr ? st->name.ptr : "?");
                diag_error(sema->diag, sema_loc(sema, recv_expr),
                           "'__deref__' cycle: %s", chain);
                return -1;
            }
        }
        if (nv < 32) visited[nv++] = st;
        expr = sema_magic_make_call(sema, expr, "__deref__", NULL, tok_begin,
                                    tok_end);
        if (!expr) return -1;
        t = R;
    }
    diag_error(sema->diag, sema_loc(sema, recv_expr),
               "'__deref__' expansion exceeded the configured depth (%u)", limit);
    return -1;
}

int sema_magic_deref_star_rewrite(sema_t *sema, ast_node_t **out_expr,
                                  const type_t *recv_t, ast_node_t *recv_expr,
                                  uint32_t tok_begin, uint32_t tok_end) {
    if (!sema || !out_expr || !recv_expr) return 0;
    uint32_t limit = sema->magic_deref_max_depth;
    if (limit == 0) {
        diag_error(sema->diag, sema_loc(sema, recv_expr),
                   "'__deref__' expansion is disabled "
                   "(CLUX_MAGIC_DEREF_DEPTH=0)");
        return -1;
    }
    const type_t *visited[32];
    uint32_t nv = 0;
    const type_t *t = recv_t;
    ast_node_t *expr = recv_expr;

    for (uint32_t step = 0; step < limit; step++) {
        const type_t *core = t ? type_strip_quals(t) : NULL;
        /* 已经是指针：`.*` 直接按内建解引用。 */
        if (core && type_is_ptr(core)) {
            *out_expr = expr;
            return 1;
        }
        if (!core || type_kind_of(core) != TYPE_KIND_STRUCT) return 0;
        sema_symbol_t *dm = sema_magic_member(sema, core, "__deref__");
        if (!dm || !dm->type || type_kind_of(dm->type) != TYPE_KIND_FUNC)
            return 0;
        const type_t *R = func_type_return(dm->type);
        if (!R) return 0;
        for (uint32_t i = 0; i < nv; i++) {
            if (visited[i] == core) {
                char chain[256];
                size_t off = 0;
                for (uint32_t k = 0; k < nv && off < sizeof chain; k++) {
                    int n = snprintf(chain + off, sizeof chain - off, "%.*s -> ",
                                     (int)(visited[k]->name.ptr
                                               ? visited[k]->name.len : 0),
                                     visited[k]->name.ptr ? visited[k]->name.ptr
                                                          : "?");
                    if (n > 0) off += (size_t)n;
                }
                if (off < sizeof chain)
                    snprintf(chain + off, sizeof chain - off, "%.*s",
                             (int)core->name.len,
                             core->name.ptr ? core->name.ptr : "?");
                diag_error(sema->diag, sema_loc(sema, recv_expr),
                           "'__deref__' cycle: %s", chain);
                return -1;
            }
        }
        if (nv < 32) visited[nv++] = core;
        expr = sema_magic_make_call(sema, expr, "__deref__", NULL, tok_begin,
                                    tok_end);
        if (!expr) return -1;
        t = R;
    }
    diag_error(sema->diag, sema_loc(sema, recv_expr),
               "'__deref__' expansion exceeded the configured depth (%u)", limit);
    return -1;
}

void sema_validate_methods(sema_t *sema, ast_program_t *prog) {
    if (!sema) return;

    /* 1) 泛型方法不在 sema->funcs 里（调用点才实例化），单独按声明校验：
       魔法方法**允许泛型**（泛型 + comptime if 承担重载表达），这里只
       校验名字——未知的魔法名依旧拒绝。 */
    if (prog) {
        for (ast_node_t *d = prog->funcs; d; d = d->next) {
            if (d->kind != AST_STRUCT_DEF) continue;
            ast_struct_def_t *sd = (ast_struct_def_t *)d;
            for (ast_node_t *m = sd->members; m; m = m->next) {
                if (m->kind != AST_FUNC_DEF) continue;
                ast_func_def_t *fn = (ast_func_def_t *)m;
                if (fn->generic.count > 0 &&
                    sema_magic_is_reserved_name(fn->name) &&
                    !sema_magic_by_name(fn->name)) {
                    diag_error(sema->diag, sema_loc(sema, m),
                               "unknown magic method '%.*s'",
                               (int)fn->name.len, fn->name.ptr);
                }
            }
        }
    }

    /* 2) 已解析签名的非泛型方法：完整校验（魔法契约）。 */
    size_t n = vec_len(sema->funcs);
    for (size_t i = 0; i < n; i++) {
        sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
        if (!sf || !sf->owner || !sf->def) continue;
        if (sf->def->kind != AST_FUNC_DEF) continue;
        ast_func_def_t *fn = (ast_func_def_t *)sf->def;
        /* 泛型方法的实例在调用点生成，校验放在实例化处（这里跳过定义本身）。 */
        if (fn->generic.count > 0) continue;

        size_t np = sema_count_siblings(fn->params);
        const type_t **params = NULL;
        if (np > 0) {
            params = (const type_t **)allocator_new_ex(
                sema->vm->alloc, "type_t*", sizeof(type_t *), NULL, NULL, NULL,
                np);
        }
        size_t j = 0;
        bool ok = true;
        for (ast_node_t *p = fn->params; p; p = p->next, j++) {
            ast_var_def_t *vd = (ast_var_def_t *)p;
            const type_t *t = sema_resolve_type_slot(sema, &vd->type_expr);
            if (!t) ok = false;
            if (params) params[j] = t;
        }
        const type_t *ret = NULL;
        if (fn->return_expr)
            ret = sema_resolve_type_slot(sema, &fn->return_expr);
        if (ok) sema_magic_check_method(sema, sf->owner, sf->def, params, np, ret);
        if (params) allocator_free(sema->vm->alloc, (void **)&params);
    }
}
