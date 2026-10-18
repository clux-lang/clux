#include "desugar/desugar.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/ffi_cond.h"  /* extern lib 的 assert(<cond>) 求值 */
#include "core/strslice.h"
#include "parser/ast_assign.h"
#include "parser/ast_binary.h"
#include "parser/ast_block.h"
#include "parser/ast_call.h"
#include "parser/ast_comptime_if.h"
#include "parser/ast_defer.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_extern_lib.h"
#include "parser/ast_for.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_foreach.h"
#include "parser/ast_func_def.h"
#include "parser/ast_if.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_return.h"
#include "parser/ast_switch.h"
#include "parser/ast_var_def.h"
#include "parser/ast_while.h"
#include "parser/ast_walk.h"
#include "parser/lexer.h" /* arena_token_symbol：合成运算符 token */

/* ===========================================================================
 * 前脱糖（front-end desugaring）——parser 之后、sema 之前的**纯 AST 变形**
 *
 * 这一层**不认识**语义概念：不知道"闭包"、"函数"、"返回值"、"作用域"，
 * 也不查符号表、不做类型检查。它只按表面的语法形状改写 AST。
 *
 * 已落地：`foreach` → 索引 for 循环（展开形状与理由见 include/desugar/
 * desugar.h）。
 *
 * 与之相对，**后脱糖**（defer -> 闭包 + 退出点调用）发生在 sema 里、且必须在
 * `comptime if` 裁剪**之后**（src/sema/stmt_build.c 的 desugar_defers）：
 * 它需要"函数 / 返回值 / 块作用域"这些语义结构，因此不属于本层。
 * =========================================================================== */

/* ---- 小工具 -------------------------------------------------------------- */

/* `<prefix>_<n>` → arena 持久化的 strslice（`__arr_3` / `__idx_3`）。
   `__` 前缀是源码层的**保留标识符**空间（parser 的 reserved-ident 规则），
   用户写不出同名变量，因此合成名不可能与用户变量撞名。 */
static strslice_t ds_name(desugar_t *ds, const char *prefix, uint32_t n) {
  char buf[64];
  int len = snprintf(buf, sizeof buf, "%s_%u", prefix, n);
  if (len <= 0 || (size_t)len >= sizeof buf) return STRSLICE_EMPTY;
  char *heap = (char *)arena_calloc(ds->arena, (size_t)len + 1, 1,
                                    ALIGNOF(max_align_t));
  if (!heap) return STRSLICE_EMPTY;
  memcpy(heap, buf, (size_t)len);
  return strslice_from_bytes(heap, (size_t)len);
}

/* 标识符节点。名字的生命周期由调用方保证：字面量名（`STRSLICE_LIT`）/
   arena 串（`ds_name`）/ 源 buffer 切片（用户的变量名）。 */
static ast_node_t *ds_ident(desugar_t *ds, uint32_t tb, uint32_t te,
                            strslice_t name) {
  if (!name.ptr || name.len == 0) return NULL;
  ast_node_t *n = ast_ident_new(ds->arena, tb, te);
  if (!n) return NULL;
  ((ast_ident_t *)n)->name = name;
  return n;
}

/* 运算符 token：arena 合成一个 SYMBOL token（与 parser 合成 `<<` / `>>` 同一
   机制——token_t 的字段对 parser 层私有，能合成的只有这条官方通道）。
   location 取 `at`（通常是源 foreach 节点的起点），于是即便诊断落在这个合成
   运算符上，也指向用户代码而不是 <desugar>。 */
static const token_t *ds_op(desugar_t *ds, const char *text,
                            const location_t *at) {
  if (!ds || !ds->arena || !text || !at) return NULL;
  return arena_token_symbol(ds->arena, text, at);
}

/* 整数字面量节点。**后缀必须归一化成平台宽度名**（u64 / u32）：
   compiler 按后缀名选 PUSH_U* 指令，`usize` 不是它认识的写法。与
   parse_int_lit 对 `Nusize` 的归一化同一口径（64 位 → u64，32 位 → u32）。 */
static ast_node_t *ds_int_lit(desugar_t *ds, uint32_t tb, uint32_t te,
                              uint64_t value) {
  ast_node_t *n = ast_int_lit_new(ds->arena, tb, te);
  if (!n) return NULL;
  ast_int_lit_t *lit = (ast_int_lit_t *)n;
  lit->value = value;
  lit->type = sizeof(void *) == 8 ? STRSLICE_LIT("u64") : STRSLICE_LIT("u32");
  return n;
}

