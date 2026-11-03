/* __init__ / __init_copy__ 的**字段 TDZ 与完整性检查**。
 *
 * 这两个魔法方法的 self 指向"整体分配、字段只置零"的存储，字段在赋值前不是
 * 有效的 T。因此需要两条约束：
 *
 *   1. 函数任一 `return` 之前，self 的所有字段都必须被赋值过
 *      （整体赋值 `self.* = v` 或逐个字段赋值都算）；
 *   2. 字段在**首次赋值之前**不得被读取（读、方法调用、复合赋值读改写）。
 *
 * 实现是函数体内的保守数据流：状态 = (每个字段是否"肯定已赋值", 是否整体赋值)。
 * 分支在汇合点取交集（只在某个分支里赋的值不算"肯定"）；循环体按进入前的状态
 * 保守处理（循环里赋的值不保证执行）。闭包/lambda 内部不检查。
 */

#include "sema/init_tdz.h"

#include "parser/ast_assign.h"
#include "parser/ast_block.h"
#include "parser/ast_ident.h"
#include "parser/ast_call.h"
#include "parser/ast_deref.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_for.h"
#include "parser/ast_if.h"
#include "parser/ast_binary.h"
#include "parser/ast_index.h"
#include "parser/ast_member.h"
#include "parser/ast_return.h"
#include "parser/ast_switch.h"
#include "parser/ast_unary.h"
#include "parser/ast_var_def.h"
#include "parser/ast_while.h"
#include "diag/diagnostic.h"
#include "vm/type_struct.h"

#include <string.h>

typedef struct {
    const type_t *owner;
    size_t        nfields;
    unsigned char *assigned; /* nfields 字节：1 = 肯定已赋值 */
    bool           whole;
} tdz_state_t;

static const struct_field_t *field_at(const tdz_state_t *st, size_t i) {
    return struct_type_field(st->owner, i);
}

static int field_index(const tdz_state_t *st, strslice_t name) {
    if (!st->owner) return -1;
    for (size_t i = 0; i < st->nfields; i++) {
        const struct_field_t *f = field_at(st, i);
        if (f && f->name.len == name.len && f->name.ptr &&
            memcmp(f->name.ptr, name.ptr, name.len) == 0)
            return (int)i;
    }
    return -1;
}

/* `self`（可被括号/取地址包着？不：只认裸 ident self）。 */
static bool is_self_ident(const ast_node_t *n, strslice_t *out) {
    if (!n) return false;
    if (n->kind == AST_IDENT) {
        strslice_t nm = ((const ast_ident_t *)n)->name;
        if (nm.len == 4 && memcmp(nm.ptr, "self", 4) == 0) {
            if (out) *out = nm;
            return true;
        }
    }
    return false;
}

/* sema 把 `self.f` 改写成 `(*self).f`：两种形态都算 self。 */
static bool is_self_ref(const ast_node_t *n) {
    if (!n) return false;
    if (n->kind == AST_DEREF)
        return is_self_ident(((const ast_deref_t *)n)->operand, NULL);
    return is_self_ident(n, NULL);
}

/* 表达式里是否读取了"未赋值字段"；以及（赋值目标位置）是否写字段。 */
static void check_reads(sema_t *sema, tdz_state_t *st, ast_node_t *e,
                        const ast_node_t *write_target);

static void diag_uninit(sema_t *sema, ast_node_t *at, const type_t *owner,
                        strslice_t field) {
    char tn[64];
    sema_type_name(owner, tn, sizeof tn);
    diag_error(sema->diag, sema_loc(sema, at),
               "field '%.*s' of %s is used before it is assigned "
               "(self starts zeroed: assign each field before reading it)",
               (int)field.len, field.ptr, tn);
}

