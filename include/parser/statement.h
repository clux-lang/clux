#ifndef _H_PARSER_STMT_
#define _H_PARSER_STMT_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/node_stmt_bitfield.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_defer.h"
#include "parser/node_stmt_do_while.h"
#include "parser/node_stmt_empty.h"
#include "parser/node_stmt_enum.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_for.h"
#include "parser/node_stmt_foreach.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_if.h"
#include "parser/node_stmt_jump.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_switch.h"
#include "parser/node_stmt_test.h"
#include "parser/node_stmt_type.h"
#include "parser/node_switch_branch.h"
#include "parser/node_stmt_union.h"
#include "parser/node_stmt_var.h"
#include "parser/node_stmt_while.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif
node_t *parser_read_stmt_block(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_expr(allocator_t *allocator, parser_t *parser);

/* 空语句 `;`（白皮书 §4.14）。 */
node_t *parser_read_stmt_empty(allocator_t *allocator, parser_t *parser);

/* break; / continue;（白皮书 §4.10）。无子节点。 */
node_t *parser_read_stmt_return(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_break(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_continue(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_defer(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_if(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_comptime_if(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_while(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_for(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_foreach(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_comptime_foreach(allocator_t *allocator,
                                          parser_t *parser);
node_t *parser_read_stmt_do_while(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_switch(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_bitfield(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_test(allocator_t *allocator, parser_t *parser);

node_t *parser_read_stmt_func(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_var(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_type(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_enum(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_union(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_struct(allocator_t *allocator, parser_t *parser);

node_t *parser_read_stmt(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
