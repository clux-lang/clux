/*
 * 编译期所有权 API 已移除：own/ref/fatal/move/clone 直接拒绝。
 * 魔法方法也延后，__ 前缀仍保留给编译器。
 */

#include <gtest/gtest.h>

#include "test_common.h"

TEST(OwnershipRemoved, ModifiersRejected) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{ 7 };\n"
      "  return p.*;\n"
      "}\n",
      "was removed", "own_modifier_removed");
}

TEST(OwnershipRemoved, MoveCloneRejected) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var p: *i32 = new i32{ 7 };\n"
      "  var q: *i32 = move(p);\n"
      "  return q.*;\n"
      "}\n",
      "was removed", "move_removed");
}

TEST(OwnershipRemoved, ShareWeakAreNotKeywords) {
  clux_expect_ok(
      "func share(x: i32): i32 { return x; }\n"
      "func weak(x: i32): i32 { return x + 1; }\n"
      "func main(): i32 { return share(3) + weak(3) - 7; }\n",
      "share_weak_not_keywords");
}

TEST(OwnershipRemoved, MagicNamesReserved) {
  clux_expect_reject(
      "struct R {\n"
      "  v: i32;\n"
      "  func __dispose__(self: *R): void { }\n"
      "}\n"
      "func main(): i32 { return 0; }\n",
      "reserved", "magic_reserved");
}
