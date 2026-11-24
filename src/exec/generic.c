#include "exec/generic.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "core/allocator.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/interface.h"
#include "engine/interface_type.h"
#include "engine/qualifier.h"
#include "engine/scope.h"
#include "engine/struct.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/exec.h"
#include "exec/expr.h"
#include "parser/node.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
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

/* 泛型约束检查（白皮书 §6.5）：约束在绑定该参数之后求值（F-bounded 的
   IWriter[W] 引用正在绑定的参数，W 须已可见）。求值结果须为 type
   value——描述 interface 类型时按结构判定：实参类型提供接口要求的
   全部成员（名字 + func 签名一致）即满足，不要求显式 implements；
   不满足时报第一个缺失或签名不符的成员。非 interface 约束（类型
   模式）按派生关系判定（clux_type_extends）。 */
static clux_value_t *check_generic_constraint(clux_context_t *ctx,
                                              clux_value_t *bound,
                                              node_t *constraint) {
  clux_value_t *required = exec_operand(ctx, constraint);
  if (!required || clux_value_is_error(required)) {
    return required ? required
                    : clux_context_create_error_message(
                          ctx,
                          node_get_location(constraint),
                          "generic constraint evaluation failed");
  }
  if (clux_type_kind_unwrap(clux_value_get_type(required)) != CLUX_TYPE_TYPE) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(constraint),
        "generic constraint must be a type expression");
  }
  clux_type_t *req = *(clux_type_t **)clux_value_get_raw(required, 0);
  /* 约束对象：类型参数取绑定描述的类型，值参数取绑定值类型。 */
  clux_type_t *actual = clux_value_get_type(bound);
  if (clux_type_kind_unwrap(actual) == CLUX_TYPE_TYPE) {
    actual = *(clux_type_t **)clux_value_get_raw(bound, 0);
  }
  clux_type_t *unwrapped = clux_type_unwrap(actual);
  if (unwrapped) {
    actual = unwrapped;
  }
  const location_t *loc = node_get_location(constraint);
  const char *actual_name = clux_type_get_name(actual);
  const char *req_name = clux_type_get_name(req);
  const char *arg_text = actual_name ? actual_name : "<unknown>";
  const char *req_text = req_name ? req_name : "<unknown>";
  if (clux_type_kind_unwrap(req) == CLUX_TYPE_INTERFACE) {
    /* 结构判定：逐成员找 method 本体（attribute 表）并比对 func 签名。 */
    size_t count = clux_interface_member_count(req);
    for (size_t idx = 0; idx < count; idx++) {
      const clux_interface_member_t *member =
          clux_interface_get_member(req, idx);
      const clux_struct_attr_t *attr = NULL;
      if (clux_type_get_kind(actual) == CLUX_TYPE_STRUCT) {
        attr = clux_struct_find_attr(actual, member->name);
      }
      if (!attr || !attr->value) {
        return clux_context_create_error_message(
            ctx,
            loc,
            "type '%s' does not satisfy interface constraint '%s': "
            "missing member '%s'",
            arg_text,
            req_text,
            member->name);
      }
      if (clux_type_get_kind(clux_value_get_type(attr->value)) !=
              CLUX_TYPE_FUNC ||
          !clux_type_equal(clux_value_get_type(attr->value), member->func)) {
        return clux_context_create_error_message(
            ctx,
            loc,
            "type '%s' does not satisfy interface constraint '%s': "
            "member '%s' signature mismatch",
            arg_text,
            req_text,
            member->name);
      }
    }
    return NULL;
  }
  /* 类型模式约束：按派生关系判定。 */
  if (!clux_type_extends(actual, req)) {
    return clux_context_create_error_message(
        ctx,
        loc,
        "type '%s' does not satisfy constraint '%s'",
        arg_text,
        req_text);
  }
  return NULL;
}

/* 实例化核心：args 按参数表位置对齐，条目可为 NULL（该参数走默认值，
   无默认值报错）——显式实例化传已求值实参前缀，调用点推导（§6.7）
   传"已推断条目 + NULL 空洞"的满长数组。绑定渐进进行：默认值（§6.2）
   与值参数声明类型的求值都发生在已含更早参数绑定的环境内；约束
   （§6.5）在绑定该参数之后检查。 */
