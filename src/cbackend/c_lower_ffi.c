/* M6 FFI：extern lib / extern cfunc 的 C 后端 lowering。
 *
 * 契约（与 VM 的 driver + libffi 路径对齐）：
 *   - `extern lib <别名> from "<库名>"`：**不生成任何 C 代码**，只把库名翻译成
 *     宿主编译器的链接参数（`"*"` = 进程自身，不链接；通用名 → `-l<name>`；
 *     显式路径/文件名 → 原样交给链接器）。`assert(<cond>)` 的裁剪在前脱糖
 *     阶段完成，因此这里只会看到仍然生效的声明。
 *   - `extern cfunc <库>::<名>(参数...): 返回`：生成 `extern` 原型（C ABI 类型），
 *     调用点直接调用真实 C 符号。类型映射与 `vm/ffi_lib.c` 的 §2 约束一致：
 *     整数/浮点/bool/str/opaque/enum/struct/cunion/指针/cfunc。
 *   - `cfunc_ptr(f)`：为无捕获的 clux 函数生成一个 C 包装（trampoline），
 *     供 C 侧回调。带捕获的闭包没有 C 等价物 → 拒绝。
 *   - `data(s)`：取序列首元素指针（`(s).data`），C ABI 适配用。
 *
 * `str` 的 C ABI 表示是 `const char *`（与 VM 的 str = char* 对齐）；本后端的
 * clux `str` 值在 C 里是 `struct clux_str { const char *data; size_t len; }`，
 * 因此边界上要做两个方向的转换：
 *   - 实参：`(v).data`；
 *   - 返回：`clux_str_from_c(<char*>)`（`strlen` 求 len；NULL → 空串）。
 */

#include "cbackend/c_lower_internal.h"

#include "core/string.h"
#include "vm/type_cfunc.h"
#include "vm/type_enum.h"
#include "vm/type_ptr.h"

#include <stdio.h>
#include <string.h>

/* ============================== 小工具 ============================== */

static const char *slice_str(lower_ctx_t *lc, strslice_t s) {
    char *p = (char *)arena_alloc(lc->arena, s.len + 1, 1);
    if (!p) return NULL;
    if (s.len) memcpy(p, s.ptr, s.len);
    p[s.len] = '\0';
    return p;
}

static const char *cat_str(lower_ctx_t *lc, const char *a, const char *b) {
    size_t na = a ? strlen(a) : 0, nb = b ? strlen(b) : 0;
    char *p = (char *)arena_alloc(lc->arena, na + nb + 1, 1);
    if (!p) return NULL;
    if (na) memcpy(p, a, na);
    if (nb) memcpy(p + na, b, nb);
    p[na + nb] = '\0';
    return p;
}

/* 聚合里是否含 `str` 字段：本后端 str = {char*, len}（16 字节），C ABI 的
 * str 是 8 字节指针——按值传聚合会布局不一致，必须拒绝。 */
static bool aggregate_has_str(const type_t *t) {
    t = type_strip_ownership(t);
    if (!t) return false;
    switch (t->kind) {
    case TYPE_KIND_STR:
        return true;
    case TYPE_KIND_STRUCT: {
        size_t n = struct_type_field_count(t);
        for (size_t i = 0; i < n; i++) {
            const struct_field_t *f = struct_type_field(t, i);
            if (f && aggregate_has_str(f->type)) return true;
        }
        return false;
    }
    case TYPE_KIND_CUNION: {
        size_t n = cunion_type_member_count(t);
        for (size_t i = 0; i < n; i++) {
            const cunion_member_t *m = cunion_type_member(t, i);
            if (m && aggregate_has_str(m->type)) return true;
        }
        return false;
    }
    case TYPE_KIND_ARRAY:
        return aggregate_has_str(array_type_elem(t));
    case TYPE_KIND_OPTION:
        return aggregate_has_str(type_option_inner(t));
    default:
        return false;
    }
}

/* ======================= clux 类型 → C ABI 类型名 ======================= */

static const char *ffi_c_type(lower_ctx_t *lc, const type_t *t_raw, bool *ok);

/* cfunc(...) → C 函数指针 typedef。依赖（嵌套 cfunc 参数/返回）先登记，
 * 因此 typedef 文本的生成顺序天然满足"先定义后引用"。 */
static const char *ffi_cfunc_typedef(lower_ctx_t *lc, const type_t *t,
                                     bool *ok) {
    for (lower_cfn_typedef_t *d = lc->cfn_typedefs; d; d = d->next)
        if (d->src == t) return d->tag;

    char tagbuf[96];
    snprintf(tagbuf, sizeof tagbuf, "%scfn_%u",
             (lc->unit && lc->unit->sym_prefix) ? lc->unit->sym_prefix
                                                : "clux_",
             lc->ffi_seq++);
    lower_cfn_typedef_t *d = (lower_cfn_typedef_t *)arena_calloc(
        lc->arena, 1, sizeof(*d), ALIGNOF(max_align_t));
    if (!d) {
        *ok = false;
        return NULL;
    }
    d->src = t;
    d->tag = copy_arena(lc->arena, tagbuf);
    d->next = lc->cfn_typedefs;
    lc->cfn_typedefs = d;

    const func_sig_t *sig = cfunc_type_sig(t);
    if (!sig) {
        *ok = false;
        return d->tag;
    }
    const char *ret = ffi_c_type(
        lc, sig->return_type ? sig->return_type : lc->sema->vm->type_void, ok);
    if (!*ok) return d->tag;
    const char **pt = NULL;
    if (sig->param_count) {
        pt = (const char **)arena_calloc(lc->arena, sig->param_count,
                                         sizeof(*pt), ALIGNOF(max_align_t));
        if (!pt) {
            *ok = false;
            return d->tag;
        }
        for (size_t i = 0; i < sig->param_count; i++) {
            pt[i] = ffi_c_type(lc, sig->params[i], ok);
            if (!*ok) return d->tag;
        }
    }

    string_t *sb = string_new(lc->sema->vm->alloc);
    if (!sb) {
        *ok = false;
        return d->tag;
    }
    string_append_cstr(sb, "typedef ");
    string_append_cstr(sb, ret);
    string_append_cstr(sb, " (*");
    string_append_cstr(sb, d->tag);
    string_append_cstr(sb, ")(");
    if (sig->param_count == 0) {
        if (sig->is_variadic) {
            /* C 不允许 `f(...)`：至少一个具名参数才能在它后面跟 ... */
            lc->ffi_error =
                "a `cfunc(...)` type whose only parameter is `...` has no C "
                "representation (C requires one named parameter before `...`)";
            *ok = false;
            string_free(&sb);
            return d->tag;
        }
        string_append_cstr(sb, "void");
    } else {
        for (size_t i = 0; i < sig->param_count; i++) {
            if (i) string_append_cstr(sb, ", ");
            string_append_cstr(sb, pt[i]);
        }
        if (sig->is_variadic) string_append_cstr(sb, ", ...");
    }
    string_append_cstr(sb, ");");
    const char *text = copy_arena(lc->arena, string_cstr(sb));
    string_free(&sb);
    if (!text) {
        *ok = false;
        return d->tag;
    }
    cir_unit_add_extern_typedef(lc->unit, text);
    return d->tag;
}

