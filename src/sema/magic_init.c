#include "sema/magic.h"
#include "diag/diagnostic.h"

#include "parser/ast_block.h"
#include "parser/ast_call.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ident.h"
#include "parser/ast_member.h"
#include "parser/ast_program.h"
#include "parser/ast_return.h"
#include "parser/ast_var_def.h"
#include "sema/struct_compose.h"
#include "vm/type_func.h"

#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * `__init__` 构造（§4.5）
 *
 * 有 `__init__` 的 struct 不再支持普通构造（具名字段 / 按序字段赋值），
 * `.T{arg1, arg2, ...}` 理解为：
 *
 *     var t: T = <zero>;          // 全部字段清零
 *     t.__init__(arg1, arg2, ...);
 *     // 值 = t
 *
 * 实现：为每个类型惰性生成一个普通函数
 *
 *     __clux_init_<id>(p1: A1, ..., pN: AN) -> T
 *
 * 构造点改写成对它的一次普通调用（参数求值顺序自然保持）。生成函数体里
 * 的 `t.__init__(...)` 是普通成员调用，走常规实例化/改写。
 * =========================================================================== */

typedef struct {
    const type_t *type;
    uint32_t      fid;
} mini_entry_t;

static mini_entry_t *mini_find(sema_t *s, const type_t *t) {
    size_t n = s->magic_init_cache ? vec_len((vec_t *)s->magic_init_cache) : 0;
    for (size_t i = 0; i < n; i++) {
        mini_entry_t *e = (mini_entry_t *)vec_get((vec_t *)s->magic_init_cache, i);
        if (e && e->type == t) return e;
    }
    return NULL;
}

/* 该类型是否有 `__init__`（struct + 成员方法）。 */
bool sema_magic_has_init(sema_t *sema, const type_t *t) {
    if (!sema || !t) return false;
    const type_t *core = type_strip_quals(t);
    if (!core || type_kind_of(core) != TYPE_KIND_STRUCT) return false;
    return sema_magic_member(sema, core, "__init__") != NULL;
}

static ast_node_t *mini_ident(sema_t *s, const char *name) {
    ast_node_t *n = ast_ident_new(s->arena, 0, 0);
    if (!n || !name) return NULL;
    size_t len = strlen(name);
    char *buf = (char *)arena_alloc(s->arena, len, 1);
    if (!buf) return NULL;
    memcpy(buf, name, len);
    ((ast_ident_t *)n)->name = strslice_from_bytes(buf, len);
    n->is_generated = true;
    return n;
}

