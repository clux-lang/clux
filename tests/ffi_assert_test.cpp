/*
 * File: ffi_assert_test.cpp
 * Description: `extern lib <别名> from "<库名>" [assert(<条件>)];` —— FFI 导入的
 *              **条件后缀**（按场景选库）。
 *
 * 语义（设计决定）：
 *   - 条件在**前脱糖**阶段求值（parser → sema 之间，纯 AST 变形）；
 *   - 条件为假 → **整条 extern lib 声明被裁剪**：不加载、不登记别名、不校验符号；
 *   - 因此"各平台各写一条同别名声明"天然不冲突（只有一条存活）；
 *   - 键集合是**进程级全局静态表**：内建 OS_TYPE / OS_ARCH + 命令行 `-D KEY=VALUE`；
 *   - 键不是变量，程序别处看不见它；未定义的键在求值时报错（拼错不静默为 false）。
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "core/ffi_cond.h"
#include "driver/driver.h"
#include "parser/ast_extern_lib.h"
#include "parser/ast_node.h"
#include "parser/ast_program.h"
#include "cmd/path.h"
#include "parser/fmt.h"
#include "parser/lexer.h"
#include "parser/parser.h"
}

#include "test_common.h"

namespace {

/* 每个用例结束后把全局键表恢复成"只有内建键"。 */
class FfiAssertTest : public ::testing::Test {
 protected:
  void SetUp() override { ffi_defines_reset(); }
  void TearDown() override { ffi_defines_reset(); }
};

/* 读取内建 OS_TYPE / OS_ARCH 的值 */
std::string builtin(const char *key) {
  const char *v = ffi_defines_get(key);
  return v ? std::string(v) : std::string();
}

} // namespace

/* ================================================================ */
/* 一、条件语言（core/ffi_cond）：校验与求值                          */
/* ================================================================ */

TEST_F(FfiAssertTest, BuiltinsAreInjected) {
  const std::string os = builtin("OS_TYPE");
  const std::string arch = builtin("OS_ARCH");
  EXPECT_FALSE(os.empty());
  EXPECT_FALSE(arch.empty());
#if defined(_WIN32)
  EXPECT_EQ(os, sizeof(void *) == 8 ? "WIN64" : "WIN32");
#elif defined(__APPLE__)
  EXPECT_EQ(os, "MACOS");
#elif defined(__linux__)
  EXPECT_EQ(os, "LINUX");
#endif
  /* 注入的键只存在于键表：不是语言变量（下面 Driver 用例覆盖可见性） */
  EXPECT_EQ(ffi_defines_get("NO_SUCH_KEY"), nullptr);
}

TEST_F(FfiAssertTest, AddAndOverrideDefine) {
  ASSERT_EQ(ffi_defines_add("BUILD_FLAVOR", "dev"), 0);
  EXPECT_STREQ(ffi_defines_get("BUILD_FLAVOR"), "dev");
  ASSERT_EQ(ffi_defines_add_kv("BUILD_FLAVOR=prod"), 0); /* 同名覆盖 */
  EXPECT_STREQ(ffi_defines_get("BUILD_FLAVOR"), "prod");
  /* 覆盖内建键也允许（-D OS_TYPE=... 用于交叉验证/测试）；复位后回到平台真值 */
  const std::string builtin_os = builtin("OS_TYPE");
  ASSERT_EQ(ffi_defines_add_kv("OS_TYPE=CROSS"), 0);
  EXPECT_STREQ(ffi_defines_get("OS_TYPE"), "CROSS");
  ffi_defines_reset();
  EXPECT_EQ(builtin("OS_TYPE"), builtin_os);
  EXPECT_STRNE(ffi_defines_get("OS_TYPE"), "CROSS");
}

