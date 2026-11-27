/* sema check pass（§8.8 P7 + P8）：类型环境 + 名字解析 + 表达式 shadow
   推断。在原树原名上做检查（与 mangling 解耦）：第一遍按声明序把模块级
   名字登记进 module scope（类型求值为 type value，非泛型函数静默构建
   func 签名——函数名携带类型后跨函数调用可签名交验）；第二遍逐顶层
   函数体建帧（作用域栈 + 形参/捕获 + 块级预扫描）。P8：表达式按
   shadow 演算产出类型结果（data==0 即 shadow，引擎 binary/prefix/
   field/index/slice/invoke 对 shadow 输出 shadow，无副作用）；非法
   调用点报 diag（个数/实参类型，消息与引擎 invoke 一致）；类型结果表
   记录 STMT_VAR（语句节点）/EXPR_CONSTRUCT_FIELD/foreach 迭代变量三类
   锚点（§8.5 第一版）。错误全收集不 fast-fail：真 error 值沿表达式
   上抛、stmt 边界转 diag 后继续；walk 通道已报过的错误静默跳过。值登记
   全部走 create_* 通道（自动挂当前 scope，绝不手工 clux_scope_define）。
   未覆盖形态一律静默放行（P9+ 扩展）。P15：装饰器纳入检查——表达式
   求值 + func 形态对被装饰物类型签名交验（§8.12），协议与
   exec_apply_decorator 对齐（标记类透传、消息逐字一致）。 */
#include "sema/sema_internal.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/vec.h"
#include "engine/bitfield.h"
#include "engine/enum.h"
#include "engine/error.h"
#include "engine/qualifier.h"
#include "engine/scope.h"
#include "engine/cunion.h"
#include "engine/interface_type.h"
#include "engine/struct.h"
#include "engine/type.h"
#include "engine/union.h"
#include "engine/array.h"
#include "engine/slice.h"
#include "engine/value.h"
#include "parser/node_assign.h"
#include "parser/node_binary.h"
#include "parser/node_bitfield_decl.h"
#include "parser/node_bitfield_field.h"
#include "parser/node_builtin.h"
#include "parser/node_call.h"
#include "parser/node_construct.h"
#include "parser/node_construct_field.h"
#include "parser/node_decorator.h"
#include "parser/node_enum_decl.h"
#include "parser/node_enum_variant.h"
#include "parser/node_field.h"
#include "parser/node_func_capture.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_generic_param.h"
#include "parser/node_group.h"
#include "parser/node_interface_decl.h"
#include "parser/node_program.h"
#include "parser/node_rest.h"
#include "parser/node_slice.h"
#include "parser/node_stmt_assert.h"
#include "parser/node_stmt_bitfield.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_defer.h"
#include "parser/node_stmt_do_while.h"
#include "parser/node_stmt_enum.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_extern_cfunc.h"
#include "parser/node_stmt_extern_var.h"
#include "parser/node_stmt_for.h"
#include "parser/node_stmt_foreach.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_if.h"
#include "parser/node_stmt_interface.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_static_assert.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_switch.h"
#include "parser/node_stmt_type.h"
#include "parser/node_stmt_union.h"
#include "parser/node_stmt_var.h"
#include "parser/node_stmt_while.h"
#include "parser/node_struct_decl.h"
#include "parser/node_subscript.h"
#include "parser/node_switch_branch.h"
#include "parser/node_ternary.h"
#include "parser/node_tuple.h"
#include "parser/node_union.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_stmt(clux_sema_t *sema, node_t *stmt);
static clux_value_t *check_expr(clux_sema_t *sema, node_t *node);
static void check_func_decl(clux_sema_t *sema, node_t *decl);
static clux_type_t *
check_func_signature(clux_sema_t *sema, node_t *decl, int report);
static clux_type_t *
check_decorator(clux_sema_t *sema, node_t *decorator, clux_type_t *subject);

/* ---- 基础提取 ---- */

/* 名字提取：location 文本拷贝（调用方 allocator_free）。 */
static char *check_name_of(allocator_t *allocator, node_t *node) {
  if (!node) {
    return NULL;
  }
  return location_get(node_get_location(node), allocator);
}

/* 名字形态：无孩子叶子且 kind ∈ {SYMBOL, LITERAL_IDENTIFIER}。 */
static int check_is_name_form(node_t *node) {
  if (!node) {
    return 0;
  }
  node_kind_t kind = node_get_kind(node);
  if (kind != CLUX_NODE_SYMBOL && kind != CLUX_NODE_LITERAL_IDENTIFIER) {
    return 0;
  }
  return vec_get_size(node_get_children(node)) == 0;
}

/* 数值叶子 → u64（负数/非数值叶子返回 0）。数组长度与 bitfield 宽度用。 */
static int
check_numeric_u64(allocator_t *allocator, node_t *node, uint64_t *out) {
  if (!node || node_get_kind(node) != CLUX_NODE_LITERAL_NUMERIC ||
      vec_get_size(node_get_children(node)) != 0) {
    return 0;
  }
  char *text = check_name_of(allocator, node);
  if (!text) {
    return 0;
  }
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 0);
  int ok = end && *end == '\0' && end != text && text[0] != '-';
  allocator_free(allocator, (void **)&text);
  if (ok) {
    *out = value;
  }
  return ok;
}

/* ---- P8：错误值通道 ---- */

/* 真 error：type 为 ERROR 且 data!=0。占位值（error 类型 + data==0）是
   shadow——被引用时静默降级，防 error 假传播。 */
static int check_is_real_error(clux_value_t *value) {
  return value && clux_value_is_error(value) && !clux_value_is_shadow(value);
}

/* 真 error 值 → diag（stmt 边界消费）：raw 里是 clux_error_t*，消息经
   diag 入列后从下一条语句继续。 */
static void
check_error_diag(clux_sema_t *sema, node_t *node, clux_value_t *value) {
  clux_error_t *error = *(clux_error_t **)clux_value_get_raw(value, 0);
  clux_context_diag(sema->ctx,
                    CLUX_DIAG_ERROR,
                    node_get_location(node),
                    "%s",
                    error_get_message(error));
}

/* 表达式求值 + stmt 边界消费：真 error 转 diag 后丢弃（错误到此为止，
   不再向上传播）。 */
static clux_value_t *check_expr_checked(clux_sema_t *sema, node_t *node) {
  clux_value_t *value = check_expr(sema, node);
  if (check_is_real_error(value)) {
    check_error_diag(sema, node, value);
    return NULL;
  }
  return value;
}

/* ---- P8：字面量定型（与 exec/literal.c 同构） ---- */

static const struct {
  const char *text;
  clux_binary_op_t op;
} k_check_binary_ops[] = {
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

static const struct {
  const char *text;
  clux_prefix_op_t op;
} k_check_prefix_ops[] = {
    {"+", CLUX_OP_PLUS},
    {"-", CLUX_OP_NEGATIVE},
    {"~", CLUX_OP_NOT},
    {"!", CLUX_OP_LNOT},
};

static int check_binary_op_text(const char *text, clux_binary_op_t *out) {
  for (size_t idx = 0;
       idx < sizeof(k_check_binary_ops) / sizeof(k_check_binary_ops[0]);
       idx++) {
    if (strcmp(text, k_check_binary_ops[idx].text) == 0) {
      *out = k_check_binary_ops[idx].op;
      return 1;
    }
  }
  return 0;
}

static int check_prefix_op_text(const char *text, clux_prefix_op_t *out) {
  for (size_t idx = 0;
       idx < sizeof(k_check_prefix_ops) / sizeof(k_check_prefix_ops[0]);
       idx++) {
    if (strcmp(text, k_check_prefix_ops[idx].text) == 0) {
      *out = k_check_prefix_ops[idx].op;
      return 1;
    }
  }
  return 0;
}

/* 类型族值（type/模板/占位）：作为操作数时类型不可演算，降级。 */
static int check_is_opaque_value(clux_value_t *value) {
  clux_type_kind_t kind = clux_type_get_kind(clux_value_get_type(value));
  return kind == CLUX_TYPE_TYPE || kind == CLUX_TYPE_TYPE_TEMPLATE ||
         kind == CLUX_TYPE_FUNC_TEMPLATE || kind == CLUX_TYPE_ERROR;
}

/* 数字字面量定性：loc 直读文本剥后缀（白名单同 parser）；无后缀整数
   默认 i32，浮点后缀 f32/f64，exec/literal.c 同构。 */
static clux_type_t *check_numeric_type(clux_sema_t *sema, node_t *node) {
  const location_t *loc = node_get_location(node);
  const char *text = loc->begin.offset;
  size_t len = (size_t)(loc->end.offset - loc->begin.offset);
  size_t num_len = len;
  const char *suffix = sema_render_strip_suffix(text, len, &num_len);
  clux_type_kind_t kind = CLUX_TYPE_I32;
  if (suffix) {
    if (strcmp(suffix, "i8") == 0) {
      kind = CLUX_TYPE_I8;
    } else if (strcmp(suffix, "i16") == 0) {
      kind = CLUX_TYPE_I16;
    } else if (strcmp(suffix, "i64") == 0) {
      kind = CLUX_TYPE_I64;
    } else if (strcmp(suffix, "u8") == 0) {
      kind = CLUX_TYPE_U8;
    } else if (strcmp(suffix, "u16") == 0) {
      kind = CLUX_TYPE_U16;
    } else if (strcmp(suffix, "u32") == 0) {
      kind = CLUX_TYPE_U32;
    } else if (strcmp(suffix, "u64") == 0) {
      kind = CLUX_TYPE_U64;
    } else if (strcmp(suffix, "isize") == 0) {
      kind = CLUX_TYPE_ISIZE;
    } else if (strcmp(suffix, "usize") == 0) {
      kind = CLUX_TYPE_USIZE;
    } else if (strcmp(suffix, "f32") == 0) {
      kind = CLUX_TYPE_F32;
    } else if (strcmp(suffix, "f64") == 0) {
      kind = CLUX_TYPE_F64;
    }
  }
  return clux_context_builtin_type(sema->ctx, kind);
}

/* 字面量 → 定型 shadow（表达式结果形态静态可知）：
   STRING→str、CHARACTER→u8、BOOLEAN→bool、UNDEFINED→NULL（TDZ，
   类型由声明标注决定）。 */
static clux_value_t *check_literal_value(clux_sema_t *sema, node_t *node) {
  clux_context_t *ctx = sema->ctx;
  switch (node_get_kind(node)) {
  case CLUX_NODE_LITERAL_NUMERIC:
    return clux_context_create_shadow_value(ctx,
                                            check_numeric_type(sema, node));
  case CLUX_NODE_LITERAL_STRING:
    return clux_context_create_shadow_value(
        ctx, clux_context_builtin_type(ctx, CLUX_TYPE_STR));
  case CLUX_NODE_LITERAL_CHARACTER:
    return clux_context_create_shadow_value(
        ctx, clux_context_builtin_type(ctx, CLUX_TYPE_U8));
  case CLUX_NODE_LITERAL_BOOLEAN:
    return clux_context_create_shadow_value(
        ctx, clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
  default:
    return NULL;
  }
}

/* ---- 名字登记 ---- */

/* 登记前门禁：本层判重 + 内建类型遮蔽诊断。
   - 本层已有同名：dup_diag（局部语句）报 "duplicate declaration"；静默
     （模块级/块级预扫描）直接跳过——重复已由 walk 的 module_declare /
     ambiguous 通道报过，check 不重复报。
   - 命中 global 内建类型名（TYPE kind 值）：报 "cannot shadow builtin
     type"，仍继续登记（外层名字允许遮蔽，遮蔽本身合法只是要提示）。
   返回 1 = 允许登记（*name_out 由调用方释放），0 = 跳过。 */
static int check_define_gate(clux_sema_t *sema,
                             node_t *name_node,
                             int dup_diag,
                             char **name_out) {
  if (!name_node || !check_is_name_form(name_node)) {
    return 0;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = check_name_of(allocator, name_node);
  if (!name) {
    return 0;
  }
  clux_scope_t *scope = clux_context_get_scope(ctx);
  if (clux_scope_defined_here(scope, name)) {
    if (dup_diag) {
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(name_node),
                        "duplicate declaration '%s'",
                        name);
    }
    allocator_free(allocator, (void **)&name);
    return 0;
  }
  clux_value_t *builtin =
      clux_scope_get(clux_context_get_global_scope(ctx), name);
  if (builtin &&
      clux_type_get_kind(clux_value_get_type(builtin)) == CLUX_TYPE_TYPE) {
    clux_context_diag(ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(name_node),
                      "cannot shadow builtin type '%s'",
                      name);
  }
  *name_out = name;
  return 1;
}

/* 类型名登记（type alias / struct / enum / union / bitfield 名）：
   described 非空 → type value（名字即类型名，check_type_find 命中）；
   空 → 占位值（error 类型 + 零存储——名字可解析但不可当类型用，
   防引用级联 "undefined name" 假阳性）。 */
static void check_define_type_name(clux_sema_t *sema,
                                   node_t *name_node,
                                   clux_type_t *described,
                                   int dup_diag) {
  char *name = NULL;
  if (!check_define_gate(sema, name_node, dup_diag, &name)) {
    return;
  }
  clux_context_t *ctx = sema->ctx;
  if (described) {
    clux_context_create_type_value(
        ctx, described, name, node_get_location(name_node));
  } else {
    clux_context_create_value(ctx,
                              clux_context_get_error_type(ctx),
                              0,
                              name,
                              node_get_location(name_node));
  }
  allocator_free(clux_context_get_allocator(ctx), (void **)&name);
}

/* 值名登记（var / 形参 / 捕获 / 函数名）：create_value 挂当前 scope，
   携带值的类型（缺省占位 error 类型，零存储）。data==0 即 shadow——
   check pass 登记的值天然无存储，引用点按 shadow 演算。 */
static void check_define_value_name(clux_sema_t *sema,
                                    node_t *name_node,
                                    clux_type_t *value_type,
                                    int dup_diag) {
  char *name = NULL;
  if (!check_define_gate(sema, name_node, dup_diag, &name)) {
    return;
  }
  clux_context_t *ctx = sema->ctx;
  clux_context_create_value(ctx,
                            value_type ? value_type
                                       : clux_context_get_error_type(ctx),
                            0,
                            name,
                            node_get_location(name_node));
  allocator_free(clux_context_get_allocator(ctx), (void **)&name);
}

/* 泛型定义登记：模板值（AST 载荷借用原树，P7 仅作名字环境占位——
   泛型使用点 EXPR_SUBSCRIPT 静默跳过，载荷无人读取）。 */
static void check_define_template(clux_sema_t *sema,
                                  node_t *name_node,
                                  node_t *ast,
                                  int is_func) {
  char *name = NULL;
  if (!check_define_gate(sema, name_node, 0, &name)) {
    return;
  }
  clux_context_t *ctx = sema->ctx;
  if (is_func) {
    clux_context_create_func_template(
        ctx, ast, name, node_get_location(name_node));
  } else {
    clux_context_create_type_template(
        ctx, ast, name, node_get_location(name_node));
  }
  allocator_free(clux_context_get_allocator(ctx), (void **)&name);
}

/* ---- 类型求值 ---- */

/* 名字 → 类型（沿 scope 链查 TYPE kind 值并解引用）。 */
static clux_type_t *check_type_find(clux_sema_t *sema, const char *name) {
  clux_value_t *found = clux_scope_get(clux_context_get_scope(sema->ctx), name);
  if (!found ||
      clux_type_get_kind(clux_value_get_type(found)) != CLUX_TYPE_TYPE) {
    return NULL;
  }
  return *(clux_type_t **)clux_value_get_raw(found, 0);
}

/* 类型表达式 → clux_type_t。report=1 时未声明类型报 diag（形参/返回/
   局部 var/构造/标注等类型位置）；模块级与预扫描一律 report=0（求值
   失败降级占位即可，未声明的引用由名字检查通道兜底）。前缀 * / [] /
   [N] / ? / const / volatile 为 EXPR_BINARY 单/双子，tuple 为
   EXPR_TUPLE；未覆盖形态（泛型实例使用点 EXPR_SUBSCRIPT、func type、
   声明本体）静默返回 NULL。 */
static clux_type_t *
check_type_eval(clux_sema_t *sema, node_t *node, int report) {
  if (!node) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  switch (node_get_kind(node)) {
  case CLUX_NODE_EXPR_GROUP:
    return check_type_eval(sema, group_node_get_expr(node), report);
  case CLUX_NODE_SYMBOL:
  case CLUX_NODE_LITERAL_IDENTIFIER: {
    if (!check_is_name_form(node)) {
      return NULL;
    }
    char *name = check_name_of(allocator, node);
    clux_type_t *type = name ? check_type_find(sema, name) : NULL;
    if (!type && report && name) {
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(node),
                        "undeclared type '%s'",
                        name);
    }
    allocator_free(allocator, (void **)&name);
    return type;
  }
  case CLUX_NODE_EXPR_BINARY: {
    node_t *symbol = binary_node_get_symbol(node);
    char *op = check_name_of(allocator, symbol);
    node_t *left = binary_node_get_left(node);
    node_t *right = binary_node_get_right(node);
    clux_type_t *result = NULL;
    if (op && strcmp(op, "*") == 0) {
      clux_type_t *base = check_type_eval(sema, right, report);
      result = base ? clux_context_get_ptr_type(ctx, base) : NULL;
    } else if (op && op[0] == '[' && op[strlen(op) - 1] == ']') {
      /* '[]' 切片与 '[N]' 数组共用二元形态：SYMBOL location 区间跨越
         方括号（长度居中时 location_get 读出 "[2]" 而非 "[]"），按
         首 '[' 尾 ']' 结构判别，LEFT 是否缺失区分切片/数组。 */
      clux_type_t *base = check_type_eval(sema, right, report);
      if (!base) {
        result = NULL;
      } else if (left) {
        uint64_t len = 0;
        result = check_numeric_u64(allocator, left, &len)
                     ? clux_context_get_array_type(ctx, base, (size_t)len)
                     : NULL;
      } else {
        result = clux_context_get_slice_type(ctx, base);
      }
    } else if (op && strcmp(op, "?") == 0) {
      clux_type_t *base = check_type_eval(sema, right, report);
      result = base ? clux_context_get_optional_type(ctx, base) : NULL;
    } else if (op && strcmp(op, "const") == 0) {
      clux_type_t *base = check_type_eval(sema, right, report);
      result =
          base ? clux_type_get_qualified_type(base, CLUX_TYPE_CONST) : NULL;
    } else if (op && strcmp(op, "volatile") == 0) {
      clux_type_t *base = check_type_eval(sema, right, report);
      result =
          base ? clux_type_get_qualified_type(base, CLUX_TYPE_VOLATILE) : NULL;
    } else if (op && strcmp(op, "as") == 0) {
      result = check_type_eval(sema, right, report);
    }
    allocator_free(allocator, (void **)&op);
    return result;
  }
  case CLUX_NODE_EXPR_TUPLE: {
    size_t count = tuple_node_get_elem_count(node);
    if (!count) {
      return clux_context_get_tuple_type(ctx, NULL, 0);
    }
    clux_type_t **elems =
        allocator_alloc(allocator, count * sizeof(clux_type_t *));
    int ok = 1;
    for (size_t idx = 0; idx < count; idx++) {
      elems[idx] =
          check_type_eval(sema, tuple_node_get_elem(node, idx), report);
      if (!elems[idx]) {
        ok = 0;
      }
    }
    clux_type_t *result =
        ok ? clux_context_get_tuple_type(ctx, elems, count) : NULL;
    allocator_free(allocator, (void **)&elems);
    return result;
  }
  default:
    return NULL;
  }
}

/* ---- 类型构建（声明体 → 引擎类型） ---- */

/* 非泛型 STMT_STRUCT → struct 类型。FIELD 成员逐个求值，STMT_VAR /
   STMT_FUNC / EMBED 成员跳过（方法与属性已由 walk §2.10 hoist）；任一
   字段求值失败整体降级（返回 NULL，登记占位）。字段名经引擎 strs
   驻留，调用方临时 buffer 构建后释放。 */
static clux_type_t *check_build_struct(clux_sema_t *sema, node_t *stmt) {
  node_t *decl = stmt_struct_node_get_struct_decl(stmt);
  node_t *body = decl ? struct_decl_node_get_body(decl) : NULL;
  if (!decl || !body) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = check_name_of(allocator, struct_decl_node_get_name(decl));
  if (!name) {
    return NULL;
  }
  vec_t *children = node_get_children(body);
  size_t count = vec_get_size(children);
  if (!count) {
    clux_type_t *empty = clux_context_begin_struct_type(ctx, name, NULL, 0);
    allocator_free(allocator, (void **)&name);
    return empty; /* 零字段合法 */
  }
  clux_struct_field_t *fields =
      allocator_alloc(allocator, count * sizeof(clux_struct_field_t));
  size_t filled = 0;
  int ok = 1;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *member = vec_get(children, idx);
    if (!member || node_get_kind(member) != CLUX_NODE_FIELD) {
      continue; /* var/func/embed 成员不参与字段表 */
    }
    char *name = check_name_of(allocator, field_node_get_name(member));
    clux_type_t *type = check_type_eval(sema, field_node_get_type(member), 0);
    /* §8.13 P16：字段装饰器 eval-only（exec 端以 type value 为实参
       invoke，check 层不可模拟同一形态——opaque gate）；求值诊断
       只报告，不影响字段表构建。 */
    for (size_t didx = 0; didx < field_node_get_decorator_count(member);
         didx++) {
      check_decorator(sema, field_node_get_decorator(member, didx), NULL);
    }
    if (name && type) {
      fields[filled].name = name;
      fields[filled].type = type;
      filled++;
    } else {
      ok = 0;
      allocator_free(allocator, (void **)&name);
    }
  }
  clux_type_t *result =
      ok ? clux_context_begin_struct_type(ctx, name, fields, filled) : NULL;
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&fields[idx].name);
  }
  allocator_free(allocator, (void **)&fields);
  allocator_free(allocator, (void **)&name);
  return result;
}

