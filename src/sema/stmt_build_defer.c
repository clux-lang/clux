#include "sema/comptime.h"
#include "sema/sema.h"
#include "core/panic.h"
#include "vm/type_ptr.h"
#include "vm/type_struct.h"
#include "vm/type_error.h"
#include "parser/ast_alignof.h"
#include "parser/ast_array.h"
#include "parser/ast_block.h"
#include "parser/ast_const.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_for.h"
#include "parser/ast_func_def.h"
#include "vm/type_tuple.h"
#include "vm/type_array.h"
#include "parser/ast_construct.h"
#include "parser/parser.h"
#include "parser/ast_assign.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_index.h"
#include "parser/ast_foreach.h"
#include "parser/ast_ident.h"
#include "parser/ast_if.h"
#include "ctfe/ctfe.h"
#include "parser/ast_comptime_if.h"
#include "parser/ast_option.h"
#include "parser/ast_return.h"
#include "parser/ast_sizeof.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_typeof.h"
#include "parser/ast_union_def.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_switch.h"
#include "parser/ast_type_def.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_var_def.h"
#include "parser/ast_volatile.h"
#include "parser/ast_walk.h"
#include "parser/ast_while.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_defer.h"
#include "parser/ast_call.h"
#include "parser/ast_expr_stmt.h"
#include "vm/type_func.h"

#include <stdio.h>
#include <string.h>

#include "sema_build_internal.h"

static void ctif_check_branch_block(sema_t *sema, ast_node_t *body);

/* ---- defer 语法糖改写（Pass 3a 内联） ----
 *
 * defer 本质是匿名无参 void 函数 + 作用域退出点逆序调用。sema 在 Pass 3a
 * build_block 遍历语句链时内联改写：
 *   1. AST_DEFER → 取 closure（已是 ast_func_def_t），设名 __defer_N，
 *      替换语句链中 AST_DEFER 节点 → 走 build_local_func 正常注册符号 +
 *      分配 fid + 建 fscope 树；登记进当前 scope->defers
 *   2. 块尾追加逆序 call 序列（每层 LIFO）
 *   3. return 前：沿 scope parent 链到 SEMA_SCOPE_FUNCTION，收集沿途各层
 *      defers 逆序插入 call
 *   4. break/continue 前：沿 scope parent 链到最近循环 scope，收集沿途
 *      各层 defers 逆序插入 call
 *
 * 改写后 AST 中不再有 AST_DEFER——Pass 3b / compiler / ctfe 完全不感知
 * defer，只看到普通局部函数定义 + 普通函数调用语句。
 */

/* 构造 `__defer_N()` call 语句（AST_EXPR_STMT(AST_CALL(callee=IDENT, args=NULL))）。
   fn_name 是 arena 持久化的 NUL 终止字符串。 */
static ast_node_t *make_defer_call(sema_t *sema, const char *fn_name,
                                   uint32_t tok_begin, uint32_t tok_end) {
  arena_t *a = sema->arena;
  /* tok_begin=0 标记为编译器生成节点——build_block 的 unreachable 检查
     跳过 tok_begin==0 的节点（defer call 是退出点清理代码，非用户语句）。 */
  (void)tok_begin; (void)tok_end;
  ast_node_t *ident = ast_ident_new(a, 0, 0);
  size_t nlen = strlen(fn_name);
  char *name_buf = (char *)arena_calloc(a, nlen + 1, 1, ALIGNOF(max_align_t));
  memcpy(name_buf, fn_name, nlen);
  ((ast_ident_t *)ident)->name = strslice_from_bytes(name_buf, nlen);

  ast_node_t *call = ast_call_new(a, 0, 0);
  ((ast_call_t *)call)->callee = ident;
  ((ast_call_t *)call)->args = NULL;
  ((ast_call_t *)call)->args_last = NULL;

  ast_node_t *stmt = ast_expr_stmt_new(a, 0, 0);
  ((ast_expr_stmt_t *)stmt)->expr = call;
  return stmt;
}

/* 沿 scope parent 链收集 defers 并构造逆序 call 语句链。
   stop_scope 不含（defers 在 stop_scope 之前各层）。
   返回 call 语句链首（已按执行顺序排列），*out_last 设为链尾。
   无 defer 时返回 NULL。 */
