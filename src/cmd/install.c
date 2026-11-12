#include "cmd/install.h"
#include "cmd/common.h"

static int cmd_install(const cmdline_options_t *options, void *userdata) {
  (void)options;
  (void)userdata;
  return cmd_not_implemented("install", NULL);
}

static const cmdline_option_t install_options[] = {
    {"allow-scripts",
     0,
     CMDLINE_OPTION_FLAG,
     false,
     false,
     NULL,
     "run opt-in postinstall hooks",
     NULL},
    {"force",
     0,
     CMDLINE_OPTION_FLAG,
     false,
     false,
     NULL,
     "rerun install hooks",
     NULL},
    {"locked",
     0,
     CMDLINE_OPTION_FLAG,
     false,
     false,
     NULL,
     "verify the lockfile before installing",
     NULL},
};

void cmd_install_register(cmdline_t *cmdline, allocator_t *allocator) {
  cmdline_command_spec_t spec = {
      "install",
      "[options]",
      "Install dependencies and run postinstall hooks.",
      cmd_install,
      allocator,
      install_options,
      CMD_ARRAY_SIZE(install_options),
  };
  cmdline_add_command(cmdline, &spec);
}
