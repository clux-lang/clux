#ifndef _H_CLUX_SEMA_SEMA_
#define _H_CLUX_SEMA_SEMA_
#include "core/allocator.h"
#include "vm/type_ownership.h"
#include "core/arena.h"
#include "core/strslice.h"
#include "core/vec.h"
#include "diag/diagnostic.h"
#include "parser/ast_node.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ptr.h"
#include "parser/ast_owned_type.h"
#include "parser/ast_scope_annot.h"
#include "parser/ast_slice_type.h"
#include "parser/parse_utils.h"
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
    sema_scope_t *def_scope; /* 符号定义所在作用域（Pass 2 符号查找用）：
                                全局函数 = global_scope；struct 方法 =
                                struct 类型的 member_scope。NULL = 全局
                                （向后兼容旧代码按 global_scope 查找）。 */
    bool          is_local; /* 局部函数（块内定义）：3b walk_block 提升签名
                               + 捕获检查（fscope parent = 定义点块作用域） */
    bool          is_literal_owned; /* 函数字面量 body 内登记的局部函数：
                                       sema_check_func_literal 已同步 walk 并
                                       标记，Pass 3b 驱动循环跳过（防二次 walk
                                       访问已销毁的临时 fscope） */
    bool          is_walked;        /* M7：泛型实例在实例化时已同步 walk
                                       （Pass 3b 队列循环跳过，防二次 walk） */
    struct sema_generic_instance *generic_inst; /* M7：本函数是泛型实例时，
                                       指向实例记录（Pass 3b walk 前据此重绑
                                       类型参数；NULL = 普通函数） */
} sema_func_t;

/**
 * sema_type_t: 语义分析层登记的类型单元（统一登记在 sema->types 队列）
 *
 * - type: 类型单例（vm 池 intern，按指针去重：同一类型只登记一次）
 * - name: 具名类型标识 "__type_N"（arena 分配，AST_TYPE_REF 引用标识；
 *   不是显示名——显示名保留在 type->name）
 * - id:   DEFINE_TYPE 的 u32 id 操作数（**全 VM 唯一**，type_id_alloc 分配）
 * - index: 本类型在 sema->types 中的登记下标。id 已不再与下标一一对应（见
 *   vm/type.h 的 type_id_alloc 说明），任何需要"数组下标"的地方（如 hoist 区
 *   的 done 三态表）都必须用 index，不能用 id - TYPE_ID_PROGRAM_BASE 推算。
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
    uint32_t      id;      /* 全 VM 唯一 program id（type_id_alloc） */
    uint32_t      index;   /* sema->types 中的登记下标（0 起） */
    struct sema_meta_entry *type_meta;
    struct sema_meta_entry *field_meta;
    struct sema_meta_entry *method_meta;
} sema_type_t;

/**
 * M7 §31：一个已通过校验的 `implements` 记录（interface 实例）。
 * type 是 struct 实例类型；iface_def 是 interface AST；args 是接口类型实参
 * （interface 泛型参数只能是类型，P6b）。用于 `T extends I[...]` 约束匹配。
 * 节点由 allocator 分配、args 由 arena 分配，随 sema 销毁/arena 回收。
 */
typedef struct sema_impl {
    const type_t      *type;
    const ast_node_t  *iface_def;
    const type_t     **args;
    size_t             arg_count;
    struct sema_impl  *next;
} sema_impl_t;

struct sema_generic_instance; /* sema_generic.h；诊断链用 */
struct module_t;              /* vm/module.h；跨模块实例归属 */

