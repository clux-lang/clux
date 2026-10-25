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

# M6 FFI：libffi（Windows x64 + clang 的 CMake 端口；见 third_party/libffi-port）
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_subdirectory(third_party/libffi-port)
endif()
# M6 FFI：POSIX 用**系统 libffi**（Ubuntu: libffi-dev 提供 -lffi 与 ffi.h）。
# 统一成同一个 FFI::ffi 目标，下游（clux_vm / 测试）不必区分来源：Windows 走
# third_party 端口，Linux/macOS 走系统库，两者都带 CLUX_HAVE_LIBFFI=1。
if(NOT TARGET FFI::ffi AND UNIX)
  find_library(CLUX_LIBFFI_LIBRARY NAMES ffi libffi)
  if(CLUX_LIBFFI_LIBRARY)
    add_library(FFI::ffi INTERFACE IMPORTED)
    set_target_properties(FFI::ffi PROPERTIES
      INTERFACE_LINK_LIBRARIES "${CLUX_LIBFFI_LIBRARY}"
      INTERFACE_COMPILE_DEFINITIONS "CLUX_HAVE_LIBFFI=1")
    message(STATUS "FFI: using system libffi: ${CLUX_LIBFFI_LIBRARY}")
  else()
    message(STATUS "FFI: system libffi not found (typed FFI calls disabled)")
  endif()
endif()

add_executable(clux src/main.c)
clux_target_warnings(clux)

