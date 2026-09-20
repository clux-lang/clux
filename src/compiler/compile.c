#include "compiler/compiler.h"
#include "core/panic.h"
#include "core/vec.h"
#include "parser/ast_program.h"
#include "parser/ast_func_def.h"
#include "parser/lexer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * 内部工具
 *
 * 共享工具（label/balance/st_push 等）供 compile_type.c / compile_expr.c /
 * compile_stmt.c / compile_func.c 经 compiler.h internal 段调用。
 * 编译期静态追踪状态（scope_depth/stack_depth/loop_stack）存于 compiler_t。
 * =========================================================================== */

static class_t g_compiler_class = {
    .name       = "clux.compiler",
    .size       = sizeof(compiler_t),
    .clone_fn   = NULL,
    .move_fn    = NULL,
    .dispose_fn = NULL,
};

static class_t g_patch_class = {
    .name       = "clux.compiler.patch",
    .size       = sizeof(compile_patch_t),
    .clone_fn   = NULL,
    .move_fn    = NULL,
    .dispose_fn = NULL,
};

location_t c_loc(compiler_t *c, const ast_node_t *node) {
  location_t zero = {0};
  if (!c || !node) return zero;
  const token_t *t = (const token_t *)vec_get(c->tokens, node->tok_begin);
  const location_t *loc = t ? token_get_location(t) : NULL;
  return loc ? *loc : zero;
}

void c_error(compiler_t *c, const ast_node_t *node, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  diag_error(c->diag, c_loc(c, node), "%s", buf);
  c->failed = true;
}

/* ---- 标签工具 ---- */

void label_init(compile_label_t *l) {
  l->defined = false;
  l->pos     = 0;
  l->patches = NULL;
}

/** 标记标签当前位置（后续跳转到此的 patch 回填此处） */
void label_here(compiler_t *c, compile_label_t *l) {
  l->defined = true;
  l->pos     = bcode_tell(c->bc);
  /* 回填所有前向 patch */
  compile_patch_t *p = l->patches;
  while (p) {
    bcode_patch_u32(c->bc, p->pos, (uint32_t)l->pos);
    compile_patch_t *next = p->next;
    allocator_free(c->alloc, (void **)&p);
    p = next;
  }
  l->patches = NULL;
}

/**
 * 发射跳转到 label：label 已定义 → 直接写目标；未定义 → 占位 0 并记 patch。
 * 跳转指令的 opcode 由调用方先发，操作数字段在 tell 时（opcode+4）取样。
 */
void emit_jump(compiler_t *c, compile_label_t *l) {
  size_t pos = bcode_tell(c->bc); /* opcode 已由调用方发出，此处即操作数字段 */
  if (l->defined) {
    bcode_write_u32(c->bc, (uint32_t)l->pos);
    return;
  }
  bcode_write_u32(c->bc, 0); /* 前向占位 */
  compile_patch_t *p = (compile_patch_t *)allocator_new(c->alloc, &g_patch_class, 1);
  if (!p) panic("compiler: out of memory allocating patch");
  memset(p, 0, sizeof(*p));
  p->pos = pos;
  p->next = l->patches;
  l->patches = p;
}

/* ---- 静态平衡工具 ---- */

void balance_push(compiler_t *c) {
  c->scope_depth++;
  bcode_write_op(c->bc, BCODE_PUSH_SCOPE);
}

void balance_pop(compiler_t *c) {
  if (c->scope_depth == 0) {
    c_error(c, NULL, "compiler: internal scope imbalance");
    return;
  }
  c->scope_depth--;
  bcode_write_op(c->bc, BCODE_POP_SCOPE);
}

/* 为跳转平衡作用域：跨出 n 层块作用域先发 n 个 POP_SCOPE。
   仅发指令、不改编译期 scope_depth——跳转（break/continue）是控制流出口，
   运行时在跳转前弹出 n 层；编译期静态深度仍保持当前块结构，后续语句
   （含 break/continue 之后的 dead code）继续按原嵌套深度编译，块级
   balance_pop 照常归位，避免双重弹出导致 imbalance。 */
void balance_scopes_out(compiler_t *c, size_t n) {
  while (n-- > 0) bcode_write_op(c->bc, BCODE_POP_SCOPE);
}

/* ---- 栈深度静态追踪 ---- */

void st_push(compiler_t *c, int delta) { c->stack_depth += delta; }

/* ===========================================================================
 * 程序编排
 * =========================================================================== */

