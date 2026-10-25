#include "sema/sema.h"
#include "sema/reflect.h"
#include "vm/type_ownership.h"
#include "sema/struct_compose.h" /* 嵌入继承的成员名字查找 */
#include "core/string.h"
#include "parser/ast_array.h"
#include "parser/ast_binary.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_return.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_const.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_error.h"
#include "parser/ast_fill.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_spread.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_member.h"
#include "parser/ast_move.h"
#include "parser/ast_new.h"
#include "parser/ast_deref.h"
#include "parser/ast_addr.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_slice.h"
#include "parser/ast_make.h"
#include "parser/ast_nil.h"
#include "parser/ast_option.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_ternary.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_typeof.h"
#include "parser/ast_alignof.h"
#include "parser/ast_sizeof.h"
#include "parser/ast_unary.h"
#include "parser/ast_undef.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_volatile.h"
#include "parser/ast_assign.h"
#include "parser/ast_var_def.h"
#include "parser/ast_block.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_is_tag.h"
#include "parser/ast_opt_is_some.h"
#include "parser/ast_clone.h"
#include "parser/ast_take.h"
#include "parser/ast_take_slot.h"
#include "parser/ast_for_index.h"
#include "parser/lexer.h"
#include "ctfe/ctfe.h"
#include "sema/comptime.h"
#include "vm/function.h"
#include "vm/type_array.h"
#include "vm/type_enum.h"
#include "vm/type_error.h"
#include "vm/type_func.h"
#include "vm/type_cfunc.h"
#include "vm/type_option.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/type_cunion.h"
#include "vm/type_int.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_opaque.h"
#include "vm/module.h"
#include "core/ffi_abi.h" /* M6 FFI：ffi_data_kind_of（extern var 的类型码） */
#include "core/ffi_abi.h"   /* M6 FFI：BCODE_FFI_RET_*（extern cfunc 的返回类型码） */
#include "core/strmap.h"

#include <stdio.h>
#include <string.h>


#include "sema_expr_internal.h"

static value_t *shadow_binary(sema_t *sema, ast_node_t **node, sema_scope_t *scope);


/* 二元运算符 token → vtable 分派函数 */
static value_t *(*binop_of(const token_t *op))(vm_t *, value_t *, value_t *) {
  if (token_is(op, "+")) return value_add;
  if (token_is(op, "-")) return value_sub;
  if (token_is(op, "*")) return value_mul;
  if (token_is(op, "/")) return value_div;
  if (token_is(op, "%")) return value_mod;
  if (token_is(op, "==")) return value_eq;
  if (token_is(op, "!=")) return value_ne;
  if (token_is(op, "<")) return value_lt;
  if (token_is(op, "<=")) return value_le;
  if (token_is(op, ">")) return value_gt;
  if (token_is(op, ">=")) return value_ge;
  if (token_is(op, "&")) return value_band;
  if (token_is(op, "|")) return value_bor;
  if (token_is(op, "^")) return value_bxor;
  if (token_is(op, "<<")) return value_shl;
  if (token_is(op, ">>")) return value_shr;
  return NULL;
}


/* ===========================================================================
 * 表达式 walker（shadow value 求值）
 *
 * 每个表达式节点返回一个 shadow value（只有类型，data=NULL）。shadow value
 * 经 vtable 运算路径，类型协商结果即为推导结果类型。错误恢复产物
 * （error/void shadow）沿运算传播但不级联二次诊断。
 *
 * 未初始化（TDZ）检查由确定性赋值分析在符号表 flow_init 上完成
 * （sema_lookup 跳过未激活符号），VM 值层不感知。
 * =========================================================================== */

/* 把操作数类型名写入诊断缓冲区 */
void op_type_name(value_t *v, char *buf, size_t cap) {
  if (!v) {
    snprintf(buf, cap, "<none>");
    return;
  }
  const type_t *t = value_type(v);
  if (t && t->name.ptr)
    snprintf(buf, cap, "%.*s", (int)t->name.len, t->name.ptr);
  else
    snprintf(buf, cap, "<none>");
}