/* map_type 侧入口：`cfunc(...)` 的 C 函数指针 typedef 名。 */
const char *ffi_c_func_pointer_type(lower_ctx_t *lc, const type_t *t,
                                    bool *ok) {
    return ffi_cfunc_typedef(lc, t, ok);
}

static const char *ffi_c_type(lower_ctx_t *lc, const type_t *t_raw,
                              bool *ok);

static const char *ffi_c_type_core(lower_ctx_t *lc, const type_t *t_raw,
                                   bool *ok) {
    const type_t *t = type_strip_quals(t_raw); /* 剥 const/volatile/所有权 */
    vm_t *vm = lc->sema->vm;
    if (!t || t == vm->type_void) return "void";
    if (t == vm->type_i8) return "int8_t";
    if (t == vm->type_i16) return "int16_t";
    if (t == vm->type_i32) return "int32_t";
    if (t == vm->type_i64) return "int64_t";
    if (t == vm->type_u8) return "uint8_t";
    if (t == vm->type_u16) return "uint16_t";
    if (t == vm->type_u32) return "uint32_t";
    if (t == vm->type_u64) return "uint64_t";
    if (t == vm->type_usize) return "size_t";
    if (t == vm->type_bool) return "bool";
    if (t == vm->type_f32) return "float";
    if (t == vm->type_f64) return "double";
    if (t == vm->type_str) return "const char *";
    if (t == vm->type_opaque) return "void *";
    switch (t->kind) {
    case TYPE_KIND_PTR: {
        const type_t *base = ptr_type_base(t);
        const char *bc = ffi_c_type(lc, base, ok);
        if (!*ok || !bc) return NULL;
        return cat_str(lc, bc,
                   (strcmp(bc, "void *") == 0 ||
                    strcmp(bc, "const void *") == 0)
                       ? "" : " *");
    }
    case TYPE_KIND_STRUCT:
    case TYPE_KIND_CUNION: {
        if (aggregate_has_str(t)) {
            lc->ffi_error =
                "aggregate type contains `str`: this backend's str layout "
                "differs from the C ABI (char*); pass a pointer instead";
            *ok = false;
            return NULL;
        }
        cir_type_t *ct = map_type(lc, t);
        if (!ct || !ct->c_name) {
            *ok = false;
            return NULL;
        }
        return ct->c_name;
    }
    case TYPE_KIND_ENUM: {
        const type_t *u = enum_type_underlying(t);
        return ffi_c_type(lc, u ? u : vm->type_i32, ok);
    }
    case TYPE_KIND_CFUNC:
        return ffi_cfunc_typedef(lc, t, ok);
    default:
        lc->ffi_error = "type has no C ABI representation";
        *ok = false;
        return NULL;
    }
}

/* const 保留：标准库原型如 `int (*)(const void *, const void *)`
   需要 clux 侧 `ref const opaque` 才能与之一致。 */
static const char *ffi_c_type(lower_ctx_t *lc, const type_t *t_raw,
                              bool *ok) {
    bool cq = t_raw && type_is_const(t_raw);
    const char *r = ffi_c_type_core(lc, t_raw, ok);
    if (!cq || !r || strcmp(r, "void") == 0) return r;
    if (strncmp(r, "const ", 6) == 0) return r;
    return cat_str(lc, "const ", r);
}

/* 为 port 导入的 C 符号在**当前编译单元**补一个 extern 原型。
 * 直接 `lib::fn` 的声明由 lower_extern_decls 生成；port 的声明在另一个
 * 编译单元，导入方必须自己声明，否则 host cc 会报隐式声明/链接自检失败。 */
static void ensure_ffi_fn_proto(lower_ctx_t *lc, const char *cname,
                                const func_sig_t *sig, uint8_t callconv) {
    if (!lc || !cname || !sig) return;
    if (cir_unit_has_extern_symbol(lc->unit, cname)) return;
    vm_t *vm = lc->sema->vm;
    bool ok = true;
    lc->ffi_error = NULL;
    const type_t *rt = sig->return_type ? sig->return_type : vm->type_void;
    const char *cret = ffi_c_type(lc, rt, &ok);
    if (rt && type_strip_quals(rt) == vm->type_str) cret = "char *";
    if (!ok || !cret) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, NULL),
                   "c backend: cannot declare C symbol '%s': %s", cname,
                   lc->ffi_error ? lc->ffi_error : "unsupported return type");
        lc->failed = true;
        return;
    }
    const char **pt = NULL;
    if (sig->param_count) {
        pt = (const char **)arena_calloc(lc->arena, sig->param_count,
                                         sizeof(*pt), ALIGNOF(max_align_t));
        if (!pt) {
            lc->failed = true;
            return;
        }
        for (size_t i = 0; i < sig->param_count; i++) {
            lc->ffi_error = NULL;
            pt[i] = ffi_c_type(lc, sig->params[i], &ok);
            if (!ok || !pt[i]) {
                diag_error(lc->sema->diag, sema_loc(lc->sema, NULL),
                           "c backend: cannot declare C symbol '%s' parameter "
                           "%zu: %s", cname, i + 1,
                           lc->ffi_error ? lc->ffi_error : "unsupported type");
                lc->failed = true;
                return;
            }
        }
    }
    string_t *sb = string_new(vm->alloc);
    if (!sb) {
        lc->failed = true;
        return;
    }
    string_append_cstr(sb, "extern ");
    string_append_cstr(sb, cret);
    string_append_cstr(sb, " ");
    if (callconv == CLUX_CC_STDCALL)
        string_append_cstr(sb, "CLUX_STDCALL ");
    string_append_cstr(sb, cname);
    string_append_cstr(sb, "(");
    if (sig->param_count == 0) {
        string_append_cstr(sb, "void");
    } else {
        for (size_t i = 0; i < sig->param_count; i++) {
            if (i) string_append_cstr(sb, ", ");
            string_append_cstr(sb, pt[i]);
        }
        if (sig->is_variadic) string_append_cstr(sb, ", ...");
    }
    string_append_cstr(sb, ");");
    const char *text = copy_arena(lc->arena, string_cstr(sb));
    string_free(&sb);
    if (!text) {
        lc->failed = true;
        return;
    }
    cir_unit_add_extern_decl(lc->unit, text);
    cir_unit_add_extern_symbol(lc->unit, cname);
}