bytecode_t *compiler_compile(compiler_t *c, ast_node_t *program) {
  if (!c || !program || program->kind != AST_PROGRAM) {
    diag_error(c ? c->diag : NULL, (location_t){0}, "compiler: expected program");
    return NULL;
  }
  ast_program_t *prog = (ast_program_t *)program;
  c->current_scope = c->global_scope;
  c->failed = false;
  c->scope_depth = 0;
  c->stack_depth = 0;

  bytecode_t *bc = bcode_new(c->alloc);
  if (!bc) {
    diag_error(c->diag, c_loc(c, program), "compiler: out of memory creating bytecode");
    return NULL;
  }
  c->bc = bc;

  /* 0. 全部函数收集（LOAD_FUNCTION 编译用）：compile_prescan_funcs 递归
     收集全部进入运行时的函数定义（全局 + 局部 + 嵌套函数字面量，含 comptime
     body 内被折叠产物引用的函数），校验读取 sema 分配的 fid（单一来源）。
     AST_FUNC_REF 纯 fid 标识，编译时直接 LOAD_FUNCTION <fid>。 */
  compile_prescan_funcs(c, program);
  if (c->failed) {
    bcode_destroy(&bc);
    c->bc = NULL;
    return NULL;
  }

  /* 产物布局（先定义类型，然后构造函数对象，最后放置函数体）：
       1. 类型提升区（hoist）— 运行时先构造并登记全部程序类型
       2. 函数对象提升区 — 每函数 LOAD_TYPE <sig_id> + PUSH_FUNCTION +
          BIND_FUNC <fid> + [SET_FUNC_NAME] + 捕获槽占位（函数体入口 pc
          未知，PUSH_FUNCTION 先写占位，函数体区编译后回填）。构造的是
          "基底"对象——绑定地址（id + entry_pc + 名字）；全局函数无捕获
          （sema 拒绝），基底即最终实例，局部函数/字面量定义点按基底
          MAKE_FUNCTION 实例化新实例。
       2.5 全局函数名绑定 — 每全局函数 LOAD_FUNCTION <fid> + DEFINE name
          （与顶层 typedef 绑定同构；局部函数在定义点 DEFINE 后 STORE
          重定向到新实例，字面量无绑定）
       3. HALT — 拦截顺序执行，函数体区不落入
       4. 函数体区 — 各函数体以 RET 结尾，仅经 PUSH_FUNCTION 记录的
          body 入口在被调用时进入
     无 JMP 守卫：hoist 区即产物开头，顺序执行即达注册段。 */

  /* 1. 类型提升区：**两遍扫描**（compile_hoist.c）——pass 1 声明所有
     类型（开放对象 DEFINE_TYPE <id> 登记，不设字段），pass 2 定义所有
     类型（LOAD_TYPE 拉回 + 设字段 + SEAL，依赖后序）。当前类型图是 DAG
     （数组 elem / 限定符 sub 无环）；若未来引入指针/自引用类型（成环），
     两遍模型天然支持向前引用（pass 1 开放对象已登记，字段构造 LOAD_TYPE
     拿到开放对象，SEAL 后再重绑）。 */
  compile_hoist_declare(c);
  if (c->failed) {
    bcode_destroy(&bc);
    c->bc = NULL;
    return NULL;
  }

  /* 1.5 顶层 typedef 名字绑定（类型声明与定义之间 → 类型定义自动提升）：
     全局 type 定义名字绑定先行。rhs 折叠为 AST_TYPE_REF（LOAD_TYPE）或
     内建 AST_IDENT（PUSH）压 type value → PUSH_UNDEFINED 作 spec 占位 →
     DEFINE 绑定 type value 到 scope（compile_stmt 与局部同构，栈深归零）。
     pass 1 后类型对象已可 LOAD_TYPE 拉回（开放/密封皆可），名字绑定先行，
     后续函数签名/变量类型槽位引用名字时类型已可查；pass 2 密封后名字解析
     到最终类型（DEFINE 只存引用，不依赖密封）。 */
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    if (f->kind != AST_TYPE_DEF) continue;
    compile_stmt(c, f);
    if (c->failed) break;
  }
  if (c->failed) {
    bcode_destroy(&bc);
    c->bc = NULL;
    return NULL;
  }

  compile_hoist_define(c);
  if (c->failed) {
    bcode_destroy(&bc);
    c->bc = NULL;
    return NULL;
  }

  /* 2. 函数对象提升区：构造全部程序函数对象（fid 序，与预扫描一致）。
     签名类型已密封（hoist pass 2 完成），LOAD_TYPE <sig_id> 直接拉取。 */
  size_t nfuncs = vec_len(c->funcs_all);
  size_t *body_slots = NULL;
  if (nfuncs > 0) {
    body_slots = (size_t *)allocator_new_ex(
        c->alloc, "size_t", sizeof(size_t), NULL, NULL, NULL, nfuncs);
    if (!body_slots) panic("compiler: out of memory allocating body slots");
  }
  for (size_t i = 0; i < nfuncs; i++) {
    ast_func_def_t *fn = (ast_func_def_t *)vec_get(c->funcs_all, i);
    body_slots[i] = compile_func_reg_hoist(c, fn);
    if (c->failed) break;
  }
  if (c->failed) {
    if (body_slots) allocator_free(c->alloc, (void **)&body_slots);
    bcode_destroy(&bc);
    c->bc = NULL;
    return NULL;
  }

  /* 2.5 全局函数名绑定：LOAD_FUNCTION <fid> + DEFINE name（全局作用域）。
     仅顶层函数（prog->funcs 中的 AST_FUNC_DEF）——局部函数在定义点
     compile_block_body 绑定（提升语义），函数字面量无作用域绑定。 */
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    if (f->kind != AST_FUNC_DEF) continue;
    ast_func_def_t *fn = (ast_func_def_t *)f;
    if (fn->is_comptime) continue;
    compile_func_bind(c, fn);
    if (c->failed) break;
  }
  if (c->failed) {
    if (body_slots) allocator_free(c->alloc, (void **)&body_slots);
    bcode_destroy(&bc);
    c->bc = NULL;
    return NULL;
  }

  /* 2.6 全局变量定义：sema 已折叠 init 为字面量/AST_FUNC_REF（可编译期
     折叠契约），按声明序发射 [value, type-spec] DEFINE name（compile_stmt
     AST_VAR_DEF 分支，与全局函数绑定 compile_func_bind 协议同构）。
     运行时此处 current_scope = root_scope（模块层），DEFINE 落到 root_scope
     ——函数调用经 root_scope 查找（func_vcall 接线 closure_scope->parent =
     root_scope）读取到全局变量。仅普通全局 var（comptime var 已由
     pass_globals 摘除，防御分支跳过）。 */
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    if (f->kind != AST_VAR_DEF) continue;
    if (((ast_var_def_t *)f)->is_comptime) continue; /* 防御：sema 已摘除 */
    compile_stmt(c, f);
    if (c->failed) break;
  }
  if (c->failed) {
    if (body_slots) allocator_free(c->alloc, (void **)&body_slots);
    bcode_destroy(&bc);
    c->bc = NULL;
    return NULL;
  }

  bcode_write_op(bc, BCODE_HALT);

  /* 3. 函数体区（产物最后）：编译各函数体（fid 序，与提升区一致），回填
     函数对象提升区 PUSH_FUNCTION 的 body 入口 pc。局部函数/函数字面量与
     全局函数统一——预扫描已全部收集进 funcs_all。 */
  for (size_t i = 0; i < nfuncs; i++) {
    ast_func_def_t *fn = (ast_func_def_t *)vec_get(c->funcs_all, i);
    size_t body = compile_func_body(c, fn);
    bcode_patch_u32(bc, body_slots[i], (uint32_t)body);
    if (c->failed) break;
  }
  if (body_slots) allocator_free(c->alloc, (void **)&body_slots);
  if (c->failed) {
    bcode_destroy(&bc);
    c->bc = NULL;
    return NULL;
  }

  c->bc = NULL; /* 产物移交调用方 */
  return bc;
}

