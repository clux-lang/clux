#include <gtest/gtest.h>
#include "core/position.h"
#include "core/allocator.h"
#include <string.h>

static position_t make_pos(const char *src) {
  position_t p;
  p.filename = "test.clux";
  p.offset = src;
  p.line = 1;
  p.column = 1;
  return p;
}

TEST(PositionTest, NextAsciiSingleByte) {
  const char *src = "abc";
  position_t p = make_pos(src);
  EXPECT_TRUE(position_next(&p));
  EXPECT_EQ(p.offset, src + 1);
  EXPECT_EQ(p.line, 1u);
  EXPECT_EQ(p.column, 2u);
  EXPECT_TRUE(position_next(&p));
  EXPECT_EQ(p.offset, src + 2);
  EXPECT_EQ(p.column, 3u);
}

TEST(PositionTest, NextNewlineResetsColumnIncrementsLine) {
  const char *src = "a\nb";
  position_t p = make_pos(src);
  position_next(&p); // 'a' -> column 2
  EXPECT_EQ(p.column, 2u);
  EXPECT_EQ(p.line, 1u);
  EXPECT_TRUE(position_next(&p)); // '\n'
  EXPECT_EQ(p.line, 2u);
  EXPECT_EQ(p.column, 1u);
  EXPECT_EQ(p.offset, src + 2);
}

TEST(PositionTest, NextMultiByteUTF8) {
  /* "世" = U+4E16, UTF-8: E4 B8 96 (3 bytes) */
  const char *src = "世x";
  position_t p = make_pos(src);
  EXPECT_TRUE(position_next(&p));
  EXPECT_EQ(p.offset, src + 3);
  EXPECT_EQ(p.column, 2u);
  EXPECT_EQ(p.line, 1u);
  // 下一个应该是 'x'
  EXPECT_EQ(*p.offset, 'x');
}

TEST(PositionTest, NextFourByteUTF8) {
  /* U+1F600 😀 = F0 9F 98 80 (4 bytes) */
  const char *src = "\xF0\x9F\x98\x80x";
  position_t p = make_pos(src);
  EXPECT_TRUE(position_next(&p));
  EXPECT_EQ(p.offset, src + 4);
  EXPECT_EQ(p.column, 2u);
}

TEST(PositionTest, NextTwoByteUTF8) {
  /* U+00E9 é = C3 A9 (2 bytes) */
  const char *src = "\xC3\xA9x";
  position_t p = make_pos(src);
  EXPECT_TRUE(position_next(&p));
  EXPECT_EQ(p.offset, src + 2);
  EXPECT_EQ(p.column, 2u);
  EXPECT_EQ(*p.offset, 'x');
}

TEST(PositionTest, NextAtNullReturnsFalse) {
  const char *src = "";
  position_t p = make_pos(src);
  EXPECT_FALSE(position_next(&p));
  EXPECT_EQ(p.offset, src);
  EXPECT_EQ(p.line, 1u);
  EXPECT_EQ(p.column, 1u);
}

TEST(PositionTest, NextToEndOfAscii) {
  const char *src = "ab";
  position_t p = make_pos(src);
  EXPECT_TRUE(position_next(&p));  // 'a'
  EXPECT_TRUE(position_next(&p));  // 'b' -> offset=src+2, points to '\0'
  EXPECT_FALSE(position_next(&p)); // at '\0'
  EXPECT_EQ(p.offset, src + 2);
}

TEST(PositionTest, NextConsecutiveNewlines) {
  const char *src = "a\n\nb";
  position_t p = make_pos(src);
  position_next(&p); // 'a'
  position_next(&p); // '\n' -> line 2
  EXPECT_EQ(p.line, 2u);
  EXPECT_EQ(p.column, 1u);
  position_next(&p); // '\n' -> line 3
  EXPECT_EQ(p.line, 3u);
  EXPECT_EQ(p.column, 1u);
  position_next(&p); // 'b'
  EXPECT_EQ(p.line, 3u);
  EXPECT_EQ(p.column, 2u);
}

TEST(PositionTest, NextMultiByteAfterNewline) {
  const char *src = "\n世";
  position_t p = make_pos(src);
  position_next(&p); // '\n' -> line 2, column 1
  EXPECT_EQ(p.line, 2u);
  EXPECT_EQ(p.column, 1u);
  position_next(&p); // '世' -> 3 bytes
  EXPECT_EQ(p.line, 2u);
  EXPECT_EQ(p.column, 2u);
  EXPECT_EQ(p.offset, src + 4);
}
