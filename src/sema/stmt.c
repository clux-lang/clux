#include "sema/sema.h"
#include <stdio.h>
#include "parser/ast_array.h"
#include "parser/ast_walk.h"
#include "parser/ast_assign.h"
#include "parser/ast_walk.h"
#include "parser/ast_binary.h"
#include "parser/ast_walk.h"
#include "parser/ast_call.h"
#include "parser/ast_walk.h"
#include "parser/ast_const.h"
#include "parser/ast_walk.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"  /* ast_construct_field_t::value */
#include "parser/ast_walk.h"
#include "parser/ast_fill.h"
#include "parser/ast_walk.h"
#include "parser/ast_func_type.h"
#include "parser/ast_walk.h"
#include "parser/ast_ident.h"
#include "parser/ast_walk.h"
#include "parser/ast_nil.h"
#include "parser/ast_walk.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_addr.h"
#include "parser/ast_undef.h"
#include "parser/ast_var_def.h"
#include "parser/ast_index.h"
#include "parser/ast_walk.h"
#include "parser/ast_member.h"
#include "parser/ast_walk.h"
#include "parser/ast_block.h"
#include "parser/ast_walk.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_walk.h"
#include "parser/ast_for.h"
#include "parser/ast_walk.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ptr.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_walk.h"
#include "parser/ast_if.h"
#include "parser/ast_walk.h"
#include "parser/ast_return.h"
#include "parser/ast_walk.h"
#include "parser/ast_switch.h"
#include "parser/ast_walk.h"
#include "parser/ast_ternary.h"
#include "parser/ast_walk.h"
#include "parser/ast_type_def.h"
#include "parser/ast_walk.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_walk.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_walk.h"
#include "parser/ast_union_def.h"
#include "parser/ast_walk.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_walk.h"
#include "parser/ast_unary.h"
#include "parser/ast_walk.h"
#include "parser/ast_var_def.h"
#include "parser/ast_walk.h"
#include "parser/ast_volatile.h"
#include "parser/ast_walk.h"
#include "parser/ast_while.h"
#include "parser/ast_walk.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_walk.h"
#include "parser/lexer.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_addr.h"
#include "parser/ast_addr.h"
#include "parser/ast_walk.h"
#include "sema/comptime.h"
#include "sema/struct_compose.h" /* 嵌入继承的成员名字查找 */
#include "vm/type_array.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/type_cunion.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "parser/ast_deref.h"
#include "parser/ast_scope_ref.h" /* M6 FFI：extern var 赋值目标的 AST 标志 */
#include "parser/ast_walk.h"
#include "core/ffi_abi.h"          /* M6 FFI：ffi_data_kind_of（数据符号类型码） */
#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * Pass 3b：Shadow VM 运行
 *
 * 按预建作用域树严格对应遍历 AST。每个块用独立局部索引遍历自己的
 * children：build 阶段子作用域按出现顺序 add_child，walk 必须逐层
 * 独立取用（共享索引会在嵌套时污染外层，导致后续作用域错位）。
 * =========================================================================== */

#include "sema_stmt_internal.h"

block_result_t walk_block(sema_t *sema, ast_node_t *block, sema_scope_t *scope, size_t *idx);

/* ---- 语句分派 ---- */

/* 索引语义：idx 是"当前 scope"的 children 迭代器。一个块内的语句按序
   消费当前 scope 的子节点（sema_scope_child(scope, (*idx)++)），进入
   嵌套子 scope（BLOCK/if then/else/while body/for body）时用新局部
   迭代器遍历子 scope 的 children——与原版共享外层迭代器相比，嵌套消费
   不再污染浅层（第二个 for 的 init 变量因此找不到定义点）。 */

/* walk_stmt 的**包装**：维护 sema->cur_stmt（扁平化 clone 的切片路径要把
   生成的 prelude 语句挂到"当前语句"上）。保存/恢复保证嵌套语句（if/while
   体里的赋值等）结束后外层语句恢复正确。 */


