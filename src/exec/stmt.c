#include "exec/stmt.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/scope.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/call.h"
#include "exec/exec.h"
#include "exec/expr.h"
#include "parser/node.h"
#include "parser/node_decorator.h"
#include "parser/node_enum_decl.h"
#include "parser/node_func_decl.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_enum.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_type.h"
#include "parser/node_stmt_union.h"
#include "parser/node_stmt_var.h"
#include "parser/node_union.h"

/* 单装饰器应用（白皮书 §3.2/§9.1）：普通装饰器是值级包装——装饰器表达式
   求值为 func 值时以当前值为唯一实参调用（[[twice]] var x = 21 落地为
   x = twice(21)；[[add100]] func raw 落地为 raw = add100(raw)）；
   求值为其余值（编译期标记，如 metadata）不改当前值原样返回。
   expr 缺失 / 装饰器求值 error / invoke error 均产出 error。 */
static clux_value_t *apply_decorator(clux_context_t *ctx,
                                     clux_value_t *value,
                                     node_t *decorator,
                                     const location_t *fallback_loc) {
  node_t *decorator_expr =
      decorator ? decorator_node_get_expr(decorator) : NULL;
  if (!decorator_expr) {
    return clux_context_create_error_message(
        ctx,
        decorator ? node_get_location(decorator) : fallback_loc,
        "decorator requires an expression");
  }
  clux_value_t *wrapper = exec_operand(ctx, decorator_expr);
  if (clux_value_is_error(wrapper)) {
    return wrapper;
  }
  if (clux_type_kind_unwrap(clux_value_get_type(wrapper)) != CLUX_TYPE_FUNC) {
    return value;
  }
  /* invoke 走帧协议执行器——先保证已挂（首次调用时懒安装） */
  exec_call_install_executor(ctx);
  return clux_context_invoke(
      ctx, wrapper, 1, &value, node_get_location(decorator));
}

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
  /* 装饰器按源码顺序链式包装（apply_decorator）。TDZ（= undefined）无
     初值可包装，装饰器不适用。 */
  if (value) {
    size_t decorator_count = stmt_var_node_get_decorator_count(node);
    for (size_t idx = 0; idx < decorator_count; idx++) {
      value = apply_decorator(ctx,
                              value,
                              stmt_var_node_get_decorator(node, idx),
                              node_get_location(node));
      if (clux_value_is_error(value)) {
        return value;
      }
    }
  }
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

/* type 别名语句 <decorator>* [export]? type name = type_expr;
   （白皮书 §3.11、§8.1）：求值右侧类型表达式——经 exec_operand 统一
   通道（error 优先传播、shadow 拒绝、TDZ 消费校验均由其内置语义化
   报错），必须产出 type value（否则按 clux_type_t* 解引用是内存错误），
   经 declare_value 以别名注册进作用域。export（run 阶段产出）是编译期
   语义，exec 层不处理；generics/decorators 待泛型实现完成后接入。 */
clux_value_t *exec_stmt_type(clux_context_t *ctx, node_t *node) {
  node_t *name_node = stmt_type_node_get_name(node);
  if (!name_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "type statement requires a name");
  }
  node_t *value_node = stmt_type_node_get_value(node);
  if (!value_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "type statement requires a type expression");
  }
  clux_value_t *value = exec_operand(ctx, value_node);
  if (!value || clux_value_is_error(value)) {
    return value;
  }
  if (clux_type_kind_unwrap(clux_value_get_type(value)) != CLUX_TYPE_TYPE) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(value_node),
        "expected type expression in type alias");
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  clux_value_t *bound = clux_context_declare_value(
      ctx, value, name, node_get_location(name_node));
  allocator_free(allocator, (void **)&name);
  return bound;
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

/* 函数语句 <decorator>* [export]? [comptime]? <func_decl>（白皮书 §3.3、
   §3.4.1、§8.1）：无装饰器时直接执行声明本体——命名函数经
   exec_expr_func_decl 自行 define。有装饰器时（§9.1 函数装饰器）：
   先匿名求值源函数值（不按名 define——scope 无重绑定通道，先声明再
   包装会撞 duplicate 或让名字继续指向源函数），装饰器按源码顺序以
   函数值为实参链式调用（[[add100]] func raw 落地为 raw = add100(raw)，
   包装后的函数值接管名字，后续 raw(x) 经包装函数走捕获调用源函数，
   conformance/09-metadata.cx 断言 raw(5) == 105）；最后 declare_value
   具名化（包装值已注册本作用域，就地命名不拷贝，捕获 view 无损）。
   export（run 阶段产出）与 comptime（解释执行下与普通函数等价）是
   编译期语义，exec 层不处理。 */