TEST_F(FfiAssertTest, ValidateAcceptsGrammar) {
  char err[256] = {0};
  EXPECT_TRUE(ffi_cond_validate("OS_TYPE == \"LINUX\"", err, sizeof err))
      << err;
  EXPECT_TRUE(ffi_cond_validate(
      "OS_TYPE == \"WIN64\" || OS_TYPE == \"WIN32\"", err, sizeof err))
      << err;
  EXPECT_TRUE(ffi_cond_validate(
      "OS_TYPE != \"MACOS\" && OS_ARCH == \"X64\"", err, sizeof err))
      << err;
  EXPECT_TRUE(ffi_cond_validate("(OS_TYPE == \"LINUX\")", err, sizeof err))
      << err;
  EXPECT_TRUE(ffi_cond_validate(
      "(A == \"1\" || B == \"2\") && C != \"3\"", err, sizeof err))
      << err;
}

TEST_F(FfiAssertTest, ValidateRejectsBadSyntax) {
  char err[256] = {0};
  EXPECT_FALSE(ffi_cond_validate("", err, sizeof err));
  EXPECT_NE(std::string(err).find("empty condition"), std::string::npos);

  EXPECT_FALSE(ffi_cond_validate("OS_TYPE", err, sizeof err));
  EXPECT_NE(std::string(err).find("expected '==' or '!='"), std::string::npos)
      << err;

  EXPECT_FALSE(ffi_cond_validate("OS_TYPE == LINUX", err, sizeof err));
  EXPECT_NE(std::string(err).find("expected a string literal"), std::string::npos)
      << err;

  EXPECT_FALSE(ffi_cond_validate("OS_TYPE = \"LINUX\"", err, sizeof err));

  EXPECT_FALSE(ffi_cond_validate("(OS_TYPE == \"LINUX\"", err, sizeof err));
  EXPECT_NE(std::string(err).find("expected ')'"), std::string::npos) << err;

  EXPECT_FALSE(ffi_cond_validate("OS_TYPE == \"LINUX\" &&", err, sizeof err));

  EXPECT_FALSE(ffi_cond_validate("OS_TYPE == \"LINUX\" $", err, sizeof err));
  EXPECT_NE(std::string(err).find("unexpected character"), std::string::npos)
      << err;

  EXPECT_FALSE(ffi_cond_validate("OS_TYPE == \"unterminated", err, sizeof err));
  EXPECT_NE(std::string(err).find("unterminated string"), std::string::npos)
      << err;
}

TEST_F(FfiAssertTest, EvalComparisonsAndPrecedence) {
  ASSERT_EQ(ffi_defines_add("OS_TYPE", "WIN64"), 0);
  ASSERT_EQ(ffi_defines_add("OS_ARCH", "X64"), 0);
  char err[256] = {0};
  bool r = false;

  ASSERT_TRUE(ffi_cond_eval("OS_TYPE == \"WIN64\"", &r, err, sizeof err)) << err;
  EXPECT_TRUE(r);
  ASSERT_TRUE(ffi_cond_eval("OS_TYPE != \"WIN64\"", &r, err, sizeof err)) << err;
  EXPECT_FALSE(r);

  /* && 优先级高于 ||：false && false || true == true */
  ASSERT_TRUE(ffi_cond_eval("OS_TYPE == \"LINUX\" && OS_ARCH == \"ARM64\" || "
                            "OS_TYPE == \"WIN64\"",
                            &r, err, sizeof err))
      << err;
  EXPECT_TRUE(r);
  /* 括号改变结合：(false || true) && false == false */
  ASSERT_TRUE(ffi_cond_eval("(OS_TYPE == \"LINUX\" || OS_TYPE == \"WIN64\") && "
                            "OS_ARCH == \"ARM64\"",
                            &r, err, sizeof err))
      << err;
  EXPECT_FALSE(r);
  /* 多重比较全真 */
  ASSERT_TRUE(ffi_cond_eval(
      "OS_TYPE == \"WIN64\" && OS_ARCH == \"X64\" && OS_TYPE != \"LINUX\"", &r,
      err, sizeof err))
      << err;
  EXPECT_TRUE(r);
}

