#include "cbackend/c_lower_internal.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <stdint.h>
#else
#include <stdint.h>
#include <unistd.h>
#endif

const char *copy_arena(arena_t *arena, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)arena_alloc(arena, n + 1, 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

const char *mangle(arena_t *arena, const char *prefix, strslice_t name) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s%.*s", prefix, (int)name.len,
                     name.ptr ? name.ptr : "");
    if (n <= 0 || (size_t)n >= sizeof buf) return NULL;
    char *p = (char *)arena_alloc(arena, (size_t)n + 1, 1);
    if (!p) return NULL;
    memcpy(p, buf, (size_t)n + 1);
    return p;
}

/* 模块符号名：`<sym_prefix><kind><name>`，例如 `clux_a61b_fn_main`。
 * 所有模块内函数/全局/函数对象/回调 trampoline 都用它，避免多编译单元
 * 链接时同名冲突（非导出符号同时是 static）。 */
/* 解析 `Type::method(...)` / `mod::Type::method(...)` 的 owner 类型。
   先走类型槽位；跨模块 `mod::Type` 用模块导出表兜底。 */
const type_t *lower_scope_ref_owner_type(lower_ctx_t *lc, ast_scope_ref_t *sr) {
    if (!lc || !sr || !sr->type_expr) return NULL;
    const type_t *t = sema_resolve_type_slot(lc->sema, &sr->type_expr);
    if (t) return t;
    if (sr->type_expr->kind != AST_SCOPE_REF) return NULL;
    ast_scope_ref_t *inner = (ast_scope_ref_t *)sr->type_expr;
    if (!inner->type_expr || inner->type_expr->kind != AST_IDENT) return NULL;
    sema_symbol_t *ms = sema_lookup(
        lc->sema->global_scope, ((ast_ident_t *)inner->type_expr)->name);
    if (!ms || ms->kind != SEMA_SYM_MODULE || !ms->module) return NULL;
    sema_symbol_t *ts = sema_module_export_symbol(ms->module, inner->variant);
    if (ts && ts->kind == SEMA_SYM_TYPE) return ts->type;
    return NULL;
}

const char *module_symbol(arena_t *arena, const cir_unit_t *unit,
                          const char *kind, strslice_t name) {
    const char *prefix = unit && unit->sym_prefix ? unit->sym_prefix : "clux_";
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s%s%.*s", prefix, kind ? kind : "",
                     (int)name.len, name.ptr ? name.ptr : "");
    if (n <= 0 || (size_t)n >= sizeof buf) return NULL;
    /* sema 的 mangling 可能把类型文本塞进名字（如 `read_ptr$ref *i32`）：
       C 标识符只允许 [_A-Za-z0-9$]（`$` 是 clang/gcc 扩展），其余替换为
       `_`。跨模块调用点与定义点都走这里，替换保持确定性一致。 */
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '$')
            continue;
        buf[i] = '_';
    }
    char *p = (char *)arena_alloc(arena, (size_t)n + 1, 1);
    if (!p) return NULL;
    memcpy(p, buf, (size_t)n + 1);
    return p;
}

/* ---- 模块指纹：源文件相对项目根目录（cwd）的路径的短 hash ---- */

static void norm_path_slashes(char *s) {
    for (; s && *s; s++)
        if (*s == '\\') *s = '/';
}

static bool path_is_absolute(const char *p) {
    if (!p || !p[0]) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p[1] == ':' && (p[2] == '/' || p[2] == '\\');
}

static bool source_rel_path(const char *src, char *buf, size_t cap) {
    char cwd[4096];
#ifdef _WIN32
    if (!_getcwd(cwd, (int)sizeof cwd)) return false;
#else
    if (!getcwd(cwd, sizeof cwd)) return false;
#endif
    norm_path_slashes(cwd);
    char path[4096];
    snprintf(path, sizeof path, "%s", src);
    norm_path_slashes(path);

    if (!path_is_absolute(path)) {
        while (path[0] == '.' && path[1] == '/')
            memmove(path, path + 2, strlen(path + 2) + 1);
        if (path[0] == '\0' || strncmp(path, "../", 3) == 0) return false;
        snprintf(buf, cap, "%s", path);
        return true;
    }
    size_t n = strlen(cwd);
#ifdef _WIN32
    bool under = _strnicmp(path, cwd, n) == 0;
#else
    bool under = strncmp(path, cwd, n) == 0;
#endif
    if (!under) return false;
    if (path[n] == '/') n++;
    else if (path[n] != '\0') return false;
    snprintf(buf, cap, "%s", path + n);
    return true;
}

/* FNV-1a 32-bit，取低 16 位 → 4 位十六进制 tag（如 "a61b"）。
 * 相对路径就是模块在依赖图里的指纹；同一路径在任意机器/构建目录下稳定。 */
void module_tag_for_path(const char *src, char *out, size_t cap) {
    char rel[4096];
    if (!source_rel_path(src, rel, sizeof rel)) {
        snprintf(rel, sizeof rel, "%s", src ? src : "");
        norm_path_slashes(rel);
    }
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)rel; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    snprintf(out, cap, "%04x", (unsigned)(h & 0xffffu));
}

const char *token_text(arena_t *arena, const token_t *tok) {
    size_t n = 0;
    const char *s = token_get_text(tok, &n);
    if (!s) return NULL;
    char *p = (char *)arena_alloc(arena, n + 1, 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

const type_t *strip(sema_t *sema, const type_t *t) {
    (void)sema;
    return t ? type_strip_quals(t) : NULL;
}
