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

  /* 产物布局（先定义类型，然后定义函数，最后放置函数体）：
       1. 类型提升区（hoist）— 运行时先构造并登记全部程序类型
       2. 函数注册段 — 签名构造 + PUSH_FUNCTION + DEFINE 名字（函数体入口
          pc 未知，PUSH_FUNCTION 先写占位，函数体区编译后回填）
       3. HALT — 拦截顺序执行，函数体区不落入
       4. 函数体区 — 各函数体以 RET 结尾，仅经 PUSH_FUNCTION 记录的
          body 入口在被调用时进入
     无 JMP 守卫：hoist 区即产物开头，顺序执行即达注册段。 */

  /* 1. 类型提升区：依赖后序递归构造，当前类型图是 DAG（数组 elem /
     限定符 sub 无环）；若未来引入指针/自引用类型（成环），须改为拓扑
     排序或两阶段构造（开放对象先 BIND、密封后再重绑），见 compile_hoist.c
     头部注释。 */
  compile_hoist(c);
  if (c->failed) {
    bcode_destroy(&bc);
    c->bc = NULL;
    return NULL;
  }

  /* 2. 函数注册段 */
  size_t nfuncs = 0;
  size_t body_slots[128];
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    if (f->kind == AST_TYPE_DEF) {
      /* 全局 type 定义进入注册段（HALT 前顺序执行）：rhs 折叠为
         AST_TYPE_REF（LOAD_TYPE）或内建 AST_IDENT（PUSH）压 type value →
         PUSH_UNDEFINED 作 spec 占位 → DEFINE 绑定 type value 到 scope。
         compile_stmt 与局部同构（栈深归零，无 body 回填）。 */
      compile_stmt(c, f);
      if (c->failed) break;
      continue;
    }
    if (f->kind != AST_FUNC_DEF) continue;
    ast_func_def_t *fn = (ast_func_def_t *)f;
    if (fn->is_comptime) continue;
    if (nfuncs < sizeof(body_slots) / sizeof(body_slots[0])) {
      body_slots[nfuncs] = compile_func_reg(c, fn);
      nfuncs++;
    }
    if (c->failed) break;
  }
  if (c->failed) {
    bcode_destroy(&bc);
    c->bc = NULL;
    return NULL;
  }

  bcode_write_op(bc, BCODE_HALT);

  /* 3. 函数体区（产物最后）：编译各函数体，回填注册段 PUSH_FUNCTION 的
     body 入口 pc。 */
  size_t fi = 0;
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    if (f->kind != AST_FUNC_DEF) continue;
    ast_func_def_t *fn = (ast_func_def_t *)f;
    if (fn->is_comptime) continue;
    size_t body = compile_func_body(c, fn);
    if (fi < sizeof(body_slots) / sizeof(body_slots[0])) {
      bcode_patch_u32(bc, body_slots[fi], (uint32_t)body);
      fi++;
    }
    if (c->failed) break;
  }
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
  c->func_id_next  = FUNC_ID_PROGRAM_BASE;
  /* 签名类型 id：sema_types 已占 [PROGRAM_BASE, PROGRAM_BASE+n)，签名类型
     （func type）从区间末尾起分配——与 sema 类型共占 types_by_id 类型 id
     表，但由 compiler 侧分配（sema 不登记 func 类型）。函数签名类型 id 与
     函数 id（func_id_next / functions_by_id）属不同表，严格分离。 */
  c->type_id_next  = TYPE_ID_PROGRAM_BASE +
                     (uint32_t)(sema_types ? vec_len(sema_types) : 0);
  return c;
}

void compiler_destroy(compiler_t **pc) {
  if (!pc || !*pc) return;
  compiler_t *c = *pc;
  /* 释放残留 patch 节点（编译中途失败时可能有未回填标签） */
  while (c->loop_stack) c->loop_stack = c->loop_stack->next; /* 仅断链 */
  allocator_free(c->alloc, (void **)pc);
}
