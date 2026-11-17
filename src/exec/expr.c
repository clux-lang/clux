#include "exec/expr.h"
#include "core/allocator.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/exec.h"
#include "parser/node.h"
#include "parser/node_assign.h"
#include "parser/node_binary.h"
#include "parser/node_group.h"
#include "parser/node_ternary.h"
#include <stdbool.h>

clux_value_t *exec_expr_group(clux_context_t *ctx, node_t *node) {
  node_t *body = group_node_get_expr(node);
  clux_value_t *value = exec_node(ctx, body);
  if (clux_value_is_shadow(value)) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(body),
        "shadow value is not allowed in execution");
  }
  return value;
}

/* 二元运算符文本→引擎槽位；逻辑 &&/|| 无槽位由 exec_expr_binary 自管短路。 */
static const struct {
  const char *text;
  clux_binary_op_t op;
} k_binary_ops[] = {
    {"+", CLUX_OP_ADD},
    {"-", CLUX_OP_SUB},
    {"*", CLUX_OP_MUL},
    {"/", CLUX_OP_DIV},
    {"%", CLUX_OP_MOD},
    {"&", CLUX_OP_AND},
    {"|", CLUX_OP_OR},
    {"^", CLUX_OP_XOR},
    {"<<", CLUX_OP_SHL},
    {">>", CLUX_OP_SHR},
    {"==", CLUX_OP_EQ},
    {"!=", CLUX_OP_NE},
    {"<", CLUX_OP_LT},
    {"<=", CLUX_OP_LE},
    {">", CLUX_OP_GT},
    {">=", CLUX_OP_GE},
};

/* 前缀一元运算符文本→引擎槽位。 */
static const struct {
  const char *text;
  clux_prefix_op_t op;
} k_prefix_ops[] = {
    {"+", CLUX_OP_PLUS},
    {"-", CLUX_OP_NEGATIVE},
    {"~", CLUX_OP_NOT},
    {"!", CLUX_OP_LNOT},
};

clux_value_t *exec_expr_binary(clux_context_t *ctx, node_t *node) {
  node_t *left_node = binary_node_get_left(node);
  node_t *symbol_node = binary_node_get_symbol(node);
  node_t *right_node = binary_node_get_right(node);
  const location_t *op_loc = node_get_location(symbol_node);
  allocator_t *allocator = clux_context_get_allocator(ctx);
  /* 前缀一元形态：LEFT 缺失，运算符作用于 RIGHT。
     后缀形态与成员访问留待后续批次，落入 unsupported。 */
  if (!left_node) {
    for (size_t i = 0; i < sizeof(k_prefix_ops) / sizeof(k_prefix_ops[0]);
         i++) {
      if (!location_is(op_loc, k_prefix_ops[i].text)) {
        continue;
      }
      clux_value_t *operand = exec_operand(ctx, right_node);
      if (clux_value_is_error(operand)) {
        return operand;
      }
      return clux_context_prefix(ctx, k_prefix_ops[i].op, operand, op_loc);
    }
    char *op = location_get(op_loc, allocator);
    clux_value_t *err = clux_context_create_error_message(
        ctx, op_loc, "unsupported unary operator '%s'", op);
    allocator_free(allocator, (void **)&op);
    return err;
  }
  /* && / || 短路：左值已定结果（&& 为 false / || 为 true）时直接返回左值，
     否则求值右值并要求 bool；两侧类型均须为 bool。 */
  bool is_and = location_is(op_loc, "&&");
  bool is_or = location_is(op_loc, "||");
  if (is_and || is_or) {
    const char *op_name = is_and ? "&&" : "||";
    clux_value_t *left = exec_operand(ctx, left_node);
    if (clux_value_is_error(left)) {
      return left;
    }
    clux_type_t *ltype = clux_value_get_type(left);
    if (clux_type_get_kind(ltype) != CLUX_TYPE_BOOL) {
      const char *ltype_name = clux_type_get_name(ltype);
      return clux_context_create_error_message(
          ctx,
          op_loc,
          "type '%s' does not support operator '%s'",
          ltype_name ? ltype_name : "<unknown>",
          op_name);
    }
    /* && 左值为 false 已定结果短路；|| 左值为 true 已定结果短路 */
    bool lval = *(bool *)clux_value_get_raw(left, 0);
    if (is_and ? !lval : lval) {
      return left;
    }
    clux_value_t *right = exec_operand(ctx, right_node);
    if (clux_value_is_error(right)) {
      return right;
    }
    clux_type_t *rtype = clux_value_get_type(right);
    if (clux_type_get_kind(rtype) != CLUX_TYPE_BOOL) {
      const char *rtype_name = clux_type_get_name(rtype);
      return clux_context_create_error_message(
          ctx,
          op_loc,
          "type '%s' does not support operator '%s'",
          rtype_name ? rtype_name : "<unknown>",
          op_name);
    }
    return right;
  }
  /* 二元形态：文本映射引擎槽位后交 clux_context_binary 分派。 */
  for (size_t i = 0; i < sizeof(k_binary_ops) / sizeof(k_binary_ops[0]); i++) {
    if (!location_is(op_loc, k_binary_ops[i].text)) {
      continue;
    }
    clux_value_t *left = exec_operand(ctx, left_node);
    if (clux_value_is_error(left)) {
      return left;
    }
    clux_value_t *right = exec_operand(ctx, right_node);
    if (clux_value_is_error(right)) {
      return right;
    }
    return clux_context_binary(ctx, k_binary_ops[i].op, left, right, op_loc);
  }
  char *op = location_get(op_loc, allocator);
  clux_value_t *err = clux_context_create_error_message(
      ctx, op_loc, "unsupported binary operator '%s'", op);
  allocator_free(allocator, (void **)&op);
  return err;
}