/* ===========================================================================
 * 生命周期
 * =========================================================================== */

compiler_t *compiler_new(allocator_t *alloc, vm_t *vm, diag_buf_t *diag,
                         vec_t *tokens, sema_scope_t *global_scope,
                         vec_t *sema_types) {
  if (!alloc || !vm || !diag) return NULL;
  compiler_t *c = (compiler_t *)allocator_new(alloc, &g_compiler_class, 1);
  if (!c) panic("compiler: out of memory allocating compiler");
  memset(c, 0, sizeof(compiler_t));
  c->alloc         = alloc;
  c->vm            = vm;
  c->diag          = diag;
  c->tokens        = tokens;
  c->global_scope  = global_scope;
  c->current_scope = global_scope;
  c->sema_types    = sema_types;
  c->loop_stack    = NULL;
  c->failed        = false;
  c->funcs_all     = vec_new(alloc, false);
  /* 签名类型 id 由 sema 分配（sema_type_register 登记进 sema->types，
     id = PROGRAM_BASE + index，已写入 sig->id）；type_id_next 仅作防御
     分支（compile_type.c AST_ARRAY 未替换场景临时分配 id）。 */
  c->type_id_next  = TYPE_ID_PROGRAM_BASE +
                     (uint32_t)(sema_types ? vec_len(sema_types) : 0);
  return c;
}

void compiler_destroy(compiler_t **pc) {
  if (!pc || !*pc) return;
  compiler_t *c = *pc;
  /* 释放残留 patch 节点（编译中途失败时可能有未回填标签） */
  while (c->loop_stack) c->loop_stack = c->loop_stack->next; /* 仅断链 */
  if (c->funcs_all) vec_free(c->alloc, &c->funcs_all);
  allocator_free(c->alloc, (void **)pc);
}
