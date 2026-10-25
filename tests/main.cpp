// clux unit-test entry point.
//
// This file hosts the GoogleTest runner. Individual test suites live in their
// own *.cpp files under tests/ and are globbed into the clux_test target by
// CMake (see the root CMakeLists.txt). Nothing project-specific needs to be
// linked here yet; add libraries to target_link_libraries(clux_test ...) as
// the code under src/ and include/ grows.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>

extern "C" {
#include "icu_data.h"
}

int main(int argc, char **argv) {
  icu_data_init();
  /* 测试产生的临时文件统一落在工作区的 build/tmp/test，不写系统 %TEMP%。
     gtest death-test 与各用例 spawn 的子进程会继承这里的 TMP/TEMP。 */
  std::error_code ec;
  std::filesystem::create_directories("build/tmp/test", ec);
#ifdef _WIN32
  _putenv_s("TMP", "build/tmp/test");
  _putenv_s("TEMP", "build/tmp/test");
#else
  setenv("TMP", "build/tmp/test", 1);
  setenv("TEMP", "build/tmp/test", 1);
#endif
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// Placeholder smoke test so the suite is non-empty and CTest discovery succeeds.
// Real test suites belong in their own tests/*.cpp files.
TEST(Sanity, Smoke) { EXPECT_EQ(1 + 1, 2); }
