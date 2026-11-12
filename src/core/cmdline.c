#include "core/cmdline.h"
#include "core/allocator.h"
#include "core/string.h"
#include "core/vec.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _cmdline_command_t {
  const char *name;
  const char *usage;
  const char *description;
  cmdline_callback_t callback;
  void *userdata;
  cmdline_option_t *options;
  size_t option_count;
} cmdline_command_t;

typedef struct _cmdline_option_value_t {
  const cmdline_option_t *spec;
  bool present;
  const char *string_value;
  int64_t int_value;
  uint64_t uint_value;
} cmdline_option_value_t;

struct _cmdline_t {
  allocator_t *allocator;
  const char *program;
  const char *description;
  vec_t *commands;
};

struct _cmdline_options_t {
  allocator_t *allocator;
  const cmdline_command_t *command;
  const char *program;
  vec_t *entries;
  vec_t *positionals;
};

static void append_fmt(string_t *out, const char *fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  (void)vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  string_append_cstr(out, buf);
}

static void append_spaces(string_t *out, size_t count) {
  for (size_t i = 0; i < count; i++) {
    string_append_char(out, ' ');
  }
}

static void
set_error(allocator_t *allocator, string_t **error, const char *fmt, ...) {
  if (!error) {
    return;
  }
  char buf[512];
  va_list args;
  va_start(args, fmt);
  (void)vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  *error = create_string(allocator);
  string_append_cstr(*error, buf);
}

static const cmdline_command_t *find_command(cmdline_t *self,
                                             const char *name) {
  size_t count = vec_get_size(self->commands);
  for (size_t i = 0; i < count; i++) {
    cmdline_command_t *command = vec_get(self->commands, i);
    if (strcmp(command->name, name) == 0) {
      return command;
    }
  }
  return NULL;
}

cmdline_t *create_cmdline(allocator_t *allocator,
                          const char *program,
                          const char *description) {
  cmdline_t *self = allocator_alloc(allocator, sizeof(struct _cmdline_t));
  self->allocator = allocator;
  self->program = program ? program : "clux";
  self->description = description;
  self->commands = create_vec(allocator);
  return self;
}

void delete_cmdline(cmdline_t **self) {
  if (!self || !*self) {
    return;
  }
  cmdline_t *cmd = *self;
  size_t count = vec_get_size(cmd->commands);
  for (size_t i = 0; i < count; i++) {
    cmdline_command_t *command = vec_get(cmd->commands, i);
    if (command->options) {
      allocator_free(cmd->allocator, (void **)&command->options);
    }
    allocator_free(cmd->allocator, (void **)&command);
  }
  delete_vec(&cmd->commands);
  allocator_free(cmd->allocator, (void **)self);
}

bool cmdline_add_command(cmdline_t *self, const cmdline_command_spec_t *spec) {
  if (!self || !spec || !spec->name || spec->name[0] == '\0') {
    return false;
  }
  if (spec->option_count > 0 && !spec->options) {
    return false;
  }
  if (find_command(self, spec->name)) {
    return false;
  }

  cmdline_command_t *command =
      allocator_alloc(self->allocator, sizeof(struct _cmdline_command_t));
  command->name = spec->name;
  command->usage = spec->usage;
  command->description = spec->description;
  command->callback = spec->callback;
  command->userdata = spec->userdata;
  command->option_count = spec->option_count;
  command->options = NULL;
  if (spec->option_count > 0) {
    command->options = allocator_alloc(
        self->allocator, sizeof(cmdline_option_t) * spec->option_count);
    memcpy(command->options,
           spec->options,
           sizeof(cmdline_option_t) * spec->option_count);
  }
  vec_push(self->commands, command);
  return true;
}

static cmdline_option_value_t *find_entry(const cmdline_options_t *self,
                                          const char *name) {
  if (!name) {
    return NULL;
  }
  size_t count = vec_get_size(self->entries);
  for (size_t i = 0; i < count; i++) {
    cmdline_option_value_t *entry = vec_get(self->entries, i);
    if (strcmp(entry->spec->name, name) == 0) {
      return entry;
    }
  }
  return NULL;
}

