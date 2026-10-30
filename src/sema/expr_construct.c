#include "sema/sema.h"
#include "sema/sema_generic.h"
#include "sema/reflect.h"
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
#include "parser/ast_struct_def.h"
#include "parser/ast_member.h"
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
#include "parser/ast_take.h"
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
#include "core/ffi_abi.h"   /* M6 FFI：CLUX_FFI_RET_*（extern cfunc 的返回类型码） */
#include "core/strmap.h"

#include <stdio.h>
#include <string.h>


#include "sema_expr_internal.h"


#include "expr_construct_internal.h"

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
      if (!ft || type_kind_of(ft) != TYPE_KIND_OPTION) {
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
    /* 匿名构造的字段槽位所有权：字段值若是将亡值（new/move/clone/
       含 own 字段的构造），该槽位即接管所有权 → OWN；否则 NONE。
       ref 槽位在匿名构造里由 type_expr 一侧决定，此处无显式标注 → NONE。 */
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
      if (!et || type_kind_of(et) != TYPE_KIND_OPTION) {
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

/* `?inner` 构造字段的类型判定：字段类型必须就是 inner。 */
bool option_inner_type_ok(const type_t *inner, const type_t *ft) {
  return inner && ft && ft == inner;
}

/* AST_CONSTRUCT 处理器（自 sema_expr 拆出；纯搬移，逻辑与拆分前逐字一致） */
static value_t *construct_result(sema_t *sema, const type_t *t) {
  return value_make_shadow(sema->vm, t);
}

/* ===========================================================================
 * 构造器里的展开 `...operand`
 *
 *   .Point{ ...s }        ⇒ .Point{ .x = s.x, .y = s.y, … }   （struct：字段 kv）
 *   .[3]i32{ ...arr }     ⇒ .[3]i32{ arr[0], arr[1], arr[2] }（array/tuple：位置）
 *   new Point{ ...s }     ⇒ 同上（AST_NEW 内部的构造同样走这里）
 *
 * 目标类型决定形态：struct 目标 ⇒ 操作数必须是 struct（按操作数字段表展开）；
 * array/tuple 目标 ⇒ 操作数必须是同长度的数组/元组（按操作数元素个数展开，
 * 长度不符由外层"字段数"检查报错）。
 *
 * 与函数实参展开一致：操作数必须是**位置表达式**（变量/成员访问）——展开后
 * 每个字段/元素都是一次 `operand.f` / `operand[i]` 读取。
 * =========================================================================== */
bool sema_expand_construct_spread(sema_t *sema, ast_node_t **fields,
                                         ast_node_t **fields_last,
                                         sema_scope_t *scope,
                                         const type_t *target) {
  ast_node_t **slot = fields;
  while (*slot) {
    ast_node_t *f = *slot;
    if (f->kind != AST_SPREAD) {
      slot = &f->next;
      continue;
    }
    ast_spread_t *sp = (ast_spread_t *)f;
    ast_node_t *op = sp->operand;
    /* `...N`（comptime 值参数包）：展开为字面量字段。 */
    if (op && op->kind == AST_IDENT &&
        sema_generic_value_pack_is_param(sema,
                                        ((ast_ident_t *)op)->name)) {
      const sema_generic_instance_t *gi = sema->generics.active_inst;
      ast_node_t *head = NULL, *tail = NULL;
      for (size_t i = 0; i < gi->pack_count; i++) {
        ast_node_t *e = sema_generic_value_pack_elem_expr(
            sema, gi, i, f->tok_begin, f->tok_end);
        if (!e) return false;
        e->next = NULL;
        if (tail) tail->next = e;
        else head = e;
        tail = e;
      }
      if (head) {
        tail->next = f->next;
        *slot = head;
        slot = &tail->next;
      } else {
        *slot = f->next;
      }
      continue;
    }
    /* `.{ ...args }`：Args 已知，展开为隐藏形参字段；own 元素自动 move。 */
    if (op && op->kind == AST_IDENT &&
        sema_generic_pack_is_param(sema, ((ast_ident_t *)op)->name)) {
      const sema_generic_instance_t *gi = sema->generics.active_inst;
      ast_node_t *head = NULL, *tail = NULL;
      for (size_t i = 0; i < gi->pack_count; i++) {
        ast_node_t *e = sema_generic_pack_arg_expr(
            sema, gi, i, f->tok_begin, f->tok_end);
        if (!e) return false;
        e->next = NULL;
        if (tail) tail->next = e;
        else head = e;
        tail = e;
      }
      if (head) {
        tail->next = f->next;
        *slot = head;
        slot = &tail->next;
      } else {
        *slot = f->next;
      }
      continue;
    }
    if (!op || (op->kind != AST_IDENT && op->kind != AST_MEMBER)) {
      diag_error(sema->diag, sema_loc(sema, f),
                 "spread operand in a constructor must be a variable or a "
                 "field access (it is read once per field/element; bind "
                 "temporaries first)");
      return false;
    }
    /* 操作数类型：决定按 kv 还是按位置展开。 */
    value_t *ov = sema_expr(sema, &sp->operand, scope);
    if (is_poison(sema, ov)) return false;
    const type_t *ot = value_type(ov);
    if (!ot) return false;

    if (type_kind_of(ot) == TYPE_KIND_STRUCT) {
      /* struct：按操作数的字段表展开成具名字段。 */
      if (target && type_kind_of(target) != TYPE_KIND_STRUCT &&
          type_kind_of(target) != TYPE_KIND_CUNION) {
        char tn[64];
        sema_type_name(target, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, f),
                   "cannot spread a struct into a constructor of type %s", tn);
        return false;
      }
      size_t cnt = struct_type_field_count(ot);
      ast_node_t *head = NULL, *tail = NULL;
      for (size_t i = 0; i < cnt; i++) {
        const struct_field_t *sf = struct_type_field(ot, i);
        if (!sf) return false;
        /* 目标 struct 必须真有这个字段：直接报"没有该字段"，比落到字段数
           不符（expected N fields, got M）好定位。 */
        if (target && type_kind_of(target) == TYPE_KIND_STRUCT &&
            struct_type_find_field(target, sf->name) < 0) {
          diag_error(sema->diag, sema_loc(sema, f),
                     "struct '%.*s' has no field '%.*s' (from spread operand)",
                     (int)target->name.len, target->name.ptr,
                     (int)sf->name.len, sf->name.ptr);
          return false;
        }
        ast_node_t *mem = ast_member_new(sema->arena, f->tok_begin, f->tok_end);
        if (!mem) return false;
        mem->is_generated = true;
        ((ast_member_t *)mem)->object = sp->operand;
        ((ast_member_t *)mem)->field = sf->name;
        ast_node_t *cf =
            ast_construct_field_new(sema->arena, f->tok_begin, f->tok_end);
        if (!cf) return false;
        cf->is_generated = true;
        ((ast_construct_field_t *)cf)->name = sf->name;
        ((ast_construct_field_t *)cf)->value = mem;
        if (tail) {
          tail->next = cf;
          tail = cf;
        } else {
          head = tail = cf;
        }
      }
      if (head) {
        tail->next = f->next;
        *slot = head;
        slot = &tail->next;
      } else {
        *slot = f->next;
      }
      continue;
    }

    if (type_kind_of(ot) == TYPE_KIND_ARRAY || type_kind_of(ot) == TYPE_KIND_TUPLE) {
      /* array / tuple：按位置展开成 `operand[i]`。 */
      if (target && type_kind_of(target) == TYPE_KIND_STRUCT) {
        char tn[64];
        sema_type_name(target, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, f),
                   "cannot spread an array/tuple into a struct constructor "
                   "(%s needs named fields)",
                   tn);
        return false;
      }
      size_t cnt = (type_kind_of(ot) == TYPE_KIND_ARRAY) ? array_type_len(ot)
                                                 : tuple_type_elem_count(ot);
      ast_node_t *head = NULL, *tail = NULL;
      for (size_t i = 0; i < cnt; i++) {
        ast_node_t *idx = ast_index_new(sema->arena, f->tok_begin, f->tok_end);
        if (!idx) return false;
        idx->is_generated = true;
        ast_node_t *lit =
            ast_int_lit_new(sema->arena, f->tok_begin, f->tok_end);
        if (!lit) return false;
        lit->is_generated = true;
        ((ast_int_lit_t *)lit)->type = strslice_from_bytes("i32", 3);
        ((ast_int_lit_t *)lit)->value = i;
        ((ast_index_t *)idx)->object = sp->operand;
        ((ast_index_t *)idx)->indices = lit;
        ((ast_index_t *)idx)->indices_last = lit;
        if (tail) {
          tail->next = idx;
          tail = idx;
        } else {
          head = tail = idx;
        }
      }
      if (head) {
        tail->next = f->next;
        *slot = head;
        slot = &tail->next;
      } else {
        *slot = f->next;
      }
      continue;
    }

    {
      char tn[64];
      sema_type_name(ot, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, f),
                 "spread operand in a constructor must be a struct, array or "
                 "tuple with a compile-time-known shape, got %s",
                 tn);
      return false;
    }
  }
  /* struct 目标：同名字段**后写的覆盖先写的**（与显式重复字段的既有语义一致），
     因此 `.{ ...p, .x = 10 }` 合法：展开出的 `.x` 被后面的显式 `.x` 覆盖。
     去重保留每个名字的**最后一次**出现，其余字段保序。 */
  if (target && type_kind_of(target) == TYPE_KIND_STRUCT) {
    ast_node_t *kept = NULL, *kept_last = NULL;
    for (ast_node_t *f = *fields; f; f = f->next) {
      bool dup = false;
      if (f->kind == AST_CONSTRUCT_FIELD) {
        strslice_t nm = ((ast_construct_field_t *)f)->name;
        for (ast_node_t *g = f->next; g; g = g->next) {
          if (g->kind == AST_CONSTRUCT_FIELD) {
            strslice_t gn = ((ast_construct_field_t *)g)->name;
            if (gn.len == nm.len && gn.ptr && nm.ptr &&
                memcmp(gn.ptr, nm.ptr, nm.len) == 0) {
              dup = true;
              break;
            }
          }
        }
      }
      if (dup) continue;
      if (kept_last)
        kept_last->next = f;
      else
        kept = f;
      kept_last = f;
    }
    if (kept_last) kept_last->next = NULL;
    *fields = kept;
  }

  /* fields_last 重算（展开改变链尾） */
  ast_node_t *last = NULL;
  for (ast_node_t *x = *fields; x; x = x->next) last = x;
  if (fields_last) *fields_last = last;
  return true;
}

