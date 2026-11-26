#include "exec/expr.h"
#include <stdio.h>
#include "core/allocator.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/scope.h"
#include "engine/struct.h"
#include "engine/tuple.h"
#include "engine/type.h"
#include "engine/value.h"
#include "engine/array.h"
#include "engine/cunion.h"
#include "engine/enum.h"
#include "engine/bitfield.h"
#include "engine/interface_type.h"
#include "engine/union.h"
#include "exec/exec.h"
#include "exec/generic.h"
#include "parser/node.h"
#include "parser/node_assign.h"
#include "parser/node_binary.h"
#include "parser/node_bitfield_decl.h"
#include "parser/node_bitfield_field.h"
#include "parser/node_construct.h"
#include "parser/node_construct_field.h"
#include "parser/node_enum_decl.h"
#include "parser/node_enum_variant.h"
#include "parser/node_field.h"
#include "parser/node_interface_decl.h"
#include "parser/node_func_capture.h"
#include "parser/node_func_decl.h"
#include "parser/node_generic_param.h"
#include "parser/node_func_param.h"
#include "parser/node_func_type.h"
#include "parser/node_group.h"
#include "parser/node_slice.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_var.h"
#include "parser/node_struct_decl.h"
#include "parser/node_struct_member.h"
#include "parser/node_subscript.h"
#include "parser/node_rest.h"
#include "parser/node_ternary.h"
#include "parser/node_tuple.h"
#include "parser/node_union.h"
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
clux_type_t *
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

/* 函数定义路径捕获项（区别于类型表达式的签名声明）：捕获的是定义点
   作用域的现值。|f: T| 带标注约束类型；|f| 无标注从作用域查询现值
   （shadow 亦可），以查得值的类型推断捕获类型；|h = expr| 无标注带
   默认值（defer 的改名捕获形态）从默认值表达式推断类型。重名在引擎
   校验前拦截以携带源位置。成功并入 captures[(*filled)++]；失败返回
   false 且 *out_fail 携带 error（已并入的项由调用方统一释放）。 */
