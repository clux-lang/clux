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
  CLUX_TYPE_UNION,
  CLUX_TYPE_FUNC,
  CLUX_TYPE_STRUCT,
  CLUX_TYPE_TYPE,
  CLUX_TYPE_TYPE_TEMPLATE,
  CLUX_TYPE_FUNC_TEMPLATE,
  /* void（枚举尾部追加，不打断既有序号）：只作为无返回值的返回类型，
     无值无运算——vtable 仅提供类型判等（白皮书 §2）。永不 seal、
     天然不完整：类型擦除边界是 *void（C 风格吸收，白皮书 §2.1）。 */
  CLUX_TYPE_VOID,
  /* tuple（枚举尾部追加，不打断既有序号）：异构元素按声明序平铺的
     值类型（白皮书 §2.6），元素按位置访问，判等按字节比较（§5.5）。
     空元组 <> 合法（size 0、align 1）。 */
  CLUX_TYPE_TUPLE,
  /* return signal（枚举尾部追加）：函数体 return 的展开通道——return
     语句产出信号值（载荷为返回值），语句层原样上抛，唯一消费点是
     调用表达式的栈帧边界；逃逸到调用边界之外即 "return outside of a
     function"。引擎级控制值（与 error 同级），不属于类型系统的用户
     可见面。 */
  CLUX_TYPE_RETURN_SIGNAL,
  /* enum（枚举尾部追加，不打断既有序号）：具名整数常量集（白皮书
     §3.6），布局与底层整数类型一致，variant 值是编译期整数常量。 */
  CLUX_TYPE_ENUM,
} clux_type_kind_t;
typedef clux_value_t *(*clux_vt_binary_fn_t)(clux_context_t *,
                                             clux_value_t *,
                                             clux_value_t *);
typedef clux_value_t *(*clux_vt_prefix_fn_t)(clux_context_t *, clux_value_t *);
typedef clux_value_t *(*clux_vt_postfix_fn_t)(clux_context_t *, clux_value_t *);
/* field 是具名成员的"借引用"读原语（x.field）：返回成员类型的读写双向
   view（内存共享、天然携带引用属性），读取与写入均由通用 assign 完成
   （写穿 view，赋值给具名变量时经 assign 的 memcpy 自动失去引用属性）。
   只有具名成员的类型（当前为 cunion、tag union 与 struct）绑定该槽位。 */
typedef clux_value_t *(*clux_vt_field_fn_t)(clux_context_t *,
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
/* is_member 是 tag union 的成员判定（is 运算）：判断 value 当前 tag 是否
   为具名成员，返回 bool 值；未知成员报语义化 error。只有 tag union 绑定
   该槽位，其余类型由 context 兜底报不支持。 */
typedef clux_value_t *(*clux_vt_is_member_fn_t)(clux_context_t *,
                                                clux_value_t *,
                                                const char *);
/* invoke 是函数值的调用原语（f(args)）：按签名校验实参个数与类型，
   返回调用结果。shadow 函数产出返回类型的 shadow；实际执行（AST 体）
   由可执行环境接线，未接线是运行期 error。只有 func 绑定该槽位。 */
typedef clux_value_t *(*clux_vt_invoke_fn_t)(clux_context_t *,
                                             clux_value_t *,
                                             size_t,
                                             clux_value_t *[]);
typedef clux_value_t *(*clux_vt_get_len_fn_t)(clux_context_t *, clux_value_t *);
/* slice 是数组/切片的切片原语（a[lo:hi]）：lo/hi 为 NULL 表示开放端点
   （缺省 0 / 长度），返回元素类型的切片值（[]T 胖指针，借用 base 内存）。
   只有数组与切片绑定该槽位，其余类型由 context 兜底报不支持。 */
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
/* 值级 extends 是值的兼容判定（如 type value 之间的派生关系查询），
   签名不带 context：这是纯数据层知识，由值 data 携带的内容回答。 */
typedef bool (*clux_vt_value_extends_fn_t)(clux_value_t *self,
                                           clux_value_t *base);
/* delete_meta 在 delete_clux_type 释放 meta 本体前分派：释放 meta 持有的
   堆资源（容器、成员块、自有字符串）。NULL 表示 meta 无自有资源
   （连体单块布局，释放 meta 本体即整块释放）。meta 本体本身始终由
   delete_clux_type 统一释放，槽位实现只负责内部资源。 */
typedef void (*clux_vt_delete_meta_fn_t)(clux_type_t *self);
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
  clux_vt_slice_fn_t slice;
  clux_vt_is_none_fn_t is_none;
  clux_vt_take_fn_t take;
  clux_vt_field_fn_t field;
  clux_vt_is_member_fn_t is_member;
  clux_vt_invoke_fn_t invoke;
  clux_vt_type_equal_fn_t type_equal;
  clux_vt_type_extends_fn_t type_extends;
  clux_vt_value_extends_fn_t value_extends;
  clux_vt_delete_meta_fn_t delete_meta;
};
#ifdef __cplusplus
}
#endif
#endif
