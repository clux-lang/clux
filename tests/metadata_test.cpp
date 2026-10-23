#include "test_common.h"

TEST(Metadata, TypeFieldMethod) {
  clux_expect_ok(
      "[[metadata(\"name\", \"user\")]]\n"
      "struct User {\n"
      "  [[metadata(\"unit\", \"px\")]]\n"
      "  x: i32;\n"
      "  [[metadata(\"role\", \"handler\")]]\n"
      "  func run(self: ref *User): i32 { return 1; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var n: str = get_metadata[User](\"name\");\n"
      "  var u: str = get_field_metadata[User](\"x\", \"unit\");\n"
      "  var r: str = get_method_metadata[User](\"run\", \"role\");\n"
      "  if (n != \"user\" || u != \"px\" || r != \"handler\") { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "metadata_members");
}

TEST(Metadata, TypeDecoratorWritesAndReads) {
  clux_expect_ok(
      "comptime func controller[T](url: str): void {\n"
      "  metadata[T](\"controller.url\", url);\n"
      "  var seen: str = get_metadata[T](\"controller.url\");\n"
      "  if (seen != url) { metadata[T](\"controller.ok\", false); }\n"
      "}\n"
      "[[controller(\"/users\")]]\n"
      "struct UserController { }\n"
      "func main(): i32 {\n"
      "  var u: str = get_metadata[UserController](\"controller.url\");\n"
      "  if (u != \"/users\") { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "metadata_type_decorator");
}

TEST(Metadata, FunctionValueAndAlias) {
  clux_expect_ok(
      "func inc(x: i32): i32 { return x + 1; }\n"
      "struct User { }\n"
      "[[metadata(\"handler\", inc)]]\n"
      "type Alias = User;\n"
      "func main(): i32 {\n"
      "  var f = get_metadata[Alias](\"handler\");\n"
      "  if (f(41) != 42) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "metadata_function_alias");
}

TEST(Metadata, RejectsMissingAndDuplicate) {
  clux_expect_reject(
      "[[metadata(\"x\", 1)]]\n"
      "[[metadata(\"x\", 2)]]\n"
      "struct S { }\n"
      "func main(): i32 { return 0; }\n",
      "duplicate metadata key", "metadata_dup");
  clux_expect_reject(
      "struct S { }\n"
      "func main(): i32 { var v: i32 = get_metadata[S](\"missing\"); return 0; }\n",
      "metadata 'missing' not found", "metadata_missing");
}
