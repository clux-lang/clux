#include "exec/expr.h"
#include "core/allocator.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/exec.h"
#include "parser/node.h"
#include "parser/node_assign.h"
#include "parser/node_binary.h"
#include "parser/node_func_capture.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_func_type.h"
#include "parser/node_group.h"
#include "parser/node_slice.h"
#include "parser/node_subscript.h"
#include "parser/node_ternary.h"
#include "parser/node_tuple.h"
#include <stdbool.h>
#include <string.h>

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

/* 复合类型定义的公共通道：求值操作数并要求 type value，返回被描述类型；
   失败返回 NULL 且 *out_err 携带 error 值（操作数 error 优先传播）。 */
static clux_type_t *
exec_operand_type(clux_context_t *ctx, node_t *node, clux_value_t **out_err) {
  clux_value_t *value = exec_operand(ctx, node);
  if (clux_value_is_error(value)) {
    *out_err = value;
    return NULL;
  }
  if (clux_type_kind_unwrap(clux_value_get_type(value)) != CLUX_TYPE_TYPE) {
    const char *type_name = clux_type_get_name(clux_value_get_type(value));
    *out_err = clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "expected a type expression, got '%s'",
        type_name ? type_name : "<unknown>");
    return NULL;
  }
  /* exec 已拒绝 shadow，raw 必有真实存储 */
  *out_err = NULL;
  return *(clux_type_t **)clux_value_get_raw(value, 0);
}

/* 捕获项求值公共体（func_type 与 func_decl 共用）：名称取 identifier
   文本，类型经公共类型通道要求 type value；重名在引擎校验前拦截以
   携带源位置。成功并入 captures[(*filled)++]；失败返回 false 且
   *out_fail 携带 error（已并入的项由调用方统一释放）。 */
static bool exec_func_capture_entry(clux_context_t *ctx,
                                    node_t *name_node,
                                    node_t *type_node,
                                    size_t idx,
                                    clux_func_capture_t *captures,
                                    size_t *filled,
                                    clux_value_t **out_fail) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name =
      name_node ? location_get(node_get_location(name_node), allocator) : NULL;
  if (!name) {
    *out_fail = clux_context_create_error_message(
        ctx,
        type_node ? node_get_location(type_node) : NULL,
        "capture %zu requires a name and a type",
        idx);
    return false;
  }
  for (size_t prev = 0; prev < *filled; prev++) {
    if (strcmp(name, captures[prev].name) == 0) {
      *out_fail = clux_context_create_error_message(
          ctx, node_get_location(name_node), "duplicate capture '%s'", name);
      allocator_free(allocator, (void **)&name);
      return false;
    }
  }
  if (!type_node) {
    *out_fail =
        clux_context_create_error_message(ctx,
                                          node_get_location(name_node),
                                          "capture '%s' requires a type",
                                          name);
    allocator_free(allocator, (void **)&name);
    return false;
  }
  clux_value_t *err = NULL;
  clux_type_t *type = exec_operand_type(ctx, type_node, &err);
  if (!type) {
    *out_fail = err;
    allocator_free(allocator, (void **)&name);
    return false;
  }
  captures[*filled].name = name;
  captures[*filled].type = type;
  (*filled)++;
  *out_fail = NULL;
  return true;
}

/* 一元类型定义前缀：运算符文本→引擎类型构造器。
   const/volatile 走限定通道单独处理（构造器签名不同）。 */
static const struct {
  const char *text;
  clux_type_t *(*build)(clux_context_t *, clux_type_t *);
} k_type_prefix_ops[] = {
    {"*", clux_context_get_ptr_type},
    {"[]", clux_context_get_slice_type},
    {"?", clux_context_get_optional_type},
};

/* 整数类型族判定：数组长度只接受整数（读取宽度随 kind 变化，
   见 clux_context_index_to_i64）。 */
