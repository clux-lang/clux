#include "sema/sema.h"
#include "parser/ast_array.h"
#include "parser/ast_assign.h"
#include "parser/ast_binary.h"
#include "parser/ast_call.h"
#include "parser/ast_const.h"
#include "parser/ast_construct.h"
#include "parser/ast_func_type.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_block.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_for.h"
#include "parser/ast_func_def.h"
#include "parser/ast_if.h"
#include "parser/ast_return.h"
#include "parser/ast_ternary.h"
#include "parser/ast_type_def.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_unary.h"
#include "parser/ast_var_def.h"
#include "parser/ast_volatile.h"
#include "parser/ast_while.h"
#include "parser/lexer.h"
#include "sema/comptime.h"
#include "vm/type_array.h"
#include "vm/type_func.h"
#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * Pass 3b：Shadow VM 运行
 *
 * 按预建作用域树严格对应遍历 AST。每个块用独立局部索引遍历自己的
 * children：build 阶段子作用域按出现顺序 add_child，walk 必须逐层
 * 独立取用（共享索引会在嵌套时污染外层，导致后续作用域错位）。
 * =========================================================================== */

typedef struct block_result {
  bool definitely_returns; /* 该块保证返回（所有路径都 return） */
} block_result_t;

static block_result_t walk_block(sema_t *sema, ast_node_t *block,
                                 sema_scope_t *scope, size_t *idx);
static block_result_t walk_stmt(sema_t *sema, ast_node_t *stmt,
                                sema_scope_t *scope, size_t *idx);
static block_result_t walk_if(sema_t *sema, ast_if_t *it, sema_scope_t *scope,
                              size_t *idx);

/* ---- 确定性赋值分析（definite assignment analysis） ---- */

/*
 * 变量初始化状态（flow_init）挂在 sema_symbol_t 上，随 Pass 3b walk 更新：
 *   - var x = expr         → flow_init = true（定义即初始化）
 *   - var x:T = undefined  → flow_init = false（未初始化声明，TDZ）
 *   - x = expr             → flow_init = true（赋值退出未初始化）
 *   - if 合并点            → meet（AND）：两分支都 flow_init 才 true
 *   - while/for            → 循环体可能执行 0 次，体内赋值不提升确定性
 *
 * 分支模拟通过"快照 → 执行 → 恢复"实现：快照收集分支前可见符号的
 * flow_init，分支执行后取 then/else 交集写回（保守策略：非全部分支
 * 赋值 → 仍视为未初始化，读取报编译错误）。
 */

typedef struct flow_snap {
  allocator_t     *alloc;   /* 快照数组分配器（scope 链 alloc，共享 vm alloc） */
  sema_symbol_t  **syms;    /* 分支前可见的变量符号 */
  bool            *before;  /* 分支前 flow_init */
  bool            *after;   /* then 分支后 flow_init（meet 用） */
  size_t           n;
} flow_snap_t;

static flow_snap_t flow_capture(sema_t *sema, sema_scope_t *scope) {
  flow_snap_t snap = {0};
  allocator_t *alloc = scope ? scope->alloc : sema->vm->alloc;
  snap.alloc = alloc;

  /* 计数：沿 scope 链所有符号（含函数符号——函数 flow_init 恒 false，无害） */
  size_t cap = 0;
  for (const sema_scope_t *s = scope; s; s = s->parent)
    cap += strmap_size(s->symbols);
  if (cap == 0) return snap;

  snap.syms = allocator_new_ex(alloc, "flow_snap_syms", sizeof(sema_symbol_t *),
                               NULL, NULL, NULL, cap);
  snap.before = allocator_new_ex(alloc, "flow_snap_before", sizeof(bool), NULL,
                                 NULL, NULL, cap);
  snap.after = allocator_new_ex(alloc, "flow_snap_after", sizeof(bool), NULL,
                                NULL, NULL, cap);

  size_t i = 0;
  for (const sema_scope_t *s = scope; s; s = s->parent) {
    const vec_t *keys = strmap_keys(s->symbols);
    size_t nk = vec_len(keys);
    for (size_t k = 0; k < nk; k++) {
      const char *key = (const char *)vec_get(keys, k);
      sema_symbol_t *sym = (sema_symbol_t *)strmap_get(s->symbols, key);
      snap.syms[i] = sym;
      snap.before[i] = sym->flow_init;
      i++;
    }
  }
  snap.n = cap;
  return snap;
}

