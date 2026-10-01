#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "core/allocator.h"
#include "core/vec.h"
#include "driver/driver.h"
#include "parser/lexer.h"
}

#include "test_common.h"

namespace {

std::string write_temp_file(const std::string &content) {
  auto path = std::filesystem::temp_directory_path() / "clux_test_XXXXXX";
  auto path_str = path.string();
  /* mkstemps is not available on Windows; use a simple unique name. */
  static int counter = 0;
  path_str += std::to_string(counter++);
  FILE *fp = fopen(path_str.c_str(), "wb");
  fwrite(content.data(), 1, content.size(), fp);
  fclose(fp);
  return path_str;
}

} // namespace

/* ---- 类型提升区（hoist）---- */

/* 编译产物含 hoist 区：类型构造收敛到产物最前的提升区（先定义类型，
   再定义函数，函数体最后），槽位发 LOAD_TYPE <id> 引用。
   driver_build_asm 产出的 .cxs 文本应可见
   DEFINE_TYPE / SEAL / PUSH_ARRAY / DEFINE_BOUND / LOAD_TYPE 指令。 */
TEST(Driver, BuildAsmHoistSectionPresent) {
  auto dir = std::filesystem::temp_directory_path() / "clux_hoist";
  std::filesystem::create_directories(dir);
  std::string src = (dir / "prog.cx").string();
  std::string cxs = (dir / "prog.cxs").string();

  {
    FILE *fp = fopen(src.c_str(), "wb");
    const char *code =
        "func main():i32 {\n"
        "  var a = .[3]i32 { 1, 2, 3 };\n"
        "  var s = a[0] + a[1] + a[2];\n"
        "  return s;\n"
        "}\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }

  ASSERT_EQ(driver_build_asm(src.c_str(), cxs.c_str()), 0);
  std::string text = slurp(cxs);
  ASSERT_FALSE(text.empty());

  /* hoist 区：两遍扫描——pass 1 数组 PUSH_ARRAY→DEFINE_TYPE 声明登记 +
     pass 2 数组 LOAD_TYPE→DEFINE_BOUND→SEAL 定义封闭 */
  EXPECT_NE(text.find("DEFINE_TYPE"), std::string::npos);
  EXPECT_NE(text.find("SEAL"), std::string::npos);
  EXPECT_NE(text.find("PUSH_ARRAY"), std::string::npos);
  EXPECT_NE(text.find("DEFINE_BOUND"), std::string::npos);
  /* 槽位引用：函数体 LOAD_TYPE <id> 查表构造数组 */
  EXPECT_NE(text.find("LOAD_TYPE"), std::string::npos);

  std::filesystem::remove_all(dir);
}

/* 顶层 type def 名字绑定插在 hoist 两遍之间（类型定义自动提升）：
   pass 1 声明（PUSH_ARRAY→DEFINE_TYPE <id> 登记开放对象）→ typedef 绑定
   （LOAD_TYPE <id>; PUSH_UNDEFINED; DEFINE "name"）→ pass 2 定义
   （LOAD_TYPE <id>→DEFINE_BOUND→SEAL）。断言 .cxs 文本中 DEFINE "Pair"
   出现在 SEAL（pass 2 收尾）之前。 */
TEST(Driver, BuildAsmTypeDefBindingBetweenHoistPasses) {
  auto dir = std::filesystem::temp_directory_path() / "clux_typedef_hoist";
  std::filesystem::create_directories(dir);
  std::string src = (dir / "prog.cx").string();
  std::string cxs = (dir / "prog.cxs").string();

  {
    FILE *fp = fopen(src.c_str(), "wb");
    const char *code =
        "type Pair = [2]i32;\n"
        "func main():i32 {\n"
        "  var p:Pair = .[2]i32{10, 20};\n"
        "  return p[0];\n"
        "}\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }

  ASSERT_EQ(driver_build_asm(src.c_str(), cxs.c_str()), 0);
  std::string text = slurp(cxs);
  ASSERT_FALSE(text.empty());

  /* typedef 绑定 DEFINE "Pair" 在 hoist 区内：位于 pass 1 的 DEFINE_TYPE 与
     pass 2 的 SEAL 之间（类型定义自动提升） */
  size_t pos_decl = text.find("DEFINE_TYPE");
  size_t pos_bind = text.find("DEFINE \"Pair\"");
  size_t pos_seal = text.find("SEAL");
  ASSERT_NE(pos_decl, std::string::npos);
  ASSERT_NE(pos_bind, std::string::npos);
  ASSERT_NE(pos_seal, std::string::npos);
  EXPECT_LT(pos_decl, pos_bind) << "typedef 绑定应晚于 pass 1 声明";
  EXPECT_LT(pos_bind, pos_seal) << "typedef 绑定应早于 pass 2 定义收尾 SEAL";

  /* 绑定序列：LOAD_TYPE <id>; PUSH_UNDEFINED; DEFINE "Pair" */
  EXPECT_NE(text.find("PUSH_UNDEFINED"), std::string::npos);

  std::filesystem::remove_all(dir);
}

/* 限定符数组组合：数组类型表达式 [N]T 的 T 是任意类型表达式，元素类型带
   const/volatile 天然支持。构造时字面量经"加限定符身份转换"（i32 →
   volatile i32 / const i32，非拓宽非缩窄，身份拷贝），编译运行端到端。 */
TEST(Driver, RunFileQualifiedElementArray) {
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var vp:[2]volatile i32 = .[2]volatile i32{1, 2};\n"
      "  vp[0] = vp[0] + 5;\n"
      "  var cp:[2]const i32 = .[2]const i32{7, 8};\n"
      "  return vp[0] + vp[1] + cp[0] + cp[1];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* typedef 类型提升端到端：别名链 + 数组 + 限定符组合 + 函数签名引用全局
   type def，验证提升区把类型构造收敛到产物最前、typedef 绑定在两遍之间。 */
TEST(Driver, RunFileTypeDefAutoHoist) {
  std::string path = write_temp_file(
      "type Elem = i64;\n"
      "type Pair = [2]Elem;\n"
      "type PairAlias = Pair;\n"
      "type VolatilePair = [2]volatile i32;\n"
      "func sum_pair(p:Pair):i64 { return p[0] + p[1]; }\n"
      "func main():i32 {\n"
      "  var pa:PairAlias = .[2]i64{10, 20};\n"
      "  var s = sum_pair(pa);\n"
      "  var vp:VolatilePair = .[2]volatile i32{1, 2};\n"
      "  vp[0] = vp[0] + 5;\n"
      "  return (s as i32) + vp[0] + vp[1];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* comptime 折叠写回端到端：comptime func 返回数组，调用点折叠为
   AST_CONSTRUCT（类型位 AST_TYPE_REF），编译运行全链路通过。
   hoist 区构造 [3]i32，函数体 LOAD_TYPE 引用，CONSTRUCT 建值，下标求和。 */
TEST(Driver, RunFileComptimeFoldArraySum) {
  std::string path = write_temp_file(
      "comptime func make_arr(): [3]i32 {\n"
      "  var r = .[3]i32{0, 0, 0};\n"
      "  r[0] = 4;\n"
      "  r[1] = 5;\n"
      "  r[2] = 6;\n"
      "  return r;\n"
      "}\n"
      "func main():i32 {\n"
      "  var a = make_arr();\n"
      "  return a[0] + a[1] + a[2];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* const/volatile 类型槽位同样走 hoist 区两遍扫描（pass 1 PUSH_CONST /
   PUSH_VOLATILE → DEFINE_TYPE <id> 声明登记开放对象；pass 2 LOAD_TYPE 拉回
   → LOAD sub → SET_TYPE 设 sub → SEAL 封闭，依赖后序），编译运行端到端。 */
TEST(Driver, RunFileQualifierTypesViaHoist) {
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a:const i32 = 7;\n"
      "  var b:volatile i32 = 3;\n"
      "  return a + b;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
