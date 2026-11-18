#include "exec/stmt.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/scope.h"
#include "engine/value.h"
#include "exec/exec.h"
#include "parser/node.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_var.h"

clux_value_t *exec_stmt_var(clux_context_t *ctx, node_t *node) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  node_t *node_type = stmt_var_node_get_type(node);
  node_t *node_name = stmt_var_node_get_name(node);
  node_t *node_value = stmt_var_node_get_value(node);
  /* var x: T = undefined：右值是 undefined 字面量时创建 TDZ 变量——
     分配 T 的未初始化存储并标记（首赋值清除、消费拒绝）。
     类型标注必填：无标注无法确定存储类型。 */
  bool is_undefined =
      node_value && node_get_kind(node_value) == CLUX_NODE_LITERAL_UNDEFINED;
  if (is_undefined && !node_type) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node_value),
        "undefined initializer requires a type annotation");
  }
  clux_value_t *value = NULL;
  if (!is_undefined) {
    value = exec_operand(ctx, node_value);
    if (clux_value_is_error(value)) {
      return value;
    }
  }
  // TODO: 待call完成后实现装饰器
  /* 类型标注可省略（var x = 10 按初值自身类型落地）。
     有标注时求值类型表达式——必须产出 type value，
     其余值当 clux_type_t* 解引用是内存错误。 */
  clux_type_t *type = NULL;
  if (node_type) {
    clux_value_t *vtype = exec_operand(ctx, node_type);
    if (clux_value_is_error(vtype)) {
      return vtype;
    }
    if (clux_type_kind_unwrap(clux_value_get_type(vtype)) != CLUX_TYPE_TYPE) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(node_type),
          "expected type expression in var declaration");
    }
    /* shadow 已在上方拒绝，raw 必有真实存储 */
    type = *(clux_type_t **)clux_value_get_raw(vtype, 0);
  }
  char *name = location_get(node_get_location(node_name), allocator);
  if (is_undefined) {
    /* 类型标注已由上方的必填检查保证存在 */
    value = clux_context_create_tdz_value(
        ctx, type, name, node_get_location(node_value));
  } else {
    const location_t *loc = node_get_location(node_value);
    if (type) {
      value = clux_context_safe_cast(ctx, value, type, loc);
      if (clux_value_is_error(value)) {
        allocator_free(allocator, (void **)&name);
        return value;
      }
    }
    value = clux_context_declare_value(ctx, value, name, loc);
  }
  allocator_free(allocator, (void **)&name);
  return value;
}

/* 块出口协议：error / return signal / 最后语句值都注册在块作用域内，
   作用域销毁后继续引用即悬空——先回退作用域，再转换出值（error 借
   err 指针在外层重建，err 本体归 context 的 errors 向量，值销毁不动
   本体；signal 载荷与普通值克隆，与「函数返回一律 copy」的值语义
   一致）。
   栈平衡：从真实栈顶逐层走 pop_scope 直到块自身——任何未配对退出
   的中间层都走各自的 pop 通道销毁（不依赖 delete 跳层递归删，defer
   批次的挂点就在每层出口），以 global 兜底防越界挂死。块自身的
   销毁排在转换之后：先 swap 回外层（注册点转移，源值仍存活）转换
   出值，再 delete 块——此时块的 children 已被上方排干，delete 即
   本层的 pop，不做跳层递归删。空块产出 NULL（无值），唯一合法消费
   点是调用帧边界（报 missing return）。 */
clux_value_t *exec_stmt_block(clux_context_t *ctx, node_t *node) {
  clux_context_push_scope(ctx);
  clux_scope_t *block = clux_context_get_scope(ctx);
  clux_scope_t *outer = clux_scope_get_parent(block);
  clux_value_t *result = NULL;
  size_t count = stmt_block_node_get_stmt_count(node);
  for (size_t idx = 0; idx < count; idx++) {
    clux_value_t *value = exec_node(ctx, stmt_block_node_get_stmt(node, idx));
    result = value;
    if (clux_value_is_error(value) || clux_value_is_return_signal(value)) {
      break;
    }
  }
  clux_scope_t *current = clux_context_get_scope(ctx);
  while (current != block && current != clux_context_get_global_scope(ctx)) {
    clux_context_pop_scope(ctx);
    current = clux_context_get_scope(ctx);
  }
  (void)clux_context_swap_scope(ctx, outer);
  clux_value_t *out = NULL;
  if (clux_value_is_error(result)) {
    clux_error_t *err = *(clux_error_t **)clux_value_get_raw(result, 0);
    out = clux_context_create_error(ctx, err);
  } else if (clux_value_is_return_signal(result)) {
    clux_value_t *payload = clux_return_signal_get_payload(result);
    clux_value_t *cloned =
        payload ? clux_context_clone(ctx, payload, NULL) : NULL;
    out = clux_context_create_return_signal(ctx, cloned);
  } else if (result) {
    out = clux_context_clone(ctx, result, NULL);
  }
  delete_clux_scope(&block);
  return out;
}

/* 表达式语句 expr ';'：表达式值即语句值。 */
clux_value_t *exec_stmt_expr(clux_context_t *ctx, node_t *node) {
  node_t *expr = stmt_expr_node_get_expr(node);
  if (!expr) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "expression statement requires an expression");
  }
  return exec_operand(ctx, expr);
}

/* return; / return expr;（白皮书 §4.11）：产出 return signal（载荷为
   返回值的借用），语句层原样上抛，唯一消费点是调用帧边界。 */
clux_value_t *exec_stmt_return(clux_context_t *ctx, node_t *node) {
  clux_value_t *payload = NULL;
  node_t *value_node = stmt_return_node_get_value(node);
  if (value_node) {
    payload = exec_operand(ctx, value_node);
    if (clux_value_is_error(payload)) {
      return payload;
    }
  }
  return clux_context_create_return_signal(ctx, payload);
}