static void flow_restore(const flow_snap_t *snap) {
  for (size_t i = 0; i < snap->n; i++)
    snap->syms[i]->flow_init = snap->before[i];
}

static void flow_release(flow_snap_t *snap) {
  if (!snap || !snap->syms) return;
  allocator_free(snap->alloc, (void **)&snap->syms);
  allocator_free(snap->alloc, (void **)&snap->before);
  allocator_free(snap->alloc, (void **)&snap->after);
  snap->n = 0;
}

/* ---- 语句：变量定义 ---- */

/* strslice → NUL 终止临时缓冲（scope_define 内部复制 key，栈缓冲安全） */
static void name_to_cstr(strslice_t s, char *buf, size_t cap) {
  size_t n = s.len < cap - 1 ? s.len : cap - 1;
  memcpy(buf, s.ptr, n);
  buf[n] = '\0';
}

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

static void shadow_var_def(sema_t *sema, ast_var_def_t *vd,
                           sema_scope_t *scope) {
  sema_symbol_t *sym = sema_scope_find_local(scope, vd->name);
  if (!sym) return; /* 3a 重复定义已诊断，符号未注册 */

  /* comptime var：编译期求值 + 符号表编码（sema_eval_comptime_var）。
     定义点不进入 VM scope——调用方（walk_block）负责从语句链摘除；
     引用点在 sema_expr 折叠为字面量。 */
  if (vd->is_comptime) {
    sema_eval_comptime_var(sema, vd, scope);
    return;
  }

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
  } else {
    /* 已初始化：先求值 init（定义尚未入 VM scope → 自引用解析到外层同名变量） */
    value_t *init = sema_expr(sema, &vd->init, scope);
    bool init_bad = value_is_error(sema->vm, init) ||
                    value_is_type(init, TYPE_KIND_VOID);
    /* 错误恢复产物（init 已诊断）不提升确定性 */
    sym->flow_init = !init_bad;

    if (vd->type_expr) {
      /* 显式类型：3a 槽位解析可能失败（引用同块后续局部 type/被 var 遮蔽，
         当时未绑定 vm scope）→ 此处兜底重解析（局部 type 已在定义点求值
         绑定）；仍失败补报诊断（见 var_type_slot_reparse）。 */
      var_type_slot_reparse(sema, vd, scope);
      /* value_assign 校验 init 可赋给声明类型（单一校验点） */
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
      /* 推断类型：init 类型即变量类型 */
      const type_t *vt = init_bad ? sema->vm->type_void : value_type(init);
      sym->type = vt;
      var_value = value_make_shadow(sema->vm, vt);
    }
  }

  /* 定义 shadow value 到当前 VM scope（与 sema scope 树同构；名字取自 ast）。
     定义完成 → 符号激活（sema_lookup 跳过未激活符号，自引用解析到外层） */
  char nb[256];
  name_to_cstr(vd->name, nb, sizeof nb);
  scope_define(sema->vm, sema->vm->current_scope, nb, var_value);
  sym->is_active = true;
}

/* ---- 语句：赋值 ---- */

/* 复合赋值 token（+= 等）→ 基础二元运算 */
static value_t *(*compound_binop(const token_t *op))(vm_t *, value_t *,
                                                     value_t *) {
  if (token_is(op, "+=")) return value_add;
  if (token_is(op, "-=")) return value_sub;
  if (token_is(op, "*=")) return value_mul;
  if (token_is(op, "/=")) return value_div;
  if (token_is(op, "%=")) return value_mod;
  return NULL;
}

static void shadow_assign_index(sema_t *sema, ast_assign_t *as,
                                sema_scope_t *scope);

