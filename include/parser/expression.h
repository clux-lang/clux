#ifndef _H_PARSER_EXPR_
#define _H_PARSER_EXPR_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/node_assign.h"
#include "parser/node_binary.h"
#include "parser/node_bitfield_decl.h"
#include "parser/node_bitfield_field.h"
#include "parser/node_call.h"
#include "parser/node_construct.h"
#include "parser/node_enum_decl.h"
#include "parser/node_enum_variant.h"
#include "parser/node_field.h"
#include "parser/node_func_capture.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_func_type.h"
#include "parser/node_generic_param.h"
#include "parser/node_group.h"
#include "parser/node_rest.h"
#include "parser/node_implements.h"
#include "parser/node_interface_decl.h"
#include "parser/node_slice.h"
#include "parser/node_struct_decl.h"
#include "parser/node_struct_member.h"
#include "parser/node_subscript.h"
#include "parser/node_ternary.h"
#include "parser/node_tuple.h"
#include "parser/node_union.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif

node_t *parser_read_expr_lor(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr_land(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr_or(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr_xor(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr_and(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr_equality(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr_relational(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr_shift(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr_additive(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr_multiplicative(allocator_t *allocator,
                                        parser_t *parser);
node_t *parser_read_expr_as_is(allocator_t *allocator, parser_t *parser);
/* 值表达式层：最低一级优先级，与 literal 平级的构造统一从这里进入。
   目前包含构造表达式、括号表达式与 literal，后续陆续加入同级的运算符。 */
node_t *parser_read_expr_value(allocator_t *allocator, parser_t *parser);

/* 前缀一元运算符（白皮书 5.3 优先级 23）：! ~ + -。
   复用二元节点形态，LEFT 为 NULL，SYMBOL 存运算符，RIGHT 存操作数。 */
node_t *parser_read_expr_unary(allocator_t *allocator, parser_t *parser);
/* 后缀一元运算符（白皮书 5.3 优先级 25）：.& .* .! .?
   与一元层镜像：LEFT 存操作数，SYMBOL 存运算符，RIGHT 为 NULL。 */
node_t *parser_read_expr_postfix(allocator_t *allocator, parser_t *parser);

node_t *parser_read_expr_rest(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr_ternary(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr_assign(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr(allocator_t *allocator, parser_t *parser);

/* 表达式类节点的 JSON 分派；node 为 NULL 时写 null。 */
void expr_to_json(json_writer_t *writer, node_t *node);
#ifdef __cplusplus
}
#endif
#endif
