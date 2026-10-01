#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "core/allocator.h"
#include "core/vec.h"
#include "driver/driver.h"
#include "parser/lexer.h"
}

#include "test_common.h"

namespace {

std::string write_temp_file(const std::string &content) {
  auto path = std::filesystem::temp_directory_path() / "clux_test_XXXXXX";
  auto path_str = path.string();
  /* mkstemps is not available on Windows; use a simple unique name. */
  static int counter = 0;
  path_str += std::to_string(counter++);
  FILE *fp = fopen(path_str.c_str(), "wb");
  fwrite(content.data(), 1, content.size(), fp);
  fclose(fp);
  return path_str;
}

} // namespace

/* ---- Stage ①: load source ---- */

TEST(Driver, LoadSource) {
  allocator_t *alloc = create_allocator(malloc, free);
  std::string path = write_temp_file("func main(): void {}");

  const char *data = NULL;
  size_t len = 0;
  EXPECT_EQ(driver_load_source(alloc, path.c_str(), &data, &len), 0);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(len, 20u);
  EXPECT_EQ(std::string(data, len), "func main(): void {}");

  allocator_free(alloc, (void **)&data);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
  std::remove(path.c_str());
}

TEST(Driver, LoadSourceMissingFile) {
  allocator_t *alloc = create_allocator(malloc, free);
  const char *data = NULL;
  size_t len = 0;
  EXPECT_EQ(driver_load_source(alloc, "no/such/file.cx", &data, &len), -1);
  EXPECT_EQ(data, nullptr);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
}

/* ---- Stage ②: lex into a pool ---- */

TEST(Driver, LexFileProducesTokens) {
  allocator_t *alloc = create_allocator(malloc, free);
  std::string path = write_temp_file("func main(): void {\n  return 0;\n}\n");

  vec_t *pool = NULL;
  ASSERT_EQ(driver_lex_file(alloc, path.c_str(), &pool), 0);
  ASSERT_NE(pool, nullptr);
  ASSERT_GT(vec_len(pool), 0u);

  /* Collect kinds in order. */
  std::vector<token_kind_t> kinds;
  for (size_t i = 0; i < vec_len(pool); i++) {
    const token_t *t = (const token_t *)vec_get(pool, i);
    kinds.push_back(token_get_kind(t));
  }

  /* Expect the leading keyword "func" and an EOF terminator. */
  EXPECT_EQ(kinds.front(), TOKEN_TYPE_KEYWORD);
  EXPECT_EQ(kinds.back(), TOKEN_TYPE_EOF);

  vec_free(alloc, &pool);
  EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
  std::remove(path.c_str());
}
