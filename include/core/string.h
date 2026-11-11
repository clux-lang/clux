#ifndef _H_CORE_STRING_
#define _H_CORE_STRING_
#include "core/allocator.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 变长字节字符串容器。size 不含结尾 NUL；capacity 是已分配字节数，
   始终至少为 size + 1，data[size] 恒为 '\0'。 */
typedef struct _string_t string_t;

string_t *create_string(allocator_t *allocator);
void delete_string(string_t **string);

size_t string_get_size(string_t *self);
size_t string_get_capacity(string_t *self);

/* 借用内部缓冲区，保证 NUL 结尾。指针在下一次修改容器前有效。 */
const char *string_get_data(string_t *self);
/* 可写借用；调用方不得改变 size 或破坏结尾 NUL。 */
char *string_get_data_mut(string_t *self);

/* 保证可容纳至少 capacity 字节（含结尾 NUL）。只增不减。 */
string_t *string_reserve(string_t *self, size_t capacity);
/* 调整 size；扩展部分填 '\0'。 */
string_t *string_resize(string_t *self, size_t size);
void string_clear(string_t *self);

/* 越界返回 '\0'。 */
char string_get(string_t *self, size_t idx);
/* 越界返回 false。 */
bool string_set(string_t *self, size_t idx, char c);

/* 追加 / 赋值，返回新的 size；参数非法或越界返回 (size_t)-1。
   data 可以与容器自身缓冲区重叠。 */
size_t string_append(string_t *self, const char *data, size_t len);
size_t string_append_cstr(string_t *self, const char *cstr);
size_t string_append_char(string_t *self, char c);
size_t string_assign(string_t *self, const char *data, size_t len);

/* 在 idx 处插入 / 删除 len 字节，返回新的 size；idx 越界返回 (size_t)-1。 */
size_t string_insert(string_t *self, size_t idx, const char *data, size_t len);
size_t string_erase(string_t *self, size_t idx, size_t len);

/* 尾部压入 / 弹出单字节，返回新的 size；空串弹出返回 (size_t)-1。 */
size_t string_push(string_t *self, char c);
size_t string_pop(string_t *self);

#ifdef __cplusplus
}
#endif
#endif