static void ensure_ffi_var_proto(lower_ctx_t *lc, const char *cname,
                                 const type_t *vt) {
    if (!lc || !cname || !vt) return;
    if (cir_unit_has_extern_symbol(lc->unit, cname)) return;
    bool ok = true;
    lc->ffi_error = NULL;
    const char *ctype = ffi_c_type(lc, vt, &ok);
    if (!ok || !ctype) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, NULL),
                   "c backend: cannot declare C data symbol '%s': %s", cname,
                   lc->ffi_error ? lc->ffi_error : "unsupported type");
        lc->failed = true;
        return;
    }
    string_t *sb = string_new(lc->sema->vm->alloc);
    if (!sb) {
        lc->failed = true;
        return;
    }
    /* port 模型推荐 `extern lib ... from "*"` + native 链接：数据符号是
       普通 extern；DLL 数据导入（dllimport）仍由模块内直接 `extern var`
       路径处理。 */
    string_append_cstr(sb, "extern ");
    string_append_cstr(sb, ctype);
    string_append_cstr(sb, " ");
    string_append_cstr(sb, cname);
    string_append_cstr(sb, ";");
    const char *text = copy_arena(lc->arena, string_cstr(sb));
    string_free(&sb);
    if (!text) {
        lc->failed = true;
        return;
    }
    cir_unit_add_extern_decl(lc->unit, text);
    cir_unit_add_extern_symbol(lc->unit, cname);
}

/* ===================== extern lib / extern cfunc 收集 ===================== */

static bool has_lib_suffix(const char *s) {
    static const char *const exts[] = {".so", ".dylib", ".dll", ".a", ".lib",
                                       ".tbd", NULL};
    size_t n = strlen(s);
    for (size_t i = 0; exts[i]; i++) {
        size_t e = strlen(exts[i]);
        if (n >= e && strcmp(s + n - e, exts[i]) == 0) return true;
    }
    return false;
}

static void collect_extern_lib(lower_ctx_t *lc, const ast_extern_lib_t *el) {
    /* 记录别名 → 源码库名：extern var 的 Windows dllimport 判定要知道
       `from "*"`（进程内/静态链接）还是显式 DLL。 */
    lower_extern_lib_t *rec = (lower_extern_lib_t *)arena_calloc(
        lc->arena, 1, sizeof(*rec), ALIGNOF(max_align_t));
    if (rec) {
        rec->alias = el->alias;
        rec->source = el->name;
        rec->next = lc->extern_libs;
        lc->extern_libs = rec;
    }
    const char *name = slice_str(lc, el->name);
    if (!name || name[0] == '\0') return;
    if (strcmp(name, "*") == 0) return;
    const char *arg = NULL;
    bool path_like = strchr(name, '/') || strchr(name, '\\') ||
                     has_lib_suffix(name);
    if (path_like) {
        arg = name;
    } else {
        arg = cat_str(lc, "-l", name);
    }
    if (!arg) return;
    for (cir_link_arg_t *a = lc->unit->link_args; a; a = a->next)
        if (a->arg && strcmp(a->arg, arg) == 0) return;
    cir_unit_add_link_arg(lc->unit, arg);
}

static lower_extern_fn_t *find_extern(lower_ctx_t *lc, strslice_t lib,
                                      strslice_t name) {
    for (lower_extern_fn_t *e = lc->externs; e; e = e->next) {
        if (e->lib.len == lib.len && e->name.len == name.len &&
            memcmp(e->lib.ptr, lib.ptr, lib.len) == 0 &&
            memcmp(e->name.ptr, name.ptr, name.len) == 0)
            return e;
    }
    return NULL;
}

/* 解析 extern cfunc 的参数/返回类型（sema 已折叠为类型槽位，重复解析幂等）。 */
static const type_t *extern_param_type(lower_ctx_t *lc, ast_func_def_t *def,
                                       size_t i) {
    size_t k = 0;
    for (ast_node_t *p = def->params; p; p = p->next, k++) {
        if (k != i) continue;
        ast_var_def_t *vd = (ast_var_def_t *)p;
        const type_t *t = vd->resolved_type;
        if (!t && vd->type_expr)
            t = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        return t;
    }
    return NULL;
}

static const type_t *extern_return_type(lower_ctx_t *lc,
                                        ast_func_def_t *def) {
    if (!def->return_expr) return lc->sema->vm->type_void;
    return sema_resolve_type_slot(lc->sema, &def->return_expr);
}

/* C ABI 调用约定前缀：只有 `[[stdcall]]` 需要显式标注（默认 cdecl）。 */
static const char *callconv_c_prefix(const ast_func_def_t *fn) {
    return (fn && fn->callconv == CLUX_CC_STDCALL) ? "CLUX_STDCALL " : "";
}

