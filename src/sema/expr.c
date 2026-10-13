#include "sema/sema.h"
#include "sema/struct_compose.h" /* 嵌入继承的成员名字查找 */
#include "core/string.h"
#include "parser/ast_array.h"
#include "parser/ast_binary.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
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
#include "vm/type_cunion.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_opaque.h"
#include "vm/module.h"
#include "vm/ffi_lib.h" /* M6 FFI：ffi_data_kind_of（extern var 的类型码） */
#include "vm/bcode.h"   /* M6 FFI：BCODE_FFI_RET_*（extern cfunc 的返回类型码） */
#include "core/strmap.h"

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
  if (!sema->fn.local_func_base) return false;
  if (sema_scope_find_local(sema->fn.local_func_base, name) == sym) return true;
  if (sema->fn.local_func_param_scope &&
      sema_scope_find_local(sema->fn.local_func_param_scope, name) == sym)
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
  /* 先查 fill：`<值, 次数>` 只适用于数组/make 元素位，在 struct 构造里它会
     被当成"一个匿名字段"计数 → 报出与真实原因无关的字段数不符。 */
  for (ast_node_t *ff = n->fields; ff; ff = ff->next) {
    if (ff->kind == AST_FILL) {
      diag_error(sema->diag, sema_loc(sema, ff),
                 "construct: fill element '<value, count>' is not allowed "
                 "in struct constructors (list every field explicitly)");
      return NULL;
    }
  }
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
      bool ct_pushed = sema_anon_push(sema, ft);
      fv = sema_expr(sema, &slot[i], scope);
      sema_anon_pop(sema, ct_pushed);
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
      bool ct_pushed = sema_anon_push(sema, et);
      fv = sema_expr(sema, &slot[i], scope);
      sema_anon_pop(sema, ct_pushed);
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

/* ===========================================================================
 * 表达式求值的公共基础设施
 *
 * 这两个模式在 sema_expr 及其各 case 里重复上百次，抽成助手后逐 kind 处理器
 * 才可能保持可读（拆分 sema_expr 的前置工作）。
 * =========================================================================== */

/** "错误/未定" 占位值：void shadow（data=NULL，类型 void）。
 *  表达式求值失败的统一返回值——调用方经 is_poison 识别后短路。 */
static value_t *val_void(sema_t *sema) {
  return value_make_shadow(sema->vm, sema->vm->type_void);
}

/** 求值结果是否不可用（错误 value / void shadow / NULL）。
 *  三者都表示"这一步没有可用类型信息"，调用方应立刻传播而非继续推断。 */
static bool is_poison(sema_t *sema, const value_t *v) {
  if (!v) return true;
  return value_is_error(sema->vm, v) || value_is_type(v, TYPE_KIND_VOID);
}

/** 守护：求值结果不可用则返回 void shadow（错误恢复产物静默通过）。 */
#define EX_GUARD(sema, v)                                                     \
  do {                                                                        \
    if (is_poison((sema), (v))) return val_void((sema));                       \
  } while (0)

/* AST_NEW 处理器（自 sema_expr 拆出；纯搬移，逻辑与拆分前逐字一致） */
static value_t *ex_new(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* new 表达式（m3-design §8.1）：new T{...}—— .T{...} 的堆分配形态，
         返回 own *T。字段校验与 AST_CONSTRUCT 同构（完全显式：数量 + 逐
         字段可赋值性）。匿名构造 new {...}：目标类型依上下文推断（与
         AST_CONSTRUCT 匿名构造同款 anon_ct 注入）。Step A 静默：仅类型
         检查，堆分配运行期行为（NEW 指令）由 compiler/vm 承载。 */
      ast_new_t *n = (ast_new_t *)*node;
      const type_t *t;
      if (!n->type) {
        /* 匿名构造：依上下文推断目标类型（anon_ct 栈顶） */
        if (sema->ex.anon_ct_depth == 0) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "anonymous 'new {...}' requires a type context "
                     "(var declaration, assignment or struct field)");
          return val_void(sema);
        }
        t = sema->ex.anon_ct[sema->ex.anon_ct_depth - 1];
      } else {
        t = sema_resolve_type_slot(sema, &n->type);
        if (!t) return val_void(sema);
      }

      /* 字段数量与逐字段可赋值性校验（struct/tuple/array/union/cunion 同
         AST_CONSTRUCT 语义；scalar 等无字段类型 = 1 个空字段集合）。Step A
         只校验类型层面，字段求值走 sema_expr 递归（含嵌套 new/匿名构造）。 */
      if (t->kind == TYPE_KIND_STRUCT) {
        const struct_type_t *st = (const struct_type_t *)t;
        size_t nfields = sema_count_siblings(n->fields);
        /* `<值, 次数>` 填充元素只适用于数组/make 元素位：struct 字段无法用
           "重复次数"表达，必须显式报错。若放过它，fill 会按"一个匿名字段"
           进入 slot，最终以误导性的字段数不符（如 expected 2 fields, got 1）
           或编译器的 "unsupported expression node 'fill'" 暴露。 */
        for (ast_node_t *ff = n->fields; ff; ff = ff->next) {
          if (ff->kind == AST_FILL) {
            diag_error(sema->diag, sema_loc(sema, ff),
                       "construct: fill element '<value, count>' is not allowed "
                       "in struct constructors (list every field explicitly)");
            return val_void(sema);
          }
        }
        if (nfields != st->field_count) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "new: expected %zu fields for %s, got %zu",
                     st->field_count, tn, nfields);
          return val_void(sema);
        }
        size_t cap = st->field_count;
        ast_node_t **slot = cap ? (ast_node_t **)arena_calloc(
            sema->arena, cap, sizeof(ast_node_t *), ALIGNOF(max_align_t)) : NULL;
        if (cap && !slot) return val_void(sema);
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
              return val_void(sema);
            }
            slot[fi] = cf->value;
          } else {
            while (anon < cap && slot[anon]) anon++;
            if (anon >= cap) {
              char tn[64];
              sema_type_name(t, tn, sizeof tn);
              diag_error(sema->diag, sema_loc(sema, f),
                         "new: too many fields for %s", tn);
              return val_void(sema);
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
                       "new: missing value for field '%.*s' of %s",
                       (int)st->fields[i].name.len, st->fields[i].name.ptr, tn);
            return val_void(sema);
          }
        }
        for (size_t i = 0; i < cap; i++) {
          ast_node_t *value = slot[i];
          const type_t *ft = st->fields[i].type;
          value_t *fv = NULL;
          bool is_nil_field = value->kind == AST_NIL;
          if (!is_nil_field) {
            bool ct_pushed = sema_anon_push(sema, ft);
            fv = sema_expr(sema, &slot[i], scope);
            sema_anon_pop(sema, ct_pushed);
          }
          if (is_nil_field) {
            if (!ft || ft->kind != TYPE_KIND_OPTION) {
              char tn[64];
              sema_type_name(ft, tn, sizeof tn);
              diag_error(sema->diag, sema_loc(sema, value),
                         "cannot initialize struct field with nil (field "
                         "type %s is not optional)", tn);
            }
          } else if (fv && !value_is_error(sema->vm, fv) &&
                     !value_is_type(fv, TYPE_KIND_VOID) && ft) {
            /* R1（own 不可 copy，§3.1）：new 字段类型是 own *T 时，值必须
               是 fatal 将亡值（move/clone/new/make 产物或函数返回值）。
               直接传 own/ref 变量 = 隐式 copy——运行期 NEW 会把指针值原样
               复制进新堆块 → 内外两个 own 指向同一堆块 → 作用域退出 double
               free。 */
            if (type_is_own(ft) && !type_is_fatal(value_type(fv))) {
              char fn[64];
              sema_type_name(value_type(fv), fn, sizeof fn);
              diag_error(sema->diag, sema_loc(sema, value),
                         "cannot copy %s into own pointer field '%.*s' "
                         "(use move() or clone())",
                         fn, (int)st->fields[i].name.len,
                         st->fields[i].name.ptr);
              continue;
            }
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
        ast_node_t *new_head = NULL, *new_last = NULL;
        for (size_t i = 0; i < cap; i++)
          ast_append(&new_head, &new_last, NULL, slot[i]);
        n->fields      = new_head;
        n->fields_last = new_last;
      } else if (t->kind == TYPE_KIND_TUPLE) {
        const tuple_type_t *tt = (const tuple_type_t *)t;
        size_t nfields = sema_count_siblings(n->fields);
        if (nfields != tt->elem_count) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "new: expected %zu elements for %s, got %zu",
                     tt->elem_count, tn, nfields);
          return val_void(sema);
        }
        ast_node_t **slot = NULL;
        if (tt->elem_count > 0) {
          slot = (ast_node_t **)arena_calloc(
              sema->arena, tt->elem_count, sizeof(ast_node_t *),
              ALIGNOF(max_align_t));
          if (!slot) return val_void(sema);
        }
        size_t anon = 0;
        for (ast_node_t *f = n->fields; f; f = f->next) {
          if (f->kind == AST_CONSTRUCT_FIELD) {
            diag_error(sema->diag, sema_loc(sema, f),
                       "new: named field is not allowed for tuple elements "
                       "(tuple elements are anonymous, use positional values)");
            return val_void(sema);
          }
          if (f->kind == AST_FILL) {
            diag_error(sema->diag, sema_loc(sema, f),
                       "new: fill is not supported for tuple elements");
            return val_void(sema);
          }
          slot[anon++] = f;
        }
        for (size_t i = 0; i < tt->elem_count; i++) {
          ast_node_t *value = slot[i];
          const type_t *et = tt->elems[i].type;
          value_t *fv = NULL;
          bool is_nil_field = value->kind == AST_NIL;
          if (!is_nil_field) {
            bool ct_pushed = sema_anon_push(sema, et);
            fv = sema_expr(sema, &slot[i], scope);
            sema_anon_pop(sema, ct_pushed);
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
        ast_node_t *new_head = NULL, *new_last = NULL;
        for (size_t i = 0; i < tt->elem_count; i++)
          ast_append(&new_head, &new_last, NULL, slot[i]);
        n->fields      = new_head;
        n->fields_last = new_last;
      } else if (t->kind == TYPE_KIND_ARRAY) {
        const type_t *et = array_type_elem(t);
        size_t len = array_type_len(t);
        size_t total = 0;
        for (ast_node_t *f = n->fields; f; f = f->next) {
          if (f->kind == AST_FILL) {
            size_t cnt;
            if (!sema_eval_array_bound(sema, &((ast_fill_t *)f)->count, &cnt))
              return val_void(sema);
            total += cnt;
          } else {
            total += 1;
          }
        }
        if (len != SIZE_MAX && total != len) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "new: expected %zu elements for %s, got %zu", len, tn,
                     total);
          return val_void(sema);
        }
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
                         "type %s is not optional)", tn);
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
                         "type %s)", idx, fn, tn);
            }
          }
          idx += 1;
        }
      } else if (t->kind == TYPE_KIND_OPTION) {
        /* new ?T{...}：字段语义与 AST_CONSTRUCT 一致——非 nil 字段值须
           匹配 inner（D1 提升），nil 字段合法。Step A 静默。 */
        size_t nfields = sema_count_siblings(n->fields);
        if (nfields != 1) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "new: optional constructor expects exactly 1 field, "
                     "got %zu", nfields);
          return val_void(sema);
        }
        ast_node_t *f = n->fields;
        if (f && f->kind != AST_NIL) {
          bool was_last = (n->fields_last == f);
          value_t *fv = sema_expr(sema, &f, scope);
          /* 折叠写回（见 AST_SIZEOF 同款说明）：传的是局部变量，须回写；
             若被改写的是链尾，同时修正 fields_last（否则后续追加会挂到
             已脱链的旧节点上）。 */
          n->fields = f;
          if (was_last) n->fields_last = f;
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
      } else if (t->kind == TYPE_KIND_UNION || t->kind == TYPE_KIND_CUNION) {
        /* new union/cunion {...}：struct 同构单字段（字段名 = member 名）。
           成员定位按名 + payload 值可赋值性校验。Step A 静默。 */
        size_t nfields = sema_count_siblings(n->fields);
        if (nfields != 1) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "new: %s constructor expects exactly 1 field, got %zu",
                     tn, nfields);
          return val_void(sema);
        }
        ast_node_t *f = n->fields;
        if (f->kind != AST_CONSTRUCT_FIELD) {
          diag_error(sema->diag, sema_loc(sema, f),
                     "new: %s field must be named (use .Value{.i = v} form)",
                     t->kind == TYPE_KIND_UNION ? "union" : "cunion");
          return val_void(sema);
        }
        ast_construct_field_t *cf = (ast_construct_field_t *)f;
        const type_t *mt = NULL;
        if (t->kind == TYPE_KIND_UNION) {
          const int mi = union_type_find_member(t, cf->name);
          if (mi < 0) {
            char tn[64];
            sema_type_name(t, tn, sizeof tn);
            diag_error(sema->diag, sema_loc(sema, f),
                       "union %s has no member '%.*s'",
                       tn, (int)cf->name.len, cf->name.ptr);
            return val_void(sema);
          }
          const union_member_t *m = union_type_member(t, (size_t)mi);
          if (m && m->payload_type)
            mt = m->payload_type;
        } else {
          const int mi = cunion_type_find_member(t, cf->name);
          if (mi < 0) {
            char tn[64];
            sema_type_name(t, tn, sizeof tn);
            diag_error(sema->diag, sema_loc(sema, f),
                       "cunion %s has no member '%.*s'",
                       tn, (int)cf->name.len, cf->name.ptr);
            return val_void(sema);
          }
          const cunion_member_t *m = cunion_type_member(t, (size_t)mi);
          if (m && m->type)
            mt = m->type;
        }
        if (!mt) return val_void(sema);
        ast_node_t *value = cf->value;
        value_t *fv = NULL;
        bool is_nil_field = value->kind == AST_NIL;
        if (!is_nil_field) {
          bool ct_pushed = sema_anon_push(sema, mt);
          fv = sema_expr(sema, &value, scope);
          cf->value = value; /* 折叠写回（见 AST_SIZEOF 同款说明） */
          sema_anon_pop(sema, ct_pushed);
        }
        if (is_poison(sema, fv))
          return val_void(sema);
        if (is_nil_field) {
          if (mt->kind != TYPE_KIND_OPTION) {
            char tn[64];
            sema_type_name(mt, tn, sizeof tn);
            diag_error(sema->diag, sema_loc(sema, value),
                       "cannot initialize %s field with nil (field type %s "
                       "is not optional)",
                       t->kind == TYPE_KIND_UNION ? "union" : "cunion", tn);
          }
        } else {
          value_t *dst = value_make_shadow(sema->vm, mt);
          if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
            char tn[64], fn[64];
            sema_type_name(mt, tn, sizeof tn);
            sema_type_name(value_type(fv), fn, sizeof fn);
            diag_error(sema->diag, sema_loc(sema, value),
                       "cannot initialize %s field '%.*s' with %s (field "
                       "type %s)",
                       t->kind == TYPE_KIND_UNION ? "union" : "cunion",
                       (int)cf->name.len, cf->name.ptr, fn, tn);
          }
        }
      } else {
        /* 标量等无字段类型：new i32{123} 是 1 个字段（与 .i32{123} 同构） */
        size_t nfields = sema_count_siblings(n->fields);
        if (nfields != 1) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "new: scalar constructor expects exactly 1 field for %s, "
                     "got %zu", tn, nfields);
          return val_void(sema);
        }
        ast_node_t *f = n->fields;
        value_t *fv = NULL;
        bool is_nil_field = f->kind == AST_NIL;
        if (!is_nil_field) {
          bool was_last = (n->fields_last == f);
          bool ct_pushed = sema_anon_push(sema, t);
          fv = sema_expr(sema, &f, scope);
          /* 折叠写回 + fields_last 修正（见 AST_SIZEOF 同款说明） */
          n->fields = f;
          if (was_last) n->fields_last = f;
          sema_anon_pop(sema, ct_pushed);
        }
        if (is_poison(sema, fv))
          return val_void(sema);
        if (is_nil_field) {
          diag_error(sema->diag, sema_loc(sema, f),
                     "new: cannot initialize scalar with nil");
        } else if (fv && t) {
          /* R1（own 不可 copy，§3.1）：new 被指向类型是 own 指针时，值
             必须是 fatal 将亡值（move/clone 产物或函数返回值）。直接传
             own/ref 变量 = 隐式 copy → 新堆块与外变量两个 own 指向同一
             堆块 → double free（new own *i32{p} 必须写
             new own *i32{move(p)}）。 */
          if (type_is_own(t) && !type_is_fatal(value_type(fv))) {
            char fn[64];
            sema_type_name(value_type(fv), fn, sizeof fn);
            diag_error(sema->diag, sema_loc(sema, f),
                       "new: cannot copy %s into own pointer target (use "
                       "move() or clone())",
                       fn);
            return val_void(sema);
          }
          value_t *dst = value_make_shadow(sema->vm, t);
          if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
            char tn[64], fn[64];
            sema_type_name(t, tn, sizeof tn);
            sema_type_name(value_type(fv), fn, sizeof fn);
            diag_error(sema->diag, sema_loc(sema, f),
                       "new: cannot initialize %s with %s", tn, fn);
          }
        }
      }

      /* 返回 fatal *T 类型的 shadow——new 产新堆块，与
         clone/move 统一：所有"产新堆块"路径都返回 fatal（§3.3 将亡值），
         由 var 定义/参数/return 显式接管为 own（value_assign → implicit_cast
         fatal→own 转移）。运行期 NEW 指令按 fatal 类型压栈。 */
      const type_t *pt = type_ptr_intern(sema->vm, OWN_FATAL, t);
      if (!pt) return val_void(sema);
      sema_type_register(sema, pt);
      return value_make_shadow(sema->vm, pt);
}