static const cmdline_option_t *
find_long_option(const cmdline_command_t *command, const char *name) {
  for (size_t i = 0; i < command->option_count; i++) {
    if (strcmp(command->options[i].name, name) == 0) {
      return &command->options[i];
    }
  }
  return NULL;
}

static const cmdline_option_t *
find_short_option(const cmdline_command_t *command, char name) {
  for (size_t i = 0; i < command->option_count; i++) {
    if (command->options[i].short_name == name) {
      return &command->options[i];
    }
  }
  return NULL;
}

static bool parse_option_value(allocator_t *allocator,
                               string_t **error,
                               const cmdline_option_t *spec,
                               const char *text,
                               cmdline_option_value_t *entry) {
  if (spec->validate) {
    string_t *validate_error = create_string(allocator);
    bool ok = spec->validate(text, validate_error);
    if (!ok) {
      if (string_get_size(validate_error) > 0) {
        set_error(allocator,
                  error,
                  "invalid value '%s' for option '--%s': %s",
                  text,
                  spec->name,
                  string_get_data(validate_error));
      } else {
        set_error(allocator,
                  error,
                  "invalid value '%s' for option '--%s'",
                  text,
                  spec->name);
      }
      delete_string(&validate_error);
      return false;
    }
    delete_string(&validate_error);
  }

  switch (spec->type) {
  case CMDLINE_OPTION_FLAG:
    entry->present = true;
    return true;
  case CMDLINE_OPTION_STRING:
    entry->string_value = text;
    entry->present = true;
    return true;
  case CMDLINE_OPTION_INT: {
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
      set_error(allocator,
                error,
                "invalid integer value '%s' for option '--%s'",
                text,
                spec->name);
      return false;
    }
    entry->int_value = (int64_t)value;
    entry->present = true;
    return true;
  }
  case CMDLINE_OPTION_UINT: {
    if (text[0] == '-') {
      set_error(allocator,
                error,
                "invalid unsigned value '%s' for option '--%s'",
                text,
                spec->name);
      return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
      set_error(allocator,
                error,
                "invalid unsigned value '%s' for option '--%s'",
                text,
                spec->name);
      return false;
    }
    entry->uint_value = (uint64_t)value;
    entry->present = true;
    return true;
  }
  }
  return false;
}

