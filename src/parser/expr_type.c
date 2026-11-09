#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/parser.h"

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
    node_add_child(expr, elem);
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

/* 类型前缀层：指针、类型修饰、切片、数组、元组、optional。
   这些构造在语法阶段与普通表达式同级，统一由值表达式层调用。
   按固定顺序尝试，命中即返回。 */
node_t *parser_read_expr_type(allocator_t *allocator, parser_t *parser) {
  node_t *node = read_pointer(allocator, parser);
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
  return read_optional_type(allocator, parser);
}
