#include "sema/sema.h"
#include "parser/ast_block.h"
#include "parser/ast_for.h"
#include "parser/ast_func_def.h"
#include "parser/ast_if.h"
#include "parser/ast_return.h"
#include "parser/ast_var_def.h"
#include "parser/ast_while.h"

/* ===========================================================================
 * Pass 3a：作用域树构建 + 控制流分析
 *
 * 遍历函数体，按词法块结构建树。只注册符号（名字 + 声明类型），
 * 不做类型检查。推断类型的变量 type=NULL，留给 Pass 3b 填充。
 * 子作用域按出现顺序追加（Pass 3b 每层独立索引按序取用，严格对应）。
 *
 * 建树同时做纯结构性的返回路径完整性分析（build_result_t.definitely_returns）：
 * 与类型无关，因此不需要等 Pass 3b 的 shadow 运行。非 void 函数所有路径
 * 必须 return 在此阶段即可检查。
 * =========================================================================== */

typedef struct build_result {
  bool definitely_returns; /* 该块保证返回（所有路径都 return） */
} build_result_t;

static build_result_t build_block(sema_t *sema, ast_block_t *block,
                                  sema_scope_t *scope);
static build_result_t build_func_if(sema_t *sema, ast_if_t *it,
                                    sema_scope_t *scope);

static void build_func(sema_t *sema, sema_func_t *sf) {
  ast_func_def_t *fn = (ast_func_def_t *)sf->def;

  sema_scope_t *fscope =
      sema_scope_new(sema->vm->alloc, SEMA_SCOPE_FUNCTION, sema->global_scope);
  sema_scope_add_child(sema->global_scope, fscope);
  sf->scope = fscope;

  /* 注册参数（已解析类型，运行时值在 Pass 3b 进入函数时定义到 VM scope） */
  for (ast_node_t *p = fn->params; p; p = p->next) {
    ast_var_def_t *vd = (ast_var_def_t *)p;
    sema_symbol_t init = {
        .type = sema_resolve_type_slot(sema, &vd->type_expr)};
    if (!sema_scope_define(fscope, vd->name, &init)) {
      diag_error(sema->diag, sema_loc(sema, p),
                 "duplicate parameter '%.*s'", (int)vd->name.len,
                 vd->name.ptr);
    }
  }

  /* 函数体 block 直接用 fscope（不再嵌套一层） */
  build_result_t r = build_block(sema, (ast_block_t *)fn->body, fscope);

  /* 控制流分析：非 void 函数所有路径必须 return（纯结构，不依赖类型） */
  const type_t *rt = fn->return_expr
                         ? sema_resolve_type_slot(sema, &fn->return_expr)
                         : NULL;
  if (rt && rt->kind != TYPE_KIND_VOID && !r.definitely_returns) {
    diag_error(sema->diag, sema_loc(sema, &fn->base),
               "function '%.*s' must return a value on all paths",
               (int)fn->name.len, fn->name.ptr);
  }
}