cmdline_options_t *
cmdline_parse(cmdline_t *self, int argc, char **argv, string_t **error) {
  if (error) {
    *error = NULL;
  }
  if (!self) {
    return NULL;
  }
  if (!argv || argc < 2) {
    set_error(self->allocator, error, "missing command");
    return NULL;
  }

  const char *program = argv[0] ? argv[0] : self->program;
  const char *command_name = argv[1];
  const cmdline_command_t *command = find_command(self, command_name);
  if (!command) {
    set_error(self->allocator, error, "unknown command '%s'", command_name);
    return NULL;
  }

  cmdline_options_t *options =
      allocator_alloc(self->allocator, sizeof(struct _cmdline_options_t));
  options->allocator = self->allocator;
  options->command = command;
  options->program = program;
  options->entries = create_vec(self->allocator);
  options->positionals = create_vec(self->allocator);

  for (size_t i = 0; i < command->option_count; i++) {
    const cmdline_option_t *spec = &command->options[i];
    cmdline_option_value_t *entry =
        allocator_alloc(self->allocator, sizeof(cmdline_option_value_t));
    entry->spec = spec;
    entry->present = false;
    entry->string_value = NULL;
    entry->int_value = 0;
    entry->uint_value = 0;
    if (spec->type != CMDLINE_OPTION_FLAG && spec->default_value) {
      if (!parse_option_value(
              self->allocator, error, spec, spec->default_value, entry)) {
        allocator_free(self->allocator, (void **)&entry);
        delete_cmdline_options(&options);
        return NULL;
      }
    }
    vec_push(options->entries, entry);
  }

  bool positional_only = false;
  for (int i = 2; i < argc; i++) {
    const char *arg = argv[i];
    if (!positional_only && strcmp(arg, "--") == 0) {
      positional_only = true;
      continue;
    }
    if (!positional_only && arg[0] == '-' && arg[1] == '-') {
      const char *name = arg + 2;
      const char *eq = strchr(name, '=');
      size_t name_len = eq ? (size_t)(eq - name) : strlen(name);
      if (name_len == 0) {
        set_error(self->allocator, error, "invalid option '%s'", arg);
        delete_cmdline_options(&options);
        return NULL;
      }
      char name_buf[128];
      if (name_len >= sizeof(name_buf)) {
        set_error(self->allocator, error, "option name too long '%s'", arg);
        delete_cmdline_options(&options);
        return NULL;
      }
      memcpy(name_buf, name, name_len);
      name_buf[name_len] = '\0';
      const cmdline_option_t *spec = find_long_option(command, name_buf);
      if (!spec) {
        set_error(self->allocator, error, "unknown option '--%s'", name_buf);
        delete_cmdline_options(&options);
        return NULL;
      }
      const char *inline_value = eq ? eq + 1 : NULL;
      cmdline_option_value_t *entry = find_entry(options, spec->name);
      if (spec->type == CMDLINE_OPTION_FLAG) {
        if (inline_value) {
          set_error(self->allocator,
                    error,
                    "option '--%s' does not take a value",
                    spec->name);
          delete_cmdline_options(&options);
          return NULL;
        }
        entry->present = true;
        continue;
      }
      const char *value = inline_value;
      if (!value) {
        if (spec->optional_value) {
          value = spec->default_value ? spec->default_value : "";
        } else {
          i++;
          if (i >= argc) {
            set_error(self->allocator,
                      error,
                      "option '--%s' requires a value",
                      spec->name);
            delete_cmdline_options(&options);
            return NULL;
          }
          value = argv[i];
        }
      }
      if (!parse_option_value(self->allocator, error, spec, value, entry)) {
        delete_cmdline_options(&options);
        return NULL;
      }
      continue;
    }
    if (!positional_only && arg[0] == '-' && arg[1] != '\0') {
      char short_name = arg[1];
      const cmdline_option_t *spec = find_short_option(command, short_name);
      if (!spec) {
        set_error(self->allocator, error, "unknown option '-%c'", short_name);
        delete_cmdline_options(&options);
        return NULL;
      }
      cmdline_option_value_t *entry = find_entry(options, spec->name);
      if (spec->type == CMDLINE_OPTION_FLAG) {
        if (arg[2] != '\0') {
          set_error(self->allocator,
                    error,
                    "unexpected characters after option '-%c'",
                    short_name);
          delete_cmdline_options(&options);
          return NULL;
        }
        entry->present = true;
        continue;
      }
      const char *value = arg[2] != '\0' ? arg + 2 : NULL;
      if (!value) {
        if (spec->optional_value) {
          value = spec->default_value ? spec->default_value : "";
        } else {
          i++;
          if (i >= argc) {
            set_error(self->allocator,
                      error,
                      "option '-%c' requires a value",
                      short_name);
            delete_cmdline_options(&options);
            return NULL;
          }
          value = argv[i];
        }
      }
      if (!parse_option_value(self->allocator, error, spec, value, entry)) {
        delete_cmdline_options(&options);
        return NULL;
      }
      continue;
    }
    vec_push(options->positionals, (void *)arg);
  }

  for (size_t i = 0; i < command->option_count; i++) {
    const cmdline_option_t *spec = &command->options[i];
    cmdline_option_value_t *entry = find_entry(options, spec->name);
    if (spec->required && (!entry || !entry->present)) {
      set_error(
          self->allocator, error, "missing required option '--%s'", spec->name);
      delete_cmdline_options(&options);
      return NULL;
    }
  }
  return options;
}

