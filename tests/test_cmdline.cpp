#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/cmdline.h"
#include "core/string.h"
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static int g_callback_calls;
static int g_callback_result;
static size_t g_alloc_count;
static size_t g_free_count;

static void *counting_alloc(size_t size) {
  g_alloc_count++;
  return malloc(size);
}

static void counting_free(void *ptr) {
  g_free_count++;
  free(ptr);
}

static int test_callback(const cmdline_options_t *options, void *userdata) {
  (void)options;
  (void)userdata;
  g_callback_calls++;
  return g_callback_result;
}

static bool validate_good(const char *value, string_t *error) {
  if (std::string(value) == "good") {
    return true;
  }
  string_append_cstr(error, "must be 'good'");
  return false;
}

static const cmdline_option_t test_options[] = {
    {"output",
     'o',
     CMDLINE_OPTION_STRING,
     false,
     false,
     NULL,
     "output path",
     NULL},
    {"keep-c", 'k', CMDLINE_OPTION_FLAG, false, false, NULL, "keep C", NULL},
    {"jobs", 0, CMDLINE_OPTION_INT, false, false, NULL, "job count", NULL},
    {"count", 0, CMDLINE_OPTION_UINT, false, false, NULL, "count", NULL},
    {"emit-c", 0, CMDLINE_OPTION_STRING, false, true, "", "emit C", NULL},
    {"backend", 0, CMDLINE_OPTION_STRING, false, false, "c", "backend", NULL},
    {"quality",
     0,
     CMDLINE_OPTION_STRING,
     false,
     false,
     NULL,
     "quality",
     validate_good},
};

static const cmdline_command_spec_t test_command = {
    "build",
    "<file.cx> [options]",
    "Build a file.",
    test_callback,
    NULL,
    test_options,
    sizeof(test_options) / sizeof(test_options[0])};

static const cmdline_option_t required_options[] = {
    {"input", 0, CMDLINE_OPTION_STRING, true, false, NULL, "input", NULL},
};

static const cmdline_command_spec_t required_command = {"required",
                                                        "[options]",
                                                        "Requires input.",
                                                        test_callback,
                                                        NULL,
                                                        required_options,
                                                        1};

static cmdline_t *make_cmdline(allocator_t *a) {
  cmdline_t *cmdline = create_cmdline(a, "clux", "test driver");
  EXPECT_TRUE(cmdline_add_command(cmdline, &test_command));
  EXPECT_TRUE(cmdline_add_command(cmdline, &required_command));
  return cmdline;
}

static cmdline_options_t *parse_args(cmdline_t *cmdline,
                                     std::vector<const char *> args,
                                     string_t **error) {
  std::vector<char *> argv;
  for (const char *arg : args) {
    argv.push_back(const_cast<char *>(arg));
  }
  return cmdline_parse(cmdline, (int)argv.size(), argv.data(), error);
}

TEST(CmdlineTest, ParsesLongAndShortOptions) {
  allocator_t *a = create_allocator(malloc, free);
  cmdline_t *cmdline = make_cmdline(a);
  string_t *error = NULL;
  cmdline_options_t *options = parse_args(cmdline,
                                          {"clux",
                                           "build",
                                           "main.cx",
                                           "--output",
                                           "out",
                                           "-k",
                                           "--jobs=4",
                                           "--count",
                                           "10"},
                                          &error);
  ASSERT_NE(options, nullptr);
  EXPECT_EQ(error, nullptr);
  EXPECT_STREQ(cmdline_options_get_program(options), "clux");
  EXPECT_STREQ(cmdline_options_get_command(options), "build");
  EXPECT_EQ(cmdline_options_get_positional_count(options), 1u);
  EXPECT_STREQ(cmdline_options_get_positional(options, 0), "main.cx");
  EXPECT_STREQ(cmdline_options_get_string(options, "output"), "out");
  EXPECT_TRUE(cmdline_options_get_flag(options, "keep-c"));
  EXPECT_EQ(cmdline_options_get_int(options, "jobs"), 4);
  EXPECT_EQ(cmdline_options_get_uint(options, "count"), 10u);
  EXPECT_TRUE(cmdline_options_has(options, "output"));
  delete_cmdline_options(&options);
  delete_cmdline(&cmdline);
  delete_allocator(&a);
}

