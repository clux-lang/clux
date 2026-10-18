#include "cmd/bc.h"
#include "cmd/path.h"
#include "driver/driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* clux bc：字节码工具（非主线）。
 *
 * 字节码（.cxb）与汇编文本（.cxs）都是非主线产物——主线产物是 build 未来
 * 输出的机器码二进制。本命令把一切字节码相关的产出与互转集中于此：
 *
 *   clux bc emit   <file.cx>  [-o PATH]   源码 → .cxb 二进制字节码
 *   clux bc asm    <file.cxs> [-o PATH]   .cxs 汇编文本 → .cxb 二进制
 *   clux bc disasm <file.cxb> [-o PATH]   .cxb 二进制 → .cxs 汇编文本
 *
 * 输出路径省略时按输入同名换扩展名；亦可用第二位置参数代替 -o。 */

static const char *USAGE =
    "usage: clux bc <emit|asm|disasm> <file> [-o PATH] [-D KEY=VALUE]...\n"
    "\n"
    "  emit    <file.cx>   compile source into .cxb binary bytecode\n"
    "  asm     <file.cxs>  assemble .cxs text assembly into .cxb binary\n"
    "  disasm  <file.cxb>  disassemble .cxb binary bytecode into .cxs text\n"
    "\n"
    "Output defaults to the input name with .cxb / .cxs extension.\n";

/* 子操作 → 输出扩展名；未知返回 NULL。 */
static const char *op_out_ext(const char *op) {
    if (strcmp(op, "emit") == 0 || strcmp(op, "asm") == 0) return ".cxb";
    if (strcmp(op, "disasm") == 0) return ".cxs";
    return NULL;
}

int cmd_bc(const cmd_args_t *args) {
    if (!args) return 1;

    /* 解析单横线 -o（cmd_args_parse 只识别 --，故落入位置参数）；
     * 过滤后 pos[0]=子操作、pos[1]=输入、pos[2]=输出。 */
    const char *pos[8];
    size_t np = 0;
    const char *opt_out = NULL;

    if (cmd_take_short_output(args, &opt_out, pos, &np, 8)) {
        fprintf(stderr, "bc: -o requires a path\n");
        fputs(USAGE, stderr);
        return 1;
    }

    /* `-D KEY=VALUE`（可重复）：emit 编译时注入 extern lib 的 assert 条件键。
       先剥离再取子操作/输入/输出，顺序与 `-o` 无关。 */
    const char *dpos[8];
    size_t dnp = 0;
    const char *kv[32];
    size_t nkv = 0;
    int derr = cmd_take_defines(pos, np, dpos, &dnp, 8, kv, &nkv, 32);
    if (derr != 0) {
        fprintf(stderr, derr == 1 ? "bc: -D requires KEY=VALUE\n"
                                  : "bc: too many -D options\n");
        fputs(USAGE, stderr);
        return 1;
    }
    if (cmd_apply_defines(kv, nkv) != 0) return 1;
    np = dnp;
    for (size_t i = 0; i < dnp; i++) pos[i] = dpos[i];

    const char *op = (np > 0) ? pos[0] : NULL;
    if (!op) {
        fprintf(stderr, "bc: missing subcommand\n");
        fputs(USAGE, stderr);
        return 1;
    }

    const char *ext = op_out_ext(op);
    if (!ext) {
        fprintf(stderr, "bc: unknown subcommand '%s'\n", op);
        fputs(USAGE, stderr);
        return 1;
    }

    const char *in_path = (np > 1) ? pos[1] : NULL;
    if (!in_path) {
        fprintf(stderr, "bc: missing input file\n");
        fputs(USAGE, stderr);
        return 1;
    }

    /* 输出路径优先级：-o/--output > 第三位置参数 > 按输入推导。 */
    const char *out_path = opt_out;
    if (!out_path) out_path = cmd_args_get(args, "output");
    if (!out_path && np > 2) out_path = pos[2];

    char *owned = NULL;
    if (!out_path) {
        owned = cmd_derive_out_path(in_path, ext);
        if (!owned) {
            fprintf(stderr, "bc: out of memory\n");
            return 1;
        }
        out_path = owned;
    }

    int rc;
    if (strcmp(op, "emit") == 0) {
        rc = driver_build_bin(in_path, out_path);   /* 源码 → .cxb */
    } else if (strcmp(op, "asm") == 0) {
        rc = driver_asm_to_bin(in_path, out_path);  /* .cxs → .cxb */
    } else {
        rc = driver_bin_to_asm(in_path, out_path);  /* .cxb → .cxs */
    }

    if (rc == 0) fprintf(stdout, "wrote %s\n", out_path);
    free(owned);
    return rc;
}
