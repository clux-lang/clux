#include "cmd/format.h"
#include "cmd/common.h"

static int cmd_format(const cmdline_options_t *options, void *userdata) {
  (void)userdata;
  return cmd_not_implemented("format", cmd_first_positional(options));
}

static const cmdline_option_t format_options[] = {
    {"output",
     'o',
     CMDLINE_OPTION_STRING,
     false,
     false,
     NULL,
     "write formatted source to path",
     NULL},
};

void cmd_format_register(cmdline_t *cmdline, allocator_t *allocator) {
  cmdline_command_spec_t spec = {
      "format",
      "[<file.cx> | -o PATH]",
      "Format clux source.",
      cmd_format,
      allocator,
      format_options,
      CMD_ARRAY_SIZE(format_options),
  };
  cmdline_add_command(cmdline, &spec);
}
