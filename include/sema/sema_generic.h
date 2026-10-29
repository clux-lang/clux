#ifndef _H_CLUX_SEMA_SEMA_GENERIC_
#define _H_CLUX_SEMA_SEMA_GENERIC_

#include "core/string.h"
#include "sema/sema.h"
#include "vm/type.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * M7 泛型单态化（P2：泛型函数 + 显式类型实参）
 *
 * 实例化模型（docs/m7-implementation-plan.md §1.1）：
 *   1. 从**原始 token 区间重新解析**泛型定义 → 一份全新的 AST
 *      （等价于深拷贝，且天然不含上一轮 sema 的折叠状态）；
 *   2. 在一个**临时 VM 作用域**里把类型参数绑定为 type value
 *      （类型即表达式——`T` 在签名/函数体里按普通类型名解析）；
 *   3. 实例按普通函数走既有流水线：签名解析 → 作用域树 → shadow walk；
 *   4. 合成声明追加到 prog->funcs，compiler 完全不知道"泛型"的存在。
 *
 * 因此**不需要** AST 克隆器或类型槽位替换器：签名/变量类型槽位在
 * sema_resolve_type_slot 里被折叠成 AST_TYPE_REF，函数体在 walk 中同样折叠，
 * compiler 看到的已经是具体类型。
 * =========================================================================== */

/* 实例种类（缓存的 mangling 名同表，kind 仅用于诊断/一致性自检）。 */
typedef enum {
    SEMA_GEN_FUNC = 0,
    SEMA_GEN_STRUCT,
    SEMA_GEN_ENUM,
    SEMA_GEN_UNION,
    SEMA_GEN_ALIAS,
} sema_generic_kind_t;

/* 一个泛型实参：类型实参或 comptime 值实参（P5）。
 * - is_value=false：type = 具体类型；
 * - is_value=true ：type = 值参数声明类型，value = 编译期常量（sema_ct_const_t）。 */
typedef struct {
    bool             is_value;
    const type_t    *type;
    sema_ct_const_t  value;
} sema_generic_arg_t;

/* 一个已实例化（或在途）的泛型实例。字符串/实参数组由 sema arena 托管，
   结构本身由 allocator 分配、随 sema 销毁。 */
typedef struct sema_generic_instance {
    strslice_t            name;      /* mangling 名（arena，如 "id$i32"） */
    uint8_t               kind;      /* sema_generic_kind_t */
    const ast_node_t     *def;       /* 原始泛型定义（借用） */
    strslice_t           *param_names; /* arena 数组：参数名（按序） */
    size_t                param_count;
    sema_generic_arg_t   *args;      /* arena 数组：类型/值实参（按序；
                                        pack 元素紧跟在 fixed 实参之后） */
    size_t                arg_count;
    /* P5b 参数包：param_names/param_count 只覆盖 fixed 参数；pack 元素从
       pack_offset（== fixed count）起，共 pack_count 个。 */
    bool                  has_pack;
    bool                  pack_is_value; /* P9：comptime 值参数包（...N: comptime） */
    strslice_t            pack_name;
    strslice_t            pack_param_name; /* 变参形参名 `args`（...Args） */
    size_t                pack_offset;
    size_t                pack_count;
    ast_node_t           *instance;  /* 合成声明（prog->funcs 中，arena；
                                        类型别名实例 = NULL） */
    sema_func_t          *func;      /* sema 函数记录（仅函数实例） */
    const type_t         *sig;       /* 实例签名（函数实例） */
    const type_t         *type;      /* 类型实例产物（struct/enum/union/alias） */
    uint32_t              fid;       /* 实例函数 id（compiler LOAD_FUNCTION） */
    bool                  in_progress; /* 函数体 walk / 类型构造在途（环检测） */
    uint32_t              depth;     /* 实例化深度（诊断用） */
    struct sema_generic_instance *parent; /* P4b：泛型 struct 实例（方法实例
                                         需要先绑定外层 T 再绑定自身参数） */
    void                 *tokens;    /* vec_t*：定义所属模块 token pool
                                       （跨模块实例；NULL = 当前模块） */
    /* §18 诊断：实例化调用点 + 调用者实例（嵌套实例化链）。 */
    location_t            site;
    struct sema_generic_instance *caller;
    /* M7 归属：泛型定义所在模块。实例体的名字解析/root_scope 都应按它。 */
    struct module_t      *owner_module;
    struct sema_generic_instance *next;
} sema_generic_instance_t;