static void shadow_assign(sema_t *sema, ast_assign_t *as,
                          sema_scope_t *scope) {
  /* 左值：下标表达式 a[i] = v（sema 校验下标合法） */
  if (as->target->kind == AST_INDEX) {
    shadow_assign_index(sema, as, scope);
    return;
  }
  /* 左值必须是标识符表达式（目前仅支持 ID_LIT） */
  if (as->target->kind != AST_IDENT) {
    diag_error(sema->diag, sema_loc(sema, as->target),
               "invalid assignment target");
    sema_expr(sema, &as->value, scope);
    return;
  }
  strslice_t name = ((ast_ident_t *)as->target)->name;

  /* 显式丢弃：_ = expr（不查符号表，直接求值右值） */
  if (strslice_eq(name, STRSLICE_LIT("_"))) {
    if (!token_is(as->op, "=")) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "discard '_' only supports simple assignment '='");
    }
    sema_expr(sema, &as->value, scope);
    return;
  }

  /* 左值从 VM scope 链 lookup（与 sema 作用域树同构） */
  value_t *lhs = scope_lookup(sema->vm->current_scope, name);
  if (!lhs) {
    /* comptime var 不在 VM scope：赋值给编译期常量 → 编译错误 */
    sema_symbol_t *sym = sema_lookup(scope, name);
    if (sym && sym->is_comptime && sym->ct_valid) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to compile-time constant '%.*s'",
                 (int)name.len, name.ptr);
      return;
    }
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "undefined variable '%.*s' in assignment", (int)name.len,
               name.ptr);
    return;
  }

  value_t *rhs = sema_expr(sema, &as->value, scope);
  bool rhs_bad = value_is_error(sema->vm, rhs) ||
                 value_is_type(rhs, TYPE_KIND_VOID);

  if (token_is(as->op, "=")) {
    if (rhs_bad) return; /* 错误恢复产物跳过，已有诊断 */

    /* const 赋值检查（TDZ 豁免）：声明 const 且已初始化（flow_init=true）
       的变量不可再赋值；flow_init=false（未初始化声明 var a:const T =
       undefined）时的赋值是首次初始化，豁免（const 变量的 TDZ 赋值 =
       初始化，仅一次）。经 value 层接口查询 const（type is value）。 */
    sema_symbol_t *sym = sema_lookup(scope, name);
    if (sym && value_has_const(lhs) && sym->flow_init) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to const variable '%.*s'",
                 (int)name.len, name.ptr);
      return;
    }

    /* 简单赋值：value_assign 校验；赋值成功 → 数据流 flow_init=true
       （TDZ 退出由确定性赋值分析承担，VM 值层不感知） */
    value_t *r = value_assign(sema->vm, lhs, rhs);
    if (value_is_error(sema->vm, r)) {
      char tn[64], rn[64];
      sema_type_name(value_type(lhs), tn, sizeof(tn));
      sema_type_name(value_type(rhs), rn, sizeof(rn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign %s to variable '%.*s' of type %s", rn,
                 (int)name.len, name.ptr, tn);
    } else {
      if (sym) sym->flow_init = true;
    }
    return;
  }

  /* const 检查：复合赋值是读+写，const 变量已初始化后禁止
     （经 value 层接口查询 const，type is value） */
  {
    sema_symbol_t *sym = sema_lookup(scope, name);
    if (sym && value_has_const(lhs) && sym->flow_init) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to const variable '%.*s'",
                 (int)name.len, name.ptr);
      return;
    }
  }

  /* 复合赋值 x op= rhs → x = x op rhs（shadow 走 vtable 类型协商） */
  value_t *(*op)(vm_t *, value_t *, value_t *) = compound_binop(as->op);
  if (!op) {
    char ob[16];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "unsupported compound assignment operator '%s'", ob);
    return;
  }
  value_t *result = op(sema->vm, lhs, rhs);
  if (value_is_error(sema->vm, result)) {
    char ob[16], tn[64], rn[64];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    sema_type_name(value_type(lhs), tn, sizeof(tn));
    sema_type_name(value_type(rhs), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "type mismatch: cannot apply '%s' to %s and %s", ob, tn, rn);
    return;
  }
  /* 复合赋值结果必须能赋回变量（value_assign 单一校验点） */
  if (value_is_error(sema->vm, value_assign(sema->vm, lhs, result))) {
    char tn[64], rn[64];
    sema_type_name(value_type(lhs), tn, sizeof(tn));
    sema_type_name(value_type(result), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign %s to variable '%.*s' of type %s", rn,
               (int)name.len, name.ptr, tn);
  } else {
    /* 复合赋值等价于读+写：变量确定已初始化 */
    sema_symbol_t *sym = sema_lookup(scope, name);
    if (sym) sym->flow_init = true;
  }
}

