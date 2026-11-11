#include "parser/statement.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/position.h"
#include "core/vec.h"
#include "parser/decorator.h"
#include "parser/expression.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_symbol.h"
#include "parser/parser.h"

/* 错误位置：优先指向当前未消费的 token；已到流末尾时退化为
   fallback 的终点（零宽区间）。 */
static location_t stmt_error_location(parser_t *parser,
                                      const location_t *fallback) {
  token_t *bad = parser_peek_token(parser);
  location_t loc;
  if (bad) {
    loc = *token_get_location(bad);
  } else {
    loc = *fallback;
    loc.begin = fallback->end;
  }
  loc.filename = parser_get_filename(parser);
  return loc;
}

/* 块语句 { stmt* }。调用前当前 token 不一定是 '{'，不是时返回 NULL 且游标不动。
   语句之间不需要分隔符（表达式语句自带 ';'），块尾的 '}' 由本层消费。
   块本身**不携带 ';'**：`{ ... };` 中的 ';' 留在块之后，作为独立的
   空语句由语句分派解析。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_block(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    return NULL;
  }
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '{' */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_block_node(allocator);
  const location_t *end_loc = &open_loc;

  for (;;) {
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (!token) {
      /* 未闭合：保留已解析的语句，报错后结束。 */
      location_t loc = stmt_error_location(parser, end_loc);
      parser_push_error(
          parser, create_error(allocator, &loc, "expected '}' to close block"));
      break;
    }
    if (token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
      end_loc = token_get_location(token);
      parser_next(parser);
      break;
    }
    node_t *stmt = parser_read_stmt(allocator, parser);
    if (!stmt) {
      /* 无法解析语句：报错并跳过该 token，避免在同一位置反复失败。 */
      location_t loc = stmt_error_location(parser, end_loc);
      parser_push_error(
          parser, create_error(allocator, &loc, "expected statement in block"));
      parser_next(parser);
      continue;
    }
    stmt_block_node_add_stmt(node, stmt);
    end_loc = node_get_location(stmt);
  }

  location_t loc = open_loc;
  loc.end = end_loc->end;
  node_set_location(node, &loc);
  return node;
}

/* 表达式语句 expr ';'。缺少 ';' 时报错但不消费后续 token：
   表达式区间就是语句区间，游标停在表达式之后，由调用方决定如何恢复。 */
node_t *parser_read_stmt_expr(allocator_t *allocator, parser_t *parser) {
  node_t *expr = parser_read_expr(allocator, parser);
  if (!expr) {
    return NULL;
  }
  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  location_t loc = *node_get_location(expr);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    loc.end = token_get_location(token)->end;
    parser_next(parser);
  } else {
    location_t err = stmt_error_location(parser, node_get_location(expr));
    parser_push_error(parser,
                      create_error(allocator,
                                   &err,
                                   "expected ';' after expression statement"));
  }
  loc.filename = parser_get_filename(parser);

  node_t *stmt = create_stmt_expr_node(allocator);
  stmt_expr_node_set_expr(stmt, expr);
  node_set_location(stmt, &loc);
  return stmt;
}

/* 空语句 `;`（白皮书 §4.14）。调用前当前 token 不是 ';' 时返回 NULL
   且游标不动。空语句不产生操作，只占一个语句位置。 */
node_t *parser_read_stmt_empty(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser); /* ';' */

  node_t *node = create_stmt_empty_node(allocator);
  node_set_location(node, &loc);
  return node;
}

/* return; / return expr;（白皮书 §4.11）。
   调用前当前 token 不一定是 return，不是时返回 NULL 且游标不动。
   空返回值与带返回值都合法，返回表达式必须能转换为声明返回类型，
   该检查属于语义阶段。出错时按容错约定返回不完整节点并 push_error。 */