/* ---- 提升遮蔽预检（局部 type 定义） ----
 *
 * 提升语义下 type RHS 在块入口求值（早于块内 var 绑定到 VM scope），
 * 若 RHS 引用"块内声明序靠前的 var 名"会错误落到外层同名 type/未定义。
 * 入口提升循环按声明序累积 prior_vars，此处递归检查 RHS 的 AST_IDENT
 * 引用（未折叠的名字，折叠产物 AST_TYPE_REF 不在此列）是否命中——
 * 命中即 var 完全遮罩（遮蔽平等），报错并跳过求值。 */

/* 类型 rhs 是否引用了 prior_vars 中的运行期变量名（返回命中的那个名字）。
   基于共享 AST 遍历（parser/ast_walk.h）：谓词只关心标识符名，遍历覆盖
   类型表达式与值表达式的全部子节点（此前手写版漏掉 ?T / *T / []T / 元组
   等子树，导致 `type T = ?[x]i32` 这类遮蔽要到 CTFE 才以另一种消息报出）。 */
typedef struct {
  const strslice_t *prior_vars;
  size_t            prior_n;
  const strslice_t *hit;
} uses_var_probe_t;

static block_result_t walk_stmt_impl(sema_t *sema, ast_node_t *stmt,
                                     sema_scope_t *scope, size_t *idx) {
  block_result_t r = {0};
  /* **sema 生成的销毁语句不再回走**（`is_generated`，见 gen_destroy_* /
     生成节点）：销毁序列的类型判定已由 type 对象完成，回走没有新信息；
     而回走复合生成形态会造成两类错误：
     - `AST_IF`/`AST_BLOCK` 会到 3a 建的 scope 树里消费子作用域，但生成
       节点没有对应作用域 → "scope tree desync"；
     - `AST_IS_TAG` 是 sema 内部直接形态节点，不是用户表达式，sema_expr
       未登记 → "unhandled expression node"。
     生成节点只由 compiler 消费（见 compile_stmt / compile_expr）。
     **注意**：defer 改写的 `__defer_N()` 调用语句同样是生成节点（tok_begin
     == 0），但**必须继续 walk**——它们要建 sema 符号/VM 绑定，CTFE 求值
     comptime 函数体时靠它解析到 `__defer_N`。故这里只按 `is_generated`
     跳过销毁语句，不按 tok_begin 一刀切。 */
  if (stmt->is_generated) return r;
  switch (stmt->kind) {
    case AST_VAR_DEF:
      shadow_var_def(sema, (ast_var_def_t *)stmt, scope);
      break;
    case AST_TYPE_DEF:
      /* 局部 type 定义：已在 walk_block 入口提升时求值绑定（作用域入口
         生效，前向引用安全）。此处保留为防御分支（未来 walk_if 单语句
         分支等若直接调用 walk_stmt 处理 type def，语义与本分支一致：
         rhs 求值 → 折叠 AST_TYPE_REF → 绑定 vm 当前作用域 → 激活符号）。
         定义点不摘除（进入字节码，运行时 DEFINE 绑定 type value）。 */
      sema_eval_type_def(sema, (ast_type_def_t *)stmt, scope);
      break;
    case AST_ASSIGN:
      shadow_assign(sema, (ast_assign_t *)stmt, scope);
      break;
    case AST_BLOCK: {
      sema_scope_t *child = sema_scope_child(scope, (*idx)++);
      vm_push_scope(sema->vm); /* VM scope 与 sema scope 树同构 */
      size_t sub = 0;
      r = walk_block(sema, stmt, child ? child : scope, &sub);
      vm_pop_scope(sema->vm);
      break;
    }
    case AST_IF:
      r = walk_if(sema, (ast_if_t *)stmt, scope, idx);
      break;
    case AST_SWITCH:
      r = walk_switch(sema, (ast_switch_t *)stmt, scope, idx);
      break;
    case AST_WHILE:
      r = walk_while(sema, (ast_while_t *)stmt, scope, idx);
      break;
    case AST_DOWHILE:
      r = walk_dowhile(sema, (ast_dowhile_t *)stmt, scope, idx);
      break;
    case AST_FOR:
      r = walk_for(sema, (ast_for_t *)stmt, scope, idx);
      break;
    case AST_RETURN:
      r = walk_return(sema, (ast_return_t *)stmt, scope);
      break;
    case AST_BREAK:
    case AST_CONTINUE:
      break;
    case AST_FUNC_DEF:
      /* 局部函数定义：入口提升已处理签名，主循环（walk_block）已跳过并
         消费 fscope 子作用域。防御分支（非块上下文直接调用时零操作）。
         defer 改写后的 __defer_N 也是 AST_FUNC_DEF，走同一路径。 */
      break;
    /* AST_DEFER 已在 Pass 3a desugar_defers 中改写为 AST_FUNC_DEF +
       退出点 call（AST_EXPR_STMT），到达此处时已不存在 AST_DEFER。 */
    case AST_EXPR_STMT: {
      ast_expr_stmt_t *es = (ast_expr_stmt_t *)stmt;
      bool noreturn = false;
      if (es->expr && es->expr->kind == AST_CALL) {
        ast_call_t *cl = (ast_call_t *)es->expr;
        ast_node_t *callee = cl->callee;
        strslice_t cname = {0};
        if (callee && callee->kind == AST_IDENT)
          cname = ((ast_ident_t *)callee)->name;
        else if (callee && callee->kind == AST_INDEX) {
          ast_node_t *obj = ((ast_index_t *)callee)->object;
          if (obj && obj->kind == AST_IDENT)
            cname = ((ast_ident_t *)obj)->name;
        }
        if (cname.ptr) {
          sema_symbol_t *cs = sema_lookup(scope, cname);
          noreturn = cs && cs->is_noreturn;
        }
      }
      value_t *v = sema_expr(sema, &es->expr, scope);
      if (!value_is_error(sema->vm, v) &&
          !value_is_type(v, TYPE_KIND_VOID)) {
        char tn[64];
        sema_type_name(value_type(v), tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, es->expr),
                   "expression result of type %s is unused; use '_ = expr' "
                   "to discard",
                   tn);
      }
      if (noreturn) r.definitely_returns = true;
      break;
    }
    case AST_FOREACH:
      /* 前脱糖（src/desugar/desugar.c）必须把 foreach 展开成索引 for 循环；
         到达这里说明前端漏展开。不静默跳过：漏一条循环会让程序"编译通过但
         少做了事"（Pass 3a 的 build 同款诊断在此前触发，这里防止单跑 3b）。 */
      diag_error(sema->diag, sema_loc(sema, stmt),
                 "internal: 'foreach' reached semantic analysis unexpanded "
                 "(front-end desugaring bug)");
      break;
    default:
      break;
  }
  return r;
}

