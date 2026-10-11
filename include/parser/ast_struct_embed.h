#ifndef _H_CLUX_PARSER_AST_STRUCT_EMBED_
#define _H_CLUX_PARSER_AST_STRUCT_EMBED_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/arena.h"
#include "core/strslice.h"
#include "parser/ast_node.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 结构体**嵌入**声明节点：struct Test { ... Base; }
 *
 * 语义（"结构体组合"，type 层）：
 *   - **纯语法节点**——parser 只记录"哪个名字"与"出现在自己第几个字段之前"，
 *     **不**在 AST 里拼接 Base 的字段（用户明确要求：组合发生在 sema 后阶段，
 *     遍历 Base 的 **type** 复制字段——这样未来的装饰器在 sema 建 type 后
 *     对 Base 类型的修改会被一并继承）。
 *   - 组合运算（sema，**目标类型 seal 之前**的开放窗口）把 Base 类型此刻的
 *     字段表（名字 + 类型 + 顺序）**插入**到 Test 开放类型的 field_index 位置，
 *     登记嵌入表，然后只 seal 一次、按 C 对齐规则定型布局。因此 `...Base` 在
 *     最前面 ⟺ Test 的字段前缀 == Base 的字段 → 既有的结构体指针向上转换
 *     （struct_type_prefix_compatible）自然生效；前面还有字段时前缀不成立 →
 *     `t.getId()` 自然被拒（无需位置特判）。
 *   - 成员（`var` 静态属性 / `func` 方法）**不复制**：Base 的成员只有一份
 *     （`Base::name`），Test 通过嵌入链**继承名字查找**（见 sema 的
 *     `sema_struct_member_lookup`）。
 *   - fmt 必须按 field_index 把 `...Base;` 渲染回原位（位置改变会改变语义）。
 */
typedef struct {
    ast_node_t base;        /* kind = AST_STRUCT_EMBED */
    strslice_t name;        /* 被嵌入的结构体名（借用源 token 切片） */
    size_t     field_index; /* 该 `...Name;` 出现在**自己**第几个字段之前
                               （0 = 在所有字段之前；== 自有字段数 = 在最后） */
} ast_struct_embed_t;

static inline ast_node_t *ast_struct_embed_new(arena_t *arena,
                                               uint32_t tok_begin,
                                               uint32_t tok_end) {
    ast_struct_embed_t *n = (ast_struct_embed_t *)arena_calloc(
        arena, 1, sizeof(ast_struct_embed_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_STRUCT_EMBED;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return (ast_node_t *)n;
}

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_STRUCT_EMBED_ */
