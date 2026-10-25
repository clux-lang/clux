#include "cbackend/c_backend.h"

#include "cbackend/c_lower.h"
#include "cbackend/c_lower_internal.h" /* module_tag_for_path */
#include "cbackend/c_opt.h"
#include "cbackend/c_writer.h"
#include "core/allocator.h"
#include "core/arena.h"
#include "core/stream.h"
#include "core/string.h"
#include "core/vec.h"
#include "desugar/desugar.h"
#include "diag/diagnostic.h"
#include "driver/compile_module.h"
#include "module/module_path.h"
#include "parser/ast_program.h"
#include "parser/ast_func_def.h"
#include "parser/ast_test.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "sema/sema.h"
#include "vm/module.h"
#include "vm/vm.h"

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

static int load_file(allocator_t *alloc, const char *path, char **out,
                     size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 1;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 1;
    }
    long n = ftell(fp);
    if (n < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 1;
    }
    char *buf = (char *)allocator_new_ex(alloc, "cbackend.source",
                                         (size_t)n + 1, NULL, NULL, NULL, 1);
    if (!buf) {
        fclose(fp);
        return 1;
    }
    size_t got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    if (got != (size_t)n) {
        allocator_free(alloc, (void **)&buf);
        return 1;
    }
    buf[n] = '\0';
    *out = buf;
    *out_len = (size_t)n;
    return 0;
}

static const char *host_cc(const cbackend_toolchain_t *tc) {
    if (tc && tc->compiler && *tc->compiler) return tc->compiler;
    const char *cc = getenv("CC");
    if (cc && *cc) return cc;
#ifdef _WIN32
    return "clang";
#else
    return "cc";
#endif
}

static void set_compiler_temp_dir(const char *out_path) {
    const char *slash = NULL;
    for (const char *p = out_path; p && *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    char dir[4096];
    if (slash) {
        size_t n = (size_t)(slash - out_path);
        if (n >= sizeof dir) return;
        memcpy(dir, out_path, n);
        dir[n] = '\0';
    } else {
        strcpy(dir, ".");
    }
#ifdef _WIN32
    _putenv_s("TMP", dir);
    _putenv_s("TEMP", dir);
#else
    setenv("TMP", dir, 1);
    setenv("TEMP", dir, 1);
#endif
}

/* 把 `s` 以双引号形式追加到命令行（`"` 与 `\` 转义由调用约定处理：
 * 这里只处理双引号；Windows/POSIX 的 shell 对路径反斜杠都按字面处理）。 */
static void cmd_append_quoted(string_t *sb, const char *s) {
    string_append_char(sb, '"');
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '"') string_append_char(sb, '\\');
        string_append_char(sb, *p);
    }
    string_append_char(sb, '"');
}