static void emit_extern_prototype(lower_ctx_t *lc, ast_func_def_t *fn) {
    bool ok = true;
    size_t nparams = 0;
    for (ast_node_t *p = fn->params; p; p = p->next) nparams++;
    if (fn->is_variadic && nparams == 0) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)fn),
                   "c backend: extern cfunc '%.*s' with only `...` parameters "
                   "has no C equivalent (C requires one named parameter "
                   "before `...`)",
                   (int)fn->name.len, fn->name.ptr);
        lc->failed = true;
        return;
    }
    const type_t *rt = extern_return_type(lc, fn);
    lc->ffi_error = NULL;
    const char *cret = ffi_c_type(lc, rt, &ok);
    /* 返回值位置的 `str` 对应 C 的 `char *`（参数位置仍是
       `const char *`）：与 libc 的 getenv/strdup 等原型一致。 */
    if (rt && type_strip_quals(rt) == lc->sema->vm->type_str)
        cret = "char *";
    if (!ok || !cret) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)fn),
                   "c backend: extern cfunc '%.*s' return type cannot cross "
                   "the C ABI: %s",
                   (int)fn->name.len, fn->name.ptr,
                   lc->ffi_error ? lc->ffi_error : "unsupported type");
        lc->failed = true;
        return;
    }
    const char **pt = NULL;
    if (nparams) {
        pt = (const char **)arena_calloc(lc->arena, nparams, sizeof(*pt),
                                         ALIGNOF(max_align_t));
        if (!pt) {
            lc->failed = true;
            return;
        }
        for (size_t i = 0; i < nparams; i++) {
            const type_t *p = extern_param_type(lc, fn, i);
            if (!p || type_strip_quals(p) == lc->sema->vm->type_void) {
                diag_error(lc->sema->diag,
                           sema_loc(lc->sema, (ast_node_t *)fn),
                           "c backend: extern cfunc '%.*s' parameter %zu has no "
                           "C ABI type",
                           (int)fn->name.len, fn->name.ptr, i + 1);
                lc->failed = true;
                return;
            }
            lc->ffi_error = NULL;
            pt[i] = ffi_c_type(lc, p, &ok);
            if (!ok || !pt[i]) {
                diag_error(lc->sema->diag,
                           sema_loc(lc->sema, (ast_node_t *)fn),
                           "c backend: extern cfunc '%.*s' parameter %zu type "
                           "cannot cross the C ABI: %s",
                           (int)fn->name.len, fn->name.ptr, i + 1,
                           lc->ffi_error ? lc->ffi_error : "unsupported type");
                lc->failed = true;
                return;
            }
        }
    }

    const char *cname = slice_str(lc, fn->name);
    if (!cname) {
        lc->failed = true;
        return;
    }
    string_t *sb = string_new(lc->sema->vm->alloc);
    if (!sb) {
        lc->failed = true;
        return;
    }
    string_append_cstr(sb, "extern ");
    string_append_cstr(sb, cret);
    string_append_cstr(sb, " ");
    string_append_cstr(sb, callconv_c_prefix(fn));
    string_append_cstr(sb, cname);
    string_append_cstr(sb, "(");
    if (nparams == 0) string_append_cstr(sb, "void");
    else {
        for (size_t i = 0; i < nparams; i++) {
            if (i) string_append_cstr(sb, ", ");
            string_append_cstr(sb, pt[i]);
        }
        if (fn->is_variadic) string_append_cstr(sb, ", ...");
    }
    string_append_cstr(sb, ");");
    const char *text = copy_arena(lc->arena, string_cstr(sb));
    string_free(&sb);
    if (!text) {
        lc->failed = true;
        return;
    }

    /* 同名 C 符号必须原型一致（不同库别名同函数名的冲突在这里暴露）。 */
    for (lower_extern_fn_t *e = lc->externs; e; e = e->next) {
        if (e->name.len != fn->name.len ||
            memcmp(e->name.ptr, fn->name.ptr, fn->name.len) != 0)
            continue;
        if (e->proto && strcmp(e->proto, text) != 0) {
            diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)fn),
                       "c backend: conflicting extern cfunc declarations for C "
                       "symbol '%.*s' (same name, different signatures)",
                       (int)fn->name.len, fn->name.ptr);
            lc->failed = true;
            return;
        }
    }
    cir_unit_add_extern_decl(lc->unit, text);
    if (!cir_unit_has_extern_symbol(lc->unit, cname))
        cir_unit_add_extern_symbol(lc->unit, cname);
}

/* `extern lib <alias> from "*"`：符号由当前进程/最终链接提供（静态库或
 * 已在进程里的符号），数据符号是普通 extern；否则按 DLL 数据导入处理。 */
static bool extern_lib_is_process(lower_ctx_t *lc, strslice_t alias) {
    for (lower_extern_lib_t *l = lc->extern_libs; l; l = l->next) {
        if (strslice_eq(l->alias, alias))
            return strslice_eq(l->source, STRSLICE_LIT("*"));
    }
    return false;
}

/* extern var：库导出的**数据符号**（C 侧就是一个 `extern` 全局变量）。
 * 支持的类型与 VM 的 ffi_data_kind_of/typed 数据符号一致：整数/浮点/bool/
 * str/opaque/enum/struct/cunion；数组/切片/option 等由 sema 拒绝。
 * `str` 比较特殊：C 符号是 `char*`，读要包成 clux_str、写要取 `.data`。 */
static void collect_extern_var(lower_ctx_t *lc, ast_var_def_t *evd) {
    const type_t *vt = evd->resolved_type;
    if (!vt && evd->type_expr)
        vt = sema_resolve_type_slot(lc->sema, &evd->type_expr);
    if (!vt) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)evd),
                   "c backend: extern var '%.*s': type could not be resolved",
                   (int)evd->name.len, evd->name.ptr);
        lc->failed = true;
        return;
    }
    const type_t *core = type_strip_quals(vt);
    bool is_str = core == lc->sema->vm->type_str;
    bool ok = true;
    lc->ffi_error = NULL;
    const char *ctype = ffi_c_type(lc, vt, &ok);
    if (!ok || !ctype) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)evd),
                   "c backend: extern var '%.*s': type cannot cross the C ABI: "
                   "%s",
                   (int)evd->name.len, evd->name.ptr,
                   lc->ffi_error ? lc->ffi_error : "unsupported type");
        lc->failed = true;
        return;
    }
    const char *cname = slice_str(lc, evd->name);
    if (!cname) {
        lc->failed = true;
        return;
    }
    string_t *sb = string_new(lc->sema->vm->alloc);
    if (!sb) {
        lc->failed = true;
        return;
    }
    string_append_cstr(sb, "extern ");
#ifdef _WIN32
    /* Windows 的**数据**导入必须 dllimport：否则链接器找 `td_i32` 而不是
       导入库里的 `__imp_td_i32`（函数有 thunk，数据没有）。
       `from "*"` = 符号由当前进程/最终链接提供（port + native 静态库），
       此时是普通 extern；DLL 数据符号仍用显式库名。 */
    if (!extern_lib_is_process(lc, evd->lib_name))
        string_append_cstr(sb, "__declspec(dllimport) ");
#endif
    string_append_cstr(sb, ctype);
    string_append_cstr(sb, " ");
    string_append_cstr(sb, cname);
    string_append_cstr(sb, ";");
    const char *text = copy_arena(lc->arena, string_cstr(sb));
    string_free(&sb);
    if (!text) {
        lc->failed = true;
        return;
    }
    cir_unit_add_extern_decl(lc->unit, text);
    /* 数据符号也登记为"由外部定义"：port 导入方的 ensure_ffi_var_proto
       据此避免重复声明（dllimport 与普通 extern 不能并存）。 */
    if (!cir_unit_has_extern_symbol(lc->unit, cname))
        cir_unit_add_extern_symbol(lc->unit, cname);

    lower_extern_var_t *ev = (lower_extern_var_t *)arena_calloc(
        lc->arena, 1, sizeof(*ev), ALIGNOF(max_align_t));
    if (!ev) {
        lc->failed = true;
        return;
    }
    ev->lib = evd->lib_name;
    ev->name = evd->name;
    ev->def = evd;
    ev->type = vt;
    ev->cname = cname;
    ev->is_str = is_str;
    ev->next = lc->extern_vars;
    lc->extern_vars = ev;
}