/* AST_CONSTRUCT 处理器（自 sema_expr 拆出；纯搬移，逻辑与拆分前逐字一致） */
static value_t *ex_construct(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
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
        if (sema->ex.anon_ct_depth == 0) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "anonymous construct '.{...}' requires a type context "
                     "(var declaration, assignment or struct field)");
          return val_void(sema);
        }
        const type_t *ct = sema->ex.anon_ct[sema->ex.anon_ct_depth - 1];
        if (ct->kind == TYPE_KIND_STRUCT) {
          /* struct 目标：字段已在 infer_anon_struct 求值+校验+重排完毕，
             直接返回（不落入下方 struct 构造分支，避免二次求值重复诊断）。 */
          t = infer_anon_struct(sema, n, scope, ct);
          if (!t) return val_void(sema);
        } else if (ct->kind == TYPE_KIND_TUPLE) {
          /* tuple 目标：元素已在 infer_anon_tuple 求值+校验+重排完毕，
             直接返回（不落入下方 tuple 构造分支，避免二次求值重复诊断）。 */
          t = infer_anon_tuple(sema, n, scope, ct);
          if (!t) return val_void(sema);
        } else if (ct->kind == TYPE_KIND_OPTION) {
          /* ?T 目标：按具名构造路径（.?T{...}）校验，返回 void shadow
             阻止落入下方 struct 分支（字段不是 struct 字段形态）。 */
          diag_error(sema->diag, sema_loc(sema, *node),
                     "anonymous construct '.{{...}}' with optional target type "
                     "requires an explicit constructor (use .?T{...})");
          return val_void(sema);
        } else {
          /* 其余目标类型暂不支持匿名构造 */
          char cn[64];
          sema_type_name(ct, cn, sizeof cn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "anonymous construct '.{{...}}' requires a struct, tuple "
                     "or optional target type, got %s",
                     cn);
          return val_void(sema);
        }
        ast_node_t *ref =
            ast_type_ref_new(sema->arena, n->base.tok_begin, n->base.tok_end);
        if (ref) {
          const sema_type_t *st = sema_type_find(sema, t);
          ((ast_type_ref_t *)ref)->name = st ? st->name : t->name;
          n->type = ref;
        }

        /* 匿名构造到此已完整处理：infer_anon_struct / infer_anon_tuple 内部
           已逐字段求值 + 可赋值性校验 + 链重排 + sema_type_register。必须
           **就此返回**——落下去会进入下方具名构造分支，对同一批字段表达式
           **再求值一遍**，对含 move() 的字段会留下假的 "used after move"
           （实测：`var c: Cell = .{ .v = (move(p)).* };` 被误拒，
           而同样代码写具名构造 `.Cell{...}` 正常）。 */
        return value_make_shadow(sema->vm, t);
      } else {
        t = sema_resolve_type_slot(sema, &n->type);
        if (!t) return val_void(sema);
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
          return val_void(sema);
        }
        ast_node_t *f = n->fields;
        if (f && f->kind != AST_NIL) {
          bool was_last = (n->fields_last == f);
          value_t *fv = sema_expr(sema, &f, scope);
          /* 折叠写回（见 AST_SIZEOF 同款说明）：传的是局部变量，须回写；
             若被改写的是链尾，同时修正 fields_last（否则后续追加会挂到
             已脱链的旧节点上）。 */
          n->fields = f;
          if (was_last) n->fields_last = f;
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
        /* `<值, 次数>` 填充元素只适用于数组/make 元素位：struct 字段无法用
           "重复次数"表达，必须显式报错。若放过它，fill 会按"一个匿名字段"
           进入 slot，最终以误导性的字段数不符（如 expected 2 fields, got 1）
           或编译器的 "unsupported expression node 'fill'" 暴露。 */
        for (ast_node_t *ff = n->fields; ff; ff = ff->next) {
          if (ff->kind == AST_FILL) {
            diag_error(sema->diag, sema_loc(sema, ff),
                       "construct: fill element '<value, count>' is not allowed "
                       "in struct constructors (list every field explicitly)");
            return val_void(sema);
          }
        }
        if (nfields != st->field_count) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "construct: expected %zu fields for %s, got %zu",
                     st->field_count, tn, nfields);
          return val_void(sema);
        }

        /* 第一遍：按类型表序收集字段值节点（具名按名定位、匿名按序顺延） */
        size_t cap = st->field_count;
        ast_node_t **slot = cap ? (ast_node_t **)arena_calloc(
            sema->arena, cap, sizeof(ast_node_t *), ALIGNOF(max_align_t)) : NULL;
        if (cap && !slot) return val_void(sema);
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
              return val_void(sema);
            }
            slot[fi] = cf->value;
          } else {
            while (anon < cap && slot[anon]) anon++;
            if (anon >= cap) {
              char tn[64];
              sema_type_name(t, tn, sizeof tn);
              diag_error(sema->diag, sema_loc(sema, f),
                         "construct: too many fields for %s", tn);
              return val_void(sema);
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
            return val_void(sema);
          }
        }

        /* 第二遍：按表序求值每个字段值（注入 anon_ct）+ 可赋值性校验 */
        for (size_t i = 0; i < cap; i++) {
          ast_node_t *value = slot[i];
          const type_t *ft = st->fields[i].type;
          value_t *fv = NULL;
          bool is_nil_field = value->kind == AST_NIL;
          if (!is_nil_field) {
            bool ct_pushed = sema_anon_push(sema, ft);
            fv = sema_expr(sema, &slot[i], scope);
            sema_anon_pop(sema, ct_pushed);
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
          return val_void(sema);
        }

        /* 第一遍：元组元素匿名——字段链按序直取（不允许具名/值包字段） */
        ast_node_t **slot = NULL;
        if (tt->elem_count > 0) {
          slot = (ast_node_t **)arena_calloc(
              sema->arena, tt->elem_count, sizeof(ast_node_t *),
              ALIGNOF(max_align_t));
          if (!slot) return val_void(sema);
        }
        size_t anon = 0;
        for (ast_node_t *f = n->fields; f; f = f->next) {
          if (f->kind == AST_CONSTRUCT_FIELD) {
            diag_error(sema->diag, sema_loc(sema, f),
                       "construct: named field is not allowed for tuple "
                       "elements (tuple elements are anonymous, use "
                       "positional values)");
            return val_void(sema);
          }
          if (f->kind == AST_FILL) {
            diag_error(sema->diag, sema_loc(sema, f),
                       "construct: fill is not supported for tuple elements");
            return val_void(sema);
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
            bool ct_pushed = sema_anon_push(sema, et);
            fv = sema_expr(sema, &slot[i], scope);
            sema_anon_pop(sema, ct_pushed);
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
        /* union 构造（.Value{.i = 42}，struct 同构单字段，docs m2-design
           §tag union）：强制恰好 1 个具名字段（AST_CONSTRUCT_FIELD，字段名
           = member 名）。member 定位按名（union_type_find_member），值按
           member 的 payload_type value_assign 校验。sema 把 member 下标写回
           AST_CONSTRUCT 节点（member_tag 字段），compiler 发 tag 哨兵 +
           payload 值 + CONSTRUCT 2（显式 tag 消除同类型 member 歧义）。 */
        size_t nfields = sema_count_siblings(n->fields);
        if (nfields != 1) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "construct: union constructor expects exactly 1 field "
                     "for %s, got %zu",
                     tn, nfields);
          return val_void(sema);
        }

        ast_node_t *f = n->fields;
        if (f->kind == AST_FILL) {
          diag_error(sema->diag, sema_loc(sema, f),
                     "construct: fill is not supported for union field");
          return val_void(sema);
        }
        if (f->kind != AST_CONSTRUCT_FIELD) {
          diag_error(sema->diag, sema_loc(sema, f),
                     "construct: union field must be named (use .Value{.i = v} "
                     "form)");
          return val_void(sema);
        }
        ast_construct_field_t *cf = (ast_construct_field_t *)f;

        /* member 定位：字段名 = member 名 */
        const int mi = union_type_find_member(t, cf->name);
        if (mi < 0) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, f),
                     "union %s has no member '%.*s'",
                     tn, (int)cf->name.len, cf->name.ptr);
          return val_void(sema);
        }
        const union_member_t *m = union_type_member(t, (size_t)mi);
        if (!m || !m->payload_type) {
          diag_error(sema->diag, sema_loc(sema, f),
                     "union member '%.*s' has no payload type",
                     (int)cf->name.len, cf->name.ptr);
          return val_void(sema);
        }

        /* 求值 member payload 值（注入 anon_ct：匿名构造按 member 类型推断） */
        ast_node_t *value = cf->value;
        value_t *fv = NULL;
        bool is_nil_field = value->kind == AST_NIL;
        if (!is_nil_field) {
          bool ct_pushed = sema_anon_push(sema, m->payload_type);
          fv = sema_expr(sema, &value, scope);
          cf->value = value; /* 折叠写回（见 AST_SIZEOF 同款说明） */
          sema_anon_pop(sema, ct_pushed);
        }
        if (is_poison(sema, fv))
          return val_void(sema);
        if (is_nil_field) {
          if (m->payload_type->kind != TYPE_KIND_OPTION) {
            char tn[64];
            sema_type_name(m->payload_type, tn, sizeof tn);
            diag_error(sema->diag, sema_loc(sema, value),
                       "cannot initialize union field with nil (field type %s "
                       "is not optional)",
                       tn);
          }
        } else {
          /* 可赋值性校验（safe_cast：同类型身份通过；结构兼容隐式转换；
             不兼容报错） */
          value_t *dst = value_make_shadow(sema->vm, m->payload_type);
          if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
            char tn[64], fn[64];
            sema_type_name(m->payload_type, tn, sizeof tn);
            sema_type_name(value_type(fv), fn, sizeof fn);
            diag_error(sema->diag, sema_loc(sema, value),
                       "cannot initialize union field '%.*s' with %s (field "
                       "type %s)",
                       (int)cf->name.len, cf->name.ptr, fn, tn);
          }
        }

        /* member 下标写回节点（compiler 发 tag 哨兵用） */
        n->member_tag = (uint32_t)mi;
        return value_make_shadow(sema->vm, t);
      }

      if (t->kind == TYPE_KIND_CUNION) {
        /* cunion 构造（.C{.i = 42}，struct 同构单字段，无 tag——C union
           语义）：强制恰好 1 个具名字段（AST_CONSTRUCT_FIELD，字段名 =
           member 名）。member 定位按名（cunion_type_find_member），值按
           member 的 type value_assign 校验。sema 把 member 下标写回
           AST_CONSTRUCT 节点（member_tag 字段复用），compiler 发 member 下标
           哨兵 + payload 值 + CONSTRUCT 2（运行期定位 member 类型做
           implicit_cast；与 tag union 同款协议，区别是不把下标写进 data）。 */
        size_t nfields = sema_count_siblings(n->fields);
        if (nfields != 1) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, *node),
                     "construct: cunion constructor expects exactly 1 field "
                     "for %s, got %zu",
                     tn, nfields);
          return val_void(sema);
        }

        ast_node_t *f = n->fields;
        if (f->kind == AST_FILL) {
          diag_error(sema->diag, sema_loc(sema, f),
                     "construct: fill is not supported for cunion field");
          return val_void(sema);
        }
        if (f->kind != AST_CONSTRUCT_FIELD) {
          diag_error(sema->diag, sema_loc(sema, f),
                     "construct: cunion field must be named (use .C{.i = v} "
                     "form)");
          return val_void(sema);
        }
        ast_construct_field_t *cf = (ast_construct_field_t *)f;

        /* member 定位：字段名 = member 名 */
        const int mi = cunion_type_find_member(t, cf->name);
        if (mi < 0) {
          char tn[64];
          sema_type_name(t, tn, sizeof tn);
          diag_error(sema->diag, sema_loc(sema, f),
                     "cunion %s has no member '%.*s'",
                     tn, (int)cf->name.len, cf->name.ptr);
          return val_void(sema);
        }
        const cunion_member_t *m = cunion_type_member(t, (size_t)mi);
        if (!m || !m->type) {
          diag_error(sema->diag, sema_loc(sema, f),
                     "cunion member '%.*s' has no type",
                     (int)cf->name.len, cf->name.ptr);
          return val_void(sema);
        }

        /* 求值 member 值（注入 anon_ct：匿名构造按 member 类型推断） */
        ast_node_t *value = cf->value;
        value_t *fv = NULL;
        bool is_nil_field = value->kind == AST_NIL;
        if (!is_nil_field) {
          bool ct_pushed = sema_anon_push(sema, m->type);
          fv = sema_expr(sema, &value, scope);
          cf->value = value; /* 折叠写回（见 AST_SIZEOF 同款说明） */
          sema_anon_pop(sema, ct_pushed);
        }
        if (is_poison(sema, fv))
          return val_void(sema);
        if (is_nil_field) {
          if (m->type->kind != TYPE_KIND_OPTION) {
            char tn[64];
            sema_type_name(m->type, tn, sizeof tn);
            diag_error(sema->diag, sema_loc(sema, value),
                       "cannot initialize cunion field with nil (field type %s "
                       "is not optional)",
                       tn);
          }
        } else {
          /* 可赋值性校验（safe_cast：同类型身份通过；结构兼容隐式转换；
             不兼容报错） */
          value_t *dst = value_make_shadow(sema->vm, m->type);
          if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
            char tn[64], fn[64];
            sema_type_name(m->type, tn, sizeof tn);
            sema_type_name(value_type(fv), fn, sizeof fn);
            diag_error(sema->diag, sema_loc(sema, value),
                       "cannot initialize cunion field '%.*s' with %s (field "
                       "type %s)",
                       (int)cf->name.len, cf->name.ptr, fn, tn);
          }
        }

        /* member 下标写回节点（compiler 发哨兵用；cunion 无 tag，不写 data） */
        n->member_tag = (uint32_t)mi;
        return value_make_shadow(sema->vm, t);
      }

      if (t->kind != TYPE_KIND_ARRAY) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "construct: unsupported type (only array, optional, struct "
                   "and tuple implemented)");
        return val_void(sema);
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
            return val_void(sema);
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
        return val_void(sema);
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