/* 见上文包装声明：进入语句时设置 cur_stmt，离开时恢复（嵌套安全）。 */
block_result_t walk_stmt(sema_t *sema, ast_node_t *stmt,
                                sema_scope_t *scope, size_t *idx) {
  ast_node_t *saved = sema->cur_stmt;
  sema->cur_stmt = stmt;
  block_result_t r = walk_stmt_impl(sema, stmt, scope, idx);
  sema->cur_stmt = saved;
  return r;
}

static ast_walk_action_t probe_uses_var(ast_node_t *n, void *user) {
  uses_var_probe_t *p = (uses_var_probe_t *)user;
  /* typeof / sizeof / alignof 的操作数是**表达式**（变量名是合法的：`typeof(a)`
     就是取 a 的类型），不是类型名引用——不下钻，否则
     `var a: i32 = 1; type T = typeof(a);` 会被误报成 "'a' is a variable, not
     a type"。这三者的操作数若要引用类型，本身就写在类型槽位里，由其它检查覆盖。 */
  if (n->kind == AST_TYPEOF || n->kind == AST_SIZEOF ||
      n->kind == AST_ALIGNOF) {
    return AST_WALK_SKIP_CHILDREN;
  }
  if (n->kind != AST_IDENT) return AST_WALK_CONTINUE;
  strslice_t name = ((ast_ident_t *)n)->name;
  for (size_t i = 0; i < p->prior_n; i++) {
    if (p->prior_vars[i].len == name.len &&
        memcmp(p->prior_vars[i].ptr, name.ptr, name.len) == 0) {
      p->hit = &p->prior_vars[i];
      return AST_WALK_STOP;
    }
  }
  return AST_WALK_CONTINUE;
}

