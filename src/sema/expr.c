#include "sema/sema.h"
#include "core/string.h"
#include "parser/ast_array.h"
#include "parser/ast_binary.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_const.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_enum_ref.h"
#include "parser/ast_error.h"
#include "parser/ast_fill.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_member.h"
#include "parser/ast_nil.h"
#include "parser/ast_option.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_ternary.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_unary.h"
#include "parser/ast_undef.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_volatile.h"
#include "parser/lexer.h"
#include "ctfe/ctfe.h"
#include "sema/comptime.h"
#include "vm/function.h"
#include "vm/type_array.h"
#include "vm/type_enum.h"
#include "vm/type_error.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"

#include <stdio.h>
#include <string.h>

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
static void op_type_name(value_t *v, char *buf, size_t cap) {
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

/* 函数自身符号归属检查（捕获检查用）：sym 是函数可见链上的符号（fscope
   直系捕获 / param_scope 直系参数）→ true。false = 外层局部（需捕获）或
   非函数自身符号。参数在参数层（fscope 子 scope），捕获在 fscope——参数
   遮蔽捕获，两者同属函数自身符号，放行。 */
static bool func_own_symbol(sema_t *sema, strslice_t name,
                            sema_symbol_t *sym) {
  if (!sema->local_func_base) return false;
  if (sema_scope_find_local(sema->local_func_base, name) == sym) return true;
  if (sema->local_func_param_scope &&
      sema_scope_find_local(sema->local_func_param_scope, name) == sym)
    return true;
  return false;
}

/* 闭包 TDZ 检查（编译期）：被引用/调用的局部函数若有捕获（fscope 捕获符号
   存在），其捕获值在定义点 resolve_func_captures 才绑定（is_active 激活）。
   walk 顺序保证：定义点前的引用点（函数提升后）捕获符号尚未激活 → TDZ，
   编译期报错（捕获槽运行期仍为 undefined 占位，但不应静默到运行期才暴露）。
   无捕获的局部函数提升后即可安全引用（hoist 只绑定地址，无捕获槽）。 */
static bool func_capture_tdz(sema_scope_t *fscope, ast_node_t *captures) {
  if (!fscope || !captures) return false;
  for (ast_node_t *c = captures; c; c = c->next) {
    ast_var_def_t *cv = (ast_var_def_t *)c;
    sema_symbol_t *cs = sema_scope_find_local(fscope, cv->name);
    if (cs && !cs->is_active) return true;
  }
  return false;
}

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

/* 从 token 取运算符文本（诊断用），写入 buf */
static void op_text(const token_t *op, char *buf, size_t cap) {
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

static value_t *shadow_binary(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope);

/* 匿名构造 .{...} 字段求值 + 匿名 struct 类型推断（m2-design §2）：
 * 1. 第一遍：按目标 struct 字段表收集字段值节点（具名字段按名匹配、匿名
 *    字段按序顺延）——乱序具名字段（.y=2,.x=1）在此重排为表序。
 * 2. 第二遍：逐字段求值（注入 anon_ct=目标字段类型，嵌套匿名构造/嵌套
 *    struct 构造可正常推断）+ value_assign safe_cast 校验（i32 字面量 →
 *    i64 字段提升，与具名构造一致）；nil 字段须目标字段为 ?T。
 * 3. 第三遍：字段表 = (目标字段名, 目标字段类型, 目标序) → type_struct_intern
 *    构造匿名类型（按字段名+类型+顺序去重 intern——与目标同构时合并为
 *    同一实例，天然零转换）+ 登记 sema_type（进 hoist 提升区）。
 * 4. 第四遍：把 fields 链重排为表序（值节点直链）——运行期 op_construct
 *    按表序布值（offset 查表），compiler 按链序压值，链序必须 == 表序。
 * 返回推断类型；失败（字段数/字段名/类型不兼容/OOM）返回 NULL。
 * 注意：调用方（AST_CONSTRUCT 匿名分支）已保证 ct 为 struct 类型且
 * anon_ct 非空。字段已在此求值+校验完毕，调用方须直接返回不落入
 * struct 构造分支（否则二次求值会重复诊断）。 */
static const type_t *infer_anon_struct(sema_t *sema, ast_construct_t *n,
                                       sema_scope_t *scope,
                                       const type_t *ct) {
  const struct_type_t *cst = (const struct_type_t *)ct;
  size_t cap = cst->field_count;
  size_t nfields = sema_count_siblings(n->fields);
  if (nfields != cap) {
    char tn[64];
    sema_type_name(ct, tn, sizeof tn);
    diag_error(sema->diag, sema_loc(sema, n->fields ? n->fields : &n->base),
               "construct: expected %zu fields for %s, got %zu", cap, tn,
               nfields);
    return NULL;
  }
  struct_field_t *fields = NULL;
  ast_node_t **slot = NULL;
  if (cap > 0) {
    fields = (struct_field_t *)arena_calloc(
        sema->arena, cap, sizeof(struct_field_t), ALIGNOF(max_align_t));
    slot = (ast_node_t **)arena_calloc(
        sema->arena, cap, sizeof(ast_node_t *), ALIGNOF(max_align_t));
    if (!fields || !slot) return NULL;
  }

  /* 第一遍：按目标表收集字段值节点（具名按名匹配，匿名按序顺延） */
  size_t anon = 0;
  for (ast_node_t *f = n->fields; f; f = f->next) {
    if (f->kind == AST_CONSTRUCT_FIELD) {
      ast_construct_field_t *cf = (ast_construct_field_t *)f;
      int fi = struct_type_find_field(ct, cf->name);
      if (fi < 0) {
        diag_error(sema->diag, sema_loc(sema, f),
                   "struct '%.*s' has no field '%.*s'",
                   (int)ct->name.len, ct->name.ptr,
                   (int)cf->name.len, cf->name.ptr);
        return NULL;
      }
      if (slot[fi]) {
        diag_error(sema->diag, sema_loc(sema, f),
                   "construct: duplicate field '%.*s'",
                   (int)cf->name.len, cf->name.ptr);
        return NULL;
      }
      slot[fi] = cf->value;
    } else if (f->kind == AST_FILL) {
      diag_error(sema->diag, sema_loc(sema, f),
                 "construct: fill is not supported for struct fields");
      return NULL;
    } else {
      while (anon < cap && slot[anon]) anon++;
      if (anon >= cap) {
        char tn[64];
        sema_type_name(ct, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, f),
                   "construct: too many fields for %s", tn);
        return NULL;
      }
      slot[anon] = f;
      anon++;
    }
  }
  for (size_t i = 0; i < cap; i++) {
    if (!slot[i]) {
      char tn[64];
      sema_type_name(ct, tn, sizeof tn);
      diag_error(sema->diag, sema_loc(sema, &n->base),
                 "construct: missing value for field '%.*s' of %s",
                 (int)cst->fields[i].name.len, cst->fields[i].name.ptr, tn);
      return NULL;
    }
    fields[i].name = cst->fields[i].name;
  }

  /* 第二遍：逐字段求值（注入 anon_ct=目标字段类型）+ safe_cast 校验；
     字段类型取目标字段类型（值与目标同构校验，i32→i64 提升放行） */
  for (size_t i = 0; i < cap; i++) {
    ast_node_t *value = slot[i];
    const type_t *ft = cst->fields[i].type;
    value_t *fv = NULL;
    bool is_nil_field = value->kind == AST_NIL;
    if (!is_nil_field) {
      if (sema->anon_ct_depth < 16)
        sema->anon_ct[sema->anon_ct_depth++] = ft;
      fv = sema_expr(sema, &slot[i], scope);
      if (sema->anon_ct_depth > 0) sema->anon_ct_depth--;
    }
    if (is_nil_field) {
      if (!ft || ft->kind != TYPE_KIND_OPTION) {
        char tn[64];
        sema_type_name(ft, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, value),
                   "cannot initialize struct field with nil (field type %s "
                   "is not optional)", tn);
      } else {
        /* 目标字段为 ?T：裸 nil 无法从 nil 推断 ?T 类型，须显式构造
           `.?T{nil}`（用户确认：optional 使用具名构造而非裸 nil） */
        char fn[64] = "?";
        if (cst->fields[i].name.ptr && cst->fields[i].name.len > 0)
          snprintf(fn, sizeof fn, "%.*s", (int)cst->fields[i].name.len,
                   cst->fields[i].name.ptr);
        diag_error(sema->diag, sema_loc(sema, value),
                   "optional field '%.*s' requires an explicit constructor "
                   "(use .?T{nil})", (int)cst->fields[i].name.len,
                   cst->fields[i].name.ptr);
      }
    } else if (fv && !value_is_error(sema->vm, fv) &&
               !value_is_type(fv, TYPE_KIND_VOID) && ft) {
      value_t *dst = value_make_shadow(sema->vm, ft);
      if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
        char tn[64], fn[64];
        sema_type_name(ft, tn, sizeof tn);
        sema_type_name(value_type(fv), fn, sizeof fn);
        diag_error(sema->diag, sema_loc(sema, value),
                   "cannot initialize struct field '%.*s' with %s "
                   "(field type %s)",
                   (int)cst->fields[i].name.len, cst->fields[i].name.ptr,
                   fn, tn);
      }
    }
    fields[i].type = ft;
  }

  /* 第三遍：构造匿名 struct 类型 + 登记 sema_type（hoist 提升区） */
  const type_t *t = type_struct_intern(sema->vm, fields, cap);
  if (!t) return NULL;
  sema_type_register(sema, t);

  /* 第四遍：把 fields 链重排为类型表序（值节点直链，ast_append 清 next） */
  ast_node_t *new_head = NULL, *new_last = NULL;
  for (size_t i = 0; i < cap; i++)
    ast_append(&new_head, &new_last, NULL, slot[i]);
  n->fields      = new_head;
  n->fields_last = new_last;
  return t;
}

