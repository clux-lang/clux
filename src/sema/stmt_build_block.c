#include "sema/comptime.h"
#include "sema/sema.h"
#include "vm/type_ownership.h"
#include "core/panic.h"
#include "vm/type_ptr.h"
#include "vm/type_struct.h"
#include "vm/type_error.h"
#include "parser/ast_alignof.h"
#include "parser/ast_array.h"
#include "parser/ast_block.h"
#include "parser/ast_const.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_for.h"
#include "parser/ast_func_def.h"
#include "vm/type_tuple.h"
#include "vm/type_array.h"
#include "parser/ast_construct.h"
#include "parser/parser.h"
#include "parser/ast_assign.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_index.h"
#include "parser/ast_foreach.h"
#include "parser/ast_ident.h"
#include "parser/ast_if.h"
#include "ctfe/ctfe.h"
#include "parser/ast_comptime_if.h"
#include "parser/ast_option.h"
#include "parser/ast_return.h"
#include "parser/ast_sizeof.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_typeof.h"
#include "parser/ast_union_def.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_switch.h"
#include "parser/ast_type_def.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_var_def.h"
#include "parser/ast_volatile.h"
#include "parser/ast_walk.h"
#include "parser/ast_while.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_defer.h"
#include "parser/ast_call.h"
#include "parser/ast_expr_stmt.h"
#include "vm/type_func.h"

#include <stdio.h>
#include <string.h>

#include "sema_build_internal.h"

static build_result_t build_func_if(sema_t *sema, ast_if_t *it, sema_scope_t *scope);