clux_value_t *exec_stmt_func(clux_context_t *ctx, node_t *node) {
  node_t *decl = stmt_func_node_get_func_decl(node);
  if (!decl) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "function statement requires a function declaration");
  }
  size_t decorator_count = stmt_func_node_get_decorator_count(node);
  if (decorator_count == 0) {
    return exec_operand(ctx, decl);
  }
  node_t *name_node = func_decl_node_get_name(decl);
  if (!name_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "decorated function declaration requires a name");
  }
  /* 匿名求值源函数值：注册进当前作用域但不具名——装饰失败无需回滚声明，
     源函数值生命周期归作用域（被装饰函数捕获引用，不可提前销毁）。 */
  clux_value_t *value = exec_func_decl_value(ctx, decl);
  if (clux_value_is_error(value)) {
    return value;
  }
  for (size_t idx = 0; idx < decorator_count; idx++) {
    value = apply_decorator(ctx,
                            value,
                            stmt_func_node_get_decorator(node, idx),
                            node_get_location(node));
    if (clux_value_is_error(value)) {
      return value;
    }
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  clux_value_t *bound = clux_context_declare_value(
      ctx, value, name, node_get_location(name_node));
  allocator_free(allocator, (void **)&name);
  return bound;
}

/* union 语句 <decorator>* [export]? <union_decl>（白皮书 §3.7、§3.8、
   §8.1）：与函数语句同构——无装饰器时直接执行声明本体（经
   exec_expr_union 按名 define 出 type value）；有装饰器时先匿名求值
   源类型值（不按名 define），装饰器按源码顺序链式应用（求值为 func
   时以类型值为实参调用，编译期标记原样返回），最后 declare_value
   具名化。语句层的声明必须具名（匿名 union 类型只在表达式位置合法）；
   export（run 阶段产出）是编译期语义，exec 层不处理。 */
clux_value_t *exec_stmt_union(clux_context_t *ctx, node_t *node) {
  node_t *decl = stmt_union_node_get_union_decl(node);
  if (!decl) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "union statement requires a union declaration");
  }
  node_t *name_node = union_node_get_name(decl);
  if (!name_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "union statement requires a named union declaration");
  }
  size_t decorator_count = stmt_union_node_get_decorator_count(node);
  if (decorator_count == 0) {
    return exec_operand(ctx, decl);
  }
  /* 匿名求值源类型值：不具名——type value 无捕获引用，装饰失败
     直接返回 error，无需回滚声明。 */
  clux_value_t *value = exec_expr_union_value(ctx, decl);
  if (clux_value_is_error(value)) {
    return value;
  }
  for (size_t idx = 0; idx < decorator_count; idx++) {
    value = apply_decorator(ctx,
                            value,
                            stmt_union_node_get_decorator(node, idx),
                            node_get_location(node));
    if (clux_value_is_error(value)) {
      return value;
    }
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  clux_value_t *bound = clux_context_declare_value(
      ctx, value, name, node_get_location(name_node));
  allocator_free(allocator, (void **)&name);
  return bound;
}

/* enum 语句 <decorator>* [export]? <enum_decl>（白皮书 §3.6、§8.1）：
   与 union 语句同构——无装饰器时直接执行声明本体（经
   exec_expr_enum_decl 按名 define 出 type value）；有装饰器时先匿名求值
   源类型值（不按名 define），装饰器按源码顺序链式应用（求值为 func 时
   以类型值为实参调用，编译期标记原样返回），最后 declare_value 具名化。
   语句层的声明必须具名（匿名 enum 类型只在表达式位置合法）；
   export（run 阶段产出）是编译期语义，exec 层不处理。 */
clux_value_t *exec_stmt_enum(clux_context_t *ctx, node_t *node) {
  node_t *decl = stmt_enum_node_get_enum_decl(node);
  if (!decl) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "enum statement requires an enum declaration");
  }
  node_t *name_node = enum_decl_node_get_name(decl);
  if (!name_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "enum statement requires a named enum declaration");
  }
  size_t decorator_count = stmt_enum_node_get_decorator_count(node);
  if (decorator_count == 0) {
    return exec_operand(ctx, decl);
  }
  /* 匿名求值源类型值：不具名——type value 无捕获引用，装饰失败
     直接返回 error，无需回滚声明。 */
  clux_value_t *value = exec_expr_enum_decl_value(ctx, decl);
  if (clux_value_is_error(value)) {
    return value;
  }
  for (size_t idx = 0; idx < decorator_count; idx++) {
    value = apply_decorator(ctx,
                            value,
                            stmt_enum_node_get_decorator(node, idx),
                            node_get_location(node));
    if (clux_value_is_error(value)) {
      return value;
    }
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  clux_value_t *bound = clux_context_declare_value(
      ctx, value, name, node_get_location(name_node));
  allocator_free(allocator, (void **)&name);
  return bound;
}
