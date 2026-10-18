/*
 * 含 own 槽位的 **tuple / array / slice / option**，以及**赋值点旧值销毁**。
 *
 * 覆盖点（docs/ownership-refactor.md §0 / §7）：
 *   - tuple：逐元素 `FREE t[i]`（own 元素释放、ref 元素跳过、复合元素递归）；
 *   - array `[N]own *T` / `[N]Box`：`for i in 0..len(a) { <元素销毁序列> }`
 *     （运行期 LENGTH，u64 计数器）；
 *   - slice `own []own *T` / `own []Box`：同款循环，元素销毁先于 `FREE s`；
 *   - option `?own *T` / `?Box`：`if (v != nil) { <inner 销毁序列，作用于 v.! > }`；
 *     none 不解包、不释放；
 *   - 赋值点旧值销毁（A 方案 §2.3"旧块摘挂"）：`p = new …` / `a[i] = …` /
 *     `p.* = …` 覆盖自有堆块前先释放旧值；
 *   - 所有权闸门：非 own 位置接含 own 元素的聚合 → 拒绝；ref 值塞 own 元素
 *     槽位 → 拒绝（array / option 两条路径）。
 *
 * 每个正例都断言"退出码 0 + stderr 无 memory leak"（`clux_expect_ok`）。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

const char *kBox = "struct Box { p: own *i32; n: i32 }\n";

} // namespace

/* ============================ tuple ============================ */

TEST(OwnAggregate, TupleOwnElementFreed) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var t: own <own *i32, i32> = .<own *i32, i32>{ new i32{7}, 3 };\n"
      "  if (t[0].* != 7) { return 1; }\n"
      "  if (t[1] != 3) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "oa_tuple");
}

TEST(OwnAggregate, TupleStructElementFreed) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var t: own <Box, i32> = .<Box, i32>{ .Box{ .p = new i32{4}, .n = 4 }, 9 };\n"
                 "  if (t[0].p.* != 4) { return 1; }\n"
                 "  if (t[1] != 9) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "oa_tuple_struct");
}

TEST(OwnAggregate, TupleRefElementNotFreed) {
  /* 同一 tuple 里 ref 元素（借用）与 own 元素共存：销毁只碰 own 元素，
     ref 指向的栈变量不被 FREE。 */
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var x: i32 = 7;\n"
      "  var t: '<*> own <ref *i32, own *i32> =\n"
      "      .<ref *i32, own *i32>{ x.&, new i32{8} };\n"
      "  if (t[0].* != 7) { return 1; }\n"
      "  if (t[1].* != 8) { return 2; }\n"
      "  if (x != 7) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "oa_tuple_ref");
}

/* ============================ array ============================ */

TEST(OwnAggregate, ArrayOwnPointerElementsFreed) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var a: own [2]own *i32 = .[2]own *i32{ new i32{7}, new i32{8} };\n"
      "  if (a[0].* != 7) { return 1; }\n"
      "  if (a[1].* != 8) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "oa_array");
}

TEST(OwnAggregate, ArrayStructElementsFreed) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var a: own [2]Box = .[2]Box{ .Box{ .p = new i32{1}, .n = 1 }, "
                 ".Box{ .p = new i32{2}, .n = 2 } };\n"
                 "  if (a[0].p.* != 1) { return 1; }\n"
                 "  if (a[1].p.* != 2) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "oa_array_struct");
}

TEST(OwnAggregate, ArrayElementReassignmentFreesOld) {
  /* `a[0] = new i32{11}`：INDEX_SET 是纯覆盖，旧元素必须由赋值点销毁序列释放。 */
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var a: own [2]own *i32 = .[2]own *i32{ new i32{1}, new i32{2} };\n"
      "  a[0] = new i32{11};\n"
      "  if (a[0].* != 11) { return 1; }\n"
      "  if (a[1].* != 2) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "oa_array_reassign");
}