/* struct 属性成员（STMT_VAR 成员）装饰器检查（§8.12 P15）：subject 取
   类型标注（无标注/求值失败降级 eval-only），链式交验——exec pass C
   同构（包装值成为 attr 值，绑定类型即链尾）。attr 值表达式 P9 不
   检查（CTFE 通道），这里只验装饰器；TDZ（= undefined）与无初值成员
   无值可包装，与 exec 同构跳过。 */
static void check_struct_attr_decorators(clux_sema_t *sema, node_t *stmt) {
  node_t *decl = stmt_struct_node_get_struct_decl(stmt);
  node_t *body = decl ? struct_decl_node_get_body(decl) : NULL;
  if (!body) {
    return;
  }
  vec_t *members = node_get_children(body);
  for (size_t idx = 0; idx < vec_get_size(members); idx++) {
    node_t *member = vec_get(members, idx);
    if (!member || node_get_kind(member) != CLUX_NODE_STMT_VAR) {
      continue;
    }
    node_t *value_node = stmt_var_node_get_value(member);
    if (!value_node ||
        node_get_kind(value_node) == CLUX_NODE_LITERAL_UNDEFINED) {
      continue; /* TDZ/无初值：exec 不适用装饰器 */
    }
    clux_type_t *subject =
        check_type_eval(sema, stmt_var_node_get_type(member), 0);
    for (size_t didx = 0; didx < stmt_var_node_get_decorator_count(member);
         didx++) {
      subject = check_decorator(
          sema, stmt_var_node_get_decorator(member, didx), subject);
      /* NULL 即失败/降级：后续装饰器经 subject=NULL 继续 eval-only。 */
    }
  }
}

/* struct 三表安装（P10）：壳建好且类型名 define 之后调用——方法签名
   可解析 struct 自身（shell 先行，与 exec 的两阶段构造同构）。方法成员
   （body 内 STMT_FUNC）静默构建签名 + 产 func shadow 值，经 context
   install 通道驻留名字并拷入三表；P15：方法装饰器链式包装签名（exec
   pass C 包装值接管 method 表的镜像），失败回退原签名（diag 已报）。
   attribute（var 成员）P9 不安装（值表达式走 CTFE 通道——装饰器经
   check_struct_attr_decorators 检查）。泛型方法跳过（随实例化通道——
   实例体同名路径逐实例覆盖）。安装失败静默（install 只允许一次，
   重复注册路径天然 no-op）。 */
static void check_struct_install_methods(clux_sema_t *sema,
                                         node_t *stmt,
                                         clux_type_t *type) {
  if (!type || clux_type_get_kind(type) != CLUX_TYPE_STRUCT) {
    return;
  }
  node_t *decl = stmt_struct_node_get_struct_decl(stmt);
  node_t *body = decl ? struct_decl_node_get_body(decl) : NULL;
  if (!body) {
    return;
  }
  vec_t *children = node_get_children(body);
  size_t count = vec_get_size(children);
  if (!count) {
    return;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  clux_struct_attr_t attrs[count];
  const char *methods[count];
  size_t filled = 0;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *member = vec_get(children, idx);
    if (!member || node_get_kind(member) != CLUX_NODE_STMT_FUNC) {
      continue;
    }
    node_t *func_decl = stmt_func_node_get_func_decl(member);
    node_t *name_node = func_decl ? func_decl_node_get_name(func_decl) : NULL;
    if (!func_decl || func_decl_node_get_generics(func_decl) || !name_node) {
      continue; /* 泛型方法/缺名：随实例化通道，不进名单 */
    }
    char *mname = location_get(node_get_location(name_node), allocator);
    clux_type_t *signature = check_func_signature(sema, func_decl, 0);
    if (!mname || !signature) {
      allocator_free(allocator, (void **)&mname);
      continue;
    }
    clux_type_t *subject = signature;
    for (size_t didx = 0; didx < stmt_func_node_get_decorator_count(member);
         didx++) {
      subject = check_decorator(
          sema, stmt_func_node_get_decorator(member, didx), subject);
      if (subject) {
        signature = subject;
      }
    }
    attrs[filled].name = mname;
    attrs[filled].value = clux_context_create_shadow_value(ctx, signature);
    methods[filled] = mname;
    filled++;
  }
  if (filled) {
    (void)clux_context_install_struct_tables(
        ctx, type, attrs, filled, methods, filled, NULL, 0);
  }
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&attrs[idx].name);
  }
}

/* 非泛型 STMT_ENUM → enum 类型。底层缺省 i32，须整数或 str；variant 值
   NULL 按序号、数值叶子按字面量，其余形态（含 str 底层常量）P7 降级。 */
static clux_type_t *check_build_enum(clux_sema_t *sema, node_t *stmt) {
  node_t *decl = stmt_enum_node_get_enum_decl(stmt);
  node_t *body = decl ? enum_decl_node_get_body(decl) : NULL;
  if (!decl || !body) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = check_name_of(allocator, enum_decl_node_get_name(decl));
  clux_type_t *underlying =
      check_type_eval(sema, enum_decl_node_get_underlying(decl), 0);
  if (!underlying) {
    underlying = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  }
  clux_type_kind_t kind = clux_type_get_kind(underlying);
  int is_integer = (kind >= CLUX_TYPE_I8 && kind <= CLUX_TYPE_U64) ||
                   kind == CLUX_TYPE_ISIZE || kind == CLUX_TYPE_USIZE;
  vec_t *children = node_get_children(body);
  size_t count = vec_get_size(children);
  if (!name || (!is_integer && kind != CLUX_TYPE_STR) || !count) {
    allocator_free(allocator, (void **)&name);
    return NULL; /* 空表/底层非整数非 str 由引擎 error 通道兜底 */
  }
  clux_enum_variant_t *variants =
      allocator_alloc(allocator, count * sizeof(clux_enum_variant_t));
  size_t filled = 0;
  int ok = 1;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *variant = vec_get(children, idx);
    if (!variant || node_get_kind(variant) != CLUX_NODE_ENUM_VARIANT) {
      continue;
    }
    char *vname = check_name_of(allocator, enum_variant_node_get_name(variant));
    node_t *value = enum_variant_node_get_value(variant);
    uint64_t raw = 0;
    if (!vname || kind == CLUX_TYPE_STR ||
        (value && !check_numeric_u64(allocator, value, &raw))) {
      ok = 0; /* str 底层常量与非数值形态留待 P8+（常量折叠通道） */
      allocator_free(allocator, (void **)&vname);
      continue;
    }
    variants[filled].name = vname;
    variants[filled].value = value ? raw : (uint64_t)filled;
    variants[filled].str = NULL;
    filled++;
  }
  clux_type_t *result = ok ? clux_context_create_enum_type(
                                 ctx, name, underlying, variants, filled)
                           : NULL;
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&variants[idx].name);
  }
  allocator_free(allocator, (void **)&variants);
  allocator_free(allocator, (void **)&name);
  return result;
}

/* 非泛型 STMT_UNION → tag union / cunion（按关键字文本分派）。成员
   FIELD 逐个求值，任一失败或空表降级。 */
static clux_type_t *check_build_union(clux_sema_t *sema, node_t *stmt) {
  node_t *decl = stmt_union_node_get_union_decl(stmt);
  node_t *body = decl ? union_node_get_body(decl) : NULL;
  if (!decl || !body) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = check_name_of(allocator, union_node_get_name(decl));
  char *keyword = check_name_of(allocator, union_node_get_keyword(decl));
  int is_cunion = keyword && strcmp(keyword, "cunion") == 0;
  allocator_free(allocator, (void **)&keyword);
  vec_t *children = node_get_children(body);
  size_t count = vec_get_size(children);
  if (!name || !count) {
    allocator_free(allocator, (void **)&name);
    return NULL; /* 空成员表非法（引擎契约） */
  }
  clux_union_member_t *members =
      allocator_alloc(allocator, count * sizeof(clux_union_member_t));
  size_t filled = 0;
  int ok = 1;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *member = vec_get(children, idx);
    if (!member || node_get_kind(member) != CLUX_NODE_FIELD) {
      continue;
    }
    char *mname = check_name_of(allocator, field_node_get_name(member));
    clux_type_t *type = check_type_eval(sema, field_node_get_type(member), 0);
    if (mname && type) {
      members[filled].name = mname;
      members[filled].type = type;
      filled++;
    } else {
      ok = 0;
      allocator_free(allocator, (void **)&mname);
    }
  }
  clux_type_t *result =
      ok && filled
          ? (is_cunion
                 ? clux_context_create_cunion_type(
                       ctx,
                       name,
                       (const clux_cunion_member_t *)(const void *)members,
                       filled)
                 : clux_context_create_union_type(ctx, name, members, filled))
          : NULL;
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&members[idx].name);
  }
  allocator_free(allocator, (void **)&members);
  allocator_free(allocator, (void **)&name);
  return result;
}

/* 非泛型 STMT_BITFIELD → bitfield 类型。底层缺省 u32 且仅 u8/u16/u32/
   u64；字段名 + 数值宽度，任一非法降级。 */
