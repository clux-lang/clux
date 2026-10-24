#include "cmd/build.h"
#include "cmd/cmd.h"
#include "cmd/path.h"
#include "cbackend/c_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *USAGE =
    "usage: clux build <file.cx> [--emit-c[=PATH]] [--emit-obj[=PATH]]\n"
    "                   [--output=PATH]\n"
    "                   [--cc=PATH] [--cflag=FLAG]... [--ldflag=FLAG]...\n"
    "                   [-I DIR]... [--include=DIR]...\n"
    "                   [-L DIR]... [--lib-path=DIR]...\n"
    "                   [-l LIB]... [--link-lib=LIB]...\n"
    "                   [-D KEY=VALUE]...\n";

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

/* 一个可重复选项的收集器：指针数组容量取 `posc + optc`（上界）。 */
typedef struct {
    const char **items;
    size_t count;
    size_t cap;
} strlist_t;

static bool strlist_init(strlist_t *l, size_t cap) {
    l->cap = cap;
    l->count = 0;
    l->items = (const char **)calloc(cap ? cap : 1, sizeof(*l->items));
    return l->items != NULL;
}

static void strlist_push(strlist_t *l, const char *s) {
    if (!s || !*s || l->count >= l->cap) return;
    l->items[l->count++] = s;
}

