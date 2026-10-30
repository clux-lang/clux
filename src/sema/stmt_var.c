#include "sema/sema.h"
#include <stdio.h>
#include <string.h>
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
#include "core/ffi_abi.h"          /* M6 FFI：ffi_data_kind_of（数据符号类型码） */
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

#include "sema_stmt_internal.h"

/* 显式类型槽位兜底重解析：3a 槽位解析可能失败（引用同块后续局部 type /
   被 type_shadowed_by_local_def 判"待绑定局部遮蔽"推迟）→ 此处定义点兜底。
   仍失败补报诊断：激活的同名 var/参数遮蔽 → "is a variable, not a type"
   （完全遮罩语义），其余（拼写错误/前向引用 type def）保持 "unknown type"。
   返回 true 表示解析成功（或无需解析）。 */
static bool var_type_slot_reparse(sema_t *sema, ast_var_def_t *vd,
                                  sema_scope_t *scope) {
  sema_symbol_t *sym = sema_scope_find_local(scope, vd->name);
  if (!sym || sym->type || !vd->type_expr) return true;
  sym->type = sema_resolve_type_slot(sema, &vd->type_expr);
  if (sym->type) return true;

  /* 提取槽位名字（仅命名类型可判遮蔽来源；复合类型报 unknown） */
  strslice_t tn = {0};
  if (vd->type_expr->kind == AST_IDENT) {
    tn = ((ast_ident_t *)vd->type_expr)->name;
  } else if (vd->type_expr->kind == AST_TYPE_REF) {
    tn = ((ast_type_ref_t *)vd->type_expr)->name;
  }
  sema_symbol_t *shadow = tn.ptr ? sema_lookup(scope, tn) : NULL;
  if (shadow && shadow->kind == SEMA_SYM_VAR) {
    /* 激活的 var/参数遮蔽（kind==SYM_VAR 区分于 type/函数符号）：
       完全遮罩语义，类型槽位引用的是变量 → 报错 */
    diag_error(sema->diag, sema_loc(sema, vd->type_expr),
               "'%.*s' is a variable, not a type", (int)tn.len, tn.ptr);
  } else {
    diag_error(sema->diag, sema_loc(sema, vd->type_expr), "unknown type");
  }
  return false;
}

/* ===========================================================================
 * `cfunc(...)` → `func(...)` 隐式转换
 *
 * 外层 func 的类型**由 cfunc 声明生成**，两侧签名必然一致。
 *
 * 生成的包装是**无捕获**的函数字面量：它按**名字**调用那个 cfunc，所以不需要
 * 把运行期函数指针捕获进闭包（也就不需要"调用指针"的新指令）。运行期指针
 * （变量里的 cfunc 值、FFI 返回的函数指针）没有可静态确定的被调者 → 拒绝。
 * =========================================================================== */

/* `cfunc_ptr(<命名 cfunc>)` / 命名 cfunc 本身 → 该 cfunc 定义的 AST；否则 NULL。 */
static ast_func_def_t *cfunc_source_def(sema_t *sema, sema_scope_t *scope,
                                        ast_node_t *expr) {
  ast_node_t *arg = expr;
  bool via_ptr = false;
  if (expr && expr->kind == AST_CALL) {
    ast_call_t *call = (ast_call_t *)expr;
    if (!call->callee || call->callee->kind != AST_IDENT) return NULL;
    strslice_t cn = ((ast_ident_t *)call->callee)->name;
    if (cn.len != 9 || !cn.ptr || memcmp(cn.ptr, "cfunc_ptr", 9) != 0)
      return NULL;
    if (sema_count_siblings(call->args) != 1 || !call->args) return NULL;
    via_ptr = true;
    arg = call->args;
  }
  if (!arg) return NULL;
  ast_func_def_t *def = NULL;
  if (arg->kind == AST_IDENT) {
    sema_symbol_t *sym = sema_lookup(scope, ((ast_ident_t *)arg)->name);
    if (sym && sym->kind == SEMA_SYM_FUNC) {
      if (sym->ast && sym->ast->kind == AST_FUNC_DEF)
        def = (ast_func_def_t *)sym->ast;
      else if (sym->fid) {
        sema_func_t *sf = sema_func_by_id(sema, sym->fid);
        if (sf && sf->def && sf->def->kind == AST_FUNC_DEF)
          def = (ast_func_def_t *)sf->def;
      }
    }
  } else if (arg->kind == AST_FUNC_REF) {
    sema_func_t *sf = sema_func_by_id(sema, ((ast_func_ref_t *)arg)->fid);
    if (sf && sf->def && sf->def->kind == AST_FUNC_DEF)
      def = (ast_func_def_t *)sf->def;
  }
  (void)via_ptr;
  if (!def || !def->is_cfunc) return NULL;
  return def;
}

