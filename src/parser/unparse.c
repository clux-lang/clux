/* AST → 源码文本序列化器（unparser）。
   设计要点：
   - 词法底座：无子节点的叶子节点（SYMBOL / 六种字面量 / SOURCE_TEXT /
     STMT_EMPTY）文本即 location 区间 [begin.offset, end.offset)，
     直接 string_append 截取，无需任何运算符映射表。
   - EXPR_BINARY 复用三形态：LEFT 缺失为前缀（一元运算符与 *T / ?T /
     []T / const / volatile 类型前缀），RIGHT 缺失为后缀（.&、.*、.!），
     两侧齐全为普通二元；数组类型 [N]T 靠 SYMBOL 区间以 '[' 开头判别
     （区间已含 [长度]，LEFT 不再输出）。
   - EXPR_GROUP 输出括号，保证 (a + b) * c 语义不变，免优先级重排。
   - 语句由调用方缩进：up_stmt 输出语句本体（块类结构自行换行，
     收尾 '}' 不带换行），up_stmt_line 负责缩进与行尾。
   - 输出全部走 string_t API（几何扩容），O(n) 拼接。 */
#include "parser/unparse.h"

#include "core/location.h"
#include "core/position.h"
#include "parser/node.h"
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
#include "parser/node_func_type.h"
#include "parser/node_generic_param.h"
#include "parser/node_group.h"
#include "parser/node_implements.h"
#include "parser/node_interface_decl.h"
#include "parser/node_literal.h"
#include "parser/node_program.h"
#include "parser/node_rest.h"
#include "parser/node_slice.h"
#include "parser/node_source_text.h"
#include "parser/node_stmt_assert.h"
#include "parser/node_stmt_bitfield.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_defer.h"
#include "parser/node_stmt_do_while.h"
#include "parser/node_stmt_empty.h"
#include "parser/node_stmt_enum.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_extern_cfunc.h"
#include "parser/node_stmt_extern_lib.h"
#include "parser/node_stmt_extern_var.h"
#include "parser/node_stmt_for.h"
#include "parser/node_stmt_foreach.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_if.h"
#include "parser/node_stmt_import.h"
#include "parser/node_stmt_interface.h"
#include "parser/node_stmt_jump.h"
#include "parser/node_stmt_reexport.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_static_assert.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_switch.h"
#include "parser/node_stmt_test.h"
#include "parser/node_stmt_type.h"
#include "parser/node_stmt_union.h"
#include "parser/node_stmt_var.h"
#include "parser/node_stmt_while.h"
#include "parser/node_struct_decl.h"
#include "parser/node_struct_member.h"
#include "parser/node_subscript.h"
#include "parser/node_switch_branch.h"
#include "parser/node_symbol.h"
#include "parser/node_ternary.h"
#include "parser/node_tuple.h"
#include "parser/node_union.h"

#include <string.h>

typedef struct {
  string_t *out;
  allocator_t *allocator;
  const unparse_options_t *options;
  size_t depth;
} unparse_ctx_t;

/* ---- 底层追加 ---- */

static void up_cstr(unparse_ctx_t *ctx, const char *text) {
  string_append_cstr(ctx->out, text);
}

static void up_char(unparse_ctx_t *ctx, char c) {
  string_append_char(ctx->out, c);
}

static void up_indent(unparse_ctx_t *ctx) {
  for (size_t i = 0; i < ctx->depth; i++) {
    up_cstr(ctx, "  ");
  }
}

/* location 区间文本：叶子节点的词法底座。区间非法时输出空。 */
static void up_loc_text(unparse_ctx_t *ctx, node_t *node) {
  if (!node) {
    return;
  }
  const location_t *loc = node_get_location(node);
  if (!loc || !loc->begin.offset || !loc->end.offset ||
      loc->end.offset < loc->begin.offset) {
    return;
  }
  string_append(ctx->out,
                loc->begin.offset,
                (size_t)(loc->end.offset - loc->begin.offset));
}

/* 借用符号节点区间文本与期望串做全等比较。 */
static bool up_sym_is(node_t *symbol, const char *expect) {
  if (!symbol) {
    return false;
  }
  const location_t *loc = node_get_location(symbol);
  if (!loc || !loc->begin.offset || !loc->end.offset ||
      loc->end.offset < loc->begin.offset) {
    return false;
  }
  size_t len = (size_t)(loc->end.offset - loc->begin.offset);
  if (strlen(expect) != len) {
    return false;
  }
  return memcmp(loc->begin.offset, expect, len) == 0;
}

static char up_sym_first(node_t *symbol) {
  const location_t *loc = symbol ? node_get_location(symbol) : NULL;
  if (!loc || !loc->begin.offset || loc->end.offset < loc->begin.offset) {
    return '\0';
  }
  return *loc->begin.offset;
}

/* ---- 前向声明 ---- */

static void up_expr(unparse_ctx_t *ctx, node_t *node);
static void up_stmt(unparse_ctx_t *ctx, node_t *node);
static void up_stmt_line(unparse_ctx_t *ctx, node_t *node);
static void up_union_decl(unparse_ctx_t *ctx, node_t *node);
static void up_enum_decl(unparse_ctx_t *ctx, node_t *node);
static void up_struct_decl(unparse_ctx_t *ctx, node_t *node);
static void up_interface_decl(unparse_ctx_t *ctx, node_t *node);
static void up_bitfield_decl(unparse_ctx_t *ctx, node_t *node);

