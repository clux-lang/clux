#include "cmd/run.h"
#include "cmd/common.h"

static int cmd_run(const cmdline_options_t *options, void *userdata) {
  (void)userdata;
  return cmd_not_implemented("run", cmd_first_positional(options));
}

static const cmdline_option_t run_options[] = {
    {"emit-c",
     0,
     CMDLINE_OPTION_STRING,
     false,
     true,
     "",
     "write generated C without compiling",
     NULL},
    {"keep-c",
     0,
     CMDLINE_OPTION_FLAG,
     false,
     false,
     NULL,
     "keep generated C after running",
     NULL},
};

void cmd_run_register(cmdline_t *cmdline, allocator_t *allocator) {
  cmdline_command_spec_t spec = {
      "run",
      "[<file.cx> | <subpkg>] [options]",
      "Build and run a clux program.",
      cmd_run,
      allocator,
      run_options,
      CMD_ARRAY_SIZE(run_options),
  };
  cmdline_add_command(cmdline, &spec);
}
