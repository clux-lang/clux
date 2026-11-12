#ifndef _H_CMD_PERF_
#define _H_CMD_PERF_
#include "core/perf.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 由 main 在启动时预创建的采样名；业务代码只持有数字下标。 */
extern perf_state_t *g_perf_state;
extern uint32_t g_perf_parse_name;
extern uint32_t g_perf_json_name;

/* 探针常驻，仅报告默认关闭；由命令行的 --perf 打开。 */
extern bool g_perf_report_enabled;
#ifdef __cplusplus
}
#endif
#endif