TEST(CmdlineTest, ParsesAttachedShortValue) {
  allocator_t *a = create_allocator(malloc, free);
  cmdline_t *cmdline = make_cmdline(a);
  string_t *error = NULL;
  cmdline_options_t *options =
      parse_args(cmdline, {"clux", "build", "-oout", "main.cx"}, &error);
  ASSERT_NE(options, nullptr);
  EXPECT_STREQ(cmdline_options_get_string(options, "output"), "out");
  EXPECT_STREQ(cmdline_options_get_positional(options, 0), "main.cx");
  delete_cmdline_options(&options);
  delete_cmdline(&cmdline);
  delete_allocator(&a);
}

TEST(CmdlineTest, OptionalValueAndDefault) {
  allocator_t *a = create_allocator(malloc, free);
  cmdline_t *cmdline = make_cmdline(a);
  string_t *error = NULL;
  cmdline_options_t *options =
      parse_args(cmdline, {"clux", "build", "--emit-c", "main.cx"}, &error);
  ASSERT_NE(options, nullptr);
  EXPECT_STREQ(cmdline_options_get_string(options, "emit-c"), "");
  EXPECT_STREQ(cmdline_options_get_positional(options, 0), "main.cx");
  delete_cmdline_options(&options);

  options = parse_args(
      cmdline, {"clux", "build", "--emit-c=gen/main.c", "main.cx"}, &error);
  ASSERT_NE(options, nullptr);
  EXPECT_STREQ(cmdline_options_get_string(options, "emit-c"), "gen/main.c");
  delete_cmdline_options(&options);

  options = parse_args(cmdline, {"clux", "build", "main.cx"}, &error);
  ASSERT_NE(options, nullptr);
  EXPECT_STREQ(cmdline_options_get_string(options, "backend"), "c");
  EXPECT_TRUE(cmdline_options_has(options, "backend"));
  delete_cmdline_options(&options);
  delete_cmdline(&cmdline);
  delete_allocator(&a);
}

TEST(CmdlineTest, DoubleDashStopsOptionParsing) {
  allocator_t *a = create_allocator(malloc, free);
  cmdline_t *cmdline = make_cmdline(a);
  string_t *error = NULL;
  cmdline_options_t *options =
      parse_args(cmdline, {"clux", "build", "--", "--not-option"}, &error);
  ASSERT_NE(options, nullptr);
  EXPECT_EQ(cmdline_options_get_positional_count(options), 1u);
  EXPECT_STREQ(cmdline_options_get_positional(options, 0), "--not-option");
  delete_cmdline_options(&options);
  delete_cmdline(&cmdline);
  delete_allocator(&a);
}

TEST(CmdlineTest, RequiredOptionMissing) {
  allocator_t *a = create_allocator(malloc, free);
  cmdline_t *cmdline = make_cmdline(a);
  string_t *error = NULL;
  cmdline_options_t *options =
      parse_args(cmdline, {"clux", "required"}, &error);
  EXPECT_EQ(options, nullptr);
  ASSERT_NE(error, nullptr);
  EXPECT_NE(strstr(string_get_data(error), "missing required option '--input'"),
            nullptr);
  delete_string(&error);
  delete_cmdline(&cmdline);
  delete_allocator(&a);
}

TEST(CmdlineTest, UnknownCommandAndOption) {
  allocator_t *a = create_allocator(malloc, free);
  cmdline_t *cmdline = make_cmdline(a);
  string_t *error = NULL;
  EXPECT_EQ(parse_args(cmdline, {"clux", "nope"}, &error), nullptr);
  ASSERT_NE(error, nullptr);
  EXPECT_NE(strstr(string_get_data(error), "unknown command 'nope'"), nullptr);
  delete_string(&error);

  EXPECT_EQ(parse_args(cmdline, {"clux", "build", "--nope"}, &error), nullptr);
  ASSERT_NE(error, nullptr);
  EXPECT_NE(strstr(string_get_data(error), "unknown option '--nope'"), nullptr);
  delete_string(&error);
  delete_cmdline(&cmdline);
  delete_allocator(&a);
}

