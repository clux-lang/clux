/*
 * 语法/词法诊断的"只报一次"回归：
 *
 * 解析器曾经同时走两条输出路径：`parse_error` 直接 fprintf(stderr)，
 * 调用点又用 `ast_error_new` 记进共享诊断缓冲区 → 同一处语法错误出现两条
 * （一条 fallback 单行、一条 Rust 风格片段）。
 *
 * 现在所有解析诊断都只进 diag 缓冲区，由 driver 出口统一打印。本测试直接
 * 在解析器层面断言：每个错误恰好产生 1 条 DIAG_ERROR。
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "core/allocator.h"
#include "core/arena.h"
#include "core/vec.h"
#include "core/stream.h"
#include "diag/diagnostic.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "parser/ast_node.h"
}

#include "test_common.h"

namespace {

struct LexResult {
  vec_t *tokens;
  lexer_t *lexer;
  char *source_buf;
  size_t source_len;
};

LexResult lex_source(allocator_t *alloc, const char *src) {
  LexResult r{};
  r.source_len = std::strlen(src);
  r.source_buf = static_cast<char *>(std::malloc(r.source_len + 1));
  std::memcpy(r.source_buf, src, r.source_len + 1);
  stream_source_t mem_src =
      stream_source_mem(alloc, r.source_buf, r.source_len, false);
  istream_t *stream = istream_open(alloc, mem_src);
  r.lexer = lexer_create(alloc, stream, "test.clx");
  r.tokens = vec_new(alloc, true);
  for (;;) {
    token_t *t = lexer_next(r.lexer);
    token_kind_t k = token_get_kind(t);
    vec_push(r.tokens, alloc, t);
    if (k == TOKEN_TYPE_EOF || k == TOKEN_TYPE_ERROR) break;
  }
  return r;
}

void lex_result_destroy(allocator_t *alloc, LexResult &r) {
  lexer_close(&r.lexer);
  vec_free(alloc, &r.tokens);
  std::free(r.source_buf);
  r.source_buf = nullptr;
}

/** 解析一段源码，返回其中 DIAG_ERROR 的消息（按记录顺序）。 */
std::vector<std::string> diagnostics_for(const std::string &src) {
  allocator_t *alloc = create_allocator(std::malloc, std::free);
  arena_t *arena = arena_new_default(alloc);
  diag_buf_t *diag = diag_buf_new(alloc);
  LexResult lex = lex_source(alloc, src.c_str());

  parser_t *parser = parser_create(alloc, arena, lex.tokens);
  parser->diag = diag;
  (void)parser_parse(parser);
  parser_destroy(&parser);

  std::vector<std::string> out;
  const diagnostic_t *items = diag_items(diag);
  for (size_t i = 0; i < diag_count(diag); i++) {
    if (items[i].level == DIAG_ERROR && items[i].message)
      out.emplace_back(items[i].message);
  }

  lex_result_destroy(alloc, lex);
  diag_buf_destroy(&diag);
  arena_destroy(alloc, &arena);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
  return out;
}

struct DiagCase {
  const char *label;
  const char *src;
  const char *needle;
};

} // namespace

TEST(ParseDiag, EverySyntaxErrorReportsExactlyOnce) {
  const DiagCase cases[] = {
      {"return_semicolon",
       "func main(): i32 { return 0 }\n",
       "expected ';' after return statement"},
      {"if_paren",
       "func main(): i32 { if (true { return 0; } return 1; }\n",
       "expected ')' after if condition"},
      {"array_bracket",
       "func main(): i32 { var a: [3i32; return 0; }\n",
       "expected ']' after array type length"},
      {"var_initializer",
       "func main(): i32 { var x: i32 1; return 0; }\n",
       "expected '=' and initializer in var definition"},
      {"scope_annotation",
       "func f(p: '< i32): i32 { return 0; }\n",
       "invalid scope annotation"},
      {"extern_func",
       "extern lib libc from \"*\";\n"
       "extern func libc::printf(format: str, ...): i32;\n"
       "func main(): i32 { return 0; }\n",
       "not a C ABI"},
      {"lexical_error",
       "func main(): i32 { var s = \"abc; return 0; }\n",
       "unterminated string literal"},
  };

  for (const DiagCase &c : cases) {
    SCOPED_TRACE(c.label);
    const std::vector<std::string> diags = diagnostics_for(c.src);
    ASSERT_EQ(diags.size(), 1u)
        << "expected exactly one diagnostic, got " << diags.size();
    EXPECT_NE(diags[0].find(c.needle), std::string::npos) << diags[0];
  }
}
