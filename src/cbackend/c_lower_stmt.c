#include "cbackend/c_lower_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void register_local(lower_ctx_t *lc, strslice_t name,
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

const type_t *lookup_local(lower_ctx_t *lc, strslice_t name) {
    for (lower_local_t *l = lc->locals; l; l = l->next)
        if (l->name.len == name.len &&
            memcmp(l->name.ptr, name.ptr, name.len) == 0)
            return l->type;
    /* 局部变量遮蔽全局变量：先查 locals，再查 globals。 */
    for (lower_local_t *g = lc->globals; g; g = g->next)
        if (g->name.len == name.len &&
            memcmp(g->name.ptr, name.ptr, name.len) == 0)
            return g->type;
    return NULL;
}

void register_global(lower_ctx_t *lc, strslice_t name, const type_t *type) {
    if (!name.ptr || !type) return;
    lower_local_t *l = (lower_local_t *)arena_calloc(
        lc->arena, 1, sizeof(*l), ALIGNOF(max_align_t));
    if (!l) return;
    l->name = name;
    l->type = type;
    l->next = lc->globals;
    lc->globals = l;
}

/* switch 的模式比较：`<tmp> == <pattern>`；str 用 clux_str_eq。
   模式按到达顺序惰性求值（|| 短路），与 VM 的 EQ/JNZ 链一致。
   注意：传入的 tmp 节点会被放进 call 的 args 链（append 会改写 next），
   因此每次比较都必须新建一个 ident 节点，不能复用同一个表达式节点。 */
static cir_expr_t *lower_switch_eq(lower_ctx_t *lc, const type_t *vt,
                                   const char *tmp_name, ast_node_t *pattern) {
    cir_expr_t *rhs = lower_expr(lc, pattern);
    if (!rhs) return NULL;
    cir_expr_t *tmp = cir_expr_ident(lc->arena, tmp_name);
    if (!tmp) return NULL;
    const type_t *pt = infer_expr_type(lc, pattern);
    bool is_str = (vt && type_kind_of(type_strip_quals(vt)) == TYPE_KIND_STR) ||
                  (pt && type_kind_of(type_strip_quals(pt)) == TYPE_KIND_STR);
    if (is_str) {
        lc->unit->uses_str_cmp = true;
        cir_expr_t *call = cir_expr_call(lc->arena, "clux_str_eq", NULL);
        if (!call) return NULL;
        cir_expr_append_arg(call, tmp);
        cir_expr_append_arg(call, rhs);
        return call;
    }
    return cir_expr_binary(lc->arena, "==", tmp, rhs);
}

/* `.?` / `.!`（AST_TRY）的语句化展开。
 *
 * sema 生成的 AST_TRY 结构：
 *   { var temp = <setup.init>;  (setup)
 *     ok_call ? value_call : <tail_stmt> }   // tail 是 return / panic
 *
 * 表达式形式在 C 里没有对应的可移植写法（需要语句 + 早退出），因此本函数
 * 产出前置语句链：`setup; if (!ok_call) { tail }`，成功值由调用方接续。
 */
cir_stmt_t *lower_try_prefix(lower_ctx_t *lc, ast_try_t *tr, bool *ok) {
    if (!tr) {
        *ok = false;
        return NULL;
    }
    cir_stmt_t *head = NULL, *tail = NULL;
    if (tr->setup) {
        cir_stmt_t *setup = lower_stmt(lc, tr->setup, ok);
        if (!*ok) return NULL;
        head = setup;
        for (cir_stmt_t *t = setup; t; t = t->next) tail = t;
    }
    cir_expr_t *ok_call = lower_expr(lc, tr->ok_call);
    if (!ok_call) {
        *ok = false;
        return NULL;
    }
    cir_stmt_t *ifs = cir_stmt_new(lc->arena, CIR_STMT_IF);
    if (!ifs) {
        *ok = false;
        return NULL;
    }
    ifs->expr = cir_expr_unary(lc->arena, "!", ok_call);
    if (tr->tail_stmt) {
        bool ok2 = true;
        ifs->body = lower_stmt(lc, tr->tail_stmt, &ok2);
        if (!ok2) {
            *ok = false;
            return NULL;
        }
    }
    if (tail) tail->next = ifs;
    else head = ifs;
    return head;
}

/* 由 TRY 的成功值构造赋值/return 时复用的 AST 节点。 */
static ast_node_t *try_value_return(arena_t *arena, ast_return_t *orig,
                                    ast_node_t *value) {
    ast_node_t *ret = ast_return_new(arena, orig->base.tok_begin,
                                     orig->base.tok_end);
    if (!ret) return NULL;
    ((ast_return_t *)ret)->value = value;
    ret->defers = orig->base.defers;
    return ret;
}