/* ============================ slice ============================ */

TEST(OwnAggregate, SliceOwnPointerElementsFreed) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var s: own []own *i32 = make(own *i32, 2, new i32{5}, new i32{6});\n"
      "  if (s[0].* != 5) { return 1; }\n"
      "  if (s[1].* != 6) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "oa_slice");
}

TEST(OwnAggregate, SliceStructElementsReassigned) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var s: own []Box = make(Box, 2,\n"
                 "      .Box{ .p = new i32{1}, .n = 1 }, .Box{ .p = new i32{2}, .n = 2 });\n"
                 "  if (s[0].p.* != 1) { return 1; }\n"
                 "  s = make(Box, 1, .Box{ .p = new i32{9}, .n = 9 });\n"
                 "  if (s[0].p.* != 9) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "oa_slice_struct");
}

/* ============================ option ============================ */

TEST(OwnAggregate, OptionOwnPointerSomeFreed) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var o: own ?own *i32 = .?own *i32{ new i32{7} };\n"
      "  if (o == nil) { return 1; }\n"
      "  if (o.!.* != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "oa_option");
}

TEST(OwnAggregate, OptionStructSomeFreed) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var o: own ?Box = .?Box{ .Box{ .p = new i32{3}, .n = 3 } };\n"
                 "  if (o == nil) { return 1; }\n"
                 "  if (o.!.p.* != 3) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "oa_option_struct");
}

TEST(OwnAggregate, OptionNoneHasNoPayload) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var e: own ?Box = nil;\n"
                 "  if (e != nil) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "oa_option_none");
}

TEST(OwnAggregate, OptionReassignmentFreesOld) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var o: own ?Box = .?Box{ .Box{ .p = new i32{1}, .n = 1 } };\n"
                 "  o = .?Box{ .Box{ .p = new i32{2}, .n = 2 } };\n"
                 "  if (o.!.p.* != 2) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "oa_option_reassign");
}

/* ==================== 赋值点旧值销毁（指针） ==================== */

TEST(OwnAggregate, OwnPointerVariableReassignmentFreesOld) {
  /* `p = new i32{2}`：旧块必须先释放（ptr_assign 是纯 memcpy）。 */
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{1};\n"
      "  p = new i32{2};\n"
      "  if (p.* != 2) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "oa_ptr_reassign");
}

TEST(OwnAggregate, OwnPointerPointeeReassignmentFreesOld) {
  /* `b.* = .Box{…}`：旧被指向值内部 own 槽位先释放，再覆盖。 */
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var b: own *Box = new Box{ .p = new i32{1}, .n = 1 };\n"
                 "  b.* = .Box{ .p = new i32{2}, .n = 2 };\n"
                 "  if (b.n != 2) { return 1; }\n"
                 "  if (b.p.* != 2) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "oa_deref_reassign");
}

/* ============================ 负例 ============================ */

TEST(OwnAggregate, NonOwnArrayOfOwnElementsRejected) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var a: [2]own *i32 = .[2]own *i32{ new i32{1}, new i32{2} };\n"
      "  return 0;\n"
      "}\n",
      "dying value (FATAL)", "oa_reject_nonown_array");
}

TEST(OwnAggregate, RefIntoOwnArrayElementRejected) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var x: i32 = 1;\n"
      "  var r: ref *i32 = x.&;\n"
      "  var a: own [2]own *i32 = .[2]own *i32{ r, new i32{2} };\n"
      "  return 0;\n"
      "}\n",
      "own slot must take over", "oa_reject_ref_elem");
}

TEST(OwnAggregate, RefIntoOwnOptionInnerRejected) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var x: i32 = 1;\n"
      "  var r: ref *i32 = x.&;\n"
      "  var o: own ?own *i32 = .?own *i32{ r };\n"
      "  return 0;\n"
      "}\n",
      "own slot must take over", "oa_reject_ref_option");
}
