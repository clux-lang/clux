#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_symbol.h"
#include "parser/parser.h"
#include "parser/statement.h"

/* 跳过一段由 open/close 包围的内容（含两端）。期间嵌套的 ()/[]/{} 计入深度，
   避免提前闭合。花括号必须计数：未来 struct 等内联类型可以带块，而块内的
   attribute 初始化可能出现与 close 相同的字符（如捕获列表的 '|'）。
   ')' / ']' / '}' 在正确语法下必然与同类的开符号平衡，因此共用一个深度计数；
   出现多余的闭符号时计数变负，最终在 EOF 处返回 false。
   当前 token 不是 open 时返回 false 且游标不动。 */
static bool
skip_balanced(parser_t *parser, const char *open, const char *close) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, open)) {
    return false;
  }
  parser_next(parser); /* open */
  parser_skip_trivia(parser);
  /* 圆/方括号与花括号分开计数：块内可能出现与 close 相同的字符
     （捕获列表的 '|'），花括号自身必须独立成对。 */
  int level = 0;
  int braces = 0;
  for (;;) {
    token = parser_peek_token(parser);
    if (!token) {
      return false;
    }
    if (token_get_kind(token) == CLUX_TOKEN_SYMBOL) {
      if (level == 0 && braces == 0 &&
          token_is(token, CLUX_TOKEN_SYMBOL, close)) {
        parser_next(parser); /* close */
        parser_skip_trivia(parser);
        return true;
      }
      /* 花括号内的内容整体跳过：块里可能有任意符号，不参与圆/方括号计数。 */
      if (token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
        braces++;
      } else if (token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
        braces--;
      } else if (braces == 0) {
        if (token_is(token, CLUX_TOKEN_SYMBOL, "(") ||
            token_is(token, CLUX_TOKEN_SYMBOL, "[")) {
          level++;
        } else if (token_is(token, CLUX_TOKEN_SYMBOL, ")") ||
                   token_is(token, CLUX_TOKEN_SYMBOL, "]")) {
          level--;
        }
      }
    }
    parser_next(parser);
    parser_skip_trivia(parser);
  }
}

/* 判断从当前位置起是否构成函数类型：可选捕获列表 |...|，随后 (params)，
   最后必须是 '->'。只扫描不消费，结束后恢复游标。 */
static bool is_func_type(parser_t *parser) {
  size_t start = parser_get_position(parser);
  token_t *token = parser_peek_token(parser);
  bool ok = false;
  /* 空捕获列表 `||` 是两个相邻的 '|'，skip_balanced 会立即闭合。 */
  if (token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
    if (!skip_balanced(parser, "|", "|")) {
      goto done;
    }
  }
  if (!skip_balanced(parser, "(", ")")) {
    goto done;
  }
  token = parser_peek_token(parser);
  ok = token_is(token, CLUX_TOKEN_SYMBOL, "->");
done:
  parser_set_position(parser, start);
  return ok;
}

/* 单个捕获项 name: type。捕获项必须有名字与类型。 */
static node_t *read_func_type_capture(allocator_t *allocator,
                                      parser_t *parser) {
  node_t *id = parser_read_literal_identifier(allocator, parser);
  if (!id) {
    return NULL;
  }
  location_t begin_loc = *node_get_location(id);
  node_t *node = create_func_capture_node(allocator);
  func_capture_node_set_name(node, id);
  node_set_location_span(node, &begin_loc, &begin_loc);

  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ':' after capture name"));
    return node;
  }
  parser_next(parser); /* ':' */
  parser_skip_trivia(parser);
  node_t *type = parser_read_expr_postfix(allocator, parser);
  if (!type) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(parser,
                      create_error(allocator, &loc, "expected capture type"));
    return node;
  }
  func_capture_node_set_type(node, type);
  node_set_location_span(node, &begin_loc, node_get_location(type));
  return node;
}

