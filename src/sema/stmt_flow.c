#include "sema/sema.h"
#include "vm/type_ownership.h"
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
#include "parser/ast_opt_is_some.h"
#include "parser/ast_for_index.h"
#include "parser/ast_move.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_addr.h"
#include "parser/ast_new.h"
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
#include "parser/ast_owned_type.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_walk.h"
#include "parser/ast_if.h"
#include "parser/ast_is_tag.h"
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
#include "parser/ast_move.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_addr.h"
#include "parser/ast_new.h"   /* ast_move_t（借源收集剥 move/clone） */
#include "parser/ast_addr.h"
#include "parser/ast_new.h"   /* ast_addr_t（x.& 的借源是取址目标） */
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
#include "vm/ffi_lib.h"          /* M6 FFI：ffi_data_kind_of（数据符号类型码） */
#include "parser/ast_free.h"     /* AST_FREE：sema 生成的 own 变量释放语句 */
#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * Pass 3b：Shadow VM 运行
 *
 * 按预建作用域树严格对应遍历 AST。每个块用独立局部索引遍历自己的
 * children：build 阶段子作用域按出现顺序 add_child，walk 必须逐层
 * 独立取用（共享索引会在嵌套时污染外层，导致后续作用域错位）。
 * =========================================================================== */

/* ---- Step B 所有权检查辅助（m3-design §3/§7） ---- */

/* R1/R2 判定：赋值 rhs 是否是 own 指针的合法初始化源。
   own *T 变量只允许两种初始化来源：
   - new 表达式（AST_NEW）：新堆块直接产 own *T（§8.1）
   - fatal 接管源（move/clone 产物或 fatal 参数，§3.3）
   ref 派生（own → ref 隐式转换）是借用（ref 总借用），不能赋回 own。
   own → own 直接赋值 = copy → 编译错误。
   非 static：AST_NEW/AST_CONSTRUCT 字段校验（sema/expr.c）共用。 */

#include "sema_stmt_internal.h"

/* ---- 语句：控制流 ---- */

/* 捕获读取被改写为 `__ctx.env.<cap>`（捕获聚合结构体）后的**同构识别**：
   返回捕获名。返回 ref 的 §6/§9 检查（来源必须是参数/捕获/全局、标注不得
   更宽）按名字做静态分析，必须能看穿这个改写。 */
static bool capture_access_name(ast_node_t *e, strslice_t *out) {
  if (!e || e->kind != AST_MEMBER) return false;
  ast_member_t *cap = (ast_member_t *)e;
  if (!cap->object || cap->object->kind != AST_MEMBER) return false;
  ast_member_t *env = (ast_member_t *)cap->object;
  if (!env->object || env->object->kind != AST_IDENT) return false;
  if (env->field.len != 3 || !env->field.ptr ||
      memcmp(env->field.ptr, "env", 3) != 0)
    return false;
  ast_ident_t *ctx = (ast_ident_t *)env->object;
  if (ctx->name.len != 5 || !ctx->name.ptr ||
      memcmp(ctx->name.ptr, "__ctx", 5) != 0)
    return false;
  *out = cap->field;
  return true;
}