static clux_type_t *check_build_bitfield(clux_sema_t *sema, node_t *stmt) {
  node_t *decl = stmt_bitfield_node_get_bitfield_decl(stmt);
  node_t *body = decl ? bitfield_decl_node_get_body(decl) : NULL;
  if (!decl || !body) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = check_name_of(allocator, bitfield_decl_node_get_name(decl));
  clux_type_t *underlying =
      check_type_eval(sema, bitfield_decl_node_get_underlying(decl), 0);
  if (!underlying) {
    underlying = clux_context_builtin_type(ctx, CLUX_TYPE_U32);
  }
  clux_type_kind_t kind = clux_type_get_kind(underlying);
  if (kind != CLUX_TYPE_U8 && kind != CLUX_TYPE_U16 && kind != CLUX_TYPE_U32 &&
      kind != CLUX_TYPE_U64) {
    allocator_free(allocator, (void **)&name);
    return NULL;
  }
  vec_t *children = node_get_children(body);
  size_t count = vec_get_size(children);
  if (!name || !count) {
    allocator_free(allocator, (void **)&name);
    return NULL; /* 空字段表非法（引擎契约） */
  }
  clux_bitfield_field_def_t *fields =
      allocator_alloc(allocator, count * sizeof(clux_bitfield_field_def_t));
  size_t filled = 0;
  int ok = 1;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *field = vec_get(children, idx);
    if (!field || node_get_kind(field) != CLUX_NODE_BITFIELD_FIELD) {
      continue;
    }
    char *fname = check_name_of(allocator, bitfield_field_node_get_name(field));
    uint64_t width = 0;
    if (!fname ||
        !check_numeric_u64(
            allocator, bitfield_field_node_get_width(field), &width) ||
        width == 0) {
      ok = 0;
      allocator_free(allocator, (void **)&fname);
      continue;
    }
    fields[filled].name = fname;
    fields[filled].width = (size_t)width;
    filled++;
  }
  clux_type_t *result = ok ? clux_context_create_bitfield_type(
                                 ctx, name, underlying, fields, filled)
                           : NULL;
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&fields[idx].name);
  }
  allocator_free(allocator, (void **)&fields);
  allocator_free(allocator, (void **)&name);
  return result;
}

/* ---- P8：func 签名构建 ---- */

/* 声明 → func 类型（供模块级/局部函数名携带类型与闭包表达式定型）。
   report 转发给类型槽求值（模块级登记静默，帧内已由 check_func_decl
   报过）。捕获表支持 FUNC_CAPTURE 单项与 FUNC_PARAMS 容错两形态；
   无标注捕获从当前作用域现值取类型（|f| 捕获的是定义点的名字），
   缺返回类型/缺形参标注整体降级 NULL（登记占位，调用点静默放行）。 */
static clux_type_t *
check_func_signature(clux_sema_t *sema, node_t *decl, int report) {
  if (!decl || func_decl_node_get_generics(decl)) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  node_t *return_node = func_decl_node_get_return(decl);
  if (!return_node) {
    return NULL; /* 缺返回类型：exec 报硬错误，check 侧 P8 降级 */
  }
  clux_type_t *return_type = check_type_eval(sema, return_node, report);
  if (!return_type) {
    return NULL;
  }
  /* 匿名闭包无函数名——签名本身携带闭包类型（形参/返回/捕获俱全），
     缺名不应让整体降级为未知类型；用内部合成名构建（clux 中不存在
     未知类型的标识符）。 */
  static const char closure_name[] = "<closure>";
  char *name = check_name_of(allocator, func_decl_node_get_name(decl));
  if (!name) {
    name = allocator_alloc(allocator, sizeof(closure_name));
    memcpy(name, closure_name, sizeof(closure_name));
  }
  /* 捕获表：与 check_captures 同两形态。 */
  node_t *captures_node = func_decl_node_get_captures(decl);
  vec_t *capture_children =
      captures_node ? node_get_children(captures_node) : NULL;
  size_t capture_capacity =
      capture_children ? vec_get_size(capture_children) : 0;
  clux_func_capture_t *captures =
      capture_capacity
          ? allocator_alloc(allocator,
                            capture_capacity * sizeof(clux_func_capture_t))
          : NULL;
  size_t capture_filled = 0;
  if (captures_node &&
      node_get_kind(captures_node) == CLUX_NODE_FUNC_CAPTURES) {
    for (size_t idx = 0; idx < capture_capacity; idx++) {
      node_t *child = vec_get(capture_children, idx);
      if (!child || node_get_kind(child) != CLUX_NODE_FUNC_CAPTURE) {
        continue;
      }
      char *cname = check_name_of(allocator, func_capture_node_get_name(child));
      node_t *ctype_node = func_capture_node_get_type(child);
      clux_type_t *ctype =
          ctype_node ? check_type_eval(sema, ctype_node, report) : NULL;
      if (!ctype && !ctype_node && cname) {
        /* |f| 无标注捕获：类型取定义点作用域现值。 */
        clux_value_t *found =
            clux_scope_get(clux_context_get_scope(ctx), cname);
        clux_type_t *found_type = found ? clux_value_get_type(found) : NULL;
        if (found_type && clux_type_get_kind(found_type) != CLUX_TYPE_ERROR &&
            clux_type_get_kind(found_type) != CLUX_TYPE_TYPE) {
          ctype = found_type;
        }
      }
      if (!cname || !ctype) {
        allocator_free(allocator, (void **)&cname);
        continue; /* 不可定型的捕获项跳过（签名仍可构建） */
      }
      captures[capture_filled].name = cname;
      captures[capture_filled].type = ctype;
      capture_filled++;
    }
  }
  /* 形参类型序列：任一缺类型整体降级。 */
  node_t *params_node = func_decl_node_get_params(decl);
  size_t param_count =
      params_node ? func_params_node_get_param_count(params_node) : 0;
  clux_type_t **params =
      param_count
          ? allocator_alloc(allocator, param_count * sizeof(clux_type_t *))
          : NULL;
  int params_ok = 1;
  for (size_t idx = 0; idx < param_count; idx++) {
    node_t *param = func_params_node_get_param(params_node, idx);
    node_t *ptype_node = param ? func_param_node_get_type(param) : NULL;
    clux_type_t *ptype =
        ptype_node ? check_type_eval(sema, ptype_node, report) : NULL;
    if (!ptype) {
      params_ok = 0;
      break;
    }
    params[idx] = ptype;
  }
  clux_type_t *signature = NULL;
  if (params_ok && name) {
    signature = clux_context_create_func_type(
        ctx, name, captures, capture_filled, params, param_count, return_type);
    /* 捕获名经 create_func_type 驻留，临时名统一释放。 */
    for (size_t idx = 0; idx < capture_filled; idx++) {
      allocator_free(allocator, (void **)&captures[idx].name);
    }
  } else {
    for (size_t idx = 0; idx < capture_filled; idx++) {
      allocator_free(allocator, (void **)&captures[idx].name);
    }
  }
  allocator_free(allocator, (void **)&params);
  allocator_free(allocator, (void **)&captures);
  allocator_free(allocator, (void **)&name);
  return signature;
}

/* §8.9 P12：interface 真实构建——成员签名逐个经 check_func_signature 求
   值（与显式 func 类型表达式结构一致即恒等），clux_context_create_
   interface_type 入库（结构去重、成员名经 strs 驻留）。泛型 interface
   （[W]）暂缓（与 exec 同步：F-bounded 随泛型 interface 批次开放）；成员
   非法（缺名/缺返回/带体/带捕获/带泛型/非签名节点）整体降级 NULL——登记
   占位保持旧行为，定义体硬错误由 exec 通道兜底。 */
static clux_type_t *check_build_interface(clux_sema_t *sema, node_t *stmt) {
  node_t *decl = stmt_interface_node_get_interface_decl(stmt);
  node_t *body = decl ? interface_decl_node_get_body(decl) : NULL;
  if (!decl || interface_decl_node_get_generics(decl) || !body) {
    return NULL;
  }
  size_t count = interface_members_node_get_member_count(body);
  if (count == 0) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  clux_interface_member_t *members =
      allocator_alloc(allocator, count * sizeof(clux_interface_member_t));
  for (size_t idx = 0; idx < count; idx++) {
    members[idx].name = NULL;
    members[idx].func = NULL;
  }
  int failed = 0;
  for (size_t idx = 0; idx < count && !failed; idx++) {
    node_t *member = interface_members_node_get_member(body, idx);
    if (!member || node_get_kind(member) != CLUX_NODE_EXPR_FUNC_DECL ||
        func_decl_node_get_body(member) ||
        func_decl_node_get_captures(member) ||
        func_decl_node_get_generics(member)) {
      failed = 1;
      break;
    }
    members[idx].name =
        check_name_of(allocator, func_decl_node_get_name(member));
    members[idx].func = check_func_signature(sema, member, 0);
    if (!members[idx].name || !members[idx].func ||
        clux_type_get_kind(members[idx].func) != CLUX_TYPE_FUNC) {
      failed = 1;
    }
  }
  clux_type_t *type = NULL;
  if (!failed) {
    char *name = check_name_of(allocator, interface_decl_node_get_name(decl));
    type = clux_context_create_interface_type(
        ctx, name ? name : "interface", members, count);
    allocator_free(allocator, (void **)&name);
  }
  for (size_t idx = 0; idx < count; idx++) {
    allocator_free(allocator, (void **)&members[idx].name);
  }
  allocator_free(allocator, (void **)&members);
  return type;
}

/* ---- 模块级注册（第一遍，按声明序） ---- */

static void check_module_register(clux_sema_t *sema, node_t *stmt);

static void check_module_register_list(clux_sema_t *sema, node_t *block) {
  if (!block) {
    return;
  }
  vec_t *children = node_get_children(block);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    check_module_register(sema, vec_get(children, idx));
  }
}

static void check_module_register(clux_sema_t *sema, node_t *stmt) {
  if (!stmt) {
    return;
  }
  switch (node_get_kind(stmt)) {
  case CLUX_NODE_STMT_VAR: {
    /* 模块级 var（含 comptime var）：TYPE 槽静默求值升级占位（值表达式
       P8 不检查——walk 的常量回填/CTFE 通道负责）。P15（§8.12）：
       有初值且非 undefined 时装饰器链式包装登记类型（TDZ 与 exec
       同构跳过；无标注 var 类型在定点通道定型，签名交验降级），
       失败回退未装饰类型登记防级联。 */
    clux_type_t *annotated =
        check_type_eval(sema, stmt_var_node_get_type(stmt), 0);
    node_t *value_node = stmt_var_node_get_value(stmt);
    clux_type_t *bound = annotated;
    if (value_node &&
        node_get_kind(value_node) != CLUX_NODE_LITERAL_UNDEFINED) {
      clux_type_t *subject = annotated;
      for (size_t didx = 0; didx < stmt_var_node_get_decorator_count(stmt);
           didx++) {
        subject = check_decorator(
            sema, stmt_var_node_get_decorator(stmt, didx), subject);
        if (subject) {
          bound = subject;
        }
      }
    }
    check_define_value_name(sema, stmt_var_node_get_name(stmt), bound, 0);
    return;
  }
  case CLUX_NODE_STMT_FUNC: {
    node_t *decl = stmt_func_node_get_func_decl(stmt);
    node_t *name = decl ? func_decl_node_get_name(decl) : NULL;
    if (decl && func_decl_node_get_generics(decl)) {
      check_define_template(sema, name, decl, 1);
    } else if (decl) {
      /* 非泛型函数：静默构建签名登记——函数名携带类型后，函数体与跨
         函数调用可签名交验（P8）。P15（§8.12）：装饰器链式包装签名
         （exec 落地后名字即包装值），define 前求值与 exec 同序；失败
         回退原签名登记防级联。 */
      clux_type_t *signature = check_func_signature(sema, decl, 0);
      clux_type_t *bound = signature;
      clux_type_t *subject = signature;
      for (size_t didx = 0; didx < stmt_func_node_get_decorator_count(stmt);
           didx++) {
        subject = check_decorator(
            sema, stmt_func_node_get_decorator(stmt, didx), subject);
        if (subject) {
          bound = subject;
        }
      }
      check_define_value_name(sema, name, bound, 0);
    } else {
      check_define_value_name(sema, name, NULL, 0);
    }
    return;
  }
  case CLUX_NODE_STMT_TYPE: {
    node_t *name = stmt_type_node_get_name(stmt);
    if (stmt_type_node_get_generics(stmt)) {
      check_define_template(sema, name, stmt, 0);
    } else {
      clux_type_t *type =
          check_type_eval(sema, stmt_type_node_get_value(stmt), 0);
      /* §8.13 P16：类型位置装饰器 eval-only（func 交验留 exec 运行期
         报告），define 前求值以对齐 exec 拦截语义。 */
      for (size_t didx = 0; didx < stmt_type_node_get_decorator_count(stmt);
           didx++) {
        check_decorator(sema, stmt_type_node_get_decorator(stmt, didx), NULL);
      }
      check_define_type_name(sema, name, type, 0);
    }
    return;
  }
  case CLUX_NODE_STMT_STRUCT: {
    node_t *decl = stmt_struct_node_get_struct_decl(stmt);
    node_t *name = decl ? struct_decl_node_get_name(decl) : NULL;
    if (decl && struct_decl_node_get_generics(decl)) {
      check_define_template(sema, name, stmt, 0);
    } else {
      clux_type_t *type = check_build_struct(sema, stmt);
      /* §8.12 P15：顶层装饰器在 define 前求值（类型语句 eval-only，对齐
         exec P14 三段式的拦截语义）；attr 成员装饰器就地链式检查。 */
      for (size_t didx = 0; didx < stmt_struct_node_get_decorator_count(stmt);
           didx++) {
        check_decorator(sema, stmt_struct_node_get_decorator(stmt, didx), NULL);
      }
      check_struct_attr_decorators(sema, stmt);
      check_define_type_name(sema, name, type, 0);
      /* 类型名 define 后安装方法名单（签名可解析 struct 自身）。 */
      check_struct_install_methods(sema, stmt, type);
    }
    return;
  }
  case CLUX_NODE_STMT_ENUM: {
    node_t *decl = stmt_enum_node_get_enum_decl(stmt);
    node_t *name = decl ? enum_decl_node_get_name(decl) : NULL;
    for (size_t didx = 0; didx < stmt_enum_node_get_decorator_count(stmt);
         didx++) {
      check_decorator(sema, stmt_enum_node_get_decorator(stmt, didx), NULL);
    }
    check_define_type_name(sema, name, check_build_enum(sema, stmt), 0);
    return;
  }
  case CLUX_NODE_STMT_UNION: {
    node_t *decl = stmt_union_node_get_union_decl(stmt);
    node_t *name = decl ? union_node_get_name(decl) : NULL;
    if (decl && union_node_get_generics(decl)) {
      check_define_template(sema, name, stmt, 0);
    } else {
      for (size_t didx = 0; didx < stmt_union_node_get_decorator_count(stmt);
           didx++) {
        check_decorator(sema, stmt_union_node_get_decorator(stmt, didx), NULL);
      }
      check_define_type_name(sema, name, check_build_union(sema, stmt), 0);
    }
    return;
  }
  case CLUX_NODE_STMT_BITFIELD: {
    node_t *decl = stmt_bitfield_node_get_bitfield_decl(stmt);
    node_t *name = decl ? bitfield_decl_node_get_name(decl) : NULL;
    for (size_t didx = 0; didx < stmt_bitfield_node_get_decorator_count(stmt);
         didx++) {
      check_decorator(sema, stmt_bitfield_node_get_decorator(stmt, didx), NULL);
    }
    check_define_type_name(sema, name, check_build_bitfield(sema, stmt), 0);
    return;
  }
  case CLUX_NODE_STMT_INTERFACE: {
    /* §8.9 P12：真实构建后登记——约束表达式经 check_type_eval 可解析出
       interface 类型（结构判定/派生判定的输入）。构建失败降级占位。 */
    node_t *decl = stmt_interface_node_get_interface_decl(stmt);
    node_t *name = decl ? interface_decl_node_get_name(decl) : NULL;
    for (size_t didx = 0; didx < stmt_interface_node_get_decorator_count(stmt);
         didx++) {
      check_decorator(
          sema, stmt_interface_node_get_decorator(stmt, didx), NULL);
    }
    check_define_type_name(sema, name, check_build_interface(sema, stmt), 0);
    return;
  }
  case CLUX_NODE_STMT_EXTERN_CFUNC:
    check_define_value_name(
        sema, stmt_extern_cfunc_node_get_name(stmt), NULL, 0);
    return;
  case CLUX_NODE_STMT_EXTERN_VAR:
    check_define_value_name(sema, stmt_extern_var_node_get_name(stmt), NULL, 0);
    return;
  case CLUX_NODE_STMT_COMPTIME_IF:
    /* 槽布局与 STMT_IF 相同：THEN/ELSE 为块，体内声明照常注册。 */
    check_module_register_list(sema, node_get_child(stmt, 2));
    check_module_register_list(sema, node_get_child(stmt, 3));
    return;
  case CLUX_NODE_STMT_COMPTIME_FOREACH:
    /* 槽布局与 STMT_FOREACH 相同：BODY 为块。 */
    check_module_register_list(sema, node_get_child(stmt, 4));
    return;
  default:
    return; /* import/reexport/extern_lib/test/表达式语句不登记 */
  }
}

