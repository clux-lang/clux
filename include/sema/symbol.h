#ifndef _H_CLUX_SEMA_SYMBOL_
#define _H_CLUX_SEMA_SYMBOL_
#include "core/allocator.h"
#include "core/strmap.h"
#include "core/strslice.h"
#include "core/vec.h"
#include "parser/ast_node.h"
#include "vm/type.h"
#include <stdbool.h>

/* M5: 前向声明，SEMA_SYM_MODULE 符号引用 module_t* */
struct module_t;
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * sema 侧符号表
 *
 * 独立于 VM scope：sema 需要追踪 TDZ 状态、激活状态等编译期语义概念，
 * 不属于运行时 VM scope 的职责。VM scope 仅作为 shadow value 的生命周期
 * 容器（每函数一个）。
 * =========================================================================== */

/* ---- 作用域种类 ---- */

typedef enum {
  SEMA_SCOPE_GLOBAL,   /* 全局作用域（函数名） */
  SEMA_SCOPE_FUNCTION, /* 函数作用域（参数 + 函数体顶层变量） */
  SEMA_SCOPE_BLOCK,    /* 块作用域 */
  SEMA_SCOPE_FOR,      /* for 作用域（init 变量） */
} sema_scope_kind_t;

/* ---- 符号种类 ---- */

/* 符号的类别身份（sema/ctfe/compiler 判断符号语义的权威依据，替代
   对定义 AST 节点的类型判别——func_t/sema_symbol 对外不透明，内建函数
   无定义 AST 节点，变量符号 ast 恒为 NULL，不能靠 ast 区分）。 */
typedef enum {
  SEMA_SYM_VAR,    /* 变量（含参数、全局 comptime var） */
  SEMA_SYM_FUNC,   /* 函数（用户函数 + 内建函数，如 printf） */
  SEMA_SYM_TYPE,   /* 类型定义（type 名字） */
  SEMA_SYM_MODULE, /* 模块（M5：import 别名，关联 module_t*，:: 成员访问用） */
SEMA_SYM_EXTERN_LIB, /* M6 FFI：`extern lib <名> from "..."` 的库别名（:: 成员访问用） */
SEMA_SYM_EXTERN_VAR, /* M6 FFI：`extern var <库>::<名>: T;` 的**数据符号**
                        （限定名 "<库>::<名>" 注册；可读可写，运行期 dlsym 解析） */
  SEMA_SYM_INTERFACE, /* M7 §31：interface 编译期实体（无 type_t / vtable；
                         可作 extends 约束，不能作值类型） */
} sema_symbol_kind_t;

/* ---- 语言级内建操作（sema_symbol_t.builtin_op）----
 * 0 = 普通符号。非 0 的符号是"语言内建"：sema 调用点按它做类型特判，
 * compiler 按它改发专用指令（`data` → BCODE_DATA）。**按标记判，不按名字/fid 判**。 */
typedef enum {
  SEMA_BUILTIN_NONE = 0,
  SEMA_BUILTIN_LEN,       /* len(s) -> usize（序列元信息） */
  SEMA_BUILTIN_DATA,      /* data(s) -> ref *T（取首元素指针，C ABI 适配；专用指令） */
  SEMA_BUILTIN_CFUNC_PTR, /* cfunc_ptr(f) -> opaque（clux 函数 → C 函数指针） */
  SEMA_BUILTIN_DEEP_CLONE, /* __deep_clone(v)：闭包 env 深拷贝 */
  SEMA_BUILTIN_PANIC,      /* panic(msg: str) -> void（stderr + abort；noreturn） */
  SEMA_BUILTIN_TAKE,       /* take(x: ?T) -> T（移出 optional payload） */
} sema_builtin_op_t;

/* ---- 符号 ---- */

typedef struct _sema_scope_t sema_scope_t;

/*
 * 编译期常量编码（comptime var / comptime func 调用折叠产物）
 *
 * 支持标量 + 字符串 + 数组 + 函数引用（M2 复合类型按需扩展）。
 * - 标量：type 决定读取 i/u/f/b/s 哪个字段（u64 → u，其余整数 → i，
 *   f32/f64 → f，bool → b，str → s）
 * - 数组：type->kind == TYPE_KIND_ARRAY → elems 是连续元素常量编码
 *   （arena 分配，count 个，递归），标量字段无效
 * - 函数引用：type->kind == TYPE_KIND_FUNC → func_id 是函数 id
 *   （sema 分配，内建 = 内建 id），折叠为 AST_FUNC_REF（compile 发
 *   LOAD_FUNCTION <id>；匿名字面量无名字也可折叠——与 name 无关）
 * 字符串 strslice 指向 arena 复制的缓冲区（生命周期 = sema arena，
 * 跨 sema/compile 阶段安全）。type 是 vm 类型池指针（借用，生命周期 = vm）。
 */
