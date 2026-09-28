#include "vm/str_pool.h"
#include "vm/vm.h"
#include "core/vec.h"
#include "core/panic.h"

#include <string.h>
#include <stddef.h>

/* ===========================================================================
 * vm 字符串池
 *
 * 池内字符串本体（含 '\0'）为 allocator 分配的独立块，str value 的 data
 * 直接存池内指针（const char *，sizeof(str)==指针大小，与 C 对齐）。按内容
 * 去重 intern：str value 的 data 全平凡化后，任意 str 的 data 都是对池内存
 * 的 memcpy 可拷贝指针，生命周期归池。
 *
 * 池 = vm->strs（vec_t*，元素为 char* 字符串块，不 owns）。vm_destroy
 * 手动释放全部元素。线性查找：clux 程序字符串数量级小，去重保正确性，
 * 性能优化（哈希表）留待后续。
 * =========================================================================== */

static char *pool_str_dup(vm_t *vm, const char *bytes, size_t len) {
    char *buf = (char *)allocator_new_ex(vm->alloc, "char", sizeof(char),
                                         NULL, NULL, NULL, len + 1);
    if (!buf) panic("vm: out of memory interning string");
    if (len) memcpy(buf, bytes, len);
    buf[len] = '\0';
    return buf;
}

void vm_str_pool_init(vm_t *vm) {
    if (!vm) return;
    if (!vm->strs) vm->strs = vec_new(vm->alloc, /*owns_element=*/false);
}

void vm_str_pool_destroy(vm_t *vm) {
    if (!vm) return;
    if (vm->strs) {
        size_t n = vec_len(vm->strs);
        for (size_t i = 0; i < n; i++) {
            char *s = (char *)vec_get(vm->strs, i);
            if (s) allocator_free(vm->alloc, (void **)&s);
        }
        vec_free(vm->alloc, &vm->strs);
    }
}

const char *vm_str_intern_len(vm_t *vm, const char *bytes, size_t len) {
    if (!vm) return NULL;
    if (!bytes) return NULL;  /* nil 字符串 */

    vm_str_pool_init(vm);
    const char *hit = vm_str_lookup(vm, bytes, len);
    if (hit) return hit;

    char *buf = pool_str_dup(vm, bytes, len);
    vec_push(vm->strs, vm->alloc, buf);
    return buf;
}

const char *vm_str_intern_cstr(vm_t *vm, const char *cstr) {
    if (!vm || !cstr) return NULL;
    return vm_str_intern_len(vm, cstr, strlen(cstr));
}

const char *vm_str_lookup(vm_t *vm, const char *bytes, size_t len) {
    if (!vm || !bytes || !vm->strs) return NULL;
    size_t n = vec_len(vm->strs);
    for (size_t i = 0; i < n; i++) {
        const char *s = (const char *)vec_get(vm->strs, i);
        if (s && strlen(s) == len && memcmp(s, bytes, len) == 0)
            return s;
    }
    return NULL;
}
