#include "driver/driver.h"
#include "driver/compile_module.h"
#include "module/module_path.h"
#include "vm/bcode_graph.h"
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
#include "parser/ast_reexport.h"
#include "parser/ast_extern_lib.h"
#include "diag/diagnostic.h"
#include "sema/sema.h"
#include "sema/symbol.h"
#include "compiler/compiler.h"
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/scope.h"
#include "vm/exec.h"
#include "vm/bcode.h"
#include "vm/bcode_disasm.h"
#include "vm/bcode_asm.h"
#include "vm/bcode_asm_defs.h"
#include "vm/bcode_serial.h"
#include "vm/type_error.h"
#include "vm/module.h"
#include "vm/ffi_lib.h"   /* M6 FFI：extern lib 的动态库加载 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
                           lexer_t **out_lexer) {
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
      fprintf(stderr, "%s:%zu:%zu: error: %s\n",
              path,
              loc ? loc->begin.line : 0,
              loc ? loc->begin.column : 0,
              msg ? msg : "unrecognized input");
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

int driver_run_file(const char *path) {
  if (!path) {
    fprintf(stderr, "run: no input file\n");
    return 1;
  }

  allocator_t *alloc = create_allocator(malloc, free);
  if (!alloc) {
    fprintf(stderr, "run: out of memory\n");
    return 1;
  }

  arena_t *arena = arena_new_default(alloc);
  if (!arena) {
    fprintf(stderr, "run: out of memory\n");
    delete_allocator(&alloc);
    return 1;
  }

  diag_buf_t *diag = diag_buf_new(alloc);
  if (!diag) {
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  /* M5：统一走 compile_module 入口——lex→parse→sema→compile→run 全流水线。
     单 vm 同时用于编译期（sema shadow 执行 + 类型登记）和运行期（注册段执行 +
     main 调用）。模块的 global_scope 由 compile_module 内 exec_run 产出，
     main 在 module->global_scope 中查找。 */
  vm_t *vm = vm_new(alloc);
  if (!vm) {
    diag_buf_destroy(&diag);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  /* 保存 vm 原始 root_scope/current_scope（vm_destroy 会销毁它们）。
     compile_module 内部会保存/恢复 root_scope/current_scope，但 run_file
     为了执行 main 需要临时切换到模块 scope。模块 scope 归 module_t 拥有
     （vm_destroy 通过 module_destroy 释放），不能让 vm_destroy 的
     scope_destroy 再释放一次。 */
  scope_t *vm_orig_root    = vm->root_scope;
  scope_t *vm_orig_current = vm->current_scope;

  vec_t *path_stack = vec_new(alloc, /*owns_element=*/false);
  if (!path_stack) {
    vm_destroy(&vm);
    diag_buf_destroy(&diag);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  /* 入口路径词法规范化（与 import 解析同一套规则：`\`→`/`、折叠 `.`/`..`）：
     使 vm->modules 的 key 稳定一致——同一文件不会因 "./x.cx" / "x.cx" /
     "a/../x.cx" 等不同写法被重复编译，也让 import 的相对解析基准可靠。 */
  char *entry_path = module_path_normalize(alloc, path);
  if (!entry_path) {
    fprintf(stderr, "run: out of memory\n");
    vec_free(alloc, &path_stack);
    vm_destroy(&vm);
    diag_buf_destroy(&diag);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  module_t *mod = compile_module(vm, alloc, diag, arena, entry_path, path_stack);
  allocator_free(alloc, (void **)&entry_path);
  vec_free(alloc, &path_stack);

  if (!mod) {
    diag_print_all(diag);
    vm_destroy(&vm);
    diag_buf_destroy(&diag);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  /* 查找 main 函数（在模块的 global_scope 中） */
  value_t *main_fn = scope_lookup(mod->global_scope, STRSLICE_LIT("main"));
  if (!main_fn) {
    fprintf(stderr, "run: no entry function 'main'\n");
    /* 恢复 vm scope 到原始值（避免 vm_destroy 释放 module 的 scope） */
    vm->root_scope    = vm_orig_root;
    vm->current_scope = vm_orig_current;
    vm_destroy(&vm); /* 释放 module（含 bc + global_scope + exports） */
    diag_buf_destroy(&diag);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  /* 设置 vm 状态到入口模块（main 函数的 root_scope 已在 func_t 中记录，
     value_call→func_vcall 会自动切换 root_scope + bc） */
  vm->root_scope    = mod->global_scope;
  vm->current_scope = mod->global_scope;
  vm->bc            = mod->bc;

  value_t *mr = value_call(vm, main_fn, NULL, 0);
  if (value_is_error(vm, mr)) {
    error_data_t *ed = (error_data_t *)value_data(mr);
    fprintf(stderr, "run: %s\n",
            ed && ed->message ? ed->message : "runtime error");
    /* 恢复 vm scope 到原始值（避免 vm_destroy 释放 module 的 scope） */
    vm->root_scope    = vm_orig_root;
    vm->current_scope = vm_orig_current;
    vm_destroy(&vm);
    diag_buf_destroy(&diag);
    arena_destroy(alloc, &arena);
    delete_allocator(&alloc);
    return 1;
  }

  /* 恢复 vm scope 到原始值（module scope 归 module_destroy 释放） */
  vm->root_scope    = vm_orig_root;
  vm->current_scope = vm_orig_current;

  /* module 归 vm->modules 拥有，vm_destroy 统一释放 */
  vm_destroy(&vm);
  diag_buf_destroy(&diag);
  arena_destroy(alloc, &arena);
  delete_allocator(&alloc);

  return 0;
}

/* ---- Test/utility entry: lex into a pool (text dangles after return) ---- */

int driver_lex_file(allocator_t *alloc, const char *path, vec_t **out_pool) {
  lexer_t *lexer = NULL;
  if (driver_lex_into(alloc, path, out_pool, &lexer) != 0) return -1;
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

static void driver_compiled_dispose(driver_compiled_t *r) {
    if (!r) return;
    if (r->diag)       diag_buf_destroy(&r->diag);
    if (r->vm)         vm_destroy(&r->vm);
    if (r->arena)      arena_destroy(r->alloc, &r->arena);
    if (r->alloc)      delete_allocator(&r->alloc);
    memset(r, 0, sizeof *r);
}

/* 复用 run 管线 Stage ①-⑤（加载→词法→语法→语义→编译），不执行。
 * 成功返回 0 并填满 *out（bc 与所有句柄有效）；失败返回非 0 且 *out 全 NULL
 * （诊断已打印）。调用方负责在反汇编后 driver_compiled_dispose。 */
static int driver_compile_to_bytecode(const char *path, driver_compiled_t *out) {
    memset(out, 0, sizeof *out);

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        fprintf(stderr, "build: out of memory\n");
        return 1;
    }
    out->alloc = alloc;

    arena_t *arena = arena_new_default(alloc);
    if (!arena) {
        fprintf(stderr, "build: out of memory\n");
        driver_compiled_dispose(out);
        return 1;
    }
    out->arena = arena;

    diag_buf_t *diag = diag_buf_new(alloc);
    if (!diag) {
        driver_compiled_dispose(out);
        return 1;
    }
    out->diag = diag;

    vm_t *vm = vm_new(alloc);
    if (!vm) {
        driver_compiled_dispose(out);
        return 1;
    }
    out->vm = vm;

    vec_t *path_stack = vec_new(alloc, /*owns_element=*/false);
    if (!path_stack) {
        driver_compiled_dispose(out);
        return 1;
    }

    /* 入口路径词法规范化（与 run 同一套规则，见 module/module_path.h）：
       使 vm->modules 的 key 稳定一致、import 相对解析基准可靠。 */
    char *entry_path = module_path_normalize(alloc, path);
    if (!entry_path) {
        fprintf(stderr, "build: out of memory\n");
        driver_compiled_dispose(out);
        return 1;
    }

    /* **与 run 共用同一核心**：lex→parse→sema→compile→运行时注册段执行 →
       module_t（注册进 vm->modules）。因此模块导入、循环依赖检测、路径
       规范化、模块缓存对 build -asm/-bin 同样生效。 */
    module_t *mod = compile_module(vm, alloc, diag, arena, entry_path, path_stack);
    /* 入口路径与路径栈由调用方拥有/释放（与 driver_run_file 同一约定：
       module 已复制 canonical 作为 vm->modules 的 key）。本项目分配器在
       delete_allocator 时把存活分配报为泄漏，故必须显式释放。 */
    allocator_free(alloc, (void **)&entry_path);
    vec_free(alloc, &path_stack);

    if (!mod) {
        diag_print_all(diag); /* compile_module 不打印；最外层入口统一打印 */
        driver_compiled_dispose(out);
        return 1;
    }
    out->module = mod;
    return 0;
}

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
static struct module_t *driver_resolve_import(void *user, sema_t *sema,
                                              ast_node_t *import_node,
                                              strslice_t raw) {
    driver_import_ctx_t *c = (driver_import_ctx_t *)user;
    if (!c || !sema) return NULL;

    /* 路径分类：只看路径形式，不嗅探扩展名。M5 只支持相对路径源码模块；
       包导入与动态库绑定不在 M5 范围内（后者留给 M6，且应由显式语法表达）。 */
    module_import_kind_t kind = MODULE_IMPORT_MODULE;
    char *canonical = module_path_resolve(c->alloc, raw, c->importer, &kind);
    if (kind == MODULE_IMPORT_NON_PATH) {
        diag_error(c->diag, sema_loc(sema, import_node),
                   "import path must be relative (start with './' or '../'); "
                   "package and dynamic-library imports are not supported in M5");
        return NULL;
    }
    if (!canonical) {
        diag_error(c->diag, sema_loc(sema, import_node),
                   "out of memory resolving module path");
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
    module_t *mod = compile_module(c->vm, c->alloc, c->diag, c->arena, canonical,
                                   c->path_stack);
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

module_t *compile_module(vm_t *vm, allocator_t *alloc, diag_buf_t *diag,
                         arena_t *arena, const char *canonical,
                         vec_t *path_stack) {    if (!vm || !alloc || !diag || !canonical) return NULL;

    /* 1. cache 查找：已编译直接返回 */
    module_t *cached = vm_module_lookup(vm, canonical);
    if (cached) return cached;

    /* 2. 循环依赖检测：检查 canonical 是否在 path_stack 中 */
    if (path_stack) {
        size_t n = vec_len(path_stack);
        for (size_t i = 0; i < n; i++) {
            const char *p = (const char *)vec_get(path_stack, i);
            if (p && strcmp(p, canonical) == 0) {
                /* 打印循环依赖链 */
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

    /* 3. push 到路径栈 */
    if (path_stack) {
        vec_push(path_stack, alloc, (void *)canonical);
    }

    /* 4. lex → parse */
    vec_t *pool = NULL;
    lexer_t *lexer = NULL;
    int lex_result = driver_lex_into(alloc, canonical, &pool, &lexer);
    if (lex_result == -2) {
        /* 词法错误已输出到 stderr */
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

    /* 5. sema（独立创建，用完即销毁；借用 vm 的全局表） */
    sema_t *sema = sema_create(vm, diag, pool, arena);
    if (!sema) {
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        return NULL;
    }
    /* 注入 import 解析回调：sema 遇 import 时回调到 driver（路径规范化 +
       存在性检查 + 递归编译 + 循环依赖检测），sema 自身不认识 driver。 */
    driver_import_ctx_t imp_ctx = {vm,    alloc,      diag,
                                   arena, path_stack, canonical};
    sema_set_import_resolver(sema, driver_resolve_import, &imp_ctx);

    bool sema_ok = sema_analyze(sema, ast);
    sema_scope_t *scope_tree = sema->global_scope;
    vec_t *sema_types = sema->types;

    if (!sema_ok) {
        sema_destroy(&sema);
        /* 诊断留给最外层入口打印（见上方 parse 分支说明） */
        if (scope_tree) sema_scope_destroy(&scope_tree);
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        return NULL;
    }

    /* 6. compile（AST + sema 作用域树 → 字节码） */
    compiler_t *comp = compiler_new(alloc, vm, diag, pool, scope_tree,
                                    sema_types);
    if (!comp) {
        sema_destroy(&sema);
        if (scope_tree) sema_scope_destroy(&scope_tree);
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        return NULL;
    }
    bytecode_t *bc = compiler_compile(comp, ast);
    compiler_destroy(&comp);
    sema_destroy(&sema);
    if (scope_tree) sema_scope_destroy(&scope_tree);

    if (!bc) {
        /* 诊断留给最外层入口打印（见上方 parse 分支说明） */
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        return NULL;
    }

    /* 字节码已自包含，但 AST 节点 name（strslice_t.ptr）仍借用 token pool
     * 中的 source buffer 文本——collect_export_names 需要读取 name。
     * lexer/pool 推迟到 collect_export_names 之后释放。 */

    /* 7. run 注册段：创建模块独立 scope，执行注册段产出 global_scope。
     *    保存/恢复 vm 的 root_scope/current_scope/bc——多模块共享同一 vm。 */
    scope_t *saved_root    = vm->root_scope;
    scope_t *saved_current = vm->current_scope;
    bytecode_t *saved_bc   = vm->bc;

    /* 注册段里的 IMPORT 操作数是**源码路径**，执行时需要以"本模块的规范路径"
       为基准重新规范化（与运行期 op_import 同一规则）。此刻 module_t 尚未创建，
       因此先借用调用方的 canonical（生命周期覆盖 exec_run）；随后 module_new
       会把它改指向 module 自己拥有的副本。 */
    if (bc) bc->module_path = canonical;

    /* 模块 global_scope：以 vm->global_scope 为 parent（内建类型可见） */
    scope_t *module_scope = scope_new(alloc, vm->global_scope);
    vm->root_scope    = module_scope;
    vm->current_scope = module_scope;

    value_t *er = exec_run(vm, bc);

    /* 恢复 vm 状态 */
    vm->root_scope    = saved_root;
    vm->current_scope = saved_current;
    vm->bc            = saved_bc;

    if (value_is_error(vm, er)) {
        error_data_t *ed = (error_data_t *)value_data(er);
        fprintf(stderr, "compile: module '%s' registration failed: %s\n",
                canonical, ed && ed->message ? ed->message : "execution error");
        scope_destroy(vm, &module_scope);
        bcode_destroy(&bc);
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        return NULL;
    }

    /* 8. 构建 exports：从 global_scope 中提取 is_exported 符号的 value 借用。
     *    AST name（strslice_t.ptr）借用 source buffer，须在 lexer_close 前读取。
     *    随后并入 `export * from "./x"` 代理导出：依赖模块已在 sema pass0
     *    递归编译并注册进 vm->modules（按同一路径解析规则重解析即可查到），
     *    其导出符号是**借用**（值归依赖模块的 global_scope），直接并入本表。 */
    vec_t *export_names = collect_export_names(alloc, ast);
    strmap_t *exports = strmap_new(alloc, /*owns_value=*/false);
    if (export_names && exports) {
        size_t n = vec_len(export_names);
        for (size_t i = 0; i < n; i++) {
            char *name = (char *)vec_get(export_names, i);
            if (!name) continue;
            value_t *v = scope_lookup(module_scope, strslice_from_cstr(name));
            if (v) {
                strmap_insert(exports, alloc, name, v);
            }
            allocator_free(alloc, (void **)&name);
        }
    }
    if (export_names) vec_free(alloc, &export_names);

    if (exports) {
        ast_program_t *prog = (ast_program_t *)ast;
        for (ast_node_t *n = prog->funcs; n; n = n->next) {
            if (n->kind != AST_REEXPORT) continue;
            ast_reexport_t *re = (ast_reexport_t *)n;

            /* 与 pass0 同一解析规则：以本模块路径为基准解析依赖路径 */
            module_import_kind_t kind = MODULE_IMPORT_MODULE;
            char *dep_path = module_path_resolve(alloc, re->path,
                                                 canonical, &kind);
            if (!dep_path) continue;   /* 非模块路径：pass0 已诊断 */
            module_t *dep = vm_module_lookup(vm, dep_path);
            allocator_free(alloc, (void **)&dep_path);
            if (!dep || !dep->exports) continue;  /* 编译失败：pass0 已诊断 */

            const vec_t *keys = strmap_keys(dep->exports);
            size_t kn = keys ? vec_len(keys) : 0;
            for (size_t i = 0; i < kn; i++) {
                const char *key = (const char *)vec_get((vec_t *)keys, i);
                if (!key) continue;
                value_t *v = (value_t *)strmap_get(dep->exports, key);
                value_t *old = (value_t *)strmap_get(exports, key);
                if (old) {
                    /* 同一值重复并入（同一模块被代理两次）视为幂等；
                       不同来源同名 → 符号冲突（m5-design §1.3）。 */
                    if (old == v) continue;
                    diag_error(diag, (location_t){0},
                               "export conflict: symbol '%s' already exported",
                               key);
                    continue;
                }
                strmap_insert(exports, alloc, key, v);
            }
        }
    }

    /* M6 FFI：extern lib → 加载动态库进**库表**（别名 → 句柄）；随后对每个
       `extern cfunc <库>::<名>` **dlsym 解析符号**，未解析即诊断（含库名/符号名
       与 OS 错误文本）。**判据是关键字 `extern lib`，路径只是加载参数**——不按
       后缀判类，能否加载由操作系统按内容判定。
       必须在 pool 释放前扫描：path/alias 是 token pool 的借出视图。
       符号解析目前只做**校验**：动态调用（libffi / 最小 ABI 层）属 Phase 4/5。 */
    strmap_t *ffi_libs = NULL;
    {
        ast_program_t *prog = (ast_program_t *)ast;
        for (ast_node_t *n = prog->funcs; n; n = n->next) {
            if (n->kind != AST_EXTERN_LIB) continue;
            ast_extern_lib_t *el = (ast_extern_lib_t *)n;
            char pathbuf[512], aliasbuf[256];
            snprintf(pathbuf, sizeof(pathbuf), "%.*s", (int)el->path.len,
                     el->path.ptr);
            snprintf(aliasbuf, sizeof(aliasbuf), "%.*s", (int)el->alias.len,
                     el->alias.ptr);
            char err[512] = {0};
            ffi_lib_t *lib = ffi_lib_open(alloc, pathbuf, err, sizeof(err));
            if (!lib) {
                diag_error(diag, (location_t){0}, "%s", err);
                continue;
            }
            if (!ffi_libs) ffi_libs = strmap_new(alloc, /*owns_value=*/false);
            if (strmap_get(ffi_libs, aliasbuf)) {
                diag_error(diag, (location_t){0},
                           "duplicate 'extern lib' alias '%s'", aliasbuf);
                ffi_lib_close(lib);
                continue;
            }
            strmap_insert(ffi_libs, alloc, aliasbuf, lib);
        }
        /* 编译期把所有 extern cfunc / extern var 的符号先解析一遍（dlsym /
           GetProcAddress）：符号不存在即在此报错，而不是等到运行期。
           函数符号与**数据符号**走同一条 dlsym 路径——数据符号返回的就是变量
           地址（extern var 的读写据此直接作用于库里的内存）。 */
        for (ast_node_t *n = prog->funcs; n; n = n->next) {
            const char *what = NULL;
            char libbuf[256], fnbuf[256];
            if (n->kind == AST_FUNC_DEF && ((ast_func_def_t *)n)->is_extern_cfunc) {
                ast_func_def_t *fd = (ast_func_def_t *)n;
                what = "extern cfunc";
                snprintf(libbuf, sizeof(libbuf), "%.*s", (int)fd->lib_name.len,
                         fd->lib_name.ptr);
                snprintf(fnbuf, sizeof(fnbuf), "%.*s", (int)fd->name.len,
                         fd->name.ptr);
            } else if (n->kind == AST_VAR_DEF &&
                       ((ast_var_def_t *)n)->is_extern_var) {
                ast_var_def_t *vd = (ast_var_def_t *)n;
                what = "extern var";
                snprintf(libbuf, sizeof(libbuf), "%.*s", (int)vd->lib_name.len,
                         vd->lib_name.ptr);
                snprintf(fnbuf, sizeof(fnbuf), "%.*s", (int)vd->name.len,
                         vd->name.ptr);
            } else {
                continue;
            }
            ffi_lib_t *lib =
                ffi_libs ? (ffi_lib_t *)strmap_get(ffi_libs, libbuf) : NULL;
            if (!lib) {
                /* sema 也会报同一问题（它需要别名符号做类型检查）——这里只在
                   driver 独占的情形下补充（sema 未跑或未诊断时），避免重复。 */
                continue;
            }
            char sym_err[512] = {0};
            (void)ffi_lib_sym(lib, fnbuf, sym_err, sizeof(sym_err));
            if (sym_err[0]) {
                /* dlsym/GetProcAddress 失败：文案含库名、符号名与 OS 错误。
                   此前同一错误被打印两次（重复的 diag_error），已合并为一处。 */
                diag_error(diag, (location_t){0}, "%s (%s %s)",
                           sym_err, what, fnbuf);
            }
        }
    }
    if (diag_has_error(diag)) {
        if (ffi_libs) {
            const vec_t *fkeys = strmap_keys(ffi_libs);
            size_t fcount = fkeys ? vec_len(fkeys) : 0;
            for (size_t fi = 0; fi < fcount; fi++) {
                const char *fk = (const char *)vec_get(fkeys, fi);
                ffi_lib_t *fl = (ffi_lib_t *)strmap_get(ffi_libs, fk);
                if (fl) ffi_lib_close(fl);
            }
            strmap_free(alloc, &ffi_libs);
        }
        lexer_close(&lexer);
        vec_free(alloc, &pool);
        if (exports) strmap_free(alloc, &exports);
        scope_destroy(vm, &module_scope);
        bcode_destroy(&bc);
        return NULL;
    }

    /* AST name 已拷贝完毕，lexer/pool 安全释放 */
    lexer_close(&lexer);
    vec_free(alloc, &pool);

    /* 9. 构建 module_t 并注册到 vm->modules */
    module_t *mod = module_new(alloc, bc, module_scope, exports, canonical);
    if (!mod) {
        fprintf(stderr, "compile: out of memory creating module '%s'\n", canonical);
if (exports) strmap_free(alloc, &exports);
        scope_destroy(vm, &module_scope);
        bcode_destroy(&bc);
        return NULL;
    }
    mod->ffi_libs = ffi_libs;  /* M6 FFI：库表所有权转移给模块（destroy 关闭全部） */
    vm_module_bind(vm, canonical, mod);

    /* 10. pop 路径栈 */
    if (path_stack && vec_len(path_stack) > 0) {
        vec_pop(path_stack);
    }

    return mod;
}

int driver_build_asm(const char *src_path, const char *out_path) {
    if (!src_path || !out_path) return 1;

    driver_compiled_t c;
    int rc = driver_compile_to_bytecode(src_path, &c);
    if (rc != 0) return rc; /* 诊断已由管线打印 */

    int dr = bcode_graph_disasm_file(c.alloc, c.vm, c.module, out_path);
    if (dr != 0) {
        fprintf(stderr, "build: cannot write asm file '%s'\n", out_path);
        driver_compiled_dispose(&c);
        return 1;
    }

    driver_compiled_dispose(&c);
    return 0;
}

int driver_build_bin(const char *src_path, const char *out_path) {
    if (!src_path || !out_path) return 1;

    driver_compiled_t c;
    int rc = driver_compile_to_bytecode(src_path, &c);
    if (rc != 0) return rc; /* 诊断已由管线打印 */

    int sr = bcode_graph_serial_file(c.alloc, c.vm, c.module, out_path);
    if (sr != 0) {
        fprintf(stderr, "build: cannot write bin file '%s'\n", out_path);
        driver_compiled_dispose(&c);
        return 1;
    }

    driver_compiled_dispose(&c);
    return 0;
}

/* ================================================================ */
/* run -asm：汇编 .cxs 文本 → 字节码 → 执行                          */
/* ================================================================ */


/* 模块图执行（定义见下方 ".cxb 模块图加载与执行" 段）：run -asm 与 run -bin
   共用——两者都先把落盘形态解析成模块图，再按依赖序执行。 */
static int driver_execute_graph(allocator_t *alloc, bcode_graph_t *g,
                                const char *origin);

int driver_run_asm(const char *asm_path) {
    if (!asm_path) {
        fprintf(stderr, "run: no asm file\n");
        return 1;
    }

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        fprintf(stderr, "run: out of memory\n");
        return 1;
    }

    /* 汇编 .cxs 文本 → 模块图（无 MODULE 指令 = 单模块入口） */
    bcode_graph_t g;
    bcode_graph_init(&g);
    int ar = bcode_graph_asm_from_file(alloc, asm_path, &g);
    if (ar != 0) {
        /* 汇编诊断已打印 */
        bcode_graph_free(&g);
        delete_allocator(&alloc);
        return 1;
    }

    int rc = driver_execute_graph(alloc, &g, asm_path);

    bcode_graph_free(&g);
    delete_allocator(&alloc);
    return rc;
}

/* ================================================================ */
/* run -bin：反序列化 .cxb 二进制 → 字节码 → 执行                    */
/* ================================================================ */

/* ================================================================ */
/* .cxb 模块图加载与执行（M5 字节码层闭环）                          */
/* ================================================================ */

/* 加载一张模块图并执行其入口模块的 main。
 *
 * 图内模块按**依赖优先**顺序（容器表序）：逐个建模块 scope → 执行注册段
 * （产出 global_scope 与模块级定义）→ 重建导出表 → 绑定进 vm->modules。
 * 依赖先于使用者就绪，因此 `IMPORT <path>` 指令在注册段里就能查到模块。
 * 导出表由容器携带的 `(name, owner_index)` 重建：owner = 自身则取本模块
 * global_scope 的符号，owner < 自身则借用该依赖模块的导出值（代理导出）。
 *
 * 模块所有权归 vm->modules（module_destroy 释放 bc/scope/exports），image->bc
 * 因此转移给 module_t。 */
static int driver_execute_graph(allocator_t *alloc, bcode_graph_t *g,
                                const char *origin) {
    /* 入口根路径：容器的相对源码路径以**产物所在目录**为基准解析（先规范化，
       与 VM 层 op_import 同一套规则）。随后按导入链推导每个模块的规范键——
       容器里存的是源码路径，键在加载时重算，产物因此与宿主布局解耦。 */
    char *root = module_path_normalize(alloc, origin ? origin : ".");
    if (!root) {
        fprintf(stderr, "run: out of memory\n");
        return 1;
    }
    if (bcode_graph_resolve_paths(alloc, g, root) != 0) {
        allocator_free(alloc, (void **)&root);
        return 1;
    }

    vm_t *vm = vm_new(alloc);
    if (!vm) {
        allocator_free(alloc, (void **)&root);
        fprintf(stderr, "run: out of memory\n");
        return 1;
    }
    /* 保存 vm 原始 scope/bc：执行期临时切到模块，结束时恢复，
       以免 vm_destroy 释放模块 scope（模块 scope 归 module_destroy）。 */
    scope_t *saved_root = vm->root_scope;
    scope_t *saved_current = vm->current_scope;
    bytecode_t *saved_bc = vm->bc;

    module_t **mods = (module_t **)allocator_new_ex(
        alloc, "clux.driver.graph.mods", sizeof(module_t *) * g->count, NULL,
        NULL, NULL, 1);
    if (!mods) {
        vm_destroy(&vm);
        fprintf(stderr, "run: out of memory\n");
        return 1;
    }
    memset(mods, 0, sizeof(module_t *) * g->count);

    int rc = 1;
    for (size_t i = 0; i < g->count; i++) {
        bcode_module_image_t *img = &g->modules[i];
        scope_t *mscope = scope_new(alloc, vm->global_scope);
        if (!mscope) {
            fprintf(stderr, "run: out of memory\n");
            goto done;
        }
        vm->root_scope = mscope;
        vm->current_scope = mscope;
        vm->bc = img->bc;
        /* 注册段里的 IMPORT 是源码路径：以本模块的推导规范路径为基准规范化 */
        img->bc->module_path = img->canonical;
        value_t *er = exec_run(vm, img->bc);
        if (value_is_error(vm, er)) {
            error_data_t *ed = (error_data_t *)value_data(er);
            fprintf(stderr, "run: module '%s' registration failed: %s\n",
                    img->canonical ? img->canonical : origin,
                    ed && ed->message ? ed->message : "execution error");
            scope_destroy(vm, &mscope);
            goto done;
        }

        strmap_t *exports = strmap_new(alloc, /*owns_value=*/false);
        if (!exports) {
            fprintf(stderr, "run: out of memory\n");
            scope_destroy(vm, &mscope);
            goto done;
        }
        for (size_t e = 0; e < img->export_count; e++) {
            const char *name = img->export_names[e];
            uint32_t owner = img->export_owners[e];
            if (!name) continue;
            value_t *v = (owner == i)
                ? scope_lookup(mscope, strslice_from_cstr(name))
                : (value_t *)strmap_get(mods[owner]->exports, name);
            if (v) strmap_insert(exports, alloc, name, v);
        }

        const char *key = img->canonical ? img->canonical : root;
        module_t *m = module_new(alloc, img->bc, mscope, exports, key);
        if (!m) {
            strmap_free(alloc, &exports);
            scope_destroy(vm, &mscope);
            fprintf(stderr, "run: out of memory\n");
            goto done;
        }
        img->bc = NULL; /* 所有权转移给 module_t */
        vm_module_bind(vm, key, m);
        mods[i] = m;

        /* M6 FFI：容器携带的 `extern lib` 声明 → 重新 dlopen 建库表。
           `EXTERN_CALL "别名::符号"` 运行期按别名查 `module_t.ffi_libs`，
           而库表是**编译期**扫描 AST 建的、字节码里没有——所以必须随容器
           落盘并在此重建，否则 `run x.cxb` 会在第一次 FFI 调用处报
           "no loaded library"。m 已绑定进 vm->modules，失败路径由
           vm_destroy → module_destroy 统一关闭已开的句柄。 */
        if (img->ffi_count > 0) {
            m->ffi_libs = strmap_new(alloc, /*owns_value=*/false);
            if (!m->ffi_libs) {
                fprintf(stderr, "run: out of memory\n");
                goto done;
            }
            for (size_t f = 0; f < img->ffi_count; f++) {
                char err[512] = {0};
                ffi_lib_t *lib = ffi_lib_open(alloc, img->ffi_paths[f], err,
                                              sizeof err);
                if (!lib) {
                    fprintf(stderr, "run: %s\n", err);
                    goto done;
                }
                strmap_insert(m->ffi_libs, alloc, img->ffi_aliases[f], lib);
            }
        }
    }

    /* 入口模块的 main */
    module_t *entry = mods[g->entry_index];
    value_t *main_fn = scope_lookup(entry->global_scope, STRSLICE_LIT("main"));
    if (!main_fn) {
        fprintf(stderr, "run: no entry function 'main'\n");
        goto done;
    }
    vm->root_scope = entry->global_scope;
    vm->current_scope = entry->global_scope;
    vm->bc = entry->bc;
    value_t *mr = value_call(vm, main_fn, NULL, 0);
    if (value_is_error(vm, mr)) {
        error_data_t *ed = (error_data_t *)value_data(mr);
        fprintf(stderr, "run: %s\n",
                ed && ed->message ? ed->message : "runtime error");
        goto done;
    }
    rc = 0;

done:
    vm->root_scope = saved_root;
    vm->current_scope = saved_current;
    vm->bc = saved_bc;
    allocator_free(alloc, (void **)&mods);
    vm_destroy(&vm); /* 经 module_destroy 释放各模块（bc + scope + exports） */
    allocator_free(alloc, (void **)&root);
    return rc;
}

int driver_run_bin(const char *bin_path) {
    if (!bin_path) {
        fprintf(stderr, "run: no bin file\n");
        return 1;
    }

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        fprintf(stderr, "run: out of memory\n");
        return 1;
    }

    /* 反序列化 .cxb → 模块图（v2 模块表；v1 单模块产物同样可运行） */
    bcode_graph_t g;
    bcode_graph_init(&g);
    if (bcode_graph_from_file(alloc, bin_path, &g) != 0) {
        /* 反序列化诊断已打印 */
        bcode_graph_free(&g);
        delete_allocator(&alloc);
        return 1;
    }

    int rc = driver_execute_graph(alloc, &g, bin_path);

    bcode_graph_free(&g);
    delete_allocator(&alloc);
    return rc;
}

/* ================================================================ */
/* 落盘格式互转：.cxs ⇄ .cxb（不执行，不经过源码前端）              */
/* ================================================================ */

int driver_asm_to_bin(const char *asm_path, const char *bin_path) {
    if (!asm_path || !bin_path) return 1;

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        fprintf(stderr, "conv: out of memory\n");
        return 1;
    }

    /* 汇编 .cxs 文本 → 模块图（无 MODULE 指令 = 单模块入口，向后兼容） */
    bcode_graph_t g;
    bcode_graph_init(&g);
    int ar = bcode_graph_asm_from_file(alloc, asm_path, &g);
    if (ar != 0) {
        /* 汇编诊断已打印 */
        bcode_graph_free(&g);
        delete_allocator(&alloc);
        return 1;
    }

    /* 模块图 → .cxb 二进制（owner 下标由文本携带，原样落盘） */
    int sr = bcode_graph_serial_image_file(alloc, &g, bin_path);
    if (sr != 0) {
        fprintf(stderr, "conv: cannot write bin file '%s'\n", bin_path);
        bcode_graph_free(&g);
        delete_allocator(&alloc);
        return 1;
    }

    bcode_graph_free(&g);
    delete_allocator(&alloc);
    return 0;
}

int driver_bin_to_asm(const char *bin_path, const char *asm_path) {
    if (!bin_path || !asm_path) return 1;

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        fprintf(stderr, "conv: out of memory\n");
        return 1;
    }

    /* 反序列化 .cxb → 模块图 */
    bcode_graph_t g;
    bcode_graph_init(&g);
    if (bcode_graph_from_file(alloc, bin_path, &g) != 0) {
        /* 反序列化诊断已打印 */
        bcode_graph_free(&g);
        delete_allocator(&alloc);
        return 1;
    }

    /* 反汇编模块图 → .cxs 文本（多模块产物含 MODULE/export 指令；
       单模块产物与旧格式逐字一致） */
    int wr = bcode_graph_disasm_to_file(alloc, &g, asm_path);
    if (wr != 0) {
        fprintf(stderr, "conv: cannot write asm file '%s'\n", asm_path);
        bcode_graph_free(&g);
        delete_allocator(&alloc);
        return 1;
    }

    bcode_graph_free(&g);
    delete_allocator(&alloc);
    return 0;
}

/* ================================================================ */
/* 输入类型探测（内容嗅探，不依赖扩展名）                            */
/* ================================================================ */

const char *driver_input_kind_name(driver_input_kind_t kind) {
    switch (kind) {
        case DRIVER_INPUT_SOURCE: return "source";
        case DRIVER_INPUT_CXS:    return "cxs";
        case DRIVER_INPUT_CXB:    return "cxb";
        default:                  return "unknown";
    }
}

/* 首 token 是否命中 BCODE_ASM_TABLE 助记符（大小写无关）。 */
static bool is_asm_mnemonic(const char *tok, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < BCODE_ASM_TABLE_COUNT; i++) {
        const bcode_asm_entry_t *e = &BCODE_ASM_TABLE[i];
        if (!e->mnemonic) continue;
        size_t n = strlen(e->mnemonic);
        if (n != len) continue;
        bool eq = true;
        for (size_t k = 0; k < len; k++) {
            char a = tok[k], b = e->mnemonic[k];
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (a != b) { eq = false; break; }
        }
        if (eq) return true;
    }
    /* .byte 伪指令（大小写无关） */
    if (len == 5) {
        char c0 = tok[0] == '.' ? '.' : 0;
        if (c0 == '.' && (tok[1] == 'b' || tok[1] == 'B') &&
            (tok[2] == 'y' || tok[2] == 'Y') &&
            (tok[3] == 't' || tok[3] == 'T') &&
            (tok[4] == 'e' || tok[4] == 'E')) {
            return true;
        }
    }
    return false;
}

/* 该行是否为汇编标签定义：去空白后形如 "name:"（无内嵌空白）。 */
static bool is_asm_label(const char *ls, const char *le) {
    if (le <= ls + 1 || le[-1] != ':') return false;
    for (const char *p = ls; p < le - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t') return false;
        bool ok = (c == '_' || c == '.' || c == '-') ||
                  (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z');
        if (!ok) return false;
    }
    return true;
}

/* 文本是否含 clux 源码关键字（作为词边界，避免误匹配标识符内的子串）。 */
static bool has_source_keyword(const char *s, size_t len) {
    static const char *kws[] = {
        "func", "var", "if", "else", "while", "do", "for", "foreach", "return",
        "struct", "enum", "type", "comptime", "import", "interface", "using",
        "sizeof", "alignof", "typeof",
        NULL,
    };
    for (size_t k = 0; kws[k]; k++) {
        size_t kl = strlen(kws[k]);
        if (kl > len) continue;
        for (size_t i = 0; i + kl <= len; i++) {
            bool eq = true;
            for (size_t j = 0; j < kl; j++) {
                if (s[i + j] != kws[k][j]) { eq = false; break; }
            }
            if (!eq) continue;
            /* 词边界检查：前后不能是标识符字符 */
            bool lb = (i == 0) ||
                      !((s[i - 1] >= 'a' && s[i - 1] <= 'z') ||
                        (s[i - 1] >= 'A' && s[i - 1] <= 'Z') ||
                        (s[i - 1] >= '0' && s[i - 1] <= '9') ||
                        s[i - 1] == '_');
            bool rb = (i + kl == len) ||
                      !((s[i + kl] >= 'a' && s[i + kl] <= 'z') ||
                        (s[i + kl] >= 'A' && s[i + kl] <= 'Z') ||
                        (s[i + kl] >= '0' && s[i + kl] <= '9') ||
                        s[i + kl] == '_');
            if (lb && rb) return true;
        }
    }
    return false;
}

driver_input_kind_t driver_detect_input(const char *path) {
    if (!path) return DRIVER_INPUT_UNKNOWN;

    FILE *fp = fopen(path, "rb");
    if (!fp) return DRIVER_INPUT_UNKNOWN;

    uint8_t head[4096];
    size_t n = fread(head, 1, sizeof head, fp);
    fclose(fp);

    if (n == 0) return DRIVER_INPUT_UNKNOWN;

    /* ① 二进制字节码：magic 判定（100% 可靠） */
    if (n >= 4 && memcmp(head, "CXBC", 4) == 0) {
        return DRIVER_INPUT_CXB;
    }

    /* ② 文本：逐行扫描，首个"有效行"决定性质 */
    const char *cur = (const char *)head;
    const char *end = (const char *)head + n;
    while (cur < end) {
        const char *ls = cur;
        while (cur < end && *cur != '\n') cur++;
        const char *le = cur;
        if (cur < end) cur++;

        while (ls < le && (*ls == ' ' || *ls == '\t' || *ls == '\r')) ls++;
        while (le > ls && (le[-1] == ' ' || le[-1] == '\t' || le[-1] == '\r')) le--;
        if (ls >= le) continue;          /* 空行 */
        if (*ls == ';') continue;        /* 汇编行注释 */
        if (*ls == '[') continue;        /* 遗留 [.section ...] 标记 */
        if (ls[0] == '/' && ls + 1 < le && ls[1] == '/') {
            /* C 风格行注释（源码）：跳过继续找 */
            continue;
        }

        /* 标签定义 → 汇编；否则检查首 token 是否助记符 */
        if (is_asm_label(ls, le)) return DRIVER_INPUT_CXS;
        const char *tp = ls;
        while (tp < le && *tp != ' ' && *tp != '\t') tp++;
        if (is_asm_mnemonic(ls, (size_t)(tp - ls))) return DRIVER_INPUT_CXS;

        /* 该行不像汇编 → 用关键字判定是否为源码 */
        break;
    }

    if (has_source_keyword((const char *)head, n)) {
        return DRIVER_INPUT_SOURCE;
    }
    return DRIVER_INPUT_UNKNOWN;
}
