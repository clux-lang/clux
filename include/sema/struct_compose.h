#ifndef _H_CLUX_SEMA_STRUCT_COMPOSE_
#define _H_CLUX_SEMA_STRUCT_COMPOSE_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/ast_program.h"
#include "parser/ast_struct_def.h"
#include "sema/sema.h"
#include "sema/symbol.h"
#include "vm/type.h"
#include <stddef.h>

/* ===========================================================================
 * 结构体组合（`...Base;`）——**目标类型 seal 之前**的类型层字段组合
 *
 * 语法面由 parser 记录为 AST_STRUCT_EMBED（名字 + 在自己第几个字段之前），
 * AST 里**不**拼接字段。组合在 sema 的类型构造窗口里发生（`sema_eval_struct_def`，
 * 类型还是**开放**对象时）：
 *   遍历被嵌入结构体（Base）**已 sealed** 的 type 字段表（含未来装饰器对它的
 *   修改），按 `...Base` 的位置插入到本结构体的开放类型字段表，登记嵌入表，
 *   然后**只 seal 一次**。
 * 语义与不变量见 src/sema/struct_compose.c 顶部说明。
 * =========================================================================== */

/** 组合结果（三态）：把"依赖尚未就绪（下一轮再试）"与"真错误（已诊断）"
 *  区分开——前者不能报错也不能把符号标成已就绪，后者必须诊断且不激活。 */
typedef enum {
  SEMA_COMPOSE_OK = 0,    /* 组合完成（结构体类型已 seal 并登记） */
  SEMA_COMPOSE_DEFERRED,  /* 依赖（`...Base` 的类型）本轮还没就绪：静默延后 */
  SEMA_COMPOSE_FAILED,    /* 非法：诊断已记录（未知/非结构体/循环/字段冲突…） */
} sema_compose_result_t;

/**
 * 求值一个结构体定义并（若有 `...Name;`）在 seal 前完成组合。
 *
 * 这是 sema 侧 struct 类型构造的**唯一入口**（全局 pass1b 与局部块提升都走它）：
 *   解析字段类型（未就绪则静默延后）→ 开放类型（type_struct_alloc_open）→
 *   逐字段 type_struct_add_field（重复字段名报错）→ 递归求值各 `...Base`
 *   （尚未 seal 就地先求值，环则报错）→ `sema_eval_struct_def` 的宿主路径
 *   完成 type_struct_insert_embed_fields + type_struct_set_embeds + 一次 seal
 *   → 登记 + 绑定 type value + 激活符号。
 *
 * 因此**组合必然发生在 seal 之前**，seal 之后不再改动类型（VM 写入 API 拒绝
 * sealed 类型，见 include/vm/type_struct.h 的不变量）。
 * 幂等：符号已激活（类型已建好）直接返回 OK（不动点轮次会反复调用）。
 */
sema_compose_result_t sema_struct_compose_def(sema_t *sema,
                                              ast_struct_def_t *sd,
                                              sema_scope_t *scope);

/** 结构体定义体内是否有 `...Name;` 组合声明。 */
bool ast_struct_def_has_embed(const ast_struct_def_t *sd);

/**
 * 结构体组合遍历：先做嵌入环检测（DFS，报"循环嵌入"），再按"嵌入依赖优先"
 * 的顺序求值 program 顶层**尚未求值**的结构体定义（同一定义被反复尝试直到
 * 不再有进展），使 `...Base` 在源码里的书写顺序不影响结果（Base 声明在 Test
 * 之后也可用）。
 *
 * 环由本函数报出；不会无限循环（环检测 + "无进展即停"），也不是死循环：
 * 环里的定义永远等不到依赖就绪，只能靠环检测把错误说出来。
 * 返回本轮**新完成**的定义数（0 = 无进展）。
 *
 * 环诊断只在**首次**发现时报出（`ast_struct_def_t::cycle_reported` 只增不清）
 * ——sema 的试跑轮会反复调用本函数，否则同一组诊断会打印多遍。
 * sema_analyze 在正式轮之前调用 sema_struct_compose_reset_reports 复位标记，
 * 保证正式轮（诊断真正被采纳的那一轮）一定会报出来。
 */
size_t sema_struct_compose_pass(sema_t *sema, ast_program_t *prog,
                                sema_scope_t *scope);

/** 复位"环已诊断"标记（只增标记的清零点）：正式轮之前调用，保证正式轮重新
 *  检测并报出循环嵌入。 */
void sema_struct_compose_reset_reports(ast_program_t *prog);

/**
 * 结构体成员名字查找（含**嵌入继承** + 新 struct 的 attribute **覆盖**）：
 * `T::name` 的解析入口。
 *
 * 查找顺序：先 `T::name` 本身；命中即返回——即"新 struct 自己声明的 attribute
 * （`var` 静态属性 / `func` 方法 / 字段之外的命名空间成员）**覆盖**继承来的同名
 * attribute"，覆盖后自己优先。否则按 `...` 出现顺序在嵌入链（传递、按序、
 * 按类型指针去重）里找：
 *   - 恰好命中一个 → 返回成员名（sema arena 分配）——调用方在命中的类型
 *     的 member_scope 中查找符号，**未覆盖时 Base 的成员只有一份**；
 *   - 命中多个 → 报歧义诊断（说明名字与涉及的 Base）并返回 NULL——**只**发生在
 *     "自己没有、而多个嵌入都提供同名 attribute"的情形；
 *   - 零命中 → 返回 NULL（**不报错**，沿用调用点既有的诊断）。
 *
 * 字段是唯一例外：字段不参与覆盖，重名由组合阶段的冲突检查报错。
 * `tnode` 只用于诊断位置（可为 NULL）。`t` 允许带 const/volatile 限定。
 */
strslice_t *sema_struct_member_lookup(sema_t *sema, const type_t *t,
                                      strslice_t name,
                                      ast_node_t *tnode);

/**
 * 在 struct 类型的 member_scope 中查找成员符号（含**嵌入继承**递归）。
 *
 * 查找顺序：先自己 member_scope（`find_own_member`）；命中即返回——
 * 自声明的 attribute 覆盖继承来的同名成员。否则沿嵌入链递归查找
 * （`collect_member_sources`，传递、按序、按类型指针去重）：
 *   - 恰好命中一个 → 返回成员符号借用指针，`out_owner` 写命中的 struct 类型；
 *   - 命中多个 → 报歧义诊断并返回 NULL；
 *   - 零命中 → 返回 NULL（不报错）。
 *
 * 与 `sema_struct_member_lookup` 的区别：后者只返回成员名（strslice），
 * 本函数返回成员符号本身 + 所属类型，供调用方直接取签名/类型。
 * `t` 允许带 const/volatile 限定。`tnode` 只用于诊断位置（可为 NULL）。
 */
sema_symbol_t *sema_struct_find_member(sema_t *sema, const type_t *t,
                                       strslice_t name, ast_node_t *tnode,
                                       const struct_type_t **out_owner);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_SEMA_STRUCT_COMPOSE_ */
