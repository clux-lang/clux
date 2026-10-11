#include "vm/bcode_graph.h"

#include "module/module_path.h" /* 加载时路径重新规范化（VM 层） */
#include "core/string.h"
#include "core/strmap.h"
#include "core/strslice.h"
#include "vm/ffi_lib.h"         /* M6 FFI：extern lib 库表（别名 → 路径） */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ================================================================
 * 模块图序列化 / 反序列化（.cxb v3）
 * ================================================================
 * 格式（全部小端）：
 *   "CXBC" : 4 字节
 *   version : u32 (= 3；加载方也接受 1/2)
 *   module_count : u32
 *   entry_index : u32
 *   重复 module_count 次：
 *     path_len : u32            （0 = 入口模块）
 *     path : path_len 字节
 *     str_count : u32
 *     [str_len : u32][bytes]  × str_count
 *     code_len : u64
 *     code : code_len 字节
 *     export_count : u32
 *     [name_len : u32][name][owner_index : u32] × export_count
 *     ffi_count : u32                         （v3 起；v2 段到此结束）
 *     [alias_len : u32][alias][path_len : u32][path] × ffi_count
 *
 * 模块顺序 = vm->modules 插入顺序 = 依赖优先（见 bcode_graph.h）。
 */

/* ---- 小端辅助（与 bcode_serial.c 同一约定） ---- */

static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put_u64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

static uint32_t get_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i] << (8 * i));
    return v;
}

/* ---- 增长式输出缓冲 ---- */

typedef struct {
    allocator_t *alloc;
    uint8_t     *data;
    size_t       len;
    size_t       cap;
} gbuf_t;

static bool gb_reserve(gbuf_t *b, size_t need) {
    if (b->len + need <= b->cap) return true;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + need) nc *= 2;
    uint8_t *nd = (uint8_t *)allocator_new_ex(b->alloc, "clux.vm.graph.buf", nc,
                                              NULL, NULL, NULL, 1);
    if (!nd) return false;
    if (b->data) {
        memcpy(nd, b->data, b->len);
        allocator_free(b->alloc, (void **)&b->data);
    }
    b->data = nd;
    b->cap = nc;
    return true;
}

static bool gb_append(gbuf_t *b, const void *src, size_t n) {
    if (!gb_reserve(b, n)) return false;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return true;
}

static bool gb_u32(gbuf_t *b, uint32_t v) {
    uint8_t t[4];
    put_u32_le(t, v);
    return gb_append(b, t, sizeof t);
}

static bool gb_u64(gbuf_t *b, uint64_t v) {
    uint8_t t[8];
    put_u64_le(t, v);
    return gb_append(b, t, sizeof t);
}

/* 写一个模块段（序列化的唯一实现：vm 路径与图路径共用）。
   `import_path` 是**源码里写的路径**（入口模块传 ""）；`importer_index` 是导入者
   下标的 u32（BCODE_NO_MODULE = 入口）。规范路径不落盘——加载时按导入链重算。
   `ffi_*` 是本模块的 `extern lib` 声明（M6，v3 段；见 bcode_graph.h）。 */
static bool gb_module_section(gbuf_t *b, const char *import_path,
                              uint32_t importer_index, const bytecode_t *bc,
                              char *const *names, const uint32_t *owners,
                              size_t ecount, char *const *ffi_aliases,
                              char *const *ffi_paths, size_t fcount) {
    size_t plen = import_path ? strlen(import_path) : 0;
    if (!gb_u32(b, (uint32_t)plen)) return false;
    if (plen && !gb_append(b, import_path, plen)) return false;
    if (!gb_u32(b, importer_index)) return false;

    size_t scount = bcode_str_count(bc);
    if (!gb_u32(b, (uint32_t)scount)) return false;
    for (size_t s = 0; s < scount; s++) {
        strslice_t str = bcode_str_at(bc, s);
        if (!gb_u32(b, (uint32_t)str.len)) return false;
        if (str.len && !gb_append(b, str.ptr, str.len)) return false;
    }

    if (!gb_u64(b, (uint64_t)bc->code.len)) return false;
    if (bc->code.len && !gb_append(b, bc->code.data, bc->code.len)) return false;

    if (!gb_u32(b, (uint32_t)ecount)) return false;
    for (size_t e = 0; e < ecount; e++) {
        const char *name = names ? names[e] : NULL;
        size_t nlen = name ? strlen(name) : 0;
        if (!gb_u32(b, (uint32_t)nlen)) return false;
        if (nlen && !gb_append(b, name, nlen)) return false;
        if (!gb_u32(b, owners ? owners[e] : 0)) return false;
    }

    /* M6 FFI（v3）：别名 → 库路径。`extern lib "path" as alias;` 的别名是运行期
       `EXTERN_CALL "alias::sym"` 的查表键，路径是 dlopen 参数（**按内容判类，
       不按后缀**）。 */
    if (!gb_u32(b, (uint32_t)fcount)) return false;
    for (size_t f = 0; f < fcount; f++) {
        const char *alias = ffi_aliases ? ffi_aliases[f] : NULL;
        const char *path = ffi_paths ? ffi_paths[f] : NULL;
        size_t alen = alias ? strlen(alias) : 0;
        size_t plen = path ? strlen(path) : 0;
        if (!gb_u32(b, (uint32_t)alen)) return false;
        if (alen && !gb_append(b, alias, alen)) return false;
        if (!gb_u32(b, (uint32_t)plen)) return false;
        if (plen && !gb_append(b, path, plen)) return false;
    }
    return true;
}

