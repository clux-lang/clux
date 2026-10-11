#include "vm/bcode_graph.h"

#include "vm/bcode_asm.h"
#include "vm/bcode_disasm.h"

#include "core/string.h"
#include "core/strmap.h"
#include "core/strslice.h"
#include "core/vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * 模块图的文本形态（.cxs）：渲染 + 解析
 *
 * 格式见 bcode_graph.h。这里只做"分段"：把每段模块代码原样交给单模块
 * 汇编器/反汇编器（bcode_asm_parse / bcode_disasm_mem），多模块语义由
 * MODULE / export 指令承载。单模块产物不含任何指令行，文本与旧格式一致。
 * =========================================================================== */

/* ---- 输出缓冲 ---- */

typedef struct {
    allocator_t *alloc;
    char        *data;
    size_t       len;
    size_t       cap;
} tbuf_t;

static void tb_append(tbuf_t *b, const char *s, size_t n) {
    if (n == 0) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 512;
        while (nc < b->len + n + 1) nc *= 2;
        char *nd = (char *)allocator_new_ex(b->alloc, "clux.vm.graph.text", nc,
                                            NULL, NULL, NULL, 1);
        if (b->data) {
            memcpy(nd, b->data, b->len);
            allocator_free(b->alloc, (void **)&b->data);
        }
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void tb_cstr(tbuf_t *b, const char *s) { tb_append(b, s, strlen(s)); }

static void tb_u32(tbuf_t *b, uint32_t v) {
    char tmp[16];
    snprintf(tmp, sizeof tmp, "%u", (unsigned)v);
    tb_cstr(b, tmp);
}

/* 写一个带转义的引号字符串（\\ 与 \" 转义；路径在 Windows 下含反斜杠） */
static void tb_quoted(tbuf_t *b, const char *s) {
    tb_append(b, "\"", 1);
    for (const char *p = s; *p; p++) {
        if (*p == '\\' || *p == '"') {
            char esc[2] = {'\\', *p};
            tb_append(b, esc, 2);
        } else {
            tb_append(b, p, 1);
        }
    }
    tb_append(b, "\"", 1);
}

/* ---- 渲染 ---- */

char *bcode_graph_disasm_mem(allocator_t *alloc, const bcode_graph_t *g,
                             size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!alloc || !g || g->count == 0) return NULL;

    tbuf_t b = {alloc, NULL, 0, 0};

    for (size_t i = 0; i < g->count; i++) {
        const bcode_module_image_t *m = &g->modules[i];

        /* 多模块才写指令行：单模块（入口）产物保持与旧 .cxs 逐字一致 */
        if (g->count > 1) {
            tb_cstr(&b, "MODULE");
            if (m->import_path && m->import_path[0]) {
                tb_cstr(&b, " ");
                tb_quoted(&b, m->import_path);
                tb_cstr(&b, " ");
                tb_u32(&b, m->importer_index);
            }
            tb_cstr(&b, "\n");
        }
        /* M6 FFI：`extern lib "<路径>" as <别名>;` → 文本指令
           `externlib "<路径>" as <别名>`（同一模块段内，紧随 MODULE 之后）。
           没有 FFI 声明的模块不写任何行 —— 非 FFI 产物的文本因此与旧格式
           **逐字一致**（旧 .cxs 手工样本与往返测试不受影响）。 */
        for (size_t f = 0; f < m->ffi_count; f++) {
            tb_cstr(&b, "externlib ");
            tb_quoted(&b, m->ffi_paths[f] ? m->ffi_paths[f] : "");
            tb_cstr(&b, " as ");
            tb_cstr(&b, m->ffi_aliases[f] ? m->ffi_aliases[f] : "");
            tb_cstr(&b, "\n");
        }
        for (size_t e = 0; e < m->export_count; e++) {
            tb_cstr(&b, "export ");
            tb_cstr(&b, m->export_names[e] ? m->export_names[e] : "");
            tb_cstr(&b, " ");
            tb_u32(&b, m->export_owners[e]);
            tb_cstr(&b, "\n");
        }

        size_t clen = 0;
        char *ctext = bcode_disasm_mem(alloc, m->bc, &clen);
        if (!ctext) {
            if (b.data) allocator_free(alloc, (void **)&b.data);
            return NULL;
        }
        tb_append(&b, ctext, clen);
        allocator_free(alloc, (void **)&ctext);
        /* 模块之间分隔（保证下一段指令行落在行首） */
        if (clen > 0 && b.data[b.len - 1] != '\n') tb_cstr(&b, "\n");
    }

    if (out_len) *out_len = b.len;
    return b.data;
}