/* 捕获列表 |name: type, ...|。空列表写作 `||`，即两个相邻的 '|'。
   调用前当前 token 必须是 '|'（由调用方保证）。 */
static node_t *read_func_type_captures(allocator_t *allocator,
                                       parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* 开头的 '|' */
  parser_skip_trivia(parser);

  node_t *node = create_func_captures_node(allocator);
  node_set_location_span(node, &open_loc, &open_loc);
  const location_t *end_loc = &open_loc;

  /* 空捕获列表：紧跟闭合 '|'。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
    end_loc = token_get_location(token);
    parser_next(parser);
    node_set_location_span(node, &open_loc, end_loc);
    return node;
  }

  for (;;) {
    node_t *capture = read_func_type_capture(allocator, parser);
    if (!capture) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected capture in captures"));
      break;
    }
    func_captures_node_add_capture(node, capture);
    end_loc = node_get_location(capture);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
      end_loc = token_get_location(token);
      parser_next(parser);
      break;
    }
    if (token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      parser_next(parser);
      parser_skip_trivia(parser);
      continue;
    }
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ',' or '|' in captures"));
    /* 容错：跳到闭合 '|'，让后续形参列表仍能解析。
       is_func_type 已确认闭合 '|' 存在，因此这里必然能找到。 */
    for (;;) {
      token = parser_peek_token(parser);
      if (token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
        break;
      }
      parser_next(parser);
    }
    end_loc = token_get_location(token);
    parser_next(parser);
    break;
  }
  node_set_location_span(node, &open_loc, end_loc);
  return node;
}

/* 函数类型的形参列表 ( type, ... )。形参是裸类型表达式，不是 FUNC_PARAM。
   形参可以是 rest 表达式（C FFI 的变参 `...`，或参数包 `...Args`）。
   调用前当前 token 必须是 '('（由 is_func_type 保证）。 */
static node_t *read_func_type_params(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  node_t *node = create_func_params_node(allocator);
  node_set_location_span(node, &open_loc, &open_loc);
  const location_t *end_loc = &open_loc;

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    end_loc = token_get_location(token);
    parser_next(parser);
    node_set_location_span(node, &open_loc, end_loc);
    return node;
  }

  for (;;) {
    /* 每个形参都可能是 rest 表达式：C FFI 的变参 `...`（孤立，无操作数）
       或参数包 `...Args`。 */
    node_t *param = parser_read_expr_rest(allocator, parser);
    if (!param) {
      param = parser_read_expr_postfix(allocator, parser);
    }
    if (!param) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected parameter type in function"));
      break;
    }
    func_params_node_add_param(node, param);
    end_loc = node_get_location(param);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
      end_loc = token_get_location(token);
      parser_next(parser);
      break;
    }
    if (token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      parser_next(parser);
      parser_skip_trivia(parser);
      continue;
    }
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ',' or ')' in parameters"));
    break;
  }
  node_set_location_span(node, &open_loc, end_loc);
  return node;
}

/* 函数类型 func |captures|(params) -> ret（白皮书 §2.7）。
   不是前导可判定的：只有扫描到形参列表之后的 '->' 才确认是函数类型，
   否则返回 NULL 且游标不动（留给函数定义等其它构造）。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_expr_func_type(allocator_t *allocator, parser_t *parser) {
  size_t start = parser_get_position(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "func") &&
      !token_is(token, CLUX_TOKEN_KEYWORD, "cfunc")) {
    return NULL;
  }
  location_t keyword_loc = *token_get_location(token);
  parser_next(parser); /* func / cfunc */
  parser_skip_trivia(parser);
  if (!is_func_type(parser)) {
    parser_set_position(parser, start);
    return NULL;
  }

  node_t *node = create_func_type_node(allocator);
  node_set_location_span(node, &keyword_loc, &keyword_loc);
  const location_t *end_loc = &keyword_loc;

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
    node_t *captures = read_func_type_captures(allocator, parser);
    func_type_node_set_captures(node, captures);
    end_loc = node_get_location(captures);
    node_set_location_span(node, &keyword_loc, end_loc);
    parser_skip_trivia(parser);
  }

  /* is_func_type 已确认形参列表与 '->' 存在，这里不再重复校验。 */
  node_t *params = read_func_type_params(allocator, parser);
  func_type_node_set_params(node, params);
  end_loc = node_get_location(params);
  node_set_location_span(node, &keyword_loc, end_loc);

  parser_skip_trivia(parser);
  parser_next(parser); /* '->' */
  parser_skip_trivia(parser);
  node_t *ret = parser_read_expr_postfix(allocator, parser);
  if (!ret) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected return type after '->'"));
    return node;
  }
  func_type_node_set_return(node, ret);
  node_set_location_span(node, &keyword_loc, node_get_location(ret));
  return node;
}