/* ---- 复用片段 ---- */

static void up_block(unparse_ctx_t *ctx, node_t *block) {
  if (!block) {
    up_cstr(ctx, "{}");
    return;
  }
  if (node_get_kind(block) != CLUX_NODE_STMT_BLOCK) {
    /* 容错：体槽不是块时按单语句体输出。 */
    up_cstr(ctx, "{\n");
    ctx->depth++;
    up_stmt_line(ctx, block);
    ctx->depth--;
    up_indent(ctx);
    up_char(ctx, '}');
    return;
  }
  up_cstr(ctx, "{\n");
  ctx->depth++;
  size_t count = stmt_block_node_get_stmt_count(block);
  for (size_t i = 0; i < count; i++) {
    up_stmt_line(ctx, stmt_block_node_get_stmt(block, i));
  }
  ctx->depth--;
  up_indent(ctx);
  up_char(ctx, '}');
}

/* 装饰器先于语句输出，每个独占一行；调用方已输出本行缩进，
   因此每个装饰器行之后补一次同层缩进，让声明回到行首列。 */
static void up_decorators(unparse_ctx_t *ctx,
                          node_t *first_decorator,
                          size_t count,
                          node_t *(*get)(node_t *, size_t)) {
  for (size_t i = 0; i < count; i++) {
    node_t *dec = get(first_decorator, i);
    if (!dec) {
      continue;
    }
    up_cstr(ctx, "[[");
    node_t *expr = decorator_node_get_expr(dec);
    if (expr) {
      up_char(ctx, ' ');
      up_expr(ctx, expr);
      up_char(ctx, ' ');
    }
    up_cstr(ctx, "]]\n");
    up_indent(ctx);
  }
}

/* ---- 表达式 ---- */