/* AST_IDENT 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_ident(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
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
        if (sema->fn.local_func_base && !func_own_symbol(sema, n->name, sym) &&
            sema_lookup(sema->global_scope, n->name) != sym) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "local function cannot reference sibling or self '%.*s' "
                     "(add it to the capture list)",
                     (int)n->name.len, n->name.ptr);
          return val_void(sema);
        }
        if (sym->is_comptime) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "comptime function '%.*s' cannot be used as a value",
                     (int)n->name.len, n->name.ptr);
          return val_void(sema);
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
        if (sema->fn.in_call_callee && tdz_sf && tdz_sf->is_local &&
            tdz_sf->def && tdz_sf->def->kind == AST_FUNC_DEF) {
          ast_func_def_t *tdz_fn = (ast_func_def_t *)tdz_sf->def;
          if (func_capture_tdz(tdz_sf->scope, tdz_fn->captures)) {
            diag_error(sema->diag, sema_loc(sema, *node),
                       "closure '%.*s' used before its captures are bound (TDZ)",
                       (int)n->name.len, n->name.ptr);
            return val_void(sema);
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
        return val_void(sema);
      }
      if (sym && sym->moved) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "variable '%.*s' used after move (ownership was "
                   "transferred)",
                   (int)n->name.len, n->name.ptr);
        return val_void(sema);
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
      if (sema->fn.local_func_base && sym &&
          !func_own_symbol(sema, n->name, sym) &&
          sema_lookup(sema->global_scope, n->name) != sym) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "local function cannot access outer local '%.*s' "
                   "(add it to the capture list)",
                   (int)n->name.len, n->name.ptr);
        return val_void(sema);
      }
      value_t *v = scope_lookup(sema->vm->current_scope, n->name);
      if (!v) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "undefined variable '%.*s'", (int)n->name.len, n->name.ptr);
        return val_void(sema);
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

/* M6 FFI：剥掉 const/volatile 限定符（它们是同一份数据的限定视图，C ABI 描述符
   与底层类型一致）。`const i32` 的实参就应该按 int32 传。 */
static const type_t *ffi_strip_quals(const type_t *t) {
  while (t && (t->kind == TYPE_KIND_CONST || t->kind == TYPE_KIND_VOLATILE)) {
    const type_t *sub = (t->kind == TYPE_KIND_CONST)
                            ? ((const const_type_t *)t)->sub
                            : ((const volatile_type_t *)t)->sub;
    if (!sub) break;
    t = sub;
  }
  return t;
}

/* M6 FFI：实参类型能否传给该形参。
 *
 * clux 的标量与 C 是 **1:1** 的，调用层用 libffi 的精确 `ffi_type` 描述符，
 * 因此判据是"能否共用同一个描述符"：
 *   - 整数族（int/bool/enum）⇄ 整数族：宽度/符号可以不同（与 C 的隐式转换同义，
 *     都由整数寄存器承载）——所以 `malloc(16)` 的 i32 字面量喂 u64 形参合法；
 *   - 浮点 ⇄ 浮点：**要求同型**（f32⇄f32、f64⇄f64）。宽度不同不能共用一个
 *     描述符；要换精度请显式 `as`；
 *   - `opaque` 收 opaque 与各种指针/切片/str（C 里都是裸指针）；
 *   - `str` 收 `str`；指针形参收 opaque 与指针/切片；
 *   - 其它（结构体/cunion）：按值传参需要构造 struct 描述符，暂不支持 → 拒绝。
 * const/volatile 限定符先剥掉再判（视图不影响 ABI）。 */
static bool ffi_arg_acceptable(sema_t *sema, const type_t *pt_raw,
                               const type_t *at_raw) {
  const type_t *pt = ffi_strip_quals(pt_raw);
  const type_t *at = ffi_strip_quals(at_raw);
  if (!pt || !at) return false;
  bool p_int = pt->kind == TYPE_KIND_INT || pt->kind == TYPE_KIND_BOOL ||
               pt->kind == TYPE_KIND_ENUM;
  bool a_int = at->kind == TYPE_KIND_INT || at->kind == TYPE_KIND_BOOL ||
               at->kind == TYPE_KIND_ENUM;
  if (p_int) return a_int;
  if (pt->kind == TYPE_KIND_FLOAT) return at == pt; /* 同型（f32/f64 不可混） */
  if (pt == sema->vm->type_opaque)
    return at == sema->vm->type_opaque || type_is_ptr(at) ||
           (type_is_slice(at) && (at->ownership == OWN_REF ||
                                  at->ownership == OWN_FATAL)) ||
           at->kind == TYPE_KIND_STR;
  if (pt == sema->vm->type_str) return at == sema->vm->type_str;
  /* 结构体/cunion 按值：要求**同型**（描述符按字段构造，布局必须完全一致）。
     指针/切片等也要求完全一致。 */
  return pt == at;
}

/* M6 FFI：变参（`...`）位置的额外实参是否可搬运。整数族/浮点/指针/str 都可以
 * （浮点按 C 规则提升：f32→double）；结构体/cunion（按值）尚不支持。 */
static bool ffi_vararg_acceptable(sema_t *sema, const type_t *at_raw) {
  const type_t *at = ffi_strip_quals(at_raw);
  if (!at) return false;
  (void)sema;
  switch (at->kind) {
  case TYPE_KIND_INT:
  case TYPE_KIND_BOOL:
  case TYPE_KIND_ENUM:
  case TYPE_KIND_FLOAT:
  case TYPE_KIND_STR:
  case TYPE_KIND_OPAQUE:
  case TYPE_KIND_PTR:
  /* 变参也能按值传结构体/cunion（libffi 支持；C 对聚合类型不做默认提升） */
  case TYPE_KIND_STRUCT:
  case TYPE_KIND_CUNION:
    return true;
  default:
    return false; /* 切片/option/tag union/元组/闭包：变参位置不可搬运 */
  }
}

/* 从 `mod::Type` 形式的 AST_SCOPE_REF 解析被导入模块导出的 struct 类型。
 * type_expr 是 AST_IDENT（模块别名），variant 是类型名。
 * 直接从 module->exports 查 type value，取其指向的 type_t*。
 * 非 struct / 未找到返回 NULL。 */
static const type_t *resolve_cross_module_struct(sema_t *sema,
                                                  ast_scope_ref_t *er,
                                                  sema_scope_t *scope) {
  if (!er || !er->type_expr || er->type_expr->kind != AST_IDENT) return NULL;
  ast_ident_t *id = (ast_ident_t *)er->type_expr;
  sema_symbol_t *sym = sema_lookup(scope, id->name);
  if (!sym || sym->kind != SEMA_SYM_MODULE || !sym->module) return NULL;
  module_t *mod = sym->module;
  if (!mod || !mod->exports) return NULL;
  char buf[256];
  if (er->variant.len >= sizeof(buf)) return NULL;
  memcpy(buf, er->variant.ptr, er->variant.len);
  buf[er->variant.len] = '\0';
  value_t *member = (value_t *)strmap_get(mod->exports, buf);
  if (!member || value_type(member) != sema->vm->type_type) return NULL;
  const type_t *t = value_as(member, const type_t *);
  if (!t || t->kind != TYPE_KIND_STRUCT) return NULL;
  return t;
}