# ---------------------------------------------------------------------------
# Core library (allocator + runtime support)
# ---------------------------------------------------------------------------
add_library(clux_core STATIC src/core/allocator.c src/core/arena.c src/core/panic.c src/core/vec.c src/core/rbtree.c src/core/omap.c src/core/strmap.c src/core/stream.c src/core/string.c src/core/json.c src/core/ffi_cond.c)
target_include_directories(clux_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(clux_core PUBLIC ICU::uc ICU::i18n)
clux_target_warnings(clux_core)

# ---------------------------------------------------------------------------
# Parser library (tokenizer + ast parser)
#
# Only src/parser/lexer.c exists so far; src/parser/parser.c (T4) is globbed
# in automatically once it lands.
# ---------------------------------------------------------------------------
file(GLOB CLUX_PARSER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/parser/*.c")
add_library(clux_parser STATIC ${CLUX_PARSER_SOURCES})
target_include_directories(clux_parser PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(clux_parser PUBLIC clux_core ICU::uc ICU::i18n)
clux_target_warnings(clux_parser)

# ---------------------------------------------------------------------------
# VM library (virtual machine: value, scope, type, function)
# ---------------------------------------------------------------------------
file(GLOB CLUX_VM_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/*.c")
add_library(clux_vm STATIC ${CLUX_VM_SOURCES})
target_include_directories(clux_vm PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(clux_vm PUBLIC clux_core clux_parser clux_module ICU::uc ICU::i18n)
# M6 FFI：libffi（third_party/libffi-port；win64.S 经预处理剥 cfi_*）
if(TARGET FFI::ffi)
  target_link_libraries(clux_vm PUBLIC FFI::ffi)
endif()
# M6 FFI：POSIX 下 dlopen/dlsym/dlclose 在 libdl（Windows 无需）
if(UNIX)
  target_link_libraries(clux_vm PUBLIC ${CMAKE_DL_LIBS})
endif()
clux_target_warnings(clux_vm)

# ---------------------------------------------------------------------------
# Semantic analysis + diagnostics
#
# src/sema and src/diag are still empty (pipeline stages T5/T6). These
# targets materialize as soon as the first *.c file lands in the directory,
# so no CMake edit is needed when they are added.
# ---------------------------------------------------------------------------
set(CLUX_STAGE_LIBS "")

file(GLOB CLUX_SEMA_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/sema/*.c")
if(CLUX_SEMA_SOURCES)
  add_library(clux_sema STATIC ${CLUX_SEMA_SOURCES})
  target_include_directories(clux_sema PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(clux_sema PUBLIC clux_core clux_parser clux_vm clux_desugar ICU::uc ICU::i18n)
  clux_target_warnings(clux_sema)
  list(APPEND CLUX_STAGE_LIBS clux_sema)
endif()

# ---------------------------------------------------------------------------
# 前脱糖层（parser → sema 之间的纯 AST 变换）
# ---------------------------------------------------------------------------
file(GLOB CLUX_DESUGAR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/desugar/*.c")
if(CLUX_DESUGAR_SOURCES)
  add_library(clux_desugar STATIC ${CLUX_DESUGAR_SOURCES})
  target_include_directories(clux_desugar PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(clux_desugar PUBLIC clux_core clux_parser clux_diag ICU::uc ICU::i18n)
  clux_target_warnings(clux_desugar)
  list(APPEND CLUX_STAGE_LIBS clux_desugar)
endif()
file(GLOB CLUX_DIAG_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/diag/*.c")
if(CLUX_DIAG_SOURCES)
  add_library(clux_diag STATIC ${CLUX_DIAG_SOURCES})
  target_include_directories(clux_diag PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(clux_diag PUBLIC clux_core)
  clux_target_warnings(clux_diag)
  list(APPEND CLUX_STAGE_LIBS clux_diag)
endif()

# ---------------------------------------------------------------------------
# Compiler library (AST -> bytecode)
#
# Materializes as soon as the first *.c file lands in src/compiler/, so the
# target only exists once the compile stage is actually implemented.
# ---------------------------------------------------------------------------
file(GLOB CLUX_COMPILER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compiler/*.c")
if(CLUX_COMPILER_SOURCES)
  add_library(clux_compiler STATIC ${CLUX_COMPILER_SOURCES})
  target_include_directories(clux_compiler PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(clux_compiler PUBLIC clux_core clux_parser clux_vm clux_sema clux_diag ICU::uc ICU::i18n)
  clux_target_warnings(clux_compiler)
  list(APPEND CLUX_STAGE_LIBS clux_compiler)
endif()

# ---------------------------------------------------------------------------
# Module library (neutral module-path utilities)
#
# Path classification/normalization/resolution used by BOTH driver (entry path,
# dependency re-resolution) and sema (import statements). Lives below both so
# neither has to depend on the other — it must not include driver or sema headers.
# ---------------------------------------------------------------------------
file(GLOB CLUX_MODULE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/module/*.c")
if(CLUX_MODULE_SOURCES)
  add_library(clux_module STATIC ${CLUX_MODULE_SOURCES})
  target_include_directories(clux_module PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(clux_module PUBLIC clux_core ICU::uc ICU::i18n)
  clux_target_warnings(clux_module)
endif()

# ---------------------------------------------------------------------------
# CTFE library (compile-time evaluation: interpret AST into real values)
#
# Materializes as soon as the first *.c file lands in src/ctfe/.
# ---------------------------------------------------------------------------
file(GLOB CLUX_CTFE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ctfe/*.c")
if(CLUX_CTFE_SOURCES)
  add_library(clux_ctfe STATIC ${CLUX_CTFE_SOURCES})
  target_include_directories(clux_ctfe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(clux_ctfe PUBLIC clux_core clux_parser clux_vm clux_sema clux_diag ICU::uc ICU::i18n)
  clux_target_warnings(clux_ctfe)
  list(APPEND CLUX_STAGE_LIBS clux_ctfe)
endif()

# ---------------------------------------------------------------------------
# Driver library (pipeline orchestration: load -> lex -> ...)
#
# Only src/driver/driver.c exists so far; later stages (parser/sema/interp)
# land here as additional *.c files (GLOB collects them automatically).
# ---------------------------------------------------------------------------
file(GLOB CLUX_DRIVER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/driver/*.c")
add_library(clux_driver STATIC ${CLUX_DRIVER_SOURCES})
target_include_directories(clux_driver PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(clux_driver PUBLIC clux_core clux_parser clux_vm clux_sema clux_desugar clux_diag clux_compiler clux_module ICU::uc ICU::i18n)
clux_target_warnings(clux_driver)

# ---------------------------------------------------------------------------
# C backend (single-file: sema AST -> C IR -> writer -> host C compiler)
# ---------------------------------------------------------------------------
file(GLOB CLUX_CBACKEND_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cbackend/*.c")
if(CLUX_CBACKEND_SOURCES)
  add_library(clux_cbackend STATIC ${CLUX_CBACKEND_SOURCES})
  target_include_directories(clux_cbackend PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(clux_cbackend PUBLIC clux_core clux_parser clux_vm
      clux_sema clux_desugar clux_diag ICU::uc ICU::i18n)
  clux_target_warnings(clux_cbackend)
  list(APPEND CLUX_STAGE_LIBS clux_cbackend)
endif()

# ---------------------------------------------------------------------------
# Command library (subcommand dispatch framework)
# ---------------------------------------------------------------------------
add_library(clux_cmd STATIC
    src/cmd/cmd.c
    src/cmd/format.c
    src/cmd/path.c
    src/cmd/package.c
    src/cmd/build.c
    src/cmd/run.c
    src/cmd/test.c
    src/cmd/version.c
)
target_include_directories(clux_cmd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(clux_cmd PUBLIC clux_core clux_driver clux_ctfe clux_module clux_cbackend ICU::uc icu_data_init)
clux_target_warnings(clux_cmd)

target_link_libraries(clux PRIVATE
    clux_cmd
    clux_core
    clux_parser
    clux_driver
    clux_vm
    ${CLUX_STAGE_LIBS}
    ICU::data
    ICU::io
)

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

  add_executable(clux_test ${TEST_SOURCES})
  target_include_directories(clux_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(clux_test PRIVATE clux_core clux_cmd clux_parser clux_driver clux_vm ${CLUX_STAGE_LIBS} icu_data_init ICU::data GTest::gtest)
  clux_target_warnings(clux_test)
  # 少数端到端用例要断言程序**自己打印的输出**（tests/foreach_test.cpp）：clux 的
  # `libc::printf` 由 FFI 解析到系统 CRT，与测试进程自身 CRT 不是同一实例，进程内
  # fd 重定向抓不到 → 改为子进程跑 CLI（`<同目录>/clux.exe run …`）。因此 CLI 必须
  # 随测试一起构建，否则 `--target clux_test` 会留下一个不存在/过期的 clux.exe。
  add_dependencies(clux_test clux)

  # M6 FFI：extern var（动态库导出的**数据符号**）的测试素材。
  # 系统库里没有"各种宽度的可写全局数据符号"可用（msvcrt 只有 int/指针那几类），
  # 因此自建一个极小的测试动态库：i8/i16/i32/i64、u8/u16/u32/u64、f32/f64、
  # bool、指针各一个导出变量，读写都作用于库里那块内存（真 dlsym + 真内存）。
  # 产物落在与 clux_test 同一目录（Windows 的加载器会先搜可执行文件所在目录，
  # 因此测试里用裸文件名即可打开）。
  add_library(clux_ffi_testdata SHARED tests/ffi_testdata.c)
  clux_target_warnings(clux_ffi_testdata)
  add_dependencies(clux_test clux_ffi_testdata)

  include(GoogleTest)
  gtest_discover_tests(clux_test)
endif()

# M6：libffi 原生类型化调用层已可用（端口用 ml64 汇编 MASM/Intel 变体，见
# third_party/libffi-port/CMakeLists.txt），打开 gated 测试以验证 ffi_call 路径。
# TODO(M6)：ffi_call_typed 的 libffi 分支启用后，取消下面一行注释即可打开 gated 测试；
# 目前 LibffiCallsFloatFunctionSqrt/Pow 仍失败（该分支当时因汇编不可用而被禁用）。
if(TARGET FFI::ffi)
  add_compile_definitions(CLUX_FFI_TYPED_CALLS=1)
endif()
