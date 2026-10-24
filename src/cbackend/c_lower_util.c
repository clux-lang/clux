#include "cbackend/c_lower_internal.h"

#include <stdio.h>
#include <string.h>

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
const char *module_symbol(arena_t *arena, const cir_unit_t *unit,
                          const char *kind, strslice_t name) {
    const char *prefix = unit && unit->sym_prefix ? unit->sym_prefix : "clux_";
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s%s%.*s", prefix, kind ? kind : "",
                     (int)name.len, name.ptr ? name.ptr : "");
    if (n <= 0 || (size_t)n >= sizeof buf) return NULL;
    char *p = (char *)arena_alloc(arena, (size_t)n + 1, 1);
    if (!p) return NULL;
    memcpy(p, buf, (size_t)n + 1);
    return p;
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
