#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/decorator.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_field.h"
#include "parser/node_implements.h"
#include "parser/node_struct_decl.h"
#include "parser/node_struct_member.h"
#include "parser/node_symbol.h"
#include "parser/parser.h"
#include "parser/statement.h"

/* 嵌入成员 ...Base;（白皮书 §3.5）。
   调用前当前 token 必须是 '...'。类型只取后缀层，避免把后续 ';' 或
   成员分隔符当作运算符吃掉。出错时返回不完整节点并 push_error。 */
static node_t *read_embed(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t begin_loc = *token_get_location(token);
  parser_next(parser); /* '...' */
  parser_skip_trivia(parser);

  node_t *node = create_struct_embed_node(allocator);
  node_set_location(node, &begin_loc);

  node_t *type = parser_read_expr_postfix(allocator, parser);
  if (!type) {
    location_t loc = expression_error_location(parser, &begin_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected embedded type after '...'"));
    return node;
  }
  struct_embed_node_set_type(node, type);
  node_set_location_span(node, &begin_loc, node_get_location(type));
  parser_skip_trivia(parser);

  location_t loc = *node_get_location(node);
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    loc.end = token_get_location(token)->end;
    parser_next(parser);
  } else {
    location_t err = expression_error_location(parser, node_get_location(node));
    parser_push_error(
        parser,
        create_error(allocator, &err, "expected ';' after embedded type"));
  }
  loc.filename = parser_get_filename(parser);
  node_set_location(node, &loc);
  return node;
}

/* struct 成员：字段、嵌入，或带装饰器的字段/var/func 语句。
   调用前当前 token 不一定是成员起始，不是时返回 NULL 且游标不动。
   字段与嵌入的 ';' 由成员列表统一消费（嵌入在本函数内消费）。 */
static node_t *read_member(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "...")) {
    return read_embed(allocator, parser);
  }
  if (parser_peek_decorator(parser)) {
    token_t *keyword = parser_peek_token_after_decorators(parser);
    if (token_is(keyword, CLUX_TOKEN_KEYWORD, "func") ||
        token_is(keyword, CLUX_TOKEN_KEYWORD, "cfunc")) {
      return parser_read_stmt_func(allocator, parser);
    }
    if (token_is(keyword, CLUX_TOKEN_KEYWORD, "var") ||
        token_is(keyword, CLUX_TOKEN_KEYWORD, "using")) {
      return parser_read_stmt_var(allocator, parser);
    }
    /* 其余情况按带装饰器的字段处理。 */
    return parser_read_field(allocator, parser, true);
  }
  if (token_is(token, CLUX_TOKEN_KEYWORD, "func") ||
      token_is(token, CLUX_TOKEN_KEYWORD, "cfunc")) {
    return parser_read_stmt_func(allocator, parser);
  }
  if (token_is(token, CLUX_TOKEN_KEYWORD, "var") ||
      token_is(token, CLUX_TOKEN_KEYWORD, "using")) {
    return parser_read_stmt_var(allocator, parser);
  }
  if (token && token_get_kind(token) == CLUX_TOKEN_IDENTIFIER) {
    return parser_read_field(allocator, parser, true);
  }
  return NULL;
}

/* 跳到下一个 ';' 或 '}'；遇到 ';' 时消费它。用于字段错误后的恢复。 */
static void skip_to_field_boundary(parser_t *parser) {
  for (;;) {
    token_t *token = parser_peek_token(parser);
    if (!token || token_is(token, CLUX_TOKEN_SYMBOL, ";") ||
        token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
      break;
    }
    parser_next(parser);
  }
  token_t *token = parser_peek_token(parser);
  if (token && token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    parser_next(parser);
    parser_skip_trivia(parser);
  }
}

/* 成员列表 { member ... }。当前 token 必须是 '{'。
   字段以 ';' 结尾；var 语句自带 ';'；func 语句自带函数体；
   嵌入在本函数内消费自己的 ';'。 */
