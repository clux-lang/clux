#ifndef CLUX_CBACKEND_C_OPT_H
#define CLUX_CBACKEND_C_OPT_H

#include "cbackend/c_ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * C IR 优化：
 * - 标记被作为函数值引用的函数对象；
 * - 未被引用的函数对象不再生成。
 */
void cbackend_optimize(cir_unit_t *unit);

#ifdef __cplusplus
}
#endif
#endif
