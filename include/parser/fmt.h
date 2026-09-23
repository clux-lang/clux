#ifndef _H_CLUX_PARSER_FMT_
#define _H_CLUX_PARSER_FMT_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"

/**
 * clux 源码格式化（AST-based）。
 *
 * 策略：**基于语法格式化**——内部构建 lexer → parser → AST 全流程，按 AST
 * 结构递归渲染，而非在 token 流上做空白启发式。因此输出与源码书写风格
 * 无关：括号、缩进、空格全部由语法树决定（固定风格，无配置项）。
 *
 * 格式规则：
 *   - 缩进一律 4 空格（源中的 TAB 被替换）
 *   - `{` 跟随前行（K&R）；非空块 `{` 后换行、缩进 +1；空块 `{}` 紧凑
 *   - `;` 后换行（for 头部括号内的 `;` 除外，改为后跟空格）
 *   - `}` 后跟 `else` / `else if` 时写在同一行（`} else {`）
 *   - 运算符两侧空格；一元前缀运算符紧贴操作数（`-x`）
 *   - 函数调用 `name(` 紧贴；控制流关键字 `if (` / `while (` 须空格
 *   - 下标 `a[i]`、成员 `a.b`、解包 `a.!` 紧贴
 *   - 类型构造块 `.T{...}` 紧凑单行；构造字段 `.field = v` 规整
 *   - 元组类型 `<T1, T2>` 与闭包捕获 `func |x|` 定界符紧贴（元素间逗号+空格）
 *   - 注释按 token 位置挂载保留（行内注释跟随后续内容、独立行注释换行输出）
 *
 * 错误处理：源码有语法错误时，只格式化错误之前的 AST（parser recover_partial），
 * 错误 token 之后的内容丢弃。词法错误返回 NULL。
 *
 * 幂等性：格式化结果再格式化应保持不变（由测试保证）。
 *
 * @param alloc    输出缓冲的分配器（内部 arena/token 池亦由此分配）
 * @param src      源码文本（NUL 结尾）
 * @param len      源码字节长度
 * @param out_len  可选，取回结果长度（不含结尾 NUL）
 * @return 格式化后的 NUL 结尾字符串（alloc 分配，调用方 allocator_free）；
 *         失败（词法错误/OOM）返回 NULL
 */
char *fmt_format_source(allocator_t *alloc, const char *src, size_t len,
                        size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_FMT_ */
