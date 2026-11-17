#include "exec/stmt.h"
#include "core/allocator.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/value.h"
#include "exec/exec.h"
#include "parser/node.h"
#include "parser/node_stmt_var.h"

clux_value_t *exec_stmt_var(clux_context_t *ctx, node_t *node) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  node_t *node_type = stmt_var_node_get_type(node);
  node_t *node_name = stmt_var_node_get_name(node);
  node_t *node_value = stmt_var_node_get_value(node);
  clux_value_t *value = exec_operand(ctx, node_value);
  if (clux_value_is_error(value)) {
    return value;
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
  const location_t *loc = node_get_location(node_value);
  if (type) {
    value = clux_context_safe_cast(ctx, value, type, loc);
    if (clux_value_is_error(value)) {
      return value;
    }
  }
  char *name = location_get(node_get_location(node_name), allocator);
  value = clux_context_declare_value(ctx, value, name, loc);
  allocator_free(allocator, (void **)&name);
  return value;
}
