#ifndef _H_ENGINE_TEMPLATE_
#define _H_ENGINE_TEMPLATE_
#include "engine/interface.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 模板值（type_template / func_template）的 vtable 完全留空：模板只是
   "参数表 + 原始 AST" 的具名载体，实例化由上层（对 Clux 是编译期的
   sema 通道）取回 AST 后展开，engine 不参与模板语义。两个 vtable
   分实例定义，为将来独立演化留位。 */
clux_vtable_t *get_clux_type_template_vtable(void);
clux_vtable_t *get_clux_func_template_vtable(void);
#ifdef __cplusplus
}
#endif
#endif