/* ---- 图生命周期 ---- */

void bcode_graph_init(bcode_graph_t *g) {
    if (!g) return;
    g->alloc = NULL;
    g->modules = NULL;
    g->count = 0;
    g->entry_index = 0;
}

void bcode_graph_free(bcode_graph_t *g) {
    if (!g || !g->alloc) return;
    allocator_t *alloc = g->alloc;
    for (size_t i = 0; i < g->count; i++) {
        bcode_module_image_t *m = &g->modules[i];
        if (m->bc) bcode_destroy(&m->bc);
        if (m->export_names) {
            for (size_t k = 0; k < m->export_count; k++) {
                if (m->export_names[k])
                    allocator_free(alloc, (void **)&m->export_names[k]);
            }
            allocator_free(alloc, (void **)&m->export_names);
        }
        if (m->export_owners) allocator_free(alloc, (void **)&m->export_owners);
        if (m->ffi_aliases) {
            for (size_t f = 0; f < m->ffi_count; f++) {
                if (m->ffi_aliases[f])
                    allocator_free(alloc, (void **)&m->ffi_aliases[f]);
            }
            allocator_free(alloc, (void **)&m->ffi_aliases);
        }
        if (m->ffi_paths) {
            for (size_t f = 0; f < m->ffi_count; f++) {
                if (m->ffi_paths[f])
                    allocator_free(alloc, (void **)&m->ffi_paths[f]);
            }
            allocator_free(alloc, (void **)&m->ffi_paths);
        }
        if (m->import_path) allocator_free(alloc, (void **)&m->import_path);
        if (m->canonical) allocator_free(alloc, (void **)&m->canonical);
    }
    if (g->modules) allocator_free(alloc, (void **)&g->modules);
    bcode_graph_init(g);
}

