#ifndef _H_CLUX_SEMA_SEMA_
#define _H_CLUX_SEMA_SEMA_
#include "core/allocator.h"
#include "core/arena.h"
#include "core/strslice.h"
#include "core/vec.h"
#include "diag/diagnostic.h"
#include "parser/ast_node.h"
#include "parser/ast_func_def.h"
#include "parser/type_qual.h"
#include "sema/symbol.h"
#include "vm/vm.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 语义分析（sema）
 *
 * 两阶段：先构建 sema 作用域树（scope 节点带完整符号表），再按作用域树
 * 用 shadow value 遍历 AST 做类型检查与推导。作用域结构与类型检查彻底分离。
 *
 * 核心契约：sema 是处理类型错误的最后一个阶段。sema 通过后，字节码编译器
 * 和 VM 可以假定一切类型正确，运行时不再做类型检查。vtable 运算返回的
 * error value 被 sema 捕获并转为诊断，不传播到下游。
 * =========================================================================== */

/* ---- sema 层函数对象 ---- */

/**
 * sema_func_t: 语义分析层的函数单元（统一登记在 sema->funcs 队列）
 *
 * - def: 函数定义 AST 节点（AST_FUNC_DEF，借用，arena 管理，不拥有）
 * - scope: 函数作用域树（Pass 3 构建；局部函数/泛型实例在解析中补建）
 * - name: 函数名（借用 def->name 的 strslice，诊断用）
 *
 * 签名类型不在此持有：函数符号 sema_symbol_t::type 即签名类型
 * （func_type_t，vm 池 intern）。符号表只负责名字解析（sym->ast 指向 def），
 * 函数自身状态（作用域树）由本对象承担——为局部函数提升与泛型单态化
 * 预留：解析过程中发现的新函数（局部函数 / 泛型实例）追加到 sema->funcs
 * 队列末尾，Pass 3 按序处理（队列驱动）。
 */
typedef struct sema_func_t {
    ast_node_t   *def;    /* AST_FUNC_DEF（借用） */
    sema_scope_t *scope;  /* 函数作用域树（Pass 3 填充）：捕获层——闭包捕获
                             符号注册在此（镜像运行时 closure_scope） */
    sema_scope_t *param_scope; /* 参数层：scope 的子作用域，参数符号注册在此
                                  （镜像运行时 func_vcall 的参数匿名层）。
                                  函数体 block 挂此层下——参数遮蔽捕获。 */
    strslice_t    name;   /* 函数名（诊断用） */
    bool          is_local; /* 局部函数（块内定义）：3b walk_block 提升签名
                               + 捕获检查（fscope parent = 定义点块作用域） */
    bool          is_literal_owned; /* 函数字面量 body 内登记的局部函数：
                                       sema_check_func_literal 已同步 walk 并
                                       标记，Pass 3b 驱动循环跳过（防二次 walk
                                       访问已销毁的临时 fscope） */
} sema_func_t;

/**
 * sema_type_t: 语义分析层登记的类型单元（统一登记在 sema->types 队列）
 *
 * - type: 类型单例（vm 池 intern，按指针去重：同一类型只登记一次）
 * - name: 具名类型标识 "__type_N"（arena 分配，AST_TYPE_REF 引用标识；
 *   不是显示名——显示名保留在 type->name）
 * - id:   DEFINE_TYPE 的 u32 id 操作数（TYPE_ID_PROGRAM_BASE + index）
 *
 * 与 funcs 同构（m2-design §comptime"类型提升"）：sema 把解析过的每个
 * 类型登记到此队列，compiler 据此生成 hoist 类型提升区（**两遍扫描**：
 * pass 1 声明所有类型 PUSH_XXXX → DEFINE_TYPE <id> 登记、pass 2 定义所有
 * 类型 LOAD_TYPE 拉回 → 设字段 → SEAL 封闭）并在类型槽位发 LOAD_TYPE
 * <id>——AST 因此保持平凡可解耦（类型槽位是 AST_TYPE_REF 名字引用，
 * 不关联任何 type_t 指针）。
 */
