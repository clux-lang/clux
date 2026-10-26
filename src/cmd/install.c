#include "cmd/install.h"
#include "cmd/cmd.h"
#include "core/allocator.h"
#include "core/arena.h"
#include "core/ffi_cond.h"
#include "module/package.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define INSTALL_MAX_DEPTH 8

static const char *USAGE =
    "usage: clux install [--allow-scripts] [--force]\n"
    "\n"
    "Run `postinstall` hooks declared by clux.mod:\n"
    "  clux install                  list hooks and refuse to run them\n"
    "  clux install --allow-scripts  execute hooks (deps first, then root)\n"
    "  clux install --force          run even if the hook already succeeded\n";

static char *dup_cstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *join_path(const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    while (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\')) dlen--;
    size_t nlen = strlen(name);
    char *p = (char *)malloc(dlen + 1 + nlen + 1);
    if (!p) return NULL;
    memcpy(p, dir, dlen);
    p[dlen] = '/';
    memcpy(p + dlen + 1, name, nlen);
    p[dlen + 1 + nlen] = '\0';
    return p;
}

static bool get_cwd(char *buf, size_t cap) {
#ifdef _WIN32
    return _getcwd(buf, (int)cap) != NULL;
#else
    return getcwd(buf, cap) != NULL;
#endif
}

static bool is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFREG) != 0;
}

static void mkdir_one(const char *path) {
#ifdef _WIN32
    (void)_mkdir(path);
#else
    (void)mkdir(path, 0777);
#endif
}

/* ---- 目录列表（只收子目录） ---- */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} dir_list_t;

static bool dir_list_push(dir_list_t *l, char *path) {
    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 16;
        char **ni = (char **)realloc(l->items, nc * sizeof(char *));
        if (!ni) return false;
        l->items = ni;
        l->cap = nc;
    }
    l->items[l->count++] = path;
    return true;
}

