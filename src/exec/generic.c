#include "exec/generic.h"
#include <stdio.h>
#include <string.h>
#include "core/allocator.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/scope.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/exec.h"
#include "exec/expr.h"
#include "parser/node.h"
#include "parser/node_func_decl.h"
#include "parser/node_generic_param.h"
#include "parser/node_stmt_type.h"
#include "parser/node_struct_decl.h"
#include "parser/node_subscript.h"

/* 泛型显式实例化（白皮书 §6.6）：tmpl[arg, ...] 的展开通道。
   参数表从模板 AST 的 GENERICS 槽重读（模板 data 只存 AST 指针）；
   实参在实例化点作用域求值，绑定值 clone 进实例化 scope 按参名
   declare——模板 AST 随后在绑定环境内重执行：
   - func 模板 → 追加绑定捕获的 func 实例（exec_func_decl_instance）
   - struct 模板 → 匿名重执行的 struct 实例（exec_expr_struct_decl_instance）
   - type 别名模板 → VALUE 表达式重求值
   实例值按块出口协议 clone 出实例化 scope（模板 AST 是声明，不执行
   defer，无信号出口）。§6.10 同组合同实例由结构判等天然达成。 */

/* 模板 AST 取参数表节点：三个泛型声明入口的 GENERICS 槽。 */
static node_t *template_params_node(node_t *ast) {
  switch (node_get_kind(ast)) {
  case CLUX_NODE_EXPR_FUNC_DECL:
    return func_decl_node_get_generics(ast);
  case CLUX_NODE_EXPR_STRUCT_DECL:
    return struct_decl_node_get_generics(ast);
  case CLUX_NODE_STMT_TYPE:
    return stmt_type_node_get_generics(ast);
  default:
    return NULL;
  }
}

/* 单参绑定：校验实参形态并 clone 进实例化 scope 按参名具名化。
   - 类型参数（TYPE 槽为 NULL）：实参必须是 type value；
   - 值参数（TYPE 槽非空，§6.3 comptime）：声明类型在绑定环境内求值
     （可引用更早参数），实参类型与之判等。
   raw 归调用方/默认值求值作用域，绑定值是 clone 本体。 */
