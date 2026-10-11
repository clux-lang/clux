#include "module/module_path.h"

#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * import 路径分类与规范化（M5 模块系统）
 *
 * 设计要点：
 *   - 纯词法处理，**不访问文件系统**（除 module_path_exists 的显式检查）——
 *     路径规范化结果同时作为 vm->modules 的 key，必须稳定可复现。
 *   - 分隔符统一为 '/'（`/` 与 `\` 都接受，Windows 反斜杠不会破坏字面量语义
 *     之外的路径）。
 *   - `..` 回退只在有上一段时发生；无可回退段时保留（相对路径），绝对路径
 *     起点（POSIX `/` 与 Windows 盘符 `C:`）不可被回退。
 * =========================================================================== */

/* ---- 分类 ---- */

static bool slice_starts_with(strslice_t s, const char *prefix) {
    size_t n = strlen(prefix);
    if (s.len < n) return false;
    return memcmp(s.ptr, prefix, n) == 0;
}

module_import_kind_t module_import_classify(strslice_t raw) {
    /* 唯一的判据是**路径形式**：相对路径（./ 或 ../，分隔符 / 与 \ 均可）。
       刻意不嗅探扩展名——后缀不表示文件类型（`libc.so.6` 也可能只是普通文件，
       某个 `.dll` 也可能不是动态库），按后缀推断会给出误导性诊断，并把错误的
       语义假设固化进语言；动态库绑定应由 M6 用显式语法表达。 */
    if (slice_starts_with(raw, "./") || slice_starts_with(raw, ".\\") ||
        slice_starts_with(raw, "../") || slice_starts_with(raw, "..\\")) {
        return MODULE_IMPORT_MODULE;
    }
    return MODULE_IMPORT_NON_PATH;
}

const char *module_import_kind_name(module_import_kind_t kind) {
    switch (kind) {
        case MODULE_IMPORT_MODULE:   return "module";
        case MODULE_IMPORT_NON_PATH: return "non-relative path";
    }
    return "unknown";
}

/* ---- 规范化 ---- */

/* 在 out[0..len) 中找最后一个 '/'，返回其下标；无则返回 SIZE_MAX。 */
static size_t last_slash(const char *out, size_t len) {
    size_t i = len;
    while (i > 0) {
        i--;
        if (out[i] == '/') return i;
    }
    return (size_t)-1;
}

/**
 * 规范化 NUL 结尾路径（就地读、另分配写）。
 * `keep_dotdot_when_empty`：无可回退段时是否保留 ".."（相对路径 true）。
 */
static char *normalize_copy(allocator_t *alloc, const char *path) {
    size_t len = strlen(path);
    char *out = (char *)allocator_new_ex(alloc, "module.path", len + 2,
                                         NULL, NULL, NULL, 1);
    if (!out) return NULL;

    const char *p = path;
    size_t out_len = 0;

    /* Windows 盘符前缀：`X:`（保留，且不可被 `..` 回退） */
    if (len >= 2 && p[1] == ':' &&
        ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))) {
        out[out_len++] = p[0];
        out[out_len++] = ':';
        p += 2;
    } else if (*p == '/') {
        /* POSIX 绝对路径前缀：保留单个 '/' 作为根标记 */
        out[out_len++] = '/';
        while (*p == '/') p++;
    }

    bool absolute = (out_len > 0);

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t seg_len = (size_t)(p - seg);

        if (seg_len == 1 && seg[0] == '.') continue;   /* "." → 丢弃 */

        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            size_t slash = last_slash(out, out_len);
            /* 盘符/根之后无可回退段，或本身以 ".." 结尾：保留（相对路径向上） */
            bool can_pop = (slash != (size_t)-1) && (slash + 1 < out_len);
            if (can_pop) {
                bool prev_is_dotdot =
                    (out_len - (slash + 1) == 2 &&
                     out[slash + 1] == '.' && out[slash + 2] == '.');
                if (!prev_is_dotdot) {
                    out_len = slash;   /* 截到 '/' 之前 */
                    continue;
                }
            }
            if (absolute) continue;    /* 绝对路径：根部 ".." 丢弃 */
            /* 相对路径：保留 ".." 段（继续向下拼接） */
        }

        if (out_len > 0 && out[out_len - 1] != '/') out[out_len++] = '/';
        memcpy(out + out_len, seg, seg_len);
        out_len += seg_len;
    }

    out[out_len] = '\0';
    return out;
}

char *module_path_normalize(allocator_t *alloc, const char *path) {
    if (!alloc || !path) return NULL;
    /* 先把 '\' 换成 '/'，再走统一规范化 */
    size_t len = strlen(path);
    char *tmp = (char *)allocator_new_ex(alloc, "module.path.tmp", len + 1,
                                         NULL, NULL, NULL, 1);
    if (!tmp) return NULL;
    for (size_t i = 0; i < len; i++) tmp[i] = (path[i] == '\\') ? '/' : path[i];
    tmp[len] = '\0';

    char *out = normalize_copy(alloc, tmp);
    allocator_free(alloc, (void **)&tmp);
    return out;
}

/* ---- 解析（相对 importer 所在目录） ---- */

/* 取路径的目录部分（含结尾 '/'）；无目录返回空串。 */
static size_t dir_prefix_len(const char *path) {
    size_t i = strlen(path);
    while (i > 0) {
        i--;
        if (path[i] == '/' || path[i] == '\\') return i + 1;
    }
    return 0;
}

/* 最后一段是否含 '.'（视为已有扩展名） */
static bool last_segment_has_ext(const char *path) {
    size_t i = strlen(path);
    while (i > 0) {
        i--;
        char c = path[i];
        if (c == '/' || c == '\\') break;
        if (c == '.') return true;
    }
    return false;
}

char *module_path_resolve(allocator_t *alloc, strslice_t raw,
                          const char *importer_path,
                          module_import_kind_t *out_kind) {
    module_import_kind_t kind = module_import_classify(raw);
    if (out_kind) *out_kind = kind;
    if (kind != MODULE_IMPORT_MODULE) return NULL;
    if (!alloc) return NULL;

    /* raw → NUL 结尾 + 分隔符归一（'\' → '/'） */
    size_t raw_len = raw.len;
    char *rel = (char *)allocator_new_ex(alloc, "module.path.rel", raw_len + 1,
                                         NULL, NULL, NULL, 1);
    if (!rel) return NULL;
    for (size_t i = 0; i < raw_len; i++)
        rel[i] = (raw.ptr[i] == '\\') ? '/' : raw.ptr[i];
    rel[raw_len] = '\0';

    /* 省略扩展名 → 补 .cx（显式 .cx 或其他扩展名保持原样） */
    bool append_ext = !last_segment_has_ext(rel);

    size_t dirlen = importer_path ? dir_prefix_len(importer_path) : 0;
    size_t total = dirlen + raw_len + (append_ext ? 3 : 0) + 1;
    char *joined = (char *)allocator_new_ex(alloc, "module.path.joined", total,
                                            NULL, NULL, NULL, 1);
    if (!joined) {
        allocator_free(alloc, (void **)&rel);
        return NULL;
    }
    size_t n = 0;
    if (dirlen > 0) {
        memcpy(joined, importer_path, dirlen);
        n = dirlen;
    }
    memcpy(joined + n, rel, raw_len);
    n += raw_len;
    if (append_ext) {
        joined[n++] = '.';
        joined[n++] = 'c';
        joined[n++] = 'x';
    }
    joined[n] = '\0';

    allocator_free(alloc, (void **)&rel);

    char *out = normalize_copy(alloc, joined);
    allocator_free(alloc, (void **)&joined);
    return out;
}

bool module_path_exists(const char *path) {
    if (!path) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fclose(fp);
    return true;
}