bool exec_func_decl_capture_entry(clux_context_t *ctx,
                                  node_t *name_node,
                                  node_t *type_node,
                                  node_t *default_node,
                                  size_t idx,
                                  clux_func_capture_t *captures,
                                  size_t *filled,
                                  clux_value_t **out_fail) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name =
      name_node ? location_get(node_get_location(name_node), allocator) : NULL;
  if (!name) {
    *out_fail = clux_context_create_error_message(
        ctx, NULL, "capture %zu requires a name", idx);
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
  clux_type_t *type = NULL;
  if (type_node) {
    clux_value_t *err = NULL;
    type = exec_operand_type(ctx, type_node, &err);
    if (!type) {
      *out_fail = err;
      allocator_free(allocator, (void **)&name);
      return false;
    }
  } else if (default_node) {
    /* |h = expr| 改名捕获（defer 形态）：从默认值表达式推断捕获类型。
       值仅为类型推断求出后即弃（本体归 scope 统一销毁）。 */
    clux_value_t *source = exec_operand(ctx, default_node);
    if (!source || clux_value_is_error(source)) {
      *out_fail = clux_value_is_error(source)
                      ? source
                      : clux_context_create_error_message(
                            ctx,
                            node_get_location(default_node),
                            "capture %zu requires a value",
                            idx);
      allocator_free(allocator, (void **)&name);
      return false;
    }
    type = clux_value_get_type(source);
  } else {
    /* 无标注 |f|：以定义点作用域现值的类型推断捕获类型。 */
    clux_value_t *found =
        clux_context_lookup(ctx, name, node_get_location(name_node));
    if (clux_value_is_error(found)) {
      *out_fail = found;
      allocator_free(allocator, (void **)&name);
      return false;
    }
    type = clux_value_get_type(found);
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
  /* 数组类型定义 [N]T：SYMBOL 首 '[' 尾 ']' 且 LEFT 为长度表达式（切片
     类型前缀 []T 的 LEFT 为 NULL，已由上方前缀形态处理）。'[]' 切片与
     '[N]' 数组共用二元形态：SYMBOL location 区间跨越方括号（长度居中时
     location_get 读出 "[2]" 而非 "[]"），按结构判别（与 check.c
     check_type_eval 同构）。长度先于元素求值，须为非负整数；元素须为
     type value。 */
  char *array_op_text = location_get(op_loc, allocator);
  bool is_array_type_def = array_op_text && array_op_text[0] == '[' &&
                           array_op_text[strlen(array_op_text) - 1] == ']';
  allocator_free(allocator, (void **)&array_op_text);
  if (is_array_type_def) {
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
  /* 显式转换 as（白皮书 §5.7）：左操作数为值，右操作数为类型表达式，
     走宽松 cast 通道——合法性由源类型 vtable 的 cast 规则裁决。 */
  if (location_is(op_loc, "as")) {
    clux_value_t *left = exec_operand(ctx, left_node);
    if (clux_value_is_error(left)) {
      return left;
    }
    clux_value_t *err = NULL;
    clux_type_t *type = exec_operand_type(ctx, right_node, &err);
    if (!type) {
      return err;
    }
    return clux_context_cast(ctx, left, type, op_loc);
  }
  /* 成员访问 obj.field（白皮书 §3.5）：与下标同源的 view 语义——返回
     字段类型的读写双向 view（与实例内存共享，自带引用底色），读取直接
     用，写入由通用 assign 写穿 view，无读/写两分。分隔符与名字由
     parser 保证（RIGHT 恒 identifier，容错分支手工 AST 校验拦截）。 */
  if (location_is(op_loc, ".")) {
    clux_value_t *receiver = exec_operand(ctx, left_node);
    if (clux_value_is_error(receiver)) {
      return receiver;
    }
    if (!right_node ||
        node_get_kind(right_node) != CLUX_NODE_LITERAL_IDENTIFIER) {
      return clux_context_create_error_message(
          ctx, op_loc, "member access requires a member name");
    }
    char *name = location_get(node_get_location(right_node),
                              clux_context_get_allocator(ctx));
    clux_value_t *view = clux_context_field(ctx, receiver, name, op_loc);
    allocator_free(clux_context_get_allocator(ctx), (void **)&name);
    return view;
  }
  /* 类型命名空间访问 Type::member（白皮书 §3.5.1/§3.6）：LEFT 须为
     type value，成员经引擎通道解析（struct attr/method/subtype、enum
     variant），方法本体是 func 值借用返回（静态调用由此取值）。 */
  if (location_is(op_loc, "::")) {
    clux_value_t *err = NULL;
    clux_type_t *type = exec_operand_type(ctx, left_node, &err);
    if (!type) {
      return err;
    }
    if (!right_node ||
        node_get_kind(right_node) != CLUX_NODE_LITERAL_IDENTIFIER) {
      return clux_context_create_error_message(
          ctx, op_loc, "namespace access requires a member name");
    }
    char *name = location_get(node_get_location(right_node),
                              clux_context_get_allocator(ctx));
    clux_value_t *member = clux_context_get_member(ctx, type, name, op_loc);
    allocator_free(clux_context_get_allocator(ctx), (void **)&name);
    return member;
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
  /* 泛型实例化（tmpl[arg, ...]，白皮书 §6.6）：BASE 是模板值
     （type_template / func_template）时走实例化通道展开。 */
  clux_type_kind_t base_kind = clux_type_kind_unwrap(clux_value_get_type(base));
  if (base_kind == CLUX_TYPE_TYPE_TEMPLATE ||
      base_kind == CLUX_TYPE_FUNC_TEMPLATE) {
    return exec_generic_instantiate(ctx, base, node);
  }
  /* 非模板类型的下标取项形态 Type[...]：泛型实例化语义不成立
     （Type 不是模板，无可绑定参数表）。 */
  if (base_kind == CLUX_TYPE_TYPE) {
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

/* 参数包引用求值（白皮书 §6.4）：...Args 的 Args 求值为包绑定——
   type value 描述 TUPLE 类型（元素即包内容，实例化/推断通道折叠）。
   返回该 TUPLE；失败置 *err 并返回 NULL。 */
static clux_type_t *
exec_pack_tuple_type(clux_context_t *ctx, node_t *rest, clux_value_t **err) {
  node_t *expr = rest_node_get_expr(rest);
  if (!expr) {
    *err = clux_context_create_error_message(
        ctx, node_get_location(rest), "parameter pack requires a name");
    return NULL;
  }
  clux_value_t *value = exec_operand(ctx, expr);
  if (!value || clux_value_is_error(value)) {
    *err = value;
    return NULL;
  }
  if (clux_type_kind_unwrap(clux_value_get_type(value)) != CLUX_TYPE_TYPE) {
    const char *value_name = clux_type_get_name(clux_value_get_type(value));
    *err = clux_context_create_error_message(
        ctx,
        node_get_location(rest),
        "parameter pack '%s' must be bound to a type",
        value_name ? value_name : "<unknown>");
    return NULL;
  }
  clux_type_t *described = *(clux_type_t **)clux_value_get_raw(value, 0);
  if (clux_type_get_kind(described) != CLUX_TYPE_TUPLE) {
    const char *pack_name = clux_type_get_name(described);
    *err = clux_context_create_error_message(
        ctx,
        node_get_location(rest),
        "parameter pack must expand to a tuple type, got '%s'",
        pack_name ? pack_name : "<unknown>");
    return NULL;
  }
  return described;
}

/* 函数类型定义 func |captures|(params) -> ret（白皮书 §2.7）：捕获表、
   参数序列与返回类型全部求值为 type value 后经引擎结构去重构造——
   捕获列表是类型签名的一部分。rest 形参（参数包 `...Args`，§6.4）
   展开为绑定包的各元素。 */
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
  /* 参数序列求值：函数类型的形参是裸类型表达式；rest 形参（参数包
     `...Args`，§6.4）先求值包定展开总长，再填充展开——rest 必须最后。 */
  node_t *params_node = func_type_node_get_params(node);
  size_t param_count =
      params_node ? func_params_node_get_param_count(params_node) : 0;
  clux_type_t *pack_types[param_count ? param_count : 1];
  size_t expanded = 0;
  for (size_t idx = 0; idx < param_count && !fail; idx++) {
    node_t *param = func_params_node_get_param(params_node, idx);
    if (!param) {
      fail = clux_context_create_error_message(
          ctx, loc, "parameter %zu requires a type", idx);
      break;
    }
    if (node_get_kind(param) == CLUX_NODE_EXPR_REST) {
      if (idx + 1 != param_count) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(param),
            "rest parameter must be the last parameter");
        break;
      }
      clux_value_t *err = NULL;
      pack_types[idx] = exec_pack_tuple_type(ctx, param, &err);
      if (!pack_types[idx]) {
        fail = err;
        break;
      }
      expanded += clux_tuple_elem_count(pack_types[idx]);
    } else {
      pack_types[idx] = NULL;
      expanded++;
    }
  }
  clux_type_t *params[expanded ? expanded : 1];
  size_t slot = 0;
  for (size_t idx = 0; idx < param_count && !fail; idx++) {
    if (pack_types[idx]) {
      size_t count = clux_tuple_elem_count(pack_types[idx]);
      for (size_t eidx = 0; eidx < count; eidx++) {
        params[slot++] = clux_tuple_get_elem(pack_types[idx], eidx);
      }
      continue;
    }
    node_t *param = func_params_node_get_param(params_node, idx);
    clux_value_t *err = NULL;
    params[slot] = exec_operand_type(ctx, param, &err);
    if (!params[slot]) {
      fail = err;
      break;
    }
    slot++;
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
        ctx, "func", captures, capture_count, params, slot, return_type);
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
   参数包（变参函数）暂不支持，等泛型推断 / FFI 批次接入。
   declare_named=false 时仅求值出函数值不按名 define（装饰器包装路径：
   具名化推迟到包装完成后的 declare_value）。 */
/* 定义点捕获快照：类型构造完成后按捕获列表逐项写入初值。
   |f: T = expr| 默认值求值结果作初值（不查作用域）；其余从定义点
   作用域查询现值（TDZ 源拒绝），经捕获 view + assign 以 copy 语义
   快照进函数值存储——写入独立于外层变量，类型不匹配由 assign 的
   safe_cast 拦截。错误逐一向上传播。 */
clux_value_t *exec_func_decl_init_captures(clux_context_t *ctx,
                                           clux_value_t *func,
                                           node_t *captures_node,
                                           node_t *node) {
  if (!captures_node) {
    return NULL;
  }
  node_t *capture_params = func_captures_node_get_capture(captures_node, 0);
  if (!capture_params) {
    return NULL;
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  size_t count = func_params_node_get_param_count(capture_params);
  for (size_t idx = 0; idx < count; idx++) {
    node_t *entry = func_params_node_get_param(capture_params, idx);
    node_t *name_node = entry ? func_param_node_get_name(entry) : NULL;
    char *name = name_node
                     ? location_get(node_get_location(name_node), allocator)
                     : NULL;
    if (!name) {
      return clux_context_create_error_message(
          ctx, node_get_location(node), "capture %zu requires a name", idx);
    }
    clux_value_t *view = clux_context_func_capture(
        ctx, func, name, node_get_location(name_node));
    if (clux_value_is_error(view)) {
      allocator_free(allocator, (void **)&name);
      return view;
    }
    node_t *default_node = entry ? func_param_node_get_default(entry) : NULL;
    clux_value_t *source = NULL;
    if (default_node) {
      source = exec_operand(ctx, default_node);
    } else {
      clux_value_t *found =
          clux_context_lookup(ctx, name, node_get_location(name_node));
      if (!clux_value_is_error(found)) {
        found = clux_context_consume_value(
            ctx, found, node_get_location(name_node));
      }
      source = found;
    }
    allocator_free(allocator, (void **)&name);
    if (clux_value_is_error(source)) {
      return source;
    }
    clux_value_t *assigned =
        clux_context_assign(ctx, view, source, node_get_location(name_node));
    if (clux_value_is_error(assigned)) {
      return assigned;
    }
  }
  return NULL;
}

/* binding_params 非空即实例模式（泛型实例化通道，exec/generic.c 进入）：
   跳过泛型门禁、不按名 define，泛型绑定作为尾随捕获并入签名（interim
   方案——函数体自由名字只解析到形参/捕获/模块级，捕获表是绑定进帧的
   唯一通道），创建后从实例化 scope 按参名初始化绑定捕获。 */
static clux_value_t *exec_func_decl_impl(clux_context_t *ctx,
                                         node_t *node,
                                         bool declare_named,
                                         bool defer_captures,
                                         node_t *binding_params) {
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
  char *name = NULL;
  node_t *name_node = func_decl_node_get_name(node);
  if (name_node) {
    name = location_get(node_get_location(name_node), allocator);
  }
  /* 泛型定义产 func 模板值（白皮书 §6.1）：data 存声明 AST 本身，
     实例化通道取回后按下标实参在绑定环境内重执行。 */
  if (func_decl_node_get_generics(node) && !binding_params) {
    clux_value_t *template_value = clux_context_create_func_template(
        ctx, node, declare_named ? name : NULL, loc);
    allocator_free(allocator, (void **)&name);
    return template_value;
  }
  if (binding_params) {
    declare_named = false; /* 实例不 define：本体归实例化出口 clone */
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
  /* 实例模式：泛型绑定计数（AST 捕获在前，绑定在后）。 */
  size_t binding_count =
      binding_params ? generic_params_node_get_param_count(binding_params) : 0;
  clux_func_capture_t captures[capture_count + binding_count];
  size_t filled = 0;
  clux_value_t *fail = NULL;
  for (size_t idx = 0; idx < capture_count && !fail; idx++) {
    node_t *entry = func_params_node_get_param(capture_params, idx);
    if (!entry || node_get_kind(entry) != CLUX_NODE_FUNC_PARAM) {
      fail = clux_context_create_error_message(
          ctx, loc, "generic capture is not supported yet");
      break;
    }
    if (!exec_func_decl_capture_entry(ctx,
                                      func_param_node_get_name(entry),
                                      func_param_node_get_type(entry),
                                      NULL,
                                      idx,
                                      captures,
                                      &filled,
                                      &fail)) {
      break;
    }
  }
  /* 泛型绑定捕获项：类型参数绑定是 type value（类型为 "type" 单例），
     值参数绑定按声明类型（§6.3 comptime）。参数包绑定同类型参数——
     type value 描述 TUPLE。名字经 create_func_type 驻留后释放（与
     AST 捕获同一通道）。 */
  for (size_t idx = 0; idx < binding_count && !fail; idx++) {
    node_t *param = generic_params_node_get_param(binding_params, idx);
    if (param && node_get_kind(param) == CLUX_NODE_EXPR_REST) {
      /* 参数包（§6.4）：名字在 rest 的 EXPR 槽，类型同 type 单例。 */
      node_t *pname_node = rest_node_get_expr(param);
      if (!pname_node) {
        fail = clux_context_create_error_message(
            ctx, loc, "generic parameter %zu requires a name", idx);
        break;
      }
      char *bname = location_get(node_get_location(pname_node), allocator);
      if (!bname) {
        fail = clux_context_create_error_message(
            ctx, loc, "generic parameter %zu requires a name", idx);
        break;
      }
      captures[filled].name = bname;
      captures[filled].type = clux_context_get_type_type(ctx);
      filled++;
      continue;
    }
    node_t *bname_node = param ? generic_param_node_get_name(param) : NULL;
    if (!bname_node) {
      fail = clux_context_create_error_message(
          ctx, loc, "generic parameter %zu requires a name", idx);
      break;
    }
    char *bname = location_get(node_get_location(bname_node), allocator);
    node_t *btype_node = generic_param_node_get_type(param);
    clux_type_t *btype = NULL;
    if (btype_node) {
      clux_value_t *err = NULL;
      btype = exec_operand_type(ctx, btype_node, &err);
      if (!btype) {
        fail = err;
        allocator_free(allocator, (void **)&bname);
        break;
      }
    } else {
      btype = clux_context_get_type_type(ctx);
    }
    captures[filled].name = bname;
    captures[filled].type = btype;
    filled++;
  }
  /* 参数序列求值：FUNC_PARAM 取 TYPE 槽；rest 形参（§6.4 参数包
     `args: ...Args`，TYPE 槽为 EXPR_REST；或裸 `...Args`）展开为绑定
     包的各元素——先求值各包定展开总长，再填表。rest 必须最后。 */
  node_t *params_node = func_decl_node_get_params(node);
  size_t param_count =
      params_node ? func_params_node_get_param_count(params_node) : 0;
  clux_type_t *pack_types[param_count ? param_count : 1];
  size_t expanded = 0;
  for (size_t idx = 0; idx < param_count && !fail; idx++) {
    node_t *param = func_params_node_get_param(params_node, idx);
    if (!param) {
      fail = clux_context_create_error_message(
          ctx, loc, "parameter %zu requires a type", idx);
      break;
    }
    node_t *rest = node_get_kind(param) == CLUX_NODE_EXPR_REST
                       ? param
                       : (node_get_kind(param) == CLUX_NODE_FUNC_PARAM
                              ? func_param_node_get_type(param)
                              : NULL);
    if (rest && node_get_kind(rest) == CLUX_NODE_EXPR_REST) {
      if (idx + 1 != param_count) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(rest),
            "rest parameter must be the last parameter");
        break;
      }
      clux_value_t *err = NULL;
      pack_types[idx] = exec_pack_tuple_type(ctx, rest, &err);
      if (!pack_types[idx]) {
        fail = err;
        break;
      }
      expanded += clux_tuple_elem_count(pack_types[idx]);
    } else {
      pack_types[idx] = NULL;
      expanded++;
    }
  }
  clux_type_t *params[expanded ? expanded : 1];
  size_t slot = 0;
  for (size_t idx = 0; idx < param_count && !fail; idx++) {
    if (pack_types[idx]) {
      size_t count = clux_tuple_elem_count(pack_types[idx]);
      for (size_t eidx = 0; eidx < count; eidx++) {
        params[slot++] = clux_tuple_get_elem(pack_types[idx], eidx);
      }
      continue;
    }
    node_t *param = func_params_node_get_param(params_node, idx);
    node_t *param_type_node = node_get_kind(param) == CLUX_NODE_FUNC_PARAM
                                  ? func_param_node_get_type(param)
                                  : param;
    if (!param_type_node) {
      fail = clux_context_create_error_message(
          ctx, loc, "parameter %zu requires a type", idx);
      break;
    }
    clux_value_t *err = NULL;
    params[slot] = exec_operand_type(ctx, param_type_node, &err);
    if (!params[slot]) {
      fail = err;
      break;
    }
    slot++;
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
    type = clux_context_create_func_type(
        ctx, name ? name : "func", captures, filled, params, slot, return_type);
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
     匿名 name NULL 安全）；随后挂接整个声明节点 AST（借用，不拥有）——
     形参名只存在于 AST 不在类型（类型判等不含名），栈帧绑形参时
     需读到 PARAMS 槽。 */
  clux_value_t *value =
      clux_context_create_func(ctx, type, declare_named ? name : NULL, loc);
  if (clux_value_is_error(value)) {
    allocator_free(allocator, (void **)&name);
    return value;
  }
  /* 挂接函数体：名字与值命名同源（匿名闭包传 NULL），context 内部
     驻留后由 function 对象借用。 */
  value = clux_context_func_set_ast(
      ctx, value, node, declare_named ? name : NULL, loc);
  allocator_free(allocator, (void **)&name);
  if (clux_value_is_error(value)) {
    return value;
  }
  /* 定义点捕获快照：函数值就绪后立即按定义点作用域写入捕获初值。
     defer_captures 时跳过（块提升 phase 1 仅定义——全部函数名字就位后
     由 phase 2 统一初始化，前向捕获由此成立）。 */
  if (!defer_captures) {
    clux_value_t *capture_fail = exec_func_decl_init_captures(
        ctx, value, func_decl_node_get_captures(node), node);
    if (capture_fail) {
      return capture_fail;
    }
  }
  /* 实例模式：绑定捕获初值从实例化 scope 按参名取（bind 阶段已
     declare），经捕获 view + assign 以 copy 语义快照进函数值存储——
     实例值此后独立于实例化 scope 存活（块出口 clone）。 */
  for (size_t idx = 0; idx < binding_count; idx++) {
    node_t *param = generic_params_node_get_param(binding_params, idx);
    node_t *bname_node = param ? generic_param_node_get_name(param) : NULL;
    if (!bname_node) {
      continue; /* 签名构造期已报错，不可达 */
    }
    char *bname = location_get(node_get_location(bname_node), allocator);
    clux_value_t *view =
        clux_context_func_capture(ctx, value, bname, node_get_location(node));
    if (clux_value_is_error(view)) {
      allocator_free(allocator, (void **)&bname);
      return view;
    }
    clux_value_t *bound =
        clux_context_lookup(ctx, bname, node_get_location(bname_node));
    if (!clux_value_is_error(bound)) {
      bound =
          clux_context_consume_value(ctx, bound, node_get_location(bname_node));
    }
    allocator_free(allocator, (void **)&bname);
    if (clux_value_is_error(bound)) {
      return bound;
    }
    clux_value_t *assigned =
        clux_context_assign(ctx, view, bound, node_get_location(bname_node));
    if (clux_value_is_error(assigned)) {
      return assigned;
    }
  }
  return value;
}

clux_value_t *exec_expr_func_decl(clux_context_t *ctx, node_t *node) {
  return exec_func_decl_impl(ctx, node, true, false, NULL);
}

/* 泛型函数实例：在绑定环境（实例化 scope）内重执行声明 AST——
   签名（形参/返回类型）此刻可引用泛型参数名。 */
clux_value_t *exec_func_decl_instance(clux_context_t *ctx, node_t *node) {
  return exec_func_decl_impl(
      ctx, node, false, false, func_decl_node_get_generics(node));
}

/* 匿名函数值求值：仅求值出函数值（带类型与 AST 挂接），不按名 define。
   函数语句装饰器包装路径用——先匿名求值源函数值，包装完成后由
   declare_value 具名化，避开 scope 的重名冲突（scope 无重绑定通道）。 */
clux_value_t *exec_func_decl_value(clux_context_t *ctx, node_t *node) {
  return exec_func_decl_impl(ctx, node, false, false, NULL);
}

/* 块提升两阶段捕获（前向捕获通道）——
   phase 1 exec_func_decl_define：仅求值签名并按名 define（挂 AST），
   跳过定义点捕获初始化；phase 2 exec_func_decl_bind_captures：全部
   提升函数名字就位后统一做定义点捕获快照——f 捕获后声明的 g 成立。
   注：相互递归（f/g 互相捕获）被类型判等拒绝（func.c signature_equal
   含捕获表，类型无限回归无法闭式书写，语义决策 2026-11-20）。TDZ 源
   仍拒绝。phase 2 必须与 phase 1 同作用域执行（lookup 按名取捕获初值）。 */
clux_value_t *exec_func_decl_define(clux_context_t *ctx, node_t *node) {
  return exec_func_decl_impl(ctx, node, true, true, NULL);
}

clux_value_t *exec_func_decl_bind_captures(clux_context_t *ctx,
                                           clux_value_t *func,
                                           node_t *node) {
  return exec_func_decl_init_captures(
      ctx, func, func_decl_node_get_captures(node), node);
}

/* union 定义（白皮书 §3.7 tag union、§3.8 C union）：tag union 与
   cunion 语法阶段共用 EXPR_UNION，exec 按 KEYWORD 文本分派引擎构造器
   （union → tag union，cunion → C union），成员表逐项求值为 type 后
   整体构造，引擎结构去重恒等返回；产出 type value，具名（NAME 槽
   非空）时按名 define 进当前作用域（重名报 duplicate），匿名（NAME
   缺省）合法——引擎类型名用关键字文本回退，不 define。成员重名/
   缺名/缺类型在引擎校验前拦截以携带源位置。泛型参数（<>）暂不
   支持，等泛型推断引擎就绪后接入。declare_named=false 时仅求值出
   不具名的 type value（装饰器包装路径：具名化推迟到包装完成后的
   declare_value）。 */
/* 块提升空壳查找：具名 union/cunion 在当前作用域链上命中「type value
   包裹的未定局同 kind 壳」时返回该壳（复用补全），其余情形（未命中、
   非 type value、kind 不符、已 seal 的外层同名类型）返回 NULL 走常规
   构造。 */
static clux_type_t *
union_shell_lookup(clux_context_t *ctx, const char *name, bool is_cunion) {
  if (!name) {
    return NULL;
  }
  clux_value_t *bound = clux_scope_get(clux_context_get_scope(ctx), name);
  if (!bound ||
      clux_type_kind_unwrap(clux_value_get_type(bound)) != CLUX_TYPE_TYPE) {
    return NULL;
  }
  clux_type_t *inner = *(clux_type_t **)clux_value_get_raw(bound, 0);
  if (!inner || (is_cunion ? clux_type_get_kind(inner) != CLUX_TYPE_CUNION
                           : clux_type_get_kind(inner) != CLUX_TYPE_UNION)) {
    return NULL;
  }
  if (clux_type_is_seal(inner)) {
    return NULL;
  }
  return inner;
}

static clux_value_t *
exec_expr_union_impl(clux_context_t *ctx, node_t *node, bool declare_named) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  node_t *keyword_node = union_node_get_keyword(node);
  bool is_cunion = false;
  if (keyword_node) {
    char *keyword = location_get(node_get_location(keyword_node), allocator);
    is_cunion = keyword && strcmp(keyword, "cunion") == 0;
    allocator_free(allocator, (void **)&keyword);
  }
  if (union_node_get_generics(node)) {
    return clux_context_create_error_message(
        ctx, loc, "generic union is not supported yet");
  }
  node_t *name_node = union_node_get_name(node);
  char *name =
      name_node ? location_get(node_get_location(name_node), allocator) : NULL;
  /* 匿名 union 合法：错误消息与引擎构造用关键字文本回退类型名，
     define 只在具名（name 非空）时发生。 */
  const char *type_name = name;
  if (!type_name) {
    type_name = is_cunion ? "cunion" : "union";
  }
  node_t *body = union_node_get_body(node);
  size_t count = body ? fields_node_get_field_count(body) : 0;
  if (count == 0) {
    clux_value_t *fail = clux_context_create_error_message(
        ctx, loc, "union '%s' requires at least one member", type_name);
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  /* tag union 与 cunion 成员表同构（name+type 各一槽），构造器不同：
     按 KEYWORD 选择填充其中一张（同一时刻只填一张，成员名生命周期
     归调用方，构造时经 strs 驻留）。 */
  clux_union_member_t union_members[count];
  clux_cunion_member_t cunion_members[count];
  size_t filled = 0;
  clux_value_t *fail = NULL;
  for (size_t idx = 0; idx < count && !fail; idx++) {
    node_t *field = fields_node_get_field(body, idx);
    node_t *member_name_node = field ? field_node_get_name(field) : NULL;
    char *member_name =
        member_name_node
            ? location_get(node_get_location(member_name_node), allocator)
            : NULL;
    if (!member_name) {
      fail = clux_context_create_error_message(
          ctx,
          field ? node_get_location(field) : loc,
          "union '%s' member %zu requires a name",
          type_name,
          idx);
      break;
    }
    node_t *type_node = field ? field_node_get_type(field) : NULL;
    clux_type_t *member_type = NULL;
    if (!type_node) {
      fail = clux_context_create_error_message(
          ctx,
          field ? node_get_location(field) : loc,
          "union '%s' member '%s' requires a type",
          type_name,
          member_name);
    } else {
      clux_value_t *err = NULL;
      member_type = exec_operand_type(ctx, type_node, &err);
      if (!member_type) {
        fail = err;
      } else if (!clux_type_is_seal(member_type)) {
        /* 完整性门禁（与 context 创建路径同语）：值位置成员必须已定局，
           指针以空壳为 base 合法（ptr 构造即 seal）。 */
        const char *member_type_name = clux_type_get_name(member_type);
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(type_node),
            "union '%s' member '%s' has incomplete type '%s'",
            type_name,
            member_name,
            member_type_name ? member_type_name : "<unknown>");
      }
    }
    if (!fail) {
      for (size_t prev = 0; prev < filled; prev++) {
        const char *prev_name =
            is_cunion ? cunion_members[prev].name : union_members[prev].name;
        if (strcmp(member_name, prev_name) == 0) {
          fail = clux_context_create_error_message(
              ctx,
              node_get_location(member_name_node),
              "union '%s' has duplicate member '%s'",
              type_name,
              member_name);
          break;
        }
      }
    }
    if (fail) {
      allocator_free(allocator, (void **)&member_name);
      break;
    }
    if (is_cunion) {
      cunion_members[filled].name = member_name;
      cunion_members[filled].type = member_type;
    } else {
      union_members[filled].name = member_name;
      union_members[filled].type = member_type;
    }
    filled++;
  }
  clux_type_t *type = NULL;
  /* 块提升空壳复用：命中本块预注册的未定局壳时逐成员补全 + 定局
     （壳已随提升 pass 托管进 named_types，不重复注册）；否则走常规
     context 一次性构造。 */
  clux_type_t *shell = union_shell_lookup(ctx, name, is_cunion);
  if (!fail && shell) {
    for (size_t idx = 0; idx < filled; idx++) {
      const char *member_name =
          is_cunion ? cunion_members[idx].name : union_members[idx].name;
      clux_type_t *member_type =
          is_cunion ? cunion_members[idx].type : union_members[idx].type;
      bool added = is_cunion ? clux_cunion_type_add_member(
                                   shell, allocator, member_name, member_type)
                             : clux_union_type_add_member(
                                   shell, allocator, member_name, member_type);
      if (!added) {
        fail = clux_context_create_error_message(
            ctx, loc, "union '%s' shell fill failed", type_name);
        break;
      }
    }
    if (!fail) {
      if (is_cunion) {
        clux_cunion_type_seal(shell);
      } else {
        clux_union_type_seal(shell);
      }
      type = shell;
    }
  }
  if (!fail && !type) {
    type = is_cunion ? clux_context_create_cunion_type(
                           ctx, type_name, cunion_members, filled)
                     : clux_context_create_union_type(
                           ctx, type_name, union_members, filled);
  }
  /* 成员名经 context 路径 strs 驻留，本地拷贝统一释放。 */
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator,
                   is_cunion ? (void **)&cunion_members[idx].name
                             : (void **)&union_members[idx].name);
  }
  if (fail) {
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  if (!type) {
    clux_value_t *create_fail = clux_context_create_error_message(
        ctx, loc, "union type creation failed");
    allocator_free(allocator, (void **)&name);
    return create_fail;
  }
  /* 空壳已按名绑定在块作用域（提升 pass 的预注册），重复具名会撞
     duplicate declaration；补全路径产出匿名 type value 作语句值。 */
  clux_value_t *value = clux_context_create_type_value(
      ctx, type, (declare_named && name && !shell) ? name : NULL, loc);
  allocator_free(allocator, (void **)&name);
  return value;
}

clux_value_t *exec_expr_union(clux_context_t *ctx, node_t *node) {
  return exec_expr_union_impl(ctx, node, true);
}

/* 匿名 union 值求值：仅求值出不具名的 type value（不按名 define），
   union 语句装饰器包装路径用——包装完成后由 declare_value 具名化。 */
clux_value_t *exec_expr_union_value(clux_context_t *ctx, node_t *node) {
  return exec_expr_union_impl(ctx, node, false);
}

/* 整数 kind 判定（调用方先经 clux_type_kind_unwrap 去 qualifier）：
   enum 底层类型与 variant 常量共用。 */
static bool enum_integer_kind(clux_type_kind_t kind) {
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

/* enum 定义（白皮书 §3.6）：enum Name: Underlying { Variant = value, ... }，
   底层类型必须是整数或 str 类型（求值 UNDERLYING 为 type value 后交引擎
   构造），variant 值是编译期常量（底层为整数时须整数常量、按值自身宽度
   取原始位型；底层为 str 时须字符串常量）。结构
   去重恒等返回；产出 type value，具名（NAME 槽非空）时按名 define 进
   当前作用域（重名报 duplicate），匿名（NAME 缺省）合法——引擎类型名
   用关键字文本回退，不 define。variant 缺名/缺值/重名在引擎校验前拦截
   以携带源位置。declare_named=false 时仅求值出不具名的 type value
   （装饰器包装路径：具名化推迟到包装完成后的 declare_value）。 */
static clux_value_t *exec_expr_enum_decl_impl(clux_context_t *ctx,
                                              node_t *node,
                                              bool declare_named) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  node_t *name_node = enum_decl_node_get_name(node);
  char *name =
      name_node ? location_get(node_get_location(name_node), allocator) : NULL;
  /* 匿名 enum 合法：错误消息与引擎构造用关键字文本回退类型名，
     define 只在具名（name 非空）时发生。 */
  const char *type_name = name;
  if (!type_name) {
    type_name = "enum";
  }
  node_t *underlying_node = enum_decl_node_get_underlying(node);
  if (!underlying_node) {
    clux_value_t *fail = clux_context_create_error_message(
        ctx, loc, "enum '%s' requires an underlying type", type_name);
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  clux_value_t *underlying_err = NULL;
  clux_type_t *underlying =
      exec_operand_type(ctx, underlying_node, &underlying_err);
  if (!underlying) {
    allocator_free(allocator, (void **)&name);
    return underlying_err;
  }
  clux_type_kind_t underlying_kind = clux_type_kind_unwrap(underlying);
  bool str_underlying = underlying_kind == CLUX_TYPE_STR;
  if (!str_underlying && !enum_integer_kind(underlying_kind)) {
    clux_value_t *fail = clux_context_create_error_message(
        ctx,
        node_get_location(underlying_node),
        "enum '%s' underlying type must be an integer or str type",
        type_name);
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  node_t *body = enum_decl_node_get_body(node);
  size_t count = body ? enum_variants_node_get_variant_count(body) : 0;
  if (count == 0) {
    clux_value_t *fail = clux_context_create_error_message(
        ctx, loc, "enum '%s' requires at least one variant", type_name);
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  clux_enum_variant_t variants[count];
  size_t filled = 0;
  clux_value_t *fail = NULL;
  for (size_t idx = 0; idx < count && !fail; idx++) {
    node_t *variant = enum_variants_node_get_variant(body, idx);
    node_t *variant_name_node =
        variant ? enum_variant_node_get_name(variant) : NULL;
    char *variant_name =
        variant_name_node
            ? location_get(node_get_location(variant_name_node), allocator)
            : NULL;
    if (!variant_name) {
      fail = clux_context_create_error_message(
          ctx,
          variant ? node_get_location(variant) : loc,
          "enum '%s' variant %zu requires a name",
          type_name,
          idx);
      break;
    }
    node_t *value_node = variant ? enum_variant_node_get_value(variant) : NULL;
    if (!value_node) {
      fail = clux_context_create_error_message(
          ctx,
          variant ? node_get_location(variant) : loc,
          "enum '%s' variant '%s' requires a value",
          type_name,
          variant_name);
      allocator_free(allocator, (void **)&variant_name);
      break;
    }
    clux_value_t *value = exec_operand(ctx, value_node);
    if (clux_value_is_error(value)) {
      fail = value;
      allocator_free(allocator, (void **)&variant_name);
      break;
    }
    /* 编译期常量：shadow 无位型可取。底层为 str 时值表达式须是 str
       值（取 C 字符串借用，经 context 创建路径 strs 驻留）；底层为
       整数时须是整数常量（按值自身宽度取原始位型）。 */
    clux_type_t *value_type = clux_value_get_type(value);
    clux_type_kind_t value_kind = clux_type_kind_unwrap(value_type);
    if (clux_value_is_shadow(value) ||
        (str_underlying ? value_kind != CLUX_TYPE_STR
                        : !enum_integer_kind(value_kind))) {
      fail = clux_context_create_error_message(
          ctx,
          node_get_location(value_node),
          str_underlying
              ? "enum '%s' variant '%s' requires a string constant value"
              : "enum '%s' variant '%s' requires an integer constant value",
          type_name,
          variant_name);
      allocator_free(allocator, (void **)&variant_name);
      break;
    }
    uint64_t bits = 0;
    const char *str_bits = NULL;
    if (str_underlying) {
      str_bits = *(const char **)clux_value_get_raw(value, 0);
    } else {
      size_t width = clux_type_get_size(value_type);
      (void)memcpy(&bits, clux_value_get_raw(value, 0), width);
    }
    for (size_t prev = 0; prev < filled; prev++) {
      if (strcmp(variant_name, variants[prev].name) == 0) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(variant_name_node),
            "enum '%s' has duplicate variant '%s'",
            type_name,
            variant_name);
        break;
      }
    }
    if (fail) {
      allocator_free(allocator, (void **)&variant_name);
      break;
    }
    variants[filled].name = variant_name;
    variants[filled].value = bits;
    variants[filled].str = str_bits;
    filled++;
  }
  clux_type_t *type = NULL;
  if (!fail) {
    type = clux_context_create_enum_type(
        ctx, type_name, underlying, variants, filled);
  }
  /* 成员名经 context 路径 strs 驻留，本地拷贝统一释放。 */
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&variants[idx].name);
  }
  if (fail) {
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  if (!type) {
    clux_value_t *create_fail = clux_context_create_error_message(
        ctx, loc, "enum type creation failed");
    allocator_free(allocator, (void **)&name);
    return create_fail;
  }
  clux_value_t *value = clux_context_create_type_value(
      ctx, type, (declare_named && name) ? name : NULL, loc);
  allocator_free(allocator, (void **)&name);
  return value;
}

clux_value_t *exec_expr_enum_decl(clux_context_t *ctx, node_t *node) {
  return exec_expr_enum_decl_impl(ctx, node, true);
}

/* 匿名 enum 值求值：仅求值出不具名的 type value（不按名 define），
   enum 语句装饰器包装路径用——包装完成后由 declare_value 具名化。 */
clux_value_t *exec_expr_enum_decl_value(clux_context_t *ctx, node_t *node) {
  return exec_expr_enum_decl_impl(ctx, node, false);
}

/* bitfield 定义（白皮书 §10）：bitfield Name: u8|u16|u32|u64 { f: width; ... }，
   底层类型必须是无符号整数（求值 UNDERLYING 为 type value 后交引擎
   构造——引擎校验合法范围 1..底层位宽），字段按 LSB 起声明序连续排布，
   宽度是编译期整数常量（按值自身宽度取原始位型）。结构去重恒等返回；
   产出 type value，具名（NAME 槽非空）时按名 define 进当前作用域
   （重名报 duplicate），匿名（NAME 缺省）合法——引擎类型名用关键字
   文本回退，不 define。字段缺名/缺宽度/宽度超界/重名在引擎校验前
   拦截以携带源位置与字段名（引擎校验保持兜底）。declare_named=false
   时仅求值出不具名的
   type value（装饰器包装路径：具名化推迟到包装完成后的 declare_value）。
   一阶段构造（字段只是宽度常量，无类型引用），不需要 union 的两阶段
   递归壳。 */
static clux_value_t *exec_expr_bitfield_decl_impl(clux_context_t *ctx,
                                                  node_t *node,
                                                  bool declare_named) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  node_t *name_node = bitfield_decl_node_get_name(node);
  char *name =
      name_node ? location_get(node_get_location(name_node), allocator) : NULL;
  const char *type_name = name;
  if (!type_name) {
    type_name = "bitfield";
  }
  node_t *underlying_node = bitfield_decl_node_get_underlying(node);
  if (!underlying_node) {
    clux_value_t *fail = clux_context_create_error_message(
        ctx, loc, "bitfield '%s' requires an underlying type", type_name);
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  clux_value_t *underlying_err = NULL;
  clux_type_t *underlying =
      exec_operand_type(ctx, underlying_node, &underlying_err);
  if (!underlying) {
    allocator_free(allocator, (void **)&name);
    return underlying_err;
  }
  clux_type_kind_t underlying_kind = clux_type_kind_unwrap(underlying);
  if (underlying_kind != CLUX_TYPE_U8 && underlying_kind != CLUX_TYPE_U16 &&
      underlying_kind != CLUX_TYPE_U32 && underlying_kind != CLUX_TYPE_U64) {
    const char *underlying_name = clux_type_get_name(underlying);
    clux_value_t *fail = clux_context_create_error_message(
        ctx,
        node_get_location(underlying_node),
        "bitfield '%s' underlying type must be u8, u16, u32 or u64, got '%s'",
        type_name,
        underlying_name ? underlying_name : "<unknown>");
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  node_t *body = bitfield_decl_node_get_body(node);
  size_t count = body ? bitfield_fields_node_get_field_count(body) : 0;
  if (count == 0) {
    clux_value_t *fail = clux_context_create_error_message(
        ctx, loc, "bitfield '%s' requires at least one field", type_name);
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  clux_bitfield_field_def_t fields[count];
  size_t filled = 0;
  clux_value_t *fail = NULL;
  size_t word_bits = clux_type_get_size(underlying) * 8;
  for (size_t idx = 0; idx < count && !fail; idx++) {
    node_t *field = bitfield_fields_node_get_field(body, idx);
    node_t *field_name_node =
        field ? bitfield_field_node_get_name(field) : NULL;
    char *field_name =
        field_name_node
            ? location_get(node_get_location(field_name_node), allocator)
            : NULL;
    if (!field_name) {
      fail = clux_context_create_error_message(
          ctx,
          field ? node_get_location(field) : loc,
          "bitfield '%s' field %zu requires a name",
          type_name,
          idx);
      break;
    }
    node_t *width_node = field ? bitfield_field_node_get_width(field) : NULL;
    if (!width_node) {
      fail = clux_context_create_error_message(
          ctx,
          field ? node_get_location(field) : loc,
          "bitfield '%s' field '%s' requires a width",
          type_name,
          field_name);
      allocator_free(allocator, (void **)&field_name);
      break;
    }
    clux_value_t *width_value = exec_operand(ctx, width_node);
    if (clux_value_is_error(width_value)) {
      fail = width_value;
      allocator_free(allocator, (void **)&field_name);
      break;
    }
    /* 编译期常量：shadow 无位型可取；宽度须整数常量（按值自身宽度
       取原始位型），合法范围 1..底层位宽与重名在本层拦截以携带源
       位置与字段名。 */
    clux_type_t *width_type = clux_value_get_type(width_value);
    if (clux_value_is_shadow(width_value) ||
        !enum_integer_kind(clux_type_kind_unwrap(width_type))) {
      fail = clux_context_create_error_message(
          ctx,
          node_get_location(width_node),
          "bitfield '%s' field '%s' requires an integer constant width",
          type_name,
          field_name);
      allocator_free(allocator, (void **)&field_name);
      break;
    }
    uint64_t bits = 0;
    size_t width_size = clux_type_get_size(width_type);
    (void)memcpy(&bits, clux_value_get_raw(width_value, 0), width_size);
    if (bits == 0 || bits > (uint64_t)word_bits) {
      fail = clux_context_create_error_message(
          ctx,
          node_get_location(width_node),
          "bitfield '%s' field '%s' requires a width in 1..%zu, got %zu",
          type_name,
          field_name,
          word_bits,
          (size_t)bits);
      allocator_free(allocator, (void **)&field_name);
      break;
    }
    for (size_t prev = 0; prev < filled; prev++) {
      if (strcmp(fields[prev].name, field_name) == 0) {
        fail = clux_context_create_error_message(
            ctx,
            field ? node_get_location(field) : loc,
            "bitfield '%s' has duplicate field '%s'",
            type_name,
            field_name);
        break;
      }
    }
    if (fail) {
      allocator_free(allocator, (void **)&field_name);
      break;
    }
    fields[filled].name = field_name;
    fields[filled].width = (size_t)bits;
    filled++;
  }
  clux_type_t *type = NULL;
  if (!fail) {
    type = clux_context_create_bitfield_type(
        ctx, type_name, underlying, fields, filled);
  }
  /* 字段名经 context 路径 strs 驻留，本地拷贝统一释放。 */
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&fields[idx].name);
  }
  if (fail) {
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  if (!type) {
    clux_value_t *create_fail = clux_context_create_error_message(
        ctx, loc, "bitfield type creation failed");
    allocator_free(allocator, (void **)&name);
    return create_fail;
  }
  clux_value_t *value = clux_context_create_type_value(
      ctx, type, (declare_named && name) ? name : NULL, loc);
  allocator_free(allocator, (void **)&name);
  return value;
}

clux_value_t *exec_expr_bitfield_decl(clux_context_t *ctx, node_t *node) {
  return exec_expr_bitfield_decl_impl(ctx, node, true);
}

/* 匿名 bitfield 值求值：仅求值出不具名的 type value（不按名 define），
   bitfield 语句装饰器包装路径用——包装完成后由 declare_value 具名化。 */
clux_value_t *exec_expr_bitfield_decl_value(clux_context_t *ctx, node_t *node) {
  return exec_expr_bitfield_decl_impl(ctx, node, false);
}

/* interface 定义（白皮书 §3.10、§6.9）：interface Name { func sig; ... }，
   纯编译期结构约束——成员逐个求值为方法签名（名 + func 类型，经
   clux_context_create_func_type 构造，与显式 func 类型表达式结构一致即
   恒等），不产生运行期对象/虚表。成员必须是签名：缺名/带函数体/带
   捕获/带泛型/缺返回类型在构造前拦截以携带源位置；约束满足是结构的，
   implements 断言属于 sema 阶段（exec 不检查）。结构去重恒等返回；
   产出 type value（外层 TYPE kind 已 seal，创建合法），具名（NAME 槽
   非空）时按名 define 进当前作用域（重名报 duplicate），匿名（NAME
   缺省）合法——引擎类型名用关键字文本回退，不 define。interface 类型
   本身永不 seal：作值类型（var x: I 等）由 create_value 的 seal 门禁
   拒绝（"cannot create a value of incomplete type"）。泛型 interface
   （[W]）暂不支持，等泛型批次接入。declare_named=false 时仅求值出
   不具名的 type value（装饰器包装路径：具名化推迟到包装完成后的
   declare_value）。 */
static clux_value_t *exec_expr_interface_decl_impl(clux_context_t *ctx,
                                                   node_t *node,
                                                   bool declare_named) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  node_t *name_node = interface_decl_node_get_name(node);
  char *name =
      name_node ? location_get(node_get_location(name_node), allocator) : NULL;
  /* 匿名 interface 合法：错误消息与引擎构造用关键字文本回退类型名，
     define 只在具名（name 非空）时发生。 */
  const char *type_name = name;
  if (!type_name) {
    type_name = "interface";
  }
  if (interface_decl_node_get_generics(node)) {
    clux_value_t *fail = clux_context_create_error_message(
        ctx, loc, "generic interface is not supported yet");
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  node_t *body = interface_decl_node_get_body(node);
  size_t count = body ? interface_members_node_get_member_count(body) : 0;
  if (count == 0) {
    clux_value_t *fail = clux_context_create_error_message(
        ctx, loc, "interface '%s' requires at least one member", type_name);
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  clux_interface_member_t members[count];
  size_t filled = 0;
  clux_value_t *fail = NULL;
  for (size_t idx = 0; idx < count && !fail; idx++) {
    node_t *member = interface_members_node_get_member(body, idx);
    if (!member || node_get_kind(member) != CLUX_NODE_EXPR_FUNC_DECL) {
      fail = clux_context_create_error_message(
          ctx,
          member ? node_get_location(member) : loc,
          "interface '%s' member must be a function signature",
          type_name);
      break;
    }
    node_t *member_name_node = func_decl_node_get_name(member);
    char *member_name =
        member_name_node
            ? location_get(node_get_location(member_name_node), allocator)
            : NULL;
    node_t *member_fail_loc = member_name_node ? member_name_node : member;
    if (!member_name) {
      fail = clux_context_create_error_message(
          ctx,
          node_get_location(member_fail_loc),
          "interface '%s' member %zu requires a name",
          type_name,
          idx);
      break;
    }
    if (func_decl_node_get_body(member)) {
      fail = clux_context_create_error_message(
          ctx,
          node_get_location(member),
          "interface '%s' member '%s' must be a signature, not a function "
          "body",
          type_name,
          member_name);
      allocator_free(allocator, (void **)&member_name);
      break;
    }
    if (func_decl_node_get_captures(member)) {
      fail = clux_context_create_error_message(
          ctx,
          node_get_location(member),
          "interface '%s' member '%s' cannot have captures",
          type_name,
          member_name);
      allocator_free(allocator, (void **)&member_name);
      break;
    }
    if (func_decl_node_get_generics(member)) {
      fail = clux_context_create_error_message(
          ctx,
          node_get_location(member),
          "interface '%s' member '%s' cannot be generic",
          type_name,
          member_name);
      allocator_free(allocator, (void **)&member_name);
      break;
    }
    node_t *return_node = func_decl_node_get_return(member);
    if (!return_node) {
      fail = clux_context_create_error_message(
          ctx,
          node_get_location(member),
          "interface '%s' member '%s' requires a return type",
          type_name,
          member_name);
      allocator_free(allocator, (void **)&member_name);
      break;
    }
    /* 参数序列求值：FUNC_PARAM 取 TYPE 槽，rest（变参/参数包）暂不
       支持。FUNC_PARAM 只取类型——签名不携带形参名。 */
    node_t *params_node = func_decl_node_get_params(member);
    size_t param_count =
        params_node ? func_params_node_get_param_count(params_node) : 0;
    clux_type_t *params[param_count];
    for (size_t pidx = 0; pidx < param_count && !fail; pidx++) {
      node_t *param = func_params_node_get_param(params_node, pidx);
      if (!param) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(member),
            "interface '%s' member '%s' parameter %zu requires a type",
            type_name,
            member_name,
            pidx);
        break;
      }
      if (node_get_kind(param) == CLUX_NODE_EXPR_REST) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(param),
            "variadic interface member is not supported yet");
        break;
      }
      node_t *param_type_node = node_get_kind(param) == CLUX_NODE_FUNC_PARAM
                                    ? func_param_node_get_type(param)
                                    : param;
      if (!param_type_node) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(member),
            "interface '%s' member '%s' parameter %zu requires a type",
            type_name,
            member_name,
            pidx);
        break;
      }
      clux_value_t *err = NULL;
      params[pidx] = exec_operand_type(ctx, param_type_node, &err);
      if (!params[pidx]) {
        fail = err;
        break;
      }
    }
    clux_type_t *return_type = NULL;
    if (!fail) {
      clux_value_t *err = NULL;
      return_type = exec_operand_type(ctx, return_node, &err);
      if (!return_type) {
        fail = err;
      }
    }
    if (!fail) {
      /* 重名在引擎校验前拦截以携带源位置。 */
      for (size_t prev = 0; prev < filled; prev++) {
        if (strcmp(member_name, members[prev].name) == 0) {
          fail = clux_context_create_error_message(
              ctx,
              node_get_location(member_name_node),
              "interface '%s' has duplicate member '%s'",
              type_name,
              member_name);
          break;
        }
      }
    }
    if (!fail) {
      clux_type_t *signature = clux_context_create_func_type(
          ctx, member_name, NULL, 0, params, param_count, return_type);
      if (!signature) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(member),
            "interface '%s' member '%s' signature creation failed",
            type_name,
            member_name);
      } else {
        members[filled].name = member_name;
        members[filled].func = signature;
        filled++;
      }
    }
    if (fail) {
      allocator_free(allocator, (void **)&member_name);
    }
  }
  clux_type_t *type = NULL;
  if (!fail) {
    type = clux_context_create_interface_type(ctx, type_name, members, filled);
  }
  /* 成员名经 context 路径 strs 驻留，本地拷贝统一释放。 */
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&members[idx].name);
  }
  if (fail) {
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  if (!type) {
    clux_value_t *create_fail = clux_context_create_error_message(
        ctx, loc, "interface type creation failed");
    allocator_free(allocator, (void **)&name);
    return create_fail;
  }
  clux_value_t *value = clux_context_create_type_value(
      ctx, type, (declare_named && name) ? name : NULL, loc);
  allocator_free(allocator, (void **)&name);
  return value;
}

