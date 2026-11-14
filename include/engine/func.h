#ifndef _H_ENGINE_FUNC_
#define _H_ENGINE_FUNC_
#include "core/allocator.h"
#include "engine/interface.h"
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 闭包捕获项（白皮书 §3-declarations 捕获列表）：捕获列表是类型签名的
   一部分——func|a:i32|(i32,i32)->i32 与 func(i32,i32)->i32 是不同类型。
   捕获项的默认值属于函数体定义（AST），不参与类型判等。 */
typedef struct _clux_func_capture_t {
  const char *name;
  clux_type_t *type;
} clux_func_capture_t;

/* 创建 func 类型：捕获表（可为空）+ 参数类型序列（可为空）+ 返回类型
   （必填）。Data 布局：头部为指向函数体 AST 的指针（void*，偏移 0，
   借用不拥有），捕获字段从类型对齐处开始按声明序平铺（各自按自身
   对齐取偏移）。类型判等 = 捕获表 + 参数序列 + 返回类型全部结构
   一致。 */
clux_type_t *create_clux_func_type(allocator_t *allocator,
                                   const char *name,
                                   const clux_func_capture_t *captures,
                                   size_t capture_count,
                                   clux_type_t **params,
                                   size_t param_count,
                                   clux_type_t *return_type);
clux_vtable_t *get_clux_func_vtable(void);

size_t clux_func_capture_count(clux_type_t *type);
const clux_func_capture_t *clux_func_get_capture(clux_type_t *type, size_t idx);
const clux_func_capture_t *clux_func_find_capture(clux_type_t *type,
                                                  const char *name);
/* 捕获表起点（连续数组）：按名定位下标用 idx = found - table。 */
const clux_func_capture_t *clux_func_get_capture_table(clux_type_t *type);
/* 捕获字段在 Data 内的绝对偏移（头部之后平铺区）。 */
size_t clux_func_capture_offset(clux_type_t *type, size_t idx);
size_t clux_func_param_count(clux_type_t *type);
clux_type_t *clux_func_get_param(clux_type_t *type, size_t idx);
clux_type_t *clux_func_get_return_type(clux_type_t *type);
/* 头部（偏移 0）即函数体 AST 指针。 */
size_t clux_func_ast_offset(void);

/* 结构判等：捕获表 + 参数序列 + 返回类型按声明序一一对应即相等，
   类型名不参与（供 context 去重）。 */
bool clux_func_signature_equal(clux_type_t *self,
                               const clux_func_capture_t *captures,
                               size_t capture_count,
                               clux_type_t **params,
                               size_t param_count,
                               clux_type_t *return_type);

/* 读写函数体 AST 指针（借用，不拥有）：shadow 值无数据可读写。 */
void *clux_func_get_ast(clux_value_t *value);
void clux_func_set_ast(clux_value_t *value, void *ast);

#ifdef __cplusplus
}
#endif
#endif
