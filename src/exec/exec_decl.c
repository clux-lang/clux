/* 声明落地：声明即执行。变量语句求初值、按标注收窄后经
   clux_context_declare_value 落名——匿名初值原地具名化（声明即
   move），借用初值按值拷贝为独立存储，重名报 duplicate declaration。 */
#include "exec_internal.h"
#include "parser/node_stmt_var.h"

exec_value_t exec_visit_var(clux_exec_t *self, node_t *node) {
  node_t *value_node = stmt_var_node_get_value(node);
  node_t *type_node = stmt_var_node_get_type(node);
  node_t *name_node = stmt_var_node_get_name(node);
  if (!value_node || !name_node) {
    return exec_errorf(
        self, exec_loc(node), "variable declaration is incomplete");
  }

  exec_value_t value = exec_dispatch_expr(self, value_node);
  if (exec_is_error(value)) {
    return value;
  }

  /* 显式类型标注先收窄（safe_cast 链路含协变/鸭子隐式转换），
   * 未标注时直接按初值类型落地（类型推断）。 */
  if (type_node) {
    exec_value_t declared = exec_dispatch_expr(self, type_node);
    if (exec_is_error(declared)) {
      return declared;
    }
    clux_type_t *target = exec_type_of_value(declared.value);
    if (!target) {
      return exec_errorf(
          self, exec_loc(type_node), "variable annotation requires a type");
    }
    clux_value_t *narrowed =
        clux_context_safe_cast(self->ctx, value.value, target, exec_loc(node));
    if (exec_is_error_value(narrowed)) {
      return exec_err(narrowed);
    }
    value.value = narrowed;
  }

  /* 落地：匿名初值在 context 侧具名化（声明即 move）；
     借用初值由 context 按值拷贝为独立存储。 */
  if (clux_value_is_shadow(value.value)) {
    return exec_errorf(
        self, exec_loc(node), "shadow values are not executable");
  }
  char *name = exec_node_text(self, name_node);
  clux_value_t *declared =
      clux_context_declare_value(self->ctx, value.value, name, exec_loc(node));
  allocator_free(self->allocator, (void **)&name);
  if (exec_is_error_value(declared)) {
    return exec_err(declared);
  }
  /* 声明语句不产值：落地值已按名注册进作用域，无值结果让
     clux_exec_run 不把声明计入"最后一条有值语句"。 */
  return exec_ok(NULL);
}
