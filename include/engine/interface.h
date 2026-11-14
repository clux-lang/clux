#ifndef _H_ENGINE_INTERFACE_
#define _H_ENGINE_INTERFACE_
#include <stdbool.h>
#include <stddef.h>
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
  CLUX_OP_EQ,
  CLUX_OP_NE,
  CLUX_OP_LT,
  CLUX_OP_LE,
  CLUX_OP_GT,
  CLUX_OP_GE,
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
  CLUX_TYPE_STR,
  CLUX_TYPE_PTR,
  CLUX_TYPE_ISIZE,
  CLUX_TYPE_USIZE,
  CLUX_TYPE_CONST,
  CLUX_TYPE_VOLATILE,
  CLUX_TYPE_SLICE,
  CLUX_TYPE_ARRAY,
  CLUX_TYPE_OPTIONAL,
  CLUX_TYPE_CUNION,
} clux_type_kind_t;
typedef clux_value_t *(*clux_vt_binary_fn_t)(clux_context_t *,
                                             clux_value_t *,
                                             clux_value_t *);
typedef clux_value_t *(*clux_vt_prefix_fn_t)(clux_context_t *, clux_value_t *);
typedef clux_value_t *(*clux_vt_postfix_fn_t)(clux_context_t *, clux_value_t *);
/* field 是具名成员的"借引用"读原语（x.field）：返回成员类型的读写双向
   view（内存共享、天然携带引用属性），读取与写入均由通用 assign 完成
   （写穿 view，赋值给具名变量时经 assign 的 memcpy 自动失去引用属性）。
   只有具名成员的类型（当前为 cunion）绑定该槽位。 */
typedef clux_value_t *(*clux_vt_get_field_fn_t)(clux_context_t *,
                                                clux_value_t *,
                                                const char *);
/* deref 与 index 是两个语义清晰的"借引用"读原语：deref 是指针解引用
   （*p），index 是数组/切片下标取项。两者均返回 base 类型的 view
   （内存共享、天然携带引用属性），写入由通用 assign 写穿 view 完成，
   赋值给具名变量时也经 assign 的 memcpy 自动失去引用属性。 */
typedef clux_value_t *(*clux_vt_deref_fn_t)(clux_context_t *, clux_value_t *);
typedef clux_value_t *(*clux_vt_index_fn_t)(clux_context_t *,
                                            clux_value_t *,
                                            clux_value_t *);
/* is_none 是 optional 的判空查询（内置函数 #is_none）：
   返回 bool 值，none 为 true。只有 optional 绑定该槽位，
   其余类型由 context 兜底报不支持。 */
typedef clux_value_t *(*clux_vt_is_none_fn_t)(clux_context_t *, clux_value_t *);
/* take 是 optional 的断言取值（内置函数 #take）：some 返回载荷 base
   类型的借引用 view，none 是运行期 error；`.!` 是编译为 #take 的
   语法糖。只有 optional 绑定该槽位，其余类型由 context 兜底报不支持。 */
typedef clux_value_t *(*clux_vt_take_fn_t)(clux_context_t *, clux_value_t *);
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
/* 类型判等与兼容由类型自身的 vtable 回答，签名不带 context：这是纯类型
   层知识（ptr 需要递归比较 base）。 */
typedef bool (*clux_vt_type_equal_fn_t)(clux_type_t *self, clux_type_t *other);
typedef bool (*clux_vt_type_extends_fn_t)(clux_type_t *self, clux_type_t *base);
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
  clux_vt_binary_fn_t eq;
  clux_vt_binary_fn_t ne;
  clux_vt_binary_fn_t lt;
  clux_vt_binary_fn_t le;
  clux_vt_binary_fn_t gt;
  clux_vt_binary_fn_t ge;
  clux_vt_prefix_fn_t plus;
  clux_vt_prefix_fn_t negative;
  clux_vt_prefix_fn_t bit_not;
  clux_vt_prefix_fn_t logic_not;
  clux_vt_safe_cast_fn_t safe_cast;
  clux_vt_cast_fn_t cast;
  clux_vt_deref_fn_t deref;
  clux_vt_index_fn_t index;
  clux_vt_is_none_fn_t is_none;
  clux_vt_take_fn_t take;
  clux_vt_get_field_fn_t get_field;
  clux_vt_type_equal_fn_t type_equal;
  clux_vt_type_extends_fn_t type_extends;
};
#ifdef __cplusplus
}
#endif
#endif