/* `<object>[<index>]` 下标访问（indices 兄弟链单元素）。 */
static ast_node_t *ds_index(desugar_t *ds, uint32_t tb, uint32_t te,
                            ast_node_t *object, ast_node_t *index) {
  if (!object || !index) return NULL;
  ast_node_t *n = ast_index_new(ds->arena, tb, te);
  if (!n) return NULL;
  ast_index_t *ix = (ast_index_t *)n;
  ix->object = object;
  ix->indices = index;
  ix->indices_last = index;
  return n;
}

/* `lhs <op> rhs`。 */
static ast_node_t *ds_binary(desugar_t *ds, uint32_t tb, uint32_t te,
                             const token_t *op, ast_node_t *lhs,
                             ast_node_t *rhs) {
  if (!op || !lhs || !rhs) return NULL;
  ast_node_t *n = ast_binary_new(ds->arena, tb, te);
  if (!n) return NULL;
  ast_binary_t *b = (ast_binary_t *)n;
  b->op = op;
  b->lhs = lhs;
  b->rhs = rhs;
  return n;
}

/* `target <op> value`（语句位置上的赋值）。 */
static ast_node_t *ds_assign(desugar_t *ds, uint32_t tb, uint32_t te,
                             ast_node_t *target, const token_t *op,
                             ast_node_t *value) {
  if (!target || !op || !value) return NULL;
  ast_node_t *n = ast_assign_new(ds->arena, tb, te);
  if (!n) return NULL;
  ast_assign_t *a = (ast_assign_t *)n;
  a->target = target;
  a->op = op;
  a->value = value;
  return n;
}

/* 诊断定位：`at` 起始 token 的源码位置；拿不到时退化为 {0}。 */
static location_t ds_loc(desugar_t *ds, const ast_node_t *at) {
  location_t loc;
  memset(&loc, 0, sizeof loc);
  if (!at || !ds || !ds->tokens) return loc;
  const token_t *t =
      (const token_t *)vec_get(ds->tokens, (size_t)at->tok_begin);
  const location_t *tl = t ? token_get_location(t) : NULL;
  return tl ? *tl : loc;
}

static void ds_error(desugar_t *ds, const ast_node_t *at, const char *msg) {
  if (!ds || !ds->diag) return;
  diag_error(ds->diag, ds_loc(ds, at), "%s", msg);
}

/* ---- foreach 展开 --------------------------------------------------------
 *
 *   foreach ([var] a [: T] of b) { <body> }
 * ⇒
 *   {
 *       var __arr_N = b;
 *       for (var __idx_N: usize = 0usize;
 *            __idx_N < len(__arr_N);
 *            __idx_N = __idx_N + 1usize) {
 *           var a [: T] = __arr_N[__idx_N];   // `var`：每轮新声明
 *           // 或：a = __arr_N[__idx_N];      // 无 `var`：写回已定义变量
 *           <body>
 *       }
 *   }
 *
 * 返回替换用的 AST_BLOCK；失败返回 NULL（已诊断）。
 */