/**
 * sema_fn_ctx_t: **当前正在 walk 的函数**的上下文（Pass 3 期间设置）
 *
 * 收敛原先散落在 sema_t 上的多个"函数 walk 状态"字段：它们总是一起被设置、
 * 一起被保存/恢复，混在 sema_t 里既看不出边界，也容易漏掉恢复（曾出现
 * `walking_comptime` 忘记复位导致后续函数被误判为 comptime body）。
 *
 * - return_type: 函数返回类型（NULL = void）；walk_return R2 校验用
 * - def: 当前函数定义 AST；walk_return R3 判定"返回 ref 是否来自参数"
 *   ——参数符号与函数体顶层变量都注册在 param_scope，sema_scope_find_local
 *   无法区分，须遍历真实参数列表（fn->params）
 * - walking_comptime: 正在 walk comptime func 体（body 内调用 comptime func
 *   只做 shadow 类型检查，CTFE 折叠仅发生在真实调用点）
 * - local_func_base / local_func_param_scope: 捕获检查上下文（非 NULL = 正在
 *   walk 局部函数体）：fscope parent = 定义点块作用域；引用外层局部符号报
 *   "无闭包"；参数层用于放行"参数遮蔽捕获"
 * - in_call_callee: 当前 AST_IDENT 求值是调用点 callee（非值引用）——
 *   func_capture_tdz 仅对调用点生效
 * - loop_depth: 循环嵌套深度（0 = 不在循环中），break/continue 位置检查用
 */
typedef struct sema_fn_ctx_t {
    const type_t   *return_type; /* 返回所有权编码在这个类型里 */
    ast_func_def_t *def;
    bool            walking_comptime;
    sema_scope_t   *local_func_base;
    sema_scope_t   *local_func_param_scope;
    bool            in_call_callee;
    int             loop_depth;
} sema_fn_ctx_t;

/**
 * sema_expr_ctx_t: **表达式求值**期间共享的上下文
 *
 * - anon_ct/anon_ct_depth: 匿名构造目标类型栈（`.{ ... }`，type=NULL 的
 *   AST_CONSTRUCT）：入栈时机——var 声明已初始化分支、简单/复合赋值右值、
 *   struct 具名构造字段递归、函数调用实参；栈空时匿名构造报错。
 */
typedef struct sema_expr_ctx_t {
    const type_t *anon_ct[16];
    size_t        anon_ct_depth;
} sema_expr_ctx_t;

/* ---- §4/R1 fatal 消费点：**唯一判据入口** ----
 *
 * fatal 值（`fatal *T` / `fatal []T`）是"必须被消费"的所有权载体，合法消费点
 * 只有两个：**move(x)**（AST_MOVE）与 **return x**（walk_return）。三处判定
 * （move 标记、return 标记、函数结束的未消费检查）统一走本组接口，判据只此
 * 一处定义——此前分散在三段代码里各写一遍 `PTR_FATAL || SLICE_FATAL`，其中
 * 一处漏了切片分支，导致 `return p`（p 为 fatal 切片参数）被误报未消费。
 *
 * 所有权纯编译期——fatal 判据从 symbol.ownership 读取，不再从 类型所有权修饰。 */
/** 符号的所有权：直接由符号类型派生（所有权是类型修饰，符号不再存副本）。 */
static inline ownership_t sema_sym_ownership(const sema_symbol_t *sym) {
  return (sym && sym->type) ? type_ownership_of(sym->type) : OWN_NONE;
}

static inline bool sema_sym_is_fatal(const sema_symbol_t *sym) {
  return sema_sym_ownership(sym) == OWN_FATAL;
}
/* 兼容旧调用：从 value 的 ownership 判 fatal */
static inline bool sema_value_is_fatal(const value_t *v) {
  return v && value_type_ownership(v) == OWN_FATAL;
}
static inline void sema_mark_fatal_consumed(sema_symbol_t *sym) {
  if (sym) sym->moved = true;
}
static inline bool sema_fatal_consumed(const sema_symbol_t *sym) {
  return sym && sym->moved;
}

/* 所有权由**类型**携带：类型表达式解析（sema_resolve_inner）直接产出
 * `own T` / `ref T` / `fatal T` 包装类型。语义分析需要所有权时一律用
 * `type_ownership_of(已解析类型)`，不再从 AST token 反查。 */

/* ---- 复合类型的 own 资源判定：**已删除**（2026-10 所有权重构） ----
 *
 * 原先有 `sema_type_contains_owned_resource`（递归）与
 * `sema_type_contains_own_resource`（仅 own）两个类型层谓词，用于
 * "含 own 字段的复合类型不可隐式拷贝"的判定与 fatal 变体构造。
 *
 * 二者随重构失效并已删除，原因有二：
 *   1. **所有权已从类型层迁移到值/符号层**——`type_t` 不再携带 ownership，
 *      类型层无从判定 own（原实现退化为恒返回 false 的空壳）；
 *   2. **复合类型已禁止携带指针/切片**——"含 own 字段的复合类型"这一形态
 *      当前不存在，递归判定没有作用对象。
 *
 * 现在的判据入口是**符号/值层**：`sema_symbol_t::ownership`（声明处所有权）与
 * `value_type_ownership()`（类型层所有权）。将来恢复含指针的复合类型时，应按
 * **扁平槽位表**（类型 → own 槽位偏移）重新设计，而不是恢复这两个递归谓词。
 */