block_result_t walk_return(sema_t *sema, ast_return_t *rt,
                                  sema_scope_t *scope) {
  block_result_t r = {.definitely_returns = true};
  if (rt->value) {
    /* `return cfunc_ptr(<命名 cfunc>)` 在返回类型是 func 时自动生成包装。 */
    if (sema->fn.return_type && type_kind_of(sema->fn.return_type) == TYPE_KIND_FUNC)
      sema_maybe_wrap_cfunc_expr(sema, &rt->value, sema->fn.return_type, scope);
    value_t *v = sema_expr(sema, &rt->value, scope);
    bool v_bad = value_is_error(sema->vm, v) ||
                 value_is_type(v, TYPE_KIND_VOID);
    if (!v_bad) {
      /* 直接 return 一个含 own 槽位的构造临时：它的值是 OWN（构造产物本人
         是所有者），不能按值交给调用方；必须显式 move/clone 转成将亡值。 */
      if (rt->value && rt->value->kind == AST_CONSTRUCT &&
          type_has_own_slots(value_type(v))) {
        diag_error(sema->diag, sema_loc(sema, rt->value),
                   "cannot return a freshly constructed aggregate with own "
                   "slots by value; wrap it in move(...) or clone(...) to hand "
                   "ownership to the caller");
        v_bad = true;
      }
    }
    if (!v_bad) {
      if (!sema->fn.return_type) {
        diag_error(sema->diag, sema_loc(sema, rt->value),
                   "void function cannot return a value");
      } else {
        /* value_assign 校验返回类型可赋给签名返回类型 */
        value_t *dst = value_make_shadow(sema->vm, sema->fn.return_type);
        if (value_is_error(sema->vm, value_assign(sema->vm, dst, v))) {
          char tn[64], rn[64];
          sema_type_name(sema->fn.return_type, tn, sizeof(tn));
          sema_type_name(value_type(v), rn, sizeof(rn));
          diag_error(sema->diag, sema_loc(sema, rt->value),
                     "cannot return %s from function returning %s", rn, tn);
        } else if (sema->fn.return_type &&
                   type_ownership_of(sema->fn.return_type) == OWN_REF &&
                   (rt->value->kind == AST_IDENT ||
                    capture_access_name(rt->value, &(strslice_t){0}))) {
          /* R3（返回 ref 来源，m3-design §6）：返回值 ref 的（标注）作用域
             必然来自参数或全局——本地 own 变量/临时借用不能作为返回值 ref
             来源（返回后栈上值销毁，借用悬垂）。检查：return 的值是
             标识符时，须是 ref/fatal 参数（fatal 返回继续传递所有权）或
             全局 ref。非标识符表达式（x.& 取栈值地址等）一律拒绝。 */
          strslice_t rname = ((ast_ident_t *)rt->value)->name;
          if (rt->value->kind != AST_IDENT)
            capture_access_name(rt->value, &rname);
          sema_symbol_t *rs = sema_lookup(scope, rname);
          if (rs && rs->kind == SEMA_SYM_VAR && rs->type &&
              sema_sym_ownership(rs) == OWN_REF) {
            /* §6 返回 ref 来源：必须是参数或全局。参数层符号
               （sema_lookup 沿 scope 链第一个命中）或全局符号放行；
               本地 own 的 ref 变量/外层局部借用 → 悬垂 → 拒绝。 */
            sema_symbol_t *g = sema_lookup(sema->global_scope, rname);
            /* 参数判定：遍历真实参数列表（body 顶层变量也注册在
               param_scope，sema_scope_find_local 无法区分） */
            bool is_param = false;
            for (ast_node_t *p = sema->fn.def ? sema->fn.def->params : NULL;
                 p && !is_param; p = p->next) {
              ast_var_def_t *pvd = (ast_var_def_t *)p;
              is_param = pvd->name.len == rname.len &&
                         memcmp(pvd->name.ptr, rname.ptr, pvd->name.len) == 0;
            }
            bool is_global = g == rs;
            /* §9 闭包捕获视同特殊参数：返回 ref 可来自捕获变量
               （调用闭包时闭包对象必然存活，捕获的 ref 有效） */
            bool is_capture = false;
            if (sema->fn.def && sema->fn.def->captures) {
              for (ast_node_t *c = sema->fn.def->captures; c; c = c->next) {
                ast_var_def_t *cv = (ast_var_def_t *)c;
                if (cv->name.len == rname.len &&
                    memcmp(cv->name.ptr, rname.ptr, rname.len) == 0) {
                  is_capture = true;
                  break;
                }
              }
            }
            if (!is_param && !is_global && !is_capture) {
              diag_error(sema->diag, sema_loc(sema, rt->value),
                         "cannot return ref borrowed from local "
                         "'%.*s' (returned ref must come from a parameter, "
                         "capture or global)",
                         (int)rname.len, rname.ptr);
            }
            /* §9 闭包返回捕获 ref 的标注关系检查：返回值标注的每个锚点
               须嵌套于闭包标注的每个锚点（返回值存活 ≤ 闭包存活 ≤ 锚点）。
               返回值来自捕获 → 返回值存活受闭包存活约束；返回值标注不得
               比闭包标注更宽（否则调用方以为返回值活到 y，实际活不过 x）。 */
            if (is_capture && sema->fn.def &&
                sema->fn.def->scope_annot_count > 0) {
              strslice_t *ret_names = NULL;
              size_t ret_count = 0;
              sema_annot_peek(sema->fn.def->return_expr,
                              &ret_names, &ret_count);
              for (size_t i = 0; i < ret_count; i++) {
                sema_scope_t *ret_anchor =
                    annot_anchor_scope(sema, ret_names[i], scope);
                if (!ret_anchor) continue; /* 未知锚点由他处报 */
                for (size_t j = 0; j < sema->fn.def->scope_annot_count;
                     j++) {
                  sema_scope_t *clo_anchor = annot_anchor_scope(
                      sema, sema->fn.def->scope_annot_names[j], scope);
                  if (!clo_anchor) continue;
                  if (!sema_scope_within(ret_anchor, clo_anchor)) {
                    diag_error(sema->diag, sema_loc(sema, rt->value),
                               "returned ref annotation '%.*s' escapes "
                               "closure annotation '%.*s' (return scope must "
                               "be nested within closure scope)",
                               (int)ret_names[i].len, ret_names[i].ptr,
                               (int)sema->fn.def->scope_annot_names[j].len,
                               sema->fn.def->scope_annot_names[j].ptr);
                  }
                }
              }
            }
          }
        }
      }
    }

    /* R1（fatal 参数消费，§4）：`return p;`（p 为 fatal 指针**或切片**参数）
       = 所有权继续传递给调用方 → 标记已消费，函数结束的"未消费"检查放行。
       **与返回类型无关**：这段标记原先写在 `: ref *T` 的分支内部，fatal 指针/
       切片返回根本走不到，于是 `func id(p: fatal []i32): fatal []i32 { return p; }`
       被误报"未消费"（诊断文本自己写的是 "move() or return it"）。
       判据与 sema_walk_function 末尾的检查（PTR_FATAL || SLICE_FATAL）一致。 */
    if (rt->value && rt->value->kind == AST_IDENT) {
      sema_symbol_t *rs = sema_lookup(scope, ((ast_ident_t *)rt->value)->name);
      if (rs && rs->kind == SEMA_SYM_VAR && sema_sym_is_fatal(rs)) {
        sema_mark_fatal_consumed(rs);
      }
    }
  } else {
    if (sema->fn.return_type &&
        type_kind_of(sema->fn.return_type) != TYPE_KIND_VOID) {
      char tn[64];
      sema_type_name(sema->fn.return_type, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &rt->base),
                 "function returning %s must return a value", tn);
    }
  }
  /* 为 return 路径沿 scope 链收集 own 变量的 FREE 语句，追加到 defers 链末尾。
     FREE 在 defer calls 之后执行（defer 可能引用 own 变量，须先执行）。
     defers 链由 Pass 3a desugar_defers 设置（可能为 NULL）；FREE 追加到末尾。 */
  {
    const sema_scope_t *stop = find_func_stop_sema(scope);
    ast_node_t *free_stmts = collect_free_stmts(sema, scope, stop);
    if (free_stmts) {
      if (rt->base.defers) {
        ast_node_t *tail = rt->base.defers;
        while (tail->next) tail = tail->next;
        tail->next = free_stmts;
      } else {
        rt->base.defers = free_stmts;
      }
      /* walk 生成的 FREE 语句 */
      for (ast_node_t *f = free_stmts; f; f = f->next)
        walk_stmt(sema, f, scope, &((size_t){0}));
    }
  }
  return r;
}

