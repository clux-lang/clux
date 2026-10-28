#include <gtest/gtest.h>


#include "test_common.h"

TEST(RawPointers, NewAndFree) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "    var p: *i32 = new i32{7};\n"
      "    defer |p| { free(p); }\n"
      "    return p.* - 7;\n"
      "}\n",
      "raw_new_free");
}

TEST(RawPointers, MakeAndFreeSlice) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "    var s: []i32 = make(i32, 3, 1, 2, 3);\n"
      "    defer |s| { free(s); }\n"
      "    return s[2] - 3;\n"
      "}\n",
      "raw_make_free");
}