value_t *ex_construct(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 类型字面量构造 .<type>{ fields }：求值类型位为真实类型，校验
         fields 数量与元素类型（value_assign 单一校验点），返回该类型
         的 shadow value（运行期由 CONSTRUCT 字节码完成值构造）。
         当前实现 array + option + struct + tuple 分支。 */
      ast_construct_t *n = (ast_construct_t *)*node;
      const type_t *t;
      /* `. { N }`：值参数包没有标量值，元素类型即 tuple 元素类型；
         用户用构造语法决定是否组装 tuple。 */
      if (!n->type && n->fields && !n->fields->next &&
          n->fields->kind == AST_IDENT &&
          sema_generic_value_pack_is_param(
              sema, ((ast_ident_t *)n->fields)->name)) {
        n->type = sema_generic_value_pack_tuple_type_expr(
            sema, sema->generics.active_inst, n->base.tok_begin,
            n->base.tok_end);
        if (!n->type) return val_void(sema);
      }
      /* `. { ...args }`：Args 已知，直接给出 `<...Args>` 目标类型；
         spread 随后展开成隐藏形参（不经过运行期 tuple）。 */
      if (!n->type && n->fields && !n->fields->next &&
          n->fields->kind == AST_SPREAD) {
        ast_spread_t *sp = (ast_spread_t *)n->fields;
        if (sp->operand && sp->operand->kind == AST_IDENT &&
            sema_generic_pack_is_param(
                sema, ((ast_ident_t *)sp->operand)->name)) {
          n->type = sema_generic_pack_tuple_type_expr(
              sema, sema->generics.active_inst, n->base.tok_begin,
              n->base.tok_end);
          if (!n->type) return val_void(sema);
        }
      }
      /* 裸值参数包字段 `.{ N }`：按元素展开为编译期字面量；是否组装
         tuple 由用户写不写构造语法决定。 */
      {
        ast_node_t **slot = &n->fields;
        while (*slot) {
          ast_node_t *f = *slot;
          if (f->kind == AST_IDENT &&
              sema_generic_value_pack_is_param(
                  sema, ((ast_ident_t *)f)->name)) {
            const sema_generic_instance_t *gi =
                sema->generics.active_inst;
            ast_node_t *head = NULL, *tail = NULL;
            for (size_t i = 0; i < gi->pack_count; i++) {
              ast_node_t *e = sema_generic_value_pack_elem_expr(
                  sema, gi, i, f->tok_begin, f->tok_end);
              if (!e) return val_void(sema);
              e->next = NULL;
              if (tail) tail->next = e;
              else head = e;
              tail = e;
            }
            if (head) {
              tail->next = f->next;
              *slot = head;
              slot = &tail->next;
            } else {
              *slot = f->next;
            }
            continue;
          }
          slot = &f->next;
        }
      }
      /* `...operand` 展开先于一切字段处理（具名/匿名、new/普通构造共用）。
         目标类型：具名构造解析类型位；匿名构造取 anon_ct 栈顶（可能没有）。 */
      {
        const type_t *ct_target = NULL;
        if (n->type)
          ct_target = sema_resolve_type_slot(sema, &n->type);
        else if (sema->ex.anon_ct_depth > 0)
          ct_target = sema->ex.anon_ct[sema->ex.anon_ct_depth - 1];
        if (!sema_expand_construct_spread(sema, &n->fields, &n->fields_last,
                                           scope, ct_target))
          return val_void(sema);
      }
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
        if (type_kind_of(ct) == TYPE_KIND_STRUCT) {
          /* struct 目标：字段已在 infer_anon_struct 求值+校验+重排完毕，
             直接返回（不落入下方 struct 构造分支，避免二次求值重复诊断）。 */
          t = infer_anon_struct(sema, n, scope, ct);
          if (!t) return val_void(sema);
        } else if (type_kind_of(ct) == TYPE_KIND_TUPLE) {
          /* tuple 目标：元素已在 infer_anon_tuple 求值+校验+重排完毕，
             直接返回（不落入下方 tuple 构造分支，避免二次求值重复诊断）。 */
          t = infer_anon_tuple(sema, n, scope, ct);
          if (!t) return val_void(sema);
        } else if (type_kind_of(ct) == TYPE_KIND_OPTION) {
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
        return construct_result(sema, t);
      } else {
        t = sema_resolve_type_slot(sema, &n->type);
        if (!t) return val_void(sema);
      }

      /* ?T 构造器（二值单槽位，docs m2-design §12.2）：字段数强制 == 1，
         字段类型 ∈ {nil, T}。nil 字段 → 运行期 PUSH_OPT_NONE；T 字段 →
         隐式提升 D1。字段类型显式校验（不走 value_assign——option_assign
         的 shadow 短路会绕过类型检查，须在此处直接比较 inner）。 */
      /* bitfield 构造：.Flags{ .a = 1, .b = 2 }。缺失字段补零；按声明序
         重排成 bit_fields 链，compiler 用 BITFIELD_MAKE 生成隐藏数组。 */
      if (type_kind_of(t) == TYPE_KIND_STRUCT) {
        const struct_type_t *bst = (const struct_type_t *)type_strip_quals(t);
        if (bst->bitfield_ast) {
          ast_struct_def_t *bsd = (ast_struct_def_t *)bst->bitfield_ast;
          const sema_type_t *ut =
              sema_type_by_id(sema, bsd->bitfield_underlying_type_id);
          const type_t *utype =
              ut && ut->type ? ut->type
                             : vm_type_load(sema->vm,
                                            bsd->bitfield_underlying_type_id);
          if (!utype) return val_void(sema);
          ast_node_t *provided[128] = {0};
          for (ast_node_t *f = n->fields; f; f = f->next) {
            if (f->kind != AST_CONSTRUCT_FIELD) {
              diag_error(sema->diag, sema_loc(sema, f),
                         "bitfield constructor requires named fields");
              return val_void(sema);
            }
            ast_construct_field_t *cf = (ast_construct_field_t *)f;
            bool found = false;
            for (size_t i = 0; i < bsd->bitfield_field_count; i++) {
              if (bsd->bitfield_fields[i].name.len == cf->name.len &&
                  memcmp(bsd->bitfield_fields[i].name.ptr, cf->name.ptr,
                         cf->name.len) == 0) {
                if (provided[i]) {
                  diag_error(sema->diag, sema_loc(sema, f),
                             "construct: duplicate bitfield field '%.*s'",
                             (int)cf->name.len, cf->name.ptr);
                  return val_void(sema);
                }
                provided[i] = cf->value;
                value_t *fv = sema_expr(sema, &cf->value, scope);
                if (fv && !value_is_error(sema->vm, fv)) {
                  bool int_ok = type_kind_of(utype) == TYPE_KIND_INT &&
                                type_kind_of(value_type(fv)) == TYPE_KIND_INT;
                  value_t *casted = int_ok ? fv
                                           : value_implicit_cast(sema->vm, fv, utype);
                  if (!casted || value_is_error(sema->vm, casted)) {
                    char tn[64], fn[64];
                    sema_type_name(utype, tn, sizeof tn);
                    sema_type_name(value_type(fv), fn, sizeof fn);
                    diag_error(sema->diag, sema_loc(sema, cf->value),
                               "cannot initialize bitfield field '%.*s' with %s "
                               "(field type %s)",
                               (int)cf->name.len, cf->name.ptr, fn, tn);
                  }
                }
                found = true;
                break;
              }
            }
            if (!found) {
              diag_error(sema->diag, sema_loc(sema, f),
                         "bitfield '%.*s' has no field '%.*s'",
                         (int)t->name.len, t->name.ptr, (int)cf->name.len,
                         cf->name.ptr);
              return val_void(sema);
            }
          }
          ast_node_t *head = NULL, *tail = NULL;
          for (size_t i = 0; i < bsd->bitfield_field_count; i++) {
            ast_node_t *v = provided[i];
            if (!v) {
              v = ast_int_lit_new(sema->arena, n->base.tok_begin,
                                  n->base.tok_end);
              if (!v) return val_void(sema);
              v->is_generated = true;
              ((ast_int_lit_t *)v)->value = 0;
              ((ast_int_lit_t *)v)->type = utype->name;
            }
            ast_node_t *cf = ast_construct_field_new(
                sema->arena, n->base.tok_begin, n->base.tok_end);
            if (!cf) return val_void(sema);
            cf->is_generated = true;
            ((ast_construct_field_t *)cf)->name = bsd->bitfield_fields[i].name;
            ((ast_construct_field_t *)cf)->value = v;
            if (tail) tail->next = cf;
            else head = cf;
            tail = cf;
          }
          n->is_bitfield = true;
          n->bit_word_bits = bsd->bitfield_word_bits;
          n->bit_field_count = (uint32_t)bsd->bitfield_field_count;
          n->bit_fields = head;
          n->bit_underlying_type_id = bsd->bitfield_underlying_type_id;
          n->bit_storage_type_id = bsd->bitfield_storage_type_id;
          return construct_result(sema, t);
        }
      }

      if (type_kind_of(t) == TYPE_KIND_OPTION) {
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
            if (ft != t && !option_inner_type_ok(inner, ft)) {
              char tn[64], fn[64];
              sema_type_name(t, tn, sizeof tn);
              /* 提示用**原始**类型名：归一后的 stripped base 可能是 intern 出来的
                 未登记实例（无名字），打印出来会是空的。 */
              sema_type_name(value_type(fv), fn, sizeof fn);
              diag_error(sema->diag, sema_loc(sema, f),
                         "cannot initialize optional %s with %s", tn, fn);
            }
          }
        }
        return construct_result(sema, t);
      }

      if (type_kind_of(t) == TYPE_KIND_STRUCT) {
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
            /* 字段类型是 func 时，`cfunc_ptr(<命名 cfunc>)` 自动转包装。 */
            if (ft && type_kind_of(ft) == TYPE_KIND_FUNC)
              sema_maybe_wrap_cfunc_expr(sema, &slot[i], ft, scope);
            bool ct_pushed = sema_anon_push(sema, ft);
            fv = sema_expr(sema, &slot[i], scope);
            sema_anon_pop(sema, ct_pushed);
          }
          if (is_nil_field) {
            if (!ft || type_kind_of(ft) != TYPE_KIND_OPTION) {
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
        return construct_result(sema, t);
      }

      if (type_kind_of(t) == TYPE_KIND_TUPLE) {
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
            if (!et || type_kind_of(et) != TYPE_KIND_OPTION) {
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
        return construct_result(sema, t);
      }

      if (type_kind_of(t) == TYPE_KIND_UNION) {
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
          if (type_kind_of(m->payload_type) != TYPE_KIND_OPTION) {
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
        return construct_result(sema, t);
      }

      if (type_kind_of(t) == TYPE_KIND_CUNION) {
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
          if (type_kind_of(m->type) != TYPE_KIND_OPTION) {
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
        return construct_result(sema, t);
      }

      /* **基本类型构造**（2026-10）：`.i32{123}` / `.bool{true}` /
         `.f64{1.5}` / `.str{"hi"}` —— 单值构造。
           - 恰好 1 个字段（位置值，不写字段名）；
           - 值可与目标类型不同（按隐式转换，如 `.i64{7}` / `.f64{1}`）；
           - 结果是该类型的**普通值**（可拷贝）——与字面量同构，
             只是把类型显式写出来。`var a = .i32{123};` ≡ `var a: i32 = 123;`。 */
      if (t && (type_kind_of(t) == TYPE_KIND_INT || type_kind_of(t) == TYPE_KIND_FLOAT ||
                type_kind_of(t) == TYPE_KIND_BOOL || type_kind_of(t) == TYPE_KIND_STR)) {
        ast_node_t *field = n->fields;
        if (!field) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "basic type construct requires exactly one value "
                     "(`.%s{<value>}`)",
                     t->name.ptr ? t->name.ptr : "T");
          return val_void(sema);
        }
        if (field->next) {
          diag_error(sema->diag, sema_loc(sema, field->next),
                     "basic type construct takes exactly one value (got more)");
          return val_void(sema);
        }
        /* 字段形态：具名 `.i32{ .value = 1 }` 拒绝（基本类型是位置值）；
           位置值 `.i32{123}` 的字段节点是**表达式本身**（与数组元素同款）。 */
        ast_node_t *value = field;
        if (field->kind == AST_CONSTRUCT_FIELD) {
          ast_construct_field_t *cf = (ast_construct_field_t *)field;
          if (cf->name.len != 0) {
            diag_error(sema->diag, sema_loc(sema, field),
                       "basic type construct takes a positional value "
                       "(write `.%s{<value>}`, without a field name)",
                       t->name.ptr ? t->name.ptr : "T");
            return val_void(sema);
          }
          value = cf->value;
        }
        {
          value_t *fv = sema_expr(sema, &value, scope);
          if (is_poison(sema, fv)) return val_void(sema);
          /* 可赋值性：同类型身份 / 隐式转换（i32 → i64、int → f64）；
             隐式不行再试**显式转换**（`.<type>{...}` 显式写出类型，允许收窄，
             语义等同 C 复合字面量 `(T){v}`）。 */
          value_t *dst = value_make_shadow(sema->vm, t);
          if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
            value_t *ex = value_explicit_cast(sema->vm, fv, t);
            if (value_is_error(sema->vm, ex)) {
              char tn[64], fn[64];
              sema_type_name(t, tn, sizeof tn);
              sema_type_name(value_type(fv), fn, sizeof fn);
              diag_error(sema->diag, sema_loc(sema, value),
                         "cannot initialize %s with %s in basic type construct",
                         tn, fn);
              return val_void(sema);
            }
          }
          return construct_result(sema, t);
        }
      }

      if (type_kind_of(t) != TYPE_KIND_ARRAY) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "construct: unsupported type (only basic types, array, "
                   "optional, struct, tuple, union and cunion implemented)");
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
          if (!et || type_kind_of(et) != TYPE_KIND_OPTION) {
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
      return construct_result(sema, t);
}
