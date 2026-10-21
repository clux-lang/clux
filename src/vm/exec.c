#include "vm/exec.h"
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/type_error.h"
#include "vm/value_owncopy.h"  /* value_own_copy_into（构造字段写入） */
#include "vm/type.h"
#include "vm/type_ownership.h"
#include "vm/type_cfunc.h"
#include "vm/type_func.h"
#include "vm/type_array.h"
#include "vm/type_option.h"
#include "vm/type_enum.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/type_cunion.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/str_pool.h"
#include "vm/type_type.h"
#include "vm/type_interrupt.h"
#include "vm/bcode_function.h"
#include "vm/module.h"
#include "vm/ffi_lib.h"   /* M6 FFI：EXTERN_CALL */
#include "module/module_path.h" /* 运行期 IMPORT 路径重新规范化（VM 层） */
#include "core/panic.h"
#include "core/string.h"
#include "core/vec.h"

#include <string.h>
#include <stdio.h>  /* snprintf */
#include <stdlib.h> /* malloc/free：FFI 返回槽（struct 返回可能大于 16 字节） */


#include "exec_internal.h"


/* ================================================================ */
/* 指令分派表（opcode → 回调）                                         */
/* ================================================================ */

static const bcode_handler_t HANDLERS[] = {
    [BCODE_PUSH]           = op_push,
    [BCODE_STORE]          = op_store,
    [BCODE_STORE_REINIT]   = op_store_reinit,
    [BCODE_STORE_NIL]      = op_store_nil,
    [BCODE_PUSH_STR]       = op_push_str,
    [BCODE_PUSH_I8]        = op_push_i8,
    [BCODE_PUSH_I16]       = op_push_i16,
    [BCODE_PUSH_I32]       = op_push_i32,
    [BCODE_PUSH_I64]       = op_push_i64,
    [BCODE_PUSH_U8]        = op_push_u8,
    [BCODE_PUSH_U16]       = op_push_u16,
    [BCODE_PUSH_U32]       = op_push_u32,
    [BCODE_PUSH_U64]       = op_push_u64,
    [BCODE_PUSH_F32]       = op_push_f32,
    [BCODE_PUSH_F64]       = op_push_f64,
    [BCODE_PUSH_BOOL]      = op_push_bool,
    [BCODE_PUSH_VALUE]     = op_push_value,
    [BCODE_LOAD]           = op_load,
    [BCODE_LOAD_TYPE]      = op_load_type,
    [BCODE_LOAD_TYPE_OWNED] = op_load_type_owned,
    [BCODE_LOAD_FUNCTION]  = op_load_function,
    [BCODE_SET_TYPE_NAME]  = op_set_type_name,
    [BCODE_PUSH_UNDEFINED] = op_push_undefined,
    [BCODE_DEFINE]         = op_define,
    [BCODE_ADD]            = op_add,
    [BCODE_SUB]            = op_sub,
    [BCODE_MUL]            = op_mul,
    [BCODE_DIV]            = op_div,
    [BCODE_MOD]            = op_mod,
    [BCODE_EQ]             = op_eq,
    [BCODE_NE]             = op_ne,
    [BCODE_LT]             = op_lt,
    [BCODE_LE]             = op_le,
    [BCODE_GT]             = op_gt,
    [BCODE_GE]             = op_ge,
    [BCODE_AND]            = op_band,
    [BCODE_OR]             = op_bor,
    [BCODE_BXOR]           = op_bxor,
    [BCODE_SHL]            = op_shl,
    [BCODE_SHR]            = op_shr,
    [BCODE_NEG]            = op_neg,
    [BCODE_NOT]            = op_not,
    [BCODE_BNOT]           = op_bnot,
    [BCODE_CAST]           = op_cast,
    [BCODE_CREATE_CONST]   = op_create_const,
    [BCODE_CREATE_VOLATILE]= op_create_volatile,
    [BCODE_PUSH_CONST]     = op_push_const,
    [BCODE_PUSH_OWN]       = op_push_own,
    [BCODE_PUSH_REF]       = op_push_ref,
    [BCODE_PUSH_FATAL]     = op_push_fatal,
    [BCODE_PUSH_VOLATILE]  = op_push_volatile,
    [BCODE_PUSH_OPT]       = op_push_opt,
    [BCODE_PUSH_ENUM]      = op_push_enum,
    [BCODE_ENUM_VARIANT]   = op_enum_variant,
    [BCODE_MAKE_ENUM]      = op_make_enum,
    [BCODE_SET_TYPE]       = op_set_type,
    [BCODE_PUSH_FUNC_TYPE]   = op_push_func_type,
    [BCODE_FUNC_TYPE_PARAM]  = op_func_type_param,
    [BCODE_FUNC_TYPE_RETURN] = op_func_type_return,
    [BCODE_FUNC_TYPE_VARARG] = op_func_type_vararg,
    [BCODE_DEFINE_TYPE]      = op_define_type,
    [BCODE_SEAL]             = op_seal,
    [BCODE_PUSH_FUNCTION]  = op_push_function,
    [BCODE_BIND_FUNC]      = op_bind_func,
    [BCODE_SET_FUNC_NAME]  = op_set_func_name,
    [BCODE_SET_FUNC_CAPTURES] = op_set_func_captures,
    [BCODE_SET_CLOSURE]    = op_set_closure,
    [BCODE_MAKE_FUNCTION]  = op_make_function,
    [BCODE_CALL]           = op_call,
    [BCODE_RET]            = op_ret,
    [BCODE_JMP]            = op_jmp,
    [BCODE_JZ]             = op_jz,
    [BCODE_JNZ]            = op_jnz,
    [BCODE_PUSH_SCOPE]     = op_push_scope,
    [BCODE_POP_SCOPE]      = op_pop_scope,
    [BCODE_POP]            = op_pop,
    [BCODE_HALT]           = op_halt,
    [BCODE_PUSH_ARRAY]      = op_push_array,
    [BCODE_DEFINE_BOUND]    = op_define_bound,
    [BCODE_PUSH_STRUCT]     = op_push_struct,
    [BCODE_DEFINE_FIELD]    = op_define_field,
    [BCODE_ADD_EMBED]       = op_add_embed,
    [BCODE_PUSH_TUPLE]      = op_push_tuple,
    [BCODE_APPEND_ELEM]     = op_append_elem,
    [BCODE_PUSH_UNION]      = op_push_union,
    [BCODE_UNION_MEMBER]    = op_union_member,
    [BCODE_IS_TAG]          = op_is_tag,
    [BCODE_PUSH_CUNION]     = op_push_cunion,
    [BCODE_CONSTRUCT]       = op_construct,
    [BCODE_INDEX_GET]       = op_index_get,
    [BCODE_INDEX_SET]       = op_index_set,
    [BCODE_FIELD_GET]       = op_field_get,
    [BCODE_FIELD_SET]       = op_field_set,
    [BCODE_LENGTH]          = op_length,
    [BCODE_PUSH_OPT_NONE]   = op_push_opt_none,
    [BCODE_UNWRAP]          = op_unwrap,
    [BCODE_OPT_IS_NONE]     = op_opt_is_none,
    [BCODE_PUSH_PTR]        = op_push_ptr,
    [BCODE_NEW]             = op_new,
    [BCODE_PTR_GET]         = op_ptr_get,
    [BCODE_PTR_SET]         = op_ptr_set,
    [BCODE_ADDR]            = op_addr,
    [BCODE_DISPOSE]         = op_dispose,
    [BCODE_PUSH_SLICE]      = op_push_slice,
    [BCODE_SLICE]           = op_slice,
    [BCODE_MAKE]            = op_make,
    [BCODE_IMPORT]          = op_import,
    [BCODE_GET_MEMBER]      = op_get_member,
    [BCODE_EXTERN_CALL]     = op_extern_call,
    [BCODE_EXTERN_LOAD]     = op_extern_load,
    [BCODE_EXTERN_STORE]    = op_extern_store,
    [BCODE_DATA]            = op_data,
    [BCODE_LOAD_ATTR]       = op_load_attr,
    [BCODE_STORE_ATTR]      = op_store_attr,
    [BCODE_MARK_UNIQUE]     = op_mark_unique,
    [BCODE_MEMBER_CALL]     = op_member_call,
    [BCODE_FREE]            = op_free,
    [BCODE_FREE_RECURSIVE]  = op_free_recursive,
    [BCODE_TAKE_SLOT]       = op_take_slot,
    [BCODE_PUSH_USIZE]      = op_push_usize,
    [BCODE_PUSH_CFUNC_TYPE] = op_push_cfunc_type,
    [BCODE_MATERIALIZE]      = op_materialize,
};


