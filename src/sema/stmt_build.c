#include "sema/sema.h"
#include "parser/ast_block.h"
#include "parser/ast_for.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ident.h"
#include "parser/ast_if.h"
#include "parser/ast_return.h"
#include "parser/ast_type_def.h"
#include "parser/ast_type_ref.h"
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

/* 类型名是否被"待绑定的局部符号"遮蔽。
 *
 * 3a 阶段局部符号只注册 sema 符号表，尚未绑定 vm scope（type def 在 3b
 * 定义点求值后才绑定；var/参数在 3b 定义点才入 vm scope）。此时若 var
 * 类型槽位经 type_lookup（vm scope）解析，会错误落到外层同名全局 type
 * （或内建）上。沿 sema scope 链查找：命中同名符号（type def 或
 * var/参数）→ 遮蔽成立，槽位推迟到 3b 解析（定义点绑定后 shadow_var_def
 * 兜底）。顺序敏感由 3b 兜底天然承担：遮蔽符号定义在槽位前 → vm scope
 * 已遮蔽 → 解析失败报 "is a variable"；定义在槽位后 → vm scope 未遮蔽
 * → 解析到外层全局 type（顺序敏感语义正确）。
 * 仅命名类型槽位（AST_IDENT / AST_TYPE_REF）可判；复合类型（AST_ARRAY
 * 等）不含裸名字，直接返回 false 走常规解析。 */
static bool type_shadowed_by_local_def(sema_t *sema, sema_scope_t *scope,
                                       ast_node_t *type_expr) {
  strslice_t name;
  if (type_expr->kind == AST_IDENT) {
    name = ((ast_ident_t *)type_expr)->name;
  } else if (type_expr->kind == AST_TYPE_REF) {
    name = ((ast_type_ref_t *)type_expr)->name;
  } else {
    return false;
  }
  (void)sema;
  for (sema_scope_t *s = scope; s; s = s->parent) {
    sema_symbol_t *sym = sema_scope_find_local(s, name);
    if (!sym) continue;
    return true; /* 最近同名符号：type def / var / 参数均遮蔽 */
  }
  return false;
}

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
          /* 类型槽位解析：若名字被待绑定局部 type 遮蔽（type_shadowed_by_local_def），
             跳过 vm scope 解析（会错误落到外层全局），sym->type 留 NULL 由 3b
             shadow_var_def 定义点后兜底；否则按常规解析，失败也留 NULL
             （3b 补报 "unknown type"）。 */
          if (!type_shadowed_by_local_def(sema, scope, vd->type_expr)) {
            vt = sema_resolve_type_slot(sema, &vd->type_expr);
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
      case AST_TYPE_DEF: {
        /* 局部 type 定义：注册符号（暂不激活，Pass 3b 定义点求值后激活）。
           类型名经 type_lookup（vm scope）解析，无需 type 字段；ast 指向
           定义节点（3a 判别"待绑定局部 type"遮蔽场景用）。 */
        ast_type_def_t *td = (ast_type_def_t *)s;
        sema_symbol_t init = {.ast = (ast_node_t *)td};
        if (!sema_scope_define(scope, td->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate name '%.*s'", (int)td->name.len,
                     td->name.ptr);
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
            /* 同 AST_VAR_DEF：局部 type 遮蔽则推迟，否则常规解析（失败留
               NULL 由 3b 兜底） */
            if (!type_shadowed_by_local_def(sema, scope, vd->type_expr)) {
              vt = sema_resolve_type_slot(sema, &vd->type_expr);
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
