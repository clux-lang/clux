#include "vm/bcode_disasm.h"
#include "vm/bcode.h"
#include "vm/bcode_asm_defs.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ================================================================ */
/* 抽象输出槽（FILE* 与内存缓冲共用同一反汇编循环）                   */
/* ================================================================ */

typedef struct {
    void *ctx;
    void (*emit)(void *ctx, const char *data, size_t n);
} disasm_sink_t;

/* ---- 字符串 C 风格转义写入（经 sink） ---- */

static void disasm_emit_escaped(disasm_sink_t *out, strslice_t s) {
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.ptr[i];
        switch (c) {
            case '"':  { char b[2] = { '\\', '"' };  out->emit(out->ctx, b, 2); } break;
            case '\\': { char b[2] = { '\\', '\\' }; out->emit(out->ctx, b, 2); } break;
            case '\n': { char b[2] = { '\\', 'n' };  out->emit(out->ctx, b, 2); } break;
            case '\t': { char b[2] = { '\\', 't' };  out->emit(out->ctx, b, 2); } break;
            case '\r': { char b[2] = { '\\', 'r' };  out->emit(out->ctx, b, 2); } break;
            default:
                if (c < 0x20) {
                    char buf[5];
                    int n = snprintf(buf, sizeof buf, "\\x%02X", c);
                    out->emit(out->ctx, buf, (size_t)n);
                } else {
                    out->emit(out->ctx, (const char *)&c, 1);
                }
                break;
        }
    }
}

/* ---- 标签支持：跳转/函数入口目标 pc 收集为 L0/L1/... ---- */

/* 该 opcode 的 u32 操作数是否为代码地址（可标签化：jmp/jz/jnz/push_function）。
   仅第一个操作数可标签化（PUSH_FUNCTION 第二操作数是 id，非地址）。 */
static bool op_is_label_addr(bcode_op_t op) {
    return op == BCODE_JMP || op == BCODE_JZ ||
           op == BCODE_JNZ || op == BCODE_PUSH_FUNCTION;
}

/* 收集标签集合：既作为地址型操作数目标、又恰好落在指令边界的 pc。
 * 即以"跳转目标/函数入口"为锚生成标签名（L0/L1… 按 pc 升序）。 */
static void collect_labels(const bytecode_t *bc, vec_t *labels) {
    vec_t *bounds  = vec_new(bc->alloc, /*owns_element=*/true);
    vec_t *targets = vec_new(bc->alloc, /*owns_element=*/true);
    size_t pc = 0;
    while (pc < bc->code.len) {
        uint32_t *bp = (uint32_t *)allocator_new_ex(
            bc->alloc, "clux.vm.disasm.bound", sizeof(uint32_t), NULL, NULL, NULL, 1);
        *bp = (uint32_t)pc;
        vec_push(bounds, bc->alloc, bp);

        bcode_op_t op = bcode_read_op(bc, &pc);
        if ((size_t)op >= BCODE_ASM_TABLE_COUNT || !BCODE_ASM_TABLE[op].mnemonic) {
            break; /* 损坏：无法自同步，停止 */
        }
        for (size_t k = 0; k < 4; k++) {
            bcode_asm_operand_t kind = BCODE_ASM_TABLE[op].operands[k];
            if (kind == BCODE_ASM_OP_NONE) break;
            if (kind == BCODE_ASM_OP_STR) {
                bcode_read_u32(bc, &pc);
            } else if (kind == BCODE_ASM_OP_U32) {
                uint32_t v = bcode_read_u32(bc, &pc);
                if (op_is_label_addr(op) && k == 0) {
                    uint32_t *tp = (uint32_t *)allocator_new_ex(
                        bc->alloc, "clux.vm.disasm.tgt", sizeof(uint32_t),
                        NULL, NULL, NULL, 1);
                    *tp = v;
                    vec_push(targets, bc->alloc, tp);
                }
            } else {
                switch (kind) {
                    case BCODE_ASM_OP_I8:  bcode_read_i8(bc, &pc);  break;
                    case BCODE_ASM_OP_I16: bcode_read_i16(bc, &pc); break;
                    case BCODE_ASM_OP_I32: bcode_read_i32(bc, &pc); break;
                    case BCODE_ASM_OP_I64: bcode_read_i64(bc, &pc); break;
                    case BCODE_ASM_OP_U8:  bcode_read_u8(bc, &pc);  break;
                    case BCODE_ASM_OP_U16: bcode_read_u16(bc, &pc); break;
                    case BCODE_ASM_OP_U64: bcode_read_u64(bc, &pc); break;
                    case BCODE_ASM_OP_F32: bcode_read_f32(bc, &pc); break;
                    case BCODE_ASM_OP_F64: bcode_read_f64(bc, &pc); break;
                    case BCODE_ASM_OP_BOOL: bcode_read_bool(bc, &pc); break;
                    default: break;
                }
            }
        }
    }
    /* labels = targets ∩ bounds，去重 */
    for (size_t i = 0; i < vec_len(targets); i++) {
        uint32_t t = *(uint32_t *)vec_get(targets, i);
        bool is_bound = false, dup = false;
        for (size_t j = 0; j < vec_len(bounds); j++)
            if (*(uint32_t *)vec_get(bounds, j) == t) { is_bound = true; break; }
        if (!is_bound) continue;
        for (size_t j = 0; j < vec_len(labels); j++)
            if (*(uint32_t *)vec_get(labels, j) == t) { dup = true; break; }
        if (!dup) {
            uint32_t *lp = (uint32_t *)allocator_new_ex(
                bc->alloc, "clux.vm.disasm.label", sizeof(uint32_t),
                NULL, NULL, NULL, 1);
            *lp = t;
            vec_push(labels, bc->alloc, lp);
        }
    }
    /* 升序排序：确定性命名（L0 对应最小 pc）+ 二分查找 */
    for (size_t i = 1; i < vec_len(labels); i++) {
        size_t j = i;
        while (j > 0) {
            uint32_t prev = *(uint32_t *)vec_get(labels, j - 1);
            uint32_t cur  = *(uint32_t *)vec_get(labels, j);
            if (prev <= cur) break;
            void *tmp = vec_get(labels, j);
            vec_set(labels, j, vec_get(labels, j - 1));
            vec_set(labels, j - 1, tmp);
            j--;
        }
    }
    vec_free(bc->alloc, &bounds);
    vec_free(bc->alloc, &targets);
}