static void up_expr(unparse_ctx_t *ctx, node_t *node) {
  if (!node) {
    return;
  }
  switch (node_get_kind(node)) {
  case CLUX_NODE_SYMBOL:
  case CLUX_NODE_LITERAL_UNDEFINED:
  case CLUX_NODE_LITERAL_STRING:
  case CLUX_NODE_LITERAL_CHARACTER:
  case CLUX_NODE_LITERAL_NUMERIC:
  case CLUX_NODE_LITERAL_BOOLEAN:
  case CLUX_NODE_LITERAL_IDENTIFIER:
  case CLUX_NODE_SOURCE_TEXT:
    up_loc_text(ctx, node);
    return;

  case CLUX_NODE_EXPR_ASSIGN: {
    up_expr(ctx, assign_node_get_left(node));
    node_t *sym = assign_node_get_symbol(node);
    if (sym) {
      up_char(ctx, ' ');
      up_loc_text(ctx, sym);
      up_char(ctx, ' ');
    }
    up_expr(ctx, assign_node_get_value(node));
    return;
  }

  case CLUX_NODE_EXPR_TERNARY:
    up_expr(ctx, ternary_node_get_condition(node));
    up_cstr(ctx, " ? ");
    up_expr(ctx, ternary_node_get_then(node));
    up_cstr(ctx, " : ");
    up_expr(ctx, ternary_node_get_else(node));
    return;

  case CLUX_NODE_EXPR_BINARY: {
    node_t *left = binary_node_get_left(node);
    node_t *sym = binary_node_get_symbol(node);
    node_t *right = binary_node_get_right(node);
    char head = up_sym_first(sym);
    if (!left) {
      /* 前缀：一元运算符与 *T / ?T / []T / const / volatile。
           qualifier 与操作数之间需要空格，其余紧贴。 */
      up_loc_text(ctx, sym);
      if (up_sym_is(sym, "const") || up_sym_is(sym, "volatile")) {
        up_char(ctx, ' ');
      }
      up_expr(ctx, right);
      return;
    }
    if (!right || head == '.') {
      /* 后缀（.&、.*、.!）与成员访问（a.b）都紧贴。 */
      up_expr(ctx, left);
      up_loc_text(ctx, sym);
      up_expr(ctx, right);
      return;
    }
    if (head == '[') {
      /* 数组类型 [N]T：SYMBOL 区间已含 '[长度]'，LEFT 冗余不输出。 */
      up_loc_text(ctx, sym);
      up_expr(ctx, right);
      return;
    }
    up_expr(ctx, left);
    up_char(ctx, ' ');
    up_loc_text(ctx, sym);
    up_char(ctx, ' ');
    up_expr(ctx, right);
    return;
  }

  case CLUX_NODE_EXPR_SUBSCRIPT: {
    up_expr(ctx, subscript_node_get_base(node));
    up_char(ctx, '[');
    size_t count = subscript_node_get_arg_count(node);
    for (size_t i = 0; i < count; i++) {
      if (i > 0) {
        up_cstr(ctx, ", ");
      }
      up_expr(ctx, subscript_node_get_arg(node, i));
    }
    up_char(ctx, ']');
    return;
  }

  case CLUX_NODE_EXPR_SLICE: {
    up_expr(ctx, slice_node_get_base(node));
    up_char(ctx, '[');
    up_expr(ctx, slice_node_get_lo(node));
    up_char(ctx, ':');
    up_expr(ctx, slice_node_get_hi(node));
    up_char(ctx, ']');
    return;
  }

  case CLUX_NODE_EXPR_REST:
    up_cstr(ctx, "...");
    up_expr(ctx, rest_node_get_expr(node));
    return;

  case CLUX_NODE_EXPR_CALL: {
    up_expr(ctx, call_node_get_callee(node));
    up_char(ctx, '(');
    size_t count = call_node_get_arg_count(node);
    for (size_t i = 0; i < count; i++) {
      if (i > 0) {
        up_cstr(ctx, ", ");
      }
      up_expr(ctx, call_node_get_arg(node, i));
    }
    up_char(ctx, ')');
    return;
  }

  case CLUX_NODE_EXPR_BUILTIN: {
    /* NAME 槽是去掉 '#' 的内建名，需要补回。 */
    up_char(ctx, '#');
    up_loc_text(ctx, builtin_node_get_name(node));
    up_char(ctx, '(');
    size_t count = builtin_node_get_arg_count(node);
    for (size_t i = 0; i < count; i++) {
      if (i > 0) {
        up_cstr(ctx, ", ");
      }
      up_expr(ctx, builtin_node_get_arg(node, i));
    }
    up_char(ctx, ')');
    return;
  }

  case CLUX_NODE_EXPR_CONSTRUCT: {
    up_char(ctx, '.');
    up_expr(ctx, construct_node_get_type(node));
    up_cstr(ctx, "{");
    size_t count = construct_node_get_elem_count(node);
    for (size_t i = 0; i < count; i++) {
      if (i > 0) {
        up_cstr(ctx, ", ");
      }
      up_expr(ctx, construct_node_get_elem(node, i));
    }
    up_char(ctx, '}');
    return;
  }

  case CLUX_NODE_EXPR_CONSTRUCT_FIELD:
    up_expr(ctx, construct_field_node_get_name(node));
    up_cstr(ctx, " = ");
    up_expr(ctx, construct_field_node_get_value(node));
    return;

  case CLUX_NODE_EXPR_GROUP:
    up_char(ctx, '(');
    up_expr(ctx, node_get_child(node, 0));
    up_char(ctx, ')');
    return;

  case CLUX_NODE_EXPR_TUPLE: {
    up_char(ctx, '<');
    size_t count = tuple_node_get_elem_count(node);
    for (size_t i = 0; i < count; i++) {
      if (i > 0) {
        up_cstr(ctx, ", ");
      }
      up_expr(ctx, tuple_node_get_elem(node, i));
    }
    up_char(ctx, '>');
    return;
  }

  case CLUX_NODE_EXPR_FUNC_TYPE:
    up_cstr(ctx, "func ");
    if (func_type_node_get_captures(node)) {
      up_expr(ctx, func_type_node_get_captures(node));
      up_char(ctx, ' ');
    }
    up_expr(ctx, func_type_node_get_params(node));
    if (func_type_node_get_return(node)) {
      up_cstr(ctx, " -> ");
      up_expr(ctx, func_type_node_get_return(node));
    }
    return;

  case CLUX_NODE_FUNC_CAPTURES: {
    up_char(ctx, '|');
    size_t count = func_captures_node_get_capture_count(node);
    for (size_t i = 0; i < count; i++) {
      if (i > 0) {
        up_cstr(ctx, ", ");
      }
      node_t *item = func_captures_node_get_capture(node, i);
      if (item && node_get_kind(item) == CLUX_NODE_FUNC_PARAMS) {
        /* 函数声明形态：捕获列表内嵌形参列表，输出不带外层括号。 */
        size_t pcount = func_params_node_get_param_count(item);
        for (size_t j = 0; j < pcount; j++) {
          if (j > 0) {
            up_cstr(ctx, ", ");
          }
          up_expr(ctx, func_params_node_get_param(item, j));
        }
      } else {
        up_expr(ctx, item);
      }
    }
    up_char(ctx, '|');
    return;
  }

  case CLUX_NODE_FUNC_CAPTURE:
    up_expr(ctx, func_capture_node_get_name(node));
    if (func_capture_node_get_type(node)) {
      up_cstr(ctx, ": ");
      up_expr(ctx, func_capture_node_get_type(node));
    }
    return;

  case CLUX_NODE_FUNC_PARAMS: {
    up_char(ctx, '(');
    size_t count = func_params_node_get_param_count(node);
    for (size_t i = 0; i < count; i++) {
      if (i > 0) {
        up_cstr(ctx, ", ");
      }
      up_expr(ctx, func_params_node_get_param(node, i));
    }
    up_char(ctx, ')');
    return;
  }

  case CLUX_NODE_FUNC_PARAM:
    if (func_param_node_get_name(node)) {
      up_expr(ctx, func_param_node_get_name(node));
      if (func_param_node_get_type(node)) {
        up_cstr(ctx, ": ");
      }
    }
    up_expr(ctx, func_param_node_get_type(node));
    if (func_param_node_get_default(node)) {
      up_cstr(ctx, " = ");
      up_expr(ctx, func_param_node_get_default(node));
    }
    return;

  case CLUX_NODE_EXPR_FUNC_DECL:
    /* 声明形态：func |captures|? name? generics? (params): ret body|; */
    up_loc_text(ctx, func_decl_node_get_keyword(node));
    up_char(ctx, ' ');
    if (func_decl_node_get_captures(node)) {
      up_expr(ctx, func_decl_node_get_captures(node));
      up_char(ctx, ' ');
    }
    /* name 紧贴 [generics] 与 (params)，中间不加空格。 */
    if (func_decl_node_get_name(node)) {
      up_expr(ctx, func_decl_node_get_name(node));
    }
    if (func_decl_node_get_generics(node)) {
      up_expr(ctx, func_decl_node_get_generics(node));
    }
    up_expr(ctx, func_decl_node_get_params(node));
    if (func_decl_node_get_return(node)) {
      up_cstr(ctx, ": ");
      up_expr(ctx, func_decl_node_get_return(node));
    }
    if (func_decl_node_get_body(node)) {
      up_char(ctx, ' ');
      up_block(ctx, func_decl_node_get_body(node));
    } else {
      up_char(ctx, ';');
    }
    return;

  case CLUX_NODE_GENERIC_PARAMS: {
    up_char(ctx, '[');
    size_t count = generic_params_node_get_param_count(node);
    for (size_t i = 0; i < count; i++) {
      if (i > 0) {
        up_cstr(ctx, ", ");
      }
      up_expr(ctx, generic_params_node_get_param(node, i));
    }
    up_char(ctx, ']');
    return;
  }

  case CLUX_NODE_GENERIC_PARAM:
    up_expr(ctx, generic_param_node_get_name(node));
    if (generic_param_node_get_type(node)) {
      up_cstr(ctx, ": ");
      up_expr(ctx, generic_param_node_get_type(node));
    }
    if (generic_param_node_get_constraint(node)) {
      up_cstr(ctx, " extends ");
      up_expr(ctx, generic_param_node_get_constraint(node));
    }
    if (generic_param_node_get_default(node)) {
      up_cstr(ctx, " = ");
      up_expr(ctx, generic_param_node_get_default(node));
    }
    return;

  case CLUX_NODE_IMPLEMENTS: {
    up_cstr(ctx, "implements ");
    size_t count = implements_node_get_type_count(node);
    for (size_t i = 0; i < count; i++) {
      if (i > 0) {
        up_cstr(ctx, " & ");
      }
      up_expr(ctx, implements_node_get_type(node, i));
    }
    return;
  }

  case CLUX_NODE_DECORATOR:
    up_cstr(ctx, "[[");
    node_t *expr = decorator_node_get_expr(node);
    if (expr) {
      up_char(ctx, ' ');
      up_expr(ctx, expr);
      up_char(ctx, ' ');
    }
    up_cstr(ctx, "]]");
    return;

  case CLUX_NODE_EXPR_UNION:
    up_union_decl(ctx, node);
    return;
  case CLUX_NODE_EXPR_ENUM_DECL:
    up_enum_decl(ctx, node);
    return;
  case CLUX_NODE_EXPR_STRUCT_DECL:
    up_struct_decl(ctx, node);
    return;
  case CLUX_NODE_EXPR_INTERFACE_DECL:
    up_interface_decl(ctx, node);
    return;
  case CLUX_NODE_EXPR_BITFIELD_DECL:
    up_bitfield_decl(ctx, node);
    return;

  default:
    up_stmt(ctx, node);
    return;
  }
}

