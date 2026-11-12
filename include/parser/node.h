#ifndef _H_PARSER_NODE_
#define _H_PARSER_NODE_
#include "core/allocator.h"
#include "core/json.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser.h"
#ifdef __cplusplus
extern "C" {
#endif
enum _node_kind_t {
  CLUX_NODE_SYMBOL,
  CLUX_NODE_PROGRAM,
  CLUX_NODE_STATEMENT_EXPRESSION,
  CLUX_NODE_LITERAL_NIL,
  CLUX_NODE_LITERAL_UNDEFINED,
  CLUX_NODE_LITERAL_STRING,
  CLUX_NODE_LITERAL_CHARACTER,
  CLUX_NODE_LITERAL_NUMERIC,
  CLUX_NODE_LITERAL_BOOLEAN,
  CLUX_NODE_LITERAL_IDENTIFIER,
  /* 内建类型名（i8..f64、isize/usize、bool/str/void/opaque）。
     语法阶段与 literal 同级，就是普通表达式。 */
  CLUX_NODE_LITERAL_TYPE,
  /* 源码原文片段：叶子节点，location 指向源码中的原始文本区间，
     文本内容即该区间（不复制、不转义）。供 static_assert / assert
     在编译期引用表达式源码。 */
  CLUX_NODE_SOURCE_TEXT,

  CLUX_NODE_EXPR_ASSIGN,
  CLUX_NODE_EXPR_TERNARY,
  CLUX_NODE_EXPR_BINARY,
  CLUX_NODE_EXPR_UNARY,
  /* 下标与实例化语法同形（base[expr, ...]），语法阶段无法区分，统一为此类型。 */
  CLUX_NODE_EXPR_SUBSCRIPT,
  /* 切片 base[lo?:hi?]，冒号使其在语法阶段即可区分。 */
  CLUX_NODE_EXPR_SLICE,
  /* rest 表达式 ...expr?，不在优先级链上；孤立 `...` 时无子节点。 */
  CLUX_NODE_EXPR_REST,
  /* 函数调用 callee(arg, ...)；实参可以是 rest 表达式。 */
  CLUX_NODE_EXPR_CALL,
  /* 编译器内建 #name(arg, ...)，与普通调用在语法阶段即区分。
     NAME 为去掉 '#' 的内建名；实参可以是 rest 表达式。 */
  CLUX_NODE_EXPR_BUILTIN,
  /* 构造表达式 .type?{ elem, ... }；类型槽可为 NULL（匿名构造），
     元素是表达式、rest 表达式或具名构造字段。 */
  CLUX_NODE_EXPR_CONSTRUCT,
  /* 具名构造字段 .name = value，只出现在构造元素位置。 */
  CLUX_NODE_EXPR_CONSTRUCT_FIELD,
  /* 括号表达式 ( expr )，只有一个子节点，用于改变结合/优先级。 */
  CLUX_NODE_EXPR_GROUP,
  /* 元组类型 <T, U, ...>；元素数量可变，可含 rest 表达式，也可为空。 */
  CLUX_NODE_EXPR_TUPLE,
  CLUX_NODE_EXPR_FUNC_TYPE,
  /* 函数声明 / 函数定义 / 闭包（白皮书 §3.3）。是 func_type 的后退方案：
     func_type 已确认不是类型时（形参括号后不是 '->'），首 token 为 func/cfunc
     的构造必然是声明，任何不匹配都是硬错误。 */
  CLUX_NODE_EXPR_FUNC_DECL,
  CLUX_NODE_FUNC_CAPTURES,
  CLUX_NODE_FUNC_CAPTURE,
  /* 形参列表，children 为 CLUX_NODE_FUNC_PARAM。 */
  CLUX_NODE_FUNC_PARAMS,
  /* 单个形参：NAME / TYPE / DEFAULT 均可为 NULL。 */
  CLUX_NODE_FUNC_PARAM,
  /* union 定义：tag union `union Name { ... }`（白皮书 §3.7）与
     C union `cunion Name { ... }`（§3.8）在语法阶段完全相同，
     只有关键词不同，因此共用此节点，由 KEYWORD 槽区分。 */
  CLUX_NODE_EXPR_UNION,
  /* 枚举声明 enum Name: Underlying { Variant = value, ... }（白皮书 §3.6）。
     装饰器属于外层语句，不在本节点内。 */
  CLUX_NODE_EXPR_ENUM_DECL,
  /* 结构体声明 struct Name [generics]? [implements ...]? { members }
     （白皮书 §3.5）。装饰器属于外层语句，不在本节点内。 */
  CLUX_NODE_EXPR_STRUCT_DECL,
  /* 位域声明 bitfield Name: u8|u16|u32|u64 { field: width; ... }
     （白皮书 §10）。装饰器属于外层语句，不在本节点内。 */
  CLUX_NODE_EXPR_BITFIELD_DECL,
  /* 接口声明 interface Name[generics]? { func signature; ... }
     （白皮书 §3.10、§6.9）。装饰器属于外层语句，不在本节点内。 */
  CLUX_NODE_EXPR_INTERFACE_DECL,
  /* 接口成员列表 { func signature; ... }，children 为无函数体的
     CLUX_NODE_EXPR_FUNC_DECL。 */
  CLUX_NODE_INTERFACE_MEMBERS,
  /* struct 成员列表 { member ... }，children 为字段、语句声明或嵌入。 */
  CLUX_NODE_STRUCT_MEMBERS,
  /* 嵌入成员 ...Base;：TYPE 为被嵌入的类型。 */
  CLUX_NODE_STRUCT_EMBED,
  /* implements 列表 implements I[S] & J[S]，children 为接口类型表达式。 */
  CLUX_NODE_IMPLEMENTS,
  /* 枚举成员 Variant = value：NAME 必有，VALUE 缺失时为 NULL（容错）。 */
  CLUX_NODE_ENUM_VARIANT,
  /* 枚举成员列表 { Variant = value, ... }，children 为 CLUX_NODE_ENUM_VARIANT。 */
  CLUX_NODE_ENUM_VARIANTS,
  /* 类型定义的字段 name: type。struct / tag union / cunion 共用，
     因此不带具体类型前缀。 */
  CLUX_NODE_FIELD,
  /* 字段列表 { field; ... }，children 为 CLUX_NODE_FIELD。 */
  CLUX_NODE_FIELDS,
  /* 位域字段 name: width：NAME 必有，WIDTH 缺失时为 NULL（容错）。 */
  CLUX_NODE_BITFIELD_FIELD,
  /* 位域字段列表 { name: width; ... }，children 为 CLUX_NODE_BITFIELD_FIELD。 */
  CLUX_NODE_BITFIELD_FIELDS,
  /* 泛型参数列表 [T, U, ...]，children 为 CLUX_NODE_GENERIC_PARAM 或 rest。 */
  CLUX_NODE_GENERIC_PARAMS,
  /* 单个泛型参数：NAME 必有；TYPE / DEFAULT / CONSTRAINT 均可为 NULL。 */
  CLUX_NODE_GENERIC_PARAM,

  CLUX_NODE_STMT_BLOCK,
  CLUX_NODE_STMT_EXPR,
  /* return; / return expr; —— VALUE 槽在无返回值时为 NULL。 */
  CLUX_NODE_STMT_RETURN,
  /* 空语句 `;`（白皮书 §4.14）—— 叶子节点，无子节点。 */
  CLUX_NODE_STMT_EMPTY,
  /* break; / continue; —— 无子节点。 */
  CLUX_NODE_STMT_BREAK,
  CLUX_NODE_STMT_CONTINUE,
  /* defer 语句 defer |captures|? { body }（白皮书 §4.12）。
     CAPTURES 为 CLUX_NODE_FUNC_CAPTURES（无捕获时为 NULL），
     BODY 为块语句（容错时可为 NULL）。 */
  CLUX_NODE_STMT_DEFER,
  /* if 语句 if (cond) { then } [else { else } | else if ...]（白皮书 §4.4）。
     CONDITION 为条件表达式；THEN 为块语句；ELSE 可缺失，存在时为块语句
     或嵌套的 CLUX_NODE_STMT_IF（else if 链）。 */
  CLUX_NODE_STMT_IF,
  /* comptime if 语句 comptime if (cond) { ... }（白皮书 §4.15）。
     槽布局与 CLUX_NODE_STMT_IF 完全相同，条件在编译期求值；
     else if 链中的嵌套节点同样是本类型。 */
  CLUX_NODE_STMT_COMPTIME_IF,
  /* while 语句 while (cond) { body }（白皮书 §4.6）。
     CONDITION 为条件表达式；BODY 为块语句。 */
  CLUX_NODE_STMT_WHILE,
  /* for 语句 for (init; cond; update) { body }（白皮书 §4.8）。
     INIT 为变量语句或表达式（可缺失）；CONDITION / UPDATE 为表达式（可缺失）；
     BODY 为块语句。 */
  CLUX_NODE_STMT_FOR,
  /* foreach 语句 foreach ([var] name [: type]? of iterable) { body }
     （白皮书 §4.9）。KEYWORD 为 var 符号（省略时为 NULL，表示只读）；
     NAME 为迭代变量；TYPE 可缺失；ITERABLE 为被迭代表达式；BODY 为块语句。 */
  CLUX_NODE_STMT_FOREACH,
  /* comptime foreach 语句 comptime foreach (...) { ... }（白皮书 §4.15）。
     槽布局与 CLUX_NODE_STMT_FOREACH 完全相同，在编译期展开。 */
  CLUX_NODE_STMT_COMPTIME_FOREACH,
  /* do-while 语句 do { body } while (cond);（白皮书 §4.7）。
     BODY 为块语句；CONDITION 为条件表达式；末尾 ';' 属于语句。 */
  CLUX_NODE_STMT_DO_WHILE,
  /* switch 语句 switch (value) { branch... }（白皮书 §4.5）。
     VALUE 为被匹配表达式；BODY 为分支列表（CLUX_NODE_SWITCH_BRANCHES）。 */
  CLUX_NODE_STMT_SWITCH,
  /* switch 分支列表 { branch... }，children 为 CLUX_NODE_SWITCH_BRANCH。 */
  CLUX_NODE_SWITCH_BRANCHES,
  /* 单个 switch 分支：PATTERNS 为模式列表（default 分支为 NULL）；
     BODY 为块语句。 */
  CLUX_NODE_SWITCH_BRANCH,
  /* switch 模式列表 (pattern, ...)，children 为模式表达式。 */
  CLUX_NODE_SWITCH_PATTERNS,

  /* 辅助节点：装饰器 [[ <expr> ]]（白皮书 §3.2、§9.1）。
     不在主节点树上，由顶层声明、struct 字段/方法等特定语句主动调用，
     作为它们的子节点挂载。 */
  CLUX_NODE_DECORATOR,

  /* 函数语句 <decorator>* <func_decl>（白皮书 §3.3）。
     FUNC_DECL 槽存函数声明节点，其后为按源码顺序排列的装饰器。 */
  CLUX_NODE_STMT_FUNC,

  /* 变量定义语句 <decorator>* (var|using) name [: type]? = expr;
     （白皮书 §4.1、§4.12.1）。KEYWORD 区分 var 与 using，
     TYPE 可缺失（由初值推断），其后为按源码顺序排列的装饰器。 */
  CLUX_NODE_STMT_VAR,

  /* 类型别名语句 <decorator>* type name [generics]? = expr;（白皮书 §3.11）。
     GENERICS 可缺失；VALUE 槽存别名指向的类型表达式，
     其后为按源码顺序排列的装饰器。 */
  CLUX_NODE_STMT_TYPE,

  /* 枚举语句 <decorator>* <enum_decl>（白皮书 §3.6）。
     ENUM_DECL 槽存枚举声明，其后为按源码顺序排列的装饰器。 */
  CLUX_NODE_STMT_ENUM,

  /* union 语句 <decorator>* <union_decl>（白皮书 §3.7、§3.8）。
     UNION_DECL 槽存 union/cunion 声明，其后为按源码顺序排列的装饰器。 */
  CLUX_NODE_STMT_UNION,

  /* 结构体语句 <decorator>* <struct_decl>（白皮书 §3.5）。
     STRUCT_DECL 槽存结构体声明，其后为按源码顺序排列的装饰器。 */
  CLUX_NODE_STMT_STRUCT,

  /* 位域语句 <decorator>* <bitfield_decl>（白皮书 §10）。
     BITFIELD_DECL 槽存位域声明，其后为按源码顺序排列的装饰器。 */
  CLUX_NODE_STMT_BITFIELD,

  /* 测试块 test "Suite.Name" { body }（白皮书 §3.14、§11.1）。
     NAME 为字符串字面量；BODY 为块语句。test 不是关键字，只在
     `test` 后接字符串字面量与块时按测试块解析。 */
  CLUX_NODE_STMT_TEST,

  /* 导入语句 import <alias> from "<path>";（白皮书 §8.2）。
     ALIAS 为模块别名标识符；PATH 为字符串字面量路径。 */
  CLUX_NODE_STMT_IMPORT,

  /* 重新导出语句 export * from "<path>";（白皮书 §8.4）。
     PATH 为字符串字面量路径。 */
  CLUX_NODE_STMT_REEXPORT,

  /* 动态库声明 extern lib <alias> from "<path>";（白皮书 §8.8）。
     ALIAS 为库别名标识符；PATH 为字符串字面量（"*" 表示当前进程）。 */
  CLUX_NODE_STMT_EXTERN_LIB,

  /* 外部 C 函数声明 [export]? extern cfunc <lib>::<name>(params): ret;
     （白皮书 §8.9）。EXPORT 可缺失；LIB / NAME 为限定名的两段；
     PARAMS 为形参列表；RETURN 为返回类型。 */
  CLUX_NODE_STMT_EXTERN_CFUNC,

  /* 外部变量声明 [export]? extern var <lib>::<name>: type;（白皮书 §8.10）。
     EXPORT 可缺失；LIB / NAME 为限定名的两段；TYPE 为变量类型。 */
  CLUX_NODE_STMT_EXTERN_VAR,

  /* 编译期断言 static_assert(cond, "msg");（白皮书 §4.16）。
     CONDITION 为条件表达式；MESSAGE 为字符串字面量；
     SOURCE 为条件表达式源码片段（CLUX_NODE_SOURCE_TEXT）。 */
  CLUX_NODE_STMT_STATIC_ASSERT,

  /* 测试断言 assert(expr);（白皮书 §11.3）。
     CONDITION 为断言表达式；SOURCE 为表达式源码片段
     （CLUX_NODE_SOURCE_TEXT），失败诊断直接引用。 */
  CLUX_NODE_STMT_ASSERT,

  /* 接口语句 <decorator>* [export]? <interface_decl>（白皮书 §3.10）。
     INTERFACE_DECL 槽存接口声明；EXPORT 可缺失；其后为装饰器。 */
  CLUX_NODE_STMT_INTERFACE,
};
typedef enum _node_kind_t node_kind_t;
typedef struct _node_t node_t;
node_t *create_node(allocator_t *allocator, node_kind_t kind);
void delete_node(node_t **node);
node_kind_t node_get_kind(node_t *self);
vec_t *node_get_children(node_t *self);
const location_t *node_get_location(node_t *self);
node_t *node_get_parent(node_t *self);
void node_set_parent(node_t *self, node_t *parent);
void node_set_location(node_t *self, const location_t *loc);
void node_resize_children(node_t *self, size_t size);
node_t *node_set_child(node_t *self, size_t idx, node_t *child);
node_t *node_get_child(node_t *self, size_t idx);
void node_add_child(node_t *self, node_t *child);

/* JSON 输出辅助：节点自己的 to_json 传入具体的子节点写出函数，
   由调用方按 child 的类型白名单决定用哪个函数。 */
typedef void (*node_json_fn_t)(json_writer_t *writer, node_t *node);
void node_json_begin(json_writer_t *writer, node_t *node, const char *kind);
void node_json_end(json_writer_t *writer);
void node_json_text(json_writer_t *writer, node_t *node);
void node_json_field(json_writer_t *writer,
                     const char *key,
                     node_t *child,
                     node_json_fn_t write);
void node_json_children(json_writer_t *writer,
                        const char *key,
                        node_t *node,
                        size_t start,
                        node_json_fn_t write);

/* 各节点自己的 JSON 实现（定义在对应 node_*.c）。
   父节点按 child 的实际类型选择其中一个函数，不做通用分派。 */
void symbol_node_to_json(json_writer_t *writer, node_t *self);
void program_node_to_json(json_writer_t *writer, node_t *self);
void literal_nil_node_to_json(json_writer_t *writer, node_t *self);
void literal_undefined_node_to_json(json_writer_t *writer, node_t *self);
void literal_string_node_to_json(json_writer_t *writer, node_t *self);
void literal_character_node_to_json(json_writer_t *writer, node_t *self);
void literal_numeric_node_to_json(json_writer_t *writer, node_t *self);
void literal_boolean_node_to_json(json_writer_t *writer, node_t *self);
void literal_identifier_node_to_json(json_writer_t *writer, node_t *self);
void literal_type_node_to_json(json_writer_t *writer, node_t *self);
void source_text_node_to_json(json_writer_t *writer, node_t *self);
void assign_node_to_json(json_writer_t *writer, node_t *self);
void ternary_node_to_json(json_writer_t *writer, node_t *self);
void binary_node_to_json(json_writer_t *writer, node_t *self);
void subscript_node_to_json(json_writer_t *writer, node_t *self);
void slice_node_to_json(json_writer_t *writer, node_t *self);
void rest_node_to_json(json_writer_t *writer, node_t *self);
void call_node_to_json(json_writer_t *writer, node_t *self);
void builtin_node_to_json(json_writer_t *writer, node_t *self);
void construct_node_to_json(json_writer_t *writer, node_t *self);
void construct_field_node_to_json(json_writer_t *writer, node_t *self);
void group_node_to_json(json_writer_t *writer, node_t *self);
void tuple_node_to_json(json_writer_t *writer, node_t *self);
void func_type_node_to_json(json_writer_t *writer, node_t *self);
void func_decl_node_to_json(json_writer_t *writer, node_t *self);
void func_captures_node_to_json(json_writer_t *writer, node_t *self);
void func_capture_node_to_json(json_writer_t *writer, node_t *self);
void func_params_node_to_json(json_writer_t *writer, node_t *self);
void func_param_node_to_json(json_writer_t *writer, node_t *self);
void union_node_to_json(json_writer_t *writer, node_t *self);
void enum_decl_node_to_json(json_writer_t *writer, node_t *self);
void struct_decl_node_to_json(json_writer_t *writer, node_t *self);
void bitfield_decl_node_to_json(json_writer_t *writer, node_t *self);
void interface_decl_node_to_json(json_writer_t *writer, node_t *self);
void interface_members_node_to_json(json_writer_t *writer, node_t *self);
void struct_members_node_to_json(json_writer_t *writer, node_t *self);
void struct_embed_node_to_json(json_writer_t *writer, node_t *self);
void implements_node_to_json(json_writer_t *writer, node_t *self);
void enum_variant_node_to_json(json_writer_t *writer, node_t *self);
void enum_variants_node_to_json(json_writer_t *writer, node_t *self);
void field_node_to_json(json_writer_t *writer, node_t *self);
void fields_node_to_json(json_writer_t *writer, node_t *self);
void bitfield_field_node_to_json(json_writer_t *writer, node_t *self);
void bitfield_fields_node_to_json(json_writer_t *writer, node_t *self);
void generic_params_node_to_json(json_writer_t *writer, node_t *self);
void generic_param_node_to_json(json_writer_t *writer, node_t *self);
void stmt_block_node_to_json(json_writer_t *writer, node_t *self);
void stmt_expr_node_to_json(json_writer_t *writer, node_t *self);
void stmt_return_node_to_json(json_writer_t *writer, node_t *self);
void stmt_empty_node_to_json(json_writer_t *writer, node_t *self);
void stmt_break_node_to_json(json_writer_t *writer, node_t *self);
void stmt_continue_node_to_json(json_writer_t *writer, node_t *self);
void stmt_defer_node_to_json(json_writer_t *writer, node_t *self);
void stmt_if_node_to_json(json_writer_t *writer, node_t *self);
void stmt_comptime_if_node_to_json(json_writer_t *writer, node_t *self);
void stmt_while_node_to_json(json_writer_t *writer, node_t *self);
void stmt_for_node_to_json(json_writer_t *writer, node_t *self);
void stmt_foreach_node_to_json(json_writer_t *writer, node_t *self);
void stmt_comptime_foreach_node_to_json(json_writer_t *writer, node_t *self);
void stmt_do_while_node_to_json(json_writer_t *writer, node_t *self);
void stmt_switch_node_to_json(json_writer_t *writer, node_t *self);
void switch_branches_node_to_json(json_writer_t *writer, node_t *self);
void switch_branch_node_to_json(json_writer_t *writer, node_t *self);
void switch_patterns_node_to_json(json_writer_t *writer, node_t *self);
void decorator_node_to_json(json_writer_t *writer, node_t *self);
void stmt_func_node_to_json(json_writer_t *writer, node_t *self);
void stmt_var_node_to_json(json_writer_t *writer, node_t *self);
void stmt_type_node_to_json(json_writer_t *writer, node_t *self);
void stmt_enum_node_to_json(json_writer_t *writer, node_t *self);
void stmt_union_node_to_json(json_writer_t *writer, node_t *self);
void stmt_struct_node_to_json(json_writer_t *writer, node_t *self);
void stmt_bitfield_node_to_json(json_writer_t *writer, node_t *self);
void stmt_test_node_to_json(json_writer_t *writer, node_t *self);
void stmt_import_node_to_json(json_writer_t *writer, node_t *self);
void stmt_reexport_node_to_json(json_writer_t *writer, node_t *self);
void stmt_extern_lib_node_to_json(json_writer_t *writer, node_t *self);
void stmt_extern_cfunc_node_to_json(json_writer_t *writer, node_t *self);
void stmt_extern_var_node_to_json(json_writer_t *writer, node_t *self);
void stmt_static_assert_node_to_json(json_writer_t *writer, node_t *self);
void stmt_assert_node_to_json(json_writer_t *writer, node_t *self);
void stmt_interface_node_to_json(json_writer_t *writer, node_t *self);

/* 类别分派（实现见 expression.c / statement.c），供父节点按槽位类型调用。 */
void expr_to_json(json_writer_t *writer, node_t *node);
void stmt_to_json(json_writer_t *writer, node_t *node);
#ifdef __cplusplus
}
#endif
#endif