/* ---- 函数体帧（第二遍） ---- */

/* 捕获表：FUNC_CAPTURES（FUNC_CAPTURE 单项，或 defer 容错下的
   FUNC_PARAMS 形态）。类型标注按类型位置报 diag，默认值 P7 跳过。 */
static void check_captures(clux_sema_t *sema, node_t *captures) {
  if (!captures || node_get_kind(captures) != CLUX_NODE_FUNC_CAPTURES) {
    return;
  }
  vec_t *children = node_get_children(captures);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    node_t *child = vec_get(children, idx);
    if (!child) {
      continue;
    }
    if (node_get_kind(child) == CLUX_NODE_FUNC_CAPTURE) {
      clux_type_t *type =
          check_type_eval(sema, func_capture_node_get_type(child), 1);
      check_define_value_name(sema, func_capture_node_get_name(child), type, 1);
    } else if (node_get_kind(child) == CLUX_NODE_FUNC_PARAMS) {
      size_t count = func_params_node_get_param_count(child);
      for (size_t pidx = 0; pidx < count; pidx++) {
        node_t *param = func_params_node_get_param(child, pidx);
        clux_type_t *type =
            check_type_eval(sema, func_param_node_get_type(param), 1);
        check_define_value_name(sema, func_param_node_get_name(param), type, 1);
      }
    }
  }
}

/* 块级预扫描：体内 hoistable 声明按原名预登记进函数 scope（前向引用
   天然成立）。重复静默跳过（ambiguous 已由 walk 报过）。闭包体与
   comptime 块不进（各有自己的检查通道/整树跳过）。非泛型 STMT_TYPE
   急切求值——否则 `var x: Foo` 引用局部类型别名会假阳性。非泛型
   STMT_FUNC 静默构建签名升级占位（与模块级同通道）。 */
static void check_local_scan(clux_sema_t *sema, node_t *node) {
  if (!node) {
    return;
  }
  switch (node_get_kind(node)) {
  case CLUX_NODE_STMT_FUNC: {
    node_t *decl = stmt_func_node_get_func_decl(node);
    node_t *name = decl ? func_decl_node_get_name(decl) : NULL;
    if (decl && func_decl_node_get_generics(decl)) {
      check_define_template(sema, name, decl, 1);
    } else if (decl) {
      clux_type_t *signature = check_func_signature(sema, decl, 0);
      check_define_value_name(sema, name, signature, 0);
    } else {
      check_define_value_name(sema, name, NULL, 0);
    }
    return; /* 嵌套函数体由自身检查通道扫描 */
  }
  case CLUX_NODE_EXPR_FUNC_DECL:
    return; /* 闭包体同理：进入闭包检查（check_expr）时自建帧 */
  case CLUX_NODE_STMT_TYPE: {
    node_t *name = stmt_type_node_get_name(node);
    if (stmt_type_node_get_generics(node)) {
      check_define_template(sema, name, node, 0);
    } else {
      clux_type_t *type =
          check_type_eval(sema, stmt_type_node_get_value(node), 0);
      /* §8.13 P16：类型位置装饰器 eval-only，与模块级 STMT_TYPE 同构。 */
      for (size_t didx = 0; didx < stmt_type_node_get_decorator_count(node);
           didx++) {
        check_decorator(sema, stmt_type_node_get_decorator(node, didx), NULL);
      }
      check_define_type_name(sema, name, type, 0);
    }
    return;
  }
  case CLUX_NODE_STMT_STRUCT: {
    node_t *decl = stmt_struct_node_get_struct_decl(node);
    node_t *name = decl ? struct_decl_node_get_name(decl) : NULL;
    if (decl && struct_decl_node_get_generics(decl)) {
      check_define_template(sema, name, node, 0);
    } else {
      check_define_type_name(sema, name, check_build_struct(sema, node), 0);
    }
    return;
  }
  case CLUX_NODE_STMT_ENUM: {
    node_t *decl = stmt_enum_node_get_enum_decl(node);
    node_t *name = decl ? enum_decl_node_get_name(decl) : NULL;
    check_define_type_name(sema, name, check_build_enum(sema, node), 0);
    return;
  }
  case CLUX_NODE_STMT_UNION: {
    node_t *decl = stmt_union_node_get_union_decl(node);
    node_t *name = decl ? union_node_get_name(decl) : NULL;
    if (decl && union_node_get_generics(decl)) {
      check_define_template(sema, name, node, 0);
    } else {
      check_define_type_name(sema, name, check_build_union(sema, node), 0);
    }
    return;
  }
  case CLUX_NODE_STMT_BITFIELD: {
    node_t *decl = stmt_bitfield_node_get_bitfield_decl(node);
    node_t *name = decl ? bitfield_decl_node_get_name(decl) : NULL;
    check_define_type_name(sema, name, check_build_bitfield(sema, node), 0);
    return;
  }
  case CLUX_NODE_STMT_INTERFACE: {
    node_t *decl = stmt_interface_node_get_interface_decl(node);
    node_t *name = decl ? interface_decl_node_get_name(decl) : NULL;
    check_define_type_name(sema, name, check_build_interface(sema, node), 0);
    return;
  }
  case CLUX_NODE_STMT_COMPTIME_IF:
  case CLUX_NODE_STMT_COMPTIME_FOREACH:
    return; /* comptime 块整树跳过（与 check_stmt 一致） */
  default:
    break;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    check_local_scan(sema, vec_get(children, idx));
  }
}

/* ---- §8.8 P10：DFA 确定性赋值（§8.6） ---- */

/* 局部 var 赋值状态栈：栈顶向下首个同名即内层遮蔽（与作用域链同构）。
   帧内活动（dfa_frames>0）才启用——模块级 var 不参与。 */

/* 自栈顶向下找首个同名条目（内层遮蔽）。 */
static sema_dfa_entry_t *check_dfa_find(clux_sema_t *sema, const char *name) {
  if (!name) {
    return NULL;
  }
  for (size_t idx = vec_get_size(sema->dfa_entries); idx > 0; idx--) {
    sema_dfa_entry_t *entry =
        (sema_dfa_entry_t *)vec_get(sema->dfa_entries, idx - 1);
    if (entry->name && strcmp(entry->name, name) == 0) {
      return entry;
    }
  }
  return NULL;
}

/* 注册点：只有函数体局部 STMT_VAR（形参/迭代变量/嵌套函数名天然已赋
   或不参与）。assigned 按初值 AST 形态：有初值且非 undefined 即已赋
   （运行期 TDZ 只在无初值/undefined 时出现，shadow 可演算性与之无关）。
   名字取 location 文本拷贝，所有权归条目（截断/销毁时释放）。 */
static void
check_dfa_define(clux_sema_t *sema, node_t *name_node, int assigned) {
  if (!sema->dfa_frames || !name_node) {
    return;
  }
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  char *name = check_name_of(allocator, name_node);
  if (!name) {
    return;
  }
  sema_dfa_entry_t *entry = allocator_alloc(allocator, sizeof(*entry));
  entry->name = name;
  entry->assigned = assigned;
  vec_push(sema->dfa_entries, entry);
}

/* 消费点：名字引用命中未赋条目即报（与运行期 TDZ 消息分离的静态消息，
   §8.6）。 */
static void
check_dfa_consume(clux_sema_t *sema, node_t *node, const char *name) {
  if (!sema->dfa_frames || !name) {
    return;
  }
  sema_dfa_entry_t *entry = check_dfa_find(sema, name);
  if (entry && !entry->assigned) {
    clux_context_diag(sema->ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(node),
                      "use before assignment '%s'",
                      name);
  }
}

/* 赋值标记：EXPR_ASSIGN 左操作数 name-form 时先置位（赋值目标不是
   消费点；先置位再求值左侧，查表天然不再误报）。非 name-form 目标
   （member/subscript）不标记——其 base 读取按正常消费点检查。 */
static void check_dfa_assign_target(clux_sema_t *sema, node_t *left) {
  if (!sema->dfa_frames || !check_is_name_form(left)) {
    return;
  }
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  char *name = check_name_of(allocator, left);
  sema_dfa_entry_t *entry = check_dfa_find(sema, name);
  if (entry) {
    entry->assigned = 1;
  }
  allocator_free(allocator, (void **)&name);
}

/* assigned 快照（if 合流/循环保守/短路与三元不提升/defer 隔离用）。
   调用方持 flags 数组，check_dfa_restore 恢复并释放。 */
static int *check_dfa_snapshot(clux_sema_t *sema, size_t *out_size) {
  size_t size = vec_get_size(sema->dfa_entries);
  *out_size = size;
  if (!size) {
    return NULL;
  }
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  int *flags = allocator_alloc(allocator, size * sizeof(int));
  for (size_t idx = 0; idx < size; idx++) {
    flags[idx] =
        ((sema_dfa_entry_t *)vec_get(sema->dfa_entries, idx))->assigned;
  }
  return flags;
}

static void check_dfa_restore(clux_sema_t *sema, int *flags, size_t size) {
  if (flags) {
    for (size_t idx = 0; idx < size && idx < vec_get_size(sema->dfa_entries);
         idx++) {
      ((sema_dfa_entry_t *)vec_get(sema->dfa_entries, idx))->assigned =
          flags[idx];
    }
    allocator_t *allocator = clux_context_get_allocator(sema->ctx);
    allocator_free(allocator, (void **)&flags);
  }
}

/* 水位标记：与 5 对 scope push/pop 配对（块/for/foreach/defer + 帧内
   任意嵌套），pop 截断 entries（vec 不持元素所有权，条目手工释放）。 */
static void check_dfa_mark_push(clux_sema_t *sema) {
  if (!sema->dfa_frames) {
    return;
  }
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  size_t *mark = allocator_alloc(allocator, sizeof(size_t));
  *mark = vec_get_size(sema->dfa_entries);
  vec_push(sema->dfa_marks, mark);
}

static void check_dfa_mark_pop(clux_sema_t *sema) {
  if (!sema->dfa_frames) {
    return;
  }
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  size_t size = vec_get_size(sema->dfa_marks);
  if (!size) {
    return;
  }
  size_t *mark = (size_t *)vec_get(sema->dfa_marks, size - 1);
  size_t target = *mark;
  allocator_free(allocator, (void **)&mark);
  vec_pop(sema->dfa_marks);
  size = vec_get_size(sema->dfa_entries);
  while (size > target) {
    sema_dfa_entry_t *entry =
        (sema_dfa_entry_t *)vec_get(sema->dfa_entries, size - 1);
    allocator_free(allocator, (void **)&entry->name);
    allocator_free(allocator, (void **)&entry);
    vec_pop(sema->dfa_entries);
    size--;
  }
}

/* 捕获消费检查：FUNC_CAPTURE 名字按定义点外层条目做消费点检查
   （捕获即拷贝，捕获点即消费点）。帧换栈/defer 隔离前调用。
   FUNC_PARAMS 容错形态与 check_captures 一致地展开。 */
static void check_dfa_capture_consumes(clux_sema_t *sema, node_t *captures) {
  if (!captures || node_get_kind(captures) != CLUX_NODE_FUNC_CAPTURES ||
      !sema->dfa_frames) {
    return;
  }
  vec_t *children = node_get_children(captures);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    node_t *child = vec_get(children, idx);
    if (!child) {
      continue;
    }
    if (node_get_kind(child) == CLUX_NODE_FUNC_CAPTURE) {
      node_t *name_node = func_capture_node_get_name(child);
      allocator_t *allocator = clux_context_get_allocator(sema->ctx);
      char *name = check_name_of(allocator, name_node);
      check_dfa_consume(sema, name_node, name);
      allocator_free(allocator, (void **)&name);
    } else if (node_get_kind(child) == CLUX_NODE_FUNC_PARAMS) {
      size_t count = func_params_node_get_param_count(child);
      for (size_t pidx = 0; pidx < count; pidx++) {
        node_t *param = func_params_node_get_param(child, pidx);
        node_t *name_node = func_param_node_get_name(param);
        allocator_t *allocator = clux_context_get_allocator(sema->ctx);
        char *name = check_name_of(allocator, name_node);
        check_dfa_consume(sema, name_node, name);
        allocator_free(allocator, (void **)&name);
      }
    }
  }
}

/* 函数体帧：push scope → 块级预扫描 → 捕获 → 形参（先全部登记再查
   默认值——默认值可引用任意形参）→ 返回类型 → 逐语句检查 → pop。
   STMT_FUNC 与闭包 EXPR_FUNC_DECL 共用本通道。泛型定义体不检查
   （实例体的检查随实例化通道，P8+）。P10：帧换栈——捕获名先按外层
   条目做消费点检查，再换空白帧进体（嵌套帧不查外层自由名，首版
   保守）；出帧清空 entries 还原外层栈。 */
static void check_func_decl(clux_sema_t *sema, node_t *decl) {
  if (!decl || func_decl_node_get_generics(decl)) {
    return;
  }
  clux_context_t *ctx = sema->ctx;
  check_dfa_capture_consumes(sema, func_decl_node_get_captures(decl));
  vec_t *saved_entries = sema->dfa_entries;
  sema->dfa_entries = create_vec(clux_context_get_allocator(ctx));
  sema->dfa_frames++;
  clux_context_push_scope(ctx);
  node_t *body = func_decl_node_get_body(decl);
  if (body) {
    check_local_scan(sema, body);
  }
  check_captures(sema, func_decl_node_get_captures(decl));
  node_t *params = func_decl_node_get_params(decl);
  size_t count = params ? func_params_node_get_param_count(params) : 0;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *param = func_params_node_get_param(params, idx);
    clux_type_t *type =
        check_type_eval(sema, func_param_node_get_type(param), 1);
    check_define_value_name(sema, func_param_node_get_name(param), type, 1);
  }
  for (size_t idx = 0; idx < count; idx++) {
    node_t *param = func_params_node_get_param(params, idx);
    check_expr_checked(sema, func_param_node_get_default(param));
  }
  /* P9：签名返回类型进 return_type 上下文（体内 return 匹配依据），
     出体恢复外层（嵌套函数互不串扰）。 */
  clux_type_t *saved_return = sema->return_type;
  sema->return_type = check_type_eval(sema, func_decl_node_get_return(decl), 1);
  if (body) {
    check_stmt(sema, body);
  }
  sema->return_type = saved_return;
  clux_context_pop_scope(ctx);
  /* P10：帧收尾——清空本帧 entries（vec 不持所有权，条目手工释放）
     后还原外层栈。 */
  {
    allocator_t *allocator = clux_context_get_allocator(ctx);
    size_t size = vec_get_size(sema->dfa_entries);
    while (size > 0) {
      sema_dfa_entry_t *entry =
          (sema_dfa_entry_t *)vec_get(sema->dfa_entries, size - 1);
      allocator_free(allocator, (void **)&entry->name);
      allocator_free(allocator, (void **)&entry);
      vec_pop(sema->dfa_entries);
      size--;
    }
    delete_vec(&sema->dfa_entries);
  }
  sema->dfa_entries = saved_entries;
  sema->dfa_frames--;
}

/* ---- 语句检查 ---- */

/* 值 → 绑定类型：可演算的值类型（排除占位 error/类型族——var 不可绑
   定 type value，var T = i32 的类型标注治理是 STMT_TYPE 的职责）。 */