struct sema_ct_capture; /* 见下：闭包捕获（值常量） */

typedef struct sema_ct_const {
  const type_t *type;  /* 常量类型 */
  int64_t       i;     /* i8..i64, u8..u32（按位图存） */
  uint64_t      u;     /* u64 */
  double        f;     /* f32/f64 */
  bool          b;     /* bool */
  strslice_t    s;     /* 字符串（arena 复制） */
  uint32_t      func_id; /* 函数引用（TYPE_KIND_FUNC，sema 分配 fid） */
  /* 闭包常量（TYPE_KIND_FUNC 且带捕获）：捕获名 + 值（arena 分配）。
     captures == NULL / capture_count == 0 → 纯函数引用（无捕获）。 */
  struct sema_ct_capture *captures;
  size_t                  capture_count;
  /* 闭包常量的**字面量 AST**（`ast_func_def_t*`，借用）：折叠时据此克隆
     出 MAKE_FUNCTION + SET_CLOSURE 所需的函数字面量。NULL = 非字面量。 */
  const struct ast_node *lit_ast;
  /* 数组（TYPE_KIND_ARRAY）：arena 分配的连续元素编码 */
  struct sema_ct_const *elems; /* count 个元素常量（arena 分配） */
  size_t                count; /* 元素个数 */
} sema_ct_const_t;

/* 闭包常量的一格捕获：名字（借用 capture_type 字段名）+ 值常量。
   comptime 函数返回的闭包用它编码成 MAKE_FUNCTION + SET_CLOSURE 序列。 */
typedef struct sema_ct_capture {
  strslice_t      name;
  struct sema_ct_const value;
} sema_ct_capture_t;



/*
 * sema 侧符号表：纯编译期元数据
 *
 * 符号真正重要的是"名字"——名字是符号表映射的 key。运行态（shadow value）
 * 的 lookup/define 不经过符号表：通过与 sema 作用域树同构的 VM scope 树
 * （scope_t::vars）完成，名字从定义节点 ast 提取（ast_var_def_t::name /
 * ast_func_def_t::name），保证两棵作用域树严格对齐。
 *
 * 遮罩机制由 VM scope 链承担（scope_lookup 沿 parent 取第一个命中）。
 * TDZ（确定性赋值分析）是编译期数据流状态，由本表 flow_init 字段承载：
 * 符号表不持有运行时状态。
 *
 * 激活语义（is_active）：Pass 3a 注册的变量符号在 Pass 3b 走到定义点
 * （shadow_var_def 完成 VM scope_define）之前不可见——与 VM scope_lookup
 * 对齐，保证 `var x = x + 1` 自引用的 x 解析到外层而非自身。
 *
 * comptime 语义：is_comptime = 主动标注的编译期上下文（comptime var/func）。
 * comptime var 求值成功后 ct_valid=true + ct 编码常量，定义点从语句链
 * 摘除（不进入运行时），引用点在 sema_expr 折叠为字面量 AST 节点。
 * comptime func 调用点在 sema_expr 折叠，函数本身不注册到运行时。
 */