int bcode_graph_disasm_to_file(allocator_t *alloc, const bcode_graph_t *g,
                               const char *out_path) {
    if (!alloc || !g || !out_path) return -1;
    size_t len = 0;
    char *text = bcode_graph_disasm_mem(alloc, g, &len);
    if (!text) {
        fprintf(stderr, "disasm: cannot render module graph\n");
        return -1;
    }
    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        fprintf(stderr, "disasm: cannot open file '%s'\n", out_path);
        allocator_free(alloc, (void **)&text);
        return -1;
    }
    size_t wrote = fwrite(text, 1, len, fp);
    int cf = fclose(fp);
    allocator_free(alloc, (void **)&text);
    if (wrote != len || cf != 0) {
        fprintf(stderr, "disasm: short write on '%s'\n", out_path);
        return -1;
    }
    return 0;
}

int bcode_graph_disasm_file(allocator_t *alloc, const vm_t *vm,
                            const module_t *entry, const char *out_path) {
    if (!alloc || !vm || !entry || !out_path) return -1;
    /* 经"序列化 → 反序列化"复用唯一渲染器：emit 出的 .cxs 与 .cxb → .cxs
       的产物因此逐字一致（owner 下标推导只有一处实现）。 */
    size_t len = 0;
    uint8_t *buf = bcode_graph_serial_mem(alloc, vm, entry, &len);
    if (!buf) {
        fprintf(stderr, "disasm: cannot serialize module graph\n");
        return -1;
    }
    bcode_graph_t g;
    bcode_graph_init(&g);
    int rc = bcode_graph_deserial(alloc, buf, len, &g);
    allocator_free(alloc, (void **)&buf);
    if (rc == 0) rc = bcode_graph_disasm_to_file(alloc, &g, out_path);
    bcode_graph_free(&g);
    return rc;
}

/* ---- 解析 ---- */

/* 解析引号字符串到 alloc 拥有的 NUL 终止副本；*pp 停在右引号之后 */
static char *parse_quoted(allocator_t *alloc, const char **pp, const char *end) {
    const char *p = *pp;
    if (p >= end || *p != '"') return NULL;
    p++;
    size_t cap = 64, len = 0;
    char *out = (char *)allocator_new_ex(alloc, "clux.vm.graph.qstr", cap, NULL,
                                         NULL, NULL, 1);
    if (!out) return NULL;
    while (p < end && *p != '"') {
        char c = *p++;
        if (c == '\\' && p < end) c = *p++;
        if (len + 2 > cap) {
            size_t nc = cap * 2;
            char *nd = (char *)allocator_new_ex(alloc, "clux.vm.graph.qstr", nc,
                                                NULL, NULL, NULL, 1);
            if (!nd) {
                allocator_free(alloc, (void **)&out);
                return NULL;
            }
            memcpy(nd, out, len);
            allocator_free(alloc, (void **)&out);
            out = nd;
            cap = nc;
        }
        out[len++] = c;
    }
    if (p >= end) { /* 未闭合 */
        allocator_free(alloc, (void **)&out);
        return NULL;
    }
    p++; /* 跳过右引号 */
    out[len] = '\0';
    *pp = p;
    return out;
}

/* 每段模块的临时状态（源码路径 + 导入者 + 导出 + FFI 库表 + 代码文本） */
typedef struct {
    char  *path;
    uint32_t importer_index;
    char **names;
    uint32_t *owners;
    size_t   ecount, ecap;
    char **ffi_aliases;
    char **ffi_paths;
    size_t   fcount, fcap;
    tbuf_t   code;
} asm_section_t;

