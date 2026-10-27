#include "cmd/cmd.h"
#include "cmd/format.h"
#include "cmd/build.h"
#include "cmd/run.h"
#include "cmd/test.h"
#include "cmd/install.h"
#include "cmd/version.h"
#include "icu_data.h"
#include <locale.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ---- Command table ---- */

static const cmd_t g_cmds[] = {
    {
        .name = "format",
        .usage = "clux format [<file.cx> | -o PATH]",
        .help =
            "Format clux source code.\n"
            "\n"
            "  clux format                    format all .cx files in package\n"
            "  clux format <file.cx>          format in place\n"
            "  clux format <file.cx> -o PATH  write result to PATH\n"
            "  clux format -                  read stdin, write stdout\n"
            "\n"
            "Without a file argument, finds clux.mod and formats every .cx\n"
            "source file in the package (recursively, excluding deps/).\n"
            "\n"
            "Style: 4-space indent, K&R braces ('{' on the same line,\n"
            "'}' on its own line, empty block stays '{}'), newline after ';'.",
        .handler = cmd_format,
    },
    {
        .name = "build",
        .usage = "clux build <file.cx> [--emit-c[=PATH]] [--emit-obj[=PATH]] "
                 "[--keep-c] [--build-dir=DIR] [--output=PATH] [--locked] "
                 "[--cc=PATH] [--cflag=FLAG]... [--ldflag=FLAG]... [-I DIR]... "
                 "[-L DIR]... [-l LIB]... [-D KEY=VALUE]...",
        .help =
            "Compile clux source into a native executable.\n"
            "\n"
            "  clux build <file.cx>              build into the mirrored build dir\n"
            "  clux build <file.cx> --emit-c     write C source only (no cc call)\n"
            "  clux build <file.cx> --emit-c=P   write C source to P\n"
            "  clux build <file.cx> --emit-obj[=P]  compile to an object only\n"
            "  clux build <file.cx> --keep-c    keep generated C after compiling\n"
            "  clux build <file.cx> --output=P   executable path P\n"
            "  clux build <file.cx> --locked     verify clux.lock first\n"
            "\n"
            "Pipeline: .cx -> .c/.h -> (cc -c) -> object -> link -> executable.\n"
            "Like cmake, outputs mirror the source tree under the build dir\n"
            "(default ./build):  ./cmd/main.cx -> ./build/cmd/main.c,\n"
            "./build/cmd/main.h, ./build/cmd/main.obj (or .o) and\n"
            "./build/cmd/main.exe (or ./build/cmd/main). Sources already\n"
            "inside the build dir keep their relative location there.\n"
            "--build-dir=DIR (or -B DIR) overrides the build directory;\n"
            "--output=P still overrides the executable path only.\n"
            "\n"
            "Generated C is a compilation unit: the .h declares exported\n"
            "types (topological order), `export func`/`export cfunc`\n"
            "prototypes and `export var` globals; everything else is static\n"
            "in the .c. All module symbols are prefixed with a short hash of\n"
            "the source path relative to the project root (clux_<hash>_,\n"
            "e.g. clux_a61b_main). The .c is removed after a successful\n"
            "build; the .h and the object are kept.\n"
            "\n"
            "import \"<module>\" compiles every dependency as its own\n"
            "compilation unit (mirrored under the build dir) and links all\n"
            "objects together; cross-module calls resolve to the defining\n"
            "module's exported symbols. Library modules without main can be\n"
            "built with --emit-c/--emit-obj.\n"
            "\n"
            "The host C compiler is $CC (clang on Windows, cc elsewhere) unless\n"
            "--cc is given.\n"
            "\n"
            "--locked verifies clux.lock (package clux.mod hashes, postinstall\n"
            "commands/scripts and native artifact hashes) before building; run\n"
            "`clux install` if it is stale.\n"
            "\n"
            "Toolchain options (passed through to the host compiler):\n"
            "  --cc=PATH            C compiler\n"
            "  --cflag=FLAG         extra compile flag (repeatable)\n"
            "  --ldflag=FLAG        extra link flag (repeatable)\n"
            "  -I DIR, --include=DIR      include search dir (repeatable)\n"
            "  -L DIR, --lib-path=DIR     library search dir (repeatable)\n"
            "  -l LIB, --link-lib=LIB     link library (repeatable)\n"
            "  -D KEY=VALUE         build-time define (repeatable)\n"
            "\n"
            "  extern lib <alias> from \"<name>\":\n"
            "    \"*\" -> no link flag (symbols from the current process)\n"
            "    generic name -> -l<name> (searched in the output dir and its\n"
            "                    two parent dirs, then the system paths)\n"
            "    path / file name -> passed to the linker as-is\n"
            "  extern cfunc <alias>::<fn>(...): emits an `extern` prototype and\n"
            "  calls the real C symbol; cfunc_ptr(f) generates a C trampoline.\n"
            "  extern var <alias>::<name>: T; reads/writes a data symbol.",
        .handler = cmd_build,
    },
    {
        .name = "run",
        .usage = "clux run [subpkg|<file>] [-D KEY=VALUE]...",
        .help =
            "Run a clux program.\n"
            "\n"
            "  clux run                  find entry from clux.mod\n"
            "  clux run <subpkg>         run a sub-package (multi-entry package)\n"
            "  clux run <file>           run a specific file directly\n"
            "\n"
            "Without a file argument, finds clux.mod and resolves the entry.\n"
            "A relative/absolute path is treated as a file; anything else is\n"
            "a sub-package name.\n"
            "\n"
            "-D KEY=VALUE  inject a build-time define (repeatable). Keys are\n"
            "              visible only inside `extern lib ... assert(...)`;\n"
            "              builtin keys: OS_TYPE (WIN64/WIN32/LINUX/MACOS/BSD),\n"
            "              OS_ARCH (X64/X86/ARM64/ARM).\n"
            "\n"
            "Compiles with the C backend (host gcc/clang) into the build\n"
            "directory, then runs the produced executable. Use\n"
            "`clux build <file> --emit-c[=PATH]` to inspect generated C.",
        .handler = cmd_run,
    },
    {
        .name = "test",
        .usage = "clux test [<file.cx>] [--backend=vm]",
        .help =
            "Run tests.\n"
            "\n"
            "  clux test <file.cx>  run test blocks in one source file\n"
            "  clux test            run all .cx test blocks in the clux.mod package\n"
            "\n"
            "  --backend=c    (default) build a C test runner and run each test\n"
            "                 in its own subprocess\n"
            "  --backend=vm   transitional fallback to the old VM runner\n"
            "\n"
            "Test blocks are ignored by `build` / `run`. Results are printed in\n"
            "a gtest-like format; exits nonzero if any test fails.",
        .handler = cmd_test,
    },
    {
        .name = "install",
        .usage = "clux install [--allow-scripts] [--force] [--locked]",
        .help =
            "Run postinstall hooks declared by clux.mod.\n"
            "\n"
            "  clux install                  list hooks and refuse to run them\n"
            "  clux install --allow-scripts  execute hooks (deps first, then root)\n"
            "  clux install --force          run even if the hook already succeeded\n"
            "  clux install --locked         verify clux.lock; do not run or write\n"
            "\n"
            "Hooks are arbitrary package scripts and are never run by\n"
            "`clux build`. They execute with cwd = package root and receive\n"
            "CLUX_PKG_ROOT, CLUX_OS, CLUX_ARCH and CLUX_TARGET. A successful\n"
            "hook writes .clux/postinstall.ok so it is not repeated until the\n"
            "command changes; --force overrides that. After hooks, clux writes\n"
            "clux.lock with clux.mod/postinstall/artifact hashes.",
        .handler = cmd_install,
    },
    {
        .name = "version",
        .usage = "clux version",
        .help = "Print version information.",
        .handler = cmd_version,
    },
};

#define NUM_CMDS (sizeof(g_cmds) / sizeof(g_cmds[0]))

/* ---- UTF-8 locale setup (cross-platform) ---- */

/* Force the C runtime to treat text as UTF-8 so output is not mis-encoded
 * on terminals that default to a local code page (notably Windows cmd). */
static void setup_utf8_locale(void) {
#ifdef _WIN32
  setlocale(LC_ALL, ".UTF-8");
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#else
  /* Try an explicit UTF-8 locale first, then fall back to whatever the
   * environment provides; if all fail we leave the C locale as-is. */
  if (!setlocale(LC_ALL, "C.UTF-8") &&
      !setlocale(LC_ALL, "en_US.UTF-8")) {
    setlocale(LC_ALL, "");
  }
#endif
}

/* ---- main ---- */

int main(int argc, char *argv[]) {
  setup_utf8_locale();

  /* Register the embedded ICU data before any ICU API (lexer's
   * identifier / grapheme-cluster handling) is touched. */
  icu_data_init();
  return cmd_dispatch(g_cmds, NUM_CMDS, argc, argv);
}
