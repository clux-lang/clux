#include "desugar/desugar.h"

#include <stdio.h>
#include <string.h>

#include "core/panic.h"
#include "parser/ast_block.h"
#include "parser/ast_if.h"
#include "parser/ast_comptime_if.h"
#include "parser/ast_while.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_for.h"
#include "parser/ast_switch.h"
#include "parser/ast_defer.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ident.h"
#include "parser/ast_call.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_var_def.h"
#include "parser/ast_assign.h"
#include "parser/ast_return.h"
#include "parser/ast_walk.h"

/* ===========================================================================
 * 前脱糖（parser → sema 之间的纯 AST 变换）
 *
 * 这一层**不看符号表、不做类型检查**，只按 AST 的嵌套结构改写。
 * 当前唯一变换：defer → 匿名局部函数 + 各退出点的逆序调用。
 *
 * 实现要点（对照原来在 sema Pass 3a 里的版本）：
 *   - 原实现沿 sema 作用域链收集 defers（`scope->defers` + parent 链），
 *     本质是在用**语义层的作用域树**当"块嵌套结构"用；
 *   - 这里改用自带的**块栈**（`groups[]`）与**循环栈**（`loop_top[]`），
 *     完全等价但不需要 sema；
 *   - break/continue 的收集范围精确到"最近的循环体块"（原实现因缺少循环
 *     scope 标记而保守地退化为函数根）——这一层反而是更精确的实现。
 * =========================================================================== */

#define DS_MAX_GROUPS 256
#define DS_MAX_LOOPS  32

typedef struct ds_group {
  ast_node_t *entries[DS_MAX_GROUPS]; /* AST_FUNC_DEF（defer 闭包） */
  size_t      n;
} ds_group_t;

typedef struct ds_ctx {
  desugar_t *ds;
  ds_group_t groups[DS_MAX_GROUPS];
  size_t     depth;                 /* groups[] 栈顶（下一个空位） */
  size_t     loop_top[DS_MAX_LOOPS];/* 循环体块的 group 下标 */
  size_t     loop_n;
  size_t     func_base;             /* 当前函数体块的 group 下标（return 边界） */
  bool       in_comptime_branch;    /* 在 comptime if 分支内（禁止 defer） */
} ds_ctx_t;

static void ds_func(ds_ctx_t *ctx, ast_func_def_t *fn);
static void ds_block(ds_ctx_t *ctx, ast_block_t *block);

/* 诊断：位置来自 token pool（与 parser 同一套） */
static void ds_error(desugar_t *ds, ast_node_t *node, const char *msg) {
  if (!ds || !ds->diag || !node) return;
  location_t loc = {0};
  if (ds->tokens) {
    const token_t *tk = (const token_t *)vec_get(ds->tokens, node->tok_begin);
    const location_t *l = tk ? token_get_location(tk) : NULL;
    if (l) loc = *l;
  }
  diag_error(ds->diag, loc, "%s", msg);
}

/* 构造 `__defer_N()` 语句（AST_EXPR_STMT(AST_CALL(IDENT))）。
   tok_begin = 0 标记为编译器生成节点：sema 的 unreachable 检查 / 3b walk
   都跳过这类节点（它们是退出点清理代码，不是用户语句）。 */
static ast_node_t *ds_make_call(ds_ctx_t *ctx, const char *name) {
  arena_t *a = ctx->ds->arena;
  ast_node_t *ident = ast_ident_new(a, 0, 0);
  if (!ident) return NULL;
  size_t nlen = strlen(name);
  char *buf = (char *)arena_calloc(a, nlen + 1, 1, ALIGNOF(max_align_t));
  if (!buf) return NULL;
  memcpy(buf, name, nlen);
  ((ast_ident_t *)ident)->name = strslice_from_bytes(buf, nlen);

  ast_node_t *call = ast_call_new(a, 0, 0);
  if (!call) return NULL;
  ((ast_call_t *)call)->callee = ident;
  ((ast_call_t *)call)->args = NULL;
  ((ast_call_t *)call)->args_last = NULL;

  ast_node_t *stmt = ast_expr_stmt_new(a, 0, 0);
  if (!stmt) return NULL;
  ((ast_expr_stmt_t *)stmt)->expr = call;
  return stmt;
}

