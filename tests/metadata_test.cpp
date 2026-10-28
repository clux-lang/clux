#include "test_common.h"

TEST(Metadata, TypeFieldMethod) {
  clux_expect_ok(
      "[[metadata(\"name\", \"user\")]]\n"
      "struct User {\n"
      "  [[metadata(\"unit\", \"px\")]]\n"
      "  x: i32;\n"
      "  [[metadata(\"role\", \"handler\")]]\n"
      "  func run(self: *User): i32 { return 1; }\n"
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

TEST(Metadata, GenericInstances) {
  clux_expect_ok(
      "comptime func controller[T](url: str): void {\n"
      "  metadata[T](\"controller.url\", url);\n"
      "}\n"
      "[[controller(\"/box\")]]\n"
      "[[metadata(\"size\", sizeof(T))]]\n"
      "struct Box[T] {\n"
      "  [[metadata(\"unit\", \"px\")]] x: T;\n"
      "  [[metadata(\"role\", \"getter\")]]\n"
      "  func get(self: *Box[T]): T { return self.x; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: usize = get_metadata[Box[i32]](\"size\");\n"
      "  var b: usize = get_metadata[Box[i64]](\"size\");\n"
      "  var u: str = get_field_metadata[Box[i32]](\"x\", \"unit\");\n"
      "  var r: str = get_method_metadata[Box[i32]](\"get\", \"role\");\n"
      "  var c: str = get_metadata[Box[i32]](\"controller.url\");\n"
      "  if (a != 4usize || b != 8usize || u != \"px\" || r != \"getter\" ||\n"
      "      c != \"/box\") { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "metadata_generic");
}

TEST(Metadata, CompileTimeGenericRead) {
  clux_expect_ok(
      "[[metadata(\"size\", sizeof(T))]]\n"
      "struct Box[T] { x: T; }\n"
      "comptime func check[U](): void {\n"
      "  var n: usize = get_metadata[Box[i32]](\"size\");\n"
      "  if (n == 4usize) { metadata[U](\"ok\", true); }\n"
      "}\n"
      "[[check]]\n"
      "struct S { }\n"
      "func main(): i32 {\n"
      "  var b: bool = get_metadata[S](\"ok\");\n"
      "  if (!b) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "metadata_ct_generic");
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