/* 把 `( name: type, ... )` 里的形参逐个追加到 owner 的 children。
   每个形参都必须是 `name: type`；类型可以是 rest 表达式（参数包形参
   `args: ...Args`）。函数参数一律不允许默认值（仅闭包捕获的括号形态
   允许，见 read_func_decl_capture_paren）。
   当前 token 必须是 '('。 */
static void read_func_decl_param_list(allocator_t *allocator,
                                      parser_t *parser,
                                      node_t *owner) {
  token_t *token = parser_peek_token(parser);
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    parser_next(parser);
    return;
  }

  for (;;) {
    /* rest 形参：...Args（参数包，无名字）。 */
    node_t *rest = parser_read_expr_rest(allocator, parser);
    if (rest) {
      func_params_node_add_param(owner, rest);
    } else {
      node_t *name = parser_read_literal_identifier(allocator, parser);
      if (!name) {
        location_t loc =
            expression_error_location(parser, node_get_location(owner));
        parser_push_error(parser,
                          create_error(allocator,
                                       &loc,
                                       "expected parameter name in function"));
        break;
      }
      node_t *param = create_func_param_node(allocator);
      func_param_node_set_name(param, name);
      node_set_location_span(
          param, node_get_location(name), node_get_location(name));

      parser_skip_trivia(parser);
      token = parser_peek_token(parser);
      if (!token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
        location_t loc =
            expression_error_location(parser, node_get_location(name));
        parser_push_error(
            parser,
            create_error(allocator, &loc, "expected ':' after parameter name"));
        func_params_node_add_param(owner, param);
        break;
      }
      parser_next(parser); /* ':' */
      parser_skip_trivia(parser);
      /* 类型可以是 rest 表达式（参数包形参 `args: ...Args`）。
         用 ternary 层而非顶层：顶层 assign 会把捕获项的 `= default` 吃掉。 */
      node_t *type = parser_read_expr_rest(allocator, parser);
      if (!type) {
        type = parser_read_expr_ternary(allocator, parser);
      }
      if (!type) {
        location_t loc =
            expression_error_location(parser, node_get_location(name));
        parser_push_error(
            parser, create_error(allocator, &loc, "expected parameter type"));
        func_params_node_add_param(owner, param);
        break;
      }
      func_param_node_set_type(param, type);
      node_set_location_span(
          param, node_get_location(name), node_get_location(type));
      func_params_node_add_param(owner, param);
    }
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
      parser_next(parser);
      break;
    }
    if (token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      parser_next(parser);
      parser_skip_trivia(parser);
      continue;
    }
    location_t loc =
        expression_error_location(parser, node_get_location(owner));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ',' or ')' in parameters"));
    /* 容错：跳到闭合 ')'，让后续返回类型仍能解析。 */
    for (;;) {
      token = parser_peek_token(parser);
      if (!token || token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
        break;
      }
      parser_next(parser);
    }
    if (token) {
      parser_next(parser);
    }
    break;
  }
}

