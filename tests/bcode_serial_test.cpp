#include "vm/bcode.h"
#include "vm/bcode_serial.h"
#include "vm/bcode_disasm.h"

#include "core/allocator.h"
#include "core/strslice.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "test_common.h"

/* 构造一个带字符串、立即数、跳转的样例模块：
 *   PUSH_STRING "hello\n" ; CALL 1 ; PUSH_I32 -7 ; JMP 0 ; HALT */
static bytecode_t *make_sample(allocator_t *a) {
    bytecode_t *bc = bcode_new(a);
    /* strtable: 0="printf", 1="hello\n" */
    (void)bcode_str_index(bc, strslice_from_cstr("printf"));
    (void)bcode_str_index(bc, strslice_from_cstr("hello\n"));

    bcode_write_op(bc, BCODE_PUSH_STR);
    bcode_write_u32(bc, 1);
    bcode_write_op(bc, BCODE_CALL);
    bcode_write_u32(bc, 1);
    bcode_write_op(bc, BCODE_PUSH_I32);
    bcode_write_i32(bc, -7);
    bcode_write_op(bc, BCODE_JMP);
    bcode_write_u32(bc, 0);
    bcode_write_op(bc, BCODE_HALT);
    return bc;
}

/* 序列化 → 反序列化后，字节码流与字符串表应逐字节等价。 */
TEST(BcodeSerial, RoundTripBytes) {
    allocator_t *a = create_allocator(malloc, free);
    ASSERT_NE(a, nullptr);

    bytecode_t *bc = make_sample(a);
    size_t n = 0;
    uint8_t *blob = bcode_serial_mem(a, bc, &n);
    ASSERT_NE(blob, nullptr);
    EXPECT_GT(n, 0u);

    /* magic 前缀 */
    EXPECT_EQ(std::memcmp(blob, "CXBC", 4), 0);

    bytecode_t *bc2 = nullptr;
    ASSERT_EQ(bcode_deserial(a, blob, n, &bc2), 0);
    ASSERT_NE(bc2, nullptr);

    /* code 流逐字节一致 */
    ASSERT_EQ(bc->code.len, bc2->code.len);
    EXPECT_EQ(std::memcmp(bc->code.data, bc2->code.data, bc->code.len), 0);

    /* strtable 逐条一致 */
    ASSERT_EQ(bcode_str_count(bc), bcode_str_count(bc2));
    for (size_t i = 0; i < bcode_str_count(bc); i++) {
        strslice_t s1 = bcode_str_at(bc, i);
        strslice_t s2 = bcode_str_at(bc2, i);
        EXPECT_EQ(std::string(s1.ptr, s1.len), std::string(s2.ptr, s2.len));
    }

    allocator_free(a, (void **)&blob);
    bcode_destroy(&bc);
    bcode_destroy(&bc2);
    EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* 空模块（无字符串、无 code）应能正常往返。 */
TEST(BcodeSerial, RoundTripEmpty) {
    allocator_t *a = create_allocator(malloc, free);

    bytecode_t *bc = bcode_new(a);
    size_t n = 0;
    uint8_t *blob = bcode_serial_mem(a, bc, &n);
    ASSERT_NE(blob, nullptr);

    bytecode_t *bc2 = nullptr;
    ASSERT_EQ(bcode_deserial(a, blob, n, &bc2), 0);
    ASSERT_NE(bc2, nullptr);
    EXPECT_EQ(bcode_str_count(bc2), 0u);
    EXPECT_EQ(bc2->code.len, 0u);

    allocator_free(a, (void **)&blob);
    bcode_destroy(&bc);
    bcode_destroy(&bc2);
    EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* 字符串中的转义/控制字符应原样还原（长度前缀，无 NUL 截断问题）。 */
TEST(BcodeSerial, PreservesEmbeddedControlBytes) {
    allocator_t *a = create_allocator(malloc, free);

    const char tricky[] = {'"', '\\', '\t', 'a', '\0', 'b'};
    bytecode_t *bc = bcode_new(a);
    (void)bcode_str_index(bc, strslice_from_bytes(tricky, sizeof tricky));
    bcode_write_op(bc, BCODE_HALT);

    size_t n = 0;
    uint8_t *blob = bcode_serial_mem(a, bc, &n);
    ASSERT_NE(blob, nullptr);

    bytecode_t *bc2 = nullptr;
    ASSERT_EQ(bcode_deserial(a, blob, n, &bc2), 0);
    ASSERT_NE(bc2, nullptr);

    strslice_t s = bcode_str_at(bc2, 0);
    ASSERT_EQ(s.len, sizeof tricky);
    EXPECT_EQ(std::memcmp(s.ptr, tricky, sizeof tricky), 0);

    allocator_free(a, (void **)&blob);
    bcode_destroy(&bc);
    bcode_destroy(&bc2);
    EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}

/* 文件写读往返：bcode_serial → bcode_deserial_from_file。 */
TEST(BcodeSerial, FileRoundTrip) {
    allocator_t *a = create_allocator(malloc, free);

    std::string path =
        (clux_test_temp_dir() + "/" + ("clux_serial_test.cxb"));

    bytecode_t *bc = make_sample(a);
    ASSERT_EQ(bcode_serial(bc, path.c_str()), 0);

    bytecode_t *bc2 = nullptr;
    ASSERT_EQ(bcode_deserial_from_file(a, path.c_str(), &bc2), 0);
    ASSERT_NE(bc2, nullptr);

    /* 反汇编文本应一致（间接比对 code 与 strtable） */
    char *t1 = bcode_disasm_mem(a, bc, NULL);
    char *t2 = bcode_disasm_mem(a, bc2, NULL);
    ASSERT_NE(t1, nullptr);
    ASSERT_NE(t2, nullptr);
    EXPECT_STREQ(t1, t2);

    allocator_free(a, (void **)&t1);
    allocator_free(a, (void **)&t2);
    bcode_destroy(&bc);
    bcode_destroy(&bc2);
    EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
    std::remove(path.c_str());
}

/* 坏输入应被拒绝（不崩溃，非零返回，*out_bc 置 NULL）。 */
TEST(BcodeSerial, RejectsCorruptInput) {
    allocator_t *a = create_allocator(malloc, free);
    bytecode_t *bc = nullptr;

    /* 坏 magic */
    const uint8_t bad_magic[] = {'X', 'X', 'X', 'X', 1, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_NE(bcode_deserial(a, bad_magic, sizeof bad_magic, &bc), 0);
    EXPECT_EQ(bc, nullptr);

    /* 版本不匹配 */
    const uint8_t bad_ver[] = {'C', 'X', 'B', 'C', 99, 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_NE(bcode_deserial(a, bad_ver, sizeof bad_ver, &bc), 0);
    EXPECT_EQ(bc, nullptr);

    /* 声明过长 code（截断） */
    const uint8_t bad_len[] = {'C', 'X', 'B', 'C', 1, 0, 0, 0, /* version */
                               0, 0, 0, 0,                      /* str_count=0 */
                               0xFF, 0, 0, 0, 0, 0, 0, 0};      /* code_len=255 */
    EXPECT_NE(bcode_deserial(a, bad_len, sizeof bad_len, &bc), 0);
    EXPECT_EQ(bc, nullptr);

    /* 文件不存在 */
    EXPECT_NE(bcode_deserial_from_file(a, "no_such_file_12345.cxb", &bc), 0);

    EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
}