/** 泛型实例化深度上限（与 CTFE max_depth 一致，设计 §7）。 */
#define SEMA_GENERIC_MAX_DEPTH 128u

/**
 * 实例化泛型函数。
 *
 * generic_sym 必须是 SEMA_SYM_FUNC + is_generic，ast 为 AST_FUNC_DEF。
 * type_args 已由调用方用 resolve_type_expr 求值（具体 type_t*）。
 * site 是使用点 AST（仅用于诊断位置，可为 NULL）。
 *
 * 成功返回实例签名类型，*out 写入实例记录（可为 NULL 指针忽略）。
 * 失败返回 NULL（诊断已记录）。
 */
const type_t *sema_generic_instantiate_func(sema_t *sema,
                                            sema_symbol_t *generic_sym,
                                            const sema_generic_arg_t *args,
                                            size_t arg_count,
                                            const ast_node_t *site,
                                            const sema_generic_instance_t **out);

/**
 * 调用点推导 + 实例化（M7 P3）。
 *
 * explicit_args 是显式写出的类型实参（`f[i32, ...]` 的前 explicit_count 个，
 * 可为 NULL/0）；arg_shadows 是**已求值**的实参（调用方保证只求值一次）。
 * 剩余类型参数按形参类型表达式与实参类型做结构化匹配（含所有权不对称：
 * fatal 可匹配 own，own/fatal 可匹配 ref），最后用默认值补齐。
 *
 * 成功返回实例签名，*out 写入实例；失败返回 NULL（诊断已记录）。
 */
const type_t *sema_generic_instantiate_inferred(
    sema_t *sema, sema_symbol_t *generic_sym,
    const sema_generic_arg_t *explicit_args, size_t explicit_count,
    value_t *const *arg_shadows, size_t argc, const ast_node_t *site,
    const sema_generic_instance_t **out);

/**
 * 实例化泛型**类型**（struct / enum / union / type 别名）。
 *
 * 类型上下文**不做推导**（设计 §4）：type_args 必须由调用方按显式
 * `Name[...]` 求值得到，数量必须与泛型参数表一致（默认值 P4 后续补齐）。
 * 返回实例类型（类型池单例）；失败返回 NULL（诊断已记录）。
 *
 * struct 实例支持递归（字段经指针引用自身）：实例在字段解析前先物化成
 * **开放类型对象**，字段里的 `Node[T]` 命中缓存直接拿到开放对象。
 */
const type_t *sema_generic_instantiate_type(
    sema_t *sema, sema_symbol_t *generic_sym, const sema_generic_arg_t *args,
    size_t arg_count, const ast_node_t *site,
    const sema_generic_instance_t **out);

/**
 * 把显式实参表达式（`f[...]` 的 indices 链）按泛型参数表解析成
 * sema_generic_arg_t 数组：类型参数走 resolve_type_expr，comptime 值参数走
 * CTFE + sema_ct_encode（并转换到声明类型）。调用方负责释放返回数组。
 * 返回 false = 失败（诊断已记录）；`*out_count` 是实际解析个数。
 */
bool sema_generic_parse_explicit_args(sema_t *sema, sema_symbol_t *generic_sym,
                                      ast_node_t *indices, size_t *out_count,
                                      sema_generic_arg_t **out_args);

/** 当前 active 实例的**类型参数包变参形参**是否名为 name。 */
bool sema_generic_pack_is_param(sema_t *sema, strslice_t name);

/** 当前 active 实例的 **comptime 值参数包**是否名为 name。 */
bool sema_generic_value_pack_is_param(sema_t *sema, strslice_t name);

/** 生成值参数包第 index 个元素的字面量表达式（int/bool/str）。 */
ast_node_t *sema_generic_value_pack_elem_expr(sema_t *sema,
                                              const sema_generic_instance_t *inst,
                                              size_t index, uint32_t tb, uint32_t te);

/** 值参数包元素类型组成的 tuple 类型表达式 `<T0,...>`。 */
ast_node_t *sema_generic_value_pack_tuple_type_expr(
    sema_t *sema, const sema_generic_instance_t *inst, uint32_t tb, uint32_t te);