static const strslice_t *type_rhs_uses_var(const ast_node_t *e,
                                           const strslice_t *prior_vars,
                                           size_t prior_n) {
  if (!e) return NULL;
  uses_var_probe_t p = {prior_vars, prior_n, NULL};
  ast_walk_preorder((ast_node_t *)e, probe_uses_var, &p);
  return p.hit;
}

/* ---- 局部函数签名解析（3b 提升时） ---- */

/* 按定义 AST 节点反查 sema_func_t（局部函数提升时定位 fscope，补全参数
   符号 type）。线性扫描，函数数量少。 */
static sema_func_t *sema_func_find_by_def(sema_t *sema, ast_node_t *def) {
  size_t n = vec_len(sema->funcs);
  for (size_t i = 0; i < n; i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    if (sf->def == def) return sf;
  }
  return NULL;
}

/* 局部函数签名解析：与 pass2_types 同构（type_func_sig 建签名 →
   sema_type_register 登记 → fn->sig_id 记录），但类型槽位在定义点块作用域
   的 VM 链上解析（局部 type 已由入口提升按声明序绑定，可见）。
   参数符号 type 一并补全（3a 建树时局部类型槽位可能解析失败为 NULL，
   函数体 walk 前就绪——sema_walk_function 参数 shadow 用）。 */
static void resolve_local_func_sig(sema_t *sema, ast_func_def_t *fn,
                                   sema_scope_t *scope) {
  size_t nparams = sema_count_siblings(fn->params);
  const type_t **params = NULL;
  if (nparams > 0) {
    params = allocator_new_ex(sema->vm->alloc, "type_t*", sizeof(type_t *),
                              NULL, NULL, NULL, nparams);
    size_t j = 0;
    for (ast_node_t *p = fn->params; p; p = p->next, j++) {
      ast_var_def_t *vd = (ast_var_def_t *)p;
      const type_t *t = sema_resolve_type_slot(sema, &vd->type_expr);
      if (!t) {
        diag_error(sema->diag, sema_loc(sema, p),
                   "unknown type in parameter '%.*s'",
                   (int)vd->name.len, vd->name.ptr);
      }
      params[j] = t; /* 失败置 NULL，位置对齐，func_shadow_call 校验时跳过 */
    }
  }

  const type_t *rt = NULL;
  if (fn->return_expr) {
    rt = sema_resolve_type_slot(sema, &fn->return_expr);
    if (!rt) {
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                 "unknown return type");
    }
  }
  /* §5 强制规则（m3-design §5）：返回 ref 必标注（集合 = 参数名 ∪ {*}）。 */
  const type_t *sig = type_func_sig(sema->vm, params, nparams, rt, false);
  /* 符号 type 填充（调用点/引用点签名校验 + AST_FUNC_REF 改写用） */
  sema_symbol_t *sym = sema_lookup(scope, fn->name);
  if (sym) {
    sym->type = sig;
    sym->ast = (ast_node_t *)fn;
  }
  /* 签名类型登记（hoist 构造 + fn->sig_id：compiler LOAD_TYPE <sig_id>） */
  const sema_type_t *st = sema_type_register(sema, sig);
  if (st) fn->sig_id = st->id;

  /* 参数符号 type 补全（3a 建树时解析失败的局部类型槽位）；
     参数符号在参数层（param_scope，fscope 子） */
  sema_func_t *sf = sema_func_find_by_def(sema, (ast_node_t *)fn);
  if (sf && sf->param_scope) {
    size_t j = 0;
    for (ast_node_t *p = fn->params; p; p = p->next, j++) {
      ast_var_def_t *vd = (ast_var_def_t *)p;
      sema_symbol_t *ps = sema_scope_find_local(sf->param_scope, vd->name);
      if (ps && !ps->type) ps->type = params ? params[j] : NULL;
    }
  }

  if (params) allocator_free(sema->vm->alloc, (void **)&params);
}

