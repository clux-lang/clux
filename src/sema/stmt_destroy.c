#include "sema/sema.h"

#include "core/vec.h"
#include "parser/ast_free.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ident.h"
#include "sema_stmt_internal.h"

#include <string.h>

/* 为作用域内**闭包值**生成 `FREE <name>`（释放捕获 env 块）。
 *
 * 指针/切片由用户显式 `free`；普通复合值不持有独立堆块。因此只有两类符号
 * 需要作用域退出时释放：
 *   - 局部函数 / defer 闭包（SEMA_SYM_FUNC 符号，且有捕获 → env）；
 *   - 持有闭包值的变量（SEMA_SYM_VAR 且类型是 func）。
 * comptime 函数体由 CTFE 解释执行，不生成 FREE。 */
ast_node_t *make_free_stmts(sema_t *sema, sema_scope_t *scope) {
  if (!scope || !scope->symbols) return NULL;
  if (sema->fn.walking_comptime) return NULL;
  const vec_t *keys = strmap_keys(scope->symbols);
  if (!keys) return NULL;
  size_t n = vec_len(keys);
  ast_node_t *head = NULL;
  ast_node_t *tail = NULL;
  for (size_t i = 0; i < n; i++) {
    const char *name = (const char *)vec_get(keys, i);
    if (!name) continue;
    sema_symbol_t *sym = (sema_symbol_t *)strmap_get(scope->symbols, name);
    if (!sym) continue;

    bool needs_free = false;
    if (sym->kind == SEMA_SYM_FUNC) {
      const ast_func_def_t *fd = (const ast_func_def_t *)sym->ast;
      needs_free = fd && fd->base.kind == AST_FUNC_DEF &&
                   fd->capture_type_id != 0;
    } else if (sym->kind == SEMA_SYM_VAR) {
      /* 定义点尚未走到（提前 return/break 的路径）→ 该变量在运行时还不存在，
         不能生成 FREE（否则 PUSH 一个尚未 DEFINE 的名字）。 */
      needs_free = sym->type && !sym->is_comptime && sym->declared &&
                   type_kind_of(sym->type) == TYPE_KIND_FUNC;
    }
    if (!needs_free) continue;

    /* arena 持久化变量名（AST_IDENT 引用 NUL 终止字符串）。 */
    size_t nlen = strlen(name);
    char *name_buf = (char *)arena_calloc(
        sema->arena, nlen + 1, 1, ALIGNOF(max_align_t));
    if (!name_buf) continue;
    memcpy(name_buf, name, nlen);
    ast_node_t *obj = ast_ident_new(sema->arena, 0, 0);
    if (!obj) continue;
    ((ast_ident_t *)obj)->name = strslice_from_bytes(name_buf, nlen);
    ast_node_t *fs = ast_free_new(sema->arena, obj);
    if (!fs) continue;
    if (!head) head = tail = fs;
    else { tail->next = fs; tail = fs; }
  }
  return head;
}

/* 沿 scope parent 链到 stop_scope（不含）收集各层闭包变量的 FREE 语句链。
    与 collect_defer_calls 同构——FREE 在 defer calls 之后执行（追加到 defers
    链末尾）。返回 FREE 语句链首（按执行顺序：内层 scope 先），NULL = 无。 */
ast_node_t *collect_free_stmts(sema_t *sema, sema_scope_t *from_scope,
                                      const sema_scope_t *stop_scope) {
  ast_node_t *head = NULL;
  ast_node_t *tail = NULL;
  for (sema_scope_t *s = from_scope; s && s != stop_scope; s = s->parent) {
    ast_node_t *free_stmts = make_free_stmts(sema, s);
    if (!free_stmts) continue;
    if (!head) {
      head = free_stmts;
    } else {
      tail->next = free_stmts;
    }
    ast_node_t *ft = free_stmts;
    while (ft->next) ft = ft->next;
    tail = ft;
  }
  return head;
}

/* 找到函数 scope 的 parent（return 的 FREE 收集范围 stop）。 */
const sema_scope_t *find_func_stop_sema(sema_scope_t *scope) {
  for (sema_scope_t *s = scope; s; s = s->parent) {
    if (s->kind == SEMA_SCOPE_FUNCTION) return s->parent;
  }
  return NULL;
}

/* 找到最近循环 scope 的 parent（break/continue 的 FREE 收集范围 stop）。 */
const sema_scope_t *find_loop_stop_sema(sema_scope_t *scope) {
  for (sema_scope_t *s = scope; s; s = s->parent) {
    if (s->kind == SEMA_SCOPE_FOR) return s->parent;
  }
  return NULL;
}
