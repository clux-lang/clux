#include "cmd/build.h"
#include "cmd/cmd.h"
#include "cmd/path.h"
#include "cbackend/c_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *USAGE =
    "usage: clux build <file.cx> [--emit-c[=PATH]] [--output=PATH]\n";

/* 第一个位置参数（跳过 `--flag` 与 `-x`；cmd_args_parse 只保证数量，
   交错出现的选项会让 posargs 不再连续）。 */
static const char *first_positional(const cmd_args_t *args) {
    for (size_t i = 0; i < args->posc; i++) {
        const char *a = args->posargs[i];
        if (!a || a[0] == '\0') continue;
        if (a[0] == '-') continue;
        return a;
    }
    return NULL;
}

static char *default_output(const char *src) {
    size_t n = strlen(src);
    const char *dot = NULL;
    for (size_t i = n; i > 0; i--) {
        if (src[i - 1] == '.') {
            dot = src + i - 1;
            break;
        }
        if (src[i - 1] == '/' || src[i - 1] == '\\') break;
    }
    size_t stem = dot ? (size_t)(dot - src) : n;
#ifdef _WIN32
    const char *suffix = ".exe";
#else
    const char *suffix = "";
#endif
    size_t slen = strlen(suffix);
    char *out = (char *)malloc(stem + slen + 1);
    if (!out) return NULL;
    memcpy(out, src, stem);
    memcpy(out + stem, suffix, slen);
    out[stem + slen] = '\0';
    return out;
}

int cmd_build(const cmd_args_t *args) {
    if (!args) {
        fputs(USAGE, stderr);
        return 1;
    }
    const char *src = first_positional(args);
    if (!src) {
        fputs(USAGE, stderr);
        return 1;
    }

    /* --emit-c[=PATH]：只生成 C 源，不调用 host cc。PATH 省略时与源码同名。 */
    bool emit_c = cmd_args_has(args, "emit-c") ||
                  cmd_args_get(args, "emit-c") != NULL;
    if (emit_c) {
        const char *c_path = cmd_args_get(args, "emit-c");
        char *owned = NULL;
        if (!c_path || !*c_path) {
            owned = cmd_derive_out_path(src, ".c");
            if (!owned) {
                fprintf(stderr, "build: out of memory\n");
                return 1;
            }
            c_path = owned;
        }
        int rc = cbackend_generate_c(src, c_path);
        if (rc == 0) printf("generated %s\n", c_path);
        free(owned);
        return rc;
    }

    char *out = NULL;
    const char *explicit_out = cmd_args_get(args, "output");
    if (explicit_out && *explicit_out) {
        size_t n = strlen(explicit_out);
        out = (char *)malloc(n + 1);
        if (!out) {
            fprintf(stderr, "build: out of memory\n");
            return 1;
        }
        memcpy(out, explicit_out, n + 1);
    } else {
        out = default_output(src);
    }
    if (!out) {
        fprintf(stderr, "build: out of memory\n");
        return 1;
    }
    int rc = cbackend_build_file(src, out);
    if (rc == 0) printf("built %s\n", out);
    free(out);
    return rc;
}
