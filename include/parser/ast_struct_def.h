#ifndef _H_CLUX_PARSER_AST_STRUCT_DEF_
#define _H_CLUX_PARSER_AST_STRUCT_DEF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/parser.h"
#include <stdbool.h>
#include <stdint.h>

/* 结构体定义节点：struct Name { field: type; ... }
 *
 * fields 为 AST_STRUCT_FIELD 兄弟链（name: type，分号分隔，与 enum variant
 * 的逗号分隔不同——见 M2 设计 §2）。字段类型表达式（AST_IDENT/类型修饰等）
 * sema 阶段折叠为类型引用。
 */
typedef struct {
    ast_node_t  base;
    strslice_t  name;            /* 结构体名 */
    ast_node_t *fields;          /* AST_STRUCT_FIELD 兄弟链 */
    ast_node_t *fields_last;
    /* ---- 命名空间成员（语法糖）----
     * 结构体体内还可声明：
     *     var  data = 123;                         // 静态属性
     *     func getId(self: ref *const T): i32 {…}   // 方法（第一个参数即接收者）
     * 二者**本质都是模块级全局声明**（变量 / 函数），只是以
     * `<结构体名>::<成员名>` 的命名空间方式访问。`members` 挂 AST_VAR_DEF /
     * AST_FUNC_DEF（名字是**未限定**的成员名）；
     * `ast_expand_struct_members`（编译流水线在 parse 之后调用）把它们改名为
     * `T::name` 并追加进程序声明表 → 此后 sema / compiler / 运行期完全走既有
     * 全局路径，无需任何新机制。fmt 直接渲染 members（**不**走展开，避免重复）。 */
    ast_node_t *members;
    ast_node_t *members_last;
    uint32_t    type_id;         /* sema 登记的类型 id（TYPE_ID_PROGRAM_BASE+idx；
                                    compiler 顶层名字绑定 LOAD_TYPE 用） */
} ast_struct_def_t;

/* 单个字段声明（独立节点，兄弟链挂接在 ast_struct_def_t::fields） */
typedef struct {
    ast_node_t  base;    /* kind = AST_STRUCT_FIELD */
    strslice_t  name;    /* 字段名 */
    ast_node_t *type;    /* 字段类型表达式 */
} ast_struct_field_t;

static inline ast_node_t *ast_struct_def_new(arena_t *arena,
                                             uint32_t tok_begin,
                                             uint32_t tok_end) {
    ast_struct_def_t *n = (ast_struct_def_t *)arena_calloc(
        arena, 1, sizeof(ast_struct_def_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_STRUCT_DEF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

static inline ast_node_t *ast_struct_field_new(arena_t *arena,
                                               uint32_t tok_begin,
                                               uint32_t tok_end) {
    ast_struct_field_t *n = (ast_struct_field_t *)arena_calloc(
        arena, 1, sizeof(ast_struct_field_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_STRUCT_FIELD;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析结构体定义 struct Name { field: type; ... } */
ast_node_t *parse_struct_def(parser_t *p);

/** 把结构体的命名空间成员（var/func）展开成模块级全局声明：
 *  名字改写为 `<结构体名>::<成员名>` 并**追加到 `prog->funcs` 尾部**。
 *  编译流水线在 parse 之后、sema 之前调用（见 driver.c 的 compile_module）；
 *  fmt 路径不调用（成员已随结构体渲染）。*/
void ast_expand_struct_members(arena_t *arena, ast_node_t *prog);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_STRUCT_DEF_ */
