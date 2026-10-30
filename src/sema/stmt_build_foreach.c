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

/* ===========================================================================
 * comptime foreach：编译期展开（数组/元组，长度编译期已知）
 *
 *   comptime foreach (var a of c) { body }
 *     ⇒ { var a = c[0]; body } { var a = c[1]; body } …   （N 份独立作用域）
 *
 * 为什么放在 Pass 3a（与 comptime if 同一位置）：展开会**改变语句链结构**，
 * 作用域树必须从展开后的语句建起（3b 只消费 3a 建好的树）。长度是类型信息，
 * 前脱糖没有类型，所以只能在这里做。
 *
 * body 的 N 份副本**按 token 区间重新解析**得到（parser 支持在既有 token 池上
 * 定位后重新解析）——不需要给 AST 写深拷贝，副本天然独立，且 token 下标仍指向
 * 原始源码（诊断位置正确）。
 * =========================================================================== */

/* 在既有 token 池上从 tok_begin 起重新解析一个块（返回新的 AST_BLOCK）。 */
static ast_node_t *ctfe_reparse_block(sema_t *sema, uint32_t tok_begin) {
  parser_t *p = parser_create(sema->vm->alloc, sema->arena, sema->tokens);
  if (!p) return NULL;
  p->pos = tok_begin;
  ast_node_t *b = parse_block(p);
  if (b && b->kind == AST_ERROR) b = NULL;
  parser_destroy(&p);
  return b;
}

/* 重新解析一个类型表达式（tok_begin 指向类型起点）。 */
static ast_node_t *ctfe_reparse_type(sema_t *sema, uint32_t tok_begin) {
  parser_t *p = parser_create(sema->vm->alloc, sema->arena, sema->tokens);
  if (!p) return NULL;
  p->pos = tok_begin;
  ast_node_t *te = parse_scope_annotated_type(p);
  if (te && te->kind == AST_ERROR) te = NULL;
  parser_destroy(&p);
  return te;
}

static strslice_t ctfe_name(sema_t *sema, const char *s) {
  size_t len = strlen(s);
  char *buf = (char *)arena_alloc(sema->arena, len + 1, 1);
  if (!buf) return STRSLICE_EMPTY;
  memcpy(buf, s, len + 1);
  return strslice_from_bytes(buf, len);
}

/* 容器表达式的**静态类型**（3a 阶段能拿到的）：
   - 裸标识符：符号已有类型 → 用它；
   - 同一块里靠前的 `var c = …`：由声明推出（显式标注 / 构造字面量的类型）；
   - 构造字面量（`.[N]T{…}` / `.<…>{…}`）：解析其类型槽位。
   其它形态无法静态定型 → 报错并建议显式标注或先绑定变量。 */
static const type_t *ctfe_container_type(sema_t *sema, ast_node_t *expr,
                                         sema_scope_t *scope) {
  if (!expr) return NULL;
  if (expr->kind == AST_CONSTRUCT) {
    ast_construct_t *c = (ast_construct_t *)expr;
    if (!c->type) return NULL; /* 匿名构造：无目标类型上下文 */
    return sema_resolve_type_slot(sema, &c->type);
  }
  if (expr->kind == AST_IDENT) {
    sema_symbol_t *sym = sema_lookup(scope, ((ast_ident_t *)expr)->name);
    if (sym && sym->type) return sym->type;
    return NULL;
  }
  return NULL;
}

/* 在块 blk 的声明里找名为 want 的 var，预解析它的类型：
     显式标注 → 解析槽位；推断 → 构造字面量可静态定型；标识符 → 递归取符号类型。
   3a 阶段推断型 var 的符号 type 仍是 NULL（要等 3b 的 shadow_var_def），所以
   只能回看**声明 AST**。before 非 NULL 时只扫它之前的语句。 */