/* 检查操作数必须为 bool；错误/void shadow（错误恢复产物）静默通过 */
void sema_check_bool(sema_t *sema, ast_node_t *node, value_t *v,
                     const char *what) {
  if (value_is_error(sema->vm, v)) return;
  if (value_is_type(v, TYPE_KIND_VOID)) return;
  if (!value_is_type(v, TYPE_KIND_BOOL)) {
    char tn[64];
    op_type_name(v, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, node), "%s operand must be bool, got %s",
               what, tn);
  }
}

/* 从 token 取运算符文本（诊断用），写入 buf */
void op_text(const token_t *op, char *buf, size_t cap) {
  size_t len = 0;
  const char *text = op ? token_get_text(op, &len) : NULL;
  if (!text || len == 0) {
    snprintf(buf, cap, "?");
    return;
  }
  size_t n = len < cap - 1 ? len : cap - 1;
  memcpy(buf, text, n);
  buf[n] = '\0';
}

/* ===========================================================================
 * 表达式求值的公共基础设施
 *
 * 这两个模式在 sema_expr 及其各 case 里重复上百次，抽成助手后逐 kind 处理器
 * 才可能保持可读（拆分 sema_expr 的前置工作）。
 * =========================================================================== */

/** "错误/未定" 占位值：void shadow（data=NULL，类型 void）。
 *  表达式求值失败的统一返回值——调用方经 is_poison 识别后短路。 */
value_t *val_void(sema_t *sema) {
  return value_make_shadow(sema->vm, sema->vm->type_void);
}

/** 求值结果是否不可用（错误 value / void shadow / NULL）。
 *  三者都表示"这一步没有可用类型信息"，调用方应立刻传播而非继续推断。 */
bool is_poison(sema_t *sema, const value_t *v) {
  if (!v) return true;
  return value_is_error(sema->vm, v) || value_is_type(v, TYPE_KIND_VOID);
}

/* 槽位接收规则（§0.4/§0.5，与 own 变量定义/形参同一张表）——聚合构造、
   `new` 的字段/成员/元素初始化、字段赋值共用（声明见 sema.h）：
   - **own 槽位**只接受将亡值（FATAL）：new / move / clone / 构造产物 /
     fatal 返回值。直接放 own/ref 变量 = copy → 内外两个位置各释放一次
     （实测 0xC0000005），或把 ref 借用目标当自己的一样 free 掉。
   - **非 own 槽位**不得用 copy 收下 OWN 聚合（type_has_own_slots）——
     own 是 move-only，复制会让两个聚合共享内嵌 own 堆块。
   复合槽位接 FATAL 是**转移**（op_construct 只 memcpy，源临时值不再持有），
   这是嵌套构造 `.S{ .b = .B{...} }` 的合法形态。 */
bool sema_check_slot_flow(sema_t *sema, ownership_t slot_own,
                          const type_t *slot_type, ast_node_t *value,
                          value_t *fv, const char *what) {
  if (!fv || value_is_error(sema->vm, fv) ||
      value_is_type(fv, TYPE_KIND_VOID))
    return true; /* 错误恢复产物，已诊断 */
  if (slot_own == OWN_OWN) {
    if (sema_value_is_fatal(fv)) return true;
    char fn[64];
    sema_type_name(value_type(fv), fn, sizeof fn);
    diag_error(sema->diag, sema_loc(sema, value),
               "cannot initialize %s with %s: an own slot must take over a "
               "dying value (FATAL), produced by new / move / clone (or a "
               "fatal-returning call)",
               what, fn);
    return false;
  }
  if (slot_own == OWN_NONE && slot_type && type_has_own_slots(slot_type) &&
      !sema_value_is_fatal(fv) && sema_value_holds_own(fv)) {
    char tn[64];
    sema_type_name(slot_type, tn, sizeof tn);
    diag_error(sema->diag, sema_loc(sema, value),
               "cannot initialize %s by copying a value of type %s that holds "
               "owned resources: own is move-only (use move() or clone())",
               what, tn);
    return false;
  }
  return true;
}

