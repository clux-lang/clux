#include "vm/bcode.h"
#include "vm/bcode_asm.h"
#include "vm/bcode_disasm.h"

#include "core/allocator.h"
#include "core/strslice.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

static bytecode_t *make_sample(allocator_t *a) {
    bytecode_t *bc = bcode_new(a);
    /* strtable: 0="printf", 1="hello world\n" */
    (void)bcode_str_index(bc, strslice_from_cstr("printf"));
    (void)bcode_str_index(bc, strslice_from_cstr("hello world\n"));

    /* code: PUSH_STRING 1 ; CALL 1 ; HALT */
    bcode_write_op(bc, BCODE_PUSH_STR);
    bcode_write_u32(bc, 1);
    bcode_write_op(bc, BCODE_CALL);
    bcode_write_u32(bc, 1);
    bcode_write_op(bc, BCODE_HALT);
    return bc;
}

/* disasm → asm → disasm 往返后文本应逐字节一致（反汇编为规范形式）。 */
TEST(BcodeAsm, RoundTripText) {
    allocator_t *a = create_allocator(malloc, free);

    bytecode_t *bc = make_sample(a);
    char *t1 = bcode_disasm_mem(a, bc, NULL);
    ASSERT_NE(t1, nullptr);

    bytecode_t *bc2 = nullptr;
    EXPECT_EQ(bcode_asm_parse(a, t1, std::strlen(t1), &bc2), 0);
    ASSERT_NE(bc2, nullptr);

    char *t2 = bcode_disasm_mem(a, bc2, NULL);
    ASSERT_NE(t2, nullptr);

    EXPECT_STREQ(t1, t2);

    allocator_free(a, (void **)&t1);
    allocator_free(a, (void **)&t2);
    bcode_destroy(&bc);
    bcode_destroy(&bc2);
    delete_allocator(&a);
}

/* 覆盖带操作数的各类指令，验证变长/立即数往返（含浮点、跳转、算术）。 */
TEST(BcodeAsm, RoundTripOperands) {
    allocator_t *a = create_allocator(malloc, free);

    bytecode_t *bc = bcode_new(a);
    (void)bcode_str_index(bc, strslice_from_cstr("x"));

    bcode_write_op(bc, BCODE_PUSH_I8);   bcode_write_i8(bc, -12);
    bcode_write_op(bc, BCODE_PUSH_I32);  bcode_write_i32(bc, -123456);
    bcode_write_op(bc, BCODE_PUSH_U64);  bcode_write_u64(bc, 0xDEADBEEFULL);
    bcode_write_op(bc, BCODE_PUSH_F64);  bcode_write_f64(bc, 3.141592653589793);
    bcode_write_op(bc, BCODE_PUSH_BOOL); bcode_write_bool(bc, true);
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_PUSH_NIL);
    bcode_write_op(bc, BCODE_ADD);
    bcode_write_op(bc, BCODE_JMP);       bcode_write_u32(bc, 99);
    bcode_write_op(bc, BCODE_LOAD);      bcode_write_u32(bc, 0);
    bcode_write_op(bc, BCODE_HALT);

    char *t1 = bcode_disasm_mem(a, bc, NULL);
    ASSERT_NE(t1, nullptr);

    bytecode_t *bc2 = nullptr;
    EXPECT_EQ(bcode_asm_parse(a, t1, std::strlen(t1), &bc2), 0);

    char *t2 = bcode_disasm_mem(a, bc2, NULL);
    EXPECT_STREQ(t1, t2);

    allocator_free(a, (void **)&t1);
    allocator_free(a, (void **)&t2);
    bcode_destroy(&bc);
    bcode_destroy(&bc2);
    delete_allocator(&a);
}

/* 字符串 C 风格转义（" \ 换行等）内联往返后字节应完全一致。 */
TEST(BcodeAsm, StringEscapeRoundTrip) {
    allocator_t *a = create_allocator(malloc, free);

    const char *orig = "a\"b\\c\nd\te";
    bytecode_t *bc = bcode_new(a);
    bcode_str_index(bc, strslice_from_cstr(orig)); /* index 0 */
    bcode_write_op(bc, BCODE_PUSH_STR);
    bcode_write_u32(bc, 0);
    bcode_write_op(bc, BCODE_HALT);

    char *t1 = bcode_disasm_mem(a, bc, NULL);
    ASSERT_NE(t1, nullptr);

    bytecode_t *bc2 = nullptr;
    ASSERT_EQ(bcode_asm_parse(a, t1, std::strlen(t1), &bc2), 0);

    strslice_t got = bcode_str_at(bc2, 0);
    EXPECT_EQ(std::string(got.ptr, got.len), std::string(orig));

    allocator_free(a, (void **)&t1);
    bcode_destroy(&bc);
    bcode_destroy(&bc2);
    delete_allocator(&a);
}

