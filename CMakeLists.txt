cmake_minimum_required(VERSION 3.14)
project(clux C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Compiler warnings
#
# Applied to project targets only — third_party (ICU, GoogleTest) keeps its
# own flags. MSVC-style drivers (cl / clang-cl) get /W4, GNU-style drivers
# get -Wall -Wextra -Wpedantic. Diagnostics that are deliberately not
# followed are disabled in this single place, each with a reason.
# ---------------------------------------------------------------------------
function(clux_target_warnings target)
  # clux deliberately uses the portable C API (fopen, tmpnam, ...) instead of
  # the MSVC-only *_s variants, so silence that family of CRT diagnostics.
  if(WIN32)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

add_subdirectory(third_party/icu)

aux_source_directory(src/core CORE_SOURCE)
aux_source_directory(src/parser PARSER_SOURCE)

# ---------------------------------------------------------------------------
# Code coverage (opt-in)
#
#   cmake -B build-cov -DCLUX_COVERAGE=ON
#   cmake --build build-cov --target clux_test
#   ./build-cov/clux_test.exe
#   llvm-profdata merge -sparse *.profraw -o cov.profdata
#   llvm-cov report ./build-cov/clux_test.exe -instr-profile=cov.profdata \
#       --ignore-filename-regex='(third_party|tests|_deps)'
#
# Only the project's own sources are instrumented; third_party (ICU, gtest)
# is excluded so the report focuses on code we own.
# ---------------------------------------------------------------------------
option(CLUX_COVERAGE "Enable LLVM source-based coverage instrumentation" OFF)

function(clux_enable_coverage target)
  if(NOT CLUX_COVERAGE)
    return()
  endif()
  if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "CLUX_COVERAGE requires clang; ignored for ${target}")
    return()
  endif()
  target_compile_options(${target} PRIVATE
    -fprofile-instr-generate
    -fcoverage-mapping)
  target_link_options(${target} PRIVATE -fprofile-instr-generate)
endfunction()

add_executable(clux src/main.c ${CORE_SOURCE} ${PARSER_SOURCE})
clux_target_warnings(clux)
clux_enable_coverage(clux)

# ICU 库编译时禁用了版本后缀重命名 (U_DISABLE_VERSION_SUFFIX=1)，
# 消费方必须同步定义，否则头文件将 u_isIDStart 重命名为 u_isIDStart_74 导致链接失败。
target_compile_definitions(clux PRIVATE U_DISABLE_VERSION_SUFFIX=1)

target_link_libraries(clux PRIVATE
    ICU::data
    ICU::io
)

target_include_directories(clux PRIVATE include)

# ---------------------------------------------------------------------------
# Unit tests (GoogleTest, introduced as a git submodule at third_party/gtest)
# ---------------------------------------------------------------------------
option(BUILD_TESTS "Build the unit test suite" ON)
if(BUILD_TESTS)
  enable_testing()

  # Bring in the GTest::gtest / GTest::gmock targets built from the submodule.
  # Force gtest to use the same (dynamic) MSVC CRT as the rest of the project,
  # otherwise linking the test executable fails with a RuntimeLibrary mismatch
  # (MDd vs MTd) under clang-cl / MSVC. Harmless on Linux (no CRT distinction).
  set(gtest_force_shared_crt ON CACHE BOOL "Use shared (DLL) CRT for gtest" FORCE)
  add_subdirectory(third_party/gtest)

  # Glob every *.cpp under tests/ so new test files are picked up automatically
  # (CONFIGURE_DEPENDS re-runs globbing on each configure when the filesystem changes).
  file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

  # 测试临时文件统一放在构建目录下的 tmp，避免依赖操作系统临时目录。
  set(CLUX_TEST_TMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/tmp")
  file(MAKE_DIRECTORY "${CLUX_TEST_TMP_DIR}")

  add_executable(clux_test ${TEST_SOURCES} ${CORE_SOURCE} ${PARSER_SOURCE})
  target_include_directories(clux_test PRIVATE include)
  target_include_directories(clux_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_compile_definitions(clux_test PRIVATE
      U_DISABLE_VERSION_SUFFIX=1
      CLUX_TEST_TMP_DIR="${CLUX_TEST_TMP_DIR}")
  target_link_libraries(clux_test PRIVATE icu_data_init ICU::data ICU::uc GTest::gtest)
  clux_target_warnings(clux_test)
  clux_enable_coverage(clux_test)
  # 少数端到端用例要断言程序**自己打印的输出**（tests/foreach_test.cpp）：clux 的
  # `libc::printf` 由 FFI 解析到系统 CRT，与测试进程自身 CRT 不是同一实例，进程内
  # fd 重定向抓不到 → 改为子进程跑 CLI（`<同目录>/clux.exe run …`）。因此 CLI 必须
  # 随测试一起构建，否则 `--target clux_test` 会留下一个不存在/过期的 clux.exe。
  add_dependencies(clux_test clux)
  include(GoogleTest)
  gtest_discover_tests(clux_test)
endif()

# libffi/third_party port removed with the VM executor.