struct _sema_symbol_t {
  sema_symbol_kind_t kind; /* 符号种类（SYM_VAR/SYM_FUNC/SYM_TYPE） */
  const type_t *type; /* 已解析类型；NULL = 待推断（shadow VM 阶段填充）。
                         函数符号：签名类型（func_type_t，vm 池 intern）。 */
  ast_node_t   *ast;  /* 定义节点（借用，arena 管理，不拥有）：
                         函数符号 = AST_FUNC_DEF（Pass 2 填充）；
                         变量符号 = AST_VAR_DEF */
  bool flow_init;     /* 确定性赋值分析（Pass 3b）：变量是否确定已初始化。
                         false = 未初始化（TDZ），读取时编译错误
                         "used before initialization"。仅变量符号有意义。 */
  bool is_active;     /* 符号是否已定义到 VM scope（运行时可见）。函数/内置
                          符号注册即激活；变量在 shadow_var_def 定义时激活。 */
  bool is_noreturn;   /* `[[noreturn]]` 函数：调用表达式永不正常返回 */
  bool declared;      /* Pass 3b 是否已走到**定义点**（局部变量：3a 预注册全部块内
                         变量，但 3b 按源序逐个处理定义）。销毁收集
                         （make_free_stmts）只包含已声明的变量——否则提前 return
                         的路径会销毁**后面才声明**的变量（字节码 PUSH 一个尚未
                         DEFINE 的名字 → 运行期 "undefined variable"）。 */
  bool is_capture;    /* 本符号是**闭包捕获**（`func |caps|` 的捕获槽，含括号捕获
                         的参数槽）。由 resolve_func_captures 在解析捕获时置位
                         （那里的符号可能没有 own_scope，所以不能靠作用域反查）。 */
  /* 定义所在作用域：变量定义时记录所在 sema 作用域。用于捕获/局部函数的
     "引用点是否在函数自身子树内"判定（expr_ops 的函数自身符号检查）。 */
  sema_scope_t *own_scope;

  /* ---- 路径窄化已移除（2026-09-20）：.? / .! 解包方案取代 flow 窄化记录。
     符号级窄化状态（narrow 字段）与 narrow_collect/walk_if 窄化应用已删除——
     用户范式改为显式解包：if (a != nil) { var v = a.!; ... }。 ---- */
  /* ---- comptime（M2） ---- */
  bool           is_comptime; /* comptime var/func 标注 */
  /* ---- M7 泛型 ----
     泛型定义只登记符号，不是运行时实体：type 为 NULL、无 fid、无 type_t。
     使用点必须实例化（sema_generic 阶段把 mangling 实例登记为普通符号）。 */
  bool           is_generic;
  /* M7 跨模块：泛型定义的 token pool（vec_t*，借用被导入模块，模块存活期
     内有效）。NULL = 当前模块 sema->tokens。实例化重解析定义点必须用
     定义所属模块的 token pool（tok_begin/tok_end 是该 pool 的下标）。 */
  void          *generic_tokens;
  /* 跨模块泛型 mangling 前缀（如 `mlib$Box`；空 = 无）。保证不同模块的
     同名泛型实例互不冲突。 */
  strslice_t     generic_prefix;
  bool           ct_valid;    /* 已编译期求值（常量有效；comptime var 求值成功
                                 或 comptime func 调用折叠后引用点改写） */
  sema_ct_const_t ct;         /* 编译期常量编码（ct_valid 时有效） */

  /* 函数 id（SEMA_SYM_FUNC 符号）：创建函数对象时由 sema 分配
     （全局 pass1 / 局部 build_local_func / 内建预注册 = 内建 id）。
     源码 AST_IDENT 函数引用替换 AST_FUNC_REF 时取此字段；comptime
     折叠产物经 func_t->id 携带同一 fid。fid 单一来源在 sema。 */
  uint32_t       fid;

  /* 模块引用（SEMA_SYM_MODULE 符号，M5）：import 别名关联的被导入模块。
     sema 解析 alias::name 时从此字段取 module_t*，查 module->exports
     获取成员类型信息。借用 vm->modules 生命周期。 */
  struct module_t *module;

  /* ---- 结构体成员作用域（SEMA_SYM_TYPE 符号，struct 成员运行时模型）----
   * struct_type 的静态变量和方法注册在此子作用域（global_scope 的 child）。
   * `t.f()` 方法调用与 `T::x` 静态访问通过此作用域解析成员名字，不再拼
   * `T::name` 限定名查全局。NULL = 非 struct 类型 / struct 无成员。
   * 生命周期归 sema 作用域树（sema_scope_destroy 统一销毁）。 */
  sema_scope_t *member_scope;

  /* ---- 语言级内建操作标记（M6 之后）----
   * 非 0 表示该符号是"语言内建"，semas 调用点据此做类型特判（返回类型精化 /
   * 实参校验），compiler 也可能据此改发专用指令（`data` → BCODE_DATA）。
   *
   * **不要用 fid 判内建**：内建 id 随注册顺序变化（printf 从内建删除后就踩过
   * 一次），而且 `ex_call` 求值 callee 时 AST_IDENT 会被折叠成 FUNC_REF——
   * 曾经"按 callee 名字 + AST_IDENT 匹配"的 upgrade/len 特判因此**从未生效**。
   * 标记跟着**符号**走，与节点形态和 id 顺序都无关。 */
  uint8_t        builtin_op;
};
typedef struct _sema_symbol_t sema_symbol_t;