static strslice_t synth_name(sema_t *sema, const char *s) {
  size_t len = strlen(s);
  char *buf = (char *)arena_alloc(sema->arena, len + 1, 1);
  if (!buf) return STRSLICE_EMPTY;
  memcpy(buf, s, len + 1);
  return strslice_from_bytes(buf, len);
}

/* 生成包装函数字面量（无捕获）。所有权/返回类型不匹配返回 NULL（已诊断）。 */
static ast_node_t *gen_cfunc_wrapper(sema_t *sema, ast_func_def_t *src,
                                     const type_t *target,
                                     ast_node_t *origin) {
  const func_sig_t *sig = &((const func_type_t *)target)->sig;
  size_t nparams = sema_count_siblings(src->params);
  if (sig->param_count != nparams) {
    diag_error(sema->diag, sema_loc(sema, origin),
               "cannot convert cfunc(...) to func(...): the cfunc declares %zu "
               "parameters but func type has %zu",
               nparams, sig->param_count);
    return NULL;
  }

  uint32_t tb = src->base.tok_begin, te = src->base.tok_end;
  ast_node_t *params = NULL, *params_last = NULL;
  ast_node_t *args = NULL, *args_last = NULL;
  size_t i = 0;
  for (ast_node_t *sp = src->params; sp; sp = sp->next, i++) {
    ast_var_def_t *svd = (ast_var_def_t *)sp;
    const type_t *src_pt = sema_resolve_type_slot(sema, &svd->type_expr);
    const type_t *tgt_pt = func_type_param(target, i);
    if (!src_pt || !tgt_pt || src_pt != tgt_pt) {
      diag_error(sema->diag, sema_loc(sema, origin),
                 "cannot convert cfunc(...) to func(...): parameter %zu of the "
                 "cfunc has a different type",
                 i + 1);
      return NULL;
    }

    char nb[32];
    snprintf(nb, sizeof nb, "__cfp%zu", i);
    /* 形参类型表达式直接用**已带所有权修饰的类型引用**（tgt_pt = own *T /
       ref []T …）：类型解析现在会产出包装类型，无需再按 token 手工重建
       （旧实现把 `ref *Box` 又包了一层 → `ref **Box`）。 */
    ast_node_t *tref = sema_ct_type_ref(sema, tgt_pt, origin);
    if (!tref) return NULL;
    ast_node_t *texpr = tref;
    ast_node_t *vn = ast_var_def_new(sema->arena, tb, te);
    if (!vn) return NULL;
    ast_var_def_t *pv = (ast_var_def_t *)vn;
    pv->name = synth_name(sema, nb);
    pv->type_expr = texpr;
    if (params_last) params_last->next = vn; else params = vn;
    params_last = vn;

    ast_node_t *a = ast_ident_new(sema->arena, tb, te);
    if (!a) return NULL;
    ((ast_ident_t *)a)->name = synth_name(sema, nb);
    a->is_generated = true;
    if (args_last) args_last->next = a; else args = a;
    args_last = a;
  }

  const type_t *tgt_ret = func_type_return(target);
  const type_t *src_ret =
      src->return_expr ? sema_resolve_type_slot(sema, &src->return_expr) : NULL;
  if (tgt_ret != src_ret) {
    diag_error(sema->diag, sema_loc(sema, origin),
               "cannot convert cfunc(...) to func(...): return type differs");
    return NULL;
  }

  ast_node_t *callee = ast_ident_new(sema->arena, tb, te);
  if (!callee) return NULL;
  ((ast_ident_t *)callee)->name = src->name;
  callee->is_generated = true;
  ast_node_t *call = ast_call_new(sema->arena, tb, te);
  if (!call) return NULL;
  ((ast_call_t *)call)->callee = callee;
  ((ast_call_t *)call)->args = args;
  ((ast_call_t *)call)->args_last = args_last;

  ast_node_t *stmt;
  if (tgt_ret) {
    stmt = ast_return_new(sema->arena, tb, te);
    if (stmt) ((ast_return_t *)stmt)->value = call;
  } else {
    stmt = ast_expr_stmt_new(sema->arena, tb, te);
    if (stmt) ((ast_expr_stmt_t *)stmt)->expr = call;
  }
  if (!stmt) return NULL;
  ast_node_t *blk = ast_block_new(sema->arena, tb, te);
  if (!blk) return NULL;
  ((ast_block_t *)blk)->stmts = stmt;
  ((ast_block_t *)blk)->stmts_last = stmt;

  ast_node_t *lit = ast_func_def_new(sema->arena, tb, te);
  if (!lit) return NULL;
  ast_func_def_t *fd = (ast_func_def_t *)lit;
  fd->name = STRSLICE_EMPTY;
  fd->params = params;
  fd->params_last = params_last;
  fd->return_expr = tgt_ret ? sema_ct_type_ref(sema, tgt_ret, origin) : NULL;
  fd->body = blk;
  /* **不要**标记 is_generated：sema 会跳过 generated 语句/节点的 walk，
     参数符号就不会注册，函数体里的参数引用解析不到符号。 */
  return lit;
}