/* 拷贝一个 NUL 终止字符串（alloc 拥有） */
static char *dup_cstr(allocator_t *alloc, const char *s, size_t len) {
    char *p = (char *)allocator_new_ex(alloc, "clux.vm.graph.str", len + 1, NULL,
                                       NULL, NULL, 1);
    if (!p) return NULL;
    if (len) memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

/* ---- 序列化 ---- */

/* 定位入口模块在 vm->modules 中的下标；不在表中返回 (size_t)-1 */
static size_t graph_index_of(const vec_t *keys, const char *canonical) {
    size_t n = keys ? vec_len((vec_t *)keys) : 0;
    for (size_t i = 0; i < n; i++) {
        const char *k = (const char *)vec_get((vec_t *)keys, i);
        if (k && canonical && strcmp(k, canonical) == 0) return i;
    }
    return (size_t)-1;
}

/* 导出表 owner 解析：值指针与更早模块的同名导出相同 → 归该模块（代理导出）。 */
static uint32_t graph_owner_of(const vec_t *keys, size_t self_index,
                               const char *name, const value_t *v,
                               const vm_t *vm) {
    for (size_t j = 0; j < self_index; j++) {
        const char *path = (const char *)vec_get((vec_t *)keys, j);
        if (!path) continue;
        module_t *dep = vm_module_lookup((vm_t *)vm, path);
        if (!dep || !dep->exports) continue;
        value_t *dv = (value_t *)strmap_get(dep->exports, name);
        if (dv && dv == v) return (uint32_t)j;
    }
    return (uint32_t)self_index;
}

static uint8_t *graph_serial_mem(allocator_t *alloc, const vm_t *vm,
                                 const module_t *entry, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!vm || !vm->modules || !entry) return NULL;

    const vec_t *keys = strmap_keys(vm->modules);
    size_t count = keys ? vec_len((vec_t *)keys) : 0;
    if (count == 0) return NULL;

    size_t entry_index = graph_index_of(keys, entry->canonical);
    if (entry_index == (size_t)-1) return NULL; /* 入口不在表中：调用方搞错了 */

    gbuf_t b = {alloc, NULL, 0, 0};
    bool ok = gb_append(&b, "CXBC", 4) && gb_u32(&b, BCODE_GRAPH_VERSION) &&
              gb_u32(&b, (uint32_t)count) && gb_u32(&b, (uint32_t)entry_index);

    for (size_t i = 0; ok && i < count; i++) {
        const char *path = (const char *)vec_get((vec_t *)keys, i);
        module_t *mod = vm_module_lookup((vm_t *)vm, path);
        if (!mod || !mod->bc) {
            ok = false;
            break;
        }
        /* 存**源码里写的路径**与导入者下标（不是规范化宿主路径）：入口模块的
           import_path 为空、importer 为"无"。加载方据此重新规范化。 */
        const char *stored = (i == entry_index) ? "" :
                             (mod->import_path ? mod->import_path : "");
        uint32_t importer_idx = BCODE_NO_MODULE;
        if (mod->importer) {
            size_t fi = graph_index_of(keys, mod->importer);
            if (fi != (size_t)-1) importer_idx = (uint32_t)fi;
        }

        const vec_t *ekeys = mod->exports ? strmap_keys(mod->exports) : NULL;
        size_t ecount = ekeys ? vec_len((vec_t *)ekeys) : 0;
        char **names = NULL;
        uint32_t *owners = NULL;
        if (ecount) {
            names = (char **)allocator_new_ex(alloc, "clux.vm.graph.w.names",
                                              sizeof(char *) * ecount, NULL,
                                              NULL, NULL, 1);
            owners = (uint32_t *)allocator_new_ex(
                alloc, "clux.vm.graph.w.owners", sizeof(uint32_t) * ecount, NULL,
                NULL, NULL, 1);
            if (!names || !owners) {
                if (names) allocator_free(alloc, (void **)&names);
                if (owners) allocator_free(alloc, (void **)&owners);
                ok = false;
                break;
            }
            for (size_t e = 0; e < ecount; e++) {
                const char *name = (const char *)vec_get((vec_t *)ekeys, e);
                value_t *val =
                    name ? (value_t *)strmap_get(mod->exports, name) : NULL;
                names[e] = (char *)name;
                owners[e] = name ? graph_owner_of(keys, i, name, val, vm)
                                 : (uint32_t)i;
            }
        }

        /* M6 FFI：本模块的 extern lib 库表（别名 → 路径）。key 是 alloc 拥有的
           别名，path 借用句柄内部的副本——仅本次序列化使用，不必拷贝。 */
        const vec_t *fkeys = mod->ffi_libs ? strmap_keys(mod->ffi_libs) : NULL;
        size_t fcount = fkeys ? vec_len((vec_t *)fkeys) : 0;
        char **ffi_aliases = NULL;
        char **ffi_paths = NULL;
        if (fcount) {
            ffi_aliases = (char **)allocator_new_ex(
                alloc, "clux.vm.graph.w.libalias", sizeof(char *) * fcount, NULL,
                NULL, NULL, 1);
            ffi_paths = (char **)allocator_new_ex(
                alloc, "clux.vm.graph.w.libpath", sizeof(char *) * fcount, NULL,
                NULL, NULL, 1);
            if (!ffi_aliases || !ffi_paths) {
                if (ffi_aliases) allocator_free(alloc, (void **)&ffi_aliases);
                if (ffi_paths) allocator_free(alloc, (void **)&ffi_paths);
                if (names) allocator_free(alloc, (void **)&names);
                if (owners) allocator_free(alloc, (void **)&owners);
                ok = false;
                break;
            }
            for (size_t f = 0; f < fcount; f++) {
                const char *alias = (const char *)vec_get((vec_t *)fkeys, f);
                ffi_lib_t *lib =
                    alias ? (ffi_lib_t *)strmap_get(mod->ffi_libs, alias) : NULL;
                ffi_aliases[f] = (char *)alias;
                ffi_paths[f] = lib ? (char *)ffi_lib_path(lib) : NULL;
                if (!ffi_aliases[f] || !ffi_paths[f]) {
                    ok = false; /* 库表里有空项：容器会写出不可加载的产物 */
                    break;
                }
            }
        }
        if (ok)
            ok = gb_module_section(&b, stored, importer_idx, mod->bc, names,
                                   owners, ecount, ffi_aliases, ffi_paths,
                                   fcount);
        if (names) allocator_free(alloc, (void **)&names);
        if (owners) allocator_free(alloc, (void **)&owners);
        if (ffi_aliases) allocator_free(alloc, (void **)&ffi_aliases);
        if (ffi_paths) allocator_free(alloc, (void **)&ffi_paths);
    }

    if (!ok) {
        if (b.data) allocator_free(alloc, (void **)&b.data);
        return NULL;
    }
    if (out_len) *out_len = b.len;
    return b.data;
}