clux_value_t *exec_expr_interface_decl(clux_context_t *ctx, node_t *node) {
  return exec_expr_interface_decl_impl(ctx, node, true);
}

/* 匿名 interface 值求值：仅求值出不具名的 type value（不按名 define），
   interface 语句装饰器包装路径用——包装完成后由 declare_value 具名化。 */
clux_value_t *exec_expr_interface_decl_value(clux_context_t *ctx,
                                             node_t *node) {
  return exec_expr_interface_decl_impl(ctx, node, false);
}

/* struct 定义（白皮书 §3.5）两阶段构造：func 成员形参类型急切求值
   （self: *User 引用 struct 自身）与"壳按名 define"互为先后——先收集
   字段表并建壳（布局即时 seal），具名时先 define 出壳的 type value
   （方法签名此刻可解析自身），再求值 var（attribute）与 func（method）
   成员，最后 install 三表收尾。implements 是 sema 断言（exec 忽略）；
   泛型定义产 type 模板值（实例化通道按下标实参重执行）；匿名 struct
   合法（define 只在具名时发生——装饰器包装走匿名路径，方法签名引用
   自身会报 use of undeclared identifier）。is_instance 为真时（泛型
   实例重执行）跳过门禁且不 define：保留原名作 shell 名（类型名不参与
   判等），实例值归实例化出口 clone。 */
