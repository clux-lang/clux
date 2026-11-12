#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/json.h"
#include "core/string.h"
#include "parser/node.h"
#include "parser/node_program.h"
#include "parser/parser.h"
#include "parser/program.h"
#include <stdlib.h>
#include <string.h>
#include <string>

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

static std::string
program_to_json(allocator_t *a, const char *source, const char *json_source) {
  parser_t *parser = create_parser(a, "test.cx", source);
  node_t *program = parser_read_program(a, parser);
  json_writer_t *writer = create_json_writer(a, json_source);
  program_node_to_json(writer, program);
  string_t *json = json_writer_release(writer);
  std::string result(string_get_data(json));
  delete_string(&json);
  delete_json_writer(&writer);
  delete_node(&program);
  delete_parser(&parser);
  return result;
}

TEST(NodeJsonTest, ProgramAndStatements) {
  allocator_t *a = create_allocator(malloc, free);
  const char *source = "var x: i32 = 1;";
  std::string json = program_to_json(a, source, source);
  EXPECT_NE(json.find("\"kind\": \"Program\""), std::string::npos);
  EXPECT_NE(json.find("\"stmts\""), std::string::npos);
  EXPECT_NE(json.find("\"kind\": \"StmtVar\""), std::string::npos);
  EXPECT_NE(json.find("\"text\": \"var\""), std::string::npos);
  EXPECT_NE(json.find("\"text\": \"x\""), std::string::npos);
  EXPECT_NE(json.find("\"kind\": \"LiteralNumeric\""), std::string::npos);
  EXPECT_NE(json.find("\"text\": \"1\""), std::string::npos);
  delete_allocator(&a);
}

TEST(NodeJsonTest, NestedExpressionsDispatchByChildType) {
  allocator_t *a = create_allocator(malloc, free);
  const char *source = "var y = a + b * 2;";
  std::string json = program_to_json(a, source, source);
  EXPECT_NE(json.find("\"kind\": \"ExprBinary\""), std::string::npos);
  EXPECT_NE(json.find("\"left\""), std::string::npos);
  EXPECT_NE(json.find("\"right\""), std::string::npos);
  EXPECT_NE(json.find("\"kind\": \"LiteralIdentifier\""), std::string::npos);
  EXPECT_NE(json.find("\"symbol\""), std::string::npos);
  delete_allocator(&a);
}

TEST(NodeJsonTest, DeclarationsAndMembers) {
  allocator_t *a = create_allocator(malloc, free);
  const char *source = "struct S { x: i32; } enum E: u8 { A = 1 } interface I "
                       "{ func f(): void; }";
  std::string json = program_to_json(a, source, source);
  EXPECT_NE(json.find("\"kind\": \"StmtStruct\""), std::string::npos);
  EXPECT_NE(json.find("\"kind\": \"ExprStructDecl\""), std::string::npos);
  EXPECT_NE(json.find("\"kind\": \"StructMembers\""), std::string::npos);
  EXPECT_NE(json.find("\"kind\": \"Field\""), std::string::npos);
  EXPECT_NE(json.find("\"kind\": \"ExprEnumDecl\""), std::string::npos);
  EXPECT_NE(json.find("\"kind\": \"EnumVariants\""), std::string::npos);
  EXPECT_NE(json.find("\"kind\": \"StmtInterface\""), std::string::npos);
  EXPECT_NE(json.find("\"kind\": \"InterfaceMembers\""), std::string::npos);
  delete_allocator(&a);
}

TEST(NodeJsonTest, EscapesStrings) {
  allocator_t *a = create_allocator(malloc, free);
  const char *source = "var s = \"a\\\"b\";";
  std::string json = program_to_json(a, source, source);
  EXPECT_NE(json.find("\\\""), std::string::npos);
  EXPECT_NE(json.find("\\\\"), std::string::npos);
  delete_allocator(&a);
}

TEST(NodeJsonTest, SpanOffsets) {
  allocator_t *a = create_allocator(malloc, free);
  const char *source = "var x = 1;";
  std::string json = program_to_json(a, source, source);
  EXPECT_NE(json.find("\"offset\": 0"), std::string::npos);
  EXPECT_NE(json.find("\"offset\": 10"), std::string::npos);
  delete_allocator(&a);
}

TEST(NodeJsonTest, MissingSourceWritesNullText) {
  allocator_t *a = create_allocator(malloc, free);
  const char *source = "var x = 1;";
  std::string json = program_to_json(a, source, NULL);
  EXPECT_NE(json.find("\"text\": null"), std::string::npos);
  delete_allocator(&a);
}

TEST(NodeJsonTest, NoLeak) {
  g_alloc_count = 0;
  g_free_count = 0;
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  const char *source = "[[test]] export func f(a: i32): i32 { if (a > 0) { "
                       "return a; } return 0; }";
  std::string json = program_to_json(a, source, source);
  EXPECT_FALSE(json.empty());
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}