/* 匿名构造 .{...} 的匿名 tuple 类型推断（m2-design §3，按序推断）：
 * 元组元素匿名（无名字），字段链只能是值表达式（AST_CONSTRUCT_FIELD 具名
 * 字段与 AST_FILL 值包在元组构造中非法）——字段数量必须 == 目标元素数量，
 * 逐字段按位置匹配目标元素类型：
 * 1. 第一遍：字段数校验 + 逐字段求值（注入 anon_ct=目标元素类型，嵌套
 *    匿名构造/嵌套 tuple/struct 构造可正常推断）+ value_assign safe_cast
 *    校验（i32 字面量 → i64 元素提升，与 struct/具名 tuple 构造一致）；
 *    nil 字段须目标元素为 ?T。
 * 2. 第二遍：tuple_elem_t 表 = (目标元素类型, 目标序, offset 待 seal) →
 *    type_tuple_intern 构造匿名类型（按元素类型+顺序去重 intern——与目标
 *    同构时合并为同一实例，天然零转换）。
 * 3. 第三遍：把 fields 链重排为表序（值节点直链）——运行期 op_construct
 *    按表序布值，compiler 按链序压值，链序必须 == 表序。
 * 返回推断类型；失败返回 NULL（已诊断）。
 * 注意：调用方（AST_CONSTRUCT 匿名分支）已保证 ct 为 tuple 类型且
 * anon_ct 非空。字段已在此求值+校验完毕，调用方须直接返回不落入
 * tuple 构造分支（否则二次求值会重复诊断）。 */
static const type_t *infer_anon_tuple(sema_t *sema, ast_construct_t *n,
                                      sema_scope_t *scope,
                                      const type_t *ct) {
  const tuple_type_t *ctt = (const tuple_type_t *)ct;
  size_t cap = ctt->elem_count;
  size_t nfields = sema_count_siblings(n->fields);
  if (nfields != cap) {
    char tn[64];
    sema_type_name(ct, tn, sizeof tn);
    diag_error(sema->diag, sema_loc(sema, n->fields ? n->fields : &n->base),
               "construct: expected %zu elements for %s, got %zu", cap, tn,
               nfields);
    return NULL;
  }
  ast_node_t **slot = NULL;
  if (cap > 0) {
    slot = (ast_node_t **)arena_calloc(
        sema->arena, cap, sizeof(ast_node_t *), ALIGNOF(max_align_t));
    if (!slot) return NULL;
  }

  /* 第一遍：元组元素匿名——字段链按序直取（不允许具名/值包字段） */
  size_t i = 0;
  for (ast_node_t *f = n->fields; f; f = f->next, i++) {
    if (f->kind == AST_CONSTRUCT_FIELD) {
      diag_error(sema->diag, sema_loc(sema, f),
                 "construct: named field is not allowed for tuple elements "
                 "(tuple elements are anonymous, use positional values)");
      return NULL;
    }
    if (f->kind == AST_FILL) {
      diag_error(sema->diag, sema_loc(sema, f),
                 "construct: fill is not supported for tuple elements");
      return NULL;
    }
    slot[i] = f;
  }

  /* 第二遍：逐元素求值（注入 anon_ct=目标元素类型）+ safe_cast 校验；
     元素类型取目标元素类型（值与目标同构校验，i32→i64 提升放行） */
  tuple_elem_t *elems = NULL;
  if (cap > 0) {
    elems = (tuple_elem_t *)arena_calloc(
        sema->arena, cap, sizeof(tuple_elem_t), ALIGNOF(max_align_t));
    if (!elems) return NULL;
  }
  for (i = 0; i < cap; i++) {
    ast_node_t *value = slot[i];
    const type_t *et = ctt->elems[i].type;
    value_t *fv = NULL;
    bool is_nil_field = value->kind == AST_NIL;
    if (!is_nil_field) {
      if (sema->anon_ct_depth < 16)
        sema->anon_ct[sema->anon_ct_depth++] = et;
      fv = sema_expr(sema, &slot[i], scope);
      if (sema->anon_ct_depth > 0) sema->anon_ct_depth--;
    }
    if (is_nil_field) {
      if (!et || et->kind != TYPE_KIND_OPTION) {
        char tn[64];
        sema_type_name(et, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, value),
                   "cannot initialize tuple element with nil (element type %s "
                   "is not optional)", tn);
      } else {
        /* 目标元素为 ?T：裸 nil 无法从 nil 推断 ?T 类型，须显式构造
           `.?T{nil}`（与 struct 字段一致） */
        diag_error(sema->diag, sema_loc(sema, value),
                   "optional tuple element requires an explicit constructor "
                   "(use .?T{nil})");
      }
    } else if (fv && !value_is_error(sema->vm, fv) &&
               !value_is_type(fv, TYPE_KIND_VOID) && et) {
      value_t *dst = value_make_shadow(sema->vm, et);
      if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
        char tn[64], fn[64];
        sema_type_name(et, tn, sizeof tn);
        sema_type_name(value_type(fv), fn, sizeof fn);
        diag_error(sema->diag, sema_loc(sema, value),
                   "cannot initialize tuple element %zu with %s (element "
                   "type %s)", i, fn, tn);
      }
    }
    elems[i].type = et;
  }

  /* 第三遍：构造匿名 tuple 类型 + 登记 sema_type（hoist 提升区） */
  const type_t *t = type_tuple_intern(sema->vm, cap > 0 ? elems : NULL, cap);
  if (!t) return NULL;
  sema_type_register(sema, t);

  /* 第四遍：把 fields 链重排为表序（值节点直链，ast_append 清 next） */
  ast_node_t *new_head = NULL, *new_last = NULL;
  for (i = 0; i < cap; i++)
    ast_append(&new_head, &new_last, NULL, slot[i]);
  n->fields      = new_head;
  n->fields_last = new_last;
  return t;
}