static ast_node_t *ds_expand_foreach(desugar_t *ds, ast_foreach_t *fe) {
  const uint32_t tb = fe->base.tok_begin;
  const uint32_t te = fe->base.tok_end;
  /* 迭代变量名 token 区间：元素绑定语句的诊断应指向 `a`，而不是 `foreach`。 */
  const uint32_t nb = fe->name_tok ? fe->name_tok : tb;
  const uint32_t ne = fe->name_tok ? fe->name_tok + 1 : te;

  if (!fe->body || fe->body->kind != AST_BLOCK || !fe->iterable ||
      !fe->name.ptr) {
    ds_error(ds, &fe->base, "malformed 'foreach' (missing iterable or body)");
    return NULL;
  }

  const location_t loc = ds_loc(ds, &fe->base);
  const uint32_t seq = ds->seq++;
  const strslice_t arr_name = ds_name(ds, "__arr", seq);
  const strslice_t idx_name = ds_name(ds, "__idx", seq);
  if (!idx_name.ptr) {
    ds_error(ds, &fe->base, "out of memory while desugaring 'foreach'");
    return NULL;
  }

  /* 迭代器是不是**裸标识符**（`foreach (x of v)`）：
   *
   *   是 → 条件与下标里各造一个**独立**的 `v` 引用，不引入临时变量：
   *        own 容器（move-only）因此可以遍历——`var __arr_N = v` 会被
   *        sema 判为"own 值不能拷贝初始化"（必须 move()/clone()），而
   *        `len(v)` / `v[i]` 只是读，合法。
   *   否 → 先求值到临时变量 `var __arr_N = <expr>`（语义 = 迭代器只求值
   *        一次，副作用不重复）。own 的**右值**（`make(...)`、返回 own 的
   *        调用）可以 move 进临时变量，因此同样支持。
   *
   * 不能直接复用 fe->iterable 节点：cond 与下标是**两个**位置，而 sema 会
   * 就地改写表达式节点（IDENT → FUNC_REF / TYPE_REF 折叠），共享同一节点等于
   * 让两处共用一份可变状态。裸标识符按名字重建节点最干净。 */
  const bool direct = fe->iterable->kind == AST_IDENT &&
                      ((ast_ident_t *)fe->iterable)->name.ptr != NULL;
  const strslice_t iter_name =
      direct ? ((ast_ident_t *)fe->iterable)->name : arr_name;

  /* ---- 非裸标识符：var __arr_N = <iterable>; ---- */
  ast_node_t *arr_def = NULL;
  if (!direct) {
    if (!arr_name.ptr) {
      ds_error(ds, &fe->base, "out of memory while desugaring 'foreach'");
      return NULL;
    }
    arr_def = ast_var_def_new(ds->arena, tb, te);
    if (!arr_def) goto oom;
    ((ast_var_def_t *)arr_def)->name = arr_name;
    ((ast_var_def_t *)arr_def)->init = fe->iterable;
  }

  /* ---- var __idx_N: usize = 0usize; （for 的 init） ---- */
  ast_node_t *idx_def = ast_var_def_new(ds->arena, tb, te);
  ast_node_t *usize_t = ds_ident(ds, tb, te, STRSLICE_LIT("usize"));
  ast_node_t *zero = ds_int_lit(ds, tb, te, 0);
  if (!idx_def || !usize_t || !zero) goto oom;
  ((ast_var_def_t *)idx_def)->name = idx_name;
  ((ast_var_def_t *)idx_def)->type_expr = usize_t;
  ((ast_var_def_t *)idx_def)->init = zero;

  /* ---- cond: __idx_N < len(<iter>) ---- */
  const token_t *op_lt = ds_op(ds, "<", &loc);
  ast_node_t *len_fn = ds_ident(ds, tb, te, STRSLICE_LIT("len"));
  ast_node_t *arr_arg = ds_ident(ds, tb, te, iter_name);
  ast_node_t *len_call = ast_call_new(ds->arena, tb, te);
  ast_node_t *cond_lhs = ds_ident(ds, tb, te, idx_name);
  if (!op_lt || !len_fn || !arr_arg || !len_call || !cond_lhs) goto oom;
  {
    ast_call_t *cl = (ast_call_t *)len_call;
    cl->callee = len_fn;
    cl->args = arr_arg;
    cl->args_last = arr_arg;
  }
  ast_node_t *cond = ds_binary(ds, tb, te, op_lt, cond_lhs, len_call);
  if (!cond) goto oom;

  /* ---- update: __idx_N = __idx_N + 1usize ---- */
  const token_t *op_add = ds_op(ds, "+", &loc);
  const token_t *op_eq = ds_op(ds, "=", &loc);
  ast_node_t *one = ds_int_lit(ds, tb, te, 1);
  ast_node_t *inc_lhs = ds_ident(ds, tb, te, idx_name);
  ast_node_t *upd_target = ds_ident(ds, tb, te, idx_name);
  if (!op_add || !op_eq || !one || !inc_lhs || !upd_target) goto oom;
  ast_node_t *sum = ds_binary(ds, tb, te, op_add, inc_lhs, one);
  if (!sum) goto oom;
  ast_node_t *update = ds_assign(ds, tb, te, upd_target, op_eq, sum);
  if (!update) goto oom;

  /* ---- 元素绑定：<iter>[__idx_N] → `var a [: T] = …` 或 `a = …` ---- */
  ast_node_t *elem_arr = ds_ident(ds, tb, te, iter_name);
  ast_node_t *elem_idx = ds_ident(ds, tb, te, idx_name);
  ast_node_t *elem = ds_index(ds, tb, te, elem_arr, elem_idx);
  if (!elem) goto oom;

  ast_node_t *bind = NULL;
  if (fe->is_decl) {
    ast_node_t *vd = ast_var_def_new(ds->arena, nb, ne);
    if (!vd) goto oom;
    ast_var_def_t *v = (ast_var_def_t *)vd;
    v->name = fe->name;
    v->type_expr = fe->type_expr;
    v->init = elem;
    /* 所有权编码必须在**类型槽位折叠前**取出（与 parse_var_def 同一口径：
       sema 的 sema_resolve_type_slot 会把 AST_PTR / AST_SLICE_TYPE 折叠成
       AST_TYPE_REF，之后就取不到 ownership token 了）。 */
    v->ownership = ast_type_expr_ownership_code(fe->type_expr);
    bind = vd;
  } else {
    const token_t *op_set = ds_op(ds, "=", &loc);
    ast_node_t *target = ds_ident(ds, nb, ne, fe->name);
    if (!op_set || !target) goto oom;
    bind = ds_assign(ds, nb, ne, target, op_set, elem);
    if (!bind) goto oom;
  }

  /* 绑定语句插到循环体**首位**：每轮先绑定当前元素，再执行用户语句。
     复用原 body 块（不改写其 token 区间），于是 body 内语句的 parent /
     兄弟链保持原样；stmt_last 只在 body 为空时才需要补。 */
  {
    ast_block_t *body = (ast_block_t *)fe->body;
    bind->next = body->stmts;
    body->stmts = bind;
    if (!body->stmts_last) body->stmts_last = bind;
  }

  /* ---- for (init; cond; update) <body> ---- */
  ast_node_t *for_node = ast_for_new(ds->arena, tb, te);
  if (!for_node) goto oom;
  {
    ast_for_t *fo = (ast_for_t *)for_node;
    fo->init = idx_def;
    fo->cond = cond;
    fo->update = update;
    fo->body = fe->body;
  }

  /* ---- 收尾：需要临时变量时包一层块把 `__arr_N` 圈在循环内 ---- */
  if (!arr_def) return for_node;
  {
    ast_node_t *outer = ast_block_new(ds->arena, tb, te);
    if (!outer) goto oom;
    ast_block_t *blk = (ast_block_t *)outer;
    arr_def->next = for_node;
    blk->stmts = arr_def;
    blk->stmts_last = for_node;
    return outer;
  }

oom:
  ds_error(ds, &fe->base, "out of memory while desugaring 'foreach'");
  return NULL;
}

