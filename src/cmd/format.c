#include "cmd/format.h"
#include "cmd/path.h"
#include "core/allocator.h"
#include "core/stream.h"
#include "core/vec.h"
#include "parser/fmt.h"
#include "parser/lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* clux format：格式化 clux 源码。
 *
 *   clux format <file.cx>            原地格式化（写回源文件）
 *   clux format <file.cx> -o PATH    输出到 PATH
 *   clux format -                    从 stdin 读，结果写 stdout
 *
 * 最小版：基于 token 流规整空白/缩进（4 空格缩进、K&R 花括号、';' 后换行），
 * 不重排代码结构，注释原样保留。 */

static const char *USAGE =
    "usage: clux format <file.cx> [-o PATH]\n"
    "       clux format -          (read stdin, write stdout)\n";

/* 读文件全部内容（alloc 管理，返回 NULL 表示失败）。 */
static char *read_all(allocator_t *alloc, const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long size = ftell(fp);
    if (size < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    size_t n = (size_t)size;
    char *buf = (char *)allocator_new_ex(
        alloc, "clux.cmd.format.src", n > 0 ? n : 1, NULL, NULL, NULL, 1);
    size_t rd = fread(buf, 1, n, fp);
    fclose(fp);
    if (rd != n) { allocator_free(alloc, (void **)&buf); return NULL; }
    *out_len = n;
    return buf;
}

/* 从 stdin 读取全部内容。 */
static char *read_stdin(allocator_t *alloc, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = (char *)allocator_new_ex(
        alloc, "clux.cmd.format.stdin", cap, NULL, NULL, NULL, 1);
    for (;;) {
        if (len == cap) {
            size_t nc = cap * 2;
            char *nb = (char *)allocator_new_ex(
                alloc, "clux.cmd.format.stdin", nc, NULL, NULL, NULL, 1);
            memcpy(nb, buf, len);
            allocator_free(alloc, (void **)&buf);
            buf = nb;
            cap = nc;
        }
        size_t rd = fread(buf + len, 1, cap - len, stdin);
        if (rd == 0) break;
        len += rd;
    }
    *out_len = len;
    return buf;
}

/* 写文件（覆盖）。返回 0 成功。 */
static int write_all(const char *path, const char *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t wr = fwrite(data, 1, len, fp);
    int cf = fclose(fp);
    return (wr == len && cf == 0) ? 0 : -1;
}

/* 核心：源码文本 → 格式化文本（alloc 管理）。失败返回 NULL。 */
static char *format_source(allocator_t *alloc, const char *src, size_t len,
                           size_t *out_len) {
    /* AST-based 格式化：内部 lexer → parser（recover_partial）→ 渲染 */
    return fmt_format_source(alloc, src, len, out_len);
}

int cmd_format(const cmd_args_t *args) {
    if (!args) return 1;

    /* 解析单横线 -o（cmd_args_parse 只识别 --，故落入位置参数）；
     * 过滤后 pos[0]=输入、pos[1]=输出。 */
    const char *pos[4];
    size_t np = 0;
    const char *opt_out = NULL;
    if (cmd_take_short_output(args, &opt_out, pos, &np, 4)) {
        fprintf(stderr, "format: -o requires a path\n");
        fputs(USAGE, stderr);
        return 1;
    }

    const char *path = (np > 0) ? pos[0] : NULL;
    if (!path) {
        fprintf(stderr, "format: missing input file\n");
        fputs(USAGE, stderr);
        return 1;
    }

    allocator_t *alloc = create_allocator(malloc, free);
    if (!alloc) {
        fprintf(stderr, "format: out of memory\n");
        return 1;
    }

    bool from_stdin = (strcmp(path, "-") == 0);

    size_t len = 0;
    char *src = from_stdin ? read_stdin(alloc, &len)
                           : read_all(alloc, path, &len);
    if (!src) {
        fprintf(stderr, "format: cannot read '%s'\n", path);
        delete_allocator(&alloc);
        return 1;
    }

    size_t out_len = 0;
    char *formatted = format_source(alloc, src, len, &out_len);

    int rc = 0;
    if (!formatted) {
        /* 诊断已打印 */
        rc = 1;
    } else if (from_stdin) {
        /* 原样写出：fmt_format 已保证结果以换行结尾，不再追加 */
        fwrite(formatted, 1, out_len, stdout);
    } else {
        /* 输出目标：-o/--output 指定，否则位置参数，否则原地写回 */
        const char *out_path = opt_out;
        if (!out_path) out_path = cmd_args_get(args, "output");
        if (!out_path && np > 1) out_path = pos[1];
        if (!out_path) out_path = path;   /* 原地格式化 */

        if (write_all(out_path, formatted, out_len) != 0) {
            fprintf(stderr, "format: cannot write '%s'\n", out_path);
            rc = 1;
        } else if (strcmp(out_path, path) == 0) {
            fprintf(stdout, "formatted %s\n", path);
        } else {
            fprintf(stdout, "wrote %s\n", out_path);
        }
    }

    allocator_free(alloc, (void **)&formatted);
    allocator_free(alloc, (void **)&src);
    delete_allocator(&alloc);
    return rc;
}
