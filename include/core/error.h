#ifndef _H_CORE_ERROR_
#define _H_CORE_ERROR_
#include "core/allocator.h"
#include "core/location.h"
#include "core/string.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 避免与 glibc <bits/types/error_t.h> 的 `typedef int error_t` 冲突 */
typedef struct _clux_error_t clux_error_t;
clux_error_t *create_error(allocator_t *allocator,
                           const location_t *loc,
                           const char *fmt,
                           ...);
const location_t *error_get_location(clux_error_t *self);
const char *error_get_message(clux_error_t *self);
/* 借用调用方源码，用于 error_to_string 打印源码行；不复制、不释放。 */
void error_set_source(clux_error_t *self, const char *source);
const char *error_get_source(clux_error_t *self);
/* 渲染为 Rust 风格诊断：
     error: <message>
      --> <file>:<line>:<column>
       |
     <line> | <source line>
       |     ^^^
   源码不可用时只输出前两行。返回的 string_t 由调用方释放。 */
string_t *error_to_string(clux_error_t *self, allocator_t *allocator);
void delete_error(clux_error_t **error);
#ifdef __cplusplus
}
#endif
#endif