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

/* make(pointer, len) → []T：只构造切片视图，不分配内存。 */
value_t *ex_make(sema_t *sema, ast_node_t **node, sema_scope_t *scope) {
  ast_make_t *n = (ast_make_t *)*node;
  value_t *pv = sema_expr(sema, &n->pointer, scope);
  if (is_poison(sema, pv)) return val_void(sema);
  const type_t *pt = pv ? value_type(pv) : NULL;
  pt = pt ? type_strip_quals(pt) : NULL;
  if (!pt || type_kind_of(pt) != TYPE_KIND_PTR) {
    char tn[64];
    op_type_name(pv, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, n->pointer),
               "make: first argument must be a pointer (*T), got %s", tn);
    return val_void(sema);
  }
  value_t *lv = sema_expr(sema, &n->length, scope);
  if (is_poison(sema, lv)) return val_void(sema);
  if (!value_is_type(lv, TYPE_KIND_INT)) {
    char tn[64];
    op_type_name(lv, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, n->length),
               "make: length must be an integer, got %s", tn);
    return val_void(sema);
  }
  const type_t *slice = type_slice_intern(sema->vm, ptr_type_base(pt));
  if (!slice) return val_void(sema);
  sema_type_register(sema, slice);
  return value_make_shadow(sema->vm, slice);
}
