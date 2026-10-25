#include "cbackend/c_writer_internal.h"

static void reverse_functions(cir_function_t **head) {
    cir_function_t *prev = NULL;
    cir_function_t *cur = *head;
    while (cur) {
        cir_function_t *next = cur->next;
        cur->next = prev;
        prev = cur;
        cur = next;
    }
    *head = prev;
}

static void reverse_signatures(cir_signature_t **head) {
    cir_signature_t *prev = NULL;
    cir_signature_t *cur = *head;
    while (cur) {
        cir_signature_t *next = cur->next;
        cur->next = prev;
        prev = cur;
        cur = next;
    }
    *head = prev;
}

static void reverse_globals(cir_global_t **head) {
    cir_global_t *prev = NULL;
    cir_global_t *cur = *head;
    while (cur) {
        cir_global_t *next = cur->next;
        cur->next = prev;
        prev = cur;
        cur = next;
    }
    *head = prev;
}

static const char *path_basename(const char *p) {
    const char *b = p;
    for (const char *s = p; s && *s; s++)
        if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

static void write_bitfield_runtime(string_t *sb) {
    cw_put(sb, "/* ---- bitfield helpers (bit stream shared with VM) ---- */\n");
    cw_put(sb, "struct clux_bf_item {\n");
    cw_put(sb, "    uint32_t off;\n");
    cw_put(sb, "    uint32_t width;\n");
    cw_put(sb, "    uint64_t value;\n");
    cw_put(sb, "};\n\n");
    cw_put(sb, "static uint64_t clux_bf_mask(uint32_t width) {\n");
    cw_put(sb, "    return width >= 64 ? UINT64_MAX : "
               "((UINT64_C(1) << width) - 1);\n");
    cw_put(sb, "}\n\n");
    cw_put(sb, "static void clux_bf_load(const uint8_t *data, uint32_t word_bits,"
               " uint32_t idx, uint64_t *out) {\n");
    cw_put(sb, "    size_t ws = word_bits / 8;\n");
    cw_put(sb, "    uint64_t w = 0;\n");
    cw_put(sb, "    memcpy(&w, data + (size_t)idx * ws, ws);\n");
    cw_put(sb, "    *out = w;\n");
    cw_put(sb, "}\n\n");
    cw_put(sb, "static void clux_bf_store(uint8_t *data, uint32_t word_bits,"
               " uint32_t idx, uint64_t w) {\n");
    cw_put(sb, "    size_t ws = word_bits / 8;\n");
    cw_put(sb, "    memcpy(data + (size_t)idx * ws, &w, ws);\n");
    cw_put(sb, "}\n\n");
    cw_put(sb, "static uint64_t clux_bf_get(const void *p, uint32_t word_bits,"
               " uint32_t offset, uint32_t width) {\n");
    cw_put(sb, "    const uint8_t *data = (const uint8_t *)p;\n");
    cw_put(sb, "    uint32_t idx = offset / word_bits;\n");
    cw_put(sb, "    uint32_t sh = offset % word_bits;\n");
    cw_put(sb, "    uint64_t lo = 0, hi = 0;\n");
    cw_put(sb, "    clux_bf_load(data, word_bits, idx, &lo);\n");
    cw_put(sb, "    uint64_t v = sh ? (lo >> sh) : lo;\n");
    cw_put(sb, "    if (sh + width > word_bits) {\n");
    cw_put(sb, "        clux_bf_load(data, word_bits, idx + 1, &hi);\n");
    cw_put(sb, "        v |= hi << (word_bits - sh);\n");
    cw_put(sb, "    }\n");
    cw_put(sb, "    return v & clux_bf_mask(width);\n");
    cw_put(sb, "}\n\n");
    cw_put(sb, "static void clux_bf_set(void *p, uint32_t word_bits,"
               " uint32_t offset, uint32_t width, uint64_t value) {\n");
    cw_put(sb, "    uint8_t *data = (uint8_t *)p;\n");
    cw_put(sb, "    value &= clux_bf_mask(width);\n");
    cw_put(sb, "    uint32_t idx = offset / word_bits;\n");
    cw_put(sb, "    uint32_t sh = offset % word_bits;\n");
    cw_put(sb, "    uint64_t lo = 0;\n");
    cw_put(sb, "    clux_bf_load(data, word_bits, idx, &lo);\n");
    cw_put(sb, "    uint32_t lobits = width < word_bits - sh ? width :"
               " word_bits - sh;\n");
    cw_put(sb, "    uint64_t lo_mask = clux_bf_mask(lobits) << sh;\n");
    cw_put(sb, "    lo = (lo & ~lo_mask) | ((value << sh) & lo_mask);\n");
    cw_put(sb, "    clux_bf_store(data, word_bits, idx, lo);\n");
    cw_put(sb, "    if (sh + width > word_bits) {\n");
    cw_put(sb, "        uint32_t rest = sh + width - word_bits;\n");
    cw_put(sb, "        uint64_t hi = 0;\n");
    cw_put(sb, "        clux_bf_load(data, word_bits, idx + 1, &hi);\n");
    cw_put(sb, "        uint64_t hi_mask = clux_bf_mask(rest);\n");
    cw_put(sb, "        hi = (hi & ~hi_mask) |"
               " ((value >> (word_bits - sh)) & hi_mask);\n");
    cw_put(sb, "        clux_bf_store(data, word_bits, idx + 1, hi);\n");
    cw_put(sb, "    }\n");
    cw_put(sb, "}\n\n");
}

static const char *guard_name(const cir_unit_t *unit, char *buf, size_t cap) {
    if (unit->header_guard && unit->header_guard[0]) return unit->header_guard;
    const char *tag = unit->module_tag && unit->module_tag[0] ? unit->module_tag
                                                              : "module";
    int n = snprintf(buf, cap, "CLUX_%s_H", tag);
    if (n <= 0 || (size_t)n >= cap) return "CLUX_MODULE_H";
    for (char *p = buf; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    return buf;
}

/* 头文件：导出类型（拓扑序）+ 导出函数/cfunc/全局变量声明。 */
static void write_header(string_t *sb, const cir_unit_t *unit,
                         cir_function_t *fns, cir_signature_t *sigs,
                         cir_global_t *globals) {
    char gbuf[64];
    const char *guard = guard_name(unit, gbuf, sizeof gbuf);
    cw_printf(sb, "/* generated by clux C backend (module %s) */\n",
              unit->module_tag ? unit->module_tag : "");
    cw_printf(sb, "#ifndef %s\n#define %s\n\n", guard, guard);
    cw_put(sb, "#include <stdint.h>\n");
    cw_put(sb, "#include <stddef.h>\n");
    cw_put(sb, "#include <stdbool.h>\n\n");

    if (unit->extern_typedefs) {
        cw_put(sb, "/* ---- C ABI function pointer types ---- */\n");
        for (const cir_raw_t *r = unit->extern_typedefs; r; r = r->next) {
            cw_put(sb, r->code ? r->code : "");
            cw_put(sb, "\n");
        }
        cw_put(sb, "\n");
    }
    if (unit->types) {
        cw_put(sb, "/* ---- exported type declarations ---- */\n");
        cw_write_type_defs(sb, unit->types, true);
        cw_put(sb, "/* ---- exported type definitions (dependency order) ---- */\n");
        cw_emit_header_types(sb, unit->types);
    }
    cw_emit_header_signatures(sb, sigs);
    if (unit->extern_decls_pub) {
        cw_put(sb, "/* ---- exported extern declarations ---- */\n");
        for (const cir_raw_t *r = unit->extern_decls_pub; r; r = r->next) {
            cw_put(sb, r->code ? r->code : "");
            cw_put(sb, "\n");
        }
        cw_put(sb, "\n");
    }
    bool has_fn = false;
    for (cir_function_t *f = fns; f; f = f->next)
        if (f->is_exported) has_fn = true;
    if (has_fn) {
        cw_put(sb, "/* ---- exported functions ---- */\n");
        for (cir_function_t *f = fns; f; f = f->next)
            if (f->is_exported) cw_write_function_decl(sb, f);
        cw_put(sb, "\n");
    }
    bool has_var = false;
    for (cir_global_t *g = globals; g; g = g->next)
        if (g->is_exported) has_var = true;
    if (has_var) {
        cw_put(sb, "/* ---- exported global variables ---- */\n");
        for (cir_global_t *g = globals; g; g = g->next) {
            if (!g->is_exported) continue;
            cw_printf(sb, "extern %s %s;\n",
                      g->type && g->type->c_name ? g->type->c_name : "int32_t",
                      g->name ? g->name : "clux_v_global");
        }
        cw_put(sb, "\n");
    }
    cw_printf(sb, "#endif /* %s */\n", guard);
}

/* .c 文件：包含头文件后输出其余类型、static/导出函数与入口。 */
static void write_c(string_t *sb, const cir_unit_t *unit, const char *c_path,
                    const char *h_include, cir_function_t *fns,
                    cir_signature_t *sigs, cir_global_t *globals) {
    bool has_deferred = false;
    cw_put(sb, "/* generated by clux C backend */\n");
    if (h_include) cw_printf(sb, "#include \"%s\"\n", h_include);
    /* stdio/stdlib/string 按实际用到的运行期设施分别引入：避免不必要地
       把 libc 的全部原型拉进本单元（用户 FFI 声明必须与已包含的头一致）。 */
    if (unit->uses_alloc || unit->uses_opt_unwrap || unit->uses_panic ||
        unit->uses_index_check || unit->uses_slice_check ||
        unit->uses_union_check || unit->test_runner || unit->tests)
        cw_put(sb, "#include <stdlib.h>\n");
    if (unit->uses_alloc || unit->uses_str_cmp || unit->uses_bitfield || unit->uses_mem_eq ||
        unit->uses_str_from_c || unit->uses_slice_check ||
        unit->uses_union_check || unit->test_runner || unit->tests)
        cw_put(sb, "#include <string.h>\n");
    if (unit->uses_index_check || unit->uses_slice_check ||
        unit->uses_union_check || unit->uses_panic || unit->test_runner ||
        unit->tests)
        cw_put(sb, "#include <stdio.h>\n");
    if (unit->test_runner || unit->tests) {
        /* 断言运行时用 `struct clux_str` 传消息：测试单元无条件给出定义
           （include guard 与类型发射器共享，重复定义安全）。必须先于
           `extern void clux_assert(..., struct clux_str);`。 */
        cw_put(sb, "#ifndef CLUX_STR_STRUCT_DEFINED\n");
        cw_put(sb, "#define CLUX_STR_STRUCT_DEFINED\n");
        cw_put(sb, "struct clux_str { const char *data; size_t len; };\n");
        cw_put(sb, "#endif\n");
    }
    if (unit->test_runner) {
        /* 跨模块共享的失败跳转点（各模块的 clux_assert 都 longjmp 到这里）。 */
        cw_put(sb, "void *clux_test_fail[5];\n");
    } else if (unit->tests) {
        cw_put(sb, "extern void *clux_test_fail[5];\n");
        cw_put(sb,
               "extern void clux_assert(bool cond, struct clux_str text);\n");
    }
    if (unit->uses_alloc) {
        cw_put(sb, "static void *clux_new(size_t n, const void *src) {\n");
        cw_put(sb, "    void *p = calloc(1, n);\n");
        cw_put(sb, "    if (!p) abort();\n");
        cw_put(sb, "    if (src) memcpy(p, src, n);\n");
        cw_put(sb, "    return p;\n");
        cw_put(sb, "}\n");
        cw_put(sb, "static void *clux_make(size_t elem_size, size_t len, "
                   "const void *src, size_t count) {\n");
        cw_put(sb, "    void *p = calloc(len ? len : 1, elem_size);\n");
        cw_put(sb, "    if (!p) abort();\n");
        cw_put(sb, "    if (src && count) memcpy(p, src, "
                   "(count < len ? count : len) * elem_size);\n");
        cw_put(sb, "    return p;\n");
        cw_put(sb, "}\n");
        cw_put(sb, "#define clux_free free\n");
    }
    if (unit->uses_opt_unwrap) {
        cw_put(sb, "static void clux_panic_none(void) { abort(); }\n");
    }
    if (unit->uses_panic) {
        cw_put(sb, "#ifndef CLUX_STR_STRUCT_DEFINED\n");
        cw_put(sb, "#define CLUX_STR_STRUCT_DEFINED\n");
        cw_put(sb, "struct clux_str { const char *data; size_t len; };\n");
        cw_put(sb, "#endif\n");
        cw_put(sb,
               "static void clux_panic(struct clux_str msg) {\n");
        cw_put(sb, "    printf(\"panic: %.*s\\n\", (int)msg.len, "
                   "msg.data ? msg.data : \"\");\n");
        cw_put(sb, "    fflush(NULL);\n");
        cw_put(sb, "    _Exit(1);\n");
        cw_put(sb, "}\n");
    }
    if (unit->uses_index_check) {
        cw_put(sb,
               "static int64_t clux_index_check(int64_t idx, int64_t len) {\n");
        cw_put(sb, "    if (idx < 0 || idx >= len) {\n");
        cw_put(sb, "        printf(\"run: array index %lld out of "
                   "bounds (len=%lld)\\n\", (long long)idx, (long long)len);\n");
        cw_put(sb, "        fflush(NULL);\n");
        cw_put(sb, "        abort();\n");
        cw_put(sb, "    }\n");
        cw_put(sb, "    return idx;\n");
        cw_put(sb, "}\n");
    }
    if (unit->uses_slice_check) {
        cw_put(sb,
               "static void *clux_slice_at(const void *slice, int64_t idx, "
               "size_t elem, const char *what) {\n");
        cw_put(sb, "    struct { void *data; uint64_t len; } h;\n");
        cw_put(sb, "    memcpy(&h, slice, sizeof h);\n");
        cw_put(sb, "    if (idx < 0 || (uint64_t)idx >= h.len) {\n");
        cw_put(sb, "        printf(\"run: %s index %lld out of "
                   "bounds (len=%llu)\\n\", what, (long long)idx, "
                   "(unsigned long long)h.len);\n");
        cw_put(sb, "        fflush(NULL);\n");
        cw_put(sb, "        abort();\n");
        cw_put(sb, "    }\n");
        cw_put(sb, "    return (char *)h.data + (size_t)idx * elem;\n");
        cw_put(sb, "}\n");
    }
    if (unit->uses_union_check) {
        cw_put(sb,
               "static void *clux_union_at(const void *p, int64_t expected, "
               "size_t tag_size, size_t payload_off, const char *member) {\n");
        cw_put(sb, "    uint64_t tag = 0;\n");
        cw_put(sb, "    memcpy(&tag, p, tag_size);\n");
        cw_put(sb, "    if ((int64_t)tag != expected) {\n");
        cw_put(sb, "        printf(\"run: union member '%s' is not "
                   "active (tag=%lld, expected=%lld)\\n\", member, "
                   "(long long)tag, (long long)expected);\n");
        cw_put(sb, "        fflush(NULL);\n");
        cw_put(sb, "        abort();\n");
        cw_put(sb, "    }\n");
        cw_put(sb, "    return (char *)p + payload_off;\n");
        cw_put(sb, "}\n");
        cw_put(sb,
               "static void *clux_union_at_lvalue(void *p, int64_t expected, "
               "size_t tag_size, size_t payload_off, const char *member) {\n");
        cw_put(sb, "    uint64_t tag = 0;\n");
        cw_put(sb, "    memcpy(&tag, p, tag_size);\n");
        cw_put(sb, "    if ((int64_t)tag != expected) {\n");
        cw_put(sb, "        printf(\"run: union member '%s' is not "
                   "active (tag=%lld, expected=%lld)\\n\", member, "
                   "(long long)tag, (long long)expected);\n");
        cw_put(sb, "        fflush(NULL);\n");
        cw_put(sb, "        abort();\n");
        cw_put(sb, "    }\n");
        cw_put(sb, "    return (char *)p + payload_off;\n");
        cw_put(sb, "}\n");
    }
    cw_put(sb, "\n");

    if (unit->uses_bitfield) write_bitfield_runtime(sb);

    if (unit->types) {
        cw_put(sb, "/* ---- composite type declarations ---- */\n");
        cw_write_type_defs(sb, unit->types, false);
        cw_put(sb, "/* ---- composite type definitions ---- */\n");
        cw_emit_all_types_except(sb, unit->types, "struct clux_env_");
        cw_emit_bf_helpers(sb, unit->types);
        if (unit->uses_str_cmp) {
            cw_put(sb, "static bool clux_str_eq(struct clux_str a, "
                       "struct clux_str b) {\n");
            cw_put(sb, "    return a.len == b.len && "
                       "(a.len == 0 || memcmp(a.data, b.data, a.len) == 0);\n");
            cw_put(sb, "}\n\n");
        }
    }

    if (unit->uses_str_from_c) {
        cw_put(sb, "static struct clux_str clux_str_from_c(const char *s) {\n");
        cw_put(sb, "    struct clux_str r;\n");
        cw_put(sb, "    r.data = s ? s : \"\";\n");
        cw_put(sb, "    r.len = s ? strlen(s) : 0;\n");
        cw_put(sb, "    return r;\n");
        cw_put(sb, "}\n\n");
    }
    if (!h_include) {
        if (unit->extern_typedefs) {
            cw_put(sb, "/* ---- C ABI function pointer types ---- */\n");
            for (const cir_raw_t *r = unit->extern_typedefs; r; r = r->next) {
                cw_put(sb, r->code ? r->code : "");
                cw_put(sb, "\n");
            }
            cw_put(sb, "\n");
        }
    }
    if (unit->extern_decls) {
        cw_put(sb, "/* ---- extern cfunc declarations ---- */\n");
        for (const cir_raw_t *r = unit->extern_decls; r; r = r->next) {
            cw_put(sb, r->code ? r->code : "");
            cw_put(sb, "\n");
        }
        cw_put(sb, "\n");
    }

    if (unit->test_runner && unit->uses_assert) {
        /* 断言失败：打印信息后 longjmp 回 runner 的 setjmp 点——该用例判为
           FAILED，**进程继续跑后面的用例**。 */
        cw_put(sb, "void clux_assert(bool cond, struct clux_str text) {\n");
        cw_put(sb, "    if (cond) return;\n");
        cw_put(sb, "    printf(\"assert failed: %.*s\\n\", (int)text.len,"
                   " text.data ? text.data : \"\");\n");
        cw_put(sb, "    fflush(NULL);\n");
        cw_put(sb, "    __builtin_longjmp(clux_test_fail, 1);\n");
        cw_put(sb, "}\n\n");
    }

    if (sigs) {
        bool any_priv = false;
        for (cir_signature_t *s = sigs; s; s = s->next)
            if (!s->in_header) any_priv = true;
        if (any_priv) {
            cw_put(sb, "/* ---- function signature declarations ---- */\n");
            for (cir_signature_t *s = sigs; s; s = s->next)
                if (!s->in_header) cw_printf(sb, "struct %s;\n", s->tag);
            cw_put(sb, "\n");
            cw_put(sb, "/* ---- function signature definitions ---- */\n");
            for (cir_signature_t *s = sigs; s; s = s->next)
                if (!s->in_header && !s->emitted) cw_write_signature_def(sb, s);
        }
    }

    /* 闭包 env 结构体字段引用函数签名 struct：签名定义之后发射。 */
    if (unit->types)
        cw_emit_types_with_prefix(sb, unit->types, "struct clux_env_");

    if (fns) {
        cw_put(sb, "/* ---- function declarations ---- */\n");
        for (cir_function_t *f = fns; f; f = f->next)
            cw_write_function_decl(sb, f);
        cw_put(sb, "\n");
        for (const cir_raw_t *r = unit->raw_helpers; r; r = r->next) {
            cw_put(sb, r->code ? r->code : "");
            cw_put(sb, "\n");
        }

        cw_put(sb, "/* ---- global variables and function objects ---- */\n");
        for (cir_function_t *f = fns; f; f = f->next)
            cw_write_function_object(sb, f);
        for (cir_global_t *g = globals; g; g = g->next) {
            cw_printf(sb, "%s%s %s",
                      g->is_exported ? "" : "static ",
                      g->type && g->type->c_name ? g->type->c_name : "int32_t",
                      g->name ? g->name : "clux_v_global");
            if (g->init && !g->needs_init) {
                cw_put(sb, " = ");
                cw_write_expr(sb, g->init);
            }
            cw_put(sb, ";\n");
            if (g->needs_init) has_deferred = true;
        }
        if (has_deferred) {
            cw_put(sb, "\nstatic void clux_init_globals(void) {\n");
            for (cir_global_t *g = globals; g; g = g->next) {
                if (!g->needs_init || !g->init) continue;
                cw_printf(sb, "    %s = ", g->name ? g->name : "clux_v_global");
                cw_write_expr(sb, g->init);
                cw_put(sb, ";\n");
            }
            cw_put(sb, "}\n");
        }
        cw_put(sb, "\n");
    }

    if (fns) {
        cw_put(sb, "/* ---- function implementations ---- */\n");
        for (cir_function_t *f = fns; f; f = f->next) {
            if (f->source_line > 0) {
                cw_printf(sb, "#line %d \"", f->source_line);
                cw_write_c_string(sb, unit->source_path ? unit->source_path
                                                      : "<clux>");
                cw_put(sb, "\"\n");
            }
            cw_write_function_def(sb, f);
        }
    }

    if (unit->test_runner) {
        /* 跨模块用例：先为不在本单元的用例函数声明 extern（多文件测试时
           用例来自各个模块的编译单元）。 */
        for (size_t i = 0; i < unit->test_case_count; i++) {
            const char *sym = unit->test_cases[i].symbol;
            if (!sym) continue;
            bool local = false;
            for (cir_function_t *f = fns; f; f = f->next) {
                if (f->name && strcmp(f->name, sym) == 0) {
                    local = true;
                    break;
                }
            }
            if (!local) cw_printf(sb, "extern void %s(void *env);\n", sym);
        }
        /* 用例表：显示名 + 函数指针（跨模块 run 模式用）。 */
        cw_put(sb, "static void clux_no_test(void *env) { (void)env; }\n");
        cw_printf(sb, "static const char *const clux_test_names[%zu] = {\n",
                  unit->test_case_count);
        for (size_t i = 0; i < unit->test_case_count; i++) {
            cw_put(sb, "    \"");
            cw_write_c_string(sb, unit->test_cases[i].display
                                      ? unit->test_cases[i].display : "");
            cw_put(sb, "\",\n");
        }
        cw_put(sb, "};\n");
        cw_printf(sb, "static void (*const clux_test_fns[%zu])(void *) = {\n",
                  unit->test_case_count);
        for (size_t i = 0; i < unit->test_case_count; i++) {
            const char *sym = unit->test_cases[i].symbol;
            cw_printf(sb, "    %s,\n", sym ? sym : "clux_no_test");
        }
        cw_put(sb, "};\n");
        cw_put(sb, "int main(int argc, char **argv) {\n");
        if (has_deferred)
            cw_put(sb, "    clux_init_globals();\n");
        cw_put(sb, "    if (argc >= 2 && argv[1][0] >= '0' && argv[1][0] <= "
                   "'9') {\n");
        cw_put(sb, "        int idx = 0;\n");
        cw_put(sb, "        for (const char *p = argv[1]; *p; p++) {\n");
        cw_put(sb, "            if (*p < '0' || *p > '9') return 2;\n");
        cw_put(sb, "            idx = idx * 10 + (*p - '0');\n");
        cw_put(sb, "        }\n");
        cw_put(sb, "        if (idx < 0 || (size_t)idx >= "
                   "sizeof clux_test_fns / sizeof clux_test_fns[0]) "
                   "return 3;\n");
        cw_put(sb, "        clux_test_fns[idx](NULL);\n");
        cw_put(sb, "        return 0;\n");
        cw_put(sb, "    }\n");
        /* 无参数：进程内依次跑全部用例；断言失败 longjmp 回来继续下一条。 */
        cw_put(sb, "    size_t total = sizeof clux_test_fns / sizeof "
                   "clux_test_fns[0];\n");
        cw_put(sb, "    size_t failed = 0, passed = 0, suites = 0;\n");
        cw_put(sb, "    for (size_t i = 0; i < total; i++) {\n");
        cw_put(sb, "        size_t n = 0;\n");
        cw_put(sb, "        while (clux_test_names[i][n] && "
                   "clux_test_names[i][n] != '.') n++;\n");
        cw_put(sb, "        bool seen = false;\n");
        cw_put(sb, "        for (size_t k = 0; k < i; k++) {\n");
        cw_put(sb, "            size_t m = 0;\n");
        cw_put(sb, "            while (clux_test_names[k][m] && "
                   "clux_test_names[k][m] != '.') m++;\n");
        cw_put(sb, "            if (m == n && strncmp(clux_test_names[k], "
                   "clux_test_names[i], n) == 0) { seen = true; break; }\n");
        cw_put(sb, "        }\n");
        cw_put(sb, "        if (!seen) suites++;\n");
        cw_put(sb, "    }\n");
        cw_put(sb, "    printf(\"[==========] Running %zu tests from %zu test "
                   "suites.\\n\", total, suites);\n");
        cw_put(sb, "    for (size_t i = 0; i < total; i++) {\n");
        cw_put(sb, "        const char *name = clux_test_names[i];\n");
        cw_put(sb, "        printf(\"[ RUN      ] %s\\n\", name);\n");
        cw_put(sb, "        fflush(NULL);\n");
        cw_put(sb, "        if (__builtin_setjmp(clux_test_fail) == 0) {\n");
        cw_put(sb, "            clux_test_fns[i](NULL);\n");
        cw_put(sb, "            printf(\"[       OK ] %s\\n\", name);\n");
        cw_put(sb, "            passed++;\n");
        cw_put(sb, "        } else {\n");
        cw_put(sb, "            printf(\"[  FAILED  ] %s\\n\", name);\n");
        cw_put(sb, "            failed++;\n");
        cw_put(sb, "        }\n");
        cw_put(sb, "        fflush(NULL);\n");
        cw_put(sb, "    }\n");
        cw_put(sb, "    printf(\"[==========] %zu tests from %zu test suites "
                   "ran.\\n\", total, suites);\n");
        cw_put(sb, "    printf(\"[  PASSED  ] %zu tests.\\n\", passed);\n");
        cw_put(sb, "    if (failed) printf(\"[  FAILED  ] %zu tests.\\n\", "
                   "failed);\n");
        cw_put(sb, "    return failed ? 1 : 0;\n");
        cw_put(sb, "}\n");
    } else {
        for (cir_function_t *f = fns; f; f = f->next) {
            if (!f->is_entry) continue;
            cw_put(sb, "int main(void) {\n");
            if (has_deferred)
                cw_put(sb, "    clux_init_globals();\n");
            if (f->sig && f->sig->ret && f->sig->ret->kind != CIR_TYPE_VOID) {
                cw_printf(sb, "    return (int)%s(NULL);\n", f->name);
            } else {
                cw_printf(sb, "    %s(NULL);\n", f->name);
                cw_put(sb, "    return 0;\n");
            }
            cw_put(sb, "}\n");
        }
    }
    (void)c_path;
}

static int write_string_file(const char *path, string_t *sb) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return 1;
    size_t n = string_len(sb);
    int ok = fwrite(string_cstr(sb), 1, n, fp) == n;
    fclose(fp);
    return ok ? 0 : 1;
}

int cir_write_pair(const cir_unit_t *unit, const char *c_path,
                   const char *h_path, allocator_t *alloc) {
    if (!unit || !c_path || !alloc) return 1;
    cir_function_t *fns = unit->functions;
    reverse_functions(&fns);
    cir_signature_t *sigs = unit->signatures;
    reverse_signatures(&sigs);
    cir_global_t *globals = unit->globals;
    reverse_globals(&globals);

    int rc = 0;
    if (h_path) {
        string_t *hs = string_new(alloc);
        if (!hs) return 1;
        write_header(hs, unit, fns, sigs, globals);
        rc = write_string_file(h_path, hs);
        string_free(&hs);
        if (rc != 0) return rc;
    }

    string_t *sb = string_new(alloc);
    if (!sb) return 1;
    const char *h_include = h_path ? path_basename(h_path) : NULL;
    write_c(sb, unit, c_path, h_include, fns, sigs, globals);
    rc = write_string_file(c_path, sb);
    string_free(&sb);
    return rc;
}

int cir_write_file(const cir_unit_t *unit, const char *path,
                   allocator_t *alloc) {
    return cir_write_pair(unit, path, NULL, alloc);
}
