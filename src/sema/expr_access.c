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
#include "sema/sema_cross.h"
#include "sema/sema_generic.h"
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
#include "vm/ffi_lib.h" /* M6 FFI：ffi_data_kind_of（extern var 的类型码） */
#include "vm/bcode.h"   /* M6 FFI：BCODE_FFI_RET_*（extern cfunc 的返回类型码） */
#include "core/strmap.h"

#include <stdio.h>
#include <string.h>


#include "sema_expr_internal.h"

/* AST_MEMBER 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
/* ===========================================================================
 * 反射注入（`T.<key>` / `T.<key>(i)`）—— sema 折叠为常量
 *
 * 设计见 docs/reflect-builtin-design.md：
 *   - 接收者必须是 **type value**（类型名 / typeof(x) / 类型别名 / 反射链式结果）；
 *   - 值域 str / usize / type / bool（kind 与 ownership 都是 str，自描述）；
 *   - 折叠成字面量/类型引用，运行期零指令。
 * =========================================================================== */

/* 该 AST 形态可能是类型表达式（用于安静地尝试 resolve，避免对变量成员访问
   误报类型诊断）。 */
static bool reflect_may_be_type(ast_node_t *n) {
  if (!n) return false;
  switch (n->kind) {
  case AST_IDENT:
  case AST_TYPE_REF:
  case AST_TYPEOF:
  case AST_ARRAY:
  case AST_TUPLE:
  case AST_FUNC_TYPE:
  case AST_OPTION:
  case AST_PTR:
  case AST_SLICE_TYPE:
  case AST_OWNED_TYPE:
  case AST_CONST:
  case AST_VOLATILE:
    return true;
  default:
    return false;
  }
}

/* 接收者若是类型表达式，返回其类型；否则 NULL（安静）。 */
static const type_t *reflect_receiver_type(sema_t *sema, ast_node_t *obj) {
  if (!reflect_may_be_type(obj)) return NULL;
  return resolve_type_expr(sema, obj);
}

/* arena 拷贝字符串（字面量要活到 compiler 阶段） */
static strslice_t reflect_str(sema_t *sema, const char *s) {
  size_t n = s ? strlen(s) : 0;
  char *buf = (char *)arena_alloc(sema->arena, n + 1, 1);
  if (!buf) return STRSLICE_EMPTY;
  if (n) memcpy(buf, s, n);
  buf[n] = '\0';
  return strslice_from_bytes(buf, n);
}

/* 折叠产物 → 表达式节点（替换 *node）。返回该表达式的 shadow 值。 */
static value_t *reflect_fold(sema_t *sema, ast_node_t **node,
                             const sema_rv_t *rv) {
  ast_node_t *old = *node;
  ast_node_t *lit = NULL;
  value_t *sv = NULL;
  switch (rv->kind) {
  case SEMA_RV_STR: {
    lit = ast_string_lit_new(sema->arena, old->tok_begin, old->tok_end);
    if (!lit) return val_void(sema);
    strslice_t s = reflect_str(sema, rv->s.ptr ? (const char *)rv->s.ptr : "");
    /* 注意：rv->s 可能带内嵌 NUL 之外的内容——按 len 截断拷贝更准确 */
    if (rv->s.ptr && rv->s.len > 0) {
      char *buf = (char *)arena_alloc(sema->arena, rv->s.len + 1, 1);
      if (!buf) return val_void(sema);
      memcpy(buf, rv->s.ptr, rv->s.len);
      buf[rv->s.len] = '\0';
      s = strslice_from_bytes(buf, rv->s.len);
    }
    ((ast_string_lit_t *)lit)->text = s;
    sv = value_make_shadow(sema->vm, sema->vm->type_str);
    break;
  }
  case SEMA_RV_USIZE: {
    lit = ast_int_lit_new(sema->arena, old->tok_begin, old->tok_end);
    if (!lit) return val_void(sema);
    ((ast_int_lit_t *)lit)->type = STRSLICE_LIT("usize");
    ((ast_int_lit_t *)lit)->value = (int64_t)rv->u;
    sv = value_make_shadow(sema->vm, vm_type_usize(sema->vm));
    break;
  }
  case SEMA_RV_I64: {
    lit = ast_int_lit_new(sema->arena, old->tok_begin, old->tok_end);
    if (!lit) return val_void(sema);
    ((ast_int_lit_t *)lit)->type = STRSLICE_LIT("i64");
    ((ast_int_lit_t *)lit)->value = rv->i;
    sv = value_make_shadow(sema->vm, sema->vm->type_i64);
    break;
  }
  case SEMA_RV_BOOL: {
    lit = ast_bool_lit_new(sema->arena, old->tok_begin, old->tok_end);
    if (!lit) return val_void(sema);
    ((ast_bool_lit_t *)lit)->value = rv->b;
    sv = value_make_shadow(sema->vm, sema->vm->type_bool);
    break;
  }
  case SEMA_RV_TYPE: {
    const type_t *t = rv->t;
    if (!t) {
      diag_error(sema->diag, sema_loc(sema, old),
                 "reflection: this member has no type");
      return val_void(sema);
    }
    const sema_type_t *st = sema_type_register(sema, t);
    lit = ast_type_ref_new(sema->arena, old->tok_begin, old->tok_end);
    if (!lit) return val_void(sema);
    ((ast_type_ref_t *)lit)->name = st ? st->name : t->name;
    sv = type_as_value(sema->vm, t);
    break;
  }
  default:
    return val_void(sema);
  }
  lit->is_generated = true;
  lit->next = old->next;
  *node = lit;
  return sv;
}

