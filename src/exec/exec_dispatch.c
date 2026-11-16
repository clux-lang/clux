/* 分派表：node kind -> handler 的静态 map（designated initializer 按
   枚举值直接索引，无运行时注册、无 switch 链）。表项为 NULL 的 kind
   即执行器尚未支持的构造，统一走 not-supported 语义化错误。
   parser 增删节点 kind 时由 _Static_assert 编译期强制同步本文件。 */
#include "exec_internal.h"

/* 最后一个 kind 的序号校验：kind 数与 EXEC_NODE_KIND_COUNT 对齐。 */
_Static_assert(CLUX_NODE_STMT_INTERFACE == 81,
               "node kinds changed: update exec dispatch tables");

static exec_value_t exec_unsupported(clux_exec_t *self, node_t *node) {
  return exec_errorf(self,
                     node_get_location(node),
                     "node kind %d is not supported in script mode",
                     (int)node_get_kind(node));
}

static exec_value_t
table_dispatch(exec_visit_fn *table, clux_exec_t *self, node_t *node) {
  node_kind_t kind = node_get_kind(node);
  exec_visit_fn visit = kind < EXEC_NODE_KIND_COUNT ? table[kind] : NULL;
  return visit ? visit(self, node) : exec_unsupported(self, node);
}

/* 表按 kind 语义分两张：表达式与语句。声明语句（var/func/…）走语句表
   ——声明即执行。 */
static exec_visit_fn g_expr_table[EXEC_NODE_KIND_COUNT] = {
    [CLUX_NODE_LITERAL_UNDEFINED] = exec_visit_undefined,
    [CLUX_NODE_LITERAL_STRING] = exec_visit_string,
    [CLUX_NODE_LITERAL_CHARACTER] = exec_visit_character,
    [CLUX_NODE_LITERAL_NUMERIC] = exec_visit_numeric,
    [CLUX_NODE_LITERAL_BOOLEAN] = exec_visit_boolean,
    [CLUX_NODE_LITERAL_IDENTIFIER] = exec_visit_identifier,
    [CLUX_NODE_LITERAL_TYPE] = exec_visit_type_literal,
    [CLUX_NODE_EXPR_ASSIGN] = exec_visit_assign,
    [CLUX_NODE_EXPR_TERNARY] = exec_visit_ternary,
    [CLUX_NODE_EXPR_BINARY] = exec_visit_binary,
    [CLUX_NODE_EXPR_UNARY] = exec_visit_binary,
    [CLUX_NODE_EXPR_GROUP] = exec_visit_group,
    [CLUX_NODE_EXPR_FUNC_DECL] = exec_visit_func_decl,
};

static exec_visit_fn g_stmt_table[EXEC_NODE_KIND_COUNT] = {
    [CLUX_NODE_STMT_BLOCK] = exec_visit_block,
    [CLUX_NODE_STMT_EXPR] = exec_visit_stmt_expr,
    [CLUX_NODE_STMT_EMPTY] = exec_visit_empty,
    [CLUX_NODE_STMT_RETURN] = exec_visit_return,
    [CLUX_NODE_STMT_BREAK] = exec_visit_break,
    [CLUX_NODE_STMT_CONTINUE] = exec_visit_continue,
    [CLUX_NODE_STMT_IF] = exec_visit_if,
    [CLUX_NODE_STMT_WHILE] = exec_visit_while,
    [CLUX_NODE_STMT_VAR] = exec_visit_var,
    [CLUX_NODE_STMT_FUNC] = exec_visit_func_stmt,
    [CLUX_NODE_STMT_TYPE] = exec_visit_type_stmt,
    [CLUX_NODE_STMT_ENUM] = exec_visit_enum_stmt,
    [CLUX_NODE_STMT_UNION] = exec_visit_union_stmt,
    [CLUX_NODE_STMT_STRUCT] = exec_visit_struct_stmt,
};

exec_value_t exec_dispatch_expr(clux_exec_t *self, node_t *node) {
  return table_dispatch(g_expr_table, self, node);
}

exec_value_t exec_dispatch_stmt(clux_exec_t *self, node_t *node) {
  return table_dispatch(g_stmt_table, self, node);
}