static lower_extern_var_t *find_extern_var(lower_ctx_t *lc, strslice_t lib,
                                           strslice_t name) {
    for (lower_extern_var_t *v = lc->extern_vars; v; v = v->next) {
        if (v->lib.len == lib.len && v->name.len == name.len &&
            memcmp(v->lib.ptr, lib.ptr, lib.len) == 0 &&
            memcmp(v->name.ptr, name.ptr, name.len) == 0)
            return v;
    }
    return NULL;
}

void lower_extern_decls(lower_ctx_t *lc, ast_program_t *prog) {
    if (!lc || !prog) return;
    /* 先收集所有 extern lib：extern var 的 dllimport 判定依赖别名→库名
       （`"*"` = 进程内/静态链接），不能依赖声明顺序。 */
    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind == AST_EXTERN_LIB)
            collect_extern_lib(lc, (const ast_extern_lib_t *)n);
    }
    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind == AST_EXTERN_LIB) continue;
        if (n->kind == AST_VAR_DEF && ((ast_var_def_t *)n)->is_extern_var) {
            collect_extern_var(lc, (ast_var_def_t *)n);
            if (lc->failed) return;
            continue;
        }
        if (n->kind != AST_FUNC_DEF) continue;
        ast_func_def_t *fn = (ast_func_def_t *)n;
        if (!fn->is_extern_cfunc) continue;
        /* 先登记（调用点查表用），再生成原型（原型文本也要记到登记项上）。 */
        lower_extern_fn_t *e = (lower_extern_fn_t *)arena_calloc(
            lc->arena, 1, sizeof(*e), ALIGNOF(max_align_t));
        if (!e) {
            lc->failed = true;
            return;
        }
        e->lib = fn->lib_name;
        e->name = fn->name;
        e->def = fn;
        e->proto = NULL;
        e->next = lc->externs;
        lc->externs = e;
        size_t before = 0;
        for (cir_raw_t *r = lc->unit->extern_decls; r; r = r->next) before++;
        emit_extern_prototype(lc, fn);
        if (lc->failed) return;
        size_t after = 0;
        for (cir_raw_t *r = lc->unit->extern_decls; r; r = r->next) after++;
        if (after > before && lc->unit->extern_decls_last)
            e->proto = lc->unit->extern_decls_last->code;
    }
}

const type_t *ffi_extern_var_type(lower_ctx_t *lc, ast_scope_ref_t *sr) {
    if (!sr) return NULL;
    if (sr->ffi_var_type) return (const type_t *)sr->ffi_var_type;
    if (!sr->type_expr || sr->type_expr->kind != AST_IDENT) return NULL;
    strslice_t lib = ((ast_ident_t *)sr->type_expr)->name;
    lower_extern_var_t *ev = find_extern_var(lc, lib, sr->variant);
    return ev ? ev->type : NULL;
}

/* sema 已解析的 extern var（直接 lib::var 或 port 导入的 port::var）：
 * 实际 C 符号名 + 变量类型。 */
static bool ffi_var_resolved(lower_ctx_t *lc, ast_scope_ref_t *sr,
                             const type_t **out_type, const char **out_cname,
                             bool *out_is_str) {
    if (!sr || !sr->ffi_c_name.len || !sr->ffi_var_type) return false;
    const char *cname = slice_str(lc, sr->ffi_c_name);
    if (!cname) return false;
    const type_t *t = (const type_t *)sr->ffi_var_type;
    if (out_type) *out_type = t;
    if (out_cname) *out_cname = cname;
    if (out_is_str)
        *out_is_str = type_strip_quals(t) == lc->sema->vm->type_str;
    return true;
}

cir_expr_t *lower_ffi_var_read(lower_ctx_t *lc, ast_scope_ref_t *sr) {
    const type_t *rt = NULL;
    const char *rc = NULL;
    bool rstr = false;
    if (ffi_var_resolved(lc, sr, &rt, &rc, &rstr)) {
        ensure_ffi_var_proto(lc, rc, rt);
        if (lc->failed) return NULL;
        cir_type_t *ct = map_type(lc, rt);
        if (!ct) {
            lc->failed = true;
            return NULL;
        }
        cir_expr_t *id = cir_expr_ident(lc->arena, rc);
        if (!id) return NULL;
        if (!rstr) {
            id->type = ct;
            return id;
        }
        lc->unit->uses_str_from_c = true;
        cir_expr_t *wrap = cir_expr_call(lc->arena, "clux_str_from_c", NULL);
        if (!wrap) return NULL;
        wrap->type = ct;
        cir_expr_append_arg(wrap, id);
        return wrap;
    }
    if (!sr->type_expr || sr->type_expr->kind != AST_IDENT) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)sr),
                   "c backend: extern var: invalid library alias");
        lc->failed = true;
        return NULL;
    }
    strslice_t lib = ((ast_ident_t *)sr->type_expr)->name;
    lower_extern_var_t *ev = find_extern_var(lc, lib, sr->variant);
    if (!ev) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)sr),
                   "c backend: internal: extern var '%.*s::%.*s' not registered",
                   (int)lib.len, lib.ptr, (int)sr->variant.len,
                   sr->variant.ptr);
        lc->failed = true;
        return NULL;
    }
    cir_type_t *ct = map_type(lc, ev->type);
    if (!ct) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)sr),
                   "c backend: extern var '%.*s': unsupported type",
                   (int)sr->variant.len, sr->variant.ptr);
        lc->failed = true;
        return NULL;
    }
    cir_expr_t *id = cir_expr_ident(lc->arena, ev->cname);
    if (!id) return NULL;
    if (!ev->is_str) {
        id->type = ct;
        return id;
    }
    /* C 符号是 char* → 读成 clux_str（len = strlen）。 */
    lc->unit->uses_str_from_c = true;
    cir_expr_t *wrap = cir_expr_call(lc->arena, "clux_str_from_c", NULL);
    if (!wrap) return NULL;
    wrap->type = ct;
    cir_expr_append_arg(wrap, id);
    return wrap;
}

