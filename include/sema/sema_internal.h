/* sema 内部共享声明：主文件（生命周期 + walk/emit 编排）与克隆器
   （clone.c）之间的契约，不对外安装。 */
#ifndef _H_SEMA_INTERNAL_
#define _H_SEMA_INTERNAL_

#include "core/location.h"
#include "core/program.h"
#include "parser/node.h"

/* 引用点改写条目：原 SYMBOL 文本 from → 提升名 to（to 为 NUL 结尾串，
   克隆时按 to 的内容 intern 进 program_t 的常量表）。 */
typedef struct {
  const char *from;
  const char *to;
} sema_rename_t;

/* 提升条目（walk 收集，engine allocator 持有，随 sema 销毁）。 */
typedef struct {
  node_t *stmt;  /* 借用：原 AST 中的声明语句（emit 时从中克隆） */
  node_t *owner; /* 借用：所属顶层函数 stmt（rename 表按 owner 分组） */
  char *name;    /* 原名（ctx allocator），改写匹配键 */
  char *mangled; /* 提升名（ctx allocator），如 f$Foo */
} sema_hoist_t;

/* 实例化条目（§2.3 最小版，walk 收集）：泛型类型使用点（EXPR_SUBSCRIPT）
   → 合成实例名。emit 克隆时命中节点折叠为单个名字节点（location 指向
   intern 实例名，line/col/filename 沿 base 的值）。实例体的深拷贝代换
   是 Phase 5（§2.3 完整版）。 */
typedef struct {
  node_t *node;        /* 借用：原 AST 的使用点（EXPR_SUBSCRIPT） */
  char *name;          /* 实例名（ctx allocator），如 List$i32 */
  location_t base_loc; /* base 名字节的 location（位置字段模板） */
} sema_instant_t;

/* 克隆器通道（逆向编排两条通道中的克隆器）：按 children vec 原序逐槽
   复制（NULL 槽保留下标），loc 按值浅拷，parent 由添加顺序重建。
   返回全新子树，分配全部来自 out。
   - renames：SYMBOL/IDENTIFIER 文本命中即改指 to 的 intern 文本；
   - skip：集合内的原树语句整条不进入克隆（提升声明原位置摘除）；
   - instants：命中使用点（EXPR_SUBSCRIPT）折叠为单个实例名节点
     （§2.3 最小版），折叠需 intern，因此依赖 program。
   三者均可为空，纯克隆即全传空。 */
node_t *sema_clone_node_renamed(node_t *node,
                                allocator_t *out,
                                program_t *program,
                                const sema_rename_t *renames,
                                size_t rename_count,
                                vec_t *skip,
                                vec_t *instants);

#endif