struct sema_t; /* 前向声明：import 回调签名与 fn 上下文配对接口用 */
struct sema_generic_instance;


/**
 * sema_import_resolver_t: **import 解析/编译回调**（驱动层注入）
 *
 * sema 遇 `AST_IMPORT` / `AST_REEXPORT` 时调用注入的回调，由驱动层负责：
 * 路径分类与规范化（module 层的 module_path_*）、存在性检查、递归编译
 * （模块缓存 + 循环依赖检测）以及全部诊断。
 *
 * 目的：**消除 sema → driver 反向依赖**。sema 不再 include 任何 driver 头、
 * 不认识 module_t 的内容（只把返回的不透明句柄存进 SEMA_SYM_MODULE 符号）、
 * 也不做路径处理——依赖方向保持单向（driver → sema）。
 *
 * 返回：被导入模块的句柄（`module_t *`，驱动层已注册进 vm->modules）；
 *       NULL = 失败，诊断已由回调记录（sema 静默跳过）。
 *       未注入回调时 sema 对 import 报"当前编译模式不支持 import"。
 */
typedef struct module_t *(*sema_import_resolver_t)(void *user,
                                                   struct sema_t *sema,
                                                   ast_node_t *import_node,
                                                   strslice_t raw);

/* 生成的销毁函数（sema_destroy_fn_t）：每个**含 own 槽位的复合类型**一个
 * `__destroy_<n>(dv: ref *T)`，函数体销毁 T 值的 own 槽位（不释放值自身）。
 * 销毁点只发 `CALL __destroy_<n>(&value)`；自引用类型（链表/树）由函数自身
 * 递归调用，天然终止。type 是去限定后的类型对象（intern 后唯一）。 */
typedef struct sema_destroy_fn {
    const type_t          *type;
    ast_node_t            *fn;    /* AST_FUNC_DEF（sema_attach_destroy_fns 追加到 prog） */
    strslice_t             name;
    struct sema_destroy_fn *next;
} sema_destroy_fn_t;

/* 生成的 clone 函数（sema_clone_fn_t）：与销毁函数同构（运行时递归是合理的，
 * 扁平化属于未来优化）。三种形态，按 kind 区分：
 *   kind 0 `__clone_<n>(dv: ref *T): fatal T`        —— 值克隆（T 为复合值类型）
 *   kind 1 `__cloneptr_<n>(dv: ref *U): fatal *U`    —— 指针克隆（分配新块 + 克隆被指向值）
 *   kind 2 `__cloneslice_<n>(dv: ref []T): fatal []T`—— 切片克隆（循环逐元素 clone）
 * type 是去限定后的 key（kind 0 = T；kind 1 = 被指向类型 U；kind 2 = 切片类型 []T）。 */
