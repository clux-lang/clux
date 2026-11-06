#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/parser.h"
#include "parser/function.h"

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
static location_t expression_error_location(parser_t *parser,
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

/* 关系运算符表：同一优先级，按最长匹配顺序排列（>= 先于 >）。 */
static const char *relational_operators[] = {">=", "<=", ">", "<", NULL};

/* 若当前 token 是关系运算符则返回其文本，否则返回 NULL。 */
static const char *peek_relational_operator(parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  for (size_t idx = 0; relational_operators[idx] != NULL; idx++) {
    if (token_is(token, CLUX_TOKEN_SYMBOL, relational_operators[idx])) {
      return relational_operators[idx];
    }
  }
  return NULL;
}

/* 取当前 token 之后第 offset 个 token（不移动游标），越界返回 NULL。 */
static token_t *peek_token_at(parser_t *parser, size_t offset) {
  size_t pos = parser_get_position(parser) + offset;
  vec_t *tokens = parser_get_tokens(parser);
  if (pos >= vec_get_size(tokens)) {
    return NULL;
  }
  return vec_get(tokens, pos);
}

/* 移位运算符 `<<` / `>>` 不在词法表中：它们与嵌套元组的尖括号冲突，
   由 parser 在相邻字符位置组合。返回运算符文本，否则 NULL。 */
static const char *peek_shift_operator(parser_t *parser) {
  token_t *first = parser_peek_token(parser);
  if (!first || token_get_kind(first) != CLUX_TOKEN_SYMBOL) {
    return NULL;
  }
  const location_t *first_loc = token_get_location(first);
  const char *op;
  if (location_is(first_loc, "<")) {
    op = "<<";
  } else if (location_is(first_loc, ">")) {
    op = ">>";
  } else {
    return NULL;
  }
  /* 中间有空白/注释时第二个 token 是 trivia，不构成移位。
     token 在源码中首尾相接，因此第二个 token 是符号即意味着两者相邻。 */
  token_t *second = peek_token_at(parser, 1);
  if (!second || token_get_kind(second) != CLUX_TOKEN_SYMBOL) {
    return NULL;
  }
  if (!location_is(token_get_location(second), op[0] == '<' ? "<" : ">")) {
    return NULL;
  }
  return op;
}

/* 消费相邻的两个字符 token，产出一个跨越二者的 SYMBOL 节点。 */
static node_t *read_shift_symbol(allocator_t *allocator, parser_t *parser) {
  token_t *first = parser_peek_token(parser);
  location_t loc = *token_get_location(first);
  parser_next(parser); /* 第一个 '<' / '>' */
  loc.end = token_get_location(parser_peek_token(parser))->end;
  parser_next(parser); /* 第二个 */
  node_t *node = create_node(allocator, CLUX_NODE_SYMBOL);
  node_set_location(node, &loc);
  return node;
}

/* 加减运算符表：同一优先级。 */
static const char *additive_operators[] = {"+", "-", NULL};

/* 若当前 token 是加减运算符则返回其文本，否则返回 NULL。 */
static const char *peek_additive_operator(parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  for (size_t idx = 0; additive_operators[idx] != NULL; idx++) {
    if (token_is(token, CLUX_TOKEN_SYMBOL, additive_operators[idx])) {
      return additive_operators[idx];
    }
  }
  return NULL;
}

/* as/is/extends 是关键字而非符号，消费后同样产出 SYMBOL 节点。
   调用方已通过 peek_* 确认当前 token 是目标关键字，无需再校验。 */
static node_t *read_keyword_symbol(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t loc = *token_get_location(token);
  parser_next(parser);
  node_t *node = create_node(allocator, CLUX_NODE_SYMBOL);
  node_set_location(node, &loc);
  return node;
}

/* 后缀一元运算符表（白皮书 5.3 优先级 25）。
   `.?` 不在词法表中（与 optional 构造冲突），由 parser 组合 `.` + `?`。 */
static const char *postfix_operators[] = {".&", ".*", ".!", NULL};

/* 当前 token 是 '.' 且紧随其后（无空白/注释）的 token 是 '?' 时，
   构成后缀 `.?`。 */
static bool peek_try_operator(parser_t *parser) {
  token_t *dot = parser_peek_token(parser);
  if (!token_is(dot, CLUX_TOKEN_SYMBOL, ".")) {
    return false;
  }
  token_t *question = peek_token_at(parser, 1);
  if (!token_is(question, CLUX_TOKEN_SYMBOL, "?")) {
    return false;
  }
  return token_get_location(question)->begin.offset ==
         token_get_location(dot)->end.offset;
}

/* 若当前 token 是后缀一元运算符则返回其文本，否则返回 NULL。 */
static const char *peek_postfix_operator(parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  for (size_t idx = 0; postfix_operators[idx] != NULL; idx++) {
    if (token_is(token, CLUX_TOKEN_SYMBOL, postfix_operators[idx])) {
      return postfix_operators[idx];
    }
  }
  if (peek_try_operator(parser)) {
    return ".?";
  }
  return NULL;
}

/* 消费当前后缀运算符，产出 SYMBOL 节点。`.*` / `.&` / `.!` 是单个 token；
   `.?` 由 '.' 与 '?' 两个 token 组合，节点区间跨越二者。 */
static node_t *read_postfix_symbol(allocator_t *allocator, parser_t *parser) {
  if (!peek_try_operator(parser)) {
    return parser_read_symbol(allocator, parser);
  }
  location_t loc = *token_get_location(parser_peek_token(parser));
  parser_next(parser); /* '.' */
  loc.end = token_get_location(parser_peek_token(parser))->end;
  parser_next(parser); /* '?' */
  node_t *node = create_node(allocator, CLUX_NODE_SYMBOL);
  node_set_location(node, &loc);
  return node;
}

/* 解析方括号后缀。调用前已确认当前 token 是 '['。
   语法阶段无法区分下标与实例化（两者同形 base[expr, ...]），统一产出
   CLUX_NODE_EXPR_SUBSCRIPT；带冒号的切片产出 CLUX_NODE_EXPR_SLICE。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *
read_bracket_suffix(allocator_t *allocator, parser_t *parser, node_t *base) {
  location_t open_loc = *token_get_location(parser_peek_token(parser));
  parser_next(parser); /* '[' */
  parser_skip_trivia(parser);

  token_t *token = parser_peek_token(parser);
  node_t *first = NULL;
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ":") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "]")) {
    first = parser_read_expr(allocator, parser);
    parser_skip_trivia(parser);
  }

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    /* 切片 base[lo?:hi?]，两端均可省略。 */
    parser_next(parser); /* ':' */
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    node_t *hi = NULL;
    if (!token_is(token, CLUX_TOKEN_SYMBOL, "]")) {
      hi = parser_read_expr(allocator, parser);
      parser_skip_trivia(parser);
    }
    location_t close_loc = open_loc;
    token = parser_peek_token(parser);
    if (token_is(token, CLUX_TOKEN_SYMBOL, "]")) {
      close_loc = *token_get_location(token);
      parser_next(parser);
    } else {
      location_t loc = expression_error_location(parser, &open_loc);
      parser_push_error(
          parser, create_error(allocator, &loc, "expected ']' to close slice"));
    }
    node_t *expr = create_node(allocator, CLUX_NODE_EXPR_SLICE);
    node_resize_children(expr, CLUX_EXPR_SLICE_CHILDREN_SIZE);
    node_set_child(expr, CLUX_EXPR_SLICE_BASE, base);
    node_set_child(expr, CLUX_EXPR_SLICE_LO, first);
    node_set_child(expr, CLUX_EXPR_SLICE_HI, hi);
    node_set_location_span(expr, node_get_location(base), &close_loc);
    return expr;
  }

  /* 下标或实例化：base[expr, expr, ...]。 */
  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_SUBSCRIPT);
  node_resize_children(expr, CLUX_EXPR_SUBSCRIPT_FIRST_ARG);
  node_set_child(expr, CLUX_EXPR_SUBSCRIPT_BASE, base);
  const location_t *end_loc = node_get_location(base);
  if (!first) {
    /* a[] 或 a[ , ...]：缺第一个实参。 */
    location_t loc = expression_error_location(parser, &open_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression in subscript"));
  } else {
    vec_push(node_get_children(expr), first);
    end_loc = node_get_location(first);
  }
  for (;;) {
    token = parser_peek_token(parser);
    if (!token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      break;
    }
    parser_next(parser); /* ',' */
    parser_skip_trivia(parser);
    node_t *arg = parser_read_expr(allocator, parser);
    if (!arg) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected expression after ','"));
      break;
    }
    vec_push(node_get_children(expr), arg);
    end_loc = node_get_location(arg);
    parser_skip_trivia(parser);
  }
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "]")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ']' to close subscript"));
  }
  node_set_location_span(expr, node_get_location(base), end_loc);
  return expr;
}

