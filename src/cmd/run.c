#include "cmd/run.h"
#include "cmd/build.h"
#include "cmd/path.h"
#include "cmd/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

/* clux run：C 后端编译并执行。
 *
 *   clux run                  从 clux.mod 的 entry 查找入口文件
 *   clux run <subpkg>         多 entry 项目指定子包名
 *   clux run <file.cx>        编译（C 后端）并执行
 *
 * 构建产物的位置与 `clux build` 一致（默认 build/ 镜像目录），便于调试时
 * 直接检查生成的 exe；`--emit-c` 是 build 专用选项（只生成 C 不链接），
 * run 下报错提示改用 `clux build --emit-c`。 */

static const char *USAGE =
    "usage: clux run [subpkg|<file.cx>] [-D KEY=VALUE]...\n";

static int run_executable(const char *exe) {
#ifdef _WIN32
    const char *argv[] = {exe, NULL};
    intptr_t rc = _spawnv(_P_WAIT, exe, argv);
    return rc < 0 ? 1 : (int)rc;
#else
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        execl(exe, exe, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

/* 判断字符串是否像文件路径：
 *   - 以 "./" 或 ".\" 或 "../" 或 "..\" 开头（相对路径，/ 与 \ 均可）
 *   - 以 "/" 或 "\" 开头（绝对路径）
 *   - 以盘符开头如 "C:/" 或 "C:\"（Windows 绝对路径）
 * 子包名如 "std/array" 不匹配上述任何条件 → 不是文件 */
static int looks_like_file(const char *s) {
    if (!s || !s[0]) return 0;
    if (s[0] == '.') {
        if (s[1] == '/' || s[1] == '\\') return 1;
        if (s[1] == '.' && (s[2] == '/' || s[2] == '\\')) return 1;
    }
    if (s[0] == '/' || s[0] == '\\') return 1;
    if (((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')) &&
        s[1] == ':' && (s[2] == '/' || s[2] == '\\'))
        return 1;
    return 0;
}

static int ends_with_ci(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n) return 0;
    s += n - m;
    for (size_t i = 0; i < m; i++) {
        char a = s[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* 编译 + 运行一个源文件；返回 main 的退出码（编译失败返回 1）。 */
static int build_and_run(const cmd_args_t *args, const char *path) {
    if (ends_with_ci(path, ".cxb") || ends_with_ci(path, ".cxs")) {
        fprintf(stderr,
                "run: bytecode input is no longer supported (C backend only); "
                "pass a .cx source file\n");
        return 1;
    }
    if (cmd_args_has(args, "emit-c")) {
        fprintf(stderr,
                "run: --emit-c only generates C without linking; use "
                "`clux build <file> --emit-c` for inspection\n");
        return 1;
    }
    char *exe = NULL;
    int rc = cmd_build_for_run(args, path, &exe);
    if (rc != 0) return rc;
    if (!exe) {
        fprintf(stderr, "run: build did not produce an executable\n");
        return 1;
    }
    int code = run_executable(exe);
    free(exe);
    return code;
}

int cmd_run(const cmd_args_t *args) {
    if (!args) return 1;

    /* `-D KEY=VALUE`（可重复）：注入 extern lib 的 assert 条件键表。
       剥离后其余位置参数才是"子包名 / 文件路径"；实际应用由 build 侧统一
       处理（同一份 args 透传）。 */
    const char *pos[16];
    size_t np = 0;
    const char *kv[32];
    size_t nkv = 0;
    int derr = cmd_take_defines((const char *const *)args->posargs, args->posc,
                                pos, &np, 16, kv, &nkv, 32);
    if (derr != 0) {
        fprintf(stderr, derr == 1 ? "run: -D requires KEY=VALUE\n"
                                  : "run: too many -D options\n");
        fputs(USAGE, stderr);
        return 1;
    }
    (void)kv;
    (void)nkv;

    const char *arg0 = np > 0 ? pos[0] : NULL;

    /* 无参数：从 clux.mod 查找入口 */
    if (!arg0) {
        char *entry = NULL;
        int rc = cmd_resolve_entry(NULL, &entry, NULL);
        if (rc != 0) return rc;
        rc = build_and_run(args, entry);
        free(entry);
        return rc;
    }

    if (arg0[0] == '-' && arg0[1] != '\0') {
        fprintf(stderr,
                "run: unknown option '%s' (clux options use '--')\n", arg0);
        fputs(USAGE, stderr);
        return 1;
    }

    /* 看起来像文件路径 → 直接编译执行 */
    if (looks_like_file(arg0)) return build_and_run(args, arg0);

    /* 非文件路径 → 当子包名从 clux.mod 查找 */
    char *entry = NULL;
    int rc = cmd_resolve_entry(arg0, &entry, NULL);
    if (rc != 0) return rc;
    rc = build_and_run(args, entry);
    free(entry);
    return rc;
}