const char *lower_ffi_var_target(lower_ctx_t *lc, ast_scope_ref_t *sr,
                                 bool *is_str) {
    if (is_str) *is_str = false;
    const type_t *rt = NULL;
    const char *rc = NULL;
    bool rstr = false;
    if (ffi_var_resolved(lc, sr, &rt, &rc, &rstr)) {
        ensure_ffi_var_proto(lc, rc, rt);
        if (lc->failed) return NULL;
        if (is_str) *is_str = rstr;
        return rc;
    }
    if (!sr->type_expr || sr->type_expr->kind != AST_IDENT) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)sr),
                   "c backend: extern var: invalid library alias");
        lc->failed = true;
        return NULL;
    }
    strslice_t lib = ((ast_ident_t *)sr->type_expr)->name;
    lower_extern_var_t *ev = find_extern_var(lc, lib, sr->variant);
    if (!ev) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)sr),
                   "c backend: internal: extern var '%.*s::%.*s' not registered",
                   (int)lib.len, lib.ptr, (int)sr->variant.len,
                   sr->variant.ptr);
        lc->failed = true;
        return NULL;
    }
    if (is_str) *is_str = ev->is_str;
    return ev->cname;
}

/* ============================ 调用点 lowering ============================ */

/* FFI 实参适配：`str` → `(v).data`（C ABI 的 char*）。其余类型 C 表示一致。 */
static cir_expr_t *adapt_ffi_arg(lower_ctx_t *lc, cir_expr_t *v,
                                 const type_t *want) {
    if (!v || !want) return v;
    const type_t *t = type_strip_quals(want);
    if (t && t == lc->sema->vm->type_str)
        return cir_expr_member(lc->arena, v, "data");
    return v;
}

/* 已解析的 C ABI 调用：sema 把 `func_sig_t` + 实际 C 符号名填进 ast_call_t。
 * 直接 `lib::fn` 与 port 导入的 `port::fn` 都走这里，不需要在 codegen 里
 * 重新查本单元的 extern 表（port 的声明在另一个编译单元）。 */
static cir_expr_t *lower_ffi_call_resolved(lower_ctx_t *lc, ast_call_t *call,
                                           const func_sig_t *sig,
                                           strslice_t cname_slice) {
    const char *cname = slice_str(lc, cname_slice);
    if (!cname) {
        lc->failed = true;
        return NULL;
    }
    ensure_ffi_fn_proto(lc, cname, sig, call->ffi_callconv);
    if (lc->failed) return NULL;
    cir_expr_t *out = cir_expr_call(lc->arena, cname, NULL);
    if (!out) {
        lc->failed = true;
        return NULL;
    }
    size_t nfixed = sig ? sig->param_count : 0;
    size_t i = 0;
    for (ast_node_t *a = call->args; a; a = a->next, i++) {
        cir_expr_t *v = lower_expr(lc, a);
        if (!v) return NULL;
        const type_t *want = (sig && i < nfixed) ? sig->params[i]
                                                 : infer_expr_type(lc, a);
        v = adapt_ffi_arg(lc, v, want);
        if (!v) return NULL;
        cir_expr_append_arg(out, v);
    }

    const type_t *rt = sig ? sig->return_type : lc->sema->vm->type_void;
    const type_t *rtc = type_strip_quals(rt);
    cir_type_t *ct = map_type(lc, rt);
    if (!ct) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: extern cfunc return type is not supported");
        lc->failed = true;
        return NULL;
    }
    out->type = ct;
    if (rtc && rtc == lc->sema->vm->type_str) {
        lc->unit->uses_str_from_c = true;
        cir_expr_t *wrap = cir_expr_call(lc->arena, "clux_str_from_c", NULL);
        if (!wrap) {
            lc->failed = true;
            return NULL;
        }
        wrap->type = ct;
        cir_expr_append_arg(wrap, out);
        return wrap;
    }
    return out;
}

cir_expr_t *lower_ffi_call(lower_ctx_t *lc, ast_call_t *call) {
    /* sema 已解析签名 + 实际 C 符号：直接走统一路径（port 导入必须有它）。 */
    const type_t *fsig = (const type_t *)call->ffi_sig;
    if (call->ffi_c_name.len && fsig &&
        type_kind_of(type_strip_ownership(fsig)) == TYPE_KIND_FUNC) {
        const func_sig_t *sig =
            &((const func_type_t *)type_strip_ownership(fsig))->sig;
        return lower_ffi_call_resolved(lc, call, sig, call->ffi_c_name);
    }
    if (!call->callee || call->callee->kind != AST_SCOPE_REF) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: extern call: callee is not `<lib>::<name>`");
        lc->failed = true;
        return NULL;
    }
    ast_scope_ref_t *sr = (ast_scope_ref_t *)call->callee;
    if (!sr->type_expr || sr->type_expr->kind != AST_IDENT) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: extern call: invalid library alias");
        lc->failed = true;
        return NULL;
    }
    strslice_t lib = ((ast_ident_t *)sr->type_expr)->name;
    lower_extern_fn_t *e = find_extern(lc, lib, sr->variant);
    if (!e) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: internal: extern cfunc '%.*s::%.*s' not "
                   "registered",
                   (int)lib.len, lib.ptr, (int)sr->variant.len,
                   sr->variant.ptr);
        lc->failed = true;
        return NULL;
    }
    const char *cname = slice_str(lc, e->name);
    if (!cname) {
        lc->failed = true;
        return NULL;
    }
    cir_expr_t *out = cir_expr_call(lc->arena, cname, NULL);
    if (!out) {
        lc->failed = true;
        return NULL;
    }
    size_t nfixed = 0;
    for (ast_node_t *p = e->def->params; p; p = p->next) nfixed++;
    size_t i = 0;
    for (ast_node_t *a = call->args; a; a = a->next, i++) {
        cir_expr_t *v = lower_expr(lc, a);
        if (!v) return NULL;
        const type_t *want = i < nfixed ? extern_param_type(lc, e->def, i)
                                        : infer_expr_type(lc, a);
        v = adapt_ffi_arg(lc, v, want);
        if (!v) return NULL;
        cir_expr_append_arg(out, v);
    }

    const type_t *rt = extern_return_type(lc, e->def);
    const type_t *rtc = type_strip_quals(rt);
    cir_type_t *ct = map_type(lc, rt);
    if (!ct) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: extern cfunc return type is not supported");
        lc->failed = true;
        return NULL;
    }
    out->type = ct;
    if (rtc && rtc == lc->sema->vm->type_str) {
        /* C 返回 `const char *`：包成 clux str（len = strlen）。 */
        lc->unit->uses_str_from_c = true;
        cir_expr_t *wrap = cir_expr_call(lc->arena, "clux_str_from_c", NULL);
        if (!wrap) {
            lc->failed = true;
            return NULL;
        }
        wrap->type = ct;
        cir_expr_append_arg(wrap, out);
        return wrap;
    }
    return out;
}

