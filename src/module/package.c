#include "module/package.h"
#include "core/json.h"
#include "core/panic.h"
#include "core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===========================================================================
 * clux.mod 解析实现
 * =========================================================================== */

/* 读取文件全部内容到 arena 分配的缓冲区 */
static char *read_file(allocator_t *alloc, arena_t *arena,
                       const char *path, size_t *out_len) {
    (void)alloc;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    /* 读取全部 */
    /* 先获取文件大小 */
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return NULL; }

    char *buf = (char *)arena_alloc(arena, (size_t)sz + 1, ALIGNOF(char));
    if (!buf) { fclose(fp); return NULL; }

    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

/* 解析依赖规格 "protocol:path@version" → 拆分各部分。
   借用 spec 的文本，不分配新内存。
   格式：protocol:path[@version]
   - protocol 是第一个 ':' 之前的部分
   - path 是 ':' 之后、'@' 之前的部分
   - version 是 '@' 之后的部分（可为空） */
static void parse_dep_spec(strslice_t spec, pkg_dep_t *dep) {
    dep->spec = spec;

    /* 找第一个 ':' */
    const char *colon = NULL;
    for (size_t i = 0; i < spec.len; i++) {
        if (spec.ptr[i] == ':') { colon = spec.ptr + i; break; }
    }
    if (!colon) {
        /* 无协议前缀——整体当作 path */
        dep->protocol = STRSLICE_EMPTY;
        dep->path = spec;
        dep->version = STRSLICE_EMPTY;
        return;
    }
    dep->protocol = strslice_make(spec.ptr, (size_t)(colon - spec.ptr));

    /* 在 ':' 之后找 '@' */
    const char *at = NULL;
    for (const char *p = colon + 1; p < spec.ptr + spec.len; p++) {
        if (*p == '@') { at = p; break; }
    }
    if (!at) {
        dep->path = strslice_make(colon + 1,
                                  (size_t)(spec.ptr + spec.len - colon - 1));
        dep->version = STRSLICE_EMPTY;
    } else {
        dep->path = strslice_make(colon + 1, (size_t)(at - colon - 1));
        dep->version = strslice_make(at + 1,
                                     (size_t)(spec.ptr + spec.len - at - 1));
    }
}

/* 从 JSON value 填充 package_desc_t */
static pkg_result_t fill_from_json(allocator_t *alloc, arena_t *arena,
                                   const json_value_t *root,
                                   const char *dir,
                                   package_desc_t *out) {
    (void)alloc;
    if (!root || root->kind != JSON_OBJECT)
        return PKG_ERR_SCHEMA;

    /* name（必需） */
    const json_value_t *jname = json_object_get(root, "name");
    if (!jname || jname->kind != JSON_STRING)
        return PKG_ERR_SCHEMA;
    out->name = strslice_from_cstr(jname->u.str);

    /* version（可选，默认空） */
    const json_value_t *jver = json_object_get(root, "version");
    if (jver && jver->kind == JSON_STRING)
        out->version = strslice_from_cstr(jver->u.str);
    else
        out->version = STRSLICE_EMPTY;

    /* entry（必需，string | object 互斥） */
    const json_value_t *jentry = json_object_get(root, "entry");
    if (!jentry)
        return PKG_ERR_SCHEMA;

    if (jentry->kind == JSON_STRING) {
        out->entry_kind = PKG_ENTRY_SINGLE;
        out->entry.single = strslice_from_cstr(jentry->u.str);
    } else if (jentry->kind == JSON_OBJECT) {
        out->entry_kind = PKG_ENTRY_MULTI;
        size_t n = jentry->u.obj.count;
        if (n == 0) {
            out->entry.multi.entries = NULL;
            out->entry.multi.count = 0;
        } else {
            pkg_entry_t *entries = (pkg_entry_t *)arena_calloc(
                arena, n, sizeof(pkg_entry_t), ALIGNOF(max_align_t));
            if (!entries) return PKG_ERR_SCHEMA;
            for (size_t i = 0; i < n; i++) {
                entries[i].name =
                    strslice_from_cstr(jentry->u.obj.members[i].key.ptr == NULL
                                           ? ""
                                           : jentry->u.obj.members[i].key.ptr);
                /* key 是 strslice_t，可能不以 NUL 终止。但 parse_string
                   生成的是 arena 拷贝（NUL 终止），所以安全。 */
                /* 直接用 strslice */
                entries[i].name = jentry->u.obj.members[i].key;
                const json_value_t *v = jentry->u.obj.members[i].val;
                if (!v || v->kind != JSON_STRING)
                    return PKG_ERR_SCHEMA;
                entries[i].path = strslice_from_cstr(v->u.str);
            }
            out->entry.multi.entries = entries;
            out->entry.multi.count = n;
        }
    } else {
        return PKG_ERR_SCHEMA;
    }

    /* deps（可选） */
    const json_value_t *jdeps = json_object_get(root, "deps");
    if (jdeps && jdeps->kind == JSON_OBJECT) {
        size_t n = jdeps->u.obj.count;
        if (n > 0) {
            pkg_dep_t *deps = (pkg_dep_t *)arena_calloc(
                arena, n, sizeof(pkg_dep_t), ALIGNOF(max_align_t));
            if (!deps) return PKG_ERR_SCHEMA;
            for (size_t i = 0; i < n; i++) {
                deps[i].name = jdeps->u.obj.members[i].key;
                const json_value_t *v = jdeps->u.obj.members[i].val;
                if (!v || v->kind != JSON_STRING)
                    return PKG_ERR_SCHEMA;
                strslice_t spec = strslice_from_cstr(v->u.str);
                parse_dep_spec(spec, &deps[i]);
            }
            out->deps = deps;
            out->dep_count = n;
        } else {
            out->deps = NULL;
            out->dep_count = 0;
        }
    } else {
        out->deps = NULL;
        out->dep_count = 0;
    }

    /* 包根目录 */
    strncpy(out->root_dir, dir, sizeof(out->root_dir) - 1);
    out->root_dir[sizeof(out->root_dir) - 1] = '\0';

    return PKG_OK;
}

