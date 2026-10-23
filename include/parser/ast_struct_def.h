#ifndef _H_CLUX_PARSER_AST_STRUCT_DEF_
#define _H_CLUX_PARSER_AST_STRUCT_DEF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_generic_param.h"
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
/* bitfield 字段元数据（偏移从 LSB 起，连续位流）。 */
typedef struct ast_bitfield_field {
    strslice_t name;
    uint32_t   offset;
    uint32_t   width;
} ast_bitfield_field_t;

typedef struct {
    ast_node_t  base;
    strslice_t  name;            /* 结构体名 */
    ast_node_t *fields;          /* AST_STRUCT_FIELD 兄弟链 */
    ast_node_t *fields_last;
    /* ---- 命名空间成员（运行时成员模型）----
     * 结构体体内还可声明：
     *     var  data = 123;                         // 静态属性
     *     func getId(self: ref *const T): i32 {…}   // 方法（第一个参数即接收者）
     * 成员的生命周期由 struct_type_t 运行时拥有（member_scope + attributes +
     * methods），不再展开为模块级全局声明。`members` 挂 AST_VAR_DEF /
     * AST_FUNC_DEF（名字是**未限定**的成员名）；sema 在类型 seal 之后注册到
     * struct_type 的成员容器中。fmt 直接渲染 members。 */
    ast_node_t *members;
    ast_node_t *members_last;
    /* ---- 结构体嵌入（`...Base;`，纯语法）----
     * AST_STRUCT_EMBED 兄弟链，声明序。**只记录位置与名字**——字段组合由 sema
     * 在**目标类型 seal 之前**按 `field_index` 完成（遍历 Base 已 sealed 的
     * type 复制字段），本列表**不**参与字段拼接（见 ast_struct_embed.h）。 */
    ast_node_t *embeds;
    ast_node_t *embeds_last;
    /* 组合状态（sema 用，transient）：
     * composing：嵌入环检测 DFS 的**在途标记**——A→B→A 在第二次进入 A 时命中它。
     *   作用域限定在一次 DFS 路径内，进入/退出成对置位/清零。
     * cycle_reported：本结构体所在嵌入环已经报过"循环嵌入"。sema 的试跑轮会反复
     *   调用同一个 pass，没有它会重复打印同一组诊断；只增不清（一次诊断足够）。 */
    bool        composing;
    bool        cycle_reported;
    uint32_t    type_id;         /* sema 登记的类型 id（TYPE_ID_PROGRAM_BASE+idx；
                                    compiler 顶层名字绑定 LOAD_TYPE 用） */
    /* M7 泛型参数表（count == 0 = 普通结构体）。 */
    ast_generic_params_t generic;
    /* M7 §31.2：`struct W implements IWriter[W], ISerializable { ... }`
       的接口类型表达式链（AST_IDENT / AST_INDEX），声明序。 */
    ast_node_t *implements;
    ast_node_t *implements_last;

    /* ---- bitfield（sema/compiler 用） ----
       位域在语法上被改写成一个单字段 struct（隐藏 `__bits` 数组），
       这些元数据描述逻辑字段。字段按声明顺序从 LSB 连续排布，允许跨
       底层无符号字的边界。 */
    bool        is_bitfield;
    ast_node_t *bitfield_underlying; /* 底层无符号类型表达式 */
    uint32_t    bitfield_word_bits;  /* u8/u16/u32/u64 的位宽 */
    uint32_t    bitfield_total_bits; /* 所有字段宽度之和 */
    uint32_t    bitfield_word_count; /* 隐藏数组元素数 */
    size_t      bitfield_field_count;
    ast_bitfield_field_t *bitfield_fields;
    ast_node_t *decorators;
    ast_node_t *metadata_decorators;
    uint32_t    bitfield_underlying_type_id; /* sema 填 */
    uint32_t    bitfield_storage_type_id;    /* sema 填：隐藏数组类型 id */
} ast_struct_def_t;

/* 单个字段声明（独立节点，兄弟链挂接在 ast_struct_def_t::fields） */
typedef struct ast_struct_field {
    ast_node_t  base;    /* kind = AST_STRUCT_FIELD */
    strslice_t  name;    /* 字段名 */
    ast_node_t *type;    /* 字段类型表达式 */
    ast_node_t *decorators;
    ast_node_t *metadata_decorators;
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

/** 解析 bitfield Name: u32 { field: width; ... } */
ast_node_t *parse_bitfield_def(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_STRUCT_DEF_ */