/**
 * `*slot` 是 cfunc 指针表达式、目标是 func 类型时，替换成生成的包装字面量。
 * 变量初始化与**调用实参**共用（两者都是"表达式要变成 func 值"）。
 * 返回 true 表示已改写。
 */
bool sema_maybe_wrap_cfunc_expr(sema_t *sema, ast_node_t **slot,
                                const type_t *target, sema_scope_t *scope) {
  if (!slot || !*slot) return false;
  if (!target || type_kind_of(target) != TYPE_KIND_FUNC) return false;
  if ((*slot)->kind == AST_UNDEF) return false;
  ast_func_def_t *src = cfunc_source_def(sema, scope, *slot);
  if (!src) {
    /* `cfunc_ptr(<非命名 cfunc>)`：给出针对性诊断（否则落到
       "cannot initialize ... with opaque"）。 */
    if ((*slot)->kind == AST_CALL) {
      ast_call_t *call = (ast_call_t *)*slot;
      if (call->callee && call->callee->kind == AST_IDENT) {
        strslice_t cn = ((ast_ident_t *)call->callee)->name;
        if (cn.len == 9 && cn.ptr && memcmp(cn.ptr, "cfunc_ptr", 9) == 0) {
          diag_error(sema->diag, sema_loc(sema, *slot),
                     "cannot convert a C function pointer to func(...): the "
                     "wrapper is generated from a named cfunc; write "
                     "`cfunc_ptr(<named cfunc>)`");
          return true;
        }
      }
    }
    return false;
  }
  ast_node_t *lit = gen_cfunc_wrapper(sema, src, target, *slot);
  if (!lit) return true;
  lit->next = (*slot)->next;
  *slot = lit;
  return true;
}

/** 变量初始化位置的薄封装（保留原调用点写法）。 */
static void sema_maybe_wrap_cfunc_init(sema_t *sema, ast_var_def_t *vd,
                                       sema_scope_t *scope,
                                       const type_t *target) {
  sema_maybe_wrap_cfunc_expr(sema, &vd->init, target, scope);
}