static void append_stmt_chain(cir_stmt_t **head, cir_stmt_t **tail,
                              cir_stmt_t *s) {
    if (!s) return;
    if (*tail) (*tail)->next = s;
    else *head = s;
    cir_stmt_t *t = s;
    while (t->next) t = t->next;
    *tail = t;
}

static cir_stmt_t *lower_stmt_core(lower_ctx_t *lc, ast_node_t *s, bool *ok);

/* 语句 lowering 入口：先 prelude、再语句主体、最后 postlude。
 * sema 把 `.?'/`.!' 之类的错误处理语法糖展开成挂在语句上的
 * prelude/postlude（临时变量 + if 分支），compiler 也是这么消费的。 */
cir_stmt_t *lower_stmt(lower_ctx_t *lc, ast_node_t *s, bool *ok) {
    if (!s || lc->failed) return NULL;
    bool noop = s->kind == AST_ASSIGN && ((ast_assign_t *)s)->sugar_noop;
    cir_stmt_t *head = NULL, *tail = NULL;
    if (s->prelude) {
        cir_stmt_t *pre = lower_ast_list(lc, s->prelude, ok);
        if (!*ok) return head;
        append_stmt_chain(&head, &tail, pre);
    }
    if (!noop) {
        cir_stmt_t *core = lower_stmt_core(lc, s, ok);
        if (!*ok) return head;
        append_stmt_chain(&head, &tail, core);
    }
    if (s->postlude) {
        cir_stmt_t *post = lower_ast_list(lc, s->postlude, ok);
        if (!*ok) return head;
        append_stmt_chain(&head, &tail, post);
    }
    return head;
}