static int section_add_export(allocator_t *alloc, asm_section_t *s,
                              char *name, uint32_t owner) {
    if (s->ecount == s->ecap) {
        size_t nc = s->ecap ? s->ecap * 2 : 8;
        char **nn = (char **)allocator_new_ex(alloc, "clux.vm.graph.exnames",
                                              sizeof(char *) * nc, NULL, NULL,
                                              NULL, 1);
        uint32_t *no = (uint32_t *)allocator_new_ex(
            alloc, "clux.vm.graph.exowners", sizeof(uint32_t) * nc, NULL, NULL,
            NULL, 1);
        if (!nn || !no) return -1;
        if (s->ecount) {
            memcpy(nn, s->names, sizeof(char *) * s->ecount);
            memcpy(no, s->owners, sizeof(uint32_t) * s->ecount);
        }
        if (s->names) allocator_free(alloc, (void **)&s->names);
        if (s->owners) allocator_free(alloc, (void **)&s->owners);
        s->names = nn;
        s->owners = no;
        s->ecap = nc;
    }
    s->names[s->ecount] = name;
    s->owners[s->ecount] = owner;
    s->ecount++;
    return 0;
}

/* M6 FFI：往当前段追加一条 `externlib "<路径>" as <别名>`（两个串均由 alloc 拥有）。 */
static int section_add_ffi(allocator_t *alloc, asm_section_t *s, char *path,
                           char *alias) {
    if (s->fcount == s->fcap) {
        size_t nc = s->fcap ? s->fcap * 2 : 4;
        char **na = (char **)allocator_new_ex(
            alloc, "clux.vm.graph.libalias", sizeof(char *) * nc, NULL, NULL,
            NULL, 1);
        char **np = (char **)allocator_new_ex(
            alloc, "clux.vm.graph.libpath", sizeof(char *) * nc, NULL, NULL,
            NULL, 1);
        if (!na || !np) {
            if (na) allocator_free(alloc, (void **)&na);
            if (np) allocator_free(alloc, (void **)&np);
            return -1;
        }
        if (s->fcount) {
            memcpy(na, s->ffi_aliases, sizeof(char *) * s->fcount);
            memcpy(np, s->ffi_paths, sizeof(char *) * s->fcount);
        }
        if (s->ffi_aliases) allocator_free(alloc, (void **)&s->ffi_aliases);
        if (s->ffi_paths) allocator_free(alloc, (void **)&s->ffi_paths);
        s->ffi_aliases = na;
        s->ffi_paths = np;
        s->fcap = nc;
    }
    s->ffi_aliases[s->fcount] = alias;
    s->ffi_paths[s->fcount] = path;
    s->fcount++;
    return 0;
}

/* 解析 `externlib "<库路径>" as <别名>`（M6：extern lib 声明的文本形态）。
   成功返回 0 并把两个串（alloc 拥有）交给当前段；失败返回 -1（已打印原因）。 */
static int parse_externlib_line(allocator_t *alloc, const char *p,
                                const char *nl, asm_section_t *sec) {
    const char *q = p + 10; /* 跳过 "externlib " */
    while (q < nl && (*q == ' ' || *q == '\t')) q++;
    if (q >= nl || *q != '"') {
        fprintf(stderr, "asm: malformed externlib path\n");
        return -1;
    }
    char *path = parse_quoted(alloc, &q, nl);
    if (!path) {
        fprintf(stderr, "asm: malformed externlib path\n");
        return -1;
    }
    while (q < nl && (*q == ' ' || *q == '\t')) q++;
    if (q + 2 > nl || memcmp(q, "as", 2) != 0) {
        fprintf(stderr, "asm: expected 'as <alias>' in externlib line\n");
        allocator_free(alloc, (void **)&path);
        return -1;
    }
    q += 2;
    while (q < nl && (*q == ' ' || *q == '\t')) q++;
    const char *ae = q;
    while (ae < nl && *ae != ' ' && *ae != '\t' && *ae != '\r') ae++;
    size_t alen = (size_t)(ae - q);
    if (alen == 0) {
        fprintf(stderr, "asm: missing alias in externlib line\n");
        allocator_free(alloc, (void **)&path);
        return -1;
    }
    char *alias = (char *)allocator_new_ex(alloc, "clux.vm.graph.libalias",
                                           alen + 1, NULL, NULL, NULL, 1);
    if (!alias) {
        allocator_free(alloc, (void **)&path);
        return -1;
    }
    memcpy(alias, q, alen);
    alias[alen] = '\0';
    if (section_add_ffi(alloc, sec, path, alias) != 0) {
        allocator_free(alloc, (void **)&path);
        allocator_free(alloc, (void **)&alias);
        return -1;
    }
    return 0;
}