value_t *sema_expr(sema_t *sema, ast_node_t **node, sema_scope_t *scope) {
  if (!node || !*node) return value_make_shadow(sema->vm, sema->vm->type_void);
  switch ((*node)->kind) {
    case AST_INT_LIT: {
      ast_int_lit_t *n = (ast_int_lit_t *)*node;
      const type_t *t = n->type.len ? type_lookup(sema->vm, n->type)
                                    : sema->vm->type_i32;
      if (!t) {
        diag_error(sema->diag, sema_loc(sema, *node), "unknown type '%.*s'",
                   (int)n->type.len, n->type.ptr);
        t = sema->vm->type_i32;
      }
      return value_make_shadow(sema->vm, t);
    }
    case AST_FLOAT_LIT: {
      ast_float_lit_t *n = (ast_float_lit_t *)*node;
      const type_t *t = n->type.len ? type_lookup(sema->vm, n->type)
                                    : sema->vm->type_f64;
      if (!t) {
        diag_error(sema->diag, sema_loc(sema, *node), "unknown type '%.*s'",
                   (int)n->type.len, n->type.ptr);
        t = sema->vm->type_f64;
      }
      return value_make_shadow(sema->vm, t);
    }
    case AST_BOOL_LIT:
      return value_make_shadow(sema->vm, sema->vm->type_bool);
    case AST_CHAR_LIT:
      return value_make_shadow(sema->vm, sema->vm->type_u8);
    case AST_STRING_LIT:
      return value_make_shadow(sema->vm, sema->vm->type_str);
    case AST_IDENT: {
      /* 变量读取：从 VM scope 链 lookup shadow value（与 sema 作用域树同构，
         天然遮罩）。未初始化检查走符号表 flow_init（确定性赋值分析，
         VM 值层不感知 TDZ）。
         comptime var：定义点已从语句链摘除（不在 VM scope），引用点折叠为
         字面量 AST 节点（sema_ct_lit，arena 分配），下游（编译器）零感知。 */
      ast_ident_t *n = (ast_ident_t *)*node;
      sema_symbol_t *sym = sema_lookup(scope, n->name);

      /* 函数引用（函数值，编译期/运行期二元）：符号种类 SYM_FUNC（用户函数
         与内建函数 printf 统一——两者在 sema/ctfe/compiler/vm 视角无区别，
         仅 func_t id 段不同，由 func_new 创建时分配）。
         遮蔽平等：局部变量遮蔽函数名时 sym 解析到变量符号（SYM_VAR），
         不走此分支——`var add = 2; var x = add;` 中 add 是变量引用。
         改写为 AST_FUNC_REF（compiler 发 LOAD_FUNCTION <id> 运行期加载
         真实函数值），此处返回签名类型 shadow（编译期折叠，供赋值/实参/
         返回值的签名匹配校验）。comptime func 不进入运行时，作为值引用
         是非法用法（调用点在 AST_CALL 折叠）。 */
      if (sym && sym->kind == SEMA_SYM_FUNC && sym->type &&
          sym->type->kind == TYPE_KIND_FUNC) {
        /* 局部函数体内引用兄弟/自身函数值：运行时函数体查找链只有参数 +
           全局（closure_scope 为空、调用时临时接 root_scope），兄弟/自身
           符号在定义点块作用域，不可见（需闭包）。FUNC 分支改写 AST_FUNC_REF
           → LOAD_FUNCTION 查 functions_by_id 全局表，会绕过作用域可见性
           ——在此显式拦截。参数（param_scope 直系，含函数类型参数——可调用，
           运行时参数可见）与全局函数（global_scope 符号）放行，与变量
           分支捕获检查同构。 */
        if (sema->local_func_base && !func_own_symbol(sema, n->name, sym) &&
            sema_lookup(sema->global_scope, n->name) != sym) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "local function cannot reference sibling or self '%.*s' "
                     "(add it to the capture list)",
                     (int)n->name.len, n->name.ptr);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        if (sym->is_comptime) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "comptime function '%.*s' cannot be used as a value",
                     (int)n->name.len, n->name.ptr);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        /* 闭包捕获 TDZ（编译期）：局部函数的捕获值在定义点 resolve_func_captures
           才绑定（is_active 激活）。walk 顺序保证定义点前的调用点捕获符号未激活
           → 编译期报错，不静默到运行期（捕获槽运行期仍为 undefined 占位）。
           仅对调用点生效（in_call_callee）——值引用（var f = b）放行：f/b
           浅拷贝共享同一 func_t 实例，运行时块入口已 MAKE_FUNCTION 实例化并
           绑定名字，定义点前取值拿到的是同实例浅拷贝，捕获槽在定义点后由
           SET_CLOSURE 填齐，调用时才读取，无 TDZ 悬垂。
           全局函数 captures 恒空（hoist 基底即最终实例），天然放行。 */
        sema_func_t *tdz_sf = sema_func_by_id(sema, sym->fid);
        if (sema->in_call_callee && tdz_sf && tdz_sf->is_local &&
            tdz_sf->def && tdz_sf->def->kind == AST_FUNC_DEF) {
          ast_func_def_t *tdz_fn = (ast_func_def_t *)tdz_sf->def;
          if (func_capture_tdz(tdz_sf->scope, tdz_fn->captures)) {
            diag_error(sema->diag, sema_loc(sema, *node),
                       "closure '%.*s' used before its captures are bound (TDZ)",
                       (int)n->name.len, n->name.ptr);
            return value_make_shadow(sema->vm, sema->vm->type_void);
          }
        }
        ast_node_t *ref = ast_func_ref_new(sema->arena, (*node)->tok_begin,
                                           (*node)->tok_end);
        if (ref) {
          /* fid 由 sema 创建函数对象时分配（符号表字段；内建函数 = 内建 id）。
             name 指向源标识符 token（compiler 发 PUSH name 沿作用域链查找——
             局部函数取块入口 MAKE_FUNCTION 绑定的实例（全块同一实例，浅拷贝
             共享 func_t）；全局/内建取全局绑定基底，与定义点实例化语义对齐）。 */
          ast_func_ref_t *fr = (ast_func_ref_t *)ref;
          fr->fid  = sym->fid;
          fr->name = n->name;
          ref->next = (*node)->next; /* 保留兄弟链 */
          *node = ref;
        }
        return value_make_shadow(sema->vm, sym->type);
      }

      if (sym && !sym->flow_init) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "variable '%.*s' used before initialization",
                   (int)n->name.len, n->name.ptr);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      if (sym && sym->is_comptime && sym->ct_valid) {
        /* 折叠引用点为字面量（保留兄弟链，供调用点/语句链继续遍历） */
        ast_node_t *lit = sema_ct_lit(sema, *node, &sym->ct);
        if (lit) {
          lit->next = (*node)->next;
          *node = lit;
          return value_make_shadow(sema->vm, sym->ct.type);
        }
      }
      /* 捕获检查（无闭包）：局部函数体内引用外层局部符号（外层块/外层函数
         的变量、参数、类型名）→ 运行时函数体查找链只有参数 + 全局
         （closure_scope 为空、调用时临时接 root_scope），外层局部不可见。
         fscope parent = 定义点块作用域（同块局部函数互相可见）——沿链命中的
         "非 fscope 直系、非 global"符号即外层局部。全局符号（sema_lookup
         global 命中）、函数名（FUNC 分支已先行返回）、comptime 符号（上方
         已折叠，不捕获）合法。 */
      if (sema->local_func_base && sym &&
          !func_own_symbol(sema, n->name, sym) &&
          sema_lookup(sema->global_scope, n->name) != sym) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "local function cannot access outer local '%.*s' "
                   "(add it to the capture list)",
                   (int)n->name.len, n->name.ptr);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      value_t *v = scope_lookup(sema->vm->current_scope, n->name);
      if (!v) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "undefined variable '%.*s'", (int)n->name.len, n->name.ptr);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      /* 类型即表达式：内建/自定义类型值注册在作用域链（global/root/current），
         与变量同机制查找。命中 type value 直接返回该真实值（type value 是
         编译期实体，data 恒为 type_t*——无 shadow 形态，见 sema 注释）。 */
      if (value_type(v) == sema->vm->type_type)
        return v;

      /* ?T 变量读取：始终按 ?T 类型返回 shadow（无窄化退化——optional
         解包由显式 .! assert 承担，见 docs m2-design §12.5） */
      return value_make_shadow(sema->vm, value_type(v));
    }
    case AST_CONST: {
      /* const 类型修饰（类型即表达式）：操作数必须是类型值，结果仍是
         类型值。遮蔽/类型检查在此感知（as 右值、M2 类型字面量）。 */
      ast_const_t *n = (ast_const_t *)*node;
      value_t *sub = sema_expr(sema, &n->sub, scope);
      if (value_is_error(sema->vm, sub) || value_is_type(sub, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);
      if (!value_is_type(sub, TYPE_KIND_TYPE)) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "'const' requires a type operand");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      /* type value 恒真实（编译期实体）：取 data 里的类型指针 intern 后
         返回新的 type value（auto-track 当前 scope） */
      const type_t *sub_t = value_as(sub, const type_t *);
      const type_t *ct = type_const_intern(sema->vm, sub_t);
      return type_as_value(sema->vm, ct);
    }
    case AST_VOLATILE: {
      ast_volatile_t *n = (ast_volatile_t *)*node;
      value_t *sub = sema_expr(sema, &n->sub, scope);
      if (value_is_error(sema->vm, sub) || value_is_type(sub, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);
      if (!value_is_type(sub, TYPE_KIND_TYPE)) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "'volatile' requires a type operand");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      const type_t *sub_t = value_as(sub, const type_t *);
      const type_t *vt = type_volatile_intern(sema->vm, sub_t);
      return type_as_value(sema->vm, vt);
    }
    case AST_UNDEF:
      /* undefined 只允许作为 var 初始化的"未初始化声明"（shadow_var_def
         消费）；普通表达式位置引用是非法用法。 */
      diag_error(sema->diag, sema_loc(sema, *node),
                 "'undefined' can only be used as a variable initializer");
      return value_make_shadow(sema->vm, sema->vm->type_void);
    case AST_NIL:
      /* nil 不是 value：只允许四种 nil 判定（x==nil 等，窄化）、?T 初始化
         赋值（a = nil）、构造器字段（.?T{nil}）与 fill 值包（<nil,N>）等
         特殊位置消费（shadow_binary / 赋值 / 构造器 / fill 分别处理）。
         普通表达式位置引用是非法用法。 */
      diag_error(sema->diag, sema_loc(sema, *node),
                 "'nil' can only be used in nil comparisons, ?T assignment, "
                 "constructor fields or fills");
      return value_make_shadow(sema->vm, sema->vm->type_void);
    case AST_BINARY:
      return shadow_binary(sema, node, scope);
    case AST_UNARY: {
      ast_unary_t *n = (ast_unary_t *)*node;
      value_t *operand = sema_expr(sema, &n->operand, scope);
      /* 错误恢复产物静默通过，避免级联二次诊断 */
      if (value_is_error(sema->vm, operand) ||
          value_is_type(operand, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);
      value_t *result = NULL;
      if (token_is(n->op, "-")) {
        result = value_neg(sema->vm, operand);
      } else if (token_is(n->op, "!")) {
        sema_check_bool(sema, n->operand, operand, "logical not");
        result = value_lnot(sema->vm, operand);
      } else if (token_is(n->op, "~")) {
        result = value_bnot(sema->vm, operand);
      } else {
        char ob[16];
        op_text(n->op, ob, sizeof(ob));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "unsupported unary operator '%s'", ob);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      if (value_is_error(sema->vm, result)) {
        char ob[16], tn[64];
        op_text(n->op, ob, sizeof(ob));
        op_type_name(operand, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "operator '%s' cannot be applied to %s", ob, tn);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      return result;
    }
    case AST_CALL: {
      /* 函数调用：统一路径 = exec callee（shadow 求值）→ exec 各实参 →
         value_call 分派到 func_vcall 的 shadow 分支（唯一校验点：参数
         数量/隐式转换，不执行 cfunc）→ 错误翻译为诊断。
         遮蔽/可见性全部交给作用域机制：callee 是 AST_IDENT 时经
         sema_expr → AST_IDENT 分支自然解析（局部变量遮蔽函数名 → 变量
         符号 → 下方 func 类型检查报"不可调用"；兄弟/自身函数 → 该分支
         闭包检查拦截；未定义 → "undefined variable"）。callee 是函数值
         表达式（get_fn()()、函数字面量）时递归求值 shadow（可能触发内层
         comptime 折叠）。
         唯一保留的符号感知前置：comptime func 调用点折叠（真实调用点
         CTFE 求值；walking_comptime 时实参是参数 shadow value 无法求值，
         只构造 callee shadow 做类型检查，折叠留到真实调用点）。 */
      ast_call_t *call = (ast_call_t *)*node;
      if (!call->callee) {
        diag_error(sema->diag, sema_loc(sema, &call->base),
                   "M1: missing callee");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      value_t *callee = NULL;
      if (call->callee->kind == AST_IDENT) {
        sema_symbol_t *sym =
            sema_lookup(scope, ((ast_ident_t *)call->callee)->name);
        if (sym && sym->is_comptime) {
          /* comptime func 调用：实参改写（折叠引用为字面量）+ ctfe 求值 →
             整个调用折叠为字面量。函数本身不注册到运行时。 */
          if (!sema->walking_comptime)
            return sema_eval_comptime_call(sema, node, scope);
          /* walking_comptime：只做 shadow 类型检查（callee shadow 走下方
             统一 value_call 路径，返回 return type 的 shadow） */
          callee = value_make_shadow(sema->vm, sym->type);
        }
      }
      if (!callee) {
        /* 一般 callee（函数名 / 函数值表达式）统一 shadow 求值。
           置 in_call_callee：TDZ 检查仅对调用点生效（值引用放行）。 */
        sema->in_call_callee = true;
        callee = sema_expr(sema, &call->callee, scope);
        sema->in_call_callee = false;
        if (value_is_error(sema->vm, callee) ||
            value_is_type(callee, TYPE_KIND_VOID))
          return value_make_shadow(sema->vm, sema->vm->type_void);
        const type_t *ctype = value_type(callee);
        if (!ctype || ctype->kind != TYPE_KIND_FUNC) {
          char tn[64];
          op_type_name(callee, tn, sizeof(tn));
          diag_error(sema->diag, sema_loc(sema, &call->base),
                     "cannot call value of type %s", tn);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
      }

      size_t argc = sema_count_siblings(call->args);
      value_t *arg_shadows[argc > 0 ? argc : 1];
      /* 逐实参求值（匿名构造注入：参数类型已知 → push anon_ct，使实参的
         .{...} 推断目标类型。签名参数类型从 callee 的 func 类型取——shadow
         value（data=NULL）仍携带完整签名；可变参数后无类型 → 不注入。 */
      const type_t *fnt = value_type(callee);
      ast_node_t **link = &call->args;
      size_t i = 0;
      for (; *link; link = &(*link)->next, i++) {
        const type_t *pt = fnt ? func_type_param(fnt, i) : NULL;
        bool injected = false;
        if (pt && sema->anon_ct_depth < 16) {
          sema->anon_ct[sema->anon_ct_depth++] = pt;
          injected = true;
        }
        arg_shadows[i] = sema_expr(sema, link, scope);
        if (injected && sema->anon_ct_depth > 0) sema->anon_ct_depth--;
      }
      value_t *result = value_call(sema->vm, callee, arg_shadows, argc);

      if (value_is_error(sema->vm, result)) {
        error_data_t *ed = (error_data_t *)value_data(result);
        const char *msg = ed && ed->message ? string_cstr(ed->message)
                                            : "function call failed";
        diag_error(sema->diag, sema_loc(sema, &call->base), "%s", msg);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      return result; /* shadow in → shadow out（return_type shadow） */
    }
    case AST_UNWRAP: {
      /* optional 解包：.!（assert）/ .?（try）。
         .! 要求操作数是 ?T：none 时运行期 panic（用户范式先判空再解包：
         if (x != nil) { var v = x.!; ... }）。返回 inner 类型 shadow。
         .? 仅词法预留（try），语义未实现 → 编译期报错。 */
      ast_unwrap_t *uw = (ast_unwrap_t *)*node;
      value_t *operand = sema_expr(sema, &uw->operand, scope);
      if (value_is_error(sema->vm, operand) ||
          value_is_type(operand, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);

      const type_t *ot = value_type(operand);
      if (!ot || ot->kind != TYPE_KIND_OPTION) {
        char tn[64], ob[16];
        size_t len = 0;
        const char *text =
            uw->op ? token_get_text(uw->op, &len) : NULL;
        snprintf(ob, sizeof(ob), ".%.*s", (int)len, text ? text : "?");
        op_type_name(operand, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "operator '%s' requires an optional operand, got %s", ob,
                   tn);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      if (uw->op && token_is(uw->op, "?")) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "operator '.?' (try) is not implemented yet");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      return value_make_shadow(sema->vm, type_option_inner(ot));
    }
    case AST_MEMBER: {
      /* 字段读取 p.field：object 求值 → 校验 struct/union → 按名查字段
         （struct_type_find_field / union_type_find_field）→ 返回字段类型
         shadow。嵌套 p.a.b 递归（object 是 AST_MEMBER 时 sema_expr 返回
         内层字段类型）。字段不存在 → 报错。union 字段不做静态 tag 断言
         （sema 不感知运行期 tag——运行期 FIELD_GET 反查 member + tag 校验，
         不符返回 error value 即 panic）。 */
      ast_member_t *n = (ast_member_t *)*node;
      value_t *obj = sema_expr(sema, &n->object, scope);
      if (value_is_error(sema->vm, obj) ||
          value_is_type(obj, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);
      const type_t *ot = value_type(obj);
      if (!ot) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot access field of value of unknown type");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      if (ot->kind == TYPE_KIND_UNION) {
        /* union 字段：union_type_find_field 反查所属 member（字段名全局
           唯一，sema 已校验）→ 返回该 member payload 字段类型 shadow */
        int mi, fi;
        union_type_find_field(ot, n->field, &mi, &fi);
        if (mi < 0 || fi < 0) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "union '%.*s' has no field '%.*s'",
                     (int)ot->name.len, ot->name.ptr,
                     (int)n->field.len, n->field.ptr);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        const union_member_t *m = union_type_member(ot, (size_t)mi);
        if (m && m->payload_struct) {
          const struct_field_t *f =
              struct_type_field(m->payload_struct, (size_t)fi);
          if (f) return value_make_shadow(sema->vm, f->type);
        }
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      if (ot->kind != TYPE_KIND_STRUCT) {
        char tn[64];
        sema_type_name(ot, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot access field of value of type %s", tn);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      int idx = struct_type_find_field(ot, n->field);
      if (idx < 0) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "struct '%.*s' has no field '%.*s'",
                   (int)ot->name.len, ot->name.ptr,
                   (int)n->field.len, n->field.ptr);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      return value_make_shadow(sema->vm, struct_type_field(ot, (size_t)idx)->type);
    }
    case AST_INDEX: {
      /* 下标 / 泛型索引（GAP 延后落点）：parser 只收集 <expr>[<expr,...>
         的 object + indices 链，此处首次可区分——base 是数组 → 下标（GET）；
         base 是类型值 → 泛型实例化语法（M2 未实现，占位诊断）。 */
      ast_index_t *n = (ast_index_t *)*node;
      value_t *base = sema_expr(sema, &n->object, scope);
      if (value_is_error(sema->vm, base) ||
          value_is_type(base, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);

      const type_t *bt = value_type(base);
      if (bt && bt->kind == TYPE_KIND_TYPE) {
        /* 泛型实例化占位：<type>[<expr,...> 与下标语法重叠，延后到 sema
           才可区分，M2 泛型未实现前明确占位。 */
        diag_error(sema->diag, sema_loc(sema, *node),
                   "generic instantiation is not implemented (index on a type value)");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      if (!bt || (bt->kind != TYPE_KIND_ARRAY && bt->kind != TYPE_KIND_TUPLE)) {
        char tn[64];
        sema_type_name(bt, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot index value of type %s", tn);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      /* 下标只消费 1 个索引；a[i,j] 多索引 = 泛型实参语法预留。
         a[i][j] 多维是 parse 链式嵌套（((a[i])[j])），逐维下降天然处理。 */
      size_t nidx = sema_count_siblings(n->indices);
      if (nidx != 1) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "array subscript expects exactly 1 index, got %zu", nidx);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      value_t *idx = sema_expr(sema, &n->indices, scope);
      if (value_is_error(sema->vm, idx) ||
          value_is_type(idx, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);
      if (!value_is_type(idx, TYPE_KIND_INT)) {
        char tn[64];
        op_type_name(idx, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, n->indices),
                   "array index must be an integer, got %s", tn);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      if (bt->kind == TYPE_KIND_TUPLE) {
        /* tuple 下标 t[i]：元素匿名 → 按位置访问（运行期 INDEX_GET/INDEX_SET
           带越界检查）。结果类型取决于 i——i 须编译期常量（字面量直接读，
           复杂表达式走 ctfe 真实求值，与数组边界一致），越界编译期报错
           （运行期指令仍兜底 panic）。 */
        size_t cap = tuple_type_elem_count(bt);
        uint64_t raw = 0;
        if (n->indices && n->indices->kind == AST_INT_LIT) {
          raw = ((ast_int_lit_t *)n->indices)->value;
        } else {
          vm_t *vm = sema->vm;
          bool saved = vm->comptime;
          vm->comptime = true;
          ctfe_ctx_t ctx;
          memset(&ctx, 0, sizeof ctx);
          ctx.vm = vm;
          ctx.sema = sema;
          ctx.budget = 100000;
          ctx.max_depth = 128;
          value_t *r = ctfe_eval(&ctx, n->indices);
          vm->comptime = saved;
          if (!r || value_is_error(vm, r)) {
            diag_error(sema->diag, sema_loc(sema, n->indices),
                       "tuple index must be a compile-time constant");
            return value_make_shadow(sema->vm, sema->vm->type_void);
          }
          const type_t *rt = value_type(r);
          if (!rt || rt->kind != TYPE_KIND_INT) {
            diag_error(sema->diag, sema_loc(sema, n->indices),
                       "tuple index must be an integer constant");
            return value_make_shadow(sema->vm, sema->vm->type_void);
          }
          switch (rt->size) {
            case 1: raw = *(const uint8_t  *)value_data(r); break;
            case 2: raw = *(const uint16_t *)value_data(r); break;
            case 4: raw = *(const uint32_t *)value_data(r); break;
            default: raw = *(const uint64_t *)value_data(r); break;
          }
        }
        if (raw >= cap) {
          diag_error(sema->diag, sema_loc(sema, n->indices),
                     "tuple index %llu out of bounds (len=%zu)",
                     (unsigned long long)raw, cap);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        const tuple_elem_t *e = tuple_type_elem(bt, (size_t)raw);
        return value_make_shadow(sema->vm, e ? e->type : sema->vm->type_void);
      }

      const type_t *et = array_type_elem(bt);
      return value_make_shadow(sema->vm, et ? et : sema->vm->type_void);
    }
    case AST_ARRAY: {
      /* 数组类型表达式 [N]T（类型即表达式）：表达式位置求值 = 类型值。
         解析为真实类型并就地替换为 AST_TYPE_REF（编译器发 LOAD_TYPE），
         返回真实 type value（data=type_t*，auto-track 当前 scope），供
         type 定义 / as 右值 / sizeof / 嵌套类型构造消费。 */
      const type_t *t = sema_resolve_type_slot(sema, node);
      if (!t) return value_make_shadow(sema->vm, sema->vm->type_void);
      return type_as_value(sema->vm, t);
    }
    case AST_TYPE_REF: {
      /* 具名类型引用（sema 登记的 "__type_N"）：重复求值（折叠重访）时
         命中——解析回类型，返回真实 type value。 */
      const type_t *t = resolve_type_expr(sema, *node);
      if (!t) return value_make_shadow(sema->vm, sema->vm->type_void);
      return type_as_value(sema->vm, t);
    }
    case AST_ENUM_REF: {
      /* 枚举 variant 引用 Color::Red：type_expr 求值为 enum 类型（折叠为
         AST_TYPE_REF）→ enum_type_find_variant 查 variant → 未找到报错；
         找到则折叠底层值入节点（compiler 发 LOAD_TYPE + PUSH_I* + MAKE_ENUM），
         返回 enum 类型 shadow（赋值/判等类型检查用）。 */
      ast_enum_ref_t *n = (ast_enum_ref_t *)*node;
      const type_t *t = sema_resolve_type_slot(sema, &n->type_expr);
      if (!t) return value_make_shadow(sema->vm, sema->vm->type_void);
      if (t->kind != TYPE_KIND_ENUM) {
        char tn[64];
        sema_type_name(t, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "'%s' is not an enum type", tn);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      int idx = enum_type_find_variant(t, n->variant);
      if (idx < 0) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "enum '%.*s' has no variant '%.*s'",
                   (int)t->name.len, t->name.ptr,
                   (int)n->variant.len, n->variant.ptr);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      n->value = enum_type_variant(t, (size_t)idx)->value;
      return value_make_shadow(sema->vm, t);
    }
    case AST_CONSTRUCT: {
      /* 类型字面量构造 .<type>{ fields }：求值类型位为真实类型，校验
         fields 数量与元素类型（value_assign 单一校验点），返回该类型
         的 shadow value（运行期由 CONSTRUCT 字节码完成值构造）。
         当前实现 array + option + struct + tuple 分支。 */
      ast_construct_t *n = (ast_construct_t *)*node;
      const type_t *t;
      if (!n->type) {
        /* 匿名构造 .{...}（m2-design §2）：字段即全部类型信息。目标 struct
           时按目标字段表驱动推断（infer_anon_struct：按名匹配 + 按目标字段
           类型 safe_cast 校验 + 表序重排 + intern 匿名类型），与目标同构时
           intern 去重为同一实例（天然零转换），否则结构兼容经
           value_implicit_cast 转换。optional 字段显式 `.?T{nil}` 构造
           （用户确认：nil 裸用不推断，须显式类型）。无上下文 → 报错。 */
        if (sema->anon_ct_depth == 0) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "anonymous construct '.{...}' requires a type context "
                     "(var declaration, assignment or struct field)");
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        const type_t *ct = sema->anon_ct[sema->anon_ct_depth - 1];
        if (ct->kind == TYPE_KIND_STRUCT) {
          /* struct 目标：字段已在 infer_anon_struct 求值+校验+重排完毕，
             直接返回（不落入下方 struct 构造分支，避免二次求值重复诊断）。 */
          t = infer_anon_struct(sema, n, scope, ct);
          if (!t) return value_make_shadow(sema->vm, sema->vm->type_void);
        } else if (ct->kind == TYPE_KIND_TUPLE) {
          /* tuple 目标：元素已在 infer_anon_tuple 求值+校验+重排完毕，
             直接返回（不落入下方 tuple 构造分支，避免二次求值重复诊断）。 */
          t = infer_anon_tuple(sema, n, scope, ct);
          if (!t) return value_make_shadow(sema->vm, sema->vm->type_void);
        } else if (ct->kind == TYPE_KIND_OPTION) {
          /* ?T 目标：按具名构造路径（.?T{...}）校验，返回 void shadow
             阻止落入下方 struct 分支（字段不是 struct 字段形态）。 */
          diag_error(sema->diag, sema_loc(sema, *node),
                     "anonymous construct '.{{...}}' with optional target type "
                     "requires an explicit constructor (use .?T{...})");
          return value_make_shadow(sema->vm, sema->vm->type_void);
        } else {
          /* 其余目标类型暂不支持匿名构造 */
          char cn[64];
          sema_type_name(ct, cn, sizeof cn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "anonymous construct '.{{...}}' requires a struct, tuple "
                     "or optional target type, got %s",
                     cn);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        ast_node_t *ref =
            ast_type_ref_new(sema->arena, n->base.tok_begin, n->base.tok_end);
        if (ref) {
          const sema_type_t *st = sema_type_find(sema, t);
          ((ast_type_ref_t *)ref)->name = st ? st->name : t->name;
          n->type = ref;
        }
      } else {
        t = sema_resolve_type_slot(sema, &n->type);
        if (!t) return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      /* ?T 构造器（二值单槽位，docs m2-design §12.2）：字段数强制 == 1，
         字段类型 ∈ {nil, T}。nil 字段 → 运行期 PUSH_OPT_NONE；T 字段 →
         隐式提升 D1。字段类型显式校验（不走 value_assign——option_assign
         的 shadow 短路会绕过类型检查，须在此处直接比较 inner）。 */
      if (t->kind == TYPE_KIND_OPTION) {
        size_t nfields = sema_count_siblings(n->fields);
        if (nfields != 1) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "optional constructor expects exactly 1 field, got %zu",
                     nfields);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        ast_node_t *f = n->fields;
        if (f && f->kind != AST_NIL) {
          value_t *fv = sema_expr(sema, &f, scope);
          if (!value_is_error(sema->vm, fv) &&
              !value_is_type(fv, TYPE_KIND_VOID)) {
            const type_t *inner = type_option_inner(t);
            const type_t *ft = value_type(fv);
            if (ft != t && ft != inner) {
              char tn[64], fn[64];
              sema_type_name(t, tn, sizeof tn);
              sema_type_name(ft, fn, sizeof fn);
              diag_error(sema->diag, sema_loc(sema, f),
                         "cannot initialize optional %s with %s", tn, fn);
            }
          }
        }
        return value_make_shadow(sema->vm, t);
      }

      if (t->kind == TYPE_KIND_STRUCT) {
        /* struct 构造（具名/匿名，docs m2-design §2）：字段数必须 == 类型
           字段数（完全显式）。具名字段 .name = value（AST_CONSTRUCT_FIELD）
           按名匹配；匿名字段（值表达式）按声明序匹配（鸭子构造，链序即
           字段序）。每个字段 value_assign 校验可赋值给字段类型。嵌套匿名
           构造注入：字段类型已知 → push anon_ct → 递归 sema_expr → pop。
           校验后把 fields 链重排为类型表序（值节点直链）——运行期
           op_construct 按表序布值（offset 查表），compiler 按链序压值，
           链序必须 == 表序，否则具名乱序（.y=2,.x=1）会错位。
           **注意**：匿名构造 .{...} 已在 infer_anon_struct 求值过字段并
           推断 t0（含重排），此处只处理具名构造（n->type 折叠为
           AST_TYPE_REF 后的 t 即匿名类型时，字段链已是表序，重复求值
           幂等）——区分标志：匿名构造字段求值时 anon_ct 栈顶是目标类型
           （或 type_void 占位），此时 t==t0，字段校验结果与 infer 一致。 */
        const struct_type_t *st = (const struct_type_t *)t;
        size_t nfields = sema_count_siblings(n->fields);
        if (nfields != st->field_count) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "construct: expected %zu fields for %s, got %zu",
                     st->field_count, tn, nfields);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }

        /* 第一遍：按类型表序收集字段值节点（具名按名定位、匿名按序顺延） */
        size_t cap = st->field_count;
        ast_node_t **slot = cap ? (ast_node_t **)arena_calloc(
            sema->arena, cap, sizeof(ast_node_t *), ALIGNOF(max_align_t)) : NULL;
        if (cap && !slot) return value_make_shadow(sema->vm, sema->vm->type_void);
        size_t anon = 0;
        for (ast_node_t *f = n->fields; f; f = f->next) {
          if (f->kind == AST_CONSTRUCT_FIELD) {
            ast_construct_field_t *cf = (ast_construct_field_t *)f;
            int fi = struct_type_find_field(t, cf->name);
            if (fi < 0) {
              diag_error(sema->diag, sema_loc(sema, f),
                         "struct '%.*s' has no field '%.*s'",
                         (int)t->name.len, t->name.ptr,
                         (int)cf->name.len, cf->name.ptr);
              return value_make_shadow(sema->vm, sema->vm->type_void);
            }
            slot[fi] = cf->value;
          } else {
            while (anon < cap && slot[anon]) anon++;
            if (anon >= cap) {
              char tn[64];
              sema_type_name(t, tn, sizeof tn);
              diag_error(sema->diag, sema_loc(sema, f),
                         "construct: too many fields for %s", tn);
              return value_make_shadow(sema->vm, sema->vm->type_void);
            }
            slot[anon] = f;
            anon++;
          }
        }
        for (size_t i = 0; i < cap; i++) {
          if (!slot[i]) {
            char tn[64];
            sema_type_name(t, tn, sizeof tn);
            diag_error(sema->diag, sema_loc(sema, *node),
                       "construct: missing value for field '%.*s' of %s",
                       (int)st->fields[i].name.len, st->fields[i].name.ptr, tn);
            return value_make_shadow(sema->vm, sema->vm->type_void);
          }
        }

        /* 第二遍：按表序求值每个字段值（注入 anon_ct）+ 可赋值性校验 */
        for (size_t i = 0; i < cap; i++) {
          ast_node_t *value = slot[i];
          const type_t *ft = st->fields[i].type;
          value_t *fv = NULL;
          bool is_nil_field = value->kind == AST_NIL;
          if (!is_nil_field) {
            if (sema->anon_ct_depth < 16)
              sema->anon_ct[sema->anon_ct_depth++] = ft;
            fv = sema_expr(sema, &slot[i], scope);
            if (sema->anon_ct_depth > 0) sema->anon_ct_depth--;
          }
          if (is_nil_field) {
            if (!ft || ft->kind != TYPE_KIND_OPTION) {
              char tn[64];
              sema_type_name(ft, tn, sizeof tn);
              diag_error(sema->diag, sema_loc(sema, value),
                         "cannot initialize struct field with nil (field "
                         "type %s is not optional)",
                         tn);
            }
          } else if (fv && !value_is_error(sema->vm, fv) &&
                     !value_is_type(fv, TYPE_KIND_VOID) && ft) {
            value_t *dst = value_make_shadow(sema->vm, ft);
            if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
              char tn[64], fn[64];
              sema_type_name(ft, tn, sizeof tn);
              sema_type_name(value_type(fv), fn, sizeof fn);
              diag_error(sema->diag, sema_loc(sema, value),
                         "cannot initialize struct field '%.*s' with %s "
                         "(field type %s)",
                         (int)st->fields[i].name.len,
                         st->fields[i].name.ptr, fn, tn);
            }
          }
        }

        /* 第三遍：重排 fields 链为类型表序（值节点直链，ast_append 清 next） */
        ast_node_t *new_head = NULL, *new_last = NULL;
        for (size_t i = 0; i < cap; i++)
          ast_append(&new_head, &new_last, NULL, slot[i]);
        n->fields      = new_head;
        n->fields_last = new_last;
        return value_make_shadow(sema->vm, t);
      }

      if (t->kind == TYPE_KIND_TUPLE) {
        /* tuple 构造（具名/匿名，docs m2-design §3）：元素数必须 == 类型
           元素数（完全显式）。元组元素匿名 → 字段链只能是值表达式（具名
           字段/值包非法），按声明序匹配元素类型（鸭子构造，链序即元素序）。
           每个元素 value_assign 校验可赋值给元素类型。嵌套匿名构造注入：
           元素类型已知 → push anon_ct → 递归 sema_expr → pop。
           **注意**：匿名构造 .{...} 已在 infer_anon_tuple 求值过元素并
           推断 t0（含重排），此处只处理具名构造（n->type 折叠为
           AST_TYPE_REF 后的 t 即匿名类型时，字段链已是表序，重复求值
           幂等）——区分标志：匿名构造元素求值时 anon_ct 栈顶是目标类型
           （或 type_void 占位），此时 t==t0，元素校验结果与 infer 一致。 */
        const tuple_type_t *tt = (const tuple_type_t *)t;
        size_t nfields = sema_count_siblings(n->fields);
        if (nfields != tt->elem_count) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "construct: expected %zu elements for %s, got %zu",
                     tt->elem_count, tn, nfields);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }

        /* 第一遍：元组元素匿名——字段链按序直取（不允许具名/值包字段） */
        ast_node_t **slot = NULL;
        if (tt->elem_count > 0) {
          slot = (ast_node_t **)arena_calloc(
              sema->arena, tt->elem_count, sizeof(ast_node_t *),
              ALIGNOF(max_align_t));
          if (!slot) return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        size_t anon = 0;
        for (ast_node_t *f = n->fields; f; f = f->next) {
          if (f->kind == AST_CONSTRUCT_FIELD) {
            diag_error(sema->diag, sema_loc(sema, f),
                       "construct: named field is not allowed for tuple "
                       "elements (tuple elements are anonymous, use "
                       "positional values)");
            return value_make_shadow(sema->vm, sema->vm->type_void);
          }
          if (f->kind == AST_FILL) {
            diag_error(sema->diag, sema_loc(sema, f),
                       "construct: fill is not supported for tuple elements");
            return value_make_shadow(sema->vm, sema->vm->type_void);
          }
          slot[anon++] = f;
        }

        /* 第二遍：按表序求值每个元素值（注入 anon_ct）+ 可赋值性校验 */
        for (size_t i = 0; i < tt->elem_count; i++) {
          ast_node_t *value = slot[i];
          const type_t *et = tt->elems[i].type;
          value_t *fv = NULL;
          bool is_nil_field = value->kind == AST_NIL;
          if (!is_nil_field) {
            if (sema->anon_ct_depth < 16)
              sema->anon_ct[sema->anon_ct_depth++] = et;
            fv = sema_expr(sema, &slot[i], scope);
            if (sema->anon_ct_depth > 0) sema->anon_ct_depth--;
          }
          if (is_nil_field) {
            if (!et || et->kind != TYPE_KIND_OPTION) {
              char tn[64];
              sema_type_name(et, tn, sizeof tn);
              diag_error(sema->diag, sema_loc(sema, value),
                         "cannot initialize tuple element with nil (element "
                         "type %s is not optional)", tn);
            }
          } else if (fv && !value_is_error(sema->vm, fv) &&
                     !value_is_type(fv, TYPE_KIND_VOID) && et) {
            value_t *dst = value_make_shadow(sema->vm, et);
            if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
              char tn[64], fn[64];
              sema_type_name(et, tn, sizeof tn);
              sema_type_name(value_type(fv), fn, sizeof fn);
              diag_error(sema->diag, sema_loc(sema, value),
                         "cannot initialize tuple element %zu with %s "
                         "(element type %s)", i, fn, tn);
            }
          }
        }

        /* 第三遍：重排 fields 链为表序（值节点直链，ast_append 清 next） */
        ast_node_t *new_head = NULL, *new_last = NULL;
        for (size_t i = 0; i < tt->elem_count; i++)
          ast_append(&new_head, &new_last, NULL, slot[i]);
        n->fields      = new_head;
        n->fields_last = new_last;
        return value_make_shadow(sema->vm, t);
      }

      if (t->kind == TYPE_KIND_UNION) {
        /* union 构造（.Shape{.Circle{...}} 两层嵌套，docs m2-design
           §tag union）：外层 union 构造收 1 个 member payload value。
           member 定位按值类型：payload member 值是内层 payload_struct 类型
           （AST_CONSTRUCT 的 .Circle 构造），纯 tag member 值是 tag 整数
           常量字面量（编译期省略内层构造，直接发 tag 常量）。 */
        const union_type_t *ut = (const union_type_t *)t;
        size_t nfields = sema_count_siblings(n->fields);
        if (nfields != 1) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "construct: union constructor expects exactly 1 member "
                     "for %s, got %zu",
                     tn, nfields);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }

        ast_node_t *f = n->fields;
        if (f->kind == AST_CONSTRUCT_FIELD) {
          diag_error(sema->diag, sema_loc(sema, f),
                     "construct: named field is not allowed for union member "
                     "(use .Shape{.Circle{...}} form)");
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        if (f->kind == AST_FILL) {
          diag_error(sema->diag, sema_loc(sema, f),
                     "construct: fill is not supported for union member");
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }

        /* member 定位：payload member（内层 .Circle{...} 构造，type 位 = member
           名 AST_IDENT → 按名定位 payload_struct → 改写 type 位为类型引用，
           使内层构造按 payload struct 类型正常推断/校验）；纯 tag member
           （tag 整数常量字面量，编译期省略内层构造）。 */
        int member_idx = -1;
        if (f->kind == AST_CONSTRUCT) {
          ast_construct_t *cf = (ast_construct_t *)f;
          if (cf->type && cf->type->kind == AST_IDENT) {
            const int mi = union_type_find_member(
                t, ((ast_ident_t *)cf->type)->name);
            if (mi < 0) {
              char tn[64];
              sema_type_name(t, tn, sizeof tn);
              diag_error(sema->diag, sema_loc(sema, f),
                         "union %s has no member '%.*s'",
                         tn, (int)((ast_ident_t *)cf->type)->name.len,
                         ((ast_ident_t *)cf->type)->name.ptr);
              return value_make_shadow(sema->vm, sema->vm->type_void);
            }
            const union_member_t *m = union_type_member(t, (size_t)mi);
            if (!m || !m->payload_struct) {
              diag_error(sema->diag, sema_loc(sema, f),
                         "union member '%.*s' has no payload (use tag value)",
                         (int)m->name.len, m->name.ptr);
              return value_make_shadow(sema->vm, sema->vm->type_void);
            }
            /* 改写 type 位：member 名 → payload_struct 类型引用（登记 +
               AST_TYPE_REF，compiler 发 LOAD_TYPE <id>） */
            ast_node_t *ref =
                sema_ct_type_ref(sema, m->payload_struct, cf->type);
            if (!ref) return value_make_shadow(sema->vm, sema->vm->type_void);
            ref->next = cf->type->next;
            cf->type  = ref;
            member_idx = mi;
          }
        }

        /* 求值 member payload value（注入 anon_ct：内层 .Circle{...} 匿名
           构造按 payload struct 类型推断） */
        value_t *fv = sema_expr(sema, &f, scope);
        if (value_is_error(sema->vm, fv) ||
            value_is_type(fv, TYPE_KIND_VOID))
          return value_make_shadow(sema->vm, sema->vm->type_void);
        const type_t *vt = value_type(fv);

        /* member 定位确认：payload member（struct 值 = payload_struct）或纯
           tag member（整数值 = tag）。已按名定位时直接确认命中。 */
        bool found = false;
        if (member_idx >= 0) {
          if (vt && vt->kind == TYPE_KIND_STRUCT &&
              ut->members[(size_t)member_idx].payload_struct == vt)
            found = true;
        } else if (vt && vt->kind == TYPE_KIND_STRUCT) {
          for (size_t i = 0; i < ut->member_count; i++) {
            if (ut->members[i].payload_struct &&
                ut->members[i].payload_struct == vt) {
              found = true;
              break;
            }
          }
        } else if (vt && vt->kind == TYPE_KIND_INT) {
          /* 纯 tag member：值是 tag 整数常量（AST_INT_LIT，运行期
             op_construct 按整数值匹配纯 tag member）。校验整数值在 member
             范围内且对应 member 无 payload。 */
          if (f->kind == AST_INT_LIT) {
            uint64_t tv = ((ast_int_lit_t *)f)->value;
            if (tv < ut->member_count && !ut->members[tv].payload_struct)
              found = true;
          }
        }
        if (!found) {
          char tn[64], fn[64];
          sema_type_name(t, tn, sizeof tn);
          sema_type_name(vt, fn, sizeof fn);
          diag_error(sema->diag, sema_loc(sema, f),
                     "construct: member value of type %s does not match any "
                     "member of union %s",
                     fn, tn);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        return value_make_shadow(sema->vm, t);
      }

      if (t->kind != TYPE_KIND_ARRAY) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "construct: unsupported type (only array, optional, struct "
                   "and tuple implemented)");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      /* 数组构造（完全显式，docs m2-design §12.4）：总元素数 =
         Σfill counts + 显式字段数，必须 == 数组长度（无自动 0 填充、
         无运行时裁剪）。fill 的 N 编译期常量（sema_eval_array_bound
         求值 + 折叠为 AST_INT_LIT，编译器读立即数）。 */
      const type_t *et = array_type_elem(t);
      size_t len = array_type_len(t);

      /* 第一遍：统计总元素数 + 求值 fill count（折叠写回） */
      size_t total = 0;
      for (ast_node_t *f = n->fields; f; f = f->next) {
        if (f->kind == AST_FILL) {
          size_t cnt;
          if (!sema_eval_array_bound(sema, &((ast_fill_t *)f)->count, &cnt))
            return value_make_shadow(sema->vm, sema->vm->type_void);
          total += cnt;
        } else {
          total += 1;
        }
      }
      if (len != SIZE_MAX && total != len) {
        char tn[64];
        sema_type_name(t, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, *node),
                   "construct: expected %zu elements for %s, got %zu", len, tn,
                   total);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      /* 第二遍：逐字段求值 + 元素类型校验（经链上指针传递，使 comptime
         引用折叠就地写回字段链，编译器零感知）。fill 的 v 求值一次，
         count 份同类型；nil 字段/fill 的 nil 值 → 元素须为 ?T（运行期
         PUSH_OPT_NONE，跳过求值——nil 非 value）。 */
      size_t idx = 0;
      for (ast_node_t **link = &n->fields; *link; link = &(*link)->next) {
        ast_node_t *f = *link;
        value_t *fv = NULL;
        bool is_nil_field = false;
        if (f->kind == AST_FILL) {
          ast_fill_t *fl = (ast_fill_t *)f;
          is_nil_field = fl->value->kind == AST_NIL;
          if (!is_nil_field)
            fv = sema_expr(sema, &fl->value, scope);
        } else if (f->kind == AST_NIL) {
          is_nil_field = true;
        } else {
          fv = sema_expr(sema, link, scope);
        }

        if (is_nil_field) {
          if (!et || et->kind != TYPE_KIND_OPTION) {
            char tn[64];
            sema_type_name(et, tn, sizeof tn);
            diag_error(sema->diag, sema_loc(sema, f),
                       "cannot initialize array element with nil (element "
                       "type %s is not optional)",
                       tn);
          }
          idx += 1;
          continue;
        }

        if (fv && !value_is_error(sema->vm, fv) &&
            !value_is_type(fv, TYPE_KIND_VOID) && et) {
          value_t *dst = value_make_shadow(sema->vm, et);
          if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
            char tn[64], fn[64];
            sema_type_name(et, tn, sizeof tn);
            sema_type_name(value_type(fv), fn, sizeof fn);
            diag_error(sema->diag, sema_loc(sema, f),
                       "cannot initialize array element %zu with %s (element "
                       "type %s)",
                       idx, fn, tn);
          }
        }
        idx += 1;
      }
      return value_make_shadow(sema->vm, t);
    }
    case AST_FUNC_DEF: {
      /* 函数字面量（表达式内 func 定义，函数值）：签名解析 + body 类型
         检查（sema_check_func_literal：临时 fscope 同步建树+walk，不注册
         作用域符号、不提升）。返回签名类型的 shadow——函数值表达式的类型
         即签名。失败（已诊断）返回 void shadow。 */
      ast_func_def_t *fn = (ast_func_def_t *)*node;
      const type_t *sig = sema_check_func_literal(sema, fn, scope);
      if (!sig) return value_make_shadow(sema->vm, sema->vm->type_void);
      return value_make_shadow(sema->vm, sig);
    }
    case AST_FUNC_REF: {
      /* 函数引用折叠产物（sema AST_IDENT 确认函数符号改写 / comptime 折叠
         AST_FUNC_REF）：纯 fid 标识，shadow walk 只关心签名类型——返回签名
         类型的 shadow（函数值表达式的类型即签名；运行期 LOAD_FUNCTION 加载
         真实函数值）。
         签名查询双路径（与 CTFE 一致）：程序函数（全局/局部）按 fid 查
         sema->funcs（def->sig_id → sema_type_by_id 反查）；内建函数与 CTFE
         构造的函数字面量引用对象按 fid 查 vm->functions。 */
      ast_func_ref_t *n = (ast_func_ref_t *)*node;
      const type_t *sig = NULL;
      sema_func_t *sf = sema_func_by_id(sema, n->fid);
      if (sf && sf->def && sf->def->kind == AST_FUNC_DEF) {
        ast_func_def_t *fd = (ast_func_def_t *)sf->def;
        const sema_type_t *st = sema_type_by_id(sema, fd->sig_id);
        if (st) sig = st->type;
      }
      /* 内建函数 / CTFE 构造的函数字面量引用对象：vm 侧按 fid 查签名。
         内建函数登记 functions_by_id（id < PROGRAM_BASE）；字面量引用对象
         （func_new_program_ref）只注册 vm->functions 不登记 functions_by_id
         ——线性扫描兜底（函数数量少）。 */
      if (!sig) {
        func_t *fn = vm_func_load(sema->vm, n->fid);
        if (!fn && sema->vm->functions) {
          size_t nf = vec_len(sema->vm->functions);
          for (size_t i = 0; i < nf; i++) {
            func_t *f = (func_t *)vec_get(sema->vm->functions, i);
            if (f && f->id == n->fid && f->type) { fn = f; break; }
          }
        }
        if (fn) sig = fn->type;
      }
      if (!sig) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "unknown function reference (id %u)", n->fid);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      return value_make_shadow(sema->vm, sig);
    }
    case AST_TERNARY: {
      /* 三元条件表达式：cond 必须 bool；两分支都 shadow 求值（两侧都要
         类型检查，运行时惰性只执行选中分支）；结果类型要求两分支一致。
         类型即表达式天然支持：`cond ? i32 : i64` 两侧都是 type_type。 */
      ast_ternary_t *n = (ast_ternary_t *)*node;
      value_t *cond = sema_expr(sema, &n->cond, scope);
      sema_check_bool(sema, n->cond, cond, "ternary condition");
      value_t *tval = sema_expr(sema, &n->then_branch, scope);
      value_t *eval = sema_expr(sema, &n->else_branch, scope);
      if (value_is_error(sema->vm, cond) || value_is_error(sema->vm, tval) ||
          value_is_error(sema->vm, eval) ||
          value_is_type(tval, TYPE_KIND_VOID) ||
          value_is_type(eval, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);
      const type_t *tt = value_type(tval);
      const type_t *et = value_type(eval);
      if (tt != et) {
        char tn[64], en[64];
        op_type_name(tval, tn, sizeof(tn));
        op_type_name(eval, en, sizeof(en));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "ternary branches must have the same type, got %s and %s",
                   tn, en);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      return tval; /* shadow，类型 = 两分支共同类型 */
    }
    case AST_ERROR:
    default:
      return value_make_shadow(sema->vm, sema->vm->type_void);
  }
}

static value_t *shadow_binary(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  ast_binary_t *b = (ast_binary_t *)*node;
  /* as：显式类型转换（普通中缀运算符）。lhs shadow 求值；rhs 是类型表达式，
     走 ctfe 编译期真实求值（vm->comptime 模式，调用 ctfe_eval），拿真实
     type value 的 data（type_t*）作为转换目标——遮蔽语义在此感知。 */
  if (token_is(b->op, "as")) {
    value_t *expr = sema_expr(sema, &b->lhs, scope);
    if (value_is_error(sema->vm, expr) ||
        value_is_type(expr, TYPE_KIND_VOID))
      return value_make_shadow(sema->vm, sema->vm->type_void);
    vm_t *vm = sema->vm;
    bool saved = vm->comptime;
    vm->comptime = true;
    ctfe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vm = vm;
    ctx.sema = sema;
    ctx.budget = 100000;
    ctx.max_depth = 128;
    value_t *ty = ctfe_eval(&ctx, b->rhs);
    vm->comptime = saved;
    if (!ty || value_is_error(vm, ty) ||
        !value_is_type(ty, TYPE_KIND_TYPE)) {
      char tn[64];
      op_type_name(expr, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "cast target is not a type (operand is %s)", tn);
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    const type_t *target = value_as(ty, const type_t *);
    value_t *result = value_explicit_cast(sema->vm, expr, target);
    if (value_is_error(sema->vm, result)) {
      char tn[64], tt[64];
      op_type_name(expr, tn, sizeof(tn));
      sema_type_name(target, tt, sizeof(tt));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "cannot cast %s to %s", tn, tt);
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    return result;
  }

  /* is：tag union tag 判定（<expr> is <member>，docs m2-design §tag
     union）。lhs shadow 求值（必须 union 类型）；rhs 是 member 名表达式
     （AST_IDENT）→ 编译期查 member → 折叠 rhs 为 AST_INT_LIT（member 的
     tag 整数值，compiler 发 IS_TAG 立即数，零感知 union 类型）。member
     名不是类型名（payload struct 匿名、纯 tag member 无类型），直接按名
     查 member（union_type_find_member）。合法 → 返回 bool shadow。 */
  if (token_is(b->op, "is")) {
    value_t *expr = sema_expr(sema, &b->lhs, scope);
    if (value_is_error(sema->vm, expr) ||
        value_is_type(expr, TYPE_KIND_VOID))
      return value_make_shadow(sema->vm, sema->vm->type_void);
    const type_t *ut = value_type(expr);
    if (!ut || ut->kind != TYPE_KIND_UNION) {
      char tn[64];
      op_type_name(expr, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "is: left operand must be a union value, got %s", tn);
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    /* rhs：member 名（AST_IDENT）。编译期校验 member 存在 + 折叠为 tag
       整数（compiler 发 IS_TAG 立即数）。非 member 名表达式（如函数调用/
       复合）报错。 */
    if (!b->rhs || b->rhs->kind != AST_IDENT) {
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "is: right operand must be a member name");
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    const strslice_t mname = ((ast_ident_t *)b->rhs)->name;
    const int mi = union_type_find_member(ut, mname);
    if (mi < 0) {
      char tn[64];
      sema_type_name(ut, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "is: union %s has no member '%.*s'", tn,
                 (int)mname.len, mname.ptr);
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    /* 折叠 rhs → AST_INT_LIT（tag 值）：compiler 发 IS_TAG <tag> 立即数。
       与 comptime 折叠同模式：sema 阶段确定 member 归属，下游零感知。 */
    const union_member_t *m = union_type_member(ut, (size_t)mi);
    ast_node_t *lit = ast_int_lit_new(sema->arena, b->rhs->tok_begin,
                                      b->rhs->tok_end);
    if (!lit) return value_make_shadow(sema->vm, sema->vm->type_void);
    ((ast_int_lit_t *)lit)->value = m ? (uint64_t)m->tag : 0u;
    b->rhs = lit;
    return value_make_shadow(sema->vm, sema->vm->type_bool);
  }

  /* extends：类型兼容判断（编译期类型计算，ctfe 求值后折叠为常量）。
     两侧都是类型表达式（类型即表达式），ctfe 在 vm->comptime 模式下真实
     求值（scope 中的类型值为真实 type value，非 shadow），value_extends
     分派 vtable->extends → bool。成功后把二元节点折叠为 AST_BOOL_LIT
     写回（保留兄弟链），下游编译器零感知——纯编译期，无运行时指令。 */
  if (token_is(b->op, "extends")) {
    vm_t *vm = sema->vm;
    bool saved = vm->comptime;
    vm->comptime = true;
    ctfe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vm = vm;
    ctx.sema = sema;
    ctx.budget = 100000;
    ctx.max_depth = 128;
    value_t *r = ctfe_eval(&ctx, *node);
    vm->comptime = saved;
    if (!r || value_is_error(vm, r)) {
      const char *msg = NULL;
      if (r) {
        error_data_t *ed = (error_data_t *)value_data(r);
        msg = ed && ed->message ? string_cstr(ed->message) : NULL;
      }
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "extends: not a compile-time type computation%s%s",
                 msg ? ": " : "", msg ? msg : "");
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    if (value_type(r) != vm->type_bool) {
      char tn[64];
      sema_type_name(value_type(r), tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "extends must yield bool (got %s)", tn);
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    sema_ct_const_t ct;
    if (!sema_ct_encode(sema, r, &ct)) {
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "extends result cannot be folded");
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    ast_node_t *lit = sema_ct_lit(sema, *node, &ct);
    if (!lit) {
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "extends result cannot be folded");
      return value_make_shadow(sema->vm, sema->vm->type_void);
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
    ast_node_t *other = b->lhs->kind == AST_NIL ? b->rhs : b->lhs;
    if (other->kind != AST_IDENT) {
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "nil can only be compared with an optional variable");
      return value_make_shadow(sema->vm, sema->vm->type_bool);
    }
    ast_ident_t *oid = (ast_ident_t *)other;
    sema_symbol_t *sym = sema_lookup(scope, oid->name);
    const type_t *ot = sym ? sym->type : NULL;
    if (!ot || ot->kind != TYPE_KIND_OPTION) {
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "variable '%.*s' is not optional; cannot compare with nil",
                 (int)oid->name.len, oid->name.ptr);
      return value_make_shadow(sema->vm, sema->vm->type_bool);
    }
    return value_make_shadow(sema->vm, sema->vm->type_bool);
  }

  value_t *lhs = sema_expr(sema, &b->lhs, scope);
  value_t *rhs = sema_expr(sema, &b->rhs, scope);
  /* 错误恢复产物（error/void shadow）静默通过，避免级联二次诊断 */
  if (value_is_error(sema->vm, lhs) || value_is_error(sema->vm, rhs) ||
      value_is_type(lhs, TYPE_KIND_VOID) || value_is_type(rhs, TYPE_KIND_VOID))
    return value_make_shadow(sema->vm, sema->vm->type_void);
  value_t *(*op)(vm_t *, value_t *, value_t *) = binop_of(b->op);
  if (!op) {
    char ob[16];
    op_text(b->op, ob, sizeof(ob));
    diag_error(sema->diag, sema_loc(sema, &b->base),
               "unsupported binary operator '%s'", ob);
    return value_make_shadow(sema->vm, sema->vm->type_void);
  }
  value_t *result = op(sema->vm, lhs, rhs);
  if (value_is_error(sema->vm, result)) {
    char ob[16], ln[64], rn[64];
    op_text(b->op, ob, sizeof(ob));
    op_type_name(lhs, ln, sizeof(ln));
    op_type_name(rhs, rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &b->base),
               "type mismatch: cannot apply '%s' to %s and %s", ob, ln, rn);
    return value_make_shadow(sema->vm, sema->vm->type_void);
  }
  return result; /* shadow in → shadow out */
}