static clux_value_t *exec_expr_struct_decl_impl(clux_context_t *ctx,
                                                node_t *node,
                                                bool declare_named,
                                                bool is_instance) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  node_t *name_node = struct_decl_node_get_name(node);
  char *name =
      name_node ? location_get(node_get_location(name_node), allocator) : NULL;
  /* 匿名 struct 合法：错误消息与引擎构造用关键字文本回退类型名。 */
  const char *type_name = name ? name : "struct";
  if (struct_decl_node_get_generics(node) && !is_instance) {
    /* 泛型定义产 type 模板值（白皮书 §6.1）：data 存声明 AST 本身。 */
    clux_value_t *template_value = clux_context_create_type_template(
        ctx, node, declare_named ? name : NULL, loc);
    allocator_free(allocator, (void **)&name);
    return template_value;
  }
  node_t *body = struct_decl_node_get_body(node);
  size_t count = body ? struct_members_node_get_member_count(body) : 0;
  /* pass A：embed 类型先行求值（依赖外部类型，不依赖壳——自引用 embed
     此刻名字未就位，undeclared 自然拦截），同时统计字段总数（embed 按
     声明位置就地展开，白皮书 §3.5 嵌入语义）。 */
  clux_type_t *embed_types[count ? count : 1];
  size_t field_total = 0;
  clux_value_t *fail = NULL;
  for (size_t idx = 0; idx < count && !fail; idx++) {
    node_t *member = struct_members_node_get_member(body, idx);
    embed_types[idx] = NULL;
    if (!member || node_get_kind(member) != CLUX_NODE_STRUCT_EMBED) {
      if (member && node_get_kind(member) == CLUX_NODE_FIELD) {
        field_total++;
      }
      continue;
    }
    node_t *embed_type_node = struct_embed_node_get_type(member);
    if (!embed_type_node) {
      fail = clux_context_create_error_message(
          ctx, node_get_location(member), "struct embed requires a type");
      break;
    }
    clux_value_t *err = NULL;
    clux_type_t *embed_type = exec_operand_type(ctx, embed_type_node, &err);
    if (!embed_type) {
      fail = err;
      break;
    }
    if (clux_type_get_kind(embed_type) != CLUX_TYPE_STRUCT) {
      fail = clux_context_create_error_message(
          ctx,
          node_get_location(embed_type_node),
          "struct embed requires a struct type");
      break;
    }
    embed_types[idx] = embed_type;
    field_total += clux_struct_field_count(embed_type);
  }
  /* pass B：填充字段表（FIELD 名字+类型；EMBED 按声明序展开），建壳并
     具名 define（方法签名可解析 struct 名）。EMBED 展开的字段名借用自
     embed 类型（strs 驻留），owned_names 逐槽标记仅供本地释放循环跳过
     借用指针，避免把 embed 类型的驻留名当本地拷贝释放。 */
  clux_struct_field_t fields[field_total ? field_total : 1];
  const char /* nullable */ *owned_names[field_total ? field_total : 1];
  size_t filled = 0;
  for (size_t idx = 0; idx < count && !fail; idx++) {
    node_t *member = struct_members_node_get_member(body, idx);
    if (member && node_get_kind(member) == CLUX_NODE_STRUCT_EMBED) {
      clux_type_t *embed_type = embed_types[idx];
      size_t embed_count = clux_struct_field_count(embed_type);
      for (size_t fidx = 0; fidx < embed_count; fidx++) {
        fields[filled] = *clux_struct_get_field(embed_type, fidx);
        owned_names[filled] = NULL;
        filled++;
      }
      continue;
    }
    if (!member || node_get_kind(member) != CLUX_NODE_FIELD) {
      continue;
    }
    node_t *field_name_node = field_node_get_name(member);
    if (!field_name_node) {
      fail = clux_context_create_error_message(
          ctx,
          node_get_location(member),
          "struct '%s' field %zu requires a name",
          type_name,
          idx);
      break;
    }
    char *field_name =
        location_get(node_get_location(field_name_node), allocator);
    fields[filled].name = field_name;
    owned_names[filled] = field_name;
    node_t *field_type_node = field_node_get_type(member);
    if (!field_type_node) {
      fail = clux_context_create_error_message(
          ctx,
          node_get_location(member),
          "struct '%s' field '%s' requires a type",
          type_name,
          field_name);
      allocator_free(allocator, (void **)&field_name);
      break;
    }
    clux_value_t *err = NULL;
    fields[filled].type = exec_operand_type(ctx, field_type_node, &err);
    if (!fields[filled].type) {
      fail = err;
      allocator_free(allocator, (void **)&field_name);
      break;
    }
    filled++;
  }
  clux_type_t *type = NULL;
  if (!fail) {
    type = clux_context_begin_struct_type(ctx, type_name, fields, filled);
    if (!type) {
      fail = clux_context_create_error_message(
          ctx, loc, "struct '%s' field collection failed", type_name);
    }
  }
  /* 字段名经 context 路径 strs 驻留（或校验失败），本地拷贝统一释放
     （embed 展开的借用名由 owned_names 标记跳过）。 */
  for (size_t idx = 0; idx < filled; idx++) {
    if (owned_names[idx]) {
      allocator_free(allocator, (void **)&owned_names[idx]);
    }
  }
  clux_value_t *bound = NULL;
  if (!fail && declare_named && name) {
    /* 具名 define 壳：type value 通道自动 scope_define——func 成员形参
       类型求值时 struct 名可解析。 */
    bound = clux_context_create_type_value(ctx, type, name, loc);
    if (!bound || clux_value_is_error(bound)) {
      fail = bound;
      bound = NULL;
    }
  }
  /* pass C：attribute（var 成员求初值）与 method（func 成员匿名求值）
     按声明序收集——重名在引擎校验前拦截以携带源位置。 */
  clux_struct_attr_t attrs[count ? count : 1];
  const char *method_names[count ? count : 1];
  size_t attr_filled = 0;
  size_t method_filled = 0;
  for (size_t idx = 0; idx < count && !fail; idx++) {
    node_t *member = struct_members_node_get_member(body, idx);
    if (!member) {
      continue;
    }
    if (node_get_kind(member) == CLUX_NODE_STMT_VAR) {
      if (stmt_var_node_get_decorator_count(member) != 0) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(member),
            "decorated struct attribute is not supported yet");
        break;
      }
      node_t *attr_name_node = stmt_var_node_get_name(member);
      if (!attr_name_node) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(member),
            "struct '%s' attribute %zu requires a name",
            type_name,
            idx);
        break;
      }
      node_t *init = stmt_var_node_get_value(member);
      if (!init) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(member),
            "struct attribute requires an initializer");
        break;
      }
      char *attr_name =
          location_get(node_get_location(attr_name_node), allocator);
      for (size_t prev = 0; prev < attr_filled && !fail; prev++) {
        if (strcmp(attr_name, attrs[prev].name) == 0) {
          fail = clux_context_create_error_message(
              ctx,
              node_get_location(attr_name_node),
              "struct '%s' has duplicate attribute '%s'",
              type_name,
              attr_name);
        }
      }
      if (fail) {
        allocator_free(allocator, (void **)&attr_name);
        break;
      }
      clux_value_t *value = exec_operand(ctx, init);
      if (!value || clux_value_is_error(value)) {
        fail = value;
        allocator_free(allocator, (void **)&attr_name);
        break;
      }
      attrs[attr_filled].name = attr_name;
      attrs[attr_filled].value = value;
      attr_filled++;
      continue;
    }
    if (node_get_kind(member) == CLUX_NODE_STMT_FUNC) {
      if (stmt_func_node_get_decorator_count(member) != 0) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(member),
            "decorated struct method is not supported yet");
        break;
      }
      node_t *func_decl = stmt_func_node_get_func_decl(member);
      if (!func_decl) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(member),
            "struct '%s' method %zu requires a function declaration",
            type_name,
            idx);
        break;
      }
      node_t *method_name_node = func_decl_node_get_name(func_decl);
      if (!method_name_node) {
        fail = clux_context_create_error_message(
            ctx,
            node_get_location(func_decl),
            "struct '%s' method %zu requires a name",
            type_name,
            idx);
        break;
      }
      char *method_name =
          location_get(node_get_location(method_name_node), allocator);
      for (size_t prev = 0; prev < attr_filled && !fail; prev++) {
        if (strcmp(method_name, attrs[prev].name) == 0) {
          fail = clux_context_create_error_message(
              ctx,
              node_get_location(method_name_node),
              "struct '%s' has duplicate attribute '%s'",
              type_name,
              method_name);
        }
      }
      if (fail) {
        allocator_free(allocator, (void **)&method_name);
        break;
      }
      clux_value_t *func =
          exec_func_decl_value(ctx, func_decl); /* 匿名求值，不 define */
      if (!func || clux_value_is_error(func)) {
        fail = func;
        allocator_free(allocator, (void **)&method_name);
        break;
      }
      attrs[attr_filled].name = method_name;
      attrs[attr_filled].value = func;
      method_names[method_filled] = method_name;
      attr_filled++;
      method_filled++;
      continue;
    }
    /* FIELD/STRUCT_EMBED 已在字段表处理（装饰器是 metadata 标记，不参与
       值），其余成员形态（type 等）暂无 parser 来源。 */
  }
  if (!fail) {
    if (!clux_context_install_struct_tables(ctx,
                                            type,
                                            attrs,
                                            attr_filled,
                                            method_names,
                                            method_filled,
                                            NULL,
                                            0)) {
      fail = clux_context_create_error_message(
          ctx, loc, "struct '%s' table installation failed", type_name);
    }
  }
  /* 成员名经 context 路径 strs 驻留（或校验失败），本地拷贝统一释放。 */
  for (size_t idx = 0; idx < attr_filled; idx++) {
    allocator_free(allocator, (void **)&attrs[idx].name);
  }
  if (fail) {
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  if (bound) {
    allocator_free(allocator, (void **)&name);
    return bound;
  }
  /* 匿名/装饰路径：求值出不具名的 type value（不按名 define）。 */
  clux_value_t *value = clux_context_create_type_value(ctx, type, NULL, loc);
  allocator_free(allocator, (void **)&name);
  return value;
}