/* 形参列表 ( name: type, ... )。当前 token 必须是 '('。
   children 为 CLUX_NODE_FUNC_PARAM 或 rest 表达式。 */
static node_t *read_func_decl_params(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  node_t *node = create_func_params_node(allocator);
  node_set_location_span(node, &open_loc, &open_loc);
  read_func_decl_param_list(allocator, parser, node);
  /* 区间 = '(' 起点 .. 最后一个子节点终点；空列表时退化为 '(' 本身。 */
  size_t count = func_params_node_get_param_count(node);
  const location_t *end_loc =
      count ? node_get_location(func_params_node_get_param(node, count - 1))
            : &open_loc;
  node_set_location_span(node, &open_loc, end_loc);
  return node;
}

/* 函数声明的形参列表，供 extern cfunc 等外部声明复用。 */
node_t *parser_read_func_decl_params(allocator_t *allocator, parser_t *parser) {
  return read_func_decl_params(allocator, parser);
}

/* 单个带括号捕获项 ( name: type = default )（白皮书 §3.3）。
   括号逐捕获包裹：隔离捕获表达式内可能出现的 '|'，防止与捕获列表
   闭合符冲突。产出与形参同构的 FUNC_PARAM（类型/默认值可选）。
   当前 token 必须是 '('。出错时按容错约定返回不完整节点并 push_error，
   并跳到闭合 ')' 让外层循环继续。 */
static node_t *read_func_decl_capture_paren(allocator_t *allocator,
                                            parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  node_t *param = create_func_param_node(allocator);
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    location_t loc = expression_error_location(parser, &open_loc);
    parser_push_error(parser,
                      create_error(allocator, &loc, "expected capture name"));
    goto recover;
  }
  func_param_node_set_name(param, name);
  node_set_location_span(
      param, node_get_location(name), node_get_location(name));

  parser_skip_trivia(parser);
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    parser_next(parser); /* ':' */
    parser_skip_trivia(parser);
    /* 类型解析用 ternary 层：顶层 assign 会把 `= default` 吃掉
       （与形参列表同层）。 */
    node_t *type = parser_read_expr_rest(allocator, parser);
    if (!type) {
      type = parser_read_expr_ternary(allocator, parser);
    }
    if (!type) {
      location_t loc =
          expression_error_location(parser, node_get_location(name));
      parser_push_error(parser,
                        create_error(allocator, &loc, "expected capture type"));
      goto recover;
    }
    func_param_node_set_type(param, type);
    node_set_location_span(
        param, node_get_location(name), node_get_location(type));
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }
  if (token_is(token, CLUX_TOKEN_SYMBOL, "=")) {
    parser_next(parser); /* '=' */
    parser_skip_trivia(parser);
    /* 默认值用顶层表达式：括号已隔离 '|'，')' 是天然终止符。 */
    node_t *def = parser_read_expr(allocator, parser);
    if (!def) {
      location_t loc =
          expression_error_location(parser, node_get_location(param));
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected capture default value"));
      goto recover;
    }
    func_param_node_set_default(param, def);
    node_set_location_span(
        param, node_get_location(name), node_get_location(def));
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    location_t loc =
        expression_error_location(parser, node_get_location(param));
    parser_push_error(
        parser, create_error(allocator, &loc, "expected ')' to close capture"));
  } else {
    parser_next(parser); /* ')' */
  }
  return param;

recover:
  /* 跳到闭合 ')' 并消费；EOF 处安全退出。 */
  for (;;) {
    token = parser_peek_token(parser);
    if (!token || token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
      break;
    }
    parser_next(parser);
  }
  if (token) {
    parser_next(parser); /* ')' */
  }
  return param;
}

/* 裸名捕获项 name（无括号）：仅标识符，类型从定义点现值推断。
   带类型标注或默认值必须用括号形态——裸名后出现 ':'/'=' 即报错。
   名字解析失败返回 NULL（由调用方容错），成功返回仅含名字的 FUNC_PARAM。 */
