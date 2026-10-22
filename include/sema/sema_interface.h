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

/** t 是否实现了指定 interface 实例（def + 类型实参逐位相等）。 */
bool sema_type_implements(sema_t *sema, const type_t *t,
                          const ast_node_t *iface_def,
                          const type_t *const *args, size_t arg_count);

#ifdef __cplusplus
}
#endif
#endif