/* 把一个 defer 闭包的名字读成 C 串（名字是 arena NUL 终止串） */
static const char *ds_group_name(ds_ctx_t *ctx, const ast_node_t *entry) {
  (void)ctx;
  return ((const ast_func_def_t *)entry)->name.ptr;
}

/* 收集 groups[from .. to) 的逆序调用链（LIFO：后定义先调用）。
   from = 内层下标（含），to = 外层边界下标（含）。
   返回链首；无内容返回 NULL。 */
static ast_node_t *ds_collect_calls(ds_ctx_t *ctx, size_t inner, size_t outer) {
  ast_node_t *head = NULL, *tail = NULL;
  /* 从最内层向外层，每层内部从后往前 → 全局 LIFO */
  for (size_t g = inner + 1; g-- > outer;) {
    ds_group_t *grp = &ctx->groups[g];
    for (size_t i = grp->n; i-- > 0;) {
      const char *name = ds_group_name(ctx, grp->entries[i]);
      if (!name) continue;
      ast_node_t *call = ds_make_call(ctx, name);
      if (!call) continue;
      if (!head) { head = tail = call; }
      else { tail->next = call; tail = call; }
    }
  }
  return head;
}

/* 表达式里嵌套的**函数字面量**的 body 也要脱糖（defer 可出现在其中）。
   两段式：先用 ast_walk 收集（不改结构），再逐个脱糖——避免在遍历回调里
   改动正在遍历的树。 */
typedef struct ds_lit_buf {
  ast_func_def_t *items[64];
  size_t          n;
} ds_lit_buf_t;

static ast_walk_action_t ds_collect_literals(ast_node_t *n, void *user) {
  ds_lit_buf_t *b = (ds_lit_buf_t *)user;
  if (n && n->kind == AST_FUNC_DEF && b->n < 64)
    b->items[b->n++] = (ast_func_def_t *)n;
  return AST_WALK_CONTINUE;
}

static void ds_expr_literals(ds_ctx_t *ctx, ast_node_t *expr) {
  if (!expr) return;
  ds_lit_buf_t buf;
  buf.n = 0;
  (void)ast_walk_preorder(expr, ds_collect_literals, &buf);
  for (size_t i = 0; i < buf.n; i++) ds_func(ctx, buf.items[i]);
}

/* 语句里可能的表达式槽位（局部函数字面量可能在其中）：
   var 定义 init / 表达式语句 / return 值 / 赋值 rhs / if 条件 … */
static void ds_stmt_exprs(ds_ctx_t *ctx, ast_node_t *s) {
  switch (s->kind) {
  case AST_VAR_DEF:
    ds_expr_literals(ctx, ((ast_var_def_t *)s)->init);
    break;
  case AST_EXPR_STMT:
    ds_expr_literals(ctx, ((ast_expr_stmt_t *)s)->expr);
    break;
  case AST_ASSIGN:
    ds_expr_literals(ctx, ((ast_assign_t *)s)->value);
    break;
  case AST_RETURN:
    ds_expr_literals(ctx, ((ast_return_t *)s)->value);
    break;
  default:
    break;
  }
}

/* 循环体：进入时登记循环边界 = 即将建立的 group 下标（ds_block 会 push），
   退出时恢复。break/continue 只收集到该边界（含）。 */
static void ds_loop_body(ds_ctx_t *ctx, ast_node_t *body) {
  size_t boundary = ctx->depth; /* ds_block 将 push 到该下标 */
  if (ctx->loop_n < DS_MAX_LOOPS) ctx->loop_top[ctx->loop_n++] = boundary;
  if (body) {
    if (body->kind == AST_BLOCK) ds_block(ctx, (ast_block_t *)body);
    else ds_expr_literals(ctx, body); /* 防御：非块体（语法上不应出现） */
  }
  if (ctx->loop_n > 0) ctx->loop_n--;
}