static node_t *read_func_decl_capture_bare(allocator_t *allocator,
                                           parser_t *parser) {
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    return NULL;
  }
  node_t *param = create_func_param_node(allocator);
  func_param_node_set_name(param, name);
  node_set_location_span(
      param, node_get_location(name), node_get_location(name));

  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ":") ||
      token_is(token, CLUX_TOKEN_SYMBOL, "=")) {
    location_t loc = expression_error_location(parser, node_get_location(name));
    parser_push_error(parser,
                      create_error(allocator,
                                   &loc,
                                   "capture type or default requires "
                                   "parentheses: |(name: type = expr)|"));
  }
  return param;
}

/* 函数声明的捕获列表（白皮书 §3.3，defer 语句共用——defer 本质上是函数）。
   两种捕获形态可逐项混用：
   - 裸名 `|x, y|`：仅标识符，类型从定义点作用域现值推断；
   - 逐捕获括号 `|(x: i32 = 1), (y: i32 = 2)|`：带类型标注/默认值，
     括号隔离捕获表达式内可能出现的 '|' 与捕获列表闭合符的冲突。
   空捕获列表写作 `||`。children 归一化为一个 CLUX_NODE_FUNC_PARAMS
   （与形参列表同构，下游契约不变）；空列表 `||` 不带该子节点。
   调用前当前 token 必须是 '|'。 */
node_t *parser_read_func_decl_captures(allocator_t *allocator,
                                       parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* 开头的 '|' */
  parser_skip_trivia(parser);

  node_t *node = create_func_captures_node(allocator);
  node_set_location_span(node, &open_loc, &open_loc);

  /* 空捕获列表 `||`：紧跟闭合 '|'，没有形参列表节点。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
    location_t close_loc = *token_get_location(token);
    parser_next(parser);
    node_set_location_span(node, &open_loc, &close_loc);
    return node;
  }

  node_t *params = create_func_params_node(allocator);
  node_set_location_span(params, &open_loc, &open_loc);
  for (;;) {
    token = parser_peek_token(parser);
    node_t *param = NULL;
    if (token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
      param = read_func_decl_capture_paren(allocator, parser);
    } else {
      param = read_func_decl_capture_bare(allocator, parser);
      if (!param) {
        location_t loc = expression_error_location(parser, &open_loc);
        parser_push_error(
            parser,
            create_error(allocator, &loc, "expected capture in captures"));
        /* 容错：跳到分隔 ',' 或闭合 '|'。 */
        for (;;) {
          token = parser_peek_token(parser);
          if (!token || token_is(token, CLUX_TOKEN_SYMBOL, ",") ||
              token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
            break;
          }
          parser_next(parser);
        }
      }
    }
    if (param) {
      func_params_node_add_param(params, param);
    }
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      parser_next(parser); /* ',' */
      parser_skip_trivia(parser);
      continue;
    }
    break;
  }

  /* 归一化：捕获项全部并入唯一的 FUNC_PARAMS 子节点（可能为空——
     容错路径下的占位，下游按零捕获处理）。 */
  size_t count = func_params_node_get_param_count(params);
  if (count) {
    node_set_location_span(
        params,
        node_get_location(func_params_node_get_param(params, 0)),
        node_get_location(func_params_node_get_param(params, count - 1)));
  }
  func_captures_node_add_capture(node, params);
  const location_t *end_loc = node_get_location(params);

  parser_skip_trivia(parser);
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '|' to close captures"));
  }
  node_set_location_span(node, &open_loc, end_loc);
  return node;
}

