#include "sema/sema.h"
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

/* AST_NEW 处理器（自 sema_expr 拆出；纯搬移，逻辑与拆分前逐字一致） */
value_t *ex_new(sema_t *sema, ast_node_t **node,
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

      /* `...operand` 展开（与 `.T{...}` 同一份实现：struct 目标按字段 kv，
         array/tuple 目标按位置）。 */
      if (!sema_expand_construct_spread(sema, &n->fields, &n->fields_last,
                                        scope, t))
        return val_void(sema);

      /* 字段数量与逐字段可赋值性校验（struct/tuple/array/union/cunion 同
         AST_CONSTRUCT 语义；scalar 等无字段类型 = 1 个空字段集合）。Step A
         只校验类型层面，字段求值走 sema_expr 递归（含嵌套 new/匿名构造）。 */
      if (type_kind_of(t) == TYPE_KIND_STRUCT) {
        const struct_type_t *bst = (const struct_type_t *)type_strip_quals(t);
        if (bst->bitfield_ast) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "new: bitfield heap construction is not supported yet");
          return val_void(sema);
        }
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
                         "type %s is not optional)", tn);
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
        ast_node_t *new_head = NULL, *new_last = NULL;
        for (size_t i = 0; i < cap; i++)
          ast_append(&new_head, &new_last, NULL, slot[i]);
        n->fields      = new_head;
        n->fields_last = new_last;
      } else if (type_kind_of(t) == TYPE_KIND_TUPLE) {
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
        ast_node_t *new_head = NULL, *new_last = NULL;
        for (size_t i = 0; i < tt->elem_count; i++)
          ast_append(&new_head, &new_last, NULL, slot[i]);
        n->fields      = new_head;
        n->fields_last = new_last;
      } else if (type_kind_of(t) == TYPE_KIND_ARRAY) {
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
            if (!et || type_kind_of(et) != TYPE_KIND_OPTION) {
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
      } else if (type_kind_of(t) == TYPE_KIND_OPTION) {
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
            const type_t *ft = option_field_type_normalized(sema, value_type(fv));
            if (ft != t && !option_inner_type_ok(inner, ft)) {
              char tn[64], fn[64];
              sema_type_name(t, tn, sizeof tn);
              sema_type_name(value_type(fv), fn, sizeof fn);
              diag_error(sema->diag, sema_loc(sema, f),
                         "cannot initialize optional %s with %s", tn, fn);
            }
          }
        }
      } else if (type_kind_of(t) == TYPE_KIND_UNION || type_kind_of(t) == TYPE_KIND_CUNION) {
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
                     type_kind_of(t) == TYPE_KIND_UNION ? "union" : "cunion");
          return val_void(sema);
        }
        ast_construct_field_t *cf = (ast_construct_field_t *)f;
        const type_t *mt = NULL;
        if (type_kind_of(t) == TYPE_KIND_UNION) {
          const int mi = union_type_find_member(t, cf->name);
          if (mi < 0) {
            char tn[64];
            sema_type_name(t, tn, sizeof tn);
            diag_error(sema->diag, sema_loc(sema, f),
                       "union %s has no member '%.*s'",
                       tn, (int)cf->name.len, cf->name.ptr);
            return val_void(sema);
          }
          /* member 下标回写（compiler 发 tag 哨兵用，与 AST_CONSTRUCT
             同协议——`new U{...}` 不是"payload → union"的隐式转换，必须
             走 CONSTRUCT 才写得进 tag）。 */
          n->member_tag = (uint32_t)mi;
          const union_member_t *m = union_type_member(t, (size_t)mi);
          if (m && m->payload_type) {
            mt = m->payload_type;
          }
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
          n->member_tag = (uint32_t)mi;
          const cunion_member_t *m = cunion_type_member(t, (size_t)mi);
          if (m && m->type) {
            mt = m->type;
          }
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
          if (type_kind_of(mt) != TYPE_KIND_OPTION) {
            char tn[64];
            sema_type_name(mt, tn, sizeof tn);
            diag_error(sema->diag, sema_loc(sema, value),
                       "cannot initialize %s field with nil (field type %s "
                       "is not optional)",
                       type_kind_of(t) == TYPE_KIND_UNION ? "union" : "cunion", tn);
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
                       type_kind_of(t) == TYPE_KIND_UNION ? "union" : "cunion",
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

      /* 返回 fatal *T 的 shadow——new 产新堆块，与
         clone/move 统一：所有"产新堆块"路径都返回 fatal（§3.3 将亡值），
         由 var 定义/参数/return 显式接管为 own（value_assign → implicit_cast
         fatal→own 转移）。所有权纯编译期：fatal 是值层 ownership 状态。 */
      const type_t *ptr = type_ptr_intern(sema->vm, t);
      if (!ptr) return val_void(sema);
      sema_type_register(sema, ptr);
      return value_make_shadow(sema->vm, ptr);
}

/* AST_MAKE 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_make(sema_t *sema, ast_node_t **node,
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

      /* 返回 fatal []T shadow——make 产新堆块，与 new 统一。
         所有权纯编译期：fatal 是值层 ownership 状态。
         元素槽位所有权进类型（`make(own *i32, …)` 的元素按 own 释放）。 */
      const type_t *slice_base = type_slice_intern(sema->vm, et);
      if (!slice_base) return val_void(sema);
      sema_type_register(sema, slice_base);
      return value_make_shadow(sema->vm, slice_base);
}