node_t *parser_read_stmt_return(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "return")) {
    return NULL;
  }
  location_t keyword_loc = *token_get_location(token);
  parser_next(parser); /* return */
  parser_skip_trivia(parser);

  node_t *node = create_stmt_return_node(allocator);
  node_set_location(node, &keyword_loc);

  token = parser_peek_token(parser);
  location_t loc = keyword_loc;
  if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    /* return; */
    loc.end = token_get_location(token)->end;
    parser_next(parser);
  } else {
    node_t *value = parser_read_expr(allocator, parser);
    if (!value) {
      location_t err = stmt_error_location(parser, &keyword_loc);
      parser_push_error(
          parser,
          create_error(allocator, &err, "expected expression after 'return'"));
      return node;
    }
    stmt_return_node_set_value(node, value);
    loc.end = node_get_location(value)->end;
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
      loc.end = token_get_location(token)->end;
      parser_next(parser);
    } else {
      location_t err = stmt_error_location(parser, node_get_location(value));
      parser_push_error(
          parser,
          create_error(allocator, &err, "expected ';' after return statement"));
    }
  }
  loc.filename = parser_get_filename(parser);
  node_set_location(node, &loc);
  return node;
}

/* break; / continue;（白皮书 §4.10）。两者语法相同，只有节点类型不同。
   只作用于当前最近一层循环，该约束属于语义阶段。
   调用前当前 token 不一定是 keyword，不是时返回 NULL 且游标不动。 */
static node_t *read_jump_stmt(allocator_t *allocator,
                              parser_t *parser,
                              const char *keyword,
                              node_t *(*create)(allocator_t *)) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, keyword)) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser); /* break / continue */
  parser_skip_trivia(parser);

  node_t *node = create(allocator);
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    loc.end = token_get_location(token)->end;
    parser_next(parser);
  } else {
    location_t err = stmt_error_location(parser, &loc);
    parser_push_error(
        parser,
        create_error(allocator, &err, "expected ';' after '%s'", keyword));
  }
  loc.filename = parser_get_filename(parser);
  node_set_location(node, &loc);
  return node;
}

node_t *parser_read_stmt_break(allocator_t *allocator, parser_t *parser) {
  return read_jump_stmt(allocator, parser, "break", create_stmt_break_node);
}

node_t *parser_read_stmt_continue(allocator_t *allocator, parser_t *parser) {
  return read_jump_stmt(
      allocator, parser, "continue", create_stmt_continue_node);
}

/* 读取 0 个或多个前导装饰器，按源码顺序追加到 node 的 children。
   add 由调用方按语句类型提供，避免在这里按节点种类分派。
   parser_peek_decorator 已确认 `[[` 存在，因此每次必然解析成功。 */
typedef void (*decorator_adder_t)(node_t *node, node_t *decorator);

static void read_leading_decorators(allocator_t *allocator,
                                    parser_t *parser,
                                    node_t *node,
                                    decorator_adder_t add) {
  while (parser_peek_decorator(parser)) {
    add(node, parser_read_decorator(allocator, parser));
    parser_skip_trivia(parser);
  }
}

static token_t *peek_next_non_trivia(parser_t *parser, size_t pos);

/* 返回 pos 起第一个非 trivia token 的位置；越界返回 token 总数。 */
static size_t next_non_trivia_position(parser_t *parser, size_t pos) {
  vec_t *tokens = parser_get_tokens(parser);
  size_t count = vec_get_size(tokens);
  while (pos < count) {
    token_t *token = vec_get(tokens, pos);
    token_kind_t kind = token_get_kind(token);
    if (kind != CLUX_TOKEN_WHITESPACE && kind != CLUX_TOKEN_COMMENT &&
        kind != CLUX_TOKEN_MULTILINE_COMMENT) {
      return pos;
    }
    pos++;
  }
  return count;
}

/* 当前 token 是 export 时消费并返回符号节点；否则返回 NULL 且游标不动。 */
static node_t *read_export(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "export")) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser); /* export */
  parser_skip_trivia(parser);
  node_t *node = create_symbol_node(allocator);
  node_set_location(node, &loc);
  return node;
}

/* 跳过装饰器与可选 export 后返回声明关键字 token（不消费）。 */
static token_t *peek_decl_keyword(parser_t *parser) {
  size_t pos = parser_get_position_after_decorators(parser);
  token_t *token = peek_next_non_trivia(parser, pos);
  if (token_is(token, CLUX_TOKEN_KEYWORD, "export")) {
    pos = next_non_trivia_position(parser, pos + 1);
    token = peek_next_non_trivia(parser, pos);
  }
  return token;
}

/* 当前 token 是 comptime 且其后（跳过 trivia）是 func/cfunc。 */
static bool comptime_starts_func(parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "comptime")) {
    return false;
  }
  token_t *after =
      peek_next_non_trivia(parser, parser_get_position(parser) + 1);
  return token_is(after, CLUX_TOKEN_KEYWORD, "func") ||
         token_is(after, CLUX_TOKEN_KEYWORD, "cfunc");
}

