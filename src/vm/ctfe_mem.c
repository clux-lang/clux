#include "vm/ctfe_mem.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ===========================================================================
 * 已知限制（未修）
 *
 * 从 comptime 函数**返回**的指针/切片，其地址归属只有在经过赋值登记
 * （`ctfe_mem_note_addr`）时才进入 g_prov；没有登记时，解引用检查退化为
 * "查不到 → 放行"。是否登记取决于之前的执行顺序，因此 `DriverComptime.*`
 * 里若干个 use-after-free 用例在**全量运行**时偶发漏报（单独跑必过）。
 *
 * 正确修法：在 comptime 函数返回处做逃逸检查——返回值是指针/切片时，若其
 * 目标落在"被调方局部存储块"的范围内（需要块的尺寸/范围信息）就直接报错，
 * 不依赖后续解引用时的归属表。试过两版（块代次、地址范围兜底）都会误伤
 * 合法场景（同地址 realloc / 栈上切片），已回退。
 * =========================================================================== */


/* 登记上限：超过后停止登记（只降低诊断精度，不影响程序语义）。
   每项约 16 字节，200 万项 ≈ 32MB，足够覆盖正常编译期求值。 */
#define CTFE_MEM_MAX_ENTRIES 2000000u

/* 存储块：身份 = 基址；alive 表示 data 块尚未被 value_dispose 释放。 */
typedef struct {
    void *base;
    bool  alive;
} ctfe_blk_t;

/* 地址归属：addr 曾作为"取地址"的结果出现，其存储块基址为 owner。 */
typedef struct {
    void *addr;
    void *owner;
} ctfe_prov_t;

static ctfe_blk_t  *g_blk;
static size_t       g_blk_n;
static size_t       g_blk_cap;
static int32_t     *g_blk_idx;   /* 开放寻址：存 g_blk 下标 + 1，0 = 空槽 */
static size_t       g_blk_idx_cap;

static ctfe_prov_t *g_prov;
static size_t       g_prov_n;
static size_t       g_prov_cap;
static int32_t     *g_prov_idx;
static size_t       g_prov_idx_cap;

/* 达到上限后彻底停用（不再登记，避免无限增长）。 */
static bool g_disabled;

static size_t mix_ptr(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (size_t)x;
}

/* ---------------------------------------------------------------- 块表 */

static void blk_rehash(size_t new_cap) {
    int32_t *nidx = (int32_t *)calloc(new_cap, sizeof(int32_t));
    if (!nidx) {
        g_disabled = true;
        return;
    }
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < g_blk_n; i++) {
        size_t h = mix_ptr(g_blk[i].base) & mask;
        while (nidx[h]) h = (h + 1) & mask;
        nidx[h] = (int32_t)(i + 1);
    }
    free(g_blk_idx);
    g_blk_idx = nidx;
    g_blk_idx_cap = new_cap;
}

static int blk_find(const void *base) {
    if (g_blk_idx_cap == 0) return -1;
    size_t mask = g_blk_idx_cap - 1;
    size_t h = mix_ptr(base) & mask;
    while (g_blk_idx[h]) {
        int i = g_blk_idx[h] - 1;
        if (g_blk[i].base == base) return i;
        h = (h + 1) & mask;
    }
    return -1;
}

/* 取得 base 的块下标（不存在则登记为 alive）。失败返回 -1。 */
static int blk_intern(void *base) {
    int i = blk_find(base);
    if (i >= 0) {
        g_blk[i].alive = true;
        return i;
    }
    if (g_disabled) return -1;
    if (g_blk_n >= CTFE_MEM_MAX_ENTRIES) {
        g_disabled = true;
        return -1;
    }
    /* 负载因子 > 3/4 时扩容（首次 64 槽） */
    if (g_blk_idx_cap == 0 || (g_blk_n + 1) * 4 > g_blk_idx_cap * 3) {
        size_t want = g_blk_idx_cap ? g_blk_idx_cap * 2 : 64;
        while (want * 3 < (g_blk_n + 1) * 4) want *= 2;
        blk_rehash(want);
        if (g_disabled) return -1;
    }
    if (g_blk_n == g_blk_cap) {
        size_t cap = g_blk_cap ? g_blk_cap * 2 : 64;
        ctfe_blk_t *nb = (ctfe_blk_t *)realloc(g_blk, cap * sizeof(*nb));
        if (!nb) {
            g_disabled = true;
            return -1;
        }
        g_blk = nb;
        g_blk_cap = cap;
    }
    i = (int)g_blk_n++;
    g_blk[i].base = base;
    g_blk[i].alive = true;
    size_t mask = g_blk_idx_cap - 1;
    size_t h = mix_ptr(base) & mask;
    while (g_blk_idx[h]) h = (h + 1) & mask;
    g_blk_idx[h] = (int32_t)(i + 1);
    return i;
}