/* ---- 下标左值赋值：a[i] = v / a[i] op= v ---- */

/*
 * 校验 base 可下标（数组）、索引为整数、元素类型可赋值（value_assign
 * 单一校验点）。元素非独立符号，不写回 flow_init（无 TDZ 概念）。
 * 泛型实例化（base 是类型值）不落入此路径——sema_expr 已报占位诊断。
 * 复合赋值走同一元素类型校验（op 结果可赋回元素）。
 */
static void shadow_assign_index(sema_t *sema, ast_assign_t *as,
                                sema_scope_t *scope) {
  ast_index_t *ix = (ast_index_t *)as->target;

  /* base 求值 + 可下标校验（多维 m[i][j] = v：ix->object 是 AST_INDEX，
     sema_expr 递归走右值下标分支返回元素类型，此处对最外层索引校验；
     vm 层借用引用让写回直达原数组，见 type_array.c 借用引用支持） */
  value_t *base = sema_expr(sema, &ix->object, scope);
  bool bad = value_is_error(sema->vm, base) ||
             value_is_type(base, TYPE_KIND_VOID);
  const type_t *bt = bad ? NULL : value_type(base);
  if (!bad && (!bt || bt->kind != TYPE_KIND_ARRAY)) {
    char tn[64];
    sema_type_name(bt, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "invalid assignment target: cannot index value of type %s", tn);
    bad = true;
  }

  /* 单索引校验（多索引 = 泛型实参语法预留） */
  if (!bad) {
    size_t nidx = sema_count_siblings(ix->indices);
    if (nidx != 1) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "array subscript expects exactly 1 index, got %zu", nidx);
      bad = true;
    }
  }

  /* 索引表达式求值 + 整数校验 */
  if (!bad) {
    value_t *idx = sema_expr(sema, &ix->indices, scope);
    if (value_is_error(sema->vm, idx) ||
        value_is_type(idx, TYPE_KIND_VOID))
      bad = true;
    else if (!value_is_type(idx, TYPE_KIND_INT)) {
      char tn[64];
      sema_type_name(value_type(idx), tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, ix->indices),
                 "array index must be an integer, got %s", tn);
      bad = true;
    }
  }

  value_t *rhs = sema_expr(sema, &as->value, scope);
  if (bad) return; /* 已有诊断，右值已求值（错误恢复） */
  if (value_is_error(sema->vm, rhs) ||
      value_is_type(rhs, TYPE_KIND_VOID))
    return;

  /* const 数组元素不可写 */
  if (value_has_const(base)) {
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign to element of const array");
    return;
  }

  /* 元素类型可赋值性（value_assign 单一校验点） */
  const type_t *et = array_type_elem(bt);
  if (!et) return;
  value_t *dst = value_make_shadow(sema->vm, et);
  if (value_is_error(sema->vm, value_assign(sema->vm, dst, rhs))) {
    char tn[64], rn[64];
    sema_type_name(et, tn, sizeof(tn));
    sema_type_name(value_type(rhs), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, as->value),
               "cannot assign %s to array element of type %s", rn, tn);
  }
}

/* ---- 语句：控制流 ---- */

static block_result_t walk_return(sema_t *sema, ast_return_t *rt,
                                  sema_scope_t *scope) {
  block_result_t r = {.definitely_returns = true};
  if (rt->value) {
    value_t *v = sema_expr(sema, &rt->value, scope);
    bool v_bad = value_is_error(sema->vm, v) ||
                 value_is_type(v, TYPE_KIND_VOID);
    if (!v_bad) {
      if (!sema->func_return_type) {
        diag_error(sema->diag, sema_loc(sema, rt->value),
                   "void function cannot return a value");
      } else {
        /* value_assign 校验返回类型可赋给签名返回类型 */
        value_t *dst = value_make_shadow(sema->vm, sema->func_return_type);
        if (value_is_error(sema->vm, value_assign(sema->vm, dst, v))) {
          char tn[64], rn[64];
          sema_type_name(sema->func_return_type, tn, sizeof(tn));
          sema_type_name(value_type(v), rn, sizeof(rn));
          diag_error(sema->diag, sema_loc(sema, rt->value),
                     "cannot return %s from function returning %s", rn, tn);
        }
      }
    }
  } else {
    if (sema->func_return_type) {
      char tn[64];
      sema_type_name(sema->func_return_type, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &rt->base),
                 "function returning %s must return a value", tn);
    }
  }
  sema->func_has_return = true;
  return r;
}