/* 沿 struct 嵌入链查找拥有指定属性名的 struct_type_t（自己优先，再沿 embeds
 * 深度优先）。运行时 attributes 查找用——跨模块时 sema member_scope 已销毁，
 * 只能依赖运行时 struct_type_t.attributes。 */
static const struct_type_t *find_runtime_attr_owner(const type_t *t,
                                                    const char *name) {
  if (!t || t->kind != TYPE_KIND_STRUCT) return NULL;
  const struct_type_t *st = (const struct_type_t *)t;
  if (st->attributes && strmap_contains(st->attributes, name))
    return st;
  size_t ne = 0;
  const type_t *const *emb = type_struct_embeds(t, &ne);
  for (size_t i = 0; i < ne; i++) {
    const type_t *eb = type_strip_quals(emb[i]);
    const struct_type_t *owner = find_runtime_attr_owner(eb, name);
    if (owner) return owner;
  }
  return NULL;
}

/* AST_CALL 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_call(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      ast_call_t *call = (ast_call_t *)*node;
      /* ---- 方法调用糖（结构体命名空间成员）----
       *   a.b(args)   —— 实例方法：receiver = a（值或指针），self 隐式首参
       *   T::f(args)  —— 静态方法：receiver = T 的 type value，无 self
       *
       * sema 解析出方法名 + 方法所在的 struct 类型后，置 is_member_call +
       * member_type_id，compiler 据此发 `MEMBER_CALL <STR name> <U32 argc>`
       * （不复用 CALL）。receiver 由 compiler 编译 callee 的 object（实例方法）
       * 或 type_expr（静态方法）压栈，VM 从 receiver 的 struct 类型 attributes
       * 中查方法 func value 并调用。
       *
       * `self` **不是关键字**——它只是普通参数名，第一个参数按位置就是接收者。
       * 实例方法的接收者传递由 VM 的 op_member_call 完成（值 → 取地址 ref *T，
       * 指针 → 原样），sema 不再改写 AST。 */
      if (call->callee && call->callee->kind == AST_MEMBER) {
        ast_member_t *mem = (ast_member_t *)call->callee;
        const type_t *ot = NULL;
        if (mem->object && mem->object->kind == AST_IDENT) {
          sema_symbol_t *os =
              sema_lookup(scope, ((ast_ident_t *)mem->object)->name);
          if (os && os->type) ot = os->type;
        } else {
          value_t *ov = sema_expr(sema, &mem->object, scope);
          if (ov && !value_is_error(sema->vm, ov)) ot = value_type(ov);
        }
        const bool by_ptr = ot && ptr_type_base(ot) != NULL;
        const type_t *st = type_strip_quals(by_ptr ? ptr_type_base(ot) : ot);
        if (st && st->kind == TYPE_KIND_STRUCT && st->name.len) {
          /* 在 struct 类型的 member_scope 中查方法名（自己优先 + 嵌入链递归） */
          const struct_type_t *owner = NULL;
          sema_symbol_t *ms = sema_struct_find_member(sema, st, mem->field,
                                                      call->callee, &owner);
          /* 以名字命中、但不是函数（自己的 var 成员 / 继承来的静态属性）：
             明确说清"它是变量，不是方法"，而不是落到下面的字段诊断。 */
          if (ms && ms->kind != SEMA_SYM_FUNC) {
            diag_error(sema->diag, sema_loc(sema, call->callee),
                       "cannot call '%.*s': it is a struct member variable, "
                       "not a method",
                       (int)mem->field.len, mem->field.ptr);
            return val_void(sema);
          }
          if (ms && ms->kind == SEMA_SYM_FUNC) {
            /* 标记为 member_call：compiler 发 MEMBER_CALL，不改写 AST */
            const sema_type_t *reg = sema_type_find(sema, st);
            if (reg) {
              call->is_member_call = true;
              call->member_type_id = reg->id;
            }
          } else if (!ms) {
            /* 跨模块回退：sema_struct_find_member 找不到 member_scope（源模块
               sema 已销毁），从运行时 struct_type_t.attributes 查方法。 */
            char fbuf[256];
            size_t fl = mem->field.len < sizeof(fbuf) - 1
                            ? mem->field.len : sizeof(fbuf) - 1;
            memcpy(fbuf, mem->field.ptr, fl);
            fbuf[fl] = '\0';
            const struct_type_t *rt_owner = find_runtime_attr_owner(st, fbuf);
            if (rt_owner && rt_owner->attributes) {
              value_t *mval = (value_t *)strmap_get(rt_owner->attributes,
                                                    fbuf);
              if (mval && value_type(mval) &&
                  value_type(mval)->kind == TYPE_KIND_FUNC) {
                /* 跨模块类型已由被导入模块注册并 vm_type_bind，直接用 t->id */
                if (rt_owner->base.id != 0) {
                  call->is_member_call = true;
                  call->member_type_id = rt_owner->base.id;
                }
              }
            }
          }
        }
      }
      /* ---- 静态方法调用 `T::f(args)` ----
       * callee 是 AST_SCOPE_REF，type_expr 解析到 SEMA_SYM_TYPE（struct），
       * variant 是方法名。sema 查 member_scope 命中函数 → 置 is_member_call。
       * compiler 编译 type_expr 压 type value（receiver），发 MEMBER_CALL。
       *
       * 跨模块 `mod::Type::f(args)`：type_expr 是嵌套 AST_SCOPE_REF（mod::Type）。
       * 被导入模块已 exec_run，运行时 struct_type_t.attributes / methods 已填充。
       * 从运行时 attributes 查方法 func value，取其签名类型做参数校验。 */
      if (!call->is_member_call && call->callee &&
          call->callee->kind == AST_SCOPE_REF) {
        ast_scope_ref_t *er = (ast_scope_ref_t *)call->callee;
        if (er->type_expr && er->type_expr->kind == AST_SCOPE_REF) {
          /* ---- 跨模块静态方法 `mod::Type::f(args)` ---- */
          ast_scope_ref_t *inner_er = (ast_scope_ref_t *)er->type_expr;
          const type_t *st = resolve_cross_module_struct(sema, inner_er,
                                                         scope);
          if (!st) {
            diag_error(sema->diag, sema_loc(sema, call->callee),
                       "cross-module static method call: '%.*s::%.*s' is not "
                       "a struct type",
                       (int)inner_er->variant.len, inner_er->variant.ptr,
                       (int)er->variant.len, er->variant.ptr);
            return val_void(sema);
          }
          char mbuf[256];
          size_t mn = er->variant.len < sizeof(mbuf) - 1
                          ? er->variant.len : sizeof(mbuf) - 1;
          memcpy(mbuf, er->variant.ptr, mn);
          mbuf[mn] = '\0';
          const struct_type_t *owner = find_runtime_attr_owner(st, mbuf);
          if (!owner) {
            diag_error(sema->diag, sema_loc(sema, call->callee),
                       "struct '%.*s' has no method '%.*s'",
                       (int)st->name.len, st->name.ptr,
                       (int)er->variant.len, er->variant.ptr);
            return val_void(sema);
          }
          value_t *mval = owner->attributes
              ? (value_t *)strmap_get(owner->attributes, mbuf) : NULL;
          if (mval && value_type(mval) &&
              value_type(mval)->kind == TYPE_KIND_FUNC) {
            /* 跨模块类型已由被导入模块注册并 vm_type_bind，直接用 t->id。
               不调 sema_type_register——会覆盖 t->id 破坏原有绑定。 */
            if (owner->base.id != 0) {
              call->is_member_call = true;
              call->member_type_id = owner->base.id;
            }
          }
        } else {
        /* type_expr 可能是 AST_IDENT（首次）或 AST_TYPE_REF（被 ex_scope_ref
           折叠过）——两种情况都取类型名查全局符号。 */
        strslice_t tn = STRSLICE_EMPTY;
        if (er->type_expr && er->type_expr->kind == AST_IDENT)
          tn = ((ast_ident_t *)er->type_expr)->name;
        else if (er->type_expr && er->type_expr->kind == AST_TYPE_REF)
          tn = ((ast_type_ref_t *)er->type_expr)->name;
        if (tn.ptr) {
          sema_symbol_t *sym = sema_lookup(scope, tn);
          if (sym && sym->kind == SEMA_SYM_TYPE && sym->type &&
              sym->type->kind == TYPE_KIND_STRUCT) {
            const struct_type_t *owner = NULL;
            sema_symbol_t *ms = sema_struct_find_member(sema, sym->type,
                                                        er->variant,
                                                        call->callee, &owner);
            if (ms && ms->kind == SEMA_SYM_FUNC) {
              const sema_type_t *reg = sema_type_find(sema, sym->type);
              if (reg) {
                call->is_member_call = true;
                call->member_type_id = reg->id;
              }
            }
          }
        }
        }
      }
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
      if (!call->callee) {
        diag_error(sema->diag, sema_loc(sema, &call->base),
                   "M1: missing callee");
        return val_void(sema);
      }

      /* ---- member_call 提前返回 ----
       * is_member_call 为 true 时，callee 不走 shadow 求值（它是 AST_MEMBER/
       * AST_SCOPE_REF，不是函数值表达式）。方法签名已在 member_scope 注册时由
       * pass2 解析；此处从符号取签名，做实参校验（含接收者），返回 return_type
       * shadow。compiler 据此发 MEMBER_CALL。
       *
       * 跨模块方法：源模块 sema 已销毁，sema_struct_find_member 找不到 member_scope，
       * 从运行时 struct_type_t.attributes 取方法 func value 的签名类型做校验。 */
      if (call->is_member_call) {
        /* 从 member_type_id 对应的 struct 类型 + 方法名查方法符号。
           编译期 struct 类型可能尚未 vm_type_bind（SEAL 在运行时执行），
           先用 sema_type_by_id 查 sema 登记的类型指针；失败再 fallback
           到 vm_type_load（跨模块已 bind 的类型）。 */
        const type_t *st = NULL;
        const sema_type_t *sreg = sema_type_by_id(sema, call->member_type_id);
        if (sreg) st = sreg->type;
        if (!st) st = vm_type_load(sema->vm, call->member_type_id);
        if (!st) return val_void(sema);
        strslice_t mname = {0};
        if (call->callee->kind == AST_MEMBER)
          mname = ((ast_member_t *)call->callee)->field;
        else if (call->callee->kind == AST_SCOPE_REF)
          mname = ((ast_scope_ref_t *)call->callee)->variant;
        const struct_type_t *owner = NULL;
        sema_symbol_t *msym = sema_struct_find_member(sema, st, mname,
                                                      call->callee, &owner);
        const type_t *sig_type = NULL;  /* 方法签名类型（func_type_t） */
        if (msym && msym->kind == SEMA_SYM_FUNC && msym->type) {
          sig_type = msym->type;
        } else {
          /* 跨模块回退：从运行时 attributes 取方法 func value 的签名 */
          char mbuf[256];
          size_t ml = mname.len < sizeof(mbuf) - 1 ? mname.len : sizeof(mbuf) - 1;
          memcpy(mbuf, mname.ptr, ml);
          mbuf[ml] = '\0';
          const struct_type_t *rt_owner = find_runtime_attr_owner(st, mbuf);
          if (rt_owner && rt_owner->attributes) {
            value_t *mval = (value_t *)strmap_get(rt_owner->attributes, mbuf);
            if (mval && value_type(mval) &&
                value_type(mval)->kind == TYPE_KIND_FUNC)
              sig_type = value_type(mval);
          }
        }
        if (!sig_type) return val_void(sema);
        const func_sig_t *sig = &((const func_type_t *)sig_type)->sig;
        /* 实参校验（参数数量 + 隐式转换），与 value_call 的 shadow 校验同构。
           实例方法：第一个参数是 self（接收者），实参从 args 开始对齐 self
           之后的形参。静态方法：无 self，实参从 args 开始对齐全部形参。 */
        bool is_instance = (call->callee->kind == AST_MEMBER);
        size_t nargs = sema_count_siblings(call->args);
        size_t expected = sig->param_count - (is_instance ? 1 : 0);
        if (!sig->is_variadic && nargs != expected) {
          diag_error(sema->diag, sema_loc(sema, &call->base),
                     "method '%.*s': expected %zu argument(s), got %zu",
                     (int)mname.len, mname.ptr, expected, nargs);
          return val_void(sema);
        }
        /* 逐实参求值（shadow）——匿名构造注入形参类型 */
        size_t ai = 0;
        for (ast_node_t **slot = &call->args; *slot;
             slot = &(*slot)->next, ai++) {
          const type_t *pt = (is_instance)
              ? func_type_param(sig_type, ai + 1)  /* skip self */
              : func_type_param(sig_type, ai);
          bool ct_pushed = sema_anon_push(sema, pt);
          sema_expr(sema, slot, scope);
          sema_anon_pop(sema, ct_pushed);
        }
        const type_t *rt = func_type_return(sig_type);
        return value_make_shadow(sema->vm, rt ? rt : sema->vm->type_void);
      }

      /* M6 FFI：extern cfunc 调用（`<库>::<名>(...)`）——callee 是 AST_SCOPE_REF 且库
         别名解析为 SEMA_SYM_EXTERN_LIB 时：**不做** value_call（那里没有运行时函数
         对象），置 AST 标志供 compiler 发 EXTERN_CALL（契约见 ast_call.h），并返回
         返回类型的 shadow 作为本表达式的类型。限定名符号由 pass1_names 注册
         （步骤 ①，含 §2 C ABI 校验）。 */
      if (call->callee->kind == AST_SCOPE_REF) {
        ast_scope_ref_t *er = (ast_scope_ref_t *)call->callee;
        if (er->type_expr && er->type_expr->kind == AST_IDENT) {
          ast_ident_t *lib_id = (ast_ident_t *)er->type_expr;
          sema_symbol_t *lsym = sema_lookup(scope, lib_id->name);
          if (lsym && lsym->kind == SEMA_SYM_EXTERN_LIB) {
            char qual[512];
            int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s",
                              (int)lib_id->name.len, lib_id->name.ptr,
                              (int)er->variant.len, er->variant.ptr);
            sema_symbol_t *fs = (qn > 0 && (size_t)qn < sizeof(qual))
                ? sema_lookup(sema->global_scope,
                              strslice_from_bytes(qual, (size_t)qn))
                : NULL;
            if (!fs || fs->kind != SEMA_SYM_FUNC || !fs->type) {
              diag_error(sema->diag, sema_loc(sema, &call->base),
                         "extern call %.*s: no matching `extern cfunc` declaration",
                         qn > 0 ? qn : 0, qual);
              return val_void(sema);
            }
            const type_t *rt = func_type_return(fs->type);
            /* 返回类型码：**精确到类型**（vm/bcode.h 的 BCODE_FFI_RET_*）。
               clux 标量与 C 是 1:1 的，调用层用 libffi 的精确描述符，因此返回值
               的类型必须与源码声明完全一致（旧编码把 u64 返回造成 i64，使用点会
               在比较/赋值时撞上隐式转换）。 */
            uint32_t rk = BCODE_FFI_RET_VOID;
            if (!rt || rt == sema->vm->type_void) rk = BCODE_FFI_RET_VOID;
            else if (rt == sema->vm->type_i8) rk = BCODE_FFI_RET_I8;
            else if (rt == sema->vm->type_i16) rk = BCODE_FFI_RET_I16;
            else if (rt == sema->vm->type_i32) rk = BCODE_FFI_RET_I32;
            else if (rt == sema->vm->type_i64) rk = BCODE_FFI_RET_I64;
            else if (rt == sema->vm->type_u8) rk = BCODE_FFI_RET_U8;
            else if (rt == sema->vm->type_u16) rk = BCODE_FFI_RET_U16;
            else if (rt == sema->vm->type_u32) rk = BCODE_FFI_RET_U32;
            else if (rt == sema->vm->type_u64) rk = BCODE_FFI_RET_U64;
            else if (rt == sema->vm->type_bool) rk = BCODE_FFI_RET_BOOL;
            else if (rt == sema->vm->type_f32) rk = BCODE_FFI_RET_F32;
            else if (rt == sema->vm->type_f64) rk = BCODE_FFI_RET_F64;
            else if (rt == sema->vm->type_opaque) rk = BCODE_FFI_RET_PTR;
            else if (rt == sema->vm->type_str) rk = BCODE_FFI_RET_STR;
            else if (rt && (rt->kind == TYPE_KIND_STRUCT ||
                            rt->kind == TYPE_KIND_CUNION ||
                            rt->kind == TYPE_KIND_ENUM ||
                            type_is_ptr(rt))) {
              /* 程序自定义类型 / 带类型的指针：宽度与布局必须查类型表 →
                "按类型值返回"。compiler 会在 EXTERN_CALL 之前压一个类型值
                （类型表达式取自 extern cfunc 的声明，sema 已挂在符号的 ast 上）。 */
              rk = BCODE_FFI_RET_TYPED;
              ast_func_def_t *efd =
                  (fs->ast && fs->ast->kind == AST_FUNC_DEF)
                      ? (ast_func_def_t *)fs->ast
                      : NULL;
              call->ffi_ret_type_expr = efd ? efd->return_expr : NULL;
            } else {
              diag_error(sema->diag, sema_loc(sema, &call->base),
                         "extern call %s: return type is not marshalable",
                         qual);
              return val_void(sema);
            }
            const func_sig_t *sig = &((const func_type_t *)fs->type)->sig;
            call->is_ffi_call = true;
            /* 返回类型码 + 变参信息打包（编解码见 vm/bcode.h）。
               变参调用必须知道"前几个是固定参数"，运行期才能按 C 的约定铺寄存器/
               栈并对变参部分施加默认实参提升。 */
            call->ffi_ret_kind =
                BCODE_FFI_RET_PACK(rk, sig->is_variadic, sig->param_count);

            /* 实参校验（此前**完全没做**）：数量 + 每个实参能否共用形参的 C 类型
               描述符（判据见 ffi_arg_acceptable）。变参的额外实参按 C 的默认实参
               提升处理（f32→double、窄整型→int），因此浮点/整型/指针都可以。 */
            size_t nargs = 0;
            for (ast_node_t *a = call->args; a; a = a->next) nargs++;
            if (!sig->is_variadic && nargs > sig->param_count)
              diag_error(sema->diag, sema_loc(sema, &call->base),
                         "extern call %s: expected %zu argument(s), got %zu",
                         qual, sig->param_count, nargs);
            /* 逐个求值实参：**必须经真正的 AST 槽**（`&call->args` / `&node->next`）——
               comptime 折叠会把节点就地替换成字面量（如 `add(1,2)` → `3`），写在
               局部指针副本上会丢，编译器随后仍去编译未折叠的调用）。 */
            size_t ai = 0;
            for (ast_node_t **slot = &call->args; *slot; slot = &(*slot)->next, ai++) {
              value_t *av = sema_expr(sema, slot, scope);
              if (!av || value_is_error(sema->vm, av) ||
                  value_is_type(av, TYPE_KIND_VOID))
                continue;
              ast_node_t *a = *slot;
              const type_t *at = value_type(av);
              const type_t *pt =
                  (ai < sig->param_count && sig->params) ? sig->params[ai] : NULL;
              if (!pt) {
                if (!ffi_vararg_acceptable(sema, at))
                  diag_error(sema->diag, sema_loc(sema, a),
                             "extern call %s: variadic argument %zu: this type "
                             "cannot cross the C ABI", qual, ai + 1);
                continue;
              }
              if (!ffi_arg_acceptable(sema, pt, at))
                diag_error(sema->diag, sema_loc(sema, a),
                           "extern call %s: argument %zu: cannot pass this value "
                           "as the declared parameter type", qual, ai + 1);
            }
            return value_make_shadow(sema->vm, rt ? rt : sema->vm->type_void);
          }
        }
      }

      value_t *callee = NULL;
      sema_symbol_t *callee_sym = NULL;
      if (call->callee->kind == AST_IDENT) {
        callee_sym =
            sema_lookup(scope, ((ast_ident_t *)call->callee)->name);
        sema_symbol_t *sym = callee_sym;
        if (sym && sym->is_comptime) {
          /* comptime func 调用：实参改写（折叠引用为字面量）+ ctfe 求值 →
             整个调用折叠为字面量。函数本身不注册到运行时。 */
          if (!sema->fn.walking_comptime)
            return sema_eval_comptime_call(sema, node, scope);
          /* walking_comptime：只做 shadow 类型检查（callee shadow 走下方
             统一 value_call 路径，返回 return type 的 shadow） */
          callee = value_make_shadow(sema->vm, sym->type);
        }
      }
      if (!callee) {
        /* 一般 callee（函数名 / 函数值表达式）统一 shadow 求值。
           置 in_call_callee：TDZ 检查仅对调用点生效（值引用放行）。 */
        sema->fn.in_call_callee = true;
        callee = sema_expr(sema, &call->callee, scope);
        sema->fn.in_call_callee = false;
        if (is_poison(sema, callee))
          return val_void(sema);
        const type_t *ctype = value_type(callee);
        if (!ctype || ctype->kind != TYPE_KIND_FUNC) {
          char tn[64];
          op_type_name(callee, tn, sizeof(tn));
          diag_error(sema->diag, sema_loc(sema, &call->base),
                     "cannot call value of type %s", tn);
          return val_void(sema);
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
        bool ct_pushed = sema_anon_push(sema, pt);
        arg_shadows[i] = sema_expr(sema, link, scope);
        sema_anon_pop(sema, ct_pushed);
      }
      value_t *result = value_call(sema->vm, callee, arg_shadows, argc);

      /* 语言级内建的类型特判（**按符号上的 builtin_op 标记**，不按 callee
         名字/AST 形态/fid）：callee 求值后 AST_IDENT 已被折叠成 FUNC_REF，
         "按名字 + AST_IDENT 匹配"的老写法因此从未生效（见 symbol.h 的说明）。 */

      /* upgrade（§12.4）：泛型 T 从实参 weak *T 推断，返回类型由占位
         ?share *opaque 精化为 ?share *T。 */
      if (callee_sym && callee_sym->builtin_op == SEMA_BUILTIN_UPGRADE &&
          argc == 1 && arg_shadows[0]) {
        const type_t *at = value_type(arg_shadows[0]);
        if (at && type_is_weak(at)) {
          const type_t *base = ptr_type_base(at);
          if (base) {
            const type_t *share_t = type_ptr_intern(sema->vm,
                OWN_SHARE, base);
            if (share_t) {
              const type_t *opt_t = type_option_intern(sema->vm, share_t);
              if (opt_t) {
                /* upgrade 特判：直接换用更精确的返回类型 shadow。原 result 是
                   scope 拥有的值——不得手工 dispose（那会留下 owned 里的悬垂
                   条目），交由作用域退出统一释放。 */
                result = value_make_shadow(sema->vm, opt_t);
              }
            }
          }
        }
      }

      /* len（m4-design §5/§11.3）：注册为 variadic，参数类型不经签名校验——
         此处校验实参是可长序列（array/slice/str）。返回类型由注册签名给出
         （usize），无需替换。 */
      if (callee_sym && callee_sym->builtin_op == SEMA_BUILTIN_LEN && argc == 1 &&
          arg_shadows[0]) {
        const type_t *at = value_type(arg_shadows[0]);
        if (at && at->kind != TYPE_KIND_ARRAY &&
            !type_is_slice(at) &&
            at->kind != TYPE_KIND_STR) {
          char tn[64];
          sema_type_name(at, tn, sizeof(tn));
          diag_error(sema->diag, sema_loc(sema, *node),
                     "len: argument must be array/slice/str, got %s", tn);
          return val_void(sema);
        }
      }

      if (value_is_error(sema->vm, result)) {
        error_data_t *ed = (error_data_t *)value_data(result);
        const char *msg = ed && ed->message ? ed->message
                                               : "function call failed";
        diag_error(sema->diag, sema_loc(sema, &call->base), "%s", msg);
        return val_void(sema);
      }

      /* data（C ABI 适配）：从**序列**取首元素指针。实参必须是 array/slice/str；
         返回类型精化为 `ref *<元素类型>`（str → ref *u8，即 C 的 const char*）。
         置 is_data_call：compiler 改发 BCODE_DATA 指令（不是 CALL——内置 cfunc
         的实参是副本，取不到调用方的存储身份，见 ast_call.h / vm/bcode.h）。 */
      if (callee_sym && callee_sym->builtin_op == SEMA_BUILTIN_DATA) {
        if (argc != 1) {
          diag_error(sema->diag, sema_loc(sema, &call->base),
                     "data: expects exactly 1 argument, got %zu", argc);
          return val_void(sema);
        }
        const type_t *at = value_type(arg_shadows[0]);
        const type_t *elem = NULL;
        switch (at ? at->kind : TYPE_KIND_VOID) {
        case TYPE_KIND_SLICE:
          elem = slice_type_elem(at);
          break;
        case TYPE_KIND_ARRAY:
          elem = array_type_elem(at);
          break;
        case TYPE_KIND_STR:
          elem = sema->vm->type_u8;
          break;
        default:
          break;
        }
        if (!elem) {
          char tn[64];
          sema_type_name(at, tn, sizeof(tn));
          diag_error(sema->diag, sema_loc(sema, &call->base),
                     "data: argument must be array/slice/str, got %s", tn);
          return val_void(sema);
        }
        const type_t *rp = type_ptr_intern(sema->vm, OWN_REF, elem);
        if (!rp) return val_void(sema);
        sema_type_register(sema, rp);
        call->is_data_call = true;
        return value_make_shadow(sema->vm, rp);
      }

      return result; /* shadow in → shadow out（return_type shadow） */
}

