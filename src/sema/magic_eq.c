#include "sema/magic.h"

#include "parser/ast_addr.h"
#include "parser/ast_assign.h"
#include "parser/ast_binary.h"
#include "parser/ast_block.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_deref.h"
#include "parser/ast_for.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_if.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_member.h"
#include "parser/ast_nil.h"
#include "parser/ast_program.h"
#include "parser/ast_return.h"
#include "parser/ast_unary.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_var_def.h"
#include "parser/lexer.h"
#include "sema/struct_compose.h"
#include "vm/type_array.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_struct.h"
#include "vm/type_union.h"

#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * 聚合判等的语法糖展开（sema 生成 `__clux_eq_<id>` 函数）
 *
 * 设计：`a == b` 的糖只有一条规则——操作数类型有 `__eq__` 就展开成对它的
 * 调用，否则按类型结构展开。结构展开需要循环/switch，所以物化成一个**普通
 * 函数** `__clux_eq_<typeid>(a: *const T, b: *const T) -> bool`；函数体里
 * 逐字段/逐元素/按 tag 写普通 `==` 表达式，这些 `==` 在 walk 时再次走同一
 * 条规则（递归），于是叶子上的 `__eq__` 就是一次普通方法调用——有实参、
 * 可泛型实例化。C 后端与 CTFE 不需要任何判等特判。
 *
 * 生成体按类型：
 *   struct（无 __eq__，非位域） → return a.f1 == b.f1 && ... ;
 *   optional                     → 拷贝到局部（const → 值），nil 分支 + `.!`
 *   array                        → for 循环逐元素 ==
 *   tag union                    → 拷贝到局部，`is` 分支 + 成员读
 * =========================================================================== */

typedef struct {
    const type_t *type;
    uint32_t      fid;
} meq_entry_t;

/* 生成体用到的 AST 构造助手。
   **语句**节点不标 is_generated（sema walker 会整体跳过生成语句）；**表达式**
   节点标 is_generated（启用"生成代码专用"规则：optional/array 的 `==`、
   非左值取址物化）。 */

static void meq_link(ast_node_t *head, ast_node_t *tail) {
    if (!head) return;
    ast_node_t *t = head;
    while (t->next) t = t->next;
    t->next = tail;
}

static ast_node_t *meq_ident(sema_t *s, const char *name) {
    ast_node_t *n = ast_ident_new(s->arena, 0, 0);
    if (!n || !name) return NULL;
    /* 名字必须 arena 持久化：调用点传进来的可能是栈上缓冲区。 */
    size_t len = strlen(name);
    char *buf = (char *)arena_alloc(s->arena, len, 1);
    if (!buf) return NULL;
    memcpy(buf, name, len);
    ((ast_ident_t *)n)->name = strslice_from_bytes(buf, len);
    n->is_generated = true;
    return n;
}

static ast_node_t *meq_member(sema_t *s, ast_node_t *obj, strslice_t field) {
    ast_node_t *n = ast_member_new(s->arena, 0, 0);
    if (!n) return NULL;
    ((ast_member_t *)n)->object = obj;
    ((ast_member_t *)n)->field = field;
    n->is_generated = true;
    return n;
}

static ast_node_t *meq_deref(sema_t *s, ast_node_t *e) {
    ast_node_t *n = ast_deref_new(s->arena, 0, 0);
    if (!n) return NULL;
    ((ast_deref_t *)n)->operand = e;
    n->is_generated = true;
    return n;
}

static ast_node_t *meq_addr(sema_t *s, ast_node_t *e) {
    ast_node_t *n = ast_addr_new(s->arena, 0, 0);
    if (!n) return NULL;
    ((ast_addr_t *)n)->operand = e;
    n->is_generated = true;
    return n;
}

