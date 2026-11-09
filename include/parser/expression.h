#ifndef _H_PARSER_EXPR_
#define _H_PARSER_EXPR_
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

/* 括号表达式 ( expr )：只有一个子节点。
   语法层不校验括号内是值还是语句——顶层入口 parser_read_expr 也允许，
   是否为合法值推迟到语义阶段。 */
#define CLUX_EXPR_GROUP_EXPR 0
/* 前缀一元运算符（白皮书 5.3 优先级 23）：! ~ + -。
   复用二元节点形态，LEFT 为 NULL，SYMBOL 存运算符，RIGHT 存操作数。 */
node_t *parser_read_expr_unary(allocator_t *allocator, parser_t *parser);
/* 后缀一元运算符（白皮书 5.3 优先级 25）：.& .* .! .?
   与一元层镜像：LEFT 存操作数，SYMBOL 存运算符，RIGHT 为 NULL。 */
node_t *parser_read_expr_postfix(allocator_t *allocator, parser_t *parser);

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
node_t *parser_read_expr_rest(allocator_t *allocator, parser_t *parser);

/* 构造表达式 .type?{ elem, ... }。
   TYPE 为被构造的类型表达式（匿名构造时为 NULL），其后为逗号分隔的元素，
   数量可变，因此 children 长度 = CLUX_EXPR_CONSTRUCT_FIRST_ELEM + 元素个数。
   语法阶段类型就是普通表达式，按后缀层解析；元素可以是 rest 表达式。 */
#define CLUX_EXPR_CONSTRUCT_TYPE 0
#define CLUX_EXPR_CONSTRUCT_FIRST_ELEM 1

/* 函数类型 func |captures|(params) -> ret（白皮书 §2.7）。
   CAPTURES 为捕获列表（无捕获时为 NULL），PARAMS 为形参类型列表，
   RETURN 为返回类型。形参是裸类型表达式，不是 FUNC_PARAM。 */
#define CLUX_EXPR_FUNC_TYPE_CAPTURES 0
#define CLUX_EXPR_FUNC_TYPE_PARAMS 1
#define CLUX_EXPR_FUNC_TYPE_RETURN 2
#define CLUX_EXPR_FUNC_TYPE_CHILDREN_SIZE 3

/* 捕获列表 |name: type, ...|（空列表写作 ||），
   children 为 CLUX_NODE_FUNC_CAPTURE。 */

/* 单个捕获项：NAME 必有；TYPE 缺失时为 NULL（容错）。 */
#define CLUX_FUNC_CAPTURE_NAME 0
#define CLUX_FUNC_CAPTURE_TYPE 1
#define CLUX_FUNC_CAPTURE_CHILDREN_SIZE 2

#define CLUX_EXPR_TERNARY_CHILDREN_SIZE 3
#define CLUX_EXPR_TERNARY_CONDITION 0
#define CLUX_EXPR_TERNARY_THEN 1
#define CLUX_EXPR_TERNARY_ELSE 2
node_t *parser_read_expr_ternary(allocator_t *allocator, parser_t *parser);
#define CLUX_EXPR_ASSIGN_CHILDREN_SIZE 3
#define CLUX_EXPR_ASSIGN_ID 0
#define CLUX_EXPR_ASSIGN_SYMBOL 1
#define CLUX_EXPR_ASSIGN_VALUE 2
node_t *parser_read_expr_assign(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expr(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