/* ------------------------------------------------------------- 归属表 */

static void prov_rehash(size_t new_cap) {
    int32_t *nidx = (int32_t *)calloc(new_cap, sizeof(int32_t));
    if (!nidx) {
        g_disabled = true;
        return;
    }
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < g_prov_n; i++) {
        size_t h = mix_ptr(g_prov[i].addr) & mask;
        while (nidx[h]) h = (h + 1) & mask;
        nidx[h] = (int32_t)(i + 1);
    }
    free(g_prov_idx);
    g_prov_idx = nidx;
    g_prov_idx_cap = new_cap;
}

static int prov_find(const void *addr) {
    if (g_prov_idx_cap == 0) return -1;
    size_t mask = g_prov_idx_cap - 1;
    size_t h = mix_ptr(addr) & mask;
    while (g_prov_idx[h]) {
        int i = g_prov_idx[h] - 1;
        if (g_prov[i].addr == addr) return i;
        h = (h + 1) & mask;
    }
    return -1;
}

static void prov_intern(void *addr, void *owner) {
    int i = prov_find(addr);
    if (i >= 0) {
        g_prov[i].owner = owner; /* 地址复用：以最新归属为准 */
        return;
    }
    if (g_disabled) return;
    if (g_prov_n >= CTFE_MEM_MAX_ENTRIES) {
        g_disabled = true;
        return;
    }
    if (g_prov_idx_cap == 0 || (g_prov_n + 1) * 4 > g_prov_idx_cap * 3) {
        size_t want = g_prov_idx_cap ? g_prov_idx_cap * 2 : 64;
        while (want * 3 < (g_prov_n + 1) * 4) want *= 2;
        prov_rehash(want);
        if (g_disabled) return;
    }
    if (g_prov_n == g_prov_cap) {
        size_t cap = g_prov_cap ? g_prov_cap * 2 : 64;
        ctfe_prov_t *np = (ctfe_prov_t *)realloc(g_prov, cap * sizeof(*np));
        if (!np) {
            g_disabled = true;
            return;
        }
        g_prov = np;
        g_prov_cap = cap;
    }
    i = (int)g_prov_n++;
    g_prov[i].addr = addr;
    g_prov[i].owner = owner;
    size_t mask = g_prov_idx_cap - 1;
    size_t h = mix_ptr(addr) & mask;
    while (g_prov_idx[h]) h = (h + 1) & mask;
    g_prov_idx[h] = (int32_t)(i + 1);
}

/* ------------------------------------------------------------ 公共接口 */

void ctfe_mem_note_addr(void *addr, void *owner) {
    if (!addr || g_disabled) return;
    const void *blk = owner ? owner : addr;
    /* 归属只记录，不改存活状态；未知块按 alive 登记（无证据说明它已释放）。 */
    if (blk_find(blk) < 0 && blk_intern((void *)blk) < 0) return;
    prov_intern(addr, (void *)blk);
}

void ctfe_mem_on_alloc(void *base) {
    if (!base || g_disabled) return;
    /* 地址复用：同一 base 可能对应**上一次**已释放的块。分配时必须显式把
       块状态置回 alive，否则会残留 dead → 误报 use-after-free（全量运行时
       顺序相关的 flake 就是这么来的）。 */
    int bi = blk_find(base);
    if (bi < 0) bi = blk_intern(base);
    if (bi >= 0) g_blk[bi].alive = true;
}

void *ctfe_mem_owner_of(const void *addr) {
    int pi = prov_find(addr);
    return pi >= 0 ? g_prov[pi].owner : NULL;
}

void ctfe_mem_on_free(void *base) {
    if (!base || g_blk_idx_cap == 0) return;
    int bi = blk_find(base);
    if (bi >= 0) g_blk[bi].alive = false;
}

const char *ctfe_mem_check(const void *addr) {
    static char buf[256];
    if (!addr || g_prov_idx_cap == 0) return NULL;
    int pi = prov_find(addr);
    if (pi < 0) return NULL;
    int bi = blk_find(g_prov[pi].owner);
    if (bi < 0 || g_blk[bi].alive) return NULL;
    snprintf(buf, sizeof buf,
             "ctfe: use-after-free: dereferencing %p, whose storage block %p "
             "has already been released (the pointer escaped the lifetime of "
             "its owner)",
             addr, g_prov[pi].owner);
    return buf;
}

void ctfe_mem_reset(void) {
    free(g_blk);
    free(g_blk_idx);
    free(g_prov);
    free(g_prov_idx);
    g_blk = NULL;
    g_blk_n = 0;
    g_blk_cap = 0;
    g_blk_idx = NULL;
    g_blk_idx_cap = 0;
    g_prov = NULL;
    g_prov_n = 0;
    g_prov_cap = 0;
    g_prov_idx = NULL;
    g_prov_idx_cap = 0;
    g_disabled = false;
}
