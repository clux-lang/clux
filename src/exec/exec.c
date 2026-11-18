#include "exec/exec.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/interface.h"
#include "engine/value.h"
#include "exec/stmt.h"
#include "parser/node.h"
#include "exec/literal.h"
#include "exec/expr.h"

typedef clux_value_t *(*exec_fn_t)(clux_context_t *ctx, node_t *node);
static exec_fn_t g_exec_callbacks[CLUX_NODE_KIND_MAX] = {
    /*CLUX_NODE_SYMBOL*/ NULL,
    /*CLUX_NODE_PROGRAM*/ NULL,
    /*CLUX_NODE_LITERAL_UNDEFINED*/ NULL,
    /*CLUX_NODE_LITERAL_STRING*/ exec_literal_string,
    /*CLUX_NODE_LITERAL_CHARACTER*/ exec_literal_charactor,
    /*CLUX_NODE_LITERAL_NUMERIC*/ exec_literal_numeric,
    /*CLUX_NODE_LITERAL_BOOLEAN*/ exec_literal_boolean,
    /*CLUX_NODE_LITERAL_IDENTIFIER*/ exec_literal_identifier,
    /*CLUX_NODE_SOURCE_TEXT*/ NULL,
    /*CLUX_NODE_EXPR_ASSIGN*/ exec_expr_assign,
    /*CLUX_NODE_EXPR_TERNARY*/ exec_expr_ternary,
    /*CLUX_NODE_EXPR_BINARY*/ exec_expr_binary,
    /*CLUX_NODE_EXPR_SUBSCRIPT*/ exec_expr_subscript,
    /*CLUX_NODE_EXPR_SLICE*/ exec_expr_slice,
    /*CLUX_NODE_EXPR_REST*/ NULL,
    /*CLUX_NODE_EXPR_CALL*/ NULL,
    /*CLUX_NODE_EXPR_BUILTIN*/ NULL,
    /*CLUX_NODE_EXPR_CONSTRUCT*/ NULL,
    /*CLUX_NODE_EXPR_CONSTRUCT_FIELD*/ NULL,
    /*CLUX_NODE_EXPR_GROUP*/ exec_expr_group,
    /*CLUX_NODE_EXPR_TUPLE*/ exec_expr_tuple,
    /*CLUX_NODE_EXPR_FUNC_TYPE*/ exec_expr_func_type,
    /*CLUX_NODE_EXPR_FUNC_DECL*/ exec_expr_func_decl,
    /*CLUX_NODE_FUNC_CAPTURES*/ NULL,
    /*CLUX_NODE_FUNC_CAPTURE*/ NULL,
    /*CLUX_NODE_FUNC_PARAMS*/ NULL,
    /*CLUX_NODE_FUNC_PARAM*/ NULL,
    /*CLUX_NODE_EXPR_UNION*/ NULL,
    /*CLUX_NODE_EXPR_ENUM_DECL*/ NULL,
    /*CLUX_NODE_EXPR_STRUCT_DECL*/ NULL,
    /*CLUX_NODE_EXPR_BITFIELD_DECL*/ NULL,
    /*CLUX_NODE_EXPR_INTERFACE_DECL*/ NULL,
    /*CLUX_NODE_INTERFACE_MEMBERS*/ NULL,
    /*CLUX_NODE_STRUCT_MEMBERS*/ NULL,
    /*CLUX_NODE_STRUCT_EMBED*/ NULL,
    /*CLUX_NODE_IMPLEMENTS*/ NULL,
    /*CLUX_NODE_ENUM_VARIANT*/ NULL,
    /*CLUX_NODE_ENUM_VARIANTS*/ NULL,
    /*CLUX_NODE_FIELD*/ NULL,
    /*CLUX_NODE_FIELDS*/ NULL,
    /*CLUX_NODE_BITFIELD_FIELD*/ NULL,
    /*CLUX_NODE_BITFIELD_FIELDS*/ NULL,
    /*CLUX_NODE_GENERIC_PARAMS*/ NULL,
    /*CLUX_NODE_GENERIC_PARAM*/ NULL,
    /*CLUX_NODE_STMT_BLOCK*/ NULL,
    /*CLUX_NODE_STMT_EXPR*/ NULL,
    /*CLUX_NODE_STMT_RETURN*/ NULL,
    /*CLUX_NODE_STMT_EMPTY*/ NULL,
    /*CLUX_NODE_STMT_BREAK*/ NULL,
    /*CLUX_NODE_STMT_CONTINUE*/ NULL,
    /*CLUX_NODE_STMT_DEFER*/ NULL,
    /*CLUX_NODE_STMT_IF*/ NULL,
    /*CLUX_NODE_STMT_COMPTIME_IF*/ NULL,
    /*CLUX_NODE_STMT_WHILE*/ NULL,
    /*CLUX_NODE_STMT_FOR*/ NULL,
    /*CLUX_NODE_STMT_FOREACH*/ NULL,
    /*CLUX_NODE_STMT_COMPTIME_FOREACH*/ NULL,
    /*CLUX_NODE_STMT_DO_WHILE*/ NULL,
    /*CLUX_NODE_STMT_SWITCH*/ NULL,
    /*CLUX_NODE_SWITCH_BRANCHES*/ NULL,
    /*CLUX_NODE_SWITCH_BRANCH*/ NULL,
    /*CLUX_NODE_SWITCH_PATTERNS*/ NULL,
    /*CLUX_NODE_DECORATOR*/ NULL,
    /*CLUX_NODE_STMT_FUNC*/ NULL,
    /*CLUX_NODE_STMT_VAR*/ exec_stmt_var,
    /*CLUX_NODE_STMT_TYPE*/ NULL,
    /*CLUX_NODE_STMT_ENUM*/ NULL,
    /*CLUX_NODE_STMT_UNION*/ NULL,
    /*CLUX_NODE_STMT_STRUCT*/ NULL,
    /*CLUX_NODE_STMT_BITFIELD*/ NULL,
    /*CLUX_NODE_STMT_TEST*/ NULL,
    /*CLUX_NODE_STMT_IMPORT*/ NULL,
    /*CLUX_NODE_STMT_REEXPORT*/ NULL,
    /*CLUX_NODE_STMT_EXTERN_LIB*/ NULL,
    /*CLUX_NODE_STMT_EXTERN_CFUNC*/ NULL,
    /*CLUX_NODE_STMT_EXTERN_VAR*/ NULL,
    /*CLUX_NODE_STMT_STATIC_ASSERT*/ NULL,
    /*CLUX_NODE_STMT_ASSERT*/ NULL,
    /*CLUX_NODE_STMT_INTERFACE*/ NULL,
};