/* 生成（或取缓存）`__clux_init_<id>` 的 fid；0 = 失败/不适用。 */
static uint32_t mini_generate(sema_t *s, const type_t *t) {
    vm_t *vm = s->vm;
    mini_entry_t *cached = mini_find(s, t);
    if (cached) return cached->fid;

    sema_symbol_t *ms = sema_magic_member(s, t, "__init__");
    if (!ms || !ms->type || type_kind_of(ms->type) != TYPE_KIND_FUNC) return 0;
    if (ms->is_generic) return 0; /* 泛型 __init__ 暂不支持 */

    size_t np = func_type_param_count(ms->type);
    if (np < 1) return 0;
    size_t nargs = np - 1;

    const sema_type_t *st = sema_type_register(s, t);
    if (!st) return 0;
    char name_buf[64];
    snprintf(name_buf, sizeof name_buf, "__clux_init_%u", st->id);
    size_t nlen = strlen(name_buf);
    char *name = (char *)arena_alloc(s->arena, nlen, 1);
    if (!name) return 0;
    memcpy(name, name_buf, nlen);

    /* 形参：`__init__` 的参数（跳过 self），名字 v1..vN。 */
    ast_node_t *params = NULL, *params_last = NULL;
    const type_t *param_types[64];
    if (nargs > 64) return 0;
    for (size_t i = 0; i < nargs; i++) {
        const type_t *pt = func_type_param(ms->type, i + 1);
        if (!pt) return 0;
        char pn[32];
        snprintf(pn, sizeof pn, "v%zu", i + 1);
        /* 形参名必须 arena 持久化（strslice 指向栈缓冲会悬空）。 */
        size_t plen = strlen(pn);
        char *pbuf = (char *)arena_alloc(s->arena, plen, 1);
        if (!pbuf) return 0;
        memcpy(pbuf, pn, plen);
        ast_node_t *vd = ast_var_def_new(s->arena, 0, 0);
        if (!vd) return 0;
        ast_var_def_t *p = (ast_var_def_t *)vd;
        p->name = strslice_from_bytes(pbuf, plen);
        p->type_expr = sema_gen_type_ref(s, pt);
        if (!p->type_expr) return 0;
        param_types[i] = pt;
        if (params_last) params_last->next = vd;
        else params = vd;
        params_last = vd;
    }

    /* 体：
         var t: T = <zero>;      （zero_init）
         t.__init__(v1, ..., vN);
         return t; */
    ast_node_t *tdef = ast_var_def_new(s->arena, 0, 0);
    if (!tdef) return 0;
    ast_var_def_t *tv = (ast_var_def_t *)tdef;
    tv->name = strslice_from_cstr("t");
    tv->type_expr = sema_gen_type_ref(s, t);
    tv->zero_init = true;
    if (!tv->type_expr) return 0;

    ast_node_t *tref = mini_ident(s, "t");
    ast_node_t *member = ast_member_new(s->arena, 0, 0);
    if (!tref || !member) return 0;
    ((ast_member_t *)member)->object = tref;
    ((ast_member_t *)member)->field = STRSLICE_LIT("__init__");
    ast_node_t *call = ast_call_new(s->arena, 0, 0);
    if (!call) return 0;
    ast_node_t *args = NULL, *args_last = NULL;
    for (size_t i = 0; i < nargs; i++) {
        char pn[32];
        snprintf(pn, sizeof pn, "v%zu", i + 1);
        ast_node_t *a = mini_ident(s, pn);
        if (!a) return 0;
        if (args_last) args_last->next = a;
        else args = a;
        args_last = a;
    }
    ((ast_call_t *)call)->callee = member;
    ((ast_call_t *)call)->args = args;
    ((ast_call_t *)call)->args_last = args_last;

    ast_node_t *stmt = ast_expr_stmt_new(s->arena, 0, 0);
    if (!stmt) return 0;
    ((ast_expr_stmt_t *)stmt)->expr = call;

    ast_node_t *ret = ast_return_new(s->arena, 0, 0);
    if (!ret) return 0;
    ((ast_return_t *)ret)->value = mini_ident(s, "t");

    tdef->next = stmt;
    stmt->next = ret;

    ast_node_t *body = ast_block_new(s->arena, 0, 0);
    if (!body) return 0;
    ((ast_block_t *)body)->stmts = tdef;
    ((ast_block_t *)body)->stmts_last = ret;

    ast_node_t *def = ast_func_def_new(s->arena, 0, 0);
    if (!def) return 0;
    ast_func_def_t *fn = (ast_func_def_t *)def;
    fn->name = strslice_from_bytes(name, nlen);
    fn->params = params;
    fn->params_last = params_last;
    fn->return_expr = sema_gen_type_ref(s, t);
    fn->body = body;
    fn->base.is_generated = true;

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

    const type_t *sig =
        type_func_sig(vm, param_types, nargs, t, false);
    if (!sig) return 0;
    const sema_type_t *sig_st = sema_type_register(s, sig);
    if (!sig_st) return 0;
    fn->sig_id = sig_st->id;
    sema_symbol_t sym = {.kind = SEMA_SYM_FUNC,
                         .ast = def,
                         .fid = fid,
                         .type = sig,
                         .is_active = true};
    sema_scope_define(s->global_scope, fn->name, &sym);

    mini_entry_t *e = (mini_entry_t *)allocator_new_ex(
        vm->alloc, "mini_entry_t", sizeof(mini_entry_t), NULL, NULL, NULL, 1);
    if (!e) return fid;
    memset(e, 0, sizeof(*e));
    e->type = t;
    e->fid = fid;
    vec_push((vec_t *)s->magic_init_cache, vm->alloc, e);
    return fid;
}

ast_node_t *sema_magic_init_call(sema_t *s, const type_t *t, ast_node_t *args,
                                 uint32_t tok_begin, uint32_t tok_end) {
    if (!s || !t) return NULL;
    uint32_t fid = mini_generate(s, t);
    if (!fid) return NULL;
    const sema_type_t *st = sema_type_find(s, t);
    if (!st) return NULL;
    char name_buf[64];
    snprintf(name_buf, sizeof name_buf, "__clux_init_%u", st->id);
    ast_node_t *callee = mini_ident(s, name_buf);
    if (!callee) return NULL;
    ast_node_t *call = ast_call_new(s->arena, tok_begin, tok_end);
    if (!call) return NULL;
    ((ast_call_t *)call)->callee = callee;
    ((ast_call_t *)call)->args = args;
    ast_node_t *last = args;
    while (last && last->next) last = last->next;
    ((ast_call_t *)call)->args_last = last;
    return call;
}