TEST_F(FfiAssertTest, EvalUnknownKeyRejected) {
  char err[256] = {0};
  bool r = true;
  EXPECT_FALSE(ffi_cond_eval("OS_TYPO == \"LINUX\"", &r, err, sizeof err));
  EXPECT_NE(std::string(err).find("unknown define 'OS_TYPO'"), std::string::npos)
      << err;
  EXPECT_NE(std::string(err).find("-D OS_TYPO="), std::string::npos) << err;
  /* 只查语法时不因为键不存在而失败 */
  EXPECT_TRUE(ffi_cond_validate("OS_TYPO == \"LINUX\"", err, sizeof err)) << err;
}

/* ================================================================ */
/* 二、解析：assert 后缀进入 AST（规范化文本）                        */
/* ================================================================ */

namespace {

/* 解析一段源码，返回第一个 AST_EXTERN_LIB（没有则 nullptr）。 */
ast_extern_lib_t *parse_first_extern_lib(const char *src, diag_buf_t *diag,
                                         allocator_t *alloc, arena_t **arena,
                                         lexer_t **lexer, vec_t **pool,
                                         char **out_buf) {
  size_t len = std::strlen(src);
  char *buf = (char *)malloc(len + 1);
  memcpy(buf, src, len + 1);
  if (out_buf) *out_buf = buf; /* token 的 strslice 借用它：调用方负责释放 */
  stream_source_t mem = stream_source_mem(alloc, buf, len, false);
  istream_t *stream = istream_open(alloc, mem);
  *lexer = lexer_create(alloc, stream, "test.cx");
  *pool = vec_new(alloc, true);
  for (;;) {
    token_t *t = lexer_next(*lexer);
    token_kind_t k = token_get_kind(t);
    vec_push(*pool, alloc, t);
    if (k == TOKEN_TYPE_EOF || k == TOKEN_TYPE_ERROR) break;
  }
  *arena = arena_new_default(alloc);
  parser_t *p = parser_create(alloc, *arena, *pool);
  p->diag = diag;
  ast_node_t *ast = parser_parse(p);
  parser_destroy(&p);
  if (!ast || ast->kind != AST_PROGRAM) return nullptr;
  for (ast_node_t *n = ((ast_program_t *)ast)->funcs; n; n = n->next) {
    if (n->kind == AST_EXTERN_LIB) return (ast_extern_lib_t *)n;
  }
  return nullptr;
}

void free_parse(allocator_t *alloc, arena_t **arena, lexer_t **lexer,
                vec_t **pool) {
  if (*arena) arena_destroy(alloc, arena);
  if (*lexer) lexer_close(lexer);
  if (*pool) vec_free(alloc, pool);
}

} // namespace

TEST_F(FfiAssertTest, ParsesCondIntoAst) {
  allocator_t *alloc = create_allocator(malloc, free);
  diag_buf_t *diag = diag_buf_new(alloc);
  arena_t *arena = nullptr;
  lexer_t *lexer = nullptr;
  vec_t *pool = nullptr;
  char *buf = nullptr;

  ast_extern_lib_t *el = parse_first_extern_lib(
      "extern lib crt from \"msvcrt\" assert(OS_TYPE==\"WIN64\"||"
      "OS_TYPE==\"WIN32\");\n"
      "func main(): i32 { return 0; }\n",
      diag, alloc, &arena, &lexer, &pool, &buf);
  ASSERT_NE(el, nullptr);
  EXPECT_FALSE(diag_has_error(diag));
  ASSERT_EQ(el->alias.len, 3u);
  EXPECT_EQ(memcmp(el->alias.ptr, "crt", 3), 0);
  /* 规范化：token 之间单空格、'(' 后 / ')' 前不留空格 */
  const std::string cond(el->cond.ptr, el->cond.len);
  EXPECT_EQ(cond, "OS_TYPE == \"WIN64\" || OS_TYPE == \"WIN32\"") << cond;

  free_parse(alloc, &arena, &lexer, &pool);
  free(buf);
  diag_buf_destroy(&diag);
  delete_allocator(&alloc);
}

