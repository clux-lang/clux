#include "driver/driver.h"
#include "desugar/desugar.h"
#include "driver/compile_module.h"
#include "module/module_path.h"
#include "module/package.h"

#include "core/allocator.h"
#include "core/stream.h"
#include "core/vec.h"
#include "core/arena.h"
#include "core/string.h"
#include "parser/lexer.h"
#include "parser/location.h"
#include "parser/parser.h"
#include "parser/ast_node.h"
#include "parser/ast_kind.h"
#include "parser/ast_program.h"
#include "parser/ast_error.h"
#include "parser/ast_func_def.h"
#include "parser/ast_var_def.h"
#include "parser/ast_type_def.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_union_def.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_interface_def.h"
#include "parser/ast_reexport.h"
#include "parser/ast_test.h"
#include "parser/ast_block.h"
#include "parser/ast_extern_lib.h"
#include "diag/diagnostic.h"
#include "sema/sema.h"
#include "sema/sema_generic.h"
#include "sema/symbol.h"

#include "vm/vm.h"
#include "vm/value.h"
#include "vm/scope.h"







#include "vm/module.h"

#include "cbackend/c_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ---- Internal: byte buffer class (for file-backed source buffers) ---- */

static class_t g_bytes_class = {
    .name = "clux.driver.bytes",
    .size = sizeof(char),
    .clone_fn = default_clone,
    .move_fn = default_move,
    .dispose_fn = NULL,
};

/* ---- Stage ①: load a source file fully into an allocator buffer ---- */

int driver_load_source(allocator_t *alloc,
                       const char *path,
                       const char **out_data,
                       size_t *out_len) {
  if (!alloc || !path || !out_data || !out_len) return -1;

  *out_data = NULL;
  *out_len = 0;

  FILE *fp = fopen(path, "rb");
  if (!fp) return -1;

  /* Determine the file size via a seek to the end. */
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  long size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return -1;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }

  /* stream_source_mem requires a non-NULL data pointer, so allocate at
   * least one byte even for an empty file. */
  size_t n = (size_t)size;
  char *buf = (char *)allocator_new(alloc, &g_bytes_class, n > 0 ? n : 1);
  if (!buf) {
    fclose(fp);
    return -1;
  }

  size_t read = fread(buf, 1, n, fp);
  fclose(fp);

  if (read != n) {
    /* Short read: the buffer is owned by the allocator; the caller is
     * expected to hand it to a mechanism that frees it (e.g. an
     * owns_data memory source). Signal failure but leave the buffer
     * allocated so it is not leaked. */
    *out_data = buf;
    *out_len = read;
    return -1;
  }

  *out_data = buf;
  *out_len = n;
  return 0;
}

/* ---- Internal: load + lex into a pool, keeping the lexer alive ---- */

static int driver_lex_into(allocator_t *alloc,
                           const char *path,
                           vec_t **out_pool,
                           lexer_t **out_lexer,
                           diag_buf_t *diag) {
  const char *data = NULL;
  size_t len = 0;
  if (driver_load_source(alloc, path, &data, &len) != 0) return -1;

  stream_source_t src = stream_source_mem(alloc, data, len, /*owns_data=*/true);
  istream_t *stream = istream_open(alloc, src);
  if (!stream) return -1;

  lexer_t *lexer = lexer_create(alloc, stream, path);
  if (!lexer) {
    istream_close(&stream);
    return -1;
  }

  vec_t *pool = vec_new(alloc, /*owns_element=*/true);
  if (!pool) {
    lexer_close(&lexer);
    return -1;
  }

  for (;;) {
    token_t *t = lexer_next(lexer);
    if (!t) break;
    vec_push(pool, alloc, t);
    token_kind_t kind = token_get_kind(t);
    if (kind == TOKEN_TYPE_EOF) break;
    if (kind == TOKEN_TYPE_ERROR) {
      const location_t *loc = token_get_location(t);
      const char *msg = token_get_error_message(t);
      if (diag && loc) {
        /* 编译路径：进共享诊断缓冲区，由流水线出口统一打印一次
           （带源码片段）；不要再直接 fprintf，否则会出现两条输出。 */
        diag_error(diag, *loc, "%s",
                   msg ? msg : "unrecognized input");
      } else {
        /* 无 diag 的调用方（formatter / 独立 lex 测试）保留直接输出。 */
        fprintf(stderr, "%s:%zu:%zu: error: %s\n",
                path,
                loc ? loc->begin.line : 0,
                loc ? loc->begin.column : 0,
                msg ? msg : "unrecognized input");
      }
      lexer_close(&lexer);
      vec_free(alloc, &pool);
      return -2;
    }
  }

  *out_pool = pool;
  *out_lexer = lexer;
  return 0;
}

