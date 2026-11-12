#include "cmd/common.h"
#include <stdio.h>

const char *cmd_first_positional(const cmdline_options_t *options) {
  return cmdline_options_get_positional(options, 0);
}

int cmd_not_implemented(const char *command, const char *input) {
  if (input) {
    fprintf(stderr,
            "clux: %s is not implemented yet (input: %s)\n",
            command,
            input);
  } else {
    fprintf(stderr, "clux: %s is not implemented yet\n", command);
  }
  return 1;
}