/* 泛型参数列表 [T, U, ...]（白皮书 §6.1–§6.5）。
   调用前当前 token 必须是 '['（由调用方保证）。
   单个参数的写法：`T`、`N: usize`、`W extends IWriter[W]`、
   `T = i32`，以及参数包 `...Args`。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_func_decl_generics(allocator_t *allocator,
                                       parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '[' */
  parser_skip_trivia(parser);

  node_t *params = create_generic_params_node(allocator);
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
        location_t loc = expression_error_location(parser, end_loc);
        parser_push_error(
            parser,
            create_error(allocator, &loc, "expected generic parameter name"));
        break;
      }
      param = create_generic_param_node(allocator);
      generic_param_node_set_name(param, name);
      parser_skip_trivia(parser);
      token = parser_peek_token(parser);
      /* 编译期值参数：N: usize。泛型参数恒为 comptime，无需修饰词。
         旧语法的 `comptime` 不再支持：报单条诊断后跳过继续解析类型。 */
      if (token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
        parser_next(parser); /* ':' */
        parser_skip_trivia(parser);
        token = parser_peek_token(parser);
        if (token_is(token, CLUX_TOKEN_KEYWORD, "comptime")) {
          const location_t *kw_loc = token_get_location(token);
          parser_push_error(
              parser,
              create_error(
                  allocator,
                  kw_loc,
                  "generic parameters are always comptime: "
                  "write 'N: <type>' instead of 'N: comptime <type>'"));
          parser_next(parser); /* comptime（错误恢复：跳过继续解析类型） */
          parser_skip_trivia(parser);
        }
        /* 类型解析用 ternary 层：顶层 assign 会把 `= default` 吃掉
           （与捕获项同层）。 */
        node_t *type = parser_read_expr_ternary(allocator, parser);
        if (!type) {
          location_t loc = expression_error_location(parser, &open_loc);
          parser_push_error(
              parser,
              create_error(allocator, &loc, "expected generic parameter type"));
        } else {
          generic_param_node_set_type(param, type);
        }
        parser_skip_trivia(parser);
        token = parser_peek_token(parser);
      }
      /* 约束：W extends IWriter[W] */
      if (token_is(token, CLUX_TOKEN_KEYWORD, "extends")) {
        parser_next(parser); /* extends */
        parser_skip_trivia(parser);
        /* 约束解析用 ternary 层：顶层 assign 会把 `= default` 吃掉
           （与类型槽同层）。 */
        node_t *constraint = parser_read_expr_ternary(allocator, parser);
        if (!constraint) {
          location_t loc = expression_error_location(parser, &open_loc);
          parser_push_error(
              parser,
              create_error(
                  allocator, &loc, "expected constraint after 'extends'"));
        } else {
          generic_param_node_set_constraint(param, constraint);
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
          location_t loc = expression_error_location(parser, &open_loc);
          parser_push_error(
              parser,
              create_error(allocator, &loc, "expected generic default value"));
        } else {
          generic_param_node_set_default(param, def);
        }
      }
      /* 区间 = 名字起点 .. 最后一个非空子节点终点。 */
      node_t *param_last = generic_param_node_get_default(param);
      if (!param_last) {
        param_last = generic_param_node_get_constraint(param);
      }
      if (!param_last) {
        param_last = generic_param_node_get_type(param);
      }
      node_set_location_span(param,
                             node_get_location(name),
                             node_get_location(param_last ? param_last : name));
    }
    generic_params_node_add_param(params, param);
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
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(parser,
                      create_error(allocator,
                                   &loc,
                                   "expected ']' to close generic parameters"));
  }
  node_set_location_span(params, &open_loc, end_loc);
  return params;
}