/* 函数语句 <decorator>* [export]? [comptime]? <func_decl>
   （白皮书 §3.3、§3.4.1、§8.1）。
   调用前当前 token 不一定是装饰器、export、comptime 或 func/cfunc，不是时
   返回 NULL 且游标不动。comptime 只有在后接 func/cfunc 时才由本函数认领，
   以便 `comptime if` / `comptime foreach` / `comptime var` 留给其他语句。
   本阶段不做名字检查；装饰器按源码顺序挂在函数声明之后。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_func(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  bool has_decorators = parser_peek_decorator(parser);
  bool has_export = token_is(token, CLUX_TOKEN_KEYWORD, "export");
  if (!has_decorators && !has_export) {
    if (!comptime_starts_func(parser) &&
        !token_is(token, CLUX_TOKEN_KEYWORD, "func") &&
        !token_is(token, CLUX_TOKEN_KEYWORD, "cfunc")) {
      return NULL;
    }
  }
  location_t begin_loc = *token_get_location(token);

  node_t *node = create_stmt_func_node(allocator);
  node_set_location(node, &begin_loc);

  read_leading_decorators(
      allocator, parser, node, stmt_func_node_add_decorator);

  /* 可选 export 标记，位于装饰器之后、comptime 之前。 */
  node_t *export_node = read_export(allocator, parser);
  if (export_node) {
    stmt_func_node_set_export(node, export_node);
  }

  /* 可选 comptime 标记。装饰器后若不是 comptime func，保留 comptime
     不消费，交给函数声明路径报错（装饰器不适用于 comptime if/foreach）。 */
  if (comptime_starts_func(parser)) {
    token = parser_peek_token(parser);
    location_t comptime_loc = *token_get_location(token);
    node_t *comptime = create_symbol_node(allocator);
    node_set_location(comptime, &comptime_loc);
    stmt_func_node_set_comptime(node, comptime);
    parser_next(parser); /* comptime */
    parser_skip_trivia(parser);
  }

  /* 函数声明。 */
  node_t *func_decl = parser_read_expr_func_decl(allocator, parser);
  if (!func_decl) {
    location_t loc = stmt_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(allocator,
                     &loc,
                     "expected function declaration in function statement"));
    return node;
  }
  stmt_func_node_set_func_decl(node, func_decl);

  location_t loc = begin_loc;
  loc.end = node_get_location(func_decl)->end;
  loc.filename = parser_get_filename(parser);
  node_set_location(node, &loc);
  return node;
}

/* 返回 pos 之后第一个非 trivia token；越界返回 NULL。 */
static token_t *peek_next_non_trivia(parser_t *parser, size_t pos) {
  vec_t *tokens = parser_get_tokens(parser);
  size_t count = vec_get_size(tokens);
  while (pos < count) {
    token_t *token = vec_get(tokens, pos);
    token_kind_t kind = token_get_kind(token);
    if (kind != CLUX_TOKEN_WHITESPACE && kind != CLUX_TOKEN_COMMENT &&
        kind != CLUX_TOKEN_MULTILINE_COMMENT) {
      return token;
    }
    pos++;
  }
  return NULL;
}