static const char *node_kind_name(node_kind_t kind) {
  switch (kind) {
  case CLUX_NODE_SYMBOL:
    return "SYMBOL";
  case CLUX_NODE_PROGRAM:
    return "PROGRAM";
  case CLUX_NODE_LITERAL_UNDEFINED:
    return "LITERAL_UNDEFINED";
  case CLUX_NODE_LITERAL_STRING:
    return "LITERAL_STRING";
  case CLUX_NODE_LITERAL_CHARACTER:
    return "LITERAL_CHARACTER";
  case CLUX_NODE_LITERAL_NUMERIC:
    return "LITERAL_NUMERIC";
  case CLUX_NODE_LITERAL_BOOLEAN:
    return "LITERAL_BOOLEAN";
  case CLUX_NODE_LITERAL_IDENTIFIER:
    return "LITERAL_IDENTIFIER";
  case CLUX_NODE_SOURCE_TEXT:
    return "SOURCE_TEXT";
  case CLUX_NODE_EXPR_ASSIGN:
    return "EXPR_ASSIGN";
  case CLUX_NODE_EXPR_TERNARY:
    return "EXPR_TERNARY";
  case CLUX_NODE_EXPR_BINARY:
    return "EXPR_BINARY";
  case CLUX_NODE_EXPR_SUBSCRIPT:
    return "EXPR_SUBSCRIPT";
  case CLUX_NODE_EXPR_SLICE:
    return "EXPR_SLICE";
  case CLUX_NODE_EXPR_REST:
    return "EXPR_REST";
  case CLUX_NODE_EXPR_CALL:
    return "EXPR_CALL";
  case CLUX_NODE_EXPR_BUILTIN:
    return "EXPR_BUILTIN";
  case CLUX_NODE_EXPR_CONSTRUCT:
    return "EXPR_CONSTRUCT";
  case CLUX_NODE_EXPR_CONSTRUCT_FIELD:
    return "EXPR_CONSTRUCT_FIELD";
  case CLUX_NODE_EXPR_GROUP:
    return "EXPR_GROUP";
  case CLUX_NODE_EXPR_TUPLE:
    return "EXPR_TUPLE";
  case CLUX_NODE_EXPR_FUNC_TYPE:
    return "EXPR_FUNC_TYPE";
  case CLUX_NODE_EXPR_FUNC_DECL:
    return "EXPR_FUNC_DECL";
  case CLUX_NODE_FUNC_CAPTURES:
    return "FUNC_CAPTURES";
  case CLUX_NODE_FUNC_CAPTURE:
    return "FUNC_CAPTURE";
  case CLUX_NODE_FUNC_PARAMS:
    return "FUNC_PARAMS";
  case CLUX_NODE_FUNC_PARAM:
    return "FUNC_PARAM";
  case CLUX_NODE_EXPR_UNION:
    return "EXPR_UNION";
  case CLUX_NODE_EXPR_ENUM_DECL:
    return "EXPR_ENUM_DECL";
  case CLUX_NODE_EXPR_STRUCT_DECL:
    return "EXPR_STRUCT_DECL";
  case CLUX_NODE_EXPR_BITFIELD_DECL:
    return "EXPR_BITFIELD_DECL";
  case CLUX_NODE_EXPR_INTERFACE_DECL:
    return "EXPR_INTERFACE_DECL";
  case CLUX_NODE_INTERFACE_MEMBERS:
    return "INTERFACE_MEMBERS";
  case CLUX_NODE_STRUCT_MEMBERS:
    return "STRUCT_MEMBERS";
  case CLUX_NODE_STRUCT_EMBED:
    return "STRUCT_EMBED";
  case CLUX_NODE_IMPLEMENTS:
    return "IMPLEMENTS";
  case CLUX_NODE_ENUM_VARIANT:
    return "ENUM_VARIANT";
  case CLUX_NODE_ENUM_VARIANTS:
    return "ENUM_VARIANTS";
  case CLUX_NODE_FIELD:
    return "FIELD";
  case CLUX_NODE_FIELDS:
    return "FIELDS";
  case CLUX_NODE_BITFIELD_FIELD:
    return "BITFIELD_FIELD";
  case CLUX_NODE_BITFIELD_FIELDS:
    return "BITFIELD_FIELDS";
  case CLUX_NODE_GENERIC_PARAMS:
    return "GENERIC_PARAMS";
  case CLUX_NODE_GENERIC_PARAM:
    return "GENERIC_PARAM";
  case CLUX_NODE_STMT_BLOCK:
    return "STMT_BLOCK";
  case CLUX_NODE_STMT_EXPR:
    return "STMT_EXPR";
  case CLUX_NODE_STMT_RETURN:
    return "STMT_RETURN";
  case CLUX_NODE_STMT_EMPTY:
    return "STMT_EMPTY";
  case CLUX_NODE_STMT_BREAK:
    return "STMT_BREAK";
  case CLUX_NODE_STMT_CONTINUE:
    return "STMT_CONTINUE";
  case CLUX_NODE_STMT_DEFER:
    return "STMT_DEFER";
  case CLUX_NODE_STMT_IF:
    return "STMT_IF";
  case CLUX_NODE_STMT_COMPTIME_IF:
    return "STMT_COMPTIME_IF";
  case CLUX_NODE_STMT_WHILE:
    return "STMT_WHILE";
  case CLUX_NODE_STMT_FOR:
    return "STMT_FOR";
  case CLUX_NODE_STMT_FOREACH:
    return "STMT_FOREACH";
  case CLUX_NODE_STMT_COMPTIME_FOREACH:
    return "STMT_COMPTIME_FOREACH";
  case CLUX_NODE_STMT_DO_WHILE:
    return "STMT_DO_WHILE";
  case CLUX_NODE_STMT_SWITCH:
    return "STMT_SWITCH";
  case CLUX_NODE_SWITCH_BRANCHES:
    return "SWITCH_BRANCHES";
  case CLUX_NODE_SWITCH_BRANCH:
    return "SWITCH_BRANCH";
  case CLUX_NODE_SWITCH_PATTERNS:
    return "SWITCH_PATTERNS";
  case CLUX_NODE_DECORATOR:
    return "DECORATOR";
  case CLUX_NODE_STMT_FUNC:
    return "STMT_FUNC";
  case CLUX_NODE_STMT_VAR:
    return "STMT_VAR";
  case CLUX_NODE_STMT_TYPE:
    return "STMT_TYPE";
  case CLUX_NODE_STMT_ENUM:
    return "STMT_ENUM";
  case CLUX_NODE_STMT_UNION:
    return "STMT_UNION";
  case CLUX_NODE_STMT_STRUCT:
    return "STMT_STRUCT";
  case CLUX_NODE_STMT_BITFIELD:
    return "STMT_BITFIELD";
  case CLUX_NODE_STMT_TEST:
    return "STMT_TEST";
  case CLUX_NODE_STMT_IMPORT:
    return "STMT_IMPORT";
  case CLUX_NODE_STMT_REEXPORT:
    return "STMT_REEXPORT";
  case CLUX_NODE_STMT_EXTERN_LIB:
    return "STMT_EXTERN_LIB";
  case CLUX_NODE_STMT_EXTERN_CFUNC:
    return "STMT_EXTERN_CFUNC";
  case CLUX_NODE_STMT_EXTERN_VAR:
    return "STMT_EXTERN_VAR";
  case CLUX_NODE_STMT_STATIC_ASSERT:
    return "STMT_STATIC_ASSERT";
  case CLUX_NODE_STMT_ASSERT:
    return "STMT_ASSERT";
  case CLUX_NODE_STMT_INTERFACE:
    return "STMT_INTERFACE";
  default:
    return "<unknown>";
  }
}

clux_value_t *exec_node(clux_context_t *ctx, node_t *node) {
  if (!node) {
    return clux_context_create_error_message(
        ctx, NULL, "execution requires a node");
  }
  node_kind_t kind = node_get_kind(node);
  if (g_exec_callbacks[kind] == NULL) {
    const location_t *loc = node_get_location(node);
    return clux_context_create_error_message(ctx,
                                             (location_t *)loc,
                                             "unsupported node kind: %s",
                                             node_kind_name(kind));
  }
  return g_exec_callbacks[kind](ctx, node);
}

/* 操作数求值统一通道（exec 的消费点）：error 优先传播，shadow 拒绝
   （shadow 只存在于 sema 的影子执行），TDZ 消费校验
   （声明后未经首次赋值，报 use of a value before initialization）。 */
clux_value_t *exec_operand(clux_context_t *ctx, node_t *node) {
  clux_value_t *value = exec_node(ctx, node);
  if (clux_value_is_error(value)) {
    return value;
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "shadow value is not allowed in execution");
  }
  return clux_context_consume_value(ctx, value, node_get_location(node));
}