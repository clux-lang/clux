#include "cbackend/c_backend.h"

#include "cbackend/c_lower.h"
#include "cbackend/c_opt.h"
#include "cbackend/c_writer.h"
#include "core/allocator.h"
#include "core/arena.h"
#include "core/stream.h"
#include "core/vec.h"
#include "desugar/desugar.h"
#include "diag/diagnostic.h"
#include "parser/ast_program.h"
#include "parser/ast_func_def.h"
#include "parser/ast_test.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "sema/sema.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_file(allocator_t *alloc, const char *path, char **out,
                     size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 1;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 1;
    }
    long n = ftell(fp);
    if (n < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 1;
    }
    char *buf = (char *)allocator_new_ex(alloc, "cbackend.source",
                                         (size_t)n + 1, NULL, NULL, NULL, 1);
    if (!buf) {
        fclose(fp);
        return 1;
    }
    size_t got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    if (got != (size_t)n) {
        allocator_free(alloc, (void **)&buf);
        return 1;
    }
    buf[n] = '\0';
    *out = buf;
    *out_len = (size_t)n;
    return 0;
}

static const char *host_cc(void) {
    const char *cc = getenv("CC");
    if (cc && *cc) return cc;
#ifdef _WIN32
    return "clang";
#else
    return "cc";
#endif
}

static void set_compiler_temp_dir(const char *out_path) {
    const char *slash = NULL;
    for (const char *p = out_path; p && *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    char dir[4096];
    if (slash) {
        size_t n = (size_t)(slash - out_path);
        if (n >= sizeof dir) return;
        memcpy(dir, out_path, n);
        dir[n] = '\0';
    } else {
        strcpy(dir, ".");
    }
#ifdef _WIN32
    _putenv_s("TMP", dir);
    _putenv_s("TEMP", dir);
#else
    setenv("TMP", dir, 1);
    setenv("TEMP", dir, 1);
#endif
}

static int compile_c(const char *c_path, const char *out_path) {
    set_compiler_temp_dir(out_path);
    const char *cc = host_cc();
    char cmd[4096];
    int n;
    if (strchr(cc, ' ') || strchr(cc, '\t'))
        n = snprintf(cmd, sizeof cmd,
                     "\"%s\" -std=c11 -O0 -o \"%s\" \"%s\"", cc, out_path,
                     c_path);
    else
        n = snprintf(cmd, sizeof cmd, "%s -std=c11 -O0 -o \"%s\" \"%s\"", cc,
                     out_path, c_path);
    if (n <= 0 || (size_t)n >= sizeof cmd) {
        fprintf(stderr, "build: compiler command too long\n");
        return 1;
    }
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "build: command: %s\n", cmd);
        fprintf(stderr, "build: C compilation failed (compiler: %s)\n", cc);
        fprintf(stderr, "build: generated C kept at '%s'\n", c_path);
        return 1;
    }
    return 0;
}

/* 编译中间产物 `<out>.clux.c`：编译失败时保留，便于用 #line 映射回 .cx。
 * 用 malloc 而非 allocator：该路径只在本文件内临时使用，不参与分配器记账。 */
static char *derive_c_path(const char *out_path) {
    size_t plen = strlen(out_path) + sizeof(".clux.c");
    char *c_path = (char *)malloc(plen);
    if (!c_path) return NULL;
    snprintf(c_path, plen, "%s.clux.c", out_path);
    return c_path;
}

/* 编译/生成选项。 */
typedef struct cbackend_opts {
    bool     compile;    /* true：调用 host cc 生成可执行文件 */
    bool     tests;      /* 测试运行器模式 */
    char  ***out_names;  /* tests：返回 malloc 的用例显示名数组 */
    size_t  *out_count;
} cbackend_opts_t;

/* 普通构建：test 块不参与运行期（与 driver 的非测试模式一致，直接摘链）。 */
static void strip_test_blocks(ast_program_t *prog) {
    ast_node_t *prev = NULL;
    ast_node_t *s = prog->funcs;
    while (s) {
        ast_node_t *next = s->next;
        if (s->kind == AST_TEST) {
            if (prev) prev->next = next;
            else prog->funcs = next;
            if (prog->funcs_last == s) prog->funcs_last = prev;
            s = next;
            continue;
        }
        prev = s;
        s = next;
    }
}