static void ds_block(ds_ctx_t *ctx, ast_block_t *block) {
  if (!block) return;
  if (ctx->depth >= DS_MAX_GROUPS) {
    /* 嵌套过深：放弃脱糖（防御；正常情况下语法嵌套远小于 64） */
    return;
  }
  size_t my = ctx->depth++;
  ctx->groups[my].n = 0;

  ast_node_t *prev = NULL;
  ast_node_t *s = block->stmts;
  while (s) {
    ast_node_t *next = s->next;

    switch (s->kind) {
    case AST_DEFER: {
      ast_defer_t *d = (ast_defer_t *)s;
      ast_func_def_t *fn = d->closure;
      if (!fn) break;
      if (ctx->in_comptime_branch) {
        ds_error(ctx->ds, s,
                 "'defer' cannot be used inside a 'comptime if' branch "
                 "(the branch may be pruned; write the defer outside)");
        break;
      }
      /* 设名 __defer_N（arena NUL 终止串） */
      char nb[64];
      snprintf(nb, sizeof nb, "__defer_%u", ctx->ds->defer_seq++);
      size_t nlen = strlen(nb);
      char *name_buf = (char *)arena_calloc(ctx->ds->arena, nlen + 1, 1,
                                            ALIGNOF(max_align_t));
      if (!name_buf) panic("desugar: out of memory naming defer closure");
      memcpy(name_buf, nb, nlen);
      fn->name = strslice_from_bytes(name_buf, nlen);

      /* 语句链替换：AST_DEFER → 闭包（AST_FUNC_DEF） */
      fn->base.next = next;
      if (prev) prev->next = &fn->base;
      else block->stmts = &fn->base;
      prev = &fn->base;

      /* 登记进本块 group（块尾/退出点收集） */
      ds_group_t *grp = &ctx->groups[my];
      if (grp->n < DS_MAX_GROUPS) grp->entries[grp->n++] = &fn->base;

      /* defer 闭包自己的 body 也要脱糖 */
      ds_func(ctx, fn);
      s = &fn->base;
      s = s->next;
      continue;
    }
    case AST_RETURN:
    case AST_BREAK:
    case AST_CONTINUE: {
      ast_node_t *calls = NULL;
      if (s->kind == AST_RETURN) {
        /* 到**当前函数体块**（含）——嵌套函数有自己的边界 */
        calls = ds_collect_calls(ctx, my, ctx->func_base);
      } else if (ctx->loop_n > 0) {
        /* 到最近循环体块（含）——循环外的 defer 不执行 */
        calls = ds_collect_calls(ctx, my, ctx->loop_top[ctx->loop_n - 1]);
      }
      s->defers = calls; /* compiler 在跳转前编译该链 */
      ds_stmt_exprs(ctx, s);
      break;
    }
    case AST_IF: {
      ast_if_t *it = (ast_if_t *)s;
      ds_expr_literals(ctx, it->cond);
      ds_block(ctx, (ast_block_t *)it->then_body);
      if (it->else_body) {
        if (it->else_body->kind == AST_IF) {
          /* else-if 链：递归处理其 then/else（同一层的新的块） */
          ds_stmt_exprs(ctx, it->else_body);
          ds_block(ctx, (ast_block_t *)((ast_if_t *)it->else_body)->then_body);
        } else if (it->else_body->kind == AST_BLOCK) {
          ds_block(ctx, (ast_block_t *)it->else_body);
        }
      }
      break;
    }
    case AST_COMPTIME_IF: {
      ast_comptime_if_t *ci = (ast_comptime_if_t *)s;
      ds_expr_literals(ctx, ci->cond);
      /* 分支内禁止 defer：标记后递归（分支不是作用域，但可能被裁剪） */
      bool saved = ctx->in_comptime_branch;
      ctx->in_comptime_branch = true;
      if (ci->then_body) ds_block(ctx, (ast_block_t *)ci->then_body);
      if (ci->else_body) {
        if (ci->else_body->kind == AST_COMPTIME_IF)
          ds_stmt_exprs(ctx, ci->else_body);
        else
          ds_block(ctx, (ast_block_t *)ci->else_body);
      }
      ctx->in_comptime_branch = saved;
      break;
    }
    case AST_WHILE: {
      ast_while_t *w = (ast_while_t *)s;
      ds_expr_literals(ctx, w->cond);
      ds_loop_body(ctx, w->body);
      break;
    }
    case AST_DOWHILE: {
      ast_dowhile_t *dw = (ast_dowhile_t *)s;
      ds_expr_literals(ctx, dw->cond);
      ds_loop_body(ctx, dw->body);
      break;
    }
    case AST_FOR: {
      ast_for_t *fo = (ast_for_t *)s;
      /* init / update 是语句槽位（for-init 里不出现 defer，只收表达式里的
         函数字面量） */
      if (fo->init) ds_stmt_exprs(ctx, fo->init);
      ds_expr_literals(ctx, fo->cond);
      ds_expr_literals(ctx, fo->update);
      ds_loop_body(ctx, fo->body);
      break;
    }
    case AST_SWITCH: {
      ast_switch_t *sw = (ast_switch_t *)s;
      ds_expr_literals(ctx, sw->cond);
      for (ast_node_t *c = sw->cases; c; c = c->next) {
        ast_switch_case_t *sc = (ast_switch_case_t *)c;
        ds_block(ctx, (ast_block_t *)sc->body);
      }
      if (sw->default_body) ds_block(ctx, (ast_block_t *)sw->default_body);
      break;
    }
    case AST_BLOCK:
      ds_block(ctx, (ast_block_t *)s);
      break;
    default:
      ds_stmt_exprs(ctx, s);
      break;
    }

    prev = s;
    s = next;
  }

  /* 块尾正常退出路径：追加逆序调用（后定义先调用）。
     有 return/break/continue 跳出的路径由退出点节点的 defers 链处理，
     不会走到块尾，因此不会重复执行。 */
  ds_group_t *grp = &ctx->groups[my];
  if (grp->n > 0) {
    ast_node_t *head = NULL, *tail = NULL;
    for (size_t i = grp->n; i-- > 0;) {
      const char *name = ds_group_name(ctx, grp->entries[i]);
      if (!name) continue;
      ast_node_t *call = ds_make_call(ctx, name);
      if (!call) continue;
      if (!head) { head = tail = call; }
      else { tail->next = call; tail = call; }
    }
    if (head) {
      if (!block->stmts) block->stmts = head;
      else {
        ast_node_t *t = block->stmts;
        while (t->next) t = t->next;
        t->next = head;
      }
      block->stmts_last = tail;
    }
  }

  ctx->depth = my;
}