/* 读取检查：member 访问链 self.f / self.f... 的**第一层字段**必须已赋值。 */
static void check_reads(sema_t *sema, tdz_state_t *st, ast_node_t *e,
                        const ast_node_t *write_target) {
    if (!e) return;
    switch (e->kind) {
    case AST_MEMBER: {
        ast_member_t *m = (ast_member_t *)e;
        if (is_self_ref(m->object)) {
            int fi = field_index(st, m->field);
            if (fi >= 0 && !st->whole && !st->assigned[fi] &&
                !(write_target && write_target == e))
                diag_uninit(sema, e, st->owner, m->field);
            return; /* self.f.g：第一层已检查 */
        }
        check_reads(sema, st, m->object, NULL);
        return;
    }
    case AST_ADDR:
        return; /* 取地址不读值 */
    case AST_CALL: {
        ast_call_t *c = (ast_call_t *)e;
        check_reads(sema, st, c->callee, NULL);
        for (ast_node_t *a = c->args; a; a = a->next)
            check_reads(sema, st, a, NULL);
        return;
    }
    case AST_ASSIGN:
        return; /* 赋值在 check_stmt 里专门处理 */
    default:
        break;
    }
    /* 其余节点：按子链递归（子节点用 next 链） */
    for (ast_node_t *it = e; it; it = it->next) {
        if (it == e && it->kind == e->kind) { /* 首节点自身 */
        }
        /* 常见二元/一元/下标/解引用：递归 lhs/rhs/operand/object 等 */
        switch (it->kind) {
        case AST_BINARY: {
            ast_binary_t *b = (ast_binary_t *)it;
            check_reads(sema, st, b->lhs, NULL);
            check_reads(sema, st, b->rhs, NULL);
            break;
        }
        case AST_UNARY: {
            ast_unary_t *u = (ast_unary_t *)it;
            check_reads(sema, st, u->operand, NULL);
            break;
        }
        case AST_INDEX: {
            ast_index_t *ix = (ast_index_t *)it;
            check_reads(sema, st, ix->object, NULL);
            for (ast_node_t *a = ix->indices; a; a = a->next)
                check_reads(sema, st, a, NULL);
            break;
        }
        case AST_DEREF: {
            ast_deref_t *d = (ast_deref_t *)it;
            check_reads(sema, st, d->operand, NULL);
            break;
        }
        default:
            break;
        }
        if (it->kind == AST_CALL) break; /* 已单独处理 */
    }
}

/* 赋值目标：返回 true = 是 self 的字段/整体；并做相应状态更新。 */
static void handle_assign(sema_t *sema, tdz_state_t *st, ast_assign_t *as) {
    ast_node_t *t = as->target;
    /* self.* = v（整体赋值） */
    if (t && t->kind == AST_DEREF) {
        ast_deref_t *d = (ast_deref_t *)t;
        strslice_t dummy;
        if (is_self_ident(d->operand, &dummy)) {
            check_reads(sema, st, as->value, NULL);
            st->whole = true;
            memset(st->assigned, 1, st->nfields);
            return;
        }
        check_reads(sema, st, d->operand, NULL);
        check_reads(sema, st, as->value, NULL);
        return;
    }
    if (t && t->kind == AST_MEMBER) {
        ast_member_t *m = (ast_member_t *)t;
        if (is_self_ref(m->object)) {
            int fi = field_index(st, m->field);
            /* 复合赋值（`self.f += v`）读改写：先要求已赋值。
               注意 `as->op` 对普通 `=` 也非 NULL，必须比较 token 文本。 */
            bool compound = as->op && !token_is(as->op, "=");
            if (compound && !st->whole && fi >= 0 && !st->assigned[fi])
                diag_uninit(sema, t, st->owner, m->field);
            check_reads(sema, st, as->value, NULL);
            if (fi >= 0) st->assigned[fi] = 1;
            return;
        }
    }
    if (t && t->kind == AST_INDEX) {
        ast_index_t *ix = (ast_index_t *)t;
        /* `self.arr[i] = v`：把 arr 视为"已初始化"（元素级不追踪，v1 保守）。
           `self.arr[i]` 的读取：只有在 arr 未赋值时才报错。 */
        check_reads(sema, st, ix->object, t);
        for (ast_node_t *a = ix->indices; a; a = a->next)
            check_reads(sema, st, a, NULL);
        check_reads(sema, st, as->value, NULL);
        if (ix->object && ix->object->kind == AST_MEMBER) {
            ast_member_t *m = (ast_member_t *)ix->object;
            if (is_self_ref(m->object)) {
                int fi = field_index(st, m->field);
                if (fi >= 0) st->assigned[fi] = 1;
            }
        }
        return;
    }
    /* 局部变量 / 其它目标：两侧都按读处理 */
    check_reads(sema, st, t, NULL);
    check_reads(sema, st, as->value, NULL);
}

static void state_copy(tdz_state_t *dst, const tdz_state_t *src) {
    dst->owner = src->owner;
    dst->nfields = src->nfields;
    dst->whole = src->whole;
    if (dst->assigned && src->assigned && dst->nfields)
        memcpy(dst->assigned, src->assigned, dst->nfields);
}

static void check_stmt(sema_t *sema, tdz_state_t *st, ast_node_t *s);

