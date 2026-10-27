#include "module/lockfile.h"
#include "core/ffi_cond.h"
#include "core/json.h"
#include "core/sha256.h"
#include "core/string.h"
#include "module/package.h"

#include <ctype.h>
#include <stdarg.h>
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
#include <unistd.h>
#endif

/* ===========================================================================
 * 小工具
 * =========================================================================== */

static char *dup_cstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static const char *arena_strdup(arena_t *arena, const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)arena_alloc(arena, n, ALIGNOF(char));
    if (!p) return NULL;
    memcpy(p, s, n);
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

static bool is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFREG) != 0;
}

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *s = p; s && *s; s++)
        if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n) return false;
    s += n - m;
    for (size_t i = 0; i < m; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i]))
            return false;
    return true;
}

/* ===========================================================================
 * 目录遍历
 * =========================================================================== */

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
        if (!is_dir(child)) {
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

/* ===========================================================================
 * 包图
 * =========================================================================== */

typedef struct {
    char *root;
    package_desc_t desc;
    const char *rel;
} lock_node_t;

typedef struct {
    lock_node_t *items;
    size_t count;
    size_t cap;
} lock_nodes_t;

static bool nodes_push(lock_nodes_t *n, lock_node_t node) {
    if (n->count == n->cap) {
        size_t nc = n->cap ? n->cap * 2 : 16;
        lock_node_t *ni = (lock_node_t *)realloc(n->items, nc * sizeof(*ni));
        if (!ni) return false;
        n->items = ni;
        n->cap = nc;
    }
    n->items[n->count++] = node;
    return true;
}

static void nodes_free(lock_nodes_t *n) {
    for (size_t i = 0; i < n->count; i++) free(n->items[i].root);
    free(n->items);
}

static bool collect_packages(allocator_t *alloc, arena_t *arena,
                             const char *root, const char *rel, int depth,
                             lock_nodes_t *nodes, char *err, size_t errcap) {
    if (depth > 8) {
        set_err(err, errcap, "dependency nesting too deep at '%s'", root);
        return false;
    }
    package_desc_t desc;
    pkg_result_t pr = package_load(alloc, arena, root, &desc);
    if (pr != PKG_OK) {
        set_err(err, errcap, "cannot load package '%s': %s", root,
                pkg_result_name(pr));
        return false;
    }

    char *deps = join_path(root, "deps");
    if (deps) {
        dir_list_t list = {0};
        if (list_subdirs(deps, &list)) {
            for (size_t i = 0; i < list.count; i++) {
                char *mod = join_path(list.items[i], "clux.mod");
                bool has = mod && is_file(mod);
                free(mod);
                if (!has) continue;
                const char *bn = base_name(list.items[i]);
                const char *child_rel;
                if (strcmp(rel, ".") == 0) {
                    char tmp[4096];
                    snprintf(tmp, sizeof tmp, "deps/%s", bn);
                    child_rel = arena_strdup(arena, tmp);
                } else {
                    char tmp[4096];
                    snprintf(tmp, sizeof tmp, "%s/deps/%s", rel, bn);
                    child_rel = arena_strdup(arena, tmp);
                }
                if (!child_rel ||
                    !collect_packages(alloc, arena, list.items[i], child_rel,
                                      depth + 1, nodes, err, errcap)) {
                    dir_list_free(&list);
                    free(deps);
                    return false;
                }
            }
        }
        dir_list_free(&list);
        free(deps);
    }

    lock_node_t node = {
        .root = dup_cstr(root),
        .desc = desc,
        .rel = arena_strdup(arena, rel),
    };
    if (!node.root || !node.rel || !nodes_push(nodes, node)) {
        free(node.root);
        set_err(err, errcap, "out of memory");
        return false;
    }
    return true;
}

/* ===========================================================================
 * 产物 hash
 * =========================================================================== */

typedef struct {
    const char *key;
    char hash[65];
} lock_art_t;

typedef struct {
    lock_art_t *items;
    size_t count;
    size_t cap;
} lock_arts_t;

static bool art_push(lock_arts_t *a, const char *key, const char *hash) {
    for (size_t i = 0; i < a->count; i++)
        if (strcmp(a->items[i].key, key) == 0) return true; /* dedupe */
    if (a->count == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 16;
        lock_art_t *ni = (lock_art_t *)realloc(a->items, nc * sizeof(*ni));
        if (!ni) return false;
        a->items = ni;
        a->cap = nc;
    }
    a->items[a->count].key = key;
    snprintf(a->items[a->count].hash, sizeof a->items[a->count].hash, "%s",
             hash);
    a->count++;
    return true;
}

static bool path_like(const char *s) {
    if (!s || !*s || s[0] == '-') return false;
    if (strchr(s, '/') || strchr(s, '\\')) return true;
    return ends_with_ci(s, ".a") || ends_with_ci(s, ".lib") ||
           ends_with_ci(s, ".so") || ends_with_ci(s, ".dll") ||
           ends_with_ci(s, ".dylib") || ends_with_ci(s, ".tbd") ||
           ends_with_ci(s, ".o") || ends_with_ci(s, ".obj");
}

static bool hash_artifact(arena_t *arena, const char *root, const char *arg,
                          lock_arts_t *arts, char *err, size_t errcap) {
    const char *key = arg;
    while (key[0] == '.' && key[1] == '/') key += 2;
    char *full = NULL;
    if (key[0] == '/' || (key[0] && key[1] == ':'))
        full = dup_cstr(key);
    else
        full = join_path(root, key);
    if (!full) {
        set_err(err, errcap, "out of memory");
        return false;
    }
    char hash[65];
    if (!clux_sha256_file_hex(full, hash)) {
        set_err(err, errcap, "native artifact is missing: '%s'", full);
        free(full);
        return false;
    }
    free(full);
    const char *k = arena_strdup(arena, key);
    if (!k) {
        set_err(err, errcap, "out of memory");
        return false;
    }
    return art_push(arts, k, hash);
}

static bool collect_artifacts(arena_t *arena, const lock_node_t *node,
                              lock_arts_t *arts, char *err, size_t errcap) {
    ffi_defines_add_builtins();
    const char *os = ffi_defines_get("OS_TYPE");
    const char *arch = ffi_defines_get("OS_ARCH");
    pkg_native_spec_t spec;
    if (!pkg_native_effective(&node->desc.native, os, arch, &spec)) return true;
    for (size_t i = 0; i < spec.link_count; i++) {
        const char *arg = spec.link[i];
        if (!path_like(arg)) continue;
        if (!hash_artifact(arena, node->root, arg, arts, err, errcap))
            return false;
    }
    for (size_t i = 0; i < spec.runtime_count; i++) {
        const char *arg = spec.runtime[i];
        if (!arg || !*arg) continue;
        if (!hash_artifact(arena, node->root, arg, arts, err, errcap))
            return false;
    }
    return true;
}

/* ===========================================================================
 * postinstall
 * =========================================================================== */

typedef struct {
    const char *command;
    char command_hash[65];
    const char *script; /* NULL = 无法从命令推导脚本文件 */
    char script_hash[65];
} lock_hook_t;

static size_t split_args(char *s, char **out, size_t max) {
    size_t n = 0;
    char *p = s;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        char quote = 0;
        if (*p == '"' || *p == '\'') quote = *p++;
        char *start = p;
        while (*p && (quote ? *p != quote : (*p != ' ' && *p != '\t')))
            p++;
        if (*p) *p++ = '\0';
        out[n++] = start;
    }
    return n;
}