/* AST_MEMBER 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_member(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 字段读取 p.field：object 求值 → 校验 struct/union → 按名查字段
         （struct_type_find_field / union_type_find_field）→ 返回字段类型
         shadow。嵌套 p.a.b 递归（object 是 AST_MEMBER 时 sema_expr 返回
         内层字段类型）。字段不存在 → 报错。union 字段不做静态 tag 断言
         （sema 不感知运行期 tag——运行期 FIELD_GET 反查 member + tag 校验，
         不符返回 error value 即 panic）。 */
      ast_member_t *n = (ast_member_t *)*node;
      value_t *obj = sema_expr(sema, &n->object, scope);
      if (is_poison(sema, obj))
        return val_void(sema);
      const type_t *ot = value_type(obj);
      if (!ot) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot access field of value of unknown type");
        return val_void(sema);
      }
      /* const/volatile 只是同一份数据的视图：**读**字段始终合法（写由赋值路径按
         const 规则拒绝），故先剥掉限定再按底层类型查字段。 */
      ot = type_strip_quals(ot);
      /* 指针上的成员访问**自动解引用**：`p.field` ≡ `p.*.field`（任意指针修饰，
         指向 struct/cunion；pointee 可带 const/volatile）。做法是就地改写 AST——
         把当前 object 节点包进一个 AST_DEREF（compiler 据此发 PTR_GET，与手写
         `p.*.field` 完全同路），然后按**剥掉限定符**的 base 类型继续查字段。
         object 已经求值过，**不重新求值**（避免调用/函数字面量被二次处理）。 */
      {
        const type_t *base = type_strip_quals(ptr_type_base(ot));
        if (base && (base->kind == TYPE_KIND_STRUCT ||
                     base->kind == TYPE_KIND_CUNION)) {
          ast_node_t *deref = ast_deref_new(sema->arena, (*node)->tok_begin,
                                            (*node)->tok_end);
          if (!deref) return val_void(sema);
          ((ast_deref_t *)deref)->operand = n->object;
          n->object = deref;
          ot = base;
        }
      }
      if (ot->kind == TYPE_KIND_UNION) {
        /* union 字段：字段名 = member 名（tag 名），按名反查 member →
           返回该 member 的 payload 类型 shadow。运行期 FIELD_GET 按 tag
           校验，不符返回 error value 即 panic。 */
        int mi = union_type_find_member(ot, n->field);
        if (mi < 0) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "union '%.*s' has no field '%.*s'",
                     (int)ot->name.len, ot->name.ptr,
                     (int)n->field.len, n->field.ptr);
          return val_void(sema);
        }
        const union_member_t *m = union_type_member(ot, (size_t)mi);
        if (m && m->payload_type)
          return value_make_shadow(sema->vm, m->payload_type);
        return val_void(sema);
      }
      if (ot->kind == TYPE_KIND_CUNION) {
        /* cunion 字段：字段名 = member 名，按名反查 member → 返回该 member
           的 type shadow。无 tag 校验（C union 语义，开发者自负安全）——
           运行期 FIELD_GET 按名反查后直接借用 offset 0。 */
        int mi = cunion_type_find_member(ot, n->field);
        if (mi < 0) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "cunion '%.*s' has no field '%.*s'",
                     (int)ot->name.len, ot->name.ptr,
                     (int)n->field.len, n->field.ptr);
          return val_void(sema);
        }
        const cunion_member_t *m = cunion_type_member(ot, (size_t)mi);
        if (m && m->type)
          return value_make_shadow(sema->vm, m->type);
        return val_void(sema);
      }
      if (ot->kind != TYPE_KIND_STRUCT) {
        char tn[64];
        sema_type_name(ot, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot access field of value of type %s", tn);
        return val_void(sema);
      }
      int idx = struct_type_find_field(ot, n->field);
      if (idx < 0) {
        /* 不是字段，但可能是**成员**（`var` 静态属性 / `func` 方法，含沿嵌入链
           继承来的）：`t.f` / `t.n` 这种不带 `()` 的写法要说清"它是 struct 的
           成员，不是字段"，而不是含混地报"没有这个字段"。按成员 kind 给出
           对应写法建议（方法可 `t.f(...)` 调用，静态属性只能 `T::n` 限定访问）。 */
        strslice_t *mem = sema_struct_member_lookup(sema, ot, n->field, *node);
        if (mem) {
          sema_symbol_t *msym = sema_lookup(sema->global_scope, *mem);
          bool is_func = msym && msym->kind == SEMA_SYM_FUNC;
          diag_error(sema->diag, sema_loc(sema, *node),
                     is_func
                         ? "'%.*s' is a member of struct '%.*s', not a field "
                           "(methods are accessed as `T::%.*s` or called as "
                           "`t.%.*s(...)`)"
                         : "'%.*s' is a member of struct '%.*s', not a field "
                           "(static properties are accessed as `T::%.*s`)",
                     (int)n->field.len, n->field.ptr, (int)ot->name.len,
                     ot->name.ptr, (int)n->field.len, n->field.ptr,
                     (int)n->field.len, n->field.ptr);
          return val_void(sema);
        }
        diag_error(sema->diag, sema_loc(sema, *node),
                   "struct '%.*s' has no field '%.*s'",
                   (int)ot->name.len, ot->name.ptr,
                   (int)n->field.len, n->field.ptr);
        return val_void(sema);
      }
      return value_make_shadow(sema->vm, struct_type_field(ot, (size_t)idx)->type);
}