/* 单参 `__init__` 的隐式构造：目标类型 T 恰好一个初始化参数 P，源表达式
   类型（src_t）不是 T 且可隐式赋给 P 时，把 *slot 改写成
   `__clux_init_T(<原表达式>)`。返回新的 shadow（*converted = true）；
   不适用时返回 NULL/*converted = false。 */

/* 初始化表达式是否是**已存在值的位置**（变量 / 成员 / 下标 / 解引用 / 解包）。
   只有这种"把一个已有值复制进目标"的形态才受单参 __init__ 拦截：
   构造（`.T{...}`）与函数调用返回的是新值，初始化就是把它放进去。 */
static bool init_expr_is_place(ast_node_t *n) {
  if (!n) return false;
  switch (n->kind) {
    case AST_IDENT:
    case AST_MEMBER:
    case AST_INDEX:
    case AST_DEREF:
    case AST_UNWRAP:
    case AST_SCOPE_REF:
      return true;
    default:
      return false;
  }
}

value_t *sema_magic_init_convert(sema_t *sema, ast_node_t **slot,
                                 const type_t *src_t, const type_t *target_t,
                                 sema_scope_t *scope, bool *converted) {
    if (converted) *converted = false;
    if (!sema || !slot || !*slot || !target_t) return NULL;
    const type_t *core = type_strip_quals(target_t);
    if (!core || type_kind_of(core) != TYPE_KIND_STRUCT) return NULL;
    sema_symbol_t *ms = sema_magic_member(sema, core, "__init__");
    if (!ms || !ms->type || type_kind_of(ms->type) != TYPE_KIND_FUNC) return NULL;
    if (ms->is_generic) return NULL;
    if (func_type_param_count(ms->type) != 2) return NULL; /* 恰好 self + 1 */
    const type_t *src = src_t ? type_strip_quals(src_t) : NULL;
    /* 同类型但**不是**已有值的位置（显式构造 / 函数调用返回新值）：直接放入
       目标变量，不再构造一次；反之（`var b = a`）要继续走 __init__ 检查，
       这正是 move-only 类型禁止 copy 的位置。 */
    if (src && src == core && !init_expr_is_place(*slot)) return NULL;
    const type_t *pt = func_type_param(ms->type, 1);
    if (!pt) return NULL;
    value_t *probe = value_make_shadow(sema->vm, src ? src : sema->vm->type_void);
    if (!probe) return NULL;
    if (value_is_error(sema->vm, value_implicit_cast(sema->vm, probe, pt))) {
        /* 目标类型有单参 __init__：**所有**初始化位置（var / 实参 / 返回值 /
           字段）都走构造，源表达式必须能转换成构造的第二个形参。源类型与
           目标类型相同时也不退化成值拷贝——这正是 move-only 类型（Own /
           Fatal 之类）靠在参数上拒绝自身类型来禁止 copy 的方式。 */
        char tn[64], pn[64], sn[64];
        sema_type_name(core, tn, sizeof tn);
        sema_type_name(pt, pn, sizeof pn);
        if (src) sema_type_name(src, sn, sizeof sn);
        else snprintf(sn, sizeof sn, "void");
        location_t loc = sema_loc(sema, *slot);
        /* 合成节点（多遍 walk 里被改写的中间形态）不重复报：只对用户源码
           位置诊断一次。 */
        if ((*slot)->is_generated || loc.begin.line == 0) {
            if (converted) *converted = true;
            return value_make_error(sema->vm, "bad __init__ argument");
        }
        /* sema 会把同一段函数体走多遍：同一位置的同类诊断只报一次。 */
        bool dup = false;
        const diagnostic_t *items = diag_items(sema->diag);
        size_t nitems = diag_count(sema->diag);
        for (size_t i = 0; i < nitems; i++)
          if (items[i].message && items[i].loc.begin.line == loc.begin.line &&
              items[i].loc.begin.column == loc.begin.column &&
              strncmp(items[i].message, "cannot initialize ", 19) == 0) {
            dup = true;
            break;
          }
        if (!dup)
          diag_error(sema->diag, loc,
                     "cannot initialize %s with %s: %s::__init__ expects %s "
                     "(initialization always goes through __init__)",
                     tn, sn, tn, pn);
        if (converted) *converted = true;
        return value_make_error(sema->vm, "bad __init__ argument");
    }

    /* `.T{ x }` 的展开：调用生成的构造函数（参数是原表达式节点）。 */
    ast_node_t *arg = *slot;
    ast_node_t *call = sema_magic_init_call(sema, core, arg, arg->tok_begin,
                                            arg->tok_end);
    if (!call) return NULL;
    call->next = (*slot)->next;
    *slot = call;
    value_t *v = sema_expr(sema, slot, scope);
    if (converted) *converted = true;
    return v;
}