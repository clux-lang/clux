/*
 * `clux install` + `clux.mod` postinstall 端到端测试：
 *   - 默认拒绝执行脚本（需要 --allow-scripts）
 *   - 成功后在包根生成产出的 marker
 *   - 已成功的 hook 不重复执行（除非 --force）
 *   - cwd / CLUX_PKG_ROOT / CLUX_OS / CLUX_ARCH 正确
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_common.h"

namespace {

std::string q(const std::string &s) { return "\"" + s + "\""; }

std::string cli_abs() {
  namespace fs = std::filesystem;
#ifdef _WIN32
  return fs::absolute("build/clux.exe").string();
#else
  return fs::absolute("build/clux").string();
#endif
}

int run_cli(const std::string &cli, const std::vector<std::string> &args) {
  std::string inner = q(cli);
  for (const std::string &a : args) inner += " " + q(a);
#ifdef _WIN32
  const std::string cmd = "(" + inner + ")";
#else
  const std::string cmd = inner;
#endif
  return std::system(cmd.c_str());
}

void write_file(const std::filesystem::path &path, const std::string &text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream ofs(path, std::ios::binary);
  ofs << text;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

class ScopedCwd {
public:
  explicit ScopedCwd(const std::filesystem::path &p) {
    std::error_code ec;
    old_ = std::filesystem::current_path(ec);
    std::filesystem::current_path(p, ec);
  }
  ~ScopedCwd() {
    std::error_code ec;
    std::filesystem::current_path(old_, ec);
  }
  ScopedCwd(const ScopedCwd &) = delete;
  ScopedCwd &operator=(const ScopedCwd &) = delete;

private:
  std::filesystem::path old_;
};

} // namespace

TEST(Install, PostinstallRequiresAllowScriptsAndCaches) {
  namespace fs = std::filesystem;
  static int seq = 0;
  fs::path root = fs::absolute(fs::path(clux_test_temp_dir()) /
                               ("install_pkg_" + std::to_string(seq++)));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "scripts", ec);

  write_file(root / "clux.mod",
             "{\n"
             "  \"name\": \"installpkg\",\n"
             "  \"version\": \"0.1.0\",\n"
             "  \"entry\": \"main.cx\",\n"
             "  \"postinstall\": {\n"
#ifdef _WIN32
             "    \"WIN64\": \"scripts\\\\post.bat\"\n"
#else
             "    \"LINUX\": \"sh scripts/post.sh\",\n"
             "    \"MACOS\": \"sh scripts/post.sh\"\n"
#endif
             "  }\n"
             "}\n");
  write_file(root / "main.cx", "func main(): i32 { return 0; }\n");

  /* 依赖包先安装；root 的 hook 依赖它产出的 dep.txt，验证 deps-first。 */
  fs::create_directories(root / "deps" / "dep" / "scripts", ec);
  write_file(root / "deps" / "dep" / "clux.mod",
             "{\n"
             "  \"name\": \"dep\",\n"
             "  \"version\": \"0.1.0\",\n"
             "  \"entry\": \"dep.cx\",\n"
             "  \"postinstall\": {\n"
#ifdef _WIN32
             "    \"WIN64\": \"scripts\\\\dep.bat\"\n"
#else
             "    \"LINUX\": \"sh scripts/dep.sh\",\n"
             "    \"MACOS\": \"sh scripts/dep.sh\"\n"
#endif
             "  },\n"
             "  \"native\": { \"link\": [\"vendor/lib/libdep.a\"] }\n"
             "}\n");
  write_file(root / "deps" / "dep" / "dep.cx", "func dep(): i32 { return 1; }\n");
  write_file(root / "deps" / "dep" / "vendor" / "lib" / "libdep.a", "dep-archive\n");
#ifdef _WIN32
  write_file(root / "deps" / "dep" / "scripts" / "dep.bat",
             "@echo off\n"
             "echo ok>dep.txt\n");
#else
  write_file(root / "deps" / "dep" / "scripts" / "dep.sh",
             "echo ok > dep.txt\n");
#endif

#ifdef _WIN32
  write_file(root / "scripts" / "post.bat",
             "@echo off\n"
             "if not exist deps\\dep\\dep.txt exit /b 1\n"
             "echo %CLUX_PKG_ROOT%>marker.txt\n"
             "echo %CLUX_OS%/%CLUX_ARCH%>>marker.txt\n");
#else
  write_file(root / "scripts" / "post.sh",
             "test -f deps/dep/dep.txt || exit 1\n"
             "echo \"$CLUX_PKG_ROOT\" > marker.txt\n"
             "echo \"$CLUX_OS/$CLUX_ARCH\" >> marker.txt\n");
