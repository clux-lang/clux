#include "vm/vm.h"
#include "vm/value.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "vm/exec.h"
#include "exec_internal.h"

#include <stdint.h>
#include <string.h>

static uint64_t mask_width(uint32_t width) {
    return width >= 64 ? UINT64_MAX : ((UINT64_C(1) << width) - 1);
}

static void load_word(const uint8_t *data, uint32_t word_bits, uint32_t idx,
                      uint64_t *out) {
    size_t ws = word_bits / 8;
    uint64_t w = 0;
    memcpy(&w, data + (size_t)idx * ws, ws);
    *out = w;
}

static void store_word(uint8_t *data, uint32_t word_bits, uint32_t idx,
                       uint64_t w) {
    size_t ws = word_bits / 8;
    memcpy(data + (size_t)idx * ws, &w, ws);
}

static uint64_t bitfield_read(const uint8_t *data, uint32_t word_bits,
                              uint32_t offset, uint32_t width) {
    uint32_t idx = offset / word_bits;
    uint32_t sh = offset % word_bits;
    uint64_t lo = 0, hi = 0;
    load_word(data, word_bits, idx, &lo);
    uint64_t v = sh ? (lo >> sh) : lo;
    if (sh + width > word_bits) {
        load_word(data, word_bits, idx + 1, &hi);
        v |= hi << (word_bits - sh);
    }
    return v & mask_width(width);
}

static void bitfield_write(uint8_t *data, uint32_t word_bits, uint32_t offset,
                           uint32_t width, uint64_t value) {
    value &= mask_width(width);
    uint32_t idx = offset / word_bits;
    uint32_t sh = offset % word_bits;
    uint64_t lo = 0;
    load_word(data, word_bits, idx, &lo);
    uint64_t lo_mask = mask_width(width < word_bits - sh ? width : word_bits - sh)
                       << sh;
    lo = (lo & ~lo_mask) | ((value << sh) & lo_mask);
    store_word(data, word_bits, idx, lo);
    if (sh + width > word_bits) {
        uint32_t rest = sh + width - word_bits;
        uint64_t hi = 0;
        load_word(data, word_bits, idx + 1, &hi);
        uint64_t hi_mask = mask_width(rest);
        hi = (hi & ~hi_mask) | ((value >> (word_bits - sh)) & hi_mask);
        store_word(data, word_bits, idx + 1, hi);
    }
}

value_t *op_bitfield_get(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t type_id = bcode_read_u32(bc, pc);
    uint32_t offset = bcode_read_u32(bc, pc);
    uint32_t width = bcode_read_u32(bc, pc);
    uint32_t word_bits = bcode_read_u32(bc, pc);
    const type_t *vt = vm_type_load(vm, type_id);
    value_t *self = exec_stack_pop(vm);
    if (!vt || !self || !value_data(self))
        return value_make_error(vm, "exec: bitfield get: invalid value");
    uint64_t raw = bitfield_read((const uint8_t *)value_data(self), word_bits,
                                 offset, width);
    void *data = value_alloc_data(vm->alloc, vt);
    if (!data) return value_make_error(vm, "exec: bitfield get: out of memory");
    memcpy(data, &raw, vt->size < sizeof(raw) ? vt->size : sizeof(raw));
    return value_make(vm, vt, data);
}

value_t *op_bitfield_set(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t type_id = bcode_read_u32(bc, pc);
    uint32_t offset = bcode_read_u32(bc, pc);
    uint32_t width = bcode_read_u32(bc, pc);
    uint32_t word_bits = bcode_read_u32(bc, pc);
    (void)type_id;
    value_t *val = exec_stack_pop(vm);
    value_t *self = exec_stack_pop(vm);
    if (!val || !self || !value_data(self) || !value_data(val))
        return value_make_error(vm, "exec: bitfield set: invalid value");
    uint64_t raw = 0;
    memcpy(&raw, value_data(val), value_type(val)->size < sizeof(raw)
                                      ? value_type(val)->size
                                      : sizeof(raw));
    bitfield_write((uint8_t *)value_data(self), word_bits, offset, width, raw);
    return self;
}

value_t *op_bitfield_make(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t type_id = bcode_read_u32(bc, pc); /* storage array type */
    uint32_t word_bits = bcode_read_u32(bc, pc);
    uint32_t word_count = bcode_read_u32(bc, pc);
    uint32_t field_count = bcode_read_u32(bc, pc);
    const type_t *st = vm_type_load(vm, type_id);
    if (!st) return value_make_error(vm, "exec: bitfield make: unknown type");
    uint32_t offsets[128], widths[128];
    if (field_count > 128)
        return value_make_error(vm, "exec: bitfield make: too many fields");
    for (uint32_t i = 0; i < field_count; i++) {
        offsets[i] = bcode_read_u32(bc, pc);
        widths[i] = bcode_read_u32(bc, pc);
    }
    void *data = value_alloc_data(vm->alloc, st);
    if (!data) return value_make_error(vm, "exec: bitfield make: out of memory");
    memset(data, 0, st->size);
    value_t *vals[128];
    for (uint32_t i = field_count; i > 0; i--) vals[i - 1] = exec_stack_pop(vm);
    for (uint32_t i = 0; i < field_count; i++) {
        value_t *v = vals[i];
        uint64_t raw = 0;
        if (v && value_data(v) && value_type(v))
            memcpy(&raw, value_data(v), value_type(v)->size < sizeof(raw)
                                            ? value_type(v)->size
                                            : sizeof(raw));
        bitfield_write((uint8_t *)data, word_bits, offsets[i], widths[i], raw);
    }
    (void)word_count;
    return value_make(vm, st, data);
}