/* 变量定义语句 <decorator>* [export]? [comptime]? (var|using) name
   [: type]? = expr;（白皮书 §4.1、§3.4、§4.12.1、§8.1）。
   调用前当前 token 不一定是装饰器、export、comptime 或 var/using，不是时
   返回 NULL 且游标不动。comptime 只有在后接 var/using 时才由本函数认领，
   以便 `comptime if` 留给 comptime if 语句。
   类型只取后缀层，避免把 `=` 当作赋值运算符吃掉；初值走顶层入口。
   `comptime using` 语法上可解析，是否合法由语义阶段检查。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_var(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  bool has_decorators = parser_peek_decorator(parser);

  /* 跳过装饰器后看关键字；若是 export 再往后看，若是 comptime 再往后看
     一个非 trivia token。 */
  size_t lookahead_pos = has_decorators
                             ? parser_get_position_after_decorators(parser)
                             : parser_get_position(parser);
  token_t *lookahead =
      has_decorators ? parser_peek_token_after_decorators(parser) : token;
  bool has_export = token_is(lookahead, CLUX_TOKEN_KEYWORD, "export");
  if (has_export) {
    lookahead_pos = next_non_trivia_position(parser, lookahead_pos + 1);
    lookahead = peek_next_non_trivia(parser, lookahead_pos);
  }
  bool is_var_stmt = token_is(lookahead, CLUX_TOKEN_KEYWORD, "var") ||
                     token_is(lookahead, CLUX_TOKEN_KEYWORD, "using");
  if (!is_var_stmt && token_is(lookahead, CLUX_TOKEN_KEYWORD, "comptime")) {
    size_t after_pos = next_non_trivia_position(parser, lookahead_pos + 1);
    token_t *after = peek_next_non_trivia(parser, after_pos);
    is_var_stmt = token_is(after, CLUX_TOKEN_KEYWORD, "var") ||
                  token_is(after, CLUX_TOKEN_KEYWORD, "using");
  }
  /* 保留旧契约：装饰器后不是 var/using 时由本函数报错；但 comptime/export
     后不是 var/using（如 comptime if、export func）必须返回 NULL，
     交给其他语句处理。 */
  bool claim_anyway = has_decorators &&
                      !token_is(lookahead, CLUX_TOKEN_KEYWORD, "comptime") &&
                      !has_export;
  if (!is_var_stmt && !claim_anyway) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);

  node_t *node = create_stmt_var_node(allocator);
  node_set_location(node, &begin_loc);

  read_leading_decorators(allocator, parser, node, stmt_var_node_add_decorator);

  /* 可选 export 标记，位于装饰器之后、comptime 之前。 */
  node_t *export_node = read_export(allocator, parser);
  if (export_node) {
    stmt_var_node_set_export(node, export_node);
  }

  /* 可选 comptime 标记。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_KEYWORD, "comptime")) {
    location_t comptime_loc = *token_get_location(token);
    node_t *comptime = create_symbol_node(allocator);
    node_set_location(comptime, &comptime_loc);
    stmt_var_node_set_comptime(node, comptime);
    parser_next(parser); /* comptime */
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }

  /* var / using 关键字。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "var") &&
      !token_is(token, CLUX_TOKEN_KEYWORD, "using")) {
    location_t loc = stmt_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(allocator,
                     &loc,
                     "expected 'var' or 'using' in variable statement"));
    return node;
  }
  location_t keyword_loc = *token_get_location(token);
  node_t *keyword = create_symbol_node(allocator);
  node_set_location(keyword, &keyword_loc);
  stmt_var_node_set_keyword(node, keyword);
  parser_next(parser); /* var / using */
  parser_skip_trivia(parser);

  /* 变量名。 */
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    location_t loc = stmt_error_location(parser, &keyword_loc);
    parser_push_error(
        parser,
        create_error(
            allocator, &loc, "expected variable name in variable statement"));
    return node;
  }
  stmt_var_node_set_name(node, name);
  parser_skip_trivia(parser);

  /* 可选类型标注 : type。只取后缀层，`=` 留给初值。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    parser_next(parser); /* ':' */
    parser_skip_trivia(parser);
    node_t *type = parser_read_expr_postfix(allocator, parser);
    if (!type) {
      location_t loc = stmt_error_location(parser, node_get_location(name));
      parser_push_error(
          parser, create_error(allocator, &loc, "expected type after ':'"));
      return node;
    }
    stmt_var_node_set_type(node, type);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }

  /* 初值必须显式给出。 */
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "=")) {
    location_t loc = stmt_error_location(parser, node_get_location(name));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '=' before initializer"));
    return node;
  }
  parser_next(parser); /* '=' */
  parser_skip_trivia(parser);
  node_t *value = parser_read_expr(allocator, parser);
  if (!value) {
    location_t loc = stmt_error_location(parser, node_get_location(name));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected initializer expression"));
    return node;
  }
  stmt_var_node_set_value(node, value);
  /* 语句区间从首个装饰器/关键字起点开始，终点随 ';' 更新。 */
  location_t loc = begin_loc;
  loc.end = node_get_location(value)->end;
  parser_skip_trivia(parser);

  /* 结尾 ';'。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    loc.end = token_get_location(token)->end;
    parser_next(parser);
  } else {
    location_t err = stmt_error_location(parser, node_get_location(value));
    parser_push_error(
        parser,
        create_error(allocator, &err, "expected ';' after variable statement"));
  }
  loc.filename = parser_get_filename(parser);
  node_set_location(node, &loc);
  return node;
}

/* 类型别名语句 <decorator>* [export]? type name [generics]? = expr;
   （白皮书 §3.11、§8.1）。
   调用前当前 token 不一定是装饰器、export 或 type，不是时返回 NULL 且游标
   不动。泛型参数列表复用函数声明的解析；别名指向的类型表达式走顶层入口。
   出错时按容错约定返回不完整节点并 push_error。 */