/* 在已排序的 label 集合中二分查找 pc，返回索引或 -1 */
static long label_index(vec_t *labels, uint32_t pc) {
    size_t lo = 0, hi = vec_len(labels);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t v = *(uint32_t *)vec_get(labels, mid);
        if (v == pc) return (long)mid;
        else if (v < pc) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

/* ---- 反汇编主循环（与具体输出后端无关） ---- */

static void disasm_to(const bytecode_t *bc, disasm_sink_t *out) {
    /* ---- code 段：线性自同步反汇编 ----
       字符串表为编译器内部概念，不在文本暴露；字符串操作数直接内联为
       "..." 字面量。跳转/函数入口目标以标签 L0/L1… 表示，引用用 [Lx]
       与裸数字地址区分（汇编器据此区分标签引用与直接地址）。 */
    vec_t *labels = vec_new(bc->alloc, /*owns_element=*/true);
    collect_labels(bc, labels);

    size_t pc = 0;
    while (pc < bc->code.len) {
        /* 标签行：当前 pc 是某跳转/函数入口目标 */
        long lidx = label_index(labels, (uint32_t)pc);
        if (lidx >= 0) {
            char buf[32];
            int n = snprintf(buf, sizeof buf, "L%ld:\n", lidx);
            out->emit(out->ctx, buf, (size_t)n);
        }

        bcode_op_t op = bcode_read_op(bc, &pc);
        if ((size_t)op >= BCODE_ASM_TABLE_COUNT || !BCODE_ASM_TABLE[op].mnemonic) {
            /* 未知 opcode：原样输出数值，跳过（防御性） */
            char buf[32];
            int n = snprintf(buf, sizeof buf, ".byte %u\n", (unsigned)op);
            out->emit(out->ctx, buf, (size_t)n);
            continue;
        }
        out->emit(out->ctx, BCODE_ASM_TABLE[op].mnemonic,
                  strlen(BCODE_ASM_TABLE[op].mnemonic));
        for (size_t k = 0; k < 4; k++) {
            bcode_asm_operand_t kind = BCODE_ASM_TABLE[op].operands[k];
            if (kind == BCODE_ASM_OP_NONE) break;
            char sp = ' ';
            out->emit(out->ctx, &sp, 1);
            switch (kind) {
                case BCODE_ASM_OP_STR: {
                    uint32_t idx = bcode_read_u32(bc, &pc);
                    out->emit(out->ctx, "\"", 1);
                    disasm_emit_escaped(out, bcode_str_at(bc, idx));
                    out->emit(out->ctx, "\"", 1);
                    break;
                }
                case BCODE_ASM_OP_U32: {
                    uint32_t v = bcode_read_u32(bc, &pc);
                    if (op_is_label_addr(op) && k == 0) {
                        long li = label_index(labels, v);
                        if (li >= 0) {
                            char buf[32];
                            int n = snprintf(buf, sizeof buf, "[L%ld]", li);
                            out->emit(out->ctx, buf, (size_t)n);
                            break;
                        }
                    }
                    char buf[32];
                    int n = snprintf(buf, sizeof buf, "%u", (unsigned)v);
                    out->emit(out->ctx, buf, (size_t)n);
                    break;
                }
                case BCODE_ASM_OP_I8: {
                    char buf[32];
                    int n = snprintf(buf, sizeof buf, "%d",
                                     (int)bcode_read_i8(bc, &pc));
                    out->emit(out->ctx, buf, (size_t)n);
                    break;
                }
                case BCODE_ASM_OP_I16: {
                    char buf[32];
                    int n = snprintf(buf, sizeof buf, "%d",
                                     (int)bcode_read_i16(bc, &pc));
                    out->emit(out->ctx, buf, (size_t)n);
                    break;
                }
                case BCODE_ASM_OP_I32: {
                    char buf[32];
                    int n = snprintf(buf, sizeof buf, "%d",
                                     (int)bcode_read_i32(bc, &pc));
                    out->emit(out->ctx, buf, (size_t)n);
                    break;
                }
                case BCODE_ASM_OP_I64: {
                    char buf[40];
                    int n = snprintf(buf, sizeof buf, "%lld",
                                     (long long)bcode_read_i64(bc, &pc));
                    out->emit(out->ctx, buf, (size_t)n);
                    break;
                }
                case BCODE_ASM_OP_U8: {
                    char buf[32];
                    int n = snprintf(buf, sizeof buf, "%u",
                                     (unsigned)bcode_read_u8(bc, &pc));
                    out->emit(out->ctx, buf, (size_t)n);
                    break;
                }
                case BCODE_ASM_OP_U16: {
                    char buf[32];
                    int n = snprintf(buf, sizeof buf, "%u",
                                     (unsigned)bcode_read_u16(bc, &pc));
                    out->emit(out->ctx, buf, (size_t)n);
                    break;
                }
                case BCODE_ASM_OP_U64: {
                    char buf[40];
                    int n = snprintf(buf, sizeof buf, "%llu",
                                     (unsigned long long)bcode_read_u64(bc, &pc));
                    out->emit(out->ctx, buf, (size_t)n);
                    break;
                }
                case BCODE_ASM_OP_F32: {
                    char buf[40];
                    int n = snprintf(buf, sizeof buf, "%.9g",
                                     (double)bcode_read_f32(bc, &pc));
                    out->emit(out->ctx, buf, (size_t)n);
                    break;
                }
                case BCODE_ASM_OP_F64: {
                    char buf[40];
                    int n = snprintf(buf, sizeof buf, "%.17g",
                                     (double)bcode_read_f64(bc, &pc));
                    out->emit(out->ctx, buf, (size_t)n);
                    break;
                }
                case BCODE_ASM_OP_BOOL: {
                    char buf[8];
                    int n = snprintf(buf, sizeof buf, "%d",
                                     bcode_read_bool(bc, &pc) ? 1 : 0);
                    out->emit(out->ctx, buf, (size_t)n);
                    break;
                }
                default:
                    break;
            }
        }
        out->emit(out->ctx, "\n", 1);
    }

    vec_free(bc->alloc, &labels);
}

/* ---- FILE* 后端 ---- */

static void file_emit(void *ctx, const char *data, size_t n) {
    fwrite(data, 1, n, (FILE *)ctx);
}

int bcode_disasm(const bytecode_t *bc, const char *out_path) {
    if (!bc || !out_path) return -1;

    FILE *out = fopen(out_path, "wb");
    if (!out) return -1;

    disasm_sink_t sink = { .ctx = out, .emit = file_emit };
    disasm_to(bc, &sink);

    if (fclose(out) != 0) return -1;
    return 0;
}

/* ---- 内存后端（NUL 结尾，alloc 管理，alloc 存活期间有效） ---- */

typedef struct {
    allocator_t *a;
    char        *buf;
    size_t       len;
    size_t       cap;
} mem_sink_t;

static void mem_emit(void *ctx, const char *data, size_t n) {
    mem_sink_t *m = (mem_sink_t *)ctx;
    if (m->len + n + 1 > m->cap) {
        size_t nc = m->cap ? m->cap : 256;
        while (nc < m->len + n + 1) nc *= 2;
        char *nb = (char *)allocator_new_ex(
            m->a, "clux.vm.disasm.buf", nc, NULL, NULL, NULL, 1);
        if (m->buf) {
            memcpy(nb, m->buf, m->len);
            allocator_free(m->a, (void **)&m->buf);
        }
        m->buf = nb;
        m->cap = nc;
    }
    memcpy(m->buf + m->len, data, n);
    m->len += n;
}

char *bcode_disasm_mem(allocator_t *alloc, const bytecode_t *bc, size_t *out_len) {
    if (!alloc || !bc) return NULL;

    mem_sink_t m;
    m.a = alloc;
    m.buf = NULL;
    m.len = 0;
    m.cap = 0;

    disasm_sink_t sink = { .ctx = &m, .emit = mem_emit };
    disasm_to(bc, &sink);

    /* 确保 NUL 结尾 */
    if (m.len + 1 > m.cap) {
        size_t nc = m.cap ? m.cap : 1;
        while (nc < m.len + 1) nc *= 2;
        char *nb = (char *)allocator_new_ex(
            m.a, "clux.vm.disasm.buf", nc, NULL, NULL, NULL, 1);
        if (m.buf) {
            memcpy(nb, m.buf, m.len);
            allocator_free(m.a, (void **)&m.buf);
        }
        m.buf = nb;
        m.cap = nc;
    }
    m.buf[m.len] = '\0';

    if (out_len) *out_len = m.len;
    return m.buf;
}
