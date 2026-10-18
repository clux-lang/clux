/*
 * 聚合 ref 槽位的**借源载体**（2026-10）。
 *
 * 构造本质上是"逐字段赋值"的语法糖：
 *     var s2 = <undefined>;  s2.q = b.p;        // 底层逻辑
 * 所以 `var s2: S = .S{ .q = b.p };` 与 `s2.q = b.p;` 都必须把 **b** 登记为
 * s2 的借源——s2 里存着指向 b 拥有堆块的 ref；b 被 move 后该 ref 悬垂。
 *
 * 载体规则：
 *   - 聚合变量（含 ref 槽位）拥有借源集合（与 ref 变量同一机制）；
 *   - 构造 / 字段赋值 / 元素赋值 / 解引用赋值 / new / 拷贝都往里合并来源；
 *   - scope 退出统一释放（`sema_scope_release_borrows`）；
 *   - 字段级重绑定无法逐槽位精确回收 → **保守保留**旧来源（只会更严格）。
 */
#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

namespace {

const char *kBox = "struct Box { p: own *i32; n: i32 }\n";
const char *kHolder = "struct Holder { q: ref *i32; m: i32 }\n";

} // namespace

TEST(RefSlotCarrier, ConstructIntoRefSlotBlocksMove) {
  clux_expect_reject(std::string(kBox) + kHolder +
                     "func main(): i32 {\n"
                     "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                     "  var h: Holder = .Holder{ .q = b.p, .m = 1 };\n"
                     "  _ = move(b);\n"     /* 构造 = 字段赋值糖 → b 是 h 的借源 */
                     "  return 0;\n"
                     "}\n",
                     "live borrow(s) still reference it", "rc_ctor");
}

TEST(RefSlotCarrier, FieldAssignmentBlocksMove) {
  clux_expect_reject(std::string(kBox) + kHolder +
                     "func main(): i32 {\n"
                     "  var x: i32 = 0;\n"
                     "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                     "  var h: Holder = .Holder{ .q = x.&, .m = 1 };\n"
                     "  h.q = b.p;\n"
                     "  _ = move(b);\n"
                     "  return 0;\n"
                     "}\n",
                     "live borrow(s) still reference it", "rc_field");
}

TEST(RefSlotCarrier, NewNestedConstructBlocksMove) {
  clux_expect_reject(std::string(kBox) +
                     "struct Inner { q: ref *i32; m: i32 }\n"
                     "struct Outer { inner: Inner; k: i32 }\n"
                     "func main(): i32 {\n"
                     "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                     "  var o: own *Outer = new Outer{\n"
                     "      .inner = .Inner{ .q = b.p, .m = 1 }, .k = 2 };\n"
                     "  _ = move(b);\n"
                     "  return 0;\n"
                     "}\n",
                     "live borrow(s) still reference it", "rc_new_nested");
}

TEST(RefSlotCarrier, ArrayElementRefSlotBlocksMove) {
  clux_expect_reject(std::string(kBox) +
                     "func main(): i32 {\n"
                     "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                     "  var arr: [2]ref *i32 = .[2]ref *i32{ b.p, b.p };\n"
                     "  _ = move(b);\n"
                     "  return 0;\n"
                     "}\n",
                     "live borrow(s) still reference it", "rc_array");
}

TEST(RefSlotCarrier, OptionRefInnerBlocksMove) {
  clux_expect_reject(std::string(kBox) +
                     "func main(): i32 {\n"
                     "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                     "  var o: ?ref *i32 = .?ref *i32{ b.p };\n"
                     "  _ = move(b);\n"
                     "  return 0;\n"
                     "}\n",
                     "live borrow(s) still reference it", "rc_option");
}

TEST(RefSlotCarrier, DerefAssignmentBlocksMove) {
  clux_expect_reject(std::string(kBox) + kHolder +
                     "func main(): i32 {\n"
                     "  var x: i32 = 0;\n"
                     "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                     "  var p: own *Holder = new Holder{ .q = x.&, .m = 1 };\n"
                     "  p.* = .Holder{ .q = b.p, .m = 2 };\n"
                     "  _ = move(b);\n"
                     "  return 0;\n"
                     "}\n",
                     "live borrow(s) still reference it", "rc_deref");
}

TEST(RefSlotCarrier, CopyInheritsSources) {
  clux_expect_reject(std::string(kBox) + kHolder +
                     "func main(): i32 {\n"
                     "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                     "  var h: Holder = .Holder{ .q = b.p, .m = 1 };\n"
                     "  var h2: Holder = h;\n"   /* 拷贝继承 h 的借源 */
                     "  _ = move(b);\n"
                     "  return 0;\n"
                     "}\n",
                     "live borrow(s) still reference it", "rc_copy");
}

TEST(RefSlotCarrier, ScopeEndReleasesSources) {
  clux_expect_ok(std::string(kBox) + kHolder +
                 "func main(): i32 {\n"
                 "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                 "  {\n"
                 "    var h: Holder = .Holder{ .q = b.p, .m = 1 };\n"
                 "    if (h.q.* != 7) { return 1; }\n"
                 "  }\n"
                 "  _ = move(b);\n"          /* 载体已随作用域消亡 → 允许 */
                 "  return 0;\n"
                 "}\n",
                 "rc_scope_end");
}

TEST(RefSlotCarrier, UnrelatedMoveAllowedWithRefSlots) {
  /* 载体只拦**自己的来源**：借源是标量局部变量时，其它 own 值照常可 move。 */
  clux_expect_ok(std::string(kHolder) +
                 "func main(): i32 {\n"
                 "  var x: i32 = 5;\n"
                 "  var h: Holder = .Holder{ .q = x.&, .m = 1 };\n"
                 "  var p: own *i32 = new i32{9};\n"
                 "  _ = move(p);\n"      /* 与 h 的借用无关 → 允许 */
                 "  if (h.q.* != 5) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "rc_unrelated");
}

TEST(RefSlotCarrier, FieldRebindKeepsOldSourceConservatively) {
  /* 字段级重绑定无法逐槽位回收 → 保守保留旧来源（更严格，不漏拦）。 */
  clux_expect_reject(std::string(kBox) + kHolder +
                     "func main(): i32 {\n"
                     "  var x: i32 = 0;\n"
                     "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                     "  var h: Holder = .Holder{ .q = b.p, .m = 1 };\n"
                     "  h.q = x.&;\n"       /* 覆盖了旧 ref，但仍保守保留 b */
                     "  _ = move(b);\n"
                     "  return 0;\n"
                     "}\n",
                     "live borrow(s) still reference it", "rc_rebind");
}
