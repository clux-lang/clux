#include "sema/sema.h"
#include "vm/type_ownership.h"
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

/* ---- Step B 所有权检查辅助（m3-design §3/§7） ---- */

/* R1/R2 判定：赋值 rhs 是否是 own 指针的合法初始化源。
   own *T 变量只允许两种初始化来源：
   - new 表达式（AST_NEW）：新堆块直接产 own *T（§8.1）
   - fatal 接管源（move/clone 产物或 fatal 参数，§3.3）
   ref 派生（own → ref 隐式转换）是借用（ref 总借用），不能赋回 own。
   own → own 直接赋值 = copy → 编译错误。
   非 static：AST_NEW/AST_CONSTRUCT 字段校验（sema/expr.c）共用。 */

#include "sema_stmt_internal.h"

/* 标注锚点解析（m3-design §5 检查算法）：标注名字 → 锚点作用域。
   - "*"   → global（global 作用域，恒放行：全局值存活到程序结束）
   - 参数名 → 参数层作用域（参数整个函数体期间存活，恒放行）
   - 局部变量名 → 该符号定义所在作用域（own_scope，3b 定义点记录）
   未解析（非参数/全局/局部变量名）返回 NULL（错误由调用方报）。 */
sema_scope_t *annot_anchor_scope(sema_t *sema, strslice_t name,
                                        sema_scope_t *scope) {
  if (name.len == 1 && name.ptr[0] == '*') return sema->global_scope;

  /* 参数判定：遍历当前函数真实参数列表（参数符号在 param_scope 直系，但
     函数体顶层变量也注册在 param_scope，sema_scope_find_local 无法区分）。 */
  if (sema->fn.def) {
    for (ast_node_t *p = sema->fn.def->params; p; p = p->next) {
      ast_var_def_t *pvd = (ast_var_def_t *)p;
      if (pvd->name.len == name.len &&
          memcmp(pvd->name.ptr, name.ptr, name.len) == 0)
        return sema->fn.local_func_param_scope;
    }
  }

  /* 局部变量：沿 scope 链解析符号，取其定义作用域（own_scope 3b 记录；
     未走到定义点的符号 own_scope 为 NULL → 锚点解析失败，调用方报错）。 */
  sema_symbol_t *sym = sema_lookup(scope, name);
  if (sym && sym->kind == SEMA_SYM_VAR) return sym->own_scope;
  return NULL;
}

/* §5 逃逸比较（m3-design §5 检查算法：ref 的作用域 ≤ own 的作用域）：
   持有 ref 的变量（own_scope = 变量定义作用域，即 ref 的推断存活）须被
   标注集合的 min 锚点涵盖。**AND 语义**：'<a,b,c> = 存活不超过 min(a,b,c)，
   每个锚点都必须涵盖 ref 定义作用域（任一不涵盖 → 逃逸 → 悬垂）。参数/
   全局锚点恒成立（scope_within 天然处理：函数体内所有块 ⊆ 参数层，所有
   作用域 ⊆ global 根）。 */
static void check_annot_escape(sema_t *sema, ast_node_t *at, sema_scope_t *own_scope,
                               strslice_t *names, size_t count) {
  if (count == 0) return;
  for (size_t i = 0; i < count; i++) {
    sema_scope_t *anchor = annot_anchor_scope(sema, names[i], own_scope);
    if (!anchor) {
      diag_error(sema->diag, sema_loc(sema, at),
                 "scope annotation '%.*s' does not name a parameter, global "
                 "or local variable",
                 (int)names[i].len, names[i].ptr);
      continue;
    }
    if (!sema_scope_within(own_scope, anchor)) {
      diag_error(sema->diag, sema_loc(sema, at),
                 "variable escapes scope annotation '%.*s' (its scope is not "
                 "nested within the annotated scope)",
                 (int)names[i].len, names[i].ptr);
    }
  }
}

/* §5 逃逸检查统一入口（shadow_var_def 调用）：声明类型含 ref 指针（顶层
   ref *T 或含 ref 字段的 struct/tuple/union）时做逃逸比较。
   - 显式标注（顶层 AST_SCOPE_ANNOT）→ 锚点集合 = 标注名字（'<p> ref *T
     借自 p；'<*> = global）
   - 未标注 → 按定义作用域推断（§5"推断：ref 变量未显式标注时推断为
     当前定义作用域"）→ 锚点 = 借源对象：init 直读变量（var r: ref *i32
     = p）取该符号定义作用域；init 非直读（x.&、函数调用返回、new 产物）
     无静态可判借源 → 跳过（ref 存活 = 当前作用域，逃逸点由 §6/调用点
     检查兜底）。
   - §5 强制规则 B：含 ref 字段的聚合类型（struct/tuple/union/cunion 等
     非顶层 ref）变量定义**必须标注**——聚合内 ref 字段借源无法从 init
     静态推断（构造器可能多源），强制用户显式声明。顶层 ref *T 仍走推断
     语义（单源可判）。 */