static node_t *read_members(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '{' */
  parser_skip_trivia(parser);

  node_t *node = create_struct_members_node(allocator);
  node_set_location_span(node, &open_loc, &open_loc);
  const location_t *end_loc = &open_loc;

  for (;;) {
    token = parser_peek_token(parser);
    if (!token) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected '}' to close struct body"));
      break;
    }
    if (token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
      end_loc = token_get_location(token);
      parser_next(parser);
      break;
    }
    node_t *member = read_member(allocator, parser);
    if (!member) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser, create_error(allocator, &loc, "expected struct member"));
      parser_next(parser);
      continue;
    }
    struct_members_node_add_member(node, member);
    end_loc = node_get_location(member);
    parser_skip_trivia(parser);

    if (node_get_kind(member) != CLUX_NODE_FIELD) {
      continue;
    }
    /* 字段以 ';' 严格结尾。 */
    if (!field_node_get_name(member)) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected field name in struct body"));
      skip_to_field_boundary(parser);
      continue;
    }
    token = parser_peek_token(parser);
    if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
      end_loc = token_get_location(token);
      parser_next(parser);
      parser_skip_trivia(parser);
      continue;
    }
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected ';' after field"));
    skip_to_field_boundary(parser);
  }
  node_set_location_span(node, &open_loc, end_loc);
  return node;
}

/* 接口列表 implements I[S] & J[S]（白皮书 §3.5）。当前 token 必须是
   implements 关键字。接口类型只取后缀层，避免把 '&' 当作位与运算符吃掉。 */
static node_t *read_implements(allocator_t *allocator, parser_t *parser) {
  token_t *keyword = parser_peek_token(parser);
  location_t begin_loc = *token_get_location(keyword);
  parser_next(parser); /* implements */
  parser_skip_trivia(parser);

  node_t *node = create_implements_node(allocator);
  node_set_location_span(node, &begin_loc, &begin_loc);
  for (;;) {
    node_t *type = parser_read_expr_postfix(allocator, parser);
    if (!type) {
      location_t loc = expression_error_location(parser, &begin_loc);
      parser_push_error(
          parser,
          create_error(
              allocator, &loc, "expected interface type after 'implements'"));
      break;
    }
    implements_node_add_type(node, type);
    node_set_location_span(node, &begin_loc, node_get_location(type));
    parser_skip_trivia(parser);
    token_t *token = parser_peek_token(parser);
    if (!token_is(token, CLUX_TOKEN_SYMBOL, "&")) {
      break;
    }
    parser_next(parser); /* '&' */
    parser_skip_trivia(parser);
  }
  return node;
}

/* 结构体声明 struct Name [generics]? [implements ...]? { members }
   （白皮书 §3.5）。装饰器属于外层语句（stmt_struct），本函数只解析声明。
   与 func_decl / union / enum 一样不是前导可判定的：首 token 是 struct 时
   必然是声明，任何不匹配都是硬错误。调用前当前 token 必须是 struct。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_expr_struct_decl(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_KEYWORD, "struct")) {
    return NULL;
  }
  location_t keyword_loc = *token_get_location(token);
  node_t *keyword = create_symbol_node(allocator);
  node_set_location(keyword, &keyword_loc);
  parser_next(parser); /* struct */
  parser_skip_trivia(parser);

  node_t *node = create_struct_decl_node(allocator);
  struct_decl_node_set_keyword(node, keyword);
  node_set_location_span(node, &keyword_loc, &keyword_loc);
  const location_t *end_loc = &keyword_loc;

  /* 结构体名。 */
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected name after 'struct'"));
    return node;
  }
  struct_decl_node_set_name(node, name);
  end_loc = node_get_location(name);
  node_set_location_span(node, &keyword_loc, end_loc);
  parser_skip_trivia(parser);

  /* 泛型参数列表 [T, U]（可选）。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "[")) {
    node_t *generics = parser_read_func_decl_generics(allocator, parser);
    struct_decl_node_set_generics(node, generics);
    end_loc = node_get_location(generics);
    node_set_location_span(node, &keyword_loc, end_loc);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }

  /* implements 列表（可选）。 */
  if (token_is(token, CLUX_TOKEN_KEYWORD, "implements")) {
    node_t *implements = read_implements(allocator, parser);
    struct_decl_node_set_implements(node, implements);
    end_loc = node_get_location(implements);
    node_set_location_span(node, &keyword_loc, end_loc);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }

  /* 成员列表。 */
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '{' to start struct body"));
    return node;
  }
  node_t *body = read_members(allocator, parser);
  struct_decl_node_set_body(node, body);
  node_set_location_span(node, &keyword_loc, node_get_location(body));
  return node;
}
