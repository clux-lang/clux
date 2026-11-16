/* 表达式求值：二元/一元前缀/一元后缀共用 binary 节点形态（缺失的一侧
   为 NULL），assign 左子树任意表达式（左值是 produce 可写 view 的
   子表达式：identifier/field/index/deref），&&/|| 在执行器层短路——
   engine 不感知求值顺序。 */
#include "exec_internal.h"
#include "parser/node_assign.h"
#include "parser/node_binary.h"
#include "parser/node_group.h"
#include "parser/node_ternary.h"
#include <string.h>

exec_value_t exec_visit_group(clux_exec_t *self, node_t *node) {
  node_t *expr = group_node_get_expr(node);
  if (!expr) {
    return exec_errorf(self, exec_loc(node), "empty group");
  }
  return exec_dispatch_expr(self, expr);
}

exec_value_t exec_visit_ternary(clux_exec_t *self, node_t *node) {
  node_t *condition = ternary_node_get_condition(node);
  node_t *then_branch = ternary_node_get_then(node);
  node_t *else_branch = ternary_node_get_else(node);
  if (!condition || !then_branch || !else_branch) {
    return exec_errorf(
        self, exec_loc(node), "incomplete ternary expression");
  }
  exec_value_t cond = exec_dispatch_expr(self, condition);
  if (exec_is_error(cond)) {
    return cond;
  }
  exec_value_t truth = exec_cond_bool(self, cond.value,
                                      exec_loc(condition));
  if (exec_is_error(truth)) {
    return truth;
  }
  bool taken = *(bool *)ptr_to_raw(clux_value_get_data(truth.value), 0);
  return exec_dispatch_expr(self, taken ? then_branch : else_branch);
}

/* 二元运算符文本 -> engine 运算符；返回 false 表示非简单二元
   （短路逻辑或未知符号由调用方处理）。 */
static bool map_binary_op(const char *symbol, clux_binary_op_t *op) {
  struct entry {
    const char *text;
    clux_binary_op_t op;
  };
  static const struct entry table[] = {
      {"+", CLUX_OP_ADD},    {"-", CLUX_OP_SUB},    {"*", CLUX_OP_MUL},
      {"/", CLUX_OP_DIV},    {"%", CLUX_OP_MOD},    {"<<", CLUX_OP_SHL},
      {">>", CLUX_OP_SHR},   {"&", CLUX_OP_AND},    {"|", CLUX_OP_OR},
      {"^", CLUX_OP_XOR},    {"==", CLUX_OP_EQ},    {"!=", CLUX_OP_NE},
      {"<", CLUX_OP_LT},     {"<=", CLUX_OP_LE},    {">", CLUX_OP_GT},
      {">=", CLUX_OP_GE},
  };
  for (size_t idx = 0; idx < sizeof(table) / sizeof(table[0]); idx++) {
    if (strcmp(symbol, table[idx].text) == 0) {
      *op = table[idx].op;
      return true;
    }
  }
  return false;
}

static exec_value_t short_circuit(clux_exec_t *self,
                                  node_t *node,
                                  node_t *left,
                                  node_t *right,
                                  bool is_and) {
  (void)node; /* 出错定位由操作数各自携带 */
  exec_value_t lvalue = exec_dispatch_expr(self, left);
  if (exec_is_error(lvalue)) {
    return lvalue;
  }
  exec_value_t truth = exec_cond_bool(self, lvalue.value,
                                      exec_loc(left));
  if (exec_is_error(truth)) {
    return truth;
  }
  bool taken = *(bool *)ptr_to_raw(clux_value_get_data(truth.value), 0);
  if (is_and ? !taken : taken) {
    return exec_ok(clux_context_create_bool(self->ctx, taken, NULL, NULL));
  }
  exec_value_t rvalue = exec_dispatch_expr(self, right);
  if (exec_is_error(rvalue)) {
    return rvalue;
  }
  return exec_cond_bool(self, rvalue.value, exec_loc(right));
}