/* AST_INDEX 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_index(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 下标 / 泛型索引（GAP 延后落点）：parser 只收集 <expr>[<expr,...>
         的 object + indices 链，此处首次可区分——base 是数组 → 下标（GET）；
         base 是类型值 → 泛型实例化语法（M2 未实现，占位诊断）。 */
      ast_index_t *n = (ast_index_t *)*node;
      value_t *base = sema_expr(sema, &n->object, scope);
      if (is_poison(sema, base))
        return val_void(sema);

      const type_t *bt = value_type(base);
      if (bt && bt->kind == TYPE_KIND_TYPE) {
        /* 泛型实例化占位：<type>[<expr,...> 与下标语法重叠，延后到 sema
           才可区分，M2 泛型未实现前明确占位。 */
        diag_error(sema->diag, sema_loc(sema, *node),
                   "generic instantiation is not implemented (index on a type value)");
        return val_void(sema);
      }
      if (!bt || (bt->kind != TYPE_KIND_ARRAY && bt->kind != TYPE_KIND_TUPLE &&
                  !type_is_slice(bt))) {
        char tn[64];
        sema_type_name(bt, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot index value of type %s", tn);
        return val_void(sema);
      }

      /* 下标只消费 1 个索引；a[i,j] 多索引 = 泛型实参语法预留。
         a[i][j] 多维是 parse 链式嵌套（((a[i])[j])），逐维下降天然处理。 */
      size_t nidx = sema_count_siblings(n->indices);
      if (nidx != 1) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "array subscript expects exactly 1 index, got %zu", nidx);
        return val_void(sema);
      }

      value_t *idx = sema_expr(sema, &n->indices, scope);
      if (is_poison(sema, idx))
        return val_void(sema);
      if (!value_is_type(idx, TYPE_KIND_INT)) {
        char tn[64];
        op_type_name(idx, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, n->indices),
                   "array index must be an integer, got %s", tn);
        return val_void(sema);
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
          ctfe_ctx_t ctx;
          memset(&ctx, 0, sizeof ctx);
          ctx.vm = vm;
          ctx.sema = sema;
          ctx.budget = 100000;
          ctx.max_depth = 128;
          value_t *r = ctfe_eval(&ctx, n->indices);
          if (!r || value_is_error(vm, r)) {
            diag_error(sema->diag, sema_loc(sema, n->indices),
                       "tuple index must be a compile-time constant");
            return val_void(sema);
          }
          const type_t *rt = value_type(r);
          if (!rt || rt->kind != TYPE_KIND_INT) {
            diag_error(sema->diag, sema_loc(sema, n->indices),
                       "tuple index must be an integer constant");
            return val_void(sema);
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
          return val_void(sema);
        }
        const tuple_elem_t *e = tuple_type_elem(bt, (size_t)raw);
        return value_make_shadow(sema->vm, e ? e->type : sema->vm->type_void);
      }

      const type_t *et;
      if (type_is_slice(bt)) {
        et = slice_type_elem(bt);
      } else {
        et = array_type_elem(bt);
      }
      return value_make_shadow(sema->vm, et ? et : sema->vm->type_void);
}

/* AST_SLICE 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_slice(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 切片表达式 s[low:high]（m4-design §4）：适用于数组/切片/str，
         恒产 ref []T（借用胖指针）。low/high 可省略（NULL = 0/len）。
         arr[:] 是显式数组转切片的唯一路径（[N]T → ref []T 不自动）。 */
      ast_slice_t *n = (ast_slice_t *)*node;
      value_t *base = sema_expr(sema, &n->object, scope);
      if (is_poison(sema, base))
        return val_void(sema);

      const type_t *bt = value_type(base);
      if (!bt) return val_void(sema);

      /* 确定元素类型 + 基类合法性 */
      const type_t *et = NULL;
      if (bt->kind == TYPE_KIND_ARRAY) {
        et = array_type_elem(bt);
      } else if (type_is_slice(bt)) {
        et = slice_type_elem(bt);
      } else if (bt->kind == TYPE_KIND_STR) {
        /* str → ref [] const u8（元素不可写，m4-design §4/§8） */
        et = type_const_intern(sema->vm, sema->vm->type_u8);
      } else {
        char tn[64];
        sema_type_name(bt, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot slice value of type %s", tn);
        return val_void(sema);
      }

      /* 求值 low/high（均可是运行期值，运行期做越界检查） */
      if (n->low) {
        value_t *low = sema_expr(sema, &n->low, scope);
        if (is_poison(sema, low))
          return val_void(sema);
        if (!value_is_type(low, TYPE_KIND_INT)) {
          char tn[64];
          op_type_name(low, tn, sizeof(tn));
          diag_error(sema->diag, sema_loc(sema, n->low),
                     "slice lower bound must be an integer, got %s", tn);
          return val_void(sema);
        }
      }
      if (n->high) {
        value_t *high = sema_expr(sema, &n->high, scope);
        if (is_poison(sema, high))
          return val_void(sema);
        if (!value_is_type(high, TYPE_KIND_INT)) {
          char tn[64];
          op_type_name(high, tn, sizeof(tn));
          diag_error(sema->diag, sema_loc(sema, n->high),
                     "slice upper bound must be an integer, got %s", tn);
          return val_void(sema);
        }
      }

      /* 恒产 ref []T */
      const type_t *slice_t = type_slice_intern(sema->vm, OWN_REF,
                                                 et ? et : sema->vm->type_void);
      if (!slice_t) return val_void(sema);
      sema_type_register(sema, slice_t);
      return value_make_shadow(sema->vm, slice_t);
}