/* 解析调用实参列表。调用前已确认当前 token 是 '('。
   实参可以是 rest 表达式；`a(b, ...c, d)` 合法。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *
read_call_suffix(allocator_t *allocator, parser_t *parser, node_t *callee) {
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_CALL);
  node_resize_children(expr, CLUX_EXPR_CALL_FIRST_ARG);
  node_set_child(expr, CLUX_EXPR_CALL_CALLEE, callee);
  const location_t *end_loc = node_get_location(callee);

  token_t *token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    /* 无实参：f() */
    end_loc = token_get_location(token);
    parser_next(parser);
    node_set_location_span(expr, node_get_location(callee), end_loc);
    return expr;
  }

  for (;;) {
    /* 每个实参都可能是 rest 表达式。 */
    node_t *arg = parser_read_expr_rest(allocator, parser);
    if (!arg) {
      arg = parser_read_expr(allocator, parser);
    }
    if (!arg) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser, create_error(allocator, &loc, "expected expression in call"));
      break;
    }
    vec_push(node_get_children(expr), arg);
    end_loc = node_get_location(arg);
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
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected ')' to close call"));
  }
  node_set_location_span(expr, node_get_location(callee), end_loc);
  return expr;
}

/* 解析构造表达式 .type?{ elem, ... }。调用前当前 token 不一定是 '.'；
   只有形如 `.` + 可选表达式 + `{` 时才构成构造，否则返回 NULL 且游标不动，
   把 '.' 留给调用方（成员访问 / 后缀运算符）。
   类型槽在语法阶段就是普通表达式，按后缀层解析（与成员、调用、命名空间
   访问同级），因此 `.T{}`、`.aaa(b){}`、`.123{}` 同构。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *read_construct(allocator_t *allocator, parser_t *parser) {
  size_t start = parser_get_position(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ".")) {
    return NULL;
  }
  location_t dot_loc = *token_get_location(token);
  parser_next(parser); /* '.' */
  parser_skip_trivia(parser);

  node_t *type = NULL;
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    type = parser_read_expr_postfix(allocator, parser);
    if (!type) {
      /* '.' 后既不是 '{' 也不是表达式：不构成构造。 */
      parser_set_position(parser, start);
      return NULL;
    }
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
      /* 有表达式但没有花括号：不是构造，回退到 '.' 之前。 */
      delete_node(&type);
      parser_set_position(parser, start);
      return NULL;
    }
  }

  const location_t *end_loc = type ? node_get_location(type) : &dot_loc;
  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_CONSTRUCT);
  node_resize_children(expr, CLUX_EXPR_CONSTRUCT_FIRST_ELEM);
  node_set_child(expr, CLUX_EXPR_CONSTRUCT_TYPE, type);
  node_set_location_span(expr, &dot_loc, end_loc);

  parser_next(parser); /* '{' */
  parser_skip_trivia(parser);

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
    /* 空构造：.{ } */
    end_loc = token_get_location(token);
    parser_next(parser);
    node_set_location_span(expr, &dot_loc, end_loc);
    return expr;
  }

  for (;;) {
    /* 每个元素都可能是 rest 表达式。 */
    node_t *elem = parser_read_expr_rest(allocator, parser);
    if (!elem) {
      elem = parser_read_expr(allocator, parser);
    }
    if (!elem) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected expression in construct"));
      break;
    }
    vec_push(node_get_children(expr), elem);
    end_loc = node_get_location(elem);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (!token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      break;
    }
    parser_next(parser); /* ',' */
    parser_skip_trivia(parser);
  }

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '}' to close construct"));
  }
  node_set_location_span(expr, &dot_loc, end_loc);
  return expr;
}