/** 生成参数包元素表达式 `__pack_arg_i`；move_own 时对含 own 槽位的元素
 *  自动包 `move(...)`（供展开 / 裸 args 元组构造使用）。 */
ast_node_t *sema_generic_pack_arg_expr(sema_t *sema,
                                       const sema_generic_instance_t *inst,
                                       size_t index, uint32_t tb, uint32_t te);

/** 构造 `<...Args>{ __pack_arg_... }` 表达式（裸 args 的显式 tuple 值）。 */
ast_node_t *sema_generic_pack_tuple_expr(sema_t *sema,
                                         const sema_generic_instance_t *inst,
                                         uint32_t tb, uint32_t te);

/** 构造 `<...Args>` 类型表达式（`.{ ...args }` 的显式目标类型）。 */
ast_node_t *sema_generic_pack_tuple_type_expr(
    sema_t *sema, const sema_generic_instance_t *inst, uint32_t tb, uint32_t te);

/** 两阶段：walk 本模块所有尚未 walked 的泛型实例（含 walk 过程中新增的）。
 *  返回 true = 至少推进了一个实例（调用方据此跨模块循环到耗尽）。 */
bool sema_generic_drain_pending(sema_t *sema);

/** 按 mangling 名查实例（未找到返回 NULL）。 */
const sema_generic_instance_t *sema_generic_find(sema_t *sema,
                                                 strslice_t name);

/** 按类型单例反查类型实例（struct/enum/union）；未找到 NULL。 */
const sema_generic_instance_t *
sema_generic_find_type_instance(sema_t *sema, const type_t *t);

/**
 * P4b：实例化泛型**方法**（方法自身带类型参数，struct 参数由 owner 绑定）。
 *
 * arg_shadows 是 `[接收者, 实参...]`（调用方保证只求值一次）；方法类型参数
 * 从接收者/实参推导（不支持显式方法类型实参）。owner 可为 NULL（非泛型
 * struct，只绑方法自身参数）。实例以 `Owner$method$args` mangling 登记，
 * 由调用点直接 CALL 到实例 fid（绕过 MEMBER_CALL 的按名分派）。
 */
const type_t *sema_generic_instantiate_method_inferred(
    sema_t *sema, sema_symbol_t *method_sym,
    const sema_generic_instance_t *owner, strslice_t owner_name,
    value_t *const *arg_shadows, size_t argc, const ast_node_t *site,
    const sema_generic_instance_t **out);

/** 显式类型实参的泛型方法调用 `v.method[U](...)`。 */
const type_t *sema_generic_instantiate_method_explicit(
    sema_t *sema, sema_symbol_t *method_sym,
    const sema_generic_instance_t *owner, strslice_t owner_name,
    const sema_generic_arg_t *args, size_t arg_count,
    value_t *const *arg_shadows, size_t argc, const ast_node_t *site,
    const sema_generic_instance_t **out);

/**
 * Pass 3b 队列循环：walk 一个泛型实例函数体。
 *
 * 实例化只做「签名 + 作用域树」并把函数追加到 sema->funcs 队列；函数体由
 * 队列循环**迭代** walk（不递归进实例化调用栈）。本函数负责：
 *   - 重新压入类型参数绑定层（把 type_t 作为 type value 定义到 VM scope）；
 *   - 把 sema->generics.depth 临时设为 inst->depth（嵌套推导的深度基准）；
 *   - walk 完成后恢复深度并弹出绑定层。
 */
void sema_generic_walk_instance(sema_t *sema, sema_func_t *sf);

/** 释放实例表（sema_destroy 调用）。 */
void sema_generic_destroy(sema_t *sema);

/** 类型实参的规范 mangling 编码（供诊断/缓存键复用）。 */
void sema_generic_mangle_type(string_t *out, const type_t *t);

/**
 * `Type[args]` 作为 `::` 限定访问的左侧时，实例化类型并把槽位改写成
 * AST_IDENT(实例运行时名)；这样静态方法/静态属性的既有解析路径（按名字查
 * 全局 type 符号）无需特殊分支。返回 true = 已处理（成功或已诊断）。
 */
bool sema_generic_rewrite_type_expr(sema_t *sema, ast_node_t **slot);

#ifdef __cplusplus
}
#endif
#endif