static ast_node_t *collect_defer_calls(sema_t *sema, sema_scope_t *from_scope,
                                       const sema_scope_t *stop_scope,
                                       ast_node_t **out_last) {
  ast_node_t *calls[64];
  size_t n = sema_scope_collect_defers(from_scope, stop_scope, calls, 64);
  if (n == 0) {
    if (out_last) *out_last = NULL;
    return NULL;
  }
  ast_node_t *head = NULL;
  ast_node_t *tail = NULL;
  for (size_t i = 0; i < n; i++) {
    ast_func_def_t *fn = (ast_func_def_t *)calls[i];
    /* 名字是 arena NUL 终止字符串（设名时已分配） */
    char fname[64];
    snprintf(fname, sizeof(fname), "%.*s", (int)fn->name.len, fn->name.ptr);
    ast_node_t *call = make_defer_call(sema, fname, fn->base.tok_begin,
                                       fn->base.tok_end);
    if (!head) { head = call; tail = call; }
    else { tail->next = call; tail = call; }
  }
  if (out_last) *out_last = tail;
  return head;
}

/* 沿 parent 链找到最近的循环作用域（while/dowhile/for 的 body scope 的
   parent，或 for scope 本身）。
   当前 sema_scope_kind_t 没有 LOOP 标记——循环体的 scope kind 是
   SEMA_SCOPE_BLOCK，for 的 init scope 是 SEMA_SCOPE_FOR。
   我们靠 sema->fn.loop_depth > 0 判断在循环内，沿 parent 找到
   loop_depth 减为 0 的边界——但 scope 没有 loop_depth 字段。
   替代方案：build_block 的循环分支传入循环 scope，通过 sema 临时字段
   记录当前循环 scope 栈（类似 loop_stack）。这里先用 parent 链上溯到
   SEMA_SCOPE_FUNCTION（与 return 相同的 stop），break/continue 的位置
   检查已在 Pass 3a 做过（loop_depth==0 报错），所以到这里一定在循环内。
   精确的循环边界稍后用 sema 循环 scope 栈处理。 */
/* break/continue 的 defer 收集范围：从当前 scope 沿 parent 链收集到
   循环体 scope **包含**。stop 设为循环 scope 的 parent。
   TODO: 精确循环边界——当前回退到函数根（break/continue 的 defer call
   会执行到函数根所有层，而非只到循环层）。需要 sema 循环 scope 栈来
   精确定位。先保持与 return 相同的行为（安全过保守）。 */
static const sema_scope_t *find_loop_stop(sema_scope_t *scope) {
  for (sema_scope_t *s = scope; s; s = s->parent) {
    if (s->kind == SEMA_SCOPE_FUNCTION) return s->parent;
  }
  return NULL;
}

/* return 的 defer 收集范围：从当前 scope 沿 parent 链收集到函数 scope
   **包含**（函数体顶层的 defers 也要执行）。因此 stop 设为函数 scope 的
   parent（函数调用方的 scope），使 collect_defer_calls 的 `s != stop`
   循环能包含函数 scope 自身的 defers。 */
static const sema_scope_t *find_func_stop(sema_scope_t *scope) {
  for (sema_scope_t *s = scope; s; s = s->parent) {
    if (s->kind == SEMA_SCOPE_FUNCTION) return s->parent;
  }
  return NULL;
}

static void ctif_check_branch_stmt(sema_t *sema, ast_node_t *s) {
  for (; s; s = s->next) {
    switch (s->kind) {
    case AST_VAR_DEF: {
      ast_var_def_t *vd = (ast_var_def_t *)s;
      if (vd->is_comptime)
        diag_error(sema->diag, sema_loc(sema, s),
                   "a 'comptime var' cannot be defined inside a 'comptime if' "
                   "branch (declare it before the 'comptime if')");
      break;
    }
    case AST_FUNC_DEF: {
      ast_func_def_t *fd = (ast_func_def_t *)s;
      if (fd->is_comptime)
        diag_error(sema->diag, sema_loc(sema, s),
                   "a 'comptime func' cannot be defined inside a 'comptime if' "
                   "branch (declare it before the 'comptime if')");
      /* 不进入函数体：嵌套函数是独立作用域 */
      break;
    }
    case AST_DEFER:
      /* defer 的闭包是独立函数作用域：不进入 */
      break;
    case AST_BLOCK:
      ctif_check_branch_stmt(sema, ((ast_block_t *)s)->stmts);
      break;
    case AST_IF: {
      ast_if_t *it = (ast_if_t *)s;
      ctif_check_branch_block(sema, it->then_body);
      ctif_check_branch_block(sema, it->else_body);
      break;
    }
    case AST_COMPTIME_IF: {
      ast_comptime_if_t *ci = (ast_comptime_if_t *)s;
      ctif_check_branch_block(sema, ci->then_body);
      ctif_check_branch_block(sema, ci->else_body);
      break;
    }
    case AST_WHILE: {
      ast_while_t *w = (ast_while_t *)s;
      ctif_check_branch_block(sema, w->body);
      break;
    }
    case AST_DOWHILE: {
      ast_dowhile_t *d = (ast_dowhile_t *)s;
      ctif_check_branch_block(sema, d->body);
      break;
    }
    case AST_FOR: {
      ast_for_t *fo = (ast_for_t *)s;
      ctif_check_branch_stmt(sema, fo->init);
      ctif_check_branch_block(sema, fo->body);
      break;
    }
    case AST_SWITCH: {
      ast_switch_t *sw = (ast_switch_t *)s;
      for (ast_node_t *c = sw->cases; c; c = c->next) {
        ast_switch_case_t *sc = (ast_switch_case_t *)c;
        ctif_check_branch_block(sema, sc->body);
      }
      ctif_check_branch_block(sema, sw->default_body);
      break;
    }
    default:
      break;
    }
  }
}

