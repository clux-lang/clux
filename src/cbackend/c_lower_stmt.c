#include "cbackend/c_lower_internal.h"
#include "vm/type_func.h"

#include "parser/ast_type_ref.h"

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

static lower_defer_t *find_defer_entry(lower_ctx_t *lc, strslice_t name) {
    for (lower_defer_t *d = lc->defers; d; d = d->next)
        if (d->name.len == name.len &&
            memcmp(d->name.ptr, name.ptr, name.len) == 0)
            return d;
    return NULL;
}

lower_local_fn_t *find_local_fn_fid(lower_ctx_t *lc, uint32_t fid) {
    for (lower_local_fn_t *f = lc->local_fns; f; f = f->next)
        if (f->fid && f->fid == fid) return f;
    return NULL;
}

lower_local_fn_t *find_local_fn(lower_ctx_t *lc, strslice_t name) {
    for (lower_local_fn_t *f = lc->local_fns; f; f = f->next)
        if (f->name.len == name.len &&
            memcmp(f->name.ptr, name.ptr, name.len) == 0)
            return f;
    return NULL;
}

void register_local_as(lower_ctx_t *lc, strslice_t name,
                        const type_t *type, const char *cname) {
    if (!name.ptr || !type || !cname) return;
    lower_local_t *l = (lower_local_t *)arena_calloc(
        lc->arena, 1, sizeof(*l), ALIGNOF(max_align_t));
    if (!l) return;
    l->name = name;
    l->type = type;
    l->cname = cname;
    l->next = lc->locals;
    lc->locals = l;
    lower_local_t *d = (lower_local_t *)arena_calloc(
        lc->arena, 1, sizeof(*d), ALIGNOF(max_align_t));
    if (d) {
        d->name = name;
        d->cname = cname;
        d->type = type;
        d->next = lc->local_decls;
        lc->local_decls = d;
    }
}

void register_local(lower_ctx_t *lc, strslice_t name,
                           const type_t *type) {
    if (!name.ptr || !type) return;
    lower_local_t *l = (lower_local_t *)arena_calloc(
        lc->arena, 1, sizeof(*l), ALIGNOF(max_align_t));
    if (!l) return;
    l->name = name;
    l->type = type;
    /* 同一函数内重复声明同名局部（comptime 展开/循环展开会产生）在 C 里
       不能重定义：按出现顺序加 `_2`、`_3` 后缀。读取走同一张表，因此
       后续引用解析到当前声明。 */
    const char *base = module_symbol(lc->arena, lc->unit, "v_", name);
    if (!base) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", base);
    unsigned seq = 2;
    bool clash = true;
    while (clash) {
        clash = false;
        for (lower_local_t *o = lc->local_decls; o; o = o->next) {
            if (o->cname && strcmp(o->cname, buf) == 0) {
                clash = true;
                break;
            }
        }
        if (clash) {
            snprintf(buf, sizeof buf, "%s_%u", base, seq++);
        }
    }
    l->cname = copy_arena(lc->arena, buf);
    l->next = lc->locals;
    lc->locals = l;
    /* 记入函数级声明全集（不随块弹出）：块拍平后仍保证 C 名字唯一。 */
    lower_local_t *d = (lower_local_t *)arena_calloc(
        lc->arena, 1, sizeof(*d), ALIGNOF(max_align_t));
    if (d) {
        d->name = name;
        d->cname = l->cname;
        d->type = type;
        d->next = lc->local_decls;
        lc->local_decls = d;
    }
}

const char *lookup_local_cname(lower_ctx_t *lc, strslice_t name) {
    for (lower_local_t *l = lc->locals; l; l = l->next)
        if (l->name.len == name.len &&
            memcmp(l->name.ptr, name.ptr, name.len) == 0)
            return l->cname;
    return NULL;
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
/* 闭包值 `ex` 的销毁（内联 env）：按捕获槽位声明序递归释放 own 资源。
 * 嵌套闭包捕获（`func |f: own Inner|`）按同样规则下钻——不能用 clux_free
 * 释放一个闭包结构体值。 */
static bool emit_closure_free(lower_ctx_t *lc, cir_expr_t *ex,
                              const type_t *ft, cir_stmt_t **head,
                              cir_stmt_t **tail) {
    size_t ncap = func_type_capture_count(ft);
    for (size_t ci = 0; ci < ncap; ci++) {
        const type_t *capt = func_type_capture_type(ft, ci);
        if (!capt) continue;
        cir_expr_t *envm = cir_expr_member(lc->arena, ex, "env");
        char fb[64];
        snprintf(fb, sizeof fb, "clux_f_%zu", ci);
        cir_expr_t *field =
            envm ? cir_expr_member(lc->arena, envm,
                                   copy_arena(lc->arena, fb))
                 : NULL;
        if (!field) return false;
        const type_t *cs = type_strip_quals(capt);
        if (cs && type_kind_of(cs) == TYPE_KIND_FUNC) {
            ownership_t oq = type_ownership_of(capt);
            if (oq == OWN_OWN && func_type_is_closure(cs)) {
                if (!emit_closure_free(lc, field, cs, head, tail)) return false;
            }
            continue; /* 普通函数值 / 借用闭包：无 env 资源 */
        }
        ownership_t oq = type_ownership_of(capt);
        if (cs && (type_kind_of(cs) == TYPE_KIND_PTR ||
                   type_kind_of(cs) == TYPE_KIND_SLICE)) {
            if (oq != OWN_OWN && oq != OWN_FATAL) continue;
            cir_expr_t *target = field;
            if (type_kind_of(cs) == TYPE_KIND_SLICE)
                target = cir_expr_slice_data(lc->arena, field);
            cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_FREE);
            if (!st) return false;
            lc->unit->uses_alloc = true;
            st->expr = target;
            if (*tail) (*tail)->next = st;
            else *head = st;
            *tail = st;
            continue;
        }
        /* 含 own 槽位的聚合捕获：按类型生成的 `__destroy_<T>(ref *T)` 递归
           销毁（不能对结构体值 clux_free）。 */
        if (cs && type_has_own_slots(cs)) {
            uint32_t dfid = 0;
            strslice_t dn = STRSLICE_EMPTY;
            if (!sema_find_destroy_fn(lc->sema, cs, &dfid, &dn)) continue;
            const char *cname = module_symbol(lc->arena, lc->unit, "fn_", dn);
            if (!cname) continue;
            cir_expr_t *addr = cir_expr_unary(lc->arena, "&", field);
            cir_expr_t *call = cir_expr_call(lc->arena, cname, NULL);
            if (!addr || !call) return false;
            /* 生成函数的 C 形态是 `void f(void *env, T *dv)`。 */
            cir_expr_append_arg(call, cir_expr_ident(lc->arena, "NULL"));
            cir_expr_append_arg(call, addr);
            cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_EXPR);
            if (!st) return false;
            st->expr = call;
            if (*tail) (*tail)->next = st;
            else *head = st;
            *tail = st;
            continue;
        }
        if (oq != OWN_OWN && oq != OWN_FATAL) continue;
    }
    return true;
}

