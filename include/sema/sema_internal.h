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
  size_t inst_defer_counter; /* 物化循环已消耗的 $defer$N 计数（§2.3
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
   - disposes（§4.17）：disposable 局部 var 的作用域出口析构登记
     （与 defer 共用同一条 LIFO 栈），#dispose 内建实参可命中时脱糖为
     $dispose$T 调用（谓词假静默走通用克隆=无操作）；
   - 结构改写（§2.4/2.6/2.7）：STMT_DEFER → 立即捕获闭包 var + 退出点
     LIFO 调用；STMT_FOREACH → 索引 for；STMT_COMPTIME_FOREACH（ITERABLE
     为 EXPR_TUPLE）→ 按元素逐份展开并替换迭代变量引用
     （subst_name/subst_src 是展开期的临时替换态，由展开函数进出配对置位）；
   - defer_counter：$defer$N 编号，emit 全程单调递增保证模块内唯一；
   - lift_temps/counter/skip_top（§4.17 P3 临时值析构，lift.c）：
     克隆体语句槽内的 disposable 临时根（CALL/CONSTRUCT，类型经
     sema_get_node_type 查 §8.5 结果表）捕获为隐藏 var $tmp$N 提升到
     语句前，出口逆序追加 $dispose$T($tmp$N.&)；skip_top=1 时槽内顶层
     单一根直通不捕获（var 初值 bind-to-local / return 所有权转移）。 */
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
  size_t defer_counter; /* $defer$N 编号 */
  vec_t *disposes; /* §4.17 收集表（sema_dispose_type_t*，emit 持有），
                           NULL=RAII 通道关闭（登记/脱糖全部静默） */
  clux_sema_t *sema; /* 借用：walk 主体（§8.5 类型结果表查询），可 NULL */
  vec_t *lift_temps; /* §4.17 P3 当前语句槽捕获表（sema_lift_temp_t*，
                        env->out 持有），emit 整程共用，初始为空表 */
  size_t lift_counter; /* $tmp$N/$ret$N 编号，emit 全程单调递增 */
  int lift_skip_top;   /* 槽内顶层单一临时根直通不捕获（§4.17 P3） */
  int lift_active; /* 槽驱动武装位：仅块级语句槽克隆期为 1，惰性
                      （&&/||/?:）与闭包（defer/展开）上下文抑制 */
  vec_t *unwind; /* §4.17.3 出口 unwind 链：defer/dispose 登记项
                      （sema_defer_ref_t*，clone.c/lift.c 共享，条目
                      结构体 out 持有），随克隆路径 push/截断维护（块级
                      水位协议：进块记水位，出块截断回水位并释放条目）。 */
  size_t unwind_func_mark; /* 函数体边界水位：return unwind 的底。 */
  size_t unwind_loop_mark; /* 最近循环体边界水位：break/continue unwind
                              的底（循环体克隆期由循环通道设置，foreach
                              元素绑定登记在水位之上、$iter 之下）。 */
  vec_t *closures; /* 借用：闭包退化条目表（closure.c emit 期收集），
                      可 NULL（通道关闭） */
  node_t *closure_owner; /* 借用：当前克隆语境的归属 owner（块级条目
                            资格过滤键：顶层 owner stmt 或模块级条目
                            自身），emit 各克隆点配对设置 */
  vec_t *closure_caps;      /* 借用：$callee 体克隆期激活的捕获表，可 NULL */
  const char *closure_self; /* $callee 体克隆期的自身原名（自递归
                               $env.$callee($env, ...) 改写键），可 NULL */
} sema_clone_env_t;

/* ---- 闭包退化（closure.c，§2.11/§2.12） ----
   具名非 cfunc 函数退化为「$closure$M struct + $callee$M 纯函数」双产物：
   捕获字段平铺进 struct、体内捕获引用改写 $env.<name>、调用点
   N(args) → N.$callee(N.&, args)。合成协议键 $callee/$env（§1.3 三分法
   '$' 前缀）与用户捕获名/形参名天然隔离——捕获名 callee、形参/捕获名
   env 均放行并存。
   块级条目（块尾延迟绑定）定义点换 TDZ var 占位、块尾（unwind 之前）
   追加构造赋值；块级零捕获 P2 起定义点原位构造；模块级条目定义语句
   原位换 var N = .$closure$N{...}。全部机制落 emit 期：收集在 walk
   完成后（§8.5 类型表可查）、合成体直进产物不经 check。 */

/* 捕获条目（entry->captures 元素，closure.c 收集期定型）：
   - 标注捕获（|(c: T = e)|）：type_src 借用原树 TYPE 节点（合成时
     克隆），default_src 借用默认值表达式（可 NULL），type NULL；
   - 裸捕获（|c|）：type 为 §8.5 锚点类型（借用引擎类型表，合成时
     render_type 重建），type_src/default_src NULL。
   ok=0 表示定型失败（整条目不退化）。 */
