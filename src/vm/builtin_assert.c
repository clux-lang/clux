#include "vm/vm.h"
#include "vm/function.h"
#include "vm/value.h"
#include "vm/type.h"
#include "vm/type_func.h"

#include <stdio.h>
#include <string.h>

/* assert(cond: bool, expr: str) -> void
 *
 * 仅 test 上下文可用。失败时打印被断言的表达式并返回错误值，让运行器
 * 把当前用例标为 FAILED。错误传播会展开调用帧；测试模式的 defer 注册
 * 与清理由 VM 错误路径负责。 */
static value_t *builtin_assert_cfunc(vm_t *vm, func_t *self, size_t argc,
                                     value_t **args) {
    (void)self;
    if (!vm->in_test)
        return value_make_error(vm, "assert: not in test context");
    if (argc != 2 || !args[0] || !args[1])
        return value_make_error(vm, "assert: expects (bool, str)");
    const char *text = "";
    if (type_kind_of(value_type(args[1])) == TYPE_KIND_STR &&
        value_data(args[1])) {
        const char *s = *(const char *const *)value_data(args[1]);
        if (s) text = s;
    }
    if (type_kind_of(value_type(args[0])) != TYPE_KIND_BOOL ||
        !value_data(args[0]))
        return value_make_error(vm, "assert: condition is not bool");
    if (*(const bool *)value_data(args[0])) return NULL;

    printf("assert failed: %s\n", text);
    fflush(stdout);
    vm->test_failed = true;
    return value_make_error(vm, "assert failed");
}

void vm_register_assert(vm_t *vm) {
    if (!vm || !vm->global_scope) return;
    const type_t *params[2] = {vm->type_bool, vm->type_str};
    const type_t *sig = type_func_sig(vm, params, 2, vm->type_void, false);
    if (!sig) return;
    value_t *fnv = func_new(vm, builtin_assert_cfunc, NULL, vm->root_scope, sig,
                            STRSLICE_LIT("assert"));
    if (!fnv) return;
    scope_define(vm, vm->global_scope, "assert", fnv);
}