/* 索引实参：编译期常量（CTFE 求值）→ 非负整数。 */
static bool reflect_eval_index(sema_t *sema, ast_node_t *arg,
                               sema_scope_t *scope, uint64_t *out) {
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.vm = sema->vm;
  ctx.sema = sema;
  ctx.sema_scope = scope;
  ctx.budget = 100000;
  ctx.max_depth = 128;
  value_t *v = ctfe_eval(&ctx, arg);
  if (!v || value_is_error(sema->vm, v)) {
    diag_error(sema->diag, sema_loc(sema, arg),
               "reflection: index argument must be a compile-time constant");
    return false;
  }
  const type_t *vt = value_type(v);
  if (!vt || type_kind_of(vt) != TYPE_KIND_INT || !value_data(v)) {
    diag_error(sema->diag, sema_loc(sema, arg),
               "reflection: index argument must be an integer");
    return false;
  }
  int64_t sv = 0;
  switch (vt->size) {
  case 1: sv = vt->vtable == &VTABLE_INT_SIGNED
                   ? *(const int8_t *)value_data(v)
                   : *(const uint8_t *)value_data(v);
          break;
  case 2: sv = vt->vtable == &VTABLE_INT_SIGNED
                   ? *(const int16_t *)value_data(v)
                   : *(const uint16_t *)value_data(v);
          break;
  case 4: sv = vt->vtable == &VTABLE_INT_SIGNED
                   ? *(const int32_t *)value_data(v)
                   : *(const uint32_t *)value_data(v);
          break;
  default: sv = *(const int64_t *)value_data(v); break;
  }
  if (sv < 0) {
    diag_error(sema->diag, sema_loc(sema, arg),
               "reflection: index must be non-negative, got %lld",
               (long long)sv);
    return false;
  }
  *out = (uint64_t)sv;
  return true;
}

/* 名字实参（`method_type("run")`）：编译期常量字符串。 */
static bool reflect_eval_name_arg(sema_t *sema, ast_node_t *arg,
                                  sema_scope_t *scope, strslice_t *out) {
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.vm = sema->vm;
  ctx.sema = sema;
  ctx.sema_scope = scope;
  ctx.budget = 100000;
  ctx.max_depth = 128;
  value_t *v = ctfe_eval(&ctx, arg);
  if (!v || value_is_error(sema->vm, v)) {
    diag_error(sema->diag, sema_loc(sema, arg),
               "reflection: name argument must be a compile-time constant string");
    return false;
  }
  if (value_type(v) != sema->vm->type_str || !value_data(v)) {
    diag_error(sema->diag, sema_loc(sema, arg),
               "reflection: name argument must be a string");
    return false;
  }
  const char *s = *(const char *const *)value_data(v);
  if (!s) s = "";
  *out = strslice_from_bytes(s, strlen(s));
  return true;
}

/* 是否是"反射表达式"（`T.key(i)`，接收者是类型或另一个反射表达式）。
   链式场景要先把它折叠成类型引用，外层才拿得到接收者类型。 */
