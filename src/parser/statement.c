#include "parser/statement.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/position.h"
#include "parser/decorator.h"
#include "parser/expression.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
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
   块本身**不携带 ';'**：`{ ... };` 中的 ';' 留在块之后，属于后续要实现的
   空语句；在空语句落地前，块内的孤立 ';' 按无法解析的语句报错。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_block(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    return NULL;
  }
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '{' */
  parser_skip_trivia(parser);

  node_t *node = create_node(allocator, CLUX_NODE_STMT_BLOCK);
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
    node_add_child(node, stmt);
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

  node_t *stmt = create_node(allocator, CLUX_NODE_STMT_EXPR);
  node_resize_children(stmt, 1);
  node_set_child(stmt, 0, expr);
  node_set_location(stmt, &loc);
  return stmt;
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

  node_t *node = create_node(allocator, CLUX_NODE_STMT_RETURN);
  node_resize_children(node, CLUX_STMT_RETURN_CHILDREN_SIZE);
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
    node_set_child(node, CLUX_STMT_RETURN_VALUE, value);
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
                              node_kind_t kind) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, keyword)) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser); /* break / continue */
  parser_skip_trivia(parser);

  node_t *node = create_node(allocator, kind);
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
  return read_jump_stmt(allocator, parser, "break", CLUX_NODE_STMT_BREAK);
}

node_t *parser_read_stmt_continue(allocator_t *allocator, parser_t *parser) {
  return read_jump_stmt(allocator, parser, "continue", CLUX_NODE_STMT_CONTINUE);
}

/* 当前 token 是否开启一个装饰器（`[[` 两个相邻的 '['）。 */
static bool peek_decorator(parser_t *parser) {
  token_t *first = parser_peek_token(parser);
  if (!token_is(first, CLUX_TOKEN_SYMBOL, "[")) {
    return false;
  }
  return token_is(peek_token_at(parser, 1), CLUX_TOKEN_SYMBOL, "[");
}

/* 读取 0 个或多个前导装饰器，按源码顺序追加到 node 的 children。
   peek_decorator 已确认 `[[` 存在，因此每次必然解析成功。 */
static void read_leading_decorators(allocator_t *allocator,
                                    parser_t *parser,
                                    node_t *node) {
  while (peek_decorator(parser)) {
    node_add_child(node, parser_read_decorator(allocator, parser));
    parser_skip_trivia(parser);
  }
}

/* 函数语句 <decorator>* <func_decl>（白皮书 §3.3）。
   调用前当前 token 不一定是装饰器或 func/cfunc，不是时返回 NULL 且游标不动。
   本阶段不做名字检查；装饰器按源码顺序挂在函数声明之后。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_func(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  bool has_decorators = peek_decorator(parser);
  if (!has_decorators && !token_is(token, CLUX_TOKEN_KEYWORD, "func") &&
      !token_is(token, CLUX_TOKEN_KEYWORD, "cfunc")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);

  node_t *node = create_node(allocator, CLUX_NODE_STMT_FUNC);
  node_resize_children(node, CLUX_STMT_FUNC_FIRST_DECORATOR);
  node_set_location(node, &begin_loc);

  read_leading_decorators(allocator, parser, node);

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
  node_set_child(node, CLUX_STMT_FUNC_FUNC_DECL, func_decl);

  location_t loc = begin_loc;
  loc.end = node_get_location(func_decl)->end;
  loc.filename = parser_get_filename(parser);
  node_set_location(node, &loc);
  return node;
}

/* 跳过 0 个或多个装饰器，返回其后的 token（不消费任何 token）。
   装饰器内部可能出现方括号，因此按 `[` / `]` 深度扫描，深度归零后的
   下一个 token 即声明关键字。 */
static token_t *peek_token_after_decorators(parser_t *parser) {
  size_t start = parser_get_position(parser);
  token_t *token = parser_peek_token(parser);
  while (token_is(token, CLUX_TOKEN_SYMBOL, "[")) {
    int depth = 0;
    for (;;) {
      token = parser_peek_token(parser);
      if (!token) {
        break;
      }
      if (token_is(token, CLUX_TOKEN_SYMBOL, "[")) {
        depth++;
      } else if (token_is(token, CLUX_TOKEN_SYMBOL, "]")) {
        depth--;
      }
      parser_next(parser);
      if (depth == 0) {
        break;
      }
    }
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (!token) {
      break;
    }
  }
  token_t *result = parser_peek_token(parser);
  parser_set_position(parser, start);
  return result;
}

/* 变量定义语句 <decorator>* (var|using) name [: type]? = expr;
   （白皮书 §4.1、§4.12.1）。
   调用前当前 token 不一定是装饰器或 var/using，不是时返回 NULL 且游标不动。
   类型只取后缀层，避免把 `=` 当作赋值运算符吃掉；初值走顶层入口。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_stmt_var(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  bool has_decorators = peek_decorator(parser);
  if (!has_decorators && !token_is(token, CLUX_TOKEN_KEYWORD, "var") &&
      !token_is(token, CLUX_TOKEN_KEYWORD, "using")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);

  node_t *node = create_node(allocator, CLUX_NODE_STMT_VAR);
  node_resize_children(node, CLUX_STMT_VAR_FIRST_DECORATOR);
  node_set_location(node, &begin_loc);

  read_leading_decorators(allocator, parser, node);

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
  node_t *keyword = create_node(allocator, CLUX_NODE_SYMBOL);
  node_set_location(keyword, &keyword_loc);
  node_set_child(node, CLUX_STMT_VAR_KEYWORD, keyword);
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
  node_set_child(node, CLUX_STMT_VAR_NAME, name);
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
    node_set_child(node, CLUX_STMT_VAR_TYPE, type);
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
  node_set_child(node, CLUX_STMT_VAR_VALUE, value);
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

/* 语句分派。调用方负责清理前导 trivia。
   装饰器既可能引导函数语句也可能引导变量语句，因此先跳过装饰器看关键字。 */
node_t *parser_read_stmt(allocator_t *allocator, parser_t *parser) {
  node_t *node = parser_read_stmt_block(allocator, parser);
  if (node) {
    return node;
  }
  if (peek_decorator(parser)) {
    token_t *keyword = peek_token_after_decorators(parser);
    if (token_is(keyword, CLUX_TOKEN_KEYWORD, "var") ||
        token_is(keyword, CLUX_TOKEN_KEYWORD, "using")) {
      return parser_read_stmt_var(allocator, parser);
    }
    return parser_read_stmt_func(allocator, parser);
  }
  node = parser_read_stmt_var(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_stmt_func(allocator, parser);
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
  return parser_read_stmt_expr(allocator, parser);
}