TEST_F(FfiAssertTest, ParsesNoCondAsEmpty) {
  allocator_t *alloc = create_allocator(malloc, free);
  diag_buf_t *diag = diag_buf_new(alloc);
  arena_t *arena = nullptr;
  lexer_t *lexer = nullptr;
  vec_t *pool = nullptr;
  char *buf = nullptr;
  ast_extern_lib_t *el = parse_first_extern_lib(
      "extern lib libc from \"*\";\nfunc main(): i32 { return 0; }\n", diag,
      alloc, &arena, &lexer, &pool, &buf);
  ASSERT_NE(el, nullptr);
  EXPECT_EQ(el->cond.len, 0u);
  free_parse(alloc, &arena, &lexer, &pool);
  free(buf);
  diag_buf_destroy(&diag);
  delete_allocator(&alloc);
}

TEST_F(FfiAssertTest, RejectsMalformedCond) {
  allocator_t *alloc = create_allocator(malloc, free);
  diag_buf_t *diag = diag_buf_new(alloc);
  arena_t *arena = nullptr;
  lexer_t *lexer = nullptr;
  vec_t *pool = nullptr;
  char *buf = nullptr;
  (void)parse_first_extern_lib(
      "extern lib crt from \"c\" assert(OS_TYPE LINUX);\n"
      "func main(): i32 { return 0; }\n",
      diag, alloc, &arena, &lexer, &pool, &buf);
  ASSERT_TRUE(diag_has_error(diag));
  const diagnostic_t *items = diag_items(diag);
  ASSERT_NE(items, nullptr);
  EXPECT_NE(std::strstr(items[0].message, "expected '==' or '!='"), nullptr)
      << items[0].message;
  free_parse(alloc, &arena, &lexer, &pool);
  free(buf);
  diag_buf_destroy(&diag);
  delete_allocator(&alloc);
}

TEST_F(FfiAssertTest, AssertIsSoftKeywordOnlyInExternLib) {
  /* `assert` 不是保留字：普通标识符照常可用 */
  const std::string src =
      "func main(): i32 {\n"
      "  var assert: i32 = 3;\n"
      "  return assert;\n"
      "}\n";
  clux_expect_ok(src, "assert_soft_kw");
}

/* ================================================================ */
/* 三、端到端：裁剪、同别名多平台声明、-D 注入                        */
/* ================================================================ */

TEST_F(FfiAssertTest, InactiveDeclarationIsPruned) {
  /* 同别名三条互斥声明（三平台各一条真实库名）：只有条件为真的那条生效
     （否则 duplicate alias）。这本身就是"跨平台选库"的正例。 */
  const std::string src =
      "extern lib libc from \"*\";\n"
      "extern func libc::printf(format: str, ...): void;\n"
      "extern lib crt from \"msvcrt\" assert(OS_TYPE == \"WIN64\" || "
      "OS_TYPE == \"WIN32\");\n"
      "extern lib crt from \"libc.so.6\" assert(OS_TYPE == \"LINUX\");\n"
      "extern lib crt from \"libSystem.B.dylib\" assert(OS_TYPE == \"MACOS\");\n"
      "extern func crt::strlen(s: str): usize;\n"
      "func main(): i32 {\n"
      "  libc::printf(\"len=%d\\n\", crt::strlen(\"hello\") as i32);\n"
      "  return 0;\n"
      "}\n";
  clux_expect_ok(src, "assert_prune");

  /* 反向：注入一个谁都不匹配的 OS_TYPE → 三条全被裁剪 → 别名 crt 无声明可依 */
  ASSERT_EQ(ffi_defines_add("OS_TYPE", "NOSUCHOS"), 0);
  std::string path = write_temp_file(src, "assert_prune_d");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("crt"), std::string::npos) << err;
  std::remove(path.c_str());
  ffi_defines_reset();
}

