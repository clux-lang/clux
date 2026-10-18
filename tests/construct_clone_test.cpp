/*
 * 统一 **NEW / CONSTRUCT** 协议 + **扁平化 clone**。
 *
 * NEW/CONSTRUCT 职责分离（2026-10）：
 *   - `NEW`（无操作数）只申请内存：弹字节数 → 压 opaque 原始块句柄；
 *   - `CONSTRUCT <n>` 在给定 dest 上构造：dest=undefined → 新建值块（栈值）；
 *     dest=opaque → 在 NEW 的原始块上构造（结果 fatal *T）。
 *   于是 `new T{...}` = `PUSH_U64 sizeof(T); NEW; <type>; <fields>; CONSTRUCT n`，
 *   `.T{...}` = `PUSH_UNDEFINED; <type>; <fields>; CONSTRUCT n`。
 *
 * 扁平化 clone：sema 在编译期把 `clone(x)` 展开成「申请 + 赋值」序列
 * （构造 / NEW / CLONE 叶子 / slice 的 make+循环），运行期不再按类型递归
 * ——与销毁序列对称，C 后端可逐条等价翻译。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "test_common.h"

extern "C" {
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/exec.h"
#include "vm/bcode.h"
#include "vm/bcode_asm.h"
#include "vm/bcode_disasm.h"
#include "vm/type.h"
#include "vm/type_ptr.h"
#include "vm/type_error.h"
#include "core/allocator.h"
}

namespace {

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr)     { free(ptr); }

class ConstructProtocolTest : public ::testing::Test {
protected:
  allocator_t *alloc = nullptr;
  vm_t        *vm    = nullptr;
  bytecode_t  *bc    = nullptr;

  void SetUp() override {
    alloc = create_allocator(test_alloc, test_free);
    vm    = vm_new(alloc);
    bc    = bcode_new(alloc);
  }
  void TearDown() override {
    bcode_destroy(&bc);
    vm_destroy(&vm);
    EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
  }

  bool assemble_and_run(const char *src) {
    bcode_destroy(&bc);
    if (bcode_asm_parse(alloc, src, strlen(src), &bc) != 0) return false;
    value_t *r = exec_run(vm, bc);
    if (r && value_is_error(vm, r)) {
      const error_data_t *ed = (const error_data_t *)value_data(r);
      ADD_FAILURE() << "runtime error: " << (ed && ed->message ? ed->message : "?");
      return false;
    }
    return true;
  }

  value_t *stack_top() { return exec_stack_peek(vm, 0); }
};

static int64_t read_sint(const value_t *v) {
  switch (value_type(v)->size) {
  case 1: return (int64_t)*(const int8_t *)value_data(v);
  case 2: return (int64_t)*(const int16_t *)value_data(v);
  case 4: return (int64_t)*(const int32_t *)value_data(v);
  default: return *(const int64_t *)value_data(v);
  }
}

} // namespace

/* NEW = 申请 size 字节原始块 → opaque 句柄；CONSTRUCT 带 dest 在块上构造，
   结果 fatal *T（这里 ptr_get 读回验证）。 */
TEST_F(ConstructProtocolTest, NewAllocatesThenConstructOnHeap) {
  ASSERT_TRUE(assemble_and_run(
      "    push_u64 4\n"      /* sizeof(i32) */
      "    new\n"             /* → opaque 原始块句柄 */
      "    load \"i32\"\n"    /* 构造类型 */
      "    push_i32 7\n"      /* 字段值 */
      "    construct 1\n"     /* dest=块 → 在块上构造 → *i32 */
      "    push_value 0\n"    /* dup 指针 */
      "    ptr_get\n"         /* 解引用读回 → [*i32, i32] */
      "    push_undefined\n"
      "    define \"r\"\n"    /* 存进变量 r（栈: [*i32]） */
      "    dispose\n"         /* 丢弃 *i32：释放 NEW 的堆块（防泄漏） */
      "    push \"r\"\n"
      "    halt\n"));

  value_t *top = stack_top();
  ASSERT_NE(top, nullptr);
  EXPECT_EQ(value_type(top), vm->type_i32);
  EXPECT_EQ(read_sint(top), 7);
}