static bool is_integer_kind(clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_I8:
  case CLUX_TYPE_I16:
  case CLUX_TYPE_I32:
  case CLUX_TYPE_I64:
  case CLUX_TYPE_U8:
  case CLUX_TYPE_U16:
  case CLUX_TYPE_U32:
  case CLUX_TYPE_U64:
  case CLUX_TYPE_ISIZE:
  case CLUX_TYPE_USIZE:
    return true;
  default:
    return false;
  }
}

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
    /* 类型定义前缀（*T / []T / ?T，白皮书 §2）：操作数须为 type value，
       经引擎 interning 后包装成 type value 返回（var x: *i32 由此落地）。 */
    for (size_t i = 0;
         i < sizeof(k_type_prefix_ops) / sizeof(k_type_prefix_ops[0]);
         i++) {
      if (!location_is(op_loc, k_type_prefix_ops[i].text)) {
        continue;
      }
      clux_value_t *err = NULL;
      clux_type_t *base = exec_operand_type(ctx, right_node, &err);
      if (!base) {
        return err;
      }
      clux_type_t *composite = k_type_prefix_ops[i].build(ctx, base);
      return clux_context_create_type_value(ctx, composite, NULL, op_loc);
    }
    /* 限定类型前缀（const T / volatile T）：经 qualifier 通道叠加限定，
       支持 const volatile T 链式叠加。 */
    clux_type_kind_t qualifier_kind = CLUX_TYPE_ERROR;
    if (location_is(op_loc, "const")) {
      qualifier_kind = CLUX_TYPE_CONST;
    } else if (location_is(op_loc, "volatile")) {
      qualifier_kind = CLUX_TYPE_VOLATILE;
    }
    if (qualifier_kind != CLUX_TYPE_ERROR) {
      clux_value_t *err = NULL;
      clux_type_t *base = exec_operand_type(ctx, right_node, &err);
      if (!base) {
        return err;
      }
      clux_type_t *qualified =
          clux_type_get_qualified_type(base, qualifier_kind);
      return clux_context_create_type_value(ctx, qualified, NULL, op_loc);
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
  /* 数组类型定义 [N]T：SYMBOL '[]' 且 LEFT 为长度表达式（切片类型前缀
     []T 的 LEFT 为 NULL，已由上方前缀形态处理）。长度先于元素求值，
     须为非负整数；元素须为 type value。 */
  if (location_is(op_loc, "[]")) {
    clux_value_t *length = exec_operand(ctx, left_node);
    if (clux_value_is_error(length)) {
      return length;
    }
    if (!is_integer_kind(clux_type_kind_unwrap(clux_value_get_type(length)))) {
      const char *len_name = clux_type_get_name(clux_value_get_type(length));
      return clux_context_create_error_message(
          ctx,
          op_loc,
          "array length must be an integer, got '%s'",
          len_name ? len_name : "<unknown>");
    }
    int64_t count = 0;
    clux_value_t *len_err = clux_context_index_to_i64(ctx, length, &count);
    if (len_err) {
      return len_err;
    }
    if (count < 0) {
      return clux_context_create_error_message(
          ctx, op_loc, "array length must not be negative");
    }
    clux_value_t *err = NULL;
    clux_type_t *base = exec_operand_type(ctx, right_node, &err);
    if (!base) {
      return err;
    }
    clux_type_t *array_type =
        clux_context_get_array_type(ctx, base, (size_t)count);
    return clux_context_create_type_value(ctx, array_type, NULL, op_loc);
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

/* 下标/实例化同形（base[expr, ...]）：
   BASE 求值为 type value 时是泛型实例化，否则是下标取项。 */
clux_value_t *exec_expr_subscript(clux_context_t *ctx, node_t *node) {
  node_t *base_node = subscript_node_get_base(node);
  size_t arg_count = subscript_node_get_arg_count(node);
  clux_value_t *base = exec_operand(ctx, base_node);
  if (clux_value_is_error(base)) {
    return base;
  }
  /* 泛型实例化（Type[arg, ...]）：BASE 是 type value。
     TODO: 泛型实例化暂不支持——等泛型推断引擎就绪后接入。 */
  if (clux_type_kind_unwrap(clux_value_get_type(base)) == CLUX_TYPE_TYPE) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "generic instantiation is not supported yet");
  }
  if (arg_count != 1) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "subscript takes exactly one index");
  }
  clux_value_t *index = exec_operand(ctx, subscript_node_get_arg(node, 0));
  if (clux_value_is_error(index)) {
    return index;
  }
  /* 下标取项：返回 base 类型 view，写穿由通用 assign 完成。 */
  return clux_context_index(ctx, base, index, node_get_location(node));
}

/* 切片 base[lo?:hi?]：parser 允许两端省略（开放端点为 NULL 子节点），
   结果是借用 base 内存的 []base 胖指针（不拥有内存）。 */
clux_value_t *exec_expr_slice(clux_context_t *ctx, node_t *node) {
  clux_value_t *base = exec_operand(ctx, slice_node_get_base(node));
  if (clux_value_is_error(base)) {
    return base;
  }
  clux_value_t *lo = NULL;
  node_t *lo_node = slice_node_get_lo(node);
  if (lo_node) {
    lo = exec_operand(ctx, lo_node);
    if (clux_value_is_error(lo)) {
      return lo;
    }
  }
  clux_value_t *hi = NULL;
  node_t *hi_node = slice_node_get_hi(node);
  if (hi_node) {
    hi = exec_operand(ctx, hi_node);
    if (clux_value_is_error(hi)) {
      return hi;
    }
  }
  return clux_context_slice(ctx, base, lo, hi, node_get_location(node));
}

