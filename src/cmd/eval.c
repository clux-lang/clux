#include "cmd/eval.h"
#include "ctfe/ctfe.h"
#include "core/allocator.h"
#include "core/arena.h"
#include "core/stream.h"
#include "core/string.h"
#include "core/strslice.h"
#include "core/vec.h"
#include "parser/ast_error.h"
#include "parser/ast_kind.h"
#include "parser/ast_node.h"
#include "parser/lexer.h"
#include "parser/parse_expr.h"
#include "parser/parser.h"
#include "vm/str_pool.h"
#include "vm/type_error.h"
#include "vm/value.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * 值打印：按 value 实际类型读取 data 输出到 stdout
 * =========================================================================== */

static void eval_print_value(vm_t *vm, value_t *v) {
  const type_t *t = value_type(v);
  if (!t || t == vm->type_void) return; /* void：不输出 */

  if (t == vm->type_bool) {
    printf("%s", *(const bool *)value_data(v) ? "true" : "false");
    return;
  }
  if (t == vm->type_str) {
    const char *sv = *(const char *const *)value_data(v);
    if (sv) fwrite(sv, 1, strlen(sv), stdout);
    return;
  }
  if (t == vm->type_type) {
    const type_t *inner = *(const type_t **)value_data(v);
    printf("%.*s", (int)inner->name.len, inner->name.ptr);
    return;
  }
  if (t == vm->type_i8 || t == vm->type_i16 ||
      t == vm->type_i32 || t == vm->type_i64) {
    int64_t i;
    switch (t->size) {
    case 1: i = *(const int8_t  *)value_data(v); break;
    case 2: i = *(const int16_t *)value_data(v); break;
    case 4: i = *(const int32_t *)value_data(v); break;
    default: i = *(const int64_t *)value_data(v); break;
    }
    printf("%lld", (long long)i);
    return;
  }
  if (t == vm->type_u8 || t == vm->type_u16 ||
      t == vm->type_u32 || t == vm->type_u64) {
    uint64_t u;
    switch (t->size) {
    case 1: u = *(const uint8_t  *)value_data(v); break;
    case 2: u = *(const uint16_t *)value_data(v); break;
    case 4: u = *(const uint32_t *)value_data(v); break;
    default: u = *(const uint64_t *)value_data(v); break;
    }
    printf("%llu", (unsigned long long)u);
    return;
  }
  if (t == vm->type_f32) {
    printf("%g", (double)*(const float *)value_data(v));
    return;
  }
  if (t == vm->type_f64) {
    printf("%g", *(const double *)value_data(v));
    return;
  }
  printf("<%s>", t->name.len ? t->name.ptr : "value");
}

/* ===========================================================================
 * cmd_eval：clux eval "<expr>"
 * =========================================================================== */

int cmd_eval(const cmd_args_t *args) {
  if (!args) return 1;
  const char *src = cmd_args_pos(args, 0);
  if (!src) {
    fprintf(stderr, "eval: missing expression\n");
    fprintf(stderr, "usage: clux eval <expr>\n");
    return 1;
  }

  allocator_t *alloc = create_allocator(malloc, free);
  if (!alloc) {
    fprintf(stderr, "eval: out of memory\n");
    return 1;
  }
  arena_t *arena = arena_new_default(alloc);
  if (!arena) {
    delete_allocator(&alloc);
    return 1;
  }

  /* ---- 词法分析：字符串 → token pool（owns_data=false，argv 持有源串） ---- */
  stream_source_t mem_src =
      stream_source_mem(alloc, src, strlen(src), /*owns_data=*/false);
  istream_t *stream = istream_open(alloc, mem_src);
  if (!stream) {
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }
  lexer_t *lexer = lexer_create(alloc, stream, "<eval>");
  if (!lexer) {
    istream_close(&stream);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  vec_t *pool = vec_new(alloc, /*owns_element=*/true);
  if (!pool) {
    lexer_close(&lexer);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  int lex_err = 0;
  for (;;) {
    token_t *t = lexer_next(lexer);
    if (!t) break;
    vec_push(pool, alloc, t);
    token_kind_t k = token_get_kind(t);
    if (k == TOKEN_TYPE_EOF) break;
    if (k == TOKEN_TYPE_ERROR) {
      const char *msg = token_get_error_message(t);
      fprintf(stderr, "eval: lexical error: %s\n",
              msg ? msg : "unrecognized input");
      lex_err = 1;
      break;
    }
  }
  if (lex_err) {
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  /* ---- 语法分析：单表达式 ---- */
  parser_t *parser = parser_create(alloc, arena, pool);
  if (!parser) {
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }
  ast_node_t *ast = parse_expr(parser);

  /* 检查表达式后是否只剩空白/注释/EOF（拒绝 "1+2 3" 这类多余输入） */
  if (ast && ast->kind != AST_ERROR) {
    for (size_t i = parser->pos; i < vec_len(pool); i++) {
      token_t *t = (token_t *)vec_get(pool, i);
      token_kind_t k = token_get_kind(t);
      if (k == TOKEN_TYPE_EOF) break;
      if (k == TOKEN_TYPE_WHITESPACE || k == TOKEN_TYPE_COMMENT ||
          k == TOKEN_TYPE_MULTILINE_COMMENT) {
        continue;
      }
      fprintf(stderr, "eval: syntax error: unexpected token after expression\n");
      ast = NULL;
      break;
    }
  }

  if (!ast || ast->kind == AST_ERROR) {
    if (!ast) fprintf(stderr, "eval: syntax error\n");
    parser_destroy(&parser);
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  /* ---- CTFE 求值 ---- */
  vm_t *vm = vm_new(alloc);
  if (!vm) {
    parser_destroy(&parser);
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }
  vm_push_scope(vm); /* 求值结果 track 到本 scope，pop 时统一回收 */

  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.vm        = vm;
  ctx.sema      = NULL; /* eval 场景无符号表，仅表达式求值 */
  ctx.budget    = 100000;
  ctx.max_depth = 128;

  value_t *r = ctfe_eval(&ctx, ast);
  int rc = 0;
  if (value_is_error(vm, r)) {
    error_data_t *ed = (error_data_t *)value_data(r);
    fprintf(stderr, "eval: %s\n",
            ed && ed->message ? ed->message : "evaluation error");
    rc = 1;
  } else {
    eval_print_value(vm, r);
    printf("\n");
  }

  vm_pop_scope(vm);
  vm_destroy(&vm);
  parser_destroy(&parser);
  lexer_close(&lexer);
  vec_free(alloc, &pool);
  arena_destroy(alloc, &arena);
  delete_allocator(&alloc);

  return rc;
}
