#ifndef _H_CLUX_VM_TYPE_NIL_
#define _H_CLUX_VM_TYPE_NIL_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/vtable.h"

/** nil 类型 vtable：nil 是内置类型，唯一值 nil。
 *  data 为 func_t* 宽度的零块（NULL 指针），当前表示函数 0 初始化
 *  （*((func_t **)value.data) == NULL），未来用于空指针。 */
extern const vtable_t VTABLE_NIL;

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_NIL_ */
