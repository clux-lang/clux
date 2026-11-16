/* 语句执行：块作用域进出、控制流信号传播与吸收。
   块是信号的透传通道（return/break/continue 穿块而过），循环是
   break/continue 的吸收点；块的求值不产生跨作用域的值——块值语义
   仅块内有效，块的 exec 结果为无值。 */
#include "exec_internal.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_empty.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_if.h"
#include "parser/node_stmt_jump.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_while.h"

exec_value_t exec_visit_block(clux_exec_t *self, node_t *node) {
  clux_context_push_scope(self->ctx);
  exec_value_t result = exec_ok(NULL);
  vec_t *children = node_get_children(node);
  size_t count = vec_get_size(children);
  for (size_t idx = 0; idx < count; idx++) {
    result = exec_dispatch_stmt(self, vec_get(children, idx));
    /* 硬错误与控制流信号立即透传，由外层吸收点处理；
       正常值对本块无意义（块不是表达式），继续执行下一条。 */
    if (exec_is_error(result) || result.tag != EXEC_VALUE_TAG) {
      break;
    }
  }
  clux_context_pop_scope(self->ctx);
  /* 块内产出的值随块作用域销毁，块不是表达式：正常结果一律无值，
     控制流信号（其 RETURN 载荷属于外层作用域的求值产物）原样透传。 */
  return result.tag == EXEC_VALUE_TAG ? exec_ok(NULL) : result;
}

exec_value_t exec_visit_stmt_expr(clux_exec_t *self, node_t *node) {
  node_t *expr = stmt_expr_node_get_expr(node);
  if (!expr) {
    return exec_errorf(self, exec_loc(node), "empty expression statement");
  }
  return exec_dispatch_expr(self, expr);
}

exec_value_t exec_visit_empty(clux_exec_t *self, node_t *node) {
  (void)node;
  (void)self;
  return exec_ok(NULL);
}

exec_value_t exec_visit_return(clux_exec_t *self, node_t *node) {
  node_t *value = stmt_return_node_get_value(node);
  if (!value) {
    return exec_signal(EXEC_SIGNAL_RETURN, NULL);
  }
  exec_value_t result = exec_dispatch_expr(self, value);
  if (exec_is_error(result)) {
    return result;
  }
  return exec_signal(EXEC_SIGNAL_RETURN, result.value);
}

exec_value_t exec_visit_break(clux_exec_t *self, node_t *node) {
  (void)node;
  (void)self;
  return exec_signal(EXEC_SIGNAL_BREAK, NULL);
}

exec_value_t exec_visit_continue(clux_exec_t *self, node_t *node) {
  (void)node;
  (void)self;
  return exec_signal(EXEC_SIGNAL_CONTINUE, NULL);
}

exec_value_t exec_visit_if(clux_exec_t *self, node_t *node) {
  node_t *condition = stmt_if_node_get_condition(node);
  node_t *then_branch = stmt_if_node_get_then(node);
  node_t *else_branch = stmt_if_node_get_else(node);
  if (!condition || !then_branch) {
    return exec_errorf(self, exec_loc(node), "if is incomplete");
  }
  exec_value_t cond = exec_dispatch_expr(self, condition);
  if (exec_is_error(cond)) {
    return cond;
  }
  exec_value_t truth =
      exec_cond_bool(self, cond.value, exec_loc(condition));
  if (exec_is_error(truth)) {
    return truth;
  }
  bool taken = *(bool *)ptr_to_raw(clux_value_get_data(truth.value), 0);
  if (taken) {
    return exec_dispatch_stmt(self, then_branch);
  }
  if (else_branch) {
    return exec_dispatch_stmt(self, else_branch);
  }
  return exec_ok(NULL);
}

exec_value_t exec_visit_while(clux_exec_t *self, node_t *node) {
  node_t *condition = stmt_while_node_get_condition(node);
  node_t *body = stmt_while_node_get_body(node);
  if (!condition || !body) {
    return exec_errorf(self, exec_loc(node), "while is incomplete");
  }
  for (;;) {
    exec_value_t cond = exec_dispatch_expr(self, condition);
    if (exec_is_error(cond)) {
      return cond;
    }
    exec_value_t truth =
        exec_cond_bool(self, cond.value, exec_loc(condition));
    if (exec_is_error(truth)) {
      return truth;
    }
    bool taken = *(bool *)ptr_to_raw(clux_value_get_data(truth.value), 0);
    if (!taken) {
      return exec_ok(NULL);
    }
    exec_value_t round = exec_dispatch_stmt(self, body);
    if (exec_is_error(round)) {
      return round;
    }
    if (round.tag == EXEC_SIGNAL_BREAK) {
      return exec_ok(NULL);
    }
    /* CONTINUE 与正常值进入下一轮；RETURN 等其余信号透传。 */
    if (round.tag != EXEC_SIGNAL_CONTINUE &&
        round.tag != EXEC_VALUE_TAG) {
      return round;
    }
  }
}
