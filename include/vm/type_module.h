#ifndef _H_CLUX_VM_TYPE_MODULE_
#define _H_CLUX_VM_TYPE_MODULE_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/vtable.h"

/* module 类型（M5）：命名空间值，data=module_t*（借用）。
 * 内建单例，无开放构造阶段。仅由 IMPORT 指令内部构造。
 * 不可实例化/赋值/传递——仅作命名空间载体。 */
extern const vtable_t VTABLE_MODULE;

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_MODULE_ */