/* 函数类型定义 func |captures|(params) -> ret（白皮书 §2.7）：捕获表、
   参数序列与返回类型全部求值为 type value 后经引擎结构去重构造——
   捕获列表是类型签名的一部分。参数为 rest 表达式（C FFI 变参 `...`
   或参数包 `...Args`）暂不支持，等 FFI / 泛型推断批次接入。 */
clux_value_t *exec_expr_func_type(clux_context_t *ctx, node_t *node) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  /* 捕获项求值：公共通道逐项求值（名称、类型、重名拦截）。 */
  node_t *captures_node = func_type_node_get_captures(node);
  size_t capture_count =
      captures_node ? func_captures_node_get_capture_count(captures_node) : 0;
  clux_func_capture_t captures[capture_count];
  size_t filled = 0;
  clux_value_t *fail = NULL;
  for (size_t idx = 0; idx < capture_count && !fail; idx++) {
    node_t *capture = func_captures_node_get_capture(captures_node, idx);
    node_t *name_node = capture ? func_capture_node_get_name(capture) : NULL;
    node_t *type_node = capture ? func_capture_node_get_type(capture) : NULL;
    if (!exec_func_capture_entry(
            ctx, name_node, type_node, idx, captures, &filled, &fail)) {
      break;
    }
  }
  /* 参数序列求值：函数类型的形参是裸类型表达式，rest（变参/参数包）
     暂不支持。 */
  node_t *params_node = func_type_node_get_params(node);
  size_t param_count =
      params_node ? func_params_node_get_param_count(params_node) : 0;
  clux_type_t *params[param_count];
  for (size_t idx = 0; idx < param_count && !fail; idx++) {
    node_t *param = func_params_node_get_param(params_node, idx);
    if (!param) {
      fail = clux_context_create_error_message(
          ctx, loc, "parameter %zu requires a type", idx);
      break;
    }
    if (node_get_kind(param) == CLUX_NODE_EXPR_REST) {
      fail = clux_context_create_error_message(ctx,
                                               node_get_location(param),
                                               "variadic function type is "
                                               "not supported yet");
      break;
    }
    clux_value_t *err = NULL;
    params[idx] = exec_operand_type(ctx, param, &err);
    if (!params[idx]) {
      fail = err;
      break;
    }
  }
  /* 返回类型必填（'->' 后的类型表达式）。 */
  clux_type_t *return_type = NULL;
  node_t *return_node = func_type_node_get_return(node);
  if (!fail) {
    if (!return_node) {
      fail = clux_context_create_error_message(
          ctx, loc, "function type requires a return type");
    } else {
      clux_value_t *err = NULL;
      return_type = exec_operand_type(ctx, return_node, &err);
      if (!return_type) {
        fail = err;
      }
    }
  }
  clux_type_t *type = NULL;
  if (!fail) {
    type = clux_context_create_func_type(
        ctx, "func", captures, capture_count, params, param_count, return_type);
  }
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&captures[idx].name);
  }
  if (fail) {
    return fail;
  }
  if (!type) {
    return clux_context_create_error_message(
        ctx, loc, "function type creation failed");
  }
  return clux_context_create_type_value(ctx, type, NULL, loc);
}

/* 函数定义 func |captures| name(params): ret {body}（白皮书 §3.3）：
   定义本身隐含生成函数类型——捕获表、参数序列与返回类型全部求值为
   type value 后经引擎结构去重构造 FUNC 类型（与显式 func|...|(...)->...
   类型表达式产出的类型恒等），再创建函数零值、按名 define 进当前作用域
   （匿名函数 name 为 NULL 走匿名路径），最后挂接函数体 AST（借用）。
   捕获项默认值属于函数体（AST），不参与类型构造。泛型参数与 rest
   参数包（变参函数）暂不支持，等泛型推断 / FFI 批次接入。 */
