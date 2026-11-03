#include "vm/ctfe_mem.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ===========================================================================
 * CTFE 逃逸指针诊断：存储块 + 地址归属
 *
 * 目标：在编译期求值里发现"指针逃出了它所属存储的生命周期"并在**解引用前**
 * 报错。难点在于地址会被分配器复用，因此不能只用"地址 + alive 标志"：
 *
 *   - 同一个地址先属于对象 A（已释放），后属于对象 B（存活）——
 *     指向 A 的旧指针必须仍然报错，不能因为地址被 B 复用而放行；
 *   - 同一个对象被反复登记（分配、取址、建切片）——不能因为重复登记就
 *     把对象当成"新对象"。
 *
 * 解法的关键是给每个**分配对象**一个递增 `id`（身份），归属登记记录
 * `(块基址, 该块的 id)`：
 *
 *   - 分配（`ctfe_mem_on_alloc`）：地址已被占用时——
 *       原块 alive（同一对象重复登记）→ 保留 id；
 *       原块 dead（地址复用，新对象）    → 分配新 id；
 *   - 释放（`ctfe_mem_on_free`）：只把 alive 置 false，id 保留；
 *   - 检查（`ctfe_mem_check`）：归属块存在且 id 相同且 alive → 通过；
 *       否则（dead / id 不同 = 地址已归新对象 / 归属未知）→ 报错。
 *     归属**从未登记**时退化为按地址范围判定：落在存活块里 → 通过
 *     （合法的新对象），落在已释放块的范围里 → 报错。
 *     这条兜底只用于"没有归属信息"的指针，且存活块优先，避免把
 *     "已释放块被新分配切分复用"误判成逃逸。
 * =========================================================================== */

/* 登记上限：超过后停止登记（只降低诊断精度，不影响程序语义）。 */
#define CTFE_MEM_MAX_ENTRIES 2000000u

/* 存储块：base 是 data 块基址；alive 表示尚未被 value_dispose 释放；
   id 是该块（这一份对象）的身份，地址复用时会换新 id。 */
typedef struct {
    void *base;
    size_t size;    /* 块大小（0 = 未知），范围兜底用 */
    bool  alive;
    uint32_t id;    /* 对象身份（从 1 递增） */
} ctfe_blk_t;

/* 地址归属：addr 曾作为"取地址/切片/建视图"的结果出现；
   owner/owner_id 是当时的存储块基址与身份。owner == NULL 表示归属未知。 */
