#ifndef _H_CLUX_SEMA_COMPTIME_
#define _H_CLUX_SEMA_COMPTIME_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_call.h"
#include "parser/ast_func_def.h"
#include "parser/ast_node.h"
#include "parser/ast_type_def.h"
#include "parser/ast_var_def.h"
#include "sema/sema.h"
#include "sema/symbol.h"

/**
 * comptime 与 sema → CTFE 集成（M2 第一阶段）
 *
 * sema 阶段直接解释 AST 产生真实 value（非 shadow），成功则把表达式
 * **折叠为字面量 AST 节点**（arena 分配），使编译器零感知：
 *   - comptime var：定义点求值 → 符号表编码常量 → 定义点从语句链摘除；
 *     引用点（AST_IDENT）折叠为字面量。
 *   - comptime func：调用点（AST_CALL）实参改写后 ctfe 求值 → 整个调用
 *     折叠为字面量；函数本身不注册到运行时（编译器跳过注册）。
 *
 * 生命周期：ctfe 中间值 track 到 vm->current_scope（sema 阶段临时 scope，
 * 随函数 walk pop 销毁）；编码到符号表的标量已提取，字符串复制到
 * sema arena（跨阶段安全）。折叠出的字面量节点归 sema arena 管理。
 */

/* ---- 工具 ---- */

/**
 * 编码真实 value → 编译期常量（标量/字符串）。字符串复制到 sema arena。
 * 返回 false = 类型不可折叠（void/type/func/复合，M2 复合后置）。
 */
bool sema_ct_encode(sema_t *sema, value_t *v, sema_ct_const_t *out);

/**
 * 编译期常量 → 字面量 AST 节点（arena 分配，kind/tok 继承 origin）。
 * 返回 NULL = 类型不可折叠（调用方报错）。
 */
ast_node_t *sema_ct_lit(sema_t *sema, const ast_node_t *origin,
                        const sema_ct_const_t *ct);

/* ---- comptime 求值 ---- */

/**
 * 求值 comptime var 定义：改写 init 中的 comptime 引用 → ctfe 求值 →
 * 编码符号表（ct_valid）。失败返回 false（诊断已记录）。
 * 调用方负责从语句链摘除定义节点。
 */
bool sema_eval_comptime_var(sema_t *sema, ast_var_def_t *vd,
                            sema_scope_t *scope);

/**
 * 求值全局变量定义（运行时实体，init 编译期折叠）：
 * 改写 init 中的 comptime 引用 → ctfe 求值 → 折叠 init 为字面量/
 * 函数引用（sema_ct_lit 写回 vd->init，compiler 发射字面量字节码）。
 * 与 comptime var 的关键差异：不编码符号表（引用点不折叠——运行期
 * 经 root_scope 读取变量），不设 is_comptime/ct_valid。
 * 失败返回 false（诊断已记录）；调用方摘除失败节点防级联。
 */
bool sema_eval_global_var(sema_t *sema, ast_var_def_t *vd,
                          sema_scope_t *scope);

/**
 * 求值 comptime func 调用：改写实参引用 → ctfe 求值整个调用 →
 * 折叠 *node 为字面量。返回常量 shadow value；
 * 失败返回 void shadow（错误恢复产物，诊断已记录）。
 */
value_t *sema_eval_comptime_call(sema_t *sema, ast_node_t **node,
                                 sema_scope_t *scope);

/**
 * 求值 type 定义（type name = <type-expr>;）：
 *  rhs 经 sema_expr 求值（类型表达式恒返回真实 type value，data=type_t*）→
 *  校验为类型值 → 折叠 rhs 为 AST_TYPE_REF（内建类型保持原 AST_IDENT）→
 *  绑定 type value 到编译期 vm 当前作用域 → 激活符号。
 *  失败返回 false（诊断已记录）。调用方不摘除定义节点（进入字节码）。
 */
bool sema_eval_type_def(sema_t *sema, ast_type_def_t *td, sema_scope_t *scope);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_SEMA_COMPTIME_ */
