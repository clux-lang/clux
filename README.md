# clux

> **C, but with LUX** — a minimal, statically-typed, C-like language.

clux is a small systems-language project experimenting with a clean, modern take on
C syntax: `var` type inference, `as` casts (no `(T)x`), strict `bool`, no implicit
conversions, and `{}` block statements. The compiler front-end is written in C11
using a hand-rolled "C-style OOP" style (opaque structs + vtable, all memory via a
custom `allocator_t`).

```
func main(): i32 {
    var name = "clux";
    printf("hello, %s\n", name);
    return 0;
}
```

## Status

M1–M5 are implemented and interpretable end-to-end (lex → parse → sema → bytecode
compile → VM). M6 (FFI) is designed but not started. See `docs/milestones.md`.

**Language features available today**

- **M1 — core**: `i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 bool str void`; `var` with
  inference or `: T` annotation; numeric/float suffixes (`7i8`, `2.5f32`) and
  multi-base literals (`0xFF`, `0o17`, `0b1010`); functions (no forward declarations);
  `if`/`else`, `while`, `for`, `break`/`continue`/`return`; full operator set with
  compound assignment; explicit `as` casts and **no implicit cross-type conversions**;
  `printf` builtin; duck-typed layout compatibility; TDZ (definite-assignment) analysis;
  compile-time evaluation (CTFE) for constant slots.
- **M2 — C-level expressiveness (minus pointers)**: `struct` / `enum` / tag `union` /
  `cunion`; static arrays `[N]T`; tuples `<T1,T2>` (with array↔tuple conversion);
  `switch` (no fallthrough); `do`/`while`; `type` aliases and compile-time type
  computation (`extends`, `==`, `?:`); `sizeof` / `alignof` / `typeof`; function types
  `func(...)->T` and function literals; optional `?T` with `nil` (no path narrowing) and
  `.!` unwrap; `comptime var` / `comptime func`; `const` / `volatile` as real types;
  fill value packs `.[N]T{ <v, M> }`; `.cxb` / `.cxs` bytecode toolchain;
  **`foreach`** — for-each sugar over arrays/slices, expanded by the front-end
  desugaring layer (`src/desugar/`) into an index `for` loop before sema, so
  sema / compiler / CTFE never see it (`tests/desugar_test.cpp`, `tests/foreach_test.cpp`).
- **M3 — pointers and ownership**: single-ownership `own *T` / `ref *T` / `fatal *T`
  with explicit scope annotations (`'<a>`), `move` / `clone`, `new T{...}`, postfix
  `.&` / `.*`, `opaque` (≈ `void*`), closure capture rules; RC family `share *T` /
  `weak *T` with atomic control blocks and the `upgrade` builtin.
- **M4 — slices and strings**: `own []T` / `ref []T` / `fatal []T` fat pointers,
  `make(T, N, init...)`, `len`, `s[a:b]` slicing (`[N]T → ref []T` is explicit),
  `str` slicing to `ref [] const u8`, bounds checks.