static ast_node_t *meq_unwrap(sema_t *s, ast_node_t *e) {
    ast_node_t *n = ast_unwrap_new(s->arena, 0, 0);
    if (!n) return NULL;
    location_t loc;
    memset(&loc, 0, sizeof loc);
    const token_t *op = arena_token_symbol(s->arena, "!", &loc);
    if (op) ((ast_unwrap_t *)n)->op = op;
    ((ast_unwrap_t *)n)->operand = e;
    n->is_generated = true;
    return n;
}

static const token_t *meq_op(sema_t *s, const char *op) {
    location_t loc;
    memset(&loc, 0, sizeof loc);
    return arena_token_symbol(s->arena, op, &loc);
}

static ast_node_t *meq_bin(sema_t *s, const char *op, ast_node_t *l,
                           ast_node_t *r) {
    ast_node_t *n = ast_binary_new(s->arena, 0, 0);
    const token_t *tok = meq_op(s, op);
    if (!n || !tok) return NULL;
    ((ast_binary_t *)n)->op = tok;
    ((ast_binary_t *)n)->lhs = l;
    ((ast_binary_t *)n)->rhs = r;
    n->is_generated = true;
    return n;
}

static ast_node_t *meq_un(sema_t *s, const char *op, ast_node_t *e) {
    ast_node_t *n = ast_unary_new(s->arena, 0, 0);
    const token_t *tok = meq_op(s, op);
    if (!n || !tok) return NULL;
    ((ast_unary_t *)n)->op = tok;
    ((ast_unary_t *)n)->operand = e;
    n->is_generated = true;
    return n;
}

static ast_node_t *meq_ret(sema_t *s, ast_node_t *v) {
    ast_node_t *n = ast_return_new(s->arena, 0, 0);
    if (!n) return NULL;
    ((ast_return_t *)n)->value = v;
    return n;
}

static ast_node_t *meq_block(sema_t *s, ast_node_t *stmts) {
    ast_block_t *b = (ast_block_t *)ast_block_new(s->arena, 0, 0);
    if (!b) return NULL;
    b->stmts = stmts;
    ast_node_t *last = stmts;
    while (last && last->next) last = last->next;
    b->stmts_last = last;
    return &b->base;
}

static ast_node_t *meq_if(sema_t *s, ast_node_t *cond, ast_node_t *then_s,
                          ast_node_t *else_s) {
    ast_node_t *n = ast_if_new(s->arena, 0, 0);
    if (!n) return NULL;
    ((ast_if_t *)n)->cond = cond;
    ((ast_if_t *)n)->then_body = then_s;
    ((ast_if_t *)n)->else_body = else_s;
    return n;
}

static ast_node_t *meq_bool(sema_t *s, bool v) {
    ast_node_t *n = ast_bool_lit_new(s->arena, 0, 0);
    if (!n) return NULL;
    ((ast_bool_lit_t *)n)->value = v;
    n->is_generated = true;
    return n;
}

/* usize 整数字面量（后缀按平台宽度归一，与 foreach 展开一致）。 */
static ast_node_t *meq_usize_lit(sema_t *s, uint64_t v) {
    ast_node_t *n = ast_int_lit_new(s->arena, 0, 0);
    if (!n) return NULL;
    ast_int_lit_t *lit = (ast_int_lit_t *)n;
    lit->value = v;
    lit->type = sizeof(void *) == 8 ? strslice_from_cstr("u64")
                                    : strslice_from_cstr("u32");
    n->is_generated = true;
    return n;
}

static ast_node_t *meq_index(sema_t *s, ast_node_t *obj, ast_node_t *idx) {
    ast_node_t *n = ast_index_new(s->arena, 0, 0);
    if (!n) return NULL;
    ((ast_index_t *)n)->object = obj;
    ((ast_index_t *)n)->indices = idx;
    ((ast_index_t *)n)->indices_last = idx;
    n->is_generated = true;
    return n;
}