static const char *infer_script(char *cmd, const char *root) {
    (void)root;
    char *tok[16];
    size_t n = split_args(cmd, tok, 16);
    if (n == 0) return NULL;
    const char *cand = NULL;
    const char *b0 = base_name(tok[0]);
    if (ends_with_ci(tok[0], ".sh") || ends_with_ci(tok[0], ".ps1") ||
        ends_with_ci(tok[0], ".bat") || ends_with_ci(tok[0], ".cmd")) {
        cand = tok[0];
    } else if ((strcmp(b0, "sh") == 0 || strcmp(b0, "bash") == 0 ||
                strcmp(b0, "dash") == 0 || strcmp(b0, "zsh") == 0) &&
               n > 1 && tok[1][0] != '-') {
        cand = tok[1];
    } else if (strcmp(b0, "powershell") == 0 || strcmp(b0, "pwsh") == 0) {
        for (size_t i = 1; i + 1 < n; i++) {
            if (strcmp(tok[i], "-File") == 0 || strcmp(tok[i], "-f") == 0) {
                cand = tok[i + 1];
                break;
            }
        }
    } else if (strcmp(b0, "cmd") == 0 || strcmp(b0, "cmd.exe") == 0) {
        for (size_t i = 1; i + 1 < n; i++) {
            if (strcmp(tok[i], "/c") == 0 || strcmp(tok[i], "/C") == 0) {
                if (ends_with_ci(tok[i + 1], ".bat") ||
                    ends_with_ci(tok[i + 1], ".cmd"))
                    cand = tok[i + 1];
                break;
            }
        }
    }
    if (!cand) return NULL;
    if (cand[0] == '/' || (cand[0] && cand[1] == ':')) return cand;
    return cand; /* 相对包根；调用方拼接 */
}

