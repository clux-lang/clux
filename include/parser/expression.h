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

/* 函数声明 / 函数定义 / 闭包 func |captures| name[generics](params): ret {body}
   （白皮书 §3.3）。是 func_type 的后退方案：首 token 为 func/cfunc 且
   func_type 不匹配时，必然是声明，任何不匹配都是硬错误。
   KEYWORD 区分 func 与 cfunc；NAME 为函数名（匿名函数与 cfunc 为 NULL）；
   GENERICS 为泛型参数列表（无泛型参数时为 NULL）；
   CAPTURES 为捕获列表（无捕获时为 NULL）；PARAMS 为形参列表；
   RETURN 为返回类型；BODY 为函数体块（cfunc 与匿名函数类型除外）。 */
#define CLUX_EXPR_FUNC_DECL_KEYWORD 0
#define CLUX_EXPR_FUNC_DECL_NAME 1
#define CLUX_EXPR_FUNC_DECL_GENERICS 2
#define CLUX_EXPR_FUNC_DECL_CAPTURES 3
#define CLUX_EXPR_FUNC_DECL_PARAMS 4
#define CLUX_EXPR_FUNC_DECL_RETURN 5
#define CLUX_EXPR_FUNC_DECL_BODY 6
#define CLUX_EXPR_FUNC_DECL_CHILDREN_SIZE 7

/* C union 定义 cunion Name { field; ... }（白皮书 §3.8）。
   NAME 为类型名；GENERICS 为泛型参数列表（无泛型时为 NULL）；
   BODY 为字段列表节点，children 为 CLUX_NODE_FIELD。 */
#define CLUX_EXPR_CUNION_NAME 0
#define CLUX_EXPR_CUNION_GENERICS 1
#define CLUX_EXPR_CUNION_BODY 2
#define CLUX_EXPR_CUNION_CHILDREN_SIZE 3

/* 类型定义的字段 name: type。struct / tag union / cunion 共用。
   NAME 必有；TYPE 缺失时为 NULL（容错）。 */
#define CLUX_FIELD_NAME 0
#define CLUX_FIELD_TYPE 1
#define CLUX_FIELD_CHILDREN_SIZE 2

/* 字段列表 { field; ... }，children 为 CLUX_NODE_FIELD。 */

/* 形参列表，children 为 CLUX_NODE_FUNC_PARAM 或 rest 表达式，数量可变。
   函数声明与函数类型共用此节点；函数声明的捕获列表也复用它
   （CLUX_NODE_FUNC_CAPTURES 的唯一子节点）。 */

/* 单个形参：NAME / TYPE / DEFAULT 均可为 NULL。
   `name: type` 有 NAME；函数类型里的裸 `type` 只有 TYPE。
   只有闭包的捕获列表允许 `= default`（DEFAULT 非 NULL），
   函数参数没有默认值。 */
#define CLUX_FUNC_PARAM_NAME 0
#define CLUX_FUNC_PARAM_TYPE 1
#define CLUX_FUNC_PARAM_DEFAULT 2
#define CLUX_FUNC_PARAM_CHILDREN_SIZE 3

/* 泛型参数列表 [T, U, ...]，children 为 CLUX_NODE_GENERIC_PARAM 或 rest。 */

/* 单个泛型参数，按白皮书 §6.1–§6.5 的写法：
     T                      —— 类型参数
     N: comptime usize      —— 编译期值参数（TYPE 非 NULL）
     W extends IWriter[W]   —— 约束（CONSTRAINT 非 NULL）
     T = i32                —— 默认值（DEFAULT 非 NULL）
   NAME 必有；其余槽位按需出现，顺序为 TYPE → CONSTRAINT → DEFAULT。 */
#define CLUX_GENERIC_PARAM_NAME 0
#define CLUX_GENERIC_PARAM_TYPE 1
#define CLUX_GENERIC_PARAM_CONSTRAINT 2
#define CLUX_GENERIC_PARAM_DEFAULT 3
#define CLUX_GENERIC_PARAM_CHILDREN_SIZE 4

/* 函数类型的捕获列表 |name: type, ...|（空列表写作 ||），
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
