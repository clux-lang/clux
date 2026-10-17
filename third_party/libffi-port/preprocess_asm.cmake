# 预处理 libffi 的 win64.S：用 C 预处理器展开宏，然后剥掉 cfi_* 行。
#
# 为什么需要：win64.S 是 GAS 语法，内含 `cfi_startproc` / `cfi_def_cfa(%rbp, 16)`
# 这类 CFI 伪指令；clang 在 **COFF**（Windows）目标下不认它们（invalid instruction
# mnemonic）。上游在 Windows 上走 msvcc.sh 把 .S 预处理成 MASM（.asm）交给 ml64，
# CFI 在那条路上被丢弃；本环境没有 ml64/MSBuild，因此改为"预处理后剥行再汇编"。
# 注意 win64.S 自带的 SEH(...) 宏会展开成 .seh_* 指令，而 clang **支持** .seh_*
# （已实测），所以只剥 cfi_*，保留 SEH。
#
# 入参（由 CMake 以 -D 传入）：CC / SRC / OUT / INC（分号分隔的 include 目录）

if(NOT DEFINED CC OR NOT DEFINED SRC OR NOT DEFINED OUT)
  message(FATAL_ERROR "preprocess_asm.cmake: CC/SRC/OUT are required")
endif()

set(inc_flags "")
foreach(d ${INC})
  list(APPEND inc_flags "-I${d}")
endforeach()

set(stage "${OUT}.pre")
execute_process(
  COMMAND "${CC}" -E -P -x assembler-with-cpp -DLIBFFI_ASM -DX86_WIN64 -D_CET_ENDBR=endbr64
          ${inc_flags} "${SRC}" -o "${stage}"
  RESULT_VARIABLE rc
  ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "preprocess_asm: 预处理失败：${err}")
endif()

file(READ "${stage}" content)
file(REMOVE "${stage}")

# 剥掉 CFI 伪指令行（含缩进与可选点号）：
#   cfi_startproc / .cfi_startproc / cfi_def_cfa(%rbp, 16) / .cfi_sections ...
string(REGEX REPLACE "[ \t]*\\.?cfi_[A-Za-z0-9_]*[^\n]*\n" "" content "${content}")
string(REGEX REPLACE "[ \t]*\\.seh_[A-Za-z0-9_]*[^\n]*\n" "" content "${content}")

file(WRITE "${OUT}" "${content}")