typedef struct sema_clone_fn {
    const type_t          *type;
    uint8_t                kind;
    ast_node_t            *fn;    /* AST_FUNC_DEF（sema_attach_clone_fns 追加到 prog） */
    strslice_t             name;
    struct sema_clone_fn  *next;
} sema_clone_fn_t;

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

    /* **含捕获的函数**（闭包字面量 / 局部函数 / defer）：元素是 ast_func_def_t*。
       resolve_func_captures 建好 env 结构体后登记；sema_analyze 收尾时为每个条目
       生成 env 的 vtable（__destroy_<env> / __clone_<env>）并把 fid 写回 AST，
       compiler 在 hoist 随 SET_FUNC_CAPTURES 写入函数对象。 */
    vec_t        *closures;

    /* 类型队列：sema 层解析过的全部类型（resolve_type_expr 登记 + comptime
       折叠逆向登记），按 type_t 指针去重，驱动编译器 hoist 类型提升区。
       sema 拥有元素生命周期。 */
    vec_t        *types;        /* sema_type_t* */

    /* **当前函数 walk 上下文**：Pass 3 期间设置/保存恢复的全部"当前函数"
       状态（返回类型、函数定义、comptime body 标志、捕获检查上下文、
       调用点 callee 标志、循环深度）。字段语义见 sema_fn_ctx_t 文档。 */
    sema_fn_ctx_t fn;

  /* >0 表示"当前正在解析所有权修饰符（own/ref/fatal）的 base 类型"：
     AST_PTR / AST_OWNED_TYPE 的 base 位置允许 `opaque`；裸 `opaque` 是
     指针缺少所有权修饰（opaque 是 `void *` 语义），必须报错。 */
  size_t        opaque_base_ctx;
  /* comptime foreach 展开的临时变量序号（__cfcN）。 */
  uint32_t      foreach_seq;

    /* defer 改写计数器：每个 defer 语句改写为临时函数定义 __defer_N，
       N 从 0 起在 sema 生命周期内单调递增（块内唯一，跨块亦唯一——
       名字仅用于 sema/compiler 内部绑定，不暴露给用户）。 */
    uint32_t      defer_seq;

    /* 销毁序列里的生成循环计数器：`for i in 0..len(src)` 的循环变量名
       __del_i_N，N 从 0 起单调递增（块内唯一；仅 sema/compiler 内部使用）。 */
    uint32_t      destroy_seq;

    /* 扁平化 clone 的临时变量/循环变量计数器：__cl_N / __cl_i_N，N 从 0 起
       单调递增（块内唯一）。 */
    uint32_t      clone_seq;
    /* 错误处理语法糖左值临时变量计数器：__try_N。 */
    uint32_t      try_seq;

    /* ---- 生成的销毁函数（2026-10，方案 B：所有复合类型统一走函数调用）----
       每个含 own 槽位的复合类型生成一次 `__destroy_<n>(dv: ref *T)`：
         - 销毁点（作用域退出/return/break/continue/赋值点/显式销毁）只发
           `CALL __destroy_<n>(&value)`，不再内联展开整棵槽位树；
         - 函数体内部：叶子槽位 FREE、复合槽位 CALL（自引用类型 = 递归调用自身）；
         - C 后端可逐函数翻译（函数体是平铺的 FREE/CALL/循环）。
       `destroy_fns` 是注册表（按类型对象指针去重）；`destroy_fn_seq` 是命名计数；
       生成的函数在 sema_analyze 结束时统一追加到 prog->funcs（compiler 预扫描
       会为它们构造函数对象 + 全局名字绑定）。 */
    sema_destroy_fn_t *destroy_fns;
    uint32_t           destroy_fn_seq;

    /* ---- 生成的 clone 函数（2026-10，与销毁函数同构）----
       clone() 调用点只发 `CALL __clone*_<n>(...)`；函数体内部按槽位展开，嵌套
       复合/指针/切片槽位再调用对应函数 —— 自引用类型（链表/树）由函数自身
       递归，天然终止（不再需要运行期 CLONE 兜底，扁平化留给未来优化）。
       `clone_fns` 按 (kind, key type) 去重；生成的函数在 sema_analyze 收尾
       统一追加到 prog->funcs。 */
    sema_clone_fn_t *clone_fns;
    uint32_t         clone_fn_seq;



    /* **类型展开环检测**（自引用类型：链表/树）。销毁序列（gen_destroy）与
       扁平化 clone（gen_clone_expr）都按类型对象递归展开；一旦展开路径上
       再次遇到同一类型（只能经指针形成），静态展开无法穷尽 —— 该边退化为
       运行期原语（`FREE_RECURSIVE` / `CLONE`），其余部分保持逐槽位展开。
       destroy_path/clone_path 是展开路径栈（各 32 层上限，超出按环处理）。 */
    const type_t *destroy_path[32];
    uint32_t      destroy_depth;
    const type_t *clone_path[32];
    uint32_t      clone_depth;

    /* **当前正在 walk 的语句**（Pass 3b 设置，进入 walk_stmt 时保存/恢复）。
       扁平化 clone 的切片路径需要把"临时变量 + make + for 循环"语句挂到
       当前语句的 prelude 上（compiler 在语句主体之前编译）。为 NULL 表示
       不在语句上下文（如 comptime 折叠）→ clone 回退到运行期 CLONE 指令。 */
    ast_node_t   *cur_stmt;

    /* **表达式求值上下文**：匿名构造目标类型栈（见 sema_expr_ctx_t）。 */
    sema_expr_ctx_t ex;

    /* import 解析/编译回调（驱动层注入，见 sema_import_resolver_t）。
       未注入（NULL）时 import 语句报"当前编译模式不支持 import"——
       sema 因此完全不认识 driver 层与 module_t 内容。借用，sema 不拥有。 */
    struct {
        sema_import_resolver_t resolve;
        void                  *user;
    } imports;

    /* 两阶段：sema 在跨模块 struct 静态方法/属性这类必须读 runtime
       attributes 的路径上，请求 driver 提前 finalize 依赖模块。
       泛型实例仍由导入方桥接编译，提前 finalize 不丢实例。 */
    struct {
        struct module_t *(*finalize)(void *user, struct module_t *mod);
        void             *user;
    } module_finalize;

    /* **M7 泛型实例化**：实例表 + 深度计数（见 sema/sema_generic.h）。
       实例的 AST 副本追加到 prog->funcs（compiler 零感知），实例表只供
       sema 去重/诊断。depth 在每次实例化进入时 +1、离开时 -1。 */
    struct {
        struct sema_generic_instance *instances;
        uint32_t                      depth;
        uint32_t                      max_depth;
        /* 正在 walk 的泛型实例（comptime 值参数的引用要折叠为字面量；
           见 sema_generic_walk_instance）。 */
        struct sema_generic_instance *active_inst;
    } generics;

    /* sema_analyze 的程序根（借用）：实例化把合成声明追加到 prog->funcs
       尾部，compiler 的预扫描/hoist 直接看到普通声明。NULL = 未进入分析。 */
    ast_node_t *prog;

    /* M7 §31：struct 已实现的 interface 实例（P6b；见 sema_impl_t）。 */
    sema_impl_t *impls;

    /* M7 §18：当前正在 walk 的泛型实例（嵌套实例化的 caller 链）。 */
    struct sema_generic_instance *current_instance;

    /* M7 跨模块泛型：当前实例化/ walk 的**定义模块**词法环境。
       NULL = 当前模块（普通本地泛型）。module_path 是本 sema 所属模块的
       规范路径（driver 注入；用于跨模块 mangling 前缀）。 */
    struct module_t *current_module;
    const char         *module_path;

    /* `extends` 约束匹配的临时环境：约束参数名 → 当前类型实参（check 期间
       设置，匹配结束恢复）。只用于 PATTERN 里引用参数名（如 IWriter[T]）。 */
    struct {
        strslice_t       *names;
        const type_t    **types;
        size_t            count;
    } pattern_env;
} sema_t;

