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

/* ---- const/volatile 限定类型 ---- */

TEST(Driver, ConstTdzFirstAssignAllowed) {
  /* const 变量 TDZ 首次赋值 = 初始化，豁免合法：
     var a:const i32 = undefined; a = 123; */
  std::string path = write_temp_file(
      "func main():i32 { var a:const i32 = undefined; a = 123; return a; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ConstReassignAfterInitRejected) {
  /* const 变量已初始化后再赋值 → 语义错误 */
  std::string path = write_temp_file(
      "func main(): void { var a:const i32 = undefined; a = 123; a = 456; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, ConstInitThenAssignRejected) {
  /* const 变量带初始值定义（flow_init=true）后再赋值 → 语义错误 */
  std::string path =
      write_temp_file("func main(): void { var a:const i32 = 1; a = 2; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, ConstCompoundAssignRejected) {
  /* const 变量复合赋值（读+写）同样禁止 */
  std::string path =
      write_temp_file("func main(): void { var a:const i32 = 1; a += 1; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, ConstReadAndArithmeticAllowed) {
  /* const 变量只读 + 参与运算合法 */
  std::string path = write_temp_file(
      "func main():i32 { var a:const i32 = 5; var b:i32 = a * 2; return b; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, VolatileVarNormalOps) {
  /* volatile 变量读写/运算/赋值全合法（代理子类型） */
  std::string path = write_temp_file(
      "func main():i32 { var a:volatile i32 = 1; a = a + 1; return a; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ConstVolatileCombined) {
  /* const volatile i32（固定组合顺序 volatile(const(i32))）：
     首次赋值豁免，之后禁止 */
  std::string path = write_temp_file(
      "func main():i32 { var a:const volatile i32 = undefined; a = 7; return a; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ConstVolatileReassignRejected) {
  std::string path = write_temp_file(
      "func main(): void { var a:volatile const i32 = undefined; a = 1; a = 2; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, VolatileCompoundAssignAllowed) {
  /* volatile 不影响赋值（非 const），复合赋值合法 */
  std::string path = write_temp_file(
      "func main():i32 { var a:volatile i32 = 1; a += 2; return a; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ConstValueCopyToNonConst) {
  /* const 值可复制到非 const 变量（const T extends T 复制语义） */
  std::string path = write_temp_file(
      "func main():i32 { var a:const i32 = 10; var b:i32 = a; return b; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileAnnotatedWideVarKeepsDeclaredType) {
  /* 显式类型标注 + 默认宽度字面量初始化：DEFINE 须按声明类型 implicit_cast
     init（var x:i64 = 7 的 7 是 i32 字面量，须拓宽为 i64）。此前 DEFINE 直接
     clone init 原值，运行时 x 实为 i32 与 sema 符号表 i64 不一致，后续
     `x = 9 as i64`（同类型赋值）误报 not a widening conversion。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:i64 = 7;\n"
      "  x = 9 as i64;\n"
      "  x = x + 5;\n"
      "  printf(\"%d\\n\", x as i32);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileAnnotatedWideVarInferenceMatchesDeclared) {
  /* var t = x（推断自显式标注的 i64 变量）→ t 运行时亦为 i64，
     与 sema 推断一致；赋值 i64 值合法 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:i64 = 7;\n"
      "  var t = x;\n"
      "  t = 9 as i64;\n"
      "  printf(\"%d\\n\", t as i32);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileAnnotatedWideVarNarrowLiteralRejected) {
  /* 显式 i64 标注 + 不可 widening 的初始化（i64 → i8 收窄）：
     sema 应拦截，运行期不执行 */
  std::string path = write_temp_file(
      "func main(): void { var x:i8 = 300; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* 源码 → .cxb → .cxs → .cxb：两次 .cxb 应字节级一致（往返稳定），
 * 且中间 .cxs 可被执行。 */
TEST(Driver, AsmBinRoundTripStable) {
  auto dir = std::filesystem::temp_directory_path() / "clux_conv_test";
  std::filesystem::create_directories(dir);
  std::string src  = (dir / "prog.cx").string();
  std::string cxb1 = (dir / "prog.cxb").string();
  std::string cxs  = (dir / "prog.cxs").string();
  std::string cxb2 = (dir / "prog2.cxb").string();

  {
    FILE *fp = fopen(src.c_str(), "wb");
    const char *code = "func main():void { printf(\"ok\\n\"); }\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }

  /* 源码 → .cxb 与 .cxs（一次编译） */
  EXPECT_EQ(driver_build_bin(src.c_str(), cxb1.c_str()), 0);
  EXPECT_EQ(driver_build_asm(src.c_str(), cxs.c_str()), 0);

  /* .cxb → .cxs 与源码编译出的 .cxs 应一致（同一反汇编器） */
  std::string cxs2 = (dir / "prog_from_bin.cxs").string();
  EXPECT_EQ(driver_bin_to_asm(cxb1.c_str(), cxs2.c_str()), 0);
  EXPECT_EQ(slurp(cxs), slurp(cxs2));

  /* .cxs → .cxb，与直接编译的 .cxb 字节级一致 */
  EXPECT_EQ(driver_asm_to_bin(cxs.c_str(), cxb2.c_str()), 0);
  EXPECT_EQ(slurp(cxb1), slurp(cxb2));

  /* 中间产物可执行 */
  EXPECT_EQ(driver_run_asm(cxs.c_str()), 0);
  EXPECT_EQ(driver_run_bin(cxb2.c_str()), 0);

  std::filesystem::remove_all(dir);
}

/* 内容嗅探：按内容（而非扩展名）判定输入类型。 */
TEST(Driver, DetectInputByContent) {
  auto dir = std::filesystem::temp_directory_path() / "clux_detect_test";
  std::filesystem::create_directories(dir);

  /* 源码（关键字特征）——扩展名故意用 .txt，验证不依赖扩展名 */
  std::string src = (dir / "a.txt").string();
  {
    FILE *fp = fopen(src.c_str(), "wb");
    const char *code = "func main():void { var x:i32 = 1; }\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_EQ(driver_detect_input(src.c_str()), DRIVER_INPUT_SOURCE);

  /* 汇编文本（助记符/标签特征）——扩展名 .txt */
  std::string cxs = (dir / "b.txt").string();
  {
    FILE *fp = fopen(cxs.c_str(), "wb");
    const char *code = "; comment\nstart:\n    push_i32 1\n    halt\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_EQ(driver_detect_input(cxs.c_str()), DRIVER_INPUT_CXS);

  /* 汇编文本：以助记符行开头（无标签、无注释） */
  std::string cxs2 = (dir / "c.txt").string();
  {
    FILE *fp = fopen(cxs2.c_str(), "wb");
    const char *code = "push_i32 1\nhalt\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_EQ(driver_detect_input(cxs2.c_str()), DRIVER_INPUT_CXS);

  /* 源码首行是 C 风格注释，应跳过注释后按关键字判定 */
  std::string src2 = (dir / "d.txt").string();
  {
    FILE *fp = fopen(src2.c_str(), "wb");
    const char *code = "// clux source\nfunc main():void {}\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_EQ(driver_detect_input(src2.c_str()), DRIVER_INPUT_SOURCE);

  /* 二进制 .cxb（magic）——扩展名故意用 .dat */
  std::string cxsrc = (dir / "prog.cx").string();
  std::string cxb = (dir / "e.dat").string();
  {
    FILE *fp = fopen(cxsrc.c_str(), "wb");
    const char *code = "func main():void {}\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  ASSERT_EQ(driver_build_bin(cxsrc.c_str(), cxb.c_str()), 0);
  EXPECT_EQ(driver_detect_input(cxb.c_str()), DRIVER_INPUT_CXB);

  /* 空文件 / 纯空白 → UNKNOWN */
  std::string empty = (dir / "f.txt").string();
  {
    FILE *fp = fopen(empty.c_str(), "wb");
    const char *code = "   \n\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_EQ(driver_detect_input(empty.c_str()), DRIVER_INPUT_UNKNOWN);

  /* 文件不存在 → UNKNOWN */
  EXPECT_EQ(driver_detect_input((dir / "nope.bin").string().c_str()),
            DRIVER_INPUT_UNKNOWN);

  /* 名称映射稳定 */
  EXPECT_STREQ(driver_input_kind_name(DRIVER_INPUT_SOURCE), "source");
  EXPECT_STREQ(driver_input_kind_name(DRIVER_INPUT_CXS), "cxs");
  EXPECT_STREQ(driver_input_kind_name(DRIVER_INPUT_CXB), "cxb");
  EXPECT_STREQ(driver_input_kind_name(DRIVER_INPUT_UNKNOWN), "unknown");

  std::filesystem::remove_all(dir);
}

/* 内容嗅探驱动的互转：即使扩展名不标准也能正确转换。 */
TEST(Driver, ConvByContentSniffing) {
  auto dir = std::filesystem::temp_directory_path() / "clux_conv_sniff";
  std::filesystem::create_directories(dir);

  /* 用 .txt 扩展名承载汇编文本 → 应能汇编为 .cxb 并执行 */
  std::string weird = (dir / "prog.txt").string();
  {
    FILE *fp = fopen(weird.c_str(), "wb");
    const char *code =
        "_start:\n"
        "    push_func_type\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    push \"void\"\n"
        "    func_type_return\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_function [main]\n"
        "    bind_func 64\n"
        "    set_func_name \"main\"\n"
        "    push_undefined\n"
        "    define \"main\"\n"
        "    halt\n"
        "main:\n"
        "    push_scope\n"
        "    push_undefined\n"
        "    ret\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_EQ(driver_detect_input(weird.c_str()), DRIVER_INPUT_CXS);

  std::string out_cxb = (dir / "out.bin").string();
  EXPECT_EQ(driver_asm_to_bin(weird.c_str(), out_cxb.c_str()), 0);
  EXPECT_EQ(driver_detect_input(out_cxb.c_str()), DRIVER_INPUT_CXB);

  /* 转回文本仍可执行（往返） */
  std::string out_cxs = (dir / "back.txt").string();
  EXPECT_EQ(driver_bin_to_asm(out_cxb.c_str(), out_cxs.c_str()), 0);
  EXPECT_EQ(driver_run_asm(out_cxs.c_str()), 0);
  EXPECT_EQ(driver_run_bin(out_cxb.c_str()), 0);

  std::filesystem::remove_all(dir);
}

/* 互转的失败路径：文件不存在 / 输入内容损坏。 */
TEST(Driver, ConvErrorPaths) {
  auto dir = std::filesystem::temp_directory_path() / "clux_conv_err";
  std::filesystem::create_directories(dir);

  /* 源文件不存在 */
  EXPECT_NE(driver_asm_to_bin((dir / "nope.cxs").string().c_str(),
                              (dir / "out.cxb").string().c_str()), 0);
  EXPECT_NE(driver_bin_to_asm((dir / "nope.cxb").string().c_str(),
                              (dir / "out.cxs").string().c_str()), 0);

  /* .cxs 内容非法（未知助记符） */
  std::string bad_cxs = (dir / "bad.cxs").string();
  {
    FILE *fp = fopen(bad_cxs.c_str(), "wb");
    const char *code = "FOOBAR 1\n";
    fwrite(code, 1, std::strlen(code), fp);
    fclose(fp);
  }
  EXPECT_NE(driver_asm_to_bin(bad_cxs.c_str(),
                              (dir / "bad.cxb").string().c_str()), 0);

  /* .cxb 内容非法（坏 magic） */
  std::string bad_cxb = (dir / "bad.cxb").string();
  {
    FILE *fp = fopen(bad_cxb.c_str(), "wb");
    const char code[] = "NOTABINARYFILE";
    fwrite(code, 1, sizeof code - 1, fp);
    fclose(fp);
  }
  EXPECT_NE(driver_bin_to_asm(bad_cxb.c_str(),
                              (dir / "bad_out.cxs").string().c_str()), 0);

  std::filesystem::remove_all(dir);
}
