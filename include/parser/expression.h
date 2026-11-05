#ifndef _H_PARSER_EXPRESSION_
#define _H_PARSER_EXPRESSION_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif

#define CLUX_EXPR_BINARY_CHILDREN_SIZE 3
#define CLUX_EXPR_BINARY_LEFT 0
#define CLUX_EXPR_BINARY_SYMBOL 1
#define CLUX_EXPR_BINARY_RIGHT 2
node_t *parser_read_expression_lor(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression_land(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression_or(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression_xor(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression_and(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression_equality(allocator_t *allocator,
                                        parser_t *parser);
node_t *parser_read_expression_relational(allocator_t *allocator,
                                          parser_t *parser);
node_t *parser_read_expression_shift(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression_additive(allocator_t *allocator,
                                        parser_t *parser);
node_t *parser_read_expression_multiplicative(allocator_t *allocator,
                                              parser_t *parser);
node_t *parser_read_expression_as_is(allocator_t *allocator, parser_t *parser);
/* 前缀一元运算符（白皮书 5.3 优先级 23）：! ~ + -。
   复用二元节点形态，LEFT 为 NULL，SYMBOL 存运算符，RIGHT 存操作数。 */
node_t *parser_read_expression_unary(allocator_t *allocator, parser_t *parser);
/* 后缀一元运算符（白皮书 5.3 优先级 25）：.& .* .! .?
   与一元层镜像：LEFT 存操作数，SYMBOL 存运算符，RIGHT 为 NULL。 */
node_t *parser_read_expression_postfix(allocator_t *allocator,
                                       parser_t *parser);

/* 下标/实例化节点：base[arg, ...]。
   BASE 为被索引/被实例化的表达式，ARGS 起为逗号分隔的实参列表，
   数量可变，因此 children 长度 = CLUX_EXPR_SUBSCRIPT_FIRST_ARG + 实参个数。
   语法阶段无法区分下标与实例化，由 sema 按 BASE 的类型判定。 */
#define CLUX_EXPR_SUBSCRIPT_BASE 0
#define CLUX_EXPR_SUBSCRIPT_FIRST_ARG 1

/* 切片节点：base[lo?:hi?]，LO/HI 可为 NULL（省略该端）。 */
#define CLUX_EXPR_SLICE_CHILDREN_SIZE 3
#define CLUX_EXPR_SLICE_BASE 0
#define CLUX_EXPR_SLICE_LO 1
#define CLUX_EXPR_SLICE_HI 2

/* 函数调用节点：callee(arg, ...)。
   CALLEE 为被调用表达式，其后为逗号分隔的实参列表，数量可变，
   因此 children 长度 = CLUX_EXPR_CALL_FIRST_ARG + 实参个数。
   实参可以是 rest 表达式（含孤立 `...`，由 sema 报错）。 */
#define CLUX_EXPR_CALL_CALLEE 0
#define CLUX_EXPR_CALL_FIRST_ARG 1

/* rest 表达式 ...expr?，不在优先级链上。
   有操作数时 children = [expr]（长度 1），孤立 `...` 时长度 0。 */
#define CLUX_EXPR_REST_EXPR 0
node_t *parser_read_expression_rest(allocator_t *allocator, parser_t *parser);

/* 构造表达式 .type?{ elem, ... }。
   TYPE 为被构造的类型表达式（匿名构造时为 NULL），其后为逗号分隔的元素，
   数量可变，因此 children 长度 = CLUX_EXPR_CONSTRUCT_FIRST_ELEM + 元素个数。
   语法阶段类型就是普通表达式，按后缀层解析；元素可以是 rest 表达式。 */
#define CLUX_EXPR_CONSTRUCT_TYPE 0
#define CLUX_EXPR_CONSTRUCT_FIRST_ELEM 1

#define CLUX_EXPR_TERNARY_CHILDREN_SIZE 3
#define CLUX_EXPR_TERNARY_CONDITION 0
#define CLUX_EXPR_TERNARY_THEN 1
#define CLUX_EXPR_TERNARY_ELSE 2
node_t *parser_read_expression_ternary(allocator_t *allocator,
                                       parser_t *parser);
#define CLUX_EXPR_ASSIGN_CHILDREN_SIZE 3
#define CLUX_EXPR_ASSIGN_ID 0
#define CLUX_EXPR_ASSIGN_SYMBOL 1
#define CLUX_EXPR_ASSIGN_VALUE 2
node_t *parser_read_expression_assign(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