value_t *sema_expr(sema_t *sema, ast_node_t **node, sema_scope_t *scope) {
  if (!node || !*node) return val_void(sema);
  switch ((*node)->kind) {
    case AST_INT_LIT:
      return ex_int_lit(sema, node, scope);
    case AST_FLOAT_LIT:
      return ex_float_lit(sema, node, scope);
    case AST_BOOL_LIT:
      return value_make_shadow(sema->vm, sema->vm->type_bool);
    case AST_CHAR_LIT:
      return value_make_shadow(sema->vm, sema->vm->type_u8);
    case AST_STRING_LIT:
      return value_make_shadow(sema->vm, sema->vm->type_str);
    case AST_IDENT:
      return ex_ident(sema, node, scope);
    case AST_CONST:
      return ex_const(sema, node, scope);
    case AST_VOLATILE:
      return ex_volatile(sema, node, scope);
    case AST_SIZEOF:
      return ex_sizeof_alignof(sema, node, scope);
    case AST_ALIGNOF:
      return ex_sizeof_alignof(sema, node, scope);
    case AST_TYPEOF:
      return ex_typeof(sema, node, scope);
    case AST_UNDEF:
      /* undefined 只允许作为 var 初始化的"未初始化声明"（shadow_var_def
         消费）；普通表达式位置引用是非法用法。 */
      diag_error(sema->diag, sema_loc(sema, *node),
                 "'undefined' can only be used as a variable initializer");
      return val_void(sema);
    case AST_NIL:
      /* nil 不是 value：只允许四种 nil 判定（x==nil 等，窄化）、?T 初始化
         赋值（a = nil）、构造器字段（.?T{nil}）与 fill 值包（<nil,N>）等
         特殊位置消费（shadow_binary / 赋值 / 构造器 / fill 分别处理）。
         普通表达式位置引用是非法用法。 */
      diag_error(sema->diag, sema_loc(sema, *node),
                 "'nil' can only be used in nil comparisons, ?T assignment, "
                 "constructor fields or fills");
      return val_void(sema);
    case AST_BINARY:
      return shadow_binary(sema, node, scope);
    case AST_UNARY:
      return ex_unary(sema, node, scope);
    case AST_CALL:
      return ex_call(sema, node, scope);
    case AST_UNWRAP:
      return ex_unwrap(sema, node, scope);
    case AST_MEMBER:
      return ex_member(sema, node, scope);
    case AST_INDEX:
      return ex_index(sema, node, scope);
    case AST_SLICE_TYPE:
      return ex_slice_type(sema, node, scope);
    case AST_SLICE:
      return ex_slice(sema, node, scope);
    case AST_ARRAY:
      return ex_array(sema, node, scope);
    case AST_TUPLE:
      return ex_type_expr(sema, node, scope);
    case AST_FUNC_TYPE:
      return ex_type_expr(sema, node, scope);
    case AST_OPTION:
      return ex_type_expr(sema, node, scope);
    case AST_PTR:
      return ex_type_expr(sema, node, scope);
    case AST_OWNED_TYPE:
      /* `ref opaque` / `fatal T` / `own T` 出现在**表达式位置**（典型
         `sizeof(ref opaque)`）：与 AST_PTR 同族，求值为类型值。 */
      return ex_type_expr(sema, node, scope);
    case AST_TYPE_REF:
      return ex_type_ref(sema, node, scope);
    case AST_SCOPE_REF:
      return ex_scope_ref(sema, node, scope);
    case AST_NEW:
      return ex_new(sema, node, scope);
    case AST_MAKE:
      return ex_make(sema, node, scope);
    case AST_DEREF:
      return ex_deref(sema, node, scope);
    case AST_ADDR:
      return ex_addr(sema, node, scope);
    case AST_MOVE:
      return ex_move(sema, node, scope);
    case AST_CONSTRUCT:
      return ex_construct(sema, node, scope);
    case AST_FUNC_DEF:
      return ex_func_def(sema, node, scope);
    case AST_FUNC_REF:
      return ex_func_ref(sema, node, scope);
    case AST_TERNARY:
      return ex_ternary(sema, node, scope);
    case AST_ERROR:
      /* 错误恢复产物：静默返回 void shadow（调用方按"错误/未定"处理，
         避免级联二次诊断）。 */
      return val_void(sema);
    case AST_FILL: {
      /* `<值, 次数>` 填充元素：只允许出现在**数组构造**与 make(...) 的
         元素位——那两处由 AST_CONSTRUCT 数组分支 / AST_MAKE 自行识别处理，
         不会把 fill 交给 sema_expr。走到这里说明它落在 struct/union 字段位
         或别的表达式位置：明确报错。此前无 case，落 default 静默返回 void
         shadow ⇒ 字段可赋值性校验被跳过、fill 节点被交给编译器，最终以
         "unsupported expression node 'fill'" 这种内部文案暴露给用户。 */
      diag_error(sema->diag, sema_loc(sema, *node),
                 "fill element '<value, count>' is only allowed in array "
                 "constructs and in make(...) element lists");
      return val_void(sema);
    }
    default:
      /* 未处理的表达式节点 = 编译器自身缺陷（新增 AST kind 漏了分支）。
         显式报内部诊断而不是静默返回 void shadow——后者会让漏分支表现为
         "校验被跳过"，错误在更晚处以误导性形式出现。 */
      diag_error(sema->diag, sema_loc(sema, *node),
                 "internal: unhandled expression node '%s'",
                 ast_kind_name((*node)->kind));
      return val_void(sema);
  }
}