clux_value_t *exec_expr_struct_decl(clux_context_t *ctx, node_t *node) {
  return exec_expr_struct_decl_impl(ctx, node, true, false);
}

/* 匿名 struct 值求值：仅求值出不具名的 type value（不按名 define），
   struct 语句装饰器包装路径用——包装完成后由 declare_value 具名化。
   注意：此路径方法签名引用 struct 自身不可解析（壳名未 define），
   值级装饰路径待泛型批次一并处理。 */
clux_value_t *exec_expr_struct_decl_value(clux_context_t *ctx, node_t *node) {
  return exec_expr_struct_decl_impl(ctx, node, false, false);
}

/* 泛型 struct 实例：在绑定环境（实例化 scope）内重执行声明 AST——
   字段/方法签名此刻可引用泛型参数名；不 define（方法签名引用原名
   不可解析，§6.8 自引用留待调用点推导批次）。 */
clux_value_t *exec_expr_struct_decl_instance(clux_context_t *ctx,
                                             node_t *node) {
  return exec_expr_struct_decl_impl(ctx, node, false, true);
}

/* 元组类型定义 <T, U, ...>（白皮书 §2.6）：元素逐个求值为 type value
   后经引擎结构去重构造，空元组 <> 合法；<...Ts> 参数包引用（§6.4）
   展开为绑定包的各元素——先求值各包定展开总长，再填表。 */