static clux_type_t *check_value_bind_type(clux_value_t *value) {
  if (!value || check_is_opaque_value(value)) {
    return NULL;
  }
  return clux_value_get_type(value);
}

/* ---- P9：语句级检查 ---- */

/* 兼容判定（§8.4）：值与目标类型恒等或经引擎 safe_cast 吸收即兼容
   （var 初始化/赋值/实参/return 与运行期走同一通道，失败消息天然与
   exec 逐字一致）。任一侧不可演算或类型族值降级放行（P9 不硬报
   未知形态）。不兼容返回真 error（调用侧转 diag），兼容返回 NULL。 */
static clux_value_t *check_safe_cast_gap(clux_sema_t *sema,
                                         clux_value_t *value,
                                         clux_type_t *target) {
  if (!value || !target || check_is_opaque_value(value)) {
    return NULL;
  }
  clux_type_t *source = clux_value_get_type(value);
  if (clux_type_equal(source, target)) {
    return NULL;
  }
  clux_value_t *cast = clux_context_safe_cast(sema->ctx, value, target, NULL);
  return check_is_real_error(cast) ? cast : NULL;
}

/* 条件须 bool（if/while/do-while/for/assert/static assert，与 exec/stmt.c
   的运行期判定同构、消息逐字一致）。可演算非 bool 报 diag；不可演算
   降级。what 为语句名（"while"/"assert" 等）。 */
static void check_cond_bool(clux_sema_t *sema,
                            clux_value_t *cond,
                            node_t *node,
                            const char *what) {
  if (!cond || check_is_opaque_value(cond)) {
    return;
  }
  clux_type_t *type = clux_value_get_type(cond);
  if (clux_type_get_kind(type) != CLUX_TYPE_BOOL) {
    const char *type_name = clux_type_get_name(type);
    clux_context_diag(sema->ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(node),
                      "%s condition must be 'bool', got '%s'",
                      what,
                      type_name ? type_name : "<unknown>");
  }
}

static void check_stmt(clux_sema_t *sema, node_t *stmt) {
  if (!stmt) {
    return;
  }
  clux_context_t *ctx = sema->ctx;
  switch (node_get_kind(stmt)) {
  case CLUX_NODE_STMT_BLOCK: {
    clux_context_push_scope(ctx);
    check_dfa_mark_push(sema);
    size_t count = stmt_block_node_get_stmt_count(stmt);
    for (size_t idx = 0; idx < count; idx++) {
      check_stmt(sema, stmt_block_node_get_stmt(stmt, idx));
    }
    check_dfa_mark_pop(sema);
    clux_context_pop_scope(ctx);
    return;
  }
  case CLUX_NODE_STMT_VAR: {
    /* 标注优先、推断兜底（声明类型治理 var 绑定）；类型结果表锚点
       是 STMT_VAR 语句节点本体。P9：标注与初值兼有时走 safe_cast
       兼容判定（与运行期 var 初始化同一通道）。 */
    clux_type_t *type = check_type_eval(sema, stmt_var_node_get_type(stmt), 1);
    node_t *value_node = stmt_var_node_get_value(stmt);
    clux_value_t *value = check_expr_checked(sema, value_node);
    if (type && value) {
      clux_value_t *gap = check_safe_cast_gap(sema, value, type);
      if (gap) {
        check_error_diag(sema, value_node ? value_node : stmt, gap);
      }
    }
    clux_type_t *bind = type ? type : check_value_bind_type(value);
    /* §8.12 P15：有初值且非 undefined 时装饰器链式包装绑定类型（TDZ
       与 exec 同构跳过；无初值也不适用）。失败回退 last-good 防级联。 */
    if (value_node &&
        node_get_kind(value_node) != CLUX_NODE_LITERAL_UNDEFINED && bind) {
      clux_type_t *subject = bind;
      for (size_t didx = 0; didx < stmt_var_node_get_decorator_count(stmt);
           didx++) {
        subject = check_decorator(
            sema, stmt_var_node_get_decorator(stmt, didx), subject);
        if (subject) {
          bind = subject;
        }
      }
    }
    check_define_value_name(sema, stmt_var_node_get_name(stmt), bind, 1);
    sema_record_node_type(sema, stmt, bind);
    /* P10：函数体局部 var 登记赋值状态（初值可演算即已赋；`var x: T;`
       与 `= undefined` 均未赋）。 */
    check_dfa_define(sema, stmt_var_node_get_name(stmt), value != NULL);
    return;
  }
  case CLUX_NODE_STMT_IF: {
    node_t *cond_node = stmt_if_node_get_condition(stmt);
    check_cond_bool(sema, check_expr_checked(sema, cond_node), cond_node, "if");
    /* P10：两臂分别演算后合流——未赋集合取并集（两臂都赋才算赋）。
       无 else 时 else 臂等价于快照（不提升）。 */
    size_t base_size = 0;
    int *base = sema->dfa_frames ? check_dfa_snapshot(sema, &base_size) : NULL;
    check_stmt(sema, stmt_if_node_get_then(stmt));
    size_t then_size = 0;
    int *then_flags =
        sema->dfa_frames ? check_dfa_snapshot(sema, &then_size) : NULL;
    check_dfa_restore(sema, base, base_size);
    check_stmt(sema, stmt_if_node_get_else(stmt));
    if (then_flags) {
      for (size_t idx = 0;
           idx < then_size && idx < vec_get_size(sema->dfa_entries);
           idx++) {
        sema_dfa_entry_t *entry =
            (sema_dfa_entry_t *)vec_get(sema->dfa_entries, idx);
        entry->assigned = entry->assigned && then_flags[idx];
      }
      allocator_t *allocator = clux_context_get_allocator(ctx);
      allocator_free(allocator, (void **)&then_flags);
    }
    return;
  }
  case CLUX_NODE_STMT_WHILE: {
    /* P10：循环保守——出口保持循环前状态（体内赋值不提升，可能零次
       执行；cond 内赋值按循环前快照一并恢复）。 */
    size_t snap_size = 0;
    int *snap = sema->dfa_frames ? check_dfa_snapshot(sema, &snap_size) : NULL;
    node_t *cond_node = stmt_while_node_get_condition(stmt);
    check_cond_bool(
        sema, check_expr_checked(sema, cond_node), cond_node, "while");
    check_stmt(sema, stmt_while_node_get_body(stmt));
    check_dfa_restore(sema, snap, snap_size);
    return;
  }
  case CLUX_NODE_STMT_DO_WHILE: {
    check_stmt(sema, stmt_do_while_node_get_body(stmt));
    node_t *cond_node = stmt_do_while_node_get_condition(stmt);
    check_cond_bool(
        sema, check_expr_checked(sema, cond_node), cond_node, "do-while");
    return;
  }
  case CLUX_NODE_STMT_FOR: {
    clux_context_push_scope(ctx);
    check_dfa_mark_push(sema);
    node_t *init = stmt_for_node_get_init(stmt);
    if (init) {
      if (node_get_kind(init) == CLUX_NODE_STMT_VAR) {
        check_stmt(sema, init);
      } else {
        check_expr_checked(sema, init);
      }
    }
    /* P10：循环保守——init 后快照，cond/update/body 的赋值不提升。 */
    size_t snap_size = 0;
    int *snap = sema->dfa_frames ? check_dfa_snapshot(sema, &snap_size) : NULL;
    node_t *cond_node = stmt_for_node_get_condition(stmt);
    check_cond_bool(
        sema, check_expr_checked(sema, cond_node), cond_node, "for");
    check_expr_checked(sema, stmt_for_node_get_update(stmt));
    check_stmt(sema, stmt_for_node_get_body(stmt));
    check_dfa_restore(sema, snap, snap_size);
    check_dfa_mark_pop(sema);
    clux_context_pop_scope(ctx);
    return;
  }
  case CLUX_NODE_STMT_FOREACH: {
    /* iterable 在外层作用域求值（迭代变量对 iterable 不可见）。迭代
       变量绑定：标注优先、iterable 元素类型兜底（ARRAY/SLICE 取 base）；
       类型结果表锚点是迭代变量 NAME 节点。 */
    clux_value_t *iterable =
        check_expr_checked(sema, stmt_foreach_node_get_iterable(stmt));
    clux_context_push_scope(ctx);
    check_dfa_mark_push(sema);
    clux_type_t *type =
        check_type_eval(sema, stmt_foreach_node_get_type(stmt), 1);
    clux_type_t *bind = type;
    if (!bind && iterable) {
      clux_type_t *source = clux_value_get_type(iterable);
      clux_type_kind_t kind = clux_type_get_kind(source);
      if (kind == CLUX_TYPE_ARRAY) {
        bind = clux_array_get_base(source);
      } else if (kind == CLUX_TYPE_SLICE) {
        bind = clux_slice_get_base(source);
      }
    }
    node_t *name_node = stmt_foreach_node_get_name(stmt);
    check_define_value_name(sema, name_node, bind, 1);
    sema_record_node_type(sema, name_node, bind);
    /* P10：循环保守——体内赋值不提升到出口（可能零次执行）。 */
    size_t snap_size = 0;
    int *snap = sema->dfa_frames ? check_dfa_snapshot(sema, &snap_size) : NULL;
    check_stmt(sema, stmt_foreach_node_get_body(stmt));
    check_dfa_restore(sema, snap, snap_size);
    check_dfa_mark_pop(sema);
    clux_context_pop_scope(ctx);
    return;
  }
  case CLUX_NODE_STMT_COMPTIME_IF:
  case CLUX_NODE_STMT_COMPTIME_FOREACH:
    return; /* 编译期构造整树跳过（walk 已剪枝/展开，emit 通道负责） */
  case CLUX_NODE_STMT_SWITCH: {
    check_expr_checked(sema, stmt_switch_node_get_value(stmt));
    node_t *branches = stmt_switch_node_get_body(stmt);
    size_t count =
        branches ? switch_branches_node_get_branch_count(branches) : 0;
    for (size_t idx = 0; idx < count; idx++) {
      node_t *branch = switch_branches_node_get_branch(branches, idx);
      if (!branch) {
        continue;
      }
      node_t *patterns = switch_branch_node_get_patterns(branch);
      size_t pcount =
          patterns ? switch_patterns_node_get_pattern_count(patterns) : 0;
      for (size_t pidx = 0; pidx < pcount; pidx++) {
        check_expr_checked(sema,
                           switch_patterns_node_get_pattern(patterns, pidx));
      }
      check_stmt(sema, switch_branch_node_get_body(branch));
    }
    return;
  }
  case CLUX_NODE_STMT_DEFER:
    /* P10：捕获点即消费点（先按外层条目检查）；体内赋值/登记不外泄
     （快照恢复 + 水位截断）。 */
    clux_context_push_scope(ctx);
    check_dfa_capture_consumes(sema, stmt_defer_node_get_captures(stmt));
    size_t defer_snap_size = 0;
    int *defer_snap =
        sema->dfa_frames ? check_dfa_snapshot(sema, &defer_snap_size) : NULL;
    check_dfa_mark_push(sema);
    check_captures(sema, stmt_defer_node_get_captures(stmt));
    check_stmt(sema, stmt_defer_node_get_body(stmt));
    check_dfa_mark_pop(sema);
    check_dfa_restore(sema, defer_snap, defer_snap_size);
    clux_context_pop_scope(ctx);
    return;
  case CLUX_NODE_STMT_RETURN: {
    /* P9：return 值与签名返回类型走 safe_cast 兼容判定（与帧边界的
       运行期 cast 同一通道）。return_type 上下文 NULL（不在函数体/
       void/未定局）或值不可演算时放行。 */
    node_t *value_node = stmt_return_node_get_value(stmt);
    clux_value_t *value = check_expr_checked(sema, value_node);
    if (value && sema->return_type) {
      clux_value_t *gap = check_safe_cast_gap(sema, value, sema->return_type);
      if (gap) {
        check_error_diag(sema, value_node ? value_node : stmt, gap);
      }
    }
    return;
  }
  case CLUX_NODE_STMT_ASSERT: {
    node_t *cond_node = stmt_assert_node_get_condition(stmt);
    check_cond_bool(
        sema, check_expr_checked(sema, cond_node), cond_node, "assert");
    return;
  }
  case CLUX_NODE_STMT_STATIC_ASSERT: {
    node_t *cond_node = stmt_static_assert_node_get_condition(stmt);
    check_cond_bool(
        sema, check_expr_checked(sema, cond_node), cond_node, "static assert");
    return;
  }
  case CLUX_NODE_STMT_EXPR:
    check_expr_checked(sema, stmt_expr_node_get_expr(stmt));
    return;
  case CLUX_NODE_STMT_FUNC: {
    /* 嵌套函数：声明名（含签名）已由预扫描登记。§8.12 P15：装饰器
       对预扫描登记的 shadow 签名就地 retype（链式包装）；error 类型
       占位（签名构建失败）不 retype，防二次误报。随后照常进体。 */
    node_t *decl = stmt_func_node_get_func_decl(stmt);
    if (decl && !func_decl_node_get_generics(decl)) {
      if (stmt_func_node_get_decorator_count(stmt) > 0) {
        allocator_t *allocator = clux_context_get_allocator(ctx);
        char *name = check_name_of(allocator, func_decl_node_get_name(decl));
        clux_value_t *entry =
            name ? clux_scope_get(clux_context_get_scope(ctx), name) : NULL;
        if (entry && clux_value_is_shadow(entry) &&
            clux_type_get_kind(clux_value_get_type(entry)) != CLUX_TYPE_ERROR) {
          clux_type_t *subject = clux_value_get_type(entry);
          for (size_t didx = 0; didx < stmt_func_node_get_decorator_count(stmt);
               didx++) {
            clux_type_t *wrapped = check_decorator(
                sema, stmt_func_node_get_decorator(stmt, didx), subject);
            if (wrapped) {
              subject = wrapped;
              clux_value_retype(entry, subject);
            }
          }
        }
        allocator_free(allocator, (void **)&name);
      }
      check_func_decl(sema, decl);
    }
    return;
  }
  case CLUX_NODE_STMT_STRUCT:
  case CLUX_NODE_STMT_ENUM:
  case CLUX_NODE_STMT_UNION:
  case CLUX_NODE_STMT_BITFIELD:
  case CLUX_NODE_STMT_INTERFACE: {
    /* §8.12 P15：局部类型族语句——顶层装饰器 eval-only（exec 同构丢弃
       类型改写，仅求值表达式）；类型名与成员已由预扫描登记，这里不
       重复 define。 */
    switch (node_get_kind(stmt)) {
    case CLUX_NODE_STMT_STRUCT:
      for (size_t didx = 0; didx < stmt_struct_node_get_decorator_count(stmt);
           didx++) {
        check_decorator(sema, stmt_struct_node_get_decorator(stmt, didx), NULL);
      }
      check_struct_attr_decorators(sema, stmt);
      break;
    case CLUX_NODE_STMT_ENUM:
      for (size_t didx = 0; didx < stmt_enum_node_get_decorator_count(stmt);
           didx++) {
        check_decorator(sema, stmt_enum_node_get_decorator(stmt, didx), NULL);
      }
      break;
    case CLUX_NODE_STMT_UNION:
      for (size_t didx = 0; didx < stmt_union_node_get_decorator_count(stmt);
           didx++) {
        check_decorator(sema, stmt_union_node_get_decorator(stmt, didx), NULL);
      }
      break;
    case CLUX_NODE_STMT_BITFIELD:
      for (size_t didx = 0; didx < stmt_bitfield_node_get_decorator_count(stmt);
           didx++) {
        check_decorator(
            sema, stmt_bitfield_node_get_decorator(stmt, didx), NULL);
      }
      break;
    default: /* STMT_INTERFACE */
      for (size_t didx = 0;
           didx < stmt_interface_node_get_decorator_count(stmt);
           didx++) {
        check_decorator(
            sema, stmt_interface_node_get_decorator(stmt, didx), NULL);
      }
      break;
    }
    return;
  }
  default:
    return; /* 声明类已由预扫描处理；import/extern/test/…跳过 */
  }
}

