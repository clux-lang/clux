#include "parser/function.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/parser.h"
#include "parser/statement.h"

/* 用首尾子节点的位置拼出表达式自身的区间：起点取 begin->begin，
   终点取 end->end。子节点位置本身不含前后 trivia，因此表达式区间
   也不会携带空白/注释。 */
static void node_set_location_span(node_t *node,
                                   const location_t *begin,
                                   const location_t *end) {
  location_t loc = *begin;
  loc.end = end->end;
  node_set_location(node, &loc);
}

/* 错误位置：优先指向当前未消费的 token；已到流末尾时退化为
   fallback 的终点（零宽区间）。 */
static location_t function_error_location(parser_t *parser,
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

/* 解析形参列表 ( param, ... )。调用前当前 token 不一定是 '('，
   不是时返回 NULL 且游标不动。形参可以是 rest 表达式。
   单个形参支持两种写法：
     name: type          —— 有名字
     type                —— 函数类型里的裸类型
   allow_default 为 true 时额外接受 `name: type = expr`：只有闭包的捕获
   列表允许默认值，函数参数一律不允许。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *
read_func_params(allocator_t *allocator, parser_t *parser, bool allow_default) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
    return NULL;
  }
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  node_t *params = create_node(allocator, CLUX_NODE_FUNC_PARAMS);
  node_set_location_span(params, &open_loc, &open_loc);
  const location_t *end_loc = &open_loc;

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    end_loc = token_get_location(token);
    parser_next(parser);
    node_set_location_span(params, &open_loc, end_loc);
    return params;
  }

  for (;;) {
    /* rest 形参：...Args */
    node_t *param = parser_read_expr_rest(allocator, parser);
    if (!param) {
      node_t *first = parser_read_expr_postfix(allocator, parser);
      if (!first) {
        location_t loc = function_error_location(parser, end_loc);
        parser_push_error(
            parser,
            create_error(allocator, &loc, "expected parameter in function"));
        break;
      }
      param = create_node(allocator, CLUX_NODE_FUNC_PARAM);
      node_resize_children(param, CLUX_FUNC_PARAM_CHILDREN_SIZE);
      parser_skip_trivia(parser);
      token = parser_peek_token(parser);
      if (token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
        /* name: type。类型可以是 rest 表达式（参数包形参 `args: ...Args`）。 */
        node_set_child(param, CLUX_FUNC_PARAM_NAME, first);
        parser_next(parser); /* ':' */
        parser_skip_trivia(parser);
        node_t *type = parser_read_expr_rest(allocator, parser);
        if (!type) {
          type = parser_read_expr(allocator, parser);
        }
        if (!type) {
          location_t loc = function_error_location(parser, &open_loc);
          parser_push_error(
              parser, create_error(allocator, &loc, "expected parameter type"));
        } else {
          node_set_child(param, CLUX_FUNC_PARAM_TYPE, type);
        }
      } else {
        /* 裸类型（函数类型形参）。 */
        node_set_child(param, CLUX_FUNC_PARAM_TYPE, first);
      }
      /* 捕获列表允许默认值；函数参数不允许。 */
      parser_skip_trivia(parser);
      token = parser_peek_token(parser);
      if (allow_default && token_is(token, CLUX_TOKEN_SYMBOL, "=")) {
        parser_next(parser); /* '=' */
        parser_skip_trivia(parser);
        node_t *def = parser_read_expr(allocator, parser);
        if (!def) {
          location_t loc = function_error_location(parser, &open_loc);
          parser_push_error(
              parser,
              create_error(allocator, &loc, "expected capture default value"));
        } else {
          node_set_child(param, CLUX_FUNC_PARAM_DEFAULT, def);
        }
      }
      /* 形参区间 = 名字/类型起点 .. 最后一个非空子节点终点。 */
      node_t *param_last = node_get_child(param, CLUX_FUNC_PARAM_DEFAULT);
      if (!param_last) {
        param_last = node_get_child(param, CLUX_FUNC_PARAM_TYPE);
      }
      node_set_location_span(
          param,
          node_get_location(first),
          node_get_location(param_last ? param_last : first));
    }
    node_add_child(params, param);
    end_loc = node_get_location(param);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (!token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      break;
    }
    parser_next(parser); /* ',' */
    parser_skip_trivia(parser);
  }

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = function_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ')' to close parameters"));
  }
  node_set_location_span(params, &open_loc, end_loc);
  return params;
}