clux_value_t *exec_expr_tuple(clux_context_t *ctx, node_t *node) {
  size_t count = tuple_node_get_elem_count(node);
  clux_type_t *pack_types[count ? count : 1];
  size_t expanded = 0;
  clux_value_t *fail = NULL;
  for (size_t idx = 0; idx < count && !fail; idx++) {
    node_t *elem = tuple_node_get_elem(node, idx);
    if (!elem) {
      fail = clux_context_create_error_message(
          ctx, node_get_location(node), "tuple element %zu is missing", idx);
      break;
    }
    if (node_get_kind(elem) == CLUX_NODE_EXPR_REST) {
      clux_value_t *err = NULL;
      pack_types[idx] = exec_pack_tuple_type(ctx, elem, &err);
      if (!pack_types[idx]) {
        fail = err;
        break;
      }
      expanded += clux_tuple_elem_count(pack_types[idx]);
    } else {
      pack_types[idx] = NULL;
      expanded++;
    }
  }
  clux_type_t *elems[expanded ? expanded : 1];
  size_t slot = 0;
  for (size_t idx = 0; idx < count && !fail; idx++) {
    if (pack_types[idx]) {
      size_t pack_count = clux_tuple_elem_count(pack_types[idx]);
      for (size_t eidx = 0; eidx < pack_count; eidx++) {
        elems[slot++] = clux_tuple_get_elem(pack_types[idx], eidx);
      }
      continue;
    }
    clux_value_t *err = NULL;
    elems[slot] = exec_operand_type(ctx, tuple_node_get_elem(node, idx), &err);
    if (!elems[slot]) {
      fail = err;
      break;
    }
    slot++;
  }
  if (fail) {
    return fail;
  }
  clux_type_t *type =
      clux_context_get_tuple_type(ctx, slot ? elems : NULL, slot);
  if (!type) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "tuple type creation failed");
  }
  return clux_context_create_type_value(
      ctx, type, NULL, node_get_location(node));
}

/* —— 构造表达式（白皮书 §5.10）——
   构造必穷尽：类型系统没有默认零值，省略字段是错误。具名构造按类型分流
   （struct 具名字段穷尽 / union+cunion 恰好一个成员 / 数组+元组位置元素）；
   匿名构造产出匿名类型（具名字段 → 匿名 struct，位置元素 → 匿名 tuple），
   在赋值/实参/返回位置经鸭子类型（safe_cast）隐式转换到接收方。 */

/* view 写入：safe_cast 吸收协变（继承/指针擦除）后经通用 assign 写穿。
   错误自传播（error 是硬错误值）。 */
static clux_value_t *construct_write_view(clux_context_t *ctx,
                                          clux_value_t *view,
                                          clux_value_t *value,
                                          const location_t *loc) {
  clux_value_t *cast =
      clux_context_safe_cast(ctx, value, clux_value_get_type(view), loc);
  if (clux_value_is_error(cast)) {
    return cast;
  }
  return clux_context_assign(ctx, view, cast, loc);
}

/* 按名查 struct 字段下标（线性扫描），未找到返回 (size_t)-1。 */
static size_t struct_field_index(clux_type_t *type, const char *name) {
  size_t count = clux_struct_field_count(type);
  for (size_t idx = 0; idx < count; idx++) {
    if (strcmp(clux_struct_get_field(type, idx)->name, name) == 0) {
      return idx;
    }
  }
  return (size_t)-1;
}

/* 构造字段覆盖状态：0=未写，1=spread 展开覆盖，2=显式字段写入。 */
#define CONSTRUCT_FIELD_UNSET 0
#define CONSTRUCT_FIELD_SPREAD 1
#define CONSTRUCT_FIELD_EXPLICIT 2
/* spread 展开 ...value → 按名合并进目标 struct（后写覆盖先展开）：
   展开值须为 struct，字段名须存在于目标类型。返回 NULL 表示展开
   成功，非 NULL 为待上抛的 error 值。 */
static clux_value_t *construct_spread_struct(clux_context_t *ctx,
                                             node_t *elem,
                                             clux_value_t *target,
                                             unsigned char *covered,
                                             clux_type_t *type,
                                             const location_t *loc) {
  node_t *operand = rest_node_get_expr(elem);
  if (!operand) {
    return clux_context_create_error_message(
        ctx, node_get_location(elem), "spread element requires a value");
  }
  clux_value_t *spread = exec_operand(ctx, operand);
  if (clux_value_is_error(spread)) {
    return spread;
  }
  clux_type_t *src_type = clux_value_get_type(spread);
  if (clux_type_get_kind(src_type) != CLUX_TYPE_STRUCT) {
    const char *src_name = clux_type_get_name(src_type);
    return clux_context_create_error_message(
        ctx,
        node_get_location(elem),
        "spread of non-struct type '%s' is not supported in construction",
        src_name ? src_name : "<unknown>");
  }
  size_t src_count = clux_struct_field_count(src_type);
  for (size_t fidx = 0; fidx < src_count; fidx++) {
    const char *name = clux_struct_get_field(src_type, fidx)->name;
    size_t midx = struct_field_index(type, name);
    if (midx == (size_t)-1) {
      const char *type_name = clux_type_get_name(type);
      return clux_context_create_error_message(ctx,
                                               node_get_location(elem),
                                               "type '%s' has no field '%s'",
                                               type_name ? type_name
                                                         : "<unknown>",
                                               name);
    }
    clux_value_t *src_view = clux_context_field(ctx, spread, name, loc);
    if (clux_value_is_error(src_view)) {
      return src_view;
    }
    clux_value_t *dst_view = clux_context_field(ctx, target, name, loc);
    if (clux_value_is_error(dst_view)) {
      return dst_view;
    }
    clux_value_t *written = construct_write_view(ctx, dst_view, src_view, loc);
    if (clux_value_is_error(written)) {
      return written;
    }
    covered[midx] = CONSTRUCT_FIELD_SPREAD;
  }
  return NULL;
}

/* 具名 struct 构造：字段顺序任意、构造必穷尽（缺字段报语义化 error）；
   spread 合并按名展开（后写覆盖先展开），spread 之后显式同名字段覆盖
   重写，两个显式同名字段才是 duplicate。 */
