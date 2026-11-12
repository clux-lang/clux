#ifndef _H_CORE_CMDLINE_
#define _H_CORE_CMDLINE_
#include "core/allocator.h"
#include "core/string.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 轻量命令行解析器：每个子命令声明自己的 options，解析后调用回调。
   所有字符串参数都借用 argv / 静态字符串，不复制。 */
typedef struct _cmdline_t cmdline_t;
typedef struct _cmdline_options_t cmdline_options_t;

typedef enum _cmdline_option_type_t {
  CMDLINE_OPTION_FLAG,
  CMDLINE_OPTION_STRING,
  CMDLINE_OPTION_INT,
  CMDLINE_OPTION_UINT,
} cmdline_option_type_t;

/* 自定义校验；返回 false 时可在 error 中追加具体原因。 */
typedef bool (*cmdline_option_validate_t)(const char *value, string_t *error);
typedef int (*cmdline_callback_t)(const cmdline_options_t *options,
                                  void *userdata);

typedef struct _cmdline_option_t {
  const char *name; /* 长选项名，不含 "--" */
  char short_name;  /* 短选项字符；0 表示没有 */
  cmdline_option_type_t type;
  bool required;
  /* 可选值：只接受 --name=value 形式，--name 单独出现时使用 default_value。 */
  bool optional_value;
  const char *default_value;
  const char *help;
  cmdline_option_validate_t validate;
} cmdline_option_t;

typedef struct _cmdline_command_spec_t {
  const char *name;
  /* 位置参数摘要，例如 "<file.cx> [options]"；NULL 时只显示命令名。 */
  const char *usage;
  const char *description;
  cmdline_callback_t callback;
  void *userdata;
  const cmdline_option_t *options;
  size_t option_count;
} cmdline_command_spec_t;

cmdline_t *create_cmdline(allocator_t *allocator,
                          const char *program,
                          const char *description);
void delete_cmdline(cmdline_t **self);

/* 注册子命令；options 数组会被复制，字符串本身仍需在 cmdline 生命周期内有效。
   重名或参数非法返回 false。 */
bool cmdline_add_command(cmdline_t *self, const cmdline_command_spec_t *spec);

/* 解析 argv（argv[0] 为程序名，argv[1] 为子命令）。
   成功返回 options；失败返回 NULL 并设置 *error（调用方 delete_string）。 */
cmdline_options_t *
cmdline_parse(cmdline_t *self, int argc, char **argv, string_t **error);
void delete_cmdline_options(cmdline_options_t **options);

/* 解析并调用子命令回调；错误与 usage 输出到 stderr。返回进程退出码。 */
int cmdline_run(cmdline_t *self, int argc, char **argv);

string_t *cmdline_usage(cmdline_t *self, allocator_t *allocator);
string_t *cmdline_command_usage(cmdline_t *self,
                                const char *command,
                                allocator_t *allocator);

/* 查询解析结果。name 为长选项名。类型不匹配时返回 0 / NULL / false。 */
bool cmdline_options_has(const cmdline_options_t *self, const char *name);
bool cmdline_options_get_flag(const cmdline_options_t *self, const char *name);
const char *cmdline_options_get_string(const cmdline_options_t *self,
                                       const char *name);
int64_t cmdline_options_get_int(const cmdline_options_t *self,
                                const char *name);
uint64_t cmdline_options_get_uint(const cmdline_options_t *self,
                                  const char *name);

size_t cmdline_options_get_positional_count(const cmdline_options_t *self);
/* 越界返回 NULL。 */
const char *cmdline_options_get_positional(const cmdline_options_t *self,
                                           size_t idx);
const char *cmdline_options_get_command(const cmdline_options_t *self);
const char *cmdline_options_get_program(const cmdline_options_t *self);

#ifdef __cplusplus
}
#endif
#endif