/* 解析泛型参数列表 [T, U, ...]（白皮书 §6.1–§6.5）。
   调用前当前 token 必须是 '['（由调用方保证）。
   单个参数的写法：`T`、`N: comptime usize`、`W extends IWriter[W]`、
   `T = i32`，以及参数包 `...Args`。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *read_generic_params(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '[' */
  parser_skip_trivia(parser);

  node_t *params = create_node(allocator, CLUX_NODE_GENERIC_PARAMS);
  node_set_location_span(params, &open_loc, &open_loc);
  const location_t *end_loc = &open_loc;

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "]")) {
    end_loc = token_get_location(token);
    parser_next(parser);
    node_set_location_span(params, &open_loc, end_loc);
    return params;
  }

  for (;;) {
    /* 参数包 ...Args */
    node_t *param = parser_read_expr_rest(allocator, parser);
    if (!param) {
      node_t *name = parser_read_literal_identifier(allocator, parser);
      if (!name) {
        location_t loc = function_error_location(parser, end_loc);
        parser_push_error(
            parser,
            create_error(allocator, &loc, "expected generic parameter name"));
        break;
      }
      param = create_node(allocator, CLUX_NODE_GENERIC_PARAM);
      node_resize_children(param, CLUX_GENERIC_PARAM_CHILDREN_SIZE);
      node_set_child(param, CLUX_GENERIC_PARAM_NAME, name);
      parser_skip_trivia(parser);
      token = parser_peek_token(parser);
      /* 编译期值参数：N: comptime usize。
         `comptime` 是纯语法标记，不进入 AST，类型槽存 `usize`。 */
      if (token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
        parser_next(parser); /* ':' */
        parser_skip_trivia(parser);
        token = parser_peek_token(parser);
        if (token_is(token, CLUX_TOKEN_KEYWORD, "comptime")) {
          parser_next(parser); /* comptime */
          parser_skip_trivia(parser);
        }
        node_t *type = parser_read_expr(allocator, parser);
        if (!type) {
          location_t loc = function_error_location(parser, &open_loc);
          parser_push_error(
              parser,
              create_error(allocator, &loc, "expected generic parameter type"));
        } else {
          node_set_child(param, CLUX_GENERIC_PARAM_TYPE, type);
        }
        parser_skip_trivia(parser);
        token = parser_peek_token(parser);
      }
      /* 约束：W extends IWriter[W] */
      if (token_is(token, CLUX_TOKEN_KEYWORD, "extends")) {
        parser_next(parser); /* extends */
        parser_skip_trivia(parser);
        node_t *constraint = parser_read_expr(allocator, parser);
        if (!constraint) {
          location_t loc = function_error_location(parser, &open_loc);
          parser_push_error(
              parser,
              create_error(
                  allocator, &loc, "expected constraint after 'extends'"));
        } else {
          node_set_child(param, CLUX_GENERIC_PARAM_CONSTRAINT, constraint);
        }
        parser_skip_trivia(parser);
        token = parser_peek_token(parser);
      }
      /* 默认值：T = i32 */
      if (token_is(token, CLUX_TOKEN_SYMBOL, "=")) {
        parser_next(parser); /* '=' */
        parser_skip_trivia(parser);
        node_t *def = parser_read_expr(allocator, parser);
        if (!def) {
          location_t loc = function_error_location(parser, &open_loc);
          parser_push_error(
              parser,
              create_error(allocator, &loc, "expected generic default value"));
        } else {
          node_set_child(param, CLUX_GENERIC_PARAM_DEFAULT, def);
        }
      }
      /* 区间 = 名字起点 .. 最后一个非空子节点终点。 */
      node_t *param_last = node_get_child(param, CLUX_GENERIC_PARAM_DEFAULT);
      if (!param_last) {
        param_last = node_get_child(param, CLUX_GENERIC_PARAM_CONSTRAINT);
      }
      if (!param_last) {
        param_last = node_get_child(param, CLUX_GENERIC_PARAM_TYPE);
      }
      node_set_location_span(param,
                             node_get_location(name),
                             node_get_location(param_last ? param_last : name));
    }
    node_add_child(params, param);
    end_loc = node_get_location(param);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (!token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      break;
    }
    parser_next(parser); /* ',' */
    parser_skip_trivia(parser);
  }

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "]")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = function_error_location(parser, end_loc);
    parser_push_error(parser,
                      create_error(allocator,
                                   &loc,
                                   "expected ']' to close generic parameters"));
  }
  node_set_location_span(params, &open_loc, end_loc);
  return params;
}