static void ctif_check_branch_block(sema_t *sema, ast_node_t *body) {
  if (!body) return;
  if (body->kind == AST_BLOCK)
    ctif_check_branch_stmt(sema, ((ast_block_t *)body)->stmts);
  else if (body->kind == AST_COMPTIME_IF)
    ctif_check_branch_stmt(sema, body); /* else-if 链 */
}

/* 条件求值：CTFE 直接解释条件表达式（字面量 / 全局 comptime var（已在
   root_scope 绑定）/ sizeof·alignof / 运算符）。失败时产出诊断并返回 false。 */
static bool ctif_eval_cond(sema_t *sema, sema_scope_t *scope, ast_node_t *cond,
                           bool *out) {
  if (!cond) return false;
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.vm = sema->vm;
  ctx.sema = sema;
  ctx.sema_scope = scope;
  ctx.budget = 100000;
  ctx.max_depth = 128;
  value_t *r = ctfe_eval(&ctx, cond);
  if (!r || value_is_error(sema->vm, r)) {
    const char *msg = NULL;
    if (r) {
      error_data_t *ed = (error_data_t *)value_data(r);
      msg = ed && ed->message ? ed->message : NULL;
    }
    diag_error(sema->diag, sema_loc(sema, cond),
               "'comptime if' condition is not a compile-time constant%s%s",
               msg ? ": " : "", msg ? msg : "");
    return false;
  }
  if (value_type(r) != sema->vm->type_bool) {
    char tn[64];
    sema_type_name(value_type(r), tn, sizeof tn);
    diag_error(sema->diag, sema_loc(sema, cond),
               "'comptime if' condition must be bool (got %s)", tn);
    return false;
  }
  *out = *(const bool *)value_data(r);
  return true;
}

/* 主流程：把块内（含嵌套块）的 comptime if 替换成存活分支的语句链。
   返回 true = 发生过裁剪。 */
void comptime_if_splice(sema_t *sema, ast_block_t *block,
                               sema_scope_t *scope) {
  if (!block || !block->stmts) return;
  bool changed = true;
  while (changed) {
    changed = false;
    for (ast_node_t **cur = &block->stmts; *cur;) {
      ast_node_t *s = *cur;
      if (s->kind != AST_COMPTIME_IF) {
        cur = &s->next;
        continue;
      }
      ast_comptime_if_t *ci = (ast_comptime_if_t *)s;

      /* comptime func 体内不支持（CTFE 解释执行，条件可能依赖参数） */
      if (sema->fn.walking_comptime) {
        diag_error(sema->diag, sema_loc(sema, s),
                   "'comptime if' is not supported inside a 'comptime func' "
                   "(its body is interpreted by CTFE; the condition may depend "
                   "on parameters)");
        *cur = s->next;
        changed = true;
        break;
      }

      /* 结构检查（两个分支都查：这是纯语法形状规则） */
      ctif_check_branch_block(sema, ci->then_body);
      ctif_check_branch_block(sema, ci->else_body);

      bool taken = false;
      if (!ctif_eval_cond(sema, scope, ci->cond, &taken)) {
        /* 已诊断：整条语句丢弃（避免下游二次诊断） */
        *cur = s->next;
        changed = true;
        break;
      }

      /* 存活分支的语句链：then/else 既可能是块，也可能是 else-if 的
         comptime if 节点本身；没有 else 且条件为假 → 整条消失。 */
      ast_node_t *pick = taken ? ci->then_body : ci->else_body;
      ast_node_t *chain = NULL;
      if (pick) {
        chain = pick->kind == AST_BLOCK ? ((ast_block_t *)pick)->stmts : pick;
      }
      if (chain) {
        ast_node_t *tail = chain;
        while (tail->next) tail = tail->next;
        tail->next = s->next;   /* 接上 if 之后的语句 */
        *cur = chain;
      } else {
        *cur = s->next;
      }
      changed = true;
      break; /* 重新从头扫描（拼接进来的链可能还含 comptime if） */
    }
  }
}

