#ifndef _H_ENGINE_OPTIONAL_
#define _H_ENGINE_OPTIONAL_
#include "engine/interface.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 取载荷为 base 的可选类型（"?<base>"）：要么 none（零值），要么含一个 T。
   value data 布局：首字节 present 标志，payload 在 base 对齐偏移处。
   同 base 恒等返回同一实例，新类型注册为 base 的 child，随 base 递归释放。
   解引用（!）是唯一的借引用读：返回 base 类型的 view，none 时运行期
   error；判空用 #is_none（vtable is_none 槽位）；写入由通用 assign
   写穿 view 完成。 */
clux_type_t *clux_type_get_optional_type(clux_type_t *base);
/* optional 类型的载荷 base，非 optional 类型返回 NULL。 */
clux_type_t *clux_type_get_optional_base(clux_type_t *type);
/* value data 布局辅助：present 标志后的 payload 字节偏移。 */
size_t clux_optional_payload_offset(clux_type_t *base);
clux_vtable_t *get_clux_optional_vtable(void);
#ifdef __cplusplus
}
#endif
#endif