/** move(x) / clone(x) 的产物类型（将亡值变体，AST_MOVE 处理器用）：
 *  - 指针 own/ref → fatal *T；切片 own/ref → fatal []T；
 *  - 含 own 资源的复合类型（struct/tuple/array/option/union）→ 其 fatal 变体；
 *  - 其余（标量 / 平凡 struct / ref 切片）→ 原样返回 t（无资源可转移）。
 *  已是 fatal 的 t 原样返回（幂等）。返回 NULL = 内部构造失败。 */
const type_t *sema_fatal_variant_of(sema_t *sema, const type_t *t);

/* ---- 函数 walk 上下文的配对进入/离开 ----
 *
 * `sema_walk_function` 可被嵌套调用（`sema_check_func_literal` 在外层 walk 中
 * 调用它走函数字面量 body），因此必须"进入时整体保存、离开时整体恢复"。
 * 逐字段备份/恢复曾经漏过字段（上下文新增字段即静默失配）；用这组接口把
 * "整体快照 + 整体恢复"写成显式配对，且**进入点必须在写入任何字段之前**。 */
static inline sema_fn_ctx_t sema_fn_ctx_enter(const sema_t *sema) {
  return sema->fn;
}
static inline void sema_fn_ctx_leave(sema_t *sema, sema_fn_ctx_t saved) {
  sema->fn = saved;
}