uint8_t *bcode_graph_serial_mem(allocator_t *alloc, const vm_t *vm,
                                const module_t *entry, size_t *out_len) {
    if (!alloc) return NULL;
    return graph_serial_mem(alloc, vm, entry, out_len);
}

int bcode_graph_serial_file(allocator_t *alloc, const vm_t *vm,
                            const module_t *entry, const char *out_path) {
    if (!alloc || !out_path) return -1;
    size_t len = 0;
    uint8_t *buf = graph_serial_mem(alloc, vm, entry, &len);
    if (!buf) {
        fprintf(stderr, "serial: cannot serialize module graph\n");
        return -1;
    }
    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        fprintf(stderr, "serial: cannot open file '%s'\n", out_path);
        allocator_free(alloc, (void **)&buf);
        return -1;
    }
    size_t wrote = fwrite(buf, 1, len, fp);
    int cf = fclose(fp);
    allocator_free(alloc, (void **)&buf);
    if (wrote != len || cf != 0) {
        fprintf(stderr, "serial: short write on '%s'\n", out_path);
        return -1;
    }
    return 0;
}

/* 已加载/已汇编的图直接落盘（.cxs → .cxb：owner 下标由文本携带，原样写出，
   因此与源 .cxb 逐字节一致）。 */
uint8_t *bcode_graph_serial_image_mem(allocator_t *alloc,
                                      const bcode_graph_t *g,
                                      size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!alloc || !g || g->count == 0) return NULL;

    gbuf_t b = {alloc, NULL, 0, 0};
    bool ok = gb_append(&b, "CXBC", 4) && gb_u32(&b, BCODE_GRAPH_VERSION) &&
              gb_u32(&b, (uint32_t)g->count) &&
              gb_u32(&b, (uint32_t)g->entry_index);
    for (size_t i = 0; ok && i < g->count; i++) {
        const bcode_module_image_t *m = &g->modules[i];
        if (!m->bc) {
            ok = false;
            break;
        }
        ok = gb_module_section(&b, m->import_path ? m->import_path : "",
                               m->importer_index, m->bc, m->export_names,
                               m->export_owners, m->export_count,
                               m->ffi_aliases, m->ffi_paths, m->ffi_count);
    }
    if (!ok) {
        if (b.data) allocator_free(alloc, (void **)&b.data);
        return NULL;
    }
    if (out_len) *out_len = b.len;
    return b.data;
}

int bcode_graph_serial_image_file(allocator_t *alloc, const bcode_graph_t *g,
                                  const char *out_path) {
    if (!alloc || !out_path) return -1;
    size_t len = 0;
    uint8_t *buf = bcode_graph_serial_image_mem(alloc, g, &len);
    if (!buf) {
        fprintf(stderr, "serial: cannot serialize module graph\n");
        return -1;
    }
    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        fprintf(stderr, "serial: cannot open file '%s'\n", out_path);
        allocator_free(alloc, (void **)&buf);
        return -1;
    }
    size_t wrote = fwrite(buf, 1, len, fp);
    int cf = fclose(fp);
    allocator_free(alloc, (void **)&buf);
    if (wrote != len || cf != 0) {
        fprintf(stderr, "serial: short write on '%s'\n", out_path);
        return -1;
    }
    return 0;
}

