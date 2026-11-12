#include "cmd/build.h"
#include "cmd/format.h"
#include "cmd/install.h"
#include "cmd/run.h"
#include "cmd/test.h"
#include "cmd/version.h"
#include "core/allocator.h"
#include "core/cmdline.h"
#include <stdlib.h>

int main(int argc, char *argv[]) {
  allocator_t *allocator = create_allocator(malloc, free);
  cmdline_t *cmdline =
      create_cmdline(allocator, "clux", "clux compiler driver");

  cmd_build_register(cmdline, allocator);
  cmd_run_register(cmdline, allocator);
  cmd_test_register(cmdline, allocator);
  cmd_format_register(cmdline, allocator);
  cmd_install_register(cmdline, allocator);
  cmd_version_register(cmdline, allocator);

  int result = cmdline_run(cmdline, argc, argv);
  delete_cmdline(&cmdline);
  delete_allocator(&allocator);
  return result;
}