int cmd_build(const cmd_args_t *args) {
    if (!args) {
        fputs(USAGE, stderr);
        return 1;
    }

    /* 位置参数里可能是 `-D/-I/-L/-l`（短选项 + 空格值），也可能是粘写形式
       `-Ibuild`；`--长选项` 由 cmd_args_parse 收进 opts。 */
    const size_t cap = args->posc + args->optc + 2;
    strlist_t cflags, ldflags, incdirs, libdirs, libs, defs;
    if (!strlist_init(&cflags, cap) || !strlist_init(&ldflags, cap) ||
        !strlist_init(&incdirs, cap) || !strlist_init(&libdirs, cap) ||
        !strlist_init(&libs, cap) || !strlist_init(&defs, cap)) {
        fprintf(stderr, "build: out of memory\n");
        return 1;
    }

    const char *src = NULL;
    const char *compiler = NULL;
    for (size_t i = 0; i < args->posc; i++) {
        const char *a = args->posargs[i];
        if (!a || !a[0]) continue;
        /* 短选项：`-X value` 与 `-Xvalue` 都接受（gcc 风格）。 */
        if (strcmp(a, "-D") == 0 || strcmp(a, "-I") == 0 ||
            strcmp(a, "-L") == 0 || strcmp(a, "-l") == 0) {
            if (i + 1 >= args->posc) {
                fprintf(stderr, "build: option '%s' requires a value\n", a);
                goto fail;
            }
            const char *v = args->posargs[++i];
            if (a[1] == 'D') strlist_push(&defs, v);
            else if (a[1] == 'I') strlist_push(&incdirs, v);
            else if (a[1] == 'L') strlist_push(&libdirs, v);
            else strlist_push(&libs, v);
            continue;
        }
        if (a[0] == '-' && a[1] != '\0' && a[2] != '\0' &&
            (a[1] == 'D' || a[1] == 'I' || a[1] == 'L' || a[1] == 'l')) {
            if (a[1] == 'D') strlist_push(&defs, a + 2);
            else if (a[1] == 'I') strlist_push(&incdirs, a + 2);
            else if (a[1] == 'L') strlist_push(&libdirs, a + 2);
            else strlist_push(&libs, a + 2);
            continue;
        }
        if (a[0] == '-') continue; /* 其它选项（--emit-c/--output=...） */
        if (!src) src = a;
    }

    /* `--长选项`：可重复参数逐个收集（cmd_args_get 只返回第一个）。 */
    for (size_t i = 0; i < args->optc; i++) {
        const cmd_opt_t *o = &args->opts[i];
        if (!o->key) continue;
        bool needs_value = strcmp(o->key, "cc") == 0 ||
                           strcmp(o->key, "cflag") == 0 ||
                           strcmp(o->key, "ldflag") == 0 ||
                           strcmp(o->key, "include") == 0 ||
                           strcmp(o->key, "lib-path") == 0 ||
                           strcmp(o->key, "link-lib") == 0;
        if (needs_value && (!o->value || !*o->value)) {
            fprintf(stderr, "build: option '--%s' requires a value\n", o->key);
            goto fail;
        }
        if (strcmp(o->key, "cc") == 0) compiler = o->value;
        else if (strcmp(o->key, "cflag") == 0) strlist_push(&cflags, o->value);
        else if (strcmp(o->key, "ldflag") == 0) strlist_push(&ldflags, o->value);
        else if (strcmp(o->key, "include") == 0)
            strlist_push(&incdirs, o->value);
        else if (strcmp(o->key, "lib-path") == 0)
            strlist_push(&libdirs, o->value);
        else if (strcmp(o->key, "link-lib") == 0) strlist_push(&libs, o->value);
    }

    if (!src) {
        fputs(USAGE, stderr);
        goto fail;
    }
    if (defs.count) {
        if (cmd_apply_defines(defs.items, defs.count) != 0) goto fail;
    }

    const bool emit_c = cmd_args_has(args, "emit-c");
    const bool emit_obj = cmd_args_has(args, "emit-obj");
    if (emit_c && emit_obj) {
        fprintf(stderr, "build: --emit-c and --emit-obj are mutually "
                        "exclusive\n");
        goto fail;
    }

    cbackend_toolchain_t tc = {
        .compiler = compiler,
        .cflags = cflags.items,
        .cflag_count = cflags.count,
        .ldflags = ldflags.items,
        .ldflag_count = ldflags.count,
        .include_dirs = incdirs.items,
        .include_count = incdirs.count,
        .lib_dirs = libdirs.items,
        .libdir_count = libdirs.count,
        .libs = libs.items,
        .lib_count = libs.count,
    };

    int rc = 1;
    if (emit_c) {
        const char *c_path = cmd_args_get(args, "emit-c");
        char *owned = NULL;
        if (!c_path || !*c_path) {
            owned = cmd_derive_out_path(src, ".c");
            if (!owned) {
                fprintf(stderr, "build: out of memory\n");
                goto done;
            }
            c_path = owned;
        }
        rc = cbackend_generate_c(src, c_path);
        if (rc == 0) printf("generated %s\n", c_path);
        free(owned);
        goto done;
    }

    if (emit_obj) {
        const char *obj = cmd_args_get(args, "emit-obj");
        char *owned = NULL;
        if (!obj || !*obj) {
#ifdef _WIN32
            const char *ext = ".obj";
#else
            const char *ext = ".o";
#endif
            owned = cmd_derive_out_path(src, ext);
            if (!owned) {
                fprintf(stderr, "build: out of memory\n");
                goto done;
            }
            obj = owned;
        }
        rc = cbackend_emit_object(src, obj, &tc);
        if (rc == 0) printf("compiled %s\n", obj);
        free(owned);
        goto done;
    }

    char *out = NULL;
    const char *explicit_out = cmd_args_get(args, "output");
    if (explicit_out && *explicit_out) {
        size_t n = strlen(explicit_out);
        out = (char *)malloc(n + 1);
        if (!out) {
            fprintf(stderr, "build: out of memory\n");
            goto done;
        }
        memcpy(out, explicit_out, n + 1);
    } else {
        out = default_output(src);
    }
    if (!out) {
        fprintf(stderr, "build: out of memory\n");
        goto done;
    }
    rc = cbackend_build_file_ex(src, out, &tc);
    if (rc == 0) printf("built %s\n", out);
    free(out);

done:
    free(cflags.items);
    free(ldflags.items);
    free(incdirs.items);
    free(libdirs.items);
    free(libs.items);
    free(defs.items);
    return rc;

fail:
    free(cflags.items);
    free(ldflags.items);
    free(incdirs.items);
    free(libdirs.items);
    free(libs.items);
    free(defs.items);
    return 1;
}