TEST_F(FfiAssertTest, UnknownAssertKeyRejectedAtCompileTime) {
  const std::string src =
      "extern lib crt from \"c\" assert(OS_TYPO == \"LINUX\");\n"
      "func main(): i32 { return 0; }\n";
  std::string path = write_temp_file(src, "assert_unknown");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("unknown define 'OS_TYPO'"), std::string::npos) << err;
  std::remove(path.c_str());
}

TEST_F(FfiAssertTest, CustomKeySwitchesDeclaration) {
  /* 自定义键（不是 OS_TYPE）：同别名两条声明按 BUILD_FLAVOR 切换 */
  /* 两条声明都用三平台都可加载的库（"*" = 当前进程；测试库随构建产出），
     只验证"注入的键决定哪一条生效" */
  const std::string src =
      "extern lib crt from \"*\" assert(BUILD_FLAVOR == \"dev\");\n"
      "extern lib crt from \"clux_ffi_testdata\" assert(BUILD_FLAVOR != \"dev\");\n"
      "func main(): i32 {\n"
      "  return 0;\n"
      "}\n";
  /* 未注入 BUILD_FLAVOR → 求值报未知键（不是静默 false） */
  {
    std::string path = write_temp_file(src, "assert_custom_nokey");
    std::string err;
    EXPECT_EQ(clux_run_capture(path, &err), 1);
    EXPECT_NE(err.find("unknown define 'BUILD_FLAVOR'"), std::string::npos)
        << err;
    std::remove(path.c_str());
  }
  /* 注入 dev → msvcrt 生效 */
  ASSERT_EQ(ffi_defines_add("BUILD_FLAVOR", "dev"), 0);
  clux_expect_ok(src, "assert_custom_dev");
  /* 注入 prod → 测试库生效（两条声明都不引用符号，只验证"加载的是哪条"） */
  ASSERT_EQ(ffi_defines_add("BUILD_FLAVOR", "prod"), 0);
  clux_expect_ok(src, "assert_custom_prod");
  ffi_defines_reset();
}

TEST_F(FfiAssertTest, EmitKeepsOnlyActiveDeclaration) {
  /* 两条声明都不引用符号：容器内容只反映"哪一条存活" */
  const std::string src =
      "extern lib crt from \"clux_ffi_testdata\" assert(OS_TYPE == \"WIN64\" || "
      "OS_TYPE == \"WIN32\");\n"
      "extern lib crt from \"*\" assert(OS_TYPE == \"LINUX\" || "
      "OS_TYPE == \"MACOS\");\n"
      "func main(): i32 { return 0; }\n";
  std::string path = write_temp_file(src, "assert_emit");
  std::string asm_path = path + ".cxs";
  ASSERT_EQ(driver_build_asm(path.c_str(), asm_path.c_str()), 0);
  const std::string text = slurp(asm_path);
  /* 只留一条 externlib：Windows 上是测试库那条，POSIX 上是 "*" 那条 */
#if defined(_WIN32)
  EXPECT_NE(text.find("externlib \"clux_ffi_testdata\" as crt"), std::string::npos)
      << text;
  EXPECT_EQ(text.find("externlib \"*\" as crt"), std::string::npos) << text;
#else
  EXPECT_NE(text.find("externlib \"*\" as crt"), std::string::npos) << text;
  EXPECT_EQ(text.find("externlib \"clux_ffi_testdata\" as crt"),
            std::string::npos)
      << text;
#endif
  std::remove(path.c_str());
  std::remove(asm_path.c_str());
}

/* ================================================================ */
/* 四、fmt：条件后缀必须原样保留（幂等）                              */
/* ================================================================ */

namespace {
std::string fmt_src(const char *src) {
  allocator_t *a = create_allocator(malloc, free);
  char *out = fmt_format_source(a, src, std::strlen(src), nullptr);
  std::string result = out ? out : "";
  if (out) allocator_free(a, (void **)&out);
  delete_allocator(&a);
  return result;
}
} // namespace