/* ---- 声明体（成员列表统一样式：'{\n' + 每成员一行 + 缩进 '}') ---- */

static void up_field(unparse_ctx_t *ctx, node_t *field) {
  up_decorators(ctx,
                field,
                field_node_get_decorator_count(field),
                field_node_get_decorator);
  up_expr(ctx, field_node_get_name(field));
  if (field_node_get_type(field)) {
    up_cstr(ctx, ": ");
    up_expr(ctx, field_node_get_type(field));
  }
  up_char(ctx, ';');
}

static void up_fields(unparse_ctx_t *ctx, node_t *fields) {
  if (!fields) {
    up_cstr(ctx, " {}");
    return;
  }
  up_cstr(ctx, " {\n");
  ctx->depth++;
  size_t count = fields_node_get_field_count(fields);
  for (size_t i = 0; i < count; i++) {
    up_indent(ctx);
    up_field(ctx, fields_node_get_field(fields, i));
    up_char(ctx, '\n');
  }
  ctx->depth--;
  up_indent(ctx);
  up_char(ctx, '}');
}

static void up_struct_members(unparse_ctx_t *ctx, node_t *members) {
  if (!members) {
    up_cstr(ctx, " {}");
    return;
  }
  up_cstr(ctx, " {\n");
  ctx->depth++;
  size_t count = struct_members_node_get_member_count(members);
  for (size_t i = 0; i < count; i++) {
    node_t *member = struct_members_node_get_member(members, i);
    if (node_get_kind(member) == CLUX_NODE_STRUCT_EMBED) {
      up_indent(ctx);
      up_cstr(ctx, "...");
      up_expr(ctx, struct_embed_node_get_type(member));
      up_cstr(ctx, ";\n");
    } else if (node_get_kind(member) == CLUX_NODE_FIELD) {
      up_indent(ctx);
      up_field(ctx, member);
      up_char(ctx, '\n');
    } else {
      up_stmt_line(ctx, member);
    }
  }
  ctx->depth--;
  up_indent(ctx);
  up_char(ctx, '}');
}

static void up_enum_variants(unparse_ctx_t *ctx, node_t *variants) {
  if (!variants) {
    up_cstr(ctx, " {}");
    return;
  }
  up_cstr(ctx, " {\n");
  ctx->depth++;
  size_t count = enum_variants_node_get_variant_count(variants);
  for (size_t i = 0; i < count; i++) {
    node_t *variant = enum_variants_node_get_variant(variants, i);
    up_indent(ctx);
    up_expr(ctx, enum_variant_node_get_name(variant));
    if (enum_variant_node_get_value(variant)) {
      up_cstr(ctx, " = ");
      up_expr(ctx, enum_variant_node_get_value(variant));
    }
    if (i + 1 < count) {
      up_char(ctx, ',');
    }
    up_char(ctx, '\n');
  }
  ctx->depth--;
  up_indent(ctx);
  up_char(ctx, '}');
}