/* ---- 匿名构造目标类型栈（anon_ct / anon_ct_depth）的配对接口 ----
 *
 * 必须成对使用：`sema_anon_push` 可能因类型为 NULL 或栈满（上限 16 层）
 * **不入栈**，返回的令牌要原样交给 `sema_anon_pop`。此前各调用点手写
 * "有条件压栈 + 无条件弹栈"（12 处如此），`ft == NULL` 或栈满时弹栈会
 * 误减外层上下文，使后续 `.{}` 推断到错误的目标类型（静默错误类型）。
 */
static inline bool sema_anon_push(sema_t *sema, const type_t *t) {
    if (!sema || !t || sema->ex.anon_ct_depth >= 16) return false;
    sema->ex.anon_ct[sema->ex.anon_ct_depth++] = t;
    return true;
}

static inline void sema_anon_pop(sema_t *sema, bool pushed) {
    if (sema && pushed && sema->ex.anon_ct_depth > 0) sema->ex.anon_ct_depth--;
}

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
 * 注入 import 解析回调（见 sema_import_resolver_t）。driver 在 sema_create 后、
 * sema_analyze 前调用。user 是回调上下文（借用，sema 不拥有）。
 */
void sema_set_import_resolver(sema_t *sema, sema_import_resolver_t resolve,
                              void *user);

/**
 * 三遍扫描：Pass 1 函数名收集 → Pass 2 类型解析（func_t 签名）→
 * Pass 3a 作用域树构建 + Pass 3b shadow VM 运行（类型检查）。
 *
 * 返回 false 表示存在语义错误（诊断已记录到 diag）。
 * 作用域树在返回后保持有效（持久化数据，交字节码编译器复用）。
 */
bool sema_analyze(sema_t *sema, ast_node_t *program);

/** 把 Pass 3b 期间按需生成的销毁函数追加到程序函数链尾部（sema_analyze 收尾调用）。
 *  compiler 预扫描据此为它们构造函数对象（LOAD_TYPE sig_id + PUSH_FUNCTION +
 *  BIND_FUNC）并在全局作用域绑定名字（LOAD_FUNCTION + DEFINE）。 */
void sema_attach_destroy_fns(sema_t *sema, ast_node_t *program);

/** 为闭包 env 类型生成/取得两个 vtable 函数（见 ast_func_def_t 的
 *  env_dispose_fid / env_clone_fid）：
 *   - `__destroy_<env>(dv: ref *Env)`（复用销毁函数生成器）→ *dispose_fid
 *   - `__clone_<env>(dv: ref *Env): fatal Env`（复用 clone 函数生成器）→ *clone_fid
 * 返回 false = 生成失败（类型不可销毁/克隆，例如 env 含 cunion 指针）。 */
/** 取/生成类型 t 的 clone 函数（三种形态之一），返回其 fid（0 = 失败）。
 *  形态：0 值克隆（dv: ref *T）、1 指针克隆（dv: ref *U）、2 切片克隆。 */
uint32_t sema_gen_clone_fn_fid(sema_t *sema, uint8_t kind, const type_t *t);

bool sema_gen_env_vtable(sema_t *sema, const type_t *env_type,
                         uint32_t *dispose_fid, uint32_t *clone_fid);

/** 把 Pass 3b 期间按需生成的 clone 函数追加到程序函数链尾部（sema_analyze 收尾）。 */
void sema_attach_clone_fns(sema_t *sema, ast_node_t *program);



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
 * 生成**类型引用表达式**（AST_TYPE_REF）：已登记类型取 sema 登记名
 * （`__type_N`）；内建标量（未登记）取规范名，compiler 的 AST_TYPE_REF 分支
 * 用 `type_lookup` 兜底。用于 sema 生成的构造 / NEW / 临时变量声明（扁平化
 * clone、显式销毁）。返回 NULL = 分配失败。
 */
ast_node_t *sema_gen_type_ref(sema_t *sema, const type_t *t);

/**
 * 把**生成语句链**追加到当前语句的 prelude（compiler 在语句主体之前、
 * 同一作用域编译）。返回 false = 不在语句上下文（如 comptime 折叠）或分配
 * 失败——调用方应回退（运行期指令 / DISPOSE）。
 *
 * 用途：扁平化 clone 的切片路径（临时变量 + make + 循环）与显式销毁
 * `_ = move(x)` 的销毁序列。
 */