/* 函数体：建立全新的 defer/循环上下文（函数是 defer 的边界） */
static void ds_func(ds_ctx_t *ctx, ast_func_def_t *fn) {
  if (!fn || !fn->body) return;
  /* 嵌套函数体：**保留外层的 group 栈**（不能重置 depth，否则会覆盖外层块的
     defer 列表）；只切换"函数边界"（return 收集的终点）与循环/分支上下文。 */
  size_t saved_base = ctx->func_base;
  size_t saved_loops = ctx->loop_n;
  bool saved_branch = ctx->in_comptime_branch;
  ctx->func_base = ctx->depth;   /* 函数体块将在该下标 push */
  ctx->loop_n = 0;
  ctx->in_comptime_branch = false;
  ds_block(ctx, (ast_block_t *)fn->body);
  ctx->func_base = saved_base;
  ctx->loop_n = saved_loops;
  ctx->in_comptime_branch = saved_branch;
}

void desugar_program(desugar_t *ds, ast_program_t *prog) {
  if (!ds || !prog) return;
  ds_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.ds = ds;

  /* 顶层：全局函数 / 方法 / 全局 comptime func 的定义链 */
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    if (f->kind == AST_FUNC_DEF) ds_func(&ctx, (ast_func_def_t *)f);
  }
}