/* ---- AST 遍历 ------------------------------------------------------------
 *
 * 语句层与表达式层分开走：
 *   - 语句层要知道"块 / 分支 / 循环体的语句链在哪"（ast_visit_children 不覆盖
 *     语句层，见 ast_walk.h 的范围说明）；
 *   - 表达式层复用 ast_visit_children / ast_walk_preorder，只额外**下钻函数
 *     字面量**（AST_FUNC_DEF 出现在表达式位置，例如 `var f = func() { … };`）。
 * ------------------------------------------------------------------------- */

static void ds_walk_stmt(desugar_t *ds, ast_node_t *stmt);
static void ds_walk_expr(desugar_t *ds, ast_node_t *expr);
static void ds_walk_func(desugar_t *ds, ast_func_def_t *fn);

/* 变量定义（含闭包捕获 / 函数形参 / comptime var）：类型槽位与初始化式都
   可能含函数字面量（而函数字面量体里又可能有 foreach）。 */
static void ds_walk_var_def(desugar_t *ds, ast_var_def_t *v) {
  if (!v) return;
  ds_walk_expr(ds, v->type_expr);
  ds_walk_expr(ds, v->init);
}

static ast_walk_action_t ds_expr_visit(ast_node_t *n, void *user) {
  desugar_t *ds = (desugar_t *)user;
  if (n && n->kind == AST_FUNC_DEF) ds_walk_func(ds, (ast_func_def_t *)n);
  return AST_WALK_CONTINUE;
}

static void ds_walk_expr(desugar_t *ds, ast_node_t *expr) {
  if (!expr) return;
  ast_walk_preorder(expr, ds_expr_visit, ds);
}

static void ds_walk_func(desugar_t *ds, ast_func_def_t *fn) {
  if (!fn) return;
  for (ast_node_t *c = fn->captures; c; c = c->next)
    ds_walk_var_def(ds, (ast_var_def_t *)c);
  for (ast_node_t *p = fn->params; p; p = p->next)
    ds_walk_var_def(ds, (ast_var_def_t *)p);
  ds_walk_expr(ds, fn->return_expr);
  ds_walk_stmt(ds, fn->body);
}

/* 遍历一条语句链，把链上的 AST_FOREACH 就地替换成展开后的块。
 * `owner` 是链的所属节点（块），只用于设置新节点的 parent。
 * 返回后 *tail_out 指向链尾（让块节点的 stmts_last 保持一致）。 */