/* ===========================================================================
 * 3a 建树 / 3b 消费的**对位自检**（作用域树索引协议）
 *
 * 协议：3a（stmt_build）用 sema_scope_add_child 按源码序为每个建作用域的语句
 * （block / if 的 then·else / switch 的 case·default / while / do-while /
 * for + body / 函数定义）挂一个子作用域；3b（本文件 walker）用
 * sema_scope_child(scope, (*idx)++) 按同一顺序消费。
 *
 * 两侧一旦失配，此前只会**静默降级**（调用点统一写 `child ? child : scope`）
 * ——表现为符号永不激活、遮罩语义反转、借用计数泄漏等极难定位的错误。
 * 这里在每个块消费结束时校验"消费数 == 子作用域数"，使失配立刻变响。
 *
 * 唯一允许的差异：块内出现"确定返回"之后的不可达尾部——3a 仍按结构建树，
 * 而 3b 提前 break（不可达语句本就不做类型检查），故跳过校验。
 * =========================================================================== */

static void check_scope_consumed(sema_t *sema, sema_scope_t *scope,
                                 ast_node_t *where_node, size_t consumed) {
  size_t have = sema_scope_children_count(scope);
  if (consumed == have) return;
  diag_error(sema->diag, sema_loc(sema, where_node),
             "internal: scope tree desync (consumed %zu of %zu child scopes) "
             "— sema 3a/3b 建树与消费不一致，请报告此缺陷",
             consumed, have);
}