static clux_value_t *
construct_struct(clux_context_t *ctx, node_t *node, clux_type_t *type) {
  const location_t *loc = node_get_location(node);
  size_t field_count = clux_struct_field_count(type);
  unsigned char covered[field_count ? field_count : 1];
  memset(covered, 0, sizeof(covered));
  clux_value_t *target = clux_context_create_struct(ctx, type, NULL, loc);
  if (clux_value_is_error(target)) {
    return target;
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const char *type_name = clux_type_get_name(type);
  type_name = type_name ? type_name : "<anonymous>";
  size_t elem_count = construct_node_get_elem_count(node);
  for (size_t idx = 0; idx < elem_count; idx++) {
    node_t *elem = construct_node_get_elem(node, idx);
    if (!elem) {
      continue;
    }
    if (node_get_kind(elem) == CLUX_NODE_EXPR_REST) {
      clux_value_t *fail =
          construct_spread_struct(ctx, elem, target, covered, type, loc);
      if (fail) {
        return fail;
      }
      continue;
    }

    if (node_get_kind(elem) != CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(elem),
          "named and positional elements cannot be mixed in construction");
    }
    node_t *name_node = construct_field_node_get_name(elem);
    node_t *value_node = construct_field_node_get_value(elem);
    if (!name_node || !value_node) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(elem),
          "construction field requires a name and a value");
    }
    char *name = location_get(node_get_location(name_node), allocator);
    size_t midx = struct_field_index(type, name);
    if (midx == (size_t)-1) {
      clux_value_t *fail =
          clux_context_create_error_message(ctx,
                                            node_get_location(elem),
                                            "type '%s' has no field '%s'",
                                            type_name,
                                            name);
      allocator_free(allocator, (void **)&name);
      return fail;
    }
    if (covered[midx] == CONSTRUCT_FIELD_EXPLICIT) {
      clux_value_t *fail = clux_context_create_error_message(
          ctx,
          node_get_location(elem),
          "duplicate field '%s' in construction of '%s'",
          name,
          type_name);
      allocator_free(allocator, (void **)&name);
      return fail;
    }
    clux_value_t *value = exec_operand(ctx, value_node);
    if (clux_value_is_error(value)) {
      allocator_free(allocator, (void **)&name);
      return value;
    }
    clux_value_t *view = clux_context_field(ctx, target, name, loc);
    if (clux_value_is_error(view)) {
      allocator_free(allocator, (void **)&name);
      return view;
    }
    clux_value_t *written = construct_write_view(ctx, view, value, loc);
    allocator_free(allocator, (void **)&name);
    if (clux_value_is_error(written)) {
      return written;
    }
    covered[midx] = CONSTRUCT_FIELD_EXPLICIT;
  }
  for (size_t fidx = 0; fidx < field_count; fidx++) {
    if (!covered[fidx]) {
      return clux_context_create_error_message(
          ctx,
          loc,
          "missing field '%s' in construction of '%s'",
          clux_struct_get_field(type, fidx)->name,
          type_name);
    }
  }
  return target;
}

/* 成员未找到的语义化 error（union/cunion 共用）。 */
static clux_value_t *construct_member_not_found(clux_context_t *ctx,
                                                clux_type_t *type,
                                                const char *name,
                                                const location_t *loc) {
  const char *type_name = clux_type_get_name(type);
  const char *kind_text =
      clux_type_get_kind(type) == CLUX_TYPE_UNION ? "tag union" : "cunion";
  return clux_context_create_error_message(ctx,
                                           loc,
                                           "%s '%s' has no member '%s'",
                                           kind_text,
                                           type_name ? type_name
                                                     : "<anonymous>",
                                           name);
}

/* 单成员构造（tag union / cunion）：恰好一个具名字段，字段名即 member
   选择。tag union 经 union_set_member 激活成员后写字段 view；cunion 直接
   写共享块。多成员与位置元素报语义化 error。 */
static clux_value_t *
construct_union_like(clux_context_t *ctx, node_t *node, clux_type_t *type) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  const char *kind_text =
      clux_type_get_kind(type) == CLUX_TYPE_UNION ? "tag union" : "cunion";
  size_t elem_count = construct_node_get_elem_count(node);
  node_t *field_elem = NULL;
  for (size_t idx = 0; idx < elem_count; idx++) {
    node_t *elem = construct_node_get_elem(node, idx);
    if (!elem) {
      continue;
    }
    if (node_get_kind(elem) != CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(elem),
          "%s construction takes exactly one named member",
          kind_text);
    }
    if (field_elem) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(elem),
          "%s construction takes exactly one named member",
          kind_text);
    }
    field_elem = elem;
  }
  if (!field_elem) {
    return clux_context_create_error_message(
        ctx, loc, "%s construction takes one named member", kind_text);
  }
  node_t *name_node = construct_field_node_get_name(field_elem);
  node_t *value_node = construct_field_node_get_value(field_elem);
  if (!name_node || !value_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(field_elem),
        "construction field requires a name and a value");
  }
  char *name = location_get(node_get_location(name_node), allocator);
  bool tag = clux_type_get_kind(type) == CLUX_TYPE_UNION;
  clux_type_t *member_type = NULL;
  if (tag) {
    const clux_union_member_t *member = clux_union_find_member(type, name);
    if (!member) {
      clux_value_t *fail = construct_member_not_found(ctx, type, name, loc);
      allocator_free(allocator, (void **)&name);
      return fail;
    }
    member_type = member->type;
  } else {
    const clux_cunion_member_t *member = clux_cunion_find_member(type, name);
    if (!member) {
      clux_value_t *fail = construct_member_not_found(ctx, type, name, loc);
      allocator_free(allocator, (void **)&name);
      return fail;
    }
    member_type = member->type;
  }
  clux_value_t *value = exec_operand(ctx, value_node);
  if (clux_value_is_error(value)) {
    allocator_free(allocator, (void **)&name);
    return value;
  }
  clux_value_t *union_value =
      tag ? clux_context_create_union(ctx, type, NULL, loc)
          : clux_context_create_cunion(ctx, type, NULL, loc);
  if (clux_value_is_error(union_value)) {
    allocator_free(allocator, (void **)&name);
    return union_value;
  }
  if (tag) {
    clux_value_t *set =
        clux_context_union_set_member(ctx, union_value, name, loc);
    if (clux_value_is_error(set)) {
      allocator_free(allocator, (void **)&name);
      return set;
    }
  }
  clux_value_t *view = clux_context_field(ctx, union_value, name, loc);
  if (clux_value_is_error(view)) {
    allocator_free(allocator, (void **)&name);
    return view;
  }
  clux_value_t *cast = clux_context_safe_cast(ctx, value, member_type, loc);
  allocator_free(allocator, (void **)&name);
  if (clux_value_is_error(cast)) {
    return cast;
  }
  clux_value_t *written = clux_context_assign(ctx, view, cast, loc);
  if (clux_value_is_error(written)) {
    return written;
  }
  return union_value;
}

/* 位域构造（白皮书 §10）：.Name{ .f = v, ... }——元素必须全部具名，
   值截断到字段宽度（引擎 RMW 掩码写入），缺失字段补零（零初始化
   存储天然满足），空构造 .Name{} 合法（全零）。字段值须底层无符号
   类型可表达（safe_cast 吸收整型转换）。 */
static clux_value_t *
construct_bitfield(clux_context_t *ctx, node_t *node, clux_type_t *type) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  const char *type_name = clux_type_get_name(type);
  size_t elem_count = construct_node_get_elem_count(node);
  /* 预检：必须全部具名且无重名（写入按字段名分派，重名即歧义）。 */
  for (size_t idx = 0; idx < elem_count; idx++) {
    node_t *elem = construct_node_get_elem(node, idx);
    if (!elem) {
      continue;
    }
    if (node_get_kind(elem) != CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(elem),
          "bitfield construction takes named fields only");
    }
    node_t *elem_name = construct_field_node_get_name(elem);
    char *name = elem_name
                     ? location_get(node_get_location(elem_name), allocator)
                     : NULL;
    clux_value_t *fail = NULL;
    if (!name) {
      fail = clux_context_create_error_message(
          ctx, node_get_location(elem), "construction field requires a name");
    } else {
      for (size_t prev = 0; prev < idx && !fail; prev++) {
        node_t *prev_elem = construct_node_get_elem(node, prev);
        if (!prev_elem ||
            node_get_kind(prev_elem) != CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
          continue;
        }
        node_t *prev_name_node = construct_field_node_get_name(prev_elem);
        char *prev_name =
            prev_name_node
                ? location_get(node_get_location(prev_name_node), allocator)
                : NULL;
        if (prev_name && strcmp(name, prev_name) == 0) {
          fail = clux_context_create_error_message(
              ctx,
              node_get_location(elem),
              "bitfield '%s' has duplicate field '%s' in construction",
              type_name ? type_name : "<unknown>",
              name);
        }
        allocator_free(allocator, (void **)&prev_name);
      }
      allocator_free(allocator, (void **)&name);
    }
    if (fail) {
      return fail;
    }
  }
  clux_value_t *value = clux_context_create_bitfield(ctx, type, NULL, loc);
  if (clux_value_is_error(value)) {
    return value;
  }
  clux_type_t *underlying = clux_bitfield_get_underlying(type);
  for (size_t idx = 0; idx < elem_count; idx++) {
    node_t *elem = construct_node_get_elem(node, idx);
    if (!elem) {
      continue;
    }
    node_t *name_node = construct_field_node_get_name(elem);
    node_t *value_node = construct_field_node_get_value(elem);
    if (!name_node || !value_node) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(elem),
          "construction field requires a name and a value");
    }
    char *name = location_get(node_get_location(name_node), allocator);
    if (!clux_bitfield_find_field(type, name)) {
      clux_value_t *fail = construct_member_not_found(ctx, type, name, loc);
      allocator_free(allocator, (void **)&name);
      return fail;
    }
    clux_value_t *field_value = exec_operand(ctx, value_node);
    if (clux_value_is_error(field_value)) {
      allocator_free(allocator, (void **)&name);
      return field_value;
    }
    clux_value_t *cast =
        clux_context_safe_cast(ctx, field_value, underlying, loc);
    if (clux_value_is_error(cast)) {
      allocator_free(allocator, (void **)&name);
      return cast;
    }
    clux_value_t *written =
        clux_context_bitfield_set_field(ctx, value, name, cast, loc);
    allocator_free(allocator, (void **)&name);
    if (clux_value_is_error(written)) {
      return written;
    }
  }
  return value;
}

/* 位置元素预检（数组/元组构造，白皮书 §5.10）：混用报错，spread 展开
   暂不支持，个数必须与类型长度一致（构造必穷尽——少即缺元素，多即溢出）。
   返回 NULL 表示预检通过，非 NULL 为待上抛 error。 */
static clux_value_t *construct_positional_check(clux_context_t *ctx,
                                                node_t *node,
                                                size_t expected,
                                                const char *what) {
  size_t elem_count = construct_node_get_elem_count(node);
  for (size_t idx = 0; idx < elem_count; idx++) {
    node_t *elem = construct_node_get_elem(node, idx);
    if (!elem) {
      continue;
    }
    if (node_get_kind(elem) == CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(elem),
          "named and positional elements cannot be mixed in construction");
    }
    if (node_get_kind(elem) == CLUX_NODE_EXPR_REST) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(elem),
          "%s construction does not support spread elements yet",
          what);
    }
  }
  if (elem_count != expected) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "%s construction requires %zu elements, got %zu",
        what,
        expected,
        elem_count);
  }
  return NULL;
}

/* 位置元素写入：按序 index 取 view + assign 写穿（i32 下标值逐个创建，
   数量有界——与构造元素个数一致）。返回 NULL 表示写入成功。 */