typedef struct {
  const char *name;    /* 捕获名（out 分配） */
  node_t *type_src;    /* 借用：标注捕获的 TYPE 节点，可 NULL */
  node_t *default_src; /* 借用：标注捕获默认值表达式，可 NULL */
  clux_type_t *type;   /* 借用：裸捕获锚点类型，可 NULL */
  int ok;              /* 定型成功 */
} sema_closure_cap_t;

/* 退化条目（emit 期收集，out allocator 持有，随产物释放）。 */
typedef struct {
  node_t *stmt;  /* 借用：原树 STMT_FUNC 语句（skip/占位匹配键） */
  node_t *owner; /* 借用：块级=所属顶层函数 stmt；模块级 NULL */
  node_t *block; /* 借用：块级=原树中直接包含 stmt 的块；模块级
                           NULL（块尾构造赋值的落点匹配键） */
  const char *name;         /* 原名（out 分配） */
  const char *closure_name; /* $closure$M（out 分配） */
  const char *callee_name;  /* $callee$M（out 分配） */
  vec_t *captures;          /* sema_closure_cap_t*（out 持有） */
} sema_closure_t;

/* 收集（emit 开头调用一次）：模块级 STMT_FUNC + 块级提升项中满足
   资格谓词全集的具名 func（cfunc/泛型/export/comptime/装饰器/匿名
   不退化；块级另需 ≥1 捕获 + 敏感名字域检查）。返回条目表（out
   持有），无合格条目返回空表。 */
vec_t *
sema_closure_collect(clux_sema_t *sema, allocator_t *out, program_t *program);

/* 按原树 stmt 查退化条目（skip 命中/提升尾部剔除/rename 排除共用），
   未命中返回 NULL。 */
sema_closure_t *sema_closure_find_by_stmt(vec_t *closures, node_t *stmt);

/* 条目表收尾释放（emit 结束调用）；合成名 intern 进产物 program，
   不随本表释放失效。 */
void sema_closure_entries_free(allocator_t *out, vec_t **closures);

/* 定义点原位构造 var N: $closure$M = .$closure$M{...}：模块级条目主循环
   替换 + 块级零捕获条目定义点原位（§2.12 P2——无捕获即无前向依赖，
   不走 TDZ 占位/块尾赋值）。 */
node_t *sema_closure_define_var(sema_closure_t *entry, sema_clone_env_t *env);

/* 块级条目（含捕获）定义点 TDZ 占位：var N: $closure$M = undefined; */
node_t *sema_closure_placeholder(sema_closure_t *entry, sema_clone_env_t *env);

/* 块级条目块尾构造赋值：N = .$closure$M{.$callee=$callee$M, ...};
   STMT_EXPR(EXPR_ASSIGN) 包装。捕获字段值：标注捕获带默认值时克隆
   默认值表达式，否则为捕获名 identifier（块尾现值快照）。 */
node_t *sema_closure_construct_assign(sema_closure_t *entry,
                                      sema_clone_env_t *env);

/* 合成 struct $closure$M { $callee: func(*$closure$M, 参数类型...)->ret;
   捕获字段... }——$callee 字段打头，捕获字段按捕获序平铺。 */
node_t *sema_closure_struct_synth(sema_closure_t *entry, sema_clone_env_t *env);

/* 合成 func $callee$M ($env: *$closure$M, 原形参...) -> ret { 原体 }——
   体克隆期置 closure_caps/closure_self（$env.<cap> 改写 + 自递归
   $env.$callee($env, ...)），调用 clone.c 者负责配对恢复。 */
node_t *sema_closure_callee_synth(sema_closure_t *entry, sema_clone_env_t *env);

/* 调用点退化（clone.c EXPR_CALL 分派）：callee 名字叶命中退化条目
   → N.$callee(N.&, args)；closure_self 命中 → $env.$callee($env, args)。
   未命中返回 NULL 走通用克隆。 */
node_t *sema_desugar_closure_call(node_t *node, sema_clone_env_t *env);

/* 捕获引用改写（clone.c 标识符叶早期通道）：closure_caps 激活期
   命中捕获名 → env.<name> 成员访问。未命中返回 NULL。 */
node_t *sema_closure_cap_rewrite(node_t *node, sema_clone_env_t *env);

/* §4.17.3 unwind 登记项（如上）。type 为析构形态登记的绑定类型（借用
   引擎类型表，defer 形态 NULL）——return 值引用链上待析构绑定时 $ret
   物化的类型反查数据源。 */
