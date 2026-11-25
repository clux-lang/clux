#ifndef _H_CORE_PROGRAM_
#define _H_CORE_PROGRAM_
#include "core/allocator.h"
#include "core/string.h"
#include "core/vec.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 前置声明（完整定义在 parser/node.h）；core 头不反向依赖 parser。 */
typedef struct _node_t node_t;

/* sema → compiler 的中性交接物（docs/sema-design.md §7.2）。
   一个 module 一个 program_t，所有权归调用方，内存全部来自构造时
   传入的 allocator（sema 侧为 out_allocator，与 engine 无关）。
   - decls：本 module 的 PROGRAM 顶层声明集，所有权由 set 转入；
   - strings：常量字符串表（intern 去重，emit 渲染的稳定归宿）。 */
typedef struct _program_t program_t;

program_t *create_program(allocator_t *allocator);
void delete_program(program_t **program);

/* 接管 decls（所有权转移；原值先释放）。 */
void program_set_decls(program_t *self, node_t *decls);
node_t *program_get_decls(program_t *self);

/* 名字拷贝进自有 string_t；传 NULL 清空。 */
void program_set_module_name(program_t *self, const char *name);
const char *program_get_module_name(program_t *self);
void program_set_source_path(program_t *self, const char *path);
const char *program_get_source_path(program_t *self);

/* 常量字符串表：去重登记，返回稳定借用（NUL 结尾，program 存活期内
   有效）。同内容重复登记返回同一指针。 */
const char *
program_intern_string(program_t *self, const char *data, size_t len);
vec_t *program_get_strings(program_t *self);
#ifdef __cplusplus
}
#endif
#endif
