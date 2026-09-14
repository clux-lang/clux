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

/* ---- Stage ①: load source ---- */

TEST(Driver, LoadSource) {
  allocator_t *alloc = create_allocator(malloc, free);
  std::string path = write_temp_file("func main() {}");

  const char *data = NULL;
  size_t len = 0;
  EXPECT_EQ(driver_load_source(alloc, path.c_str(), &data, &len), 0);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(len, 14u);
  EXPECT_EQ(std::string(data, len), "func main() {}");

  allocator_free(alloc, (void **)&data);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
  std::remove(path.c_str());
}

TEST(Driver, LoadSourceMissingFile) {
  allocator_t *alloc = create_allocator(malloc, free);
  const char *data = NULL;
  size_t len = 0;
  EXPECT_EQ(driver_load_source(alloc, "no/such/file.cx", &data, &len), -1);
  EXPECT_EQ(data, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
}

/* ---- Stage ②: lex into a pool ---- */

TEST(Driver, LexFileProducesTokens) {
  allocator_t *alloc = create_allocator(malloc, free);
  std::string path = write_temp_file("func main() {\n  return 0;\n}\n");

  vec_t *pool = NULL;
  ASSERT_EQ(driver_lex_file(alloc, path.c_str(), &pool), 0);
  ASSERT_NE(pool, nullptr);
  ASSERT_GT(vec_len(pool), 0u);

  /* Collect kinds in order. */
  std::vector<token_kind_t> kinds;
  for (size_t i = 0; i < vec_len(pool); i++) {
    const token_t *t = (const token_t *)vec_get(pool, i);
    kinds.push_back(token_get_kind(t));
  }

  /* Expect the leading keyword "func" and an EOF terminator. */
  EXPECT_EQ(kinds.front(), TOKEN_TYPE_KEYWORD);
  EXPECT_EQ(kinds.back(), TOKEN_TYPE_EOF);

  vec_free(alloc, &pool);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
  std::remove(path.c_str());
}

/* ---- Stage ①+②+③: run entry point ---- */

TEST(Driver, RunFileValidReturnsZero) {
  /* 合法程序：词法 + 语法 + 语义全通过。main 无返回类型（void），
     函数体不含 return 值，sema 无诊断。 */
  std::string path = write_temp_file("func main() { var x = 1; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileNestedArrayConstructPasses) {
  /* 多维数组构造：内层 construct 的 value_make_array 不得残留 type value
     到操作数栈（回归：曾因此外层 construct 栈布局错位报 missing type slot）。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var m = .[2][2]i32 {\n"
      "    .[2]i32 { 1, 2 },\n"
      "    .[2]i32 { 3, 4 }\n"
      "  };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMultipleArrayConstructsPasses) {
  /* 同一作用域内多次数组构造（三个独立同类型数组）不应残留 type value */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[2]i32 { 1, 2 };\n"
      "  var b = .[2]i32 { 3, 4 };\n"
      "  var c = .[2]i32 { 5, 6 };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileArrayConstructAssignPasses) {
  /* 数组构造 + 数组变量间赋值（不涉及索引） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[2]i32 { 1, 2 };\n"
      "  var b = a;\n"
      "  var c = .[2]i32 { 3, 4 };\n"
      "  var d = c;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileIndexGetPasses) {
  /* 下标右值读取（INDEX_GET）：一维 + 多维 + 变量下标 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[3]i32 { 10, 20, 30 };\n"
      "  var x = a[0] + a[2];\n"
      "  var m = .[2][2]i32 { .[2]i32 { 1, 2 }, .[2]i32 { 3, 4 } };\n"
      "  var y = m[1][0];\n"
      "  var i = 1;\n"
      "  var z = a[i];\n"
      "  return x + y + z;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileIndexSetPasses) {
  /* 下标左值赋值（INDEX_SET）：直接赋值 + 复合赋值 + 变量下标 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[3]i32 { 10, 20, 30 };\n"
      "  a[1] = 99;\n"
      "  a[2] += 1;\n"
      "  a[0] *= 2;\n"
      "  var i = 1;\n"
      "  a[i] -= 2;\n"
      "  var s = a[0] + a[1] + a[2];\n"
      "  return s;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMultiIndexSubscriptRejected) {
  /* a[i,j] 多索引（泛型实参语法预留）落到数组下标 → 诊断 */
  std::string path = write_temp_file(
      "func main() {\n"
      "  var a = .[2]i32 { 1, 2 };\n"
      "  var x = a[0, 1];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileIndexNonArrayRejected) {
  /* 对非数组类型下标 → 诊断 */
  std::string path = write_temp_file(
      "func main() {\n"
      "  var x = 42;\n"
      "  var y = x[0];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileIndexStringIndexRejected) {
  /* 非整数下标 → 诊断 */
  std::string path = write_temp_file(
      "func main() {\n"
      "  var a = .[2]i32 { 1, 2 };\n"
      "  var x = a[\"k\"];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMultidimIndexSetPasses) {
  /* 多维下标左值赋值（借用引用写回直达原数组）：m[1][0] = 7 应生效 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var m = .[2][2]i32 { .[2]i32 { 1, 2 }, .[2]i32 { 3, 4 } };\n"
      "  m[1][0] = 7;\n"
      "  m[0][1] += 10;\n"
      "  var i = 1;\n"
      "  var j = 1;\n"
      "  m[i][j] = 8;\n"
      "  var s = m[0][0] + m[0][1] + m[1][0] + m[1][1];\n"
      "  return s;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMultidimBorrowIsIndependentOnBind) {
  /* 借用引用绑定变量时 materialize 深拷贝：var row = m[1] 是独立副本，
     修改 row 不影响 m（用户确认的借用生命周期语义） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var m = .[2][2]i32 { .[2]i32 { 1, 2 }, .[2]i32 { 3, 4 } };\n"
      "  var row = m[1];\n"
      "  row[0] = 100;\n"
      "  var s = m[1][0] + m[1][1];\n"
      "  return s;\n"   /* m[1] 仍是 3,4 → 7 */
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMissingReturnsOne) {
  EXPECT_EQ(driver_run_file("no/such/file.cx"), 1);
}

TEST(Driver, RunFileLexErrorReturnsOne) {
  std::string path = write_temp_file("@ not a token\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* ---- Stage ④: sema ---- */

TEST(Driver, RunFileSemaErrorReturnsOne) {
  /* 语义错误：实参类型不匹配（str → i32），sema 应快速失败返回 1 */
  std::string path =
      write_temp_file("func foo(a:i32) { } func main() { foo(\"s\"); }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileValidSemaPassesReturnsZero) {
  /* 合法程序：带返回类型（:i32）+ 函数调用 + 变量推断，sema 全通过 */
  std::string path = write_temp_file(
      "func add(a:i32, b:i32):i32 { return a + b; }"
      "func main() { var x = add(1, 2); }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileBareBlockReturnsZero) {
  /* 裸块语句（独立作用域）应完整通过流水线 */
  std::string path = write_temp_file(
      "func main():i32 { var x = 1; { var y = 2; x = x + y; } return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEmptyBareBlockReturnsZero) {
  /* 空裸块语句也应完整通过流水线 */
  std::string path =
      write_temp_file("func main():i32 { var x = 1; { } return x; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

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
      "func main() { var a:const i32 = undefined; a = 123; a = 456; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, ConstInitThenAssignRejected) {
  /* const 变量带初始值定义（flow_init=true）后再赋值 → 语义错误 */
  std::string path =
      write_temp_file("func main() { var a:const i32 = 1; a = 2; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, ConstCompoundAssignRejected) {
  /* const 变量复合赋值（读+写）同样禁止 */
  std::string path =
      write_temp_file("func main() { var a:const i32 = 1; a += 1; }\n");
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
      "func main() { var a:volatile const i32 = undefined; a = 1; a = 2; }\n");
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

/* ---- 落盘格式互转：.cxs ⇄ .cxb ---- */

namespace {

/** 读取整个文件到 std::string（-1 表示不存在）。 */
std::string slurp(const std::string &path) {
  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp) return std::string();
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, fp)) > 0) out.append(buf, n);
  fclose(fp);
  return out;
}

} // namespace

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
        "    push \"void\"\n"
        "    func_type_return\n"
        "    seal\n"
        "    push_function [main]\n"
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

/* ---- 类型提升区（hoist）---- */

/* 编译产物含 hoist 区：类型构造收敛到产物最前的提升区（先定义类型，
   再定义函数，函数体最后），槽位发 LOAD_TYPE <id> 引用。
   driver_build_asm 产出的 .cxs 文本应可见
   BIND_TYPE / PUSH_ARRAY / DEFINE_BOUND / SEAL / LOAD_TYPE 指令。 */
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

  /* hoist 区：内建别名 BIND_TYPE + 数组 PUSH_ARRAY→DEFINE_BOUND→SEAL→BIND_TYPE */
  EXPECT_NE(text.find("BIND_TYPE"), std::string::npos);
  EXPECT_NE(text.find("PUSH_ARRAY"), std::string::npos);
  EXPECT_NE(text.find("DEFINE_BOUND"), std::string::npos);
  EXPECT_NE(text.find("SEAL"), std::string::npos);
  /* 槽位引用：函数体 LOAD_TYPE <id> 查表构造数组 */
  EXPECT_NE(text.find("LOAD_TYPE"), std::string::npos);

  std::filesystem::remove_all(dir);
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

/* const/volatile 类型槽位同样走 hoist 区（CREATE_CONST / CREATE_VOLATILE
   依赖 LOAD sub → BIND_TYPE 提升序列），编译运行端到端。 */
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