node_t *parser_read_stmt_type(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  bool has_decorators = parser_peek_decorator(parser);
  bool has_export = token_is(token, CLUX_TOKEN_KEYWORD, "export");
  if (!has_decorators) {
    token_t *lookahead = token;
    if (has_export) {
      size_t pos =
          next_non_trivia_position(parser, parser_get_position(parser) + 1);
      lookahead = peek_next_non_trivia(parser, pos);
    }
    if (!token_is(lookahead, CLUX_TOKEN_KEYWORD, "type")) {
      return NULL;
    }
  }
  location_t begin_loc = *token_get_location(token);

  node_t *node = create_stmt_type_node(allocator);
  node_set_location(node, &begin_loc);

  read_leading_decorators(
      allocator, parser, node, stmt_type_node_add_decorator);

  /* 可选 export 标记，位于装饰器之后。 */
  node_t *export_node = read_export(allocator, parser);
  if (export_node) {
    stmt_type_node_set_export(node, export_node);
  }

  /* type 关键字。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "type")) {
    location_t loc = stmt_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected 'type' in type statement"));
    return node;
  }
  location_t keyword_loc = *token_get_location(token);
  parser_next(parser); /* type */
  parser_skip_trivia(parser);

  /* 别名。 */
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    location_t loc = stmt_error_location(parser, &keyword_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected type name in type statement"));
    return node;
  }
  stmt_type_node_set_name(node, name);
  parser_skip_trivia(parser);

  /* 泛型参数列表 [T, U]（可选）。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "[")) {
    node_t *generics = parser_read_func_decl_generics(allocator, parser);
    stmt_type_node_set_generics(node, generics);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }

  /* '='。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "=")) {
    location_t loc = stmt_error_location(parser, node_get_location(name));
    parser_push_error(
        parser, create_error(allocator, &loc, "expected '=' after type name"));
    return node;
  }
  parser_next(parser); /* '=' */
  parser_skip_trivia(parser);

  /* 别名指向的类型表达式。 */
  node_t *value = parser_read_expr(allocator, parser);
  if (!value) {
    location_t loc = stmt_error_location(parser, node_get_location(name));
    parser_push_error(
        parser, create_error(allocator, &loc, "expected type expression"));
    return node;
  }
  stmt_type_node_set_value(node, value);
  location_t loc = begin_loc;
  loc.end = node_get_location(value)->end;
  parser_skip_trivia(parser);

  /* 结尾 ';'。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    loc.end = token_get_location(token)->end;
    parser_next(parser);
  } else {
    location_t err = stmt_error_location(parser, node_get_location(value));
    parser_push_error(
        parser,
        create_error(allocator, &err, "expected ';' after type statement"));
  }
  loc.filename = parser_get_filename(parser);
  node_set_location(node, &loc);
  return node;
}

/* 枚举语句 <decorator>* [export]? <enum_decl>（白皮书 §3.6、§8.1）。
   调用前当前 token 不一定是装饰器、export 或 enum，不是时返回 NULL 且游标
   不动。装饰器按源码顺序挂在枚举声明之后。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_enum(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  bool has_decorators = parser_peek_decorator(parser);
  bool has_export = token_is(token, CLUX_TOKEN_KEYWORD, "export");
  if (!has_decorators) {
    token_t *lookahead = token;
    if (has_export) {
      size_t pos =
          next_non_trivia_position(parser, parser_get_position(parser) + 1);
      lookahead = peek_next_non_trivia(parser, pos);
    }
    if (!token_is(lookahead, CLUX_TOKEN_KEYWORD, "enum")) {
      return NULL;
    }
  }
  location_t begin_loc = *token_get_location(token);

  node_t *node = create_stmt_enum_node(allocator);
  node_set_location(node, &begin_loc);

  read_leading_decorators(
      allocator, parser, node, stmt_enum_node_add_decorator);

  /* 可选 export 标记，位于装饰器之后。 */
  node_t *export_node = read_export(allocator, parser);
  if (export_node) {
    stmt_enum_node_set_export(node, export_node);
  }

  /* 枚举声明。 */
  node_t *enum_decl = parser_read_expr_enum_decl(allocator, parser);
  if (!enum_decl) {
    location_t loc = stmt_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(
            allocator, &loc, "expected enum declaration in enum statement"));
    return node;
  }
  stmt_enum_node_set_enum_decl(node, enum_decl);

  location_t loc = begin_loc;
  loc.end = node_get_location(enum_decl)->end;
  loc.filename = parser_get_filename(parser);
  node_set_location(node, &loc);
  return node;
}