/* 解析括号表达式 ( expr )。调用前当前 token 不一定是 '('；
   不是 '(' 时返回 NULL 且游标不动。
   括号内走顶层入口 parser_read_expr，因此赋值、三元等整条链都允许；
   括号内是否为合法值由语义阶段判定，语法层不剥离。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *read_group(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
    return NULL;
  }
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '(' */
  parser_skip_trivia(parser);

  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_GROUP);
  node_resize_children(expr, 1);
  node_set_location_span(expr, &open_loc, &open_loc);

  node_t *inner = parser_read_expr(allocator, parser);
  if (!inner) {
    location_t loc = expression_error_location(parser, &open_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression in parentheses"));
  } else {
    node_set_child(expr, CLUX_EXPR_GROUP_EXPR, inner);
    node_set_location_span(expr, &open_loc, node_get_location(inner));
  }
  parser_skip_trivia(parser);

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ")")) {
    location_t close_loc = *token_get_location(token);
    parser_next(parser);
    node_set_location_span(expr, &open_loc, &close_loc);
  } else {
    const location_t *end_loc = inner ? node_get_location(inner) : &open_loc;
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ')' to close parentheses"));
  }
  return expr;
}

/* 指针类型前缀 *<postfix>。操作数只取后缀层，因此 *a.b 解析为 *(a.b)，
   而 *a * b 解析为 (*a) * b。复用一元节点形态：LEFT 为 NULL，
   SYMBOL 存 '*'，RIGHT 存操作数。递归调用自身所在的值层，支持 **T。 */
static node_t *read_pointer(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "*")) {
    return NULL;
  }
  location_t star_loc = *token_get_location(token);
  parser_next(parser); /* '*' */

  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_t *symbol = create_node(allocator, CLUX_NODE_SYMBOL);
  node_set_location(symbol, &star_loc);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  node_set_location_span(expr, &star_loc, &star_loc);

  size_t after_star = parser_get_position(parser);
  parser_skip_trivia(parser);
  node_t *operand = parser_read_expr_postfix(allocator, parser);
  if (!operand) {
    /* 容错：缺操作数仍返回不完整的节点（RIGHT 为 NULL）。 */
    location_t loc = expression_error_location(parser, &star_loc);
    parser_set_position(parser, after_star);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected expression after '*'"));
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, operand);
  node_set_location_span(expr, &star_loc, node_get_location(operand));
  return expr;
}

/* 类型修饰前缀 const / volatile <postfix>（白皮书 §2.3）。
   与指针前缀同构：操作数只取后缀层，因此 const *T 解析为 const (*T)，
   而 const T * x 解析为 (const T) * x。复用一元节点形态：LEFT 为 NULL，
   SYMBOL 存修饰关键字，RIGHT 存操作数。操作数与指针前缀一致只取后缀层，
   因此 const a.b 解析为 const (a.b)；后缀层会回到值层，支持
   const volatile T 这类叠加。 */
static node_t *read_qualifier(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "const") &&
      !token_is(token, CLUX_TOKEN_KEYWORD, "volatile")) {
    return NULL;
  }
  location_t keyword_loc = *token_get_location(token);
  node_t *symbol = create_node(allocator, CLUX_NODE_SYMBOL);
  node_set_location(symbol, &keyword_loc);
  parser_next(parser); /* const / volatile */

  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  node_set_location_span(expr, &keyword_loc, &keyword_loc);

  size_t after_keyword = parser_get_position(parser);
  parser_skip_trivia(parser);
  node_t *operand = parser_read_expr_postfix(allocator, parser);
  if (!operand) {
    /* 容错：缺操作数仍返回不完整的节点（RIGHT 为 NULL）。 */
    location_t loc = expression_error_location(parser, &keyword_loc);
    parser_set_position(parser, after_keyword);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression after qualifier"));
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, operand);
  node_set_location_span(expr, &keyword_loc, node_get_location(operand));
  return expr;
}

/* 切片类型前缀 []<postfix>（白皮书 §2.4）。
   与指针/修饰前缀同构：操作数只取后缀层，因此 []a.b 解析为 [](a.b)，
   []T * x 解析为 ([]T) * x。复用一元节点形态：LEFT 为 NULL，
   SYMBOL 存 '[]'，RIGHT 存操作数。
   `[` 后必须紧跟 `]` 才构成切片类型；否则不消费任何 token 并返回 NULL，
   把 `[` 留给调用方（数组类型 [N]T 等将来在此扩展）。 */
static node_t *read_slice_type(allocator_t *allocator, parser_t *parser) {
  token_t *open = parser_peek_token(parser);
  if (!token_is(open, CLUX_TOKEN_SYMBOL, "[")) {
    return NULL;
  }
  token_t *close = peek_token_at(parser, 1);
  if (!token_is(close, CLUX_TOKEN_SYMBOL, "]")) {
    return NULL;
  }
  location_t bracket_loc = *token_get_location(open);
  bracket_loc.end = token_get_location(close)->end;
  node_t *symbol = create_node(allocator, CLUX_NODE_SYMBOL);
  node_set_location(symbol, &bracket_loc);
  parser_next(parser); /* '[' */
  parser_next(parser); /* ']' */

  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  node_set_location_span(expr, &bracket_loc, &bracket_loc);

  size_t after_bracket = parser_get_position(parser);
  parser_skip_trivia(parser);
  node_t *operand = parser_read_expr_postfix(allocator, parser);
  if (!operand) {
    /* 容错：缺操作数仍返回不完整的节点（RIGHT 为 NULL）。 */
    location_t loc = expression_error_location(parser, &bracket_loc);
    parser_set_position(parser, after_bracket);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression after '[]'"));
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, operand);
  node_set_location_span(expr, &bracket_loc, node_get_location(operand));
  return expr;
}

/* 数组类型前缀 [<expr>]<postfix>（白皮书 §2.4）。
   长度是任意表达式（走顶层入口，因此 a + 1、N * 2 等都合法），元素只取
   后缀层，因此 [4]T * x 解析为 ([4]T) * x。
   复用二元节点形态：LEFT 存长度，SYMBOL 存 '[]'（区间跨越方括号），
   RIGHT 存元素。切片类型 []T 的 LEFT 为 NULL，两者靠 LEFT 是否缺失区分。
   调用方保证 `[]` 已由 read_slice_type 消费，因此这里 `[` 后必有长度。 */
