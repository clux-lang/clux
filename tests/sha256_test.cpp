#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

extern "C" {
#include "core/sha256.h"
}

TEST(Sha256, KnownVectors) {
  char hex[65];
  clux_sha256_hex("", 0, hex);
  EXPECT_STREQ(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  clux_sha256_hex("abc", 3, hex);
  EXPECT_STREQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  clux_sha256_hex("hello", 5, hex);
  EXPECT_STREQ(hex, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");

  std::string a56(56, 'a');
  clux_sha256_hex(a56.data(), a56.size(), hex);
  EXPECT_STREQ(hex, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");

  std::string a64(64, 'a');
  clux_sha256_hex(a64.data(), a64.size(), hex);
  EXPECT_STREQ(hex, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

  std::string a100(100, 'a');
  clux_sha256_hex(a100.data(), a100.size(), hex);
  EXPECT_STREQ(hex, "2816597888e4a0d3a36b82b83316ab32680eb8f00f8cd3b904d681246d285a0e");
}

TEST(Sha256, FileMatchesBuffer) {
  const char *path = "build/tmp/sha256_file_test.bin";
  std::filesystem::create_directories("build/tmp");
  FILE *f = std::fopen(path, "wb");
  ASSERT_NE(f, nullptr);
  std::fputs("hello", f);
  std::fclose(f);

  char file_hex[65];
  ASSERT_TRUE(clux_sha256_file_hex(path, file_hex));
  EXPECT_STREQ(file_hex, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
  std::remove(path);
}