/* union 语句 <decorator>* [export]? <union_decl>（白皮书 §3.7、§3.8、§8.1）。
   tag union 与 cunion 共用同一节点，由声明内的 KEYWORD 槽区分。
   调用前当前 token 不一定是装饰器、export 或 union/cunion，不是时返回 NULL
   且游标不动。出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_union(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  bool has_decorators = parser_peek_decorator(parser);
  bool has_export = token_is(token, CLUX_TOKEN_KEYWORD, "export");
  if (!has_decorators) {
    token_t *lookahead = token;
    if (has_export) {
      size_t pos =
          next_non_trivia_position(parser, parser_get_position(parser) + 1);
      lookahead = peek_next_non_trivia(parser, pos);
    }
    if (!token_is(lookahead, CLUX_TOKEN_KEYWORD, "union") &&
        !token_is(lookahead, CLUX_TOKEN_KEYWORD, "cunion")) {
      return NULL;
    }
  }
  location_t begin_loc = *token_get_location(token);

  node_t *node = create_stmt_union_node(allocator);
  node_set_location(node, &begin_loc);

  read_leading_decorators(
      allocator, parser, node, stmt_union_node_add_decorator);

  /* 可选 export 标记，位于装饰器之后。 */
  node_t *export_node = read_export(allocator, parser);
  if (export_node) {
    stmt_union_node_set_export(node, export_node);
  }

  /* union / cunion 声明。 */
  node_t *union_decl = parser_read_expr_union(allocator, parser);
  if (!union_decl) {
    location_t loc = stmt_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(
            allocator, &loc, "expected union declaration in union statement"));
    return node;
  }
  stmt_union_node_set_union_decl(node, union_decl);

  location_t loc = begin_loc;
  loc.end = node_get_location(union_decl)->end;
  loc.filename = parser_get_filename(parser);
  node_set_location(node, &loc);
  return node;
}

/* 结构体语句 <decorator>* [export]? <struct_decl>（白皮书 §3.5、§8.1）。
   调用前当前 token 不一定是装饰器、export 或 struct，不是时返回 NULL 且游标
   不动。装饰器按源码顺序挂在结构体声明之后。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_struct(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  bool has_decorators = parser_peek_decorator(parser);
  bool has_export = token_is(token, CLUX_TOKEN_KEYWORD, "export");
  if (!has_decorators) {
    token_t *lookahead = token;
    if (has_export) {
      size_t pos =
          next_non_trivia_position(parser, parser_get_position(parser) + 1);
      lookahead = peek_next_non_trivia(parser, pos);
    }
    if (!token_is(lookahead, CLUX_TOKEN_KEYWORD, "struct")) {
      return NULL;
    }
  }
  location_t begin_loc = *token_get_location(token);

  node_t *node = create_stmt_struct_node(allocator);
  node_set_location(node, &begin_loc);

  read_leading_decorators(
      allocator, parser, node, stmt_struct_node_add_decorator);

  /* 可选 export 标记，位于装饰器之后。 */
  node_t *export_node = read_export(allocator, parser);
  if (export_node) {
    stmt_struct_node_set_export(node, export_node);
  }

  /* 结构体声明。 */
  node_t *struct_decl = parser_read_expr_struct_decl(allocator, parser);
  if (!struct_decl) {
    location_t loc = stmt_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(allocator,
                     &loc,
                     "expected struct declaration in struct statement"));
    return node;
  }
  stmt_struct_node_set_struct_decl(node, struct_decl);

  location_t loc = begin_loc;
  loc.end = node_get_location(struct_decl)->end;
  loc.filename = parser_get_filename(parser);
  node_set_location(node, &loc);
  return node;
}