static node_t *read_array_type(allocator_t *allocator, parser_t *parser) {
  token_t *open = parser_peek_token(parser);
  if (!token_is(open, CLUX_TOKEN_SYMBOL, "[")) {
    return NULL;
  }
  location_t open_loc = *token_get_location(open);
  parser_next(parser); /* '[' */
  parser_skip_trivia(parser);

  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_set_location_span(expr, &open_loc, &open_loc);

  node_t *length = parser_read_expr(allocator, parser);
  if (!length) {
    location_t loc = expression_error_location(parser, &open_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression in array length"));
  } else {
    node_set_child(expr, CLUX_EXPR_BINARY_LEFT, length);
    node_set_location_span(expr, &open_loc, node_get_location(length));
  }
  parser_skip_trivia(parser);

  /* 方括号区间：'[' 起点 .. ']' 终点。 */
  location_t bracket_loc = open_loc;
  token_t *token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "]")) {
    bracket_loc.end = token_get_location(token)->end;
    parser_next(parser); /* ']' */
  } else {
    const location_t *end_loc = length ? node_get_location(length) : &open_loc;
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ']' to close array type"));
    bracket_loc.end = end_loc->end;
  }
  node_t *symbol = create_node(allocator, CLUX_NODE_SYMBOL);
  node_set_location(symbol, &bracket_loc);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  node_set_location_span(expr, &open_loc, &bracket_loc);

  size_t after_bracket = parser_get_position(parser);
  parser_skip_trivia(parser);
  node_t *element = parser_read_expr_postfix(allocator, parser);
  if (!element) {
    /* 容错：缺元素仍返回不完整的节点（RIGHT 为 NULL）。 */
    location_t loc = expression_error_location(parser, &bracket_loc);
    parser_set_position(parser, after_bracket);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression after array type"));
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, element);
  node_set_location_span(expr, &open_loc, node_get_location(element));
  return expr;
}

/* 元组类型 <T, U, ...>（白皮书 §2.2）。
   元素只取后缀层，逗号分隔，数量可变，也可为空（<>）。
   每个元素都可以是 rest 表达式，因此 <...Ts> 合法；孤立 `...` 由语义阶段报错。
   `<<` / `>>` 不在词法表中，嵌套元组的 <a, <b>>> 逐层各消费一个 '>'。
   元素从下标 0 起，children 长度 = 元素个数。 */
static node_t *read_tuple_type(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "<")) {
    return NULL;
  }
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '<' */
  parser_skip_trivia(parser);

  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_TUPLE);
  node_set_location_span(expr, &open_loc, &open_loc);
  const location_t *end_loc = &open_loc;

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ">")) {
    /* 空元组 <> */
    end_loc = token_get_location(token);
    parser_next(parser);
    node_set_location_span(expr, &open_loc, end_loc);
    return expr;
  }

  for (;;) {
    /* 每个元素都可能是 rest 表达式。 */
    node_t *elem = parser_read_expr_rest(allocator, parser);
    if (!elem) {
      elem = parser_read_expr_postfix(allocator, parser);
    }
    if (!elem) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected expression in tuple type"));
      break;
    }
    vec_push(node_get_children(expr), elem);
    end_loc = node_get_location(elem);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
    if (!token_is(token, CLUX_TOKEN_SYMBOL, ",")) {
      break;
    }
    parser_next(parser); /* ',' */
    parser_skip_trivia(parser);
  }

  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ">")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '>' to close tuple type"));
  }
  node_set_location_span(expr, &open_loc, end_loc);
  return expr;
}

/* optional 类型前缀 ?<postfix>（白皮书 §2.5）。
   与指针/修饰前缀同构：操作数只取后缀层，因此 ?a.b 解析为 ?(a.b)，
   ?T * x 解析为 (?T) * x。复用一元节点形态：LEFT 为 NULL，SYMBOL 存 '?'，
   RIGHT 存操作数。后缀层会回到值层，支持 ??T 与 ?[]u8 这类叠加。
   `?` 在值位置才是指类型前缀；三元运算符的 `?` 出现在左操作数之后，
   由三元层消费，两者不冲突。 */
static node_t *read_optional_type(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "?")) {
    return NULL;
  }
  location_t question_loc = *token_get_location(token);
  node_t *symbol = create_node(allocator, CLUX_NODE_SYMBOL);
  node_set_location(symbol, &question_loc);
  parser_next(parser); /* '?' */

  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  node_set_location_span(expr, &question_loc, &question_loc);

  size_t after_question = parser_get_position(parser);
  parser_skip_trivia(parser);
  node_t *operand = parser_read_expr_postfix(allocator, parser);
  if (!operand) {
    /* 容错：缺操作数仍返回不完整的节点（RIGHT 为 NULL）。 */
    location_t loc = expression_error_location(parser, &question_loc);
    parser_set_position(parser, after_question);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected expression after '?'"));
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, operand);
  node_set_location_span(expr, &question_loc, node_get_location(operand));
  return expr;
}

/* 值表达式层：最低一级优先级，与 literal 平级的构造都从这一层进入。
   目前有构造、括号、指针类型、类型修饰、切片类型、数组类型、元组类型、
   optional 类型、函数与 literal 十个来源，后续陆续加入同级运算符。 */
node_t *parser_read_expr_value(allocator_t *allocator, parser_t *parser) {
  node_t *node = read_construct(allocator, parser);
  if (node) {
    return node;
  }
  node = read_group(allocator, parser);
  if (node) {
    return node;
  }
  node = read_pointer(allocator, parser);
  if (node) {
    return node;
  }
  node = read_qualifier(allocator, parser);
  if (node) {
    return node;
  }
  node = read_slice_type(allocator, parser);
  if (node) {
    return node;
  }
  node = read_array_type(allocator, parser);
  if (node) {
    return node;
  }
  node = read_tuple_type(allocator, parser);
  if (node) {
    return node;
  }
  node = read_optional_type(allocator, parser);
  if (node) {
    return node;
  }
  node = parser_read_expr_func(allocator, parser);
  if (node) {
    return node;
  }
  return parser_read_literal(allocator, parser);
}

