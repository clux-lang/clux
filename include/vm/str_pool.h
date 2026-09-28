#ifndef _H_CLUX_VM_STR_POOL_
#define _H_CLUX_VM_STR_POOL_
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* 前向声明（vm_t 定义在 vm/vm.h） */
typedef struct vm_t vm_t;

/* ================================================================ */
/* vm 字符串池（str 生命周期托管）                                   */
/* ================================================================ */

/**
 * str value 的 data 布局：池内字符串的 const char * 指针（含 '\0'）。
 * 池内字符串本体由 vm 统一分配、去重 intern、vm_destroy 统一释放。
 * 因此 str 的 data 是平凡可 memcpy 的指针（sizeof(str)==指针大小，
 * 与 C 语言 const char * 对齐）；NULL 表示 nil 字符串。
 */

/** 创建 vm 字符串池（vm_new 内调用；vec 元素不 owns，vm_destroy 手动释放） */
void vm_str_pool_init(vm_t *vm);

/** 销毁 vm 字符串池（vm_destroy 内调用：释放全部 intern 的字符串本体） */
void vm_str_pool_destroy(vm_t *vm);

/** 去重 intern：按内容在池中查找，命中复用；未命中分配副本并登记。
 *  返回池内字符串的持久指针（含 '\0'；调用方只存指针，长度用 strlen）。
 *  NULL 输入返回 NULL（nil 字符串）。panic 于 OOM。 */
const char *vm_str_intern_len(vm_t *vm, const char *bytes, size_t len);

/** 快捷：intern C 字符串（strlen 定长） */
const char *vm_str_intern_cstr(vm_t *vm, const char *cstr);

/** 池查询：按内容查找（不 intern），命中返回池指针，未命中返回 NULL */
const char *vm_str_lookup(vm_t *vm, const char *bytes, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_STR_POOL_ */
