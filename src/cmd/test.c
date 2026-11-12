#include "cmd/test.h"
#include "cmd/common.h"

static int cmd_test(const cmdline_options_t *options, void *userdata) {
  (void)userdata;
  return cmd_not_implemented("test", cmd_first_positional(options));
}

static const cmdline_option_t test_options[] = {
    {"backend",
     0,
     CMDLINE_OPTION_STRING,
     false,
     false,
     "c",
     "code generation backend",
     NULL},
    {"keep-c",
     0,
     CMDLINE_OPTION_FLAG,
     false,
     false,
     NULL,
     "keep generated C after testing",
     NULL},
};

void cmd_test_register(cmdline_t *cmdline, allocator_t *allocator) {
  cmdline_command_spec_t spec = {
      "test",
      "[<file.cx>] [options]",
      "Run test blocks.",
      cmd_test,
      allocator,
      test_options,
      CMD_ARRAY_SIZE(test_options),
  };
  cmdline_add_command(cmdline, &spec);
}