/* ---- 加载时路径重新规范化（VM 层） ---- */

int bcode_graph_resolve_paths(allocator_t *alloc, bcode_graph_t *g,
                              const char *entry_root) {
    if (!alloc || !g || g->count == 0) return 1;
    if (g->entry_index >= g->count) return 1;

    /* 入口模块：规范路径取调用方给的根（通常 = .cxb/.cxs 文件自身的规范化
       路径）；其余模块沿导入链推导：canonical = resolve(import_path, 导入者)。 */
    bcode_module_image_t *entry = &g->modules[g->entry_index];
    if (entry->canonical) allocator_free(alloc, (void **)&entry->canonical);
    entry->canonical = dup_cstr(alloc, entry_root ? entry_root : "", 
                                entry_root ? strlen(entry_root) : 0);
    if (!entry->canonical) return 1;

    /* 不动点：每轮把"导入者已解析"的模块补上（导入链最长 = 模块数） */
    for (size_t round = 0; round < g->count; round++) {
        bool progress = false;
        for (size_t i = 0; i < g->count; i++) {
            bcode_module_image_t *m = &g->modules[i];
            if (m->canonical) continue;
            if (m->importer_index == BCODE_NO_MODULE ||
                m->importer_index >= g->count)
                continue;
            const char *base = g->modules[m->importer_index].canonical;
            if (!base) continue;
            module_import_kind_t kind = MODULE_IMPORT_MODULE;
            strslice_t raw = strslice_from_cstr(m->import_path
                                                    ? m->import_path : "");
            m->canonical = module_path_resolve(alloc, raw, base, &kind);
            if (!m->canonical) {
                fprintf(stderr,
                        "deserial: cannot resolve module path '%s'\n",
                        m->import_path ? m->import_path : "");
                return 1;
            }
            progress = true;
        }
        if (!progress) break;
    }

    for (size_t i = 0; i < g->count; i++) {
        if (!g->modules[i].canonical) {
            fprintf(stderr,
                    "deserial: module %zu is unreachable from the entry module\n",
                    i);
            return 1;
        }
    }
    return 0;
}

/* ---- 反序列化 ---- */

/* 读一段 [len u32][bytes] 的字符串；返回 alloc 拥有的 NUL 终止副本 */
static char *read_blob(allocator_t *alloc, const uint8_t *data, size_t len,
                       size_t *off, const char *what) {
    if (*off + 4 > len) {
        fprintf(stderr, "deserial: truncated %s length\n", what);
        return NULL;
    }
    uint32_t n = get_u32_le(data + *off);
    *off += 4;
    if (*off + n > len) {
        fprintf(stderr, "deserial: truncated %s data\n", what);
        return NULL;
    }
    char *s = dup_cstr(alloc, (const char *)(data + *off), n);
    *off += n;
    return s;
}

/* 读一个模块的 strtable + code 段（v1 单模块段与 v2 每模块段同构） */
static bytecode_t *read_bc(allocator_t *alloc, const uint8_t *data, size_t len,
                           size_t *off) {
    if (*off + 4 > len) {
        fprintf(stderr, "deserial: truncated strtable header\n");
        return NULL;
    }
    uint32_t scount = get_u32_le(data + *off);
    *off += 4;

    bytecode_t *bc = bcode_new(alloc);
    if (!bc) {
        fprintf(stderr, "deserial: out of memory\n");
        return NULL;
    }
    for (uint32_t i = 0; i < scount; i++) {
        if (*off + 4 > len) {
            fprintf(stderr, "deserial: truncated string length\n");
            bcode_destroy(&bc);
            return NULL;
        }
        uint32_t slen = get_u32_le(data + *off);
        *off += 4;
        if (*off + slen > len) {
            fprintf(stderr, "deserial: truncated string data\n");
            bcode_destroy(&bc);
            return NULL;
        }
        string_t *copy = (slen == 0)
            ? string_new(bc->alloc)
            : string_from_bytes(bc->alloc, (const char *)(data + *off), slen);
        vec_push(bc->strs, bc->alloc, copy);
        *off += slen;
    }

    if (*off + 8 > len) {
        fprintf(stderr, "deserial: truncated code length\n");
        bcode_destroy(&bc);
        return NULL;
    }
    uint64_t code_len = get_u64_le(data + *off);
    *off += 8;
    if (code_len > (uint64_t)(len - *off)) {
        fprintf(stderr, "deserial: truncated code (declared %llu, avail %zu)\n",
                (unsigned long long)code_len, len - *off);
        bcode_destroy(&bc);
        return NULL;
    }
    for (uint64_t i = 0; i < code_len; i++) bcode_write_u8(bc, data[*off + i]);
    *off += code_len;
    return bc;
}

