#ifndef _H_CORE_PERF_
#define _H_CORE_PERF_
#include "core/allocator.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 轻量性能采样器：perf_start/perf_end 只接受 uint32_t 数字 name，
   避免热路径上的字符串查找。name 由 perf_create 在初始化阶段预先分配，
   每个 name 拥有 max_count 个环形槽位，写满后覆盖最旧样本。 */
typedef struct _perf_state_t perf_state_t;

#define PERF_INVALID_NAME ((uint32_t)-1)

perf_state_t *create_perf_state(allocator_t *allocator);
void delete_perf_state(perf_state_t **state);

/* 注册一个采样名，返回槽位起始下标；name 字符串由调用方保证生命周期。
   max_count 为环形槽位数，0 或槽位下标溢出时返回 PERF_INVALID_NAME。 */
uint32_t perf_create(perf_state_t *state, const char *name, size_t max_count);

/* 记录一次采样；name 非法时为空操作。 */
void perf_start(perf_state_t *state, uint32_t name);
void perf_end(perf_state_t *state, uint32_t name);

/* 查询环形缓冲区中的样本（按时间顺序，0 为最旧）。 */
size_t perf_get_sample_count(const perf_state_t *state, uint32_t name);
bool perf_get_sample(const perf_state_t *state,
                     uint32_t name,
                     size_t idx,
                     uint64_t *start_ns,
                     uint64_t *end_ns);

/* 输出每个采样名的 count/min/avg/max（微秒）。 */
void perf_report(const perf_state_t *state, FILE *out);

#ifdef __cplusplus
}
#endif
#endif