/* ---- 表达式检查（P8：shadow 演算） ---- */

/* 泛型使用点判定：EXPR_SUBSCRIPT 的 base 是泛型定义名。args 是类型
   实参，整树不按值引用检查。 */
static int check_is_generic_use(clux_sema_t *sema, node_t *node) {
  node_t *base = subscript_node_get_base(node);
  if (!check_is_name_form(base)) {
    return 0;
  }
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  char *name = check_name_of(allocator, base);
  if (!name) {
    return 0;
  }
  int hit = 0;
  for (size_t idx = 0; idx < vec_get_size(sema->generic_defs); idx++) {
    sema_generic_def_t *def =
        (sema_generic_def_t *)vec_get(sema->generic_defs, idx);
    if (def->name && strcmp(def->name, name) == 0) {
      hit = 1;
      break;
    }
  }
  allocator_free(allocator, (void **)&name);
  return hit;
}

/* 名字引用 → 值（借用）：未声明的名字报 diag（P7 通道不变）后降级。 */
static clux_value_t *check_lookup_value(clux_sema_t *sema, node_t *node) {
  if (!check_is_name_form(node)) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = check_name_of(allocator, node);
  clux_value_t *found =
      name ? clux_scope_get(clux_context_get_scope(ctx), name) : NULL;
  /* P10：DFA 消费点——名字引用命中未赋条目即报（§8.6）。 */
  check_dfa_consume(sema, node, name);
  if (name && !found) {
    clux_context_diag(ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(node),
                      "undefined name '%s'",
                      name);
  }
  allocator_free(allocator, (void **)&name);
  return found;
}

/* 前缀一元：操作数 error 自传播；类型族/不可知操作数降级；其余交给
   引擎 prefix 槽位（shadow 演算，不支持报 error 值）。 */
static clux_value_t *
check_prefix_value(clux_sema_t *sema, node_t *operand, clux_prefix_op_t op) {
  clux_value_t *value = check_expr(sema, operand);
  if (check_is_real_error(value) || !value) {
    return value;
  }
  if (check_is_opaque_value(value)) {
    return NULL;
  }
  return clux_context_prefix(sema->ctx, op, value, node_get_location(operand));
}

/* 槽位二元：类型族操作数（type/模板/占位）不可演算降级；其余交给引擎
   binary 槽位（shadow 演算，槽位缺失报 "does not support operator"）。 */
static clux_value_t *check_binary_value(clux_sema_t *sema,
                                        clux_value_t *left,
                                        clux_value_t *right,
                                        clux_binary_op_t op,
                                        node_t *node) {
  if (!left || !right) {
    return NULL;
  }
  if (check_is_opaque_value(left) || check_is_opaque_value(right)) {
    return NULL;
  }
  return clux_context_binary(
      sema->ctx, op, left, right, node_get_location(node));
}

/* 逻辑 &&/||（无引擎槽位）：两支须 bool——可演算类型非 bool 报
   "does not support operator"（与引擎槽位缺失消息同构）；任一支不可
   知降级。 */
static clux_value_t *check_logic_value(clux_sema_t *sema,
                                       clux_value_t *left,
                                       clux_value_t *right,
                                       const char *op,
                                       node_t *node) {
  if (!left || !right) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  clux_type_t *ltype = clux_value_get_type(left);
  clux_type_t *rtype = clux_value_get_type(right);
  int lknown = !check_is_opaque_value(left);
  int rknown = !check_is_opaque_value(right);
  clux_type_kind_t lk = lknown ? clux_type_get_kind(ltype) : CLUX_TYPE_ERROR;
  clux_type_kind_t rk = rknown ? clux_type_get_kind(rtype) : CLUX_TYPE_ERROR;
  if (lknown && lk != CLUX_TYPE_BOOL) {
    const char *name = clux_type_get_name(ltype);
    clux_context_diag(ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(node),
                      "type '%s' does not support operator '%s'",
                      name ? name : "<unknown>",
                      op);
    return NULL;
  }
  if (rknown && rk != CLUX_TYPE_BOOL) {
    const char *name = clux_type_get_name(rtype);
    clux_context_diag(ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(node),
                      "type '%s' does not support operator '%s'",
                      name ? name : "<unknown>",
                      op);
    return NULL;
  }
  if (!lknown || !rknown) {
    return NULL;
  }
  return clux_context_create_shadow_value(
      ctx, clux_context_builtin_type(ctx, CLUX_TYPE_BOOL));
}

/* "." 成员访问：LEFT 值经引擎 field 槽位（shadow 产出成员类型 shadow）；
   RIGHT 恒为成员名叶子。未定型 LEFT 降级。 */
static clux_value_t *
check_field_value(clux_sema_t *sema, clux_value_t *left, node_t *right) {
  if (!left || !check_is_name_form(right)) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *member = check_name_of(allocator, right);
  if (!member) {
    return NULL;
  }
  clux_value_t *result =
      clux_context_field(ctx, left, member, node_get_location(right));
  allocator_free(allocator, (void **)&member);
  return result;
}

/* "::" 命名空间访问：LEFT 须 type value（raw 解引用取被描述类型），
   经 clux_context_get_member 读 attribute/variant/subtype。 */
static clux_value_t *
check_member_value(clux_sema_t *sema, clux_value_t *left, node_t *right) {
  if (!left ||
      clux_type_get_kind(clux_value_get_type(left)) != CLUX_TYPE_TYPE) {
    return NULL;
  }
  if (!check_is_name_form(right)) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *member = check_name_of(allocator, right);
  if (!member) {
    return NULL;
  }
  clux_type_t *described = *(clux_type_t **)clux_value_get_raw(left, 0);
  clux_value_t *result =
      described ? clux_context_get_member(
                      ctx, described, member, node_get_location(right))
                : NULL;
  allocator_free(allocator, (void **)&member);
  return result;
}

/* as 转换：目标类型按类型位置求值后经引擎 cast 槽位。 */
static clux_value_t *check_cast_value(clux_sema_t *sema,
                                      clux_value_t *value,
                                      node_t *type_node,
                                      node_t *node) {
  clux_type_t *target = check_type_eval(sema, type_node, 1);
  if (!target || !value) {
    return NULL;
  }
  return clux_context_cast(sema->ctx, value, target, node_get_location(node));
}

/* 调用点签名交验（§8.8 P8）：engine invoke 对 shadow 实参跳过 type_equal，
   类型比对由这里全量完成——个数与消息均与 func.c invoke 一致。个数错/
   类型错报 diag 后降级；校验通过产出返回类型 shadow（不执行函数体）。 */
static clux_value_t *check_call_value(clux_sema_t *sema,
                                      node_t *node,
                                      clux_value_t *callee,
                                      clux_value_t **argv,
                                      size_t argc) {
  if (!callee ||
      clux_type_get_kind(clux_value_get_type(callee)) != CLUX_TYPE_FUNC) {
    return NULL; /* 模板/类型/占位 callee：推导随实例化通道（P8+） */
  }
  clux_context_t *ctx = sema->ctx;
  clux_type_t *ftype = clux_value_get_type(callee);
  size_t pcount = clux_func_param_count(ftype);
  if (argc != pcount) {
    clux_context_diag(ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(node),
                      "function expects %zu argument%s, got %zu",
                      pcount,
                      pcount == 1 ? "" : "s",
                      argc);
    return NULL;
  }
  for (size_t idx = 0; idx < argc; idx++) {
    clux_value_t *arg = argv[idx];
    if (!arg || check_is_opaque_value(arg)) {
      continue; /* 不可演算实参不硬报 */
    }
    clux_type_t *ptype = clux_func_get_param(ftype, idx);
    clux_type_t *atype = clux_value_get_type(arg);
    if (!ptype || !clux_type_equal(atype, ptype)) {
      const char *pname = ptype ? clux_type_get_name(ptype) : NULL;
      const char *aname = clux_type_get_name(atype);
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(node),
                        "function argument %zu requires type '%s', got '%s'",
                        idx,
                        pname ? pname : "<unknown>",
                        aname ? aname : "<unknown>");
      return NULL;
    }
  }
  clux_type_t *rtype = clux_func_get_return_type(ftype);
  if (!rtype || !clux_type_is_seal(rtype)) {
    return NULL; /* void/未定局返回类型：调用结果不可用 */
  }
  return clux_context_create_shadow_value(ctx, rtype);
}

/* ---- §8.12 P15：装饰器检查通道 ---- */

/* 单装饰器静态检查（与 exec_apply_decorator 协议对齐，§9.1/§9.6）：
   - 表达式缺失：报 "decorator requires an expression"（与 exec 逐字
     一致——parser 容错产出的残缺节点 walk 通道不覆盖）。
   - 求值真 error：转 diag 后返回 NULL（后续装饰器降级 eval-only）。
   - 求值为 func 且 subject 可演算：以 subject 的 shadow 为唯一实参走
     check_call_value 签名交验（个数/实参类型，消息与运行期 invoke
     一致），通过返回包装后类型（供后续装饰器链与名字登记消费）。
   - 其余（编译期标记/模板/不可演算/subject 未知）：假定透传，原样
     返回（§9.1 标记类；类型装饰器不改写类型本体 §9.6；模板装饰器
     随实例化通道逐实例检查）。 */
static clux_type_t *
check_decorator(clux_sema_t *sema, node_t *decorator, clux_type_t *subject) {
  if (!decorator) {
    return subject;
  }
  node_t *expr = decorator_node_get_expr(decorator);
  if (!expr) {
    clux_context_diag(sema->ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(decorator),
                      "decorator requires an expression");
    return NULL;
  }
  clux_value_t *wrapper = check_expr(sema, expr);
  if (check_is_real_error(wrapper)) {
    check_error_diag(sema, expr, wrapper);
    return NULL;
  }
  if (!wrapper || check_is_opaque_value(wrapper) ||
      clux_type_kind_unwrap(clux_value_get_type(wrapper)) != CLUX_TYPE_FUNC) {
    return subject; /* 标记透传 / 模板实例通道 / 不可演算降级 */
  }
  if (!subject) {
    return subject; /* 被装饰物类型不可演算：签名交验降级 */
  }
  clux_value_t *argv[1] = {
      clux_context_create_shadow_value(sema->ctx, subject)};
  clux_value_t *result = check_call_value(sema, expr, wrapper, argv, 1);
  return result ? clux_value_get_type(result) : NULL;
}

/* 方法调用 a.b(args) 的静态推断（与 exec/call.c 的 exec_member_call 同构，
   白皮书 §3.5.1：实例调用是 T::b(a, args) 的语法糖）。检查通道：
   receiver 经 check_expr 演算；ptr 接收者 deref 作查找通道（argv[0] 仍
   用原接收者）；qualifier unwrap 后查 struct 方法名单——可演算的
   STRUCT 接收者无此方法报 diag（消息与 exec 逐字一致）；非 struct /
   不可演算形态降级 NULL（P9 不硬报未知形态）。receiver 前置为第一
   实参（值接收者产 *T shadow，指针接收者原样），self 形参经 safe_cast
   兼容判定（*T 对 *const T 协变放行），其余实参严格判等（与
   check_call_value 同规则）。 */
static clux_value_t *check_member_call_value(clux_sema_t *sema, node_t *node) {
  node_t *callee_node = call_node_get_callee(node);
  node_t *receiver_node = binary_node_get_left(callee_node);
  node_t *name_node = binary_node_get_right(callee_node);
  const location_t *op_loc =
      node_get_location(binary_node_get_symbol(callee_node));
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  clux_value_t *receiver = check_expr(sema, receiver_node);
  if (check_is_real_error(receiver)) {
    return receiver;
  }
  if (!receiver || check_is_opaque_value(receiver)) {
    return NULL; /* 接收者不可演算：整调用降级 */
  }
  if (!name_node || node_get_kind(name_node) != CLUX_NODE_LITERAL_IDENTIFIER) {
    return NULL; /* 非 name 形态方法名（P9 不硬报） */
  }
  size_t argc = call_node_get_arg_count(node);
  clux_value_t **argv =
      argc ? allocator_alloc(allocator, argc * sizeof(clux_value_t *)) : NULL;
  for (size_t idx = 0; idx < argc; idx++) {
    argv[idx] = check_expr(sema, call_node_get_arg(node, idx));
  }
  clux_value_t *arg_error = NULL;
  for (size_t idx = 0; idx < argc; idx++) {
    if (check_is_real_error(argv[idx])) {
      arg_error = argv[idx];
      break;
    }
  }
  if (arg_error) {
    allocator_free(allocator, (void **)&argv);
    return arg_error; /* 实参错误按序传播（receiver 已先行检查） */
  }
  char *name = location_get(node_get_location(name_node), allocator);
  /* 指针接收者先 deref 产 base 作查找通道（与 exec 同构：argv[0] 仍用
     原接收者，deref 只管名单查找）。 */
  clux_value_t *lookup = receiver;
  if (clux_type_get_kind(clux_value_get_type(receiver)) == CLUX_TYPE_PTR) {
    lookup = clux_context_deref(ctx, receiver, op_loc);
    if (check_is_real_error(lookup)) {
      allocator_free(allocator, (void **)&name);
      allocator_free(allocator, (void **)&argv);
      return lookup;
    }
  }
  clux_type_t *receiver_type = clux_type_unwrap(clux_value_get_type(lookup));
  if (!receiver_type) {
    receiver_type = clux_value_get_type(lookup);
  }
  clux_value_t *result = NULL;
  do {
    if (clux_type_get_kind(receiver_type) != CLUX_TYPE_STRUCT) {
      break; /* 非 struct 接收者降级（namespace/内建形态 P9 不硬报） */
    }
    if (!clux_struct_is_method(receiver_type, name)) {
      const char *type_name = clux_type_get_name(receiver_type);
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        op_loc,
                        "type '%s' has no method '%s'",
                        type_name ? type_name : "<unknown>",
                        name);
      break;
    }
    clux_value_t *method =
        clux_context_struct_attr(ctx, receiver_type, name, op_loc);
    if (check_is_real_error(method)) {
      result = method;
      break;
    }
    /* receiver 前置为一般首参：值接收者产 *T shadow，指针接收者原样。 */
    clux_value_t *first = receiver;
    if (clux_type_get_kind(clux_value_get_type(receiver)) != CLUX_TYPE_PTR) {
      first = clux_context_create_shadow_value(
          ctx, clux_context_get_ptr_type(ctx, clux_value_get_type(receiver)));
    }
    clux_type_t *ftype = clux_value_get_type(method);
    size_t pcount = clux_func_param_count(ftype);
    if (argc + 1 != pcount) {
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(node),
                        "function expects %zu argument%s, got %zu",
                        pcount,
                        pcount == 1 ? "" : "s",
                        argc + 1);
      break;
    }
    /* self 形参安全转换兼容判定（*T 对 *const T 协变放行），失败消息
       与运行期 safe_cast 一致。 */
    clux_type_t *self_param = clux_func_get_param(ftype, 0);
    if (self_param) {
      clux_value_t *gap = check_safe_cast_gap(sema, first, self_param);
      if (gap) {
        result = gap;
        break;
      }
    }
    /* 其余实参严格判等（与 check_call_value 同规则，锚定形参 idx+1）。 */
    int mism = 0;
    for (size_t idx = 0; idx < argc; idx++) {
      clux_value_t *arg = argv[idx];
      if (!arg || check_is_opaque_value(arg)) {
        continue; /* 不可演算实参不硬报 */
      }
      clux_type_t *ptype = clux_func_get_param(ftype, idx + 1);
      clux_type_t *atype = clux_value_get_type(arg);
      if (!ptype || !clux_type_equal(atype, ptype)) {
        const char *pname = ptype ? clux_type_get_name(ptype) : NULL;
        const char *aname = clux_type_get_name(atype);
        clux_context_diag(ctx,
                          CLUX_DIAG_ERROR,
                          node_get_location(node),
                          "function argument %zu requires type '%s', got '%s'",
                          idx + 1,
                          pname ? pname : "<unknown>",
                          aname ? aname : "<unknown>");
        mism = 1;
        break;
      }
    }
    if (mism) {
      break;
    }
    clux_type_t *rtype = clux_func_get_return_type(ftype);
    if (!rtype || !clux_type_is_seal(rtype)) {
      break; /* void/未定局返回类型：调用结果不可用 */
    }
    result = clux_context_create_shadow_value(ctx, rtype);
  } while (0);
  allocator_free(allocator, (void **)&name);
  allocator_free(allocator, (void **)&argv);
  return result;
}

