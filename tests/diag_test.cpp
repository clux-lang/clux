/*
 * Description: diag_buf_t diagnostic collector unit tests
 * Create: 2026-09-08
 */

#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "diag/diagnostic.h"
#include "parser/location.h"
}

#include "test_common.h"

namespace {

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr)     { free(ptr); }

static location_t make_loc(size_t line, size_t col) {
    location_t loc;
    loc.filename = "test.clx";
    loc.begin.line   = line;
    loc.begin.column = col;
    loc.end.line     = line;
    loc.end.column   = col;
    return loc;
}

class DiagTest : public ::testing::Test {
protected:
    void SetUp() override {
        alloc_ = create_allocator(test_alloc, test_free);
        db_    = diag_buf_new(alloc_);
    }
    void TearDown() override {
        diag_buf_destroy(&db_);
        EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc_);
    }

    allocator_t *alloc_ = nullptr;
    diag_buf_t  *db_    = nullptr;
};

/* ================================================================ */
/* 生命周期                                                          */
/* ================================================================ */

TEST_F(DiagTest, CreateDestroy) {
    EXPECT_NE(db_, nullptr);
}

TEST_F(DiagTest, DestroyNullSafe) {
    diag_buf_t *null_db = nullptr;
    diag_buf_destroy(&null_db); /* no-op, must not crash */
}

TEST_F(DiagTest, CreateWithNullAllocReturnsNull) {
    EXPECT_EQ(diag_buf_new(nullptr), nullptr);
}

/* ================================================================ */
/* 记录与查询                                                        */
/* ================================================================ */

TEST_F(DiagTest, EmptyBufferHasNoError) {
    EXPECT_EQ(diag_count(db_), 0u);
    EXPECT_FALSE(diag_has_error(db_));
    EXPECT_EQ(diag_items(db_), nullptr);
}

TEST_F(DiagTest, RecordSingleError) {
    location_t loc = make_loc(3, 5);
    diag_error(db_, loc, "type mismatch: cannot apply '%s' to %s", "+",
               "i32 and str");

    EXPECT_EQ(diag_count(db_), 1u);
    EXPECT_TRUE(diag_has_error(db_));

    const diagnostic_t *items = diag_items(db_);
    ASSERT_NE(items, nullptr);
    EXPECT_EQ(items[0].level, DIAG_ERROR);
    EXPECT_EQ(items[0].loc.begin.line, 3u);
    EXPECT_EQ(items[0].loc.begin.column, 5u);
    EXPECT_STREQ(items[0].message, "type mismatch: cannot apply '+' to i32 and str");
}

TEST_F(DiagTest, RecordWarningDoesNotSetError) {
    diag_warning(db_, make_loc(1, 1), "deprecated feature");
    EXPECT_EQ(diag_count(db_), 1u);
    EXPECT_FALSE(diag_has_error(db_));
    EXPECT_EQ(diag_items(db_)[0].level, DIAG_WARNING);
}

TEST_F(DiagTest, RecordNote) {
    diag_note(db_, make_loc(1, 1), "see declaration here");
    EXPECT_EQ(diag_count(db_), 1u);
    EXPECT_EQ(diag_items(db_)[0].level, DIAG_NOTE);
}

TEST_F(DiagTest, MixedLevelsHasErrorIfAny) {
    diag_warning(db_, make_loc(1, 1), "w1");
    diag_error(db_, make_loc(2, 1), "e1");
    EXPECT_EQ(diag_count(db_), 2u);
    EXPECT_TRUE(diag_has_error(db_));
}

TEST_F(DiagTest, MessageCopiedNotAliased) {
    /* 格式化缓冲是内部拷贝：调用后源 fmt 不再相关 */
    diag_error(db_, make_loc(1, 1), "%s", "hello");
    EXPECT_STREQ(diag_items(db_)[0].message, "hello");
}

TEST_F(DiagTest, MultipleRecordsOrdered) {
    diag_error(db_, make_loc(1, 1), "first");
    diag_error(db_, make_loc(2, 2), "second");
    diag_error(db_, make_loc(3, 3), "third");

    ASSERT_EQ(diag_count(db_), 3u);
    const diagnostic_t *items = diag_items(db_);
    EXPECT_STREQ(items[0].message, "first");
    EXPECT_EQ(items[0].loc.begin.line, 1u);
    EXPECT_STREQ(items[1].message, "second");
    EXPECT_STREQ(items[2].message, "third");
}

TEST_F(DiagTest, GrowsBeyondInitialCapacity) {
    /* 初始容量 16，写 40 条触发几何扩容 */
    for (int i = 0; i < 40; i++) {
        diag_error(db_, make_loc(i + 1, 1), "message %d", i);
    }
    ASSERT_EQ(diag_count(db_), 40u);
    EXPECT_TRUE(diag_has_error(db_));
    EXPECT_STREQ(diag_items(db_)[39].message, "message 39");
    EXPECT_EQ(diag_items(db_)[39].loc.begin.line, 40u);
}

TEST_F(DiagTest, LongMessageTruncationSafe) {
    char big[2048];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    diag_error(db_, make_loc(1, 1), "%s", big);

    ASSERT_EQ(diag_count(db_), 1u);
    /* 内部缓冲按 vsnprintf 结果精确分配，不应截断 */
    EXPECT_EQ(strlen(diag_items(db_)[0].message), sizeof(big) - 1);
}

TEST_F(DiagTest, PrintAllSmoke) {
    diag_error(db_, make_loc(1, 2), "boom");
    testing::internal::CaptureStderr();
    diag_print_all(db_);
    std::string out = testing::internal::GetCapturedStderr();
    EXPECT_NE(out.find("test.clx:1:2: error: boom"), std::string::npos);
}

TEST_F(DiagTest, PrintSnippetBlockRustStyle) {
    /* 真实存在的临时文件 → 输出 Rust 风格源码片段块 */
    std::string path = std::filesystem::temp_directory_path().string();
    path += "/clux_diag_snippet.txt";
    {
        FILE *fp = fopen(path.c_str(), "wb");
        ASSERT_NE(fp, nullptr);
        fputs("func main(): void { foo(1, 2); }\n", fp);
        fclose(fp);
    }

    location_t loc;
    loc.filename = path.c_str();
    loc.begin.line   = 1;
    loc.begin.column = 18;
    loc.end.line     = 1;
    loc.end.column   = 21; /* 覆盖 "foo"（3 字符） */
    diag_error(db_, loc, "expects 1 arguments, got 2");

    testing::internal::CaptureStderr();
    diag_print_all(db_);
    std::string out = testing::internal::GetCapturedStderr();

    EXPECT_NE(out.find("error: expects 1 arguments, got 2"), std::string::npos);
    EXPECT_NE(out.find(" --> " + path + ":1:18"), std::string::npos);
    EXPECT_NE(out.find("1 | func main(): void { foo(1, 2); }"), std::string::npos);
    EXPECT_NE(out.find("^"), std::string::npos);

    std::remove(path.c_str());
}

TEST_F(DiagTest, NullBufferOpsAreNoop) {
    diag_error(nullptr, make_loc(1, 1), "no-op");
    diag_warning(nullptr, make_loc(1, 1), "no-op");
    diag_note(nullptr, make_loc(1, 1), "no-op");
    EXPECT_EQ(diag_count(nullptr), 0u);
    EXPECT_FALSE(diag_has_error(nullptr));
    EXPECT_EQ(diag_items(nullptr), nullptr);
}

} /* namespace */