TEST_F(FfiAssertTest, FmtPreservesAssertSuffix) {
  const std::string out = fmt_src(
      "extern lib crt from \"msvcrt\" assert(OS_TYPE==\"WIN64\"||"
      "OS_TYPE==\"WIN32\");\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_NE(out.find("extern lib crt from \"msvcrt\" assert("), std::string::npos)
      << out;
  EXPECT_NE(out.find("OS_TYPE == \"WIN64\" || OS_TYPE == \"WIN32\""),
            std::string::npos)
      << out;
  EXPECT_EQ(out, fmt_src(out.c_str())); /* 幂等 */
}

/* ================================================================ */
/* 五、命令行参数：`-D KEY=VALUE` / `-DKEY=VALUE` 的收集              */
/* ================================================================ */

TEST_F(FfiAssertTest, TakeDefinesTwoTokenAndGluedForms) {
  const char *pos[8];
  size_t np = 0;
  const char *kv[8];
  size_t nkv = 0;

  /* `-D KEY=VALUE`（两 token）*/
  const char *a[] = {"prog.cx", "-D", "OS_TYPE=LINUX"};
  ASSERT_EQ(cmd_take_defines(a, 3, pos, &np, 8, kv, &nkv, 8), 0);
  EXPECT_EQ(np, 1u);
  EXPECT_STREQ(pos[0], "prog.cx");
  ASSERT_EQ(nkv, 1u);
  EXPECT_STREQ(kv[0], "OS_TYPE=LINUX");

  /* `-DKEY=VALUE`（粘连）*/
  const char *b[] = {"-DFOO=bar", "prog.cx"};
  ASSERT_EQ(cmd_take_defines(b, 2, pos, &np, 8, kv, &nkv, 8), 0);
  EXPECT_EQ(np, 1u);
  EXPECT_STREQ(pos[0], "prog.cx");
  ASSERT_EQ(nkv, 1u);
  EXPECT_STREQ(kv[0], "FOO=bar");

  /* 多个 -D、与 -o 风格参数共存（-o 由另一个 helper 先剥） */
  const char *c[] = {"-DA=1", "-D", "B=2", "in.cx"};
  ASSERT_EQ(cmd_take_defines(c, 4, pos, &np, 8, kv, &nkv, 8), 0);
  EXPECT_EQ(np, 1u);
  EXPECT_STREQ(pos[0], "in.cx");
  ASSERT_EQ(nkv, 2u);
  EXPECT_STREQ(kv[0], "A=1");
  EXPECT_STREQ(kv[1], "B=2");
}

TEST_F(FfiAssertTest, TakeDefinesErrors) {
  const char *pos[8];
  size_t np = 0;
  const char *kv[8];
  size_t nkv = 0;

  /* `-D` 缺值 */
  const char *a[] = {"prog.cx", "-D"};
  EXPECT_EQ(cmd_take_defines(a, 2, pos, &np, 8, kv, &nkv, 8), 1);

  /* 条目超过容量 */
  const char *b[] = {"-DA=1", "-DB=2"};
  EXPECT_EQ(cmd_take_defines(b, 2, pos, &np, 8, kv, &nkv, 1), 2);
}

TEST_F(FfiAssertTest, ApplyDefinesWritesGlobalTable) {
  const char *kv[] = {"BUILD_FLAVOR=dev", "OS_ARCH=ARM64"};
  ASSERT_EQ(cmd_apply_defines(kv, 2), 0);
  EXPECT_STREQ(ffi_defines_get("BUILD_FLAVOR"), "dev");
  EXPECT_STREQ(ffi_defines_get("OS_ARCH"), "ARM64");
  /* 非法条目：键不是标识符 */
  const char *bad[] = {"1BAD=1"};
  EXPECT_EQ(cmd_apply_defines(bad, 1), 1);
  ffi_defines_reset();
}