/* defer 捕获结构体是栈上局部；defer 体执行完后必须按槽位释放 own 捕获
   （否则 `defer |(q: own *i32 = move(p))| {}` 泄漏）。 */
static void defer_env_free(lower_ctx_t *lc, lower_defer_t *de,
                           ast_func_def_t *closure, cir_stmt_t **head,
                           cir_stmt_t **tail) {
    if (!de || !de->env_name || !closure) return;
    for (ast_node_t *c = closure->captures; c; c = c->next) {
        ast_var_def_t *vd = (ast_var_def_t *)c;
        const type_t *ct = vd->resolved_type;
        if (!ct) ct = lookup_local(lc, vd->name);
        if (!ct && vd->type_expr)
            ct = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        if (!ct) continue;
        const type_t *cs = type_strip_quals(ct);
        ownership_t oq = type_ownership_of(ct);
        char fb[256];
        snprintf(fb, sizeof fb, "clux_f_%.*s", (int)vd->name.len,
                 vd->name.ptr ? vd->name.ptr : "");
        cir_expr_t *field =
            cir_expr_member(lc->arena,
                            cir_expr_ident(lc->arena, de->env_name),
                            copy_arena(lc->arena, fb));
        if (!field) continue;
        if (cs && (type_kind_of(cs) == TYPE_KIND_PTR ||
                   type_kind_of(cs) == TYPE_KIND_SLICE) &&
            (oq == OWN_OWN || oq == OWN_FATAL)) {
            cir_expr_t *target = field;
            if (type_kind_of(cs) == TYPE_KIND_SLICE)
                target = cir_expr_slice_data(lc->arena, field);
            cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_FREE);
            if (!st) return;
            lc->unit->uses_alloc = true;
            st->expr = target;
            append_stmt_chain(head, tail, st);
            continue;
        }
        if (cs && type_kind_of(cs) == TYPE_KIND_FUNC &&
            func_type_is_closure(cs) && oq == OWN_OWN) {
            (void)emit_closure_free(lc, field, cs, head, tail);
            continue;
        }
        if (cs && type_has_own_slots(cs)) {
            uint32_t dfid = 0;
            strslice_t dn = STRSLICE_EMPTY;
            if (!sema_find_destroy_fn(lc->sema, cs, &dfid, &dn)) continue;
            const char *cname = module_symbol(lc->arena, lc->unit, "fn_", dn);
            if (!cname) continue;
            cir_expr_t *call = cir_expr_call(lc->arena, cname, NULL);
            cir_expr_append_arg(call, cir_expr_ident(lc->arena, "NULL"));
            cir_expr_append_arg(call, cir_expr_unary(lc->arena, "&", field));
            cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_EXPR);
            if (!st) return;
            st->expr = call;
            append_stmt_chain(head, tail, st);
        }
    }
}


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
            d->def = fn;
            d->next = lc->defers;
            lc->defers = d;
            /* 捕获结构体：defer 体内的 `__ctx.env.<cap>` 指向这里声明的
               局部值（登记点按值复制一次）。 */
            if (fn->captures) {
                char tn[96];
                snprintf(tn, sizeof tn, "struct %sde_%u",
                         lc->unit->sym_prefix ? lc->unit->sym_prefix
                                              : "clux_",
                         lc->fnlit_seq++);
                cir_type_t *env = cir_type_new(
                    lc->arena, CIR_TYPE_STRUCT,
                    copy_arena(lc->arena, tn));
                if (!env) return NULL;
                cir_unit_add_type(lc->unit, env);
                cir_expr_t *ctor = cir_expr_construct(lc->arena, env, NULL);
                if (!ctor) return NULL;
                for (ast_node_t *c = fn->captures; c; c = c->next) {
                    ast_var_def_t *vd = (ast_var_def_t *)c;
                    const type_t *ct = vd->resolved_type;
                    if (!ct) ct = lookup_local(lc, vd->name);
                    if (!ct && vd->type_expr)
                        ct = sema_resolve_type_slot(lc->sema, &vd->type_expr);
                    if (!ct) {
                        diag_error(lc->sema->diag, sema_loc(lc->sema, c),
                                   "c backend: cannot determine defer "
                                   "capture type");
                        lc->failed = true;
                        *ok = false;
                        return NULL;
                    }
                    cir_type_t *fct = map_type(lc, ct);
                    if (!fct) {
                        *ok = false;
                        return NULL;
                    }
                    char fb[256];
                    snprintf(fb, sizeof fb, "clux_f_%.*s", (int)vd->name.len,
                             vd->name.ptr ? vd->name.ptr : "");
                    const char *fname = copy_arena(lc->arena, fb);
                    cir_field_t *f = cir_field_new(lc->arena, fname, fct);
                    if (!f) return NULL;
                    cir_type_add_field(env, f);
                    const char *vn = lookup_local_cname(lc, vd->name);
                    cir_expr_t *ve = NULL;
                    /* 带初始化表达式的捕获（`|(q: ref *Box = p.*.&)|`）：
                       登记点按该表达式求值。 */
                    if (vd->init && vd->init->kind != AST_UNDEF) {
                        ve = lower_expr(lc, vd->init);
                    }
                    if (!ve && vn)
                        ve = cir_expr_ident(lc->arena, vn);
                    if (!ve) {
                        /* 捕获可能来自外层闭包的捕获层或全局。 */
                        for (lower_capture_t *cp = lc->captures; cp;
                             cp = cp->next)
                            if (cp->name.len == vd->name.len &&
                                memcmp(cp->name.ptr, vd->name.ptr,
                                       cp->name.len) == 0) {
                                ve = cir_expr_ident(lc->arena, cp->expr);
                                break;
                            }
                    }
                    if (!ve) {
                        const char *gn = module_symbol(
                            lc->arena, lc->unit, "v_", vd->name);
                        if (gn) ve = cir_expr_ident(lc->arena, gn);
                    }
                    if (!ve) {
                        diag_error(lc->sema->diag, sema_loc(lc->sema, c),
                                   "c backend: defer capture '%.*s' is not "
                                   "a local",
                                   (int)vd->name.len, vd->name.ptr);
                        lc->failed = true;
                        *ok = false;
                        return NULL;
                    }
                    cir_init_t *init =
                        cir_init_new(lc->arena, fname, ve);
                    if (!init) return NULL;
                    cir_expr_add_init(ctor, init);
                }
                char en[64];
                snprintf(en, sizeof en, "clux_defer_env_%u",
                         lc->defer_seq++);
                d->env_name = copy_arena(lc->arena, en);
                cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_VAR);
                if (!st) return NULL;
                st->type = env;
                st->name = d->env_name;
                st->expr = ctor;
                return st;
            }
            return NULL;
        }
        /* 块内具名函数：无**值捕获**时降成独立 static C 函数（自递归/后向
           兄弟只是函数引用，env 传 NULL）；有值捕获时降成闭包值局部变量
           （捕获在定义点 clone；自递归经闭包值暂不支持）。 */
        bool value_cap = false;
        for (ast_node_t *c = fn->captures; c; c = c->next) {
            ast_var_def_t *cd = (ast_var_def_t *)c;
            const type_t *ct = cd->resolved_type;
            if (!ct && cd->type_expr)
                ct = sema_resolve_type_slot(lc->sema, &cd->type_expr);
            if (!ct || type_kind_of(type_strip_quals(ct)) != TYPE_KIND_FUNC ||
                (cd->init && cd->init->kind != AST_UNDEF))
                value_cap = true;
        }
        if (value_cap) {
            strslice_t orig_name = fn->name;
            cir_expr_t *cl = lower_closure_literal(lc, fn);
            if (cl) {
                lower_local_fn_t *fe = find_local_fn(lc, orig_name);
                if (fe) fe->env_tag = lc->last_closure_env_tag;
            }
            if (!cl) {
                *ok = false;
                return NULL;
            }
            const type_t *ft = func_type_of_def(lc, fn);
            cir_type_t *ct = ft ? map_type(lc, ft) : NULL;
            if (!ct) {
                *ok = false;
                return NULL;
            }
            lower_local_fn_t *lfe = find_local_fn(lc, orig_name);
            const char *vn = (lfe && lfe->is_closure && lfe->var_name)
                                 ? lfe->var_name
                                 : module_symbol(lc->arena, lc->unit,
                                                 "fnval_", orig_name);
            register_local_as(lc, orig_name, ft, vn);
            if (lfe && lfe->predeclared) {
                cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_ASSIGN);
                if (!st) return NULL;
                st->lhs = cir_expr_ident(lc->arena, vn);
                st->op = "=";
                st->expr = cl;
                /* 互递归 fixup：先把自身登记为 defined，再补写 env 中的函数
                   引用字段（captures 里函数值在定义点可能尚未初始化）。 */
                lfe->defined = true;
                cir_stmt_t *tail = st;
                for (lower_local_fn_t *other = lc->local_fns; other;
                     other = other->next) {
                    if (!other->is_closure || !other->var_name) continue;
                    if (!other->defined) continue;
                    bool wants = false;
                    for (size_t ci = 0; ci < other->cap_ref_count; ci++) {
                        strslice_t cn = other->cap_refs[ci];
                        if (cn.len == orig_name.len &&
                            memcmp(cn.ptr, orig_name.ptr, cn.len) == 0) {
                            wants = true;
                            break;
                        }
                    }
                    if (!wants) continue;
                    char fb[256];
                    snprintf(fb, sizeof fb, "clux_f_%.*s", (int)orig_name.len,
                             orig_name.ptr ? orig_name.ptr : "");
                    char eb[512];
                    snprintf(eb, sizeof eb,
                             "((%s *)(%s.env))->%s",
                             other->env_tag ? other->env_tag
                                            : "struct clux_env_0",
                             other->var_name, fb);
                    cir_expr_t *field =
                        cir_expr_ident(lc->arena, copy_arena(lc->arena, eb));
                    cir_expr_t *val = cir_expr_ident(lc->arena, vn);
                    if (!field || !val) break;
                    cir_stmt_t *fix = cir_stmt_new(lc->arena, CIR_STMT_ASSIGN);
                    if (!fix) break;
                    fix->lhs = field;
                    fix->op = "=";
                    fix->expr = val;
                    tail->next = fix;
                    tail = fix;
                }
                return st;
            }
            cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_VAR);
            if (!st) return NULL;
            st->type = ct;
            st->name = vn;
            st->expr = cl;
            return st;
        }
        const char *cname = module_symbol(lc->arena, lc->unit, "fn_", fn->name);
        const char *obj = module_symbol(lc->arena, lc->unit, "obj_", fn->name);
        const type_t *ftype = closure_type_of_def(lc, fn);
        if (!cname || !obj || !ftype) {
            lc->failed = true;
            *ok = false;
            return NULL;
        }
        lower_local_fn_t *lf = (lower_local_fn_t *)arena_calloc(
            lc->arena, 1, sizeof(*lf), ALIGNOF(max_align_t));
        if (!lf) return NULL;
        lf->name = fn->name;
        lf->cname = cname;
        lf->obj = obj;
        lf->type = ftype;
        lf->fid = fn->fid;
        lower_local_fn_t *save_fns = lc->local_fns;
        lf->next = lc->local_fns;
        lc->local_fns = lf;

        lower_local_t *save_locals = lc->locals;
        lower_defer_t *save_defers = lc->defers;
        cir_stmt_t *save_cleanup = lc->cleanup;
        cir_type_t *save_ret = lc->return_type;
        unsigned save_ret_seq = lc->ret_seq;
        lc->locals = NULL;
        lc->defers = NULL;
        lc->cleanup = NULL;
        lc->ret_seq = 0;
        cir_function_t *cf = lower_function(lc, fn, false);
        lc->locals = save_locals;
        lc->defers = save_defers;
        lc->cleanup = save_cleanup;
        lc->return_type = save_ret;
        lc->ret_seq = save_ret_seq;
        lc->local_fns = save_fns;
        if (!cf) {
            *ok = false;
            return NULL;
        }
        cir_unit_add_function(lc->unit, cf);
        return NULL;
    }
    case AST_VAR_DEF: {
        ast_var_def_t *vd = (ast_var_def_t *)s;
        const type_t *t = vd->resolved_type;
        if (!t && vd->type_expr) t = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        if (!t || type_kind_of(t) == TYPE_KIND_VOID)
            t = infer_expr_type(lc, vd->init);
        t = decl_value_type(lc, t, vd->init);
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
        st->name = lookup_local_cname(lc, vd->name);
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
                       (!it || type_kind_of(type_strip_quals(it)) != TYPE_KIND_OPTION) &&
                       !c_lower_is_clone_call(vd->init)) {
                cir_expr_t *v = lower_expr(lc, vd->init);
                if (v && v->type && v->type->kind == CIR_TYPE_OPTION)
                    st->expr = v; /* 已是 optional（如 weak.get()） */
                else
                    st->expr = v ? cir_expr_option(lc->arena, ct, true, v)
                                 : NULL;
            } else {
                /* 匿名构造（`var b: Box[U] = .{ ... }`）：sema 推断出的
                   interned 结构与声明类型布局相同但 type_t 不同 → C 里是
                   两个 struct 类型。把构造的类型槽指向声明类型，直接生成
                   `(声明类型){ ... }`，避免不兼容赋值。 */
                bool patched_ctor = false;
                ast_node_t *saved_ctor_type = NULL;
                if (vd->init->kind == AST_CONSTRUCT && t &&
                    type_kind_of(type_strip_quals(t)) == TYPE_KIND_STRUCT) {
                    ast_construct_t *cc = (ast_construct_t *)vd->init;
                    const type_t *decl = t;
                    const type_t *own = cc->type
                                            ? sema_resolve_type_slot(
                                                  lc->sema, &cc->type)
                                            : NULL;
                    if (type_strip_quals(decl) !=
                        (own ? type_strip_quals(own) : NULL)) {
                        ast_node_t *ref =
                            sema_gen_type_ref(lc->sema, type_strip_quals(t));
                        if (ref) {
                            saved_ctor_type = cc->type;
                            cc->type = ref;
                            patched_ctor = true;
                        }
                    }
                }
                st->expr = lower_rc_copy(
                    lc, t, lower_compat_value(lc, t, vd->init), vd->init);
                if (patched_ctor) ((ast_construct_t *)vd->init)->type =
                    saved_ctor_type;
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
        /* `_ = expr`（discard 赋值）：目标不是真实变量，只求值表达式。
           若表达式是 own/fatal 的指针/切片/闭包，丢弃时必须释放——
           否则 `_ = new T{...}` / `_ = move(p)` 会泄漏。 */
        if (a->target && a->target->kind == AST_IDENT &&
            ((ast_ident_t *)a->target)->name.len == 1 &&
            ((ast_ident_t *)a->target)->name.ptr &&
            ((ast_ident_t *)a->target)->name.ptr[0] == '_') {
            /* discard_mode 2：sema 已把销毁序列放进 prelude（`__disc_N`
               临时变量 + destroy），语句本身不需要再做任何事，否则会
               重复求值右值（多分配一次 → 泄漏）。 */
            if (a->discard_mode == 2) return NULL;
            const type_t *vt = infer_expr_type(lc, a->value);
            const type_t *vs = vt ? type_strip_quals(vt) : NULL;
            ownership_t vo = vt ? type_ownership_of(vt) : OWN_NONE;
            if (vs && (a->discard_mode == 3 || vo == OWN_OWN ||
                       vo == OWN_FATAL)) {
                if (type_kind_of(vs) == TYPE_KIND_PTR ||
                    type_kind_of(vs) == TYPE_KIND_SLICE) {
                    cir_expr_t *ex = lower_expr(lc, a->value);
                    if (!ex) {
                        *ok = false;
                        return NULL;
                    }
                    if (type_kind_of(vs) == TYPE_KIND_SLICE)
                        ex = cir_expr_slice_data(lc->arena, ex);
                    cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_FREE);
                    if (!st) {
                        *ok = false;
                        return NULL;
                    }
                    lc->unit->uses_alloc = true;
                    st->expr = ex;
                    return st;
                }
                if (type_kind_of(vs) == TYPE_KIND_FUNC &&
                    func_type_capture_count(vs) > 0) {
                    cir_expr_t *ex = lower_expr(lc, a->value);
                    if (!ex) {
                        *ok = false;
                        return NULL;
                    }
                    cir_stmt_t *head = NULL, *tail = NULL;
                    if (!emit_closure_free(lc, ex, vs, &head, &tail)) {
                        *ok = false;
                        return NULL;
                    }
                    return head;
                }
                if (type_has_own_slots(vs)) {
                    uint32_t dfid = 0;
                    strslice_t dn = STRSLICE_EMPTY;
                    if (sema_find_destroy_fn(lc->sema, vs, &dfid, &dn)) {
                        const char *cname =
                            module_symbol(lc->arena, lc->unit, "fn_", dn);
                        cir_expr_t *ex = lower_expr(lc, a->value);
                        if (!cname || !ex) {
                            *ok = false;
                            return NULL;
                        }
                        cir_stmt_t *blk =
                            cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
                        cir_stmt_t *tv =
                            cir_stmt_new(lc->arena, CIR_STMT_VAR);
                        if (!blk || !tv) {
                            *ok = false;
                            return NULL;
                        }
                        char nb[48];
                        snprintf(nb, sizeof nb, "clux_disc_%u",
                                 lc->assign_seq++);
                        tv->name = copy_arena(lc->arena, nb);
                        tv->expr = ex;
                        cir_stmt_append(blk, tv);
                        cir_expr_t *call =
                            cir_expr_call(lc->arena, cname, NULL);
                        cir_expr_append_arg(call,
                                            cir_expr_ident(lc->arena, "NULL"));
                        cir_expr_append_arg(
                            call, cir_expr_unary(lc->arena, "&",
                                                 cir_expr_ident(lc->arena,
                                                                tv->name)));
                        cir_stmt_t *ds =
                            cir_stmt_new(lc->arena, CIR_STMT_EXPR);
                        ds->expr = call;
                        cir_stmt_append(blk, ds);
                        return blk;
                    }
                }
            }
            cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_EXPR);
            if (!st) {
                *ok = false;
                return NULL;
            }
            st->expr = lower_expr(lc, a->value);
            if (!st->expr) {
                *ok = false;
                return NULL;
            }
            return st;
        }
        /* 跨模块全局变量写：`math::PI = v`（C 符号是导出方模块前缀的全局）。
           注意先排除 port 导入的 extern var（is_ffi_var）：它走下面的
           C 数据符号路径，不是模块前缀全局。 */
        if (a->target && a->target->kind == AST_SCOPE_REF &&
            is_import_scope_ref(lc, (ast_scope_ref_t *)a->target) &&
            !((ast_scope_ref_t *)a->target)->is_ffi_var) {
            const char *cname = lower_import_var_target(
                lc, (ast_scope_ref_t *)a->target);
            if (!cname) {
                *ok = false;
                return NULL;
            }
            cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_ASSIGN);
            if (!st) return NULL;
            st->lhs = cir_expr_ident(lc->arena, cname);
            st->op = op;
            st->expr = lower_expr(lc, a->value);
            if (!st->lhs || !st->expr) {
                *ok = false;
                return NULL;
            }
            return st;
        }
        /* extern var 写：C 符号是普通全局变量（`td_i32 = v;`）；str 目标
           需要把 clux_str 取值成 char*（`.data`）。 */
        if (a->target && a->target->kind == AST_SCOPE_REF &&
            ((ast_scope_ref_t *)a->target)->is_ffi_var) {
            bool is_str = false;
            const char *cname = lower_ffi_var_target(
                lc, (ast_scope_ref_t *)a->target, &is_str);
            if (!cname) {
                *ok = false;
                return NULL;
            }
            cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_ASSIGN);
            if (!st) return NULL;
            st->lhs = cir_expr_ident(lc->arena, cname);
            st->op = op;
            cir_expr_t *value = lower_expr(lc, a->value);
            if (!st->lhs || !value) {
                *ok = false;
                return NULL;
            }
            if (is_str) {
                if (strcmp(op, "=") != 0) {
                    diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                               "c backend: extern `str` variable does not "
                               "support compound assignment");
                    *ok = false;
                    return NULL;
                }
                value = cir_expr_member(lc->arena, value, "data");
            }
            st->expr = value;
            return st;
        }
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
            if (c_lower_is_clone_call(a->value)) {
                st->expr = lower_expr(lc, a->value);
            } else if (!vt || type_kind_of(type_strip_quals(vt)) != TYPE_KIND_OPTION) {
                cir_type_t *ot = map_type(lc, tt);
                cir_expr_t *v = lower_expr(lc, a->value);
                st->expr = v ? cir_expr_option(lc->arena, ot, true, v) : NULL;
            } else {
                st->expr = lower_expr(lc, a->value);
            }
        } else {
            st->expr = lower_rc_copy(
                lc, tt, lower_compat_value(lc, tt, a->value), a->value);
        }
        /* 赋值点旧值销毁（sema 生成的 `a->destroy`）：compiler 在"右值求值
           之后、写入之前"逐条执行。C 里先把右值存进临时变量，再销毁旧值，
           最后赋值；否则 `p = new ...` 会先释放再求值，语义错。
           聚合（struct/option/union/tuple/array）的旧值销毁在 VM 由
           compound_assign_transfer 完成，sema 不再生成 destroy 链；C 里用
           类型生成的 `__destroy_<T>` 补上。 */
        ast_node_t *destroy_chain = a->destroy;
        cir_stmt_t *synth_destroy = NULL;
        bool gen_uninit = false;
        if (a->target && a->target->kind == AST_IDENT) {
            strslice_t tn = ((ast_ident_t *)a->target)->name;
            if (tn.ptr && tn.len >= 7 && memcmp(tn.ptr, "__disc_", 7) == 0)
                gen_uninit = true; /* sema 生成的未初始化临时变量 */
        }
        const type_t *tts = tt ? type_strip_quals(tt) : NULL;
        if (!destroy_chain && !gen_uninit && !a->reinit && tts && st->lhs &&
            type_has_own_slots(tts) &&
            (type_kind_of(tts) == TYPE_KIND_STRUCT ||
             type_kind_of(tts) == TYPE_KIND_OPTION ||
             type_kind_of(tts) == TYPE_KIND_UNION ||
             type_kind_of(tts) == TYPE_KIND_TUPLE ||
             type_kind_of(tts) == TYPE_KIND_ARRAY)) {
            uint32_t dfid = 0;
            strslice_t dn = STRSLICE_EMPTY;
            if (sema_find_destroy_fn(lc->sema, tts, &dfid, &dn)) {
                const char *cname =
                    module_symbol(lc->arena, lc->unit, "fn_", dn);
                if (cname) {
                    cir_expr_t *call =
                        cir_expr_call(lc->arena, cname, NULL);
                    if (call) {
                        cir_expr_append_arg(call,
                                            cir_expr_ident(lc->arena, "NULL"));
                        cir_expr_append_arg(
                            call, cir_expr_unary(lc->arena, "&", st->lhs));
                        synth_destroy =
                            cir_stmt_new(lc->arena, CIR_STMT_EXPR);
                        if (synth_destroy) synth_destroy->expr = call;
                    }
                }
            }
        }
        if ((destroy_chain || synth_destroy) && st->lhs && st->expr) {
            cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
            if (!blk) {
                *ok = false;
                return NULL;
            }
            cir_stmt_t *tv = cir_stmt_new(lc->arena, CIR_STMT_VAR);
            if (!tv) {
                *ok = false;
                return NULL;
            }
            char nb[48];
            snprintf(nb, sizeof nb, "clux_av_%u", lc->assign_seq++);
            tv->name = copy_arena(lc->arena, nb);
            tv->expr = st->expr;
            cir_stmt_append(blk, tv);
            if (destroy_chain) {
                for (ast_node_t *d = destroy_chain; d; d = d->next) {
                    bool okd = true;
                    cir_stmt_t *ds = lower_stmt(lc, d, &okd);
                    if (!okd) {
                        *ok = false;
                        return NULL;
                    }
                    for (cir_stmt_t *it = ds; it;) {
                        cir_stmt_t *nx = it->next;
                        it->next = NULL;
                        cir_stmt_append(blk, it);
                        it = nx;
                    }
                }
            } else {
                cir_stmt_append(blk, synth_destroy);
            }
            st->expr = cir_expr_ident(lc->arena, tv->name);
            cir_stmt_append(blk, st);
            return blk;
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
                lower_defer_t *de = find_defer_entry(lc, name);
                ast_node_t *body = de ? de->body : find_defer_body(lc, name);
                if (body) {
                    bool okd = true;
                    cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
                    if (!st) return NULL;
                    const char *save_env = lc->ctx_env_name;
                    lc->ctx_env_name = de ? de->env_name : NULL;
                    st->body = lower_block(lc, body, &okd);
                    lc->ctx_env_name = save_env;
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
                                     TYPE_KIND_OPTION) &&
                    !c_lower_is_clone_call(r->value)) {
                    cir_expr_t *v = lower_expr(lc, r->value);
                    st->expr = v ? cir_expr_option(lc->arena,
                                                   lc->return_type, true, v)
                                 : NULL;
                } else {
                    /* 泛型返回值里的匿名/实例构造：把构造类型槽临时指向声明的
                       返回类型，避免 C 里两个等布局 struct 不兼容。 */
                    ast_node_t *saved = NULL;
                    bool patched = false;
                    const type_t *rtt = lc->return_sema_type;
                    if (rtt && r->value->kind == AST_CONSTRUCT &&
                        type_kind_of(type_strip_quals(rtt)) ==
                            TYPE_KIND_STRUCT) {
                        ast_construct_t *cc = (ast_construct_t *)r->value;
                        const type_t *own =
                            cc->type ? sema_resolve_type_slot(
                                           lc->sema, &cc->type)
                                     : NULL;
                        if (type_strip_quals(rtt) !=
                            (own ? type_strip_quals(own) : NULL)) {
                            ast_node_t *ref = sema_gen_type_ref(
                                lc->sema, type_strip_quals(rtt));
                            if (ref) {
                                saved = cc->type;
                                cc->type = ref;
                                patched = true;
                            }
                        }
                    }
                    st->expr = lower_expr(lc, r->value);
                    if (patched)
                        ((ast_construct_t *)r->value)->type = saved;
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
        ast_node_t *src = fi->source;
        ast_node_t *base = src;
        bool deref = false;
        if (src && src->kind == AST_DEREF) {
            deref = true;
            base = ((ast_deref_t *)src)->operand;
        }
        cir_stmt_t *srcvar = NULL;
        const char *raw = NULL;
        if (base && base->kind == AST_IDENT) {
            raw = module_symbol(lc->arena, lc->unit, "v_",
                                ((ast_ident_t *)base)->name);
        } else {
            /* foreach 的源可以是成员/解引用等任意左值：先取地址到局部
               指针（只求值一次），循环长度/元素都通过 `(*p)` 访问。 */
            cir_expr_t *mexpr = lower_expr(lc, src);
            if (!mexpr) {
                *ok = false;
                return NULL;
            }
            char pn[48];
            snprintf(pn, sizeof pn, "clux_src_%u", lc->assign_seq++);
            const char *pname = copy_arena(lc->arena, pn);
            srcvar = cir_stmt_new(lc->arena, CIR_STMT_VAR);
            if (!srcvar) return NULL;
            srcvar->name = pname;
            srcvar->expr = cir_expr_unary(lc->arena, "&", mexpr);
            char rb[80];
            snprintf(rb, sizeof rb, "(*%s)", pname);
            raw = copy_arena(lc->arena, rb);
        }
        if (!raw) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, s),
                       "c backend: generated index loop source is unsupported");
            *ok = false;
            return NULL;
        }
        const char *vn = module_symbol(lc->arena, lc->unit, "v_", fi->var);
        if (!vn) return NULL;
        char srcbuf[512];
        snprintf(srcbuf, sizeof srcbuf, deref ? "(*%s)" : "%s", raw);
        const char *an = copy_arena(lc->arena, srcbuf);
        const type_t *stype =
            base && base->kind == AST_IDENT
                ? lookup_local(lc, ((ast_ident_t *)base)->name)
                : infer_expr_type(lc, src);
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
        if (srcvar) {
            cir_stmt_t *blk = cir_stmt_new(lc->arena, CIR_STMT_BLOCK);
            if (!blk) return NULL;
            blk->body = srcvar;
            srcvar->next = st;
            return blk;
        }
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
        cir_stmt_t *head = NULL, *tail = NULL;
        for (ast_node_t *op = f->operand; op; op = op->next) {
            /* `FREE __defer_N`：defer 体已被内联，捕获结构体是栈上局部，
               没有堆环境可释放。 */
            if (op->kind == AST_IDENT &&
                is_defer_name(((ast_ident_t *)op)->name)) {
                /* `FREE __defer_N`：defer 体已执行完，释放栈上捕获结构体
                   里的 own 槽位（结构体本身随函数返回）。 */
                lower_defer_t *de =
                    find_defer_entry(lc, ((ast_ident_t *)op)->name);
                if (de && de->env_name && de->def) {
                    defer_env_free(lc, de, de->def, &head, &tail);
                }
                continue;
            }
            /* 块内具名函数（含自捕获递归）：C 里是静态函数对象，没有堆
               env；sema 按闭包类型生成的 FREE 不应释放静态对象。 */
            if (op->kind == AST_IDENT) {
                lower_local_fn_t *lf =
                    find_local_fn(lc, ((ast_ident_t *)op)->name);
                if (lf && !lf->var_name) continue;
            }
            const type_t *ot = infer_expr_type(lc, op);
            cir_expr_t *ex = lower_expr(lc, op);
            if (!ex) {
                *ok = false;
                return head;
            }
            if (ot && type_is_share(type_strip_quals(ot))) {
                cir_expr_t *call = cir_expr_call(lc->arena, "clux_rc_release",
                                                 NULL);
                if (!call) return head;
                cir_expr_append_arg(call, ex);
                cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_EXPR);
                if (!st) return head;
                st->expr = call;
                lc->unit->uses_rc = true;
                if (tail) tail->next = st;
                else head = st;
                tail = st;
                continue;
            }
            if (ot && type_is_weak(type_strip_quals(ot))) {
                cir_expr_t *call = cir_expr_call(
                    lc->arena, "clux_rc_weak_release", NULL);
                if (!call) return head;
                cir_expr_append_arg(call, ex);
                cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_EXPR);
                if (!st) return head;
                st->expr = call;
                lc->unit->uses_rc = true;
                if (tail) tail->next = st;
                else head = st;
                tail = st;
                continue;
            }
            if (ot && type_kind_of(ot) == TYPE_KIND_SLICE) {
                ex = cir_expr_slice_data(lc->arena, ex);
            } else if (ot && type_kind_of(ot) == TYPE_KIND_FUNC &&
                       func_type_capture_count(type_strip_quals(ot)) > 0) {
                /* 闭包值 env 内联：按捕获槽位递归释放 own 资源（嵌套闭包
                   捕获下钻；env_clone/env_dispose 是静态类型辅助，不在值里）。 */
                if (!emit_closure_free(lc, ex, type_strip_quals(ot), &head,
                                       &tail)) {
                    *ok = false;
                    return head;
                }
                continue;
            } else if (ot && type_kind_of(ot) == TYPE_KIND_FUNC) {
                /* 普通函数值无 env（空 env 结构），FREE 为 no-op。 */
                continue;
            }
            cir_stmt_t *st = cir_stmt_new(lc->arena, CIR_STMT_FREE);
            if (!st) return head;
            lc->unit->uses_alloc = true;
            st->expr = ex;
            if (tail) tail->next = st;
            else head = st;
            tail = st;
        }
        return head;
    }
    case AST_EMPTY_STMT:
        return NULL;
    /* 声明级节点（局部 typedef/struct/enum/union 定义、import/export 等）：
       sema 已把它们折叠成类型引用/导出标记，类型在用到时按需物化，这里
       没有对应的 C 语句。 */
    case AST_TYPE_DEF:
    case AST_ENUM_DEF:
    case AST_STRUCT_DEF:
    case AST_UNION_DEF:
    case AST_CUNION_DEF:
    case AST_INTERFACE_DEF:
    case AST_IMPORT:
    case AST_REEXPORT:
    case AST_EXTERN_LIB:
    case AST_EXPORT:
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