/* 把顶层 test 块改写成无参 void 函数 `__clux_test_<n>`，并收集显示名。
 * 与 driver 的测试模式同构：sema/compiler/lowering 只看到普通函数。 */
static bool prepare_tests(arena_t *arena, ast_program_t *prog,
                          char ***out_names, size_t *out_count) {
    char **names = NULL;
    size_t count = 0, cap = 0;
    ast_node_t *prev = NULL;
    ast_node_t *s = prog->funcs;
    size_t seq = 0;
    while (s) {
        ast_node_t *next = s->next;
        if (s->kind != AST_TEST) {
            prev = s;
            s = next;
            continue;
        }
        ast_test_t *t = (ast_test_t *)s;
        char nb[64];
        snprintf(nb, sizeof nb, "__clux_test_%zu", seq++);
        size_t nlen = strlen(nb);
        char *ncopy = (char *)arena_alloc(arena, nlen, 1);
        if (!ncopy) goto fail;
        memcpy(ncopy, nb, nlen);
        ast_node_t *fn_node = ast_func_def_new(arena, s->tok_begin, s->tok_end);
        if (!fn_node) goto fail;
        ast_func_def_t *fn = (ast_func_def_t *)fn_node;
        fn->name = strslice_from_bytes(ncopy, nlen);
        fn->params = NULL;
        fn->params_last = NULL;
        fn->return_expr = NULL; /* void */
        fn->body = t->body;
        fn_node->next = next;

        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            char **grown = (char **)realloc(names, ncap * sizeof(*grown));
            if (!grown) goto fail;
            names = grown;
            cap = ncap;
        }
        char *display = (char *)malloc(t->name.len + 1);
        if (!display) goto fail;
        if (t->name.len) memcpy(display, t->name.ptr, t->name.len);
        display[t->name.len] = '\0';
        names[count++] = display;

        if (prev) prev->next = fn_node;
        else prog->funcs = fn_node;
        if (prog->funcs_last == s) prog->funcs_last = fn_node;
        prev = fn_node;
        s = next;
    }
    *out_names = names;
    *out_count = count;
    return true;

fail:
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    return false;
}

