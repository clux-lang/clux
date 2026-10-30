#include "sema/sema.h"
#include "parser/ast_if.h"
#include "parser/ast_return.h"
#include "parser/ast_switch.h"
#include "parser/ast_while.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_for.h"
#include "parser/ast_assign.h"
#include "parser/ast_var_def.h"
#include "sema_stmt_internal.h"
#include "core/vec.h"
#include <string.h>

flow_snap_t flow_capture(sema_t *sema, sema_scope_t *scope) {
  flow_snap_t snap = {0};
  allocator_t *alloc = scope ? scope->alloc : sema->vm->alloc;
  snap.alloc = alloc;

  /* 计数：沿 scope 链所有符号（含函数符号——函数 flow_init 恒 false，无害） */
  size_t cap = 0;
  for (const sema_scope_t *s = scope; s; s = s->parent)
    cap += strmap_size(s->symbols);
  if (cap == 0) return snap;

  snap.syms = allocator_new_ex(alloc, "flow_snap_syms", sizeof(sema_symbol_t *),
                               NULL, NULL, NULL, cap);
  snap.before = allocator_new_ex(alloc, "flow_snap_before", sizeof(bool), NULL,
                                 NULL, NULL, cap);
  snap.after = allocator_new_ex(alloc, "flow_snap_after", sizeof(bool), NULL,
                                NULL, NULL, cap);

  size_t i = 0;
  for (const sema_scope_t *s = scope; s; s = s->parent) {
    const vec_t *keys = strmap_keys(s->symbols);
    size_t nk = vec_len(keys);
    for (size_t k = 0; k < nk; k++) {
      const char *key = (const char *)vec_get(keys, k);
      sema_symbol_t *sym = (sema_symbol_t *)strmap_get(s->symbols, key);
      snap.syms[i] = sym;
      snap.before[i] = sym->flow_init;
      i++;
    }
  }
  snap.n = cap;
  return snap;
}

void flow_restore(const flow_snap_t *snap) {
  for (size_t i = 0; i < snap->n; i++)
    snap->syms[i]->flow_init = snap->before[i];
}

void flow_release(flow_snap_t *snap) {
  if (!snap || !snap->syms) return;
  allocator_free(snap->alloc, (void **)&snap->syms);
  allocator_free(snap->alloc, (void **)&snap->before);
  allocator_free(snap->alloc, (void **)&snap->after);
  snap->n = 0;
}

void name_to_cstr(strslice_t s, char *buf, size_t cap) {
  size_t n = s.len < cap - 1 ? s.len : cap - 1;
  memcpy(buf, s.ptr, n);
  buf[n] = '\0';
}



bool sema_scope_within(const sema_scope_t *a, const sema_scope_t *b) {
  for (const sema_scope_t *s = a; s; s = s->parent)
    if (s == b) return true;
  return false;
}