clux_value_t *exec_expr_func_decl(clux_context_t *ctx, node_t *node) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  node_t *return_node = func_decl_node_get_return(node);
  if (!return_node) {
    return clux_context_create_error_message(
        ctx, loc, "function definition requires a return type");
  }
  node_t *body_node = func_decl_node_get_body(node);
  if (!body_node) {
    return clux_context_create_error_message(
        ctx, loc, "function definition requires a body");
  }
  if (func_decl_node_get_generics(node)) {
    return clux_context_create_error_message(
        ctx, loc, "generic function is not supported yet");
  }
  char *name = NULL;
  node_t *name_node = func_decl_node_get_name(node);
  if (name_node) {
    name = location_get(node_get_location(name_node), allocator);
  }
  /* 捕获列表形态：FUNC_CAPTURES 的唯一子节点是 FUNC_PARAMS，内为
     FUNC_PARAM（name: type），非 FUNC_PARAM 即参数包，暂不支持。 */
  node_t *captures_node = func_decl_node_get_captures(node);
  node_t *capture_params = NULL;
  size_t capture_count = 0;
  if (captures_node) {
    capture_params = func_captures_node_get_capture(captures_node, 0);
    if (capture_params &&
        node_get_kind(capture_params) == CLUX_NODE_FUNC_PARAMS) {
      capture_count = func_params_node_get_param_count(capture_params);
    }
  }
  clux_func_capture_t captures[capture_count];
  size_t filled = 0;
  clux_value_t *fail = NULL;
  for (size_t idx = 0; idx < capture_count && !fail; idx++) {
    node_t *entry = func_params_node_get_param(capture_params, idx);
    if (!entry || node_get_kind(entry) != CLUX_NODE_FUNC_PARAM) {
      fail = clux_context_create_error_message(
          ctx, loc, "generic capture is not supported yet");
      break;
    }
    if (!exec_func_capture_entry(ctx,
                                 func_param_node_get_name(entry),
                                 func_param_node_get_type(entry),
                                 idx,
                                 captures,
                                 &filled,
                                 &fail)) {
      break;
    }
  }
  /* 参数序列求值：FUNC_PARAM 取 TYPE 槽，rest（变参/参数包）暂不支持。 */
  node_t *params_node = func_decl_node_get_params(node);
  size_t param_count =
      params_node ? func_params_node_get_param_count(params_node) : 0;
  clux_type_t *params[param_count];
  for (size_t idx = 0; idx < param_count && !fail; idx++) {
    node_t *param = func_params_node_get_param(params_node, idx);
    if (!param) {
      fail = clux_context_create_error_message(
          ctx, loc, "parameter %zu requires a type", idx);
      break;
    }
    if (node_get_kind(param) == CLUX_NODE_EXPR_REST) {
      fail = clux_context_create_error_message(
          ctx,
          node_get_location(param),
          "variadic function parameter is not supported yet");
      break;
    }
    node_t *param_type_node = node_get_kind(param) == CLUX_NODE_FUNC_PARAM
                                  ? func_param_node_get_type(param)
                                  : param;
    if (!param_type_node) {
      fail = clux_context_create_error_message(
          ctx, loc, "parameter %zu requires a type", idx);
      break;
    }
    clux_value_t *err = NULL;
    params[idx] = exec_operand_type(ctx, param_type_node, &err);
    if (!params[idx]) {
      fail = err;
      break;
    }
  }
  /* 返回类型（':' 后的类型表达式，必填已校验）。 */
  clux_type_t *return_type = NULL;
  if (!fail) {
    clux_value_t *err = NULL;
    return_type = exec_operand_type(ctx, return_node, &err);
    if (!return_type) {
      fail = err;
    }
  }
  /* 隐含生成函数类型：与显式类型表达式共用构造器，结构一致即恒等。 */
  clux_type_t *type = NULL;
  if (!fail) {
    type = clux_context_create_func_type(ctx,
                                         name ? name : "func",
                                         captures,
                                         filled,
                                         params,
                                         param_count,
                                         return_type);
  }
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&captures[idx].name);
  }
  if (fail) {
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  if (!type) {
    allocator_free(allocator, (void **)&name);
    return clux_context_create_error_message(
        ctx, loc, "function type creation failed");
  }
  /* 创建函数零值并按名 define 进当前作用域（重名报 duplicate，
     匿名 name NULL 安全）；随后挂接函数体 AST（借用，不拥有）。 */
  clux_value_t *value = clux_context_create_func(ctx, type, name, loc);
  allocator_free(allocator, (void **)&name);
  if (clux_value_is_error(value)) {
    return value;
  }
  value = clux_context_func_set_ast(ctx, value, body_node, loc);
  if (clux_value_is_error(value)) {
    return value;
  }
  return value;
}

/* 元组类型定义 <T, U, ...>（白皮书 §2.6）：元素逐个求值为 type value
   后经引擎结构去重构造，空元组 <> 合法；<...Ts> 参数包（rest 表达式）
   暂不支持，等泛型推断引擎就绪后接入。 */
clux_value_t *exec_expr_tuple(clux_context_t *ctx, node_t *node) {
  size_t count = tuple_node_get_elem_count(node);
  clux_type_t *elems[count];
  for (size_t idx = 0; idx < count; idx++) {
    node_t *elem = tuple_node_get_elem(node, idx);
    if (node_get_kind(elem) == CLUX_NODE_EXPR_REST) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(elem),
          "generic parameter pack is not supported yet");
    }
    clux_value_t *err = NULL;
    elems[idx] = exec_operand_type(ctx, elem, &err);
    if (!elems[idx]) {
      return err;
    }
  }
  clux_type_t *type = clux_context_get_tuple_type(ctx, elems, count);
  if (!type) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "tuple type creation failed");
  }
  return clux_context_create_type_value(
      ctx, type, NULL, node_get_location(node));
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