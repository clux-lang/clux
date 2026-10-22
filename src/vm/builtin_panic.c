#include "vm/vm.h"
#include "vm/function.h"
#include "vm/value.h"
#include "vm/type.h"
#include "vm/type_func.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* panic(msg: str) -> void：错误处理语法糖的终止分支。
   打印到 stderr 后 abort；它不是普通返回函数（sema 标 is_noreturn）。 */
static value_t *builtin_panic_cfunc(vm_t *vm, func_t *self, size_t argc,
                                    value_t **args) {
    (void)self;
    const char *msg = "panic";
    size_t len = 0;
    if (argc >= 1 && args[0] && value_type(args[0]) &&
        type_kind_of(value_type(args[0])) == TYPE_KIND_STR &&
        value_data(args[0])) {
        /* str 的 data 槽存 `const char *`（len 内置同款解引用） */
        const char *s = *(const char *const *)value_data(args[0]);
        if (s) {
            msg = s;
            len = strlen(s);
        }
    }
    fprintf(stderr, "panic: %.*s\n", (int)len, msg);
    fflush(stderr);
    abort();
    return value_make_error(vm, "panic did not abort"); /* unreachable */
}

void vm_register_panic(vm_t *vm) {
    if (!vm || !vm->global_scope) return;
    const type_t *params[1] = {vm->type_str};
    const type_t *sig = type_func_sig(vm, params, 1, vm->type_void,
                                      /*is_variadic=*/false);
    if (!sig) return;
    value_t *fnv = func_new(vm, builtin_panic_cfunc, NULL, vm->root_scope, sig,
                            STRSLICE_LIT("panic"));
    if (!fnv) return;
    value_t *stored = scope_define(vm, vm->global_scope, "panic", fnv);
    (void)stored;
}