static const type_t *ctfe_var_decl_type(sema_t *sema, ast_block_t *blk,
                                        ast_node_t *before, strslice_t want,
                                        sema_scope_t *scope) {
  if (!blk || blk->base.kind != AST_BLOCK) return NULL;
  for (ast_node_t *s = blk->stmts; s; s = s->next) {
    if (before && s == before) break;
    if (s->kind != AST_VAR_DEF) continue;
    ast_var_def_t *vd = (ast_var_def_t *)s;
    if (!(vd->name.len == want.len &&
          memcmp(vd->name.ptr, want.ptr, want.len) == 0))
      continue;
    if (vd->type_expr) return sema_resolve_type_slot(sema, &vd->type_expr);
    if (vd->init) {
      if (vd->init->kind == AST_SLICE) {
        diag_error(sema->diag, sema_loc(sema, &vd->base),
                   "'comptime foreach': container is a slice — its length is "
                   "only known at runtime; use an array ([N]T) or a tuple");
        return NULL;
      }
      if (vd->init->kind == AST_CONSTRUCT && ((ast_construct_t *)vd->init)->type)
        return sema_resolve_type_slot(sema,
                                      &((ast_construct_t *)vd->init)->type);
      if (vd->init->kind == AST_IDENT) {
        sema_symbol_t *is = sema_lookup(scope, ((ast_ident_t *)vd->init)->name);
        if (is && is->type) return is->type;
      }
    }
    return NULL;
  }
  return NULL;
}

/* 容器的声明类型：先当前块（foreach 之前），再沿 block_ast 链向外层块找
   （嵌套 `comptime foreach` 的容器常常声明在外层块）。 */
static const type_t *ctfe_preceding_var_type(sema_t *sema, ast_block_t *block,
                                             ast_foreach_t *fe,
                                             sema_scope_t *scope) {
  if (!fe->iterable || fe->iterable->kind != AST_IDENT) return NULL;
  strslice_t want = ((ast_ident_t *)fe->iterable)->name;
  const type_t *t = ctfe_var_decl_type(sema, block, (ast_node_t *)fe, want,
                                       scope);
  if (t) return t;
  for (sema_scope_t *sc = scope; sc; sc = sc->parent) {
    if (!sc->block_ast || sc->block_ast->kind != AST_BLOCK) continue;
    ast_block_t *blk = (ast_block_t *)sc->block_ast;
    if (blk == block) continue;
    t = ctfe_var_decl_type(sema, blk, NULL, want, scope);
    if (t) return t;
  }
  return NULL;
}

/* 生成 `<obj>[<i>]` 的下标表达式（每次新建节点：sema 会就地改写节点）。 */
static ast_node_t *ctfe_index_expr(sema_t *sema, strslice_t obj_name,
                                   const type_t *obj_type, size_t i,
                                   uint32_t tb, uint32_t te) {
  ast_node_t *obj = ast_ident_new(sema->arena, tb, te);
  if (!obj) return NULL;
  ((ast_ident_t *)obj)->name = obj_name;
  ast_node_t *idx = ast_int_lit_new(sema->arena, tb, te);
  if (!idx) return NULL;
  ((ast_int_lit_t *)idx)->type = STRSLICE_LIT("i32");
  ((ast_int_lit_t *)idx)->value = (int64_t)i;
  ast_node_t *node = ast_index_new(sema->arena, tb, te);
  if (!node) return NULL;
  ((ast_index_t *)node)->object = obj;
  ((ast_index_t *)node)->indices = idx;
  ((ast_index_t *)node)->indices_last = idx;
  (void)obj_type;
  return node;
}

