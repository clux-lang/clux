#include "sema/sema.h"
#include "sema/sema_cross.h"
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
#include "sema/sema_generic.h"
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

static void shadow_assign_index(sema_t *sema, ast_assign_t *as, sema_scope_t *scope);
static void shadow_assign_member(sema_t *sema, ast_assign_t *as, sema_scope_t *scope);
static void shadow_assign_extern_var(sema_t *sema, ast_assign_t *as, sema_scope_t *scope);
static void shadow_assign_deref(sema_t *sema, ast_assign_t *as, sema_scope_t *scope);

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

void shadow_assign(sema_t *sema, ast_assign_t *as,
                          sema_scope_t *scope) {
  /* `b = a.?` / `b = a.!`（a 为 result struct）：展开为
     `if (a.ok()) b = a.value(); else ...`，赋值在 prelude 完成。 */
  if (as->value && as->value->kind == AST_UNWRAP) {
    sema_try_info_t info;
    if (sema_try_probe(sema, as->value, scope, &info)) {
      if (!as->target || as->target->kind != AST_IDENT) {
        diag_error(sema->diag, sema_loc(sema, as->target),
                   "error-handling sugar requires a simple variable target");
        return;
      }
      sema_symbol_t *ts =
          sema_lookup(scope, ((ast_ident_t *)as->target)->name);
      if (!ts || !ts->type) {
        diag_error(sema->diag, sema_loc(sema, as->target),
                   "error-handling sugar target is not declared");
        return;
      }
      ownership_t to = type_ownership_of(ts->type);
      (void)to;
      value_t *dst = value_make_shadow(sema->vm, ts->type);
      value_t *src = value_make_shadow(sema->vm, info.value_type);
      if (value_is_error(sema->vm, value_assign(sema->vm, dst, src))) {
        char a[64], b[64];
        sema_type_name(ts->type, a, sizeof a);
        sema_type_name(info.value_type, b, sizeof b);
        diag_error(sema->diag, sema_loc(sema, as->value),
                   "cannot assign value of type %s from 'a.?' to %s", b, a);
        return;
      }
      uint32_t tb = as->value->tok_begin, te = as->value->tok_end;
      ast_node_t *iff = sema_try_make_if(sema, &info, as->target, NULL, tb,
                                         te);
      if (iff) {
        if (info.setup) {
          info.setup->next = iff;
          as->base.prelude = info.setup;
        } else {
          as->base.prelude = iff;
        }
        as->sugar_noop = true;
        ts->flow_init = true;
      }
      return;
    }
  }
  /* 左值：下标表达式 a[i] = v（sema 校验下标合法） */
  if (as->target->kind == AST_INDEX) {
    shadow_assign_index(sema, as, scope);
    return;
  }
  /* 左值：字段赋值 p.field = v（读+写+复合赋值，嵌套 p.a.b 递归） */
  if (as->target->kind == AST_MEMBER) {
    shadow_assign_member(sema, as, scope);
    return;
  }
  /* 左值：解引用赋值 r.* = v（m3-design §8.2 解引用 SET 严格分离） */
  if (as->target->kind == AST_DEREF) {
    shadow_assign_deref(sema, as, scope);
    return;
  }
  /* 左值：`<库>::<数据符号>` = v（M6 FFI，extern var 的写入）。
     与读取同源：C ABI 层面就是"往 dlsym 得到的变量地址写 size 字节"。
     复合赋值（`crt::x += 1`）走 LOAD → op → STORE，与普通变量同构。 */
  if (as->target->kind == AST_SCOPE_REF) {
    /* 结构体静态属性：`T::x = v`。在类型的 member_scope 中查成员。
       comptime var 成员是 const（不可赋值）；func 成员不可赋值。
       type_expr 可能是 AST_IDENT（首次访问）或 AST_TYPE_REF（被 ex_scope_ref
       折叠过）——两种情况都取类型名查全局符号。 */
    ast_scope_ref_t *er = (ast_scope_ref_t *)as->target;
    /* M7：`Vec[i32]::attr = v` —— 类型位实例化 + 改写为运行时类型名。 */
    (void)sema_generic_rewrite_type_expr(sema, &er->type_expr);
    strslice_t type_name = STRSLICE_EMPTY;
    if (er->type_expr && er->type_expr->kind == AST_IDENT)
      type_name = ((ast_ident_t *)er->type_expr)->name;
    else if (er->type_expr && er->type_expr->kind == AST_TYPE_REF)
      type_name = ((ast_type_ref_t *)er->type_expr)->name;
    if (type_name.ptr) {
      sema_symbol_t *ls = sema_lookup(scope, type_name);
      if (ls && ls->kind == SEMA_SYM_TYPE) {
        /* 在 member_scope 中查成员名（自己优先 + 嵌入链递归） */
        const struct_type_t *owner = NULL;
        sema_symbol_t *ms = sema_struct_find_member(sema, ls->type,
                                                    er->variant, as->target,
                                                    &owner);
        /* 名字命中的是**函数成员**：`T::f = v` 非法 */
        if (ms && ms->kind == SEMA_SYM_FUNC) {
          diag_error(sema->diag, sema_loc(sema, as->target),
                     "cannot assign to '%.*s::%.*s': it is a `func` member "
                     "(a method, not a variable)",
                     (int)type_name.len, type_name.ptr,
                     (int)er->variant.len, er->variant.ptr);
          sema_expr(sema, &as->value, scope);
          return;
        }
        /* comptime var 成员是 const：不可赋值 */
        if (ms && ms->kind == SEMA_SYM_VAR && ms->is_comptime) {
          diag_error(sema->diag, sema_loc(sema, as->target),
                     "cannot assign to '%.*s::%.*s': it is a `comptime var` "
                     "(const, cannot be reassigned)",
                     (int)type_name.len, type_name.ptr,
                     (int)er->variant.len, er->variant.ptr);
          sema_expr(sema, &as->value, scope);
          return;
        }
        /* 普通静态属性：类型检查右值后放行（compiler 发 STORE_ATTR）。
           置 is_struct_attr + struct_attr_type_id 供 compiler 识别。 */
        if (ms && ms->kind == SEMA_SYM_VAR) {
          /* struct_attr_type_id 用**实际拥有属性的 struct 类型**（owner），
             覆盖语义下写自身、共享语义下写继承源。 */
          if (owner) {
            const sema_type_t *reg = sema_type_find(sema, &owner->base);
            if (reg) {
              er->is_struct_attr = true;
              er->struct_attr_type_id = reg->id;
            }
          }
          /* 求值右值做类型检查 */
          value_t *rv = sema_expr(sema, &as->value, scope);
          if (rv && !value_is_error(sema->vm, rv) && ms->type) {
            /* 隐式转换检查 */
            value_t *casted = value_implicit_cast(sema->vm, rv, ms->type);
            if (value_is_error(sema->vm, casted)) {
              char exp[256], got[256];
              sema_type_name(ms->type, exp, sizeof exp);
              sema_type_name(value_type(rv), got, sizeof got);
              diag_error(sema->diag, sema_loc(sema, as->value),
                         "type mismatch in assignment: expected '%s', got '%s'",
                         exp, got);
            }
          }
          return;
        }
      }
    }
  }
  if (as->target->kind == AST_SCOPE_REF) {
    shadow_assign_extern_var(sema, as, scope);
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

  /* 闭包捕获赋值：与捕获**读取**同构，改写为 `__ctx.env.<cap> = ...`。
     捕获槽在运行期是闭包实例 env 的字段（VM FIELD_SET 的闭包分支 / C 的
     `(*ctx_env).clux_f_i`），按成员赋值路径做类型/所有权校验。 */
  {
    sema_symbol_t *cs = sema_lookup(scope, name);
    if (cs && cs->kind == SEMA_SYM_VAR && cs->is_capture && sema->fn.def &&
        sema->fn.def->capture_type_id != 0) {
      uint32_t tb = as->target->tok_begin, te = as->target->tok_end;
      ast_node_t *ctx = ast_ident_new(sema->arena, tb, te);
      ast_node_t *env = ast_member_new(sema->arena, tb, te);
      ast_node_t *cap = ast_member_new(sema->arena, tb, te);
      if (!ctx || !env || !cap) {
        sema_expr(sema, &as->value, scope);
        return;
      }
      ctx->is_generated = true;
      env->is_generated = true;
      cap->is_generated = true;
      ((ast_ident_t *)ctx)->name = strslice_from_bytes("__ctx", 5);
      ((ast_member_t *)env)->object = ctx;
      ((ast_member_t *)env)->field = strslice_from_bytes("env", 3);
      ((ast_member_t *)cap)->object = env;
      ((ast_member_t *)cap)->field = name;
      cap->next = as->target->next;
      as->target = cap;
      shadow_assign_member(sema, as, scope);
      return;
    }
  }

  /* ===== 显式销毁 / 丢弃：`_ = expr`（m3-design §3.3）=====
     sema 在**当前位置**生成销毁序列（与作用域退出同一套扁平槽位展开），把
     "提前释放"编成显式 FREE 序列——与 `move` 一起构成"显式销毁"：
         `_ = move(x)`        —— 销毁整个变量 x（x 标 moved，退出不再释放）
         `_ = new T{...}`     —— 丢弃将亡值（临时变量 + 销毁）
         `_ = clone(x)`       —— 同上（源不受影响）
         `_ = <借用/无资源>`  —— 只求值 + POP（**不得 DISPOSE**：借用不拥有）
     `as->discard_mode` 告诉 compiler 走哪条路径（见 ast_assign.h）。
     拒绝：`_ = x`（own/fatal 变量**不 move**）——own 不可 copy；
           `move(ref)` —— 借用不拥有堆块（ex_move 约束 0）。 */
  if (strslice_eq(name, STRSLICE_LIT("_"))) {
    if (!as->op || !token_is(as->op, "=")) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "discard '_' only supports simple assignment '='");
      sema_expr(sema, &as->value, scope);
      return;
    }
    /* move(x) 形态先记下（sema_expr 之后节点仍是 AST_MOVE） */
    bool is_move = as->value && as->value->kind == AST_MOVE &&
                   ((ast_move_t *)as->value)->op &&
                   token_is(((ast_move_t *)as->value)->op, "move");
    ast_node_t *mv_ident = NULL;
    if (is_move) {
      ast_move_t *mv = (ast_move_t *)as->value;
      if (mv->operand && mv->operand->kind == AST_IDENT) mv_ident = mv->operand;
    }

    value_t *rhs = sema_expr(sema, &as->value, scope);
    if (!rhs || value_is_error(sema->vm, rhs) ||
        value_is_type(rhs, TYPE_KIND_VOID))
      return;
    const type_t *rt = value_type(rhs);

    /* 目标变量符号（ident 或 move(ident)） */
    ast_node_t *vsym_node = mv_ident;
    if (!vsym_node && !is_move && as->value &&
        as->value->kind == AST_IDENT)
      vsym_node = as->value;
    sema_symbol_t *vsym = vsym_node
        ? sema_lookup(scope, ((ast_ident_t *)vsym_node)->name)
        : NULL;

    /* `_ = x`（own/fatal 变量不 move）→ 拒绝：这不是"销毁"，是 own 拷贝。 */
    if (vsym && vsym->kind == SEMA_SYM_VAR && !is_move &&
        (sema_sym_ownership(vsym) == OWN_OWN || sema_sym_is_fatal(vsym))) {
      strslice_t vn = ((ast_ident_t *)vsym_node)->name;
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot discard '%.*s' by copy: an own value must be "
                 "transferred — use `_ = move(%.*s)` for explicit destruction "
                 "(or clone() to copy)",
                 (int)vn.len, vn.ptr, (int)vn.len, vn.ptr);
      as->discard_mode = 1;   /* 编译已失败，防后续再生成 */
      return;
    }

    /* 该值是否有**自有资源**需要释放：来源是 own/fatal（而不是借用）且类型
       含资源（指针/切片/含 own 槽位的聚合）。 */
    bool own_value = false;
    if (vsym && vsym->kind == SEMA_SYM_VAR)
      own_value = (sema_sym_ownership(vsym) == OWN_OWN || sema_sym_is_fatal(vsym));
    if (!own_value) own_value = sema_value_is_fatal(rhs);
    bool has_res = rt && (type_kind_of(rt) == TYPE_KIND_PTR ||
                          type_kind_of(rt) == TYPE_KIND_SLICE ||
                          type_kind_of(rt) == TYPE_KIND_FUNC ||
                          type_has_own_slots(rt));
    if (!own_value || !has_res) {
      as->discard_mode = 1;   /* 借用 / 无资源：expr + POP */
      return;
    }

    ast_node_t *dh = NULL, *dt = NULL;
    if (vsym && vsym->type &&
        (sema_sym_ownership(vsym) == OWN_OWN || sema_sym_is_fatal(vsym))) {
      /* 变量：直接销毁它（move 已在 ex_move 标 moved / 消费 fatal） */
      ast_node_t *id = ast_ident_new(sema->arena, 0, 0);
      if (id) {
        ((ast_ident_t *)id)->name = ((ast_ident_t *)vsym_node)->name;
        if (!gen_destroy(sema, vsym->type, id, &dh, &dt)) dh = NULL;
      }
      if (!dh) {
        as->discard_mode = 3;   /* 回退：运行期 DISPOSE */
        return;
      }
      if (!sema_append_prelude(sema, dh)) {
        as->discard_mode = 3;
        return;
      }
      as->discard_mode = 2;
      return;
    }

    /* 将亡值（new / 构造 / clone / 返回 fatal 的调用）：临时变量 + 赋值 +
       销毁序列。临时变量是 own，符号标 moved（销毁由生成的序列完成，块退出
       不再 FREE）。 */
    uint32_t dn = sema->clone_seq++;
    char nb[64];
    snprintf(nb, sizeof nb, "__disc_%u", dn);
    strslice_t tname = discard_name(sema, nb);
    if (!tname.ptr) { as->discard_mode = 3; return; }
    sema_symbol_t dsym;
    memset(&dsym, 0, sizeof dsym);
    dsym.kind      = SEMA_SYM_VAR;
    dsym.type      = rt;
    dsym.type = type_ownership_apply(sema->vm, OWN_OWN,
                                   type_strip_ownership(dsym.type));
    dsym.flow_init = true;
    dsym.moved     = true;
    dsym.is_active = true;
    dsym.own_scope = scope;
    if (!sema_scope_define(scope, tname, &dsym)) {
      as->discard_mode = 3;
      return;
    }
    ast_node_t *vd   = ast_var_def_new(sema->arena, 0, 0);
    ast_node_t *ud   = ast_undef_new(sema->arena, 0, 0);
    ast_node_t *tre  = sema_gen_type_ref(sema, rt);
    ast_node_t *asgn = ast_assign_new(sema->arena, 0, 0);
    ast_node_t *lhs  = ast_ident_new(sema->arena, 0, 0);
    ast_node_t *did  = ast_ident_new(sema->arena, 0, 0);
    if (!vd || !ud || !tre || !asgn || !lhs || !did) {
      as->discard_mode = 3;
      return;
    }
    vd->is_generated = true;
    ((ast_var_def_t *)vd)->name      = tname;
    ((ast_var_def_t *)vd)->type_expr = tre;
    ((ast_var_def_t *)vd)->init      = ud;
    asgn->is_generated = true;
    ((ast_assign_t *)asgn)->target = lhs;
    ((ast_assign_t *)asgn)->op     = NULL;   /* NULL = 普通赋值（compiler 特判） */
    ((ast_assign_t *)asgn)->value  = as->value;
    ((ast_ident_t *)lhs)->name = tname;
    ((ast_ident_t *)did)->name = tname;
    if (!gen_destroy(sema, rt, did, &dh, &dt)) dh = NULL;
    vd->next = asgn;
    asgn->next = dh;
    if (dh) dt->next = NULL;
    if (!sema_append_prelude(sema, vd)) {
      as->discard_mode = 3;
      return;
    }
    as->discard_mode = 2;
    return;
  }

  /* 左值从 VM scope 链 lookup（与 sema 作用域树同构）。
     先判 comptime var：它的**常量真实值**现在也会绑定进 VM 作用域
     （常量槽位解析需要，见 sema_eval_comptime_var 步骤 5），因此不能再靠
     "VM scope 查不到" 来识别编译期常量——必须在命中之前查符号表。 */
  sema_symbol_t *csym = sema_lookup(scope, name);
  if (csym && csym->is_comptime && csym->ct_valid) {
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign to compile-time constant '%.*s'",
               (int)name.len, name.ptr);
    return;
  }

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

  /* nil 赋值（a = nil）：nil 非 value（AST_NIL 的 sema_expr 会报错），
     先于 rhs 求值识别——合法消费点在 shadow_assign 内特判（STORE_NIL）。 */
  bool rhs_nil = as->value->kind == AST_NIL;
  /* 简单赋值 RHS 匿名构造注入：左值类型已知 → push anon_ct，使 RHS 的
     .{...} 推断目标类型（q: Point = .{...} 赋值给已声明变量）。 */
  const type_t *lt_ty = value_type(lhs);
  /* `f = cfunc_ptr(<命名 cfunc>)`（f 为 func 类型）→ 自动生成包装。 */
  if (lt_ty && type_kind_of(lt_ty) == TYPE_KIND_FUNC && !rhs_nil)
    sema_maybe_wrap_cfunc_expr(sema, &as->value, lt_ty, scope);
  value_t *rhs = NULL;
  if (rhs_nil) {
    rhs = NULL;
  } else {
    bool ct_pushed = sema_anon_push(sema, lt_ty);
    rhs = sema_expr(sema, &as->value, scope);
    sema_anon_pop(sema, ct_pushed);
  }
  bool rhs_bad = value_is_error(sema->vm, rhs) ||
                 value_is_type(rhs, TYPE_KIND_VOID);

  if (token_is(as->op, "=")) {
    sema_symbol_t *sym = sema_lookup(scope, name);

    /* const 赋值检查（TDZ 豁免）：声明 const 且已初始化（flow_init=true）
       的变量不可再赋值；flow_init=false（未初始化声明 var a:const T =
       undefined）时的赋值是首次初始化，豁免（const 变量的 TDZ 赋值 =
       初始化，仅一次）。经 value 层接口查询 const（type is value）。 */
    if (sym && value_has_const(lhs) && sym->flow_init) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to const variable '%.*s'",
                 (int)name.len, name.ptr);
      return;
    }

    if (rhs_nil) {
      /* nil 赋值（docs m2-design §12.5/12.6）：a = nil → STORE_NIL <name>
         （运行期只置 ok=false，不释放 value 字段）。左值必须 ?T 类型。 */
      const type_t *lt = value_type(lhs);
      if (!lt || type_kind_of(lt) != TYPE_KIND_OPTION) {
        char tn[64];
        sema_type_name(lt, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, as->value),
                   "cannot assign nil to variable '%.*s' of type %s (only "
                   "optional types accept nil)",
                   (int)name.len, name.ptr, tn);
        return;
      }
      if (sym) sym->flow_init = true;
      return;
    }

    if (rhs_bad) return; /* 错误恢复产物跳过，已有诊断 */

    /* **赋值点旧值销毁**（A 方案 §2.3 表 "旧块摘挂"）：`p = new i32{2}` 这类
       覆盖自有堆块的位置，旧块必须先释放（否则再也找不到 → 泄漏）。
       由 sema 生成销毁序列挂在 as->destroy，compiler 在「右值求值之后、
       STORE 之前」逐条编译（右值可能读旧值，顺序不能反）。

       只对 **ptr / slice** 生成：聚合（struct/tuple/union/array/option）的
       `value_assign` 走 compound_assign_transfer，运行期已按槽位所有权
       释放旧值——这里再生成会双释放。指针/切片的 assign 是纯 memcpy
       （设计如此：释放权归 sema），所以旧值只能由这里释放。
       move 过的变量（moved）已交出堆块，不再释放；未初始化（flow_init=false）
       也没有旧值。 */
    if (sym && sym->type && sym->flow_init && !sym->moved &&
        sema_sym_ownership(sym) == OWN_OWN &&
        (type_kind_of(sym->type) == TYPE_KIND_PTR ||
         type_kind_of(sym->type) == TYPE_KIND_SLICE ||
         type_kind_of(sym->type) == TYPE_KIND_FUNC)) {
      /* FUNC：闭包 = 捕获聚合结构体。`func_assign` 只是 memcpy（不释放旧
         env）→ 旧闭包的 own 捕获必须在这里 `FREE f` 掉（VM 按 capture_type
         聚合释放）。own 闭包是 move-only，唯一持有者，安全。 */
      size_t nlen = name.len;
      char *nbuf = (char *)arena_calloc(sema->arena, nlen + 1, 1,
                                        ALIGNOF(max_align_t));
      if (nbuf) {
        memcpy(nbuf, name.ptr, nlen);
        ast_node_t *old = ast_ident_new(sema->arena, 0, 0);
        if (old) {
          ((ast_ident_t *)old)->name = strslice_from_bytes(nbuf, nlen);
          ast_node_t *dhead = NULL, *dtail = NULL;
          if (gen_destroy(sema, sym->type, old, &dhead, &dtail))
            as->destroy = dhead;
        }
      }
    }

    /* 闭包值拷贝 = 深拷贝 env（与 var 初始化同规则；move 是转移、调用/构造
       产物是新实例，都不克隆）。 */
    if (as->value && as->value->kind != AST_MOVE &&
        (as->value->kind == AST_IDENT || as->value->kind == AST_MEMBER ||
         as->value->kind == AST_INDEX || as->value->kind == AST_DEREF ||
         as->value->kind == AST_UNWRAP)) {
      const type_t *rt = value_type(rhs);
      const type_t *rs = rt ? type_strip_quals(rt) : NULL;
      bool needs_copy = false;
      if (rs && !type_has_own_slots(rs)) {
        if (type_kind_of(rs) == TYPE_KIND_FUNC)
          needs_copy = func_type_capture_count(rs) > 0;
        else
          needs_copy = type_has_closure_slots(rs);
      }
      if (needs_copy) {
        ast_node_t *dc = sema_gen_deep_clone(sema, as->value);
        if (dc) as->value = dc;
      }
    }

    /* 简单赋值：value_assign 校验；赋值成功 → 数据流 flow_init=true
       （TDZ 退出由确定性赋值分析承担，VM 值层不感知）。
       R1（own 不可 copy，§3.1）：own *T 变量被赋值 = copy → 编译错误
       （own 唯一所有权只能 move/clone 转移，不能按值复制）。
       R2 恢复：赋值成功 → moved 标记清除（move 后重新赋值 = 重新获得
       所有权，变量恢复可用）。 */
    value_t *r = value_assign(sema->vm, lhs, rhs);
    if (value_is_error(sema->vm, r)) {
      char tn[64], rn[64];
      sema_type_name(value_type(lhs), tn, sizeof(tn));
      sema_type_name(value_type(rhs), rn, sizeof(rn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign %s to variable '%.*s' of type %s", rn,
                 (int)name.len, name.ptr, tn);
    } else {
      const type_t *lt = value_type(lhs);
      if (lt && value_type_ownership(lhs) == OWN_OWN &&
          !sema_value_is_fatal(rhs)) {
        char rn[64];
        sema_type_name(value_type(rhs), rn, sizeof(rn));
        diag_error(sema->diag, sema_loc(sema, as->value),
                   "cannot copy %s into own pointer '%.*s' (own is "
                   "move-only; use move() or clone())",
                   rn, (int)name.len, name.ptr);
        return;
      }
      /* 注 2：share/weak 引用计数共享已移除。
         注 3：原先这里还有一条"含 own 字段的复合类型赋值须 move/clone"的守卫
         （所有权扩散）。复合类型字段现已禁止携带指针/切片，该形态不存在；
         own 指针本身的 copy 拦截由上方分支负责（判据是**值/符号层 ownership**，
         不是类型层——所有权已从类型剥离）。 */
      /* 嵌套 own 槽位（`?own *T` / 含 own 字段的聚合）的赋值目标：同样只
         能由将亡值接管。否则 compound_assign_transfer 会释放左值旧块，并把
         右值（可能是**借源视图**，如 `p = q.n`）的 own 指针清空——借源被
         悄悄改成 nil，旧块与借源所有者重复释放（实测 access violation）。 */
      if (lt && sym && sema_sym_ownership(sym) != OWN_OWN &&
          type_has_own_slots(lt) && !sema_value_is_fatal(rhs) &&
          sema_value_holds_own(rhs)) {
        char tn[64], rn[64];
        sema_type_name(lt, tn, sizeof(tn));
        sema_type_name(value_type(rhs), rn, sizeof(rn));
        diag_error(sema->diag, sema_loc(sema, as->value),
                   "cannot copy a value of type %s into variable '%.*s' of "
                   "type %s: own is move-only (use move() or clone())",
                   rn, (int)name.len, name.ptr, tn);
        return;
      }
      if (sym) {
        sym->flow_init = true;
        /* R2 恢复：赋值成功 → moved 清除。**之前**是 moved 的变量，旧内容所有权
           已随 move 转移（内存里是陈旧字节）——重新赋值必须走 STORE_REINIT
           （只 memcpy，不释放旧值），否则运行期 compound_assign_transfer 会按
           旧字节二次释放 own 槽位（实测 0xC0000005）。 */
        if (sym->moved) as->reinit = true;
        sym->moved = false;
        /* 注：这里原先还有一条"含 ref 字段的聚合类型赋值作用域检查"
           （判据 `type_contains_ref`）。所有权从类型层迁移到值/符号层后该谓词
           失效，且复合类型已禁止携带指针/切片，含 ref 字段的聚合不存在——整段
           检查恒不可达，已删除。
           将来恢复含指针的聚合时，判据应改为"聚合的 ref 槽位表"（扁平槽位模型，
           见 docs/ownership-refactor.md §2.5），而不是恢复类型层递归谓词。
           顶层 ref 的重绑定检查见下面的 R3 分支。 */
      }
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

  /* 复合赋值 x op= nil：nil 非 value，无二元运算语义 → 编译错误 */
  if (rhs_nil) {
    diag_error(sema->diag, sema_loc(sema, as->value),
               "cannot use nil in compound assignment");
    return;
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
  /* 限定视图不影响字段定位（写 const 由下方 value_has_const(base) 拒绝） */
  if (!bad) bt = type_strip_quals(bt);
  if (!bad && (!bt || (type_kind_of(bt) != TYPE_KIND_ARRAY &&
                       type_kind_of(bt) != TYPE_KIND_TUPLE &&
                       !type_is_slice(bt)))) {
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

  /* const 数组/切片元素不可写 */
  if (value_has_const(base)) {
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign to element of const array");
    return;
  }
  /* §10.3 [] const T：元素类型是 const → 不可写 */
  if (type_is_slice(bt)) {
    const type_t *elem = slice_type_elem(bt);
    if (elem && type_is_const(elem)) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to element of const slice (element type is const)");
      return;
    }
  }

  /* 元素类型可赋值性（value_assign 单一校验点）：数组取元素类型；
     tuple 按位置取元素类型（索引须编译期常量，sema_expr AST_INDEX 已
     校验越界；此处直接按 0 号元素类型校验可赋值性——元素类型同构，
     任意位置校验等价） */
  const type_t *et;
  ownership_t et_own = OWN_NONE;
  if (type_kind_of(bt) == TYPE_KIND_TUPLE) {
    et = tuple_type_elem(bt, 0) ? tuple_type_elem(bt, 0)->type : NULL;
    /* 元素槽位所有权按编译期下标取（越界已被 sema_expr 拒绝） */
    if (ix->indices && ix->indices->kind == AST_INT_LIT) {
      uint64_t k = ((ast_int_lit_t *)ix->indices)->value;
      const tuple_elem_t *te = tuple_type_elem(bt, (size_t)k);
      if (te) et_own = type_ownership_of(te->type);
    }
  } else if (type_is_slice(bt)) {
    et = slice_type_elem(bt);
    et_own = type_ownership_of(slice_type_elem(bt));
  } else {
    et = array_type_elem(bt);
    et_own = type_ownership_of(array_type_elem(bt));
  }
  if (!et) return;

  /* 元素是 ref 槽位（或含 ref 槽位）：`s[i] = b.p` 把 b 登记为载体的借源。 */

  /* **赋值点旧值销毁**：`a[i] = v` 是纯覆盖（INDEX_SET 只 memcpy），
     旧元素里的 own 堆块必须先释放。own 叶子 / 含 own 槽位的复合元素
     都要展开（ref 元素跳过）。 */
  {
    ast_node_t *dh = NULL, *dt = NULL;
    if (gen_destroy_slot(sema, et, et_own, as->target, &dh, &dt))
      as->destroy = dh;
  }

  value_t *dst = value_make_shadow(sema->vm, et);
  if (value_is_error(sema->vm, value_assign(sema->vm, dst, rhs))) {
    char tn[64], rn[64];
    sema_type_name(et, tn, sizeof(tn));
    sema_type_name(value_type(rhs), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, as->value),
               type_kind_of(bt) == TYPE_KIND_TUPLE
                   ? "cannot assign %s to tuple element of type %s"
                   : "cannot assign %s to array/slice element of type %s",
               rn, tn);
  }
}

/* ---- 字段赋值（p.field = v）：读+写+复合赋值 ----
 * 目标求值（sema_expr AST_MEMBER 返回字段类型 shadow，嵌套 p.a.b 递归）、
 * const 检查（value_has_const(base)）、右值求值（匿名构造注入：字段类型
 * 已知 → push anon_ct）、value_assign 校验可赋值给字段类型。复合赋值
 * 走同一字段类型校验（op 结果可赋回字段）。
 * 运行期由 FIELD_SET 指令写回（compiler 按 op 分派 FIELD_GET/op/FIELD_SET）。
 */
static void shadow_assign_member(sema_t *sema, ast_assign_t *as,
                                 sema_scope_t *scope) {
  ast_member_t *m = (ast_member_t *)as->target;

  /* base 求值 + 可访问校验（sema_expr AST_MEMBER 返回字段类型 shadow；
     非 struct base 已在其中诊断） */
  value_t *base = sema_expr(sema, &m->object, scope);
  bool bad = value_is_error(sema->vm, base) ||
             value_is_type(base, TYPE_KIND_VOID);
  const type_t *bt = bad ? NULL : value_type(base);
  /* 限定视图不影响字段定位（写 const 由下方 value_has_const(base) 拒绝） */
  if (!bad) bt = type_strip_quals(bt);
  /* 指针上的成员写入同样**自动解引用**：`p.field = v` ≡ `p.*.field = v`
     （与读取路径 ex_member 同一改写：包一层 AST_DEREF，compiler 发 PTR_GET）。
     pointee 带 const/volatile 时**拒绝写入**（C 语义：const 只能加不能去）。 */
  if (!bad && bt && ptr_type_base(bt)) {
    const type_t *pbase = ptr_type_base(bt);
    const type_t *ub = type_strip_quals(pbase);
    if (ub && (type_kind_of(ub) == TYPE_KIND_STRUCT ||
               type_kind_of(ub) == TYPE_KIND_CUNION ||
               type_kind_of(ub) == TYPE_KIND_UNION)) {
      if (type_has_quals(pbase)) {
        diag_error(sema->diag, sema_loc(sema, &as->base),
                   "cannot assign to field through a pointer to const value");
        sema_expr(sema, &as->value, scope);
        return;
      }
      ast_node_t *deref =
          ast_deref_new(sema->arena, as->target->tok_begin, as->target->tok_end);
      if (!deref) {
        sema_expr(sema, &as->value, scope);
        return;
      }
      ((ast_deref_t *)deref)->operand = m->object;
      m->object = deref;
      bt = ub;
    }
  }
  if (!bad && (!bt || (type_kind_of(bt) != TYPE_KIND_STRUCT &&
                       type_kind_of(bt) != TYPE_KIND_UNION &&
                       type_kind_of(bt) != TYPE_KIND_CUNION))) {
    char tn[64];
    sema_type_name(bt, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "invalid assignment target: cannot access field of value of "
               "type %s",
               tn);
    bad = true;
  }

  /* 字段存在性 + 字段类型（struct：直接按名查表；union：反查所属 member，
     字段名全局唯一——sema 已校验，命中即得字段类型） */
  int idx = -1;
  const type_t *ft = NULL;
  ownership_t slot_own = OWN_NONE;
  if (!bad) {
    if (type_kind_of(bt) == TYPE_KIND_STRUCT) {
      const struct_type_t *bst = (const struct_type_t *)bt;
      if (bst->bitfield_ast) {
        const ast_struct_def_t *bsd =
            (const ast_struct_def_t *)bst->bitfield_ast;
        bool found = false;
        for (size_t bi = 0; bi < bsd->bitfield_field_count; bi++) {
          const ast_bitfield_field_t *bf = &bsd->bitfield_fields[bi];
          if (bf->name.len == m->field.len &&
              memcmp(bf->name.ptr, m->field.ptr, m->field.len) == 0) {
            const sema_type_t *ut =
                sema_type_by_id(sema, bsd->bitfield_underlying_type_id);
            const type_t *utype =
                ut && ut->type ? ut->type
                               : vm_type_load(sema->vm,
                                              bsd->bitfield_underlying_type_id);
            if (!utype) { bad = true; break; }
            m->is_bitfield = true;
            m->bit_offset = bf->offset;
            m->bit_width = bf->width;
            m->bit_word_bits = bsd->bitfield_word_bits;
            m->bit_value_type_id = bsd->bitfield_underlying_type_id;
            m->bit_storage_type_id = bsd->bitfield_storage_type_id;
            ft = utype;
            slot_own = OWN_NONE;
            found = true;
            break;
          }
        }
        if (!found && !bad) {
          diag_error(sema->diag, sema_loc(sema, &as->base),
                     "bitfield '%.*s' has no field '%.*s'",
                     (int)bt->name.len, bt->name.ptr, (int)m->field.len,
                     m->field.ptr);
          bad = true;
        }
      } else {
        idx = struct_type_find_field(bt, m->field);
        if (idx < 0) {
          diag_error(sema->diag, sema_loc(sema, &as->base),
                     "struct '%.*s' has no field '%.*s'", (int)bt->name.len,
                     bt->name.ptr, (int)m->field.len, m->field.ptr);
          bad = true;
        } else {
          ft = struct_type_field(bt, (size_t)idx)->type;
          slot_own = type_ownership_of(struct_type_field(bt, (size_t)idx)->type);
        }
      }
    } else if (type_kind_of(bt) == TYPE_KIND_UNION) {
      /* union 字段：字段名 = member 名（tag 名），按名反查 member →
         该 member 的 payload 类型即字段类型 */
      int mi = union_type_find_member(bt, m->field);
      if (mi < 0) {
        diag_error(sema->diag, sema_loc(sema, &as->base),
                   "union '%.*s' has no field '%.*s'", (int)bt->name.len,
                   bt->name.ptr, (int)m->field.len, m->field.ptr);
        bad = true;
      } else {
        const union_member_t *mem = union_type_member(bt, (size_t)mi);
        if (!mem || !mem->payload_type) {
          bad = true; /* 防御：字段已命中则 payload 必存在 */
        } else {
          ft = mem->payload_type;
          slot_own = type_ownership_of(mem->payload_type);
        }
      }
    } else {
      /* cunion 字段：字段名 = member 名，按名反查 member → 该 member 的
         type 即字段类型（无 tag 校验，C union 语义） */
      int mi = cunion_type_find_member(bt, m->field);
      if (mi < 0) {
        diag_error(sema->diag, sema_loc(sema, &as->base),
                   "cunion '%.*s' has no field '%.*s'", (int)bt->name.len,
                   bt->name.ptr, (int)m->field.len, m->field.ptr);
        bad = true;
      } else {
        const cunion_member_t *mem = cunion_type_member(bt, (size_t)mi);
        if (!mem || !mem->type) {
          bad = true; /* 防御：字段已命中则 member 类型必存在 */
        } else {
          ft = mem->type;
          slot_own = OWN_NONE;   /* cunion 不参与所有权运算 */
        }
      }
    }
  }

  /* const 字段不可写 */
  if (!bad && value_has_const(base)) {
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign to field of const value");
    return;
  }

  /* 右值求值（匿名构造注入：字段类型已知） */
  bool rhs_nil = as->value->kind == AST_NIL;
  value_t *rhs = NULL;
  if (!rhs_nil) {
    bool ct_pushed = sema_anon_push(sema, ft);
    rhs = sema_expr(sema, &as->value, scope);
    sema_anon_pop(sema, ct_pushed);
  }
  if (bad) return; /* 已有诊断，右值已求值（错误恢复） */
  if (value_is_error(sema->vm, rhs) ||
      value_is_type(rhs, TYPE_KIND_VOID))
    return;


  if (rhs_nil) {
    if (!ft || type_kind_of(ft) != TYPE_KIND_OPTION) {
      char tn[64];
      sema_type_name(ft, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign nil to field '%.*s' (field type %s is not "
                 "optional)",
                 (int)m->field.len, m->field.ptr, tn);
      return;
    }
    /* 改写为匿名构造 .{nil}（AST_CONSTRUCT type=NULL fields=[AST_NIL]）：
       复用 struct 构造 nil 字段路径（compiler 发 PUSH_OPT_NONE <field id>
       → FIELD_SET 隐式转换 ?T→?T 身份短路）。sema_expr 经 anon_ct 注入
       字段类型完成校验。 */
    ast_node_t *nil_node = ast_nil_new(sema->arena, as->value->tok_begin,
                                       as->value->tok_end);
    ast_node_t *ctor = ast_construct_new(sema->arena, as->value->tok_begin,
                                         as->value->tok_end);
    if (nil_node && ctor) {
      ((ast_construct_t *)ctor)->type        = NULL;
      ((ast_construct_t *)ctor)->fields      = nil_node;
      ((ast_construct_t *)ctor)->fields_last = nil_node;
      bool ct_pushed = sema_anon_push(sema, ft);
      sema_expr(sema, &as->value, scope);
      sema_anon_pop(sema, ct_pushed);
    }
    return;
  }

  if (token_is(as->op, "=")) {
    /* own 槽位接收规则（§0.4）：字段/成员位置也是槽位——own 槽位只接
       FATAL，非 own 复合槽位不得 copy OWN 聚合（与构造/`new` 同一张表）。 */
    char what[96];
    snprintf(what, sizeof what, "field '%.*s'", (int)m->field.len,
             m->field.ptr);
    if (!sema_check_slot_flow(sema, slot_own, ft, as->value, rhs, what))
      return;
    /* 简单赋值：value_assign 校验字段类型可赋值性 */
    value_t *dst = value_make_shadow(sema->vm, ft);
    if (value_is_error(sema->vm, value_assign(sema->vm, dst, rhs))) {
      char tn[64], rn[64];
      sema_type_name(ft, tn, sizeof(tn));
      sema_type_name(value_type(rhs), rn, sizeof(rn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign %s to field '%.*s' of type %s", rn,
                 (int)m->field.len, m->field.ptr, tn);
    }
    return;
  }

  /* 复合赋值 p.field op= rhs → p.field = p.field op rhs（shadow 走 vtable
     类型协商，结果须能赋回字段类型） */
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
  value_t *lhs_field = value_make_shadow(sema->vm, ft);
  value_t *result = op(sema->vm, lhs_field, rhs);
  if (value_is_error(sema->vm, result)) {
    char ob[16], tn[64], rn[64];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    sema_type_name(ft, tn, sizeof(tn));
    sema_type_name(value_type(rhs), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "type mismatch: cannot apply '%s' to %s and %s", ob, tn, rn);
    return;
  }
  if (value_is_error(sema->vm, value_assign(sema->vm, lhs_field, result))) {
    char tn[64], rn[64];
    sema_type_name(ft, tn, sizeof(tn));
    sema_type_name(value_type(result), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign %s to field '%.*s' of type %s", rn,
               (int)m->field.len, m->field.ptr, tn);
  }
}

/* ---- M6 FFI：`<库>::<数据符号>` = v（extern var 的写入）----
 *
 * 与读取同源：C ABI 层面就是"往 dlsym 得到的变量地址写 size 字节"。这里做三件事：
 *   ① 解析限定名符号（必须是 SEMA_SYM_EXTERN_VAR——函数符号要报"这是函数"）；
 *   ② 在目标节点上置 is_ffi_var/ffi_var_kind（compiler 据此发 EXTERN_STORE，
 *      契约见 ast_scope_ref.h；读取态的 EXTERN_LOAD 也读同样两个字段）；
 *   ③ 右值类型校验：简单赋值 value_assign(T, rhs)；复合赋值先 shadow 算
 *      `T op rhs` 再校验结果可赋回 T（与普通变量的复合赋值同一套判据）。
 *
 * `nil` 不是合法右值（数据符号没有 optional 形态）。 */
static void shadow_assign_extern_var(sema_t *sema, ast_assign_t *as,
                                     sema_scope_t *scope) {
  ast_scope_ref_t *n = (ast_scope_ref_t *)as->target;

  /* ② 先取符号（type_expr 是库别名 AST_IDENT）。
     `::` 左侧可能是：extern lib 别名（我们的目标）、模块命名空间、枚举类型。
     parser 只放行语法形态，语义在这里分流——每种情况给出各自的精确诊断。 */
  sema_symbol_t *ds = NULL;
  sema_symbol_t *left = NULL;
  const char *libname = NULL;
  size_t liblen = 0;
  if (n->type_expr && n->type_expr->kind == AST_IDENT) {
    ast_ident_t *id = (ast_ident_t *)n->type_expr;
    libname = id->name.ptr;
    liblen = id->name.len;
    left = sema_lookup(scope, id->name);
    char qual[512];
    int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s", (int)id->name.len,
                      id->name.ptr, (int)n->variant.len, n->variant.ptr);
    if (qn > 0 && (size_t)qn < sizeof(qual))
      ds = sema_lookup(sema->global_scope,
                       strslice_from_bytes(qual, (size_t)qn));
    /* port 导入：`port::global = v` 的导出符号是 SEMA_SYM_EXTERN_VAR。 */
    if (!ds && left && left->kind == SEMA_SYM_MODULE && left->module) {
      sema_symbol_t *msym =
          sema_module_export_symbol(left->module, n->variant);
      if (msym && msym->kind == SEMA_SYM_EXTERN_VAR) ds = msym;
    }
  }
  const type_t *ft = (ds && ds->kind == SEMA_SYM_EXTERN_VAR) ? ds->type : NULL;
  if (!ft) {
    if (left && left->kind == SEMA_SYM_MODULE)
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to module member '%.*s::%.*s' (module exports "
                 "are read-only through the namespace)",
                 (int)liblen, libname, (int)n->variant.len, n->variant.ptr);
    else if (left && left->kind == SEMA_SYM_TYPE)
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to '%.*s::%.*s' (enum variant is not a variable)",
                 (int)liblen, libname, (int)n->variant.len, n->variant.ptr);
    else if (ds && ds->kind == SEMA_SYM_FUNC)
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to '%.*s::%.*s': it is an `extern cfunc` "
                 "(a function, not a variable)",
                 (int)liblen, libname, (int)n->variant.len, n->variant.ptr);
    else
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "invalid assignment target: '%.*s::%.*s' is not an "
                 "`extern var` declaration",
                 (int)liblen, libname, (int)n->variant.len, n->variant.ptr);
    sema_expr(sema, &as->value, scope);
    return;
  }
  uint32_t kind = 0;
  if (ffi_data_kind_of(ft, sema->vm, &kind)) {
    /* 标量/指针/str：宽度由类型码表达 */
  } else if (ffi_data_kind_needs_type(ft) && token_is(as->op, "=")) {
    /* struct/cunion/enum：compiler 在写之前压一个类型值（类型表达式取自
       extern var 的声明）。复合赋值（+= 等）对聚合类型无意义 → 拒绝。 */
    kind = CLUX_FFI_DATA_TYPED;
    ast_var_def_t *evd = (ds->ast && ds->ast->kind == AST_VAR_DEF)
                             ? (ast_var_def_t *)ds->ast
                             : NULL;
    n->ffi_var_type_expr = evd ? evd->type_expr : NULL;
  } else {
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign to '%.*s::%.*s': type is not supported for "
               "extern var", (int)liblen, libname, (int)n->variant.len,
               n->variant.ptr);
    sema_expr(sema, &as->value, scope);
    return;
  }
  n->is_ffi_var = true;
  n->ffi_var_kind = kind;
  n->ffi_var_type = ft;
  if (ds->ast && ds->ast->kind == AST_VAR_DEF)
    n->ffi_c_name = ((ast_var_def_t *)ds->ast)->name;
  else
    n->ffi_c_name = n->variant;

  if (as->value->kind == AST_NIL) {
    diag_error(sema->diag, sema_loc(sema, as->value),
               "cannot assign nil to `extern var` (no optional form for a "
               "library data symbol)");
    return;
  }

  /* ③ 右值求值 + 类型校验（简单 / 复合两条路径） */
  value_t *rhs = sema_expr(sema, &as->value, scope);
  if (value_is_error(sema->vm, rhs) || value_is_type(rhs, TYPE_KIND_VOID))
    return;

  value_t *dst = value_make_shadow(sema->vm, ft);
  if (token_is(as->op, "=")) {
    if (value_is_error(sema->vm, value_assign(sema->vm, dst, rhs))) {
      char tn[64], rn[64];
      sema_type_name(ft, tn, sizeof(tn));
      sema_type_name(value_type(rhs), rn, sizeof(rn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign %s to `%.*s::%.*s` of type %s", rn,
                 (int)liblen, libname, (int)n->variant.len, n->variant.ptr, tn);
    }
    return;
  }
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
  value_t *result = op(sema->vm, dst, rhs);
  if (value_is_error(sema->vm, result)) {
    char ob[16], tn[64], rn[64];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    sema_type_name(ft, tn, sizeof(tn));
    sema_type_name(value_type(rhs), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "type mismatch: cannot apply '%s' to %s and %s", ob, tn, rn);
    return;
  }
  if (value_is_error(sema->vm, value_assign(sema->vm, dst, result))) {
    char tn[64], rn[64];
    sema_type_name(ft, tn, sizeof(tn));
    sema_type_name(value_type(result), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, as->value),
               "cannot assign %s to `%.*s::%.*s` of type %s", rn,
               (int)liblen, libname, (int)n->variant.len, n->variant.ptr, tn);
  }
}

