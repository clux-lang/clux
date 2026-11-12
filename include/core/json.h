#ifndef _H_CORE_JSON_
#define _H_CORE_JSON_
#include "core/allocator.h"
#include "core/location.h"
#include "core/string.h"
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 轻量 JSON 输出器：自动处理逗号、缩进与字符串转义。
   每个节点自己的 to_json 只负责按自己的字段顺序调用 key / value。 */
typedef struct _json_writer_t json_writer_t;

/* source 用于 span 偏移与 text 原文；可为 NULL。 */
json_writer_t *create_json_writer(allocator_t *allocator, const char *source);
void delete_json_writer(json_writer_t **self);
/* 交出内部 string_t 的所有权；writer 仍须 delete。 */
string_t *json_writer_release(json_writer_t *self);

void json_writer_begin_object(json_writer_t *self);
void json_writer_end_object(json_writer_t *self);
void json_writer_begin_array(json_writer_t *self);
void json_writer_end_array(json_writer_t *self);

/* 仅在对象内调用；随后必须写一个值。 */
void json_writer_key(json_writer_t *self, const char *key);
void json_writer_string(json_writer_t *self, const char *value);
void json_writer_number(json_writer_t *self, size_t value);
void json_writer_null(json_writer_t *self);

/* 便捷字段：key 后写值。 */
void json_writer_field_string(json_writer_t *self,
                              const char *key,
                              const char *value);
void json_writer_field_number(json_writer_t *self,
                              const char *key,
                              size_t value);
void json_writer_field_span(json_writer_t *self, const location_t *loc);
void json_writer_field_text(json_writer_t *self, const location_t *loc);

#ifdef __cplusplus
}
#endif
#endif