/* ================================================================ */
/* 操作数栈（借用引用，归 scope 管理生命周期）                         */
/* ================================================================ */

void exec_stack_push(vm_t *vm, value_t *v) {
    vec_push(vm->stack, vm->alloc, v);
}

value_t *exec_stack_pop(vm_t *vm) {
    return (value_t *)vec_pop(vm->stack);
}

value_t *exec_stack_peek(const vm_t *vm, size_t offset) {
    size_t sp = vec_len(vm->stack);
    if (offset >= sp) panic("exec: stack underflow (sp=%zu, offset=%zu)", sp, offset);
    return (value_t *)vec_get(vm->stack, sp - 1 - offset);
}

/* ---- 终止 ---- */

value_t *op_halt(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    vm->halted = true;
    return NULL;
}

/* ================================================================ */
/* 驱动循环 + 主循环                                                  */
/* ================================================================ */

value_t *exec_drive(vm_t *vm, bytecode_t *bc, size_t start_pc) {
    vm->pc = start_pc;
    for (;;) {
        if (vm->halted) return NULL;  /* HALT 指令设 halted，正常退出 */
        bcode_op_t op = bcode_read_op(bc, &vm->pc);
        bcode_handler_t h = HANDLERS[op];
        if (!h) return value_make_error(vm, "exec: unimplemented opcode");
        value_t *r = h(vm, bc, &vm->pc);
        if (r) exec_stack_push(vm, r);
        if (value_is_error(vm, r)) return r;      /* 引擎级硬错误，停止 */
        if (value_is_interrupt(vm, r)) return r;  /* RET 哨兵，函数子循环捕获 */
    }
}

value_t *exec_run(vm_t *vm, bytecode_t *bc) {
    vm->bc     = bc;
    vm->halted = false;

    /* 清空操作数栈（借用引用，不释放） */
    while (!vec_is_empty(vm->stack)) vec_pop(vm->stack);

    /* clux 无顶层语句：只执行函数注册段，入口函数由调用方
       scope_lookup + value_call 显式触发 */
    return exec_drive(vm, bc, 0);
}
