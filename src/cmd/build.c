#include "cmd/build.h"
#include "cmd/cmd.h"
#include "cmd/path.h"
#include "cbackend/c_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static const char *USAGE =
    "usage: clux build <file.cx> [--emit-c[=PATH]] [--emit-obj[=PATH]]\n"
    "                   [--build-dir=DIR] [--output=PATH]\n"
    "                   [--cc=PATH] [--cflag=FLAG]... [--ldflag=FLAG]...\n"
    "                   [-I DIR]... [--include=DIR]...\n"
    "                   [-L DIR]... [--lib-path=DIR]...\n"
    "                   [-l LIB]... [--link-lib=LIB]...\n"
    "                   [-D KEY=VALUE]...\n";

/* -------------------------------------------------------------------------
 * 路径工具：构建目录镜像（类似 cmake 的 build/）
 * ---------------------------------------------------------------------- */

static void norm_seps(char *s) {
    for (; s && *s; s++)
        if (*s == '\\') *s = '/';
}

static bool get_cwd(char *buf, size_t cap) {
#ifdef _WIN32
    return _getcwd(buf, (int)cap) != NULL;
#else
    return getcwd(buf, cap) != NULL;
#endif
}

static bool is_absolute_path(const char *p) {
    if (!p || !p[0]) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p[1] == ':' && (p[2] == '/' || p[2] == '\\');
}

static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *s = p; s && *s; s++)
        if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

/* 若 `src` 位于 cwd 之下，返回相对路径（写入 buf）。`src`/cwd 都按 '/' 归一。 */
static bool rel_to_cwd(const char *src, char *buf, size_t cap) {
    char cwd[4096];
    if (!get_cwd(cwd, sizeof cwd)) return false;
    norm_seps(cwd);
    char path[4096];
    snprintf(path, sizeof path, "%s", src);
    norm_seps(path);

    if (!is_absolute_path(path)) {
        /* 相对路径：`./a` 去掉前缀；`../a` 视为在 cwd 之外。 */
        while (path[0] == '.' && path[1] == '/') memmove(path, path + 2, strlen(path + 2) + 1);
        if (path[0] == '\0' || strncmp(path, "../", 3) == 0) return false;
        snprintf(buf, cap, "%s", path);
        return true;
    }
    size_t n = strlen(cwd);
    bool under = false;
#ifdef _WIN32
    under = _strnicmp(path, cwd, n) == 0;
#else
    under = strncmp(path, cwd, n) == 0;
#endif
    if (!under) return false;
    if (path[n] == '/') n++;
    else if (path[n] != '\0') return false;
    snprintf(buf, cap, "%s", path + n);
    return true;
}

/* 去掉最后一个路径分量的扩展名（`cmd/main.cx` → `cmd/main`）。 */
static void strip_ext(char *rel) {
    char *dot = strrchr(rel, '.');
    if (!dot) return;
    if (strchr(dot, '/')) return; /* '.' 在目录部分 */
    *dot = '\0';
}

static char *concat_path(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    char *p = (char *)malloc(na + nb + 1);
    if (!p) return NULL;
    memcpy(p, a, na);
    memcpy(p + na, b, nb);
    p[na + nb] = '\0';
    return p;
}

/* 构建目录镜像：`<build-dir>/<src 相对 cwd 的路径（去扩展名）>`。
 * 源文件本身已经在构建目录内时，去掉该前缀（避免 `build/build/...`）。 */
static char *mirror_base(const char *src, const char *build_dir) {
    char rel[4096];
    if (!rel_to_cwd(src, rel, sizeof rel)) {
        snprintf(rel, sizeof rel, "%s", base_name(src));
    }

    char bd[4096];
    snprintf(bd, sizeof bd, "%s", build_dir);
    norm_seps(bd);
    size_t bl = strlen(bd);
    while (bl > 1 && bd[bl - 1] == '/') bd[--bl] = '\0';
    char bdrel[4096];
    bdrel[0] = '\0';
    if (is_absolute_path(bd)) {
        (void)rel_to_cwd(bd, bdrel, sizeof bdrel);
    } else {
        snprintf(bdrel, sizeof bdrel, "%s", bd);
        while (bdrel[0] == '.' && bdrel[1] == '/')
            memmove(bdrel, bdrel + 2, strlen(bdrel + 2) + 1);
    }
    size_t brl = strlen(bdrel);
    while (brl > 0 && bdrel[brl - 1] == '/') bdrel[--brl] = '\0';
    if (brl > 0 && strncmp(rel, bdrel, brl) == 0 && rel[brl] == '/')
        memmove(rel, rel + brl + 1, strlen(rel + brl + 1) + 1);

    strip_ext(rel);
    char *base = concat_path(bd, "/");
    if (!base) return NULL;
    char *full = concat_path(base, rel);
    free(base);
    return full;
}