/* &&= / ||= 短路复合赋值：左值已是结果时跳过右值求值直接返回目标；
   求值的右值 error 优先传播，再检查 bool 类型。
   shadow 目标已在调用入口拒绝，此处可安全读左值。 */
static clux_value_t *exec_assign_logical(clux_context_t *ctx,
                                         clux_value_t *target,
                                         node_t *value_node,
                                         const location_t *op_loc,
                                         const char *op_name,
                                         bool is_and) {
  bool lval = *(bool *)clux_value_get_raw(target, 0);
  /* &&= 左值为 false 已定结果短路；||= 左值为 true 已定结果短路 */
  if (is_and ? !lval : lval) {
    return target;
  }
  clux_value_t *value = exec_operand(ctx, value_node);
  if (clux_value_is_error(value)) {
    return value;
  }
  clux_type_t *rtype = clux_value_get_type(value);
  if (clux_type_get_kind(rtype) != CLUX_TYPE_BOOL) {
    const char *rtype_name = clux_type_get_name(rtype);
    return clux_context_create_error_message(
        ctx,
        op_loc,
        "type '%s' does not support operator '%s'",
        rtype_name ? rtype_name : "<unknown>",
        op_name);
  }
  return clux_context_assign(ctx, target, value, op_loc);
}

clux_value_t *exec_expr_assign(clux_context_t *ctx, node_t *node) {
  node_t *target_node = assign_node_get_left(node);
  /* 左侧括号分组逐层解包：(x) = 5 取 x 而非 "(x)" 文本 */
  while (node_get_kind(target_node) == CLUX_NODE_EXPR_GROUP) {
    target_node = group_node_get_expr(target_node);
  }
  node_t *op_node = assign_node_get_symbol(node);
  const location_t *op_loc = node_get_location(op_node);
  node_t *value_node = assign_node_get_value(node);
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(target_node), allocator);
  clux_value_t *target =
      clux_context_lookup(ctx, name, node_get_location(target_node));
  allocator_free(allocator, (void **)&name);
  if (clux_value_is_error(target)) {
    return target;
  }
  /* exec 是真实执行：shadow 值只存在于 sema 的影子执行，禁止出现 */
  if (clux_value_is_shadow(target)) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(target_node),
        "shadow value is not allowed in execution");
  }
  /* 复合赋值（含 &&= / ||=）读取目标当前值：TDZ 拒绝；
     纯 "=" 是首次赋值通道，TDZ 合法（由 clux_context_assign 旁路
     mutable 检查并在写入后清除标记）。 */
  if (!location_is(op_loc, "=") && clux_value_is_tdz(target)) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(target_node),
        "use of a value before initialization");
  }
  clux_type_t *type = clux_value_get_type(target);
  if (location_is(op_loc, "&&=")) {
    if (clux_type_get_kind(type) != CLUX_TYPE_BOOL) {
      return clux_context_create_error_message(
          ctx,
          op_loc,
          "type '%s' does not support operator '&&='",
          clux_type_get_name(type));
    }
    return exec_assign_logical(ctx, target, value_node, op_loc, "&&=", true);
  }
  if (location_is(op_loc, "||=")) {
    if (clux_type_get_kind(type) != CLUX_TYPE_BOOL) {
      return clux_context_create_error_message(
          ctx,
          op_loc,
          "type '%s' does not support operator '||='",
          clux_type_get_name(type));
    }
    return exec_assign_logical(ctx, target, value_node, op_loc, "||=", false);
  }
  clux_value_t *value = exec_operand(ctx, value_node);
  if (clux_value_is_error(value)) {
    return value;
  }
  if (location_is(op_loc, "=")) {
    value =
        clux_context_safe_cast(ctx, value, type, node_get_location(value_node));
  } else if (location_is(op_loc, "+=")) {
    value = clux_context_binary(ctx, CLUX_OP_ADD, target, value, op_loc);
  } else if (location_is(op_loc, "-=")) {
    value = clux_context_binary(ctx, CLUX_OP_SUB, target, value, op_loc);
  } else if (location_is(op_loc, "*=")) {
    value = clux_context_binary(ctx, CLUX_OP_MUL, target, value, op_loc);
  } else if (location_is(op_loc, "/=")) {
    value = clux_context_binary(ctx, CLUX_OP_DIV, target, value, op_loc);
  } else if (location_is(op_loc, "%=")) {
    value = clux_context_binary(ctx, CLUX_OP_MOD, target, value, op_loc);
  } else if (location_is(op_loc, "&=")) {
    value = clux_context_binary(ctx, CLUX_OP_AND, target, value, op_loc);
  } else if (location_is(op_loc, "|=")) {
    value = clux_context_binary(ctx, CLUX_OP_OR, target, value, op_loc);
  } else if (location_is(op_loc, "^=")) {
    value = clux_context_binary(ctx, CLUX_OP_XOR, target, value, op_loc);
  } else if (location_is(op_loc, "<<=")) {
    value = clux_context_binary(ctx, CLUX_OP_SHL, target, value, op_loc);
  } else if (location_is(op_loc, ">>=")) {
    value = clux_context_binary(ctx, CLUX_OP_SHR, target, value, op_loc);
  } else {
    char *op = location_get(op_loc, allocator);
    clux_value_t *err = clux_context_create_error_message(
        ctx, op_loc, "unsupported assign operator '%s'", op);
    allocator_free(allocator, (void **)&op);
    return err;
  }
  return clux_context_assign(ctx, target, value, node_get_location(node));
}

clux_value_t *exec_expr_ternary(clux_context_t *ctx, node_t *node) {
  node_t *cond_node = ternary_node_get_condition(node);
  node_t *then_node = ternary_node_get_then(node);
  node_t *else_node = ternary_node_get_else(node);
  clux_value_t *cond = exec_operand(ctx, cond_node);
  if (clux_value_is_error(cond)) {
    return cond;
  }
  clux_type_t *type = clux_value_get_type(cond);
  if (clux_type_get_kind(type) != CLUX_TYPE_BOOL) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        ctx,
        node_get_location(cond_node),
        "type '%s' does not support operator '?:'",
        type_name ? type_name : "<unknown>");
  }
  bool cond_bl = *(bool *)clux_value_get_raw(cond, 0);
  if (cond_bl) {
    return exec_operand(ctx, then_node);
  }
  return exec_operand(ctx, else_node);
}