typedef struct {
    void *addr;
    void *owner;
    uint32_t owner_id;
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

static uint32_t     g_next_id = 1;

/* 隔离释放表：本次编译内释放的 value data 暂不还给分配器，
   vm 销毁时统一 free（保证地址不复用 → 地址即对象身份）。 */
static void        **g_q;
static size_t        g_q_n;
static size_t        g_q_cap;
static allocator_t  *g_q_alloc;
/* 上限：约 400 万个块（超出后退化为立即释放，只降低诊断确定性）。 */
#define CTFE_QUARANTINE_MAX 4000000u

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

static void blk_add_index(int i) {
    size_t mask = g_blk_idx_cap - 1;
    size_t h = mix_ptr(g_blk[i].base) & mask;
    while (g_blk_idx[h]) h = (h + 1) & mask;
    g_blk_idx[h] = (int32_t)(i + 1);
}

/* 分配登记：base 处出现一个**新对象**。
   - 地址首次出现 → 新建块、新 id；
   - 地址已被一个 dead 块占用 → 地址复用：标 alive 并换新 id；
   - 地址被一个 alive 块占用 → 同一对象的重复登记：保留 id。
   失败返回 -1（表停用）。 */
static int blk_alloc(void *base, size_t size) {
    int i = blk_find(base);
    if (i >= 0) {
        if (!g_blk[i].alive) {
            g_blk[i].alive = true;
            g_blk[i].id = g_next_id++;
        }
        if (size) g_blk[i].size = size;
        return i;
    }
    if (g_disabled) return -1;
    if (g_blk_n >= CTFE_MEM_MAX_ENTRIES) {
        g_disabled = true;
        return -1;
    }
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
    g_blk[i].size = size;
    g_blk[i].alive = true;
    g_blk[i].id = g_next_id++;
    blk_add_index(i);
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

static void prov_intern(void *addr, void *owner, uint32_t owner_id) {
    int i = prov_find(addr);
    if (i >= 0) {
        /* 同一地址的最新一次归属为准（新指针覆盖旧指针的记录）。 */
        g_prov[i].owner = owner;
        g_prov[i].owner_id = owner_id;
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
    g_prov[i].owner_id = owner_id;
    size_t mask = g_prov_idx_cap - 1;
    size_t h = mix_ptr(addr) & mask;
    while (g_prov_idx[h]) h = (h + 1) & mask;
    g_prov_idx[h] = (int32_t)(i + 1);
}

/* ------------------------------------------------------------ 公共接口 */

void ctfe_mem_note_addr(void *addr, void *owner) {
    if (!addr || g_disabled) return;
    if (!owner) {
        /* 归属未知：不要凭空造一个 alive 块（那会永久放行已释放的地址），
           交给 ctfe_mem_check 的范围兜底判定。 */
        prov_intern(addr, NULL, 0);
        return;
    }
    int bi = blk_find(owner);
    if (bi < 0) {
        /* owner 不是已登记的块基址：归属未知。 */
        prov_intern(addr, NULL, 0);
        return;
    }
    prov_intern(addr, owner, g_blk[bi].id);
}

void ctfe_mem_on_alloc(void *base, size_t size) {
    if (!base || g_disabled) return;
    (void)blk_alloc(base, size);
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

/* addr 是否落在某个存活块的范围内。 */
static int range_in_alive(const void *addr) {
    uintptr_t a = (uintptr_t)addr;
    for (size_t i = 0; i < g_blk_n; i++) {
        if (!g_blk[i].alive || !g_blk[i].size) continue;
        uintptr_t b = (uintptr_t)g_blk[i].base;
        if (a >= b && a < b + g_blk[i].size) return (int)i;
    }
    return -1;
}

/* addr 是否落在某个**已释放**块的范围内。 */
static int range_in_dead(const void *addr) {
    uintptr_t a = (uintptr_t)addr;
    for (size_t i = 0; i < g_blk_n; i++) {
        if (g_blk[i].alive || !g_blk[i].size) continue;
        uintptr_t b = (uintptr_t)g_blk[i].base;
        if (a >= b && a < b + g_blk[i].size) return (int)i;
    }
    return -1;
}

const char *ctfe_mem_check(const void *addr) {
    static char buf[256];
    if (!addr || g_disabled) return NULL;

    int pi = prov_find(addr);
    if (pi >= 0 && g_prov[pi].owner) {
        int bi = blk_find(g_prov[pi].owner);
        if (bi < 0) {
            snprintf(buf, sizeof buf,
                     "ctfe: use-after-free: dereferencing %p, whose storage "
                     "block %p is no longer tracked",
                     addr, g_prov[pi].owner);
            return buf;
        }
        if (g_blk[bi].alive && g_blk[bi].id == g_prov[pi].owner_id) return NULL;
        snprintf(buf, sizeof buf,
                 "ctfe: use-after-free: dereferencing %p, whose storage block "
                 "%p has already been %s (the pointer escaped the lifetime of "
                 "its owner)",
                 addr, g_prov[pi].owner,
                 g_blk[bi].alive ? "reused" : "released");
        return buf;
    }

    /* 没有归属信息：按地址范围兜底。存活块优先（地址复用的合法情形：
       `churn` 里每轮重新分配同一批地址，新指针指向新对象）。 */
    if (range_in_alive(addr) >= 0) return NULL;
    int di = range_in_dead(addr);
    if (di >= 0) {
        snprintf(buf, sizeof buf,
                 "ctfe: use-after-free: dereferencing %p, which lies in "
                 "released storage %p (the pointer escaped the lifetime of "
                 "its owner)",
                 addr, g_blk[di].base);
        return buf;
    }
    return NULL;
}

void ctfe_mem_set_allocator(allocator_t *alloc) {
    if (!g_q_alloc) g_q_alloc = alloc;
}

bool ctfe_mem_quarantine(void *data) {
    if (!data || g_disabled) return false;
    if (g_q_n >= CTFE_QUARANTINE_MAX) return false;
    if (g_q_n == g_q_cap) {
        size_t cap = g_q_cap ? g_q_cap * 2 : 256;
        void **nq = (void **)realloc(g_q, cap * sizeof(*nq));
        if (!nq) return false;
        g_q = nq;
        g_q_cap = cap;
    }
    g_q[g_q_n++] = data;
    return true;
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
    g_next_id = 1;
    g_disabled = false;
    if (g_q_alloc) {
        for (size_t i = 0; i < g_q_n; i++) {
            void *q = g_q[i];
            allocator_free(g_q_alloc, &q);
        }
    }
    free(g_q);
    g_q = NULL;
    g_q_n = 0;
    g_q_cap = 0;
    g_q_alloc = NULL;
}