int bcode_graph_asm_parse(allocator_t *alloc, const char *text, size_t len,
                          bcode_graph_t *out) {
    if (!out) return -1;
    bcode_graph_init(out);
    if (!alloc || !text) {
        fprintf(stderr, "asm: invalid input\n");
        return 1;
    }

    /* 先切段：指令行只出现在行首（无反汇编缩进），因此按行首关键字切分 */
    asm_section_t *secs = NULL;
    size_t nsec = 0, cap = 0;
    const char *p = text;
    const char *end = text + len;

    asm_section_t *cur = NULL;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        const char *nl = line_end ? line_end : end;
        const char *next = line_end ? line_end + 1 : end;
        size_t llen = (size_t)(nl - p);

        bool is_mod = (llen >= 6 && memcmp(p, "MODULE", 6) == 0 &&
                       (llen == 6 || p[6] == ' ' || p[6] == '\t' ||
                        p[6] == '\r'));
        bool is_exp = (llen >= 7 && memcmp(p, "export ", 7) == 0);
        bool is_ffi = (llen >= 10 && memcmp(p, "externlib ", 10) == 0);

        if (is_mod || is_exp || is_ffi) {
            if (is_mod || !cur) {
                /* 新段 */
                if (nsec == cap) {
                    size_t nc = cap ? cap * 2 : 4;
                    asm_section_t *ns = (asm_section_t *)allocator_new_ex(
                        alloc, "clux.vm.graph.secs", sizeof(asm_section_t) * nc,
                        NULL, NULL, NULL, 1);
                    if (!ns) goto oom;
                    if (nsec) memcpy(ns, secs, sizeof(asm_section_t) * nsec);
                    if (secs) allocator_free(alloc, (void **)&secs);
                    secs = ns;
                    cap = nc;
                }
                cur = &secs[nsec++];
                memset(cur, 0, sizeof *cur);
            cur->importer_index = BCODE_NO_MODULE;
                cur->code.alloc = alloc;
                cur->path = NULL; /* 暂记 NULL：结束时统一判定入口 */
                cur->importer_index = BCODE_NO_MODULE;
                if (is_mod) {
                    const char *q = p + 6;
                    while (q < nl && (*q == ' ' || *q == '\t')) q++;
                    if (q < nl && *q == '"') {
                        char *path = parse_quoted(alloc, &q, nl);
                        if (!path) {
                            fprintf(stderr, "asm: malformed MODULE path\n");
                            goto fail;
                        }
                        cur->path = path;
                        /* `MODULE "<源码路径>" <importer_index>`：尾部下标是
                           导入者模块下标（容器的键由它与 import_path 推导）。 */
                        while (q < nl && (*q == ' ' || *q == '\t')) q++;
                        if (q < nl && *q != '\r')
                            cur->importer_index = (uint32_t)strtoul(q, NULL, 10);
                    } else {
                        /* 裸 MODULE = 入口模块（无导入者、路径为空） */
                        cur->path = (char *)allocator_new_ex(
                            alloc, "clux.vm.graph.path", 1, NULL, NULL, NULL, 1);
                        if (!cur->path) goto oom;
                        cur->path[0] = '\0';
                        cur->importer_index = BCODE_NO_MODULE;
                    }
                }
                if (is_exp) {
                    /* MODULE 与 export 同行的情况不存在：export 只改写当前段 */
                    const char *q = p + 7;
                    const char *sp = q;
                    while (sp < nl && *sp != ' ' && *sp != '\t' && *sp != '\r')
                        sp++;
                    size_t nlen = (size_t)(sp - q);
                    char *name = (char *)allocator_new_ex(
                        alloc, "clux.vm.graph.exname", nlen + 1, NULL, NULL,
                        NULL, 1);
                    if (!name) goto oom;
                    if (nlen) memcpy(name, q, nlen);
                    name[nlen] = '\0';
                    while (sp < nl && (*sp == ' ' || *sp == '\t')) sp++;
                    unsigned long owner = strtoul(sp, NULL, 10);
                    if (section_add_export(alloc, cur, name, (uint32_t)owner) !=
                        0) {
                        allocator_free(alloc, (void **)&name);
                        goto oom;
                    }
                }
                if (is_ffi) {
                    if (parse_externlib_line(alloc, p, nl, cur) != 0) goto fail;
                }
                p = next;
                continue;
            }
            if (is_ffi) {
                /* externlib 行（cur 非空）：追加到当前段 */
                if (parse_externlib_line(alloc, p, nl, cur) != 0) goto fail;
                p = next;
                continue;
            }
            /* export 行（cur 非空）：解析名字 + owner */
            const char *q = p + 7;
            const char *sp = q;
            while (sp < nl && *sp != ' ' && *sp != '\t' && *sp != '\r') sp++;
            size_t nlen = (size_t)(sp - q);
            char *name = (char *)allocator_new_ex(
                alloc, "clux.vm.graph.exname", nlen + 1, NULL, NULL, NULL, 1);
            if (!name) goto oom;
            if (nlen) memcpy(name, q, nlen);
            name[nlen] = '\0';
            while (sp < nl && (*sp == ' ' || *sp == '\t')) sp++;
            unsigned long owner = strtoul(sp, NULL, 10);
            if (section_add_export(alloc, cur, name, (uint32_t)owner) != 0) {
                allocator_free(alloc, (void **)&name);
                goto oom;
            }
            p = next;
            continue;
        }

        /* 普通代码行：归入当前段（无段时先建默认入口段） */
        if (!cur) {
            if (nsec == cap) {
                size_t nc = cap ? cap * 2 : 4;
                asm_section_t *ns = (asm_section_t *)allocator_new_ex(
                    alloc, "clux.vm.graph.secs", sizeof(asm_section_t) * nc,
                    NULL, NULL, NULL, 1);
                if (!ns) goto oom;
                if (nsec) memcpy(ns, secs, sizeof(asm_section_t) * nsec);
                if (secs) allocator_free(alloc, (void **)&secs);
                secs = ns;
                cap = nc;
            }
            cur = &secs[nsec++];
            memset(cur, 0, sizeof *cur);
            cur->importer_index = BCODE_NO_MODULE;
            cur->code.alloc = alloc;
        }
        tb_append(&cur->code, p, llen);
        tb_append(&cur->code, "\n", 1);
        p = next;
    }

    if (nsec == 0) {
        fprintf(stderr, "asm: no module section found\n");
        goto fail;
    }

    /* 组装图：入口 = 第一段 path 为空者（裸 MODULE / 单模块默认） */
    bcode_module_image_t *mods = (bcode_module_image_t *)allocator_new_ex(
        alloc, "clux.vm.graph.mods", sizeof(bcode_module_image_t) * nsec, NULL,
        NULL, NULL, 1);
    if (!mods) goto oom;
    memset(mods, 0, sizeof(bcode_module_image_t) * nsec);

    size_t entry = (size_t)-1;
    for (size_t i = 0; i < nsec; i++) {
        asm_section_t *s = &secs[i];
        char *path = s->path;
        if (!path) {
            path = (char *)allocator_new_ex(alloc, "clux.vm.graph.path", 1, NULL,
                                            NULL, NULL, 1);
            if (!path) goto oom;
            path[0] = '\0';
        }
        mods[i].import_path = path;
        mods[i].importer_index = s->importer_index;
        mods[i].export_names = s->names;
        mods[i].export_owners = s->owners;
        mods[i].export_count = s->ecount;
        s->names = NULL;
        s->owners = NULL;
        /* M6 FFI：extern lib 库表（所有权转移给模块映像） */
        mods[i].ffi_aliases = s->ffi_aliases;
        mods[i].ffi_paths = s->ffi_paths;
        mods[i].ffi_count = s->fcount;
        s->ffi_aliases = NULL;
        s->ffi_paths = NULL;
        s->fcount = 0;
        if (path[0] == '\0' && entry == (size_t)-1) entry = i;

        size_t clen = s->code.len;
        bytecode_t *bc = NULL;
        if (clen > 0) {
            if (bcode_asm_parse(alloc, s->code.data ? s->code.data : "", clen,
                                &bc) != 0) {
                fprintf(stderr, "asm: module %zu failed to assemble\n", i);
                goto fail;
            }
        } else {
            bc = bcode_new(alloc);
            if (!bc) goto oom;
        }
        mods[i].bc = bc;
    }
    if (entry == (size_t)-1) {
        fprintf(stderr, "asm: no entry module (expected one module without path)\n");
        goto fail;
    }

    for (size_t i = 0; i < nsec; i++) {
        if (secs[i].code.data) allocator_free(alloc, (void **)&secs[i].code.data);
    }
    allocator_free(alloc, (void **)&secs);

    out->alloc = alloc;
    out->modules = mods;
    out->count = nsec;
    out->entry_index = entry;
    return 0;

