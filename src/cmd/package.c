#include "cmd/package.h"
#include "core/allocator.h"
#include "core/arena.h"
#include "module/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Windows 目录遍历 */
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ===========================================================================
 * CLI 包上下文辅助实现
 * =========================================================================== */

/* 获取当前工作目录 */
static char *get_cwd(void) {
    char buf[4096];
#ifdef _WIN32
    if (!_getcwd(buf, sizeof(buf))) return NULL;
#else
    if (!getcwd(buf, sizeof(buf))) return NULL;
#endif
    return strdup(buf);
}

/* 拼接路径（dir/file），返回 malloc 分配的字符串 */
static char *join_path(const char *dir, const char *file) {
    size_t dlen = strlen(dir);
    size_t flen = strlen(file);
    /* 去除 dir 尾部分隔符 */
    while (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\'))
        dlen--;
    char *path = (char *)malloc(dlen + 1 + flen + 1);
    if (!path) return NULL;
    memcpy(path, dir, dlen);
    path[dlen] = '/';
    memcpy(path + dlen + 1, file, flen);
    path[dlen + 1 + flen] = '\0';
    return path;
}

int cmd_resolve_entry(const char *subdir, char **out_path, char **out_root) {
    if (!out_path) return 1;
    *out_path = NULL;
    if (out_root) *out_root = NULL;

    char *cwd = get_cwd();
    if (!cwd) {
        fprintf(stderr, "error: cannot get current directory\n");
        return 1;
    }

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        free(cwd);
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    arena_t *arena = arena_new_default(alloc);
    if (!arena) {
        delete_allocator(&alloc);
        free(cwd);
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }

    char *pkg_dir = NULL;
    package_desc_t desc;
    pkg_result_t r = package_find(alloc, arena, cwd, &pkg_dir, &desc);
    if (r != PKG_OK) {
        if (r == PKG_ERR_NOT_FOUND) {
            if (subdir) {
                fprintf(stderr, "error: no clux.mod found (from %s)\n", cwd);
            } else {
                fprintf(stderr, "error: no clux.mod found in current directory tree\n");
            }
        } else {
            fprintf(stderr, "error: %s\n", pkg_result_name(r));
        }
        arena_destroy(alloc, &arena);
        if (pkg_dir) allocator_free(alloc, (void **)&pkg_dir);
        delete_allocator(&alloc);
        free(cwd);
        return 1;
    }

    /* 根据入口类型解析 */
    char *entry_path = NULL;

    if (desc.entry_kind == PKG_ENTRY_SINGLE) {
        if (subdir) {
            fprintf(stderr,
                    "error: package '%.*s' has a single entry; "
                    "sub-package '%s' not expected\n",
                    (int)desc.name.len, desc.name.ptr, subdir);
            goto fail;
        }
        /* 单入口：entry.single 是相对路径 */
        entry_path = join_path(pkg_dir, "");
        /* 重新拼接：desc.entry.single 可能不是 NUL 终止 */
        size_t plen = desc.entry.single.len;
        char *single_copy = (char *)malloc(plen + 1);
        if (!single_copy) goto oom;
        memcpy(single_copy, desc.entry.single.ptr, plen);
        single_copy[plen] = '\0';
        free(entry_path);
        entry_path = join_path(pkg_dir, single_copy);
        free(single_copy);
    } else {
        /* 多子包：需要 subdir 参数 */
        if (!subdir) {
            fprintf(stderr,
                    "error: package '%.*s' has multiple entries; "
                    "specify a sub-package name\n",
                    (int)desc.name.len, desc.name.ptr);
            fprintf(stderr, "available sub-packages:\n");
            for (size_t i = 0; i < desc.entry.multi.count; i++) {
                fprintf(stderr, "  %.*s\n",
                        (int)desc.entry.multi.entries[i].name.len,
                        desc.entry.multi.entries[i].name.ptr);
            }
            goto fail;
        }

        /* 查找匹配的子包 */
        bool found = false;
        for (size_t i = 0; i < desc.entry.multi.count; i++) {
            strslice_t name = desc.entry.multi.entries[i].name;
            if (name.len == strlen(subdir) &&
                memcmp(name.ptr, subdir, name.len) == 0) {
                strslice_t path = desc.entry.multi.entries[i].path;
                size_t plen = path.len;
                char *path_copy = (char *)malloc(plen + 1);
                if (!path_copy) goto oom;
                memcpy(path_copy, path.ptr, plen);
                path_copy[plen] = '\0';
                entry_path = join_path(pkg_dir, path_copy);
                free(path_copy);
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr,
                    "error: sub-package '%s' not found in package '%.*s'\n",
                    subdir, (int)desc.name.len, desc.name.ptr);
            goto fail;
        }
    }

    /* 成功 */
    *out_path = entry_path;
    if (out_root) *out_root = strdup(pkg_dir);

    arena_destroy(alloc, &arena);
    if (pkg_dir) allocator_free(alloc, (void **)&pkg_dir);
    delete_allocator(&alloc);
    free(cwd);
    return 0;

oom:
    fprintf(stderr, "error: out of memory\n");
fail:
    if (entry_path) free(entry_path);
    arena_destroy(alloc, &arena);
    if (pkg_dir) allocator_free(alloc, (void **)&pkg_dir);
    delete_allocator(&alloc);
    free(cwd);
    return 1;
}

/* ---- 目录遍历收集 .cx 文件 ---- */

/* 递归收集 .cx 文件到动态数组 */
typedef struct {
    char **items;
    size_t count;
    size_t cap;
} path_list_t;

static int path_list_push(path_list_t *list, char *path) {
    if (list->count >= list->cap) {
        size_t nc = list->cap ? list->cap * 2 : 16;
        char **ni = (char **)realloc(list->items, nc * sizeof(char *));
        if (!ni) return -1;
        list->items = ni;
        list->cap = nc;
    }
    list->items[list->count++] = path;
    return 0;
}

static int has_cx_ext(const char *name) {
    size_t len = strlen(name);
    if (len < 3) return 0;
    return (name[len - 3] == '.' &&
            (name[len - 2] == 'c' || name[len - 2] == 'C') &&
            (name[len - 1] == 'x' || name[len - 1] == 'X'));
}

static int collect_recursive(const char *dir, path_list_t *list) {
#ifdef _WIN32
    char *pattern = join_path(dir, "*");
    if (!pattern) return -1;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return -1;

    do {
        if (strcmp(fd.cFileName, ".") == 0 ||
            strcmp(fd.cFileName, "..") == 0)
            continue;
        char *full = join_path(dir, fd.cFileName);
        if (!full) { FindClose(h); return -1; }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* 跳过 deps 目录 */
            if (strcmp(fd.cFileName, "deps") == 0) {
                free(full);
                continue;
            }
            collect_recursive(full, list);
            free(full);
        } else if (has_cx_ext(fd.cFileName)) {
            if (path_list_push(list, full) != 0) { free(full); FindClose(h); return -1; }
        } else {
            free(full);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char *full = join_path(dir, e->d_name);
        if (!full) { closedir(d); return -1; }

        struct stat st;
        if (stat(full, &st) != 0) { free(full); continue; }

        if (S_ISDIR(st.st_mode)) {
            if (strcmp(e->d_name, "deps") == 0) {
                free(full);
                continue;
            }
            collect_recursive(full, list);
            free(full);
        } else if (has_cx_ext(e->d_name)) {
            if (path_list_push(list, full) != 0) { free(full); closedir(d); return -1; }
        } else {
            free(full);
        }
    }
    closedir(d);
    return 0;
#endif
}

int cmd_collect_sources(const char *root_dir, char ***out_paths,
                        size_t *out_count) {
    if (!root_dir || !out_paths || !out_count) return 1;
    *out_paths = NULL;
    *out_count = 0;

    path_list_t list = {0};
    if (collect_recursive(root_dir, &list) != 0) {
        /* 清理已收集的 */
        for (size_t i = 0; i < list.count; i++) free(list.items[i]);
        free(list.items);
        return 1;
    }

    *out_paths = list.items;
    *out_count = list.count;
    return 0;
}
