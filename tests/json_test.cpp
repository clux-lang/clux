/*
 * Description: JSON parser unit tests
 * Create: 2026-10-12
 */

#include <gtest/gtest.h>
#include <cstring>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "core/arena.h"
#include "core/json.h"
#include "core/panic.h"
}

#include "test_common.h"

namespace {

class JsonTest : public ::testing::Test {
protected:
    allocator_t *alloc = nullptr;
    arena_t     *arena = nullptr;

    void SetUp() override {
        alloc = create_allocator(malloc, free);
        arena = arena_new_default(alloc);
    }

    void TearDown() override {
        arena_destroy(alloc, &arena);
        EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
    }

    /* 解析一段 JSON 文本，返回根值 */
    const json_value_t *parse(const std::string &text,
                              json_error_t *err = nullptr) {
        return json_parse(arena, text.data(), text.size(), err);
    }
};

/* ---- 基本类型 ---- */

TEST_F(JsonTest, ParseNull) {
    auto v = parse("null");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_NULL);
}

TEST_F(JsonTest, ParseTrue) {
    auto v = parse("true");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_BOOL);
    EXPECT_EQ(json_as_bool(v, false), true);
}

TEST_F(JsonTest, ParseFalse) {
    auto v = parse("false");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_BOOL);
    EXPECT_EQ(json_as_bool(v, true), false);
}

TEST_F(JsonTest, ParseInteger) {
    auto v = parse("42");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_NUMBER);
    EXPECT_DOUBLE_EQ(json_as_number(v, 0), 42.0);
}

TEST_F(JsonTest, ParseNegativeFloat) {
    auto v = parse("-3.14");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_NUMBER);
    EXPECT_DOUBLE_EQ(json_as_number(v, 0), -3.14);
}

TEST_F(JsonTest, ParseExponent) {
    auto v = parse("1e5");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_NUMBER);
    EXPECT_DOUBLE_EQ(json_as_number(v, 0), 100000.0);
}

TEST_F(JsonTest, ParseString) {
    auto v = parse("\"hello\"");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_STRING);
    EXPECT_STREQ(json_as_string(v), "hello");
}

TEST_F(JsonTest, ParseStringWithEscapes) {
    auto v = parse("\"hello\\nworld\\t!\"");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_STRING);
    EXPECT_STREQ(json_as_string(v), "hello\nworld\t!");
}

TEST_F(JsonTest, ParseEmptyString) {
    auto v = parse("\"\"");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_STRING);
    EXPECT_STREQ(json_as_string(v), "");
}

TEST_F(JsonTest, ParseEmptyObject) {
    auto v = parse("{}");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_OBJECT);
    EXPECT_EQ(v->u.obj.count, 0u);
}

TEST_F(JsonTest, ParseEmptyArray) {
    auto v = parse("[]");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_ARRAY);
    EXPECT_EQ(v->u.arr.count, 0u);
}

/* ---- 对象 ---- */

TEST_F(JsonTest, ParseObjectWithMembers) {
    auto v = parse("{\"name\": \"clux\", \"version\": \"0.1.0\"}");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_OBJECT);
    EXPECT_EQ(v->u.obj.count, 2u);

    auto name = json_object_get(v, "name");
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(json_as_string(name), "clux");

    auto ver = json_object_get(v, "version");
    ASSERT_NE(ver, nullptr);
    EXPECT_STREQ(json_as_string(ver), "0.1.0");
}

TEST_F(JsonTest, ParseObjectGetMissingKey) {
    auto v = parse("{\"a\": 1}");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(json_object_get(v, "b"), nullptr);
}

/* ---- 数组 ---- */

TEST_F(JsonTest, ParseArrayOfStrings) {
    auto v = parse("[\"a\", \"b\", \"c\"]");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_ARRAY);
    EXPECT_EQ(v->u.arr.count, 3u);
    EXPECT_STREQ(json_as_string(json_array_get(v, 0)), "a");
    EXPECT_STREQ(json_as_string(json_array_get(v, 1)), "b");
    EXPECT_STREQ(json_as_string(json_array_get(v, 2)), "c");
    EXPECT_EQ(json_array_get(v, 3), nullptr);
}

TEST_F(JsonTest, ParseArrayOfNumbers) {
    auto v = parse("[1, 2, 3]");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_ARRAY);
    EXPECT_EQ(v->u.arr.count, 3u);
    EXPECT_DOUBLE_EQ(json_as_number(json_array_get(v, 0), 0), 1.0);
    EXPECT_DOUBLE_EQ(json_as_number(json_array_get(v, 1), 0), 2.0);
    EXPECT_DOUBLE_EQ(json_as_number(json_array_get(v, 2), 0), 3.0);
}

/* ---- 嵌套 ---- */

TEST_F(JsonTest, ParseNestedObjectInArray) {
    auto v = parse("[{\"k\": 1}, {\"k\": 2}]");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_ARRAY);
    auto e0 = json_array_get(v, 0);
    ASSERT_NE(e0, nullptr);
    EXPECT_DOUBLE_EQ(json_as_number(json_object_get(e0, "k"), 0), 1.0);
    auto e1 = json_array_get(v, 1);
    ASSERT_NE(e1, nullptr);
    EXPECT_DOUBLE_EQ(json_as_number(json_object_get(e1, "k"), 0), 2.0);
}

