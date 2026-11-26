/* sema 内部共享声明：主文件（生命周期 + walk/emit 编排）与克隆器
   （clone.c）之间的契约，不对外安装。 */
#ifndef _H_SEMA_INTERNAL_
#define _H_SEMA_INTERNAL_

#include "core/location.h"
#include "core/program.h"
#include "engine/context.h"
#include "engine/module.h"
#include "parser/node.h"
#include <stddef.h>
#include <stdint.h>

#include "sema/sema.h"

/* sema 本体定义（主文件生命周期 + check.c 共享）。字段语义见
   sema_create / docs/sema-design.md。 */
struct _clux_sema_t {
  clux_context_t *ctx;        /* 借用：engine 侧全部状态（scope/module/diag） */
  allocator_t *out_allocator; /* 借用：emit 产物（program_t）的唯一分配来源 */
  clux_module_t *module;      /* 借用：ctx 的 modules 注册表持有 */
  int walked;                 /* 0 未走 / 1 已走（walk 只允许一次） */
  vec_t *hoists;              /* sema_hoist_t*，engine allocator 持有 */
  vec_t *instants;            /* sema_instant_t*，模块级实例化表（§2.3） */
  vec_t *generic_defs;        /* sema_generic_def_t*，泛型名→定义（§2.3） */
  vec_t *instance_bodies;     /* sema_instance_body_t*，实例体表（§2.3） */
  program_t *inst_program; /* 物化期 intern 文本池（§2.3 P11）：walk 期
                                 物化实例体新造文本 intern 于此（仅在有实
                                 例体时创建），物化体的新造名 location 借
                                 其缓冲——与产物借 parser 源码缓冲同构：
                                 调用方先消费产物后销毁 sema */
  size_t inst_defer_counter; /* 物化循环已消耗的 __deferN 计数（§2.3
                                 P11）：emit env 初始化后预置起点，防物化
                                 体固化名与 emit 主循环产物撞模块级名 */
  vec_t *const_defs;   /* sema_const_def_t*，顶层 comptime 常量表（§2.1） */
  vec_t *const_uses;   /* sema_const_use_t*，常量引用点表（§2.1） */
  vec_t *prunes;       /* sema_prune_t*，comptime if 剪枝决策表（§2.5） */
  vec_t *anons;        /* sema_anon_t*，匿名构造表（§2.9） */
  vec_t *methods;      /* sema_method_t*，struct 方法表（§2.10） */
  vec_t *type_results; /* sema_type_result_t*，类型结果表（§8.5 第一版） */
  node_t *ast;         /* 借用：walk 入参 PROGRAM（emit 只读） */
  clux_type_t *return_type; /* 借用：当前函数签名返回类型上下文（§8.8 P9
                               return 匹配；NULL=不在函数体/void/未定局），
                               check_func_decl 进出体保存恢复配对 */
  vec_t *dfa_entries; /* sema_dfa_entry_t*，局部 var 赋值状态栈（§8.6）；
                          函数体帧内活动（dfa_frames>0），帧进出整体换栈 */
  size_t dfa_frames; /* 函数体帧嵌套层数：>0 时 DFA 消费点生效（模块级
                          语句与预扫描不启用，§8.6 只管函数体局部 var） */
  vec_t *dfa_marks; /* size_t 水位标记栈：scope 进出配对截断 entries */
};

/* §8.6 DFA 条目：局部 var 的赋值状态。name 归 ctx allocator（条目
   死亡即释放）；assigned=1 已赋 / 0 未赋（含 var x: T; 与 = undefined，
   两者运行期同为 TDZ 起点）。查找自栈顶向下取首个同名条目（内层
   遮蔽外层，与 scope 语义对齐；截断保证同级/臂内条目随块死亡）。 */
typedef struct {
  char *name;
  int assigned;
} sema_dfa_entry_t;

/* 类型结果表条目（§8.5 第一版）：节点 → 类型（借用，类型归 context 的
   具名类型表/children 表托管）。check pass 记录三类锚点：STMT_VAR 语句
   节点（标注优先、推断兜底）、EXPR_CONSTRUCT_FIELD 节点（字段值演算类
   型）、foreach 迭代变量 name 节点（标注优先、iterable 元素类型兜底）。
   同节点后写覆盖先写（NULL 不覆盖——首次成功的推断不被降级清掉）。 */
typedef struct {
  node_t *node;      /* 借用：原树锚点节点 */
  clux_type_t *type; /* 借用：check pass 演算结果 */
} sema_type_result_t;

/* check.c 记录通道（§8.5）：NULL type 不覆盖既有条目。 */
void sema_record_node_type(clux_sema_t *sema, node_t *node, clux_type_t *type);

/* 泛型定义条目（§2.3）：模块级泛型声明名 → 定义语句（借用原树）。 */
typedef struct {
  char *name;   /* 泛型名（ctx allocator），如 List */
  node_t *stmt; /* 借用：泛型定义语句 */
} sema_generic_def_t;

/* 实例体条目（§2.3 完整版，P11 重构：walk 期物化）：一个实参组合 →
   定义深拷贝 + 代换产出的实例声明。inst_stmt 在 walk 期物化（ctx
   allocator，文本 intern 进 sema->inst_program），check pass 当普通
   声明检查（§8.7 泛型体检查），emit 尾部对其普通克隆进输出（emit
   不再关心实例化）。param_names/args 即代换依据（同序对位）。 */