static bool is_reflect_expr(ast_node_t *n) {
  if (!n) return false;
  /* `T.key` 形式：接收者是类型表达式或另一个反射表达式 */
  if (n->kind == AST_MEMBER) {
    ast_member_t *m = (ast_member_t *)n;
    if (!sema_reflect_is_key(m->field)) return false;
    if (reflect_may_be_type(m->object)) return true;
    return is_reflect_expr(m->object);
  }
  /* `T.key(i)` 形式 */
  if (n->kind == AST_CALL) {
    ast_call_t *c = (ast_call_t *)n;
    if (!c->callee || c->callee->kind != AST_MEMBER) return false;
    return is_reflect_expr(c->callee);
  }
  return false;
}

/* 链式接收者：先折叠内层反射调用（把节点换成类型引用/字面量）。 */
static bool reflect_prefold(sema_t *sema, ast_node_t **obj, sema_scope_t *scope) {
  if (!obj || !*obj || !is_reflect_expr(*obj)) return true;
  value_t *v = sema_expr(sema, obj, scope);
  return !is_poison(sema, v);
}

/* ex_member 的反射分支：接收者是类型 + key 命中 → 折叠。
   handled == true 表示已处理，*out 为折叠结果的 shadow（诊断失败时 NULL）。 */
static bool reflect_member(sema_t *sema, ast_node_t **node,
                           sema_scope_t *scope, value_t **out) {
  ast_member_t *m = (ast_member_t *)*node;
  *out = NULL;
  if (!reflect_prefold(sema, &m->object, scope)) return true;
  const type_t *T = reflect_receiver_type(sema, m->object);
  if (!T) return false;
  sema_rv_t rv;
  char err[512] = {0};
  if (sema_reflect_needs_index(T, m->field)) {
    diag_error(sema->diag, sema_loc(sema, *node),
               "reflection: '%.*s' requires an index argument (write "
               "T.%.*s(0))",
               (int)m->field.len, m->field.ptr, (int)m->field.len,
               m->field.ptr);
    return true;
  }
  if (!sema_reflect_eval(sema, T, m->field, NULL, &rv, err, sizeof err)) {
    diag_error(sema->diag, sema_loc(sema, *node), "%s", err);
    return true;
  }
  *out = reflect_fold(sema, node, &rv);
  return true;
}

/* ex_call 的反射分支：`T.key(i)`。返回 true 表示已处理（*out 为结果 shadow）。 */
bool reflect_member_call(sema_t *sema, ast_node_t **node,
                                sema_scope_t *scope, value_t **out) {
  ast_call_t *call = (ast_call_t *)*node;
  *out = NULL;
  if (!call->callee || call->callee->kind != AST_MEMBER) return false;
  ast_member_t *m = (ast_member_t *)call->callee;
  if (!reflect_prefold(sema, &m->object, scope)) return true;
  const type_t *T = reflect_receiver_type(sema, m->object);
  if (!T) return false;
  sema_rv_t rv;
  char err[512] = {0};
  sema_rarg_t ak = sema_reflect_arg_kind(T, m->field);
  if (ak == SEMA_RARG_NONE) {
    /* 未知 key → 走 lookup 让它给出"没有该成员 + 可用列表"的诊断；
       已知但不带实参的 key → "takes no arguments"。 */
    if (!sema_reflect_is_key(m->field)) {
      if (!sema_reflect_eval(sema, T, m->field, NULL, &rv, err, sizeof err))
        diag_error(sema->diag, sema_loc(sema, *node), "%s", err);
      return true;
    }
    diag_error(sema->diag, sema_loc(sema, *node),
               "reflection: '%.*s' takes no arguments", (int)m->field.len,
               m->field.ptr);
    return true;
  }
  size_t nargs = sema_count_siblings(call->args);
  if (nargs != 1) {
    diag_error(sema->diag, sema_loc(sema, *node),
               "reflection: '%.*s' expects exactly 1 %s argument, got %zu",
               (int)m->field.len, m->field.ptr,
               ak == SEMA_RARG_INDEX ? "index" : "name", nargs);
    return true;
  }
  sema_reflect_arg_t rarg;
  memset(&rarg, 0, sizeof rarg);
  rarg.kind = ak;
  if (ak == SEMA_RARG_INDEX) {
    if (!reflect_eval_index(sema, call->args, scope, &rarg.index)) return true;
  } else {
    if (!reflect_eval_name_arg(sema, call->args, scope, &rarg.name))
      return true;
  }
  if (!sema_reflect_eval(sema, T, m->field, &rarg, &rv, err, sizeof err)) {
    diag_error(sema->diag, sema_loc(sema, *node), "%s", err);
    return true;
  }
  *out = reflect_fold(sema, node, &rv);
  return true;
}