/* ---- 公开 API ---- */

pkg_result_t package_load(allocator_t *alloc, arena_t *arena,
                          const char *dir, package_desc_t *out_desc) {
    if (!alloc || !arena || !dir || !out_desc)
        return PKG_ERR_SCHEMA;
    memset(out_desc, 0, sizeof(*out_desc));

    /* 拼接 dir/clux.mod */
    size_t dlen = strlen(dir);
    char path[4096];
    /* 去除尾部路径分隔符 */
    while (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\'))
        dlen--;
    if (dlen + sizeof("/clux.mod") >= sizeof(path))
        return PKG_ERR_SCHEMA;
    memcpy(path, dir, dlen);
    path[dlen] = '/';
    memcpy(path + dlen + 1, "clux.mod", sizeof("clux.mod")); /* 含 NUL */

    /* 读取文件 */
    size_t len = 0;
    char *text = read_file(alloc, arena, path, &len);
    if (!text)
        return PKG_ERR_NOT_FOUND;

    /* 解析 JSON */
    json_error_t err;
    const json_value_t *root = json_parse(arena, text, len, &err);
    if (!root)
        return PKG_ERR_PARSE;

    /* 填充描述符 */
    return fill_from_json(alloc, arena, root, dir, out_desc);
}

pkg_result_t package_find(allocator_t *alloc, arena_t *arena,
                          const char *start,
                          char **out_dir, package_desc_t *out_desc) {
    if (!alloc || !arena || !start || !out_dir || !out_desc)
        return PKG_ERR_SCHEMA;

    /* 拷贝 start 到可修改的缓冲区 */
    char cur[4096];
    strncpy(cur, start, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';

    /* 最多向上遍历 64 层（防止异常路径无限循环） */
    for (int hop = 0; hop < 64; hop++) {
        /* 去除尾部路径分隔符 */
        size_t clen = strlen(cur);
        while (clen > 0 && (cur[clen - 1] == '/' || cur[clen - 1] == '\\'))
            clen--;
        cur[clen] = '\0';

        if (clen == 0) break;

        /* 尝试 cur/clux.mod */
        char test[4096];
        if (clen + sizeof("/clux.mod") >= sizeof(test))
            return PKG_ERR_SCHEMA;
        memcpy(test, cur, clen);
        test[clen] = '/';
        memcpy(test + clen + 1, "clux.mod", sizeof("clux.mod"));

        FILE *fp = fopen(test, "rb");
        if (fp) {
            fclose(fp);
            /* 找到了——分配目录路径返回给调用方 */
            size_t dlen = strlen(cur);
            char *dir = (char *)allocator_new_ex(
                alloc, "pkg_dir", dlen + 1, NULL, NULL, NULL, 1);
            if (!dir) return PKG_ERR_SCHEMA;
            memcpy(dir, cur, dlen);
            dir[dlen] = '\0';
            *out_dir = dir;

            return package_load(alloc, arena, dir, out_desc);
        }

        /* 向上一级：找最后一个路径分隔符 */
        size_t last_sep = (size_t)-1;
        for (size_t i = 0; i < clen; i++) {
            if (cur[i] == '/' || cur[i] == '\\') last_sep = i;
        }

        if (last_sep == (size_t)-1) {
            /* 无分隔符——只剩一级名字（如 "foo" 或 "C:" 或 "."） */
            /* POSIX 根 "/" 已被 clen==0 处理 */
            /* "." 也到此分支——到顶了 */
            break;
        }

        /* 保留盘符：如 "C:/foo" → "C:" 而非 "C" */
        if (last_sep == 1 && cur[1] == ':') {
            /* Windows 盘符 "C:..." → 截断到 "C:" */
            cur[2] = '\0';
        } else if (last_sep == 0 && cur[0] == '/') {
            /* POSIX 根 "/foo" → "/" */
            cur[1] = '\0';
        } else {
            /* 普通截断 */
            cur[last_sep] = '\0';
        }

        /* 如果截断后为空串，再试一次 "." */
        if (cur[0] == '\0') {
            cur[0] = '.';
            cur[1] = '\0';
        }
    }

    return PKG_ERR_NOT_FOUND;
}

const char *pkg_result_name(pkg_result_t r) {
    switch (r) {
        case PKG_OK:               return "ok";
        case PKG_ERR_NOT_FOUND:    return "clux.mod not found";
        case PKG_ERR_READ_FAILED:  return "failed to read clux.mod";
        case PKG_ERR_PARSE:        return "JSON parse error in clux.mod";
        case PKG_ERR_SCHEMA:       return "invalid clux.mod schema";
    }
    return "unknown";
}

const char *pkg_entry_kind_name(pkg_entry_kind_t k) {
    switch (k) {
        case PKG_ENTRY_SINGLE: return "single";
        case PKG_ENTRY_MULTI:  return "multi";
    }
    return "unknown";
}