static ast_node_t *meq_var_def(sema_t *s, const char *name,
                               const type_t *t, ast_node_t *init) {
    ast_node_t *n = ast_var_def_new(s->arena, 0, 0);
    if (!n) return NULL;
    ast_var_def_t *vd = (ast_var_def_t *)n;
    vd->name = strslice_from_cstr(name);
    vd->type_expr = sema_gen_type_ref(s, t);
    vd->init = init;
    vd->resolved_type = t;
    return n;
}

static ast_node_t *meq_call(sema_t *s, ast_node_t *callee, ast_node_t *args) {
    ast_node_t *n = ast_call_new(s->arena, 0, 0);
    if (!n) return NULL;
    ast_call_t *c = (ast_call_t *)n;
    c->callee = callee;
    c->args = args;
    ast_node_t *last = args;
    while (last && last->next) last = last->next;
    c->args_last = last;
    n->is_generated = true;
    return n;
}

/* 指针类型 `*const T`（只读：生成体只读两个操作数）。 */
static const type_t *meq_ptr_const(sema_t *s, const type_t *t) {
    vm_t *vm = s->vm;
    const type_t *ct = type_const_intern(vm, t);
    if (!ct) ct = t;
    return type_ptr_intern(vm, ct);
}

/* ---------------------------------------------------------------- 生成体 */

/* struct（无 __eq__、非位域）：return a.f1 == b.f1 && ...; */
static ast_node_t *meq_body_struct(sema_t *s, const type_t *t,
                                   ast_node_t *a, ast_node_t *b) {
    size_t n = struct_type_field_count(t);
    ast_node_t *acc = NULL;
    for (size_t i = 0; i < n; i++) {
        const struct_field_t *f = struct_type_field(t, i);
        if (!f || !f->type) continue;
        ast_node_t *cmp = meq_bin(s, "==",
                                  meq_member(s, meq_ident(s, "a"), f->name),
                                  meq_member(s, meq_ident(s, "b"), f->name));
        if (!cmp) return NULL;
        if (!acc) acc = cmp;
        else {
            ast_node_t *and = meq_bin(s, "&&", acc, cmp);
            if (!and) return NULL;
            acc = and;
        }
    }
    if (!acc) acc = meq_bool(s, true);
    return meq_block(s, meq_ret(s, acc));
}

/* optional：拷贝到局部（`const ?U` → `?U`），nil 分支 + `.!`。 */
static ast_node_t *meq_body_option(sema_t *s, const type_t *t) {
    ast_node_t *oa = meq_var_def(s, "oa", t, meq_deref(s, meq_ident(s, "a")));
    ast_node_t *ob = meq_var_def(s, "ob", t, meq_deref(s, meq_ident(s, "b")));
    if (!oa || !ob) return NULL;
    ast_node_t *nil1 = ast_nil_new(s->arena, 0, 0);
    if (!nil1) return NULL;
    nil1->is_generated = true;
    ast_node_t *nil2 = ast_nil_new(s->arena, 0, 0);
    if (!nil2) return NULL;
    nil2->is_generated = true;
    ast_node_t *a_nil = meq_bin(s, "==", meq_ident(s, "oa"), nil1);
    ast_node_t *b_nil = meq_bin(s, "==", meq_ident(s, "ob"), nil2);
    ast_node_t *if_a_nil = meq_if(s, a_nil, meq_block(s, meq_ret(s, b_nil)), NULL);
    ast_node_t *if_b_nil =
        meq_if(s, meq_bin(s, "==", meq_ident(s, "ob"), ast_nil_new(s->arena, 0, 0)),
               meq_block(s, meq_ret(s, meq_bool(s, false))), NULL);
    ast_node_t *va = meq_unwrap(s, meq_ident(s, "oa"));
    ast_node_t *vb = meq_unwrap(s, meq_ident(s, "ob"));
    ast_node_t *ret_eq = meq_ret(s, meq_bin(s, "==", va, vb));
    ast_node_t *stmts = oa;
    meq_link(stmts, ob);
    meq_link(stmts, if_a_nil);
    meq_link(stmts, if_b_nil);
    meq_link(stmts, ret_eq);
    return meq_block(s, stmts);
}