typedef struct sema_type_t {
    const type_t *type;    /* vm 池 intern 单例 */
    strslice_t    name;    /* "__type_N"（sema arena 生命周期） */
    uint32_t      id;      /* TYPE_ID_PROGRAM_BASE + index */
} sema_type_t;

typedef struct sema_t {
    vm_t         *vm;           /* 复用 VM 类型注册表 + vtable + shadow value */
    diag_buf_t   *diag;         /* 诊断收集器 */
    vec_t        *tokens;       /* token pool（借用，诊断取源码位置） */
    arena_t      *arena;        /* AST 折叠分配（借用 driver arena；comptime
                                   折叠出字面量节点与字符串常量） */
    sema_scope_t *global_scope; /* 全局作用域树根 */

    /* 函数队列：sema 层全部函数（顶层函数 Pass 1 登记；局部函数/泛型实例
       在 Pass 3 解析中追加，队列驱动、可增长）。sema 拥有元素生命周期。 */
    vec_t        *funcs;        /* sema_func_t* */

    /* 类型队列：sema 层解析过的全部类型（resolve_type_expr 登记 + comptime
       折叠逆向登记），按 type_t 指针去重，驱动编译器 hoist 类型提升区。
       sema 拥有元素生命周期。 */
    vec_t        *types;        /* sema_type_t* */

    /* 函数上下文（Pass 3 walk 时设置） */
    const type_t *func_return_type; /* NULL = void */
    bool          func_has_return;

    /* comptime 函数体 walk 标志：sema_walk_function 对 comptime func 置
       true（保存/恢复）。walk 期间 body 内对 comptime func 的调用只做
       普通 shadow 类型检查（实参是参数 shadow value，无法编译期求值）——
       CTFE 折叠仅发生在真实调用点（非 comptime body 内）。 */
    bool          walking_comptime;

    /* 捕获检查上下文（Pass 3b walk 时设置）：非 NULL = 正在 walk 局部函数体，
       fscope parent = 定义点块作用域（同块局部函数互相可见）。sema_expr
       引用外层局部符号（非 fscope 直系、非 global）时据此报"无闭包"——
       运行时函数体查找链只有参数 + 全局（closure_scope 为空，调用时临时接
       root_scope），外层局部不可见。
       local_func_param_scope：函数参数层（fscope 子 scope）——参数符号
       归属检查（参数遮蔽捕获，参数与捕获同属函数自身符号，放行）。 */
    sema_scope_t *local_func_base;
    sema_scope_t *local_func_param_scope;

    /* 调用点 callee 上下文（AST_CALL 分支设置）：true = 当前 AST_IDENT 求值
       是函数调用点 callee（非值引用）。func_capture_tdz（闭包捕获 TDZ 检查）
       仅对调用点生效——值引用（var f = b，f/b 浅拷贝共享 func_t）放行，定义
       点前调用仍编译期拦截。sema_expr 递归进入 callee 前置位、返回后复位。 */
    bool          in_call_callee;

    /* 循环上下文 */
    int           loop_depth;       /* 0 = 不在循环中 */

    /* 函数 id 分配计数器：sema 登记每个函数定义（全局函数 pass1_names、
       局部函数 build_local_func、函数字面量 sema_check_func_literal / CTFE
       求值）时从 FUNC_ID_PROGRAM_BASE 起统一分配（写回 fn->fid）。compiler
       预扫描只读取不再分配——fid 单一来源在 sema（用户函数对象创建即持 id，
       comptime 折叠产物按 fid 加载函数，与 name 无关）。 */
    uint32_t      func_id_next;
} sema_t;

/* ---- 公共 API ---- */

/**
 * 闭包捕获解析（函数"定义点"调用，外层 scope 在线）：
 * - 纯 id 捕获：外层符号查找（变量须确定已初始化）+ 类型写回 fscope 捕获符号
 * - 括号捕获：init 在外层作用域求值 + 显式 type_expr 校验 / init 类型推断
 * 调用点：walk_block AST_FUNC_DEF 分支（局部函数）与 sema_check_func_literal
 * （函数字面量）。捕获符号 type 就绪后，sema_walk_function /
 * sema_check_func_literal 据此定义捕获 shadow value。
 */
