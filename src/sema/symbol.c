#include "sema/symbol.h"
#include <string.h>

/* ---- strslice -> NUL-terminated temp buffer ---- */

static char *slice_to_cstr(allocator_t *alloc, strslice_t s) {
  char *buf = allocator_new_ex(alloc, "char", sizeof(char), default_move,
                               default_clone, NULL, s.len + 1);
  memcpy(buf, s.ptr, s.len);
  buf[s.len] = '\0';
  return buf;
}

/* ---- 生命周期 ---- */

sema_scope_t *sema_scope_new(allocator_t *alloc, sema_scope_kind_t kind,
                             sema_scope_t *parent) {
  if (!alloc) return NULL;
  sema_scope_t *scope =
      allocator_new_ex(alloc, "sema_scope_t", sizeof(sema_scope_t), NULL, NULL,
                       NULL, 1);
  scope->parent = parent;
  scope->alloc = alloc;
  scope->children = vec_new(alloc, false); /* 元素由递归销毁管理，非 vec_free */
  scope->symbols = strmap_new(alloc, true); /* owns sema_symbol_t* */
  scope->kind = kind;
  return scope;
}

void sema_scope_destroy(sema_scope_t **scope) {
  if (!scope || !*scope) return;
  sema_scope_t *self = *scope;
  allocator_t *alloc = self->alloc;

  /* 1. 递归销毁子作用域 */
  size_t n = vec_len(self->children);
  for (size_t i = 0; i < n; i++) {
    sema_scope_t *child = (sema_scope_t *)vec_get(self->children, i);
    sema_scope_destroy(&child);
  }
  vec_free(alloc, &self->children);

  /* 2. 符号的 ast 借用 AST（arena 管理），不释放；直接销毁符号表 */
  strmap_free(alloc, &self->symbols);

  /* 3. 释放自身 */
  allocator_free(alloc, (void **)scope);
}

/* ---- 树结构 ---- */

void sema_scope_add_child(sema_scope_t *scope, sema_scope_t *child) {
  if (!scope || !child) return;
  vec_push(scope->children, scope->alloc, child);
}

size_t sema_scope_children_count(const sema_scope_t *scope) {
  return scope ? vec_len(scope->children) : 0;
}

sema_scope_t *sema_scope_child(const sema_scope_t *scope, size_t idx) {
  if (!scope) return NULL;
  return (sema_scope_t *)vec_get(scope->children, idx);
}

/* ---- 符号操作 ---- */

sema_symbol_t *sema_scope_define(sema_scope_t *scope, strslice_t name,
                                 const sema_symbol_t *init) {
  if (!scope) return NULL;
  char *key = slice_to_cstr(scope->alloc, name);
  bool dup = strmap_contains(scope->symbols, key);
  if (dup) {
    allocator_free(scope->alloc, (void **)&key);
    return NULL;
  }
  sema_symbol_t *sym = allocator_new_ex(scope->alloc, "sema_symbol_t",
                                        sizeof(sema_symbol_t), NULL, NULL,
                                        NULL, 1);
  if (init) {
    *sym = *init;
  } else {
    memset(sym, 0, sizeof(*sym));
  }
  strmap_insert(scope->symbols, scope->alloc, key, sym);
  allocator_free(scope->alloc, (void **)&key);
  return sym;
}

/**
 * 沿 parent 链查找符号，返回第一个**已激活**的命中（跳过未激活符号：
 * Pass 3a 已注册但 Pass 3b 尚未走到定义点的变量——与 VM scope_lookup
 * 对齐，自引用 `var x = x + 1` 解析到外层而非自身）。遮罩过滤由
 * VM scope 链承担，本函数只需激活过滤。
 * 未找到返回 NULL。
 */
sema_symbol_t *sema_lookup(const sema_scope_t *scope, strslice_t name) {
  if (!scope || !name.ptr) return NULL;
  allocator_t *alloc = scope->alloc;
  char *key = slice_to_cstr(alloc, name);
  sema_symbol_t *found = NULL;
  for (const sema_scope_t *s = scope; s && !found; s = s->parent) {
    sema_symbol_t *sym = (sema_symbol_t *)strmap_get(s->symbols, key);
    if (sym && sym->is_active) found = sym;
  }
  allocator_free(alloc, (void **)&key);
  return found;
}

sema_symbol_t *sema_scope_find_local(const sema_scope_t *scope,
                                     strslice_t name) {
  if (!scope || !name.ptr) return NULL;
  allocator_t *alloc = scope->alloc;
  char *key = slice_to_cstr(alloc, name);
  sema_symbol_t *sym = (sema_symbol_t *)strmap_get(scope->symbols, key);
  allocator_free(alloc, (void **)&key);
  return sym;
}