bool sema_append_prelude(sema_t *sema, ast_node_t *stmts);

/**
 * Pass 3a 作用域树构建（stmt_build.c 实现）：遍历 sema->funcs 队列，对每个函数
 * 按词法块结构建树，只注册符号（名字 + 声明类型 + TDZ 标志），不做类型检查。
 * 作用域树存入 sema_func_t::scope。
 */
void sema_build_scope_tree(sema_t *sema);

/**
 * 为**单个**函数建作用域树（M7 泛型实例用）：与 sema_build_scope_tree 内部
 * 的 build_func_tree 同一实现，parent = global_scope。实例化发生在 Pass 3b
 * 期间（全局建树已结束），必须为追加的实例单独建树后才能 walk。
 */
void sema_build_func_scope(sema_t *sema, sema_func_t *sf);

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

/**
 * 槽位接收规则（§0.4/§0.5，与 own 变量定义/形参同一张表）——聚合构造、
 * `new` 字段/成员/元素初始化、字段赋值共用（expr.c 实现）：
 *   - own 槽位只接受将亡值（FATAL：new / move / clone / 构造产物 /
 *     fatal 返回值）；
 *   - 非 own 复合槽位不得用 copy 收下 OWN 聚合（own 是 move-only）。
 * 返回 true = 通过；false = 已发出诊断。`what` 是槽位的人类可读名
 * （如 "union field 'p'"），用于拼诊断。
 */
/** RC 创建通道（§12.1）：`share *T` 目标 + fatal/own `*T` 源 → 改写成
 *  `__make_share` 调用并返回 share 类型 shadow（见 expr.c 实现）。 */
bool rc_adapt_share_init(sema_t *sema, const type_t *target,
                         ast_node_t **pvalue, value_t **pv);

bool sema_check_slot_flow(sema_t *sema, ownership_t slot_own,
                          const type_t *slot_type, ast_node_t *value,
                          value_t *fv, const char *what);

/**
 * R3 借用释放（m3-design §7）：作用域退出时遍历本作用域符号，递减
 * borrow_src 的 borrow_count 并清空 borrow_src。stmt.c 各 vm_pop_scope
 * 前调用（借用载体随作用域消亡，源符号恢复可 move）。
 */
void sema_scope_release_borrows(sema_scope_t *scope);

/**
 * 作用域标注读取（m3-design §5）：type_expr 最外层是 AST_SCOPE_ANNOT 时
 * 返回其标注集合（names 指向 arena 数组，count = 个数；'<*>' = 单元素
 * "*"，global）；否则 *names=NULL, *count=0。标注节点保留在 AST（sema_
 * resolve_type_slot 折叠 sub 但保留外层标注节点），任何消费点可随时读取。
 * 嵌套标注 '<a> '<b> T：只读最外层（消费层按序收敛——内层标注属内层类型）。
 */
void sema_annot_peek(ast_node_t *type_expr, strslice_t **names, size_t *count);

/**
 * §5 强制规则（m3-design §5）：函数返回 ref *T 时返回类型必须标注作用域
 * （'<p> ref *T，集合 = 参数名 ∪ {*}）——未标注无法确定返回值存活上限，
 * 调用点无法检查逃逸。fn->return_expr 已折叠（标注节点保留），rt 是折叠
 * 出的返回类型。返回 true = 通过（非 ref 返回 / 无返回类型 / 已标注）。
 */
bool sema_check_ret_ref_annot(sema_t *sema, ast_func_def_t *fn,
                              const type_t *rt);

/**
 * 作用域嵌套比较（m3-design §5）：a 沿 parent 链可达 b（或 a == b）→
 * a 嵌套于 b。b 为 NULL 恒 true（保守放行）。用于 §5/§9 逃逸检查。
 */
bool sema_scope_within(const sema_scope_t *a, const sema_scope_t *b);

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

/** `cfunc(...)` → `func(...)`：把表达式槽位替换成生成的包装字面量（实参/初始化共用）。 */
bool sema_maybe_wrap_cfunc_expr(sema_t *sema, ast_node_t **slot,
                                const type_t *target, sema_scope_t *scope);

#ifdef __cplusplus
}
#endif
#endif