TEST(CmdlineTest, InvalidValuesAndFlagWithValue) {
  allocator_t *a = create_allocator(malloc, free);
  cmdline_t *cmdline = make_cmdline(a);
  string_t *error = NULL;
  EXPECT_EQ(parse_args(cmdline, {"clux", "build", "--jobs=x"}, &error),
            nullptr);
  ASSERT_NE(error, nullptr);
  EXPECT_NE(strstr(string_get_data(error), "invalid integer value"), nullptr);
  delete_string(&error);

  EXPECT_EQ(parse_args(cmdline, {"clux", "build", "--count=-1"}, &error),
            nullptr);
  ASSERT_NE(error, nullptr);
  EXPECT_NE(strstr(string_get_data(error), "invalid unsigned value"), nullptr);
  delete_string(&error);

  EXPECT_EQ(parse_args(cmdline, {"clux", "build", "--keep-c=1"}, &error),
            nullptr);
  ASSERT_NE(error, nullptr);
  EXPECT_NE(strstr(string_get_data(error), "does not take a value"), nullptr);
  delete_string(&error);
  delete_cmdline(&cmdline);
  delete_allocator(&a);
}

TEST(CmdlineTest, CustomValidator) {
  allocator_t *a = create_allocator(malloc, free);
  cmdline_t *cmdline = make_cmdline(a);
  string_t *error = NULL;
  EXPECT_EQ(parse_args(cmdline, {"clux", "build", "--quality=bad"}, &error),
            nullptr);
  ASSERT_NE(error, nullptr);
  EXPECT_NE(strstr(string_get_data(error), "must be 'good'"), nullptr);
  delete_string(&error);

  cmdline_options_t *options =
      parse_args(cmdline, {"clux", "build", "--quality=good"}, &error);
  ASSERT_NE(options, nullptr);
  EXPECT_STREQ(cmdline_options_get_string(options, "quality"), "good");
  delete_cmdline_options(&options);
  delete_cmdline(&cmdline);
  delete_allocator(&a);
}

TEST(CmdlineTest, RunInvokesCallback) {
  allocator_t *a = create_allocator(malloc, free);
  cmdline_t *cmdline = make_cmdline(a);
  g_callback_calls = 0;
  g_callback_result = 7;
  char arg0[] = "clux";
  char arg1[] = "build";
  char arg2[] = "main.cx";
  char *argv[] = {arg0, arg1, arg2};
  EXPECT_EQ(cmdline_run(cmdline, 3, argv), 7);
  EXPECT_EQ(g_callback_calls, 1);
  delete_cmdline(&cmdline);
  delete_allocator(&a);
}

TEST(CmdlineTest, UsageListsCommandsAndOptions) {
  allocator_t *a = create_allocator(malloc, free);
  cmdline_t *cmdline = make_cmdline(a);
  string_t *usage = cmdline_usage(cmdline, a);
  EXPECT_NE(strstr(string_get_data(usage), "build"), nullptr);
  EXPECT_NE(strstr(string_get_data(usage), "required"), nullptr);
  delete_string(&usage);

  usage = cmdline_command_usage(cmdline, "build", a);
  EXPECT_NE(strstr(string_get_data(usage), "--output"), nullptr);
  EXPECT_NE(strstr(string_get_data(usage), "--keep-c"), nullptr);
  EXPECT_NE(strstr(string_get_data(usage), "[=<VALUE>]"), nullptr);
  delete_string(&usage);
  delete_cmdline(&cmdline);
  delete_allocator(&a);
}

TEST(CmdlineTest, DuplicateCommandRejected) {
  allocator_t *a = create_allocator(malloc, free);
  cmdline_t *cmdline = make_cmdline(a);
  EXPECT_FALSE(cmdline_add_command(cmdline, &test_command));
  delete_cmdline(&cmdline);
  delete_allocator(&a);
}

TEST(CmdlineTest, NoLeak) {
  g_alloc_count = 0;
  g_free_count = 0;
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  cmdline_t *cmdline = make_cmdline(a);
  string_t *error = NULL;
  cmdline_options_t *options =
      parse_args(cmdline, {"clux", "build", "main.cx", "--jobs=2"}, &error);
  delete_cmdline_options(&options);
  delete_string(&error);
  delete_cmdline(&cmdline);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}