static void ds_walk_stmt_list(desugar_t *ds, ast_node_t *owner,
                              ast_node_t **head, ast_node_t **tail_out) {
  ast_node_t *last = NULL;
  ast_node_t **cur = head;
  while (*cur) {
    ast_node_t *s = *cur;

    if (s->kind == AST_FOREACH) {
      ast_node_t *repl = ds_expand_foreach(ds, (ast_foreach_t *)s);
      if (!repl) {
        /* 已诊断（或静默模式）：丢弃该语句，别让 sema 见到 AST_FOREACH。 */
        *cur = s->next;
        continue;
      }
      repl->parent = owner;
      repl->next = s->next; /* 接上 foreach 之后的语句 */
      *cur = repl;
      /* 展开体里可能还有 foreach（用户写了嵌套 foreach）→ 继续下钻。 */
      ds_walk_stmt(ds, repl);
      last = repl;
      cur = &repl->next;
      continue;
    }

    ds_walk_stmt(ds, s);
    last = s;
    cur = &s->next;
  }
  if (tail_out) *tail_out = last;
}

static void ds_walk_switch(desugar_t *ds, ast_switch_t *sw) {
  ds_walk_expr(ds, sw->cond);
  for (ast_node_t *c = sw->cases; c; c = c->next) {
    if (c->kind != AST_SWITCH_CASE) continue;
    ast_switch_case_t *sc = (ast_switch_case_t *)c;
    ds_walk_expr(ds, sc->patterns); /* 模式表达式兄弟链 */
    ds_walk_stmt(ds, sc->body);
  }
  ds_walk_stmt(ds, sw->default_body);
}

static void ds_walk_stmt(desugar_t *ds, ast_node_t *stmt) {
  if (!stmt) return;
  switch (stmt->kind) {
    case AST_BLOCK: {
      ast_block_t *b = (ast_block_t *)stmt;
      ds_walk_stmt_list(ds, stmt, &b->stmts, &b->stmts_last);
      break;
    }
    case AST_IF: {
      ast_if_t *n = (ast_if_t *)stmt;
      ds_walk_expr(ds, n->cond);
      ds_walk_stmt(ds, n->then_body);
      ds_walk_stmt(ds, n->else_body); /* AST_BLOCK / else-if 的 AST_IF */
      break;
    }
    case AST_COMPTIME_IF: {
      ast_comptime_if_t *n = (ast_comptime_if_t *)stmt;
      ds_walk_expr(ds, n->cond);
      ds_walk_stmt(ds, n->then_body);
      ds_walk_stmt(ds, n->else_body); /* AST_BLOCK / else-if */
      break;
    }
    case AST_WHILE: {
      ast_while_t *n = (ast_while_t *)stmt;
      ds_walk_expr(ds, n->cond);
      ds_walk_stmt(ds, n->body);
      break;
    }
    case AST_DOWHILE: {
      ast_dowhile_t *n = (ast_dowhile_t *)stmt;
      ds_walk_stmt(ds, n->body);
      ds_walk_expr(ds, n->cond);
      break;
    }
    case AST_FOR: {
      ast_for_t *n = (ast_for_t *)stmt;
      ds_walk_stmt(ds, n->init); /* AST_VAR_DEF / AST_ASSIGN / AST_EXPR_STMT */
      ds_walk_expr(ds, n->cond);
      ds_walk_expr(ds, n->update);
      ds_walk_stmt(ds, n->body);
      break;
    }
    case AST_SWITCH:
      ds_walk_switch(ds, (ast_switch_t *)stmt);
      break;
    case AST_EXPR_STMT:
      ds_walk_expr(ds, ((ast_expr_stmt_t *)stmt)->expr);
      break;
    case AST_VAR_DEF:
      ds_walk_var_def(ds, (ast_var_def_t *)stmt);
      break;
    case AST_ASSIGN: {
      ast_assign_t *n = (ast_assign_t *)stmt;
      ds_walk_expr(ds, n->target);
      ds_walk_expr(ds, n->value);
      ds_walk_stmt(ds, n->destroy); /* sema 之后才可能非空，防御性覆盖 */
      break;
    }
    case AST_RETURN:
      ds_walk_expr(ds, ((ast_return_t *)stmt)->value);
      break;
    case AST_DEFER:
      /* defer 的闭包体是独立函数作用域：块内的 foreach 同样要展开。 */
      ds_walk_func(ds, ((ast_defer_t *)stmt)->closure);
      break;
    case AST_FUNC_DEF:
      ds_walk_func(ds, (ast_func_def_t *)stmt);
      break;
    case AST_FOREACH:
      /* 只应在语句链上出现（由 ds_walk_stmt_list 替换）。孤立节点无链可换，
         保持原样（由 sema 报"未知语句"或根本不出现）。 */
      break;
    default:
      break;
  }
}

