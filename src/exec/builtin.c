#include "exec/exec.h"

#include "engine/context.h"
#include "engine/error.h"
#include "engine/type.h"
#include "engine/value.h"
#include "parser/node.h"
#include "parser/node_builtin.h"

/* 编译器内建表达式的执行入口。
   名字由 builtin 节点的 name 符号 location 文本判定（不含 '#')，
   实参与普通调用一致已由 parser 收集为子节点。
   当前仅实现引擎通道齐备的内建：optional 四件套与类型运算三件套；
   未知名与通道未就绪的内建统一报语义化错误。 */
static clux_value_t *builtin_error(clux_context_t *ctx,
                                   const location_t *loc,
                                   const char *name,
                                   const char *detail) {
  return clux_context_create_error_message(
      ctx, loc, "builtin '#%s' %s", name ? name : "<unknown>", detail);
}

clux_value_t *exec_expr_builtin(clux_context_t *ctx, node_t *node) {
  node_t *name_node = builtin_node_get_name(node);
  const location_t *loc = node_get_location(node);
  const location_t *name_loc = name_node ? node_get_location(name_node) : NULL;
  size_t argc = builtin_node_get_arg_count(node);

  if (!name_loc) {
    return builtin_error(ctx, loc, NULL, "requires a name and arguments");
  }

  /* 错误路径专用：取实际名字文本，取完即释放。 */
  allocator_t *allocator = clux_context_get_allocator(ctx);
  if (argc == 0) {
    char *name = location_get(name_loc, allocator);
    clux_value_t *err =
        builtin_error(ctx, loc, name, "requires a name and arguments");
    allocator_free(allocator, (void **)&name);
    return err;
  }

  if (location_is(name_loc, "none") || location_is(name_loc, "some")) {
    bool is_some = location_is(name_loc, "some");
    const char *name = is_some ? "some" : "none";
    clux_value_t *err = NULL;
    clux_type_t *base =
        exec_operand_type(ctx, builtin_node_get_arg(node, 0), &err);
    if (!base) {
      return err;
    }
    if (!is_some) {
      if (argc != 1) {
        return builtin_error(ctx, loc, name, "takes exactly one type argument");
      }
      return clux_context_create_optional_none(ctx, base, NULL, loc);
    }
    if (argc != 2) {
      return builtin_error(ctx, loc, name, "takes a type and a value");
    }
    clux_value_t *value = exec_operand(ctx, builtin_node_get_arg(node, 1));
    if (clux_value_is_error(value)) {
      return value;
    }
    return clux_context_create_optional_some(ctx, base, value, NULL, loc);
  }

  if (location_is(name_loc, "is_none") || location_is(name_loc, "take")) {
    bool is_take = location_is(name_loc, "take");
    const char *name = is_take ? "take" : "is_none";
    if (argc != 1) {
      return builtin_error(ctx, loc, name, "takes exactly one argument");
    }
    clux_value_t *value = exec_operand(ctx, builtin_node_get_arg(node, 0));
    if (clux_value_is_error(value)) {
      return value;
    }
    if (is_take) {
      return clux_context_take(ctx, value, loc);
    }
    return clux_context_is_none(ctx, value, loc);
  }

  if (location_is(name_loc, "sizeof") || location_is(name_loc, "alignof")) {
    bool is_sizeof = location_is(name_loc, "sizeof");
    const char *name = is_sizeof ? "sizeof" : "alignof";
    if (argc != 1) {
      return builtin_error(ctx, loc, name, "takes exactly one type argument");
    }
    clux_value_t *err = NULL;
    clux_type_t *type =
        exec_operand_type(ctx, builtin_node_get_arg(node, 0), &err);
    if (!type) {
      return err;
    }
    size_t size =
        is_sizeof ? clux_type_get_size(type) : clux_type_get_align(type);
    return clux_context_create_usize(ctx, size, NULL, loc);
  }

  if (location_is(name_loc, "typeof")) {
    if (argc != 1) {
      return builtin_error(ctx, loc, "typeof", "takes exactly one argument");
    }
    /* typeof 是编译期类型运算：实参走 shadow 计算（推断不落存储、
       函数体不执行、TDZ 读取按变量类型产 shadow），只取类型不真实
       消费，求完恢复原 shadow 模式。 */
    bool prev_mode = clux_context_is_shadow_mode(ctx);
    clux_context_set_shadow_mode(ctx, true);
    clux_value_t *value = exec_operand(ctx, builtin_node_get_arg(node, 0));
    clux_context_set_shadow_mode(ctx, prev_mode);
    if (clux_value_is_error(value)) {
      return value;
    }
    return clux_context_create_type_value(
        ctx, clux_value_get_type(value), NULL, loc);
  }

  /* 未知名（或通道未就绪的内建）：错误消息带上实际名字文本 */
  char *name = location_get(name_loc, allocator);
  clux_value_t *err =
      builtin_error(ctx, loc, name, "is not supported in execution");
  allocator_free(allocator, (void **)&name);
  return err;
}