int bcode_graph_deserial(allocator_t *alloc, const uint8_t *data, size_t len,
                         bcode_graph_t *out) {
    if (!out) return -1;
    bcode_graph_init(out);
    if (!alloc || !data) {
        fprintf(stderr, "deserial: invalid input\n");
        return 1;
    }
    if (len < 8 || memcmp(data, "CXBC", 4) != 0) {
        fprintf(stderr, "deserial: bad magic (not a .cxb file)\n");
        return 1;
    }
    size_t off = 4;
    uint32_t version = get_u32_le(data + off);
    off += 4;

    if (version == 1u) {
        /* v1：单模块、空路径、无导出表（旧产物继续可运行） */
        bytecode_t *bc = read_bc(alloc, data, len, &off);
        if (!bc) return 1;
        bcode_module_image_t *mods = (bcode_module_image_t *)allocator_new_ex(
            alloc, "clux.vm.graph.mods", sizeof(bcode_module_image_t), NULL,
            NULL, NULL, 1);
        if (!mods) {
            bcode_destroy(&bc);
            fprintf(stderr, "deserial: out of memory\n");
            return 1;
        }
        memset(mods, 0, sizeof *mods);
        mods[0].import_path = dup_cstr(alloc, "", 0);
        mods[0].importer_index = BCODE_NO_MODULE;
        mods[0].bc = bc;
        out->alloc = alloc;
        out->modules = mods;
        out->count = 1;
        out->entry_index = 0;
        return 0;
    }
    if (version != 2u && version != BCODE_GRAPH_VERSION) {
        fprintf(stderr, "deserial: unsupported version %u (expected %u)\n",
                (unsigned)version, (unsigned)BCODE_GRAPH_VERSION);
        return 1;
    }
    /* v2 = 无 FFI 段（每模块段结束于导出表）；v3 起每模块多一段 FFI 库表。
       段内其余布局完全一致，因此只用一个"是否有 FFI 段"的开关分流。 */
    bool has_ffi = (version >= 3u);
    if (off + 8 > len) {
        fprintf(stderr, "deserial: truncated module table header\n");
        return 1;
    }
    uint32_t count = get_u32_le(data + off);
    off += 4;
    uint32_t entry_index = get_u32_le(data + off);
    off += 4;
    if (count == 0) {
        fprintf(stderr, "deserial: empty module table\n");
        return 1;
    }
    if (entry_index >= count) {
        fprintf(stderr, "deserial: entry index %u out of range (%u modules)\n",
                (unsigned)entry_index, (unsigned)count);
        return 1;
    }

    bcode_module_image_t *mods = (bcode_module_image_t *)allocator_new_ex(
        alloc, "clux.vm.graph.mods", sizeof(bcode_module_image_t) * count, NULL,
        NULL, NULL, 1);
    if (!mods) {
        fprintf(stderr, "deserial: out of memory\n");
        return 1;
    }
    memset(mods, 0, sizeof(bcode_module_image_t) * count);
    out->alloc = alloc;
    out->modules = mods;
    out->count = count;
    out->entry_index = entry_index;

    for (uint32_t i = 0; i < count; i++) {
        bcode_module_image_t *m = &mods[i];
        m->import_path = read_blob(alloc, data, len, &off, "module import path");
        if (!m->import_path) goto fail;
        if (off + 4 > len) {
            fprintf(stderr, "deserial: truncated importer index\n");
            goto fail;
        }
        m->importer_index = get_u32_le(data + off);
        off += 4;
        if (m->importer_index != BCODE_NO_MODULE && m->importer_index >= count) {
            fprintf(stderr, "deserial: importer index %u out of range\n",
                    (unsigned)m->importer_index);
            goto fail;
        }
        m->bc = read_bc(alloc, data, len, &off);
        if (!m->bc) goto fail;

        if (off + 4 > len) {
            fprintf(stderr, "deserial: truncated export count\n");
            goto fail;
        }
        uint32_t ecount = get_u32_le(data + off);
        off += 4;
        m->export_count = ecount;
        /* 注意：ecount == 0 时**不能** continue —— v3 段后面还有 FFI 库表 */
        if (ecount > 0) {
            m->export_names = (char **)allocator_new_ex(
                alloc, "clux.vm.graph.exnames", sizeof(char *) * ecount, NULL,
                NULL, NULL, 1);
            m->export_owners = (uint32_t *)allocator_new_ex(
                alloc, "clux.vm.graph.exowners", sizeof(uint32_t) * ecount, NULL,
                NULL, NULL, 1);
            if (!m->export_names || !m->export_owners) {
                fprintf(stderr, "deserial: out of memory\n");
                goto fail;
            }
            memset(m->export_names, 0, sizeof(char *) * ecount);
            for (uint32_t e = 0; e < ecount; e++) {
                m->export_names[e] =
                    read_blob(alloc, data, len, &off, "export name");
                if (!m->export_names[e]) goto fail;
                if (off + 4 > len) {
                    fprintf(stderr, "deserial: truncated export owner\n");
                    goto fail;
                }
                m->export_owners[e] = get_u32_le(data + off);
                off += 4;
                if (m->export_owners[e] > i) {
                    /* owner 必须指向自身或更早（依赖优先）模块，否则加载时无法解析 */
                    fprintf(stderr,
                            "deserial: export owner %u not yet available (module %u)\n",
                            (unsigned)m->export_owners[e], (unsigned)i);
                    goto fail;
                }
            }
        }

        /* M6 FFI（v3）：extern lib 库表（别名 → 路径） */
        if (!has_ffi) continue;
        if (off + 4 > len) {
            fprintf(stderr, "deserial: truncated ffi lib count (module %u)\n",
                    (unsigned)i);
            goto fail;
        }
        uint32_t fcount = get_u32_le(data + off);
        off += 4;
        m->ffi_count = fcount;
        if (fcount == 0) continue;
        m->ffi_aliases = (char **)allocator_new_ex(
            alloc, "clux.vm.graph.libalias", sizeof(char *) * fcount, NULL, NULL,
            NULL, 1);
        m->ffi_paths = (char **)allocator_new_ex(
            alloc, "clux.vm.graph.libpath", sizeof(char *) * fcount, NULL, NULL,
            NULL, 1);
        if (!m->ffi_aliases || !m->ffi_paths) {
            fprintf(stderr, "deserial: out of memory\n");
            goto fail;
        }
        memset(m->ffi_aliases, 0, sizeof(char *) * fcount);
        memset(m->ffi_paths, 0, sizeof(char *) * fcount);
        for (uint32_t f = 0; f < fcount; f++) {
            m->ffi_aliases[f] = read_blob(alloc, data, len, &off, "ffi alias");
            if (!m->ffi_aliases[f]) goto fail;
            m->ffi_paths[f] = read_blob(alloc, data, len, &off, "ffi lib path");
            if (!m->ffi_paths[f]) goto fail;
            if (m->ffi_aliases[f][0] == '\0') {
                /* 空别名在运行期永远查不到（EXTERN_CALL 的别名非空） */
                fprintf(stderr, "deserial: empty ffi alias (module %u)\n",
                        (unsigned)i);
                goto fail;
            }
        }
    }
    return 0;

fail:
    bcode_graph_free(out);
    return 1;
}

int bcode_graph_from_file(allocator_t *alloc, const char *path,
                          bcode_graph_t *out) {
    if (!out) return -1;
    bcode_graph_init(out);
    if (!alloc || !path) {
        fprintf(stderr, "deserial: invalid input\n");
        return 1;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "deserial: cannot open file '%s'\n", path);
        return 1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        fprintf(stderr, "deserial: cannot seek '%s'\n", path);
        return 1;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        fprintf(stderr, "deserial: cannot size '%s'\n", path);
        return 1;
    }
    rewind(fp);
    uint8_t *buf = (uint8_t *)allocator_new_ex(alloc, "clux.vm.graph.file",
                                               (size_t)sz + 1, NULL, NULL, NULL,
                                               1);
    if (!buf) {
        fclose(fp);
        fprintf(stderr, "deserial: out of memory\n");
        return 1;
    }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        allocator_free(alloc, (void **)&buf);
        fprintf(stderr, "deserial: short read on '%s'\n", path);
        return 1;
    }
    int rc = bcode_graph_deserial(alloc, buf, got, out);
    allocator_free(alloc, (void **)&buf);
    return rc;
}
