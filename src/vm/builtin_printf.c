#include "vm/vm.h"
#include "vm/value.h"
#include "vm/function.h"
#include "vm/type.h"
#include "vm/type_func.h"
#include "vm/value_dump.h"
#include "core/string.h"
#include "core/strslice.h"

#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * printf 内置函数（临时注册，M1 硬编码绑定 C printf）
 *
 * 签名：printf(format: str, ...) -> void（variadic）
 * 支持格式符：%d %i %u %x %X %o %f %e %g %c %s %%（含宽度/精度修饰符，
 * 格式片段原样透传给 C printf）。
 * =========================================================================== */

/* 按 value 实际类型宽度读取有符号整数 */
static int64_t read_signed(const value_t *v) {
    switch (value_type(v)->size) {
    case 1: return (int64_t)*(const int8_t  *)value_data(v);
    case 2: return (int64_t)*(const int16_t *)value_data(v);
    case 4: return (int64_t)*(const int32_t *)value_data(v);
    default: return *(const int64_t *)value_data(v);
    }
}

/* 按 value 实际类型宽度读取无符号整数 */
static uint64_t read_unsigned(const value_t *v) {
    switch (value_type(v)->size) {
    case 1: return (uint64_t)*(const uint8_t  *)value_data(v);
    case 2: return (uint64_t)*(const uint16_t *)value_data(v);
    case 4: return (uint64_t)*(const uint32_t *)value_data(v);
    default: return *(const uint64_t *)value_data(v);
    }
}

/* 按 value 实际类型宽度读取浮点数 */
static double read_double(const value_t *v) {
    if (value_type(v)->size == sizeof(float))
        return (double)*(const float *)value_data(v);
    return *(const double *)value_data(v);
}

/* 格式符判定 */
static bool is_conv_char(char c) {
    return c && strchr("diufFeEgGxXocspv", c) != NULL;
}

value_t *builtin_printf_cfunc(vm_t *vm, func_t *self, size_t argc,
                              value_t **args) {
    (void)self;
    if (argc < 1)
        return value_make_error(vm, "printf: missing format string");
    if (value_type(args[0]) != vm->type_str)
        return value_make_error(vm, "printf: format must be a string");

    const char *fmt = string_cstr(*(string_t **)value_data(args[0]));
    size_t ai = 1; /* 可变实参下标（0 = format） */

    for (const char *p = fmt; *p; ) {
        if (*p != '%') { putchar(*p); p++; continue; }

        /* 收集完整格式片段 [% 修饰符...转换符]，原样透传给 C printf，
           天然支持 %5d / %-10s / %.2f 等宽度精度修饰符 */
        const char *start = p;
        p++;
        if (*p == '%') { putchar('%'); p++; continue; }  /* 转义 %% */

        while (*p && !is_conv_char(*p)) p++;  /* 跳过修饰符（-+ #0 数字 . l h L） */
        if (!*p) break;                        /* 尾部孤立 % */

        char spec[32];
        size_t slen = (size_t)(p - start) + 1; /* 含转换符 */
        if (slen >= sizeof(spec)) slen = sizeof(spec) - 1;
        memcpy(spec, start, slen);
        spec[slen] = '\0';
        char conv = *p;
        p++;

        if (ai >= argc)
            return value_make_error(vm, "printf: too few arguments");
        value_t *arg = args[ai++];

        switch (conv) {
        case 'd': case 'i': case 'c':
            printf(spec, read_signed(arg));
            break;
        case 'u': case 'x': case 'X': case 'o':
            printf(spec, read_unsigned(arg));
            break;
        case 'f': case 'e': case 'E': case 'g': case 'G':
            printf(spec, read_double(arg));
            break;
        case 's': {
            if (value_type(arg) != vm->type_str)
                return value_make_error(vm, "printf: %%s requires a string");
            printf(spec, string_cstr(*(string_t **)value_data(arg)));
            break;
        }
        case 'p':
            printf(spec, (void *)read_unsigned(arg));
            break;
        case 'v': {
            /* 携带类型输出任意 value：`.type { value }`，复杂类型递归包裹。
               参数已在进入 switch 前由 line 84-86 读取并自增 ai，此处直接复用，
               切忌再次 args[ai++] 否则会跳过参数、读取越界导致崩溃。
               宽度/精度修饰符对 %v 无语义，收集后忽略（spec 不使用）。 */
            string_t *buf = string_new(vm->alloc);
            if (!buf) return value_make_error(vm, "printf: out of memory");
            value_dump_string(vm, arg, buf);
            fputs(string_cstr(buf), stdout);
            string_free(&buf);
            break;
        }
        default:
            return value_make_error(vm, "printf: unsupported conversion");
        }
    }

    return value_make_undefined(vm);
}

/* 注册 printf 到 vm->global_scope（调用方在 vm_new 内调用）。
   签名：printf(str, ...) -> void，variadic。 */
void vm_register_printf(vm_t *vm) {
    if (!vm || !vm->global_scope) return;

    const type_t *params[1] = { vm->type_str };
    const type_t *sig = type_func_sig(vm, params, 1, NULL, /*is_variadic=*/true);
    if (!sig) return;

    value_t *fnv = func_new(vm, builtin_printf_cfunc, NULL, vm->root_scope,
                            sig, STRSLICE_LIT("printf"));
    if (!fnv) return;

    value_t *stored = scope_define(vm, vm->global_scope, "printf", fnv);
    if (!stored || value_is_error(vm, stored)) {
        value_dispose(vm, fnv);
        allocator_free(vm->alloc, (void **)&fnv);
        return;
    }
    /* scope_define clone 进 scope，原值壳归调用方管理（与 vm_register_builtin_types 同模式） */
    value_dispose(vm, fnv);
    allocator_free(vm->alloc, (void **)&fnv);
}