static void check_var_annot(sema_t *sema, ast_var_def_t *vd,
                            sema_scope_t *scope, const type_t *vt,
                            sema_symbol_t *sym) {
  /* 只有**顶层 ref**（sema_sym_ownership(sym) == OWN_REF）需要标注检查与 annot 记录
     ——重绑定逃逸校验依赖 annot_count > 0。
     注：含 ref 字段的聚合类型已不支持（复合类型禁止携带指针/切片），
     所以不再有类型层的 `type_contains_ref` 判据。 */
  if (!vt || !(sym && sema_sym_ownership(sym) == OWN_REF))
    return;

  strslice_t *names = NULL;
  size_t count = 0;
  sema_annot_peek(vd->type_expr, &names, &count);
  if (count == 0) {
    /* §5 强制规则 B：含 ref 字段的聚合类型（非顶层 ref）必须标注。
       顶层 ref *T 走推断语义（不强制）。 */
    if (!sym || sema_sym_ownership(sym) != OWN_REF) {
      char tn[64];
      sema_type_name(vt, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, vd->type_expr ? vd->type_expr
                                                           : (ast_node_t *)vd),
                 "variable of type '%s' contains ref fields and must declare a "
                 "scope annotation (e.g. '<a> %s)",
                 tn, tn);
      return;
    }
    if (!vd->init) return;
    if (vd->init->kind == AST_IDENT) {
      /* 推断：init 直读变量（var r: ref *i32 = p）→ 锚点 = 借源变量 */
      names = &((ast_ident_t *)vd->init)->name;
      count = 1;
    } else if (vd->init->kind == AST_MEMBER) {
      /* §5 字段访问继承（m3-design §5"var p = s.aaa; 中 p 继承 s 的
         作用域绑定"）：init 是 s.a（object 直读变量 s）→ 继承 s 的
         标注集合（p 借自 s 的字段，p 存活 ≤ s 存活）。s 无标注（推断
         或未含 ref）→ 跳过（s 自身无约束可继承）。 */
      ast_member_t *m = (ast_member_t *)vd->init;
      if (!m->object || m->object->kind != AST_IDENT) return;
      sema_symbol_t *obj_sym =
          sema_lookup(scope, ((ast_ident_t *)m->object)->name);
      if (!obj_sym || obj_sym->annot_count == 0) return;
      names = obj_sym->annot_names;
      count = obj_sym->annot_count;
    } else {
      return;
    }
  }
  /* 记录标注集合到符号（重绑定逃逸校验用）。显式标注 names 指向
     ast_scope_annot_t->names（arena 管理）；推断分支 names 指向
     ast_ident_t->name（arena 管理）——均随 AST 存活，重绑定时可安全读取。 */
  if (sym) {
    sym->annot_names = names;
    sym->annot_count = count;
  }
  check_annot_escape(sema, vd->type_expr ? vd->type_expr : vd->init,
                     scope, names, count);
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

/* 从 fatal 类型剥离 OWN_FATAL：
 * - 复合类型（struct/tuple/array/option/union）→ OWN_NONE（fatal T → T）
 * - 指针/切片 → OWN_OWN（fatal *T → own *T，fatal []T → own []T）
 *   fatal 是将亡值，只能赋值给 own（move/clone/new 产物），所以自动推断
 *   为 own 而非要求显式标注。
 * 返回 NULL = 非 fatal 类型或无法处理。 */
static const type_t *strip_fatal(sema_t *sema, const type_t *t) {
  if (!sema || !t) return t;
  if (type_ownership_of(t) != OWN_FATAL) return t;
  const type_t *core = type_strip_ownership(t);
  if (!core) return NULL;
  /* fatal 指针/切片/闭包/含 own 槽位的聚合：变量接管为 own；
     标量/无 own 槽位的复合类型：fatal 无意义，落回裸类型。 */
  bool resource = type_is_ptr(core) || type_is_slice(core) ||
                  type_kind_of(core) == TYPE_KIND_FUNC ||
                  type_has_own_slots(core);
  return resource ? type_ownership_apply(sema->vm, OWN_OWN, core) : core;
}