static void up_bitfield_fields(unparse_ctx_t *ctx, node_t *fields) {
  if (!fields) {
    up_cstr(ctx, " {}");
    return;
  }
  up_cstr(ctx, " {\n");
  ctx->depth++;
  size_t count = bitfield_fields_node_get_field_count(fields);
  for (size_t i = 0; i < count; i++) {
    node_t *field = bitfield_fields_node_get_field(fields, i);
    up_indent(ctx);
    up_expr(ctx, bitfield_field_node_get_name(field));
    if (bitfield_field_node_get_width(field)) {
      up_cstr(ctx, ": ");
      up_expr(ctx, bitfield_field_node_get_width(field));
    }
    up_cstr(ctx, ";\n");
  }
  ctx->depth--;
  up_indent(ctx);
  up_char(ctx, '}');
}

static void up_interface_members(unparse_ctx_t *ctx, node_t *members) {
  if (!members) {
    up_cstr(ctx, " {}");
    return;
  }
  up_cstr(ctx, " {\n");
  ctx->depth++;
  size_t count = interface_members_node_get_member_count(members);
  for (size_t i = 0; i < count; i++) {
    up_indent(ctx);
    /* 成员是无函数体的 FUNC_DECL，up_expr 输出尾 ';'. */
    up_expr(ctx, interface_members_node_get_member(members, i));
    up_char(ctx, '\n');
  }
  ctx->depth--;
  up_indent(ctx);
  up_char(ctx, '}');
}

static void up_union_decl(unparse_ctx_t *ctx, node_t *node) {
  up_loc_text(ctx, union_node_get_keyword(node));
  up_char(ctx, ' ');
  up_expr(ctx, union_node_get_name(node));
  if (union_node_get_generics(node)) {
    up_expr(ctx, union_node_get_generics(node));
  }
  up_fields(ctx, union_node_get_body(node));
}

static void up_enum_decl(unparse_ctx_t *ctx, node_t *node) {
  up_loc_text(ctx, enum_decl_node_get_keyword(node));
  up_char(ctx, ' ');
  up_expr(ctx, enum_decl_node_get_name(node));
  if (enum_decl_node_get_underlying(node)) {
    up_cstr(ctx, ": ");
    up_expr(ctx, enum_decl_node_get_underlying(node));
  }
  up_enum_variants(ctx, enum_decl_node_get_body(node));
}

static void up_struct_decl(unparse_ctx_t *ctx, node_t *node) {
  up_loc_text(ctx, struct_decl_node_get_keyword(node));
  up_char(ctx, ' ');
  up_expr(ctx, struct_decl_node_get_name(node));
  if (struct_decl_node_get_generics(node)) {
    up_expr(ctx, struct_decl_node_get_generics(node));
  }
  if (struct_decl_node_get_implements(node)) {
    up_char(ctx, ' ');
    up_expr(ctx, struct_decl_node_get_implements(node));
  }
  up_struct_members(ctx, struct_decl_node_get_body(node));
}

static void up_interface_decl(unparse_ctx_t *ctx, node_t *node) {
  up_loc_text(ctx, interface_decl_node_get_keyword(node));
  up_char(ctx, ' ');
  up_expr(ctx, interface_decl_node_get_name(node));
  if (interface_decl_node_get_generics(node)) {
    up_expr(ctx, interface_decl_node_get_generics(node));
  }
  up_interface_members(ctx, interface_decl_node_get_body(node));
}

static void up_bitfield_decl(unparse_ctx_t *ctx, node_t *node) {
  up_loc_text(ctx, bitfield_decl_node_get_keyword(node));
  up_char(ctx, ' ');
  up_expr(ctx, bitfield_decl_node_get_name(node));
  if (bitfield_decl_node_get_underlying(node)) {
    up_cstr(ctx, ": ");
    up_expr(ctx, bitfield_decl_node_get_underlying(node));
  }
  up_bitfield_fields(ctx, bitfield_decl_node_get_body(node));
}

/* ---- 语句 ---- */

/* var 语句核心：for 初始化位置复用（无尾分号），其余场景加分号。 */
static void up_var_core(unparse_ctx_t *ctx, node_t *node, bool semicolon) {
  up_decorators(ctx,
                node,
                stmt_var_node_get_decorator_count(node),
                stmt_var_node_get_decorator);
  if (stmt_var_node_get_export(node)) {
    up_cstr(ctx, "export ");
  }
  if (stmt_var_node_get_comptime(node)) {
    up_cstr(ctx, "comptime ");
  }
  up_loc_text(ctx, stmt_var_node_get_keyword(node));
  up_char(ctx, ' ');
  up_expr(ctx, stmt_var_node_get_name(node));
  if (stmt_var_node_get_type(node)) {
    up_cstr(ctx, ": ");
    up_expr(ctx, stmt_var_node_get_type(node));
  }
  if (stmt_var_node_get_value(node)) {
    up_cstr(ctx, " = ");
    up_expr(ctx, stmt_var_node_get_value(node));
  }
  if (semicolon) {
    up_char(ctx, ';');
  }
}