static block_result_t walk_while(sema_t *sema, ast_while_t *wl,
                                 sema_scope_t *scope, size_t *idx) {
  value_t *cond = sema_expr(sema, &wl->cond, scope);
  sema_check_bool(sema, wl->cond, cond, "while condition");

  /* 循环体可能执行 0 次：体内赋值不提升外层变量的确定性（保守） */
  flow_snap_t snap = flow_capture(sema, scope);

  sema_scope_t *body_scope = sema_scope_child(scope, (*idx)++);
  vm_push_scope(sema->vm); /* 循环体块：VM scope 与 sema scope 树同构 */
  size_t sub = 0;
  walk_block(sema, wl->body, body_scope ? body_scope : scope, &sub);
  vm_pop_scope(sema->vm);

  flow_restore(&snap);
  flow_release(&snap);
  return (block_result_t){0}; /* 循环体可能不执行，不贡献 definitely_returns */
}

static block_result_t walk_for(sema_t *sema, ast_for_t *fr,
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
  vm_pop_scope(sema->vm);

  if (fr->update) sema_expr(sema, &fr->update, fs);

  flow_restore(&snap); /* 丢弃体内确定性提升（保守） */
  flow_release(&snap);

  vm_pop_scope(sema->vm); /* 退出 for 作用域 */
  return (block_result_t){0};
}

static block_result_t walk_if(sema_t *sema, ast_if_t *it, sema_scope_t *scope,
                              size_t *idx) {
  block_result_t r = {0};
  value_t *cond = sema_expr(sema, &it->cond, scope);
  sema_check_bool(sema, it->cond, cond, "if condition");

  /* 确定性赋值合并点：快照分支前状态 → then → 记录 → 恢复 → else →
     meet（AND）：两分支都 flow_init 才 true。保守策略：非全部分支赋值
     （如 if(c){a=1;}else{}）→ 合并后仍 UNKNOWN，读取编译错误。 */
  flow_snap_t snap = flow_capture(sema, scope);

  block_result_t tr = {0};
  sema_scope_t *then_scope = sema_scope_child(scope, (*idx)++);
  vm_push_scope(sema->vm); /* then 块 */
  size_t sub = 0;
  tr = walk_block(sema, it->then_body, then_scope ? then_scope : scope, &sub);
  vm_pop_scope(sema->vm);

  /* 记录 then 后状态，恢复分支前 */
  for (size_t i = 0; i < snap.n; i++) snap.after[i] = snap.syms[i]->flow_init;
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

/* ---- 语句分派 ---- */

/* 索引语义：idx 是"当前 scope"的 children 迭代器。一个块内的语句按序
   消费当前 scope 的子节点（sema_scope_child(scope, (*idx)++)），进入
   嵌套子 scope（BLOCK/if then/else/while body/for body）时用新局部
   迭代器遍历子 scope 的 children——与原版共享外层迭代器相比，嵌套消费
   不再污染浅层（第二个 for 的 init 变量因此找不到定义点）。 */

static block_result_t walk_stmt(sema_t *sema, ast_node_t *stmt,
                                sema_scope_t *scope, size_t *idx) {
  block_result_t r = {0};
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
    case AST_WHILE:
      r = walk_while(sema, (ast_while_t *)stmt, scope, idx);
      break;
    case AST_FOR:
      r = walk_for(sema, (ast_for_t *)stmt, scope, idx);
      break;
    case AST_RETURN:
      r = walk_return(sema, (ast_return_t *)stmt, scope);
      break;
    case AST_BREAK:
    case AST_CONTINUE:
      break; /* 位置检查已在 Pass 3a 完成 */
    case AST_FUNC_DEF:
      /* 局部函数定义：入口提升已处理签名，主循环（walk_block）已跳过并
         消费 fscope 子作用域。防御分支（非块上下文直接调用时零操作）。 */
      break;
    case AST_EXPR_STMT: {
      ast_expr_stmt_t *es = (ast_expr_stmt_t *)stmt;
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
      break;
    }
    default:
      break;
  }
  return r;
}

