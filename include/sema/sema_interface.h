#ifndef _H_CLUX_SEMA_SEMA_INTERFACE_
#define _H_CLUX_SEMA_SEMA_INTERFACE_

#include "parser/ast_node.h"
#include "parser/ast_program.h"
#include "sema/sema.h"
#include "vm/type.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * M7 §31：校验 struct 的 `implements` 子句（必须在 pass2 之后调用——方法签名
 * 已解析、member_scope 已建立）。只检查非泛型 struct 定义；泛型 struct 的实例
 * 在实例化完成成员签名后调用 sema_check_struct_implements（绑定层在线）。
 */
void sema_check_implements_pass(sema_t *sema, ast_program_t *prog);

/** 校验一个 struct 类型对 implements 链的每个接口；成功记录 impl 表。 */
bool sema_check_struct_implements(sema_t *sema, const type_t *struct_type,
                                  ast_node_t *implements,
                                  const ast_node_t *site);

/**
 * E1.2 结构匹配：`extends I[...]` 的满足条件不是"写了 implements"，而是
 * **类型提供了接口要求的全部成员/方法（名字 + 签名）**。本函数用已解析的
 * 接口符号 + 具体类型实参检查形状；silent 模式（本 API）不产生诊断，
 * 只把第一个缺失成员写进 out_missing/out_attr（供约束诊断使用）。
 */
bool sema_interface_shape_check(sema_t *sema, sema_symbol_t *iface_sym,
                                const type_t *const *args, size_t arg_count,
                                const type_t *st, strslice_t *out_missing,
                                bool *out_attr);

/**
 * 把 `extends I[...]` 形态的**约束模式**解析成（接口符号, 具体类型实参）。
 * 支持的实参形态：已绑定的模式名（`T`）、具体类型名、类型表达式。
 * 返回 false = 不是接口约束形态（调用方回退到普通模式匹配）。
 */
bool sema_interface_constraint_args(sema_t *sema, ast_node_t *pat,
                                    sema_symbol_t **out_sym,
                                    const type_t ***out_args, size_t *out_count);

/** 便利入口：`pat` 是接口约束模式时直接做形状检查。 */
bool sema_interface_shape_check_pat(sema_t *sema, ast_node_t *pat,
                                    const type_t *st, strslice_t *out_missing,
                                    bool *out_attr);

/** t 是否实现了指定 interface 实例（def + 类型实参逐位相等）。 */
bool sema_type_implements(sema_t *sema, const type_t *t,
                          const ast_node_t *iface_def,
                          const type_t *const *args, size_t arg_count);

#ifdef __cplusplus
}
#endif
#endif