typedef struct {
  const char *name; /* $defer$N 或 $dispose$T（intern 文本） */
  const location_t *loc;
  int dispose;          /* 1=析构调用形态：追加实参 var_name.& */
  const char *var_name; /* dispose 目标变量名（intern 文本） */
  clux_type_t *type;    /* 借用：dispose 形态登记的绑定类型。 */
} sema_defer_ref_t;

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

/* env 收尾：lift_temps 是 env 唯一自建所有物，通道结束统一释放
   （防御性 drain 后 delete_vec）。与 init 成对，emit/物化收尾调用。 */
void sema_clone_env_destroy(sema_clone_env_t *env);

node_t *sema_clone_node_renamed(node_t *node, sema_clone_env_t *env);

/* §4.17 P3 临时值捕获 guard（clone.c）：武装上下文命中 disposable
   临时根先克隆后捕获为 $tmp$N 替身，未武装/未命中退化为通用克隆。
   closure.c 的调用点改写实参克隆复用此通道（与成员调用同模式）。 */
node_t *sema_clone_lift_guard(node_t *node, sema_clone_env_t *env);

/* ---- §4.17 RAII P1：自动析构（dispose.c） ---- */

/* 需合成析构函数的 struct 类型条目（emit 期收集，out allocator 持有）：
   type 借用引擎类型表；loc 为收集点（登记处的源位置），合成节点的
   location 模板。 */
typedef struct {
  clux_type_t *type; /* 借用：struct 引擎类型（字段表/名字来源） */
  location_t loc;    /* 源位置模板（合成节点与渲染文本的 location） */
} sema_dispose_type_t;

/* disposable 谓词（白皮书 §4.17.1）——struct 实现 __dispose__ 钩子，
   或传递性包含需要析构的成分（struct 字段、?T 载荷、[N]T 元素、
   tag union variant、元组成员，P2 扩展）；指针/切片/基础类型/cunion
   不析构。钩子判定走 sema->methods 预扫描表（check 期引擎 struct
   类型只有字段表，method 三表在 exec 两阶段 install，
   clux_struct_is_method 不可用）。 */
int sema_type_needs_dispose(clux_sema_t *sema, clux_type_t *type);

/* 登记需合成 $dispose$T 的类型（按引擎类型指针去重，递归登记
   disposable 成分类型），env->disposes 为 NULL 时静默。 */
void sema_dispose_register(sema_clone_env_t *env,
                           clux_type_t *type,
                           const location_t *loc);

/* 合成顶层语句 func $dispose$T(self: *T): void { ... }——按类型 kind
   分派 §4.17.2 展开表：struct=钩子+逆声明序字段；?T=is_none 门控
   #take 载荷；[N]T=固定 for 循环逆序元素；tag union=逐 variant is
   判定链；元组=逆序下标。合成体不经 check（emit 尾部直接进产物）。 */
node_t *sema_dispose_synth(sema_clone_env_t *env,
                           const sema_dispose_type_t *entry);

/* 合成析构函数名（$dispose$ + mangled 类型名）——非 [A-Za-z0-9_$]
   字节转 $xHH 转义（"?i32"→"$x3fi32"）；interning 保证定义点与
   调用点同一字符串实例，经纯名字通道对齐。失败返回 NULL。 */
const char *sema_dispose_callee_name(allocator_t *allocator,
                                     program_t *program,
                                     const char *type_name);

/* clux_type_t → 类型表达式 AST：按 kind 递归重建（具名叶子 identifier、
   指针/optional/切片/限定前缀、[N]T 二元形态、元组节点）。失败（无名
   叶子、零元元组）返回 NULL，调用方放弃该展开。lift.c 提升 var 的
   TYPE 槽数据源。 */
node_t *sema_dispose_render_type(allocator_t *out,
                                 program_t *program,
                                 clux_type_t *type,
                                 const location_t *loc);

/* ---- §4.17 RAII P3：临时值提升（lift.c） ---- */

/* 当前语句槽捕获的临时条目（env->out 持有）：name 为 $tmp$N interned
   名；value 为已克隆初始化子树、type_node 为预渲染类型表达式（均于
   flush 时移入提升 var）；callee 为 $dispose$T 合成 callee 名（interned，
   析构点公用）；loc 为源位置模板。捕获期渲染/合成即验证——类型不可名
   或分配失败时 sema_lift_capture 返回 NULL，调用方原样保留克隆子树。 */
typedef struct {
  const char *name;
  node_t *value;
  node_t *type_node;
  const char *callee;
  const location_t *loc;
} sema_lift_temp_t;

/* 临时根判定：CALL/CONSTRUCT 且类型表命中 disposable 类型（§8.5 结果
   表查询，锚点由 P3a check 侧登记）。env->sema/disposes 为 NULL 或
   clone 上下文未武装（lift_active）时由调用方先行短路。 */
