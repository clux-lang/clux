/* fficonfig.h —— libffi 在 **Windows x64 + clang** 下的静态构建配置
 *
 * 为什么需要这份文件：libffi 只有 autotools 构建（README "Installing libffi"
 * 一节的 configure/make 流程），而 `msvc_build/` 里只随附 **aarch64** 的
 * Visual Studio 工程与预生成配置头（README 末段），没有 x64 版本。本环境没有
 * autoconf/automake/libtool/make/sh，也没有 MSBuild/cl/ml64；但 clang 能直接汇编
 * libffi 的 `src/x86/win64.S`（实测：唯一报错是缺本文件），因此照 `msvc_build`
 * 的模式手写一份 x64 配置，用 CMake + Ninja + clang 构建。
 *
 * 依据：msvc_build/aarch64/Ffi_staticLib.vcxproj 的
 *   PreprocessorDefinitions = FFI_BUILDING_DLL; USE_DL_PREFIX; <ARCH>; NDEBUG
 *   AdditionalIncludeDirectories = ../../include; .\<arch>_include; ../../src/<arch>
 * 以及 msvc_build/aarch64/aarch64_include/fficonfig.h（同款配置头模板）。
 *
 * 与 aarch64 模板的差异（Windows x64 修正）：
 *   - 不定义 HAVE_MMAP / HAVE_MMAP_* / HAVE_SYS_MMAN_H / HAVE_UNISTD_H /
 *     HAVE_DLFCN_H —— Windows 没有这些；closures.c 走 _WIN32 分支。
 *   - 不定义 HAVE_AS_CFI_PSEUDO_OP —— COFF 目标下 clang 集成汇编器不接受
 *     .cfi_* 指令，故 ffi_cfi.h 取"空宏"变体（另一份文件）。
 *   - 不定义 LIBFFI_GNU_SYMBOL_VERSIONING。
 *   - 目标架构宏 X86_WIN64 由 CMakeLists 以编译定义传入（等价于模板的 ARM64）。
 */

/* Define this if you want extra debugging. */
/* #undef FFI_DEBUG */

/* Cannot use PROT_EXEC on this target, so, we revert to alternative means */
/* #undef FFI_EXEC_TRAMPOLINE_TABLE */

/* Define this if you want to enable pax emulated trampolines */
/* #undef FFI_MMAP_EXEC_EMUTRAMP_PAX */

/* Cannot use malloc on this target, so, we revert to alternative means */
/* #undef FFI_MMAP_EXEC_WRIT */

/* Define this if you do not want support for the raw API. */
/* #undef FFI_NO_RAW_API */

/* Define this if you do not want support for aggregate types. */
/* #undef FFI_NO_STRUCTS */

/* Define to 1 if you have `alloca', as a function or macro. */
#define HAVE_ALLOCA 1

/* Define if __attribute__((visibility("hidden"))) is supported. */
/* Windows/COFF 无 visibility 属性：留空（ffi.h 会用 FFI_HIDDEN 宏） */
/* #undef HAVE_HIDDEN_VISIBILITY_ATTRIBUTE */

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define if you have the long double type and it is bigger than a double */
/* #undef HAVE_LONG_DOUBLE */

/* Define if you support more than one size of the long double type */
/* #undef HAVE_LONG_DOUBLE_VARIANT */

/* Define to 1 if you have the `memcpy' function. */
#define HAVE_MEMCPY 1

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Name of package */
#define PACKAGE "libffi"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "http://github.com/libffi/libffi/issues"

/* Define to the full name of this package. */
#define PACKAGE_NAME "libffi"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "libffi 3.8.0"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "libffi"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "3.8.0"

/* The size of `double', as computed by sizeof. */
#define SIZEOF_DOUBLE 8

/* The size of `long double', as computed by sizeof. */
#define SIZEOF_LONG_DOUBLE 8

/* The size of `size_t', as computed by sizeof. */
#define SIZEOF_SIZE_T 8

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Define if symbols are underscored. */
/* #undef SYMBOL_UNDERSCORE */

/* Version number of package */
#define VERSION "3.8.0"

/* Define to `unsigned int' if <sys/types.h> does not define. */
/* #undef size_t */

/* FFI_HIDDEN：非 ELF 目标（Windows）无 visibility，统一给空 */
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name)
#else
#define FFI_HIDDEN
#endif

/* CET（控制流强制）的 endbr64 前缀：上游由 configure 写入 fficonfig.h。
   非 CET 构建定义为空。**必须定义**：win64.S 里直接使用 _CET_ENDBR，
   未定义时会作为裸 token 汇编进函数序言（症状：ffi_call 首次调用即崩）。 */
#ifndef _CET_ENDBR
#define _CET_ENDBR endbr64
#endif