value_t *ex_member(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 字段读取 p.field：object 求值 → 校验 struct/union → 按名查字段
         （struct_type_find_field / union_type_find_field）→ 返回字段类型
         shadow。嵌套 p.a.b 递归（object 是 AST_MEMBER 时 sema_expr 返回
         内层字段类型）。字段不存在 → 报错。union 字段不做静态 tag 断言
         （sema 不感知运行期 tag——运行期 FIELD_GET 反查 member + tag 校验，
         不符返回 error value 即 panic）。 */
      ast_member_t *n = (ast_member_t *)*node;
      /* `T.<key>`：接收者是 type value 时按反射 key 折叠（见 reflect_member）。 */
      {
        value_t *rsv = NULL;
        if (reflect_member(sema, node, scope, &rsv))
          return rsv ? rsv : val_void(sema);
      }
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
         指向 struct/cunion/union；pointee 可带 const/volatile）。做法是就地改写
         AST——把当前 object 节点包进一个 AST_DEREF（compiler 据此发 PTR_GET，
         与手写 `p.*.field` 完全同路），然后按**剥掉限定符**的 base 类型继续查
         字段。仅指向复合类型的指针才解引用（`pp.*` 指向指针时须显式写）。
         object 已经求值过，**不重新求值**（避免调用/函数字面量被二次处理）。 */
      {
        const type_t *base = type_strip_quals(ptr_type_base(ot));
        if (base && (type_kind_of(base) == TYPE_KIND_STRUCT ||
                     type_kind_of(base) == TYPE_KIND_CUNION ||
                     type_kind_of(base) == TYPE_KIND_UNION)) {
          ast_node_t *deref = ast_deref_new(sema->arena, (*node)->tok_begin,
                                            (*node)->tok_end);
          if (!deref) return val_void(sema);
          ((ast_deref_t *)deref)->operand = n->object;
          n->object = deref;
          ot = base;
        }
      }
      /* 闭包 `.env`：func 值的捕获聚合结构体（capture_type）。这是 sema 为
         "捕获读取 → __ctx.env.<cap>" 改写提供的类型入口；用户源码里没有
         `f.env` 语法（sema 不放行对闭包值的字段访问）。 */
      if (type_kind_of(ot) == TYPE_KIND_FUNC && sema->fn.def &&
          sema->fn.def->capture_type_id != 0 && n->field.len == 3 &&
          memcmp(n->field.ptr, "env", 3) == 0) {
        const sema_type_t *caps =
            sema_type_by_id(sema, sema->fn.def->capture_type_id);
        if (caps && caps->type)
          return value_make_shadow(sema->vm, caps->type);
      }

      if (type_kind_of(ot) == TYPE_KIND_UNION) {
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
      if (type_kind_of(ot) == TYPE_KIND_CUNION) {
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
      if (type_kind_of(ot) != TYPE_KIND_STRUCT) {
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
/* P9：comptime 值参数包 `...N: comptime u64` 的索引消费 `N[i]`。
 * 在实例化体内把 `N[i]` 折叠为对应值的整数字面量（编译期 tuple 语义；
 * 裸 `N` 无标量含义，不绑定）。 */
/* 类型参数包变参 `args[i]`：编译期折叠为隐藏形参 `__pack_arg_i`。
 * Args 已知，索引必须是编译期整数；不产生运行期 tuple。 */
static value_t *type_pack_index_try(sema_t *sema, ast_node_t **node,
                                    ast_index_t *n) {
  const sema_generic_instance_t *gi = sema->generics.active_inst;
  if (!gi || !gi->has_pack || gi->pack_is_value) return NULL;
  if (!n->object || n->object->kind != AST_IDENT) return NULL;
  if (!sema_generic_pack_is_param(
          sema, ((ast_ident_t *)n->object)->name))
    return NULL;
  if (sema_count_siblings(n->indices) != 1) {
    diag_error(sema->diag, sema_loc(sema, *node),
               "variadic parameter '%.*s' expects exactly one index",
               (int)gi->pack_param_name.len, gi->pack_param_name.ptr);
    return val_void(sema);
  }
  uint64_t raw = 0;
  ast_node_t *idx_expr = n->indices;
  if (idx_expr->kind == AST_INT_LIT) {
    raw = ((ast_int_lit_t *)idx_expr)->value;
  } else {
    ctfe_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.vm = sema->vm;
    ctx.sema = sema;
    ctx.budget = 100000;
    ctx.max_depth = 128;
    value_t *r = ctfe_eval(&ctx, idx_expr);
    if (!r || value_is_error(sema->vm, r) || !value_type(r) ||
        type_kind_of(value_type(r)) != TYPE_KIND_INT) {
      diag_error(sema->diag, sema_loc(sema, idx_expr),
                 "variadic parameter index must be a compile-time integer");
      return val_void(sema);
    }
    switch (value_type(r)->size) {
    case 1: raw = *(const uint8_t *)value_data(r); break;
    case 2: raw = *(const uint16_t *)value_data(r); break;
    case 4: raw = *(const uint32_t *)value_data(r); break;
    default: raw = *(const uint64_t *)value_data(r); break;
    }
  }
  if (raw >= gi->pack_count) {
    diag_error(sema->diag, sema_loc(sema, *node),
               "variadic parameter '%.*s' index %llu out of range (count %zu)",
               (int)gi->pack_param_name.len, gi->pack_param_name.ptr,
               (unsigned long long)raw, gi->pack_count);
    return val_void(sema);
  }
  const sema_generic_arg_t *a = &gi->args[gi->pack_offset + raw];
  ast_node_t *id = sema_generic_pack_arg_expr(
      sema, gi, (size_t)raw, /*move_own=*/false, (*node)->tok_begin,
      (*node)->tok_end);
  if (!id) return val_void(sema);
  id->next = (*node)->next;
  *node = id;
  return value_make_shadow(sema->vm, a->type);
}

static value_t *value_pack_index_try(sema_t *sema, ast_node_t **node,
                                     ast_index_t *n) {
  const sema_generic_instance_t *gi = sema->generics.active_inst;
  if (!gi || !gi->has_pack || !gi->pack_is_value) return NULL;
  if (!n->object || n->object->kind != AST_IDENT) return NULL;
  if (!strslice_eq(((ast_ident_t *)n->object)->name, gi->pack_name))
    return NULL;
  if (sema_count_siblings(n->indices) != 1) {
    diag_error(sema->diag, sema_loc(sema, *node),
               "value parameter pack '%.*s' expects exactly one index",
               (int)gi->pack_name.len, gi->pack_name.ptr);
    return val_void(sema);
  }
  uint64_t raw = 0;
  ast_node_t *idx_expr = n->indices;
  if (idx_expr->kind == AST_INT_LIT) {
    raw = ((ast_int_lit_t *)idx_expr)->value;
  } else {
    ctfe_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.vm = sema->vm;
    ctx.sema = sema;
    ctx.budget = 100000;
    ctx.max_depth = 128;
    value_t *r = ctfe_eval(&ctx, idx_expr);
    if (!r || value_is_error(sema->vm, r) || !value_type(r) ||
        type_kind_of(value_type(r)) != TYPE_KIND_INT) {
      diag_error(sema->diag, sema_loc(sema, idx_expr),
                 "value parameter pack index must be a compile-time integer");
      return val_void(sema);
    }
    switch (value_type(r)->size) {
    case 1: raw = *(const uint8_t *)value_data(r); break;
    case 2: raw = *(const uint16_t *)value_data(r); break;
    case 4: raw = *(const uint32_t *)value_data(r); break;
    default: raw = *(const uint64_t *)value_data(r); break;
    }
  }
  if (raw >= gi->pack_count) {
    diag_error(sema->diag, sema_loc(sema, *node),
               "value parameter pack '%.*s' index %llu out of range (count %zu)",
               (int)gi->pack_name.len, gi->pack_name.ptr,
               (unsigned long long)raw, gi->pack_count);
    return val_void(sema);
  }
  const sema_generic_arg_t *a = &gi->args[gi->pack_offset + raw];
  if (!a->is_value || !a->value.type) {
    diag_error(sema->diag, sema_loc(sema, *node),
               "value parameter pack element is not a compile-time value");
    return val_void(sema);
  }
  uint64_t v = 0;
  if (a->value.type->kind == TYPE_KIND_BOOL) {
    v = a->value.b ? 1 : 0;
  } else if (a->value.type->name.len == 3 && a->value.type->name.ptr &&
             memcmp(a->value.type->name.ptr, "u64", 3) == 0) {
    v = a->value.u;
  } else {
    switch (a->value.type->size) {
    case 1: v = (uint8_t)a->value.i; break;
    case 2: v = (uint16_t)a->value.i; break;
    case 4: v = (uint32_t)a->value.i; break;
    default: v = (uint64_t)a->value.i; break;
    }
  }
  ast_node_t *lit = ast_int_lit_new(sema->arena, (*node)->tok_begin,
                                    (*node)->tok_end);
  if (!lit) return val_void(sema);
  ((ast_int_lit_t *)lit)->value = v;
  ((ast_int_lit_t *)lit)->type = a->value.type->name;
  lit->is_generated = true;
  lit->next = (*node)->next;
  *node = lit;
  return value_make_shadow(sema->vm, a->value.type);
}

value_t *ex_index(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 下标 / 泛型索引（GAP 延后落点）：parser 只收集 <expr>[<expr,...>
         的 object + indices 链，此处首次可区分——base 是数组 → 下标（GET）；
         base 是类型值 → 泛型实例化语法（M2 未实现，占位诊断）。 */
      ast_index_t *n = (ast_index_t *)*node;

      /* P9：`N[i]`（comptime 值参数包）先折叠为整数字面量。 */
      {
        value_t *pv = type_pack_index_try(sema, node, n);
        if (pv) return pv;
      }
      {
        value_t *pv = value_pack_index_try(sema, node, n);
        if (pv) return pv;
      }

      /* M7 泛型函数取实例：`id[i32]`（函数值，非调用）。与调用点同一套
         实例化；改写为 AST_FUNC_REF（fid）——函数值就是普通函数实体。 */
      if (n->object &&
          (n->object->kind == AST_IDENT || n->object->kind == AST_SCOPE_REF)) {
        sema_symbol_t *gs =
            n->object->kind == AST_IDENT
                ? sema_lookup(scope, ((ast_ident_t *)n->object)->name)
                : sema_cross_generic_symbol(sema, n->object);
        if (gs && gs->is_generic && gs->ast &&
            gs->ast->kind == AST_FUNC_DEF) {
          size_t nta = 0;
          sema_generic_arg_t *eargs = NULL;
          bool ok = sema_generic_parse_explicit_args(sema, gs, n->indices, &nta,
                                                     &eargs);
          const sema_generic_instance_t *inst = NULL;
          const type_t *sig =
              ok ? sema_generic_instantiate_func(sema, gs, eargs, nta, *node,
                                                 &inst)
                 : NULL;
          if (eargs) allocator_free(sema->vm->alloc, (void **)&eargs);
          if (!sig || !inst) return val_void(sema);
          ast_node_t *ref =
              ast_func_ref_new(sema->arena, (*node)->tok_begin, (*node)->tok_end);
          if (!ref) return val_void(sema);
          ((ast_func_ref_t *)ref)->fid = inst->fid;
          ref->is_generated = true;
          ref->next = (*node)->next;
          *node = ref;
          return value_make_shadow(sema->vm, sig);
        }
      }

      value_t *base = sema_expr(sema, &n->object, scope);
      if (is_poison(sema, base))
        return val_void(sema);

      const type_t *bt = value_type(base);
      if (bt && type_kind_of(bt) == TYPE_KIND_TYPE) {
        /* 泛型实例化占位：<type>[<expr,...> 与下标语法重叠，延后到 sema
           才可区分，M2 泛型未实现前明确占位。 */
        diag_error(sema->diag, sema_loc(sema, *node),
                   "generic instantiation is not implemented (index on a type value)");
        return val_void(sema);
      }
      if (!bt || (type_kind_of(bt) != TYPE_KIND_ARRAY && type_kind_of(bt) != TYPE_KIND_TUPLE &&
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

      if (type_kind_of(bt) == TYPE_KIND_TUPLE) {
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
          if (!rt || type_kind_of(rt) != TYPE_KIND_INT) {
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
value_t *ex_slice(sema_t *sema, ast_node_t **node,
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
      if (type_kind_of(bt) == TYPE_KIND_ARRAY) {
        et = array_type_elem(bt);
      } else if (type_is_slice(bt)) {
        et = slice_type_elem(bt);
      } else if (type_kind_of(bt) == TYPE_KIND_STR) {
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
      const type_t *slice_t = type_slice_intern(sema->vm,
                                                 et ? et : sema->vm->type_void);
      if (!slice_t) return val_void(sema);
      sema_type_register(sema, slice_t);
      return value_make_shadow(sema->vm, slice_t);
}

/* AST_SCOPE_REF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_scope_ref(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* M5：:: 成员访问有两种语义——枚举 variant（Color::Red）与
         模块成员（std::add）。先检查 type_expr 是否是模块别名
         （AST_IDENT 解析到 SEMA_SYM_MODULE 符号），若是则走模块成员
         路径；否则走枚举 variant 路径。链式访问 a::b::c 由 parser 产出
         嵌套 AST_SCOPE_REF（lhs 也可以是 AST_SCOPE_REF），递归处理。 */
      ast_scope_ref_t *n = (ast_scope_ref_t *)*node;

      /* M7：`Vec[i32]::member` —— 先把类型位实例化并改写为运行时类型名。 */
      (void)sema_generic_rewrite_type_expr(sema, &n->type_expr);

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
        /* 阶段 1：依赖模块可能尚未 finalize（runtime attributes 未建）→ 用
           其 sema 成员作用域解析。 */
        if (inner_er->type_expr &&
            inner_er->type_expr->kind == AST_IDENT) {
          sema_symbol_t *modsym = sema_lookup(
              scope, ((ast_ident_t *)inner_er->type_expr)->name);
          if (modsym && modsym->kind == SEMA_SYM_MODULE && modsym->module &&
              !modsym->module->finalized &&
              sema->module_finalize.finalize) {
            if (!sema->module_finalize.finalize(sema->module_finalize.user,
                                                modsym->module))
              return val_void(sema);
          }
          if (modsym && modsym->kind == SEMA_SYM_MODULE && modsym->module &&
              modsym->module->sema_global) {
            /* 直接查模块导出的类型符号 member_scope（不依赖 runtime
               attributes；嵌入继承的回退仍走 sema_struct_find_member）。 */
            sema_symbol_t *tsym =
                sema_module_export_symbol(modsym->module, inner_er->variant);
            sema_symbol_t *ms = NULL;
            if (tsym && tsym->kind == SEMA_SYM_TYPE && tsym->member_scope)
              ms = sema_scope_find_local(tsym->member_scope, n->variant);
            sema_scope_t *saved_global = sema->global_scope;
            sema->global_scope = modsym->module->sema_global;
            const struct_type_t *owner1 = NULL;
            if (!ms)
              ms = sema_struct_find_member(sema, st, n->variant, *node,
                                           &owner1);
            sema->global_scope = saved_global;
            if (ms && (ms->kind == SEMA_SYM_FUNC || ms->kind == SEMA_SYM_VAR)) {
              if (owner1 && owner1->base.id != 0) {
                n->is_struct_attr = true;
                n->struct_attr_type_id = owner1->base.id;
              }
              return ms->type ? value_make_shadow(sema->vm, ms->type)
                              : val_void(sema);
            }
          }
        }
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
          module_t *mod = sym->module;
          /* 阶段 1：依赖模块未 finalize 时从 sema_exports 取符号。 */
          sema_symbol_t *msym =
              sema_module_export_symbol(mod, n->variant);
          if (msym) {
            if (msym->kind == SEMA_SYM_TYPE)
              return value_make_shadow(sema->vm, sema->vm->type_type);
            if (msym->kind == SEMA_SYM_FUNC) {
              if (msym->is_generic) {
                diag_error(sema->diag, sema_loc(sema, *node),
                           "generic function '%.*s' requires type arguments",
                           (int)n->variant.len, n->variant.ptr);
                return val_void(sema);
              }
              return msym->type
                         ? value_make_shadow(sema->vm, msym->type)
                         : val_void(sema);
            }
            if (msym->kind == SEMA_SYM_VAR && msym->type)
              return value_make_shadow(sema->vm, msym->type);
          }
          /* 模块成员访问：从 module->exports 查 variant 名 */
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
      if (type_kind_of(t) != TYPE_KIND_ENUM) {
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