/* AST_SCOPE_REF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_scope_ref(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* M5：:: 成员访问有两种语义——枚举 variant（Color::Red）与
         模块成员（std::add）。先检查 type_expr 是否是模块别名
         （AST_IDENT 解析到 SEMA_SYM_MODULE 符号），若是则走模块成员
         路径；否则走枚举 variant 路径。链式访问 a::b::c 由 parser 产出
         嵌套 AST_SCOPE_REF（lhs 也可以是 AST_SCOPE_REF），递归处理。 */
      ast_scope_ref_t *n = (ast_scope_ref_t *)*node;

      /* ---- 跨模块 struct 成员访问：`mod::Type::member` ----
       * type_expr 是嵌套 AST_SCOPE_REF（mod::Type），从模块导出直接解析 struct
       * 类型。被导入模块已完整走完流水线（含 exec_run），运行时
       * struct_type_t.attributes / methods 已填充，从此查成员。
       * 命中方法 → 返回函数签名 shadow（compiler 发 IMPORT→GET_MEMBER 压 type
       * value receiver + MEMBER_CALL，或 LOAD_ATTR 取方法值）。
       * 命中静态属性 → 标记 is_struct_attr + struct_attr_type_id（LOAD_ATTR）。
       * （源模块 sema 已销毁，sema_struct_find_member 的 member_scope 不存在，
       *  只能依赖运行时 attributes。） */
      if (n->type_expr && n->type_expr->kind == AST_SCOPE_REF) {
        ast_scope_ref_t *inner_er = (ast_scope_ref_t *)n->type_expr;
        const type_t *st = resolve_cross_module_struct(sema, inner_er, scope);
        if (!st) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "'%.*s::%.*s' is not a struct type",
                     (int)inner_er->variant.len, inner_er->variant.ptr,
                     (int)n->variant.len, n->variant.ptr);
          return val_void(sema);
        }
        char mbuf[256];
        size_t mn = n->variant.len < sizeof(mbuf) - 1
                        ? n->variant.len : sizeof(mbuf) - 1;
        memcpy(mbuf, n->variant.ptr, mn);
        mbuf[mn] = '\0';
        const struct_type_t *owner = find_runtime_attr_owner(st, mbuf);
        if (!owner) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "struct '%.*s' has no member '%.*s'",
                     (int)st->name.len, st->name.ptr,
                     (int)n->variant.len, n->variant.ptr);
          return val_void(sema);
        }
        value_t *mval = owner->attributes
            ? (value_t *)strmap_get(owner->attributes, mbuf) : NULL;
        if (!mval) return val_void(sema);
        /* 跨模块类型已由被导入模块注册并 vm_type_bind，直接用 t->id */
        if (owner->base.id != 0) {
          n->is_struct_attr = true;
          n->struct_attr_type_id = owner->base.id;
        }
        const type_t *mt = value_type(mval);
        if (mt)
          return value_make_shadow(sema->vm, mt);
        return val_void(sema);
      }

      /* 模块成员访问检查：type_expr 是 AST_IDENT 且解析到 MODULE 符号 */
      if (n->type_expr && n->type_expr->kind == AST_IDENT) {
        ast_ident_t *id = (ast_ident_t *)n->type_expr;
        sema_symbol_t *sym = sema_lookup(scope, id->name);
        /* M6 FFI：`<库别名>::<名>` —— 限定名符号由 pass1_names 注册：
             - `extern cfunc`  → SEMA_SYM_FUNC：**调用**（callee 位置的拦截在
                                  ex_call，本分支落到这里说明是"当值用"）；
             - `extern var`    → SEMA_SYM_EXTERN_VAR：**数据符号引用**（读值；
                                 作为赋值目标由 shadow_assign 的另一入口处理）。
             这里按类型码置 AST 标志（契约见 ast_scope_ref.h），compiler 据
             此发 EXTERN_LOAD；返回声明类型的 shadow 供类型检查。 */
        /* ---- 结构体命名空间成员：`T::x`（静态属性）/ `T::f`（方法 / 限定调用）----
         * struct 成员注册在类型的 member_scope 中（不再展开为全局 `T::name`）。
         * 在 member_scope 中查成员名；未命中时沿嵌入链继承查找。
         * 命中函数 → 改写为 AST_FUNC_REF（fid 引用，compiler 发 LOAD_FUNCTION）。
         * 命中变量 → 返回类型的 shadow（compiler 在 Phase 4 发 LOAD_ATTR）。
         * （枚举 variant 不走这里：它们不注册成全局名，仍由下面的 variant 分支处理。）*/
        if (sym && sym->kind == SEMA_SYM_TYPE) {
          /* 在 member_scope 中查成员名（自己优先 + 嵌入链递归） */
          const struct_type_t *owner = NULL;
          sema_symbol_t *ms = sema_struct_find_member(sema, sym->type,
                                                      n->variant, *node,
                                                      &owner);
          if (ms && ms->kind == SEMA_SYM_FUNC) {
            /* 方法引用 `T::f`（当值用，非调用）：返回函数签名的 shadow。
               compiler 发 LOAD_ATTR 取方法 func value（存在 attributes 中）。
               struct_attr_type_id 用**实际拥有方法的 struct 类型**（owner），
               而非访问者类型——嵌入继承时方法 func value 存在 owner 的
               attributes 中。 */
            if (owner) {
              const sema_type_t *reg = sema_type_find(sema, &owner->base);
              if (reg) {
                n->is_struct_attr = true;
                n->struct_attr_type_id = reg->id;
              }
            }
            sema_resolve_type_slot(sema, &n->type_expr);
            if (ms->type)
              return value_make_shadow(sema->vm, ms->type);
            return val_void(sema);
          }
          if (ms && ms->kind == SEMA_SYM_VAR) {
            /* 静态属性 `T::x`：标记 is_struct_attr + struct_attr_type_id，
               compiler 据此发 LOAD_ATTR <type_id> <name>。返回变量类型的 shadow。
               struct_attr_type_id 用**实际拥有属性的 struct 类型**（owner），
               而非访问者类型——覆盖语义下 Test::count 写 Test 自己的 attributes，
               共享语义下 Test::count（继承自 Base）写 Base 的 attributes。 */
            if (owner) {
              const sema_type_t *reg = sema_type_find(sema, &owner->base);
              if (reg) {
                n->is_struct_attr = true;
                n->struct_attr_type_id = reg->id;
              }
            }
            sema_resolve_type_slot(sema, &n->type_expr);
            if (ms->type)
              return value_make_shadow(sema->vm, ms->type);
            return val_void(sema);
          }
        }
        if (sym && sym->kind == SEMA_SYM_EXTERN_LIB) {
          char qual[512];
          int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s",
                            (int)id->name.len, id->name.ptr,
                            (int)n->variant.len, n->variant.ptr);
          sema_symbol_t *ds = (qn > 0 && (size_t)qn < sizeof(qual))
              ? sema_lookup(sema->global_scope,
                            strslice_from_bytes(qual, (size_t)qn))
              : NULL;
          if (ds && ds->kind == SEMA_SYM_EXTERN_VAR && ds->type) {
            uint32_t kind = 0;
            if (ffi_data_kind_of(ds->type, sema->vm, &kind)) {
              /* 标量/指针/str：宽度直接由类型码表达 */
            } else if (ffi_data_kind_needs_type(ds->type)) {
              /* struct/cunion/enum：宽度/布局要查类型表 → compiler 在读之前压
                 一个类型值（类型表达式取自 extern var 的声明，符号的 ast 有）。 */
              kind = BCODE_FFI_DATA_TYPED;
              ast_var_def_t *evd =
                  (ds->ast && ds->ast->kind == AST_VAR_DEF)
                      ? (ast_var_def_t *)ds->ast
                      : NULL;
              n->ffi_var_type_expr = evd ? evd->type_expr : NULL;
            } else {
              /* pass1c 已就类型报过错；这里只做错误恢复 */
              return val_void(sema);
            }
            n->is_ffi_var = true;
            n->ffi_var_kind = kind;
            return value_make_shadow(sema->vm, ds->type);
          }
          if (ds && ds->kind == SEMA_SYM_FUNC) {
            diag_error(sema->diag, sema_loc(sema, *node),
                       "'%s' is an `extern cfunc` (a function); call it with "
                       "arguments", qual);
            return val_void(sema);
          }
          diag_error(sema->diag, sema_loc(sema, *node),
                     "extern symbol %s: no matching `extern cfunc` or "
                     "`extern var` declaration", qual);
          return val_void(sema);
        }
        if (sym && sym->kind == SEMA_SYM_MODULE && sym->module) {
          /* 模块成员访问：从 module->exports 查 variant 名 */
          module_t *mod = sym->module;
          char buf[256];
          if (n->variant.len < sizeof(buf)) {
            memcpy(buf, n->variant.ptr, n->variant.len);
            buf[n->variant.len] = '\0';
            value_t *member = (value_t *)strmap_get(mod->exports, buf);
            if (member) {
              const type_t *mt = value_type(member);
              return value_make_shadow(sema->vm, mt);
            }
          }
          diag_error(sema->diag, sema_loc(sema, *node),
                     "module has no exported member '%.*s'",
                     (int)n->variant.len, n->variant.ptr);
          return val_void(sema);
        }
      }

      /* 枚举 variant 引用 Color::Red：type_expr 求值为 enum 类型（折叠为
         AST_TYPE_REF）→ enum_type_find_variant 查 variant → 未找到报错；
         找到则折叠底层值入节点（compiler 发 LOAD_TYPE + PUSH_I* + MAKE_ENUM），
         返回 enum 类型 shadow（赋值/判等类型检查用）。 */
      const type_t *t = sema_resolve_type_slot(sema, &n->type_expr);
      if (!t) return val_void(sema);
      if (t->kind != TYPE_KIND_ENUM) {
        char tn[64];
        sema_type_name(t, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "'%s' is not an enum type", tn);
        return val_void(sema);
      }
      int idx = enum_type_find_variant(t, n->variant);
      if (idx < 0) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "enum '%.*s' has no variant '%.*s'",
                   (int)t->name.len, t->name.ptr,
                   (int)n->variant.len, n->variant.ptr);
        return val_void(sema);
      }
      n->value = enum_type_variant(t, (size_t)idx)->value;
      return value_make_shadow(sema->vm, t);
}

/* AST_MAKE 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_make(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* make 编译期宏函数 make(T, N, init...) → fatal []T（m4-design §2）。
         T 是元素类型，N 是长度（可运行期值），init... 是初始值列表（支持
         <v,M> fill 值包，同数组构造）。clux 无默认零值，init 必须覆盖
         全部元素。返回 fatal []T（将亡值，须被 own 接管）。
         sema 校验 init 数量 + 逐元素可赋值性，运行期 MAKE 指令分配 + 布值。 */
      ast_make_t *n = (ast_make_t *)*node;

      /* 解析元素类型 T */
      const type_t *et = sema_resolve_type_slot(sema, &n->elem_type);
      if (!et) return val_void(sema);

      /* 求值长度 N（shadow 求值，运行期可变） */
      value_t *lenv = sema_expr(sema, &n->length, scope);
      if (is_poison(sema, lenv))
        return val_void(sema);
      if (!value_is_type(lenv, TYPE_KIND_INT)) {
        char tn[64];
        op_type_name(lenv, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, n->length),
                   "make: length must be an integer, got %s", tn);
        return val_void(sema);
      }

      /* 校验初始值列表：计算元素总数（含 fill 展开），逐元素类型检查 */
      size_t total = 0;
      /* 链上指针迭代：把**真实的链槽**交给 sema_expr，折叠（comptime 变量引用
         → 字面量）就地写回链表；若传局部变量副本，折叠结果只在栈上有效，
        编译期符号引用会漏到运行期（"undefined variable"）。 */
      ast_node_t **link = &n->inits;
      for (ast_node_t *it = n->inits; it; it = it->next) {
        if (it->kind == AST_FILL) {
          size_t cnt;
          if (!sema_eval_array_bound(sema, &((ast_fill_t *)it)->count, &cnt))
            return val_void(sema);
          /* fill 的值部分也要类型检查 */
          value_t *fv = sema_expr(sema, &((ast_fill_t *)it)->value, scope);
          if (is_poison(sema, fv))
            return val_void(sema);
          value_t *dst = value_make_shadow(sema->vm, et);
          if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
            char tn[64], fn[64];
            sema_type_name(et, tn, sizeof(tn));
            sema_type_name(value_type(fv), fn, sizeof(fn));
            diag_error(sema->diag, sema_loc(sema, ((ast_fill_t *)it)->value),
                       "make: cannot initialize %s element with %s", tn, fn);
          }
          total += cnt;
        } else {
          value_t *fv = sema_expr(sema, link, scope);
          it = *link; /* 折叠可能替换节点：刷新游标，后续 it->next 才有效 */
          if (is_poison(sema, fv))
            return val_void(sema);
          value_t *dst = value_make_shadow(sema->vm, et);
          if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
            char tn[64], fn[64];
            sema_type_name(et, tn, sizeof(tn));
            sema_type_name(value_type(fv), fn, sizeof(fn));
            diag_error(sema->diag, sema_loc(sema, it),
                       "make: cannot initialize %s element with %s", tn, fn);
          }
          total += 1;
        }
        link = &it->next;
      }

      /* clux 无默认零值——init 必须覆盖全部元素（N 可运行期值，编译期
         只校验 init 数量 > 0；N 运行期校验 total == N 由 MAKE 指令兜底）。
         若 N 是编译期常量，编译期即校验 total == N。
         例外：N == 0（零长度切片）允许无初始值。 */
      if (n->inits == NULL) {
        bool zero_len = false;
        if (n->length && n->length->kind == AST_INT_LIT &&
            ((ast_int_lit_t *)n->length)->value == 0)
          zero_len = true;
        if (!zero_len) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "make: initial values are required (clux has no default zero)");
          return val_void(sema);
        }
      }

      /* 若 N 是编译期常量，校验 total == N */
      if (n->length->kind == AST_INT_LIT) {
        uint64_t nval = ((ast_int_lit_t *)n->length)->value;
        if ((uint64_t)total != nval) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "make: expected %llu elements, got %zu",
                     (unsigned long long)nval, total);
          return val_void(sema);
        }
      }

      /* 返回 fatal []T shadow——make 产新堆块，与 new 统一 */
      const type_t *slice_t = type_slice_intern(sema->vm, OWN_FATAL,
                                                 et);
      if (!slice_t) return val_void(sema);
      sema_type_register(sema, slice_t);
      return value_make_shadow(sema->vm, slice_t);
}