/* dest=undefined → 栈值构造（不申请堆块）：结果是值本身，不是指针。 */
TEST_F(ConstructProtocolTest, ConstructWithUndefinedDestIsStackValue) {
  ASSERT_TRUE(assemble_and_run(
      "    push_undefined\n"
      "    load \"i32\"\n"
      "    push_i32 42\n"
      "    construct 1\n"
      "    halt\n"));

  value_t *top = stack_top();
  ASSERT_NE(top, nullptr);
  EXPECT_EQ(value_type(top), vm->type_i32);
  EXPECT_EQ(read_sint(top), 42);
}

/* ============================ 扁平化 clone ============================ */

namespace {

const char *kBox = "struct Box { p: own *i32; n: i32 }\n";
const char *kU = "union U { a: own *i32; b: i32 }\n";

} // namespace

TEST(FlatClone, StructValue) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var b: own Box = .Box{ .p = new i32{1}, .n = 1 };\n"
                 "  var c: own Box = clone(b);\n"
                 "  if (c.p.* != 1) { return 1; }\n"
                 "  if (b.p.* != 1) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "fc_struct");
}

TEST(FlatClone, PointerToStruct) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var pb: own *Box = new Box{ .p = new i32{2}, .n = 2 };\n"
                 "  var pc: own *Box = clone(pb);\n"
                 "  if (pc.n != 2) { return 1; }\n"
                 "  if (pc.p.* != 2) { return 2; }\n"
                 "  if (pb.p.* != 2) { return 3; }\n"
                 "  return 0;\n"
                 "}\n",
                 "fc_ptr");
}

TEST(FlatClone, OptionalSome) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var o: own ?Box = .?Box{ .Box{ .p = new i32{3}, .n = 3 } };\n"
                 "  var c: own ?Box = clone(o);\n"
                 "  if (c == nil) { return 1; }\n"
                 "  if (c.!.p.* != 3) { return 2; }\n"
                 "  return 0;\n"
                 "}\n",
                 "fc_option");
}

TEST(FlatClone, OptionalNone) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var o: own ?Box = nil;\n"
                 "  var c: own ?Box = clone(o);\n"
                 "  if (c != nil) { return 1; }\n"
                 "  return 0;\n"
                 "}\n",
                 "fc_option_none");
}

TEST(FlatClone, UnionActiveMember) {
  clux_expect_ok(std::string(kU) +
                 "func main(): i32 {\n"
                 "  var u: own U = .U{ .a = new i32{4} };\n"
                 "  var c: own U = clone(u);\n"
                 "  if (c is a != true) { return 1; }\n"
                 "  if (c.a.* != 4) { return 2; }\n"
                 "  if (u.a.* != 4) { return 3; }\n"
                 "  return 0;\n"
                 "}\n",
                 "fc_union");
}

TEST(FlatClone, TupleElements) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var t: own <own *i32, i32> = .<own *i32, i32>{ new i32{5}, 5 };\n"
      "  var c: own <own *i32, i32> = clone(t);\n"
      "  if (c[0].* != 5) { return 1; }\n"
      "  if (t[0].* != 5) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "fc_tuple");
}

TEST(FlatClone, ArrayElements) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var a: own [2]own *i32 = .[2]own *i32{ new i32{6}, new i32{7} };\n"
      "  var c: own [2]own *i32 = clone(a);\n"
      "  if (c[0].* != 6) { return 1; }\n"
      "  if (c[1].* != 7) { return 2; }\n"
      "  if (a[0].* != 6) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "fc_array");
}