/* ---- Stage ② + ③ + ④: lex → parse → sema ---- */

/* 运行一个已构建的可执行文件，返回其进程退出码（spawn 失败返回 -1）。 */
static int driver_spawn_wait(const char *exe) {
#ifdef _WIN32
    const char *argv[] = {exe, NULL};
    intptr_t rc = _spawnv(_P_WAIT, exe, argv);
    return rc < 0 ? -1 : (int)rc;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(exe, exe, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status)) return 1;
    return WEXITSTATUS(status);
#endif
}

/* 测试/驱动入口：C 后端编译 + 运行生成的可执行文件。
 *
 * 旧实现用进程内 VM；迁移到 C-only 后统一走 C 后端。编译期诊断仍由
 * cbackend 打印到 stderr（测试的 clux_run_capture 会捕获）；运行期的
 * `memory leak detected:` 由生成 C 的全局分配器在 main 退出时打印。 */
int driver_run_file_exit(const char *path, int *exit_code) {
    if (exit_code) *exit_code = 0;
    if (!path) {
        fprintf(stderr, "run: no input file\n");
        return 1;
    }
#ifdef _WIN32
    const char *suffix = ".run.exe";
#else
    const char *suffix = ".run";
#endif
    char *exe = (char *)malloc(strlen(path) + strlen(suffix) + 1);
    if (!exe) {
        fprintf(stderr, "run: out of memory\n");
        return 1;
    }
    sprintf(exe, "%s%s", path, suffix);
    cbackend_program_t pgm = {.build_dir = "build/tmp/test"};
    int rc = cbackend_build_program(path, exe, &pgm, NULL);
    if (rc == 0) {
        int code = driver_spawn_wait(exe);
        if (code < 0) {
            fprintf(stderr, "run: cannot execute '%s'\n", exe);
            rc = 1;
        } else if (exit_code) {
            *exit_code = code;
        }
    }
    free(exe);
    return rc;
}

int driver_run_file(const char *path) { return driver_run_file_exit(path, NULL); }

int driver_lex_file(allocator_t *alloc, const char *path, vec_t **out_pool) {
  lexer_t *lexer = NULL;
  if (driver_lex_into(alloc, path, out_pool, &lexer, NULL) != 0) return -1;
  /* Closing the lexer frees the source buffer; token kinds/locations stay
   * valid but token text slices become dangling. Consumers needing the
   * text should keep the lexer alive (see driver_run_file). */
  lexer_close(&lexer);
  return 0;
}

/* ================================================================ */
/* build --emit-asm/-bin：编译到字节码（不执行），供反汇编/序列化      */
/* ================================================================ */

/* 编译产物 + 其生命周期所需的全部对象句柄。反汇编必须在 dispose 前完成，
 * 因为 bc->strs 由 bc->alloc 分配，alloc 释放后字符串失效。
 *
 * **单一流水线**：本结构不再自持 lexer/pool/scope_tree/bc——编译统一走
 * `compile_module`（lex→parse→sema→compile→运行时注册段执行），产出
 * module_t（bc + global_scope + exports），注册进 vm->modules。因此
 * `run` 与 `build -asm/-bin` 的编译行为完全一致：模块导入、循环依赖检测、
 * 入口路径规范化、模块缓存都生效（此前 build 走另一套内联管线，既不支持
 * import 也不做路径规范化）。
 *
 * 生命周期：module 归 vm->modules 所有，`vm_destroy` 经 module_destroy 释放
 * 其 bc + global_scope + exports——dispose 因此**只**销毁 vm，不再单独
 * bcode_destroy / sema_scope_destroy（那会双重释放）。 */