block_result_t walk_while(sema_t *sema, ast_while_t *wl,
                                 sema_scope_t *scope, size_t *idx) {
  value_t *cond = sema_expr(sema, &wl->cond, scope);
  sema_check_bool(sema, wl->cond, cond, "while condition");

  /* 循环体可能执行 0 次：体内赋值不提升外层变量的确定性（保守） */
  flow_snap_t snap = flow_capture(sema, scope);

  sema_scope_t *body_scope = sema_scope_child(scope, (*idx)++);
  vm_push_scope(sema->vm); /* 循环体块：VM scope 与 sema scope 树同构 */
  size_t sub = 0;
  walk_block(sema, wl->body, body_scope ? body_scope : scope, &sub);
  sema_scope_release_borrows(body_scope); /* R3：体内借用的 ref 载体随作用域消亡 */
  vm_pop_scope(sema->vm);

  flow_restore(&snap);
  flow_release(&snap);
  return (block_result_t){0}; /* 循环体可能不执行，不贡献 definitely_returns */
}

block_result_t walk_dowhile(sema_t *sema, ast_dowhile_t *dw,
                                   sema_scope_t *scope, size_t *idx) {
  /* do-while：条件在体后。体至少执行一次——但条件不满足时循环结束，
     体内赋值对外层变量的确定性仍不提升（保守，与 while 一致） */
  flow_snap_t snap = flow_capture(sema, scope);

  sema_scope_t *body_scope = sema_scope_child(scope, (*idx)++);
  vm_push_scope(sema->vm); /* 循环体块：VM scope 与 sema scope 树同构 */
  size_t sub = 0;
  walk_block(sema, dw->body, body_scope ? body_scope : scope, &sub);
  sema_scope_release_borrows(body_scope); /* R3 */
  vm_pop_scope(sema->vm);

  value_t *cond = sema_expr(sema, &dw->cond, scope);
  sema_check_bool(sema, dw->cond, cond, "do-while condition");

  flow_restore(&snap);
  flow_release(&snap);
  return (block_result_t){0};
}