bool clux_lock_postinstall_script(const char *root, const char *command,
                                  char *script_path, size_t path_cap,
                                  char *script_hash, size_t hash_cap) {
    if (script_path && path_cap) script_path[0] = '\0';
    if (script_hash && hash_cap) script_hash[0] = '\0';
    if (!root || !command || !*command) return true;
    size_t clen = strlen(command);
    if (clen >= 4096) return false;
    char copy[4096];
    memcpy(copy, command, clen + 1);
    const char *script = infer_script(copy, root);
    if (!script) return true;

    char *full = NULL;
    if (script[0] == '/' || (script[0] && script[1] == ':'))
        full = dup_cstr(script);
    else
        full = join_path(root, script);
    if (!full) return false;
    if (!is_file(full)) {
        free(full);
        return false;
    }
    char hash[65];
    if (!clux_sha256_file_hex(full, hash)) {
        free(full);
        return false;
    }
    free(full);
    const char *key = script;
    while (key[0] == '.' && key[1] == '/') key += 2;
    if (script_path && path_cap) {
        snprintf(script_path, path_cap, "%s", key);
    }
    if (script_hash && hash_cap) {
        snprintf(script_hash, hash_cap, "%s", hash);
    }
    return true;
}

static bool build_hook(arena_t *arena, const lock_node_t *node, lock_hook_t *h,
                       char *err, size_t errcap) {
    memset(h, 0, sizeof(*h));
    ffi_defines_add_builtins();
    const char *cmd = pkg_postinstall_effective(
        &node->desc.postinstall, ffi_defines_get("OS_TYPE"),
        ffi_defines_get("OS_ARCH"));
    if (!cmd || !*cmd) return true;
    h->command = cmd;
    clux_sha256_hex(cmd, strlen(cmd), h->command_hash);

    char spath[4096];
    char shash[65];
    if (!clux_lock_postinstall_script(node->root, cmd, spath, sizeof spath,
                                      shash, sizeof shash)) {
        set_err(err, errcap, "postinstall script is missing for '%s'",
                node->rel);
        return false;
    }
    if (spath[0]) {
        h->script = arena_strdup(arena, spath);
        if (!h->script) {
            set_err(err, errcap, "out of memory");
            return false;
        }
        snprintf(h->script_hash, sizeof h->script_hash, "%s", shash);
    }
    return true;
}

/* ===========================================================================
 * JSON 写入
 * =========================================================================== */

static void json_str(string_t *sb, const char *s) {
    string_append_cstr(sb, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': string_append_cstr(sb, "\\\""); break;
        case '\\': string_append_cstr(sb, "\\\\"); break;
        case '\n': string_append_cstr(sb, "\\n"); break;
        case '\r': string_append_cstr(sb, "\\r"); break;
        case '\t': string_append_cstr(sb, "\\t"); break;
        default:
            if (*p < 0x20) {
                char esc[8];
                snprintf(esc, sizeof esc, "\\u%04x", *p);
                string_append_cstr(sb, esc);
            } else {
                string_append_char(sb, (char)*p);
            }
        }
    }
    string_append_cstr(sb, "\"");
}