- **M5 — modules**: `export` on global declarations, `import alias from "./mod"` with
  `alias::member` access, **relative path resolution against the importing file's
  directory** (`./`, `../`, `\`→`/` normalization, `.`/`..` collapsing, implicit `.cx`),
  **proxy re-export** (`export * from "./mod"`) with conflict detection, module-scoped
  globals, and circular-dependency detection. Imports must be relative: any other path
  is rejected with one diagnostic — **import kind is never inferred from the file
  extension** (a `.so`/`.dll` suffix does not mean "dynamic library").

**Not yet implemented**

- M6 FFI 已实现（`extern lib <别名> from "<库名>"` + `extern cfunc` / `extern var`，
  libffi / dlopen；库名是**通用名**，按平台展开候选文件）。声明可带条件后缀
  `assert(OS_TYPE == "WIN64" ...)`，编译期裁剪不匹配的声明；键用 `-D KEY=VALUE`
  注入。详见 `docs/m6-design.md`。
- `clux build` (native machine-code backend, M7/M12): prints `not implemented`.
- `clux test` subcommand: prints `not implemented` — run `build/clux_test` or `ctest`.
- Error handling (M8) and a standard library (M9).

## Build

Requirements:

- CMake ≥ 3.14
- A C11 / C++20 toolchain (tested with Clang / GCC; MSVC-style drivers also work)
- ICU (Unicode support, pulled in via `third_party/icu`)
- GoogleTest (git submodule at `third_party/gtest`)

```sh
git submodule update --init --recursive   # fetch GoogleTest
cmake -S . -B build
cmake --build build
```

Produces:

- `build/clux` — the compiler / runner driver
- `build/clux_test` — the unit + integration test executable

> **Note:** do **not** run `ninja clean` / `--clean-first`. A generated ICU data
> file lives in the build tree and has no rebuild rule; cleaning it breaks the
> next build. Use a fresh build directory for clean rebuilds instead.

> **Note:** the build tree is not tracked for freshness — after pulling source
> changes, rebuild before trusting `build/clux` or test results, or you will be
> running a stale binary against newer sources.

## Usage

```
clux run <file>                       # run a .cx source (or .cxb bytecode, detected by content)
clux format <file.cx> [-o PATH]       # format source in place (or to PATH / stdout)
clux bc <emit|asm|disasm> <file> [-o PATH]   # bytecode tools (debug / distribution)
clux eval <expr>                      # evaluate an expression at compile time (CTFE)
clux version                          # version info
clux test                             # reserved for the test suite — not implemented yet
clux build <file.cx>                  # native backend — not implemented yet
```

Exit codes: `0` success, `1` compile error (unreadable file, lexical / syntax /
semantic error), `2` runtime error.

Examples live in `examples/` — 45 entry directories (one or more `.cx` files each,
one of them the runnable entry) plus 5 `.cxs` bytecode-assembly files. Modules
imported by an example live in a subdirectory of it (e.g. `examples/modules/lib/`),
so every top-level `.cx` is runnable.
Try:

```sh
./build/clux run examples/hello/hello.cx
```

`clux format` rewrites only whitespace / indentation / comment placement from the token
stream — it preserves declarations (including `union` / `cunion` / `import` / `export`),
keeps semantics, and is idempotent (applying it twice yields the same file). Those three
properties are covered by regression tests (`tests/fmt_test.cpp`, `FmtRegression.*`).

Hand-written bytecode round-trip:

```sh
./build/clux bc asm examples/asm/fib.cxs -o fib.cxb && ./build/clux run fib.cxb
```

## Project layout

```
src/core/        allocator, arena, vec, rbtree, strmap, omap, stream, string
src/parser/      lexer + recursive-descent / Pratt parser (+ source formatter)
src/desugar/     front-end desugaring (parser → sema; `foreach` → index `for`)
src/vm/          value engine: type/vtable, value, scope, function, bytecode, module
src/sema/        semantic analysis (shadow-value driven) + comptime
src/ctfe/        compile-time evaluation (AST interpreter for constant slots)
src/compiler/    AST -> bytecode (+ type hoisting)
src/diag/        shared diagnostic collector
src/driver/      pipeline orchestration (load -> lex -> parse -> sema -> compile -> run)
src/cmd/         subcommand dispatch (run / format / bc / build / test / version / eval)
include/         public headers (mirrors src/)
tests/           C++20 + GoogleTest suites
examples/        sample programs (.cx sources, asm/ for .cxs assembly)
docs/            design & specification documents
specs/           core-library API contracts (allocator, vec, string, maps, stream, cmd)
```

## Documentation

Pipeline and language:

- `docs/language-spec-m1.md` — the M1 language reference (lexical, types, grammar, semantics)
- `docs/architecture-m1.md` — compiler pipeline and module responsibilities
- `docs/parse-paradigm.md` — parser design decisions (mismatch/error, fail-fast, Pratt)
- `docs/format-rules.md` — rules enforced by `clux format` (spacing, indentation, comments)
- `docs/bytecode-mnemonics.md` — bytecode instruction set and `.cxs` assembly syntax

Milestone designs:

- `docs/m2-design.md` — struct / enum / arrays / tuples / optional `?T` / comptime / CTFE / bytecode tools
- `docs/m3-design.md` — pointers, ownership (`own`/`ref`/`fatal`), scope annotations, RC (`share`/`weak`)
- `docs/m4-design.md` — slices and runtime strings
- `docs/m5-design.md`, `docs/m5-design-v2.md` — module system (v2 is the design that shipped)
- `docs/m6-design.md` — FFI (`cfunc` / `extern cfunc`, libffi)
- `docs/milestones.md` — milestone roadmap and status

Core library contracts: `specs/allocator.md`, `specs/vec.md`, `specs/string.md`,
`specs/strmap.md`, `specs/omap.md`, `specs/rbtree.md`, `specs/stream.md`, `specs/cmd.md`.

## Testing

```sh
cd build && ctest --output-on-failure    # or: ./build/clux_test
```

2196 registered tests, all passing. Unit tests cover Lexer / Parser / VM / bytecode /
core containers; integration tests execute programs end-to-end through `driver_run_file`
(for `foreach` the CLI is spawned so the program's own `printf` output can be
asserted — see `tests/foreach_test.cpp`), including the `examples/` sweep and the
`.cxs ⇄ .cxb` round-trip.

> **Host requirement for the whole suite**: some tests create temp files via
> `std::filesystem::temp_directory_path()` / `tmpnam`. On hosts where spawned
> processes are denied writes to the `%TEMP%` root (the `fopen` fails with
> `errno 13`, Permission denied), those tests fail and the UCRT may abort the
> process — which makes the suite exit early and look "mostly green" when in fact
> most tests never ran. Point TMP/TEMP at a writable directory first:
>
> ```powershell
> New-Item -ItemType Directory -Force -Path build\tmp | Out-Null
> $env:TMP = (Resolve-Path build\tmp).Path; $env:TEMP = $env:TMP
> .\build\clux_test.exe
> ```

## Source encoding

All source and documentation files are **UTF-8 without BOM**. (The exception is
`third_party/`, whose MSVC project files need a BOM to be recognised as UTF-8.)

Historical note: some editors on Chinese-locale Windows save as GBK, and `.cpp`
files had crept in with a UTF-8 BOM. Both make Chinese comments render as mojibake
in other tools. To check (and catch such files landing in a pull request):

```sh
python tools/check_encoding.py          # exit 0 = clean, 1 = reports offenders
```

If you see mojibake when reading files in a terminal, check the terminal's codepage
before suspecting the file — on a GBK console, `Get-Content`/`type` render valid
UTF-8 Chinese as garbage:

```powershell
$OutputEncoding = [Console]::OutputEncoding = [Text.Encoding]::UTF8
```

## License

See repository license terms.
