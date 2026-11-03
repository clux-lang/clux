#ifndef _H_CLUX_VM_CTFE_MEM_
#define _H_CLUX_VM_CTFE_MEM_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * CTFE 存储归属登记（compile-time evaluation 的 use-after-free 诊断）
 *
 * 问题：CTFE 解释器把**真实存储地址**交给编译期代码（`x.&`、`data(arr)`、
 * 切片基址）。若该地址指向局部变量，其作用域退出后 data 块被释放，逃逸指针
 * 再被解引用就是 use-after-free——在编译器进程里读已释放内存，结果不可预测
 * （实测读到垃圾值，偶尔崩溃）。
 *
 * 做法：不做指针编码（借用/中间指针的奇偶不可靠），而是登记**归属**——
 *
 *   1. 取地址时调用 ctfe_mem_note_addr(addr, owner)：把 addr 记到所属存储块
 *      基址 owner 名下（owner 同时登记为 alive）。
 *   2. value_dispose 释放 data 块时调用 ctfe_mem_on_free(base)：把该块标 dead。
 *   3. 解引用前调用 ctfe_mem_check(target)：target 曾被登记且其块已 dead →
 *      返回诊断文本；否则 NULL（未登记 = 无法判定，放行）。
 *
 * 只登记"被取过地址"的块，因此普通值路径零开销；表按地址精确查找，O(1)。
 * 配合**隔离释放**（value_dispose 不立即 free，编译结束统一释放），
 * 同一编译内地址不会复用 → 地址即对象身份，诊断确定性成立。
 */

/** 登记 addr 归属存储块 owner（owner 为空时以 addr 自身为块基址）。
 *
 *  **只记录归属，不改存活状态**：块的 alive 由 ctfe_mem_on_alloc /
 *  ctfe_mem_on_free 权威决定（分配即 alive、dispose 即 dead）。owner 尚未
 *  登记过时按 alive 登记（未知来源的存储只能当作活的）。
 *  addr 为 NULL 或达到登记上限时静默忽略。 */
void ctfe_mem_note_addr(void *addr, void *owner);

/** 存储块分配：标 alive（`value_alloc_data*` 在返回 data 前调用）。
 *
 *  必须与释放钩子配对，否则"归属登记不得复活已释放块"会导致**误报**：
 *  allocator 复用地址时，新块若不重新标 alive，旧登记项会让它还留在 dead
 *  状态。地址复用时只可能漏报，不可能误报——这个不变式由本钩子保证。 */
void ctfe_mem_on_alloc(void *base, size_t size);

/**
 * 隔离释放：编译期求值时**不**把 value data 还给分配器，而是延后到本次
 * 编译结束（`ctfe_mem_reset`）统一释放。
 *
 * 这样同一编译内**地址不会复用**，地址即对象身份 —— 逃逸指针诊断因此
 * 确定性成立（不会因为地址被新对象复用而漏报，也不会因为旧归属残留而
 * 误报）。返回 true 表示已接管（调用方不要再 free）。
 */
bool ctfe_mem_quarantine(void *data);

/** 记录本次编译的分配器（隔离释放表用它统一 free）。 */
void ctfe_mem_set_allocator(allocator_t *alloc);

/** 查询 addr 已登记的所属存储块基址；未登记返回 NULL。 */
void *ctfe_mem_owner_of(const void *addr);

/** 存储块释放：已登记块标 dead（同一地址被重新分配时由 note_addr 复活）。
 *  value_dispose 在真正 allocator_free 之前调用。 */
void ctfe_mem_on_free(void *base);

/** 解引用校验：NULL = 通过；否则返回静态缓冲区中的诊断文本。 */
const char *ctfe_mem_check(const void *addr);

/** 清空全部登记（vm 销毁时调用；登记表按 vm 生命周期重置，避免跨编译
 *  的地址复用造成误判）。 */
void ctfe_mem_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_CTFE_MEM_ */
