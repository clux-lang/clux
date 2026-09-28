#ifndef _H_CLUX_VM_TYPE_ERROR_
#define _H_CLUX_VM_TYPE_ERROR_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/vtable.h"
#include "vm/str_pool.h"  /* 字符串池指针类型（const char *） */

/** error 类型 vtable（仅 dispose/clone，不参与任何运算） */
extern const vtable_t VTABLE_ERROR;

/**
 * error_data_t: error value 的 data 布局
 *
 * - message: 错误消息（必填）
 * - location: 位置信息（可选，NULL 表示无位置，由 AST-walking 层填充）
 *
 * 字符串为 vm 字符串池内指针（const char *，含 NUL 终止符），data 块全平凡
 * （memcpy 可拷贝），本体生命周期归 vm 字符串池。
 */
typedef struct {
    const char *message;
    const char *location;
} error_data_t;

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_ERROR_ */