static clux_value_t *construct_positional_fill(clux_context_t *ctx,
                                               node_t *node,
                                               clux_value_t *aggregate,
                                               size_t count) {
  const location_t *loc = node_get_location(node);
  for (size_t idx = 0; idx < count; idx++) {
    clux_value_t *value = exec_operand(ctx, construct_node_get_elem(node, idx));
    if (clux_value_is_error(value)) {
      return value;
    }
    clux_value_t *index = clux_context_create_i32(ctx, (int32_t)idx, NULL, loc);
    if (clux_value_is_error(index)) {
      return index;
    }
    clux_value_t *view = clux_context_index(ctx, aggregate, index, loc);
    if (clux_value_is_error(view)) {
      return view;
    }
    clux_value_t *written = construct_write_view(ctx, view, value, loc);
    if (clux_value_is_error(written)) {
      return written;
    }
  }
  return NULL;
}

/* 具名数组构造 [N]T{ e0, e1, ... }：元素个数 == N，逐位写入。 */
static clux_value_t *
construct_array(clux_context_t *ctx, node_t *node, clux_type_t *type) {
  size_t len = clux_array_get_length(type);
  clux_value_t *fail = construct_positional_check(ctx, node, len, "array");
  if (fail) {
    return fail;
  }
  const location_t *loc = node_get_location(node);
  clux_value_t *array = clux_context_create_array(ctx, type, NULL, loc);
  if (clux_value_is_error(array)) {
    return array;
  }
  fail = construct_positional_fill(ctx, node, array, len);
  if (fail) {
    return fail;
  }
  return array;
}

/* 具名元组构造 <T, U, ...>{ e0, e1, ... }：元素个数 == 元素类型数。 */
static clux_value_t *
construct_tuple_value(clux_context_t *ctx, node_t *node, clux_type_t *type) {
  size_t count = clux_tuple_elem_count(type);
  clux_value_t *fail = construct_positional_check(ctx, node, count, "tuple");
  if (fail) {
    return fail;
  }
  const location_t *loc = node_get_location(node);
  clux_value_t *tuple = clux_context_create_tuple(ctx, type, NULL, loc);
  if (clux_value_is_error(tuple)) {
    return tuple;
  }
  fail = construct_positional_fill(ctx, node, tuple, count);
  if (fail) {
    return fail;
  }
  return tuple;
}

/* 匿名构造条目：spread 展开的字段名借用展开值类型的驻留串（owned_name
   为 false，不参与本地释放）；value 归 scope 所有，条目只持借用引用。 */
typedef struct {
  const char *name;
  bool owned_name;
  clux_value_t *value;
} construct_anon_entry_t;

static void construct_anon_cleanup(allocator_t *allocator,
                                   construct_anon_entry_t *entries,
                                   size_t used) {
  for (size_t idx = 0; idx < used; idx++) {
    if (entries[idx].owned_name) {
      allocator_free(allocator, (void **)&entries[idx].name);
    }
  }
}

/* 匿名构造（白皮书 §5.10）：具名字段产出匿名 struct（先按元素序求值，
   字段表由元素给出，经 begin_struct_type 建型后逐字段写入），位置元素
   产出匿名 tuple。混用是错误；spread 限 struct 按名合并（后写覆盖先展开）；
   显式重名字段是错误。 */
static clux_value_t *construct_anonymous(clux_context_t *ctx, node_t *node) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  size_t elem_count = construct_node_get_elem_count(node);
  bool has_named = false;
  for (size_t idx = 0; idx < elem_count; idx++) {
    node_t *elem = construct_node_get_elem(node, idx);
    if (elem && (node_get_kind(elem) == CLUX_NODE_EXPR_CONSTRUCT_FIELD ||
                 /* spread 产出具名字段集——具名语义（struct 分支），纯 spread
            的匿名构造 .{ ...src } 产出匿名 struct 而非 tuple。 */
                 node_get_kind(elem) == CLUX_NODE_EXPR_REST)) {
      has_named = true;
      break;
    }
  }
  if (has_named) {
    /* pass 1：按元素序求值（spread 求值为待展开 struct 值），统计字段
       上界；混用在此一并报错。 */
    clux_value_t *elem_values[elem_count ? elem_count : 1];
    size_t entry_cap = 0;
    for (size_t idx = 0; idx < elem_count; idx++) {
      node_t *elem = construct_node_get_elem(node, idx);
      elem_values[idx] = NULL;
      if (!elem) {
        continue;
      }
      if (node_get_kind(elem) == CLUX_NODE_EXPR_REST) {
        node_t *operand = rest_node_get_expr(elem);
        if (!operand) {
          return clux_context_create_error_message(
              ctx, node_get_location(elem), "spread element requires a value");
        }
        clux_value_t *spread = exec_operand(ctx, operand);
        if (clux_value_is_error(spread)) {
          return spread;
        }
        clux_type_t *src_type = clux_value_get_type(spread);
        if (clux_type_get_kind(src_type) != CLUX_TYPE_STRUCT) {
          const char *src_name = clux_type_get_name(src_type);
          return clux_context_create_error_message(
              ctx,
              node_get_location(elem),
              "spread of non-struct type '%s' is not supported in "
              "construction",
              src_name ? src_name : "<unknown>");
        }
        elem_values[idx] = spread;
        entry_cap += clux_struct_field_count(src_type);
        continue;
      }
      if (node_get_kind(elem) != CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
        return clux_context_create_error_message(
            ctx,
            node_get_location(elem),
            "named and positional elements cannot be mixed in construction");
      }
      node_t *value_node = construct_field_node_get_value(elem);
      node_t *name_node = construct_field_node_get_name(elem);
      if (!name_node || !value_node) {
        return clux_context_create_error_message(
            ctx,
            node_get_location(elem),
            "construction field requires a name and a value");
      }
      clux_value_t *value = exec_operand(ctx, value_node);
      if (clux_value_is_error(value)) {
        return value;
      }
      elem_values[idx] = value;
      entry_cap++;
    }
    /* pass 2：组装条目表（spread 展开的字段读取为待合并 view，显式字段
       查重拒绝——覆盖语义只保留给 spread 合并）。 */
    construct_anon_entry_t entries[entry_cap ? entry_cap : 1];
    size_t used = 0;
    for (size_t idx = 0; idx < elem_count; idx++) {
      node_t *elem = construct_node_get_elem(node, idx);
      if (!elem) {
        continue;
      }
      if (node_get_kind(elem) == CLUX_NODE_EXPR_REST) {
        clux_type_t *src_type = clux_value_get_type(elem_values[idx]);
        size_t src_count = clux_struct_field_count(src_type);
        for (size_t fidx = 0; fidx < src_count; fidx++) {
          const char *name = clux_struct_get_field(src_type, fidx)->name;
          size_t eidx = 0;
          for (; eidx < used; eidx++) {
            if (strcmp(entries[eidx].name, name) == 0) {
              break;
            }
          }
          clux_value_t *view =
              clux_context_field(ctx, elem_values[idx], name, loc);
          if (clux_value_is_error(view)) {
            construct_anon_cleanup(allocator, entries, used);
            return view;
          }
          if (eidx < used) {
            entries[eidx].value = view;
            continue;
          }
          entries[used].name = name;
          entries[used].owned_name = false;
          entries[used].value = view;
          used++;
        }
        continue;
      }
      node_t *name_node = construct_field_node_get_name(elem);
      char *name = location_get(node_get_location(name_node), allocator);
      size_t eidx = 0;
      for (; eidx < used; eidx++) {
        if (strcmp(entries[eidx].name, name) == 0) {
          break;
        }
      }
      if (eidx < used && entries[eidx].owned_name) {
        clux_value_t *fail = clux_context_create_error_message(
            ctx,
            node_get_location(elem),
            "duplicate field '%s' in construction",
            name);
        allocator_free(allocator, (void **)&name);
        construct_anon_cleanup(allocator, entries, used);
        return fail;
      }
      if (eidx < used) {
        /* spread 展开的同名字段被后写的显式字段覆盖（名字仍借用自
           spread 源，owned_name 保持 false）。 */
        allocator_free(allocator, (void **)&name);
        entries[eidx].value = elem_values[idx];
        continue;
      }
      entries[used].name = name;
      entries[used].owned_name = true;
      entries[used].value = elem_values[idx];
      used++;
    }
    /* 建型（字段表由条目给出，匿名不去重）：begin_struct_type 要求类型
       具名，匿名 struct 用计数器生成唯一占位名——safe_cast 鸭子转换只看
       字段表（#27），占位名不参与类型相容。 */
    char anon_name[64];
    static size_t anon_struct_counter = 0;
    (void)snprintf(
        anon_name, sizeof(anon_name), "<anonymous-%zu>", anon_struct_counter++);
    clux_struct_field_t fields[used ? used : 1];
    for (size_t eidx = 0; eidx < used; eidx++) {
      fields[eidx].name = entries[eidx].name;
      fields[eidx].type = clux_value_get_type(entries[eidx].value);
    }
    clux_type_t *type =
        clux_context_begin_struct_type(ctx, anon_name, fields, used);
    if (!type) {
      construct_anon_cleanup(allocator, entries, used);
      return clux_context_create_error_message(
          ctx, loc, "anonymous struct construction failed");
    }
    clux_value_t *target = clux_context_create_struct(ctx, type, NULL, loc);
    if (clux_value_is_error(target)) {
      construct_anon_cleanup(allocator, entries, used);
      return target;
    }
    for (size_t eidx = 0; eidx < used; eidx++) {
      clux_value_t *view =
          clux_context_field(ctx, target, entries[eidx].name, loc);
      if (clux_value_is_error(view)) {
        construct_anon_cleanup(allocator, entries, used);
        return view;
      }
      clux_value_t *written =
          construct_write_view(ctx, view, entries[eidx].value, loc);
      if (clux_value_is_error(written)) {
        construct_anon_cleanup(allocator, entries, used);
        return written;
      }
    }
    construct_anon_cleanup(allocator, entries, used);
    return target;
  }
  /* 匿名 tuple：位置元素逐个求值 → 元素类型序列建型 → 逐位写入。 */
  clux_value_t *values[elem_count ? elem_count : 1];
  for (size_t idx = 0; idx < elem_count; idx++) {
    node_t *elem = construct_node_get_elem(node, idx);
    if (!elem) {
      return clux_context_create_error_message(
          ctx, loc, "tuple construction element %zu is missing", idx);
    }
    if (node_get_kind(elem) == CLUX_NODE_EXPR_REST) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(elem),
          "tuple construction does not support spread elements yet");
    }
    clux_value_t *value = exec_operand(ctx, elem);
    if (clux_value_is_error(value)) {
      return value;
    }
    values[idx] = value;
  }
  clux_type_t *elem_types[elem_count ? elem_count : 1];
  for (size_t idx = 0; idx < elem_count; idx++) {
    elem_types[idx] = clux_value_get_type(values[idx]);
  }
  clux_type_t *type = clux_context_get_tuple_type(ctx, elem_types, elem_count);
  if (!type) {
    return clux_context_create_error_message(
        ctx, loc, "tuple type creation failed");
  }
  clux_value_t *tuple = clux_context_create_tuple(ctx, type, NULL, loc);
  if (clux_value_is_error(tuple)) {
    return tuple;
  }
  clux_value_t *fail = construct_positional_fill(ctx, node, tuple, elem_count);
  if (fail) {
    return fail;
  }
  return tuple;
}

/* 构造表达式 .<type?>{ elem, ... }：TYPE 槽为 NULL 时是匿名构造；
   具名构造按类型分流（struct 穷尽字段 / tag union、cunion 恰一成员 /
   数组、元组位置元素），不支持构造的类型报语义化 error。 */
clux_value_t *exec_expr_construct(clux_context_t *ctx, node_t *node) {
  node_t *type_node = construct_node_get_type(node);
  if (!type_node) {
    return construct_anonymous(ctx, node);
  }
  clux_value_t *err = NULL;
  clux_type_t *type = exec_operand_type(ctx, type_node, &err);
  if (!type) {
    return err;
  }
  switch (clux_type_get_kind(type)) {
  case CLUX_TYPE_STRUCT:
    return construct_struct(ctx, node, type);
  case CLUX_TYPE_UNION:
  case CLUX_TYPE_CUNION:
    return construct_union_like(ctx, node, type);
  case CLUX_TYPE_BITFIELD:
    return construct_bitfield(ctx, node, type);
  case CLUX_TYPE_ARRAY:
    return construct_array(ctx, node, type);
  case CLUX_TYPE_TUPLE:
    return construct_tuple_value(ctx, node, type);
  default: {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "type '%s' does not support construction",
        type_name ? type_name : "<unknown>");
  }
  }
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