/* 该表达式是否"含 own 捕获的闭包"（穿透 move/clone）。
   所有权模型：闭包 = 捕获结构体 + 函数实现，捕获了 own 的闭包**本身即 OWN**
   （唯一的 own 产生式——函数定义把 fatal→move→own 折叠了）。
   因此 `var f = <closure>` 被拒（OWN 不能给 NONE 位置），必须
   `var f = move(<closure>)`：move 产 FATAL → 变量推断为 own 绑定。
   捕获槽的所有权由 resolve_func_captures 回写进 `ast_var_def_t::ownership`。 */
static const ast_func_def_t *own_closure_of(ast_node_t *e) {
  if (!e) return NULL;
  if (e->kind == AST_MOVE) return own_closure_of(((ast_move_t *)e)->operand);
  if (e->kind != AST_FUNC_DEF) return NULL;
  ast_func_def_t *fd = (ast_func_def_t *)e;
  for (ast_node_t *c = fd->captures; c; c = c->next)
    if (type_ownership_of(((ast_var_def_t *)c)->resolved_type) == OWN_OWN)
      return fd;
  return NULL;
}

/* ===========================================================================
 * `cfunc(...)` → `func(...)` 隐式转换
 *
 * 语义（用户定）：cfunc 形参上的所有权就是**C 侧所有权契约**——
 *   - `own`：C 函数内部会释放 → 包装体必须 `move(p)` 交出去（否则 clux 与 C
 *     各释放一次 = double free）；
 *   - `ref`：C 内部不释放 → 直接传引用；clux 侧继续持有并回收。
 * 外层 func 的类型**由 cfunc 声明生成**，因此两侧签名（含所有权）必然一致，
 * 不需要处理不匹配组合。
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
  const func_sig_t *sig = &((const func_type_t *)type_strip_ownership(target))->sig;
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
    /* 所有权现在编码在类型上（AST 折叠后也能读回）。 */
    ownership_t src_own = type_ownership_of(src_pt);
    ownership_t tgt_own = type_ownership_of(tgt_pt);
    if (!src_pt || !tgt_pt || src_pt != tgt_pt || src_own != tgt_own) {
      diag_error(sema->diag, sema_loc(sema, origin),
                 "cannot convert cfunc(...) to func(...): parameter %zu of the "
                 "cfunc has a different type or ownership",
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
    if (tgt_own == OWN_OWN) {
      /* own 形参：C 内部会释放 → 必须 move 交出所有权。 */
      location_t loc = sema_loc(sema, origin);
      ast_node_t *mvn = ast_move_new(sema->arena, tb, te);
      if (!mvn) return NULL;
      ((ast_move_t *)mvn)->op =
          (token_t *)arena_token_symbol(sema->arena, "move", &loc);
      ((ast_move_t *)mvn)->operand = a;
      a = mvn;
      a->is_generated = true;
    }
    if (args_last) args_last->next = a; else args = a;
    args_last = a;
  }

  const type_t *tgt_ret = func_type_return(target);
  const type_t *src_ret =
      src->return_expr ? sema_resolve_type_slot(sema, &src->return_expr) : NULL;
  ownership_t src_ret_own = type_ownership_of(src_ret);
  if (tgt_ret != src_ret ||
      src_ret_own != type_ownership_of(tgt_ret)) {
    diag_error(sema->diag, sema_loc(sema, origin),
               "cannot convert cfunc(...) to func(...): return type or "
               "ownership differs");
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
     参数符号就不会注册，函数体里的 `move(__cfp0)` 解析不到符号。 */
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
      ownership_t vo = type_ownership_of(vt);
      if (!vt) {
        diag_error(sema->diag, sema_loc(sema, vd->init),
                   "error-handling sugar has no value type");
        return;
      }
      /* own/fatal 结果：变量声明为 own 核心类型，赋值时接受 fatal 源。 */
      const type_t *decl = vt;
      if (vo == OWN_OWN || vo == OWN_FATAL || type_has_own_slots(vt)) {
        decl = type_ownership_apply(sema->vm, OWN_OWN,
                                    type_strip_ownership(vt));
        if (!decl) return;
      }
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
  if (sema->vm->test_mode && !vd->is_comptime && sym->type &&
      (sema_sym_ownership(sym) == OWN_OWN ||
       type_has_own_slots(type_strip_ownership(sym->type))))
    vd->free_on_error = true;

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
    value_set_type_ownership(sema->vm, var_value, sema_sym_ownership(sym));
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
    value_set_type_ownership(sema->vm, var_value, sema_sym_ownership(sym));
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
        vd->init->kind != AST_MOVE && vd->init->kind != AST_TAKE) {
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
      if (from_lvalue && is && !type_has_own_slots(is)) {
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
      /* R1（fatal 禁止命名，§3.3）：fatal *T 只能作为 move/clone 表达式的
         临时值流，不能存进变量（fatal = 将亡值，必须直接接管）。函数参数
         是唯一例外（§4，sema 参数路径不经过此分支）。 */
      if (sym->type && sema_sym_is_fatal(sym)) {
        diag_error(sema->diag, sema_loc(sema, vd->type_expr),
                   "cannot declare variable '%.*s' of fatal pointer type "
                   "(fatal is a temporary; receive it as own or pass to a "
                   "function)",
                   (int)vd->name.len, vd->name.ptr);
        sym->flow_init = false;
      }
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
        /* R1（own 不可 copy，§3.1）：own *T 声明只能由 fatal 将亡值
           （move/clone/new/make 产物或函数返回值）初始化——直接赋 own/ref
           变量 = copy → 编译错误。 */
        if (sema_sym_ownership(sym) == OWN_OWN && !sema_value_is_fatal(init)) {
          char itn[64];
          sema_type_name(value_type(init), itn, sizeof(itn));
          /* 措辞不用"pointer"：所有权已与类型解耦，`own` 是**位置对值的
             状态要求**（需要将亡值/FATAL），任何类型都可能出现在这里。
             `own i32` 正是经由这条规则被淘汰——i32 没有 FATAL 产生式。 */
          diag_error(sema->diag, sema_loc(sema, vd->init),
                     "cannot initialize own value '%.*s' with %s: an 'own' "
                     "binding must take over a dying value (FATAL), produced by "
                     "new / move / clone (or a fatal-returning call)",
                     (int)vd->name.len, vd->name.ptr, itn);
          sym->flow_init = false; /* copy 失败 = 未初始化 */
        } else if (sema_sym_ownership(sym) != OWN_OWN &&
                   sema_value_is_fatal(init)) {
          /* **反向规则**（§0.4）：FATAL 必须被 own 位置接住。非 own 位置
             接收将亡值 = 值消失而内部 own 槽位无人释放 → 静默泄漏；更糟
             时源与目标同名不同状态（双释放）。聚合（含 own 槽位的
             struct/union/tuple）的 CONSTRUCT 就产 FATAL，因此
             `var b: Box = .Box{ .p = new i32{1} }` 必须写成
             `var b: own Box = …`（或 move() 进已有 own 变量）。 */
          char tn[64], itn[64];
          sema_type_name(sym->type, tn, sizeof(tn));
          sema_type_name(value_type(init), itn, sizeof(itn));
          diag_error(sema->diag, sema_loc(sema, vd->init),
                     "cannot initialize '%.*s' of type %s with the dying value "
                     "(FATAL) of type %s: a value holding owned resources must "
                     "be caught by an 'own' position (declare it `own %s`, or "
                     "use move()/clone() and store the result there)",
                     (int)vd->name.len, vd->name.ptr, tn, itn, tn);
          sym->flow_init = false;
        } else if (sema_sym_ownership(sym) == OWN_NONE && sym->type &&
                   type_has_own_slots(sym->type) &&
                   !sema_value_is_fatal(init) && sema_value_holds_own(init)) {
          /* 非 own 位置的 copy：读 own **聚合**变量/字段/解引用得到携带
             own 资源的值，复制会让两个位置共享同一批内嵌堆块 → 双释放；
             借源视图还会在 compound_assign_transfer 里被清空（悬垂）。
             own 是 move-only。判据覆盖 `?own *T` 这类**嵌套** own 槽位
             （顶层 ownership 为 NONE，但 type_has_own_slots 为真）。 */
          char tn[64];
          sema_type_name(sym->type, tn, sizeof(tn));
          diag_error(sema->diag, sema_loc(sema, vd->init),
                     "cannot initialize '%.*s' of type %s by copying a value "
                     "that holds owned resources: own is move-only (use move() "
                     "or clone(), or declare the target `own %s`)",
                     (int)vd->name.len, vd->name.ptr, tn, tn);
          sym->flow_init = false;
        } else {
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
      }
      var_value =
          value_make_shadow(sema->vm, sym->type ? sym->type : sema->vm->type_void);
      value_set_type_ownership(sema->vm, var_value, sema_sym_ownership(sym));
    } else {
      /* 推断类型：init 类型即变量类型。
         R1（own 不可 copy，§3.1）：var p = q; 且 init 是 own *T 且非
         fatal 接管 → copy → 编译错误（own 唯一所有权只能 move/clone）。 */
      const type_t *vt = init_bad ? sema->vm->type_void : value_type(init);
      /* R1（fatal 禁止命名，§3.3）：fatal 是临时值，不能直接命名。
         - fatal 复合类型（fatal Test）：自动剥离为 T（OWN_NONE）——复合类型
           的 own 字段所有权由字段自身的 ownership 编码，fatal 仅标记将亡值，
           变量接管后类型为 T。
         - fatal 指针/切片（fatal *T / fatal []T）：自动升级为 own *T / own []T
           ——fatal 是将亡值，只能赋值给 own（move/clone/new 产物），自动推断
           为 own 而非要求显式标注。 */
      if (vt && sema_value_is_fatal(init)) {
        const type_t *stripped = strip_fatal(sema, vt);
        if (stripped) {
          vt = stripped;
          /* fatal 指针/切片剥离为 own（变量接管将亡值 → own 所有权）；
             **fatal 复合类型同理**：变量接管后就是内部 own 槽位的所有者，
             否则作用域退出时没有任何 free 序列 → 静默泄漏（实测
             `var u = .U{ .p = new i32{1} }` 的 4 字节堆块从未释放）。
             类型仍是 T（所有权不进类型），只是符号层记 OWN_OWN，
             让 make_free_stmts 按 type 对象展开销毁序列。 */
          /* 含 own 捕获的闭包：move 产 FATAL → 变量接管为 own 绑定，
             并记住捕获链供 FREE 展开（逐捕获槽释放）。 */
          if (type_kind_of(vt) == TYPE_KIND_FUNC) {
            /* 闭包 = 捕获聚合结构体：**含 own 捕获**的闭包是 own（move-only），
               销毁 = `FREE f`（VM 按 capture_type 释放 env 里的 own 捕获）。 */
            const ast_func_def_t *oc = own_closure_of(vd->init);
            if (oc) {
              /* 闭包含 own 捕获：strip_fatal 已把类型升为 own func(...)。 */
            } else if (sema_value_is_fatal(init) ||
                       value_type_ownership(init) == OWN_OWN) {
              /* 非字面量初始化（clone(g) / move(g)）：结果依赖运行期 capture_type
                 → 保守按 own 处理（strip_fatal 已升级类型）。 */
            }
          } else if (type_has_own_slots(vt)) {
            /* 聚合（含 own 槽位的 struct/tuple/union/array）接管：
               变量成为内部 own 槽位的所有者（strip_fatal 已升级类型）。 */
          }
          /* 将推断出的类型写回 type_expr（AST_TYPE_REF），让 compiler 发
             LOAD_TYPE <id> 而非 PUSH_UNDEFINED。运行时 op_define 走显式
             类型路径：value_implicit_cast(fatal init → own/none 接管），
             避免变量持有 fatal 类型在 scope 退出时不释放 → 内存泄漏。
             函数参数不走此路径（参数有显式 fatal 类型标注，合法保留）。

             注意：`strip_fatal` 合成的 `own ?own *T` 这类**所有权包装**类型
             可能还没登记过（用户没写过这个类型表达式）→ sema_type_find 返回
             NULL，此前就不生成 type_expr，运行期变量拿到的是源值类型
             （`var p = clone(q.n); p == nil` 报 "opt is none requires an
             optional value"）。这里补登记后再生成类型引用。 */
          const sema_type_t *st = sema_type_find(sema, vt);
          if (!st) st = sema_type_register(sema, vt);
          if (st && st->name.ptr) {
            ast_node_t *ref = ast_type_ref_new(sema->arena,
                                               vd->init->tok_begin,
                                               vd->init->tok_end);
            if (ref) {
              ((ast_type_ref_t *)ref)->name = st->name;
              vd->type_expr = ref;
            }
          }
        } else {
          /* strip_fatal 返回 NULL 意味着遇到不支持的 fatal 类型（理论上
             不会发生——所有 fatal 变体都覆盖了），保守报错。 */
          diag_error(sema->diag, sema_loc(sema, vd->init),
                     "cannot infer fatal type for variable '%.*s'",
                     (int)vd->name.len, vd->name.ptr);
          sym->flow_init = false;
          vt = sema->vm->type_void;
        }
      } else if (vt && !sema_value_is_fatal(init) &&
                 sema_value_holds_own(init)) {
        /* R1（own 不可 copy，§3.1）：var p = q; 且 rhs 携带 own 资源
           （顶层 own，或 `?own *T` / 含 own 槽位的聚合）→ copy → 编译错误。
           顶层判据之外还要覆盖**嵌套** own 槽位：字段读 `var p = self.n`
           拿到的是借源视图，复制后两头都以为自己拥有同一块堆 → 双释放。 */
        char itn[64];
        sema_type_name(vt, itn, sizeof(itn));
        diag_error(sema->diag, sema_loc(sema, vd->init),
                   "cannot initialize variable '%.*s' with %s "
                   "(own is move-only; use move() or clone())",
                   (int)vd->name.len, vd->name.ptr, itn);
        sym->flow_init = false; /* copy 失败 = 未初始化 */
        vt = sema->vm->type_void;
      }
      sym->type = vt;
      /* 推断类型写回 AST：后端（C 降级/编译器）需要与 sema 完全相同的类型
         实例。闭包字面量的值类型是**闭包类型**（捕获槽位进入类型身份），
         若后端改用函数签名推断（func_type_of_def）会得到普通 func 类型，
         与字面量构造出的闭包 sig struct 不兼容（实测 C 编译报 incompatible
         struct type）。 */
      if (vt) vd->resolved_type = vt;
      var_value = value_make_shadow(sema->vm, vt);
      value_set_type_ownership(sema->vm, var_value, sema_sym_ownership(sym));
    }
  }

  /* §5 逃逸检查（m3-design §5）：声明类型含 ref 指针（顶层 ref *T 或含
     ref 字段的 struct/tuple/union）时定义必须带作用域标注；带标注则做
     逃逸比较（ref 存活 ≤ 锚点存活）。类型未就绪（解析失败已诊断）跳过
     ——防级联。 */
  if (sym->type) check_var_annot(sema, vd, scope, sym->type, sym);

  /* 注：原先这里还有"含 ref 字段的聚合变量记录 init 借源集合"的一步
     （`borrow_collect`），供赋值时校验"新值存活不超出承诺上限"。
     复合类型禁止携带指针/切片时代该形态不存在，整条借源集合机制
     （`borrow_set` / `borrow_collect` / …）随之删除。
     聚合恢复携带 own 槽位后（2026-10），**从聚合 own 槽位借出**的借源由
     `shadow_borrow_register` 里的 `borrow_source_from_owned_path` 补上：
     沿访问路径（MEMBER/INDEX/DEREF/UNWRAP/ADDR）判定借出的目标是否归根变量
     所有（own 字段/元素/inner/payload、own 指针的被指向内容、含 own 槽位的
     存储），是则把**根变量**登记为借源（单源 `borrow_src`）——
     move(根变量) 因此被 R3 拒绝。ref 字段/标量字段/借用指针穿透不登记。 */

  /* R3 借用登记（m3-design §7）：ref *T / opaque 变量绑定借源（显式类型
     分支与推断分支统一——推断出 ref 同样登记；opaque 恒显式）。 */
  shadow_borrow_register(sema, sym, vd->init, scope);

  /* 聚合 ref 槽位的**借源载体**：构造 = 逐字段赋值的语法糖
     （var s = .S{ .q = b.p } ≡ s.q = b.p），因此把构造/拷贝里 ref 值的来源
     合并到该变量；scope 退出时统一释放（sema_scope_release_borrows）。 */
  if (sym && sym->type && vd->init &&
      !(sema_sym_ownership(sym) == OWN_REF ||
        type_kind_of(sym->type) == TYPE_KIND_OPAQUE) &&
      type_has_ref_slots(sym->type)) {
    sema_symbol_t *csrcs[16];
    size_t cn = borrow_collect_value(sema, sym->type, OWN_NONE, vd->init,
                                     scope, csrcs, 16, 0);
    if (cn) borrow_carry_merge(sema, sym, csrcs, cn);
  }

  /* 定义 shadow value 到当前 VM scope（与 sema scope 树同构；名字取自 ast）。
     定义完成 → 符号激活（sema_lookup 跳过未激活符号，自引用解析到外层） */
  char nb[256];
  name_to_cstr(vd->name, nb, sizeof nb);
  scope_define(sema->vm, sema->vm->current_scope, nb, var_value);
  sym->is_active = true;
}