cir_expr_t *lower_data_call(lower_ctx_t *lc, ast_call_t *call) {
    if (!call->args || call->args->next) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: data() expects exactly one argument");
        lc->failed = true;
        return NULL;
    }
    cir_expr_t *obj = lower_expr(lc, call->args);
    if (!obj) return NULL;
    return cir_expr_slice_data(lc->arena, obj);
}

/* ========================== cfunc_ptr / 回调 ========================== */

bool call_is_cfunc_ptr(const ast_call_t *c) {
    if (!c || !c->callee) return false;
    if (c->callee->kind == AST_IDENT)
        return strslice_eq(((const ast_ident_t *)c->callee)->name,
                           STRSLICE_LIT("cfunc_ptr"));
    if (c->callee->kind == AST_FUNC_REF)
        return strslice_eq(((const ast_func_ref_t *)c->callee)->name,
                           STRSLICE_LIT("cfunc_ptr"));
    return false;
}

typedef struct lower_cb_wrapper lower_cb_wrapper_t;

static lower_cb_wrapper_t *cb_wrapper_find(lower_ctx_t *lc,
                                           const ast_func_def_t *def) {
    for (lower_cb_wrapper_t *w = lc->cb_wrappers; w; w = w->next)
        if (w->def == def) return w;
    return NULL;
}

/* 为 `cfunc` 定义生成 C ABI 包装（可选 static）：
 *     [static] <ret> <wname>(<C 参数...>) { [return] clux_<tag>_fn_<f>(NULL, ...); }
 * `cfunc` 没有 env；str 参数/返回值在边界上转换（C char* ⇄ clux_str）。 */
static const char *build_cfunc_wrapper(lower_ctx_t *lc, ast_func_def_t *fn,
                                       const char *wname, bool is_static) {
    size_t nparams = 0;
    for (ast_node_t *p = fn->params; p; p = p->next) nparams++;
    const type_t **pt = NULL;
    if (nparams) {
        pt = (const type_t **)arena_calloc(lc->arena, nparams, sizeof(*pt),
                                           ALIGNOF(max_align_t));
        if (!pt) {
            lc->failed = true;
            return NULL;
        }
        size_t i = 0;
        for (ast_node_t *p = fn->params; p; p = p->next, i++) {
            ast_var_def_t *vd = (ast_var_def_t *)p;
            const type_t *t = vd->resolved_type;
            if (!t && vd->type_expr)
                t = sema_resolve_type_slot(lc->sema, &vd->type_expr);
            if (!t) {
                lc->failed = true;
                return NULL;
            }
            pt[i] = t;
        }
    }
    const type_t *rt = fn->return_expr
                           ? sema_resolve_type_slot(lc->sema, &fn->return_expr)
                           : lc->sema->vm->type_void;

    bool ok = true;
    lc->ffi_error = NULL;
    const char *cret = ffi_c_type(lc, rt, &ok);
    const char **ctypes = NULL;
    if (nparams) {
        ctypes = (const char **)arena_calloc(lc->arena, nparams, sizeof(*ctypes),
                                             ALIGNOF(max_align_t));
        if (!ctypes) {
            lc->failed = true;
            return NULL;
        }
        for (size_t i = 0; i < nparams; i++) {
            lc->ffi_error = NULL;
            ctypes[i] = ffi_c_type(lc, pt[i], &ok);
            if (!ok) {
                diag_error(lc->sema->diag,
                           sema_loc(lc->sema, (ast_node_t *)fn),
                           "c backend: cfunc '%.*s' parameter %zu type cannot "
                           "cross the C ABI: %s",
                           (int)fn->name.len, fn->name.ptr, i + 1,
                           lc->ffi_error ? lc->ffi_error : "unsupported type");
                lc->failed = true;
                return NULL;
            }
        }
    }
    if (!ok || !cret) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)fn),
                   "c backend: cfunc '%.*s' return type cannot cross the C "
                   "ABI: %s",
                   (int)fn->name.len, fn->name.ptr,
                   lc->ffi_error ? lc->ffi_error : "unsupported type");
        lc->failed = true;
        return NULL;
    }

    const char *target = module_symbol(lc->arena, lc->unit, "fn_", fn->name);
    if (!target || !wname) {
        lc->failed = true;
        return NULL;
    }

    string_t *sb = string_new(lc->sema->vm->alloc);
    if (!sb) {
        lc->failed = true;
        return NULL;
    }
    if (is_static) string_append_cstr(sb, "static ");
    string_append_cstr(sb, cret);
    string_append_cstr(sb, " ");
    string_append_cstr(sb, callconv_c_prefix(fn));
    string_append_cstr(sb, wname);
    string_append_cstr(sb, "(");
    if (nparams == 0) string_append_cstr(sb, "void");
    else {
        for (size_t i = 0; i < nparams; i++) {
            if (i) string_append_cstr(sb, ", ");
            string_append_cstr(sb, ctypes[i]);
            char an[32];
            snprintf(an, sizeof an, " a%zu", i);
            string_append_cstr(sb, an);
        }
    }
    string_append_cstr(sb, ") {\n");
    bool ret_void = !rt || type_strip_quals(rt) == lc->sema->vm->type_void;
    bool ret_str = !ret_void && type_strip_quals(rt) == lc->sema->vm->type_str;
    if (!ret_void) string_append_cstr(sb, "    return ");
    else string_append_cstr(sb, "    ");
    if (ret_str) string_append_cstr(sb, "(");
    string_append_cstr(sb, target);
    string_append_cstr(sb, "(NULL");
    for (size_t i = 0; i < nparams; i++) {
        string_append_cstr(sb, ", ");
        bool is_str = type_strip_quals(pt[i]) == lc->sema->vm->type_str;
        if (is_str) {
            lc->unit->uses_str_from_c = true;
            string_append_cstr(sb, "clux_str_from_c(a");
            char an[32];
            snprintf(an, sizeof an, "%zu)", i);
            string_append_cstr(sb, an);
        } else {
            char an[32];
            snprintf(an, sizeof an, "a%zu", i);
            string_append_cstr(sb, an);
        }
    }
    string_append_cstr(sb, ")");
    if (ret_str) string_append_cstr(sb, ").data");
    string_append_cstr(sb, ";\n}\n");
    const char *text = copy_arena(lc->arena, string_cstr(sb));
    string_free(&sb);
    if (!text) {
        lc->failed = true;
        return NULL;
    }
    cir_unit_add_raw(lc->unit, text);
    return text;
}

/* C ABI 原型文本（头文件里的 `extern` 声明）：从签名重新渲染，
 * 与 build_cfunc_wrapper 的参数类型完全一致。 */