node_t *parser_read_expr_postfix(allocator_t *allocator, parser_t *parser) {
  node_t *operand = parser_read_expr_value(allocator, parser);
  if (!operand) {
    return NULL;
  }
  /* 后缀左结合：p.*.& 解析为 (p.*).&，成员链 a.b.c 解析为 (a.b).c。 */
  for (;;) {
    size_t after_operand = parser_get_position(parser);
    parser_skip_trivia(parser);
    const char *op = peek_postfix_operator(parser);
    if (op) {
      node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
      node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
      node_set_child(expr, CLUX_EXPR_BINARY_LEFT, operand);
      node_t *symbol = read_postfix_symbol(allocator, parser);
      node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
      /* RIGHT 保持 NULL：后缀形态复用二元节点，靠 RIGHT 缺失区分。 */
      node_set_location_span(
          expr, node_get_location(operand), node_get_location(symbol));
      operand = expr;
      continue;
    }
    /* 成员访问 obj.field 与命名空间访问 Type::member 结构相同，
       只差分隔符；字段/成员名恒为 identifier。
       LEFT=接收者，SYMBOL=分隔符，RIGHT=名字。 */
    token_t *token = parser_peek_token(parser);
    bool is_member = token_is(token, CLUX_TOKEN_SYMBOL, ".");
    bool is_scope = token_is(token, CLUX_TOKEN_SYMBOL, "::");
    if (is_member || is_scope) {
      node_t *separator = parser_read_symbol(allocator, parser);
      parser_skip_trivia(parser);
      node_t *name = parser_read_literal_identifier(allocator, parser);
      if (!name) {
        /* 分隔符后不是 identifier：不构成访问，回退到接收者之后。 */
        delete_node(&separator);
        parser_set_position(parser, after_operand);
        return operand;
      }
      node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
      node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
      node_set_child(expr, CLUX_EXPR_BINARY_LEFT, operand);
      node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, separator);
      node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, name);
      node_set_location_span(
          expr, node_get_location(operand), node_get_location(name));
      operand = expr;
      continue;
    }
    /* 方括号后缀：下标 / 实例化 / 切片。 */
    if (token_is(token, CLUX_TOKEN_SYMBOL, "[")) {
      operand = read_bracket_suffix(allocator, parser, operand);
      continue;
    }
    /* 调用后缀：callee(args)。 */
    if (token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
      operand = read_call_suffix(allocator, parser, operand);
      continue;
    }
    /* 既不是后缀运算符也不是访问：回退游标到操作数之后，
       不吞掉后续 trivia。 */
    parser_set_position(parser, after_operand);
    return operand;
  }
}
/* 前缀一元运算符表（白皮书 5.3 优先级 23）。 */
static const char *unary_operators[] = {"!", "~", "+", "-", NULL};

/* 若当前 token 是前缀一元运算符则返回其文本，否则返回 NULL。 */
static const char *peek_unary_operator(parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  for (size_t idx = 0; unary_operators[idx] != NULL; idx++) {
    if (token_is(token, CLUX_TOKEN_SYMBOL, unary_operators[idx])) {
      return unary_operators[idx];
    }
  }
  return NULL;
}