static void dir_list_free(dir_list_t *l) {
    for (size_t i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

static int cmp_cstr(const void *a, const void *b) {
    const char *const *pa = (const char *const *)a;
    const char *const *pb = (const char *const *)b;
    return strcmp(*pa, *pb);
}

static bool list_subdirs(const char *dir, dir_list_t *out) {
#ifdef _WIN32
    char pattern[4096];
    snprintf(pattern, sizeof pattern, "%s/*", dir);
    for (char *p = pattern; *p; p++)
        if (*p == '/') *p = '\\';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        char *child = join_path(dir, fd.cFileName);
        if (!child || !dir_list_push(out, child)) {
            free(child);
            FindClose(h);
            return false;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char *child = join_path(dir, e->d_name);
        if (!child) {
            closedir(d);
            return false;
        }
        struct stat st;
        if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) {
            free(child);
            continue;
        }
        if (!dir_list_push(out, child)) {
            free(child);
            closedir(d);
            return false;
        }
    }
    closedir(d);
#endif
    if (out->count > 1)
        qsort(out->items, out->count, sizeof(char *), cmp_cstr);
    return true;
}

/* ---- marker / hash ---- */

static uint64_t fnv1a64(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

static char *marker_path(const char *root) {
    return join_path(root, ".clux/postinstall.ok");
}

static char *read_first_line(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char buf[512];
    if (!fgets(buf, sizeof buf, f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return dup_cstr(buf);
}

static bool write_marker(const char *root, const char *hash,
                         const char *command) {
    char *dir = join_path(root, ".clux");
    if (!dir) return false;
    mkdir_one(dir);
    free(dir);
    char *path = marker_path(root);
    if (!path) return false;
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(path);
        return false;
    }
    fprintf(f, "%s\n%s\n", hash, command);
    fclose(f);
    free(path);
    return true;
}

/* ---- 进程环境 ---- */

static void set_env(const char *name, const char *value) {
    if (!name) return;
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    setenv(name, value ? value : "", 1);
#endif
}

static int run_command(const char *root, const char *command, const char *os,
                       const char *arch) {
    char cwd[4096];
    if (!get_cwd(cwd, sizeof cwd)) {
        fprintf(stderr, "error: cannot get current directory\n");
        return 1;
    }
    set_env("CLUX_PKG_ROOT", root);
    set_env("CLUX_OS", os);
    set_env("CLUX_ARCH", arch);
    char target[128];
    snprintf(target, sizeof target, "%s-%s", os ? os : "", arch ? arch : "");
    set_env("CLUX_TARGET", target);
#ifdef _WIN32
    if (_chdir(root) != 0) {
        fprintf(stderr, "error: cannot enter package directory '%s'\n", root);
        return 1;
    }
    int rc = system(command);
#else
    if (chdir(root) != 0) {
        fprintf(stderr, "error: cannot enter package directory '%s'\n", root);
        return 1;
    }
    int rc = system(command);
#endif
#ifdef _WIN32
    (void)_chdir(cwd);
#else
    (void)chdir(cwd);
    if (rc != 0) rc = WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
#endif
    return rc;
}

/* ---- 单包执行 ---- */

typedef struct {
    size_t found;
    bool failed;
} install_stats_t;

static int run_one(const char *root, const package_desc_t *desc, bool allow,
                   bool force, const char *os, const char *arch,
                   install_stats_t *st) {
    const char *cmd =
        pkg_postinstall_effective(&desc->postinstall, os, arch);
    if (!cmd || !*cmd) return 0;
    st->found++;

    if (!allow) {
        fprintf(stderr,
                "error: package '%.*s' declares postinstall:\n  %s\n"
                "run `clux install --allow-scripts` to execute package "
                "scripts\n",
                (int)desc->name.len, desc->name.ptr, cmd);
        st->failed = true;
        return 1;
    }

    char hashbuf[32];
    snprintf(hashbuf, sizeof hashbuf, "%016llx",
             (unsigned long long)fnv1a64(cmd));
    char *marker = marker_path(root);
    bool cached = false;
    if (!force && marker) {
        char *old = read_first_line(marker);
        if (old && strcmp(old, hashbuf) == 0) cached = true;
        free(old);
    }
    free(marker);
    if (cached) {
        printf("postinstall: %.*s (cached)\n", (int)desc->name.len,
               desc->name.ptr);
        return 0;
    }

    printf("postinstall: %.*s\n  cwd: %s\n  command: %s\n",
           (int)desc->name.len, desc->name.ptr, root, cmd);
    int rc = run_command(root, cmd, os, arch);
    if (rc != 0) {
        fprintf(stderr, "error: postinstall for '%.*s' failed (exit %d)\n",
                (int)desc->name.len, desc->name.ptr, rc);
        st->failed = true;
        return 1;
    }
    if (!write_marker(root, hashbuf, cmd)) {
        fprintf(stderr, "error: cannot write postinstall marker in '%s'\n",
                root);
        st->failed = true;
        return 1;
    }
    return 0;
}

/* ---- 依赖树（deps/ 下递归；先依赖，后当前包） ---- */

static void install_tree(allocator_t *alloc, arena_t *arena, const char *root,
                         int depth, bool allow, bool force, const char *os,
                         const char *arch, install_stats_t *st) {
    if (depth > INSTALL_MAX_DEPTH) return;
    package_desc_t desc;
    pkg_result_t pr = package_load(alloc, arena, root, &desc);
    if (pr != PKG_OK) {
        fprintf(stderr, "error: cannot load package '%s': %s\n", root,
                pkg_result_name(pr));
        st->failed = true;
        return;
    }

    char *deps = join_path(root, "deps");
    if (deps) {
        dir_list_t list = {0};
        if (list_subdirs(deps, &list)) {
            for (size_t i = 0; i < list.count; i++) {
                char *mod = join_path(list.items[i], "clux.mod");
                if (mod && is_file(mod))
                    install_tree(alloc, arena, list.items[i], depth + 1, allow,
                                 force, os, arch, st);
                free(mod);
            }
        }
        dir_list_free(&list);
        free(deps);
    }

    (void)run_one(root, &desc, allow, force, os, arch, st);
}

int cmd_install(const cmd_args_t *args) {
    if (cmd_args_has(args, "help")) {
        fputs(USAGE, stdout);
        return 0;
    }
    if (cmd_args_pos(args, 0) != NULL) {
        fprintf(stderr, "error: clux install takes no positional arguments\n%s",
                USAGE);
        return 1;
    }

    bool allow = cmd_args_has(args, "allow-scripts");
    bool force = cmd_args_has(args, "force");

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    arena_t *arena = arena_new_default(alloc);
    if (!arena) {
        delete_allocator(&alloc);
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }

    char cwd[4096];
    if (!get_cwd(cwd, sizeof cwd)) {
        fprintf(stderr, "error: cannot get current directory\n");
        arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }

    char *root = NULL;
    package_desc_t root_desc;
    pkg_result_t fr = package_find(alloc, arena, cwd, &root, &root_desc);
    if (fr != PKG_OK) {
        if (fr == PKG_ERR_NOT_FOUND)
            fprintf(stderr, "error: no clux.mod found in current directory tree\n");
        else
            fprintf(stderr, "error: %s\n", pkg_result_name(fr));
        if (root) allocator_free(alloc, (void **)&root);
        arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }

    ffi_defines_add_builtins();
    const char *os = ffi_defines_get("OS_TYPE");
    const char *arch = ffi_defines_get("OS_ARCH");

    install_stats_t st = {0};
    install_tree(alloc, arena, root, 0, allow, force, os, arch, &st);

    if (!st.failed && st.found == 0)
        printf("install: no postinstall scripts\n");

    allocator_free(alloc, (void **)&root);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return st.failed ? 1 : 0;
}
