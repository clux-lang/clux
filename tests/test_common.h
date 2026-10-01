#ifndef CLUX_TEST_COMMON_H
#define CLUX_TEST_COMMON_H

#include "core/allocator.h"
#include <cstdio>
#include <gtest/gtest.h>
#include <string>

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
