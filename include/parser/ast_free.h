#ifndef _H_CLUX_PARSER_AST_FREE_
#define _H_CLUX_PARSER_AST_FREE_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "core/arena.h"

/**
 * AST_FREE：**释放表达式所指的堆块**——语义上是 `FREE <expr>`。
 *
 * 分两层，注意区分：
 * - **语义层**：`FREE <expr>`，`expr` 求值得到要释放的指针/值。
 * - **字节码层**：`FREE` 指令无操作数，只消费**栈顶**。
 *   所以 compiler 先编译 `expr` 压栈，再发 `BCODE_FREE`。
 *
 * 目前只有简单指针，sema 生成的 `expr` 就是变量引用（`AST_IDENT` →
 * `BCODE_PUSH <name>`），于是字节码是 `PUSH <name>` + `FREE`。
 * **将来复杂类型**由 sema 按类型展开（optional→判断 / 数组·切片→循环 /
 * tuple→get_index / struct→get_field），生成的仍是本节点 + 对应 expr
 * ——**compiler 不需要知道任何类型结构**。
 *
 * 为什么不在 compiler 里按名字拼 PUSH：那会把"释放什么"的构造逻辑锁在
 * compiler 层，未来按类型展开时无法复用，且与其它类型的释放形式冲突。
 *
 * `operand` 是操作数表达式**兄弟链**的链首（与 `ast_node_t::defers` 同款
 * 约定），用于组合式释放（如"释放这个结构体里的一批 own 槽位"）。
 * 单个释放时它就是一条链。
 *
 * 不出现在用户源码中——由 sema 在作用域退出点（块尾 / return /
 * break / continue）自动生成。调用方保证只对 **own** 目标生成。
 */
typedef struct {
    ast_node_t  base;
    ast_node_t *operand;  /* 操作数表达式兄弟链链首（释放它所指的块） */
    /* **递归释放**：自引用类型（链表/树）的类型展开在编译期无法穷尽——
       该边退化为运行期递归（`FREE_RECURSIVE`：按槽位所有权递归释放被指向值
       的 own 槽位，再释放块本体）。普通类型保持逐槽位静态展开（flat）。 */
    bool         recursive;
} ast_free_t;

static inline ast_node_t *ast_free_new(arena_t *arena, ast_node_t *operand) {
    ast_free_t *n = (ast_free_t *)arena_calloc(
        arena, 1, sizeof(ast_free_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_FREE;
    n->base.tok_begin = 0;  /* 编译器生成节点：tok_begin=0 标记 */
    n->base.tok_end   = 0;
    n->base.is_generated = true;  /* Pass 3b 不回走（见 stmt.c walk_stmt） */
    n->operand        = operand;
    return &n->base;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_FREE_ */