#endif

  const std::string cli = cli_abs();
  {
    ScopedCwd cwd(root);
    EXPECT_NE(run_cli(cli, {"install"}), 0);
    EXPECT_FALSE(fs::exists(root / "marker.txt"));

    ASSERT_EQ(run_cli(cli, {"install", "--allow-scripts"}), 0);
    ASSERT_TRUE(fs::exists(root / "marker.txt"));
    {
      std::ifstream in(root / "marker.txt");
      std::string text((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
      EXPECT_NE(text.find(root.string()), std::string::npos) << text;
      EXPECT_NE(text.find("/"), std::string::npos) << text;
    }

    ASSERT_TRUE(fs::exists(root / "clux.lock"));
    std::string lock = read_file(root / "clux.lock");
    EXPECT_NE(lock.find("libdep.a"), std::string::npos) << lock;
    EXPECT_NE(lock.find("script_sha256"), std::string::npos) << lock;
    ASSERT_EQ(run_cli(cli, {"install", "--locked"}), 0);

    /* native 产物 hash 变化必须被 --locked 发现。 */
    write_file(root / "deps" / "dep" / "vendor" / "lib" / "libdep.a",
               "tampered\n");
    EXPECT_NE(run_cli(cli, {"install", "--locked"}), 0);
    write_file(root / "deps" / "dep" / "vendor" / "lib" / "libdep.a",
               "dep-archive\n");
    ASSERT_EQ(run_cli(cli, {"install", "--locked"}), 0);

    std::string mod = read_file(root / "clux.mod");
    std::string changed = mod;
    size_t vpos = changed.find("\"0.1.0\"");
    ASSERT_NE(vpos, std::string::npos);
    changed.replace(vpos, 7, "\"0.1.1\"");
    write_file(root / "clux.mod", changed);
    EXPECT_NE(run_cli(cli, {"install", "--locked"}), 0);
    write_file(root / "clux.mod", mod);
    ASSERT_EQ(run_cli(cli, {"install", "--locked"}), 0);

    /* 已成功的 hook 不应重复执行：删掉产物后再次 install，产物不会被重建。 */
    fs::remove(root / "marker.txt", ec);
    ASSERT_EQ(run_cli(cli, {"install", "--allow-scripts"}), 0);
    EXPECT_FALSE(fs::exists(root / "marker.txt"));

    /* --force 强制重跑。 */
    ASSERT_EQ(run_cli(cli, {"install", "--allow-scripts", "--force"}), 0);
    EXPECT_TRUE(fs::exists(root / "marker.txt"));

    /* 脚本内容变化必须使 marker 失效并重跑。 */
#ifdef _WIN32
    fs::path post = root / "scripts" / "post.bat";
#else
    fs::path post = root / "scripts" / "post.sh";
#endif
    write_file(post, read_file(post) + "\n");
    fs::remove(root / "marker.txt", ec);
    ASSERT_EQ(run_cli(cli, {"install", "--allow-scripts"}), 0);
    EXPECT_TRUE(fs::exists(root / "marker.txt"));
  }

  std::filesystem::remove_all(root, ec);
}

TEST(Install, BuildLockedVerifiesManifest) {
  namespace fs = std::filesystem;
  static int seq = 100;
  fs::path root = fs::absolute(fs::path(clux_test_temp_dir()) /
                               ("lock_build_" + std::to_string(seq++)));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  write_file(root / "clux.mod",
             "{\n"
             "  \"name\": \"lockbuild\",\n"
             "  \"version\": \"0.1.0\",\n"
             "  \"entry\": \"main.cx\"\n"
             "}\n");
  write_file(root / "main.cx", "func main(): i32 { return 0; }\n");

  const std::string cli = cli_abs();
  {
    ScopedCwd cwd(root);
    ASSERT_EQ(run_cli(cli, {"install", "--allow-scripts"}), 0);
    ASSERT_TRUE(fs::exists(root / "clux.lock"));
    ASSERT_EQ(run_cli(cli, {"build", "main.cx", "--locked"}), 0);

    std::string mod = read_file(root / "clux.mod");
    write_file(root / "clux.mod", mod + "\n");
    EXPECT_NE(run_cli(cli, {"build", "main.cx", "--locked"}), 0);
    write_file(root / "clux.mod", mod);
    EXPECT_EQ(run_cli(cli, {"build", "main.cx", "--locked"}), 0);
  }
  std::filesystem::remove_all(root, ec);
}