TEST_F(JsonTest, ParseObjectWithArray) {
    auto v = parse("{\"entries\": [\"a.clux\", \"b.clux\"]}");
    ASSERT_NE(v, nullptr);
    auto entries = json_object_get(v, "entries");
    ASSERT_NE(entries, nullptr);
    EXPECT_EQ(entries->kind, JSON_ARRAY);
    EXPECT_EQ(entries->u.arr.count, 2u);
    EXPECT_STREQ(json_as_string(json_array_get(entries, 0)), "a.clux");
    EXPECT_STREQ(json_as_string(json_array_get(entries, 1)), "b.clux");
}

/* ---- 空白处理 ---- */

TEST_F(JsonTest, ParseWithWhitespace) {
    auto v = parse("  {\n  \"key\"  :  \"value\"  \n}  ");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_OBJECT);
    EXPECT_STREQ(json_as_string(json_object_get(v, "key")), "value");
}

/* ---- 错误处理 ---- */

TEST_F(JsonTest, ErrorTrailingData) {
    json_error_t err;
    auto v = parse("{} garbage", &err);
    EXPECT_EQ(v, nullptr);
    EXPECT_NE(err.msg, nullptr);
}

TEST_F(JsonTest, ErrorUnterminatedString) {
    json_error_t err;
    auto v = parse("\"hello", &err);
    EXPECT_EQ(v, nullptr);
    EXPECT_NE(err.msg, nullptr);
}

TEST_F(JsonTest, ErrorMissingColon) {
    json_error_t err;
    auto v = parse("{\"key\" \"value\"}", &err);
    EXPECT_EQ(v, nullptr);
    EXPECT_NE(err.msg, nullptr);
}

TEST_F(JsonTest, ErrorInvalidLiteral) {
    json_error_t err;
    auto v = parse("tru", &err);
    EXPECT_EQ(v, nullptr);
    EXPECT_NE(err.msg, nullptr);
}

/* ---- clux.mod 模拟 ---- */

TEST_F(JsonTest, ParseCluxModSingleEntry) {
    std::string mod = R"({
        "name": "myapp",
        "version": "0.1.0",
        "entry": "src/main.clux"
    })";
    auto v = parse(mod);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_OBJECT);
    EXPECT_STREQ(json_as_string(json_object_get(v, "name")), "myapp");
    EXPECT_STREQ(json_as_string(json_object_get(v, "version")), "0.1.0");
    EXPECT_STREQ(json_as_string(json_object_get(v, "entry")), "src/main.clux");
}

TEST_F(JsonTest, ParseCluxModMultiEntry) {
    std::string mod = R"({
        "name": "std",
        "version": "0.1.0",
        "entry": {
            "std/array": "src/array.clux",
            "std/io": "src/io.clux"
        }
    })";
    auto v = parse(mod);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->kind, JSON_OBJECT);
    EXPECT_STREQ(json_as_string(json_object_get(v, "name")), "std");

    auto entry = json_object_get(v, "entry");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->kind, JSON_OBJECT);
    EXPECT_EQ(entry->u.obj.count, 2u);
    EXPECT_STREQ(json_as_string(json_object_get(entry, "std/array")),
                 "src/array.clux");
    EXPECT_STREQ(json_as_string(json_object_get(entry, "std/io")),
                 "src/io.clux");
}

TEST_F(JsonTest, ParseCluxModWithDeps) {
    std::string mod = R"({
        "name": "myapp",
        "version": "0.2.0",
        "entry": "src/main.clux",
        "deps": {
            "std": "github:clux-lang/std@0.1.0",
            "local": "file:/usr/local/clux-lib"
        }
    })";
    auto v = parse(mod);
    ASSERT_NE(v, nullptr);
    auto deps = json_object_get(v, "deps");
    ASSERT_NE(deps, nullptr);
    EXPECT_EQ(deps->kind, JSON_OBJECT);
    EXPECT_EQ(deps->u.obj.count, 2u);
    EXPECT_STREQ(json_as_string(json_object_get(deps, "std")),
                 "github:clux-lang/std@0.1.0");
    EXPECT_STREQ(json_as_string(json_object_get(deps, "local")),
                 "file:/usr/local/clux-lib");
}

/* ---- 访问器回退 ---- */

TEST_F(JsonTest, AsStringOnNonString) {
    auto v = parse("42");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(json_as_string(v), nullptr);
}

TEST_F(JsonTest, AsBoolOnNonBool) {
    auto v = parse("\"hello\"");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(json_as_bool(v, true), true);
    EXPECT_EQ(json_as_bool(v, false), false);
}

TEST_F(JsonTest, AsNumberOnNonNumber) {
    auto v = parse("true");
    ASSERT_NE(v, nullptr);
    EXPECT_DOUBLE_EQ(json_as_number(v, 99.0), 99.0);
}

} // namespace