/* 函数定义 / 函数类型 / 闭包（白皮书 §3.3、§2.7）。
   语法不是前导可判定的：`func` 之后的捕获列表与函数名都是可选的，
   真正的分歧点在形参括号之后——
     ':'  → 函数定义，后接返回类型与函数体块
     '->' → 函数类型，后接返回类型
   `cfunc` 只能构成函数类型（C ABI，无捕获、无函数体）。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_expr_func(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "func") &&
      !token_is(token, CLUX_TOKEN_KEYWORD, "cfunc")) {
    return NULL;
  }
  location_t keyword_loc = *token_get_location(token);
  bool is_cfunc = token_is(token, CLUX_TOKEN_KEYWORD, "cfunc");
  node_t *keyword = create_node(allocator, CLUX_NODE_SYMBOL);
  node_set_location(keyword, &keyword_loc);
  parser_next(parser); /* func / cfunc */

  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_FUNC);
  node_resize_children(expr, CLUX_EXPR_FUNC_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_FUNC_KEYWORD, keyword);
  node_set_location_span(expr, &keyword_loc, &keyword_loc);
  const location_t *end_loc = &keyword_loc;

  parser_skip_trivia(parser);
  token = parser_peek_token(parser);
  /* 捕获列表 |( ... )|，仅 func 可用。 */
  bool has_empty_captures =
      !is_cfunc && token_is(token, CLUX_TOKEN_SYMBOL, "||");
  if (has_empty_captures ||
      (!is_cfunc && token_is(token, CLUX_TOKEN_SYMBOL, "|"))) {
    location_t pipe_loc = *token_get_location(token);
    parser_next(parser); /* '|' 或 '||' */
    parser_skip_trivia(parser);
    node_t *captures;
    /* 空捕获列表：`||` 是单个 token，`| |` 是两个 token，都已在此闭合。 */
    bool captures_closed = has_empty_captures;
    token = parser_peek_token(parser);
    if (has_empty_captures || token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
      captures = create_node(allocator, CLUX_NODE_FUNC_PARAMS);
      node_set_location_span(captures, &pipe_loc, &pipe_loc);
      if (!has_empty_captures) {
        parser_next(parser); /* 第二个 '|' */
      }
      captures_closed = true;
    } else {
      captures = read_func_params(allocator, parser, true);
    }
    if (captures) {
      if (!captures_closed) {
        parser_skip_trivia(parser);
        token = parser_peek_token(parser);
        if (token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
          parser_next(parser); /* '|' */
        } else {
          location_t loc = function_error_location(parser, &keyword_loc);
          parser_push_error(
              parser,
              create_error(allocator, &loc, "expected '|' to close captures"));
        }
      }
      node_set_child(expr, CLUX_EXPR_FUNC_CAPTURES, captures);
      end_loc = node_get_location(captures);
    } else {
      location_t loc = function_error_location(parser, &keyword_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected '(' after '|' in captures"));
    }
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }
  /* 函数名（可选；匿名函数与函数类型没有）。 */
  if (!is_cfunc && token && token_get_kind(token) == CLUX_TOKEN_IDENTIFIER) {
    node_t *name = parser_read_literal_identifier(allocator, parser);
    node_set_child(expr, CLUX_EXPR_FUNC_NAME, name);
    end_loc = node_get_location(name);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }
  /* 泛型参数列表 [T, U]（可选）。 */
  if (!is_cfunc && token_is(token, CLUX_TOKEN_SYMBOL, "[")) {
    node_t *generics = read_generic_params(allocator, parser);
    node_set_child(expr, CLUX_EXPR_FUNC_GENERICS, generics);
    end_loc = node_get_location(generics);
    node_set_location_span(expr, &keyword_loc, end_loc);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }
  /* 形参列表。 */
  node_t *params = read_func_params(allocator, parser, false);
  if (!params) {
    location_t loc = function_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '(' to start parameters"));
    node_set_location_span(expr, &keyword_loc, end_loc);
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_FUNC_PARAMS, params);
  end_loc = node_get_location(params);
  node_set_location_span(expr, &keyword_loc, end_loc);

  parser_skip_trivia(parser);
  token = parser_peek_token(parser);
  bool is_definition = token_is(token, CLUX_TOKEN_SYMBOL, ":");
  bool is_type = token_is(token, CLUX_TOKEN_SYMBOL, "->");
  if (!is_definition && !is_type) {
    location_t loc = function_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(
            allocator, &loc, "expected ':' or '->' after function parameters"));
    return expr;
  }
  parser_next(parser); /* ':' / '->' */
  parser_skip_trivia(parser);
  node_t *ret = parser_read_expr(allocator, parser);
  if (!ret) {
    location_t loc = function_error_location(parser, end_loc);
    parser_push_error(parser,
                      create_error(allocator,
                                   &loc,
                                   "expected return type after '%s'",
                                   is_definition ? ":" : "->"));
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_FUNC_RETURN, ret);
  end_loc = node_get_location(ret);
  node_set_location_span(expr, &keyword_loc, end_loc);

  if (!is_definition) {
    /* 函数类型：到此结束。cfunc 只可能是类型。 */
    return expr;
  }
  parser_skip_trivia(parser);
  node_t *body = parser_read_stmt_block(allocator, parser);
  if (!body) {
    location_t loc = function_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' to start function body"));
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_FUNC_BODY, body);
  node_set_location_span(expr, &keyword_loc, node_get_location(body));
  return expr;
}