static void up_stmt(unparse_ctx_t *ctx, node_t *node) {
  if (!node) {
    return;
  }
  switch (node_get_kind(node)) {
  case CLUX_NODE_STMT_EMPTY:
    up_char(ctx, ';');
    return;

  case CLUX_NODE_STMT_EXPR:
    up_expr(ctx, stmt_expr_node_get_expr(node));
    up_char(ctx, ';');
    return;

  case CLUX_NODE_STMT_RETURN:
    up_cstr(ctx, "return");
    if (stmt_return_node_get_value(node)) {
      up_char(ctx, ' ');
      up_expr(ctx, stmt_return_node_get_value(node));
    }
    up_char(ctx, ';');
    return;

  case CLUX_NODE_STMT_BREAK:
    up_cstr(ctx, "break;");
    return;

  case CLUX_NODE_STMT_CONTINUE:
    up_cstr(ctx, "continue;");
    return;

  case CLUX_NODE_STMT_BLOCK:
    up_block(ctx, node);
    return;

  case CLUX_NODE_STMT_VAR:
    up_var_core(ctx, node, true);
    return;

  case CLUX_NODE_STMT_FUNC:
    up_decorators(ctx,
                  node,
                  stmt_func_node_get_decorator_count(node),
                  stmt_func_node_get_decorator);
    if (stmt_func_node_get_export(node)) {
      up_cstr(ctx, "export ");
    }
    if (stmt_func_node_get_comptime(node)) {
      up_cstr(ctx, "comptime ");
    }
    up_expr(ctx, stmt_func_node_get_func_decl(node));
    return;

  case CLUX_NODE_STMT_TYPE:
    up_decorators(ctx,
                  node,
                  stmt_type_node_get_decorator_count(node),
                  stmt_type_node_get_decorator);
    if (stmt_type_node_get_export(node)) {
      up_cstr(ctx, "export ");
    }
    up_cstr(ctx, "type ");
    up_expr(ctx, stmt_type_node_get_name(node));
    if (stmt_type_node_get_generics(node)) {
      up_expr(ctx, stmt_type_node_get_generics(node));
    }
    up_cstr(ctx, " = ");
    up_expr(ctx, stmt_type_node_get_value(node));
    up_char(ctx, ';');
    return;

  case CLUX_NODE_STMT_ENUM:
    up_decorators(ctx,
                  node,
                  stmt_enum_node_get_decorator_count(node),
                  stmt_enum_node_get_decorator);
    if (stmt_enum_node_get_export(node)) {
      up_cstr(ctx, "export ");
    }
    up_expr(ctx, stmt_enum_node_get_enum_decl(node));
    return;

  case CLUX_NODE_STMT_UNION:
    up_decorators(ctx,
                  node,
                  stmt_union_node_get_decorator_count(node),
                  stmt_union_node_get_decorator);
    if (stmt_union_node_get_export(node)) {
      up_cstr(ctx, "export ");
    }
    up_expr(ctx, stmt_union_node_get_union_decl(node));
    return;

  case CLUX_NODE_STMT_STRUCT:
    up_decorators(ctx,
                  node,
                  stmt_struct_node_get_decorator_count(node),
                  stmt_struct_node_get_decorator);
    if (stmt_struct_node_get_export(node)) {
      up_cstr(ctx, "export ");
    }
    up_expr(ctx, stmt_struct_node_get_struct_decl(node));
    return;

  case CLUX_NODE_STMT_BITFIELD:
    up_decorators(ctx,
                  node,
                  stmt_bitfield_node_get_decorator_count(node),
                  stmt_bitfield_node_get_decorator);
    if (stmt_bitfield_node_get_export(node)) {
      up_cstr(ctx, "export ");
    }
    up_expr(ctx, stmt_bitfield_node_get_bitfield_decl(node));
    return;

  case CLUX_NODE_STMT_INTERFACE:
    up_decorators(ctx,
                  node,
                  stmt_interface_node_get_decorator_count(node),
                  stmt_interface_node_get_decorator);
    if (stmt_interface_node_get_export(node)) {
      up_cstr(ctx, "export ");
    }
    up_expr(ctx, stmt_interface_node_get_interface_decl(node));
    return;

  case CLUX_NODE_STMT_IF:
  case CLUX_NODE_STMT_COMPTIME_IF: {
    if (node_get_kind(node) == CLUX_NODE_STMT_COMPTIME_IF) {
      up_cstr(ctx, "comptime ");
    }
    up_cstr(ctx, "if (");
    up_expr(ctx, stmt_if_node_get_condition(node));
    up_cstr(ctx, ") ");
    up_block(ctx, stmt_if_node_get_then(node));
    node_t *else_branch = stmt_if_node_get_else(node);
    if (else_branch) {
      up_cstr(ctx, " else ");
      if (node_get_kind(else_branch) == CLUX_NODE_STMT_IF ||
          node_get_kind(else_branch) == CLUX_NODE_STMT_COMPTIME_IF) {
        up_stmt(ctx, else_branch);
      } else {
        up_block(ctx, else_branch);
      }
    }
    return;
  }

  case CLUX_NODE_STMT_WHILE:
    up_cstr(ctx, "while (");
    up_expr(ctx, stmt_while_node_get_condition(node));
    up_cstr(ctx, ") ");
    up_block(ctx, stmt_while_node_get_body(node));
    return;

  case CLUX_NODE_STMT_DO_WHILE:
    up_cstr(ctx, "do ");
    up_block(ctx, stmt_do_while_node_get_body(node));
    up_cstr(ctx, " while (");
    up_expr(ctx, stmt_do_while_node_get_condition(node));
    up_cstr(ctx, ");");
    return;

  case CLUX_NODE_STMT_FOR:
    up_cstr(ctx, "for (");
    node_t *init = stmt_for_node_get_init(node);
    if (init) {
      if (node_get_kind(init) == CLUX_NODE_STMT_VAR) {
        up_var_core(ctx, init, false);
      } else {
        up_expr(ctx, init);
      }
    }
    up_cstr(ctx, "; ");
    up_expr(ctx, stmt_for_node_get_condition(node));
    up_cstr(ctx, "; ");
    up_expr(ctx, stmt_for_node_get_update(node));
    up_cstr(ctx, ") ");
    up_block(ctx, stmt_for_node_get_body(node));
    return;

  case CLUX_NODE_STMT_FOREACH:
  case CLUX_NODE_STMT_COMPTIME_FOREACH:
    if (node_get_kind(node) == CLUX_NODE_STMT_COMPTIME_FOREACH) {
      up_cstr(ctx, "comptime ");
    }
    up_cstr(ctx, "foreach (");
    if (stmt_foreach_node_get_keyword(node)) {
      up_cstr(ctx, "var ");
    }
    up_expr(ctx, stmt_foreach_node_get_name(node));
    if (stmt_foreach_node_get_type(node)) {
      up_cstr(ctx, ": ");
      up_expr(ctx, stmt_foreach_node_get_type(node));
    }
    up_cstr(ctx, " of ");
    up_expr(ctx, stmt_foreach_node_get_iterable(node));
    up_cstr(ctx, ") ");
    up_block(ctx, stmt_foreach_node_get_body(node));
    return;

  case CLUX_NODE_STMT_SWITCH: {
    up_cstr(ctx, "switch (");
    up_expr(ctx, stmt_switch_node_get_value(node));
    up_cstr(ctx, ") {");
    node_t *body = stmt_switch_node_get_body(node);
    if (body) {
      up_char(ctx, '\n');
      ctx->depth++;
      size_t count = switch_branches_node_get_branch_count(body);
      for (size_t i = 0; i < count; i++) {
        node_t *branch = switch_branches_node_get_branch(body, i);
        up_indent(ctx);
        node_t *patterns = switch_branch_node_get_patterns(branch);
        if (patterns) {
          up_char(ctx, '(');
          size_t pcount = switch_patterns_node_get_pattern_count(patterns);
          for (size_t j = 0; j < pcount; j++) {
            if (j > 0) {
              up_cstr(ctx, ", ");
            }
            up_expr(ctx, switch_patterns_node_get_pattern(patterns, j));
          }
          up_char(ctx, ')');
        } else {
          up_cstr(ctx, "default");
        }
        up_cstr(ctx, " -> ");
        up_block(ctx, switch_branch_node_get_body(branch));
        up_char(ctx, '\n');
      }
      ctx->depth--;
    }
    up_indent(ctx);
    up_char(ctx, '}');
    return;
  }

  case CLUX_NODE_STMT_DEFER:
    up_cstr(ctx, "defer ");
    if (stmt_defer_node_get_captures(node)) {
      up_expr(ctx, stmt_defer_node_get_captures(node));
      up_char(ctx, ' ');
    }
    up_block(ctx, stmt_defer_node_get_body(node));
    return;

  case CLUX_NODE_STMT_ASSERT:
    up_cstr(ctx, "assert(");
    up_expr(ctx, stmt_assert_node_get_condition(node));
    up_cstr(ctx, ");");
    return;

  case CLUX_NODE_STMT_STATIC_ASSERT:
    up_cstr(ctx, "static_assert(");
    up_expr(ctx, stmt_static_assert_node_get_condition(node));
    if (stmt_static_assert_node_get_message(node)) {
      up_cstr(ctx, ", ");
      up_expr(ctx, stmt_static_assert_node_get_message(node));
    }
    up_cstr(ctx, ");");
    return;

  case CLUX_NODE_STMT_TEST:
    up_cstr(ctx, "test ");
    up_expr(ctx, stmt_test_node_get_name(node));
    up_char(ctx, ' ');
    up_block(ctx, stmt_test_node_get_body(node));
    return;

  case CLUX_NODE_STMT_IMPORT:
    up_cstr(ctx, "import ");
    up_expr(ctx, stmt_import_node_get_alias(node));
    up_cstr(ctx, " from ");
    up_expr(ctx, stmt_import_node_get_path(node));
    up_char(ctx, ';');
    return;

  case CLUX_NODE_STMT_REEXPORT:
    up_cstr(ctx, "export * from ");
    up_expr(ctx, stmt_reexport_node_get_path(node));
    up_char(ctx, ';');
    return;

  case CLUX_NODE_STMT_EXTERN_LIB:
    up_cstr(ctx, "extern lib ");
    up_expr(ctx, stmt_extern_lib_node_get_alias(node));
    up_cstr(ctx, " from ");
    up_expr(ctx, stmt_extern_lib_node_get_path(node));
    up_char(ctx, ';');
    return;

  case CLUX_NODE_STMT_EXTERN_CFUNC:
    if (stmt_extern_cfunc_node_get_export(node)) {
      up_cstr(ctx, "export ");
    }
    up_cstr(ctx, "extern cfunc ");
    up_expr(ctx, stmt_extern_cfunc_node_get_lib(node));
    up_cstr(ctx, "::");
    up_expr(ctx, stmt_extern_cfunc_node_get_name(node));
    up_expr(ctx, stmt_extern_cfunc_node_get_params(node));
    if (stmt_extern_cfunc_node_get_return(node)) {
      up_cstr(ctx, ": ");
      up_expr(ctx, stmt_extern_cfunc_node_get_return(node));
    }
    up_char(ctx, ';');
    return;

  case CLUX_NODE_STMT_EXTERN_VAR:
    if (stmt_extern_var_node_get_export(node)) {
      up_cstr(ctx, "export ");
    }
    up_cstr(ctx, "extern var ");
    up_expr(ctx, stmt_extern_var_node_get_lib(node));
    up_cstr(ctx, "::");
    up_expr(ctx, stmt_extern_var_node_get_name(node));
    up_cstr(ctx, ": ");
    up_expr(ctx, stmt_extern_var_node_get_type(node));
    up_char(ctx, ';');
    return;

  default:
    /* 终态兜底：不认识的 kind 输出 location 区间原文，
         防止与 up_expr 的 default 互相递归。 */
    up_loc_text(ctx, node);
    return;
  }
}

