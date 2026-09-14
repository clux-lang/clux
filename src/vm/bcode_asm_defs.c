#include "vm/bcode_asm_defs.h"

/* 解码表按 opcode 枚举值索引，新增/改名 opcode 时只改此表。
 * 反汇编器与汇编器共享此表（单一事实源），保证助记符/操作数布局完全一致。 */
const bcode_asm_entry_t BCODE_ASM_TABLE[] = {
    [BCODE_PUSH]           = { "PUSH",           { BCODE_ASM_OP_STR } },
    [BCODE_STORE]          = { "STORE",          { BCODE_ASM_OP_STR } },
    [BCODE_PUSH_STR]       = { "PUSH_STRING",    { BCODE_ASM_OP_STR } },

    [BCODE_PUSH_I8]        = { "PUSH_I8",        { BCODE_ASM_OP_I8 } },
    [BCODE_PUSH_I16]       = { "PUSH_I16",       { BCODE_ASM_OP_I16 } },
    [BCODE_PUSH_I32]       = { "PUSH_I32",       { BCODE_ASM_OP_I32 } },
    [BCODE_PUSH_I64]       = { "PUSH_I64",       { BCODE_ASM_OP_I64 } },
    [BCODE_PUSH_U8]        = { "PUSH_U8",        { BCODE_ASM_OP_U8 } },
    [BCODE_PUSH_U16]       = { "PUSH_U16",       { BCODE_ASM_OP_U16 } },
    [BCODE_PUSH_U32]       = { "PUSH_U32",       { BCODE_ASM_OP_U32 } },
    [BCODE_PUSH_U64]       = { "PUSH_U64",       { BCODE_ASM_OP_U64 } },
    [BCODE_PUSH_F32]       = { "PUSH_F32",       { BCODE_ASM_OP_F32 } },
    [BCODE_PUSH_F64]       = { "PUSH_F64",       { BCODE_ASM_OP_F64 } },
    [BCODE_PUSH_BOOL]      = { "PUSH_BOOL",      { BCODE_ASM_OP_BOOL } },

    [BCODE_PUSH_VALUE]     = { "PUSH_VALUE",     { BCODE_ASM_OP_U32 } },
    [BCODE_LOAD]           = { "LOAD",           { BCODE_ASM_OP_STR } },
    [BCODE_LOAD_TYPE]      = { "LOAD_TYPE",      { BCODE_ASM_OP_U32 } },
    [BCODE_BIND_TYPE]      = { "BIND_TYPE",      { BCODE_ASM_OP_U32 } },
    [BCODE_SET_TYPE_NAME]  = { "SET_TYPE_NAME",  { BCODE_ASM_OP_STR } },
    [BCODE_PUSH_UNDEFINED] = { "PUSH_UNDEFINED", { BCODE_ASM_OP_NONE } },

    [BCODE_DEFINE]         = { "DEFINE",         { BCODE_ASM_OP_STR } },
    [BCODE_PUSH_FUNC_TYPE] = { "PUSH_FUNC_TYPE", { BCODE_ASM_OP_NONE } },
    [BCODE_FUNC_TYPE_PARAM]= { "FUNC_TYPE_PARAM",{ BCODE_ASM_OP_NONE } },
    [BCODE_FUNC_TYPE_RETURN]={ "FUNC_TYPE_RETURN",{BCODE_ASM_OP_NONE } },
    [BCODE_FUNC_TYPE_VARARG]= { "FUNC_TYPE_VARARG",{ BCODE_ASM_OP_NONE } },
    [BCODE_SEAL]           = { "SEAL",           { BCODE_ASM_OP_NONE } },
    [BCODE_PUSH_FUNCTION]  = { "PUSH_FUNCTION",  { BCODE_ASM_OP_U32, BCODE_ASM_OP_U32 } },
    [BCODE_BIND_FUNC]      = { "BIND_FUNC",      { BCODE_ASM_OP_U32 } },
    [BCODE_SET_FUNC_NAME]  = { "SET_FUNC_NAME",  { BCODE_ASM_OP_STR } },

    [BCODE_ADD]            = { "ADD",            { BCODE_ASM_OP_NONE } },
    [BCODE_SUB]            = { "SUB",            { BCODE_ASM_OP_NONE } },
    [BCODE_MUL]            = { "MUL",            { BCODE_ASM_OP_NONE } },
    [BCODE_DIV]            = { "DIV",            { BCODE_ASM_OP_NONE } },
    [BCODE_MOD]            = { "MOD",            { BCODE_ASM_OP_NONE } },
    [BCODE_EQ]             = { "EQ",             { BCODE_ASM_OP_NONE } },
    [BCODE_NE]             = { "NE",             { BCODE_ASM_OP_NONE } },
    [BCODE_LT]             = { "LT",             { BCODE_ASM_OP_NONE } },
    [BCODE_LE]             = { "LE",             { BCODE_ASM_OP_NONE } },
    [BCODE_GT]             = { "GT",             { BCODE_ASM_OP_NONE } },
    [BCODE_GE]             = { "GE",             { BCODE_ASM_OP_NONE } },
    [BCODE_AND]            = { "AND",            { BCODE_ASM_OP_NONE } },
    [BCODE_OR]             = { "OR",             { BCODE_ASM_OP_NONE } },
    [BCODE_BXOR]           = { "BXOR",           { BCODE_ASM_OP_NONE } },
    [BCODE_SHL]            = { "SHL",            { BCODE_ASM_OP_NONE } },
    [BCODE_SHR]            = { "SHR",            { BCODE_ASM_OP_NONE } },
    [BCODE_NEG]            = { "NEG",            { BCODE_ASM_OP_NONE } },
    [BCODE_NOT]            = { "NOT",            { BCODE_ASM_OP_NONE } },
    [BCODE_BNOT]           = { "BNOT",           { BCODE_ASM_OP_NONE } },

    [BCODE_CAST]           = { "CAST",           { BCODE_ASM_OP_NONE } },
    [BCODE_CREATE_CONST]   = { "CREATE_CONST",   { BCODE_ASM_OP_NONE } },
    [BCODE_CREATE_VOLATILE]= { "CREATE_VOLATILE",{ BCODE_ASM_OP_NONE } },
    [BCODE_CALL]           = { "CALL",           { BCODE_ASM_OP_U32 } },
    [BCODE_RET]            = { "RET",            { BCODE_ASM_OP_NONE } },

    [BCODE_JMP]            = { "JMP",            { BCODE_ASM_OP_U32 } },
    [BCODE_JZ]             = { "JZ",             { BCODE_ASM_OP_U32 } },
    [BCODE_JNZ]            = { "JNZ",            { BCODE_ASM_OP_U32 } },

    [BCODE_PUSH_SCOPE]     = { "PUSH_SCOPE",     { BCODE_ASM_OP_NONE } },
    [BCODE_POP_SCOPE]      = { "POP_SCOPE",      { BCODE_ASM_OP_NONE } },
    [BCODE_POP]            = { "POP",            { BCODE_ASM_OP_NONE } },
    [BCODE_HALT]           = { "HALT",           { BCODE_ASM_OP_NONE } },

    [BCODE_PUSH_ARRAY]     = { "PUSH_ARRAY",     { BCODE_ASM_OP_NONE } },
    [BCODE_DEFINE_BOUND]   = { "DEFINE_BOUND",   { BCODE_ASM_OP_U32 } },

    [BCODE_CONSTRUCT]      = { "CONSTRUCT",      { BCODE_ASM_OP_U32 } },
    [BCODE_INDEX_GET]      = { "INDEX_GET",     { BCODE_ASM_OP_NONE } },
    [BCODE_INDEX_SET]      = { "INDEX_SET",     { BCODE_ASM_OP_NONE } },
    [BCODE_LENGTH]         = { "LENGTH",        { BCODE_ASM_OP_NONE } },
};

const size_t BCODE_ASM_TABLE_COUNT =
    sizeof(BCODE_ASM_TABLE) / sizeof(BCODE_ASM_TABLE[0]);