void shadow_var_def(sema_t *sema, ast_var_def_t *vd,
                           sema_scope_t *scope) {
  sema_symbol_t *sym = sema_scope_find_local(scope, vd->name);
  if (!sym) return; /* 3a 重复定义已诊断，符号未注册 */

  /* `var b = a.?` / `var b = a.!`（a 为 result struct）：展开为
     `var b: T = undefined; if (a.ok()) b = a.value(); else ...`。
     分支放在 postlude：compiler 先 DEFINE b，再编译分支。 */
  if (vd->init && vd->init->kind == AST_UNWRAP) {
    sema_try_info_t info;
    if (sema_try_probe(sema, vd->init, scope, &info)) {
      const type_t *vt = info.value_type;
      if (!vt) {
        diag_error(sema->diag, sema_loc(sema, vd->init),
                   "error-handling sugar has no value type");
        return;
      }
      const type_t *decl = vt;
      uint32_t tb = vd->init->tok_begin, te = vd->init->tok_end;
      vd->init = ast_undef_new(sema->arena, tb, te);
      vd->type_expr = sema_gen_type_ref(sema, decl);
      vd->resolved_type = decl;
      if (!vd->init || !vd->type_expr) return;
      /* 先按普通 `var b: T = undefined` 建符号/作用域 */
      shadow_var_def(sema, vd, scope);
      sym = sema_scope_find_local(scope, vd->name);
      if (sym) sym->flow_init = true;
      ast_node_t *target = ast_ident_new(sema->arena, tb, te);
      if (!target) return;
      ((ast_ident_t *)target)->name = vd->name;
      ast_node_t *iff =
          sema_try_make_if(sema, &info, target, NULL, tb, te);
      if (iff) {
        if (info.setup) {
          info.setup->next = iff;
          vd->base.postlude = info.setup;
        } else {
          vd->base.postlude = iff;
        }
      }
      return;
    }
  }
  /* Pass 3b 已走到定义点：此后收集销毁/借源时才包含该变量（提前 return 的
     路径不会销毁后面才声明的变量）。 */
  sym->declared = true;

  /* comptime var：编译期求值 + 符号表编码（sema_eval_comptime_var）。
     定义点不进入 VM scope——调用方（walk_block）负责从语句链摘除；
     引用点在 sema_expr 折叠为字面量。comptime 值是编译期常量（无运行时
     生命周期），不参与 §5 逃逸检查。
     已求值的跳过：walk_block 的提升预扫会先按源序求值同块 comptime var
     （该块后面的 type def 如 `type T = [C]i32` 在提升阶段就要用到常量值），
     此处不能重复求值（会二次绑定同名常量）。 */
  if (vd->is_comptime) {
    if (!sym->ct_valid) sema_eval_comptime_var(sema, vd, scope);
    return;
  }

  /* `cfunc(...)` → `func(...)`：生成按名调用 cfunc 的无捕获包装（own 形参
     自动 move，交给 C 释放）。 */
  sema_maybe_wrap_cfunc_init(sema, vd, scope, sym->type);

  /* §5 锚点解析基础：记录定义所在作用域（own_scope）。作用域标注
     '<p> ref *T 中的 p 解析为符号后取其 own_scope 作锚点作用域。 */
  sym->own_scope = scope;

  value_t *var_value;
  if (vd->init && vd->init->kind == AST_UNDEF) {
    /* 未初始化声明：var x:T = undefined。要求显式类型（undefined 无类型
       可推断）。flow_init=false（确定性赋值分析 UNKNOWN，TDZ），读取
       编译错误，只可赋值退出。 */
    if (!vd->type_expr) {
      diag_error(sema->diag, sema_loc(sema, vd->init),
                 "cannot infer type of uninitialized variable '%.*s'; "
                 "add an explicit type annotation",
                 (int)vd->name.len, vd->name.ptr);
    } else {
      var_type_slot_reparse(sema, vd, scope); /* 3a 推迟的槽位定义点兜底 */
    }
    sym->flow_init = false;
    var_value =
        value_make_shadow(sema->vm, sym->type ? sym->type : sema->vm->type_void);
  } else if (vd->init && vd->init->kind == AST_NIL) {
    /* ?T 声明初始化 none（docs m2-design §12.1）：var x:?T = nil。
       nil 非 value（无类型可推断），要求显式 ?T 类型标注；声明类型须
       optional。编译为 STORE_NIL（compiler 发码），运行期置 ok=false。
       确定性 = 已初始化（nil 是确定的值）。 */
    if (!vd->type_expr) {
      diag_error(sema->diag, sema_loc(sema, vd->init),
                 "cannot infer type of optional variable '%.*s' from nil; "
                 "add an explicit type annotation",
                 (int)vd->name.len, vd->name.ptr);
    } else {
      var_type_slot_reparse(sema, vd, scope);
      if (sym->type && type_kind_of(sym->type) != TYPE_KIND_OPTION) {
        char tn[64];
        sema_type_name(sym->type, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, vd->init),
                   "cannot initialize variable '%.*s' of type %s with nil "
                   "(only optional types accept nil)",
                   (int)vd->name.len, vd->name.ptr, tn);
      }
    }
    sym->flow_init = true; /* nil 是确定值（已初始化） */
    var_value =
        value_make_shadow(sema->vm, sym->type ? sym->type : sema->vm->type_void);
  } else {
    /* 已初始化：先求值 init（定义尚未入 VM scope → 自引用解析到外层同名变量）。
       匿名构造注入：显式声明类型已知 → push anon_ct，使 init 的 .{...}
       推断目标类型（var p: Point = .{ .x = 1 };）。 */
    const type_t *dt = NULL;
    if (vd->type_expr && sema->ex.anon_ct_depth < 16) {
      /* 类型槽位可能未解析（3a 推迟），先尝试解析出声明类型用于注入 */
      ast_node_t *save = vd->type_expr;
      const type_t *rt = resolve_type_expr(sema, vd->type_expr);
      if (rt) dt = rt;
      vd->type_expr = save; /* 不在此处折叠——3a/var_type_slot_reparse 负责 */
    }
    bool ct_pushed = sema_anon_push(sema, dt);
    value_t *init = sema_expr(sema, &vd->init, scope);
    sema_anon_pop(sema, ct_pushed);
    bool init_bad = value_is_error(sema->vm, init) ||
                    value_is_type(init, TYPE_KIND_VOID);
    /* 类型不是运行期值（语言语义）：`var t = i32;` / `var t = typeof(x);` 都
       不能把 type value 绑进变量——**绑类型用 `type <name> = <type expr>`**。
       此前这类写法被静默接受（符号类型 = 元类型 type），随后任何按"type value"
       消费它的地方（`sizeof(t)` / `const t` …）都会读空 data（实测 0xC0000005）。 */
    if (!init_bad && value_is_type(init, TYPE_KIND_TYPE)) {
      diag_error(sema->diag, sema_loc(sema, vd->init),
                 "cannot initialize '%.*s' with a type value: a type is not a "
                 "runtime value (bind it with `type <name> = <type expr>`)",
                 (int)vd->name.len, vd->name.ptr);
      init_bad = true;
    }
    /* 错误恢复产物（init 已诊断）不提升确定性 */
    sym->flow_init = !init_bad;

    /* 闭包值**拷贝** = 深拷贝捕获 env（值语义）：非 own 捕获的闭包可 copy，
       但两个变量不能共享同一个 env（闭包体可修改捕获）。字面量本身是新建
       实例，不需要再克隆。 */
    if (!init_bad && vd->init && vd->init->kind != AST_FUNC_DEF &&
        vd->init->kind != AST_TAKE) {
      bool already_dc = false;
      /* 只有**左值拷贝**才需要深拷贝；调用/构造/new/make 产物是新实例。 */
      bool from_lvalue =
          vd->init->kind == AST_IDENT || vd->init->kind == AST_MEMBER ||
          vd->init->kind == AST_INDEX || vd->init->kind == AST_DEREF ||
          vd->init->kind == AST_UNWRAP;
      if (vd->init->kind == AST_CALL) {
        ast_node_t *cal = ((ast_call_t *)vd->init)->callee;
        if (cal && cal->kind == AST_IDENT) {
          strslice_t cn = ((ast_ident_t *)cal)->name;
          already_dc = (cn.len == 12 && cn.ptr &&
                        memcmp(cn.ptr, "__deep_clone", 12) == 0);
        }
      }
      const type_t *it0 = value_type(init);
      const type_t *is = it0 ? type_strip_quals(it0) : NULL;
      bool needs_copy = false;
      if (from_lvalue && is) {
        if (type_kind_of(is) == TYPE_KIND_FUNC)
          needs_copy = func_type_capture_count(is) > 0;
        else
          needs_copy = type_has_closure_slots(is);
      }
      if (!already_dc && needs_copy) {
        ast_node_t *dc = sema_gen_deep_clone(sema, vd->init);
        if (dc) vd->init = dc;
      }
    }

    if (vd->type_expr) {
      /* 显式类型：3a 槽位解析可能失败（引用同块后续局部 type/被 var 遮蔽，
         当时未绑定 vm scope）→ 此处兜底重解析（局部 type 已在定义点求值
         绑定）；仍失败补报诊断（见 var_type_slot_reparse）。 */
      var_type_slot_reparse(sema, vd, scope);
      /* value_assign 校验 init 可赋给声明类型（单一校验点） */
      if (!init_bad && sym->type) {
        /* 运行期 opaque 指针不能变成函数值：只有 `cfunc_ptr(<命名 cfunc>)`
           这类编译期已知的 C 函数指针才能包装。 */
        const type_t *dt = type_strip_quals(sym->type);
        const type_t *it0 = value_type(init);
        const type_t *its = it0 ? type_strip_quals(it0) : NULL;
        if (dt && its && type_kind_of(dt) == TYPE_KIND_FUNC &&
            type_kind_of(its) == TYPE_KIND_OPAQUE) {
          diag_error(sema->diag, sema_loc(sema, vd->init),
                     "cannot convert a runtime `opaque` pointer to a function "
                     "value: only `cfunc_ptr(<named cfunc>)` is convertible");
          init_bad = true;
        }
      }
      if (!init_bad && sym->type) {
        value_t *dst = value_make_shadow(sema->vm, sym->type);
        if (value_is_error(sema->vm, value_assign(sema->vm, dst, init))) {
          char tn[64], itn[64];
          sema_type_name(sym->type, tn, sizeof(tn));
          sema_type_name(value_type(init), itn, sizeof(itn));
          diag_error(sema->diag, sema_loc(sema, vd->init),
                     "cannot initialize variable '%.*s' of type %s with %s",
                     (int)vd->name.len, vd->name.ptr, tn, itn);
        }
      }
      var_value =
          value_make_shadow(sema->vm, sym->type ? sym->type : sema->vm->type_void);
      } else {
      /* 推断类型：init 类型即变量类型。
         R1（own 不可 copy，§3.1）：var p = q; 且 init 是 own *T 且非
         fatal 接管 → copy → 编译错误（own 唯一所有权只能 move/clone）。 */
      const type_t *vt = init_bad ? sema->vm->type_void : value_type(init);
      sym->type = vt;
      /* 推断类型写回 AST：后端（C 降级/编译器）需要与 sema 完全相同的类型
         实例。闭包字面量的值类型是**闭包类型**（捕获槽位进入类型身份），
         若后端改用函数签名推断（func_type_of_def）会得到普通 func 类型，
         与字面量构造出的闭包 sig struct 不兼容（实测 C 编译报 incompatible
         struct type）。 */
      if (vt) vd->resolved_type = vt;
      var_value = value_make_shadow(sema->vm, vt);
      }
  }

  /* §5 逃逸检查（m3-design §5）：声明类型含 ref 指针（顶层 ref *T 或含
     ref 字段的 struct/tuple/union）时定义必须带作用域标注；带标注则做
     逃逸比较（ref 存活 ≤ 锚点存活）。类型未就绪（解析失败已诊断）跳过
     ——防级联。 */

  /* 定义 shadow value 到当前 VM scope（与 sema scope 树同构；名字取自 ast）。
     定义完成 → 符号激活（sema_lookup 跳过未激活符号，自引用解析到外层） */
  char nb[256];
  name_to_cstr(vd->name, nb, sizeof nb);
  scope_define(sema->vm, sema->vm->current_scope, nb, var_value);
  sym->is_active = true;
}
