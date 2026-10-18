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
#include "parser/ast_move.h"
#include "parser/ast_option.h"
#include "parser/ast_is_tag.h"
#include "parser/ast_opt_is_some.h"
#include "parser/ast_clone.h"
#include "parser/ast_take.h"
#include "parser/ast_take_slot.h"
#include "parser/ast_owned_type.h"
#include "parser/ast_ptr.h"
#include "parser/ast_sizeof.h"
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
    case AST_IS_TAG:
      return ast_walk_preorder(((ast_is_tag_t *)node)->object, visit, user);
    case AST_OPT_IS_SOME:
      return ast_walk_preorder(((ast_opt_is_some_t *)node)->object, visit,
                               user);
    case AST_CLONE:
      return ast_walk_preorder(((ast_clone_t *)node)->operand, visit, user);
    case AST_TAKE:
      return ast_walk_preorder(((ast_take_t *)node)->operand, visit, user);
    case AST_TAKE_SLOT:
      return ast_walk_preorder(((ast_take_slot_t *)node)->operand, visit, user);
    case AST_OWNED_TYPE:
      return ast_walk_preorder(((ast_owned_type_t *)node)->base_type, visit,
                               user);
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
    case AST_UNWRAP:
      return ast_walk_preorder(((ast_unwrap_t *)node)->operand, visit, user);
    case AST_ADDR:
      return ast_walk_preorder(((ast_addr_t *)node)->operand, visit, user);
    case AST_DEREF:
      return ast_walk_preorder(((ast_deref_t *)node)->operand, visit, user);
    case AST_MOVE:
      return ast_walk_preorder(((ast_move_t *)node)->operand, visit, user);
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
  if (visit(node, user) == AST_WALK_STOP) return false;
  return ast_visit_children(node, visit, user);
}
