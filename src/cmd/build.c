#include "cmd/build.h"
#include "cmd/common.h"
#include "core/error.h"
#include "core/json.h"
#include "core/string.h"
#include "core/vec.h"
#include "parser/node.h"
#include "parser/node_program.h"
#include "parser/parser.h"
#include "parser/program.h"
#include <stdio.h>

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
  const char *input = cmd_first_positional(options);
  if (!input) {
    fprintf(stderr, "error: build requires <file.cx>\n");
    return 2;
  }
  if (cmdline_options_get_flag(options, "emit-ast")) {
    return emit_ast(allocator, options, input);
  }
  return cmd_not_implemented("build", input);
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

void cmd_build_register(cmdline_t *cmdline, allocator_t *allocator) {
  cmdline_command_spec_t spec = {
      "build",
      "<file.cx> [options]",
      "Build a clux source file.",
      cmd_build,
      allocator,
      build_options,
      CMD_ARRAY_SIZE(build_options),
  };
  cmdline_add_command(cmdline, &spec);
}
