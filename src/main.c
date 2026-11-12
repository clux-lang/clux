#include "core/allocator.h"
#include "core/cmdline.h"
#include <stdio.h>
#include <stdlib.h>

#define CLUX_VERSION "0.1.0-dev"

static const char *first_positional(const cmdline_options_t *options) {
  return cmdline_options_get_positional(options, 0);
}

static int not_implemented(const char *command, const char *input) {
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

static int cmd_build(const cmdline_options_t *options, void *userdata) {
  (void)userdata;
  const char *input = first_positional(options);
  if (!input) {
    fprintf(stderr, "error: build requires <file.cx>\n");
    return 2;
  }
  return not_implemented("build", input);
}

static int cmd_run(const cmdline_options_t *options, void *userdata) {
  (void)userdata;
  return not_implemented("run", first_positional(options));
}

static int cmd_test(const cmdline_options_t *options, void *userdata) {
  (void)userdata;
  return not_implemented("test", first_positional(options));
}

static int cmd_format(const cmdline_options_t *options, void *userdata) {
  (void)userdata;
  return not_implemented("format", first_positional(options));
}

static int cmd_install(const cmdline_options_t *options, void *userdata) {
  (void)userdata;
  (void)options;
  return not_implemented("install", NULL);
}

static int cmd_version(const cmdline_options_t *options, void *userdata) {
  (void)options;
  (void)userdata;
  printf("clux %s\n", CLUX_VERSION);
  return 0;
}

static const cmdline_option_t build_options[] = {
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
     "keep generated C after building",
     NULL},
    {"output",
     'o',
     CMDLINE_OPTION_STRING,
     false,
     false,
     NULL,
     "output executable path",
     NULL},
    {"locked",
     0,
     CMDLINE_OPTION_FLAG,
     false,
     false,
     NULL,
     "verify the lockfile before building",
     NULL},
};

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

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static const cmdline_command_spec_t commands[] = {
    {"build",
     "<file.cx> [options]",
     "Build a clux source file.",
     cmd_build,
     NULL,
     build_options,
     ARRAY_SIZE(build_options)},
    {"run",
     "[<file.cx> | <subpkg>] [options]",
     "Build and run a clux program.",
     cmd_run,
     NULL,
     run_options,
     ARRAY_SIZE(run_options)},
    {"test",
     "[<file.cx>] [options]",
     "Run test blocks.",
     cmd_test,
     NULL,
     test_options,
     ARRAY_SIZE(test_options)},
    {"format",
     "[<file.cx> | -o PATH]",
     "Format clux source.",
     cmd_format,
     NULL,
     format_options,
     ARRAY_SIZE(format_options)},
    {"install",
     "[options]",
     "Install dependencies and run postinstall hooks.",
     cmd_install,
     NULL,
     install_options,
     ARRAY_SIZE(install_options)},
    {"version", "", "Print compiler version.", cmd_version, NULL, NULL, 0},
};

int main(int argc, char *argv[]) {
  allocator_t *allocator = create_allocator(malloc, free);
  cmdline_t *cmdline =
      create_cmdline(allocator, "clux", "clux compiler driver");
  for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
    cmdline_add_command(cmdline, &commands[i]);
  }
  int result = cmdline_run(cmdline, argc, argv);
  delete_cmdline(&cmdline);
  delete_allocator(&allocator);
  return result;
}