static int cbackend_run_impl(const char *src_path, const char *out_path,
                             const char *c_path,
                             const cbackend_opts_t *opts) {
    if (!src_path || !c_path || !opts) return 1;
    bool compile = opts->compile;
    if (compile && !out_path) return 1;

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) return 1;
    arena_t *arena = arena_new_default(alloc);
    diag_buf_t *diag = diag_buf_new(alloc);
    vm_t *vm = vm_new(alloc);
    if (!arena || !diag || !vm) {
        if (vm) vm_destroy(&vm);
        if (diag) diag_buf_destroy(&diag);
        if (arena) arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }

    char *source = NULL;
    size_t source_len = 0;
    if (load_file(alloc, src_path, &source, &source_len) != 0) {
        fprintf(stderr, "build: cannot read '%s'\n", src_path);
        vm_destroy(&vm);
        diag_buf_destroy(&diag);
        arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }

    stream_source_t ss = stream_source_mem(alloc, source, source_len, true);
    istream_t *stream = istream_open(alloc, ss);
    if (!stream) {
        allocator_free(alloc, (void **)&source);
        vm_destroy(&vm);
        diag_buf_destroy(&diag);
        arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }
    lexer_t *lexer = lexer_create(alloc, stream, src_path);
    vec_t *tokens = vec_new(alloc, true);
    if (!lexer || !tokens) {
        if (lexer) lexer_close(&lexer);
        if (tokens) vec_free(alloc, &tokens);
        vm_destroy(&vm);
        diag_buf_destroy(&diag);
        arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }
    for (;;) {
        token_t *tok = lexer_next(lexer);
        if (!tok) break;
        vec_push(tokens, alloc, tok);
        token_kind_t kind = token_get_kind(tok);
        if (kind == TOKEN_TYPE_EOF) break;
        if (kind == TOKEN_TYPE_ERROR) {
            const location_t *loc = token_get_location(tok);
            const char *msg = token_get_error_message(tok);
            fprintf(stderr, "%s:%zu:%zu: error: %s\n", src_path,
                    loc ? loc->begin.line : 0, loc ? loc->begin.column : 0,
                    msg ? msg : "lexical error");
            lexer_close(&lexer);
            vec_free(alloc, &tokens);
            vm_destroy(&vm);
            diag_buf_destroy(&diag);
            arena_destroy(alloc, &arena);
            delete_allocator(&alloc);
            return 1;
        }
    }

    parser_t *parser = parser_create(alloc, arena, tokens);
    if (!parser) {
        lexer_close(&lexer);
        vec_free(alloc, &tokens);
        vm_destroy(&vm);
        diag_buf_destroy(&diag);
        arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }
    parser->diag = diag;
    ast_node_t *ast = parser_parse(parser);
    parser_destroy(&parser);

    int rc = 1;
    sema_t *sema = NULL;
    cir_unit_t *unit = NULL;
    char **test_names = NULL;
    size_t test_count = 0;
    if (ast && ast->kind == AST_PROGRAM) {
        desugar_t ds = desugar_make(arena, diag, tokens);
        desugar_program(&ds, (ast_program_t *)ast);
        bool tests_ok = true;
        if (opts->tests) {
            if (!prepare_tests(arena, (ast_program_t *)ast, &test_names,
                               &test_count)) {
                fprintf(stderr, "test: out of memory\n");
                tests_ok = false;
            } else if (test_count == 0) {
                fprintf(stderr, "test: no test blocks found in '%s'\n",
                        src_path);
                tests_ok = false;
            }
        } else {
            strip_test_blocks((ast_program_t *)ast);
        }
        if (tests_ok && !diag_has_error(diag)) {
            sema = sema_create(vm, diag, tokens, arena);
            if (sema) {
                sema->module_path = src_path;
                if (sema_analyze(sema, ast)) {
                    unit = cbackend_lower(arena, sema, (ast_program_t *)ast,
                                          src_path, opts->tests);
                }
            }
        }
    }

    if (unit) {
        cbackend_optimize(unit);
        if (cir_write_file(unit, c_path, alloc) != 0) {
            fprintf(stderr, "build: cannot write C source '%s'\n", c_path);
        } else if (!compile) {
            rc = 0;
        } else if (compile_c(c_path, out_path) == 0) {
            /* 成功：中间 C 文件是临时的；失败时保留（见 compile_c 提示）。 */
            remove(c_path);
            rc = 0;
        }
    } else if (!diag_has_error(diag) && !(opts->tests && test_count == 0)) {
        /* 没有诊断却拿不到 C unit：内部失败（降级不当），必须显式报出，
           避免静默返回 1。 */
        fprintf(stderr, "build: internal error: no C unit generated\n");
    }

    if (rc == 0 && opts->tests) {
        if (opts->out_names) *opts->out_names = test_names;
        if (opts->out_count) *opts->out_count = test_count;
        test_names = NULL; /* 所有权移交给调用方 */
    }
    if (test_names) {
        for (size_t i = 0; i < test_count; i++) free(test_names[i]);
        free(test_names);
    }

    if (diag_has_error(diag)) diag_print_all(diag);
    if (sema) {
        sema_scope_t *scope = sema->global_scope;
        sema_destroy(&sema);
        if (scope) sema_scope_destroy(&scope);
    }
    lexer_close(&lexer);
    vec_free(alloc, &tokens);
    vm_destroy(&vm);
    diag_buf_destroy(&diag);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return rc;
}

int cbackend_build_file(const char *src_path, const char *out_path) {
    if (!src_path || !out_path) return 1;
    char *c_path = derive_c_path(out_path);
    if (!c_path) {
        fprintf(stderr, "build: out of memory\n");
        return 1;
    }
    cbackend_opts_t opts = {.compile = true};
    int rc = cbackend_run_impl(src_path, out_path, c_path, &opts);
    free(c_path);
    return rc;
}

int cbackend_generate_c(const char *src_path, const char *c_path) {
    if (!src_path || !c_path) return 1;
    cbackend_opts_t opts = {.compile = false};
    return cbackend_run_impl(src_path, NULL, c_path, &opts);
}

int cbackend_build_tests(const char *src_path, const char *out_path,
                         char ***names, size_t *count) {
    if (!src_path || !out_path) return 1;
    char *c_path = derive_c_path(out_path);
    if (!c_path) {
        fprintf(stderr, "test: out of memory\n");
        return 1;
    }
    cbackend_opts_t opts = {
        .compile = true, .tests = true, .out_names = names, .out_count = count};
    int rc = cbackend_run_impl(src_path, out_path, c_path, &opts);
    free(c_path);
    return rc;
}

void cbackend_free_test_names(char **names, size_t count) {
    if (!names) return;
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}
