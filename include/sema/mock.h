#ifndef _H_CLUX_SEMA_MOCK_
#define _H_CLUX_SEMA_MOCK_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_call.h"
#include "sema/sema.h"
#include "vm/value.h"

/* ---- 测试 mock 内建（mock / unmock / mock_method / unmock_method）----
 *
 * 只在测试构建（`clux test`）可见。与 `len` / `data` / `take_opt` 一样是
 * **按 builtin_op 标记**判定的语言内建，但目标解析完全在编译期完成：
 *
 *   mock("target", repl)            name 必须是字符串字面量
 *   unmock("target")
 *   mock_method(T, "m", repl)       T 是类型（支持泛型实例 Box[i32]）
 *   unmock_method(T, "m")
 *
 * 名字可以写成 `mod::fn` 指向被导入模块的导出函数。replacement 必须是函数值，
 * 且签名与目标**完全一致**（方法含 self 形参）。
 *
 * 校验通过后把目标信息写进 call（mock_op / mock_fn_name / mock_owner_module），
 * C 后端据此生成函数槽的安装/恢复代码。失败时报诊断并返回 false。 */

bool sema_mock_check_call(sema_t *sema, ast_call_t *call, sema_builtin_op_t op,
                          value_t *const *arg_shadows, size_t argc);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_SEMA_MOCK_ */
