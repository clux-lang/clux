#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_symbol.h"
#include "parser/parser.h"

/* 单个字段 name: type;（白皮书 §3.5）。struct / tag union / cunion 共用。
   调用前当前 token 不一定是标识符，不是时返回 NULL 且游标不动。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
static node_t *read_field(allocator_t *allocator, parser_t *parser) {
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    return NULL;
  }
  location_t name_loc = *node_get_location(name);
  node_t *node = create_field_node(allocator);
  field_node_set_name(node, name);
  node_set_location_span(node, &name_loc, &name_loc);

  parser_skip_trivia(parser);
  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    location_t loc = expression_error_location(parser, &name_loc);
    parser_push_error(
        parser, create_error(allocator, &loc, "expected ':' after field name"));
    return node;
  }
  parser_next(parser); /* ':' */
  parser_skip_trivia(parser);
  node_t *type = parser_read_expr(allocator, parser);
  if (!type) {
    location_t loc = expression_error_location(parser, &name_loc);
    parser_push_error(parser,
                      create_error(allocator, &loc, "expected field type"));
    return node;
  }
  field_node_set_type(node, type);
  node_set_location_span(node, &name_loc, node_get_location(type));
  return node;
}

/* 字段列表 { field; ... }。当前 token 必须是 '{'。
   ';' 严格跟随字段：每个字段后必须写 ';'，不允许省略末尾分号，
   也不允许空字段 `;`。 */
static node_t *read_fields(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  location_t open_loc = *token_get_location(token);
  parser_next(parser); /* '{' */
  parser_skip_trivia(parser);

  node_t *node = create_fields_node(allocator);
  node_set_location_span(node, &open_loc, &open_loc);
  const location_t *end_loc = &open_loc;

  for (;;) {
    token = parser_peek_token(parser);
    if (!token) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected '}' to close fields"));
      break;
    }
    if (token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
      end_loc = token_get_location(token);
      parser_next(parser);
      break;
    }
    node_t *field = read_field(allocator, parser);
    if (!field) {
      location_t loc = expression_error_location(parser, end_loc);
      parser_push_error(
          parser, create_error(allocator, &loc, "expected field in type body"));
      /* 容错：跳到下一个 ';' 或 '}'，避免卡在同一 token 上。 */
      for (;;) {
        token = parser_peek_token(parser);
        if (!token || token_is(token, CLUX_TOKEN_SYMBOL, ";") ||
            token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
          break;
        }
        parser_next(parser);
      }
      if (token && token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
        parser_next(parser);
        parser_skip_trivia(parser);
      }
      continue;
    }
    fields_node_add_field(node, field);
    end_loc = node_get_location(field);
    parser_skip_trivia(parser);
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
    /* 容错：跳到下一个 ';' 或 '}'。 */
    for (;;) {
      token = parser_peek_token(parser);
      if (!token || token_is(token, CLUX_TOKEN_SYMBOL, ";") ||
          token_is(token, CLUX_TOKEN_SYMBOL, "}")) {
        break;
      }
      parser_next(parser);
    }
    if (token && token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
      parser_next(parser);
      parser_skip_trivia(parser);
    }
  }
  node_set_location_span(node, &open_loc, end_loc);
  return node;
}

/* union 定义（白皮书 §3.7 tag union、§3.8 C union）。
   tag union 与 cunion 在语法阶段完全相同，只有关键词不同，因此共用一条
   解析路径，由 KEYWORD 槽区分，语义差异（是否带 tag）留给语义阶段。
   与 func_decl 一样不是前导可判定的：首 token 是 union/cunion 时必然是
   定义，任何不匹配都是硬错误。调用前当前 token 必须是 union 或 cunion。
   出错时按容错约定返回不完整节点并 push_error，不返回 NULL。 */
node_t *parser_read_expr_union(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  bool is_cunion = token_is(token, CLUX_TOKEN_KEYWORD, "cunion");
  if (!is_cunion && !token_is(token, CLUX_TOKEN_KEYWORD, "union")) {
    return NULL;
  }
  location_t keyword_loc = *token_get_location(token);
  const char *keyword_text = is_cunion ? "cunion" : "union";
  node_t *keyword = create_symbol_node(allocator);
  node_set_location(keyword, &keyword_loc);
  parser_next(parser); /* union / cunion */
  parser_skip_trivia(parser);

  node_t *node = create_union_node(allocator);
  union_node_set_keyword(node, keyword);
  node_set_location_span(node, &keyword_loc, &keyword_loc);
  const location_t *end_loc = &keyword_loc;

  /* 类型名。 */
  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(
            allocator, &loc, "expected name after '%s'", keyword_text));
    return node;
  }
  union_node_set_name(node, name);
  end_loc = node_get_location(name);
  node_set_location_span(node, &keyword_loc, end_loc);
  parser_skip_trivia(parser);

  /* 泛型参数列表 [T, U]（可选）。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, "[")) {
    node_t *generics = parser_read_func_decl_generics(allocator, parser);
    union_node_set_generics(node, generics);
    end_loc = node_get_location(generics);
    node_set_location_span(node, &keyword_loc, end_loc);
    parser_skip_trivia(parser);
    token = parser_peek_token(parser);
  }

  /* 字段列表。 */
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "{")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(
            allocator, &loc, "expected '{' to start %s body", keyword_text));
    return node;
  }
  node_t *body = read_fields(allocator, parser);
  union_node_set_body(node, body);
  node_set_location_span(node, &keyword_loc, node_get_location(body));
  return node;
}