static clux_value_t *instantiate_with_args(clux_context_t *ctx,
                                           clux_value_t *template_value,
                                           clux_value_t **args,
                                           size_t arg_count,
                                           const location_t *loc) {
  void *raw_ast = clux_context_template_ast(template_value);
  node_t *ast = (node_t *)raw_ast;
  node_t *params_node = template_params_node(ast);
  size_t param_count =
      params_node ? generic_params_node_get_param_count(params_node) : 0;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  if (!clux_context_instance_depth_enter(ctx)) {
    return clux_context_create_error_message(
        ctx, loc, "maximum generic instantiation depth exceeded");
  }
  clux_context_push_scope(ctx);
  clux_scope_t *instance_scope = clux_context_get_scope(ctx);
  clux_scope_t *outer = clux_scope_get_parent(instance_scope);
  clux_value_t *result = NULL;
  clux_value_t *fail = NULL;
  for (size_t idx = 0; idx < param_count && !fail; idx++) {
    node_t *param = generic_params_node_get_param(params_node, idx);
    clux_value_t *raw = NULL;
    if (idx < arg_count && args[idx]) {
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
      break;
    }
    /* 约束在绑定之后检查（F-bounded 引用正在绑定的参数）。 */
    node_t *constraint = generic_param_node_get_constraint(param);
    if (constraint) {
      clux_value_t *violation =
          check_generic_constraint(ctx, bound, constraint);
      if (violation) {
        fail = violation;
        break;
      }
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

clux_value_t *exec_generic_instantiate(clux_context_t *ctx,
                                       clux_value_t *template_value,
                                       node_t *subscript) {
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
  /* 参数包本批不支持：先扫参数表快速失败，不绑定一半再回滚。 */
  for (size_t idx = 0; idx < param_count; idx++) {
    node_t *param = generic_params_node_get_param(params_node, idx);
    if (!param || node_get_kind(param) != CLUX_NODE_GENERIC_PARAM) {
      return clux_context_create_error_message(
          ctx, loc, "generic parameter pack is not supported yet");
    }
  }
  /* 实参在实例化点作用域求值（引用实例化点处的名字）。 */
  clux_value_t *args[arg_count ? arg_count : 1];
  for (size_t idx = 0; idx < arg_count; idx++) {
    args[idx] = exec_operand(ctx, subscript_node_get_arg(subscript, idx));
    if (!args[idx] || clux_value_is_error(args[idx])) {
      return args[idx];
    }
  }
  return instantiate_with_args(
      ctx, template_value, arg_count ? args : NULL, arg_count, loc);
}

/* 调用点推导（白皮书 §6.7）：只有函数调用触发——模板形参类型是裸
   标识符且等于某类型参数名时，从对应实参的类型推断该参数绑定
   （实参类型包装为 type value）。同一参数出现两个不判等的类型报
   推断冲突；无法从实参推断的参数留 NULL 空洞交默认值回退（无默认
   值由实例化核心报 missing）。复合类型模式（[]T、*T）本批不参与
   推断；值参数（comptime）不可从运行期实参推断，同样走默认值。
   返回推断实例化出的 func 值（调用方随后 invoke）。 */
clux_value_t *exec_generic_infer_instance(clux_context_t *ctx,
                                          clux_value_t *template_value,
                                          size_t argc,
                                          clux_value_t *argv[],
                                          const location_t *loc) {
  node_t *ast = (node_t *)clux_context_template_ast(template_value);
  node_t *params_node = template_params_node(ast);
  size_t param_count =
      params_node ? generic_params_node_get_param_count(params_node) : 0;
  if (node_get_kind(ast) != CLUX_NODE_EXPR_FUNC_DECL) {
    return clux_context_create_error_message(
        ctx, loc, "generic inference requires a function template");
  }
  /* 可推断名单：只标记类型参数（TYPE 槽为空且带名）。 */
  allocator_t *allocator = clux_context_get_allocator(ctx);
  bool inferable[param_count ? param_count : 1];
  for (size_t idx = 0; idx < param_count; idx++) {
    node_t *param = generic_params_node_get_param(params_node, idx);
    inferable[idx] = param && node_get_kind(param) == CLUX_NODE_GENERIC_PARAM &&
                     !generic_param_node_get_type(param) &&
                     generic_param_node_get_name(param);
  }
  /* 逐函数形参与实配对：裸标识符类型命中类型参数名即推断。 */
  node_t *func_params = func_decl_node_get_params(ast);
  size_t func_param_count =
      func_params ? func_params_node_get_param_count(func_params) : 0;
  clux_value_t *inferred[param_count ? param_count : 1];
  for (size_t idx = 0; idx < param_count; idx++) {
    inferred[idx] = NULL;
  }
  clux_value_t *fail = NULL;
  for (size_t idx = 0; idx < argc && idx < func_param_count && !fail; idx++) {
    node_t *fparam = func_params_node_get_param(func_params, idx);
    if (!fparam || node_get_kind(fparam) != CLUX_NODE_FUNC_PARAM) {
      continue;
    }
    node_t *type_node = func_param_node_get_type(fparam);
    if (!type_node ||
        node_get_kind(type_node) != CLUX_NODE_LITERAL_IDENTIFIER) {
      continue;
    }
    char *text = location_get(node_get_location(type_node), allocator);
    for (size_t pidx = 0; pidx < param_count && !fail; pidx++) {
      if (!inferable[pidx]) {
        continue;
      }
      node_t *pname_node = generic_param_node_get_name(
          generic_params_node_get_param(params_node, pidx));
      char *pname = location_get(node_get_location(pname_node), allocator);
      bool hit = strcmp(text, pname) == 0;
      allocator_free(allocator, (void **)&pname);
      if (!hit) {
        continue;
      }
      clux_type_t *arg_type = clux_value_get_type(argv[idx]);
      if (inferred[pidx]) {
        clux_type_t *prev =
            *(clux_type_t **)clux_value_get_raw(inferred[pidx], 0);
        if (!clux_type_equal(prev, arg_type)) {
          fail = clux_context_create_error_message(
              ctx,
              loc,
              "cannot infer generic parameter '%s': "
              "conflicting argument types",
              text);
        }
        break;
      }
      clux_value_t *binding =
          clux_context_create_type_value(ctx, arg_type, NULL, loc);
      if (!binding || clux_value_is_error(binding)) {
        fail = binding ? binding
                       : clux_context_create_error_message(
                             ctx, loc, "generic inference binding failed");
        break;
      }
      inferred[pidx] = binding;
      break;
    }
    allocator_free(allocator, (void **)&text);
  }
  if (fail) {
    return fail;
  }
  return instantiate_with_args(
      ctx, template_value, param_count ? inferred : NULL, param_count, loc);
}