static bool write_lockfile(allocator_t *alloc, arena_t *arena,
                           const char *root, lock_nodes_t *nodes, char *err,
                           size_t errcap) {
    string_t *sb = string_new(alloc);
    if (!sb) {
        set_err(err, errcap, "out of memory");
        return false;
    }
    ffi_defines_add_builtins();
    const char *os = ffi_defines_get("OS_TYPE");
    const char *arch = ffi_defines_get("OS_ARCH");
    char target[128];
    snprintf(target, sizeof target, "%s-%s", os ? os : "", arch ? arch : "");
    string_append_cstr(sb, "{\n  \"version\": 1,\n  \"target\": ");
    json_str(sb, target);
    string_append_cstr(sb, ",\n  \"packages\": [\n");
    for (size_t i = 0; i < nodes->count; i++) {
        lock_node_t *n = &nodes->items[i];
        char modpath[4096];
        snprintf(modpath, sizeof modpath, "%s/clux.mod", n->root);
        char modhash[65];
        if (!clux_sha256_file_hex(modpath, modhash)) {
            set_err(err, errcap, "cannot hash '%s'", modpath);
            string_free(&sb);
            return false;
        }
        lock_arts_t arts = {0};
        if (!collect_artifacts(arena, n, &arts, err, errcap)) {
            free(arts.items);
            string_free(&sb);
            return false;
        }
        lock_hook_t hook;
        if (!build_hook(arena, n, &hook, err, errcap)) {
            free(arts.items);
            string_free(&sb);
            return false;
        }

        string_append_cstr(sb, "    {\n      \"name\": ");
        json_str(sb, n->desc.name.ptr ? n->desc.name.ptr : "");
        string_append_cstr(sb, ",\n      \"version\": ");
        json_str(sb, n->desc.version.ptr ? n->desc.version.ptr : "");
        string_append_cstr(sb, ",\n      \"path\": ");
        json_str(sb, n->rel);
        string_append_cstr(sb, ",\n      \"mod_sha256\": ");
        json_str(sb, modhash);

        if (hook.command) {
            string_append_cstr(sb, ",\n      \"postinstall\": {\n        "
                                   "\"command\": ");
            json_str(sb, hook.command);
            string_append_cstr(sb, ",\n        \"command_sha256\": ");
            json_str(sb, hook.command_hash);
            if (hook.script) {
                string_append_cstr(sb, ",\n        \"script\": ");
                json_str(sb, hook.script);
                string_append_cstr(sb, ",\n        \"script_sha256\": ");
                json_str(sb, hook.script_hash);
            }
            string_append_cstr(sb, "\n      }");
        }

        string_append_cstr(sb, ",\n      \"artifacts\": {");
        for (size_t k = 0; k < arts.count; k++) {
            string_append_cstr(sb, k ? ",\n        " : "\n        ");
            json_str(sb, arts.items[k].key);
            string_append_cstr(sb, ": ");
            json_str(sb, arts.items[k].hash);
        }
        if (arts.count) string_append_cstr(sb, "\n      ");
        string_append_cstr(sb, "}\n    }");
        if (i + 1 < nodes->count) string_append_cstr(sb, ",");
        string_append_cstr(sb, "\n");
        free(arts.items);
    }
    string_append_cstr(sb, "  ]\n}\n");

    char lockpath[4096];
    snprintf(lockpath, sizeof lockpath, "%s/clux.lock", root);
    FILE *f = fopen(lockpath, "wb");
    if (!f) {
        set_err(err, errcap, "cannot write '%s'", lockpath);
        string_free(&sb);
        return false;
    }
    size_t len = string_len(sb);
    bool ok = fwrite(string_data(sb), 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    string_free(&sb);
    if (!ok) {
        set_err(err, errcap, "failed writing '%s'", lockpath);
        return false;
    }
    return true;
}

/* ===========================================================================
 * JSON 读取 / 校验
 * =========================================================================== */

static char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

static const json_value_t *obj_str(const json_value_t *o, const char *key,
                                   const char **out) {
    const json_value_t *v = json_object_get(o, key);
    if (!v || v->kind != JSON_STRING) return NULL;
    if (out) *out = v->u.str;
    return v;
}

static const json_value_t *find_pkg(const json_value_t *pkgs,
                                    const char *rel) {
    if (!pkgs || pkgs->kind != JSON_ARRAY) return NULL;
    for (size_t i = 0; i < pkgs->u.arr.count; i++) {
        const json_value_t *o = pkgs->u.arr.items[i];
        if (!o || o->kind != JSON_OBJECT) continue;
        const char *p = NULL;
        if (obj_str(o, "path", &p) && strcmp(p, rel) == 0) return o;
    }
    return NULL;
}

static bool verify_node(const lock_node_t *n, const json_value_t *pkg,
                        char *err, size_t errcap) {
    const char *s = NULL;
    if (!obj_str(pkg, "name", &s) ||
        strcmp(s, n->desc.name.ptr ? n->desc.name.ptr : "") != 0) {
        set_err(err, errcap, "package '%s': name mismatch", n->rel);
        return false;
    }
    if (!obj_str(pkg, "version", &s) ||
        strcmp(s, n->desc.version.ptr ? n->desc.version.ptr : "") != 0) {
        set_err(err, errcap, "package '%s': version mismatch", n->rel);
        return false;
    }
    char modpath[4096];
    snprintf(modpath, sizeof modpath, "%s/clux.mod", n->root);
    char modhash[65];
    if (!clux_sha256_file_hex(modpath, modhash)) {
        set_err(err, errcap, "cannot hash '%s'", modpath);
        return false;
    }
    if (!obj_str(pkg, "mod_sha256", &s) || strcmp(s, modhash) != 0) {
        set_err(err, errcap, "package '%s': clux.mod changed", n->rel);
        return false;
    }

    return true;
}

static bool clux_lock_verify_impl(allocator_t *alloc, arena_t *arena,
                                  const char *root, char *err, size_t errcap) {
    char lockpath[4096];
    snprintf(lockpath, sizeof lockpath, "%s/clux.lock", root);
    size_t text_len = 0;
    char *text = slurp(lockpath, &text_len);
    if (!text) {
        set_err(err, errcap, "clux.lock not found in '%s'; run `clux install`",
                root);
        return false;
    }
    json_error_t jerr;
    memset(&jerr, 0, sizeof jerr);
    const json_value_t *doc = json_parse(arena, text, text_len, &jerr);
    free(text);
    if (!doc || doc->kind != JSON_OBJECT) {
        set_err(err, errcap, "cannot parse clux.lock%s%s", jerr.msg ? ": " : "",
                jerr.msg ? jerr.msg : "");
        return false;
    }
    const json_value_t *ver = json_object_get(doc, "version");
    if (!ver || ver->kind != JSON_NUMBER || (int)ver->u.num != 1) {
        set_err(err, errcap, "unsupported clux.lock version");
        return false;
    }
    ffi_defines_add_builtins();
    char target[128];
    snprintf(target, sizeof target, "%s-%s",
             ffi_defines_get("OS_TYPE") ? ffi_defines_get("OS_TYPE") : "",
             ffi_defines_get("OS_ARCH") ? ffi_defines_get("OS_ARCH") : "");
    const char *lock_target = NULL;
    if (!obj_str(doc, "target", &lock_target) ||
        strcmp(lock_target, target) != 0) {
        set_err(err, errcap,
                "clux.lock was generated for another platform (%s); run `clux "
                "install`",
                lock_target ? lock_target : "unknown");
        return false;
    }
    const json_value_t *pkgs = json_object_get(doc, "packages");
    if (!pkgs || pkgs->kind != JSON_ARRAY) {
        set_err(err, errcap, "clux.lock has no packages array");
        return false;
    }

    lock_nodes_t nodes = {0};
    if (!collect_packages(alloc, arena, root, ".", 0, &nodes, err, errcap)) {
        nodes_free(&nodes);
        return false;
    }
    if (pkgs->u.arr.count != nodes.count) {
        set_err(err, errcap,
                "clux.lock package count changed; run `clux install`");
        nodes_free(&nodes);
        return false;
    }
    bool ok = true;
    const char *s = NULL;
    for (size_t i = 0; i < nodes.count && ok; i++) {
        lock_node_t *n = &nodes.items[i];
        const json_value_t *pkg = find_pkg(pkgs, n->rel);
        if (!pkg) {
            set_err(err, errcap, "package '%s' is missing from clux.lock",
                    n->rel);
            ok = false;
            break;
        }
        if (!verify_node(n, pkg, err, errcap)) {
            ok = false;
            break;
        }

        lock_hook_t hook;
        if (!build_hook(arena, n, &hook, err, errcap)) {
            ok = false;
            break;
        }
        const json_value_t *jp = json_object_get(pkg, "postinstall");
        if (hook.command) {
            if (!jp || jp->kind != JSON_OBJECT) {
                set_err(err, errcap,
                        "package '%s': postinstall changed; run `clux install`",
                        n->rel);
                ok = false;
                break;
            }
            if (!obj_str(jp, "command", &s) ||
                strcmp(s, hook.command) != 0 ||
                !obj_str(jp, "command_sha256", &s) ||
                strcmp(s, hook.command_hash) != 0) {
                set_err(err, errcap,
                        "package '%s': postinstall command changed; run "
                        "`clux install`",
                        n->rel);
                ok = false;
                break;
            }
            if (hook.script) {
                if (!obj_str(jp, "script", &s) ||
                    strcmp(s, hook.script) != 0 ||
                    !obj_str(jp, "script_sha256", &s) ||
                    strcmp(s, hook.script_hash) != 0) {
                    set_err(err, errcap,
                            "package '%s': postinstall script changed; run "
                            "`clux install`",
                            n->rel);
                    ok = false;
                    break;
                }
            } else if (json_object_get(jp, "script")) {
                set_err(err, errcap,
                        "package '%s': postinstall script changed; run "
                        "`clux install`",
                        n->rel);
                ok = false;
                break;
            }
        } else if (jp && jp->kind != JSON_NULL) {
            set_err(err, errcap,
                    "package '%s': unexpected postinstall in clux.lock",
                    n->rel);
            ok = false;
            break;
        }

        lock_arts_t arts = {0};
        if (!collect_artifacts(arena, n, &arts, err, errcap)) {
            free(arts.items);
            ok = false;
            break;
        }
        const json_value_t *ja = json_object_get(pkg, "artifacts");
        if (!ja || ja->kind != JSON_OBJECT || ja->u.obj.count != arts.count) {
            set_err(err, errcap,
                    "package '%s': native artifacts changed; run `clux install`",
                    n->rel);
            free(arts.items);
            ok = false;
            break;
        }
        for (size_t k = 0; k < arts.count && ok; k++) {
            const json_value_t *hv =
                json_object_get(ja, arts.items[k].key);
            if (!hv || hv->kind != JSON_STRING ||
                strcmp(hv->u.str, arts.items[k].hash) != 0) {
                set_err(err, errcap,
                        "package '%s': artifact '%s' changed; run `clux "
                        "install`",
                        n->rel, arts.items[k].key);
                ok = false;
            }
        }
        free(arts.items);
    }
    nodes_free(&nodes);
    return ok;
}

/* ===========================================================================
 * 公开 API
 * =========================================================================== */

bool clux_lock_write(allocator_t *alloc, arena_t *arena, const char *root,
                     char *err, size_t errcap) {
    if (!alloc || !arena || !root) return false;
    lock_nodes_t nodes = {0};
    if (!collect_packages(alloc, arena, root, ".", 0, &nodes, err, errcap)) {
        nodes_free(&nodes);
        return false;
    }
    bool ok = write_lockfile(alloc, arena, root, &nodes, err, errcap);
    nodes_free(&nodes);
    return ok;
}

bool clux_lock_verify(allocator_t *alloc, arena_t *arena, const char *root,
                      char *err, size_t errcap) {
    if (!alloc || !arena || !root) return false;
    return clux_lock_verify_impl(alloc, arena, root, err, errcap);
}

bool clux_lock_verify_for_source(const char *src, char *err, size_t errcap) {
    if (!src || !*src) {
        set_err(err, errcap, "no source path");
        return false;
    }
    char dir[4096];
    if (is_dir(src)) {
        snprintf(dir, sizeof dir, "%s", src);
    } else {
        snprintf(dir, sizeof dir, "%s", src);
        char *slash = strrchr(dir, '/');
        char *bslash = strrchr(dir, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        if (slash) {
            if (slash == dir)
                slash[1] = '\0';
            else
                *slash = '\0';
        } else {
            snprintf(dir, sizeof dir, ".");
        }
    }

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        set_err(err, errcap, "out of memory");
        return false;
    }
    arena_t *arena = arena_new_default(alloc);
    if (!arena) {
        delete_allocator(&alloc);
        set_err(err, errcap, "out of memory");
        return false;
    }
    char *root = NULL;
    package_desc_t desc;
    pkg_result_t pr = package_find(alloc, arena, dir, &root, &desc);
    bool ok = false;
    if (pr != PKG_OK) {
        set_err(err, errcap, "no clux.mod found for '%s'", src);
    } else {
        ok = clux_lock_verify_impl(alloc, arena, root, err, errcap);
    }
    if (root) allocator_free(alloc, (void **)&root);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return ok;
}