/* 一元前缀：! ~ + -（engine prefix 通道）。 */
static exec_value_t exec_prefix(clux_exec_t *self,
                                node_t *node,
                                node_t *symbol,
                                node_t *operand) {
  clux_prefix_op_t op;
  if (exec_node_is(symbol, "!")) {
    op = CLUX_OP_LNOT;
  } else if (exec_node_is(symbol, "~")) {
    op = CLUX_OP_NOT;
  } else if (exec_node_is(symbol, "+")) {
    op = CLUX_OP_PLUS;
  } else if (exec_node_is(symbol, "-")) {
    op = CLUX_OP_NEGATIVE;
  } else {
    return exec_errorf(self,
                       exec_loc(node),
                       "unknown prefix operator");
  }
  exec_value_t value = exec_dispatch_expr(self, operand);
  if (exec_is_error(value)) {
    return value;
  }
  return exec_ok(clux_context_prefix(
      self->ctx, op, value.value, exec_loc(node)));
}

/* 一元后缀：.& .* .! （.? 尚无引擎原语）。 */
static exec_value_t exec_postfix(clux_exec_t *self,
                                 node_t *node,
                                 node_t *symbol,
                                 node_t *operand) {
  exec_value_t value = exec_dispatch_expr(self, operand);
  if (exec_is_error(value)) {
    return value;
  }
  location_t *loc = exec_loc(node);
  if (exec_node_is(symbol, ".&")) {
    return exec_ok(clux_context_ref(self->ctx, value.value, loc));
  }
  if (exec_node_is(symbol, ".*")) {
    return exec_ok(clux_context_deref(self->ctx, value.value, loc));
  }
  if (exec_node_is(symbol, ".!")) {
    return exec_ok(clux_context_take(self->ctx, value.value, loc));
  }
  return exec_errorf(self, loc, "unknown postfix operator");
}

exec_value_t exec_visit_binary(clux_exec_t *self, node_t *node) {
  node_t *left = binary_node_get_left(node);
  node_t *right = binary_node_get_right(node);
  node_t *symbol = binary_node_get_symbol(node);
  if (!symbol) {
    return exec_errorf(
        self, exec_loc(node), "operator symbol is missing");
  }
  if (!left && right) {
    return exec_prefix(self, node, symbol, right);
  }
  if (left && !right) {
    return exec_postfix(self, node, symbol, left);
  }
  if (!left || !right) {
    return exec_errorf(
        self, exec_loc(node), "binary operands are incomplete");
  }

  char *text = exec_node_text(self, symbol);
  exec_value_t result = {EXEC_VALUE_TAG, NULL};
  bool is_and = strcmp(text, "&&") == 0;
  bool is_or = strcmp(text, "||") == 0;
  if (is_and || is_or) {
    result = short_circuit(self, node, left, right, is_and);
  } else {
    clux_binary_op_t op;
    if (!map_binary_op(text, &op)) {
      allocator_free(self->allocator, (void **)&text);
      return exec_errorf(self,
                         exec_loc(symbol),
                         "unknown binary operator");
    }
    exec_value_t lvalue = exec_dispatch_expr(self, left);
    if (exec_is_error(lvalue)) {
      allocator_free(self->allocator, (void **)&text);
      return lvalue;
    }
    exec_value_t rvalue = exec_dispatch_expr(self, right);
    if (exec_is_error(rvalue)) {
      allocator_free(self->allocator, (void **)&text);
      return rvalue;
    }
    result = exec_ok(clux_context_binary(self->ctx,
                                         op,
                                         lvalue.value,
                                         rvalue.value,
                                         exec_loc(node)));
  }
  allocator_free(self->allocator, (void **)&text);
  return result;
}

exec_value_t exec_visit_assign(clux_exec_t *self, node_t *node) {
  node_t *left = assign_node_get_left(node);
  node_t *value_node = assign_node_get_value(node);
  node_t *symbol = assign_node_get_symbol(node);
  if (!left || !value_node) {
    return exec_errorf(
        self, exec_loc(node), "assignment is incomplete");
  }
  if (!exec_node_is(symbol, "=")) {
    return exec_errorf(
        self, exec_loc(node), "compound assignment is not supported");
  }
  exec_value_t target = exec_dispatch_expr(self, left);
  if (exec_is_error(target)) {
    return target;
  }
  exec_value_t value = exec_dispatch_expr(self, value_node);
  if (exec_is_error(value)) {
    return value;
  }
  clux_value_t *written = clux_context_assign(self->ctx,
                                              target.value,
                                              value.value,
                                              exec_loc(node));
  return exec_ok(written);
}