typedef struct {
    allocator_t   *alloc;
    arena_t       *arena;
    vm_t          *vm;
    diag_buf_t    *diag;
    module_t      *module;   /* 借用：已注册进 vm->modules（vm 拥有） */
} driver_compiled_t;

/* ================================================================
 * M5 模块系统：compile_module 统一入口
 * ================================================================ */

/* 从 AST 顶层节点链提取导出符号名（is_exported=true 的声明）。
 * 遍历 program->funcs 兄弟链，收集每个 is_exported 节点的名字。
 * 返回一个 vec_t<char*>（NUL 终止的符号名拷贝，alloc 分配）。 */
static vec_t *collect_export_names(allocator_t *alloc, ast_node_t *program) {
    vec_t *names = vec_new(alloc, /*owns_element=*/false);
    if (!names) return NULL;

    ast_program_t *prog = (ast_program_t *)program;
    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (!n->is_exported) continue;
        strslice_t name = {0};
        switch (n->kind) {
            case AST_FUNC_DEF:   name = ((ast_func_def_t *)n)->name;  break;
            case AST_VAR_DEF:    name = ((ast_var_def_t *)n)->name;   break;
            case AST_TYPE_DEF:   name = ((ast_type_def_t *)n)->name;  break;
            case AST_ENUM_DEF:   name = ((ast_enum_def_t *)n)->name;  break;
            case AST_STRUCT_DEF: name = ((ast_struct_def_t *)n)->name; break;
            case AST_UNION_DEF:  name = ((ast_union_def_t *)n)->name;  break;
            case AST_CUNION_DEF: name = ((ast_cunion_def_t *)n)->name; break;
            case AST_INTERFACE_DEF:
                name = ((ast_interface_def_t *)n)->name;
                break;
            default: continue; /* export 只修饰声明节点 */
        }
        if (!name.ptr || name.len == 0) continue;
        /* 分配 NUL 终止拷贝（strslice.ptr 不保证 NUL 终止） */
        char *copy = (char *)allocator_new_ex(alloc, "export.name",
                                               name.len + 1, NULL, NULL, NULL, 1);
        if (!copy) continue;
        memcpy(copy, name.ptr, name.len);
        copy[name.len] = '\0';
        vec_push(names, alloc, copy);
    }
    return names;
}

/* ================================================================
 * sema 的 import 解析回调（消除 sema → driver 反向依赖）
 *
 * sema 遇 AST_IMPORT / AST_REEXPORT 时调用本回调：由 driver 负责路径分类与
 * 规范化（module 层）、存在性检查、递归编译（模块缓存 + 循环依赖检测），
 * 并把全部诊断记入共享 diag。返回的 module_t* 由 sema 存进 SEMA_SYM_MODULE
 * 符号（sema 不解释其内容）。
 * ================================================================ */

typedef struct {
    vm_t        *vm;
    allocator_t *alloc;
    diag_buf_t  *diag;
    arena_t     *arena;
    vec_t       *path_stack;  /* 编译路径栈（循环依赖检测；可为 NULL） */
    const char  *importer;    /* 当前模块规范路径（import 相对解析基准） */
} driver_import_ctx_t;

