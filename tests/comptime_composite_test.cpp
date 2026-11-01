#include <gtest/gtest.h>

#include <string>

#include "test_common.h"

/* ============================================================================
 * CTFE 复合类型支持
 *
 * CTFE 的值是真实 data 块（与运行期同一套 type_t 布局），因此复合值访问 =
 * 按类型布局读写 data。覆盖：
 *   struct 字段读写（含嵌套 / 数组元素）、方法调用（值接收者与 `*T` 接收者）、
 *   `x.&` / `p.*`、optional（nil 比较 / `.!` / 结构体字段）、
 *   tag union（构造 / `is` / 成员读）、cunion（构造 / 共享字节）、
 *   切片（`a[lo:hi]` 视图 / `make`）。
 * 每个用例都用 clux_expect_program_ok：程序 main 必须真的返回 0。
 * ========================================================================== */

TEST(DriverComptime, StructFieldReadAndWrite) {
  clux_expect_program_ok(
      "struct P { x: i32; y: i32; }\n"
      "comptime func f(): i32 {\n"
      "  var p: P = .P{ .x = 3, .y = 4 };\n"
      "  p.x = 10;\n"
      "  p.y += 1;\n"
      "  return p.x + p.y;\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 15) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_struct_rw");
}

TEST(DriverComptime, NestedStructAndArrayOfStruct) {
  clux_expect_program_ok(
      "struct Inner { a: i32; }\n"
      "struct Outer { inner: Inner; xs: [2]Inner; }\n"
      "comptime func f(): i32 {\n"
      "  var o: Outer = .Outer{ .inner = .Inner{ .a = 2 },\n"
      "                         .xs = .[2]Inner{ .Inner{ .a = 5 }, .Inner{ .a = 6 } } };\n"
      "  o.inner.a = 10;\n"
      "  o.xs[1].a = 7;\n"
      "  return o.inner.a + o.xs[0].a + o.xs[1].a;\n"   /* 10 + 5 + 7 = 22 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 22) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_struct_nested");
}

TEST(DriverComptime, StructReturnedFromComptimeFunction) {
  clux_expect_program_ok(
      "struct P { x: i32; y: i32; }\n"
      "comptime func mk(a: i32, b: i32): P { return .P{ .x = a, .y = b }; }\n"
      "comptime func f(): i32 {\n"
      "  var p: P = mk(4, 5);\n"
      "  return p.x * 10 + p.y;\n"      /* 45 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 45) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_struct_return");
}

TEST(DriverComptime, MethodWithPointerReceiverMutates) {
  clux_expect_program_ok(
      "struct Counter {\n"
      "  n: i32;\n"
      "  func add(self: *Counter, d: i32): void { self.n = self.n + d; }\n"
      "  func get(self: *const Counter): i32 { return self.n; }\n"
      "}\n"
      "comptime func f(): i32 {\n"
      "  var c: Counter = .Counter{ .n = 1 };\n"
      "  c.add(4);\n"
      "  c.add(5);\n"
      "  return c.get();\n"            /* 10 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 10) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_method_ptr_self");
}

TEST(DriverComptime, MethodWithConstPointerReceiver) {
  clux_expect_program_ok(
      "struct P {\n"
      "  x: i32;\n"
      "  func plus(self: *const P, d: i32): i32 { return self.x + d; }\n"
      "  func twice(self: *P): i32 { return self.x * 2; }\n"
      "}\n"
      "comptime func f(): i32 {\n"
      "  var p: P = .P{ .x = 3 };\n"
      "  var q: P = p;\n"              /* 值拷贝：改 q 不影响 p */
      "  q.x = 100;\n"
      "  return p.plus(5) + p.twice();\n"   /* 8 + 6 = 14 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 14) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_method_value_self");
}

TEST(DriverComptime, PointerAddrAndDeref) {
  clux_expect_program_ok(
      "struct P { x: i32; }\n"
      "comptime func f(): i32 {\n"
      "  var x: i32 = 5;\n"
      "  var p: *i32 = x.&;\n"
      "  p.* = 6;\n"
      "  var v: P = .P{ .x = 7 };\n"
      "  var q: *P = v.&;\n"
      "  q.x = 9;\n"                  /* 指针上的字段写入直达原对象 */
      "  return p.* + v.x + q.x;\n"   /* 6 + 9 + 9 = 24 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 24) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_ptr_deref");
}

TEST(DriverComptime, OptionalNilCompareAndUnwrap) {
  clux_expect_program_ok(
      "comptime func f(): i32 {\n"
      "  var some: ?i32 = 41;\n"
      "  var none: ?i32 = nil;\n"
      "  if (none != nil) { return 1; }\n"
      "  if (some == nil) { return 2; }\n"
      "  return some.! + 1;\n"                /* 42 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 42) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_opt_nil");
}

TEST(DriverComptime, OptionalStructFields) {
  clux_expect_program_ok(
      "struct O { v: ?i32; name: str; }\n"
      "comptime func f(): i32 {\n"
      "  var a: O = .O{ .v = 5, .name = \"x\" };\n"
      "  var b: O = .O{ .v = nil, .name = \"y\" };\n"
      "  if (a.v == nil) { return 1; }\n"
      "  if (b.v != nil) { return 2; }\n"
      "  a.v = nil;\n"
      "  if (a.v != nil) { return 3; }\n"
      "  return 5;\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 5) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_opt_fields");
}

TEST(DriverComptime, TagUnionConstructIsAndRead) {
  clux_expect_program_ok(
      "struct P { x: i32; }\n"
      "union U { i: i32; p: P; name: str; }\n"
      "comptime func f(): i32 {\n"
      "  var a: U = .U{ .i = 42 };\n"
      "  if (!(a is i)) { return 1; }\n"
      "  var b: U = .U{ .p = .P{ .x = 7 } };\n"
      "  if (!(b is p)) { return 2; }\n"
      "  if (a is p) { return 3; }\n"
      "  return a.i + b.p.x;\n"        /* 42 + 7 = 49 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 49) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_union");
}

TEST(DriverComptime, CunionSharesBytes) {
  clux_expect_program_ok(
      "cunion C { i: i32; f: f32; }\n"
      "comptime func f(): i32 {\n"
      "  var c: C = .C{ .i = 7 };\n"
      "  var d: C = c;\n"
      "  d.i = 9;\n"
      "  return c.i + d.i;\n"          /* 7 + 9 = 16，且 c/d 互不影响 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 16) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_cunion");
}

TEST(DriverComptime, ArraySliceViewAliasesStorage) {
  clux_expect_program_ok(
      "comptime func f(): i32 {\n"
      "  var a: [4]i32 = .[4]i32{ 1, 2, 3, 4 };\n"
      "  var s: []i32 = a[1:3];\n"
      "  if (len(s) != 2 as u64) { return 1; }\n"
      "  if (s[0] != 2 || s[1] != 3) { return 2; }\n"
      "  s[0] = 20;\n"                  /* 写切片 → 写回原数组存储 */
      "  if (a[1] != 20) { return 3; }\n"
      "  var t: []i32 = s[1:2];\n"      /* 切片再切片共享缓冲 */
      "  return t[0];\n"                /* 3 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 3) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_slice_view");
}

TEST(DriverComptime, MakeSliceView) {
  clux_expect_program_ok(
      "comptime func f(): i32 {\n"
      "  var a: [3]i32 = .[3]i32{ 7, 8, 9 };\n"
      "  var s: []i32 = make(a[0].&, 2);\n"
      "  if (len(s) != 2 as u64) { return 1; }\n"
      "  s[1] = 80;\n"
      "  return a[1] + s[1];\n"         /* 80 + 80 = 160 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 160) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_make_slice");
}

TEST(DriverComptime, MethodCallingMethodThroughPointerReceiver) {
  /* 回归：`self.bump()` 里 self 是 `*P` 指针值——传给被调方法的 self 必须是
     **被指向对象**的地址（pointee），不能是"指针变量自己的槽位地址"。 */
  clux_expect_program_ok(
      "struct P {\n"
      "  x: i32;\n"
      "  func bump(self: *P): void { self.x = self.x + 1; }\n"
      "  func bump2(self: *P): i32 { self.bump(); self.bump(); return self.x; }\n"
      "}\n"
      "comptime func f(): i32 {\n"
      "  var p: P = .P{ .x = 0 };\n"
      "  return p.bump2();\n"          /* 期望 2 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 2) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_method_nested");
}

TEST(DriverComptime, TupleElementWrite) {
  clux_expect_program_ok(
      "comptime func f(): i32 {\n"
      "  var t: <i32, i32> = .<i32, i32>{ 1, 2 };\n"
      "  t[0] = 9;\n"
      "  t[1] += 1;\n"
      "  return t[0] + t[1];\n"        /* 9 + 3 = 12 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 12) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_tuple_write");
}

TEST(DriverComptime, OptionalArrayElements) {
  clux_expect_program_ok(
      "comptime func f(): i32 {\n"
      "  var a: [2]?i32 = .[2]?i32{ nil, 5 };\n"
      "  if (a[0] != nil) { return 1; }\n"
      "  var b = a[1];\n"
      "  return b.!;\n"                /* 5 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 5) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_opt_array");
}

TEST(DriverComptime, CompositeValueFoldsIntoConstantPosition) {
  /* 复合值参与计算后的结果必须是**真编译期常量**：可用作数组边界与
     static_assert。 */
  clux_expect_program_ok(
      "struct Dim { w: i32; h: i32; }\n"
      "comptime func area(): i32 {\n"
      "  var d: Dim = .Dim{ .w = 3, .h = 4 };\n"
      "  return d.w * d.h;\n"
      "}\n"
      "comptime var N = area();\n"
      "func main(): i32 {\n"
      "  static_assert(area() == 12, \"area\");\n"
      "  var a: [N]i32 = .[12]i32{ <0, 12> };\n"
      "  return a[0] + a[11] + (N - 12);\n"
      "}\n",
      "ct_composite_constant");
}

/* ---- 负例：复合值上的编译期错误 ---- */

TEST(DriverComptime, UnwrapNilRejected) {
  clux_expect_reject(
      "comptime func f(): i32 {\n"
      "  var o: ?i32 = nil;\n"
      "  return o.!;\n"
      "}\n"
      "func main(): i32 { return f(); }\n",
      "unwrap of a nil optional", "ct_unwrap_nil");
}

TEST(DriverComptime, UnknownStructFieldRejected) {
  clux_expect_reject(
      "struct P { x: i32; }\n"
      "comptime func f(): i32 {\n"
      "  var p: P = .P{ .x = 1 };\n"
      "  return p.nope;\n"
      "}\n"
      "func main(): i32 { return f(); }\n",
      "has no field", "ct_bad_field");
}

TEST(DriverComptime, InactiveUnionMemberRejected) {
  clux_expect_reject(
      "union U { i: i32; s: str; }\n"
      "comptime func f(): i32 {\n"
      "  var u: U = .U{ .i = 1 };\n"
      "  return u.s as i32;\n"          /* 当前 tag 是 i */
      "}\n"
      "func main(): i32 { return f(); }\n",
      "is not active", "ct_union_inactive");
}

TEST(DriverComptime, FieldNamedLikeReflectKey) {
  /* 回归：字段名与反射键同名（name/size/...）时，只有**接收者是类型值**才是
     反射访问；`s.name` 是普通字段。 */
  clux_expect_program_ok(
      "struct S { name: str; size: i32; }\n"
      "comptime func f(): i32 {\n"
      "  var s: S = .S{ .name = \"a\", .size = 1 };\n"
      "  s.name = \"bb\";\n"
      "  s.size = s.size + 4;\n"
      "  if (s.name != \"bb\") { return 1; }\n"
      "  return s.size;\n"             /* 5 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 5) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_field_reflect_name");
}

TEST(DriverComptime, ForeachInsideComptime) {
  clux_expect_program_ok(
      "struct Item { v: i32; }\n"
      "comptime func f(): i32 {\n"
      "  var arr: [3]Item = .[3]Item{ .Item{ .v = 1 }, .Item{ .v = 2 },\n"
      "                              .Item{ .v = 3 } };\n"
      "  var sum: i32 = 0;\n"
      "  foreach (var it of arr) { sum = sum + it.v; }\n"
      "  return sum;\n"                /* 6 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 6) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_foreach");
}

TEST(DriverComptime, DataAndMakePointerSliceRoundTrip) {
  /* `data(x)` 取首元素指针，`make(ptr, len)` 再包回切片：切片 ↔ 指针互转。 */
  clux_expect_program_ok(
      "comptime func f(): i32 {\n"
      "  var a: [4]i32 = .[4]i32{ 1, 2, 3, 4 };\n"
      "  var s: []i32 = a[0:4];\n"
      "  var p1: *i32 = data(a);\n"
      "  var p2: *i32 = data(s);\n"
      "  if (p1.* != 1 || p2.* != 1) { return 1; }\n"
      "  var t: []i32 = make(data(a), 2);\n"
      "  if (len(t) != 2 as u64) { return 2; }\n"
      "  t[1] = 20;\n"
      "  if (a[1] != 20) { return 3; }\n"
      "  return p1.* + a[1];\n"        /* 1 + 20 = 21 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 21) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_data_make");
}

TEST(DriverComptime, DataPointerOnStr) {
  clux_expect_program_ok(
      "comptime func f(): i32 {\n"
      "  var s: str = \"abc\";\n"
      "  var p: *u8 = data(s);\n"
      "  if (p.* != 97 as u8) { return 1; }\n"   /* 'a' */
      "  return p.* as i32;\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 97) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_data_str");
}

TEST(DriverComptime, TakeOptInsideComptime) {
  clux_expect_program_ok(
      "comptime func f(): i32 {\n"
      "  var o: ?i32 = 5;\n"
      "  var v = take_opt(o);\n"
      "  if (o != nil) { return 1; }\n"   /* 源被置 nil */
      "  return v + 1;\n"                /* 6 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 6) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_take_opt");
}

TEST(DriverComptime, GenericMethodInsideComptime) {
  /* 泛型方法在 comptime 函数体内由 sema 实例化（改写成直接调用实例 fid），
     CTFE 按普通函数解释——这里锁定该链路。 */
  clux_expect_program_ok(
      "struct Box {\n"
      "  v: i32;\n"
      "  func add[T](self: *Box, d: T): i32 { return self.v + d as i32; }\n"
      "}\n"
      "func id[T](x: T): T { return x; }\n"
      "comptime func f(): i32 {\n"
      "  var b: Box = .Box{ .v = 10 };\n"
      "  var a = b.add(5);\n"            /* 推断类型实参 */
      "  var c = b.add[i32](2);\n"       /* 显式类型实参 */
      "  var d = id[i32](3);\n"
      "  return a + c + d;\n"            /* 15 + 12 + 3 = 30 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 30) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_generic_method");
}

TEST(DriverComptime, ResultTryInsideComptime) {
  /* 错误处理语法糖在编译期：`x.?` 由 sema 展开成 prelude/postlude（临时变量
     + if 分支），CTFE 按语句前置/后置序列执行；失败路径的 `return` 通过
     控制流（ctrl）提前结束函数。同时覆盖 `.?T{v}` optional 构造与
     `Res::of_value` 静态方法调用。 */
  clux_expect_program_ok(
      "struct Res {\n"
      "  good: bool;\n"
      "  val: ?i32;\n"
      "  err: ?i32;\n"
      "  func ok(self: *const Res): bool { return self.good; }\n"
      "  func value(self: *const Res): i32 { return self.val.!; }\n"
      "  func error(self: *const Res): i32 { return self.err.!; }\n"
      "  func panic(self: *const Res): str { return \"res\"; }\n"
      "  func of_value(v: i32): Res {\n"
      "    return .Res{ .good = true, .val = .?i32{ v }, .err = nil };\n"
      "  }\n"
      "  func of_error(e: i32): Res {\n"
      "    return .Res{ .good = false, .val = nil, .err = .?i32{ e } };\n"
      "  }\n"
      "}\n"
      "comptime func half(x: i32): Res {\n"
      "  if (x < 0) { return Res::of_error(0 - x); }\n"
      "  return Res::of_value(x / 2);\n"
      "}\n"
      "comptime func use(x: i32): Res {\n"
      "  var v = half(x).?;\n"
      "  return Res::of_value(v + 1);\n"
      "}\n"
      "comptime func f(): i32 {\n"
      "  var a: Res = use(8);\n"
      "  if (!a.ok()) { return 1; }\n"
      "  if (a.value() != 5) { return 2; }\n"
      "  var b: Res = use(0 - 3);\n"
      "  if (b.ok()) { return 3; }\n"
      "  if (b.error() != 3) { return 4; }\n"
      "  return a.value() + b.error();\n"     /* 5 + 3 = 8 */
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 8) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_result_try");
}

TEST(DriverComptime, BangOnErrorResultRejected) {
  /* `.!` 在错误 result 上：编译期 panic（带 result 的 panic() 消息）。 */
  clux_expect_reject(
      "struct Res {\n"
      "  good: bool;\n"
      "  val: ?i32;\n"
      "  func ok(self: *const Res): bool { return self.good; }\n"
      "  func value(self: *const Res): i32 { return self.val.!; }\n"
      "  func panic(self: *const Res): str { return \"bad result\"; }\n"
      "  func of_error(e: i32): Res { return .Res{ .good = false, .val = nil }; }\n"
      "}\n"
      "comptime func f(): i32 {\n"
      "  var r: Res = Res::of_error(1);\n"
      "  return r.!;\n"
      "}\n"
      "func main(): i32 { return f(); }\n",
      "'.!' on an error result (panic: bad result)", "ct_bang_result");
}

TEST(DriverComptime, PanicInsideComptimeRejected) {
  /* 编译期 panic：不会真的 abort 编译器，而是变成一条诊断。 */
  clux_expect_reject(
      "comptime func f(): i32 {\n"
      "  panic(\"boom\");\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 { return f(); }\n",
      "compile-time panic: boom", "ct_panic");
}

TEST(DriverComptime, CfuncPtrInsideComptimeRejected) {
  /* cfunc_ptr 无编译期表示：给出明确诊断（此前会误派发到 fid 0 的内建）。 */
  clux_expect_reject(
      "cfunc cb(x: i32): i32 { return x + 1; }\n"
      "comptime func f(): i32 {\n"
      "  var p = cfunc_ptr(cb);\n"
      "  _ = p;\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 { return f(); }\n",
      "cfunc_ptr() is not available in compile-time code", "ct_cfunc_ptr");
}

TEST(DriverComptime, GenericFunctionInsideComptime) {
  /* CTFE 复用 sema 的实例化 walk：泛型函数在 comptime 调用时实例体里的
     `sizeof(T)` / `var x: T` 必须按已绑定的类型参数解释。 */
  clux_expect_program_ok(
      "func sz[T](): usize { return sizeof(T); }\n"
      "func mk[T](v: T): T { var x: T = v; return x; }\n"
      "comptime func f(): i32 {\n"
      "  if (sz[i32]() != 4usize) { return 1; }\n"
      "  return mk[i32](7);\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "ct_generic_fn");
}

TEST(DriverComptime, GenericMethodWithFunctionValueArgument) {
  /* CTFE 复用函数值调用路径：泛型方法拿到的函数值参数（`map(f)`）
     在 comptime 里也能调用。 */
  clux_expect_program_ok(
      "struct Box[T] {\n"
      "  v: T;\n"
      "  func map[U](self: *Box[T], g: func(T) -> U): U { return g(self.v); }\n"
      "}\n"
      "func inc(x: i32): i32 { return x + 1; }\n"
      "comptime func f(): i32 {\n"
      "  var b: Box[i32] = .Box[i32]{ .v = 5 };\n"
      "  return b.map[i32](inc);\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 6) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_generic_method_fn");
}

TEST(DriverComptime, FunctionValueLocalCalledInsideComptime) {
  /* 局部变量里存的函数值在 comptime 里调用（不再要求"名字必须是函数"）。 */
  clux_expect_program_ok(
      "func inc(x: i32): i32 { return x + 1; }\n"
      "comptime func f(): i32 {\n"
      "  var g: func(i32) -> i32 = inc;\n"
      "  return g(4);\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 5) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_func_value_local");
}

TEST(DriverComptime, StaticMethodInsideComptime) {
  clux_expect_program_ok(
      "struct P {\n"
      "  x: i32;\n"
      "  func create(v: i32): P { return .P{ .x = v }; }\n"
      "  func get(self: *const P): i32 { return self.x; }\n"
      "}\n"
      "comptime func f(): i32 {\n"
      "  var p: P = P::create(9);\n"
      "  return p.get();\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (f() != 9) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ct_static_method");
}

TEST(DriverComptime, BuiltinWithoutFunctionObjectAsValueRejected) {
  /* 回归：data/take_opt/panic/cfunc_ptr 的 sema 符号没有运行期函数对象，
     fid 必须是 FUNC_ID_NONE 哨兵而不是 0（0 是真实内建 len）。按 fid 回落
     会静默调用 len；按值引用则应给出明确诊断。 */
  clux_expect_reject(
      "comptime func f(): i32 {\n"
      "  var g = panic;\n"
      "  _ = g;\n"
      "  return 0;\n"
      "}\n"
      "func main(): i32 { return f(); }\n",
      "builtin 'panic' has no compile-time value", "ct_builtin_value");
}

TEST(DriverComptime, SliceOutOfBoundsRejected) {
  clux_expect_reject(
      "comptime func f(): i32 {\n"
      "  var a: [3]i32 = .[3]i32{ 1, 2, 3 };\n"
      "  var s: []i32 = a[0:5];\n"
      "  return s[0];\n"
      "}\n"
      "func main(): i32 { return f(); }\n",
      "out of bounds", "ct_slice_oob");
}
