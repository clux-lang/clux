# libffi 端口（Windows x64 + clang；见同目录 fficonfig.h 顶部的"为什么"）
#
# 上游 libffi 只提供 autotools 构建（configure/make），且 msvc_build/ 仅有 aarch64
# 的 VS 工程。本环境无 autoconf/automake/libtool/make/sh；但有 VS Build Tools
# （cl.exe/ml64.exe/link.exe/MSBuild）与 CMake + Ninja + clang。汇编优先用 ml64
# 汇编 win64_intel.S（MASM 语法，上游正统路径）；缺 ml64 时回退 clang 汇编 win64.S。
#
# 放在 libffi 树**之外**：更新/切换 libffi 版本时不会与上游文件冲突。

cmake_minimum_required(VERSION 3.14)

# win64.S 必须作为汇编编译（根工程 project(clux C CXX) 未启用 ASM）
enable_language(ASM)

# win64.S 必须作为汇编编译（根工程未 enable ASM）；.S 走 C 预处理器 + clang 集成汇编器
enable_language(ASM)

set(LIBFFI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libffi")
if(NOT EXISTS "${LIBFFI_DIR}/src/prep_cif.c")
  message(FATAL_ERROR
    "third_party/libffi-port: 未找到 libffi 源码（${LIBFFI_DIR}）。"
    "请先 clone：git clone --depth 1 --branch v3.8.0 "
    "https://github.com/libffi/libffi third_party/libffi")
endif()

# 生成头文件目录：ffi.h（由 ffi.h.in 填充）与手写的 fficonfig.h / ffi_cfi.h
set(FFI_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffi_include")
file(MAKE_DIRECTORY "${FFI_GEN_DIR}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/fficonfig.h"
          "${CMAKE_CURRENT_SOURCE_DIR}/ffi_cfi.h"
     DESTINATION "${FFI_GEN_DIR}")

# ffi.h.in 的占位符（对照上游 configure.ac 的替换集）
set(VERSION "3.8.0")
set(TARGET "X86_WIN64")            # 见 fficonfig.h 顶部说明
set(HAVE_LONG_DOUBLE 0)
set(FFI_VERSION_STRING "3.8.0")
set(FFI_VERSION_NUMBER "30800")
set(FFI_EXEC_TRAMPOLINE_TABLE 0)
configure_file("${LIBFFI_DIR}/include/ffi.h.in" "${FFI_GEN_DIR}/ffi.h" @ONLY)

# ---- 汇编：优先 ml64（MASM/Intel 变体 = 上游正统 Windows 路径）----
#
# 背景（2026-10 实测）：clang 的集成汇编器把 GAS 语法的 win64.S 汇编成 COFF 时，
# 产出的 ffi_call_win64 在运行期会写坏 rsp —— 0 参调用即崩、__try/__except 完全
# 无法触发；反汇编与上游源码逐条一致，11 项假设逐一排除（见 docs/m6-design.md §14.4）。
# 而 libffi 为 MSVC 准备的 **Intel 语法变体 win64_intel.S** 用 MASM（ml64）汇编后
# 一切正常：0 参本地函数 = 42、strlen("hello") = 5、sqrt(9.0) = 3.0 ✓。
# 故这里优先走 ml64；找不到 VS Build Tools 时回退旧的 clang 汇编路径
# （仍可构建，但 ffi_call/ffi_closure 在运行期不可用）。
set(CLUX_ML64 "" CACHE FILEPATH "ml64.exe（MASM x64 汇编器）；留空则自动查找")

if(NOT CLUX_ML64)
  # 注意：CMake 的 $ENV{...} 不能含括号（ProgramFiles(x86) 会破坏解析），故用两条字面/无括号路径
  set(_vswhere "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
  if(NOT EXISTS "${_vswhere}")
    set(_vswhere "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer/vswhere.exe")
  endif()
  if(EXISTS "${_vswhere}")
    execute_process(
      COMMAND "${_vswhere}" -latest -products *
              -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
              -property installationPath
      OUTPUT_VARIABLE _vsdir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_vsdir)
      file(GLOB _ml64_candidates "${_vsdir}/VC/Tools/MSVC/*/bin/Hostx64/x64/ml64.exe")
      if(_ml64_candidates)
        list(SORT _ml64_candidates)
        list(GET _ml64_candidates -1 CLUX_ML64)   # 取版本最高的工具集
      endif()
    endif()
  endif()
endif()

set(FFI_SOURCES
  "${LIBFFI_DIR}/src/prep_cif.c"
  "${LIBFFI_DIR}/src/types.c"
  "${LIBFFI_DIR}/src/raw_api.c"
  "${LIBFFI_DIR}/src/java_raw_api.c"
  "${LIBFFI_DIR}/src/closures.c"
  "${LIBFFI_DIR}/src/tramp.c"      # ffi_tramp_*：闭包/回调所需（Phase 5）
  "${LIBFFI_DIR}/src/dlmalloc.c"
  "${LIBFFI_DIR}/src/x86/ffi.c"
  "${LIBFFI_DIR}/src/x86/ffiw64.c"
)

if(CLUX_ML64)
  message(STATUS "libffi 端口：使用 MASM/Intel 路径（ml64 = ${CLUX_ML64}）")
  set(FFI_ASM_INTEL "${CMAKE_CURRENT_BINARY_DIR}/win64_intel.i")
  add_custom_command(
    OUTPUT "${FFI_ASM_INTEL}"
    COMMAND ${CMAKE_COMMAND}
            -D "CC=${CMAKE_C_COMPILER}"
            -D "SRC=${LIBFFI_DIR}/src/x86/win64_intel.S"
            -D "OUT=${FFI_ASM_INTEL}"
            -D "INC=${FFI_GEN_DIR};${LIBFFI_DIR}/include;${LIBFFI_DIR}/src;${LIBFFI_DIR}/src/x86"
            -D "CET=endbr64"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/preprocess_asm.cmake"
    DEPENDS "${LIBFFI_DIR}/src/x86/win64_intel.S"
            "${FFI_GEN_DIR}/fficonfig.h" "${FFI_GEN_DIR}/ffi.h"
            "${FFI_GEN_DIR}/ffi_cfi.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/preprocess_asm.cmake"
    COMMENT "libffi: 预处理 win64_intel.S（MASM 路径；-P 去行标记，ml64 不认 '#'）"
    VERBATIM)
  set(FFI_ASM_OBJ "${CMAKE_CURRENT_BINARY_DIR}/win64_masm.obj")
  add_custom_command(
    OUTPUT "${FFI_ASM_OBJ}"
    # 注意：/Fo 与路径必须作为**一个**参数（写成 /Fo"path" 会被 VERBATIM 转义成 \"）
    COMMAND "${CLUX_ML64}" /c /nologo "/Fo${FFI_ASM_OBJ}" "${FFI_ASM_INTEL}"
    DEPENDS "${FFI_ASM_INTEL}"
    COMMENT "libffi: ml64 汇编 win64_intel.i → win64_masm.obj"
    VERBATIM)
  set_source_files_properties("${FFI_ASM_OBJ}" PROPERTIES
    GENERATED TRUE EXTERNAL_OBJECT TRUE)
  list(APPEND FFI_SOURCES "${FFI_ASM_OBJ}")
else()
  message(STATUS "libffi 端口：未找到 ml64 → 回退 clang 汇编路径（ffi_call 运行期不可用）")
  # win64.S 是 GAS 语法且含 cfi_* 伪指令，clang 的 COFF 汇编器不认。旧路径：
  # "C 预处理器展开 → 剥 cfi_* 行 → clang 汇编"（.seh_* 保留，clang 支持）。
  set(FFI_ASM_SRC "${LIBFFI_DIR}/src/x86/win64.S")
  set(FFI_ASM_GEN "${CMAKE_CURRENT_BINARY_DIR}/win64_nocfi.s")
  add_custom_command(
    OUTPUT "${FFI_ASM_GEN}"
    COMMAND ${CMAKE_COMMAND}
            -D "CC=${CMAKE_C_COMPILER}"
            -D "SRC=${FFI_ASM_SRC}"
            -D "OUT=${FFI_ASM_GEN}"
            -D "INC=${FFI_GEN_DIR};${LIBFFI_DIR}/include;${LIBFFI_DIR}/src;${LIBFFI_DIR}/src/x86"
            -D "CET=endbr64"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/preprocess_asm.cmake"
    DEPENDS "${FFI_ASM_SRC}"
            "${FFI_GEN_DIR}/fficonfig.h" "${FFI_GEN_DIR}/ffi.h"
            "${FFI_GEN_DIR}/ffi_cfi.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/preprocess_asm.cmake"
    COMMENT "libffi: 预处理 win64.S 并剥掉 cfi_*（COFF 汇编器不接受）"
    VERBATIM)
  # .s 不做 C 预处理，故目标级 -D 在汇编命令上"未被使用"：仅对该源关闭该告警
  set_source_files_properties("${FFI_ASM_GEN}" PROPERTIES GENERATED TRUE LANGUAGE ASM
    COMPILE_OPTIONS "-Wno-unused-command-line-argument")
  list(APPEND FFI_SOURCES "${FFI_ASM_GEN}")
endif()

add_library(ffi STATIC ${FFI_SOURCES})
add_library(FFI::ffi ALIAS ffi)

# 链上 ffi 的目标自动获得 ffi.h 与 CLUX_HAVE_LIBFFI（表示本项目已接 libffi）
target_compile_definitions(ffi PUBLIC CLUX_HAVE_LIBFFI=1)

target_include_directories(ffi
  PUBLIC  "${FFI_GEN_DIR}"
          "${LIBFFI_DIR}/src/x86"   # ffitarget.h：ffi.h 会 include 它（上游与之同级安装）
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"   # fficonfig.h / ffi_cfi.h（汇编也用它）
          "${LIBFFI_DIR}/include"
          "${LIBFFI_DIR}/src"
          "${LIBFFI_DIR}/src/x86"
)

target_compile_definitions(ffi PUBLIC
  X86_WIN64               # 目标架构（等价于 msvc_build 的 ARM64）
  X86_64                  # autotools 为 x86_64 主机同时定义它；ffi.c/ffiw64.c 的部分门控依赖
  USE_DL_PREFIX           # dlmalloc 加 dl 前缀（msvc_build 同款）
  FFI_STATIC_BUILD        # 静态构建：FFI_API 为空（libffi 3.8 的宏；旧版用 FFI_BUILDING 无效）
  FFI_BUILDING            # 同上（旧版 libffi 的写法，保留以兼容版本差异）
  HAVE_ALLOCA=1
)

# 汇编需要 LIBFFI_ASM 才能走 asmnames.h 的符号命名规则
set_source_files_properties("${LIBFFI_DIR}/src/x86/win64.S"
  PROPERTIES COMPILE_DEFINITIONS "X86_WIN64;LIBFFI_ASM")

# libffi 的 ffitarget.h 用 __int64（MSVC 扩展），clang 会报 -Wlanguage-extension-token；
# 那是上游代码风格，不是本仓库的问题 → 仅对本目标关闭该诊断。
if(MSVC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
  # 注意：必须对**消费者**也生效——clux_vm 的 ffi_lib.c 同样 include <ffi.h> →
  # ffitarget.h 的 __int64 会在那里报警（PRIVATE 只覆盖 ffi 自身，不够）。
  target_compile_options(ffi PUBLIC -Wno-language-extension-token)
endif()