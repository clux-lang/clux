#include "driver/driver.h"
#include "core/allocator.h"
#include "core/stream.h"
#include "core/vec.h"
#include "core/arena.h"
#include "core/string.h"
#include "parser/lexer.h"
#include "parser/location.h"
#include "parser/parser.h"
#include "parser/ast_node.h"
#include "parser/ast_kind.h"
#include "parser/ast_program.h"
#include "parser/ast_error.h"
#include "diag/diagnostic.h"
#include "sema/sema.h"
#include "sema/symbol.h"
#include "compiler/compiler.h"
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/scope.h"
#include "vm/exec.h"
#include "vm/bcode.h"
#include "vm/bcode_disasm.h"
#include "vm/bcode_asm.h"
#include "vm/bcode_asm_defs.h"
#include "vm/bcode_serial.h"
#include "vm/type_error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Internal: byte buffer class (for file-backed source buffers) ---- */

static class_t g_bytes_class = {
    .name = "clux.driver.bytes",
    .size = sizeof(char),
    .clone_fn = default_clone,
    .move_fn = default_move,
    .dispose_fn = NULL,
};

/* ---- Stage ①: load a source file fully into an allocator buffer ---- */

int driver_load_source(allocator_t *alloc,
                       const char *path,
                       const char **out_data,
                       size_t *out_len) {
  if (!alloc || !path || !out_data || !out_len) return -1;

  *out_data = NULL;
  *out_len = 0;

  FILE *fp = fopen(path, "rb");
  if (!fp) return -1;

  /* Determine the file size via a seek to the end. */
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  long size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return -1;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }

  /* stream_source_mem requires a non-NULL data pointer, so allocate at
   * least one byte even for an empty file. */
  size_t n = (size_t)size;
  char *buf = (char *)allocator_new(alloc, &g_bytes_class, n > 0 ? n : 1);
  if (!buf) {
    fclose(fp);
    return -1;
  }

  size_t read = fread(buf, 1, n, fp);
  fclose(fp);

  if (read != n) {
    /* Short read: the buffer is owned by the allocator; the caller is
     * expected to hand it to a mechanism that frees it (e.g. an
     * owns_data memory source). Signal failure but leave the buffer
     * allocated so it is not leaked. */
    *out_data = buf;
    *out_len = read;
    return -1;
  }

  *out_data = buf;
  *out_len = n;
  return 0;
}

/* ---- Internal: load + lex into a pool, keeping the lexer alive ---- */

static int driver_lex_into(allocator_t *alloc,
                           const char *path,
                           vec_t **out_pool,
                           lexer_t **out_lexer) {
  const char *data = NULL;
  size_t len = 0;
  if (driver_load_source(alloc, path, &data, &len) != 0) return -1;

  stream_source_t src = stream_source_mem(alloc, data, len, /*owns_data=*/true);
  istream_t *stream = istream_open(alloc, src);
  if (!stream) return -1;

  lexer_t *lexer = lexer_create(alloc, stream, path);
  if (!lexer) {
    istream_close(&stream);
    return -1;
  }

  vec_t *pool = vec_new(alloc, /*owns_element=*/true);
  if (!pool) {
    lexer_close(&lexer);
    return -1;
  }

  for (;;) {
    token_t *t = lexer_next(lexer);
    if (!t) break;
    vec_push(pool, alloc, t);
    token_kind_t kind = token_get_kind(t);
    if (kind == TOKEN_TYPE_EOF) break;
    if (kind == TOKEN_TYPE_ERROR) {
      const location_t *loc = token_get_location(t);
      const char *msg = token_get_error_message(t);
      fprintf(stderr, "%s:%zu:%zu: error: %s\n",
              path,
              loc ? loc->begin.line : 0,
              loc ? loc->begin.column : 0,
              msg ? msg : "unrecognized input");
      lexer_close(&lexer);
      vec_free(alloc, &pool);
      return -2;
    }
  }

  *out_pool = pool;
  *out_lexer = lexer;
  return 0;
}

/* ---- Stage ② + ③ + ④: lex → parse → sema ---- */