/* array：for 循环逐元素比较。`a` 是 `*const [N]U`，`(a.*)[i]` 只读下标。 */
static ast_node_t *meq_body_array(sema_t *s, const type_t *t) {
    uint64_t n = (uint64_t)array_type_len(t);
    ast_node_t *i_name = meq_ident(s, "i");
    ast_node_t *idx_ty = sema_gen_type_ref(s, vm_type_usize(s->vm));
    ast_node_t *zero = meq_usize_lit(s, 0);
    ast_node_t *init = meq_var_def(s, "i", vm_type_usize(s->vm), zero);
    (void)idx_ty;
    if (!init || !i_name) return NULL;
    ast_node_t *cond =
        meq_bin(s, "<", meq_ident(s, "i"), meq_usize_lit(s, n));
    ast_node_t *one = meq_usize_lit(s, 1);
    ast_node_t *upd = ast_assign_new(s->arena, 0, 0);
    if (!upd) return NULL;
    {
        ast_assign_t *as = (ast_assign_t *)upd;
        as->target = meq_ident(s, "i");
        as->op = meq_op(s, "=");
        as->value = meq_bin(s, "+", meq_ident(s, "i"), one);
    }
    ast_node_t *ea = meq_index(s, meq_deref(s, meq_ident(s, "a")),
                               meq_ident(s, "i"));
    ast_node_t *eb = meq_index(s, meq_deref(s, meq_ident(s, "b")),
                               meq_ident(s, "i"));
    ast_node_t *cmp = meq_bin(s, "==", ea, eb);
    ast_node_t *not_cmp = meq_un(s, "!", cmp);
    ast_node_t *if_body =
        meq_if(s, not_cmp, meq_block(s, meq_ret(s, meq_bool(s, false))), NULL);
    ast_node_t *body = meq_block(s, if_body);
    ast_node_t *loop = ast_for_new(s->arena, 0, 0);
    if (!loop) return NULL;
    ((ast_for_t *)loop)->init = init;
    ((ast_for_t *)loop)->cond = cond;
    ((ast_for_t *)loop)->update = upd;
    ((ast_for_t *)loop)->body = body;
    ast_node_t *ret = meq_ret(s, meq_bool(s, true));
    ast_node_t *stmts = loop;
    meq_link(stmts, ret);
    return meq_block(s, stmts);
}

/* tag union：拷贝到局部（`const U` → `U`），`is` 分支 + 成员读。 */
static ast_node_t *meq_body_union(sema_t *s, const type_t *t) {
    ast_node_t *ua = meq_var_def(s, "ua", t, meq_deref(s, meq_ident(s, "a")));
    ast_node_t *ub = meq_var_def(s, "ub", t, meq_deref(s, meq_ident(s, "b")));
    if (!ua || !ub) return NULL;
    ast_node_t *stmts = ua;
    meq_link(stmts, ub);
    size_t n = union_type_member_count(t);
    for (size_t i = 0; i < n; i++) {
        const union_member_t *m = union_type_member(t, i);
        if (!m || !m->name.ptr) continue;
        ast_node_t *name = ast_ident_new(s->arena, 0, 0);
        if (!name) return NULL;
        ((ast_ident_t *)name)->name = m->name;
        name->is_generated = true;
        ast_node_t *a_is =
            meq_bin(s, "is", meq_ident(s, "ua"), name);
        ast_node_t *name2 = ast_ident_new(s->arena, 0, 0);
        if (!name2) return NULL;
        ((ast_ident_t *)name2)->name = m->name;
        name2->is_generated = true;
        ast_node_t *b_is =
            meq_bin(s, "is", meq_ident(s, "ub"), name2);
        ast_node_t *b_not = meq_un(s, "!", b_is);
        ast_node_t *if_b =
            meq_if(s, b_not, meq_block(s, meq_ret(s, meq_bool(s, false))), NULL);
        ast_node_t *payload =
            meq_bin(s, "==", meq_member(s, meq_ident(s, "ua"), m->name),
                    meq_member(s, meq_ident(s, "ub"), m->name));
        ast_node_t *inner = if_b;
        meq_link(inner, meq_ret(s, payload));
        ast_node_t *then_block = meq_block(s, inner);
        ast_node_t *iff = meq_if(s, a_is, then_block, NULL);
        meq_link(stmts, iff);
    }
    meq_link(stmts, meq_ret(s, meq_bool(s, false)));
    return meq_block(s, stmts);
}