/* mkdir -p（忽略已存在）。 */
static bool ensure_dir(const char *path) {
    if (!path || !*path) return true;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    norm_seps(buf);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
#ifdef _WIN32
        (void)_mkdir(buf);
#else
        (void)mkdir(buf, 0777);
#endif
        *p = '/';
    }
#ifdef _WIN32
    (void)_mkdir(buf);
#else
    (void)mkdir(buf, 0777);
#endif
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

/* 为文件路径创建父目录。 */
static bool ensure_parent_dir(const char *file) {
    if (!file || !*file) return true;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", file);
    norm_seps(buf);
    char *slash = strrchr(buf, '/');
    if (!slash) return true;
    *slash = '\0';
    return ensure_dir(buf);
}

/* 头文件路径：与 .c 配对（`.c` → `.h`；否则追加 `.h`）。 */
static char *h_path_for(const char *c_path) {
    size_t n = strlen(c_path);
    char *h = (char *)malloc(n + 3);
    if (!h) return NULL;
    memcpy(h, c_path, n + 1);
    if (n >= 2 && h[n - 2] == '.' && h[n - 1] == 'c') {
        h[n - 1] = 'h';
    } else {
        h[n] = '.';
        h[n + 1] = 'h';
        h[n + 2] = '\0';
    }
    return h;
}

/* ------------------------------------------------------------------------- */