block_result_t walk_for(sema_t *sema, ast_for_t *fr,
                               sema_scope_t *scope, size_t *idx) {
  sema_scope_t *for_scope = sema_scope_child(scope, (*idx)++);
  sema_scope_t *fs = for_scope ? for_scope : scope;

  /* 循环体可能执行 0 次：体内对外层变量的赋值不提升确定性（保守）。
     先快照外层符号，循环结束后恢复（for 变量本身出作用域不可见） */
  flow_snap_t snap = flow_capture(sema, scope);

  vm_push_scope(sema->vm); /* for 作用域（init 变量） */

  /* init 在 for scope 内求值 */
  if (fr->init) {
    switch (fr->init->kind) {
      case AST_VAR_DEF:
        shadow_var_def(sema, (ast_var_def_t *)fr->init, fs);
        break;
      case AST_ASSIGN:
        shadow_assign(sema, (ast_assign_t *)fr->init, fs);
        break;
      case AST_EXPR_STMT:
        sema_expr(sema, &((ast_expr_stmt_t *)fr->init)->expr, fs);
        break;
      default:
        break;
    }
  }

  if (fr->cond) {
    value_t *c = sema_expr(sema, &fr->cond, fs);
    sema_check_bool(sema, fr->cond, c, "for condition");
  }

  /* body 是 for scope 的子 scope（新局部迭代器） */
  size_t body_idx = 0;
  sema_scope_t *body_scope = sema_scope_child(for_scope, body_idx++);
  vm_push_scope(sema->vm); /* 循环体块 */
  size_t sub = 0;
  walk_block(sema, fr->body, body_scope ? body_scope : fs, &sub);
  sema_scope_release_borrows(body_scope); /* R3 */
  vm_pop_scope(sema->vm);

  /* update 与 init 同形：可放赋值语句（j = j + 1）或表达式。
     AST_ASSIGN 必须走 shadow_assign（赋值是语句级检查，sema_expr 只处理
     表达式）。此前无条件走 sema_expr：AST_ASSIGN 落入 switch 的 default
     被静默返回 void shadow ⇒ **for 的步进语句从未被类型检查**。 */
  if (fr->update) {
    if (fr->update->kind == AST_ASSIGN) {
      shadow_assign(sema, (ast_assign_t *)fr->update, fs);
    } else {
      sema_expr(sema, &fr->update, fs);
    }
  }

  flow_restore(&snap); /* 丢弃体内确定性提升（保守） */
  flow_release(&snap);

  sema_scope_release_borrows(for_scope); /* R3：for 作用域（init 变量）退出 */
  vm_pop_scope(sema->vm); /* 退出 for 作用域 */
  return (block_result_t){0};
}

/* ---- if 语句 ---- */