/* ------------------------------------------------------------ 生成/缓存 */

static meq_entry_t *meq_find(sema_t *s, const type_t *t) {
    size_t n = s->magic_eq_cache ? vec_len((vec_t *)s->magic_eq_cache) : 0;
    for (size_t i = 0; i < n; i++) {
        meq_entry_t *e = (meq_entry_t *)vec_get((vec_t *)s->magic_eq_cache, i);
        if (e && e->type == t) return e;
    }
    return NULL;
}

/* 是否值得/可以生成函数：非位域 struct（无 __eq__）、tag union。 */
static bool meq_type_supported(sema_t *s, const type_t *t) {
    if (!t) return false;
    switch (type_kind_of(t)) {
    case TYPE_KIND_STRUCT:
        if (((const struct_type_t *)t)->bitfield_ast) return false;
        if (sema_magic_member(s, t, "__eq__")) return false; /* 直接调 __eq__ */
        return true;
    case TYPE_KIND_UNION:
        return true;
    case TYPE_KIND_OPTION:
    case TYPE_KIND_ARRAY:
        return true;
    default:
        return false;
    }
}

/* 生成（或取缓存）判等函数，返回 fid（0 = 不支持/失败）。 */
static uint32_t meq_generate(sema_t *s, const type_t *t) {
    vm_t *vm = s->vm;
    if (!meq_type_supported(s, t)) return 0;
    meq_entry_t *cached = meq_find(s, t);
    if (cached) return cached->fid;

    const sema_type_t *st = sema_type_register(s, t);
    if (!st) return 0;
    char name_buf[64];
    snprintf(name_buf, sizeof name_buf, "__clux_eq_%u", st->id);
    size_t nlen = strlen(name_buf);
    char *name = (char *)arena_alloc(s->arena, nlen, 1);
    if (!name) return 0;
    memcpy(name, name_buf, nlen);

    const type_t *pt = meq_ptr_const(s, t);
    if (!pt) return 0;
    ast_node_t *pa = meq_var_def(s, "a", pt, NULL);
    ast_node_t *pb = meq_var_def(s, "b", pt, NULL);
    if (!pa || !pb) return 0;
    ((ast_var_def_t *)pa)->type_expr = sema_gen_type_ref(s, pt);
    ((ast_var_def_t *)pb)->type_expr = sema_gen_type_ref(s, pt);
    meq_link(pa, pb);

    ast_node_t *body = NULL;
    switch (type_kind_of(t)) {
    case TYPE_KIND_STRUCT:
        body = meq_body_struct(s, t, pa, pb);
        break;
    case TYPE_KIND_UNION:
        body = meq_body_union(s, t);
        break;
    case TYPE_KIND_OPTION:
        body = meq_body_option(s, t);
        break;
    case TYPE_KIND_ARRAY:
        body = meq_body_array(s, t);
        break;
    default:
        return 0;
    }
    if (!body) return 0;

    ast_node_t *def = ast_func_def_new(s->arena, 0, 0);
    if (!def) return 0;
    ast_func_def_t *fn = (ast_func_def_t *)def;
    fn->name = strslice_from_bytes(name, nlen);
    fn->params = pa;
    fn->params_last = pb;
    fn->return_expr = sema_gen_type_ref(s, vm->type_bool);
    fn->body = body;
    fn->base.is_generated = true;
    fn->owner_module = NULL;

    /* 登记为普通函数：入 sema->funcs（Pass 3b worklist 会 walk 它）+ prog。
       先入队再建作用域，避免 walk 早于建树。 */
    sema_func_t *sf = (sema_func_t *)allocator_new_ex(
        vm->alloc, "sema_func_t", sizeof(sema_func_t), NULL, NULL, NULL, 1);
    if (!sf) return 0;
    memset(sf, 0, sizeof(*sf));
    sf->def = def;
    sf->def_scope = s->global_scope;
    sf->is_local = false;
    sf->is_walked = false;
    sema_func_id_alloc(s, fn);
    uint32_t fid = fn->fid;
    if (fid == 0) return 0;
    vec_push(s->funcs, vm->alloc, sf);
    if (s->prog && s->prog->kind == AST_PROGRAM) {
        ast_program_t *prog = (ast_program_t *)s->prog;
        ast_append(&prog->funcs, &prog->funcs_last, NULL, def);
    }
    sema_build_func_scope(s, sf);

    /* 签名 + 全局符号：调用点按普通函数名解析（实参按形参类型检查）。 */
    const type_t *params[2] = {pt, pt};
    const type_t *sig = type_func_sig(vm, params, 2, vm->type_bool, false);
    if (!sig) return 0;
    const sema_type_t *sig_st = sema_type_register(s, sig);
    if (!sig_st) return 0;
    fn->sig_id = sig_st->id; /* CTFE 按 fid 查签名/参数类型 */
    sema_symbol_t sym = {.kind = SEMA_SYM_FUNC,
                         .ast = def,
                         .fid = fid,
                         .type = sig,
                         .is_active = true};
    sema_scope_define(s->global_scope, fn->name, &sym);

    /* 缓存（在生成完当下写入；函数体 walk 时递归展开字段/元素会用到）。 */
    meq_entry_t *e = (meq_entry_t *)allocator_new_ex(
        vm->alloc, "meq_entry_t", sizeof(meq_entry_t), NULL, NULL, NULL, 1);
    if (!e) return fid;
    memset(e, 0, sizeof(*e));
    e->type = t;
    e->fid = fid;
    vec_push((vec_t *)s->magic_eq_cache, vm->alloc, e);
    return fid;
}