/* 非法输入应被拒绝（非零返回，不崩溃）。 */
TEST(BcodeAsm, ParseErrors) {
    allocator_t *a = create_allocator(malloc, free);
    bytecode_t *bc = nullptr;

    /* 缺少段头 */
    EXPECT_NE(bcode_asm_parse(a, "LOAD 0\n", 7, &bc), 0);

    /* 未知助记符 */
    const char *bad_mn = "[.section code]\nFOOBAR 1\n";
    EXPECT_NE(bcode_asm_parse(a, bad_mn, std::strlen(bad_mn), &bc), 0);

    /* 操作数格式错误 */
    const char *bad_op = "[.section code]\nPUSH_I32 abc\n";
    EXPECT_NE(bcode_asm_parse(a, bad_op, std::strlen(bad_op), &bc), 0);

    /* 操作数数量不足 */
    const char *few_op = "[.section code]\nCALL\n";
    EXPECT_NE(bcode_asm_parse(a, few_op, std::strlen(few_op), &bc), 0);

    /* 文件无法打开 */
    EXPECT_NE(bcode_asm_from_file(a, "this_file_does_not_exist_12345.cxs", &bc), 0);

    delete_allocator(&a);
}

/* 指令助记符大小写无关：小写/混合大小写输入应被正确识别。 */
TEST(BcodeAsm, CaseInsensitiveMnemonics) {
    allocator_t *a = create_allocator(malloc, free);

    /* 全小写 + 混合大小写的助记符与 .byte 伪指令 */
    const char *src =
        "push \"x\"\n"
        "push_i32 42\n"
        "Store \"x\"\n"
        "ADD\n"
        "Ret\n"
        ".BYTE 99\n"
        "HALT\n";
    bytecode_t *bc = nullptr;
    EXPECT_EQ(bcode_asm_parse(a, src, std::strlen(src), &bc), 0);
    ASSERT_NE(bc, nullptr);

    /* 反汇编为规范大写形式，应能被再次识别（往返稳定） */
    char *t = bcode_disasm_mem(a, bc, NULL);
    ASSERT_NE(t, nullptr);
    bytecode_t *bc2 = nullptr;
    EXPECT_EQ(bcode_asm_parse(a, t, std::strlen(t), &bc2), 0);

    allocator_free(a, (void **)&t);
    bcode_destroy(&bc);
    bcode_destroy(&bc2);
    delete_allocator(&a);
}

/* 标签定义 + 前向引用（jmp 时标签尚未定义，应回填为实际 pc）。 */
TEST(BcodeAsm, LabelsAndForwardReferences) {
    allocator_t *a = create_allocator(malloc, free);

    const char *src =
        "push_i32 1\n"
        "jmp [end]\n"   /* 前向引用 */
        "push_i32 2\n"
        "end:\n"
        "push_i32 3\n"
        "add\n"
        "halt\n";
    bytecode_t *bc = nullptr;
    EXPECT_EQ(bcode_asm_parse(a, src, std::strlen(src), &bc), 0);
    ASSERT_NE(bc, nullptr);

    /* 校验 jmp 操作数被回填为 'end' 的实际 pc（push_i32 3 在 pc=24） */
    size_t pc = 0;
    bool found = false;
    while (pc < bc->code.len) {
        bcode_op_t op = bcode_read_op(bc, &pc);
        if (op == BCODE_PUSH_I32)       bcode_read_i32(bc, &pc);
        else if (op == BCODE_JMP) {
            uint32_t v = bcode_read_u32(bc, &pc);
            EXPECT_EQ(v, 24u);
            found = true;
        } else if (op == BCODE_ADD || op == BCODE_HALT) {
            /* 无操作数 */
        } else {
            FAIL() << "unexpected opcode in label test";
        }
    }
    EXPECT_TRUE(found);

    bcode_destroy(&bc);
    delete_allocator(&a);
}

/* 未定义标签引用应被拒绝（非零返回）。 */
TEST(BcodeAsm, UndefinedLabelIsError) {
    allocator_t *a = create_allocator(malloc, free);
    bytecode_t *bc = nullptr;
    const char *src = "jmp [nofn]\nHALT\n";
    EXPECT_NE(bcode_asm_parse(a, src, std::strlen(src), &bc), 0);
    delete_allocator(&a);
}

/* LOAD_FUNCTION <id> 助记符往返：反汇编 → 汇编 → 反汇编字节稳定 */
TEST(BcodeAsm, LoadFunctionRoundTrip) {
    allocator_t *a = create_allocator(malloc, free);

    bytecode_t *bc = bcode_new(a);
    bcode_write_op(bc, BCODE_LOAD_FUNCTION); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_HALT);

    char *t1 = bcode_disasm_mem(a, bc, NULL);
    ASSERT_NE(t1, nullptr);
    EXPECT_NE(std::strstr(t1, "LOAD_FUNCTION 64"), nullptr);

    bytecode_t *bc2 = nullptr;
    EXPECT_EQ(bcode_asm_parse(a, t1, std::strlen(t1), &bc2), 0);
    ASSERT_NE(bc2, nullptr);

    char *t2 = bcode_disasm_mem(a, bc2, NULL);
    ASSERT_NE(t2, nullptr);
    EXPECT_STREQ(t1, t2);

    allocator_free(a, (void **)&t1);
    allocator_free(a, (void **)&t2);
    bcode_destroy(&bc);
    bcode_destroy(&bc2);
    delete_allocator(&a);
}