/* 展开一个 comptime foreach；返回生成的语句链（NULL = 已诊断）。 */
static ast_node_t *gen_comptime_foreach(sema_t *sema, ast_block_t *block,
                                        ast_foreach_t *fe,
                                        sema_scope_t *scope) {
  const uint32_t tb = fe->base.tok_begin;
  const uint32_t te = fe->base.tok_end;
  if (!fe->body || fe->body->kind != AST_BLOCK || !fe->iterable ||
      !fe->name.ptr) {
    diag_error(sema->diag, sema_loc(sema, &fe->base),
               "malformed 'comptime foreach' (missing iterable or body)");
    return NULL;
  }

  const type_t *ct = ctfe_container_type(sema, fe->iterable, scope);
  if (!ct) ct = ctfe_preceding_var_type(sema, block, fe, scope);
  if (!ct) {
    diag_error(sema->diag, sema_loc(sema, fe->iterable),
               "'comptime foreach': cannot determine the container's type at "
               "compile time; annotate it (`var c: [N]T = …`) or bind it in an "
               "outer scope");
    return NULL;
  }

  size_t n = 0;
  bool is_arr = false;
  if (type_kind_of(ct) == TYPE_KIND_ARRAY) {
    is_arr = true;
    n = array_type_len(ct);
  } else if (type_kind_of(ct) == TYPE_KIND_TUPLE) {
    n = tuple_type_elem_count(ct);
  } else {
    char tn[64];
    sema_type_name(ct, tn, sizeof tn);
    diag_error(sema->diag, sema_loc(sema, fe->iterable),
               "'comptime foreach' requires an array or tuple with a "
               "compile-time-known length, got %s", tn);
    return NULL;
  }
  (void)is_arr;

  /* 容器不是裸标识符时先绑定临时变量（避免重复求值副作用）。 */
  const bool direct = fe->iterable->kind == AST_IDENT;
  strslice_t obj_name = direct ? ((ast_ident_t *)fe->iterable)->name
                               : STRSLICE_EMPTY;
  ast_node_t *head = NULL, *tail = NULL;
  if (!direct) {
    char buf[32];
    snprintf(buf, sizeof buf, "__cfc%u", sema->foreach_seq++);
    obj_name = ctfe_name(sema, buf);
    if (!obj_name.ptr) return NULL;
    ast_node_t *vd = ast_var_def_new(sema->arena, tb, te);
    if (!vd) return NULL;
    ((ast_var_def_t *)vd)->name = obj_name;
    ((ast_var_def_t *)vd)->type_expr = sema_ct_type_ref(sema, ct,
                                                        fe->iterable);
    ((ast_var_def_t *)vd)->init = fe->iterable;
    head = tail = vd;
  }

  for (size_t i = 0; i < n; i++) {
    /* 每轮：{ var a [: T] = obj[i]; <body 副本> }（无 var 形态：a = obj[i]） */
    ast_node_t *body_copy = ctfe_reparse_block(sema, fe->body->tok_begin);
    if (!body_copy) {
      diag_error(sema->diag, sema_loc(sema, fe->body),
                 "'comptime foreach': failed to duplicate the loop body");
      return head;
    }
    ast_node_t *body_stmts = ((ast_block_t *)body_copy)->stmts;
    ((ast_block_t *)body_copy)->stmts = NULL;

    ast_node_t *elem = ctfe_index_expr(sema, obj_name, ct, i, tb, te);
    if (!elem) return head;
    ast_node_t *bind = NULL;
    if (fe->is_decl) {
      bind = ast_var_def_new(sema->arena, tb, te);
      if (!bind) return head;
      ((ast_var_def_t *)bind)->name = fe->name;
      /* 显式标注 `[: T]` 每轮重新解析一份（节点不能共享：sema 会折叠改写）。 */
      if (fe->type_expr)
        ((ast_var_def_t *)bind)->type_expr =
            ctfe_reparse_type(sema, fe->type_expr->tok_begin);
      ((ast_var_def_t *)bind)->init = elem;
    } else {
      ast_node_t *lhs = ast_ident_new(sema->arena, tb, te);
      if (!lhs) return head;
      ((ast_ident_t *)lhs)->name = fe->name;
      bind = ast_assign_new(sema->arena, tb, te);
      if (!bind) return head;
      ((ast_assign_t *)bind)->target = lhs;
      ((ast_assign_t *)bind)->value = elem;
      ((ast_assign_t *)bind)->op =
          (token_t *)arena_token_symbol(sema->arena, "=",
                                        &(location_t){0});
    }
    bind->next = body_stmts;
    ast_node_t *blk = ast_block_new(sema->arena, tb, te);
    if (!blk) return head;
    ((ast_block_t *)blk)->stmts = bind;
    ((ast_block_t *)blk)->stmts_last = NULL;
    if (tail) tail->next = blk; else head = blk;
    tail = blk;
  }
  return head;
}

/* 在 Pass 3a 的块遍历前把 comptime foreach 展开（与 comptime if 同一机制）。 */
void comptime_foreach_splice(sema_t *sema, ast_block_t *block,
                                    sema_scope_t *scope) {
  if (!block || !block->stmts) return;
  bool changed = true;
  while (changed) {
    changed = false;
    for (ast_node_t **cur = &block->stmts; *cur;) {
      ast_node_t *s = *cur;
      if (s->kind != AST_FOREACH || !((ast_foreach_t *)s)->is_comptime) {
        cur = &s->next;
        continue;
      }
      ast_node_t *gen = gen_comptime_foreach(sema, block, (ast_foreach_t *)s,
                                             scope);
      if (!gen) {
        *cur = s->next; /* 已诊断：摘除，别让下游再看到 */
        changed = true;
        break;
      }
      ast_node_t *tail2 = gen;
      while (tail2->next) tail2 = tail2->next;
      tail2->next = s->next;
      *cur = gen;
      /* 生成链内部可能还有嵌套的 comptime foreach（body 副本里）：留在
         各自块里由对应块的 build_block 递归展开，这里从 gen 之后继续扫。 */
      cur = &tail2->next;
      changed = true;
      break;
    }
  }
}
