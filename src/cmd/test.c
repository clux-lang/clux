#include "cmd/test.h"
#include "cmd/cmd.h"
#include "cmd/path.h"
#include "cmd/package.h"
#include "cbackend/c_backend.h"
#include "driver/driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

/* ---- C 后端的 test 运行器 ------------------------------------------------
 *
 * `clux test <file.cx> --backend=c`：
 *   1. 把 test 块改写成无参 void 函数，生成按 argv[1] 选择单个用例的 C 可执行；
 *   2. 每个用例在独立子进程里运行——panic/assert 只影响该用例；
 *   3. 输出与 VM 测试运行器同款的 gtest 风格汇总。
 */

static int run_process(const char *cmd) {
    fflush(stdout);
    fflush(stderr);
    int status = system(cmd);
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

static char *runner_output_path(const char *src) {
#ifdef _WIN32
    return cmd_derive_out_path(src, ".tests.exe");
#else
    return cmd_derive_out_path(src, ".tests");
#endif
}

/* 显示名 "Suite.Name" → 套件名 "Suite"（无 '.' 时整个名字算一个套件）。 */
static size_t suite_prefix_len(const char *name) {
    const char *dot = name ? strchr(name, '.') : NULL;
    return dot ? (size_t)(dot - name) : (name ? strlen(name) : 0);
}

static int run_tests_c(const char *src) {
    char *exe = runner_output_path(src);
    if (!exe) {
        fprintf(stderr, "test: out of memory\n");
        return 1;
    }
    char **names = NULL;
    size_t count = 0;
    int rc = cbackend_build_tests(src, exe, &names, &count);
    if (rc != 0) {
        free(exe);
        return rc;
    }

    char suite_names[256][128];
    size_t suites = 0;
    for (size_t i = 0; i < count; i++) {
        size_t n = suite_prefix_len(names[i]);
        if (n >= sizeof suite_names[0]) n = sizeof suite_names[0] - 1;
        bool seen = false;
        for (size_t k = 0; k < suites; k++)
            if (strlen(suite_names[k]) == n &&
                memcmp(suite_names[k], names[i], n) == 0) {
                seen = true;
                break;
            }
        if (!seen && suites < 256) {
            if (n) memcpy(suite_names[suites], names[i], n);
            suite_names[suites][n] = '\0';
            suites++;
        }
    }

    printf("[==========] Running %zu tests from %zu test suites.\n", count,
           suites);
    size_t passed = 0, failed = 0;
    for (size_t i = 0; i < count; i++) {
        printf("[ RUN      ] %s\n", names[i]);
        fflush(stdout);
        clock_t start = clock();
        char cmd[4096];
        snprintf(cmd, sizeof cmd, "\"%s\" %zu", exe, i);
        int code = run_process(cmd);
        clock_t end = clock();
        double ms = (double)(end - start) * 1000.0 / (double)CLOCKS_PER_SEC;
        if (code == 0) {
            passed++;
            printf("[       OK ] %s (%.0f ms)\n", names[i], ms);
        } else {
            failed++;
            printf("[  FAILED  ] %s (%.0f ms, exit %d)\n", names[i], ms, code);
        }
        fflush(stdout);
    }
    printf("[==========] %zu tests from %zu test suites ran.\n", count, suites);
    printf("[  PASSED  ] %zu tests.\n", passed);
    if (failed) printf("[  FAILED  ] %zu tests.\n", failed);

    /* 调试开关：保留生成的运行器与 C 源，便于排查用例失败。 */
    if (!getenv("CLUX_KEEP_TEST_EXE")) {
        remove(exe);
        char *kept = (char *)malloc(strlen(exe) + sizeof(".clux.c"));
        if (kept) {
            sprintf(kept, "%s.clux.c", exe);
            remove(kept);
            free(kept);
        }
    }
    cbackend_free_test_names(names, count);
    free(exe);
    return failed == 0 ? 0 : 1;
}

int cmd_test(const cmd_args_t *args) {
    if (!args) return 1;

    const char *backend = cmd_args_get(args, "backend");
    bool want_c = cmd_args_has(args, "c") ||
                  (backend && strcmp(backend, "c") == 0);
    if (backend && strcmp(backend, "c") != 0 && strcmp(backend, "vm") != 0) {
        fprintf(stderr, "test: unknown backend '%s' (expected c or vm)\n",
                backend);
        return 1;
    }

    const char *path = args->posc > 0 ? args->posargs[0] : NULL;
    if (want_c) {
        if (!path) {
            fprintf(stderr,
                    "test: --backend=c requires a single <file.cx> argument\n");
            return 1;
        }
        return run_tests_c(path);
    }

    if (path) return driver_test_file(path);

    char *entry = NULL;
    char *root = NULL;
    int rc = cmd_resolve_entry(NULL, &entry, &root);
    if (rc != 0) return rc;
    free(entry);

    char **sources = NULL;
    size_t count = 0;
    rc = cmd_collect_sources(root, &sources, &count);
    free(root);
    if (rc != 0) return rc;
    rc = driver_test_sources((const char *const *)sources, count);
    for (size_t i = 0; i < count; i++) free(sources[i]);
    free(sources);
    return rc;
}