static cir_stmt_t *lower_block_scoped(lower_ctx_t *lc, ast_node_t *block,
                                      bool *ok);

cir_stmt_t *lower_block(lower_ctx_t *lc, ast_node_t *block, bool *ok) {    if (!block || block->kind != AST_BLOCK) {
        *ok = false;
        return NULL;
    }
    /* 块作用域：块内局部声明在块结束后失效（同名遮蔽的 C 名字唯一化也
       依赖这一点回退）。 */
    lower_local_t *save_locals = lc->locals;
    lower_local_fn_t *save_fns = lc->local_fns;
    cir_stmt_t *decl_head = NULL, *decl_tail = NULL;
    /* 预登记块内**无值捕获**的具名函数：调用点可能出现在定义之前（自递归/
       后向兄弟），先建 名字→静态 C 函数 映射。值捕获的走闭包值路径，不在
       预登记范围内。 */
    for (ast_node_t *s = ((ast_block_t *)block)->stmts; s; s = s->next) {
        if (s->kind != AST_FUNC_DEF) continue;
        ast_func_def_t *fn = (ast_func_def_t *)s;
        if (is_defer_name(fn->name)) continue;
        bool value_cap = false;
        for (ast_node_t *c = fn->captures; c; c = c->next) {
            ast_var_def_t *cd = (ast_var_def_t *)c;
            const type_t *ct = cd->resolved_type;
            if (!ct && cd->type_expr)
                ct = sema_resolve_type_slot(lc->sema, &cd->type_expr);
            if (!ct || type_kind_of(type_strip_quals(ct)) != TYPE_KIND_FUNC ||
                (cd->init && cd->init->kind != AST_UNDEF))
                value_cap = true;
        }
        const char *cname = module_symbol(lc->arena, lc->unit, "fn_", fn->name);
        const char *obj = module_symbol(lc->arena, lc->unit, "obj_", fn->name);
        const type_t *ftype = closure_type_of_def(lc, fn);
        if (!cname || !obj || !ftype) continue;
        lower_local_fn_t *lf = (lower_local_fn_t *)arena_calloc(
            lc->arena, 1, sizeof(*lf), ALIGNOF(max_align_t));
        if (!lf) continue;
        lf->name = fn->name;
        lf->cname = cname;
        lf->obj = obj;
        lf->type = ftype;
        lf->fid = fn->fid;
        if (value_cap) {
            lf->is_closure = true;
            lf->var_name = module_symbol(lc->arena, lc->unit, "fnval_",
                                         fn->name);
            /* 互递归/前向引用：闭包值局部在块入口先声明，定义点赋值。 */
            cir_type_t *fct = map_type(lc, ftype);
            cir_stmt_t *decl = cir_stmt_new(lc->arena, CIR_STMT_VAR);
            if (fct && decl) {
                decl->type = fct;
                decl->name = lf->var_name;
                lf->predeclared = true;
                if (decl_tail) decl_tail->next = decl;
                else decl_head = decl;
                decl_tail = decl;
            }
        }
        lf->next = lc->local_fns;
        lc->local_fns = lf;
    }
    /* 第二轮：登记闭包值局部捕获到的本地函数名（用于定义点 fixup）。 */
    for (ast_node_t *s2 = ((ast_block_t *)block)->stmts; s2; s2 = s2->next) {
        if (s2->kind != AST_FUNC_DEF) continue;
        ast_func_def_t *fn2 = (ast_func_def_t *)s2;
        lower_local_fn_t *lf2 = find_local_fn(lc, fn2->name);
        if (!lf2 || !lf2->is_closure) continue;
        size_t ncap = 0;
        for (ast_node_t *c = fn2->captures; c; c = c->next) {
            ast_var_def_t *cd = (ast_var_def_t *)c;
            if (cd->init && cd->init->kind != AST_UNDEF) continue;
            if (find_local_fn(lc, cd->name)) ncap++;
        }
        if (!ncap) continue;
        lf2->cap_refs = (strslice_t *)arena_calloc(
            lc->arena, ncap, sizeof(strslice_t), ALIGNOF(max_align_t));
        if (!lf2->cap_refs) continue;
        for (ast_node_t *c = fn2->captures; c; c = c->next) {
            ast_var_def_t *cd = (ast_var_def_t *)c;
            if (cd->init && cd->init->kind != AST_UNDEF) continue;
            if (!find_local_fn(lc, cd->name)) continue;
            lf2->cap_refs[lf2->cap_ref_count++] = cd->name;
        }
    }
    cir_stmt_t *result = lower_block_scoped(lc, block, ok);
    if (decl_head) {
        if (decl_tail) decl_tail->next = result;
        result = decl_head;
    }
    lc->locals = save_locals;
    lc->local_fns = save_fns;
    return result;
}

static cir_stmt_t *lower_block_scoped(lower_ctx_t *lc, ast_node_t *block,
                                      bool *ok) {
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
