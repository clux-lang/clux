#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser/expression_internal.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/node_stmt_extern_cfunc.h"
#include "parser/node_stmt_extern_var.h"
#include "parser/node_symbol.h"
#include "parser/parser.h"

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

/* 读取限定名 <lib>::<name>，成功返回 true。 */
static bool read_qualified_name(allocator_t *allocator,
                                parser_t *parser,
                                node_t **lib_out,
                                node_t **name_out,
                                const location_t **end_loc,
                                const char *decl_kind) {
  node_t *lib = parser_read_literal_identifier(allocator, parser);
  if (!lib) {
    location_t loc = expression_error_location(parser, *end_loc);
    parser_push_error(parser,
                      create_error(allocator,
                                   &loc,
                                   "expected library name after 'extern %s'",
                                   decl_kind));
    return false;
  }
  *lib_out = lib;
  *end_loc = node_get_location(lib);
  parser_skip_trivia(parser);

  token_t *token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "::")) {
    location_t loc = expression_error_location(parser, *end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '::' after library name"));
    return false;
  }
  parser_next(parser); /* '::' */
  parser_skip_trivia(parser);

  node_t *name = parser_read_literal_identifier(allocator, parser);
  if (!name) {
    location_t loc = expression_error_location(parser, *end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected symbol name after '::'"));
    return false;
  }
  *name_out = name;
  *end_loc = node_get_location(name);
  parser_skip_trivia(parser);
  return true;
}

/* 外部 C 函数声明 [export]? extern cfunc <lib>::<name>(params): ret;
   （白皮书 §8.9）。没有函数体，符号由运行时动态解析。
   只有 `extern` 后（跳过 trivia）紧跟 `cfunc` 时才由本函数认领；
   否则返回 NULL 且游标不动。出错时按容错约定返回不完整节点并 push_error。 */
node_t *parser_read_stmt_extern_cfunc(allocator_t *allocator,
                                      parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  size_t pos = parser_get_position(parser);
  token_t *look = token;
  if (token_is(look, CLUX_TOKEN_KEYWORD, "export")) {
    pos = next_non_trivia_position(parser, pos + 1);
    look = peek_next_non_trivia(parser, pos);
  }
  if (!token_is(look, CLUX_TOKEN_KEYWORD, "extern")) {
    return NULL;
  }
  size_t extern_pos = next_non_trivia_position(parser, pos + 1);
  token_t *kind = peek_next_non_trivia(parser, extern_pos);
  if (!token_is(kind, CLUX_TOKEN_KEYWORD, "cfunc")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);

  node_t *node = create_stmt_extern_cfunc_node(allocator);
  node_set_location(node, &begin_loc);
  const location_t *end_loc = &begin_loc;

  node_t *export_node = read_export(allocator, parser);
  if (export_node) {
    stmt_extern_cfunc_node_set_export(node, export_node);
  }
  parser_next(parser); /* extern */
  parser_skip_trivia(parser);
  end_loc = token_get_location(parser_peek_token(parser));
  parser_next(parser); /* cfunc */
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  node_t *lib = NULL;
  node_t *name = NULL;
  bool qualified =
      read_qualified_name(allocator, parser, &lib, &name, &end_loc, "cfunc");
  /* 容错路径也可能已解析出 lib/name，必须挂到节点上由节点统一释放。 */
  if (lib) {
    stmt_extern_cfunc_node_set_lib(node, lib);
  }
  if (name) {
    stmt_extern_cfunc_node_set_name(node, name);
  }
  node_set_location_span(node, &begin_loc, end_loc);
  if (!qualified) {
    return node;
  }

  /* 形参列表。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, "(")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected '(' to start parameters"));
    return node;
  }
  node_t *params = parser_read_func_decl_params(allocator, parser);
  stmt_extern_cfunc_node_set_params(node, params);
  end_loc = node_get_location(params);
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  /* 返回类型必须显式写出，用 ':' 引导。 */
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
  stmt_extern_cfunc_node_set_return(node, ret);
  end_loc = node_get_location(ret);
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  /* 结尾 ';'，没有函数体。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(
            allocator, &loc, "expected ';' after extern cfunc declaration"));
  }
  node_set_location_span(node, &begin_loc, end_loc);
  return node;
}

/* 外部变量声明 [export]? extern var <lib>::<name>: type;（白皮书 §8.10）。
   只有 `extern` 后（跳过 trivia）紧跟 `var` 时才由本函数认领；
   否则返回 NULL 且游标不动。出错时按容错约定返回不完整节点并 push_error。 */
node_t *parser_read_stmt_extern_var(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  size_t pos = parser_get_position(parser);
  token_t *look = token;
  if (token_is(look, CLUX_TOKEN_KEYWORD, "export")) {
    pos = next_non_trivia_position(parser, pos + 1);
    look = peek_next_non_trivia(parser, pos);
  }
  if (!token_is(look, CLUX_TOKEN_KEYWORD, "extern")) {
    return NULL;
  }
  size_t extern_pos = next_non_trivia_position(parser, pos + 1);
  token_t *kind = peek_next_non_trivia(parser, extern_pos);
  if (!token_is(kind, CLUX_TOKEN_KEYWORD, "var")) {
    return NULL;
  }
  location_t begin_loc = *token_get_location(token);

  node_t *node = create_stmt_extern_var_node(allocator);
  node_set_location(node, &begin_loc);
  const location_t *end_loc = &begin_loc;

  node_t *export_node = read_export(allocator, parser);
  if (export_node) {
    stmt_extern_var_node_set_export(node, export_node);
  }
  parser_next(parser); /* extern */
  parser_skip_trivia(parser);
  end_loc = token_get_location(parser_peek_token(parser));
  parser_next(parser); /* var */
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  node_t *lib = NULL;
  node_t *name = NULL;
  bool qualified =
      read_qualified_name(allocator, parser, &lib, &name, &end_loc, "var");
  /* 容错路径也可能已解析出 lib/name，必须挂到节点上由节点统一释放。 */
  if (lib) {
    stmt_extern_var_node_set_lib(node, lib);
  }
  if (name) {
    stmt_extern_var_node_set_name(node, name);
  }
  node_set_location_span(node, &begin_loc, end_loc);
  if (!qualified) {
    return node;
  }

  /* 类型必须显式写出，用 ':' 引导。 */
  token = parser_peek_token(parser);
  if (!token_is(token, CLUX_TOKEN_SYMBOL, ":")) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected ':' before variable type"));
    return node;
  }
  parser_next(parser); /* ':' */
  parser_skip_trivia(parser);
  node_t *type = parser_read_expr(allocator, parser);
  if (!type) {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(allocator, &loc, "expected variable type after ':'"));
    return node;
  }
  stmt_extern_var_node_set_type(node, type);
  end_loc = node_get_location(type);
  node_set_location_span(node, &begin_loc, end_loc);
  parser_skip_trivia(parser);

  /* 结尾 ';'。 */
  token = parser_peek_token(parser);
  if (token_is(token, CLUX_TOKEN_SYMBOL, ";")) {
    end_loc = token_get_location(token);
    parser_next(parser);
  } else {
    location_t loc = expression_error_location(parser, end_loc);
    parser_push_error(
        parser,
        create_error(
            allocator, &loc, "expected ';' after extern var declaration"));
  }
  node_set_location_span(node, &begin_loc, end_loc);
  return node;
}