void desugar_defers(sema_t *sema, ast_block_t *block,
                           sema_scope_t *scope) {
  /* 第一遍：替换 AST_DEFER → AST_FUNC_DEF，登记进 scope->defers。
     同时记录块内是否有 return/break/continue 需要插入 defer call。 */
  ast_node_t *prev = NULL;
  ast_node_t *defer_calls_head = NULL;
  ast_node_t *defer_calls_tail = NULL;

  for (ast_node_t *s = block->stmts; s; s = s->next) {
    if (s->kind == AST_DEFER) {
      ast_defer_t *d = (ast_defer_t *)s;
      ast_func_def_t *fn = d->closure;
      if (!fn) continue;

      /* 设名 __defer_N */
      char fname[64];
      snprintf(fname, sizeof(fname), "__defer_%u", sema->defer_seq++);
      size_t nlen = strlen(fname);
      char *name_buf = (char *)arena_calloc(
          sema->arena, nlen + 1, 1, ALIGNOF(max_align_t));
      memcpy(name_buf, fname, nlen);
      fn->name = strslice_from_bytes(name_buf, nlen);

      /* 替换语句链：AST_DEFER 节点 → closure（AST_FUNC_DEF） */
      fn->base.next = s->next;
      if (prev) prev->next = &fn->base;
      else block->stmts = &fn->base;

      /* 登记 defers（build_local_func 在正式遍历时走，
         但 defers 收集需要在 build_block 遍历前完成——
         这里先登记，build_local_func 在遍历时处理注册）。
         注意：build_local_func 会注册符号 + 分配 fid + 建 fscope，
         我们在正式遍历时让它走，这里只登记 defers 数组。 */
      sema_scope_add_defer(scope, &fn->base);

      /* 准备块尾 call 语句（逆序：后登记的先 call，插到 call 链首） */
      ast_node_t *call = make_defer_call(sema, fname, s->tok_begin, s->tok_end);
      if (!defer_calls_head) {
        defer_calls_head = call;
        defer_calls_tail = call;
      } else {
        call->next = defer_calls_head;
        defer_calls_head = call;
      }

      prev = &fn->base;
      continue;
    }

    /* return/break/continue：沿 scope parent 链收集沿途 defer calls，
       挂载到退出点节点的 defers 字段（不修改语句链）。
       compiler 编译 return 时先求值返回值压栈，然后编译 defers 链中的
       call 语句，最后发 RET——保证返回值在 defer 之前求值，defer 在
       实际跳转之前执行。break/continue 同理：先编译 defers 再 POP+JMP。
       不引入临时变量，避免 fatal/ref/own 语义被 var 定义改变。 */
    if (s->kind == AST_RETURN || s->kind == AST_BREAK ||
        s->kind == AST_CONTINUE) {
      const sema_scope_t *stop = (s->kind == AST_RETURN)
                                     ? find_func_stop(scope)
                                     : find_loop_stop(scope);
      ast_node_t *tail = NULL;
      ast_node_t *calls = collect_defer_calls(sema, scope, stop, &tail);
      s->defers = calls; /* NULL 或 call 链首（compiler 消费） */
    }

    prev = s;
  }

  /* 块尾正常退出路径：追加逆序 call 序列到语句链末尾。
     这些 defer calls 只在块正常走到末尾（无 return/break/continue 跳出）
     时执行——有 return 的路径由 return 节点的 defers 字段处理，不会
     走到块尾。 */
  if (defer_calls_head) {
    if (!block->stmts) {
      block->stmts = defer_calls_head;
    } else {
      ast_node_t *tail = block->stmts;
      while (tail->next) tail = tail->next;
      tail->next = defer_calls_head;
    }
    block->stmts_last = defer_calls_tail;
  }
}