/* AST_MOVE 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_move(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 所有权原语 move(x) / clone(x)（m3-design §7）：操作数求值 → 返回
         操作数类型的 shadow。move 转移所有权（原对象进 TDZ，Step A 静默），
         clone 深拷贝。指针操作数（own/ref/fatal）返回 fatal 所有权修饰类型
         （将亡值，§3.3——运行期 MOVE/CLONE 指令产物即 fatal 类型）；
         非指针原样返回操作数类型。 */
      ast_move_t *n = (ast_move_t *)*node;
      value_t *operand = sema_expr(sema, &n->operand, scope);
      if (is_poison(sema, operand))
        return val_void(sema);
      const type_t *ot = value_type(operand);
      const type_t *fatal = sema_fatal_variant_of(sema, ot);
      if (!fatal) return val_void(sema);
      bool is_clone = n->op && token_is(n->op, "clone");
      bool produces_fatal = (fatal != ot);
      if (!is_clone && (produces_fatal || type_is_fatal(ot)) &&
          n->operand->kind == AST_IDENT) {
        sema_symbol_t *src =
            sema_lookup(scope, ((ast_ident_t *)n->operand)->name);
        if (src && src->kind == SEMA_SYM_VAR && src->type &&
            type_is_own(src->type)) {
          if (src->borrow_count > 0) {
            diag_error(
                sema->diag, sema_loc(sema, *node),
                "cannot move '%.*s': %d live borrow(s) still "
                "reference it (borrows must end before move)",
                (int)((ast_ident_t *)n->operand)->name.len,
                ((ast_ident_t *)n->operand)->name.ptr, src->borrow_count);
          } else {
            src->moved = true;
          }
        } else if (src && src->kind == SEMA_SYM_VAR &&
                   sema_type_is_fatal(src->type)) {
          sema_mark_fatal_consumed(src);
        }
      }
      return value_make_shadow(sema->vm, fatal);
}

/* AST_INT_LIT 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_int_lit(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
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

/* AST_FLOAT_LIT 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_float_lit(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
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

/* AST_CONST 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_const(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* const 类型修饰（类型即表达式）：操作数必须是类型值，结果仍是
         类型值。遮蔽/类型检查在此感知（as 右值、M2 类型字面量）。 */
      ast_const_t *n = (ast_const_t *)*node;
      value_t *sub = sema_expr(sema, &n->sub, scope);
      if (is_poison(sema, sub))
        return val_void(sema);
      if (!value_is_type(sub, TYPE_KIND_TYPE)) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "'const' requires a type operand");
        return val_void(sema);
      }
      /* type value 恒真实（编译期实体）：取 data 里的类型指针 intern 后
         返回新的 type value（auto-track 当前 scope） */
      const type_t *sub_t = value_as(sub, const type_t *);
      const type_t *ct = type_const_intern(sema->vm, sub_t);
      return type_as_value(sema->vm, ct);
}

/* AST_VOLATILE 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_volatile(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      ast_volatile_t *n = (ast_volatile_t *)*node;
      value_t *sub = sema_expr(sema, &n->sub, scope);
      if (is_poison(sema, sub))
        return val_void(sema);
      if (!value_is_type(sub, TYPE_KIND_TYPE)) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "'volatile' requires a type operand");
        return val_void(sema);
      }
      /* `volatile T` 在脚本态无意义：表达式形式同样折叠为 T（与类型解析一致） */
      return sub;
}

/* AST_SIZEOF / AST_ALIGNOF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_sizeof_alignof(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 编译期运算符（m2-design §8）：操作数 shadow 求值只取类型（不真实
         执行），结果恒 u64。sizeof = 类型宽度，alignof = 类型对齐。
         折叠为 AST_INT_LIT(u64) 写回（保留兄弟链）——编译器零感知消费
         立即数；返回 shadow u64。 */
      ast_node_t *opnd = (*node)->kind == AST_SIZEOF
                             ? ((ast_sizeof_t *)*node)->operand
                             : ((ast_alignof_t *)*node)->operand;
      value_t *op = sema_expr(sema, &opnd, scope);
      /* 折叠写回：sema_expr 通过可写节点槽就地改写（comptime 变量引用 → 字面量），
         本处因两个结构体二选一而只能传局部变量，必须显式回写——否则 comptime
         符号引用会漏到运行期（该名字在运行期不存在 → "undefined variable"）。 */
      if ((*node)->kind == AST_SIZEOF)
        ((ast_sizeof_t *)*node)->operand = opnd;
      else
        ((ast_alignof_t *)*node)->operand = opnd;
      if (is_poison(sema, op))
        return val_void(sema);
      /* type value（data=type_t*）解引用 data；普通 shadow value 取
         value_type。注意 value_type(type value) 返回元类型 vm->type_type
         （size=8），直接读取会得错值——sizeof(u8) 折叠成 8。 */
      const type_t *t = value_is_type(op, TYPE_KIND_TYPE)
                            ? value_as(op, const type_t *)
                            : value_type(op);
      uint64_t n = (*node)->kind == AST_SIZEOF ? (uint64_t)t->size
                                               : (uint64_t)t->align;
      ast_node_t *lit = ast_int_lit_new(sema->arena, (*node)->tok_begin,
                                        (*node)->tok_end);
      if (lit) {
        /* 折叠成 `usize` 字面量：宽度/长度类结果的正确类型（C 的 size_t） */
        ((ast_int_lit_t *)lit)->type = vm_type_usize(sema->vm)->name;
        ((ast_int_lit_t *)lit)->value = n;
        lit->next = (*node)->next;
        *node = lit;
      }
      return value_make_shadow(sema->vm, vm_type_usize(sema->vm));
}

/* AST_TYPEOF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_typeof(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* typeof(expr) → type value（m2-design §8）：操作数 shadow 求值只取
         类型，结果即该类型。折叠为 AST_TYPE_REF（sema_ct_type_ref 登记到
         types 表，编译器 LOAD_TYPE <id> 零感知）；返回真实 type value
         （编译期实体，data 恒为 type_t*）。 */
      ast_typeof_t *n = (ast_typeof_t *)*node;
      value_t *op = sema_expr(sema, &n->operand, scope);
      if (is_poison(sema, op))
        return val_void(sema);
      /* 同上：type value 解引用 data，普通值取 value_type */
      const type_t *t = value_is_type(op, TYPE_KIND_TYPE)
                            ? value_as(op, const type_t *)
                            : value_type(op);
      ast_node_t *ref = sema_ct_type_ref(sema, t, *node);
      if (ref) {
        ref->next = (*node)->next;
        *node = ref;
      }
      return type_as_value(sema->vm, t);
}

/* AST_UNARY 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_unary(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      ast_unary_t *n = (ast_unary_t *)*node;
      value_t *operand = sema_expr(sema, &n->operand, scope);
      /* 错误恢复产物静默通过，避免级联二次诊断 */
      if (is_poison(sema, operand))
        return val_void(sema);
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
        return val_void(sema);
      }
      if (value_is_error(sema->vm, result)) {
        char ob[16], tn[64];
        op_text(n->op, ob, sizeof(ob));
        op_type_name(operand, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "operator '%s' cannot be applied to %s", ob, tn);
        return val_void(sema);
      }
      return result;
}

/* AST_UNWRAP 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_unwrap(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* optional 解包：.!（assert）/ .?（try）。
         .! 要求操作数是 ?T：none 时运行期 panic（用户范式先判空再解包：
         if (x != nil) { var v = x.!; ... }）。返回 inner 类型 shadow。
         .? 仅词法预留（try），语义未实现 → 编译期报错。 */
      ast_unwrap_t *uw = (ast_unwrap_t *)*node;
      value_t *operand = sema_expr(sema, &uw->operand, scope);
      if (is_poison(sema, operand))
        return val_void(sema);

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
        return val_void(sema);
      }
      if (uw->op && token_is(uw->op, "?")) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "operator '.?' (try) is not implemented yet");
        return val_void(sema);
      }
      return value_make_shadow(sema->vm, type_option_inner(ot));
}

/* AST_SLICE_TYPE 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_slice_type(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
      /* 切片类型表达式 own/ref/fatal []T（类型即表达式，m4-design §1）：
         解析为真实类型并就地替换为 AST_TYPE_REF（编译器发 LOAD_TYPE），
         返回真实 type value。与 AST_ARRAY 同构。 */
      const type_t *t = sema_resolve_type_slot(sema, node);
      if (!t) return val_void(sema);
      return type_as_value(sema->vm, t);
}

/* AST_ARRAY 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_array(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
      /* 数组类型表达式 [N]T（类型即表达式）：表达式位置求值 = 类型值。
         解析为真实类型并就地替换为 AST_TYPE_REF（编译器发 LOAD_TYPE），
         返回真实 type value（data=type_t*，auto-track 当前 scope），供
         type 定义 / as 右值 / sizeof / 嵌套类型构造消费。 */
      const type_t *t = sema_resolve_type_slot(sema, node);
      if (!t) return val_void(sema);
      return type_as_value(sema->vm, t);
}

/* AST_TUPLE / AST_FUNC_TYPE / AST_OPTION / AST_PTR 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_type_expr(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
      /* 其余**类型表达式**出现在表达式位置（类型即表达式，m2-design 关键
         架构决策 6）：元组 `<T1,T2>` / 签名 `func(T)->R` / 可选 `?T` /
         指针 `own *T` 都应求值为类型值，与上方 AST_ARRAY 完全同构——解析为
         真实类型并就地替换为 AST_TYPE_REF（编译器发 LOAD_TYPE），返回
         type value 供 type 定义 / as 右值 / sizeof / 嵌套类型构造消费。

         此前这 4 种 kind 没有 case，落到 switch 的 default 静默返回 void
         shadow：`var x = <i32,i32>;` 之类合法写法被误报成
         "cannot initialize ... with void"，在字段位则静默跳过校验。 */
      const type_t *t = sema_resolve_type_slot(sema, node);
      if (!t) return val_void(sema);
      return type_as_value(sema->vm, t);
}

/* AST_TYPE_REF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_type_ref(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
      /* 具名类型引用（sema 登记的 "__type_N"）：重复求值（折叠重访）时
         命中——解析回类型，返回真实 type value。 */
      const type_t *t = resolve_type_expr(sema, *node);
      if (!t) return val_void(sema);
      return type_as_value(sema->vm, t);
}

/* AST_DEREF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_deref(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 后置解引用取值 r.*（m3-design §8.2）：由指针得值。操作数必须是
         指针类型（own/ref/fatal）→ 返回被指向类型 T 的 shadow（运行期
         PTR_GET 指令取 T 值副本）。 */
      ast_deref_t *n = (ast_deref_t *)*node;
      value_t *operand = sema_expr(sema, &n->operand, scope);
      if (is_poison(sema, operand))
        return val_void(sema);
      const type_t *ot = value_type(operand);
      const type_t *base = ptr_type_base(ot);
      if (!base) {
        char tn[64];
        op_type_name(operand, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot dereference a value of type %s (expected a pointer)",
                   tn);
        return val_void(sema);
      }
      return value_make_shadow(sema->vm, base);
}

/* AST_ADDR 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_addr(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 后置取地址 x.&（m3-design §8.2）：由值得指针。返回 ref *T 类型的
         shadow——x.& 指向栈上值（或他人堆块）是借用，ref 总借用（type kind
         区分，运行期零标志 §13.2），销毁 no-op。
         返回 ref（而非 own）与值层语义一致（Step B 借用存活检查的基座）。 */
      ast_addr_t *n = (ast_addr_t *)*node;
      value_t *operand = sema_expr(sema, &n->operand, scope);
      if (is_poison(sema, operand))
        return val_void(sema);
      const type_t *t = value_type(operand);
      if (!t || t->kind == TYPE_KIND_VOID || t->kind == TYPE_KIND_ERROR) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot take address of value of unknown type");
        return val_void(sema);
      }
      const type_t *pt = type_ptr_intern(sema->vm, OWN_REF, t);
      if (!pt) return val_void(sema);
      sema_type_register(sema, pt);
      return value_make_shadow(sema->vm, pt);
}

/* AST_FUNC_DEF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_func_def(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 函数字面量（表达式内 func 定义，函数值）：签名解析 + body 类型
         检查（sema_check_func_literal：临时 fscope 同步建树+walk，不注册
         作用域符号、不提升）。返回签名类型的 shadow——函数值表达式的类型
         即签名。失败（已诊断）返回 void shadow。 */
      ast_func_def_t *fn = (ast_func_def_t *)*node;
      const type_t *sig = sema_check_func_literal(sema, fn, scope);
      if (!sig) return val_void(sema);
      return value_make_shadow(sema->vm, sig);
}

/* AST_FUNC_REF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_func_ref(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
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
        return val_void(sema);
      }
      return value_make_shadow(sema->vm, sig);
}

/* AST_TERNARY 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
static value_t *ex_ternary(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
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
        return val_void(sema);
      const type_t *tt = value_type(tval);
      const type_t *et = value_type(eval);
      if (tt != et) {
        char tn[64], en[64];
        op_type_name(tval, tn, sizeof(tn));
        op_type_name(eval, en, sizeof(en));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "ternary branches must have the same type, got %s and %s",
                   tn, en);
        return val_void(sema);
      }
      return tval; /* shadow，类型 = 两分支共同类型 */
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
    if (!ut || ut->kind != TYPE_KIND_UNION) {
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
