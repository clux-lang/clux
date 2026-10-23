#include "cmd/build.h"
#include "cmd/cmd.h"
#include "cbackend/c_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *default_output(const char *src) {
    size_t n = strlen(src);
    const char *dot = NULL;
    for (size_t i = n; i > 0; i--) {
        if (src[i - 1] == '.') {
            dot = src + i - 1;
            break;
        }
        if (src[i - 1] == '/' || src[i - 1] == '\\') break;
    }
    size_t stem = dot ? (size_t)(dot - src) : n;
#ifdef _WIN32
    const char *suffix = ".exe";
#else
    const char *suffix = "";
#endif
    size_t slen = strlen(suffix);
    char *out = (char *)malloc(stem + slen + 1);
    if (!out) return NULL;
    memcpy(out, src, stem);
    memcpy(out + stem, suffix, slen);
    out[stem + slen] = '\0';
    return out;
}

int cmd_build(const cmd_args_t *args) {
    if (!args || args->posc != 1) {
        fprintf(stderr, "usage: clux build <file.cx>\n");
        return 1;
    }
    const char *src = args->posargs[0];
    char *out = default_output(src);
    if (!out) {
        fprintf(stderr, "build: out of memory\n");
        return 1;
    }
    int rc = cbackend_build_file(src, out);
    if (rc == 0) printf("built %s\n", out);
    free(out);
    return rc;
}