void resolve_func_captures(sema_t *sema, ast_func_def_t *fn,
                           sema_scope_t *fscope, sema_scope_t *outer);

/**
 * 创建 sema 上下文。vm 提供类型注册表/vtable/shadow value；diag 收集诊断；
 * tokens 是 token pool（借用，不拥有），用于把 AST 节点的 tok_begin 下标
 * 解析为源码位置；arena 是 AST 折叠分配器（comptime 折叠用，借用）。
 * Panics on out-of-memory. Returns NULL for invalid arguments.
 */
sema_t *sema_create(vm_t *vm, diag_buf_t *diag, vec_t *tokens, arena_t *arena);

/**
 * 三遍扫描：Pass 1 函数名收集 → Pass 2 类型解析（func_t 签名）→
 * Pass 3a 作用域树构建 + Pass 3b shadow VM 运行（类型检查）。
 *
 * 返回 false 表示存在语义错误（诊断已记录到 diag）。
 * 作用域树在返回后保持有效（持久化数据，交字节码编译器复用）。
 */
bool sema_analyze(sema_t *sema, ast_node_t *program);

/**
 * 销毁 sema 上下文。不销毁作用域树（调用方通过 sema_scope_destroy 释放）
 * 与 token pool / vm / diag（均为借用）。
 * No-op if `sema` or `*sema` is NULL.
 */
void sema_destroy(sema_t **sema);

/* ===========================================================================
 * internal（sema.c / stmt.c 共享，不对外）
 * =========================================================================== */

/**
 * 类型表达式求值唯一入口：ast_node_t* → const type_t*
 * (type is expression, m2-design 关键架构决策 6)
 *
 * 类型槽位 = 普通表达式（type is expression）：AST_IDENT 命名类型
 * （type_lookup 沿作用域链查 type value）、AST_CONST/AST_VOLATILE 修饰
 * （递归 sub + intern）、AST_TYPE_REF 具名类型引用（types 队列查表）。
 * M2 扩展：数组/元组/func 类型表达式与类型计算等在此求值。
 *
 * 副作用：每个解析出的类型（含递归 sub）登记进 sema->types（按指针去重），
 * 供编译器 hoist 提升。失败返回 NULL（已报错）。空指针入参返回 NULL 不报错。
 */
const type_t *resolve_type_expr(sema_t *sema, ast_node_t *type_expr);

/**
 * 编译期求值数组边界（数组类型 [N]T 的 N / fill 的 <v,N> 重复次数）：
 * 求值为 size_t + 折叠 *bound 就地替换为 AST_INT_LIT（编译器读立即数，
 * 零感知）。字面量直接读；复杂表达式走 ctfe 编译期求值（失败报错返回
 * false，诊断已记录）。fill count 与数组 length 复用同一机制。
 */
bool sema_eval_array_bound(sema_t *sema, ast_node_t **bound, size_t *len);

/**
 * 类型槽位替换：resolve_type_expr + 把 *slot 就地替换为 AST_TYPE_REF
 * （携带登记的 "__type_N" 名字），返回解析出的类型。
 *
 * 下游编译器遇 AST_TYPE_REF 发 LOAD_TYPE <id>，类型构造收敛到 hoist 区——
 * AST 保持平凡可解耦（不挂 type_t 指针）。slot 已是 AST_TYPE_REF 时幂等
 * （重新解析 + 同名字引用替换，无重复登记）。
 */
const type_t *sema_resolve_type_slot(sema_t *sema, ast_node_t **slot);

/**
 * 类型登记：按 type_t 指针去重，未登记则分配 "__type_N" 名字与
 * TYPE_ID_PROGRAM_BASE+index id 追加到 sema->types。复合类型（数组/const/
 * volatile）的结构依赖（elem/sub）一并递归登记，保证 hoist 构造完备。
 * 返回登记的 sema_type_t*（NULL = OOM）。
 */
const sema_type_t *sema_type_register(sema_t *sema, const type_t *t);

/**
 * 按类型指针 / 名字查找登记的 sema_type_t（线性扫描，类型数量少）。
 * 未登记返回 NULL。
 */