TEST(FlatClone, SliceBufferIndependent) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 3, 1, 2, 3);\n"
      "  var c: own []i32 = clone(s);\n"
      "  c[0] = 99;\n"
      "  if (s[0] != 1) { return 1; }   /* 源缓冲不受影响 */\n"
      "  if (c[0] != 99) { return 2; }\n"
      "  if (len(c) != 3u64) { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "fc_slice");
}

TEST(FlatClone, SliceOfStructs) {
  clux_expect_ok(std::string(kBox) +
                 "func main(): i32 {\n"
                 "  var s: own []Box = make(Box, 2,\n"
                 "      .Box{ .p = new i32{8}, .n = 8 },\n"
                 "      .Box{ .p = new i32{9}, .n = 9 });\n"
                 "  var c: own []Box = clone(s);\n"
                 "  if (c[0].p.* != 8) { return 1; }\n"
                 "  if (c[1].p.* != 9) { return 2; }\n"
                 "  if (s[1].p.* != 9) { return 3; }\n"
                 "  return 0;\n"
                 "}\n",
                 "fc_slice_struct");
}

/* ==================== move 的**字段/元素级**限制（clone 不触碰） ====================
 *
 * TDZ 是**符号级**的（只跟踪变量名）：`move(a.b)` 把资源从字段里掏走，却没有任何
 * 地方记录 `a.b` 已失效——同一字段可重复 move、被读成悬垂值。所以 move 只作用于
 * **整个变量**，字段/元素只能连整个聚合一起 move（sema `ex_move` 约束 1）。
 *
 * 扁平化 clone 与这条限制的关系：
 *   - clone 只**读**槽位（FIELD_GET / INDEX_GET）并拷贝，不改变源的绑定状态；
 *   - 唯一的 move-out（`AST_TAKE`）只作用于 sema 生成的**整个临时变量**（__cl_N），
 *     从不对字段/元素发 move。
 * 下面 4 例把这条边界钉住（前两例拒绝、后两例允许）。
 */

TEST(FlatClone, MoveStructFieldRejected) {
  clux_expect_reject(
      "struct Box { p: own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var b: own Box = .Box{ .p = new i32{1}, .n = 1 };\n"
      "  var q: own *i32 = move(b.p);\n"   /* 字段没有局部 TDZ */
      "  return q.*;\n"
      "}\n",
      "cannot move a struct/aggregate field or element", "fc_move_field");
}

TEST(FlatClone, MoveArrayElementRejected) {
  clux_expect_reject(
      "func main(): i32 {\n"
      "  var a: own [2]own *i32 = .[2]own *i32{ new i32{1}, new i32{2} };\n"
      "  var q: own *i32 = move(a[0]);\n"  /* 元素同理 */
      "  return q.*;\n"
      "}\n",
      "cannot move a struct/aggregate field or element", "fc_move_elem");
}

TEST(FlatClone, CloneStructFieldAllowed) {
  /* 字段级 clone 合法：拷贝不改变源字段的绑定状态，两个块互相独立。 */
  clux_expect_ok(
      "struct Box { p: own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
      "  var q: own *i32 = clone(b.p);\n"
      "  if (q.* != 7) { return 1; }\n"
      "  if (b.p.* != 7) { return 2; }\n"
      "  q.* = 9;\n"
      "  if (b.p.* != 7) { return 3; }\n"   /* 独立堆块 */
      "  return 0;\n"
      "}\n",
      "fc_clone_field");
}

TEST(FlatClone, CloneSliceFieldAllowed) {
  /* 字段级 clone（切片）：扁平化走 prelude 临时变量 + TAKE（对**整个临时变量**
     move），从不对字段发 move；缓冲独立。 */
  clux_expect_ok(
      "struct Holder { s: own []i32 }\n"
      "func main(): i32 {\n"
      "  var h: own Holder = .Holder{ .s = make(i32, 2, 1, 2) };\n"
      "  var c: own []i32 = clone(h.s);\n"
      "  c[0] = 9;\n"
      "  if (h.s[0] != 1) { return 1; }\n"
      "  if (c[0] != 9) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "fc_clone_slice_field");
}