/* 函数声明 / 函数定义 / 闭包（白皮书 §3.3）。
   是 func_type 的后退方案：调用前 func_type 已确认当前构造不是函数类型，
   因此首 token 为 func/cfunc 时必然是声明，任何不匹配都是硬错误。
   语法：func |captures| name[generics](params): ret {body}
   `cfunc` 在表达式上下文同样是定义，一样带函数体；它不能有捕获与泛型，
   但该限制属于语义检查，语法层照常解析（纯 FFI 声明是独立的语句形式，
   不经过这里）。出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *
read_func_decl(allocator_t *allocator, parser_t *parser, bool signature_only) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "func") &&
      !token_is(token, CLUX_TOKEN_KEYWORD, "cfunc")) {
    return NULL;
  }
  location_t keyword_loc = *token_get_location(token);
  node_t *keyword = create_symbol_node(allocator);
  node_set_location(keyword, &keyword_loc);
  parser_next(parser); /* func / cfunc */

  node_t *node = create_func_decl_node(allocator);
  func_decl_node_set_keyword(node, keyword);
  node_set_location_span(node, &keyword_loc, &keyword_loc);
  const location_t *end_loc = &keyword_loc;

  parser_skip_trivia(parser);
  token = parser_peek_token(parser);
  /* 捕获列表 |x| 或 |(x: i32 = 1), ...|。cfunc 不允许捕获，但该限制
     属于语义检查。 */
  if (token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
    node_t *captures = parser_read_func_decl_captures(allocator, parser);
    func_decl_node_set_captures(node, captures);
    end_loc = node_get_location(captures);
    node_set_location_span(node, &keyword_loc, end_loc);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }
  /* 函数名（可选；匿名函数没有）。 */
  if (token && token_get_kind(token) == CLUX_TOKEN_IDENTIFIER) {
    node_t *name = parser_read_literal_identifier(allocator, parser);
    func_decl_node_set_name(node, name);
    end_loc = node_get_location(name);
    node_set_location_span(node, &keyword_loc, end_loc);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }
  /* 泛型参数列表 [T, U]（可选）。cfunc 不允许泛型，同样留给语义检查。 */
  if (token_is(token, CLUX_TOKEN_SYMBOL, "[")) {
    node_t *generics = parser_read_func_decl_generics(allocator, parser);
    func_decl_node_set_generics(node, generics);
    end_loc = node_get_location(generics);
    node_set_location_span(node, &keyword_loc, end_loc);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }
  /* 形参列表。 */
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '(' to start parameters"));
    return node;
  }
  node_t *params = read_func_decl_params(allocator, parser);
  func_decl_node_set_params(node, params);
  end_loc = node_get_location(params);
  node_set_location_span(node, &keyword_loc, end_loc);

  /* 返回类型必须显式写出，用 ':' 引导。 */
  parser_skip_trivia(parser);
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ':' before return type"));
    return node;
  }
  parser_next(parser); /* ':' */
  parser_skip_trivia(parser);
  node_t *ret = parser_read_expr(allocator, parser);
  if (!ret) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected return type after ':'"));
    return node;
  }
  func_decl_node_set_return(node, ret);
  end_loc = node_get_location(ret);
  node_set_location_span(node, &keyword_loc, end_loc);

  parser_skip_trivia(parser);
  token = parser_peek_token(parser);

  /* 接口成员只写签名，以 ';' 结束，没有函数体。 */
  if (signature_only) {
    if (!token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(parser,
                        create_error(allocator,
                                     &loc,
                                     "expected ';' after function signature"));
      return node;
    }
    end_loc = token_get_location(token);
    parser_next(parser);
    node_set_location_span(node, &keyword_loc, end_loc);
    return node;
  }

  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' to start function body"));
    return node;
  }
  node_t *body = parser_read_stmt_block(allocator, parser);
  func_decl_node_set_body(node, body);
  node_set_location_span(node, &keyword_loc, node_get_location(body));
  return node;
}

node_t *parser_read_expr_func_decl(allocator_t *allocator, parser_t *parser) {
  return read_func_decl(allocator, parser, false);
}

/* 接口成员签名 func name(params): ret;（白皮书 §3.10），没有函数体。 */
node_t *parser_read_expr_func_signature(allocator_t *allocator,
                                        parser_t *parser) {
  return read_func_decl(allocator, parser, true);
}
