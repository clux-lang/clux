#ifndef _H_PARSER_STMT_
#define _H_PARSER_STMT_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif
node_t *parser_read_stmt_block(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_expr(allocator_t *allocator, parser_t *parser);

/* return; / return expr;（白皮书 §4.11）。
   VALUE 槽为返回表达式，无返回值时为 NULL。 */
#define CLUX_STMT_RETURN_VALUE 0
#define CLUX_STMT_RETURN_CHILDREN_SIZE 1

/* break; / continue;（白皮书 §4.10）。无子节点。 */
node_t *parser_read_stmt_return(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_break(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_continue(allocator_t *allocator, parser_t *parser);

/* 函数语句 <decorator>* <func_decl>（白皮书 §3.3）。
   FUNC_DECL 槽存函数声明；其后按源码顺序为装饰器，数量可变，
   因此 children 长度 = CLUX_STMT_FUNC_FIRST_DECORATOR + 装饰器个数。 */
#define CLUX_STMT_FUNC_FUNC_DECL 0
#define CLUX_STMT_FUNC_FIRST_DECORATOR 1
node_t *parser_read_stmt_func(allocator_t *allocator, parser_t *parser);

/* 变量定义语句 <decorator>* (var|using) name [: type]? = expr;
   （白皮书 §4.1、§4.12.1）。
   KEYWORD 区分 var 与 using；NAME 必有；TYPE 可缺失（由初值推断）；
   VALUE 为初值表达式，必有；其后按源码顺序为装饰器，数量可变，
   因此 children 长度 = CLUX_STMT_VAR_FIRST_DECORATOR + 装饰器个数。 */
#define CLUX_STMT_VAR_KEYWORD 0
#define CLUX_STMT_VAR_NAME 1
#define CLUX_STMT_VAR_TYPE 2
#define CLUX_STMT_VAR_VALUE 3
#define CLUX_STMT_VAR_FIRST_DECORATOR 4
node_t *parser_read_stmt_var(allocator_t *allocator, parser_t *parser);

node_t *parser_read_stmt(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
