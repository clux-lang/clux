#ifndef _H_CLUX_VM_EXEC_INTERNAL_
#define _H_CLUX_VM_EXEC_INTERNAL_
/* 内部头：exec.c 拆分后的跨文件声明（VM 私有，不属于公共 API）。 */
#include "vm/exec.h"

#ifdef __cplusplus
extern "C" {
#endif


value_t *op_add_embed(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_addr(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_append_elem(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_bind_func(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_bnot(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_call(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_cast(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_construct(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_create_const(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_create_volatile(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_data(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_define(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_define_bound(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_define_field(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_define_type(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_dispose(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_enum_variant(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_extern_call(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_extern_load(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_extern_store(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_field_get(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_field_set(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_free(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_free_recursive(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_func_type_param(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_func_type_return(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_func_type_vararg(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_get_member(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_halt(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_import(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_index_get(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_index_set(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_is_tag(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_jmp(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_jnz(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_jz(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_length(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_load(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_load_attr(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_load_function(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_load_type(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_load_type_owned(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_make(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_make_enum(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_make_function(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_mark_unique(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_materialize(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_member_call(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_neg(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_new(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_not(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_opt_is_none(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_pop(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_pop_scope(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_ptr_get(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_ptr_set(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_array(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_bool(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_cfunc_type(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_const(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_cunion(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_enum(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_f32(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_f64(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_fatal(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_func_type(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_function(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_i16(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_i32(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_i64(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_i8(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_opt(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_opt_none(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_own(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_ptr(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_ref(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_scope(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_slice(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_str(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_struct(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_tuple(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_u16(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_u32(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_u64(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_u8(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_undefined(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_union(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_usize(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_value(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_push_volatile(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_ret(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_seal(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_set_closure(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_set_func_captures(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_set_func_name(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_set_type(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_set_type_name(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_slice(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_store(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_store_attr(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_store_nil(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_store_reinit(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_take_slot(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_union_member(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_unwrap(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_add(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_sub(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_mul(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_div(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_mod(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_eq(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_ne(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_lt(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_le(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_gt(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_ge(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_band(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_bor(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_bxor(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_shl(vm_t *vm, bytecode_t *bc, size_t *pc);
value_t *op_shr(vm_t *vm, bytecode_t *bc, size_t *pc);

/* 守卫：被污染的值（错误已报告）→ 直接返回 void shadow。 */
#define EX_GUARD(sema, v)                                                     \
  do {                                                                        \
    if (is_poison((sema), (v))) return val_void((sema));                       \
  } while (0)

#ifdef __cplusplus
}
#endif
#endif