static char *default_exe_name(const char *base) {
#ifdef _WIN32
    return concat_path(base, ".exe");
#else
    return concat_path(base, "");
#endif
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

/* 构建实现：`src_override` 非 NULL 时忽略位置参数里的源文件（供 run 复用）；
   `out_exe` 非 NULL 时返回实际可执行文件路径（malloc，调用方 free）；
   `quiet` 抑制 "built ..." 输出。 */
static int build_impl(const cmd_args_t *args, const char *src_override,
                      char **out_exe, bool quiet) {
    if (!args) {
        fputs(USAGE, stderr);
        return 1;
    }
    if (out_exe) *out_exe = NULL;

    /* 位置参数里可能是 `-D/-I/-L/-l/-B`（短选项 + 空格值），也可能是粘写形式
       `-Ibuild`；`--长选项` 由 cmd_args_parse 收进 opts。 */
    const size_t cap = args->posc + args->optc + 2;
    strlist_t cflags, ldflags, incdirs, libdirs, libs, defs;
    if (!strlist_init(&cflags, cap) || !strlist_init(&ldflags, cap) ||
        !strlist_init(&incdirs, cap) || !strlist_init(&libdirs, cap) ||
        !strlist_init(&libs, cap) || !strlist_init(&defs, cap)) {
        fprintf(stderr, "build: out of memory\n");
        return 1;
    }

    const char *src = src_override;
    const char *compiler = NULL;
    const char *build_dir = NULL;
    for (size_t i = 0; i < args->posc; i++) {
        const char *a = args->posargs[i];
        if (!a || !a[0]) continue;
        /* 短选项：`-X value` 与 `-Xvalue` 都接受（gcc 风格）。 */
        if (strcmp(a, "-D") == 0 || strcmp(a, "-I") == 0 ||
            strcmp(a, "-L") == 0 || strcmp(a, "-l") == 0 ||
            strcmp(a, "-B") == 0) {
            if (i + 1 >= args->posc) {
                fprintf(stderr, "build: option '%s' requires a value\n", a);
                goto fail;
            }
            const char *v = args->posargs[++i];
            if (a[1] == 'D') strlist_push(&defs, v);
            else if (a[1] == 'I') strlist_push(&incdirs, v);
            else if (a[1] == 'L') strlist_push(&libdirs, v);
            else if (a[1] == 'l') strlist_push(&libs, v);
            else build_dir = v;
            continue;
        }
        if (a[0] == '-' && a[1] != '\0' && a[2] != '\0' &&
            (a[1] == 'D' || a[1] == 'I' || a[1] == 'L' || a[1] == 'l' ||
             a[1] == 'B')) {
            if (a[1] == 'D') strlist_push(&defs, a + 2);
            else if (a[1] == 'I') strlist_push(&incdirs, a + 2);
            else if (a[1] == 'L') strlist_push(&libdirs, a + 2);
            else if (a[1] == 'l') strlist_push(&libs, a + 2);
            else build_dir = a + 2;
            continue;
        }
        if (a[0] == '-') continue; /* 其它选项（--emit-c/--output=...） */
        if (!src_override && !src) src = a;
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
                           strcmp(o->key, "link-lib") == 0 ||
                           strcmp(o->key, "build-dir") == 0;
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
        else if (strcmp(o->key, "build-dir") == 0) build_dir = o->value;
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
    char *base = NULL;
    char *def_c = NULL;
    char *def_obj = NULL;
    char *def_exe = NULL;
    if (!build_dir || !*build_dir) build_dir = "build";
    base = mirror_base(src, build_dir);
    if (!base) {
        fprintf(stderr, "build: out of memory\n");
        goto done;
    }
#ifdef _WIN32
    const char *obj_ext = ".obj";
#else
    const char *obj_ext = ".o";
#endif
    def_c = concat_path(base, ".c");
    def_obj = concat_path(base, obj_ext);
    def_exe = default_exe_name(base);
    if (!def_c || !def_obj || !def_exe) {
        fprintf(stderr, "build: out of memory\n");
        goto done;
    }

    if (emit_c) {
        const char *c_path = cmd_args_get(args, "emit-c");
        char *owned = NULL;
        if (!c_path || !*c_path) {
            c_path = def_c;
        } else {
            owned = (char *)malloc(strlen(c_path) + 1);
            if (owned) memcpy(owned, c_path, strlen(c_path) + 1);
            c_path = owned ? owned : c_path;
        }
        if (!ensure_parent_dir(c_path)) {
            fprintf(stderr, "build: cannot create output directory for '%s'\n",
                    c_path);
            free(owned);
            goto done;
        }
        cbackend_program_t pgm = {
            .build_dir = build_dir, .entry_c = c_path, .emit_c_only = true};
        rc = cbackend_build_program(src, NULL, &pgm, &tc);
        if (rc == 0) {
            char *h = h_path_for(c_path);
            if (h && *h) printf("generated %s, %s\n", c_path, h);
            else printf("generated %s\n", c_path);
            free(h);
        }
        free(owned);
        goto done;
    }

    if (emit_obj) {
        const char *obj = cmd_args_get(args, "emit-obj");
        char *owned = NULL;
        if (!obj || !*obj) {
            obj = def_obj;
        } else {
            owned = (char *)malloc(strlen(obj) + 1);
            if (owned) memcpy(owned, obj, strlen(obj) + 1);
            obj = owned ? owned : obj;
        }
        if (!ensure_parent_dir(obj) || !ensure_parent_dir(def_c)) {
            fprintf(stderr, "build: cannot create output directory for '%s'\n",
                    obj);
            free(owned);
            goto done;
        }
        cbackend_program_t pgm = {
            .build_dir = build_dir,
            .entry_c = def_c,
            .entry_obj = obj,
            .object_only = true,
        };
        rc = cbackend_build_program(src, NULL, &pgm, &tc);
        if (rc == 0) printf("compiled %s\n", obj);
        free(owned);
        goto done;
    }

    char *exe = NULL;
    const char *explicit_out = cmd_args_get(args, "output");
    if (explicit_out && *explicit_out) {
        exe = (char *)malloc(strlen(explicit_out) + 1);
        if (exe) memcpy(exe, explicit_out, strlen(explicit_out) + 1);
    } else {
        exe = def_exe;
        def_exe = NULL; /* 所有权转给 exe */
    }
    if (!exe) {
        fprintf(stderr, "build: out of memory\n");
        goto done;
    }
    if (!ensure_parent_dir(exe) || !ensure_parent_dir(def_c) ||
        !ensure_parent_dir(def_obj)) {
        fprintf(stderr, "build: cannot create output directory for '%s'\n",
                exe);
        free(exe);
        goto done;
    }
    cbackend_program_t pgm = {
        .build_dir = build_dir, .entry_c = def_c, .entry_obj = def_obj};
    rc = cbackend_build_program(src, exe, &pgm, &tc);
    if (rc == 0) {
        if (out_exe) *out_exe = strdup(exe);
        if (!quiet) printf("built %s\n", exe);
    }
    free(exe);

done:
    free(base);
    free(def_c);
    free(def_obj);
    free(def_exe);
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

int cmd_build(const cmd_args_t *args) {
    return build_impl(args, NULL, NULL, false);
}

int cmd_build_for_run(const cmd_args_t *args, const char *src,
                      char **out_exe) {
    return build_impl(args, src, out_exe, true);
}