/* ===========================================================================
 * extern lib 的条件裁剪（前脱糖）
 *
 * `extern lib crt from "msvcrt" assert(OS_TYPE == "WIN64" || OS_TYPE == "WIN32");`
 * 这类声明按**编译期注入的全局键表**（core/ffi_cond.h）判定：条件为假 → 整条
 * 声明从函数/全局链表里摘除，sema/driver/容器都看不到它。于是"各平台各写一条
 * 同别名声明"天然不冲突（只有一条存活）。
 *
 * 为什么放在前脱糖：这是**纯 AST 变形**（不查符号表、不做类型检查），与
 * `foreach` 展开同一层；放这里的好处是后续所有阶段（sema 的别名登记、driver
 * 的 dlopen 与 dlsym 校验、.cxb/.cxs 的 FFI 段）都不用知道条件存在。
 * =========================================================================== */

/** 摘除条件为假（或求值失败）的 AST_EXTERN_LIB 节点。 */
static void ds_prune_extern_libs(desugar_t *ds, ast_program_t *prog) {
  ast_node_t **link = &prog->funcs;
  while (*link) {
    ast_node_t *n = *link;
    if (n->kind != AST_EXTERN_LIB) {
      link = &n->next;
      continue;
    }
    ast_extern_lib_t *el = (ast_extern_lib_t *)n;
    if (el->cond.len == 0) { /* 无条件：永远生效 */
      link = &n->next;
      continue;
    }
    char cond[512];
    size_t clen = el->cond.len < sizeof(cond) - 1 ? el->cond.len : sizeof(cond) - 1;
    memcpy(cond, el->cond.ptr, clen);
    cond[clen] = '\0';

    bool keep = true;
    char err[256] = {0};
    if (!ffi_cond_eval(cond, &keep, err, sizeof err)) {
      ds_error(ds, n, err);
      keep = false;
    }
    if (keep) {
      link = &n->next;
      continue;
    }
    *link = n->next; /* 裁剪：后续阶段完全看不到这条声明 */
  }
}


/* ===========================================================================
 * 装饰器展开（`[[<expr>]]`，前脱糖）
 *
 * 装饰器是**编译期函数**：接收"被装饰的值"，返回"装饰后的值"。
 *
 *   [[ann]] var a = 123;                    ⇒  var a = ann(123);
 *   [[ann]] func f(...): T { body }         ⇒  func __decor_N_f(...): T { body }
 *                                              var f = ann(__decor_N_f);
 *   struct S { [[ann]] func m(self: ref *S, ...) { body } }
 *                                           ⇒  struct S { func m(...) { return __decorval_N_m(self, ...); } }
 *                                              func __decorsrc_N_m(self: ref *S, ...) { body }
 *                                              var __decorval_N_m = ann(__decorsrc_N_m);
 *
 * 设计要点：
 *   - **装饰器必须是 comptime 函数**：展开结果 `var f = ann(<源>)` 是全局变量的
 *     编译期常量初始化器（语言契约：全局 init 必须可折）。这直接来自
 *     "函数是编译期的值"（comptime 函数能返回函数/闭包）。
 *   - 应用顺序与 Python 一致：`[[a]] [[b]] f` ⇒ `a(b(f))`（源码里靠前的先包）。
 *   - 顶层函数**保持函数身份**：`f` 仍是函数（递归、取函数值都正常），装饰只发生
 *     一次（模块初始化期折叠成常量闭包），不像"每次调用重新装饰"那样破坏状态。
 *   - 方法必须留在**方法表**里（`obj.m()` 走 MEMBER_CALL），所以成员变成"包装
 *     方法"（转发到模块级装饰值），原方法体搬到模块级源函数。
 *   - 合成名一律 `__` 前缀（保留给编译器生成的名字，用户不可声明）。
 * =========================================================================== */

/** 合成保留名：`__<kind>_<seq>_<原名>`（arena 拥有）。 */
static strslice_t ds_synth_name(desugar_t *ds, const char *kind,
                                strslice_t orig) {
  char buf[256];
  int n = snprintf(buf, sizeof buf, "__%s_%u_%.*s", kind, ++ds->seq,
                   (int)orig.len, orig.ptr ? orig.ptr : "");
  if (n < 0) n = 0;
  if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
  char *p = (char *)arena_alloc(ds->arena, (size_t)n + 1, 1);
  if (!p) return orig;
  memcpy(p, buf, (size_t)n + 1);
  return strslice_from_bytes(p, (size_t)n);
}

static ast_node_t *ds_new_ident(desugar_t *ds, strslice_t name, uint32_t tb,
                                uint32_t te) {
  ast_node_t *n = ast_ident_new(ds->arena, tb, te);
  if (n) ((ast_ident_t *)n)->name = name;
  return n;
}