static build_result_t build_block(sema_t *sema, ast_block_t *block,
                                  sema_scope_t *scope) {
  build_result_t r = {0};
  for (ast_node_t *s = block->stmts; s; s = s->next) {
    if (r.definitely_returns) {
      /* 严格检查：return 后不可达语句报错。
         不建作用域、不注册符号——Pass 3b 同步跳过（索引保持对齐）。 */
      diag_error(sema->diag, sema_loc(sema, s), "unreachable statement");
      continue;
    }
    switch (s->kind) {
      case AST_VAR_DEF: {
        ast_var_def_t *vd = (ast_var_def_t *)s;
        const type_t *vt = NULL;
        if (vd->type_expr) {
          vt = sema_resolve_type_slot(sema, &vd->type_expr);
          if (!vt) {
            diag_error(sema->diag, sema_loc(sema, s), "unknown type");
          }
        }
        sema_symbol_t init = {.type = vt};
        if (!sema_scope_define(scope, vd->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate variable '%.*s'", (int)vd->name.len,
                     vd->name.ptr);
        }
        break;
      }
      case AST_BLOCK: {
        sema_scope_t *child =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
        sema_scope_add_child(scope, child);
        build_result_t cr = build_block(sema, (ast_block_t *)s, child);
        if (cr.definitely_returns) r.definitely_returns = true;
        break;
      }
      case AST_IF: {
        ast_if_t *it = (ast_if_t *)s;
        sema_scope_t *then_scope =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
        sema_scope_add_child(scope, then_scope);
        build_result_t tr = build_block(sema, (ast_block_t *)it->then_body,
                                        then_scope);
        build_result_t er = {0};
        if (it->else_body) {
          if (it->else_body->kind == AST_IF) {
            /* else-if 链：同层递归（子作用域顺序与 3b 一致） */
            er = build_func_if(sema, (ast_if_t *)it->else_body, scope);
          } else {
            sema_scope_t *else_scope =
                sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
            sema_scope_add_child(scope, else_scope);
            er = build_block(sema, (ast_block_t *)it->else_body, else_scope);
          }
        }
        if (tr.definitely_returns && er.definitely_returns)
          r.definitely_returns = true;
        break;
      }
      case AST_WHILE: {
        ast_while_t *wl = (ast_while_t *)s;
        sema_scope_t *body_scope =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
        sema_scope_add_child(scope, body_scope);
        sema->loop_depth++;
        build_block(sema, (ast_block_t *)wl->body, body_scope);
        sema->loop_depth--;
        break; /* 循环体可能不执行，不贡献 definitely_returns */
      }
      case AST_FOR: {
        ast_for_t *fr = (ast_for_t *)s;
        sema_scope_t *for_scope =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_FOR, scope);
        sema_scope_add_child(scope, for_scope);
        /* init 变量注册到 for scope */
        if (fr->init && fr->init->kind == AST_VAR_DEF) {
          ast_var_def_t *vd = (ast_var_def_t *)fr->init;
          const type_t *vt = NULL;
          if (vd->type_expr) {
            vt = sema_resolve_type_slot(sema, &vd->type_expr);
            if (!vt) {
              diag_error(sema->diag, sema_loc(sema, fr->init),
                         "unknown type");
            }
          }
          sema_symbol_t init_sym = {.type = vt};
          if (!sema_scope_define(for_scope, vd->name, &init_sym)) {
            diag_error(sema->diag, sema_loc(sema, fr->init),
                       "duplicate variable '%.*s'", (int)vd->name.len,
                       vd->name.ptr);
          }
        }
        /* body 是 for scope 的子 scope */
        sema_scope_t *body_scope =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, for_scope);
        sema_scope_add_child(for_scope, body_scope);
        sema->loop_depth++;
        build_block(sema, (ast_block_t *)fr->body, body_scope);
        sema->loop_depth--;
        break; /* 循环体可能不执行，不贡献 definitely_returns */
      }
      case AST_RETURN:
        r.definitely_returns = true;
        break; /* 后续语句在循环顶部报 unreachable */
      case AST_BREAK:
      case AST_CONTINUE:
        if (sema->loop_depth == 0) {
          diag_error(sema->diag, sema_loc(sema, s), "'%.*s' outside loop",
                     (int)(s->kind == AST_BREAK ? 5 : 8),
                     s->kind == AST_BREAK ? "break" : "continue");
        }
        break;
      default:
        break; /* 其他语句不创建作用域 */
    }
  }
  return r;
}

/* if 语句的作用域构建入口（含 else-if 同层递归） */
static build_result_t build_func_if(sema_t *sema, ast_if_t *it,
                                    sema_scope_t *scope) {
  sema_scope_t *then_scope =
      sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
  sema_scope_add_child(scope, then_scope);
  build_result_t tr =
      build_block(sema, (ast_block_t *)it->then_body, then_scope);
  build_result_t er = {0};
  if (it->else_body) {
    if (it->else_body->kind == AST_IF) {
      er = build_func_if(sema, (ast_if_t *)it->else_body, scope);
    } else {
      sema_scope_t *else_scope =
          sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
      sema_scope_add_child(scope, else_scope);
      er = build_block(sema, (ast_block_t *)it->else_body, else_scope);
    }
  }
  return (build_result_t){.definitely_returns =
                              tr.definitely_returns && er.definitely_returns};
}

void sema_build_scope_tree(sema_t *sema) {
  size_t n = vec_len(sema->funcs);
  for (size_t i = 0; i < n; i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    ast_func_def_t *fn = (ast_func_def_t *)sf->def;
    /* comptime func：不建作用域树、不 shadow walk。函数体只在调用点
       （sema_eval_comptime_call → ctfe）解释求值，参数是编译期实值，
       静态 walk 参数为 shadow 无法求值；未调用则不检查（同 C++ template）。 */
    if (fn->is_comptime) continue;
    build_func(sema, sf);
  }
}