/* 位域语句 <decorator>* <bitfield_decl>（白皮书 §10）。
   调用前当前 token 不一定是装饰器或 bitfield，不是时返回 NULL 且游标不动。
   装饰器按源码顺序挂在位域声明之后。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_bitfield(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  bool has_decorators = parser_peek_decorator(parser);
  if (!has_decorators && !token_is(token, CLUX_TOKEN_KEYWORD, "bitfield")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);

  node_t *node = create_stmt_bitfield_node(allocator);
  node_set_location(node, &begin_loc);

  read_leading_decorators(
      allocator, parser, node, stmt_bitfield_node_add_decorator);

  /* 位域声明。 */
  node_t *bitfield_decl = parser_read_expr_bitfield_decl(allocator, parser);
  if (!bitfield_decl) {
    location_t loc = stmt_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(allocator,
                     &loc,
                     "expected bitfield declaration in bitfield statement"));
    return node;
  }
  stmt_bitfield_node_set_bitfield_decl(node, bitfield_decl);

  location_t loc = begin_loc;
  loc.end = node_get_location(bitfield_decl)->end;
  loc.filename = parser_get_filename(parser);
  node_set_location(node, &loc);
  return node;
}

/* 语句分派。调用方负责清理前导 trivia。
   装饰器可能引导函数/变量/类型/枚举/union/结构体/位域语句，因此先跳过装饰器
   看关键字。 */
node_t *parser_read_stmt(allocator_t *allocator, parser_t *parser) {
  node_t *node = parser_read_stmt_block(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_empty(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_reexport(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_extern_lib(allocator, parser);
  if (node) {
    return node;
  }
  token_t *first = parser_peek_token(parser);
  if (parser_peek_decorator(parser) ||
      token_is(first, CLUX_TOKEN_KEYWORD, "export")) {
    token_t *keyword = peek_decl_keyword(parser);
    if (token_is(keyword, CLUX_TOKEN_KEYWORD, "var") ||
        token_is(keyword, CLUX_TOKEN_KEYWORD, "using")) {
      return parser_read_stmt_var(allocator, parser);
    }
    /* 装饰器后可能是 comptime var / comptime using；若不是变量语句，
       交给函数语句路径报错（装饰器不适用于 comptime if）。 */
    if (token_is(keyword, CLUX_TOKEN_KEYWORD, "comptime")) {
      node_t *var_stmt = parser_read_stmt_var(allocator, parser);
      if (var_stmt) {
        return var_stmt;
      }
      return parser_read_stmt_func(allocator, parser);
    }
    if (token_is(keyword, CLUX_TOKEN_KEYWORD, "type")) {
      return parser_read_stmt_type(allocator, parser);
    }
    if (token_is(keyword, CLUX_TOKEN_KEYWORD, "enum")) {
      return parser_read_stmt_enum(allocator, parser);
    }
    if (token_is(keyword, CLUX_TOKEN_KEYWORD, "union") ||
        token_is(keyword, CLUX_TOKEN_KEYWORD, "cunion")) {
      return parser_read_stmt_union(allocator, parser);
    }
    if (token_is(keyword, CLUX_TOKEN_KEYWORD, "struct")) {
      return parser_read_stmt_struct(allocator, parser);
    }
    if (token_is(keyword, CLUX_TOKEN_KEYWORD, "bitfield")) {
      node_t *bitfield = parser_read_stmt_bitfield(allocator, parser);
      if (bitfield) {
        return bitfield;
      }
      /* export bitfield 暂不支持：交给函数语句路径报错。 */
      return parser_read_stmt_func(allocator, parser);
    }
    return parser_read_stmt_func(allocator, parser);
  }
  node = parser_read_stmt_import(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_var(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_type(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_func(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_enum(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_union(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_struct(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_bitfield(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_return(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_break(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_continue(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_defer(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_if(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_comptime_foreach(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_comptime_if(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_while(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_for(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_foreach(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_do_while(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_switch(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_test(allocator, parser);
  if (node) {
    return node;
  }
  return parser_read_stmt_expr(allocator, parser);
}