static ast_node_t *ds_call(desugar_t *ds, ast_node_t *callee, ast_node_t *args,
                           ast_node_t *args_last, uint32_t tb, uint32_t te) {
  ast_node_t *n = ast_call_new(ds->arena, tb, te);
  if (!n) return NULL;
  ast_call_t *c = (ast_call_t *)n;
  c->callee = callee;
  c->args = args;
  c->args_last = args_last;
  return n;
}

/** 把装饰器链按"源码序从内到外"包在 `value` 上：[[a]] [[b]] v ⇒ a(b(v))。 */
static ast_node_t *ds_apply_decorators(desugar_t *ds, ast_node_t *decs,
                                       ast_node_t *value, uint32_t tb,
                                       uint32_t te) {
  size_t n = 0;
  for (ast_node_t *d = decs; d; d = d->next) n++;
  if (n == 0) return value;
  ast_node_t **arr = (ast_node_t **)arena_calloc(
      ds->arena, n, sizeof(ast_node_t *), ALIGNOF(max_align_t));
  if (!arr) return value;
  size_t i = 0;
  for (ast_node_t *d = decs; d; d = d->next) arr[i++] = d;
  value->next = NULL;
  for (i = n; i > 0; i--) {
    ast_node_t *callee = arr[i - 1];
    callee->next = NULL;
    ast_node_t *call = ds_call(ds, callee, value, value, tb, te);
    if (!call) return value;
    value->next = NULL;
    value = call;
  }
  return value;
}

/** 变量装饰：`[[ann]] var a = v;` ⇒ `var a = ann(v);`。 */
static void ds_decorate_var(desugar_t *ds, ast_var_def_t *vd) {
  ast_node_t *decs = vd->decorators;
  if (!decs) return;
  vd->decorators = NULL;
  if (!vd->init || vd->init->kind == AST_UNDEF) {
    ds_error(ds, (ast_node_t *)vd,
             "decorated declaration must have an initializer");
    return;
  }
  vd->init = ds_apply_decorators(ds, decs, vd->init, vd->base.tok_begin,
                                 vd->base.tok_end);
}

/** 包装体：`return <callee>(<参数们>);`（void 返回时用表达式语句）。 */
static ast_node_t *ds_forward_body(desugar_t *ds, ast_func_def_t *fn,
                                   strslice_t callee_name) {
  uint32_t tb = fn->base.tok_begin, te = fn->base.tok_end;
  ast_node_t *args = NULL, *args_last = NULL;
  for (ast_node_t *p = fn->params; p; p = p->next) {
    ast_var_def_t *pd = (ast_var_def_t *)p;
    ast_node_t *a = ds_new_ident(ds, pd->name, tb, te);
    if (!a) return NULL;
    if (args_last)
      args_last->next = a;
    else
      args = a;
    args_last = a;
  }
  ast_node_t *callee = ds_new_ident(ds, callee_name, tb, te);
  ast_node_t *call = ds_call(ds, callee, args, args_last, tb, te);
  if (!call) return NULL;
  ast_node_t *stmt = NULL;
  if (fn->return_expr) {
    stmt = ast_return_new(ds->arena, tb, te);
    if (stmt) ((ast_return_t *)stmt)->value = call;
  } else {
    stmt = ast_expr_stmt_new(ds->arena, tb, te);
    if (stmt) ((ast_expr_stmt_t *)stmt)->expr = call;
  }
  if (!stmt) return NULL;
  ast_node_t *blk = ast_block_new(ds->arena, tb, te);
  if (!blk) return NULL;
  ((ast_block_t *)blk)->stmts = stmt;
  ((ast_block_t *)blk)->stmts_last = stmt;
  return blk;
}

/** 顶层函数装饰：源函数改名 + `var <名> = 装饰(<源>);` 紧随其后。 */
static ast_node_t *ds_decorate_top_func(desugar_t *ds, ast_func_def_t *fn) {
  ast_node_t *decs = fn->decorators;
  if (!decs) return NULL;
  fn->decorators = NULL;
  uint32_t tb = fn->base.tok_begin, te = fn->base.tok_end;
  strslice_t visible = fn->name;
  strslice_t hidden = ds_synth_name(ds, "decor", visible);
  fn->name = hidden;

  ast_node_t *src = ds_new_ident(ds, hidden, tb, te);
  if (!src) return NULL;
  ast_node_t *init = ds_apply_decorators(ds, decs, src, tb, te);
  ast_node_t *var = ast_var_def_new(ds->arena, tb, te);
  if (!var) return NULL;
  ((ast_var_def_t *)var)->name = visible;
  ((ast_var_def_t *)var)->init = init;
  return var;
}