int driver_run_file(const char *path) {
  if (!path) {
    fprintf(stderr, "run: no input file\n");
    return 1;
  }

  allocator_t *alloc = create_allocator(malloc, free);
  if (!alloc) {
    fprintf(stderr, "run: out of memory\n");
    return 1;
  }

  arena_t *arena = arena_new_default(alloc);
  if (!arena) {
    fprintf(stderr, "run: out of memory\n");
    delete_allocator(&alloc);
    return 1;
  }

  /* Stage ① + ②: 加载 → 词法分析（词法错误快速失败） */
  vec_t *pool = NULL;
  lexer_t *lexer = NULL;
  int lex_result = driver_lex_into(alloc, path, &pool, &lexer);
  if (lex_result == -2) {
    /* 词法错误已输出到 stderr */
    if (lexer) lexer_close(&lexer);
    if (pool) vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }
  if (lex_result != 0) {
    fprintf(stderr, "run: cannot open file '%s'\n", path);
    if (lexer) lexer_close(&lexer);
    if (pool) vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  /* Stage ③: 语法分析 */
  /* 诊断缓冲区在解析前创建：语法错误由 parser 记入，统一在出口打印 */
  diag_buf_t *diag = diag_buf_new(alloc);
  if (!diag) {
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  parser_t *parser = parser_create(alloc, arena, pool);
  if (!parser) {
    diag_buf_destroy(&diag);
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }
  parser->diag = diag;

  ast_node_t *ast = parser_parse(parser);
  parser_destroy(&parser);

  if (!ast || ast->kind == AST_ERROR) {
    /* 语法错误：诊断已由 parser 记入 diag，统一打印到 stderr */
    diag_print_all(diag);
    diag_buf_destroy(&diag);
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  /* Stage ④: 语义分析（sema，语法通过后才进入；语义错误快速失败） */
  vm_t *vm = vm_new(alloc);
  if (!vm) {
    diag_buf_destroy(&diag);
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  sema_t *sema = sema_create(vm, diag, pool, arena);
  if (!sema) {
    diag_buf_destroy(&diag);
    vm_destroy(&vm);
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  bool sema_ok = sema_analyze(sema, ast);
  /* 作用域树与类型登记表是持久化数据，编译器（AST → bcode）按名查找符号
     元数据 / 类型 id，必须存活到编译完成；顺序参照生命周期约定：先取树与
     types 表、再在编译结束后销毁 sema、随后销毁树与 types */
  sema_scope_t *scope_tree = sema->global_scope;
  vec_t *sema_types = sema->types;

  if (!sema_ok) {
    /* 语义诊断已由 sema 记入 diag，统一在出口打印（diag 销毁前） */
    sema_destroy(&sema);
    diag_print_all(diag);
    if (scope_tree) sema_scope_destroy(&scope_tree);
    diag_buf_destroy(&diag);
    vm_destroy(&vm);
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  /* Stage ⑤: 编译（AST + sema 作用域树 → 字节码模块） */
  compiler_t *comp = compiler_new(alloc, vm, diag, pool, scope_tree, sema_types);
  if (!comp) {
    sema_destroy(&sema);
    if (scope_tree) sema_scope_destroy(&scope_tree);
    diag_buf_destroy(&diag);
    vm_destroy(&vm);
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }
  bytecode_t *bc = compiler_compile(comp, ast);
  compiler_destroy(&comp);
  sema_destroy(&sema); /* types 表已随编译器使用完毕，统一在此销毁 */

  if (!bc) {
    /* 编译诊断已记入 diag，统一在出口打印 */
    diag_print_all(diag);
    if (scope_tree) sema_scope_destroy(&scope_tree);
    diag_buf_destroy(&diag);
    vm_destroy(&vm);
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  /* Stage ⑥: 执行（注册函数 → 调用 main） */
  value_t *er = exec_run(vm, bc);
  if (value_is_error(vm, er)) {
    error_data_t *ed = (error_data_t *)value_data(er);
    fprintf(stderr, "run: %s\n",
            ed && ed->message ? string_cstr(ed->message) : "execution error");
    bcode_destroy(&bc);
    if (scope_tree) sema_scope_destroy(&scope_tree);
    diag_buf_destroy(&diag);
    vm_destroy(&vm);
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  value_t *main_fn = scope_lookup(vm->current_scope, STRSLICE_LIT("main"));
  if (!main_fn) {
    fprintf(stderr, "run: no entry function 'main'\n");
    bcode_destroy(&bc);
    if (scope_tree) sema_scope_destroy(&scope_tree);
    diag_buf_destroy(&diag);
    vm_destroy(&vm);
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  value_t *mr = value_call(vm, main_fn, NULL, 0);
  if (value_is_error(vm, mr)) {
    error_data_t *ed = (error_data_t *)value_data(mr);
    fprintf(stderr, "run: %s\n",
            ed && ed->message ? string_cstr(ed->message) : "runtime error");
    bcode_destroy(&bc);
    if (scope_tree) sema_scope_destroy(&scope_tree);
    diag_buf_destroy(&diag);
    vm_destroy(&vm);
    lexer_close(&lexer);
    vec_free(alloc, &pool);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  bcode_destroy(&bc);
  if (scope_tree) sema_scope_destroy(&scope_tree);
  diag_buf_destroy(&diag);
  vm_destroy(&vm);

  lexer_close(&lexer);
  vec_free(alloc, &pool);
  arena_destroy(alloc, &arena);
  delete_allocator(&alloc);

  return 0;
}

/* ---- Test/utility entry: lex into a pool (text dangles after return) ---- */

int driver_lex_file(allocator_t *alloc, const char *path, vec_t **out_pool) {
  lexer_t *lexer = NULL;
  if (driver_lex_into(alloc, path, out_pool, &lexer) != 0) return -1;
  /* Closing the lexer frees the source buffer; token kinds/locations stay
   * valid but token text slices become dangling. Consumers needing the
   * text should keep the lexer alive (see driver_run_file). */
  lexer_close(&lexer);
  return 0;
}

/* ================================================================ */
/* build --emit-asm：编译到字节码（不执行），供反汇编                  */
/* ================================================================ */

/* 编译产物 + 其生命周期所需的全部对象句柄。反汇编必须在 dispose 前完成，
 * 因为 bc->strs 由 bc->alloc 分配，alloc 释放后字符串失效。 */
typedef struct {
    allocator_t   *alloc;
    arena_t       *arena;
    vm_t          *vm;
    diag_buf_t    *diag;
    lexer_t       *lexer;
    vec_t         *pool;
    sema_scope_t  *scope_tree;
    bytecode_t    *bc;
} driver_compiled_t;

static void driver_compiled_dispose(driver_compiled_t *r) {
    if (!r) return;
    if (r->bc)         bcode_destroy(&r->bc);
    if (r->scope_tree) sema_scope_destroy(&r->scope_tree);
    if (r->diag)       diag_buf_destroy(&r->diag);
    if (r->vm)         vm_destroy(&r->vm);
    if (r->lexer)      lexer_close(&r->lexer);
    if (r->pool)       vec_free(r->alloc, &r->pool);
    if (r->arena)      arena_destroy(r->alloc, &r->arena);
    if (r->alloc)      delete_allocator(&r->alloc);
    memset(r, 0, sizeof *r);
}

/* 复用 run 管线 Stage ①-⑤（加载→词法→语法→语义→编译），不执行。
 * 成功返回 0 并填满 *out（bc 与所有句柄有效）；失败返回非 0 且 *out 全 NULL
 * （诊断已打印）。调用方负责在反汇编后 driver_compiled_dispose。 */
static int driver_compile_to_bytecode(const char *path, driver_compiled_t *out) {
    memset(out, 0, sizeof *out);

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        fprintf(stderr, "build: out of memory\n");
        return 1;
    }
    out->alloc = alloc;

    arena_t *arena = arena_new_default(alloc);
    if (!arena) {
        fprintf(stderr, "build: out of memory\n");
        driver_compiled_dispose(out);
        return 1;
    }
    out->arena = arena;

    /* Stage ① + ②：加载 → 词法（词法错误快速失败） */
    vec_t *pool = NULL;
    lexer_t *lexer = NULL;
    int lex_result = driver_lex_into(alloc, path, &pool, &lexer);
    if (lex_result == -2) {
        /* 词法错误已输出到 stderr */
        driver_compiled_dispose(out);
        return 1;
    }
    if (lex_result != 0) {
        fprintf(stderr, "build: cannot open file '%s'\n", path);
        driver_compiled_dispose(out);
        return 1;
    }
    out->pool = pool;
    out->lexer = lexer;

    /* 语法错误由 parser 记入 diag，统一在出口打印 */
    diag_buf_t *diag = diag_buf_new(alloc);
    if (!diag) {
        driver_compiled_dispose(out);
        return 1;
    }
    out->diag = diag;

    /* Stage ③：语法分析 */
    parser_t *parser = parser_create(alloc, arena, pool);
    if (!parser) {
        driver_compiled_dispose(out);
        return 1;
    }
    parser->diag = diag;
    ast_node_t *ast = parser_parse(parser);
    parser_destroy(&parser);
    if (!ast || ast->kind == AST_ERROR) {
        /* 语法错误：诊断已由 parser 记入 diag，统一打印到 stderr */
        diag_print_all(diag);
        driver_compiled_dispose(out);
        return 1;
    }

    /* Stage ④：语义分析 */
    vm_t *vm = vm_new(alloc);
    if (!vm) {
        driver_compiled_dispose(out);
        return 1;
    }
    out->vm = vm;

    sema_t *sema = sema_create(vm, diag, pool, arena);
    if (!sema) {
        driver_compiled_dispose(out);
        return 1;
    }
    bool sema_ok = sema_analyze(sema, ast);

    sema_scope_t *scope_tree = sema->global_scope;
    vec_t *sema_types = sema->types;
    if (!sema_ok) {
        sema_destroy(&sema);
        diag_print_all(diag);
        driver_compiled_dispose(out);
        return 1;
    }

    /* Stage ⑤：编译（AST + sema 作用域树 → 字节码模块） */
    compiler_t *comp = compiler_new(alloc, vm, diag, pool, scope_tree,
                                    sema_types);
    if (!comp) {
        sema_destroy(&sema);
        driver_compiled_dispose(out);
        return 1;
    }
    bytecode_t *bc = compiler_compile(comp, ast);
    compiler_destroy(&comp);
    sema_destroy(&sema); /* types 表已随编译器使用完毕，统一在此销毁 */
    if (!bc) {
        diag_print_all(diag);
        driver_compiled_dispose(out);
        return 1;
    }

    out->scope_tree = scope_tree;
    out->bc = bc;
    return 0;
}

int driver_build_asm(const char *src_path, const char *out_path) {
    if (!src_path || !out_path) return 1;

    driver_compiled_t c;
    int rc = driver_compile_to_bytecode(src_path, &c);
    if (rc != 0) return rc; /* 诊断已由管线打印 */

    int dr = bcode_disasm(c.bc, out_path);
    if (dr != 0) {
        fprintf(stderr, "build: cannot write asm file '%s'\n", out_path);
        driver_compiled_dispose(&c);
        return 1;
    }

    driver_compiled_dispose(&c);
    return 0;
}

int driver_build_bin(const char *src_path, const char *out_path) {
    if (!src_path || !out_path) return 1;

    driver_compiled_t c;
    int rc = driver_compile_to_bytecode(src_path, &c);
    if (rc != 0) return rc; /* 诊断已由管线打印 */

    int sr = bcode_serial(c.bc, out_path);
    if (sr != 0) {
        fprintf(stderr, "build: cannot write bin file '%s'\n", out_path);
        driver_compiled_dispose(&c);
        return 1;
    }

    driver_compiled_dispose(&c);
    return 0;
}

/* ================================================================ */
/* run -asm：汇编 .cxs 文本 → 字节码 → 执行                          */
/* ================================================================ */

/* 执行已加载的字节码模块（复用 run 的 Stage ⑥ 流程：注册函数 → 调用 main）。
 * 只负责执行，不拥有 bc/alloc 生命周期（由调用方释放）。 */
static int driver_execute_bytecode(allocator_t *alloc, bytecode_t *bc) {
    vm_t *vm = vm_new(alloc);
    if (!vm) {
        fprintf(stderr, "run: out of memory\n");
        return 1;
    }

    value_t *er = exec_run(vm, bc);
    if (value_is_error(vm, er)) {
        error_data_t *ed = (error_data_t *)value_data(er);
        fprintf(stderr, "run: %s\n",
                ed && ed->message ? string_cstr(ed->message) : "execution error");
        vm_destroy(&vm);
        return 1;
    }

    value_t *main_fn = scope_lookup(vm->current_scope, STRSLICE_LIT("main"));
    if (!main_fn) {
        fprintf(stderr, "run: no entry function 'main'\n");
        vm_destroy(&vm);
        return 1;
    }

    value_t *mr = value_call(vm, main_fn, NULL, 0);
    if (value_is_error(vm, mr)) {
        error_data_t *ed = (error_data_t *)value_data(mr);
        fprintf(stderr, "run: %s\n",
                ed && ed->message ? string_cstr(ed->message) : "runtime error");
        vm_destroy(&vm);
        return 1;
    }

    vm_destroy(&vm);
    return 0;
}

int driver_run_asm(const char *asm_path) {
    if (!asm_path) {
        fprintf(stderr, "run: no asm file\n");
        return 1;
    }

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        fprintf(stderr, "run: out of memory\n");
        return 1;
    }

    /* 汇编 .cxs 文本 → bytecode_t（alloc 拥有全部内存） */
    bytecode_t *bc = NULL;
    int ar = bcode_asm_from_file(alloc, asm_path, &bc);
    if (ar != 0) {
        /* 汇编诊断已打印 */
        if (bc) bcode_destroy(&bc);
        delete_allocator(&alloc);
        return 1;
    }

    int rc = driver_execute_bytecode(alloc, bc);

    bcode_destroy(&bc);
    delete_allocator(&alloc);
    return rc;
}

/* ================================================================ */
/* run -bin：反序列化 .cxb 二进制 → 字节码 → 执行                    */
/* ================================================================ */

int driver_run_bin(const char *bin_path) {
    if (!bin_path) {
        fprintf(stderr, "run: no bin file\n");
        return 1;
    }

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        fprintf(stderr, "run: out of memory\n");
        return 1;
    }

    /* 反序列化 .cxb 二进制 → bytecode_t（alloc 拥有全部内存） */
    bytecode_t *bc = NULL;
    int dr = bcode_deserial_from_file(alloc, bin_path, &bc);
    if (dr != 0) {
        /* 反序列化诊断已打印 */
        if (bc) bcode_destroy(&bc);
        delete_allocator(&alloc);
        return 1;
    }

    int rc = driver_execute_bytecode(alloc, bc);

    bcode_destroy(&bc);
    delete_allocator(&alloc);
    return rc;
}

/* ================================================================ */
/* 落盘格式互转：.cxs ⇄ .cxb（不执行，不经过源码前端）              */
/* ================================================================ */

int driver_asm_to_bin(const char *asm_path, const char *bin_path) {
    if (!asm_path || !bin_path) return 1;

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        fprintf(stderr, "conv: out of memory\n");
        return 1;
    }

    /* 汇编 .cxs 文本 → bytecode_t */
    bytecode_t *bc = NULL;
    int ar = bcode_asm_from_file(alloc, asm_path, &bc);
    if (ar != 0) {
        /* 汇编诊断已打印 */
        if (bc) bcode_destroy(&bc);
        delete_allocator(&alloc);
        return 1;
    }

    /* 序列化 bytecode_t → .cxb 二进制 */
    int sr = bcode_serial(bc, bin_path);
    if (sr != 0) {
        fprintf(stderr, "conv: cannot write bin file '%s'\n", bin_path);
        bcode_destroy(&bc);
        delete_allocator(&alloc);
        return 1;
    }

    bcode_destroy(&bc);
    delete_allocator(&alloc);
    return 0;
}

int driver_bin_to_asm(const char *bin_path, const char *asm_path) {
    if (!bin_path || !asm_path) return 1;

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        fprintf(stderr, "conv: out of memory\n");
        return 1;
    }

    /* 反序列化 .cxb 二进制 → bytecode_t */
    bytecode_t *bc = NULL;
    int dr = bcode_deserial_from_file(alloc, bin_path, &bc);
    if (dr != 0) {
        /* 反序列化诊断已打印 */
        if (bc) bcode_destroy(&bc);
        delete_allocator(&alloc);
        return 1;
    }

    /* 反汇编 bytecode_t → .cxs 文本 */
    int wr = bcode_disasm(bc, asm_path);
    if (wr != 0) {
        fprintf(stderr, "conv: cannot write asm file '%s'\n", asm_path);
        bcode_destroy(&bc);
        delete_allocator(&alloc);
        return 1;
    }

    bcode_destroy(&bc);
    delete_allocator(&alloc);
    return 0;
}

/* ================================================================ */
/* 输入类型探测（内容嗅探，不依赖扩展名）                            */
/* ================================================================ */

const char *driver_input_kind_name(driver_input_kind_t kind) {
    switch (kind) {
        case DRIVER_INPUT_SOURCE: return "source";
        case DRIVER_INPUT_CXS:    return "cxs";
        case DRIVER_INPUT_CXB:    return "cxb";
        default:                  return "unknown";
    }
}

/* 首 token 是否命中 BCODE_ASM_TABLE 助记符（大小写无关）。 */
static bool is_asm_mnemonic(const char *tok, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < BCODE_ASM_TABLE_COUNT; i++) {
        const bcode_asm_entry_t *e = &BCODE_ASM_TABLE[i];
        if (!e->mnemonic) continue;
        size_t n = strlen(e->mnemonic);
        if (n != len) continue;
        bool eq = true;
        for (size_t k = 0; k < len; k++) {
            char a = tok[k], b = e->mnemonic[k];
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (a != b) { eq = false; break; }
        }
        if (eq) return true;
    }
    /* .byte 伪指令（大小写无关） */
    if (len == 5) {
        char c0 = tok[0] == '.' ? '.' : 0;
        if (c0 == '.' && (tok[1] == 'b' || tok[1] == 'B') &&
            (tok[2] == 'y' || tok[2] == 'Y') &&
            (tok[3] == 't' || tok[3] == 'T') &&
            (tok[4] == 'e' || tok[4] == 'E')) {
            return true;
        }
    }
    return false;
}

/* 该行是否为汇编标签定义：去空白后形如 "name:"（无内嵌空白）。 */
static bool is_asm_label(const char *ls, const char *le) {
    if (le <= ls + 1 || le[-1] != ':') return false;
    for (const char *p = ls; p < le - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t') return false;
        bool ok = (c == '_' || c == '.' || c == '-') ||
                  (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z');
        if (!ok) return false;
    }
    return true;
}

/* 文本是否含 clux 源码关键字（作为词边界，避免误匹配标识符内的子串）。 */
static bool has_source_keyword(const char *s, size_t len) {
    static const char *kws[] = {
        "func", "var", "if", "else", "while", "for", "foreach", "return",
        "struct", "enum", "type", "comptime", "import", "interface", "using",
        NULL,
    };
    for (size_t k = 0; kws[k]; k++) {
        size_t kl = strlen(kws[k]);
        if (kl > len) continue;
        for (size_t i = 0; i + kl <= len; i++) {
            bool eq = true;
            for (size_t j = 0; j < kl; j++) {
                if (s[i + j] != kws[k][j]) { eq = false; break; }
            }
            if (!eq) continue;
            /* 词边界检查：前后不能是标识符字符 */
            bool lb = (i == 0) ||
                      !((s[i - 1] >= 'a' && s[i - 1] <= 'z') ||
                        (s[i - 1] >= 'A' && s[i - 1] <= 'Z') ||
                        (s[i - 1] >= '0' && s[i - 1] <= '9') ||
                        s[i - 1] == '_');
            bool rb = (i + kl == len) ||
                      !((s[i + kl] >= 'a' && s[i + kl] <= 'z') ||
                        (s[i + kl] >= 'A' && s[i + kl] <= 'Z') ||
                        (s[i + kl] >= '0' && s[i + kl] <= '9') ||
                        s[i + kl] == '_');
            if (lb && rb) return true;
        }
    }
    return false;
}

driver_input_kind_t driver_detect_input(const char *path) {
    if (!path) return DRIVER_INPUT_UNKNOWN;

    FILE *fp = fopen(path, "rb");
    if (!fp) return DRIVER_INPUT_UNKNOWN;

    uint8_t head[4096];
    size_t n = fread(head, 1, sizeof head, fp);
    fclose(fp);

    if (n == 0) return DRIVER_INPUT_UNKNOWN;

    /* ① 二进制字节码：magic 判定（100% 可靠） */
    if (n >= 4 && memcmp(head, "CXBC", 4) == 0) {
        return DRIVER_INPUT_CXB;
    }

    /* ② 文本：逐行扫描，首个"有效行"决定性质 */
    const char *cur = (const char *)head;
    const char *end = (const char *)head + n;
    while (cur < end) {
        const char *ls = cur;
        while (cur < end && *cur != '\n') cur++;
        const char *le = cur;
        if (cur < end) cur++;

        while (ls < le && (*ls == ' ' || *ls == '\t' || *ls == '\r')) ls++;
        while (le > ls && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r')) le--;
        if (ls >= le) continue;          /* 空行 */
        if (*ls == ';') continue;        /* 汇编行注释 */
        if (*ls == '[') continue;        /* 遗留 [.section ...] 标记 */
        if (ls[0] == '/' && ls + 1 < le && ls[1] == '/') {
            /* C 风格行注释（源码）：跳过继续找 */
            continue;
        }

        /* 标签定义 → 汇编；否则检查首 token 是否助记符 */
        if (is_asm_label(ls, le)) return DRIVER_INPUT_CXS;
        const char *tp = ls;
        while (tp < le && *tp != ' ' && *tp != '\t') tp++;
        if (is_asm_mnemonic(ls, (size_t)(tp - ls))) return DRIVER_INPUT_CXS;

        /* 该行不像汇编 → 用关键字判定是否为源码 */
        break;
    }

    if (has_source_keyword((const char *)head, n)) {
        return DRIVER_INPUT_SOURCE;
    }
    return DRIVER_INPUT_UNKNOWN;
}