static cir_stmt_t *lower_stmt_core(lower_ctx_t *lc, ast_node_t *s, bool *ok) {
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
        if (vd->init && vd->init->kind == AST_TRY) {
            /* `var v = expr.?;`：先声明 v，再把 try 展开成
               `{ setup; if (!ok) { tail } v = value; }`。 */
            ast_try_t *tr = (ast_try_t *)vd->init;
            cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
            if (!blk) return NULL;
            blk->body = lower_try_prefix(lc, tr, ok);
            if (!*ok) return NULL;
            ast_node_t *id = ast_ident_new(lc->arena, s->tok_begin, s->tok_end);
            ast_node_t *as = ast_assign_new(lc->arena, s->tok_begin, s->tok_end);
            if (!id || !as) {
                *ok = false;
                return NULL;
            }
            ((ast_ident_t *)id)->name = vd->name;
            ((ast_assign_t *)as)->target = id;
            ((ast_assign_t *)as)->value = tr->value_call;
            bool ok2 = true;
            cir_stmt_t *assign = lower_stmt(lc, as, &ok2);
            if (!ok2) {
                *ok = false;
                return NULL;
            }
            cir_stmt_t *t = blk->body;
            if (!t) blk->body = assign;
            else {
                while (t->next) t = t->next;
                t->next = assign;
            }
            st->next = blk;
            return st;
        }
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
        if (a->value && a->value->kind == AST_TRY) {
            /* `x = expr.?;`：赋值目标只求值一次——先取 try 前缀，再用
               成功值赋值。 */
            ast_try_t *tr = (ast_try_t *)a->value;
            cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
            if (!blk) return NULL;
            blk->body = lower_try_prefix(lc, tr, ok);
            if (!*ok) return NULL;
            ast_node_t *as =
                ast_assign_new(lc->arena, s->tok_begin, s->tok_end);
            if (!as) {
                *ok = false;
                return NULL;
            }
            ((ast_assign_t *)as)->target = a->target;
            ((ast_assign_t *)as)->value = tr->value_call;
            ((ast_assign_t *)as)->op = NULL;
            bool ok2 = true;
            cir_stmt_t *assign = lower_stmt(lc, as, &ok2);
            if (!ok2) {
                *ok = false;
                return NULL;
            }
            cir_stmt_t *t = blk->body;
            if (!t) blk->body = assign;
            else {
                while (t->next) t = t->next;
                t->next = assign;
            }
            return blk;
        }
        const char *op = a->op ? token_text(lc->arena, a->op) : "=";
        if (a->target && a->target->kind == AST_MEMBER &&
            ((ast_member_t *)a->target)->is_bitfield) {
            /* bitfield 字段不是 C 左值：按位读改写（CIR_EXPR_BITFIELD_SET）。 */
            cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_EXPR);
            if (!st) return NULL;
            st->expr = lower_bitfield_assign(
                lc, (ast_member_t *)a->target, op, a->value);
            if (!st->expr) *ok = false;
            return st;
        }
        cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_ASSIGN);
        if (!st) return NULL;
        st->lhs = lower_expr(lc, a->target);
        st->op = op;
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
        if (r->value && r->value->kind == AST_TRY) {
            /* `return expr.?;`：try 前缀 + 用成功值走普通 return 路径
               （option 包装 / defer / 清理照常生效）。 */
            ast_try_t *tr = (ast_try_t *)r->value;
            cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
            if (!blk) return NULL;
            blk->body = lower_try_prefix(lc, tr, ok);
            if (!*ok) return NULL;
            ast_node_t *ret2 = try_value_return(lc->arena, r, tr->value_call);
            if (!ret2) {
                *ok = false;
                return NULL;
            }
            bool ok2 = true;
            cir_stmt_t *ret_stmt = lower_stmt(lc, ret2, &ok2);
            if (!ok2) {
                *ok = false;
                return NULL;
            }
            cir_stmt_t *t = blk->body;
            if (!t) blk->body = ret_stmt;
            else {
                while (t->next) t = t->next;
                t->next = ret_stmt;
            }
            return blk;
        }
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
    case AST_SWITCH: {
        /* switch 是 if 语法糖：先求值一次 cond 存临时变量，再展开成
           `if (t == p1 || t == p2) {...} else if (...) {...} else {default}`。
           无 fallthrough；不引入新 IR 节点。 */
        ast_switch_t *sw = (ast_switch_t *)s;
        const type_t *vt = infer_expr_type(lc, sw->cond);
        cir_type_t *cvt = map_type(lc, vt);
        if (!vt || !cvt) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                       "c backend: cannot determine switch condition type");
            *ok = false;
            return NULL;
        }
        cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
        cir_stmt_t *decl = cir_stmt_new(lc->arena, CIR_STMT_VAR);
        if (!blk || !decl) return NULL;
        char tmp[64];
        snprintf(tmp, sizeof tmp, "clux_switch_%u", lc->switch_seq++);
        decl->type = cvt;
        decl->name = copy_arena(lc->arena, tmp);
        decl->expr = lower_expr(lc, sw->cond);
        if (!decl->expr) {
            *ok = false;
            return NULL;
        }
        blk->body = decl;
        cir_stmt_t *tail = decl;

        cir_stmt_t *chain_head = NULL, *chain_prev = NULL;
        for (ast_node_t *cs = sw->cases; cs; cs = cs->next) {
            if (cs->kind != AST_SWITCH_CASE) continue;
            ast_switch_case_t *sc = (ast_switch_case_t *)cs;
            cir_expr_t *cond = NULL;
            for (ast_node_t *pt = sc->patterns; pt; pt = pt->next) {
                cir_expr_t *eq = lower_switch_eq(lc, vt, decl->name, pt);
                if (!eq) {
                    *ok = false;
                    return NULL;
                }
                cond = cond ? cir_expr_binary(lc->arena, "||", cond, eq) : eq;
            }
            if (!cond) continue;
            cir_stmt_t *ifs = cir_stmt_new(lc->arena, CIR_STMT_IF);
            if (!ifs) return NULL;
            ifs->expr = cond;
            ifs->body = lower_block(lc, sc->body, ok);
            if (!*ok) return NULL;
            if (chain_prev) chain_prev->else_body = ifs;
            else chain_head = ifs;
            chain_prev = ifs;
        }
        if (sw->default_body) {
            cir_stmt_t *def = lower_block(lc, sw->default_body, ok);
            if (!*ok) return NULL;
            if (chain_prev) chain_prev->else_body = def;
            else if (def) {
                /* 没有 case：default 体直接执行。 */
                tail->next = def;
                return blk;
            }
        }
        if (chain_head) tail->next = chain_head;
        return blk;
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
            else if (ot && type_kind_of(ot) == TYPE_KIND_FUNC)
                ex = cir_expr_closure_release(lc->arena, ex);
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

cir_stmt_t *lower_ast_list(lower_ctx_t *lc, ast_node_t *list, bool *ok) {
    cir_stmt_t *head = NULL, *tail = NULL;
    for (ast_node_t *s = list; s; s = s->next) {
        cir_stmt_t *st = lower_stmt(lc, s, ok);
        if (!*ok) return head;
        if (!st) continue;
        /* st 可能是一条语句链（prelude / 主体 / postlude）：尾指针要走到链尾，
           否则下一条语句会覆盖链中后续节点。 */
        cir_stmt_t *last = st;
        while (last->next) last = last->next;
        if (tail) tail->next = st;
        else head = st;
        tail = last;
    }
    return head;
}

cir_stmt_t *lower_block(lower_ctx_t *lc, ast_node_t *block, bool *ok) {    if (!block || block->kind != AST_BLOCK) {
        *ok = false;
        return NULL;
    }
    cir_stmt_t *head = NULL, *tail = NULL;
    for (ast_node_t *s = ((ast_block_t *)block)->stmts; s; s = s->next) {
        cir_stmt_t *st = lower_stmt(lc, s, ok);
        if (!*ok) return head;
        if (!st) continue;
        cir_stmt_t *last = st;
        while (last->next) last = last->next;
        if (tail) tail->next = st;
        else head = st;
        tail = last;
    }
    return head;
}