static value_t *shadow_binary(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  ast_binary_t *b = (ast_binary_t *)*node;
  /* as：显式类型转换（普通中缀运算符）。lhs shadow 求值；rhs 是类型表达式，
     走 ctfe 编译期真实求值（调用 ctfe_eval），拿真实
     type value 的 data（type_t*）作为转换目标——遮蔽语义在此感知。 */
  if (token_is(b->op, "as")) {
    value_t *expr = sema_expr(sema, &b->lhs, scope);
    if (is_poison(sema, expr))
      return val_void(sema);
    /* rhs 是类型表达式：sema_resolve_type_slot 解析为真实类型并就地替换
       为 AST_TYPE_REF（与 AST_ARRAY/AST_SCOPE_REF 同构——运行期 compiler
       的 compile_type_expr 命中 AST_TYPE_REF 发 LOAD_TYPE，保证 as 右侧
       压入真实 type value；否则复合类型表达式如 own *i32 落默认分支发
       PUSH_UNDEFINED，CAST 弹 undefined 崩溃）。 */
    const type_t *target = sema_resolve_type_slot(sema, &b->rhs);
    if (!target) {
      char tn[64];
      op_type_name(expr, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "cast target is not a type (operand is %s)", tn);
      return val_void(sema);
    }
    value_t *result = value_explicit_cast(sema->vm, expr, target);
    if (value_is_error(sema->vm, result)) {
      char tn[64], tt[64];
      op_type_name(expr, tn, sizeof(tn));
      sema_type_name(target, tt, sizeof(tt));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "cannot cast %s to %s", tn, tt);
      return val_void(sema);
    }
    return result;
  }

  /* is：tag union tag 判定（<expr> is <member>，docs m2-design §tag
     union）。lhs shadow 求值（必须 union 类型）；rhs 是 member 名表达式
     （AST_IDENT）→ 编译期查 member → 折叠 rhs 为 AST_INT_LIT（member 的
     tag 整数值，compiler 发 IS_TAG 立即数，零感知 union 类型）。member
     名 = 字段名（struct 同构平铺字段），直接按名查 member
     （union_type_find_member）。合法 → 返回 bool shadow。 */
  if (token_is(b->op, "is")) {
    value_t *expr = sema_expr(sema, &b->lhs, scope);
    if (is_poison(sema, expr))
      return val_void(sema);
    const type_t *ut = value_type(expr);
    if (!ut || type_kind_of(ut) != TYPE_KIND_UNION) {
      char tn[64];
      op_type_name(expr, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "is: left operand must be a union value, got %s", tn);
      return val_void(sema);
    }
    /* rhs：member 名（AST_IDENT）。编译期校验 member 存在 + 折叠为 tag
       整数（compiler 发 IS_TAG 立即数）。非 member 名表达式（如函数调用/
       复合）报错。 */
    if (!b->rhs || b->rhs->kind != AST_IDENT) {
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "is: right operand must be a member name");
      return val_void(sema);
    }
    const strslice_t mname = ((ast_ident_t *)b->rhs)->name;
    const int mi = union_type_find_member(ut, mname);
    if (mi < 0) {
      char tn[64];
      sema_type_name(ut, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "is: union %s has no member '%.*s'", tn,
                 (int)mname.len, mname.ptr);
      return val_void(sema);
    }
    /* 折叠 rhs → AST_INT_LIT（tag 值）：compiler 发 IS_TAG <tag> 立即数。
       与 comptime 折叠同模式：sema 阶段确定 member 归属，下游零感知。 */
    const union_member_t *m = union_type_member(ut, (size_t)mi);
    ast_node_t *lit = ast_int_lit_new(sema->arena, b->rhs->tok_begin,
                                      b->rhs->tok_end);
    if (!lit) return val_void(sema);
    ((ast_int_lit_t *)lit)->value = m ? (uint64_t)m->tag : 0u;
    b->rhs = lit;
    return value_make_shadow(sema->vm, sema->vm->type_bool);
  }

  /* extends：类型兼容判断（编译期类型计算，ctfe 求值后折叠为常量）。
     两侧都是类型表达式（类型即表达式），ctfe 真实
     求值（scope 中的类型值为真实 type value，非 shadow），value_extends
     分派 vtable->extends → bool。成功后把二元节点折叠为 AST_BOOL_LIT
     写回（保留兄弟链），下游编译器零感知——纯编译期，无运行时指令。 */
  if (token_is(b->op, "extends")) {
    vm_t *vm = sema->vm;
    ctfe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vm = vm;
    ctx.sema = sema;
    ctx.budget = 100000;
    ctx.max_depth = 128;
    value_t *r = ctfe_eval(&ctx, *node);
    if (!r || value_is_error(vm, r)) {
      const char *msg = NULL;
      if (r) {
        error_data_t *ed = (error_data_t *)value_data(r);
        msg = ed && ed->message ? ed->message : NULL;
      }
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "extends: not a compile-time type computation%s%s",
                 msg ? ": " : "", msg ? msg : "");
      return val_void(sema);
    }
    if (value_type(r) != vm->type_bool) {
      char tn[64];
      sema_type_name(value_type(r), tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "extends must yield bool (got %s)", tn);
      return val_void(sema);
    }
    sema_ct_const_t ct;
    if (!sema_ct_encode(sema, r, &ct)) {
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "extends result cannot be folded");
      return val_void(sema);
    }
    ast_node_t *lit = sema_ct_lit(sema, *node, &ct);
    if (!lit) {
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "extends result cannot be folded");
      return val_void(sema);
    }
    lit->next = (*node)->next; /* 保留兄弟链 */
    *node = lit;
    return value_make_shadow(sema->vm, vm->type_bool);
  }

  /* 短路 && / ||：操作数必须 bool，结果 bool */
  if (token_is(b->op, "&&") || token_is(b->op, "||")) {
    value_t *lhs = sema_expr(sema, &b->lhs, scope);
    sema_check_bool(sema, b->lhs, lhs, "logical operator");
    value_t *rhs = sema_expr(sema, &b->rhs, scope);
    sema_check_bool(sema, b->rhs, rhs, "logical operator");
    return value_make_shadow(sema->vm, sema->vm->type_bool);
  }

  /* nil 比较（docs m2-design §12.5）：x==nil / nil==x / x!=nil / nil!=x
     四种判定形式是 ?T 唯一合法比较（?T == ?T 直接比较报错，须先窄化）。
     nil 不是 value：不进入 vtable eq 分派，sema 直接识别符号形态 →
     返回 bool shadow。x 非 ?T 与 nil 比较 → 编译错误（str/func 无空值）。 */
  if ((token_is(b->op, "==") || token_is(b->op, "!=")) &&
      (b->lhs->kind == AST_NIL || b->rhs->kind == AST_NIL)) {
    /* 另一侧可以是**任意 ?T 表达式**（变量 / 字段 / 下标 / 解引用 / 解包 /
       调用结果 …）——早期只允许变量是过严限制（`n.next != nil` 无法写，
       链表判空只能先绑变量）。这里对表达式求值一次再判类型；编译期为
       OPT_IS_NONE（tag 专用指令），不进入 vtable eq 分派。 */
    /* 注意：必须把**节点槽位**交给 sema_expr（sema 可能把表达式改写/折叠，
       例如调用结果落成临时变量）——只传局部副本会让 compiler 仍编译旧节点，
       运行期报 "undefined variable"。 */
    ast_node_t **other_slot = (b->lhs->kind == AST_NIL) ? &b->rhs : &b->lhs;
    value_t *ov = sema_expr(sema, other_slot, scope);
    if (value_is_error(sema->vm, ov) ||
        value_is_type(ov, TYPE_KIND_VOID))
      return value_make_shadow(sema->vm, sema->vm->type_bool);
    const type_t *ot = value_type(ov);
    if (ot) ot = type_strip_quals(ot);
    if (!ot || type_kind_of(ot) != TYPE_KIND_OPTION) {
      char tn[64];
      sema_type_name(ot, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "value of type %s is not optional; cannot compare with nil",
                 tn);
    }
    return value_make_shadow(sema->vm, sema->vm->type_bool);
  }

  value_t *lhs = sema_expr(sema, &b->lhs, scope);
  value_t *rhs = sema_expr(sema, &b->rhs, scope);
  /* 错误恢复产物（error/void shadow）静默通过，避免级联二次诊断 */
  if (value_is_error(sema->vm, lhs) || value_is_error(sema->vm, rhs) ||
      value_is_type(lhs, TYPE_KIND_VOID) || value_is_type(rhs, TYPE_KIND_VOID))
    return val_void(sema);
  value_t *(*op)(vm_t *, value_t *, value_t *) = binop_of(b->op);
  if (!op) {
    char ob[16];
    op_text(b->op, ob, sizeof(ob));
    diag_error(sema->diag, sema_loc(sema, &b->base),
               "unsupported binary operator '%s'", ob);
    return val_void(sema);
  }
  value_t *result = op(sema->vm, lhs, rhs);
  if (value_is_error(sema->vm, result)) {
    char ob[16], ln[64], rn[64];
    op_text(b->op, ob, sizeof(ob));
    op_type_name(lhs, ln, sizeof(ln));
    op_type_name(rhs, rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &b->base),
               "type mismatch: cannot apply '%s' to %s and %s", ob, ln, rn);
    return val_void(sema);
  }
  /* 运行期协商预检（shadow 分派放行 → 运行期真实值才暴露的错误提前到
     sema）：两操作数类型不同时，模拟 VTABLE_BINARY 的协商——type_promote
     优先；协商失败则尝试隐式转 b 到 a（b 更窄）；两路都失败 → 运行期
     implicit_cast 必然报错，此处编译期报错（快速失败）。 */
  if (value_type(lhs) != value_type(rhs)) {
    const type_t *ta = value_type(lhs), *tb = value_type(rhs);
    const type_t *rt = type_promote(sema->vm, ta, tb);
    if (!rt) {
      value_t *castb = value_implicit_cast(sema->vm, rhs, ta);
      if (value_is_error(sema->vm, castb)) {
        char ob[16], ln[64], rn[64];
        op_text(b->op, ob, sizeof(ob));
        op_type_name(lhs, ln, sizeof(ln));
        op_type_name(rhs, rn, sizeof(rn));
        diag_error(sema->diag, sema_loc(sema, &b->base),
                   "type mismatch: cannot apply '%s' to %s and %s", ob, ln, rn);
        return val_void(sema);
      }
    }
  }
  return result; /* shadow in → shadow out */
}