/** 方法装饰：成员变包装方法；源函数 + 装饰值放模块级（插在 struct 之后）。 */
static ast_node_t *ds_decorate_method(desugar_t *ds, ast_func_def_t *m,
                                      ast_node_t **out_tail) {
  ast_node_t *decs = m->decorators;
  if (!decs) return NULL;
  m->decorators = NULL;
  uint32_t tb = m->base.tok_begin, te = m->base.tok_end;
  strslice_t visible = m->name;
  strslice_t src_name = ds_synth_name(ds, "decorsrc", visible);
  strslice_t val_name = ds_synth_name(ds, "decorval", visible);

  /* 1) 模块级源函数：克隆成员（共享参数 / 返回类型 / 函数体），改名。 */
  ast_func_def_t *src = (ast_func_def_t *)arena_calloc(
      ds->arena, 1, sizeof(ast_func_def_t), ALIGNOF(max_align_t));
  if (!src) return NULL;
  *src = *m;
  src->base.next = NULL;
  src->name = src_name;
  src->decorators = NULL;
  src->fid = 0;
  src->sig_id = 0;
  src->capture_type_id = 0;
  src->env_dispose_fid = 0;
  src->env_clone_fid = 0;
  src->is_comptime = false;

  /* 2) 成员变成包装方法：`return __decorval(self, ...);` */
  m->body = ds_forward_body(ds, m, val_name);
  if (!m->body) return NULL;

  /* 3) `var __decorval = 装饰(__decorsrc);` */
  ast_node_t *ident = ds_new_ident(ds, src_name, tb, te);
  if (!ident) return NULL;
  ast_node_t *init = ds_apply_decorators(ds, decs, ident, tb, te);
  ast_node_t *var = ast_var_def_new(ds->arena, tb, te);
  if (!var) return NULL;
  ((ast_var_def_t *)var)->name = val_name;
  ((ast_var_def_t *)var)->init = init;

  /* 链：src → var（源函数在前，var 依赖它折叠） */
  src->base.next = var;
  *out_tail = var;
  return &src->base;
}

/** 遍历全部顶层声明，展开装饰器（struct 成员里的方法/属性同样处理）。 */
static void ds_expand_decorators(desugar_t *ds, ast_program_t *prog) {
  ast_node_t **link = &prog->funcs;
  while (*link) {
    ast_node_t *n = *link;
    if (n->kind == AST_VAR_DEF) {
      ds_decorate_var(ds, (ast_var_def_t *)n);
    } else if (n->kind == AST_FUNC_DEF) {
      ast_node_t *var = ds_decorate_top_func(ds, (ast_func_def_t *)n);
      if (var) {
        var->next = n->next;
        n->next = var;
      }
    } else if (n->kind == AST_STRUCT_DEF) {
      ast_struct_def_t *st = (ast_struct_def_t *)n;
      ast_node_t *gen_head = NULL, *gen_tail = NULL;
      for (ast_node_t *m = st->members; m; m = m->next) {
        if (m->kind == AST_VAR_DEF) {
          ds_decorate_var(ds, (ast_var_def_t *)m);
        } else if (m->kind == AST_FUNC_DEF) {
          ast_node_t *tail = NULL;
          ast_node_t *head =
              ds_decorate_method(ds, (ast_func_def_t *)m, &tail);
          if (head) {
            if (gen_tail)
              gen_tail->next = head;
            else
              gen_head = head;
            gen_tail = tail;
          }
        }
      }
      if (gen_head) {
        gen_tail->next = n->next;
        n->next = gen_head;
      }
    }
    link = &(*link)->next;
  }
}

void desugar_program(desugar_t *ds, ast_program_t *prog) {
  if (!ds || !prog) return;
  /* 先展开装饰器（纯语法改写：装饰器链包成调用），再裁剪 extern lib 条件 */
  ds_expand_decorators(ds, prog);
  ds_prune_extern_libs(ds, prog);
  for (ast_node_t *n = prog->funcs; n; n = n->next) {
    switch (n->kind) {
      case AST_FUNC_DEF:
        ds_walk_func(ds, (ast_func_def_t *)n);
        break;
      case AST_VAR_DEF: /* 全局 var / comptime var：初始化式里可能有闭包 */
        ds_walk_var_def(ds, (ast_var_def_t *)n);
        break;
      default:
        /* import / reexport / struct / enum / type 定义等：无语句体。 */
        break;
    }
  }
}