static void up_stmt_line(unparse_ctx_t *ctx, node_t *node) {
  up_indent(ctx);
  up_stmt(ctx, node);
  up_char(ctx, '\n');
}

/* ---- 公开入口 ---- */

/* 语句类 kind：单独文本化时按行输出（缩进 + 尾换行）。 */
static bool up_kind_is_stmt(node_kind_t kind) {
  switch (kind) {
  case CLUX_NODE_STMT_BLOCK:
  case CLUX_NODE_STMT_EXPR:
  case CLUX_NODE_STMT_RETURN:
  case CLUX_NODE_STMT_EMPTY:
  case CLUX_NODE_STMT_BREAK:
  case CLUX_NODE_STMT_CONTINUE:
  case CLUX_NODE_STMT_DEFER:
  case CLUX_NODE_STMT_IF:
  case CLUX_NODE_STMT_COMPTIME_IF:
  case CLUX_NODE_STMT_WHILE:
  case CLUX_NODE_STMT_FOR:
  case CLUX_NODE_STMT_FOREACH:
  case CLUX_NODE_STMT_COMPTIME_FOREACH:
  case CLUX_NODE_STMT_DO_WHILE:
  case CLUX_NODE_STMT_SWITCH:
  case CLUX_NODE_STMT_FUNC:
  case CLUX_NODE_STMT_VAR:
  case CLUX_NODE_STMT_TYPE:
  case CLUX_NODE_STMT_ENUM:
  case CLUX_NODE_STMT_UNION:
  case CLUX_NODE_STMT_STRUCT:
  case CLUX_NODE_STMT_BITFIELD:
  case CLUX_NODE_STMT_TEST:
  case CLUX_NODE_STMT_IMPORT:
  case CLUX_NODE_STMT_REEXPORT:
  case CLUX_NODE_STMT_EXTERN_LIB:
  case CLUX_NODE_STMT_EXTERN_CFUNC:
  case CLUX_NODE_STMT_EXTERN_VAR:
  case CLUX_NODE_STMT_STATIC_ASSERT:
  case CLUX_NODE_STMT_ASSERT:
  case CLUX_NODE_STMT_INTERFACE:
    return true;
  default:
    return false;
  }
}

