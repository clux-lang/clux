#include "cmd/run.h"
#include "cmd/path.h"
#include "cmd/package.h"
#include "driver/driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* clux run：运行 clux 程序。
 *
 *   clux run                  从 clux.mod 的 entry 查找入口文件
 *   clux run <subpkg>         多 entry 项目指定子包名
 *   clux run <file.cx>        编译并执行指定源码文件
 *   clux run <file.cxb>       加载并执行二进制字节码
 *
 * 无文件名参数时自动查找 clux.mod：
 *   - 单 entry → 直接用 entry 路径
 *   - 多 entry → 第一个位置参数当子包名（如 std/array）
 *
 * 文件参数以 .cx 或 .cxb 结尾时直接当文件处理，不查 clux.mod。
 * 二进制/源码由**内容**区分：以 "CXBC" 二进制头开头者当字节码执行。 */

static const char *USAGE =
    "usage: clux run [subpkg|<file>] [-D KEY=VALUE]...\n";

/* 源码运行：main 的整数返回值即进程退出码（与 clux build 产物一致，
   差分测试依赖这一点）。 */
static int run_source(const char *path) {
    int code = 0;
    int rc = driver_run_file_exit(path, &code);
    return rc != 0 ? rc : code;
}

/* 判断字符串是否像文件路径：
 *   - 以 "./" 或 ".\" 或 "../" 或 "..\" 开头（相对路径，/ 与 \ 均可）
 *   - 以 "/" 或 "\" 开头（绝对路径）
 *   - 以盘符开头如 "C:/" 或 "C:\"（Windows 绝对路径）
 * 子包名如 "std/array" 不匹配上述任何条件 → 不是文件 */
static int looks_like_file(const char *s) {
    if (!s || !s[0]) return 0;
    /* 相对路径：./ .\ ../ ..\ */
    if (s[0] == '.') {
        if (s[1] == '/' || s[1] == '\\') return 1;
        if (s[1] == '.' && (s[2] == '/' || s[2] == '\\')) return 1;
    }
    /* 绝对路径：/ 或 \ */
    if (s[0] == '/' || s[0] == '\\') return 1;
    /* Windows 绝对路径：盘符:/ 或盘符:\ */
    if (((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')) &&
        s[1] == ':' && (s[2] == '/' || s[2] == '\\'))
        return 1;
    return 0;
}

int cmd_run(const cmd_args_t *args) {
    if (!args) return 1;

    /* `-D KEY=VALUE`（可重复）：注入 extern lib 的 assert 条件键表。
       剥离后其余位置参数才是"子包名 / 文件路径"。 */
    const char *pos[16];
    size_t np = 0;
    const char *kv[32];
    size_t nkv = 0;
    int derr = cmd_take_defines((const char *const *)args->posargs, args->posc, pos, &np, 16, kv,
                                &nkv, 32);
    if (derr != 0) {
        fprintf(stderr, derr == 1 ? "run: -D requires KEY=VALUE\n"
                                  : "run: too many -D options\n");
        fputs(USAGE, stderr);
        return 1;
    }
    if (cmd_apply_defines(kv, nkv) != 0) return 1;

    const char *arg0 = np > 0 ? pos[0] : NULL;

    /* 无参数：从 clux.mod 查找入口 */
    if (!arg0) {
        char *entry = NULL;
        int rc = cmd_resolve_entry(NULL, &entry, NULL);
        if (rc != 0) return rc;

        /* 内容判定 */
        if (driver_detect_input(entry) == DRIVER_INPUT_CXB) {
            rc = driver_run_bin(entry);
        } else {
            rc = run_source(entry);
        }
        free(entry);
        return rc;
    }

    /* 单横线选项 → 报错 */
    if (arg0[0] == '-' && arg0[1] != '\0') {
        fprintf(stderr,
                "run: unknown option '%s' (clux options use '--')\n", arg0);
        fputs(USAGE, stderr);
        return 1;
    }

    /* 看起来像文件路径 → 直接当文件处理 */
    if (looks_like_file(arg0)) {
        if (driver_detect_input(arg0) == DRIVER_INPUT_CXB) {
            return driver_run_bin(arg0);
        }
        return run_source(arg0);
    }

    /* 非文件路径 → 当子包名从 clux.mod 查找 */
    char *entry = NULL;
    int rc = cmd_resolve_entry(arg0, &entry, NULL);
    if (rc != 0) return rc;

    if (driver_detect_input(entry) == DRIVER_INPUT_CXB) {
        rc = driver_run_bin(entry);
    } else {
        rc = run_source(entry);
    }
    free(entry);
    return rc;
}