/* 拷贝一段字节为 NUL 终止字符串（alloc 拥有；driver 侧通用小工具） */
static char *dup_cstr(allocator_t *alloc, const char *s, size_t len) {
    char *p = (char *)allocator_new_ex(alloc, "clux.driver.str", len + 1, NULL,
                                       NULL, NULL, 1);
    if (!p) return NULL;
    if (len) memcpy(p, s, len);
    p[len] = '\0';
    return p;
}
static module_t *compile_module_impl(vm_t *vm, allocator_t *alloc,
                                    diag_buf_t *diag, arena_t *arena,
                                    const char *canonical, vec_t *path_stack,
                                    bool prepare_only);

typedef struct {
    vm_t       *vm;
    allocator_t *alloc;
    diag_buf_t *diag;
    arena_t    *arena;
} driver_finalize_ctx_t;

static module_t *driver_finalize_cb(void *user, module_t *mod) {
    driver_finalize_ctx_t *c = (driver_finalize_ctx_t *)user;
    if (!c || !mod) return NULL;
    return compile_module_impl(c->vm, c->alloc, c->diag, c->arena,
                               mod->canonical, NULL, false);
}

static struct module_t *driver_resolve_import(void *user, sema_t *sema,
                                              ast_node_t *import_node,
                                              strslice_t raw) {
    driver_import_ctx_t *c = (driver_import_ctx_t *)user;
    if (!c || !sema) return NULL;

    /* 路径分类：只看路径形式，不嗅探扩展名。
       相对路径（./ ../）→ 源码模块，相对 importer 解析；
       非路径（如 "std"、"std/array"）→ 包导入，从 deps/ 查找。 */
    module_import_kind_t kind = MODULE_IMPORT_MODULE;
    char *canonical = module_resolve_import(c->alloc, raw, c->importer, &kind);

    if (!canonical) {
        if (kind == MODULE_IMPORT_NON_PATH) {
            diag_error(c->diag, sema_loc(sema, import_node),
                       "dependency '%.*s' not found in deps/",
                       (int)raw.len, raw.ptr);
        } else {
            diag_error(c->diag, sema_loc(sema, import_node),
                       "out of memory resolving module path");
        }
        return NULL;
    }
    if (!module_path_exists(canonical)) {
        diag_error(c->diag, sema_loc(sema, import_node),
                   "module not found: '%.*s'", (int)raw.len, raw.ptr);
        allocator_free(c->alloc, (void **)&canonical);
        return NULL;
    }

    /* 递归编译被导入模块（同步走完整流水线 lex→parse→sema→compile→注册段
       执行）；compile_module 内部有 cache 查找 + 循环依赖检测。 */
    module_t *mod = compile_module_impl(c->vm, c->alloc, c->diag, c->arena,
                                      canonical, c->path_stack, true);
    if (!mod) {
        diag_error(c->diag, sema_loc(sema, import_node),
                   "failed to import module '%s'", canonical);
    } else if (!mod->import_path) {
        /* 记录**源码里写的路径**与导入者的规范路径：容器/文本存它（而非
           规范化后的宿主路径），加载时据此重新规范化。菱形依赖只需一条边
           ——两条边规范化后必然得到同一个 key。 */
        mod->import_path = dup_cstr(c->alloc, raw.ptr, raw.len);
        if (mod->import_path && c->importer) {
            mod->importer = dup_cstr(c->alloc, c->importer, strlen(c->importer));
        }
    }
    allocator_free(c->alloc, (void **)&canonical);
    return mod;
}

/* 阶段 1 导出符号表：name → sema_symbol_t*（借用）。跨模块 sema 解析在
   依赖模块尚未 finalize 时使用。`export *` 递归并入依赖模块的表。 */