/* ---- 提升遮蔽预检（局部 type 定义） ----
 *
 * 提升语义下 type RHS 在块入口求值（早于块内 var 绑定到 VM scope），
 * 若 RHS 引用"块内声明序靠前的 var 名"会错误落到外层同名 type/未定义。
 * 入口提升循环按声明序累积 prior_vars，此处递归检查 RHS 的 AST_IDENT
 * 引用（未折叠的名字，折叠产物 AST_TYPE_REF 不在此列）是否命中——
 * 命中即 var 完全遮罩（遮蔽平等），报错并跳过求值。 */

static const strslice_t *type_rhs_uses_var(const ast_node_t *e,
                                           const strslice_t *prior_vars,
                                           size_t prior_n) {
  if (!e) return NULL;
  if (e->kind == AST_IDENT) {
    for (size_t i = 0; i < prior_n; i++) {
      if (prior_vars[i].len == ((ast_ident_t *)e)->name.len &&
          memcmp(prior_vars[i].ptr, ((ast_ident_t *)e)->name.ptr,
                 prior_vars[i].len) == 0)
        return &prior_vars[i];
    }
    return NULL;
  }
  const strslice_t *hit = NULL;
  switch (e->kind) {
    case AST_ARRAY:
      hit = type_rhs_uses_var(((ast_array_t *)e)->base_type, prior_vars,
                              prior_n);
      if (hit) return hit;
      return type_rhs_uses_var(((ast_array_t *)e)->length, prior_vars,
                               prior_n);
    case AST_CONST:
      return type_rhs_uses_var(((ast_const_t *)e)->sub, prior_vars, prior_n);
    case AST_VOLATILE:
      return type_rhs_uses_var(((ast_volatile_t *)e)->sub, prior_vars,
                               prior_n);
    case AST_BINARY:
      hit = type_rhs_uses_var(((ast_binary_t *)e)->lhs, prior_vars, prior_n);
      if (hit) return hit;
      return type_rhs_uses_var(((ast_binary_t *)e)->rhs, prior_vars, prior_n);
    case AST_UNARY:
      return type_rhs_uses_var(((ast_unary_t *)e)->operand, prior_vars,
                               prior_n);
    case AST_TERNARY: {
      hit = type_rhs_uses_var(((ast_ternary_t *)e)->cond, prior_vars,
                              prior_n);
      if (hit) return hit;
      hit = type_rhs_uses_var(((ast_ternary_t *)e)->then_branch, prior_vars,
                              prior_n);
      if (hit) return hit;
      return type_rhs_uses_var(((ast_ternary_t *)e)->else_branch, prior_vars,
                               prior_n);
    }
    case AST_CALL: {
      hit = type_rhs_uses_var(((ast_call_t *)e)->callee, prior_vars,
                              prior_n);
      if (hit) return hit;
      for (ast_node_t *a = ((ast_call_t *)e)->args; a; a = a->next) {
        hit = type_rhs_uses_var(a, prior_vars, prior_n);
        if (hit) return hit;
      }
      return NULL;
    }
    case AST_FUNC_TYPE: {
      for (ast_node_t *p = ((ast_func_type_t *)e)->params; p; p = p->next) {
        hit = type_rhs_uses_var(p, prior_vars, prior_n);
        if (hit) return hit;
      }
      return type_rhs_uses_var(((ast_func_type_t *)e)->return_type,
                               prior_vars, prior_n);
    }
    case AST_CONSTRUCT: {
      hit = type_rhs_uses_var(((ast_construct_t *)e)->type, prior_vars,
                              prior_n);
      if (hit) return hit;
      for (ast_node_t *f = ((ast_construct_t *)e)->fields; f; f = f->next) {
        hit = type_rhs_uses_var(f, prior_vars, prior_n);
        if (hit) return hit;
      }
      return NULL;
    }
    case AST_TYPE_REF:
    default:
      return NULL; /* 折叠产物 / 无子节点字面量 */
  }
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

  /* 参数符号 type 补全（3a 建树时解析失败的局部类型槽位） */
  sema_func_t *sf = sema_func_find_by_def(sema, (ast_node_t *)fn);
  if (sf && sf->scope) {
    size_t j = 0;
    for (ast_node_t *p = fn->params; p; p = p->next, j++) {
      ast_var_def_t *vd = (ast_var_def_t *)p;
      sema_symbol_t *ps = sema_scope_find_local(sf->scope, vd->name);
      if (ps && !ps->type) ps->type = params ? params[j] : NULL;
    }
  }

  if (params) allocator_free(sema->vm->alloc, (void **)&params);
}

