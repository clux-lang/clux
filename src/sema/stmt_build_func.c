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

/* 捕获解析（函数"定义点"，外层 scope 在线）：
   - 纯 id 捕获：外层符号查找（变量必须确定已初始化——捕获即读值）+ 类型
     解析（从外层符号 type）写回 fscope 捕获符号
   - 括号捕获：init 在外层作用域求值（定义点）+ 显式 type_expr 校验 /
     无类型时从 init 推断，类型写回 fscope 捕获符号
   调用点：walk_block AST_FUNC_DEF 分支（局部函数，定义点在父函数 walk 内，
   先于 sema_walk_function）与 sema_check_func_literal（函数字面量，表达式
   求值点同步）。fscope 捕获符号 type 就绪后，sema_walk_function /
   sema_check_func_literal 据此定义捕获 shadow value（函数体内 lookup 命中）。 */
void resolve_func_captures(sema_t *sema, ast_func_def_t *fn,
                           sema_scope_t *fscope, sema_scope_t *outer) {
  if (!fn->captures || !fscope) return;
  for (ast_node_t *c = fn->captures; c; c = c->next) {
    ast_var_def_t *cv = (ast_var_def_t *)c;
    sema_symbol_t *cs = sema_scope_find_local(fscope, cv->name);
    if (!cs) continue; /* 3a 拒绝（comptime/全局/重名）已诊断，跳过防级联 */
    cs->is_capture = true;  /* 捕获槽：闭包内不可 move（见 symbol.h 说明） */

    if (cv->init) {
      /* 括号捕获：init 定义点在外层作用域求值（c+d 引用外层变量） */
      value_t *init = sema_expr(sema, &cv->init, outer);
      bool init_bad = value_is_error(sema->vm, init) ||
                      value_is_type(init, TYPE_KIND_VOID);
      if (cv->type_expr) {
        /* 显式类型：3a 已解析（或失败留 NULL）→ 兜底重解析 + 赋性校验 */
        if (!cs->type) {
          cs->type = sema_resolve_type_slot(sema, &cv->type_expr);
        }
        if (!init_bad && cs->type) {
          /* 捕获槽是**具名位置**（与 var 定义同一张表，§0.4）：
             - 非 own 捕获槽收 FATAL = 值消失而内部 own 槽位无人释放
               → 静默泄漏（实测 `|(bb: T = move(a))|` 的堆块从未释放）。
               必须写 `|(bb: own T = move(a))|`。
             - 非 own 捕获槽 copy OWN 聚合 → 两处共享内嵌堆块 → 双释放。 */
          char what[96];
          snprintf(what, sizeof what, "capture '%.*s'", (int)cv->name.len,
                   cv->name.ptr);
          if (sema_sym_ownership(cs) != OWN_OWN && sema_value_is_fatal(init)) {
            char tn[64], itn[64];
            sema_type_name(cs->type, tn, sizeof(tn));
            sema_type_name(value_type(init), itn, sizeof(itn));
            diag_error(sema->diag, sema_loc(sema, cv->init),
                       "cannot initialize %s of type %s with the dying value "
                       "(FATAL) of type %s: a capture holding owned resources "
                       "must be an own capture (declare it `own %s`)",
                       what, tn, itn, tn);
          } else if (sema_sym_ownership(cs) == OWN_NONE &&
                     value_type_ownership(init) == OWN_OWN &&
                     type_has_own_slots(cs->type)) {
            char tn[64];
            sema_type_name(cs->type, tn, sizeof(tn));
            diag_error(sema->diag, sema_loc(sema, cv->init),
                       "cannot initialize %s of type %s by copying an own "
                       "value: own is move-only (use move() or clone())",
                       what, tn);
          } else {
            value_t *dst = value_make_shadow(sema->vm, cs->type);
            if (value_is_error(sema->vm, value_assign(sema->vm, dst, init))) {
              char tn[64], itn[64];
              sema_type_name(cs->type, tn, sizeof(tn));
              sema_type_name(value_type(init), itn, sizeof(itn));
              diag_error(sema->diag, sema_loc(sema, cv->init),
                         "cannot initialize capture '%.*s' of type %s with %s",
                         (int)cv->name.len, cv->name.ptr, tn, itn);
            }
          }
        }
      } else {
        /* 推断类型：init 类型即捕获类型。
           §9.2 fatal 接管为 own：move(p)/clone(p) 产 fatal *T，闭包内
           无 fatal 变量——fatal 自动转 own *T（闭包成为所有权持有者）。 */
        const type_t *it = init_bad ? sema->vm->type_void : value_type(init);
        if (!init_bad && sema_value_is_fatal(init)) {
          /* 捕获接管了将亡值 → 该捕获槽**持有所有权**（OWN），类型升为 own T。
             这决定了闭包本身的产出状态：捕获了 own 的闭包即 OWN。 */
          it = type_ownership_apply(sema->vm, OWN_OWN,
                                    type_strip_ownership(it));
        }
        cs->type = it;
      }
    } else {
      /* 纯 id 捕获：外层符号（定义点 block scope 沿链查找） */
      sema_symbol_t *outer_sym = sema_lookup(outer, cv->name);
      if (!outer_sym) {
        diag_error(sema->diag, sema_loc(sema, c),
                   "cannot capture undefined variable '%.*s'",
                   (int)cv->name.len, cv->name.ptr);
        continue;
      }
      if (outer_sym->kind != SEMA_SYM_VAR &&
          outer_sym->kind != SEMA_SYM_FUNC) {
        diag_error(sema->diag, sema_loc(sema, c),
                   "cannot capture '%.*s': not a variable or function",
                   (int)cv->name.len, cv->name.ptr);
        continue;
      }
      /* 函数符号（局部函数自身/兄弟）提升即存在（is_active=true），无
         flow_init 概念——跳过初始化检查（运行时绑定在 STORE 后沿作用域链
         取到定义点实例或提升基底）。变量捕获要求确定已初始化。 */
      if (outer_sym->kind == SEMA_SYM_VAR && !outer_sym->flow_init) {
        diag_error(sema->diag, sema_loc(sema, c),
                   "cannot capture '%.*s' before initialization",
                   (int)cv->name.len, cv->name.ptr);
        continue;
      }
      cs->type = outer_sym->type; /* 捕获 clone 值，类型即外层变量类型 */
      /* cs->type 已等于 outer_sym->type（所有权随之携带） */
      /* §9.1 own 裸捕获禁止：纯 id 捕获 own *T = copy（own 禁止 copy，
         §3.1）→ 必须显式 move()/clone()。fatal 同理（裸捕获 fatal 变量
         = copy 将亡值，须 move/clone 接管）。 */
      if (outer_sym->type &&
          (sema_sym_ownership(outer_sym) == OWN_OWN ||
           sema_sym_is_fatal(outer_sym))) {
        diag_error(sema->diag, sema_loc(sema, c),
                   "cannot capture own/fatal pointer '%.*s' by value "
                   "(use move() or clone(), e.g. |(%.*s = move(%.*s))|)",
                   (int)cv->name.len, cv->name.ptr,
                   (int)cv->name.len, cv->name.ptr,
                   (int)cv->name.len, cv->name.ptr);
        continue;
      }
    }
    cs->flow_init = true; /* 捕获即初始化（闭包持有 clone 值） */
    cs->is_active = true; /* 函数体 walk 时可见（sema_lookup 激活过滤） */
    /* 捕获槽的实际类型（销毁序列从 type 对象反推用；推断捕获无 type_expr） */
    cv->resolved_type = cs->type;
  }

  /* §9.3 ref/opaque 捕获强制标注（m3-design §9）：闭包是隐式 struct，
     捕获 ref/opaque 视同 struct ref 字段——闭包整体必须带作用域标注
     func '<x> |...|（同 §5 强制规则 B）。无标注 → 编译错误。
     有标注 → 逃逸检查：闭包定义作用域须嵌套于标注锚点作用域。 */
  bool has_ref_capture = false;
  for (ast_node_t *c = fn->captures; c; c = c->next) {
    ast_var_def_t *cv = (ast_var_def_t *)c;
    sema_symbol_t *cs = sema_scope_find_local(fscope, cv->name);
    if (!cs || !cs->type) continue;
    /* 旧语义：只有**显式声明为 ref** 的捕获（或纯 id 捕获外层 ref 变量、
       或 opaque）才强制作用域标注；从 init 推断出的 ref（`|(r = x.&)|`）
       不在此列。声明侧修饰直接从解析后的捕获类型读。 */
    ownership_t declared = type_ownership_of(cv->resolved_type);
    if (!cv->type_expr && cv->init) declared = OWN_NONE;
    if (declared == OWN_REF ||
        type_kind_of(cs->type) == TYPE_KIND_OPAQUE) {
      has_ref_capture = true;
      break;
    }
  }
  if (has_ref_capture && fn->scope_annot_count == 0) {
    diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
               "closure capturing ref/opaque must declare a scope annotation "
               "(e.g. func '<x> |...| or defer '<x> |...|)");
  }
  /* §9.3 逃逸检查：有标注时，闭包定义作用域（outer）须嵌套于每个锚点。
     复用 stmt.c 的 annot_anchor_scope / scope_within——但它们是 static，
     此处内联简化版本：标注名解析为符号 own_scope，比较 outer ⊆ anchor。 */
  if (has_ref_capture && fn->scope_annot_count > 0) {
    for (size_t i = 0; i < fn->scope_annot_count; i++) {
      strslice_t nm = fn->scope_annot_names[i];
      sema_scope_t *anchor = NULL;
      if (nm.len == 1 && nm.ptr[0] == '*') {
        anchor = sema->global_scope;
      } else if (sema->fn.def) {
        for (ast_node_t *p = sema->fn.def->params; p; p = p->next) {
          ast_var_def_t *pvd = (ast_var_def_t *)p;
          if (pvd->name.len == nm.len &&
              memcmp(pvd->name.ptr, nm.ptr, nm.len) == 0) {
            anchor = sema->fn.local_func_param_scope;
            break;
          }
        }
      }
      if (!anchor) {
        sema_symbol_t *sym = sema_lookup(outer, nm);
        if (sym && sym->kind == SEMA_SYM_VAR) anchor = sym->own_scope;
      }
      if (!anchor) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                   "scope annotation '%.*s' does not name a parameter, "
                   "global or local variable",
                   (int)nm.len, nm.ptr);
        continue;
      }
      if (!sema_scope_within(outer, anchor)) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                   "closure escapes scope annotation '%.*s' (its scope is "
                   "not nested within the annotated scope)",
                   (int)nm.len, nm.ptr);
      }
    }
  }
  /* 捕获聚合类型（§7.1.10 "闭包 = 捕获结构体"）：把捕获槽表登记成一个 struct
     类型（name / 类型 / **槽位所有权**），VM 层据此把闭包当聚合值处理 ——
     clone 深拷贝捕获、free 按槽位所有权释放、字段读写按槽位类型。
     编译器在 hoist 注册函数对象后发 `SET_FUNC_CAPTURES <id>`（fn->capture_type_id）。 */
  {
    size_t nc = 0;
    for (ast_node_t *c = fn->captures; c; c = c->next) {
      ast_var_def_t *cv = (ast_var_def_t *)c;
      sema_symbol_t *cs = sema_scope_find_local(fscope, cv->name);
      if (cs && cs->type) nc++;
    }
    if (nc > 0) {
      struct_field_t *fields = (struct_field_t *)allocator_new_ex(
          sema->vm->alloc, "struct_field_t", sizeof(struct_field_t), NULL, NULL,
          NULL, nc);
      if (fields) {
        size_t k = 0;
        for (ast_node_t *c = fn->captures; c && k < nc; c = c->next) {
          ast_var_def_t *cv = (ast_var_def_t *)c;
          sema_symbol_t *cs = sema_scope_find_local(fscope, cv->name);
          if (!cs || !cs->type) continue;
          memset(&fields[k], 0, sizeof(fields[k]));
          fields[k].name      = cv->name;   /* type_struct_intern 深拷贝 */
          fields[k].type      = cs->type;
          k++;
        }
        const type_t *cap = type_struct_intern(sema->vm, fields, k);
        allocator_free(sema->vm->alloc, (void **)&fields);
        if (cap) {
          const sema_type_t *st = sema_type_register(sema, cap);
          if (st) {
            fn->capture_type_id = st->id;
            /* 登记：sema_analyze 收尾为它生成 env 的 vtable（dispose/clone）。 */
            if (sema->closures) vec_push(sema->closures, sema->vm->alloc, fn);
          }
        }
      }
    }
  }
}

