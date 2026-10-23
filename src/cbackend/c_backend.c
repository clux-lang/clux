#include "cbackend/c_backend.h"

#include "cbackend/c_lower.h"
#include "cbackend/c_writer.h"
#include "core/allocator.h"
#include "core/arena.h"
#include "core/stream.h"
#include "core/vec.h"
#include "desugar/desugar.h"
#include "diag/diagnostic.h"
#include "parser/ast_program.h"
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

int cbackend_build_file(const char *src_path, const char *out_path) {
    if (!src_path || !out_path) return 1;

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
    char *c_path = NULL;
    if (ast && ast->kind == AST_PROGRAM) {
        desugar_t ds = desugar_make(arena, diag, tokens);
        desugar_program(&ds, (ast_program_t *)ast);
        if (!diag_has_error(diag)) {
            sema = sema_create(vm, diag, tokens, arena);
            if (sema) {
                sema->module_path = src_path;
                if (sema_analyze(sema, ast)) {
                    unit = cbackend_lower(arena, sema, (ast_program_t *)ast,
                                          src_path);
                }
            }
        }
    }

    if (unit) {
        size_t plen = strlen(out_path) + sizeof(".clux.c");
        c_path = (char *)allocator_new_ex(alloc, "cbackend.cpath", plen, NULL,
                                          NULL, NULL, 1);
        if (c_path) {
            snprintf(c_path, plen, "%s.clux.c", out_path);
            if (cir_write_file(unit, c_path, alloc) == 0 &&
                compile_c(c_path, out_path) == 0) {
                remove(c_path);
                rc = 0;
            }
        }
    }

    if (diag_has_error(diag)) diag_print_all(diag);
    if (c_path) allocator_free(alloc, (void **)&c_path);
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
