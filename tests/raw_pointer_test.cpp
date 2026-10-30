#include <gtest/gtest.h>


#include "test_common.h"

/* clux 没有内建分配：测试用 libc::malloc/free，生命周期完全手工管理。 */
static const char *kLibcAlloc =
    "extern lib libc from \"*\";\n"
    "extern cfunc libc::malloc(size: usize): opaque;\n"
    "extern cfunc libc::free(p: opaque): void;\n";

TEST(RawPointers, MallocWriteFree) {
  std::string src = kLibcAlloc;
  src +=
      "func main(): i32 {\n"
      "    var p: *i32 = libc::malloc(4) as *i32;\n"
      "    p.* = 7 as i32;\n"
      "    defer |p| { libc::free(p as opaque); }\n"
      "    return p.* - 7;\n"
      "}\n";
  clux_expect_ok(src.c_str(), "raw_malloc_free");
}

TEST(RawPointers, MakeSliceFromMallocAndFreeData) {
  std::string src = kLibcAlloc;
  src +=
      "func main(): i32 {\n"
      "    var s: []i32 = make(libc::malloc(12) as *i32, 3);\n"
      "    s[0] = 1 as i32;\n"
      "    s[1] = 2 as i32;\n"
      "    s[2] = 3 as i32;\n"
      "    defer |s| { libc::free(data(s) as opaque); }\n"
      "    return s[2] - 3;\n"
      "}\n";
  clux_expect_ok(src.c_str(), "raw_make_free");
}