/* ===========================================================================
 * 函数字面量（表达式内 AST_FUNC_DEF，sema_expr 求值用）
 *
 * 与局部函数共享 AST_FUNC_DEF 节点，但语义差异：不注册作用域符号、不提升。
 * 在表达式求值点同步完成全部检查：
 *
 *   1. 临时 fscope（parent = 定义点作用域）——不 add_child 挂树：不消费
 *      外层 walk_block 的子作用域索引（idx 对齐不受影响），树由本函数
 *      自持并销毁（含 body 内局部函数的 fscope）。
 *   2. 建树（build_block）：注册参数 + body 内局部函数登记队列 +
 *      嵌套块作用域（临时树）；返回路径完整性分析（非 void 必须全路径
 *      return，与函数定义同语义）。
 *   3. 签名解析（与 resolve_local_func_sig 同构）：type_func_sig 建签名 →
 *      sema_type_register 登记 → fn->sig_id（compiler LOAD_TYPE 用）。
 *   4. walk body（sema_walk_block，仿 sema_walk_function）：参数 shadow
 *      定义到临时 VM scope；local_func_base = fscope → 字面量引用外层局部
 *      （闭包）编译期拒绝。
 *   5. 同步 walk body 内登记的局部函数（sema->funcs[n0..] 队列尾）并标记
 *      is_literal_owned——Pass 3b 驱动循环据此跳过（防二次 walk 访问已
 *      销毁的临时作用域树）。嵌套字面量已递归完成，跳过。
 *   6. 销毁临时作用域树。
 *
 * 返回签名类型（func_type_t，vm 池 intern）；失败返回 NULL（已诊断）。
 * =========================================================================== */