static void emit_cfunc_prototype_into(lower_ctx_t *lc, ast_func_def_t *fn,
                                      const char *wname) {
    size_t nparams = 0;
    for (ast_node_t *p = fn->params; p; p = p->next) nparams++;
    bool ok = true;
    lc->ffi_error = NULL;
    const type_t *rt = fn->return_expr
                           ? sema_resolve_type_slot(lc->sema, &fn->return_expr)
                           : lc->sema->vm->type_void;
    const char *cret = ffi_c_type(lc, rt, &ok);
    if (!ok || !cret) {
        lc->failed = true;
        return;
    }
    string_t *sb = string_new(lc->sema->vm->alloc);
    if (!sb) {
        lc->failed = true;
        return;
    }
    string_append_cstr(sb, "extern ");
    string_append_cstr(sb, cret);
    string_append_cstr(sb, " ");
    string_append_cstr(sb, callconv_c_prefix(fn));
    string_append_cstr(sb, wname);
    string_append_cstr(sb, "(");
    if (nparams == 0) string_append_cstr(sb, "void");
    size_t i = 0;
    for (ast_node_t *p = fn->params; p; p = p->next, i++) {
        ast_var_def_t *vd = (ast_var_def_t *)p;
        const type_t *t = vd->resolved_type;
        if (!t && vd->type_expr)
            t = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        lc->ffi_error = NULL;
        const char *ct = ffi_c_type(lc, t, &ok);
        if (!ok || !ct) {
            lc->failed = true;
            string_free(&sb);
            return;
        }
        if (i) string_append_cstr(sb, ", ");
        string_append_cstr(sb, ct);
    }
    string_append_cstr(sb, ");");
    const char *text = copy_arena(lc->arena, string_cstr(sb));
    string_free(&sb);
    if (!text) {
        lc->failed = true;
        return;
    }
    cir_unit_add_extern_decl_pub(lc->unit, text);
}

/* `export cfunc f(...)`：内部仍是 env-first 的 clux 函数；对外生成
 * `clux_<tag>_<name>` 的 C ABI 包装并进头文件（同一模块内调用仍走内部函数）。 */
void lower_exported_cfunc(lower_ctx_t *lc, ast_func_def_t *fn) {
    if (!lc || !fn || lc->failed) return;
    if (!fn->is_cfunc || fn->is_extern_cfunc) return;
    const char *wname = module_symbol(lc->arena, lc->unit, "", fn->name);
    if (!wname) {
        lc->failed = true;
        return;
    }
    if (!build_cfunc_wrapper(lc, fn, wname, /*is_static=*/false)) return;
    emit_cfunc_prototype_into(lc, fn, wname);
    /* 头文件需要签名类型定义：登记起来由 writer 递归展开。 */
    size_t i = 0;
    for (ast_node_t *p = fn->params; p; p = p->next, i++) {
        ast_var_def_t *vd = (ast_var_def_t *)p;
        const type_t *t = vd->resolved_type;
        if (!t && vd->type_expr)
            t = sema_resolve_type_slot(lc->sema, &vd->type_expr);
        if (t) {
            cir_type_t *ct = map_type(lc, t);
            if (ct) cir_unit_add_header_type(lc->unit, ct);
        }
    }
    if (fn->return_expr) {
        const type_t *t = sema_resolve_type_slot(lc->sema, &fn->return_expr);
        if (t) {
            cir_type_t *ct = map_type(lc, t);
            if (ct) cir_unit_add_header_type(lc->unit, ct);
        }
    }
}

/* 为 `cfunc` 定义生成 C trampoline（`cfunc_ptr` 用，static）。 */
static const char *emit_cb_wrapper(lower_ctx_t *lc, ast_func_def_t *fn) {
    lower_cb_wrapper_t *hit = cb_wrapper_find(lc, fn);
    if (hit) return hit->name;
    if (!fn->is_cfunc) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)fn),
                   "c backend: cfunc_ptr requires a `cfunc` definition: only "
                   "C-ABI functions can become C function pointers (plain "
                   "`func`/closures have no C representation)");
        lc->failed = true;
        return NULL;
    }
    if (fn->captures || fn->capture_type_id != 0) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)fn),
                   "c backend: cfunc_ptr: callback '%.*s' captures variables; "
                   "a C function pointer has no environment",
                   (int)fn->name.len, fn->name.ptr);
        lc->failed = true;
        return NULL;
    }
    char nbuf[512];
    snprintf(nbuf, sizeof nbuf, "%scb_%.*s",
             (lc->unit && lc->unit->sym_prefix) ? lc->unit->sym_prefix
                                                : "clux_",
             (int)fn->name.len, fn->name.ptr ? fn->name.ptr : "");
    const char *wname = copy_arena(lc->arena, nbuf);
    if (!wname) {
        lc->failed = true;
        return NULL;
    }
    if (!build_cfunc_wrapper(lc, fn, wname, /*is_static=*/true)) return NULL;

    lower_cb_wrapper_t *w = (lower_cb_wrapper_t *)arena_calloc(
        lc->arena, 1, sizeof(*w), ALIGNOF(max_align_t));
    if (!w) {
        lc->failed = true;
        return NULL;
    }
    w->def = fn;
    w->name = wname;
    w->next = lc->cb_wrappers;
    lc->cb_wrappers = w;
    return wname;
}

cir_expr_t *lower_cfunc_ptr(lower_ctx_t *lc, ast_call_t *call) {
    if (!call->args || call->args->next) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, (ast_node_t *)call),
                   "c backend: cfunc_ptr() expects exactly one argument");
        lc->failed = true;
        return NULL;
    }
    ast_node_t *a = call->args;
    ast_func_def_t *fn = NULL;
    if (a->kind == AST_FUNC_REF)
        fn = func_def_of_ref(lc, (ast_func_ref_t *)a);
    else if (a->kind == AST_IDENT) {
        /* sema 有时保留标识符形态：按名在全局符号表里找函数定义。 */
        sema_symbol_t *sym = sema_lookup(lc->sema->global_scope,
                                         ((ast_ident_t *)a)->name);
        if (sym && sym->ast && sym->ast->kind == AST_FUNC_DEF)
            fn = (ast_func_def_t *)sym->ast;
    }
    if (!fn) {
        diag_error(lc->sema->diag, sema_loc(lc->sema, a),
                   "c backend: cfunc_ptr requires a named function (function "
                   "literals have no C symbol)");
        lc->failed = true;
        return NULL;
    }
    const char *wname = emit_cb_wrapper(lc, fn);
    if (!wname) return NULL;
    return cir_expr_ident(lc->arena, wname);
}