string_t *node_unparse(node_t *node,
                       allocator_t *allocator,
                       const unparse_options_t *options) {
  (void)options; /* 占位：当前无可配置规则。 */
  unparse_ctx_t ctx = {create_string(allocator), allocator, options, 0};
  if (!ctx.out) {
    return NULL;
  }
  if (!node) {
    return ctx.out;
  }
  switch (node_get_kind(node)) {
  case CLUX_NODE_PROGRAM: {
    size_t count = program_node_get_stmt_count(node);
    for (size_t i = 0; i < count; i++) {
      up_stmt_line(&ctx, program_node_get_stmt(node, i));
    }
    return ctx.out;
  }
  case CLUX_NODE_EXPR_UNION:
    up_union_decl(&ctx, node);
    return ctx.out;
  case CLUX_NODE_EXPR_ENUM_DECL:
    up_enum_decl(&ctx, node);
    return ctx.out;
  case CLUX_NODE_EXPR_STRUCT_DECL:
    up_struct_decl(&ctx, node);
    return ctx.out;
  case CLUX_NODE_EXPR_INTERFACE_DECL:
    up_interface_decl(&ctx, node);
    return ctx.out;
  case CLUX_NODE_EXPR_BITFIELD_DECL:
    up_bitfield_decl(&ctx, node);
    return ctx.out;
  default:
    /* 语句按行输出，表达式与成员列表行内输出。 */
    if (up_kind_is_stmt(node_get_kind(node))) {
      up_stmt_line(&ctx, node);
    } else {
      up_expr(&ctx, node);
    }
    return ctx.out;
  }
}

string_t *program_unparse(program_t *program,
                          allocator_t *allocator,
                          const unparse_options_t *options) {
  if (!program) {
    return create_string(allocator);
  }
  return node_unparse(program_get_decls(program), allocator, options);
}