/* ---- 解引用左值赋值（r.* = v，m3-design §8.2 解引用 SET 严格分离）----
 * 目标求值（指针操作数须为 own/ref/fatal 指针 → 取被指向类型 T 为字段
 * 类型；非指针已在诊断中报错）、const 检查（被指向类型含 const → 不可
 * 写）、右值求值（匿名构造注入 T）、value_assign 校验可赋值给 T。运行期
 * 由 PTR_SET 指令写回（compiler 按 op 分派 PTR_GET/op/PTR_SET）。 */
static void shadow_assign_deref(sema_t *sema, ast_assign_t *as,
                                sema_scope_t *scope) {
  ast_deref_t *n = (ast_deref_t *)as->target;
  /* 指针操作数求值（sema_expr AST_DEREF 分支：非指针已在其中诊断） */
  value_t *base = sema_expr(sema, &n->operand, scope);
  bool bad = value_is_error(sema->vm, base) ||
             value_is_type(base, TYPE_KIND_VOID);
  const type_t *bt = bad ? NULL : value_type(base);
  /* 限定视图不影响字段定位（写 const 由下方 value_has_const(base) 拒绝） */
  if (!bad) bt = type_strip_quals(bt);
  const type_t *ft = bad ? NULL : ptr_type_base(bt);
  if (!bad && !ft) {
    char tn[64];
    sema_type_name(bt, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "invalid assignment target: cannot dereference a value of "
               "type %s (expected a pointer)",
               tn);
    bad = true;
  }

  /* 被指向类型含 const → 不可写（ref 可写性取决于被指向是否 const） */
  if (!bad && type_has_const(ft)) {
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign through pointer to const value");
    return;
  }

  /* 右值求值（匿名构造注入：被指向类型已知） */
  bool rhs_nil = as->value->kind == AST_NIL;
  value_t *rhs = NULL;
  if (!rhs_nil) {
    bool ct_pushed = sema_anon_push(sema, ft);
    rhs = sema_expr(sema, &as->value, scope);
    sema_anon_pop(sema, ct_pushed);
  }
  if (bad) return; /* 已有诊断，右值已求值（错误恢复） */
  if (value_is_error(sema->vm, rhs) ||
      value_is_type(rhs, TYPE_KIND_VOID))
    return;

  /* 字段是 ref 槽位（或含 ref 槽位）：`s2.q = b.p` 把 b 登记为 s2 的借源
     ——s2 里存着指向 b 拥有堆块的 ref，b 被 move 后该 ref 悬垂。 */

  if (rhs_nil) {
    if (!ft || type_kind_of(ft) != TYPE_KIND_OPTION) {
      char tn[64];
      sema_type_name(ft, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign nil through pointer (pointee type %s is not "
                 "optional)",
                 tn);
      return;
    }
    /* 改写为匿名构造 .{nil}（与 shadow_assign_member 同款：复用 struct
       构造 nil 字段路径，compiler 发 PUSH_OPT_NONE → PTR_SET 隐式转换
       ?T→?T 身份短路）。 */
    ast_node_t *nil_node = ast_nil_new(sema->arena, as->value->tok_begin,
                                       as->value->tok_end);
    ast_node_t *ctor = ast_construct_new(sema->arena, as->value->tok_begin,
                                         as->value->tok_end);
    if (nil_node && ctor) {
      ((ast_construct_t *)ctor)->type        = NULL;
      ((ast_construct_t *)ctor)->fields      = nil_node;
      ((ast_construct_t *)ctor)->fields_last = nil_node;
      bool ct_pushed = sema_anon_push(sema, ft);
      sema_expr(sema, &as->value, scope);
      sema_anon_pop(sema, ct_pushed);
    }
    return;
  }

  if (token_is(as->op, "=")) {
    /* **赋值点旧值销毁**：`p.* = v` 是纯覆盖（PTR_SET 只 memcpy），
       旧被指向值里的 own 槽位必须先释放。只对 **own 指针**生成——
       经 ref 指针写别人拥有的内容时不能替对方释放（借用方不拥有）。 */
    if (value_type_ownership(base) == OWN_OWN && ft) {
      ast_node_t *dh = NULL, *dt = NULL;
      if (gen_destroy(sema, ft, as->target, &dh, &dt)) as->destroy = dh;
    }
    /* 简单赋值：value_assign 校验被指向类型可赋值性 */
    value_t *dst = value_make_shadow(sema->vm, ft);
    if (value_is_error(sema->vm, value_assign(sema->vm, dst, rhs))) {
      char tn[64], rn[64];
      sema_type_name(ft, tn, sizeof(tn));
      sema_type_name(value_type(rhs), rn, sizeof(rn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign %s through pointer to %s", rn, tn);
    }
    return;
  }

  /* 复合赋值 r.* op= v → r.* = r.* op v（shadow 走 vtable 类型协商，
     结果须能赋回被指向类型） */
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
  value_t *lhs_field = value_make_shadow(sema->vm, ft);
  value_t *result = op(sema->vm, lhs_field, rhs);
  if (value_is_error(sema->vm, result)) {
    char ob[16], tn[64], rn[64];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    sema_type_name(ft, tn, sizeof(tn));
    sema_type_name(value_type(rhs), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "type mismatch: cannot apply '%s' to %s and %s", ob, tn, rn);
    return;
  }
  if (value_is_error(sema->vm, value_assign(sema->vm, lhs_field, result))) {
    char tn[64], rn[64];
    sema_type_name(ft, tn, sizeof(tn));
    sema_type_name(value_type(result), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign %s through pointer to %s", rn, tn);
  }
}
