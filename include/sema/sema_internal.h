/* sema 内部共享声明：主文件（生命周期 + walk/emit 编排）与克隆器
   （clone.c）之间的契约，不对外安装。 */
#ifndef _H_SEMA_INTERNAL_
#define _H_SEMA_INTERNAL_

#include "core/location.h"
#include "core/program.h"
#include "engine/context.h"
#include "parser/node.h"
#include <stddef.h>
#include <stdint.h>

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

/* ---- comptime if 剪枝（§2.5） ---- */

/* 剪枝决策条目（walk 收集）：条件 CTFE 求值后选中臂，emit 克隆时
   comptime if 整体替换为选中臂的克隆（arm 为源树 THEN/ELSE 块）；
   假臂无 else 时 arm=NULL，语句整体摘除（父块语句位跳过）。 */
typedef struct {
  node_t *stmt; /* 借用：原树 STMT_COMPTIME_IF */
  node_t *arm;  /* 借用：选中臂块，NULL=整体摘除 */
} sema_prune_t;

/* ---- 常量回填（§2.1 最小版） ---- */

/* 常量值：comptime var 的 CTFE 求值结果。整数按 magnitude+negative
   存（与 exec 字面量解码同构），type_name/suffix 均为静态串
   （suffix NULL 表示无后缀整数，类型默认 i32）。 */
typedef enum {
  SEMA_CONST_INT,
  SEMA_CONST_FLOAT,
  SEMA_CONST_BOOL,
} sema_const_kind_t;

typedef struct {
  sema_const_kind_t kind;
  int negative;          /* INT 符号（magnitude 恒非负存储） */
  uint64_t magnitude;    /* INT 幅值 */
  double float_value;    /* FLOAT */
  int bool_value;        /* BOOL */
  const char *type_name; /* "i32"/"f64"/"bool" 等静态串 */
  const char *suffix;    /* 字面量后缀（静态串），BOOL 为 NULL */
} sema_const_t;

/* 常量定义条目（walk 按源码序收集，engine allocator 持有，随 sema
   销毁）。求值失败的条目不入表（diag 已报，emit 被阻断）。 */
typedef struct {
  node_t *stmt;       /* 借用：原树 comptime var 语句（emit 摘除判定） */
  char *name;         /* 常量名（ctx allocator），引用点匹配键 */
  sema_const_t value; /* CTFE 求值结果 */
} sema_const_def_t;

/* 常量引用条目（per-use-site，与 instants 同模式）：名字命中常量表的
   LITERAL_IDENTIFIER 节点逐使用点登记（同名每个引用点各一条），
   emit 克隆时命中节点指针 → 渲染为字面量节点。 */
typedef struct {
  node_t *node;          /* 借用：原树引用点（LITERAL_IDENTIFIER） */
  sema_const_def_t *def; /* 指向常量表条目 */
} sema_const_use_t;

/* ctfe.c：常量表达式直推求值（字面量/一元负号/二元 + - * / %/引用
   已求值常量）。成功返回 0 写入 *out；不可求值报 ERROR diag 返回 -1。 */
int sema_ctfe_eval(clux_context_t *ctx,
                   vec_t *const_defs,
                   node_t *expr,
                   sema_const_t *out);

/* render.c：文本 → 全新 AST 节点（out allocator，文本经 intern 落
   program 常量表），location 沿 loc 的 line/col/filename，offset
   改指 intern 文本。 */
node_t *sema_render_text_node(allocator_t *out,
                              program_t *program,
                              node_kind_t kind,
                              const char *text,
                              const location_t *loc);

/* render.c：常量值 → 字面量节点（INT 补 i32 族后缀、FLOAT 补 f 族
   后缀与必要的小数点、BOOL 产 true/false 文本）。 */
node_t *sema_render_const(allocator_t *out,
                          program_t *program,
                          const sema_const_t *value,
                          const location_t *loc);

/* render.c：尾部剥类型后缀（与 parser/literal.c 同一白名单；exec 侧
   是 static 不可复用）。返回后缀静态串或 NULL，*num_len 写数字部分
   长度。 */
const char *
sema_render_strip_suffix(const char *text, size_t len, size_t *num_len);

/* 克隆器通道（逆向编排两条通道中的克隆器）：按 children vec 原序逐槽
   复制（NULL 槽保留下标），loc 按值浅拷，parent 由添加顺序重建。
   返回全新子树，分配全部来自 out。变换通道全部经 env 注入：
   - renames/skip：提升重编排（skip 内原树语句整条不进克隆）；
   - instants：命中使用点（EXPR_SUBSCRIPT）折叠为实例名节点（§2.3）；
   - consts：命中引用点（LITERAL_IDENTIFIER）折叠为常量字面量（§2.1）；
   - prunes：STMT_COMPTIME_IF 命中则替换为选中臂（§2.5，walk 已决策）；
   - STMT_VAR 后处理（§2.8）：TYPE 槽 NULL 时按 VALUE 字面量回填类型；
   - 结构改写（§2.4/2.6/2.7）：STMT_DEFER → 立即捕获闭包 var + 退出点
     LIFO 调用；STMT_FOREACH → 索引 for；STMT_COMPTIME_FOREACH（ITERABLE
     为 EXPR_TUPLE）→ 按元素逐份展开并替换迭代变量引用
     （subst_name/subst_src 是展开期的临时替换态，由展开函数进出配对置位）；
   - defer_counter：__deferN 编号，emit 全程单调递增保证模块内唯一。 */
typedef struct {
  allocator_t *out;
  program_t *program;
  const sema_rename_t *renames;
  size_t rename_count;
  vec_t *skip;    /* 借用：提升摘除集（原树 stmt 指针），可 NULL */
  vec_t *instants; /* 借用：sema->instants，可 NULL */
  vec_t *consts;   /* 借用：sema->const_uses，可 NULL */
  vec_t *prunes;   /* 借用：sema->prunes，可 NULL */
  const char *subst_name; /* §2.6 展开期迭代变量名 */
  node_t *subst_src;      /* §2.6 展开期当前元素源子树 */
  size_t defer_counter;   /* __deferN 编号 */
} sema_clone_env_t;

/* env 初始化：out/program 随 emit 创建；renames/skip 由 emit 按 stmt
   覆写；subst_* 初始 NULL。instants/consts/prunes 为 sema 整个 walk
   期的收集表（可空）。 */
void sema_clone_env_init(sema_clone_env_t *env,
                         allocator_t *out,
                         program_t *program,
                         vec_t *instants,
                         vec_t *consts,
                         vec_t *prunes);

node_t *sema_clone_node_renamed(node_t *node, sema_clone_env_t *env);

#endif
