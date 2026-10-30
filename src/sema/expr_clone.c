#include "sema/sema.h"

#include "parser/ast_call.h"
#include "parser/ast_ident.h"
#include "parser/ast_type_ref.h"

#include <string.h>

/* arena 持久化 NUL 串 → strslice */
static strslice_t clone_cstr(sema_t *sema, const char *s) {
  size_t n = strlen(s);
  char *buf = (char *)arena_calloc(sema->arena, n + 1, 1,
                                   ALIGNOF(max_align_t));
  if (!buf) return STRSLICE_EMPTY;
  memcpy(buf, s, n);
  return strslice_from_bytes(buf, n);
}

/* 类型引用表达式：登记类型 → AST_TYPE_REF <__type_N>；内建标量走规范名
   （compiler 的 AST_TYPE_REF 分支对未登记名用 type_lookup 兜底）。 */
ast_node_t *sema_gen_type_ref(sema_t *sema, const type_t *t) {
  if (!t) return NULL;
  ast_node_t *n = ast_type_ref_new(sema->arena, 0, 0);
  if (!n) return NULL;
  n->is_generated = true;
  const sema_type_t *st = sema_type_register(sema, t);
  if (st) {
    ((ast_type_ref_t *)n)->name = st->name;
  } else {
    char buf[128];
    sema_type_name(t, buf, sizeof buf);
    ((ast_type_ref_t *)n)->name = clone_cstr(sema, buf);
  }
  return n;
}

/* `__deep_clone(v)` 调用节点：运行期对象图深拷贝（闭包 env 的唯一入口）。 */
static ast_node_t *gen_deep_clone_call(sema_t *sema, ast_node_t *access) {
  if (!sema || !access) return NULL;
  ast_node_t *callee = ast_ident_new(sema->arena, 0, 0);
  ast_node_t *call = ast_call_new(sema->arena, 0, 0);
  if (!callee || !call) return NULL;
  ((ast_ident_t *)callee)->name = strslice_from_bytes("__deep_clone", 12);
  callee->is_generated = true;
  ((ast_call_t *)call)->callee = callee;
  ((ast_call_t *)call)->args = access;
  call->is_generated = true;
  return call;
}

/* 公共入口：生成 `__deep_clone(v)`（闭包值拷贝/运行期深拷贝）。
   用于 sema 在"闭包值拷贝 = 深拷贝 env"处插入（字面量不需要）。 */
ast_node_t *sema_gen_deep_clone(sema_t *sema, ast_node_t *value) {
  return gen_deep_clone_call(sema, value);
}
