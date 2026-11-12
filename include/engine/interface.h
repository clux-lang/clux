#ifndef _H_ENGINE_INTERFACE_
#define _H_ENGINE_INTERFACE_
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _clux_context_t clux_context_t;
typedef struct _clux_value_t clux_value_t;
typedef struct _clux_vtable_t clux_vtable_t;
typedef struct _clux_type_t clux_type_t;
typedef struct _clux_scope_t clux_scope_t;
typedef enum _clux_binary_op_t {
  CLUX_OP_ADD,
  CLUX_OP_SUB,
  CLUX_OP_MUL,
  CLUX_OP_DIV,
  CLUX_OP_MOD,
  CLUX_OP_AND,
  CLUX_OP_OR,
  CLUX_OP_XOR,
  CLUX_OP_SHL,
  CLUX_OP_SHR,
} clux_binary_op_t;
typedef enum _clux_prefix_op_t {
  CLUX_OP_PLUS,
  CLUX_OP_NEGATIVE,
  CLUX_OP_NOT,
  CLUX_OP_LNOT,
} clux_prefix_op_t;
typedef enum _clux_postfix_op_t {
  CLUX_OP_DEREF,
  CLUX_OP_REF,
  CLUX_OP_TRY,
  CLUX_OP_ASSERT,
} clux_postfix_op_t;
typedef enum _clux_type_kind_t {
  CLUX_TYPE_ERROR,
  CLUX_TYPE_I8,
  CLUX_TYPE_I16,
  CLUX_TYPE_I32,
  CLUX_TYPE_I64,
  CLUX_TYPE_U8,
  CLUX_TYPE_U16,
  CLUX_TYPE_U32,
  CLUX_TYPE_U64,
  CLUX_TYPE_F32,
  CLUX_TYPE_F64,
  CLUX_TYPE_BOOL,
} clux_type_kind_t;
typedef clux_value_t *(*clux_vt_binary_fn_t)(clux_context_t *,
                                             clux_value_t *,
                                             clux_value_t *);
typedef clux_value_t *(*clux_vt_prefix_fn_t)(clux_context_t *, clux_value_t *);
typedef clux_value_t *(*clux_vt_postfix_fn_t)(clux_context_t *, clux_value_t *);
typedef clux_value_t *(*clux_vt_get_field_fn_t)(clux_context_t *,
                                                clux_value_t *,
                                                const char *);
typedef clux_value_t *(*clux_vt_set_field_fn_t)(clux_context_t *,
                                                clux_value_t *,
                                                const char *,
                                                clux_value_t *);
typedef clux_value_t *(*clux_vt_get_index_fn_t)(clux_context_t *,
                                                clux_value_t *,
                                                clux_value_t *);
typedef clux_value_t *(*clux_vt_set_index_fn_t)(clux_context_t *,
                                                clux_value_t *,
                                                clux_value_t *,
                                                clux_value_t *);
typedef clux_value_t *(*clux_vt_get_len_fn_t)(clux_context_t *, clux_value_t *);
typedef clux_value_t *(*clux_vt_invoke_fn_t)(clux_context_t *,
                                             clux_value_t *,
                                             size_t,
                                             clux_value_t *[]);
typedef clux_value_t *(*clux_vt_slice_fn_t)(clux_context_t *,
                                            clux_value_t *,
                                            clux_value_t *,
                                            clux_value_t *);
typedef clux_value_t *(*clux_vt_safe_cast_fn_t)(clux_context_t *,
                                                clux_value_t *,
                                                clux_type_t *);
typedef clux_value_t *(*clux_vt_cast_fn_t)(clux_context_t *,
                                           clux_value_t *,
                                           clux_type_t *);
struct _clux_vtable_t {
  clux_vt_binary_fn_t add;
  clux_vt_binary_fn_t sub;
  clux_vt_binary_fn_t mul;
  clux_vt_binary_fn_t div;
  clux_vt_binary_fn_t mod;
  clux_vt_binary_fn_t shl;
  clux_vt_binary_fn_t shr;
  clux_vt_binary_fn_t bit_and;
  clux_vt_binary_fn_t bit_or;
  clux_vt_binary_fn_t bit_xor;
  clux_vt_prefix_fn_t plus;
  clux_vt_prefix_fn_t negative;
  clux_vt_prefix_fn_t bit_not;
  clux_vt_prefix_fn_t logic_not;
  clux_vt_safe_cast_fn_t safe_cast;
  clux_vt_cast_fn_t cast;
};
#ifdef __cplusplus
}
#endif
#endif