block_result_t walk_if(sema_t *sema, ast_if_t *it, sema_scope_t *scope,
                              size_t *idx) {
  block_result_t r = {0};
  value_t *cond = sema_expr(sema, &it->cond, scope);
  sema_check_bool(sema, it->cond, cond, "if condition");

  /* 确定性赋值合并点：快照分支前状态 → then → 记录 → 恢复 → else →
     meet（AND）：两分支都 flow_init 才 true。保守策略：非全部分支赋值
     （如 if(c){a=1;}else{}）→ 合并后仍 UNKNOWN，读取编译错误。
     optional 解包不在此做 flow 记录：if (x != nil) 分支内的 x 仍是 ?T，
     解包由显式 x.!（assert）承担（docs m2-design §12.5）。 */
  flow_snap_t snap = flow_capture(sema, scope);

  block_result_t tr = {0};
  sema_scope_t *then_scope = sema_scope_child(scope, (*idx)++);
  vm_push_scope(sema->vm); /* then 块 */
  size_t sub = 0;
  tr = walk_block(sema, it->then_body, then_scope ? then_scope : scope, &sub);
  sema_scope_release_borrows(then_scope); /* R3 */
  vm_pop_scope(sema->vm);

  /* 记录 then 后状态（flow），恢复分支前 */
  for (size_t i = 0; i < snap.n; i++)
    snap.after[i] = snap.syms[i]->flow_init;
  flow_restore(&snap);

  block_result_t er = {0};
  if (it->else_body) {
    if (it->else_body->kind == AST_IF) {
      /* else-if 链：同层递归（子作用域顺序与 3a 一致：else-if 不单独
         建 scope，其 then 是当前 scope 的下一个子节点） */
      er = walk_if(sema, (ast_if_t *)it->else_body, scope, idx);
    } else {
      sema_scope_t *else_scope = sema_scope_child(scope, (*idx)++);
      vm_push_scope(sema->vm); /* else 块 */
      size_t sub2 = 0;
      er = walk_block(sema, it->else_body, else_scope ? else_scope : scope,
                      &sub2);
      sema_scope_release_borrows(else_scope); /* R3 */
      vm_pop_scope(sema->vm);
    }
  }

  /* meet：两分支都 INIT 才 INIT（else 缺失视为"未赋值分支"） */
  for (size_t i = 0; i < snap.n; i++)
    snap.syms[i]->flow_init = snap.after[i] && snap.syms[i]->flow_init;
  flow_release(&snap);

  r.definitely_returns = tr.definitely_returns && er.definitely_returns;
  return r;
}

/* ---- switch 语句（docs m2-design §4：if 语法糖，语义内联） ----
 *
 * cond shadow 求值一次（C 局部持有，不注册临时符号——desugar 的
 * `var __switch_N` 是编译期实现细节，用户代码不可见）。每分支每模式
 * value_eq 校验（模式须与 cond 可比），全部分支 walk + flow meet：
 * AND 累积——某变量在**全部分支**都赋值才确定性初始化。无 default 时
 * "不匹配穿透"路径视为未赋值分支（与 else 缺失同语义）。
 * definitely_returns：有 default 且全部分支返回才 true（无 default 时
 * 可能穿透，不贡献）。 */