build_result_t build_block(sema_t *sema, ast_block_t *block,
                                  sema_scope_t *scope) {
  /* **comptime if：编译期裁剪**（必须先于 desugar_defers / 符号注册）：
     把 `comptime if` 节点替换成存活分支的语句链——丢弃分支从此不被任何
     后续阶段看到（只有解析期语法检查），存活分支的语句成为外层块的一部分
     （大括号不是作用域）。 */
  comptime_if_splice(sema, block, scope);

  /* **comptime foreach：编译期展开**（同样必须先于 desugar_defers / 符号注册，
     作用域树要按展开后的语句建）。容器类型由 3a 已解析的符号/声明推出。 */
  if (scope && !scope->block_ast) scope->block_ast = (ast_node_t *)block;
  comptime_foreach_splice(sema, block, scope);

  /* defer 语法糖改写：在正式遍历前把 AST_DEFER 替换为 AST_FUNC_DEF +
     退出点 call，后续遍历与 Pass 3b 完全不感知 defer。 */
  desugar_defers(sema, block, scope);

  build_result_t r = {0};
  for (ast_node_t *s = block->stmts; s; s = s->next) {
    if (r.definitely_returns) {
      /* 编译器生成的 defer call 语句（tok_begin==0）跳过 unreachable 检查——
         它们是作用域退出点清理代码，在 return 之后仍需存在（正常落到块尾
         路径执行），不是用户代码。Pass 3b 同步跳过。 */
      if (s->tok_begin == 0) continue;
      /* 严格检查：return 后不可达语句报错。
         不建作用域、不注册符号——Pass 3b 同步跳过（索引保持对齐）。 */
      diag_error(sema->diag, sema_loc(sema, s), "unreachable statement");
      continue;
    }
    switch (s->kind) {
      case AST_VAR_DEF: {
        ast_var_def_t *vd = (ast_var_def_t *)s;
        const type_t *vt = NULL;
        /* 所有权须在 sema_resolve_type_slot 之前提取——该函数会把
           AST_PTR/AST_SLICE_TYPE 折叠为 AST_TYPE_REF，丢失 ownership token。 */
        if (vd->type_expr) {
          /* 类型槽位解析：若名字被待绑定局部 type 遮蔽（type_shadowed_by_local_def），
             跳过 vm scope 解析（会错误落到外层全局），sym->type 留 NULL 由 3b
             shadow_var_def 定义点后兜底；否则按常规解析，失败也留 NULL
             （3b 补报 "unknown type"）。

             另两类同样推迟到 3b：
             - 槽位含 sizeof/alignof/typeof（type_has_ct_op，依赖 3b shadow 状态）；
             - 槽位含**非字面量数组边界**且作用域里有尚未求值的 comptime var
               （`comptime var C = 3; var a: [C]i32` —— C 的常量在 3b 定义点求值，
               3a 解析会因值未就绪失败并记录硬诊断，被 sema 的快速失败闸门挡掉
               3b，使本地 comptime var 完全不可用）。 */
          if (!type_shadowed_by_local_def(sema, scope, vd->type_expr) &&
              !type_has_ct_op(vd->type_expr) &&
              !(type_has_nonliteral_array_bound(vd->type_expr) &&
                scope_has_unevaluated_comptime(scope))) {
            vt = sema_resolve_type_slot(sema, &vd->type_expr);
          }
        }
        sema_symbol_t init = {.kind = SEMA_SYM_VAR, .type = vt,
                              .is_comptime = vd->is_comptime,};
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
        sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = (ast_node_t *)td};
        if (!sema_scope_define(scope, td->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate name '%.*s'", (int)td->name.len,
                     td->name.ptr);
        }
        break;
      }
      case AST_ENUM_DEF: {
        /* 局部 enum 定义：注册符号（暂不激活，Pass 3b walk_block 入口提升
           调 sema_eval_enum_def 求值后激活）。与局部 type 定义同构——ast
           指向定义节点；sema_eval_enum_def 经 sema_scope_find_local 取符号
           后写 type/激活。 */
        ast_enum_def_t *ed = (ast_enum_def_t *)s;
        sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = (ast_node_t *)ed};
        if (!sema_scope_define(scope, ed->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate name '%.*s'", (int)ed->name.len,
                     ed->name.ptr);
        }
        break;
      }
      case AST_STRUCT_DEF: {
        /* 局部 struct 定义：注册符号（暂不激活，Pass 3b walk_block 入口提升
           调 sema_eval_struct_def 求值后激活）。与局部 enum/type 定义同构。 */
        ast_struct_def_t *sd = (ast_struct_def_t *)s;
        sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = (ast_node_t *)sd};
        if (!sema_scope_define(scope, sd->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate name '%.*s'", (int)sd->name.len,
                     sd->name.ptr);
        }
        break;
      }
      case AST_UNION_DEF: {
        /* 局部 union 定义：注册符号（暂不激活，Pass 3b walk_block 入口提升
           调 sema_eval_union_def 求值后激活）。与局部 struct/enum/type 定义
           同构（struct 同构平铺字段语法）。 */
        ast_union_def_t *ud = (ast_union_def_t *)s;
        sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = (ast_node_t *)ud};
        if (!sema_scope_define(scope, ud->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate name '%.*s'", (int)ud->name.len,
                     ud->name.ptr);
        }
        break;
      }
      case AST_CUNION_DEF: {
        /* 局部 cunion 定义：注册符号（暂不激活，Pass 3b walk_block 入口提升
           调 sema_eval_cunion_def 求值后激活）。与局部 union/struct/enum/type
           定义同构（struct 同构平铺字段语法）。 */
        ast_cunion_def_t *cd = (ast_cunion_def_t *)s;
        sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = (ast_node_t *)cd};
        if (!sema_scope_define(scope, cd->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate name '%.*s'", (int)cd->name.len,
                     cd->name.ptr);
        }
        break;
      }
      case AST_FUNC_DEF:
        /* 局部函数定义：注册符号 + 登记队列 + 建 fscope 树（提升语义，
           整个块内可见）。定义点不摘除（进入字节码，运行时 DEFINE 绑定）。
           3b walk_block 入口提升签名、主循环跳过（消费 fscope 子作用域）。 */
        build_local_func(sema, s, scope);
        break;
      /* AST_DEFER 已在 desugar_defers 中改写为 AST_FUNC_DEF + 退出点 call，
         到达此处时已不存在 AST_DEFER 节点。 */
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
      case AST_SWITCH: {
        /* switch 分支体各建子作用域（声明序，与 3b walk 消费序一致）。
           default 分支最后消费。无 fallthrough，break/continue 不涉及
           switch（loop_depth 不变）——case 体自然结束。 */
        ast_switch_t *sw = (ast_switch_t *)s;
        bool all_return = true;
        for (ast_node_t *cs = sw->cases; cs; cs = cs->next) {
          ast_switch_case_t *sc = (ast_switch_case_t *)cs;
          sema_scope_t *case_scope =
              sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
          sema_scope_add_child(scope, case_scope);
          build_result_t br = build_block(sema, (ast_block_t *)sc->body,
                                          case_scope);
          if (!br.definitely_returns) all_return = false;
        }
        if (sw->default_body) {
          sema_scope_t *def_scope =
              sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
          sema_scope_add_child(scope, def_scope);
          build_result_t dr = build_block(sema, (ast_block_t *)sw->default_body,
                                          def_scope);
          if (!dr.definitely_returns) all_return = false;
        }
        /* 全部分支返回才贡献 definitely_returns（无 default 时可能不匹配
           任何分支直接穿透，不贡献） */
        if (sw->default_body && all_return)
          r.definitely_returns = true;
        break;
      }
      case AST_WHILE: {
        ast_while_t *wl = (ast_while_t *)s;
        sema_scope_t *body_scope =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
        sema_scope_add_child(scope, body_scope);
        sema->fn.loop_depth++;
        build_block(sema, (ast_block_t *)wl->body, body_scope);
        sema->fn.loop_depth--;
        break; /* 循环体可能不执行，不贡献 definitely_returns */
      }
      case AST_DOWHILE: {
        /* do-while 与 while 同构：body 建子作用域、loop_depth 包裹。
           循环体至少执行一次，但条件不满足时仍可能不返回，保守不贡献
           definitely_returns（与 while 一致）。 */
        ast_dowhile_t *dw = (ast_dowhile_t *)s;
        sema_scope_t *body_scope =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
        sema_scope_add_child(scope, body_scope);
        sema->fn.loop_depth++;
        build_block(sema, (ast_block_t *)dw->body, body_scope);
        sema->fn.loop_depth--;
        break;
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
            if (!type_shadowed_by_local_def(sema, scope, vd->type_expr) &&
                !type_has_ct_op(vd->type_expr)) {
              vt = sema_resolve_type_slot(sema, &vd->type_expr);
            }
          }
          sema_symbol_t init_sym = {.kind = SEMA_SYM_VAR, .type = vt,};
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
        sema->fn.loop_depth++;
        build_block(sema, (ast_block_t *)fr->body, body_scope);
        sema->fn.loop_depth--;
        break; /* 循环体可能不执行，不贡献 definitely_returns */
      }
      case AST_RETURN:
        r.definitely_returns = true;
        break; /* 后续语句在循环顶部报 unreachable */
      case AST_BREAK:
      case AST_CONTINUE:
        if (sema->fn.loop_depth == 0) {
          diag_error(sema->diag, sema_loc(sema, s), "'%.*s' outside loop",
                     (int)(s->kind == AST_BREAK ? 5 : 8),
                     s->kind == AST_BREAK ? "break" : "continue");
        }
        break;
      case AST_FOREACH:
        /* 前脱糖（src/desugar/desugar.c）必须把 foreach 展开成索引 for 循环：
           走到语义层说明前端漏展开。**不能静默跳过**——那会整条循环消失、
           程序"编译通过但少做了事"。 */
        diag_error(sema->diag, sema_loc(sema, s),
                   "internal: 'foreach' reached semantic analysis unexpanded "
                   "(front-end desugaring bug)");
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
