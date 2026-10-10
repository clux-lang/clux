#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <string>

extern "C" {
#include "driver/driver.h"
}

namespace {

std::string write_temp_file(const std::string &name, const std::string &content) {
  auto path = std::filesystem::temp_directory_path() / name;
  auto path_str = path.string();
  FILE *fp = fopen(path_str.c_str(), "wb");
  fwrite(content.data(), 1, content.size(), fp);
  fclose(fp);
  /* 统一为正斜杠——clux 字符串字面量中反斜杠是转义符 */
  for (auto &c : path_str) if (c == '\\') c = '/';
  return path_str;
}

} // namespace

/* ================================================================
 * M5 模块系统端到端：import + :: 模块成员访问
 * ================================================================ */

TEST(DriverImport, ImportExportedFunc) {
  /* 被导入模块导出函数，主模块通过 alias::func 调用 */
  std::string mod_path = write_temp_file("clux_import_mod1.clux",
      "export func add(a: i32, b: i32): i32 { return a + b; }\n");
  std::string main_path = write_temp_file("clux_import_main1.clux",
      "import math from \"" + mod_path + "\";\n"
      "func main(): void {\n"
      "  var x = math::add(3, 4);\n"
      "  printf(\"%d\\n\", x);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
  std::remove(mod_path.c_str());
  std::remove(main_path.c_str());
}

TEST(DriverImport, ImportExportedVar) {
  /* 被导入模块导出全局变量，主模块通过 alias::var 读取 */
  std::string mod_path = write_temp_file("clux_import_mod2.clux",
      "export var PI: i32 = 42;\n");
  std::string main_path = write_temp_file("clux_import_main2.clux",
      "import math from \"" + mod_path + "\";\n"
      "func main(): void {\n"
      "  var p = math::PI;\n"
      "  printf(\"%d\\n\", p);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
  std::remove(mod_path.c_str());
  std::remove(main_path.c_str());
}

TEST(DriverImport, ImportMixedExports) {
  /* 同时导出函数和变量 */
  std::string mod_path = write_temp_file("clux_import_mod3.clux",
      "export var PI: i32 = 42;\n"
      "export func add(a: i32, b: i32): i32 { return a + b; }\n");
  std::string main_path = write_temp_file("clux_import_main3.clux",
      "import math from \"" + mod_path + "\";\n"
      "func main(): void {\n"
      "  var x = math::add(3, 4);\n"
      "  var p = math::PI;\n"
      "  printf(\"%d %d\\n\", x, p);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
  std::remove(mod_path.c_str());
  std::remove(main_path.c_str());
}

TEST(DriverImport, ImportComptimeFoldedExports) {
  /* comptime var / comptime func 折叠结果通过 export var 导出。
     - comptime var BASE = 100 → 折叠到 export var COMPUTED = BASE + 50 = 150
     - comptime func helper(7) = 21 → 折叠到 export var HELPED = 21 */
  std::string mod_path = write_temp_file("clux_import_mod4.clux",
      "comptime var BASE = 100;\n"
      "export var COMPUTED: i32 = BASE + 50;\n"
      "comptime func helper(x: i32): i32 { return x * 3; }\n"
      "export var HELPED: i32 = helper(7);\n");
  std::string main_path = write_temp_file("clux_import_main4.clux",
      "import ct from \"" + mod_path + "\";\n"
      "func main(): void {\n"
      "  var c = ct::COMPUTED;\n"
      "  var h = ct::HELPED;\n"
      "  printf(\"%d %d\\n\", c, h);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
  std::remove(mod_path.c_str());
  std::remove(main_path.c_str());
}