static strmap_t *build_sema_exports(vm_t *vm, allocator_t *alloc, sema_t *sema,
                                    ast_node_t *ast, diag_buf_t *diag) {
    if (!vm || !alloc || !sema || !ast || ast->kind != AST_PROGRAM) return NULL;
    strmap_t *map = strmap_new(alloc, /*owns_value=*/false);
    if (!map) return NULL;
    vec_t *names = collect_export_names(alloc, ast);
    if (names) {
        size_t n = vec_len(names);
        for (size_t i = 0; i < n; i++) {
            char *name = (char *)vec_get(names, i);
            if (!name) continue;
            sema_symbol_t *sym = sema_scope_find_local(
                sema->global_scope, strslice_from_cstr(name));
            if (sym) strmap_insert(map, alloc, name, sym);
            allocator_free(alloc, (void **)&name);
        }
        vec_free(alloc, &names);
    }
    ast_program_t *prog = (ast_program_t *)ast;
    for (ast_node_t *n = prog->funcs; n; n = n->next) {
        if (n->kind != AST_REEXPORT) continue;
        ast_reexport_t *re = (ast_reexport_t *)n;
        module_import_kind_t kind = MODULE_IMPORT_MODULE;
        char *dep_path = module_path_resolve(alloc, re->path,
                                             sema->module_path, &kind);
        if (!dep_path) continue;
        module_t *dep = vm_module_lookup(vm, dep_path);
        allocator_free(alloc, (void **)&dep_path);
        if (!dep || !dep->sema_exports) continue;
        const vec_t *keys = strmap_keys(dep->sema_exports);
        size_t kn = keys ? vec_len((vec_t *)keys) : 0;
        for (size_t i = 0; i < kn; i++) {
            const char *key = (const char *)vec_get((vec_t *)keys, i);
            if (!key) continue;
            sema_symbol_t *sym =
                (sema_symbol_t *)strmap_get(dep->sema_exports, key);
            sema_symbol_t *old = (sema_symbol_t *)strmap_get(map, key);
            if (old && old != sym) {
                if (diag)
                    diag_error(diag, (location_t){0},
                               "export conflict: symbol '%s' already exported",
                               key);
                continue;
            }
            if (old == sym) continue;
            if (sym) strmap_insert(map, alloc, key, sym);
        }
    }
    return map;
}
/* 将顶层 test 块转成无参 void 函数（测试模式），普通构建直接摘除。 */
static bool driver_prepare_test_blocks(vm_t *vm, allocator_t *alloc,
                                       arena_t *arena, module_t *mod,
                                       ast_node_t *ast) {
  if (!vm || !alloc || !arena || !mod || !ast || ast->kind != AST_PROGRAM)
    return false;
  ast_program_t *prog = (ast_program_t *)ast;
  if (vm->test_mode && !mod->test_cases) {
    mod->test_cases = vec_new(alloc, /*owns_element=*/false);
    if (!mod->test_cases) return false;
  }

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

    if (!vm->test_mode) {
      if (prev) prev->next = next;
      else prog->funcs = next;
      if (prog->funcs_last == s) prog->funcs_last = prev;
      s = next;
      continue;
    }

    ast_test_t *t = (ast_test_t *)s;
    char nb[64];
    snprintf(nb, sizeof nb, "__clux_test_%zu", seq++);
    size_t nlen = strlen(nb);
    char *ncopy = (char *)arena_alloc(arena, nlen, 1);
    if (!ncopy) return false;
    memcpy(ncopy, nb, nlen);

    ast_node_t *fn_node = ast_func_def_new(arena, s->tok_begin, s->tok_end);
    if (!fn_node) return false;
    ast_func_def_t *fn = (ast_func_def_t *)fn_node;
    fn->name = (strslice_t){ncopy, nlen};
    fn->params = NULL;
    fn->params_last = NULL;
    fn->return_expr = NULL; /* void */
    fn->body = t->body;
    fn->owner_module = mod;
    fn->base.next = next;

    size_t dlen = t->name.len;
    char *dname = (char *)allocator_new_ex(alloc, "test.name", dlen + 1,
                                           NULL, NULL, NULL, 1);
    char *fname = (char *)allocator_new_ex(alloc, "test.func", nlen + 1,
                                           NULL, NULL, NULL, 1);
    module_test_case_t *tc = (module_test_case_t *)allocator_new_ex(
        alloc, "test.case", sizeof(*tc), NULL, NULL, NULL, 1);
    if (!dname || !fname || !tc) {
      if (dname) allocator_free(alloc, (void **)&dname);
      if (fname) allocator_free(alloc, (void **)&fname);
      if (tc) allocator_free(alloc, (void **)&tc);
      return false;
    }
    memcpy(dname, t->name.ptr, dlen);
    dname[dlen] = '\0';
    memcpy(fname, nb, nlen);
    fname[nlen] = '\0';
    tc->display_name = dname;
    tc->func_name = fname;
    vec_push(mod->test_cases, alloc, tc);

    if (prev) prev->next = fn_node;
    else prog->funcs = fn_node;
    if (prog->funcs_last == s) prog->funcs_last = fn_node;
    prev = fn_node;
    s = next;
  }
  return true;
}