static clux_value_t *check_expr(clux_sema_t *sema, node_t *node) {
  if (!node) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  switch (node_get_kind(node)) {
  case CLUX_NODE_SYMBOL:
  case CLUX_NODE_SOURCE_TEXT:
    return NULL; /* 运算符/源码片段叶子，不是可求值表达式 */
  case CLUX_NODE_LITERAL_IDENTIFIER:
    return check_lookup_value(sema, node);
  case CLUX_NODE_LITERAL_NUMERIC:
  case CLUX_NODE_LITERAL_STRING:
  case CLUX_NODE_LITERAL_CHARACTER:
  case CLUX_NODE_LITERAL_BOOLEAN:
    return check_literal_value(sema, node);
  case CLUX_NODE_LITERAL_UNDEFINED:
    return NULL; /* TDZ：类型由声明标注决定（§4.1） */
  case CLUX_NODE_EXPR_GROUP:
    return check_expr(sema, group_node_get_expr(node));
  case CLUX_NODE_EXPR_BINARY: {
    node_t *symbol = binary_node_get_symbol(node);
    char *op = check_name_of(allocator, symbol);
    if (!op) {
      return NULL;
    }
    node_t *left = binary_node_get_left(node);
    node_t *right = binary_node_get_right(node);
    clux_value_t *result = NULL;
    if (left == NULL) {
      /* 无左操作数：前缀一元（+ - ~ !）或类型前缀表达式
         （*T/[]T/[N]T/?T/const/volatile——后者按类型求值产 type value）。 */
      clux_prefix_op_t pop;
      if (check_prefix_op_text(op, &pop)) {
        result = check_prefix_value(sema, right, pop);
      } else {
        clux_type_t *type = check_type_eval(sema, node, 1);
        if (type) {
          result = clux_context_create_type_value(
              ctx, type, NULL, node_get_location(node));
        }
      }
    } else if (strcmp(op, ".") == 0) {
      clux_value_t *value = check_expr(sema, left);
      if (check_is_real_error(value)) {
        result = value;
      } else {
        result = check_field_value(sema, value, right);
      }
    } else if (strcmp(op, "::") == 0) {
      clux_value_t *value = check_expr(sema, left);
      if (check_is_real_error(value)) {
        result = value;
      } else {
        result = check_member_value(sema, value, right);
      }
    } else if (strcmp(op, "as") == 0) {
      clux_value_t *value = check_expr(sema, left);
      if (check_is_real_error(value)) {
        result = value;
      } else {
        result = check_cast_value(sema, value, right, node);
      }
    } else if (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
      clux_value_t *lvalue = check_expr(sema, left);
      /* P10：短路右侧可能不执行——赋值不提升（读取照常检查）。 */
      size_t rhs_size = 0;
      int *rhs_flags =
          sema->dfa_frames ? check_dfa_snapshot(sema, &rhs_size) : NULL;
      clux_value_t *rvalue = check_expr(sema, right);
      check_dfa_restore(sema, rhs_flags, rhs_size);
      if (check_is_real_error(lvalue)) {
        result = lvalue;
      } else if (check_is_real_error(rvalue)) {
        result = rvalue;
      } else {
        result = check_logic_value(sema, lvalue, rvalue, op, node);
      }
    } else {
      clux_binary_op_t bop;
      if (check_binary_op_text(op, &bop)) {
        clux_value_t *lvalue = check_expr(sema, left);
        clux_value_t *rvalue = check_expr(sema, right);
        if (check_is_real_error(lvalue)) {
          result = lvalue;
        } else if (check_is_real_error(rvalue)) {
          result = rvalue;
        } else {
          result = check_binary_value(sema, lvalue, rvalue, bop, node);
        }
      }
      /* 未知运算符：parser 容错产物，静默放行 */
    }
    allocator_free(allocator, (void **)&op);
    return result;
  }
  case CLUX_NODE_EXPR_TERNARY: {
    node_t *cond_node = ternary_node_get_condition(node);
    clux_value_t *cond = check_expr(sema, cond_node);
    /* P9：条件须 bool（与 exec/expr.c 的运行期判定同构、消息一致）。
       不可演算降级。 */
    if (cond && !check_is_real_error(cond) && !check_is_opaque_value(cond) &&
        clux_type_get_kind(clux_value_get_type(cond)) != CLUX_TYPE_BOOL) {
      const char *type_name = clux_type_get_name(clux_value_get_type(cond));
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(cond_node),
                        "type '%s' does not support operator '?:'",
                        type_name ? type_name : "<unknown>");
    }
    /* P10：两臂均可能不执行——赋值不提升（读取照常检查）。 */
    size_t then_size = 0;
    int *then_flags =
        sema->dfa_frames ? check_dfa_snapshot(sema, &then_size) : NULL;
    clux_value_t *then_value = check_expr(sema, ternary_node_get_then(node));
    check_dfa_restore(sema, then_flags, then_size);
    size_t else_size = 0;
    int *else_flags =
        sema->dfa_frames ? check_dfa_snapshot(sema, &else_size) : NULL;
    clux_value_t *else_value = check_expr(sema, ternary_node_get_else(node));
    check_dfa_restore(sema, else_flags, else_size);
    if (check_is_real_error(cond)) {
      return cond;
    }
    if (check_is_real_error(then_value)) {
      return then_value;
    }
    if (check_is_real_error(else_value)) {
      return else_value;
    }
    /* 两支类型一致时结果是该类型，否则降级（P8 不做合并）。 */
    if (then_value && else_value &&
        clux_type_equal(clux_value_get_type(then_value),
                        clux_value_get_type(else_value)) &&
        !check_is_opaque_value(then_value)) {
      return clux_context_create_shadow_value(ctx,
                                              clux_value_get_type(then_value));
    }
    return NULL;
  }
  case CLUX_NODE_EXPR_ASSIGN: {
    /* 双侧求值（左值形态错误照常传播）。P9：赋值兼容——目标可演算且
       值可演算时走 safe_cast 判定（与运行期 assign 同一通道），不兼容
       产真 error 沿表达式上抛（stmt 边界转 diag）。P10：左操作数
       name-form 时先置位再求值（赋值目标不是消费点）。 */
    check_dfa_assign_target(sema, assign_node_get_left(node));
    clux_value_t *target = check_expr(sema, assign_node_get_left(node));
    clux_value_t *value = check_expr(sema, assign_node_get_value(node));
    if (check_is_real_error(target)) {
      return target;
    }
    if (check_is_real_error(value)) {
      return value;
    }
    if (target) {
      clux_value_t *gap =
          check_safe_cast_gap(sema, value, clux_value_get_type(target));
      if (gap) {
        return gap;
      }
    }
    return value;
  }
  case CLUX_NODE_EXPR_SUBSCRIPT: {
    if (check_is_generic_use(sema, node)) {
      return NULL; /* 泛型实例使用点：args 是类型实参 */
    }
    clux_value_t *base = check_expr(sema, subscript_node_get_base(node));
    size_t count = subscript_node_get_arg_count(node);
    clux_value_t **args =
        count ? allocator_alloc(allocator, count * sizeof(clux_value_t *))
              : NULL;
    for (size_t idx = 0; idx < count; idx++) {
      args[idx] = check_expr(sema, subscript_node_get_arg(node, idx));
    }
    clux_value_t *result = NULL;
    if (check_is_real_error(base)) {
      result = base;
    } else {
      for (size_t idx = 0; idx < count; idx++) {
        if (check_is_real_error(args[idx])) {
          result = args[idx];
          break;
        }
      }
    }
    if (!result && base && count == 1 && args[0] &&
        !check_is_opaque_value(base) && !check_is_opaque_value(args[0])) {
      /* 单实参值形态：数组/切片下标（引擎 index 槽位，shadow 演算）。
         多实参/类型值 base 是实例化/类型构造形态，降级。 */
      result = clux_context_index(ctx, base, args[0], node_get_location(node));
    }
    allocator_free(allocator, (void **)&args);
    return result;
  }
  case CLUX_NODE_EXPR_SLICE: {
    clux_value_t *base = check_expr(sema, slice_node_get_base(node));
    clux_value_t *lo = check_expr(sema, slice_node_get_lo(node));
    clux_value_t *hi = check_expr(sema, slice_node_get_hi(node));
    if (check_is_real_error(base)) {
      return base;
    }
    if (check_is_real_error(lo)) {
      return lo;
    }
    if (check_is_real_error(hi)) {
      return hi;
    }
    if (!base || check_is_opaque_value(base)) {
      return NULL;
    }
    if ((lo && check_is_opaque_value(lo)) ||
        (hi && check_is_opaque_value(hi))) {
      return NULL;
    }
    return clux_context_slice(ctx, base, lo, hi, node_get_location(node));
  }
  case CLUX_NODE_EXPR_CALL: {
    node_t *callee_node = call_node_get_callee(node);
    /* 方法调用糖（obj.m(args) 的 "." callee）走专属通道：receiver 前置
       首参 + 方法名单查找（exec_member_call 同构）。 */
    if (callee_node && node_get_kind(callee_node) == CLUX_NODE_EXPR_BINARY &&
        location_is(node_get_location(binary_node_get_symbol(callee_node)),
                    ".")) {
      return check_member_call_value(sema, node);
    }
    clux_value_t *callee = check_expr(sema, callee_node);
    size_t argc = call_node_get_arg_count(node);
    clux_value_t **argv =
        argc ? allocator_alloc(allocator, argc * sizeof(clux_value_t *)) : NULL;
    for (size_t idx = 0; idx < argc; idx++) {
      argv[idx] = check_expr(sema, call_node_get_arg(node, idx));
    }
    /* 错误全收集后按序传播：callee 先于实参。 */
    clux_value_t *result = NULL;
    if (check_is_real_error(callee)) {
      result = callee;
    } else {
      for (size_t idx = 0; idx < argc; idx++) {
        if (check_is_real_error(argv[idx])) {
          result = argv[idx];
          break;
        }
      }
    }
    if (!result) {
      for (size_t idx = 0; idx < argc; idx++) {
        node_t *arg_node = call_node_get_arg(node, idx);
        if (arg_node && node_get_kind(arg_node) == CLUX_NODE_EXPR_REST) {
          result = NULL; /* 参数包实参降级（P8） */
          goto call_done;
        }
      }
      result = check_call_value(sema, node, callee, argv, argc);
    }
  call_done:
    allocator_free(allocator, (void **)&argv);
    return result;
  }
  case CLUX_NODE_EXPR_BUILTIN: {
    for (size_t idx = 0; idx < builtin_node_get_arg_count(node); idx++) {
      clux_value_t *arg = check_expr(sema, builtin_node_get_arg(node, idx));
      if (check_is_real_error(arg)) {
        return arg;
      }
    }
    return NULL; /* 内建函数的类型规则 P9+ 按名字接入 */
  }
  case CLUX_NODE_EXPR_CONSTRUCT: {
    clux_type_t *type = check_type_eval(sema, construct_node_get_type(node), 1);
    clux_value_t *first_error = NULL;
    size_t elem_count = construct_node_get_elem_count(node);
    clux_value_t *values[elem_count ? elem_count : 1];
    for (size_t idx = 0; idx < elem_count; idx++) {
      node_t *elem = construct_node_get_elem(node, idx);
      node_t *value_node =
          elem && node_get_kind(elem) == CLUX_NODE_EXPR_CONSTRUCT_FIELD
              ? construct_field_node_get_value(elem)
              : elem;
      clux_value_t *value = check_expr(sema, value_node);
      values[idx] = value;
      if (check_is_real_error(value) && !first_error) {
        first_error = value;
      }
      /* 类型结果表锚点：构造字段节点记录字段值演算类型。 */
      if (elem && node_get_kind(elem) == CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
        sema_record_node_type(sema, elem, check_value_bind_type(value));
      }
    }
    if (first_error) {
      return first_error;
    }
    /* P9：struct 构造完备性（与 exec/expr.c construct_named_struct 同构、
       消息逐字一致）——全 named 元素：未知字段/重复字段/缺字段/字段值
       safe_cast 兼容；位置元素按 mixed 报（位置构造随 exec 现状）。
       非 struct（union 族等）与未定局类型随 exec 通道，不静态硬报。 */
    if (type && clux_type_is_seal(type) &&
        clux_type_get_kind(type) == CLUX_TYPE_STRUCT) {
      const char *type_name = clux_type_get_name(type);
      type_name = type_name ? type_name : "<anonymous>";
      size_t field_count = clux_struct_field_count(type);
      unsigned char covered[field_count ? field_count : 1];
      memset(covered, 0, sizeof(covered));
      for (size_t idx = 0; idx < elem_count; idx++) {
        node_t *elem = construct_node_get_elem(node, idx);
        if (!elem) {
          continue;
        }
        if (node_get_kind(elem) != CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
          clux_context_diag(ctx,
                            CLUX_DIAG_ERROR,
                            node_get_location(elem),
                            "named and positional elements cannot be mixed "
                            "in construction");
          continue;
        }
        node_t *name_node = construct_field_node_get_name(elem);
        if (!name_node) {
          continue;
        }
        char *name = location_get(node_get_location(name_node), allocator);
        size_t midx = (size_t)-1;
        for (size_t fidx = 0; fidx < field_count; fidx++) {
          const clux_struct_field_t *field = clux_struct_get_field(type, fidx);
          if (field && field->name && strcmp(field->name, name) == 0) {
            midx = fidx;
            break;
          }
        }
        if (midx == (size_t)-1) {
          clux_context_diag(ctx,
                            CLUX_DIAG_ERROR,
                            node_get_location(elem),
                            "type '%s' has no field '%s'",
                            type_name,
                            name);
          allocator_free(allocator, (void **)&name);
          continue;
        }
        if (covered[midx]) {
          clux_context_diag(ctx,
                            CLUX_DIAG_ERROR,
                            node_get_location(elem),
                            "duplicate field '%s' in construction of '%s'",
                            name,
                            type_name);
          allocator_free(allocator, (void **)&name);
          continue;
        }
        /* 字段值兼容：与 construct_write_view 的运行期 safe_cast 同通道
           （复用外层演算结果，不二次求值防双报）。 */
        clux_value_t *gap = check_safe_cast_gap(
            sema, values[idx], clux_struct_get_field(type, midx)->type);
        if (gap) {
          node_t *value_node2 = construct_field_node_get_value(elem);
          check_error_diag(sema, value_node2 ? value_node2 : elem, gap);
        }
        allocator_free(allocator, (void **)&name);
        covered[midx] = 1;
      }
      for (size_t fidx = 0; fidx < field_count; fidx++) {
        if (!covered[fidx]) {
          clux_context_diag(ctx,
                            CLUX_DIAG_ERROR,
                            node_get_location(node),
                            "missing field '%s' in construction of '%s'",
                            clux_struct_get_field(type, fidx)->name,
                            type_name);
        }
      }
    }
    if (type && clux_type_is_seal(type)) {
      return clux_context_create_shadow_value(ctx, type);
    }
    return NULL; /* 匿名构造（TYPE 槽 NULL）与未定局类型降级 */
  }
  case CLUX_NODE_EXPR_FUNC_DECL: {
    if (func_decl_node_get_generics(node)) {
      return NULL; /* 泛型闭包定义体随实例化通道（P8+） */
    }
    check_func_decl(sema, node); /* 帧 + diag */
    /* 出帧后静默重建签名（捕获查定义点外层作用域现值）作为闭包类型。 */
    clux_type_t *signature = check_func_signature(sema, node, 0);
    return signature ? clux_context_create_shadow_value(ctx, signature) : NULL;
  }
  case CLUX_NODE_EXPR_STRUCT_DECL:
  case CLUX_NODE_EXPR_ENUM_DECL:
  case CLUX_NODE_EXPR_UNION:
  case CLUX_NODE_EXPR_BITFIELD_DECL:
  case CLUX_NODE_EXPR_INTERFACE_DECL:
  case CLUX_NODE_EXPR_FUNC_TYPE:
    /* 声明本体/函数类型在表达式位置：名字归预扫描通道；成员名/捕获名/
       形参名是声明槽，不按引用查，不产出可演算值。 */
    return NULL;
  case CLUX_NODE_EXPR_REST:
    return NULL; /* rest 形态由使用点（调用/形参表）自行降级 */
  default:
    break;
  }
  /* 未覆盖节点：沿用 P7 的名字巡检（只查引用不演算）。 */
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    clux_value_t *child = check_expr(sema, vec_get(children, idx));
    if (check_is_real_error(child)) {
      return child;
    }
  }
  return NULL;
}