node_t *parser_read_expr_unary(allocator_t *allocator, parser_t *parser) {
  const char *op = peek_unary_operator(parser);
  if (!op) {
    return parser_read_expr_postfix(allocator, parser);
  }
  /* 前缀运算符右绑定：操作数递归到本层，支持 !!a、--a 等叠加。
     无前缀运算符时落到后缀层，因此 -p.* 解析为 -(p.*)。 */
  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  /* LEFT 保持 NULL：一元形态复用二元节点，靠 LEFT 缺失区分。 */
  node_t *symbol = parser_read_symbol(allocator, parser);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  size_t after_symbol = parser_get_position(parser);
  node_set_location_span(
      expr, node_get_location(symbol), node_get_location(symbol));
  parser_skip_trivia(parser);
  node_t *operand = parser_read_expr_unary(allocator, parser);
  if (!operand) {
    /* 容错：缺操作数仍返回不完整的节点（RIGHT 为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
    location_t loc =
        expression_error_location(parser, node_get_location(symbol));
    parser_set_position(parser, after_symbol);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression after '%s'", op));
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, operand);
  node_set_location_span(
      expr, node_get_location(symbol), node_get_location(operand));
  return expr;
}
/* as/is 运算符表：同一优先级（白皮书 5.3 优先级 21）。 */
static const char *as_is_operators[] = {"as", "is", NULL};

/* 若当前 token 是 as/is 则返回其文本，否则返回 NULL。 */
static const char *peek_as_is_operator(parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  for (size_t idx = 0; as_is_operators[idx] != NULL; idx++) {
    if (token_is(token, CLUX_TOKEN_KEYWORD, as_is_operators[idx])) {
      return as_is_operators[idx];
    }
  }
  return NULL;
}

node_t *parser_read_expr_as_is(allocator_t *allocator, parser_t *parser) {
  node_t *left = parser_read_expr_unary(allocator, parser);
  if (!left) {
    return NULL;
  }
  /* as/is 左结合（白皮书 5.3）。右值按下一级解析，与 +/- 等二元层一致：
     `a as b + c` 得 `(a as b) + c`。右值实际是类型，语法阶段不做语义
     检查，交给 sema 判定。 */
  for (;;) {
    size_t after_left = parser_get_position(parser);
    parser_skip_trivia(parser);
    const char *op = peek_as_is_operator(parser);
    if (!op) {
      /* 不是 as/is：回退游标到左操作数之后，不吞掉后续 trivia。 */
      parser_set_position(parser, after_left);
      return left;
    }
    node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
    node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
    node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
    node_t *symbol = read_keyword_symbol(allocator, parser);
    node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
    size_t after_symbol = parser_get_position(parser);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(symbol));
    parser_skip_trivia(parser);
    /* 右操作数走一元层，保证 a as -b 解析为 a as (-b)。 */
    node_t *right = parser_read_expr_unary(allocator, parser);
    if (!right) {
      /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
         同时记录错误，由调用方决定是否中止。 */
      /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
      location_t loc =
          expression_error_location(parser, node_get_location(symbol));
      parser_set_position(parser, after_symbol);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected expression after '%s'", op));
      return expr;
    }
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
    left = expr;
  }
}
/* 乘除运算符表：同一优先级。 */
static const char *multiplicative_operators[] = {"*", "/", "%", NULL};

/* 若当前 token 是乘除运算符则返回其文本，否则返回 NULL。 */
static const char *peek_multiplicative_operator(parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  for (size_t idx = 0; multiplicative_operators[idx] != NULL; idx++) {
    if (token_is(token, CLUX_TOKEN_SYMBOL, multiplicative_operators[idx])) {
      return multiplicative_operators[idx];
    }
  }
  return NULL;
}

node_t *parser_read_expr_multiplicative(allocator_t *allocator,
                                        parser_t *parser) {
  node_t *left = parser_read_expr_as_is(allocator, parser);
  if (!left) {
    return NULL;
  }
  /* 乘除左结合（白皮书 5.3）：a / b * c 解析为 (a / b) * c。 */
  for (;;) {
    size_t after_left = parser_get_position(parser);
    parser_skip_trivia(parser);
    const char *op = peek_multiplicative_operator(parser);
    if (!op) {
      /* 不是乘除：回退游标到左操作数之后，不吞掉后续 trivia。 */
      parser_set_position(parser, after_left);
      return left;
    }
    node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
    node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
    node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
    node_t *symbol = parser_read_symbol(allocator, parser);
    node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
    size_t after_symbol = parser_get_position(parser);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(symbol));
    parser_skip_trivia(parser);
    /* 右操作数走更高优先级层，保证 a * b as c 解析为 a * (b as c)。 */
    node_t *right = parser_read_expr_as_is(allocator, parser);
    if (!right) {
      /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
         同时记录错误，由调用方决定是否中止。 */
      /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
      location_t loc =
          expression_error_location(parser, node_get_location(symbol));
      parser_set_position(parser, after_symbol);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected expression after '%s'", op));
      return expr;
    }
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
    left = expr;
  }
}
node_t *parser_read_expr_additive(allocator_t *allocator, parser_t *parser) {
  node_t *left = parser_read_expr_multiplicative(allocator, parser);
  if (!left) {
    return NULL;
  }
  /* 加减左结合（白皮书 5.3）：a - b + c 解析为 (a - b) + c。 */
  for (;;) {
    size_t after_left = parser_get_position(parser);
    parser_skip_trivia(parser);
    const char *op = peek_additive_operator(parser);
    if (!op) {
      /* 不是加减：回退游标到左操作数之后，不吞掉后续 trivia。 */
      parser_set_position(parser, after_left);
      return left;
    }
    node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
    node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
    node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
    node_t *symbol = parser_read_symbol(allocator, parser);
    node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
    size_t after_symbol = parser_get_position(parser);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(symbol));
    parser_skip_trivia(parser);
    /* 右操作数走更高优先级层，保证 a + b * c 解析为 a + (b * c)。 */
    node_t *right = parser_read_expr_multiplicative(allocator, parser);
    if (!right) {
      /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
         同时记录错误，由调用方决定是否中止。 */
      /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
      location_t loc =
          expression_error_location(parser, node_get_location(symbol));
      parser_set_position(parser, after_symbol);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected expression after '%s'", op));
      return expr;
    }
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
    left = expr;
  }
}
node_t *parser_read_expr_shift(allocator_t *allocator, parser_t *parser) {
  node_t *left = parser_read_expr_additive(allocator, parser);
  if (!left) {
    return NULL;
  }
  /* 移位左结合（白皮书 5.3）：a << b << c 解析为 (a << b) << c。 */
  for (;;) {
    size_t after_left = parser_get_position(parser);
    parser_skip_trivia(parser);
    const char *op = peek_shift_operator(parser);
    if (!op) {
      /* 不是移位：回退游标到左操作数之后，不吞掉后续 trivia。 */
      parser_set_position(parser, after_left);
      return left;
    }
    node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
    node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
    node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
    node_t *symbol = read_shift_symbol(allocator, parser);
    node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
    size_t after_symbol = parser_get_position(parser);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(symbol));
    parser_skip_trivia(parser);
    /* 右操作数走更高优先级层，保证 a << b + c 解析为 a << (b + c)。 */
    node_t *right = parser_read_expr_additive(allocator, parser);
    if (!right) {
      /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
         同时记录错误，由调用方决定是否中止。 */
      /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
      location_t loc =
          expression_error_location(parser, node_get_location(symbol));
      parser_set_position(parser, after_symbol);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected expression after '%s'", op));
      return expr;
    }
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
    left = expr;
  }
}
node_t *parser_read_expr_relational(allocator_t *allocator, parser_t *parser) {
  node_t *left = parser_read_expr_shift(allocator, parser);
  if (!left) {
    return NULL;
  }
  /* 关系比较左结合（白皮书 5.3）：a < b < c 解析为 (a < b) < c。 */
  for (;;) {
    size_t after_left = parser_get_position(parser);
    parser_skip_trivia(parser);
    const char *op = peek_relational_operator(parser);
    if (!op) {
      /* 不是关系比较：回退游标到左操作数之后，不吞掉后续 trivia。 */
      parser_set_position(parser, after_left);
      return left;
    }
    node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
    node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
    node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
    node_t *symbol = parser_read_symbol(allocator, parser);
    node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
    size_t after_symbol = parser_get_position(parser);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(symbol));
    parser_skip_trivia(parser);
    /* 右操作数走更高优先级层，保证 a < b << c 解析为 a < (b << c)。 */
    node_t *right = parser_read_expr_shift(allocator, parser);
    if (!right) {
      /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
         同时记录错误，由调用方决定是否中止。 */
      /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
      location_t loc =
          expression_error_location(parser, node_get_location(symbol));
      parser_set_position(parser, after_symbol);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected expression after '%s'", op));
      return expr;
    }
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
    left = expr;
  }
}
node_t *parser_read_expr_equality(allocator_t *allocator, parser_t *parser) {
  node_t *left = parser_read_expr_relational(allocator, parser);
  if (!left) {
    return NULL;
  }
  /* == / != / extends 左结合（白皮书 5.3）：a == b == c 解析为
     (a == b) == c，与右结合不等价，所以这里用循环累积而不是递归。 */
  for (;;) {
    size_t after_left = parser_get_position(parser);
    parser_skip_trivia(parser);
    token_t *token = parser_peek_token(parser);
    /* 三个运算符同级，先看是哪一个；都不是则回退。
       extends 是关键字，另两个是符号。 */
    const char *op;
    bool keyword_op = false;
    if (token_is(token, CLUX_TOKEN_SYMBOL, "==")) {
      op = "==";
    } else if (token_is(token, CLUX_TOKEN_SYMBOL, "!=")) {
      op = "!=";
    } else if (token_is(token, CLUX_TOKEN_KEYWORD, "extends")) {
      op = "extends";
      keyword_op = true;
    } else {
      /* 不是相等比较：回退游标到左操作数之后，不吞掉后续 trivia。 */
      parser_set_position(parser, after_left);
      return left;
    }
    node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
    node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
    node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
    node_t *symbol = keyword_op ? read_keyword_symbol(allocator, parser)
                                : parser_read_symbol(allocator, parser);
    node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
    size_t after_symbol = parser_get_position(parser);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(symbol));
    parser_skip_trivia(parser);
    /* 右操作数走更高优先级层，保证 a == b < c 解析为 a == (b < c)。 */
    node_t *right = parser_read_expr_relational(allocator, parser);
    if (!right) {
      /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
         同时记录错误，由调用方决定是否中止。 */
      /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
      location_t loc =
          expression_error_location(parser, node_get_location(symbol));
      parser_set_position(parser, after_symbol);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected expression after '%s'", op));
      return expr;
    }
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
    left = expr;
  }
}
node_t *parser_read_expr_and(allocator_t *allocator, parser_t *parser) {
  node_t *left = parser_read_expr_equality(allocator, parser);
  if (!left) {
    return NULL;
  }
  size_t after_left = parser_get_position(parser);
  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "&")) {
    /* 不是位与：回退游标到左操作数之后，不吞掉后续 trivia。 */
    parser_set_position(parser, after_left);
    return left;
  }
  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
  node_t *symbol = parser_read_symbol(allocator, parser);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  size_t after_symbol = parser_get_position(parser);
  node_set_location_span(
      expr, node_get_location(left), node_get_location(symbol));
  parser_skip_trivia(parser);
  /* 递归右结合：a & b & c 解析为 a & (b & c)。 */
  node_t *right = parser_read_expr_and(allocator, parser);
  if (!right) {
    /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
    location_t loc =
        expression_error_location(parser, node_get_location(symbol));
    parser_set_position(parser, after_symbol);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected expression after '&'"));
  } else {
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
  }
  return expr;
}
node_t *parser_read_expr_xor(allocator_t *allocator, parser_t *parser) {
  node_t *left = parser_read_expr_and(allocator, parser);
  if (!left) {
    return NULL;
  }
  size_t after_left = parser_get_position(parser);
  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "^")) {
    /* 不是位异或：回退游标到左操作数之后，不吞掉后续 trivia。 */
    parser_set_position(parser, after_left);
    return left;
  }
  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
  node_t *symbol = parser_read_symbol(allocator, parser);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  size_t after_symbol = parser_get_position(parser);
  node_set_location_span(
      expr, node_get_location(left), node_get_location(symbol));
  parser_skip_trivia(parser);
  /* 递归右结合：a ^ b ^ c 解析为 a ^ (b ^ c)。 */
  node_t *right = parser_read_expr_xor(allocator, parser);
  if (!right) {
    /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
    location_t loc =
        expression_error_location(parser, node_get_location(symbol));
    parser_set_position(parser, after_symbol);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected expression after '^'"));
  } else {
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
  }
  return expr;
}
node_t *parser_read_expr_or(allocator_t *allocator, parser_t *parser) {
  node_t *left = parser_read_expr_xor(allocator, parser);
  if (!left) {
    return NULL;
  }
  size_t after_left = parser_get_position(parser);
  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "|")) {
    /* 不是位或：回退游标到左操作数之后，不吞掉后续 trivia。 */
    parser_set_position(parser, after_left);
    return left;
  }
  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
  node_t *symbol = parser_read_symbol(allocator, parser);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  size_t after_symbol = parser_get_position(parser);
  node_set_location_span(
      expr, node_get_location(left), node_get_location(symbol));
  parser_skip_trivia(parser);
  /* 递归右结合：a | b | c 解析为 a | (b | c)。 */
  node_t *right = parser_read_expr_or(allocator, parser);
  if (!right) {
    /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
    location_t loc =
        expression_error_location(parser, node_get_location(symbol));
    parser_set_position(parser, after_symbol);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected expression after '|'"));
  } else {
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
  }
  return expr;
}
node_t *parser_read_expr_land(allocator_t *allocator, parser_t *parser) {
  node_t *left = parser_read_expr_or(allocator, parser);
  if (!left) {
    return NULL;
  }
  size_t after_left = parser_get_position(parser);
  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "&&")) {
    /* 不是逻辑与：回退游标到左操作数之后，不吞掉后续 trivia。 */
    parser_set_position(parser, after_left);
    return left;
  }
  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
  node_t *symbol = parser_read_symbol(allocator, parser);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  size_t after_symbol = parser_get_position(parser);
  node_set_location_span(
      expr, node_get_location(left), node_get_location(symbol));
  parser_skip_trivia(parser);
  /* 递归右结合：a && b && c 解析为 a && (b && c)，与短路求值一致。 */
  node_t *right = parser_read_expr_land(allocator, parser);
  if (!right) {
    /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
    location_t loc =
        expression_error_location(parser, node_get_location(symbol));
    parser_set_position(parser, after_symbol);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression after '&&'"));
  } else {
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
  }
  return expr;
}
node_t *parser_read_expr_lor(allocator_t *allocator, parser_t *parser) {
  node_t *left = parser_read_expr_land(allocator, parser);
  if (!left) {
    return NULL;
  }
  size_t after_left = parser_get_position(parser);
  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "||")) {
    /* 不是逻辑或：回退游标到左操作数之后，不吞掉后续 trivia。 */
    parser_set_position(parser, after_left);
    return left;
  }
  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_BINARY);
  node_resize_children(expr, CLUX_EXPR_BINARY_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_BINARY_LEFT, left);
  node_t *symbol = parser_read_symbol(allocator, parser);
  node_set_child(expr, CLUX_EXPR_BINARY_SYMBOL, symbol);
  size_t after_symbol = parser_get_position(parser);
  node_set_location_span(
      expr, node_get_location(left), node_get_location(symbol));
  parser_skip_trivia(parser);
  /* 递归右结合：a || b || c 解析为 a || (b || c)，与短路求值一致。 */
  node_t *right = parser_read_expr_lor(allocator, parser);
  if (!right) {
    /* 容错：缺右操作数仍返回不完整的节点（RIGHT 为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    /* 错误位置先取：此时游标仍停在坏 token 上（若已到流末尾则退化）。 */
    location_t loc =
        expression_error_location(parser, node_get_location(symbol));
    parser_set_position(parser, after_symbol);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression after '||'"));
  } else {
    node_set_child(expr, CLUX_EXPR_BINARY_RIGHT, right);
    node_set_location_span(
        expr, node_get_location(left), node_get_location(right));
  }
  return expr;
}
node_t *parser_read_expr_ternary(allocator_t *allocator, parser_t *parser) {
  node_t *condition = parser_read_expr_lor(allocator, parser);
  if (!condition) {
    return NULL;
  }
  size_t after_condition = parser_get_position(parser);
  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "?")) {
    /* 不是三元：回退游标到条件之后，不吞掉后续 trivia。 */
    parser_set_position(parser, after_condition);
    return condition;
  }
  location_t question_loc = *token_get_location(token);
  parser_next(parser);
  parser_skip_trivia(parser);
  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_TERNARY);
  node_resize_children(expr, CLUX_EXPR_TERNARY_CHILDREN_SIZE);
  node_set_child(expr, CLUX_EXPR_TERNARY_CONDITION, condition);
  node_set_location_span(
      expr, node_get_location(condition), node_get_location(condition));
  node_t *then = parser_read_expr_ternary(allocator, parser);
  if (!then) {
    /* 容错：缺 then 分支仍返回不完整的节点（THEN/ELSE 为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    location_t loc = expression_error_location(parser, &question_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected expression after '?'"));
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_TERNARY_THEN, then);
  node_set_location_span(
      expr, node_get_location(condition), node_get_location(then));
  parser_skip_trivia(parser);
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    location_t loc = expression_error_location(parser, node_get_location(then));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ':' in ternary expression"));
    return expr;
  }
  location_t colon_loc = *token_get_location(token);
  parser_next(parser);
  parser_skip_trivia(parser);
  node_t *else_ = parser_read_expr_ternary(allocator, parser);
  if (!else_) {
    location_t loc = expression_error_location(parser, &colon_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected expression after ':'"));
    return expr;
  }
  node_set_child(expr, CLUX_EXPR_TERNARY_ELSE, else_);
  node_set_location_span(
      expr, node_get_location(condition), node_get_location(else_));
  return expr;
}
node_t *parser_read_expr_assign(allocator_t *allocator, parser_t *parser) {
  size_t position = parser_get_position(parser);
  token_t *token = parser_peek_token(parser);
  if (!token || token_get_kind(token) != CLUX_TOKEN_IDENTIFIER) {
    goto next;
  }
  parser_next(parser);
  parser_skip_trivia(parser);
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "+=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "-=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "*=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "/=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "%=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "&=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "|=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "~=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "^=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "&&=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "||=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, ">>=") &&
      !token_is(token, CLUX_TOKEN_SYMBOL, "<<=")) {
    parser_set_position(parser, position);
    goto next;
  }
  parser_set_position(parser, position);
  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_ASSIGN);
  node_resize_children(expr, CLUX_EXPR_ASSIGN_CHILDREN_SIZE);
  node_t *id = parser_read_literal_identifier(allocator, parser);
  node_set_child(expr, CLUX_EXPR_ASSIGN_ID, id);
  parser_skip_trivia(parser);
  node_t *symbol = parser_read_symbol(allocator, parser);
  node_set_child(expr, CLUX_EXPR_ASSIGN_SYMBOL, symbol);
  parser_skip_trivia(parser);
  node_t *value = parser_read_expr_ternary(allocator, parser);
  if (!value) {
    /* 容错：缺右值仍返回不完整的节点（VALUE 子节点为 NULL），
       同时记录错误，由调用方决定是否中止。 */
    node_set_location_span(
        expr, node_get_location(id), node_get_location(symbol));
    location_t loc =
        expression_error_location(parser, node_get_location(symbol));
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected expression after assignment"));
  } else {
    node_set_child(expr, CLUX_EXPR_ASSIGN_VALUE, value);
    node_set_location_span(
        expr, node_get_location(id), node_get_location(value));
  }
  return expr;
next:
  return parser_read_expr_ternary(allocator, parser);
}
node_t *parser_read_expr(allocator_t *allocator, parser_t *parser) {
  return parser_read_expr_assign(allocator, parser);
}

/* rest 表达式 ...expr?，不在优先级链上，由构造/调用实参等位置显式调用。
   孤立 `...` 合法（无子节点），用于展开全部剩余元素。 */
node_t *parser_read_expr_rest(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "...")) {
    return NULL;
  }
  location_t loc = *token_get_location(token);
  parser_next(parser); /* '...' */
  node_t *expr = create_node(allocator, CLUX_NODE_EXPR_REST);
  /* 操作数可选：后面没有表达式时保持 children 为空。 */
  size_t after_rest = parser_get_position(parser);
  parser_skip_trivia(parser);
  node_t *operand = parser_read_expr(allocator, parser);
  if (!operand) {
    /* 孤立 `...`：游标回到 '...' 之后，不吞掉后续 trivia。 */
    parser_set_position(parser, after_rest);
    node_set_location(expr, &loc);
    return expr;
  }
  node_resize_children(expr, 1);
  node_set_child(expr, CLUX_EXPR_REST_EXPR, operand);
  node_set_location_span(expr, &loc, node_get_location(operand));
  return expr;
}