const type_t *sema_check_func_literal(sema_t *sema, ast_func_def_t *fn,
                                      sema_scope_t *outer) {
  /* fid 单一来源：创建函数对象（字面量）即分配。CTFE 求值（comptime body
     内字面量）可能已分配——幂等复用；compiler LOAD_FUNCTION <fid> 用 */
  sema_func_id_alloc(sema, fn);

  /* M7：泛型函数字面量（func[T](...)）留给后续阶段。 */
  if (fn->generic.count > 0) {
    diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
               "generic function literals are not implemented yet (M7 P2 "
               "scope: top-level generic functions only)");
    return NULL;
  }

  /* 队列登记起点：body 内局部函数（含嵌套字面量内）追加到队尾 */
  size_t n0 = vec_len(sema->funcs);

  sema_scope_t *fscope =
      sema_scope_new(sema->vm->alloc, SEMA_SCOPE_FUNCTION, outer);
  if (!fscope) panic("sema: out of memory allocating literal fscope");

  sema_scope_t *pscope =
      sema_scope_new(sema->vm->alloc, SEMA_SCOPE_FUNCTION, fscope);
  sema_scope_add_child(fscope, pscope); /* 参数层（fscope 子）——参数遮蔽捕获 */

  /* 参数注册（与 build_func_tree 同构，注册到参数层）+ 签名参数类型收集 */
  size_t nparams = sema_count_siblings(fn->params);
  const type_t **ptypes = NULL;
  if (nparams > 0) {
    ptypes = allocator_new_ex(sema->vm->alloc, "type_t*", sizeof(type_t *),
                              NULL, NULL, NULL, nparams);
  }
  size_t j = 0;
  for (ast_node_t *p = fn->params; p; p = p->next, j++) {
    ast_var_def_t *vd = (ast_var_def_t *)p;
    const type_t *t = sema_resolve_type_slot(sema, &vd->type_expr);
    if (!t) {
      diag_error(sema->diag, sema_loc(sema, p), "unknown type in parameter '%.*s'",
                 (int)vd->name.len, vd->name.ptr);
    }
    if (ptypes) ptypes[j] = t; /* 失败置 NULL，位置对齐，签名校验时跳过 */
    /* **is_active = true**：参数在函数入口即生效。缺了这个，sema_lookup
       （只认 is_active 符号）在函数体里找不到参数——`func (a: own *T): i32 {
       return f(move(a)); }` 会误报 "cannot move a borrowed value" 和
       "cannot copy a value holding owned resources"（实测）。 */
    sema_symbol_t init = {.kind = SEMA_SYM_VAR, .type = t, .is_active = true};
    if (!sema_scope_define(pscope, vd->name, &init)) {
      diag_error(sema->diag, sema_loc(sema, p), "duplicate parameter '%.*s'",
                 (int)vd->name.len, vd->name.ptr);
    }
  }

  /* 捕获符号注册（与 build_func_tree 同构，注册到 fscope 父层）：纯 id =
     外层变量（type 3b 从外层符号解析）；括号 = 临时构造（显式 type_expr
     解析，无则 init 推断）。comptime 字面量无运行时闭包，拒绝。 */
  if (fn->captures) {
    if (fn->is_comptime) {
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                 "comptime function literal cannot have captures");
      fn->captures = NULL; /* 摘除：错误已诊断，防级联 */
    } else {
      for (ast_node_t *c = fn->captures; c; c = c->next) {
        ast_var_def_t *cv = (ast_var_def_t *)c;
        const type_t *ct = NULL;
        if (cv->type_expr) {
          ct = sema_resolve_type_slot(sema, &cv->type_expr);
        }
        sema_symbol_t init = {.kind = SEMA_SYM_VAR, .type = ct, .declared = true};
        if (!sema_scope_define(fscope, cv->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, c),
                     "capture '%.*s' duplicates another capture",
                     (int)cv->name.len, cv->name.ptr);
        }
      }
    }
  }

  /* 建树：body 内局部函数登记队列 + 嵌套块作用域（临时树，自持） */
  build_result_t r = build_block(sema, (ast_block_t *)fn->body, pscope);

  /* 捕获解析（定义点即表达式求值点，外层 scope 在线）：纯 id 类型/flow_init、
     括号 init 求值与类型校验，写回 fscope 捕获符号。 */
  resolve_func_captures(sema, fn, fscope, outer);

  /* 返回路径完整性：非 void 字面量所有路径必须 return */
  const type_t *rt = NULL;
  if (fn->return_expr) {
    rt = sema_resolve_type_slot(sema, &fn->return_expr);
    if (!rt) {
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                 "unknown return type in function literal");
    }
  }
  /* §5 强制规则（m3-design §5）：返回 ref 必标注（集合 = 参数名 ∪ {*}）。 */
  sema_check_ret_ref_annot(sema, fn, rt);
  if (rt && type_kind_of(rt) != TYPE_KIND_VOID && !r.definitely_returns) {
    char nb[128];
    if (fn->name.len)
      build_name_to_cstr(fn->name, nb, sizeof nb);
    else
      snprintf(nb, sizeof nb, "<anonymous>");
    diag_error(sema->diag, sema_loc(sema, &fn->base),
               "function literal '%s' must return a value on all paths", nb);
  }

  /* 签名解析 + 登记（compiler hoist LOAD_TYPE <sig_id> 用） */
  const type_t *sig = type_func_sig(sema->vm, ptypes, nparams, rt, false);
  const sema_type_t *st = sema_type_register(sema, sig);
  if (st) fn->sig_id = st->id;
  if (ptypes) allocator_free(sema->vm->alloc, (void **)&ptypes);

  /* walk body：捕获 shadow 定义到捕获层 VM scope，参数 shadow 定义到参数层
     VM scope（子层，遮蔽捕获——与运行时 func_vcall 结构一致）；捕获检查按
     字面量 fscope + param_scope 生效（引用外层局部 = 闭包，拒绝）。保存/
     恢复外层状态——本函数嵌套在外层 walk 中调用（外层可能正 walk 局部
     函数体，local_func_base 非空）。**整体保存/恢复 sema->fn**：逐字段
     备份极易漏字段（上下文新增字段时静默失配）。 */
  sema_fn_ctx_t saved = sema_fn_ctx_enter(sema);

  sema->fn.local_func_base = fscope;
  sema->fn.local_func_param_scope = pscope;
  sema->fn.def = fn; /* walk_return §6/§9 判定用（captures 查当前闭包） */
  sema->fn.return_type = rt;
  /* 字面量 body 是真实调用点（最终进入运行时字节码）：body 内 comptime
     调用照常折叠（即使本字面量定义在 comptime func body 内）。 */
  sema->fn.walking_comptime = false;

  vm_push_scope(sema->vm); /* 捕获层（父） */
  for (ast_node_t *c = fn->captures; c; c = c->next) {
    ast_var_def_t *cv = (ast_var_def_t *)c;
    sema_symbol_t *cs = sema_scope_find_local(fscope, cv->name);
    if (!cs) continue; /* 3a 拒绝已诊断，跳过防级联 */
    value_t *cv_value = value_make_shadow(
        sema->vm, cs->type ? cs->type : sema->vm->type_void);
    char nb[256];
    build_name_to_cstr(cv->name, nb, sizeof nb);
    scope_define(sema->vm, sema->vm->current_scope, nb, cv_value);
  }
  vm_push_scope(sema->vm); /* 参数层（子，遮蔽捕获） */
  for (ast_node_t *p = fn->params; p; p = p->next) {
    ast_var_def_t *vd = (ast_var_def_t *)p;
    sema_symbol_t *ps = sema_scope_find_local(pscope, vd->name);
    if (ps) ps->flow_init = true; /* 参数由调用方传入，确定已初始化 */
    value_t *pv = value_make_shadow(
        sema->vm, ps && ps->type ? ps->type : sema->vm->type_void);
    char nb[256];
    build_name_to_cstr(vd->name, nb, sizeof nb);
    scope_define(sema->vm, sema->vm->current_scope, nb, pv);
  }
  sema_walk_block(sema, fn->body, pscope);
  vm_pop_scope(sema->vm);
  vm_pop_scope(sema->vm);

  /* 同步 walk body 内登记的局部函数（sema->funcs[n0..]）并标记。
     comptime 局部函数同样 walk（类型检查 + body 内语句折叠），不生成
     运行时字节码。 */
  for (size_t i = n0; i < vec_len(sema->funcs); i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    if (sf->is_literal_owned) continue; /* 嵌套字面量已同步 walk */
    sf->is_literal_owned = true;
    sema_walk_function(sema, sf);
  }

  /* 恢复外层 walk 上下文（整体恢复，见入口保存） */
  sema_fn_ctx_leave(sema, saved);

  /* 销毁临时作用域树（含 body 局部函数 fscope） */
  sema_scope_destroy(&fscope);

  /* 闭包类型：捕获槽位类型（按声明序）携带进函数值类型，而不是擦除成普通
     func(...)。捕获名不参与类型身份；`capture_type_id` 的字段顺序即声明序。 */
  const type_t *lit_ty = sig;
  if (fn->capture_type_id != 0) {
    const sema_type_t *cst = sema_type_by_id(sema, fn->capture_type_id);
    if (cst && cst->type) {
      size_t nc = struct_type_field_count(cst->type);
      const type_t *caps[nc > 0 ? nc : 1];
      for (size_t ci = 0; ci < nc; ci++) {
        const struct_field_t *f = struct_type_field(cst->type, ci);
        caps[ci] = f ? f->type : NULL;
      }
      size_t np = func_type_param_count(sig);
      const type_t *ps[np > 0 ? np : 1];
      for (size_t pi = 0; pi < np; pi++) ps[pi] = func_type_param(sig, pi);
      const type_t *cty =
          type_func_closure(sema->vm, np > 0 ? ps : NULL, np,
                            func_type_return(sig), false, caps, nc);
      if (cty) {
        const sema_type_t *st2 = sema_type_register(sema, cty);
        if (st2) fn->sig_id = st2->id;
        lit_ty = cty;
      }
    }
  }
  return lit_ty;
}