/* ---- 模块级 var 不动点（前向引用） ---- */

/* 值表达式引用名收集：子树内全部无孩子 LITERAL_IDENTIFIER（保守近似——
   闭包形参/捕获等同名标识符一并计入，误判只会让该 var 多等一轮，无真
   循环仍会定型；名字拷贝归 ctx allocator，随条目释放）。 */
static void
check_collect_idents(allocator_t *allocator, node_t *node, vec_t *names) {
  if (!node) {
    return;
  }
  if (node_get_kind(node) == CLUX_NODE_LITERAL_IDENTIFIER &&
      vec_get_size(node_get_children(node)) == 0) {
    vec_push(names, check_name_of(allocator, node));
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    check_collect_idents(allocator, vec_get(children, idx), names);
  }
}

/* 模块级 var 工作条目：不动点演算的最小载体。 */
typedef struct {
  node_t *stmt; /* STMT_VAR 锚点（定位 + 类型表回填） */
  char *name;   /* var 名（ctx allocator），占位值查找键 */
  vec_t *deps;  /* char*（ctx allocator），值表达式引用名 */
  int resolved; /* 已定型（含"已尝试失败"——防重复演算重复 diag） */
} check_modvar_t;

/* ---- 入口 ---- */

/* ---- §8.9 P12：泛型约束检查 ---- */

/* 约束子树是否引用任一形参名（F-bounded 形态如 IWriter[W]：泛型
   interface 暂缓，exec 侧也不支持——检查同步跳过，随泛型 interface
   批次开放）。 */
static int check_constraint_refs_param(node_t *node, vec_t *param_names) {
  if (!node) {
    return 0;
  }
  if (node_get_kind(node) == CLUX_NODE_LITERAL_IDENTIFIER) {
    for (size_t idx = 0; idx < vec_get_size(param_names); idx++) {
      const char *param = (const char *)vec_get(param_names, idx);
      if (param && location_is(node_get_location(node), param)) {
        return 1;
      }
    }
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    if (check_constraint_refs_param(vec_get(children, idx), param_names)) {
      return 1;
    }
  }
  return 0;
}

/* 约束判定（消息与 exec generic.c check_generic_constraint 逐字对齐，
   测试对照保证两侧语义一致）：interface 约束按结构判定——实参类型提供
   全部成员（attr 表名字命中 + func 签名 type_equal）即满足；非 interface
   约束（类型模式）按派生关系判定。返回 0=满足，非 0=不满足（diag 已发）。 */
static int check_constraint_satisfied(clux_sema_t *sema,
                                      clux_type_t *actual,
                                      clux_type_t *req,
                                      const location_t *loc) {
  const char *actual_name = clux_type_get_name(actual);
  const char *req_name = clux_type_get_name(req);
  const char *arg_text = actual_name ? actual_name : "<unknown>";
  const char *req_text = req_name ? req_name : "<unknown>";
  if (clux_type_get_kind(req) == CLUX_TYPE_INTERFACE) {
    size_t count = clux_interface_member_count(req);
    for (size_t idx = 0; idx < count; idx++) {
      const clux_interface_member_t *member =
          clux_interface_get_member(req, idx);
      const clux_struct_attr_t *attr = NULL;
      if (clux_type_get_kind(actual) == CLUX_TYPE_STRUCT) {
        attr = clux_struct_find_attr(actual, member->name);
      }
      if (!attr || !attr->value) {
        clux_context_diag(sema->ctx,
                          CLUX_DIAG_ERROR,
                          loc,
                          "type '%s' does not satisfy interface constraint "
                          "'%s': missing member '%s'",
                          arg_text,
                          req_text,
                          member->name);
        return 1;
      }
      clux_type_t *attr_type = clux_value_get_type(attr->value);
      if (clux_type_get_kind(attr_type) != CLUX_TYPE_FUNC ||
          !clux_type_equal(attr_type, member->func)) {
        clux_context_diag(sema->ctx,
                          CLUX_DIAG_ERROR,
                          loc,
                          "type '%s' does not satisfy interface constraint "
                          "'%s': member '%s' signature mismatch",
                          arg_text,
                          req_text,
                          member->name);
        return 1;
      }
    }
    return 0;
  }
  if (!clux_type_extends(actual, req)) {
    clux_context_diag(sema->ctx,
                      CLUX_DIAG_ERROR,
                      loc,
                      "type '%s' does not satisfy constraint '%s'",
                      arg_text,
                      req_text);
    return 1;
  }
  return 0;
}

/* 约束检查 pass：物化体 GENERICS 槽已摘除（实例体非泛型），检查走原树
   ——sema_instance_body_t 的 stmt（泛型定义原树含约束槽）+ param_names /
   args 按 §2.3 收集序对齐。逐参判定：值参数（comptime，TYPE 槽非空）的
   约束暂缓；约束引用形参名（F-bounded）暂缓；类型参数约束求值（双方均
   check_type_eval，report=0——名字级错误由 P7 通道兜底防双报）后判定。
   约束求值失败对齐 exec 报 evaluation failed；实参类型不可求值时静默剔除
   （既有通道/物化体已覆盖该错误）。失败实例体从 tops 原地剔除
   （2026-11-26 定稿：单一约束诊断，防 body 级联二次错误）。 */
static void check_generic_constraints(clux_sema_t *sema, vec_t *tops) {
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  vec_t *excluded = create_vec(allocator);
  for (size_t b = 0; b < vec_get_size(sema->instance_bodies); b++) {
    sema_instance_body_t *body =
        (sema_instance_body_t *)vec_get(sema->instance_bodies, b);
    node_t *inst_stmt = body->inst_stmt;
    if (!inst_stmt) {
      continue;
    }
    node_t *generics = sema_stmt_generics_node(body->stmt);
    vec_t *params = generics ? node_get_children(generics) : NULL;
    size_t count = params ? vec_get_size(params) : 0;
    if (count > vec_get_size(body->args)) {
      count = vec_get_size(body->args); /* 防御：收集对齐已保证等长 */
    }
    for (size_t idx = 0; idx < count; idx++) {
      node_t *param = (node_t *)vec_get(params, idx);
      if (!param || node_get_kind(param) != CLUX_NODE_GENERIC_PARAM) {
        continue; /* rest（参数包）非逐参约束形态 */
      }
      node_t *constraint = generic_param_node_get_constraint(param);
      if (!constraint || generic_param_node_get_type(param)) {
        continue; /* 无约束 / comptime 值参数约束暂缓 */
      }
      if (check_constraint_refs_param(constraint, body->param_names)) {
        continue; /* F-bounded：随泛型 interface 批次 */
      }
      clux_type_t *req = check_type_eval(sema, constraint, 0);
      if (!req) {
        clux_context_diag(ctx,
                          CLUX_DIAG_ERROR,
                          node_get_location(constraint),
                          "generic constraint evaluation failed");
        vec_push(excluded, inst_stmt);
        break;
      }
      clux_type_t *actual =
          check_type_eval(sema, (node_t *)vec_get(body->args, idx), 0);
      if (!actual) {
        vec_push(excluded, inst_stmt);
        break; /* 实参类型不可求值：错误已由使用点/物化体通道报告 */
      }
      if (check_constraint_satisfied(
              sema, actual, req, node_get_location(constraint))) {
        vec_push(excluded, inst_stmt);
        break;
      }
    }
  }
  if (vec_get_size(excluded) > 0) {
    size_t out = 0;
    for (size_t idx = 0; idx < vec_get_size(tops); idx++) {
      node_t *stmt = (node_t *)vec_get(tops, idx);
      int drop = 0;
      for (size_t e = 0; e < vec_get_size(excluded); e++) {
        if (stmt == vec_get(excluded, e)) {
          drop = 1;
          break;
        }
      }
      if (!drop) {
        vec_set(tops, out++, stmt);
      }
    }
    vec_resize(tops, out);
  }
  delete_vec(&excluded);
}

int sema_check_pass(clux_sema_t *sema) {
  if (!sema || !sema->ast) {
    return 0;
  }
  /* current scope 须为 module scope（walk 尾部、模块摘除前）。
     §8.7 P11：检查域 = 原树顶层语句 + walk 期物化的实例体（inst_stmt，
     非泛型形态——代换/自引用折叠已固化）。物化体当普通声明走同一套
     register/函数体/方法体循环，泛型体检查即免费获得：定义体错误在
     错误实参组合的物化体上暴露，错误即 diag → emit 阻断。 */
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  vec_t *tops = create_vec(allocator);
  size_t count = program_node_get_stmt_count(sema->ast);
  for (size_t idx = 0; idx < count; idx++) {
    vec_push(tops, program_node_get_stmt(sema->ast, idx));
  }
  for (size_t idx = 0; idx < vec_get_size(sema->instance_bodies); idx++) {
    node_t *inst_stmt =
        ((sema_instance_body_t *)vec_get(sema->instance_bodies, idx))
            ->inst_stmt;
    if (inst_stmt) {
      vec_push(tops, inst_stmt);
    }
  }
  count = vec_get_size(tops);
  for (size_t idx = 0; idx < count; idx++) {
    check_module_register(sema, vec_get(tops, idx));
  }
  /* §8.9 P12 约束检查（register 后：interface/struct 类型均已入库）：
     失败实例体从 tops 剔除，后续函数体/方法体/var 循环不再遍历。 */
  check_generic_constraints(sema, tops);
  count = vec_get_size(tops);
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = vec_get(tops, idx);
    if (!stmt || node_get_kind(stmt) != CLUX_NODE_STMT_FUNC) {
      continue;
    }
    node_t *decl = stmt_func_node_get_func_decl(stmt);
    if (decl && !func_decl_node_get_generics(decl)) {
      check_func_decl(sema, decl);
    }
  }
  /* struct 方法体纳入检查通道（与模块级函数体同等待遇）：方法签名已于
     register 循环安装三表（check_struct_install_methods），这里逐个非
     泛型方法走 check_func_decl（帧 + 语句检查 + DFA）；泛型方法随实例
     化通道。方法名归三表（不裸 define），check_func_decl 只登记形参，
     不产生重复登记。 */
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = vec_get(tops, idx);
    if (!stmt || node_get_kind(stmt) != CLUX_NODE_STMT_STRUCT) {
      continue;
    }
    node_t *decl = stmt_struct_node_get_struct_decl(stmt);
    node_t *body = decl ? struct_decl_node_get_body(decl) : NULL;
    if (!body) {
      continue;
    }
    vec_t *members = node_get_children(body);
    for (size_t midx = 0; midx < vec_get_size(members); midx++) {
      node_t *member = vec_get(members, midx);
      if (!member || node_get_kind(member) != CLUX_NODE_STMT_FUNC) {
        continue;
      }
      node_t *func_decl = stmt_func_node_get_func_decl(member);
      if (func_decl && !func_decl_node_get_generics(func_decl)) {
        check_func_decl(sema, func_decl);
      }
    }
  }
  /* 模块级 var 值检查（不动点，前向引用顺序无关）：名字已由 register
     循环登记（无标注 var 为 error 类型 shadow 占位），全部模块 var 视为
     互递归绑定组——每轮演算"值表达式引用的模块 var 均已定型"者，非
     error 绑定类型即定型：经 clux_value_retype 升级占位值 + 回填类型
     结果表（§8.5——clone 渲染器按 STMT_VAR 锚点取推断类型）。DFA 天然
     no-op（模块级不建帧）。演算失败（NULL/error 绑定）也算完成（diag
     已由 checked 通道上报，标记 resolved 防后续轮次重复报）。一轮无
     进展仍有未定型，即循环依赖/相互引用——唯一不可推断场景，按语法
     错误上报（含自引用：名字在本组内未定型同样阻塞）。 */
  vec_t *vars = create_vec(allocator);
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = vec_get(tops, idx);
    if (!stmt || node_get_kind(stmt) != CLUX_NODE_STMT_VAR) {
      continue;
    }
    node_t *value_node = stmt_var_node_get_value(stmt);
    if (!value_node) {
      continue;
    }
    check_modvar_t *entry = allocator_alloc(allocator, sizeof(*entry));
    entry->stmt = stmt;
    entry->name = check_name_of(allocator, stmt_var_node_get_name(stmt));
    entry->deps = create_vec(allocator);
    check_collect_idents(allocator, value_node, entry->deps);
    entry->resolved = 0;
    vec_push(vars, entry);
  }
  for (int progress = 1; progress;) {
    progress = 0;
    for (size_t idx = 0; idx < vec_get_size(vars); idx++) {
      check_modvar_t *entry = vec_get(vars, idx);
      if (entry->resolved) {
        continue;
      }
      int blocked = 0;
      for (size_t dep = 0; dep < vec_get_size(entry->deps) && !blocked; dep++) {
        char *dep_name = vec_get(entry->deps, dep);
        for (size_t other_idx = 0; other_idx < vec_get_size(vars);
             other_idx++) {
          check_modvar_t *other = vec_get(vars, other_idx);
          if (other->name && other->resolved == 0 &&
              strcmp(other->name, dep_name) == 0) {
            blocked = 1; /* 含自身（自引用）：本组内未定型同名即阻塞 */
            break;
          }
        }
      }
      if (blocked) {
        continue;
      }
      clux_value_t *value =
          check_expr_checked(sema, stmt_var_node_get_value(entry->stmt));
      clux_type_t *bind = check_value_bind_type(value);
      if (bind && clux_type_get_kind(bind) != CLUX_TYPE_ERROR) {
        sema_record_node_type(sema, entry->stmt, bind);
        clux_value_t *placeholder =
            clux_scope_get(clux_context_get_scope(sema->ctx), entry->name);
        if (placeholder && clux_value_is_shadow(placeholder) &&
            clux_type_get_kind(clux_value_get_type(placeholder)) ==
                CLUX_TYPE_ERROR) {
          clux_value_retype(placeholder, bind);
        }
      }
      entry->resolved = 1;
      progress = 1;
    }
  }
  for (size_t idx = 0; idx < vec_get_size(vars); idx++) {
    check_modvar_t *entry = vec_get(vars, idx);
    if (!entry->resolved) {
      clux_context_diag(sema->ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(entry->stmt),
                        "circular reference in module-level var '%s'",
                        entry->name ? entry->name : "<unknown>");
    }
  }
  for (size_t idx = 0; idx < vec_get_size(vars); idx++) {
    check_modvar_t *entry = vec_get(vars, idx);
    for (size_t dep = 0; dep < vec_get_size(entry->deps); dep++) {
      char *dep_name = vec_get(entry->deps, dep);
      allocator_free(allocator, (void **)&dep_name);
    }
    delete_vec(&entry->deps);
    allocator_free(allocator, (void **)&entry->name);
    allocator_free(allocator, (void **)&entry);
  }
  delete_vec(&vars);
  delete_vec(&tops); /* 只借原树/物化体指针，释放外壳 */
  return 0;
}