oom:
    fprintf(stderr, "asm: out of memory\n");
fail:
    if (secs) {
        for (size_t i = 0; i < nsec; i++) {
            if (secs[i].code.data) allocator_free(alloc, (void **)&secs[i].code.data);
            if (secs[i].path) allocator_free(alloc, (void **)&secs[i].path);
            for (size_t e = 0; e < secs[i].ecount; e++) {
                if (secs[i].names && secs[i].names[e])
                    allocator_free(alloc, (void **)&secs[i].names[e]);
            }
            if (secs[i].names) allocator_free(alloc, (void **)&secs[i].names);
            if (secs[i].owners) allocator_free(alloc, (void **)&secs[i].owners);
            for (size_t f = 0; f < secs[i].fcount; f++) {
                if (secs[i].ffi_aliases && secs[i].ffi_aliases[f])
                    allocator_free(alloc, (void **)&secs[i].ffi_aliases[f]);
                if (secs[i].ffi_paths && secs[i].ffi_paths[f])
                    allocator_free(alloc, (void **)&secs[i].ffi_paths[f]);
            }
            if (secs[i].ffi_aliases)
                allocator_free(alloc, (void **)&secs[i].ffi_aliases);
            if (secs[i].ffi_paths)
                allocator_free(alloc, (void **)&secs[i].ffi_paths);
        }
        allocator_free(alloc, (void **)&secs);
    }
    return 1;
}

int bcode_graph_asm_from_file(allocator_t *alloc, const char *path,
                              bcode_graph_t *out) {
    if (!out) return -1;
    bcode_graph_init(out);
    if (!alloc || !path) {
        fprintf(stderr, "asm: invalid input\n");
        return 1;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "asm: cannot open file '%s'\n", path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    if (sz < 0) {
        fclose(fp);
        fprintf(stderr, "asm: cannot size '%s'\n", path);
        return 1;
    }
    char *buf = (char *)allocator_new_ex(alloc, "clux.vm.graph.file",
                                         (size_t)sz + 1, NULL, NULL, NULL, 1);
    if (!buf) {
        fclose(fp);
        fprintf(stderr, "asm: out of memory\n");
        return 1;
    }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';
    int rc = bcode_graph_asm_parse(alloc, buf, got, out);
    allocator_free(alloc, (void **)&buf);
    return rc;
}
