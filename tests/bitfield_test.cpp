#include "test_common.h"

TEST(Bitfield, BasicReadWriteAndLayout) {
  clux_expect_ok(
      "bitfield Flags: u32 {\n"
      "  a: 3;\n"
      "  b: 5;\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (sizeof(Flags) != 4usize) { return 1; }\n"
      "  var f: Flags = .Flags{ .a = 1u32, .b = 3u32 };\n"
      "  if (f.a != 1u32 || f.b != 3u32) { return 2; }\n"
      "  f.a = 7u32;\n"
      "  f.b += 2u32;\n"
      "  if (f.a != 7u32 || f.b != 5u32) { return 3; }\n"
      "  var f2: Flags = .Flags{ .a = 0x1Fu32 };\n"
      "  if (f2.a != 7u32 || f2.b != 0u32) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "bitfield_basic");
}

TEST(Bitfield, MultiWordAndCrossing) {
  clux_expect_ok(
      "bitfield Big: u8 {\n"
      "  a: 4;\n"
      "  b: 8;\n"
      "  c: 5;\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (sizeof(Big) != 3usize) { return 1; }\n"
      "  var x: Big = .Big{ .a = 0xFFu8, .b = 0xABu8, .c = 0x1Fu8 };\n"
      "  if (x.a != 0xFu8 || x.b != 0xABu8 || x.c != 0x1Fu8) { return 2; }\n"
      "  x.b = 0x12u8;\n"
      "  if (x.a != 0xFu8 || x.b != 0x12u8) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "bitfield_multi");
}

TEST(Bitfield, U64AndPointerAccess) {
  clux_expect_ok(
      "bitfield B: u64 { a: 40; b: 24; }\n"
      "func main(): i32 {\n"
      "  var x: B = .B{ .a = 0x123456789Au64, .b = 0xABCDEFu64 };\n"
      "  if (x.a != 0x123456789Au64 || x.b != 0xABCDEFu64) { return 1; }\n"
      "  var p: ref *B = x.&;\n"
      "  p.a = 0xFFFFFFFFFFu64;\n"
      "  if (x.a != 0xFFFFFFFFFFu64) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "bitfield_u64_ptr");
}

TEST(Bitfield, StrictRejects) {
  clux_expect_reject(
      "bitfield F: i32 { a: 1; }\n"
      "func main(): i32 { return 0; }\n",
      "must be u8/u16/u32/u64", "bitfield_signed");
  clux_expect_reject(
      "bitfield F: u8 { a: 9; }\n"
      "func main(): i32 { return 0; }\n",
      "width must be in 1..8", "bitfield_width");
  clux_expect_reject(
      "bitfield F: u8 { a: 2; b: 0; }\n"
      "func main(): i32 { return 0; }\n",
      "width must be in 1..8", "bitfield_zero_width");
  clux_expect_reject(
      "bitfield F: u8 { a: 2; a: 3; }\n"
      "func main(): i32 { return 0; }\n",
      "duplicate bitfield field", "bitfield_dup");
}