/* ---- 作用域 ---- */

struct _sema_scope_t {
  struct _sema_scope_t *parent;
  allocator_t    *alloc;    /* 借用调用方的 allocator（内部操作自取） */
  vec_t          *children; /* sema_scope_t* 子作用域（按出现顺序，不拥有） */
  strmap_t       *symbols;  /* name -> sema_symbol_t*（owns_value=true） */
  vec_t          *defers;   /* ast_node_t* 借用（AST arena 托管，实际为
                               ast_func_def_t*——base 是 ast_node_t 首字段）：
                               defer 改写为临时函数定义后登记于此，供作用域
                               退出点（块尾/return/break/continue）逆序插入
                               call 语句（Pass 3a build_block 收集） */
  sema_scope_kind_t kind;
  /* 该作用域对应的**块/语句 AST**（Pass 3a 建树时填）：`comptime foreach`
     展开需要在 3a 里查"容器变量在同一块/外层块中的声明类型"，而 3b 之前
     推断型 var（`var a = .[3]i32{…}`）的符号 type 还是 NULL——只能回看 AST。 */
  ast_node_t     *block_ast;
};
typedef struct _sema_scope_t sema_scope_t;

/* ---- 生命周期 ---- */

/**
 * 创建新作用域。parent 可为 NULL（全局作用域）。
 * Panics on out-of-memory. Returns NULL for invalid arguments.
 */
sema_scope_t *sema_scope_new(allocator_t *alloc, sema_scope_kind_t kind,
                             sema_scope_t *parent);

/**
 * 递归销毁整棵作用域子树（children、symbols、每个符号的 ast）。
 * 作用域树是持久化数据：sema 结束后不销毁，由字节码编译器复用，
 * 编译完成后由调用方（driver / 测试）调用本函数释放。
 * No-op if `scope` or `*scope` is NULL.
 */
void sema_scope_destroy(sema_scope_t **scope);

/* ---- 树结构 ---- */

/** 追加子作用域（按出现顺序）。No-op if `scope` or `child` is NULL. */
void sema_scope_add_child(sema_scope_t *scope, sema_scope_t *child);

/** 返回子作用域数量。 */
size_t sema_scope_children_count(const sema_scope_t *scope);

/** 按序取第 idx 个子作用域，越界返回 NULL。 */
sema_scope_t *sema_scope_child(const sema_scope_t *scope, size_t idx);

/* ---- 符号操作 ---- */

/**
 * 定义符号到当前作用域（name 按 slice 拷贝为 NUL 终止字符串存储）。
 * `init` 按值拷贝构造符号（type/ast 等字段）。
 * 同作用域已有同名符号 → 返回 NULL（重复定义，由调用方报诊断）。
 * Panics on out-of-memory.
 */
sema_symbol_t *sema_scope_define(sema_scope_t *scope, strslice_t name,
                                 const sema_symbol_t *init);

/**
 * 沿 parent 链查找符号，返回第一个命中（不设遮罩过滤——遮罩由 VM scope
 * 链承担；本函数仅供编译期元数据查找：函数名解析、测试断言）。
 * 未找到返回 NULL。
 */
sema_symbol_t *sema_lookup(const sema_scope_t *scope, strslice_t name);

/**
 * 仅在当前作用域直接查找符号（不沿 parent 链）。
 * 供 Pass 3b 操作 Pass 3a 注册的符号（type 推断写回）。未找到返回 NULL。
 */
sema_symbol_t *sema_scope_find_local(const sema_scope_t *scope,
                                     strslice_t name);

/* ---- defer 登记/收集（Pass 3a build_block 用） ---- */

/** 登记一个 defer 临时函数定义到当前作用域的 defers 向量（借用指针，
    AST arena 托管）。作用域退出点逆序调用。 */
void sema_scope_add_defer(sema_scope_t *scope, ast_node_t *fn);

/** 从 from_scope 沿 parent 链向上到 stop_scope（不含）收集沿途各层 defers，
    逆序（从内到外）写入 out 数组。返回总数。out 容量由 cap 限定。
    用于 return/break/continue 前插入逆序 call 序列。 */
size_t sema_scope_collect_defers(sema_scope_t *from_scope,
                                 const sema_scope_t *stop_scope,
                                 ast_node_t **out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
