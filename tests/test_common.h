#ifndef CLUX_TEST_COMMON_H
#define CLUX_TEST_COMMON_H

#include "core/allocator.h"
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

/**
 * 测试用临时文件的落点：**工作区内的 `build/tmp/test`**，不是系统 temp。
 *
 * 为什么不用 `std::filesystem::temp_directory_path()`：本仓库的测试进程在受限
 * 环境下无法写系统 temp（`fopen(<sys temp>)` → `errno=13 Permission denied`，
 * 实测）。而原先各测试文件里的 `write_temp_file` **不检查 fopen 返回值**，于是
 * `fwrite(NULL)` 直接撞 UCRT 断言（`stream != nullptr`）→ 进程 abort，表现为
 * 整批测试"崩溃"而非"失败"。改为工作区内目录即可写，且失败可诊断。
 */
inline std::string clux_test_temp_dir() { return "build/tmp/test"; }

/**
 * 同上，但返回 `std::filesystem::path`——供测试用 `dir / "name"` 形态拼路径
 * （`std::string` 没有 `operator/`）。两者指向同一目录。
 */
inline std::filesystem::path clux_test_path() {
  return std::filesystem::path("build/tmp/test");
}

/**
 * M6 FFI：`printf` **不再是内建**——它是 libc 的函数，由源码用 FFI 声明后调用
 * （`extern lib "*" as libc;` + `extern func libc::printf(format: str, ...): void;`；
 * 别名用 `libc` 而不是 `c`：**局部变量会遮蔽库别名**）。
 *
 * 测试里要**运行**的 clux 片段凡是用到 `libc::printf(`，统一在写文件前补上这两行
 * 声明，免得每个片段都手写样板。只关心格式化的测试（fmt_test）不需要它——
 * 格式化不解析名字。
 *
 * 本函数被 `write_temp_file` 统一调用（见下），单独调用它是可选的；幂等。
 */
inline std::string clux_with_printf_prelude(const std::string &body) {
  if (body.find("libc::printf(") == std::string::npos) return body;
  /* 幂等：已声明则原样返回（有些测试文件在调用处又显式加了一次）。 */
  if (body.find("extern func libc::printf(") != std::string::npos) return body;
  return std::string("extern lib \"*\" as libc;\n"
                     "extern func libc::printf(format: str, ...): void;\n") +
         body;
}

/**
 * 把 `content` 写成临时源码文件，返回其路径。
 *
 * `tag` 只用于让文件名可读（各测试文件原先把模块名编进文件名）。
 *
 * 失败时**必须**暴露，而不是静默返回一个不存在的路径（否则后续 driver 调用
 * 会给出误导性诊断）。用 `ADD_FAILURE()` 记录并返回空串。
 */
inline std::string write_temp_file(const std::string &content,
                                   const std::string &tag = "clux_test") {
  std::filesystem::create_directories(clux_test_temp_dir());
  /* 统一挂 `libc::printf` 声明（幂等）。此前这一步由各测试文件在调用处显式做，
     集中到本函数后不会再有个别文件漏挂 → 报 "unknown enum type in enum
     reference"。只关心格式化的测试不走本函数，不受影响。 */
  const std::string src = clux_with_printf_prelude(content);
  static int counter = 0;
  std::string path = clux_test_temp_dir() + "/" + tag + "_" +
                     std::to_string(counter++) + ".cx";
  errno = 0;
  FILE *fp = fopen(path.c_str(), "wb");
  if (!fp) {
    ADD_FAILURE() << "write_temp_file: cannot open " << path << " ("
                  << std::strerror(errno) << ")";
    return std::string();
  }
  size_t wrote =
      src.empty() ? 0 : fwrite(src.data(), 1, src.size(), fp);
  int saved = errno;
  if (fclose(fp) != 0 || wrote != src.size()) {
    ADD_FAILURE() << "write_temp_file: write failed for " << path << " ("
                  << std::strerror(saved) << ")";
    return std::string();
  }
  return path;
}

/**
 * Destroy an allocator while asserting that it has no live allocations.
 *
 * Replaces a bare `delete_allocator(pa)` so that any allocation a test
 * forgot to free is surfaced as a test failure (instead of a silent leak
 * that only shows up as a stderr report at best). Use this everywhere a
 * test tears down its allocator.
 *
 * `pa` must be a pointer to the caller's `allocator_t *` (i.e. `&a`).
 */
#define EXPECT_ALLOCATOR_EMPTY_DELETE(pa)                              \
  do {                                                                 \
    if ((pa) != nullptr && *(pa) != NULL) {                            \
      EXPECT_EQ(allocator_live_count(*(pa)), 0u)                        \
          << "allocator still has live allocations before delete";     \
    }                                                                  \
    delete_allocator((pa));                                            \
  } while (0)

/**
 * Read an entire file into a std::string. Returns an empty string if the
 * file cannot be opened. Shared by the driver test files so the helper does
 * not have to be duplicated across translation units.
 */
inline std::string slurp(const std::string &path) {
  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp) return std::string();
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, fp)) > 0) out.append(buf, n);
  fclose(fp);
  return out;
}

#endif /* CLUX_TEST_COMMON_H */
