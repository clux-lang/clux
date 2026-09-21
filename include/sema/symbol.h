#ifndef _H_CLUX_SEMA_SYMBOL_
#define _H_CLUX_SEMA_SYMBOL_
#include "core/allocator.h"
#include "core/strmap.h"
#include "core/strslice.h"
#include "core/vec.h"
#include "parser/ast_node.h"
#include "vm/type.h"
#include "vm/scope_frame.h"
#include <stdbool.h>
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
  SEMA_SYM_VAR,  /* 变量（含参数、全局 comptime var） */
  SEMA_SYM_FUNC, /* 函数（用户函数 + 内建函数，如 printf） */
  SEMA_SYM_TYPE, /* 类型定义（type 名字） */
} sema_symbol_kind_t;

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
typedef struct sema_ct_const {
  const type_t *type;  /* 常量类型 */
  int64_t       i;     /* i8..i64, u8..u32（按位图存） */
  uint64_t      u;     /* u64 */
  double        f;     /* f32/f64 */
  bool          b;     /* bool */
  strslice_t    s;     /* 字符串（arena 复制） */
  uint32_t      func_id; /* 函数引用（TYPE_KIND_FUNC，sema 分配 fid） */
  /* 数组（TYPE_KIND_ARRAY）：arena 分配的连续元素编码 */
  struct sema_ct_const *elems; /* count 个元素常量（arena 分配） */
  size_t                count; /* 元素个数 */
} sema_ct_const_t;

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
  /* ---- 路径窄化已移除（2026-09-20）：.? / .! 解包方案取代 flow 窄化记录。
     符号级窄化状态（narrow 字段）与 narrow_collect/walk_if 窄化应用已删除——
     用户范式改为显式解包：if (a != nil) { var v = a.!; ... }。 ---- */
  /* ---- comptime（M2） ---- */
  bool           is_comptime; /* comptime var/func 标注 */
  bool           ct_valid;    /* 已编译期求值（常量有效；comptime var 求值成功
                                 或 comptime func 调用折叠后引用点改写） */
  sema_ct_const_t ct;         /* 编译期常量编码（ct_valid 时有效） */

  /* 函数 id（SEMA_SYM_FUNC 符号）：创建函数对象时由 sema 分配
     （全局 pass1 / 局部 build_local_func / 内建预注册 = 内建 id）。
     源码 AST_IDENT 函数引用替换 AST_FUNC_REF 时取此字段；comptime
     折叠产物经 func_t->id 携带同一 fid。fid 单一来源在 sema。 */
  uint32_t       fid;
};
typedef struct _sema_symbol_t sema_symbol_t;

/* ---- 作用域 ---- */

struct _sema_scope_t {
  struct _sema_scope_t *parent;
  scope_frame_t  frame;      /* 父子关系节点（与 parent 同步，value 借用） */
  allocator_t    *alloc;    /* 借用调用方的 allocator（内部操作自取） */
  vec_t          *children; /* sema_scope_t* 子作用域（按出现顺序，不拥有） */
  strmap_t       *symbols;  /* name -> sema_symbol_t*（owns_value=true） */
  sema_scope_kind_t kind;
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

#ifdef __cplusplus
}
#endif
#endif