ast_node_t *sema_magic_eq_call(sema_t *s, const type_t *t, ast_node_t *lhs,
                               ast_node_t *rhs, uint32_t tok_begin,
                               uint32_t tok_end) {
    if (!s || !t || !lhs || !rhs) return NULL;
    uint32_t fid = meq_generate(s, t);
    if (!fid) return NULL;
    const sema_type_t *st = sema_type_find(s, t);
    if (!st) return NULL;
    char name_buf[64];
    snprintf(name_buf, sizeof name_buf, "__clux_eq_%u", st->id);
    (void)tok_begin;
    (void)tok_end;
    ast_node_t *callee = meq_ident(s, name_buf);
    ast_node_t *arg1 = meq_addr(s, lhs);
    ast_node_t *arg2 = meq_addr(s, rhs);
    if (!callee || !arg1 || !arg2) return NULL;
    meq_link(arg1, arg2);
    ast_node_t *call = meq_call(s, callee, arg1);
    if (call) {
        call->tok_begin = tok_begin;
        call->tok_end = tok_end;
    }
    return call;
}

bool sema_magic_eq_desugarable(sema_t *s, const type_t *t) {
    if (!s || !t) return false;
    const type_t *c = type_strip_quals(t);
    if (!c) return false;
    if (type_kind_of(c) == TYPE_KIND_STRUCT) {
        if (((const struct_type_t *)c)->bitfield_ast) return false;
        return true; /* 有 __eq__ 时调用点走成员调用，不走这里 */
    }
    return type_kind_of(c) == TYPE_KIND_UNION;
}
