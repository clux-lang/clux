#include "cmd/version.h"
#include <stdio.h>

#define CLUX_VERSION "0.1.0-dev"

static int cmd_version(const cmdline_options_t *options, void *userdata) {
  (void)options;
  (void)userdata;
  printf("clux %s\n", CLUX_VERSION);
  return 0;
}

void cmd_version_register(cmdline_t *cmdline, allocator_t *allocator) {
  cmdline_command_spec_t spec = {
      "version",
      "",
      "Print compiler version.",
      cmd_version,
      allocator,
      NULL,
      0,
  };
  cmdline_add_command(cmdline, &spec);
}