/* 取 path 的目录部分（不含末尾分隔符）；无分隔符返回 "." 。写入 buf。 */
static void path_dirname(const char *path, char *buf, size_t cap) {
    const char *slash = NULL;
    for (const char *p = path; p && *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    if (!slash) {
        snprintf(buf, cap, ".");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n == 0) n = 1; /* 根目录 "/" */
    if (n >= cap) n = cap - 1;
    memcpy(buf, path, n);
    buf[n] = '\0';
}

/* 在命令行末尾追加一个选项/路径：含空格时整体加引号，否则原样。 */
static void cmd_append_arg(string_t *sb, const char *s) {
    if (!s || !*s) return;
    string_append_char(sb, ' ');
    if (strchr(s, ' ') || strchr(s, '\t')) cmd_append_quoted(sb, s);
    else string_append_cstr(sb, s);
}

/* 编译一步：`<cc> -std=c11 -O0 [cflags] [-I..] -c -o <obj> <c>`。 */
static int compile_object(allocator_t *alloc, const char *c_path,
                          const char *obj_path,
                          const cbackend_toolchain_t *tc) {
    set_compiler_temp_dir(obj_path);
    const char *cc = host_cc(tc);
    string_t *cmd = string_new(alloc);
    if (!cmd) {
        fprintf(stderr, "build: out of memory\n");
        return 1;
    }
    if (strchr(cc, ' ') || strchr(cc, '\t')) cmd_append_quoted(cmd, cc);
    else string_append_cstr(cmd, cc);
    string_append_cstr(cmd, " -std=c11 -O0");
    if (tc) {
        for (size_t i = 0; i < tc->cflag_count; i++)
            cmd_append_arg(cmd, tc->cflags ? tc->cflags[i] : NULL);
        for (size_t i = 0; i < tc->include_count; i++) {
            const char *dir = tc->include_dirs ? tc->include_dirs[i] : NULL;
            if (!dir || !*dir) continue;
            string_append_cstr(cmd, " -I");
            cmd_append_quoted(cmd, dir);
        }
    }
    string_append_cstr(cmd, " -c -o ");
    cmd_append_quoted(cmd, obj_path);
    string_append_char(cmd, ' ');
    cmd_append_quoted(cmd, c_path);
    int rc = system(string_cstr(cmd));
    if (rc != 0) {
        fprintf(stderr, "build: command: %s\n", string_cstr(cmd));
        fprintf(stderr, "build: C compilation failed (compiler: %s)\n", cc);
        fprintf(stderr, "build: generated C kept at '%s'\n", c_path);
        string_free(&cmd);
        return 1;
    }
    string_free(&cmd);
    return 0;
}

/* 链接一步：`<cc> <objs...> -o <out> [-L..] [代码声明的链接参数] [-l..] [ldflags]`。
 *
 * `-L` 顺序：用户 `--lib-path` 优先，随后是产物目录及其上两级（便于把
 * `.lib/.so` 与产物放一起）；再放代码里 `extern lib` 产生的链接参数
 * （`-l<name>` 或库文件路径），最后是用户 `--link-lib`。 */
static int link_exe(allocator_t *alloc, const char *const *objs,
                    size_t obj_count, const char *out_path,
                    const cir_link_arg_t *code_links,
                    const cbackend_toolchain_t *tc) {
    set_compiler_temp_dir(out_path);
    const char *cc = host_cc(tc);
    string_t *cmd = string_new(alloc);
    if (!cmd) {
        fprintf(stderr, "build: out of memory\n");
        return 1;
    }
    if (strchr(cc, ' ') || strchr(cc, '\t')) cmd_append_quoted(cmd, cc);
    else string_append_cstr(cmd, cc);
    for (size_t i = 0; i < obj_count; i++) {
        string_append_char(cmd, ' ');
        cmd_append_quoted(cmd, objs[i]);
    }
    string_append_cstr(cmd, " -o ");
    cmd_append_quoted(cmd, out_path);

    if (tc) {
        for (size_t i = 0; i < tc->libdir_count; i++) {
            const char *dir = tc->lib_dirs ? tc->lib_dirs[i] : NULL;
            if (!dir || !*dir) continue;
            string_append_cstr(cmd, " -L");
            cmd_append_quoted(cmd, dir);
        }
    }
    char dir[4096], up1[4096], up2[4096];
    path_dirname(out_path, dir, sizeof dir);
    path_dirname(dir, up1, sizeof up1);
    path_dirname(up1, up2, sizeof up2);
    const char *dirs[3] = {dir, up1, up2};
    for (int i = 0; i < 3; i++) {
        if (!dirs[i] || !dirs[i][0]) continue;
        if (i > 0 && strcmp(dirs[i], dirs[i - 1]) == 0) continue;
        string_append_cstr(cmd, " -L");
        cmd_append_quoted(cmd, dirs[i]);
    }
    for (const cir_link_arg_t *l = code_links; l; l = l->next) {
        if (!l->arg || !l->arg[0]) continue;
        string_append_char(cmd, ' ');
#ifdef _WIN32
        /* Windows：`extern lib foo from "foo.dll"` 直接让链接器吃 .dll 会
           失败；改用同名导入库 `foo.lib`（clang/lld-link 只接受 .lib）。 */
        size_t alen = strlen(l->arg);
        if (alen > 4 && _stricmp(l->arg + alen - 4, ".dll") == 0) {
            bool has_dir = strchr(l->arg, '/') != NULL ||
                           strchr(l->arg, '\\') != NULL;
            if (!has_dir) {
                /* 裸文件名：`-l<name>`（去 .dll 后缀）才走 -L 搜索路径。 */
                char *name = (char *)malloc(alen - 3);
                if (name) {
                    memcpy(name, l->arg, alen - 4);
                    name[alen - 4] = '\0';
                    string_append_cstr(cmd, " -l");
                    cmd_append_quoted(cmd, name);
                    free(name);
                    continue;
                }
            }
            char *lib = (char *)malloc(alen + 1);
            if (lib) {
                memcpy(lib, l->arg, alen + 1);
                memcpy(lib + alen - 4, ".lib", 4);
                cmd_append_quoted(cmd, lib);
                free(lib);
                continue;
            }
        }
#endif
        cmd_append_quoted(cmd, l->arg);
    }
    if (tc) {
        for (size_t i = 0; i < tc->lib_count; i++) {
            const char *name = tc->libs ? tc->libs[i] : NULL;
            if (!name || !*name) continue;
            string_append_cstr(cmd, " -l");
            cmd_append_quoted(cmd, name);
        }
        for (size_t i = 0; i < tc->ldflag_count; i++)
            cmd_append_arg(cmd, tc->ldflags ? tc->ldflags[i] : NULL);
    }

    int rc = system(string_cstr(cmd));
    if (rc != 0) {
        fprintf(stderr, "build: command: %s\n", string_cstr(cmd));
        fprintf(stderr, "build: linking failed (compiler: %s)\n", cc);
        string_free(&cmd);
        return 1;
    }
    string_free(&cmd);
    return 0;
}

/* 编译中间产物 `<out>.clux.c`：编译失败时保留，便于用 #line 映射回 .cx。
 * 用 malloc 而非 allocator：该路径只在本文件内临时使用，不参与分配器记账。 */
static char *derive_c_path(const char *out_path) {
    size_t plen = strlen(out_path) + sizeof(".clux.c");
    char *c_path = (char *)malloc(plen);
    if (!c_path) return NULL;
    snprintf(c_path, plen, "%s.clux.c", out_path);
    return c_path;
}

/* 目标文件路径：`<out>.obj`（Windows）/ `<out>.o`（其它平台）。 */
static char *derive_obj_path(const char *out_path) {
#ifdef _WIN32
    const char *suffix = ".obj";
#else
    const char *suffix = ".o";
#endif
    size_t plen = strlen(out_path) + strlen(suffix) + 1;
    char *obj = (char *)malloc(plen);
    if (!obj) return NULL;
    snprintf(obj, plen, "%s%s", out_path, suffix);
    return obj;
}

/* 头文件路径：与 .c 配对（`.c` → `.h`；没有 `.c` 后缀时追加 `.h`）。 */
static char *derive_h_path(const char *c_path) {
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

/* -------------------------------------------------------------------------
 * 多模块（import）构建：镜像路径 + 目录创建 + 逐模块编译
 * ---------------------------------------------------------------------- */

static void be_norm(char *s) {
    for (; s && *s; s++)
        if (*s == '\\') *s = '/';
}

static bool be_abs(const char *p) {
    if (!p || !p[0]) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p[1] == ':' && (p[2] == '/' || p[2] == '\\');
}

/* 若 `src` 在 cwd 之下，写入相对路径。 */
static bool be_rel_cwd(const char *src, char *buf, size_t cap) {
    char cwd[4096];
#ifdef _WIN32
    if (!_getcwd(cwd, (int)sizeof cwd)) return false;
#else
    if (!getcwd(cwd, sizeof cwd)) return false;
#endif
    be_norm(cwd);
    char path[4096];
    snprintf(path, sizeof path, "%s", src ? src : "");
    be_norm(path);
    if (!be_abs(path)) {
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

static char *be_concat2(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    char *p = (char *)malloc(na + nb + 1);
    if (!p) return NULL;
    memcpy(p, a, na);
    memcpy(p + na, b, nb);
    p[na + nb] = '\0';
    return p;
}

/* `<build-dir>/<相对 cwd 路径（去扩展名）>`；源在构建目录内时去掉其前缀。 */
static char *be_mirror_base(const char *src, const char *build_dir) {
    char rel[4096];
    if (!be_rel_cwd(src, rel, sizeof rel)) {
        const char *b = src ? src : "";
        for (const char *q = src; q && *q; q++)
            if (*q == '/' || *q == '\\') b = q + 1;
        snprintf(rel, sizeof rel, "%s", b);
    }
    char bd[4096];
    snprintf(bd, sizeof bd, "%s",
             (build_dir && *build_dir) ? build_dir : "build");
    be_norm(bd);
    size_t bl = strlen(bd);
    while (bl > 1 && bd[bl - 1] == '/') bd[--bl] = '\0';
    char bdrel[4096];
    bdrel[0] = '\0';
    if (be_abs(bd)) {
        (void)be_rel_cwd(bd, bdrel, sizeof bdrel);
    } else {
        snprintf(bdrel, sizeof bdrel, "%s", bd);
        while (bdrel[0] == '.' && bdrel[1] == '/')
            memmove(bdrel, bdrel + 2, strlen(bdrel + 2) + 1);
    }
    size_t brl = strlen(bdrel);
    while (brl > 0 && bdrel[brl - 1] == '/') bdrel[--brl] = '\0';
    if (brl > 0 && strncmp(rel, bdrel, brl) == 0 && rel[brl] == '/')
        memmove(rel, rel + brl + 1, strlen(rel + brl + 1) + 1);
    char *dot = strrchr(rel, '.');
    if (dot && !strchr(dot, '/')) *dot = '\0';
    char *base = be_concat2(bd, "/");
    if (!base) return NULL;
    char *full = be_concat2(base, rel);
    free(base);
    return full;
}

static bool be_ensure_dir(const char *path) {
    if (!path || !*path) return true;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    be_norm(buf);
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

static bool be_ensure_parent(const char *file) {
    if (!file || !*file) return true;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", file);
    be_norm(buf);
    char *slash = strrchr(buf, '/');
    if (!slash) return true;
    *slash = '\0';
    return be_ensure_dir(buf);
}

/* 单个模块：lower（测试模式下用例表随后由调用方决定是否合并）。 */
static cir_unit_t *lower_one_module(arena_t *arena, module_t *mod,
                                    bool tests, bool allow_no_main) {
    cir_unit_t *u = cbackend_lower(arena, mod->sema,
                                   (ast_program_t *)mod->src_ast,
                                   mod->canonical, tests, allow_no_main);
    if (!u) return NULL;
    cbackend_optimize(u);
    return u;
}

/* 写 .c/.h →（非 emit-c）编译对象。 */
static int write_one_module(allocator_t *alloc, cir_unit_t *u,
                            const char *c_path, const char *obj_path,
                            bool emit_c_only, bool keep_c,
                            const cbackend_toolchain_t *tc) {
    if (!be_ensure_parent(c_path) ||
        (!emit_c_only && !be_ensure_parent(obj_path))) {
        fprintf(stderr, "build: cannot create output directory for '%s'\n",
                c_path);
        return 1;
    }
    char *h_path = derive_h_path(c_path);
    if (!h_path) return 1;
    int rc = 0;
    if (cir_write_pair(u, c_path, h_path, alloc) != 0) {
        fprintf(stderr, "build: cannot write C source '%s' / header '%s'\n",
                c_path, h_path);
        rc = 1;
    } else if (!emit_c_only) {
        if (compile_object(alloc, c_path, obj_path, tc) != 0) rc = 1;
        else if (!keep_c) remove(c_path);
    }
    free(h_path);
    return rc;
}

int cbackend_build_program(const char *src_path, const char *out_path,
                           const cbackend_program_t *pgm,
                           const cbackend_toolchain_t *tc) {
    if (!src_path) return 1;
    bool emit_c_only = pgm && pgm->emit_c_only;
    bool object_only = pgm && pgm->object_only;
    bool tests = pgm && pgm->tests;
    bool keep_c = pgm && pgm->keep_c;
    if (!emit_c_only && !object_only && !out_path) return 1;

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) return 1;
    arena_t *arena = arena_new_default(alloc);
    diag_buf_t *diag = diag_buf_new(alloc);
    vm_t *vm = vm_new(alloc);
    if (!arena || !diag || !vm) {
        if (vm) vm_destroy(&vm);
        if (diag) diag_buf_destroy(&diag);
        if (arena) arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }
    int rc = 1;
    char *entry = module_path_normalize(alloc, src_path);
    vec_t *path_stack = vec_new(alloc, /*owns_element=*/false);
    const char **objs = NULL;
    char **owned = NULL;
    size_t nobjs = 0, nowned = 0, cap = 0;
    const char *links[128];
    size_t nlinks = 0;
    module_t *main_mod = NULL;
    /* 两遍 lowering 的暂存（入口运行时的 any_alloc 需要先看所有模块）。 */
    cir_unit_t **units = NULL;
    module_t **mods = NULL;
    /* 测试用例合并表：必须在任何 `goto cleanup` 之前初始化（跨 goto 的
       局部变量不会执行初始化）。 */
    cir_test_case_t *all_cases = NULL;
    size_t ncases = 0, cases_cap = 0;
    if (!entry || !path_stack) {
        fprintf(stderr, "build: out of memory\n");
        goto cleanup;
    }
    /* 测试模式必须在编译**之前**置位：driver 在模块准备阶段把 test 块
       改写成 `__clux_test_N` 并记录 `mod->test_cases`。 */
    if (tests) vm->test_mode = true;
    main_mod = compile_module_prepare(vm, alloc, diag, arena, entry, path_stack);
    if (!main_mod) {
        /* 诊断统一在 cleanup 打印一次；这里只在没有诊断时补通用提示，
           避免同一批错误被打印两遍。 */
        if (!diag_has_error(diag))
            fprintf(stderr, "build: cannot compile '%s'\n", src_path);
        goto cleanup;
    }
    size_t nmods = vm->module_order ? vec_len(vm->module_order) : 0;
    cap = nmods + 1;
    objs = (const char **)calloc(cap, sizeof(*objs));
    owned = (char **)calloc(cap * 3, sizeof(*owned));
    if (!objs || !owned) {
        fprintf(stderr, "build: out of memory\n");
        goto cleanup;
    }
    const char *build_dir = (pgm && pgm->build_dir) ? pgm->build_dir : "build";
    /* 测试模式：driver 把 test 块改写成各模块的 `__clux_test_N` 函数并
       记录 `mod->test_cases`；入口模块的 C 单元生成跨模块 runner。 */

    /* 第一遍：lower 所有模块，统计是否有堆分配。入口单元只有在**整个程序
       有分配**时才提供泄漏检测运行时（否则不引入 stdlib 等头，避免与用户
       FFI 声明（如 `div`）冲突）。 */
    units = (cir_unit_t **)calloc(nmods ? nmods : 1, sizeof(*units));
    mods = (module_t **)calloc(nmods ? nmods : 1, sizeof(*mods));
    if (!units || !mods) {
        fprintf(stderr, "build: out of memory\n");
        goto cleanup;
    }
    bool any_alloc = false;
    for (size_t i = 0; i < nmods; i++) {
        module_t *mod = (module_t *)vec_get(vm->module_order, i);
        if (!mod || !mod->sema || !mod->src_ast) continue;
        bool is_main = (mod == main_mod);
        bool require_main = is_main && !emit_c_only && !object_only && !tests;
        cir_unit_t *u = lower_one_module(arena, mod, tests, !require_main);
        if (!u) {
            /* 诊断留给 cleanup 统一打印。 */
            goto cleanup;
        }
        units[i] = u;
        mods[i] = mod;
        if (u->uses_alloc || u->uses_rc) any_alloc = true;
    }

    /* 第二遍：路径 / 用例表 / 写文件 / 编译。 */
    for (size_t i = 0; i < nmods; i++) {
        module_t *mod = mods[i];
        cir_unit_t *u = units[i];
        if (!u || !mod) continue;
        bool is_main = (mod == main_mod);
        u->owns_runtime = is_main && any_alloc;
        char *base = be_mirror_base(mod->canonical, build_dir);
        if (!base) {
            fprintf(stderr, "build: out of memory\n");
            goto cleanup;
        }
#ifdef _WIN32
        const char *obj_ext = ".obj";
#else
        const char *obj_ext = ".o";
#endif
        char *c_path = (is_main && pgm && pgm->entry_c && *pgm->entry_c)
                           ? be_concat2(pgm->entry_c, "")
                           : be_concat2(base, ".c");
        char *obj_path = (is_main && pgm && pgm->entry_obj && *pgm->entry_obj)
                             ? be_concat2(pgm->entry_obj, "")
                             : be_concat2(base, obj_ext);
        free(base);
        if (!c_path || !obj_path) {
            free(c_path);
            free(obj_path);
            fprintf(stderr, "build: out of memory\n");
            goto cleanup;
        }
        owned[nowned++] = c_path;
        owned[nowned++] = obj_path;
        bool require_main = is_main && !emit_c_only && !object_only && !tests;
        if (tests) {
            /* 依赖模块只出测试函数（不带 runner）；用例合并到入口单元。 */
            if (!is_main) u->test_runner = false;
            char tag[16];
            module_tag_for_path(mod->canonical, tag, sizeof tag);
            size_t tcn = mod->test_cases ? vec_len(mod->test_cases) : 0;
            if (tcn > 0) {
                size_t need = ncases + tcn;
                if (need > cases_cap) {
                    size_t ncap = need * 2 + 4;
                    cir_test_case_t *grown = (cir_test_case_t *)realloc(
                        all_cases, ncap * sizeof(*all_cases));
                    if (!grown) {
                        fprintf(stderr, "build: out of memory\n");
                        goto cleanup;
                    }
                    all_cases = grown;
                    cases_cap = ncap;
                }
                for (size_t k = 0; k < tcn; k++) {
                    module_test_case_t *mtc =
                        (module_test_case_t *)vec_get(mod->test_cases, k);
                    if (!mtc) continue;
                    char sym[512];
                    snprintf(sym, sizeof sym, "clux_%s_fn_%s", tag,
                             mtc->func_name ? mtc->func_name : "");
                    all_cases[ncases].symbol =
                        be_concat2(sym, "");
                    all_cases[ncases].display = mtc->display_name
                                                    ? be_concat2(mtc->display_name, "")
                                                    : be_concat2("test", "");
                    if (!all_cases[ncases].symbol ||
                        !all_cases[ncases].display) {
                        fprintf(stderr, "build: out of memory\n");
                        goto cleanup;
                    }
                    ncases++;
                }
            }
            if (is_main) {
                u->test_runner = true;
                u->test_cases = all_cases;
                u->test_case_count = ncases;
                if (pgm && pgm->out_test_names) {
                    char **names = (char **)calloc(ncases ? ncases : 1,
                                                   sizeof(*names));
                    if (names) {
                        for (size_t k = 0; k < ncases; k++)
                            names[k] = all_cases[k].display
                                           ? be_concat2(all_cases[k].display, "")
                                           : NULL;
                        *pgm->out_test_names = names;
                        if (pgm->out_test_count) *pgm->out_test_count = ncases;
                    }
                }
            }
        }
        if (require_main) {
            bool has_entry = false;
            for (cir_function_t *f = u->functions; f; f = f->next)
                if (f->is_entry) has_entry = true;
            if (!has_entry) {
                fprintf(stderr, "build: no 'main' function in '%s'\n",
                        mod->canonical ? mod->canonical : src_path);
                goto cleanup;
            }
        }
        /* 写 .c/.h → 编译对象（测试模式的 runner 表已在上面确定）。 */
        if (write_one_module(alloc, u, c_path, obj_path, emit_c_only, keep_c,
                             tc) != 0) {
            /* 诊断留给 cleanup 统一打印。 */
            goto cleanup;
        }
        /* 合并模块声明的链接参数（extern lib），去重。 */
        for (cir_link_arg_t *l = u->link_args; l && nlinks < 128; l = l->next) {
            bool dup = false;
            for (size_t k = 0; k < nlinks; k++)
                if (strcmp(links[k], l->arg) == 0) dup = true;
            if (!dup) links[nlinks++] = l->arg;
        }
        if (!emit_c_only) objs[nobjs++] = obj_path;
    }
    free(units);
    units = NULL;
    free(mods);
    mods = NULL;
    if (!emit_c_only && !object_only) {
        if (tests && ncases == 0) {
            fprintf(stderr, "test: no test blocks found in '%s'\n", src_path);
            goto cleanup;
        }
        cir_link_arg_t *merged = NULL, *tail = NULL;
        for (size_t i = 0; i < nlinks; i++) {
            cir_link_arg_t *n =
                (cir_link_arg_t *)calloc(1, sizeof(*n));
            if (!n) break;
            n->arg = links[i];
            if (tail) tail->next = n;
            else merged = n;
            tail = n;
        }
        int lrc = link_exe(alloc, objs, nobjs, out_path, merged, tc);
        while (merged) {
            cir_link_arg_t *next = merged->next;
            free(merged);
            merged = next;
        }
        if (lrc != 0) goto cleanup;
    }
    rc = 0;

cleanup:
    if (units) free(units);
    if (mods) free(mods);
    if (objs) free(objs);
    if (all_cases) {
        for (size_t i = 0; i < ncases; i++) {
            free((void *)all_cases[i].symbol);
            free((void *)all_cases[i].display);
        }
        free(all_cases);
        all_cases = NULL;
    }
    if (owned) {
        for (size_t i = 0; i < nowned; i++) free(owned[i]);
        free(owned);
    }
    if (path_stack) vec_free(alloc, &path_stack);
    if (entry) allocator_free(alloc, (void **)&entry);
    if (diag_has_error(diag)) diag_print_all(diag);
    vm_destroy(&vm);
    diag_buf_destroy(&diag);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return rc;
}

/* 编译/生成选项。 */
typedef struct cbackend_opts {
    bool     compile;    /* false：只生成 .c；true：继续编译/链接 */
    bool     object_only; /* true：只编译到目标文件（不链接） */
    bool     tests;      /* 测试运行器模式 */
    const char *obj_path; /* object_only 时的目标文件路径（NULL = 由 out 推导） */
    const cbackend_toolchain_t *tc; /* 宿主编译器/链接选项（可 NULL） */
    char  ***out_names;  /* tests：返回 malloc 的用例显示名数组 */
    size_t  *out_count;
} cbackend_opts_t;

/* 普通构建：test 块不参与运行期（与 driver 的非测试模式一致，直接摘链）。 */
static void strip_test_blocks(ast_program_t *prog) {
    ast_node_t *prev = NULL;
    ast_node_t *s = prog->funcs;
    while (s) {
        ast_node_t *next = s->next;
        if (s->kind == AST_TEST) {
            if (prev) prev->next = next;
            else prog->funcs = next;
            if (prog->funcs_last == s) prog->funcs_last = prev;
            s = next;
            continue;
        }
        prev = s;
        s = next;
    }
}

/* 把顶层 test 块改写成无参 void 函数 `__clux_test_<n>`，并收集显示名。
 * 与 driver 的测试模式同构：sema/compiler/lowering 只看到普通函数。 */
static bool prepare_tests(arena_t *arena, ast_program_t *prog,
                          char ***out_names, size_t *out_count) {
    char **names = NULL;
    size_t count = 0, cap = 0;
    ast_node_t *prev = NULL;
    ast_node_t *s = prog->funcs;
    size_t seq = 0;
    while (s) {
        ast_node_t *next = s->next;
        if (s->kind != AST_TEST) {
            prev = s;
            s = next;
            continue;
        }
        ast_test_t *t = (ast_test_t *)s;
        char nb[64];
        snprintf(nb, sizeof nb, "__clux_test_%zu", seq++);
        size_t nlen = strlen(nb);
        char *ncopy = (char *)arena_alloc(arena, nlen, 1);
        if (!ncopy) goto fail;
        memcpy(ncopy, nb, nlen);
        ast_node_t *fn_node = ast_func_def_new(arena, s->tok_begin, s->tok_end);
        if (!fn_node) goto fail;
        ast_func_def_t *fn = (ast_func_def_t *)fn_node;
        fn->name = strslice_from_bytes(ncopy, nlen);
        fn->params = NULL;
        fn->params_last = NULL;
        fn->return_expr = NULL; /* void */
        fn->body = t->body;
        fn_node->next = next;

        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            char **grown = (char **)realloc(names, ncap * sizeof(*grown));
            if (!grown) goto fail;
            names = grown;
            cap = ncap;
        }
        char *display = (char *)malloc(t->name.len + 1);
        if (!display) goto fail;
        if (t->name.len) memcpy(display, t->name.ptr, t->name.len);
        display[t->name.len] = '\0';
        names[count++] = display;

        if (prev) prev->next = fn_node;
        else prog->funcs = fn_node;
        if (prog->funcs_last == s) prog->funcs_last = fn_node;
        prev = fn_node;
        s = next;
    }
    *out_names = names;
    *out_count = count;
    return true;

fail:
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    return false;
}

static int cbackend_run_impl(const char *src_path, const char *out_path,
                             const char *c_path,
                             const cbackend_opts_t *opts) {
    if (!src_path || !c_path || !opts) return 1;
    bool compile = opts->compile;
    if (compile && !opts->object_only && !out_path) return 1;
    if (opts->object_only && !opts->obj_path) return 1;

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) return 1;
    arena_t *arena = arena_new_default(alloc);
    diag_buf_t *diag = diag_buf_new(alloc);
    vm_t *vm = vm_new(alloc);
    if (!arena || !diag || !vm) {
        if (vm) vm_destroy(&vm);
        if (diag) diag_buf_destroy(&diag);
        if (arena) arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }

    char *source = NULL;
    size_t source_len = 0;
    if (load_file(alloc, src_path, &source, &source_len) != 0) {
        fprintf(stderr, "build: cannot read '%s'\n", src_path);
        vm_destroy(&vm);
        diag_buf_destroy(&diag);
        arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }

    stream_source_t ss = stream_source_mem(alloc, source, source_len, true);
    istream_t *stream = istream_open(alloc, ss);
    if (!stream) {
        allocator_free(alloc, (void **)&source);
        vm_destroy(&vm);
        diag_buf_destroy(&diag);
        arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }
    lexer_t *lexer = lexer_create(alloc, stream, src_path);
    vec_t *tokens = vec_new(alloc, true);
    if (!lexer || !tokens) {
        if (lexer) lexer_close(&lexer);
        if (tokens) vec_free(alloc, &tokens);
        vm_destroy(&vm);
        diag_buf_destroy(&diag);
        arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }
    for (;;) {
        token_t *tok = lexer_next(lexer);
        if (!tok) break;
        vec_push(tokens, alloc, tok);
        token_kind_t kind = token_get_kind(tok);
        if (kind == TOKEN_TYPE_EOF) break;
        if (kind == TOKEN_TYPE_ERROR) {
            const location_t *loc = token_get_location(tok);
            const char *msg = token_get_error_message(tok);
            fprintf(stderr, "%s:%zu:%zu: error: %s\n", src_path,
                    loc ? loc->begin.line : 0, loc ? loc->begin.column : 0,
                    msg ? msg : "lexical error");
            lexer_close(&lexer);
            vec_free(alloc, &tokens);
            vm_destroy(&vm);
            diag_buf_destroy(&diag);
            arena_destroy(alloc, &arena);
            delete_allocator(&alloc);
            return 1;
        }
    }

    parser_t *parser = parser_create(alloc, arena, tokens);
    if (!parser) {
        lexer_close(&lexer);
        vec_free(alloc, &tokens);
        vm_destroy(&vm);
        diag_buf_destroy(&diag);
        arena_destroy(alloc, &arena);
        delete_allocator(&alloc);
        return 1;
    }
    parser->diag = diag;
    ast_node_t *ast = parser_parse(parser);
    parser_destroy(&parser);

    int rc = 1;
    sema_t *sema = NULL;
    cir_unit_t *unit = NULL;
    char **test_names = NULL;
    size_t test_count = 0;
    if (ast && ast->kind == AST_PROGRAM) {
        desugar_t ds = desugar_make(arena, diag, tokens);
        desugar_program(&ds, (ast_program_t *)ast);
        bool tests_ok = true;
        if (opts->tests) {
            if (!prepare_tests(arena, (ast_program_t *)ast, &test_names,
                               &test_count)) {
                fprintf(stderr, "test: out of memory\n");
                tests_ok = false;
            } else if (test_count == 0) {
                fprintf(stderr, "test: no test blocks found in '%s'\n",
                        src_path);
                tests_ok = false;
            }
        } else {
            strip_test_blocks((ast_program_t *)ast);
        }
        if (tests_ok && !diag_has_error(diag)) {
            sema = sema_create(vm, diag, tokens, arena);
            if (sema) {
                sema->module_path = src_path;
                if (sema_analyze(sema, ast)) {
                    /* 只出 C 或只出目标文件时允许没有 main（库模块/编译
                       单元）；链接可执行文件时仍要求 main。 */
                    bool allow_no_main = !compile || opts->object_only;
                    unit = cbackend_lower(arena, sema, (ast_program_t *)ast,
                                          src_path, opts->tests, allow_no_main);
                    if (unit) unit->owns_runtime = unit->uses_alloc;
                }
            }
        }
    }

    if (unit) {
        cbackend_optimize(unit);
        char *h_path = derive_h_path(c_path);
        if (!h_path) {
            fprintf(stderr, "build: out of memory\n");
        } else if (cir_write_pair(unit, c_path, h_path, alloc) != 0) {
            fprintf(stderr, "build: cannot write C source '%s' / header "
                            "'%s'\n",
                    c_path, h_path);
        } else if (!compile) {
            rc = 0;
        } else {
            /* 两步：先编译成目标文件，再链接成可执行文件（多文件构建时
               目标文件可被复用；`--emit-obj` 在此停住）。 */
            char *derived_obj = NULL;
            const char *obj = opts->obj_path;
            if (!obj) {
                derived_obj = derive_obj_path(out_path);
                obj = derived_obj;
            }
            bool ok = false;
            if (!obj) {
                fprintf(stderr, "build: out of memory (object path)\n");
            } else if (compile_object(alloc, c_path, obj, opts->tc) == 0) {
                ok = true;
                if (!opts->object_only)
                    ok = link_exe(alloc, &obj, 1, out_path, unit->link_args,
                                  opts->tc) == 0;
                if (ok) {
                    /* 成功：C 中间文件是临时的；目标文件与头文件保留。 */
                    remove(c_path);
                    rc = 0;
                }
                /* 链接失败：目标文件有效，保留便于诊断/手动重链。 */
            } else {
                remove(obj); /* 编译失败：删掉半成品目标文件 */
            }
            free(derived_obj);
        }
        free(h_path);
    } else if (!diag_has_error(diag) && !(opts->tests && test_count == 0)) {
        /* 没有诊断却拿不到 C unit：内部失败（降级不当），必须显式报出，
           避免静默返回 1。 */
        fprintf(stderr, "build: internal error: no C unit generated\n");
    }

    if (rc == 0 && opts->tests) {
        if (opts->out_names) *opts->out_names = test_names;
        if (opts->out_count) *opts->out_count = test_count;
        test_names = NULL; /* 所有权移交给调用方 */
    }
    if (test_names) {
        for (size_t i = 0; i < test_count; i++) free(test_names[i]);
        free(test_names);
    }

    if (diag_has_error(diag)) diag_print_all(diag);
    if (sema) {
        sema_scope_t *scope = sema->global_scope;
        sema_destroy(&sema);
        if (scope) sema_scope_destroy(&scope);
    }
    lexer_close(&lexer);
    vec_free(alloc, &tokens);
    vm_destroy(&vm);
    diag_buf_destroy(&diag);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return rc;
}

int cbackend_build_ex(const char *src_path, const char *out_path,
                      const char *c_path, const char *obj_path,
                      const cbackend_toolchain_t *tc) {
    if (!src_path) return 1;
    if (!out_path && !obj_path) {
        fprintf(stderr, "build: internal: neither output nor object path\n");
        return 1;
    }
    char *owned_c = NULL;
    if (!c_path) {
        owned_c = derive_c_path(out_path ? out_path : obj_path);
        if (!owned_c) {
            fprintf(stderr, "build: out of memory\n");
            return 1;
        }
        c_path = owned_c;
    }
    cbackend_opts_t opts = {
        .compile = true,
        .object_only = out_path == NULL,
        .obj_path = obj_path,
        .tc = tc,
    };
    int rc = cbackend_run_impl(src_path, out_path, c_path, &opts);
    free(owned_c);
    return rc;
}

int cbackend_build_file_ex(const char *src_path, const char *out_path,
                           const cbackend_toolchain_t *tc) {
    return cbackend_build_ex(src_path, out_path, NULL, NULL, tc);
}

int cbackend_build_file(const char *src_path, const char *out_path) {
    return cbackend_build_file_ex(src_path, out_path, NULL);
}

int cbackend_emit_object(const char *src_path, const char *obj_path,
                         const cbackend_toolchain_t *tc) {
    return cbackend_build_ex(src_path, NULL, NULL, obj_path, tc);
}

int cbackend_generate_c(const char *src_path, const char *c_path) {
    if (!src_path || !c_path) return 1;
    cbackend_opts_t opts = {.compile = false};
    return cbackend_run_impl(src_path, NULL, c_path, &opts);
}

int cbackend_build_tests(const char *src_path, const char *out_path,
                         char ***names, size_t *count, bool keep_c,
                         bool emit_c) {
    if (!src_path || !out_path) return 1;
    /* 与 build 同路径：入口 + import 依赖各自生成编译单元，入口单元生成
       跨模块用例分派的 runner main（不从 main 进入）。 */
    cbackend_program_t pgm = {
        .build_dir = "build",
        .tests = true,
        .keep_c = keep_c,
        .emit_c_only = emit_c,
        .out_test_names = names,
        .out_test_count = count,
    };
    return cbackend_build_program(src_path, out_path, &pgm, NULL);
}

void cbackend_free_test_names(char **names, size_t count) {
    if (!names) return;
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}