int sema_lift_is_root(node_t *node, sema_clone_env_t *env);

/* 惰性父判定（&&/||/?:）：子表达式条件求值，提升即提前求值破坏短路
   ——clone.c children 循环在克隆期对整棵解除武装。 */
int sema_lift_is_lazy_parent(node_t *node, sema_clone_env_t *env);

/* 捕获通道：登记 sema_lift_temp_t（含 sema_dispose_register 递归登记
   成分类型）并返回 $tmp$N 标识符替身节点。 */
node_t *
sema_lift_capture(sema_clone_env_t *env, clux_type_t *type, node_t *cloned);

/* 语句槽判定（§4.17.5 v1 槽表）：STMT_EXPR / STMT_IF / STMT_SWITCH /
   STMT_ASSERT / STMT_RETURN / STMT_VAR / STMT_FOREACH——其中
   VAR/RETURN/FOREACH 槽 lift_skip_top 置位（顶层单一临时根直通：var
   直接 bind、return 所有权随值转移、foreach iterable 绑入 $iter）。 */
int sema_lift_slot_applies(node_t *stmt);

/* 出口 flush 原语（clone.c 块通道组合）：base 为语句槽基线（嵌套块
   语句各自回到自身基线，外层 pending 条目不受波及）。 */
void sema_lift_emit_vars(sema_clone_env_t *env, node_t *block, size_t base);
void sema_lift_emit_disposes(sema_clone_env_t *env, node_t *block, size_t base);
void sema_lift_drain(sema_clone_env_t *env, size_t base);

/* return 槽 $ret 物化：捕获表非空时 return 表达式引用 $tmp，直接
   [析构][return] 会 use-after-dispose——先物化 var $ret$N: T = <return
   表达式>（类型取 return 值根的 §8.5 锚点），析构后再 return $ret$N。
   锚点缺失（值根非 CALL/CONSTRUCT）时不物化返回 0（沿用裸形态，
   惰性根已被捕获抑制，仅嵌套容器根残留该风险，P3c 登记验证）。 */
int sema_lift_materialize_return(sema_clone_env_t *env,
                                 node_t *block,
                                 node_t *src_ret,
                                 node_t *ret_clone);

/* 循环降 viability 判定与降维（零 flag 方案）：cond/update（for 另含
   init）含临时根的 while/do-while/for 统一降为 for(;;) + 段内 gate
   块——临时在 gate 块内每轮声明每轮析构（exec 块每执行 push 新
   scope），cond 求值次数与原语义一致。 */
int sema_lift_loop_applies(node_t *stmt, sema_clone_env_t *env);
void sema_lift_lower_loop(node_t *src, node_t *block, sema_clone_env_t *env);

/* 直接层退出点命中（循环体视角）：parent 为命中语句所在块，idx 为该块
   children 内下标。 */
typedef struct {
  node_t *parent;
  size_t idx;
} sema_exit_hit_t;

/* 收集块语句序列内的直接层 continue/break：块/if 臂/switch 臂递归；嵌套
   循环体（for/foreach/while/do-while）不深入——其中的 continue/break
   属于嵌套层；defer 体与嵌套函数体同理不深入。continue_only 非 0 时只
   收 CONTINUE（do-while 降维 continue 前插 gate 用——break 跳出循环
   本就不该再求值 cond，无需前插）。命中点先收集后按 idx 逆序前插
   （前插不扰动更小 idx 的命中位）。调用方负责释放收集的 hit 条目。 */
void sema_collect_exit_hits(node_t *block,
                            vec_t *hits,
                            allocator_t *out,
                            int continue_only);

/* ---- §4.17.3 return 值引用待析构绑定的 $ret 物化（clone.c） ---- */

/* return 值子树保守文本扫描：func_mark..top 区间内有 dispose 条目且
   子树任一 LITERAL_IDENTIFIER 文本命中其 var_name 时返回 1——析构前插
   先于 return 表达式求值（use-after-dispose），须物化 $ret 先求值拷贝。
   内层同名遮蔽的误判只导致多余物化（copy 语义等价 + 钩子幂等，无害）。
   跳过嵌套函数体（EXPR_FUNC_DECL 子树，其 return 是该函数自己的）。 */
int sema_unwind_return_captures(sema_clone_env_t *env, node_t *ret_stmt);

/* unwind 链按名反查绑定类型：根 identifier 文本命中 func_mark..top 区间
   内 dispose 条目 var_name 时返回该条目登记的绑定类型（借用），否则
   NULL——materialize_return 锚点 miss 时的类型兜底。 */
clux_type_t *sema_unwind_lookup_type(sema_clone_env_t *env, node_t *value);

#endif