static module_t *compile_module_impl(vm_t *vm, allocator_t *alloc,
                                    diag_buf_t *diag, arena_t *arena,
                                    const char *canonical, vec_t *path_stack,
                                    bool prepare_only) {

    module_t *mod = NULL;

    /* 1. 循环依赖检测必须先于 cache：两阶段下模块 stub 在 sema 前已登记，
       若先查 cache 会把正在 prepare 的模块当成完成，漏掉环。 */
    if (path_stack) {
        size_t n = vec_len(path_stack);
        for (size_t i = 0; i < n; i++) {
            const char *p = (const char *)vec_get(path_stack, i);
            if (p && strcmp(p, canonical) == 0) {
                fprintf(stderr, "error: circular dependency detected:\n");
                for (size_t j = i; j < n; j++) {
                    const char *cp = (const char *)vec_get(path_stack, j);
                    fprintf(stderr, "  %s\n", cp ? cp : "?");
                }
                fprintf(stderr, "  %s\n", canonical);
                diag_error(diag, (location_t){0}, "circular dependency: %s", canonical);
                return NULL;
            }
        }
    }

    /* 2. cache 查找：prepare 阶段直接复用；非 prepare 时对已 prepare 的模块
       跳到 finalize（两阶段第二遍）。 */
    module_t *cached = vm_module_lookup(vm, canonical);
    if (cached) return cached;

    /* 3. push 到路径栈 */
    if (path_stack) {
        vec_push(path_stack, alloc, (void *)canonical);
    }

    /* 4. lex → parse */
    vec_t *pool = NULL;
    lexer_t *lexer = NULL;
    int lex_result = driver_lex_into(alloc, canonical, &pool, &lexer, diag);
    if (lex_result == -2) {
        /* 词法错误已记录到诊断缓冲区（driver_lex_into 的 diag 参数）。 */
        if (lexer) lexer_close(&lexer);
        if (pool) vec_free(alloc, &pool);
        return NULL;
    }
    if (lex_result != 0) {
        fprintf(stderr, "compile: cannot open file '%s'\n", canonical);
        if (lexer) lexer_close(&lexer);
        if (pool) vec_free(alloc, &pool);
        return NULL;
    }

    parser_t *parser = parser_create(alloc, arena, pool);
    if (!parser) {
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        return NULL;
    }
    parser->diag = diag;
    ast_node_t *ast = parser_parse(parser);
    parser_destroy(&parser);

    if (!ast || ast->kind == AST_ERROR) {
        /* 诊断不在此打印：compile_module 会被外层模块经 sema 递归调用
           （import 链），统一由最外层入口在拿到 NULL 后打印一次——
           否则同一批诊断会随递归层数重复输出。 */
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        return NULL;
    }

    /* 4b. **前脱糖**（parser → sema 之间的纯 AST 变换）：
       `foreach` → 索引 for 循环这类纯语法结构改写。这一层不看符号表 / 不做
       类型检查，只按 AST 结构改写；sema 因此看不到 AST_FOREACH。
       （`defer` 是**后脱糖**，在 sema 内、comptime if 裁剪之后处理。） */
    {
      desugar_t ds = desugar_make(arena, diag, pool);
      desugar_program(&ds, (ast_program_t *)ast);
      if (diag && diag_has_error(diag)) {
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        return NULL;
      }
    }

    /* 4c. 两阶段流水线：模块对象在 sema 前创建并登记，跨模块 sema 可在
       依赖模块尚未 finalize 时通过其 sema 符号表解析。 */
    mod = module_new(alloc, NULL, NULL, NULL, canonical);
    if (!mod) {
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        return NULL;
    }
    mod->src_lexer = lexer;
    mod->src_tokens = pool;
    mod->src_ast = ast;
    vm_module_bind(vm, canonical, mod);

    if (!driver_prepare_test_blocks(vm, alloc, arena, mod, ast)) {
        fprintf(stderr, "compile: failed to prepare test blocks\n");
        mod->src_lexer = NULL;
        mod->src_tokens = NULL;
        mod->src_ast = NULL;
        vm_module_unbind(vm, mod->canonical);
        module_destroy(vm, &mod);
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        return NULL;
    }

    /* 5. sema（保留到 module 销毁；跨模块实例化/两阶段 finalize 使用） */
    sema_t *sema = sema_create(vm, diag, pool, arena);
    if (!sema) {
        mod->src_lexer = NULL;
        mod->src_tokens = NULL;
        mod->src_ast = NULL;
        vm_module_unbind(vm, mod->canonical);
        module_destroy(vm, &mod);
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        return NULL;
    }
    /* 注入 import 解析回调：sema 遇 import 时回调到 driver（路径规范化 +
       存在性检查 + 递归编译 + 循环依赖检测），sema 自身不认识 driver。 */
    sema->module_path = canonical;
    sema->current_module = mod;
    driver_import_ctx_t imp_ctx = {vm,    alloc,      diag,
                                   arena, path_stack, canonical};
    sema_set_import_resolver(sema, driver_resolve_import, &imp_ctx);

    driver_finalize_ctx_t fin_ctx = {vm, alloc, diag, arena};
    sema->module_finalize.finalize = driver_finalize_cb;
    sema->module_finalize.user = &fin_ctx;

    bool sema_ok = sema_analyze(sema, ast);
    sema_scope_t *scope_tree = sema->global_scope;

    if (!sema_ok) {
        sema_destroy(&sema);
        /* 诊断留给最外层入口打印（见上方 parse 分支说明） */
        if (scope_tree) sema_scope_destroy(&scope_tree);
        mod->src_lexer = NULL;
        mod->src_tokens = NULL;
        mod->src_ast = NULL;
        vm_module_unbind(vm, mod->canonical);
        module_destroy(vm, &mod);
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        return NULL;
    }

    /* 阶段 1 导出符号：供尚未 finalize 的依赖模块被跨模块 sema 解析。 */
    mod->sema_exports = build_sema_exports(vm, alloc, sema, ast, diag);
    mod->sema = sema;
    mod->sema_global = scope_tree;
    mod->prepared = true;
    if (vm->module_order) vec_push(vm->module_order, alloc, mod);

    if (prepare_only) {
        if (path_stack && vec_len(path_stack) > 0) vec_pop(path_stack);
        return mod;
    }

    return mod;
}

module_t *compile_module_prepare(vm_t *vm, allocator_t *alloc, diag_buf_t *diag,
                                 arena_t *arena, const char *canonical,
                                 vec_t *path_stack) {
    module_t *entry =
        compile_module_impl(vm, alloc, diag, arena, canonical, path_stack, true);
    if (!entry) return NULL;
    if (diag_has_error(diag)) return NULL;
    bool progressed = true;
    while (progressed) {
        progressed = false;
        if (!vm->module_order) break;
        size_t mn = vec_len(vm->module_order);
        for (size_t i = 0; i < mn; i++) {
            module_t *m = (module_t *)vec_get(vm->module_order, i);
            if (m && m->sema && sema_generic_drain_pending(m->sema))
                progressed = true;
        }
        if (diag_has_error(diag)) return NULL;
    }
    return entry;
}