typedef struct {
  node_t *stmt;       /* 借用：泛型定义语句（克隆源，原树） */
  char *base;         /* 泛型名（ctx allocator），rename 匹配键 */
  char *name;         /* 实例名（ctx allocator），如 List$i32 */
  vec_t *param_names; /* char* 按序形参名（无名参数记 NULL） */
  vec_t *args; /* 借用：实参节点（原树指针），与形参同序；包槽
                         例外——指 pack_arg（sema 持有的聚合 TUPLE） */
  node_t *pack_arg; /* 拥有：包槽聚合 TUPLE（§6.4，元素为剩余实参深拷贝
                       叶子），destroy 递归释放；NULL=该实例无包 */
  node_t *inst_stmt; /* 物化体：walk 期 clone+代换产物（sema 持有），
                         NULL=物化失败（防御态，emit 跳过） */
} sema_instance_body_t;

/* check.c：§8.8 P7 类型环境 + 名字解析。walk 尾部、current module 摘除
   前调用——在原树原名上建模块级名字表并逐函数体做名字/类型引用检查。
   只读 AST、只产 diag，不改节点不阻断后续 emit。 */
int sema_check_pass(clux_sema_t *sema);

/* sema.c：按 node kind 分派取声明的泛型参数表（GENERIC_PARAMS），非泛型
   声明返回 NULL。check.c 的 §8.9 约束检查按它对齐原树形参与实例实参。 */
node_t *sema_stmt_generics_node(node_t *stmt);

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

/* 匿名构造条目（§2.9，walk 收集）：匿名构造点（EXPR_CONSTRUCT，
   TYPE 槽 NULL）→ 合成名 nonamed$N。emit 克隆时命中节点在 TYPE 槽
   回填名字 identifier，尾部按收集序合成具名 struct。 */
typedef struct {
  node_t *node; /* 借用：原树匿名构造点 */
  char *name;   /* 合成名（ctx allocator），如 nonamed$1 */
} sema_anon_t;

/* 方法条目（§2.10，walk 收集）：struct 的 STMT_FUNC 成员 → 提升名
   Type$name 与 self 形参形态。member_call 退化按表改写调用点。 */
typedef struct {
  char *struct_name; /* 所属 struct 名（ctx allocator） */
  char *method_name; /* 方法原名（ctx allocator），匹配键 */
  char *mangled;     /* 提升名 Type$name（ctx allocator），改写目标 */
  int self_ptr;      /* self 形参类型为指针形态（*T 文本）→ receiver 包 .& */
} sema_method_t;

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
   - anons（§2.9）：命中匿名构造点（EXPR_CONSTRUCT TYPE 槽 NULL）在
     克隆体的 TYPE 槽回填合成名 identifier（具名 struct 由 emit 尾部
     合成）；
   - methods（§2.10）：member_call 调用点（callee 为 "." 成员访问）
     退化为 Type$name(receiver, ...)——receiver 为方法所属类型的
     具名构造时按 struct 名直匹配，否则方法名全表唯一才匹配；
     指针 self 形态 receiver 追加 .& 后缀；
   - prunes：STMT_COMPTIME_IF 命中则替换为选中臂（§2.5，walk 已决策）；
   - inst_params/inst_args + instantiate（§2.3 完整版）：实例体克隆态，
     泛型形参引用代换为实参克隆、定义体内自引用使用点（base 命中
     renames[0].from）整体折叠为代换后的实例名、GENERIC_PARAMS 槽
     摘除（实例体非泛型）；
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
  vec_t *skip;            /* 借用：提升摘除集（原树 stmt 指针），可 NULL */
  vec_t *instants;        /* 借用：sema->instants，可 NULL */
  vec_t *consts;          /* 借用：sema->const_uses，可 NULL */
  vec_t *prunes;          /* 借用：sema->prunes，可 NULL */
  vec_t *anons;           /* 借用：sema->anons，可 NULL */
  vec_t *methods;         /* 借用：sema->methods，可 NULL */
  const char *subst_name; /* §2.6 展开期迭代变量名 */
  node_t *subst_src;      /* §2.6 展开期当前元素源子树 */
  vec_t *inst_params;     /* §2.3 实例体态：形参名表（char* 借用），可 NULL */
  vec_t *inst_args; /* §2.3 实例体态：实参节点表（原树指针借用），可 NULL */
  int instantiate;  /* §2.3 实例体态：GENERIC_PARAMS→NULL + 自引用折叠 */
  program_t *reown_from; /* §2.3 P11 emit 期克隆物化体：location 文本按
                            指针命中该文本池（sema->inst_program）时重
                            intern 进目标 program——产物新造文本不得借用
                            sema 生命周期，可 NULL */
  size_t defer_counter; /* __deferN 编号 */
  clux_sema_t *sema;    /* 借用：walk 主体（§8.5 类型结果表查询），可 NULL */
} sema_clone_env_t;

/* env 初始化：out/program 随 emit 创建；renames/skip 由 emit 按 stmt
   覆写；subst_* 初始 NULL。instants/consts/prunes 为 sema 整个 walk
   期的收集表（可空）；sema 为类型结果表（§2.8 回填数据源）借用。 */
void sema_clone_env_init(sema_clone_env_t *env,
                         allocator_t *out,
                         program_t *program,
                         vec_t *instants,
                         vec_t *consts,
                         vec_t *prunes,
                         clux_sema_t *sema);

node_t *sema_clone_node_renamed(node_t *node, sema_clone_env_t *env);

#endif
