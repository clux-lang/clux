/*
 * port 包（纯 FFI 声明）+ clux.mod native 段 + 本地 C 目标文件的端到端测试：
 *
 *   deps/tdport/port.cx
 *     extern lib td from "*";
 *     export extern cfunc td::td_add(a: i32, b: i32): i32;
 *     export extern var td::td_i32: i32;
 *
 *   clux.mod
 *     "native": { "link": ["td.o"] }
 *
 * 验证 import 方可以直接 `tdport::td_add(...)` / `tdport::td_i32 = ...`，
 * 且链接参数由 clux.mod 提供（不要求系统安装库）。
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "test_common.h"

namespace {

std::string q(const std::string &s) { return "\"" + s + "\""; }

std::string host_cc() {
#ifdef _WIN32
  return "clang";
#else
  return "cc";
#endif
}

void write_file(const std::filesystem::path &path, const std::string &text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream ofs(path, std::ios::binary);
  ofs << text;
}

} // namespace

TEST(PortNative, ImportPortPackageLinksLocalCObject) {
  namespace fs = std::filesystem;
  static int seq = 0;
  fs::path root = fs::path(clux_test_temp_dir()) /
                  ("port_native_" + std::to_string(seq++));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "deps" / "tdport", ec);

  write_file(root / "clux.mod",
             "{\n"
             "  \"name\": \"portapp\",\n"
             "  \"version\": \"0.1.0\",\n"
             "  \"entry\": \"main.cx\",\n"
             "  \"native\": { \"link\": [\"td.o\"] }\n"
             "}\n");
  write_file(root / "deps" / "tdport" / "clux.mod",
             "{\n"
             "  \"name\": \"tdport\",\n"
             "  \"version\": \"0.1.0\",\n"
             "  \"entry\": \"port.cx\"\n"
             "}\n");
  write_file(root / "deps" / "tdport" / "port.cx",
             "extern lib td from \"*\";\n"
             "\n"
             "export struct Point { x: i32; y: i32; }\n"
             "\n"
             "export extern cfunc td::td_add(a: i32, b: i32): i32;\n"
             "export extern cfunc td::td_point_sum(p: Point): i32;\n"
             "export extern cfunc td::td_str_first(s: str): i32;\n"
             "export extern var td::td_i32: i32;\n"
             "\n"
             "// 宏等价常量：port 手工写编译期常量，import 方引用点折叠\n"
             "export comptime var PORT_COUNT: const usize = 3usize;\n"
             "export comptime func PORT_ADD(a: i32, b: i32): i32 {\n"
             "    return a + b;\n"
             "}\n");
  write_file(root / "td.c",
             "#include <stdint.h>\n"
             "#include <stddef.h>\n"
             "typedef struct { int32_t x; int32_t y; } Point;\n"
             "int32_t td_i32 = -123456;\n"
             "int32_t td_add(int32_t a, int32_t b) { return a + b; }\n"
             "int32_t td_point_sum(Point p) { return p.x + p.y; }\n"
             "int32_t td_str_first(const char *s) {\n"
             "    return s ? (int32_t)(unsigned char)s[0] : -1;\n"
             "}\n");
  write_file(root / "main.cx",
             "import tdport from \"tdport\";\n"
             "\n"
             "func main(): i32 {\n"
             "    if (tdport::td_add(20, 22) != 42) { return 1; }\n"
             "    var p: tdport::Point = .tdport::Point{ .x = 20, .y = 22 };\n"
             "    if (tdport::td_point_sum(p) != 42) { return 2; }\n"
             "    if (tdport::td_str_first(\"A\") != 65) { return 3; }\n"
             "    var a: [tdport::PORT_COUNT]i32 = .[tdport::PORT_COUNT]i32{\n"
             "        1, 2, 3\n"
             "    };\n"
             "    if (a[2] != 3) { return 4; }\n"
             "    if (tdport::PORT_ADD(20, 22) != 42) { return 5; }\n"
             "    if (tdport::td_i32 != -123456) { return 6; }\n"
             "    tdport::td_i32 = 42;\n"
             "    if (tdport::td_i32 != 42) { return 7; }\n"
             "    return 0;\n"
             "}\n");

  fs::path obj = root / "td.o";
  std::string cmd = host_cc() + " -c " + q((root / "td.c").string()) + " -o " +
                    q(obj.string());
  ASSERT_EQ(std::system(cmd.c_str()), 0) << cmd;

  std::string err;
  const int rc = clux_run_capture((root / "main.cx").string(), &err);
  EXPECT_EQ(rc, 0) << err;
  EXPECT_EQ(err.find("memory leak"), std::string::npos) << err;

  fs::remove_all(root, ec);
}