static block_result_t walk_block(sema_t *sema, ast_node_t *block,
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
      if (prior_n < 64) {
        prior_vars[prior_n++] = ((ast_var_def_t *)s)->name;
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
    if (s->kind == AST_TYPE_DEF) { /* 入口提升已处理 */
      prev = &s->next;
      s = s->next;
      continue;
    }
    if (s->kind == AST_FUNC_DEF) {
      /* 入口提升已处理签名；消费 3a 建的 fscope 子作用域（comptime / 被拒
         无子作用域）——与 3a 建树严格对齐。 */
      ast_func_def_t *fn = (ast_func_def_t *)s;
      if (!fn->is_comptime && sema_scope_find_local(scope, fn->name)) (*idx)++;
      prev = &s->next;
      s = s->next;
      continue;
    }
    block_result_t sr = walk_stmt(sema, s, scope, idx);
    if (sr.definitely_returns) {
      r.definitely_returns = true;
      break; /* 之后的语句不可达，不再检查 */
    }
    if (s->kind == AST_VAR_DEF && ((ast_var_def_t *)s)->is_comptime) {
      *prev = s->next; /* 摘除定义点 */
      s = s->next;
      continue;
    }
    prev = &s->next;
    s = s->next;
  }
  return r;
}

/* ---- 函数入口 ---- */

void sema_walk_function(sema_t *sema, sema_func_t *sf) {
  ast_func_def_t *fn = (ast_func_def_t *)sf->def;
  if (!sf->scope) return; /* 建树失败（结构错误已诊断），不进入 shadow run */

  /* 捕获检查上下文：局部函数体引用外层局部符号 → 报错（无闭包，运行时
     函数体查找链只有参数 + 全局）。全局函数不设（fscope parent = 全局，
     无外层局部可捕获）。 */
  sema->local_func_base = sf->is_local ? sf->scope : NULL;

  sema->func_return_type =
      fn->return_expr ? sema_resolve_type_slot(sema, &fn->return_expr) : NULL;
  sema->func_has_return = false;

  /* 函数级 VM scope（与 fscope 同构）：参数 shadow value 定义到此处，
     进入函数体即可读（名字取自 ast） */
  vm_push_scope(sema->vm);
  for (ast_node_t *p = fn->params; p; p = p->next) {
    ast_var_def_t *vd = (ast_var_def_t *)p;
    sema_symbol_t *ps = sema_scope_find_local(sf->scope, vd->name);
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

  /* 返回路径完整性分析已在 Pass 3a（建树阶段）完成；
     walk_block 的 block_result_t 仅用于跳过不可达语句的类型检查 */
  size_t child_idx = 0;
  (void)walk_block(sema, fn->body, sf->scope, &child_idx);
  vm_pop_scope(sema->vm);

  /* 返回路径完整性分析已在 Pass 3a（建树阶段）完成 */
  sema->func_return_type = NULL;
  sema->local_func_base = NULL;
}

/* 函数字面量 body walk 入口（stmt_build.c 的 sema_check_func_literal 用；
   walk_block 是 static，此处暴露薄封装）。idx 内部自持——临时作用域树
   由 sema_check_func_literal 独立构建，消费顺序与建树严格对齐。 */
void sema_walk_block(sema_t *sema, ast_node_t *block, sema_scope_t *scope) {
  size_t child_idx = 0;
  (void)walk_block(sema, block, scope, &child_idx);
}
