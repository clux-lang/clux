#include "parser/ast_walk.h"

#include "parser/ast_addr.h"
#include "parser/ast_alignof.h"
#include "parser/ast_array.h"
#include "parser/ast_binary.h"
#include "parser/ast_call.h"
#include "parser/ast_const.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_deref.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_fill.h"
#include "parser/ast_func_type.h"
#include "parser/ast_index.h"
#include "parser/ast_member.h"
#include "parser/ast_make.h"
#include "parser/ast_option.h"
#include "parser/ast_take.h"
#include "parser/ast_ptr.h"
#include "parser/ast_sizeof.h"
#include "parser/ast_slice.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_ternary.h"
#include "parser/ast_tuple.h"
#include "parser/ast_typeof.h"
#include "parser/ast_unary.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_volatile.h"

/* 兄弟链上的每个节点都是父节点的直接子节点（参数表/元素表/字段表/实参表/
   下标表）。链上节点自身的兄弟不重复遍历——由本函数统一迭代。 */
static bool visit_chain(ast_node_t *head, ast_visit_fn visit, void *user) {
  for (ast_node_t *n = head; n; n = n->next) {
    if (!ast_walk_preorder(n, visit, user)) return false;
  }
  return true;
}

bool ast_visit_children(ast_node_t *node, ast_visit_fn visit, void *user) {
  if (!node) return true;
  switch (node->kind) {
    /* ---- 类型表达式 ---- */
    case AST_ARRAY: {
      ast_array_t *n = (ast_array_t *)node;
      return ast_walk_preorder(n->length, visit, user) &&
             ast_walk_preorder(n->base_type, visit, user);
    }
    case AST_OPTION:
      return ast_walk_preorder(((ast_option_t *)node)->sub, visit, user);
    case AST_CONST:
      return ast_walk_preorder(((ast_const_t *)node)->sub, visit, user);
    case AST_VOLATILE:
      return ast_walk_preorder(((ast_volatile_t *)node)->sub, visit, user);
    case AST_PTR:
      return ast_walk_preorder(((ast_ptr_t *)node)->base_type, visit, user);
    case AST_TAKE:
      return ast_walk_preorder(((ast_take_t *)node)->operand, visit, user);
    case AST_SLICE_TYPE:
      return ast_walk_preorder(((ast_slice_type_t *)node)->elem_type, visit,
                               user);
    case AST_TUPLE:
      return visit_chain(((ast_tuple_t *)node)->elem_types, visit, user);
    case AST_FUNC_TYPE: {
      ast_func_type_t *n = (ast_func_type_t *)node;
      return visit_chain(n->params, visit, user) &&
             ast_walk_preorder(n->return_type, visit, user);
    }

    /* ---- 编译期操作符（sizeof/alignof/typeof 的操作数是子节点） ---- */
    case AST_SIZEOF:
      return ast_walk_preorder(((ast_sizeof_t *)node)->operand, visit, user);
    case AST_ALIGNOF:
      return ast_walk_preorder(((ast_alignof_t *)node)->operand, visit, user);
    case AST_TYPEOF:
      return ast_walk_preorder(((ast_typeof_t *)node)->operand, visit, user);

    /* ---- 值表达式 ---- */
    case AST_BINARY: {
      ast_binary_t *n = (ast_binary_t *)node;
      return ast_walk_preorder(n->lhs, visit, user) &&
             ast_walk_preorder(n->rhs, visit, user);
    }
    case AST_UNARY:
      return ast_walk_preorder(((ast_unary_t *)node)->operand, visit, user);
    case AST_TERNARY: {
      ast_ternary_t *n = (ast_ternary_t *)node;
      return ast_walk_preorder(n->cond, visit, user) &&
             ast_walk_preorder(n->then_branch, visit, user) &&
             ast_walk_preorder(n->else_branch, visit, user);
    }
    case AST_CALL: {
      ast_call_t *n = (ast_call_t *)node;
      return ast_walk_preorder(n->callee, visit, user) &&
             visit_chain(n->args, visit, user);
    }
    case AST_CONSTRUCT: {
      ast_construct_t *n = (ast_construct_t *)node;
      return ast_walk_preorder(n->type, visit, user) &&
             visit_chain(n->fields, visit, user);
    }
    case AST_CONSTRUCT_FIELD:
      /* 具名字段（name = value）：类型检查前 fields 链上是它，之后是裸值 */
      return ast_walk_preorder(((ast_construct_field_t *)node)->value, visit,
                               user);
    case AST_FILL: {
      ast_fill_t *n = (ast_fill_t *)node;
      return ast_walk_preorder(n->value, visit, user) &&
             ast_walk_preorder(n->count, visit, user);
    }
    case AST_INDEX: {
      ast_index_t *n = (ast_index_t *)node;
      return ast_walk_preorder(n->object, visit, user) &&
             visit_chain(n->indices, visit, user);
    }
    case AST_MEMBER:
      return ast_walk_preorder(((ast_member_t *)node)->object, visit, user);
    /* 以下三种一度漏在 switch 之外（落入 default 被当叶子），它们的子节点是
       普通表达式 —— `var f = .Box{ .cb = func(){ … } };` / `make(T, N, f())` /
       `a[lo:hi]` 里的函数字面量因此遍历不到（前脱糖漏下钻 → 闭包里的 foreach
       不展开）。type_rhs_uses_var / probe_type_slot 的谓词只关心类型表达式，
       补全不影响它们；补全是纯增量。 */
    case AST_MAKE: { /* make(pointer, len) */
      ast_make_t *n = (ast_make_t *)node;
      return ast_walk_preorder(n->pointer, visit, user) &&
             ast_walk_preorder(n->length, visit, user);
    }
    case AST_SLICE: { /* expr[low:high]（low/high 可 NULL） */
      ast_slice_t *n = (ast_slice_t *)node;
      return ast_walk_preorder(n->object, visit, user) &&
             ast_walk_preorder(n->low, visit, user) &&
             ast_walk_preorder(n->high, visit, user);
    }
    case AST_UNWRAP:
      return ast_walk_preorder(((ast_unwrap_t *)node)->operand, visit, user);
    case AST_ADDR:
      return ast_walk_preorder(((ast_addr_t *)node)->operand, visit, user);
    case AST_DEREF:
      return ast_walk_preorder(((ast_deref_t *)node)->operand, visit, user);
    case AST_SCOPE_REF:
      return ast_walk_preorder(((ast_scope_ref_t *)node)->type_expr, visit,
                               user);

    default:
      /* 叶子：字面量 / 标识符 / 类型引用 / nil / undefined / error … */
      return true;
  }
}

bool ast_walk_preorder(ast_node_t *node, ast_visit_fn visit, void *user) {
  if (!node) return true;
  ast_walk_action_t act = visit(node, user);
  if (act == AST_WALK_STOP) return false;
  if (act == AST_WALK_SKIP_CHILDREN) return true;
  return ast_visit_children(node, visit, user);
}