const sema_type_t *sema_type_find(sema_t *sema, const type_t *t);
const sema_type_t *sema_type_find_name(sema_t *sema, strslice_t name);

/**
 * 按登记 id 查 sema_type_t（线性扫描，类型数量少）。CTFE 求值函数引用
 * （AST_FUNC_REF 折叠产物）从 fn->sig_id 反查签名类型用。
 * 未登记返回 NULL（内建类型不登记，不在此队列）。
 */
const sema_type_t *sema_type_by_id(sema_t *sema, uint32_t id);

/** 将 AST 节点解析为源码位置（经 token pool）。 */
location_t sema_loc(sema_t *sema, ast_node_t *node);

/** 打印类型名到 stdout（内部调试/诊断用）。 */
void sema_type_name(const type_t *t, char *buf, size_t cap);

/**
 * Pass 3a 作用域树构建（stmt_build.c 实现）：遍历 sema->funcs 队列，对每个函数
 * 按词法块结构建树，只注册符号（名字 + 声明类型 + TDZ 标志），不做类型检查。
 * 作用域树存入 sema_func_t::scope。
 */
void sema_build_scope_tree(sema_t *sema);

/**
 * Pass 3b 单个函数的 shadow VM 运行（stmt.c 实现）。
 * 严格按预建作用域树（sf->scope）遍历函数体，做类型检查与推导。
 */
void sema_walk_function(sema_t *sema, sema_func_t *sf);

/**
 * 函数字面量（表达式内 AST_FUNC_DEF，sema_expr 求值用）：签名解析 +
 * body 类型检查（stmt_build.c 实现，同步建临时 fscope 并 walk，不注册
 * 作用域名字、不提升）。body 内登记的局部函数在此同步 walk 并标记
 * is_literal_owned。返回签名类型（失败 NULL），成功则写入 fn->sig_id。
 */
const type_t *sema_check_func_literal(sema_t *sema, ast_func_def_t *fn,
                                      sema_scope_t *outer);

/**
 * 函数字面量 body walk 入口（stmt.c 实现，sema_check_func_literal 用）：
 * walk_block 是 static，此处暴露薄封装。idx 内部自持。
 */
void sema_walk_block(sema_t *sema, ast_node_t *block, sema_scope_t *scope);

/**
 * 表达式求值（shadow value）：只有类型，data=NULL。
 * node 取指针：comptime 折叠（comptime var 引用 / comptime func 调用）会
 * 就地改写 *node 为字面量 AST 节点（arena 分配），下游（编译器）零感知。
 */
value_t *sema_expr(sema_t *sema, ast_node_t **node, sema_scope_t *scope);

/** 检查操作数必须为 bool；error/void shadow（错误恢复产物）静默通过。 */
void sema_check_bool(sema_t *sema, ast_node_t *node, value_t *v,
                     const char *what);

/** 兄弟链节点计数（参数/实参列表长度）。 */
size_t sema_count_siblings(const ast_node_t *node);

/**
 * 函数 id 分配（幂等）：fn->fid 未分配（0）时从 sema->funcs 队列尾部函数
 * 的 fid 分配写回，已分配则返回现有值。创建 sema 函数对象（pass1 全局 /
 * build 局部 / sema_check_func_literal 字面量 / CTFE 求值字面量）时调用——
 * fid 单一来源在 sema。返回分配后的 fid（0 = 溢出/无效参数）。
 */
uint32_t sema_func_id_alloc(sema_t *sema, ast_func_def_t *fn);

/**
 * 按函数 id 查 sema 函数对象（sema->funcs 线性扫描，函数数量少）。
 * 函数定义 AST 托管在 sema->funcs（sema_func_t::def），sema/ctfe 需要
 * 函数 AST / 签名时经此查询（AST_FUNC_REF 折叠产物按 fid 取签名构造引用）。
 * 未登记返回 NULL（内建函数无 AST 托管，不在此队列）。
 */
sema_func_t *sema_func_by_id(sema_t *sema, uint32_t fid);

#ifdef __cplusplus
}
#endif
#endif
