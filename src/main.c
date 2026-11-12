#include "cmd/build.h"
#include "cmd/format.h"
#include "cmd/install.h"
#include "cmd/perf.h"
#include "cmd/run.h"
#include "cmd/test.h"
#include "cmd/version.h"
#include "core/allocator.h"
#include "core/cmdline.h"
#include <stdlib.h>

perf_state_t *g_perf_state;
uint32_t g_perf_parse_name;
uint32_t g_perf_json_name;
bool g_perf_report_enabled;

int main(int argc, char *argv[]) {
  allocator_t *allocator = create_allocator(malloc, free);
  g_perf_state = create_perf_state(allocator);
  g_perf_parse_name = perf_create(g_perf_state, "parse", 10);
  g_perf_json_name = perf_create(g_perf_state, "json", 10);

  cmdline_t *cmdline =
      create_cmdline(allocator, "clux", "clux compiler driver");

  cmd_build_register(cmdline, allocator);
  cmd_run_register(cmdline, allocator);
  cmd_test_register(cmdline, allocator);
  cmd_format_register(cmdline, allocator);
  cmd_install_register(cmdline, allocator);
  cmd_version_register(cmdline, allocator);

  int result = cmdline_run(cmdline, argc, argv);
  if (g_perf_report_enabled) {
    perf_report(g_perf_state, stderr);
  }
  delete_cmdline(&cmdline);
  delete_perf_state(&g_perf_state);
  delete_allocator(&allocator);
  return result;
}
