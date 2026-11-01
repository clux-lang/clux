// conformance/ 目录的一致性用例：用语言自身的 test/assert 自声明语言特性。
//
// 用例是**实现无关**的（只用 `test` / `assert` / `mock`），未来自举的实现也能直接跑。
// 本测试只负责在 C 实现上批量验证：遍历入口文件，逐个 `clux test` 期望退出码 0。
// 依赖模块放在 `*/lib/` 下，不作为入口。

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "test_common.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::string cli_path() {
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof buf);
  std::filesystem::path self(std::string(buf, n));
  return (self.parent_path() / "clux.exe").string();
#else
  std::error_code ec;
  std::filesystem::path self =
      std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec || self.empty()) return "clux";
  return (self.parent_path() / "clux").string();
#endif
}

std::string q(const std::string &s) { return "\"" + s + "\""; }

int run_cli(const std::vector<std::string> &args) {
  std::string inner = q(cli_path());
  for (const std::string &a : args) inner += " " + q(a);
#ifdef _WIN32
  const std::string cmd = "(" + inner + ")";
#else
  const std::string cmd = inner;
#endif
  const int rc = std::system(cmd.c_str());
#ifdef _WIN32
  return rc;
#else
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
#endif
}

std::vector<std::string> entry_files() {
  std::vector<std::string> out;
  const std::filesystem::path root("conformance");
  if (!std::filesystem::exists(root)) return out;
  for (const auto &e : std::filesystem::directory_iterator(root)) {
    if (e.is_regular_file() && e.path().extension() == ".cx")
      out.push_back(e.path().string());
    else if (e.is_directory()) {
      const std::filesystem::path main = e.path() / "main.cx";
      if (std::filesystem::exists(main)) out.push_back(main.string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace

TEST(Conformance, SuiteRuns) {
  const std::vector<std::string> files = entry_files();
  ASSERT_FALSE(files.empty()) << "conformance/ has no entry files";
  for (const std::string &f : files) {
    SCOPED_TRACE(f);
    EXPECT_EQ(run_cli({"test", std::filesystem::absolute(f).string()}), 0)
        << "conformance case failed: " << f;
  }
}