void delete_cmdline_options(cmdline_options_t **options) {
  if (!options || !*options) {
    return;
  }
  cmdline_options_t *self = *options;
  size_t count = vec_get_size(self->entries);
  for (size_t i = 0; i < count; i++) {
    cmdline_option_value_t *entry = vec_get(self->entries, i);
    allocator_free(self->allocator, (void **)&entry);
  }
  delete_vec(&self->entries);
  delete_vec(&self->positionals);
  allocator_free(self->allocator, (void **)options);
}

bool cmdline_options_has(const cmdline_options_t *self, const char *name) {
  if (!self) {
    return false;
  }
  cmdline_option_value_t *entry = find_entry(self, name);
  return entry && entry->present;
}

bool cmdline_options_get_flag(const cmdline_options_t *self, const char *name) {
  if (!self) {
    return false;
  }
  cmdline_option_value_t *entry = find_entry(self, name);
  return entry && entry->spec->type == CMDLINE_OPTION_FLAG && entry->present;
}

const char *cmdline_options_get_string(const cmdline_options_t *self,
                                       const char *name) {
  if (!self) {
    return NULL;
  }
  cmdline_option_value_t *entry = find_entry(self, name);
  if (!entry || entry->spec->type != CMDLINE_OPTION_STRING || !entry->present) {
    return NULL;
  }
  return entry->string_value;
}

int64_t cmdline_options_get_int(const cmdline_options_t *self,
                                const char *name) {
  if (!self) {
    return 0;
  }
  cmdline_option_value_t *entry = find_entry(self, name);
  if (!entry || entry->spec->type != CMDLINE_OPTION_INT || !entry->present) {
    return 0;
  }
  return entry->int_value;
}

uint64_t cmdline_options_get_uint(const cmdline_options_t *self,
                                  const char *name) {
  if (!self) {
    return 0;
  }
  cmdline_option_value_t *entry = find_entry(self, name);
  if (!entry || entry->spec->type != CMDLINE_OPTION_UINT || !entry->present) {
    return 0;
  }
  return entry->uint_value;
}

size_t cmdline_options_get_positional_count(const cmdline_options_t *self) {
  return self ? vec_get_size(self->positionals) : 0;
}

const char *cmdline_options_get_positional(const cmdline_options_t *self,
                                           size_t idx) {
  return self ? vec_get(self->positionals, idx) : NULL;
}

const char *cmdline_options_get_command(const cmdline_options_t *self) {
  return self ? self->command->name : NULL;
}

const char *cmdline_options_get_program(const cmdline_options_t *self) {
  return self ? self->program : NULL;
}

static void append_option_line(allocator_t *allocator,
                               string_t *out,
                               const cmdline_option_t *spec,
                               size_t width) {
  string_t *line = create_string(allocator);
  if (spec->short_name) {
    append_fmt(line, "  -%c, --%s", spec->short_name, spec->name);
  } else {
    append_fmt(line, "      --%s", spec->name);
  }
  if (spec->type != CMDLINE_OPTION_FLAG) {
    if (spec->optional_value) {
      string_append_cstr(line, " [=<VALUE>]");
    } else {
      string_append_cstr(line, " <VALUE>");
    }
  }
  string_append(out, string_get_data(line), string_get_size(line));
  size_t line_size = string_get_size(line);
  if (line_size < width) {
    append_spaces(out, width - line_size);
  }
  string_append_cstr(out, "  ");
  if (spec->help) {
    string_append_cstr(out, spec->help);
  }
  if (spec->required) {
    string_append_cstr(out, " (required)");
  }
  string_append_char(out, '\n');
  delete_string(&line);
}

