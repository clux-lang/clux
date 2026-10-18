/**
 * File: asm_examples_test.cpp
 * Description: `examples/asm/` 下的 .cxs 手写字节码汇编样本的端到端回归：
 *              汇编（`driver_asm_to_bin`）→ 运行（`driver_run_bin`）必须成功。
 *
 * 背景：printf 自 M6 起不再是内建（改由 FFI 声明），5 个样本却仍写
 * `push "printf"; call N` —— 结果是"能汇编、运行报 `exec: undefined variable`"，
 * 而它们恰恰是"手写汇编怎么调用外部函数"的唯一示例。现在样本改用
 * `externlib "*" as libc` + `extern_call "libc::printf" <argc> 65792`。
 *
 * 这里用进程内 driver API（不 spawn CLI）：样本自己会向 stdout 打印，
 * 进程内 printf 走本进程 CRT，输出不必断言——**退出码 0 + 无泄漏**已足以
 * 区分修好前后（修好前 rc=1 且 stderr 有 `exec: undefined variable`）。
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "driver/driver.h"
}

#include "test_common.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

/** 仓库根：从 CWD 向上找含 examples/asm 的目录（ctest 可能从 build/ 跑）。 */
std::string find_repo_root() {
  std::filesystem::path p = std::filesystem::current_path();
  for (int i = 0; i < 4; i++) {
    if (std::filesystem::exists(p / "examples" / "asm" / "hello.cxs"))
      return p.string();
    if (!p.has_parent_path()) break;
    p = p.parent_path();
  }
  return std::string();
}

/** 运行 driver_run_bin，捕获 stderr（泄漏报告与运行期错误都在那里）。 */
int run_bin_capture(const std::string &bin, std::string *err) {
  fflush(stderr);
  const std::string cap = clux_test_temp_dir() + "/asm_example_stderr.txt";
  std::filesystem::create_directories(clux_test_temp_dir());
#ifdef _WIN32
  FILE *tmp = nullptr;
  if (fopen_s(&tmp, cap.c_str(), "wb") != 0 || !tmp) {
    ADD_FAILURE() << "run_bin_capture: cannot open " << cap;
    return 1;
  }
  const int saved = _dup(2);
  _dup2(_fileno(tmp), 2);
  const int rc = driver_run_bin(bin.c_str());
  fflush(stderr);
  _dup2(saved, 2);
  _close(saved);
  fclose(tmp);
#else
  FILE *tmp = fopen(cap.c_str(), "wb");
  if (!tmp) {
    ADD_FAILURE() << "run_bin_capture: cannot open " << cap;
    return 1;
  }
  const int saved = dup(2);
  dup2(fileno(tmp), 2);
  const int rc = driver_run_bin(bin.c_str());
  fflush(stderr);
  dup2(saved, 2);
  close(saved);
  fclose(tmp);
#endif
  if (err) *err = slurp(cap);
  return rc;
}

} // namespace

TEST(AsmExamples, AllSamplesAssembleAndRun) {
  const std::string root = find_repo_root();
  ASSERT_FALSE(root.empty())
      << "cannot locate examples/asm (run from the repo root or a subdir of it)";

  const char *samples[] = {"hello.cxs", "arithmetic.cxs", "factorial.cxs",
                           "fib.cxs", "loop.cxs"};
  for (const char *name : samples) {
    const std::string cxs =
        (std::filesystem::path(root) / "examples" / "asm" / name).string();
    ASSERT_TRUE(std::filesystem::exists(cxs)) << cxs;

    const std::string bin =
        (clux_test_path() / (std::string("asm_example_") + name + ".cxb")).string();
    ASSERT_EQ(driver_asm_to_bin(cxs.c_str(), bin.c_str()), 0)
        << "assemble failed: " << cxs;

    std::string err;
    const int rc = run_bin_capture(bin, &err);
    EXPECT_EQ(rc, 0) << name << "\n--- stderr ---\n" << err;
    EXPECT_EQ(err.find("memory leak"), std::string::npos)
        << name << " leaked:\n" << err;
    EXPECT_EQ(err.find("undefined variable"), std::string::npos)
        << name << " still calls printf as a builtin:\n" << err;

    std::remove(bin.c_str());
  }
}

TEST(AsmExamples, SamplesDeclareExternLib) {
  /* 结构性前置：凡调用 printf 的样本必须自带 externlib 行与 extern_call
     （M6 起的唯一写法）。避免以后有人把样本改回 `push "printf"; call N`。
     只看**非注释行**（样本头部注释会在说明里引用老写法）。 */
  const std::string root = find_repo_root();
  if (root.empty()) GTEST_SKIP() << "examples/asm not found";

  const char *samples[] = {"hello.cxs", "arithmetic.cxs", "factorial.cxs",
                           "fib.cxs", "loop.cxs"};
  for (const char *name : samples) {
    const std::string path =
        (std::filesystem::path(root) / "examples" / "asm" / name).string();
    const std::string text = slurp(path);
    ASSERT_FALSE(text.empty()) << path;

    bool has_externlib = false, has_extern_call = false, has_old_push = false;
    size_t pos = 0;
    while (pos <= text.size()) {
      size_t nl = text.find('\n', pos);
      std::string line = text.substr(pos, nl == std::string::npos
                                              ? std::string::npos
                                              : nl - pos);
      pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
      size_t first = line.find_first_not_of(" \t\r");
      if (first == std::string::npos) continue;
      if (line[first] == ';') continue; /* 注释行 */
      if (line.find("externlib \"*\" as libc") != std::string::npos)
        has_externlib = true;
      if (line.find("extern_call \"libc::printf\"") != std::string::npos)
        has_extern_call = true;
      if (line.find("push \"printf\"") != std::string::npos) has_old_push = true;
    }
    EXPECT_TRUE(has_externlib) << name << " 缺少 externlib 声明";
    EXPECT_TRUE(has_extern_call) << name << " 缺少 extern_call \"libc::printf\"";
    EXPECT_FALSE(has_old_push) << name << " 仍在用已删除的内建 printf 写法";
  }
}
