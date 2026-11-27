#ifndef _H_PARSER_UNPARSE_
#define _H_PARSER_UNPARSE_
#include "core/allocator.h"
#include "core/program.h"
#include "core/string.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* AST → 源码文本序列化选项（白皮书无对应章节，纯工程设施）。
   当前为占位：本阶段不提供外部可配置的格式化规则，固定使用内置样式
   （2 空格缩进、块内每条语句独占一行、装饰器先于声明逐行输出）。
   字段留待后续扩展（缩进宽度、行宽、空行策略等）。 */
typedef struct unparse_options {
  int reserved;
} unparse_options_t;

/* 节点文本化：按 kind 递归分派，返回新建的 string_t（NUL 结尾，
   size 不含 NUL），所有权归调用方。options 可为 NULL（使用内置默认）。
   两个用途：
   1. 接在 parser 之后做格式化（AST → 规范化源码）；
   2. 接在 sema_emit 之后文本化整体检查 sema 的结果。
   容错节点（槽位 NULL）按可解析的最小形态输出。
   已知信息损失：无——泛型参数恒为 comptime（语法层面已删去
   `N: comptime T` 修饰词，见 src/parser/func.c），unparse 输出再解析
   可还原等价 AST。 */
string_t *node_unparse(node_t *node,
                       allocator_t *allocator,
                       const unparse_options_t *options);

/* program 文本化：遍历顶层声明集（CLUX_NODE_PROGRAM）。 */
string_t *program_unparse(program_t *program,
                          allocator_t *allocator,
                          const unparse_options_t *options);

#ifdef __cplusplus
}
#endif
#endif
