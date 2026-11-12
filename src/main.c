#include "core/allocator.h"
#include "core/cmdline.h"
#include "core/error.h"
#include "core/json.h"
#include "core/string.h"
#include "core/vec.h"
#include "parser/node.h"
#include "parser/node_program.h"
#include "parser/parser.h"
#include "parser/program.h"
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

static char *read_source_file(allocator_t *allocator, const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long length = ftell(file);
  if (length < 0) {
    fclose(file);
    return NULL;
  }
  rewind(file);
  char *source = allocator_alloc(allocator, (size_t)length + 1);
  size_t read = fread(source, 1, (size_t)length, file);
  fclose(file);
  if (read != (size_t)length) {
    allocator_free(allocator, (void **)&source);
    return NULL;
  }
  source[length] = '\0';
  return source;
}

static int emit_ast(allocator_t *allocator,
                    const cmdline_options_t *options,
                    const char *input) {
  char *source = read_source_file(allocator, input);
  if (!source) {
    fprintf(stderr, "error: cannot read '%s'\n", input);
    return 1;
  }
  parser_t *parser = create_parser(allocator, input, source);
  node_t *program = parser_read_program(allocator, parser);

  json_writer_t *writer = create_json_writer(allocator, source);
  program_node_to_json(writer, program);
  string_t *json = json_writer_release(writer);
  delete_json_writer(&writer);

  int status = 0;
  vec_t *errors = parser_get_error(parser);
  for (size_t i = 0; i < vec_get_size(errors); i++) {
    clux_error_t *error = (clux_error_t *)vec_get(errors, i);
    string_t *text = error_to_string(error, allocator);
    fprintf(stderr, "%s\n", string_get_data(text));
    delete_string(&text);
    status = 1;
  }

  const char *output = cmdline_options_get_string(options, "output");
  if (output) {
    FILE *file = fopen(output, "wb");
    if (!file) {
      fprintf(stderr, "error: cannot write '%s'\n", output);
      status = 1;
    } else {
      fwrite(string_get_data(json), 1, string_get_size(json), file);
      fputc('\n', file);
      fclose(file);
    }
  } else {
    fputs(string_get_data(json), stdout);
    fputc('\n', stdout);
  }

  delete_string(&json);
  delete_node(&program);
  delete_parser(&parser);
  allocator_free(allocator, (void **)&source);
  return status;
}

static int cmd_build(const cmdline_options_t *options, void *userdata) {
  allocator_t *allocator = userdata;
  const char *input = first_positional(options);
  if (!input) {
    fprintf(stderr, "error: build requires <file.cx>\n");
    return 2;
  }
  if (cmdline_options_get_flag(options, "emit-ast")) {
    return emit_ast(allocator, options, input);
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
    {"emit-ast",
     0,
     CMDLINE_OPTION_FLAG,
     false,
     false,
     NULL,
     "print the parsed AST as JSON (or write it to --output)",
     NULL},
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

static cmdline_command_spec_t commands[] = {
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
    commands[i].userdata = allocator;
    cmdline_add_command(cmdline, &commands[i]);
  }
  int result = cmdline_run(cmdline, argc, argv);
  delete_cmdline(&cmdline);
  delete_allocator(&allocator);
  return result;
}