block_result_t walk_switch(sema_t *sema, ast_switch_t *sw,
                                  sema_scope_t *scope, size_t *idx) {
  block_result_t r = {0};

  value_t *cond = sema_expr(sema, &sw->cond, scope);
  if (value_is_error(sema->vm, cond) ||
      value_is_type(cond, TYPE_KIND_VOID)) {
    /* cond 错误/void（错误恢复产物）：仍 walk 分支体消费子作用域
       （索引对齐），跳过模式校验避免级联二次诊断 */
    for (ast_node_t *cs = sw->cases; cs; cs = cs->next) {
      ast_switch_case_t *sc = (ast_switch_case_t *)cs;
      sema_scope_t *case_scope = sema_scope_child(scope, (*idx)++);
      vm_push_scope(sema->vm);
      size_t sub = 0;
      walk_block(sema, sc->body, case_scope ? case_scope : scope, &sub);
      sema_scope_release_borrows(case_scope); /* R3 */
      vm_pop_scope(sema->vm);
    }
    if (sw->default_body) {
      sema_scope_t *def_scope = sema_scope_child(scope, (*idx)++);
      vm_push_scope(sema->vm);
      size_t sub = 0;
      walk_block(sema, sw->default_body, def_scope ? def_scope : scope, &sub);
      sema_scope_release_borrows(def_scope); /* R3 */
      vm_pop_scope(sema->vm);
    }
    return r;
  }

  /* 模式校验：每模式与 cond value_eq（值匹配，shadow 间比较）。
     类型不可比（value_eq 返回 error）→ 报错，分支体仍 walk（消费子
     作用域 + 内部检查），flow 不参与 meet（错误已 fail-fast）。 */
  for (ast_node_t *cs = sw->cases; cs; cs = cs->next) {
    ast_switch_case_t *sc = (ast_switch_case_t *)cs;
    /* 链上指针迭代：把真实链槽交给 sema_expr，折叠（comptime 变量引用 →
       字面量）就地写回；传局部变量副本会丢折叠，编译期符号引用漏到运行期。 */
    for (ast_node_t **link = &sc->patterns; *link; link = &(*link)->next) {
      value_t *pv = sema_expr(sema, link, scope);
      ast_node_t *pt = *link; /* 折叠可能替换节点：后续用刷新后的节点 */
      if (value_is_error(sema->vm, pv) ||
          value_is_type(pv, TYPE_KIND_VOID))
        continue; /* 错误恢复产物静默通过 */
      value_t *eq = value_eq(sema->vm, cond, pv);
      if (value_is_error(sema->vm, eq)) {
        char cn[64], pn[64];
        sema_type_name(value_type(cond), cn, sizeof(cn));
        sema_type_name(value_type(pv), pn, sizeof(pn));
        diag_error(sema->diag, sema_loc(sema, pt),
                   "switch pattern type mismatch: cannot match %s against %s",
                   cn, pn);
      }
    }
  }

  /* 确定性赋值合并点：快照分支前状态 → 逐分支 walk → 记录 → 恢复 →
     AND 累积。 */
  flow_snap_t snap = flow_capture(sema, scope);
  bool first = true;
  bool all_return = true;

  for (ast_node_t *cs = sw->cases; cs; cs = cs->next) {
    ast_switch_case_t *sc = (ast_switch_case_t *)cs;
    sema_scope_t *case_scope = sema_scope_child(scope, (*idx)++);
    vm_push_scope(sema->vm);
    size_t sub = 0;
    block_result_t br = walk_block(sema, sc->body, case_scope ? case_scope : scope,
                                   &sub);
    sema_scope_release_borrows(case_scope); /* R3 */
    vm_pop_scope(sema->vm);
    if (!br.definitely_returns) all_return = false;
    for (size_t i = 0; i < snap.n; i++) {
      snap.after[i] = first ? snap.syms[i]->flow_init
                            : (snap.after[i] && snap.syms[i]->flow_init);
    }
    first = false;
    flow_restore(&snap);
  }

  if (sw->default_body) {
    sema_scope_t *def_scope = sema_scope_child(scope, (*idx)++);
    vm_push_scope(sema->vm);
    size_t sub = 0;
    block_result_t dr = walk_block(sema, sw->default_body,
                                   def_scope ? def_scope : scope, &sub);
    sema_scope_release_borrows(def_scope); /* R3 */
    vm_pop_scope(sema->vm);
    if (!dr.definitely_returns) all_return = false;
    for (size_t i = 0; i < snap.n; i++) {
      snap.after[i] = first ? snap.syms[i]->flow_init
                            : (snap.after[i] && snap.syms[i]->flow_init);
    }
    first = false;
    flow_restore(&snap);
  }

  if (!first) {
    /* 有分支被 walk：meet。有 default → 全路径覆盖，final = 全分支 AND
       （当前 flow_init 是 restore 后的分支前状态，直接覆盖）；无 default
       → 穿透路径保持分支前状态，final = after && before（AND 进 meet）。 */
    for (size_t i = 0; i < snap.n; i++) {
      if (sw->default_body)
        snap.syms[i]->flow_init = snap.after[i];
      else
        snap.syms[i]->flow_init =
            snap.after[i] && snap.syms[i]->flow_init;
    }
    if (sw->default_body && all_return)
      r.definitely_returns = true;
  }
  flow_release(&snap);
  return r;
}