static clux_value_t *
bind_generic_param(clux_context_t *ctx, node_t *param, clux_value_t *raw) {
  node_t *name_node = generic_param_node_get_name(param);
  node_t *type_node = generic_param_node_get_type(param);
  const location_t *loc = node_get_location(param);
  if (!name_node) {
    return clux_context_create_error_message(
        ctx, loc, "generic parameter requires a name");
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  clux_value_t *fail = NULL;
  if (type_node) {
    clux_value_t *err = NULL;
    clux_type_t *declared = exec_operand_type(ctx, type_node, &err);
    if (!declared) {
      fail = err ? err
                 : clux_context_create_error_message(
                       ctx,
                       node_get_location(type_node),
                       "expected a type expression, got '%s'",
                       name);
    } else if (!clux_type_equal(declared, clux_value_get_type(raw))) {
      const char *declared_name = clux_type_get_name(declared);
      fail = clux_context_create_error_message(
          ctx,
          loc,
          "generic parameter '%s' requires a value of type '%s'",
          name,
          declared_name ? declared_name : "<unknown>");
    } else {
      declared = NULL;
    }
  } else if (clux_type_kind_unwrap(clux_value_get_type(raw)) !=
             CLUX_TYPE_TYPE) {
    const char *raw_name = clux_type_get_name(clux_value_get_type(raw));
    fail = clux_context_create_error_message(
        ctx,
        loc,
        "generic parameter '%s' requires a type argument, got '%s'",
        name,
        raw_name ? raw_name : "<unknown>");
  }
  if (fail) {
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  /* 绑定本体：clone 进实例化 scope（当前作用域）再按参名具名化。 */
  clux_value_t *binding = clux_context_clone(ctx, raw, NULL);
  if (!binding || clux_value_is_error(binding)) {
    clux_value_t *clone_fail =
        binding ? binding
                : clux_context_create_error_message(
                      ctx, loc, "generic parameter '%s' clone failed", name);
    allocator_free(allocator, (void **)&name);
    return clone_fail;
  }
  binding = clux_context_declare_value(
      ctx, binding, name, node_get_location(name_node));
  allocator_free(allocator, (void **)&name);
  return binding;
}

clux_value_t *exec_generic_instantiate(clux_context_t *ctx,
                                       clux_value_t *template_value,
                                       node_t *subscript) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  void *raw_ast = clux_context_template_ast(template_value);
  node_t *ast = (node_t *)raw_ast;
  const location_t *loc = node_get_location(subscript);
  node_t *params_node = template_params_node(ast);
  size_t param_count =
      params_node ? generic_params_node_get_param_count(params_node) : 0;
  size_t arg_count = subscript_node_get_arg_count(subscript);
  if (arg_count > param_count) {
    return clux_context_create_error_message(ctx,
                                             loc,
                                             "generic instantiation takes at "
                                             "most %zu arguments, got %zu",
                                             param_count,
                                             arg_count);
  }
  /* 参数包/约束本批不支持：先扫参数表快速失败，不绑定一半再回滚。 */
  for (size_t idx = 0; idx < param_count; idx++) {
    node_t *param = generic_params_node_get_param(params_node, idx);
    if (!param || node_get_kind(param) != CLUX_NODE_GENERIC_PARAM) {
      return clux_context_create_error_message(
          ctx, loc, "generic parameter pack is not supported yet");
    }
    if (generic_param_node_get_constraint(param)) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(param),
          "generic constraints are not supported yet");
    }
  }
  /* 实参在实例化点作用域求值（引用实例化点处的名字）。 */
  clux_value_t *args[arg_count];
  for (size_t idx = 0; idx < arg_count; idx++) {
    args[idx] = exec_operand(ctx, subscript_node_get_arg(subscript, idx));
    if (!args[idx] || clux_value_is_error(args[idx])) {
      return args[idx];
    }
  }
  if (!clux_context_instance_depth_enter(ctx)) {
    return clux_context_create_error_message(
        ctx, loc, "maximum generic instantiation depth exceeded");
  }
  clux_context_push_scope(ctx);
  clux_scope_t *instance_scope = clux_context_get_scope(ctx);
  clux_scope_t *outer = clux_scope_get_parent(instance_scope);
  clux_value_t *result = NULL;
  clux_value_t *fail = NULL;
  /* 绑定渐进进行：默认值（§6.2）与值参数声明类型的求值都发生在已含
     更早参数绑定的环境内（默认值可引用更早参数）。 */
  for (size_t idx = 0; idx < param_count && !fail; idx++) {
    node_t *param = generic_params_node_get_param(params_node, idx);
    clux_value_t *raw = NULL;
    if (idx < arg_count) {
      raw = args[idx]; /* 借用：实参归实例化点作用域 */
    } else {
      node_t *default_node = generic_param_node_get_default(param);
      if (!default_node) {
        node_t *name_node = generic_param_node_get_name(param);
        if (!name_node) {
          fail = clux_context_create_error_message(
              ctx, loc, "generic parameter requires a name");
          break;
        }
        char *name = location_get(node_get_location(name_node), allocator);
        fail = clux_context_create_error_message(
            ctx, loc, "missing generic argument '%s'", name);
        allocator_free(allocator, (void **)&name);
        break;
      }
      raw = exec_operand(ctx, default_node);
      if (!raw || clux_value_is_error(raw)) {
        fail = raw ? raw
                   : clux_context_create_error_message(
                         ctx, loc, "generic default value failed");
        break;
      }
    }
    clux_value_t *bound = bind_generic_param(ctx, param, raw);
    if (!bound || clux_value_is_error(bound)) {
      fail = bound ? bound
                   : clux_context_create_error_message(
                         ctx, loc, "generic parameter binding failed");
    }
  }
  /* 绑定失败快速失败：不进入模板体重执行。 */
  if (fail) {
    result = fail;
  } else {
    switch (node_get_kind(ast)) {
    case CLUX_NODE_EXPR_FUNC_DECL:
      result = exec_func_decl_instance(ctx, ast);
      break;
    case CLUX_NODE_EXPR_STRUCT_DECL:
      result = exec_expr_struct_decl_instance(ctx, ast);
      break;
    case CLUX_NODE_STMT_TYPE: {
      node_t *value_node = stmt_type_node_get_value(ast);
      result = exec_operand(ctx, value_node);
      if (result && !clux_value_is_error(result) &&
          clux_type_kind_unwrap(clux_value_get_type(result)) !=
              CLUX_TYPE_TYPE) {
        result = clux_context_create_error_message(
            ctx, loc, "expected type expression in type alias");
      }
      break;
    }
    default:
      result = clux_context_create_error_message(
          ctx, loc, "unsupported generic template");
    }
  }
  /* 块出口协议：弹回实例化 scope → swap 到外层 → error 重建/实例值
     clone 出（模板 AST 是声明，无 defer/信号出口）→ delete 实例化
     scope。 */
  clux_scope_t *current = clux_context_get_scope(ctx);
  while (current != instance_scope &&
         current != clux_context_get_global_scope(ctx)) {
    clux_context_pop_scope(ctx);
    current = clux_context_get_scope(ctx);
  }
  (void)clux_context_swap_scope(ctx, outer);
  clux_value_t *out = NULL;
  if (result && clux_value_is_error(result)) {
    clux_error_t *err = *(clux_error_t **)clux_value_get_raw(result, 0);
    out = clux_context_create_error(ctx, err);
  } else if (result) {
    out = clux_context_clone(ctx, result, NULL);
    if (!out || clux_value_is_error(out)) {
      clux_error_t *err =
          out ? *(clux_error_t **)clux_value_get_raw(out, 0) : NULL;
      out = err ? clux_context_create_error(ctx, err)
                : clux_context_create_error_message(
                      ctx, loc, "generic instantiation value clone failed");
    }
  } else {
    out = clux_context_create_error_message(
        ctx, loc, "generic instantiation produced no value");
  }
  clux_scope_t *dead = instance_scope;
  delete_clux_scope(&dead);
  clux_context_instance_depth_leave(ctx);
  return out;
}