block_result_t walk_block(sema_t *sema, ast_node_t *block,
                                 sema_scope_t *scope, size_t *idx) {
  block_result_t r = {0};
  ast_block_t *b = (ast_block_t *)block;
  /* 提升：局部 type 定义与局部函数定义在块入口按声明序绑定——名字整个
     块内可见（无 TDZ，前向引用安全，同块局部函数互相调用）。type rhs 求值
     仍按声明序（类型引用依赖前序类型，与全局 pass1b 一致）；求值会折叠
     td->expr 为 AST_TYPE_REF 并激活符号（sema_eval_type_def）。局部函数
     提升解析签名（resolve_local_func_sig：type_func_sig 建签名 → 登记 →
     fn->sig_id + 符号 type 填充）。walk 主循环跳过这两类语句（已处理）。
     prior_vars 累积块内声明序靠前的 var 名：type RHS 引用命中 → var 完全
     遮罩（遮蔽平等），预检报错并跳过求值（不绑定/不激活）。 */
  strslice_t prior_vars[64];
  size_t prior_n = 0;
  for (ast_node_t *s = b->stmts; s; s = s->next) {
    if (s->kind == AST_VAR_DEF) {
      ast_var_def_t *pv = (ast_var_def_t *)s;
      if (pv->is_comptime) {
        /* 局部 comptime var 在提升预扫里按源序求值：同块后面的 type def
           （`type T = [C]i32`）在提升阶段就要用到它的常量值（值同时绑定进
           本块 VM 作用域，常量槽位解析才查得到）。
           comptime var 是**编译期常量**，不是运行期变量——不纳入
           prior_vars 遮蔽预检（否则 `type T = [C]i32` 会被误判为
           "引用变量遮蔽类型名"）。
           守卫：init 引用了靠前的**运行期变量**时**不**提前求值——此刻那些
           变量尚未在定义点激活，提前求值会把诊断退化成 "undefined variable"；
           留给 3b 定义点求值才能给出准确的 "not a compile-time constant"。 */
        bool uses_runtime_var =
            pv->init && type_rhs_uses_var(pv->init, prior_vars, prior_n) != NULL;
        if (!uses_runtime_var) {
          sema_symbol_t *csym = sema_scope_find_local(scope, pv->name);
          if (!csym || !csym->ct_valid)
            sema_eval_comptime_var(sema, pv, scope);
        }
      } else if (prior_n < 64) {
        prior_vars[prior_n++] = pv->name;
      }
    } else if (s->kind == AST_TYPE_DEF) {
      ast_type_def_t *td = (ast_type_def_t *)s;
      const strslice_t *shadow =
          type_rhs_uses_var(td->expr, prior_vars, prior_n);
      if (shadow) {
        diag_error(sema->diag, sema_loc(sema, td->expr),
                   "'%.*s' is a variable, not a type", (int)shadow->len,
                   shadow->ptr);
        continue; /* 遮蔽：跳过求值，符号保持未激活 */
      }
      sema_eval_type_def(sema, td, scope);
    } else if (s->kind == AST_ENUM_DEF) {
      /* 局部 enum 定义提升：与 type def 同构——底层类型是类型槽位（内建
         整型名，非变量可遮蔽），variant 值是值表达式（ctfe 按 vm scope
         查找，局部 var 未绑定报 undefined——天然正确），无需 prior_vars
         遮蔽预检。sema_eval_enum_def 完成：底层校验 + variant 求值 +
         type_enum_intern + 登记 sema->types（hoist 自动构造）+ 绑定 type
         value + 激活符号。 */
      sema_eval_enum_def(sema, (ast_enum_def_t *)s, scope);
    } else if (s->kind == AST_STRUCT_DEF) {
      /* 局部 struct 定义提升：与 type def 同构——字段类型是类型槽位
         （sema_resolve_type_slot，内建/具名类型名，非变量可遮蔽），无需
         prior_vars 遮蔽预检。sema_eval_struct_def 完成：字段类型解析 +
         查重 + type_struct_intern + 登记 sema->types（hoist 自动构造）+
         绑定 type value + 激活符号。 */
      sema_eval_struct_def(sema, (ast_struct_def_t *)s, scope);
    } else if (s->kind == AST_UNION_DEF) {
      /* 局部 union 定义提升：与 struct def 同构——字段类型是类型槽位，
         字段（member）名非变量可遮蔽，无需 prior_vars 遮蔽预检。
         sema_eval_union_def 完成：字段类型解析 + 查重 + type_union_intern
         + 登记 sema->types（hoist 自动构造）+ 绑定 type value + 激活符号。 */
      sema_eval_union_def(sema, (ast_union_def_t *)s, scope);
    } else if (s->kind == AST_CUNION_DEF) {
      /* 局部 cunion 定义提升：与 union def 同构——字段类型是类型槽位，
         字段（member）名非变量可遮蔽，无需 prior_vars 遮蔽预检。
         sema_eval_cunion_def 完成：字段类型解析 + 查重 + type_cunion_intern
         + 登记 sema->types（hoist 自动构造）+ 绑定 type value + 激活符号。 */
      sema_eval_cunion_def(sema, (ast_cunion_def_t *)s, scope);
    } else if (s->kind == AST_FUNC_DEF) {
      ast_func_def_t *fn = (ast_func_def_t *)s;
      /* 统一解析签名（含 comptime：调用点折叠前符号 type 须就绪——
         调用点 !sym->type 会误报 "undefined function"）。
         3a 拒绝（遮蔽全局函数名）的符号未注册 → find_local 为空 → 跳过。 */
      if (!sema_scope_find_local(scope, fn->name)) continue;
      resolve_local_func_sig(sema, fn, scope);
    }
  }
  /* prev 维护：comptime var 定义点求值后从语句链摘除（不进入运行时）。
     var def 不消费子作用域（3a 只注册符号），摘除不影响索引对齐。 */
  ast_node_t **prev = &b->stmts;
  for (ast_node_t *s = b->stmts; s;) {
    if (s->kind == AST_TYPE_DEF || s->kind == AST_ENUM_DEF ||
        s->kind == AST_STRUCT_DEF || s->kind == AST_UNION_DEF ||
        s->kind == AST_CUNION_DEF) {
      /* 入口提升已处理（type def / enum def / struct def / union def /
         cunion def 均无子作用域） */
      prev = &s->next;
      s = s->next;
      continue;
    }
    if (s->kind == AST_FUNC_DEF) {
      /* 入口提升已处理签名；捕获解析（定义点，外层 scope 在线——纯 id 捕获
         类型/flow_init、括号捕获 init 求值与类型校验）在此完成，写回 fscope
         捕获符号（sema_walk_function 读取定义 shadow value）。
         消费 3a 建的 fscope 子作用域（被拒无子作用域）——comptime 局部函数
         同样建树，须一并消费，与 3a 建树严格对齐。 */
      ast_func_def_t *fn = (ast_func_def_t *)s;
      if (sema_scope_find_local(scope, fn->name)) {
        sema_func_t *sf = sema_func_find_by_def(sema, (ast_node_t *)fn);
        resolve_func_captures(sema, fn, sf ? sf->scope : NULL, scope);
        (*idx)++;
      }
      prev = &s->next;
      s = s->next;
      continue;
    }
    block_result_t sr = walk_stmt(sema, s, scope, idx);
    if (sr.definitely_returns) {
      r.definitely_returns = true;
      /* 退出点节点上挂载的 defer calls（desugar_defers 设到 defers 字段）
         仍需 walk（类型检查）——它们不在语句链中，是节点附载的。 */
      for (ast_node_t *d = s->defers; d; d = d->next)
        walk_stmt(sema, d, scope, idx);
      break; /* 后续语句不可达，不再检查 */
    }
    if (s->kind == AST_VAR_DEF && ((ast_var_def_t *)s)->is_comptime) {
      *prev = s->next; /* 摘除定义点 */
      s = s->next;
      continue;
    }
    prev = &s->next;
    s = s->next;
  }
  /* 3a/3b 对位自检（见 check_scope_consumed 说明）。不可达尾部不校验：
     3a 按结构建树，3b 已提前 break——属预期差异。 */
  if (!r.definitely_returns)
    check_scope_consumed(sema, scope, block, *idx);
  return r;
}