string_t *cmdline_usage(cmdline_t *self, allocator_t *allocator) {
  string_t *out = create_string(allocator);
  append_fmt(out, "Usage: %s <command> [options] [args]\n", self->program);
  if (self->description) {
    string_append_char(out, '\n');
    string_append_cstr(out, self->description);
    string_append_char(out, '\n');
  }
  string_append_cstr(out, "\nCommands:\n");
  size_t max_name = 0;
  size_t count = vec_get_size(self->commands);
  for (size_t i = 0; i < count; i++) {
    cmdline_command_t *command = vec_get(self->commands, i);
    size_t len = strlen(command->name);
    if (len > max_name) {
      max_name = len;
    }
  }
  for (size_t i = 0; i < count; i++) {
    cmdline_command_t *command = vec_get(self->commands, i);
    append_fmt(out, "  %-*s", (int)max_name, command->name);
    if (command->usage) {
      append_fmt(out, " %s", command->usage);
    }
    if (command->description) {
      string_append_cstr(out, "  ");
      string_append_cstr(out, command->description);
    }
    string_append_char(out, '\n');
  }
  append_fmt(
      out, "\nRun '%s <command> --help' for command options.\n", self->program);
  return out;
}

string_t *cmdline_command_usage(cmdline_t *self,
                                const char *command_name,
                                allocator_t *allocator) {
  string_t *out = create_string(allocator);
  const cmdline_command_t *command = find_command(self, command_name);
  if (!command) {
    append_fmt(out, "Unknown command '%s'\n", command_name);
    return out;
  }
  append_fmt(out, "Usage: %s %s", self->program, command->name);
  if (command->usage) {
    append_fmt(out, " %s", command->usage);
  }
  string_append_char(out, '\n');
  if (command->description) {
    string_append_char(out, '\n');
    string_append_cstr(out, command->description);
    string_append_char(out, '\n');
  }
  string_append_cstr(out, "\nOptions:\n");

  size_t width = 0;
  for (size_t i = 0; i < command->option_count; i++) {
    const cmdline_option_t *spec = &command->options[i];
    size_t len = 8 + strlen(spec->name);
    if (spec->type != CMDLINE_OPTION_FLAG) {
      len += spec->optional_value ? strlen(" [=<VALUE>]") : strlen(" <VALUE>");
    }
    if (len > width) {
      width = len;
    }
  }
  if (width < strlen("  -h, --help")) {
    width = strlen("  -h, --help");
  }
  for (size_t i = 0; i < command->option_count; i++) {
    append_option_line(allocator, out, &command->options[i], width);
  }
  append_spaces(out, 6);
  string_append_cstr(out, "--help");
  append_spaces(out, width - strlen("      --help") + 2);
  string_append_cstr(out, "Show this help\n");
  return out;
}

static bool has_help_flag(int argc, char **argv) {
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--") == 0) {
      return false;
    }
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      return true;
    }
  }
  return false;
}

int cmdline_run(cmdline_t *self, int argc, char **argv) {
  if (!self) {
    return 2;
  }
  if (!argv || argc < 2) {
    string_t *usage = cmdline_usage(self, self->allocator);
    fprintf(stderr, "%s", string_get_data(usage));
    delete_string(&usage);
    return 2;
  }
  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
    string_t *usage = cmdline_usage(self, self->allocator);
    fprintf(stdout, "%s", string_get_data(usage));
    delete_string(&usage);
    return 0;
  }

  const cmdline_command_t *command = find_command(self, argv[1]);
  if (!command) {
    fprintf(stderr, "error: unknown command '%s'\n", argv[1]);
    string_t *usage = cmdline_usage(self, self->allocator);
    fprintf(stderr, "%s", string_get_data(usage));
    delete_string(&usage);
    return 2;
  }

  if (has_help_flag(argc, argv)) {
    string_t *usage =
        cmdline_command_usage(self, command->name, self->allocator);
    fprintf(stdout, "%s", string_get_data(usage));
    delete_string(&usage);
    return 0;
  }

  string_t *error = NULL;
  cmdline_options_t *options = cmdline_parse(self, argc, argv, &error);
  if (!options) {
    fprintf(
        stderr, "error: %s\n", error ? string_get_data(error) : "parse error");
    string_t *usage =
        cmdline_command_usage(self, command->name, self->allocator);
    fprintf(stderr, "%s", string_get_data(usage));
    delete_string(&usage);
    delete_string(&error);
    return 2;
  }

  int result =
      command->callback ? command->callback(options, command->userdata) : 0;
  delete_cmdline_options(&options);
  return result;
}