/* 汇合：两个分支都赋过的字段才算"肯定" */
static void state_meet(tdz_state_t *out, const tdz_state_t *a,
                       const tdz_state_t *b) {
    out->whole = a->whole && b->whole;
    for (size_t i = 0; i < out->nfields; i++)
        out->assigned[i] = (unsigned char)((a->assigned[i] && b->assigned[i]) ||
                                           out->whole);
}

static void check_block(sema_t *sema, tdz_state_t *st, ast_node_t *blk) {
    ast_block_t *b = (ast_block_t *)blk;
    for (ast_node_t *s = b->stmts; s; s = s->next) check_stmt(sema, st, s);
}

static void tdz_return_check(sema_t *sema, tdz_state_t *st, ast_node_t *at) {
    if (st->whole) return;
    for (size_t i = 0; i < st->nfields; i++) {
        if (st->assigned[i]) continue;
        const struct_field_t *f = field_at(st, i);
        char tn[64];
        sema_type_name(st->owner, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, at),
                   "'%s' must assign field '%.*s' before returning "
                   "(self starts zeroed, so an unassigned field is not a valid "
                   "%s)", tn, (int)f->name.len, f->name.ptr ? f->name.ptr : "",
                   tn);
        return; /* 每个 return 只报一次 */
    }
}

static void check_stmt(sema_t *sema, tdz_state_t *st, ast_node_t *s) {
    if (!s) return;
    switch (s->kind) {
    case AST_BLOCK:
        check_block(sema, st, s);
        return;
    case AST_VAR_DEF: {
        ast_var_def_t *vd = (ast_var_def_t *)s;
        if (vd->init) check_reads(sema, st, vd->init, NULL);
        return;
    }
    case AST_EXPR_STMT: {
        ast_expr_stmt_t *es = (ast_expr_stmt_t *)s;
        if (es->expr) check_reads(sema, st, es->expr, NULL);
        return;
    }
    case AST_ASSIGN:
        handle_assign(sema, st, (ast_assign_t *)s);
        return;
    case AST_RETURN: {
        ast_return_t *r = (ast_return_t *)s;
        if (r->value) check_reads(sema, st, r->value, NULL);
        tdz_return_check(sema, st, s);
        return;
    }
    case AST_IF: {
        ast_if_t *f = (ast_if_t *)s;
        check_reads(sema, st, f->cond, NULL);
        tdz_state_t then_st = *st, else_st = *st, out = *st;
        unsigned char *buf = (unsigned char *)arena_calloc(
            sema->arena, st->nfields ? st->nfields * 2 : 2, 1,
            ALIGNOF(max_align_t));
        unsigned char *lbuf = (unsigned char *)arena_calloc(
            sema->arena, st->nfields ? st->nfields : 1, 1, ALIGNOF(max_align_t));
        if (!buf) return;
        then_st.assigned = buf;
        else_st.assigned = buf + st->nfields;
        out.assigned = lbuf;
        state_copy(&then_st, st);
        state_copy(&else_st, st);
        tdz_state_t t2 = then_st;
        if (f->then_body) check_stmt(sema, &t2, f->then_body);
        tdz_state_t e2 = else_st;
        if (f->else_body) check_stmt(sema, &e2, f->else_body);
        state_meet(&out, &t2, &e2);
        state_copy(st, &out);
        return;
    }
    case AST_WHILE: {
        ast_while_t *w = (ast_while_t *)s;
        check_reads(sema, st, w->cond, NULL);
        tdz_state_t inner = *st;
        unsigned char *buf = (unsigned char *)arena_calloc(
            sema->arena, st->nfields ? st->nfields : 1, 1, ALIGNOF(max_align_t));
        if (!buf) return;
        inner.assigned = buf;
        state_copy(&inner, st);
        if (w->body) check_stmt(sema, &inner, w->body);
        /* 循环体不保证执行：保持进入前状态 */
        return;
    }
    default:
        return;
    }
}

void sema_check_init_body(sema_t *sema, const type_t *owner,
                          ast_func_def_t *fn) {
    if (!sema || !owner || !fn || !fn->body) return;
    const type_t *core = type_strip_quals(owner);
    if (!core || type_kind_of(core) != TYPE_KIND_STRUCT) return;
    tdz_state_t st;
    memset(&st, 0, sizeof st);
    st.owner = core;
    st.nfields = struct_type_field_count(core);
    if (st.nfields == 0) return;
    st.assigned = (unsigned char *)arena_calloc(
        sema->arena, st.nfields, 1, ALIGNOF(max_align_t));
    if (!st.assigned) return;
    check_stmt(sema, &st, fn->body);
    /* 隐式 `return;`（函数体末尾）：同样要求所有字段已赋值。 */
    tdz_return_check(sema, &st, fn->body);
}