/* ---- 函数入口 ---- */

void sema_walk_function(sema_t *sema, sema_func_t *sf) {
  ast_func_def_t *fn = (ast_func_def_t *)sf->def;
  if (!sf->scope) return; /* 建树失败（结构错误已诊断），不进入 shadow run */

  /* 进入函数 walk：**整体保存**当前函数上下文并整体恢复（含 return_type /
     def / 捕获检查上下文 / comptime 标志 / callee·循环标志）。
     本函数可被嵌套调用（sema_check_func_literal 在外层 walk 中调用它走字面量
     body），逐字段清空 + 部分恢复的写法极易漏字段——整体结构赋值则天然完备。
     上下文类型见 sema_fn_ctx_t。 */
  sema_fn_ctx_t saved = sema_fn_ctx_enter(sema);

  /* 捕获检查上下文：局部函数体引用外层局部符号 → 报错（无闭包，运行时
     函数体查找链只有参数 + 全局）。全局函数不设 local_func_base（fscope
     parent = 全局，无外层局部可捕获），但参数层照设——walk_return 的 §6
     返回 ref 来源检查（参数放行）需要参数层判定。 */
  sema->fn.local_func_base = sf->is_local ? sf->scope : NULL;
  sema->fn.local_func_param_scope = sf->param_scope;

  /* comptime 函数体 walk 标志：body 内对 comptime func 的调用走普通
     shadow 校验（参数是 shadow，不能 CTFE），标志在整个 body walk 期间
     保持。 */
  sema->fn.walking_comptime = fn->is_comptime;

  sema->fn.return_type =
      fn->return_expr ? sema_resolve_type_slot(sema, &fn->return_expr) : NULL;
  sema->fn.def = fn;

  /* 函数级 VM scope（与作用域树同构）：捕获层（父）定义捕获 shadow value，
     参数层（子，遮蔽捕获）定义参数 shadow value——进入函数体即可读（名字
     取自 ast）。与运行时 func_vcall 结构一致：closure_scope（捕获）→
     参数匿名层（子）。 */
  vm_push_scope(sema->vm); /* 捕获层 */
  for (ast_node_t *c = fn->captures; c; c = c->next) {
    ast_var_def_t *cv = (ast_var_def_t *)c;
    sema_symbol_t *cs = sema_scope_find_local(sf->scope, cv->name);
    if (!cs) continue; /* 3a 拒绝已诊断，跳过防级联 */
    value_t *cv_value = value_make_shadow(
        sema->vm, cs->type ? cs->type : sema->vm->type_void);
    char nb[256];
    name_to_cstr(cv->name, nb, sizeof nb);
    scope_define(sema->vm, sema->vm->current_scope, nb, cv_value);
  }
  vm_push_scope(sema->vm); /* 参数层（子，遮蔽捕获） */
  for (ast_node_t *p = fn->params; p; p = p->next) {
    ast_var_def_t *vd = (ast_var_def_t *)p;
    sema_symbol_t *ps = sema_scope_find_local(sf->param_scope, vd->name);
    if (ps) {
      ps->flow_init = true; /* 参数由调用方传入，确定已初始化 */
      ps->is_active = true; /* 参数进入函数体立即可见 */
    }
    value_t *pv = value_make_shadow(sema->vm,
                                    ps && ps->type ? ps->type
                                                   : sema->vm->type_void);
    char nb[256];
    name_to_cstr(vd->name, nb, sizeof nb);
    scope_define(sema->vm, sema->vm->current_scope, nb, pv);
  }

  /* 闭包调用上下文（隐藏局部）：`__ctx` = 当前被调用的 func 值。捕获读取在
     ex_ident 里改写为 `__ctx.env.<cap>`（捕获聚合结构体，与 C 后端的
     `ctx->env.cap` 同构）。符号注册在参数层——函数体可见、捕获/参数同名时
     参数先行遮蔽（与捕获按名遮蔽的既有语义一致）。 */
  /* 绑定条件：**有捕获**且签名已登记。不能只看 capture_type_id——常量闭包
     折叠会在 pass_globals 阶段（早于 Pass 3b 体内 walk）就把 capture_type_id
     写回字面量，此时若该函数体还要被 walk，捕获读取会被改写成
     `__ctx.env.<cap>`，而 __ctx 只在满足本条件时才注册；条件太严 →
     "undefined variable '__ctx'"。有捕获即绑定，对普通闭包无副作用
     （多一个隐藏局部符号，捕获读取仍按各自路径解析）。 */
  if (fn->capture_type_id != 0 && fn->sig_id != 0) {
    const sema_type_t *sig_st = sema_type_by_id(sema, fn->sig_id);
    const type_t *sig_type = sig_st ? sig_st->type : NULL;
    if (sig_type) {
      sema_symbol_t ctxsym;
      memset(&ctxsym, 0, sizeof ctxsym);
      ctxsym.kind       = SEMA_SYM_VAR;
      ctxsym.type       = sig_type;
      ctxsym.flow_init  = true;
      ctxsym.is_active  = true;
      ctxsym.own_scope  = sf->param_scope;
      sema_scope_define(sf->param_scope, strslice_from_bytes("__ctx", 5),
                        &ctxsym);
      value_t *csv = value_make_shadow(sema->vm, sig_type);
      scope_define(sema->vm, sema->vm->current_scope, "__ctx", csv);
    }
  }

  /* 返回路径完整性分析已在 Pass 3a（建树阶段）完成；
     walk_block 的 block_result_t 仅用于跳过不可达语句的类型检查 */
  size_t child_idx = 0;
  (void)walk_block(sema, fn->body, sf->param_scope, &child_idx);
  vm_pop_scope(sema->vm);
  vm_pop_scope(sema->vm);

  /* 返回路径完整性分析已在 Pass 3a（建树阶段）完成。整体恢复进入函数 walk
     前的上下文（见函数入口的 saved_fn_ctx）——不再逐字段清空/部分恢复。 */
  sema_fn_ctx_leave(sema, saved);
}

/* 函数字面量 body walk 入口（stmt_build.c 的 sema_check_func_literal 用；
   walk_block 是 static，此处暴露薄封装）。idx 内部自持——临时作用域树
   由 sema_check_func_literal 独立构建，消费顺序与建树严格对齐。 */
void sema_walk_block(sema_t *sema, ast_node_t *block, sema_scope_t *scope) {
  size_t child_idx = 0;
  (void)walk_block(sema, block, scope, &child_idx);
}
