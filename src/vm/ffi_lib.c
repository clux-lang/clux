#include "vm/ffi_lib.h"
#include "vm/module.h"
#include "vm/vm.h"     /* vm_t 内建类型单例（数据符号的类型码编解码） */
#include "vm/bcode.h"  /* BCODE_FFI_DATA_*（EXTERN_LOAD/STORE 的类型码） */
#include "vm/type_struct.h"  /* 结构体字段表（构造 libffi struct 描述符） */
#include "vm/type_cunion.h"  /* cunion 成员/布局（同上） */
#include "vm/type_enum.h"    /* enum 底层类型（描述符 = 底层整数） */
#include "vm/type_array.h"       /* 数组：按值 = 同布局 struct 描述符 */
#include "vm/type_ownership.h"

#include <stdarg.h>   /* va_list/va_start：ffi_set_err（POSIX 不会由 stdio.h 传递引入） */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>   /* malloc/free：libffi 描述符树（同闭包 box 的约定） */
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>   /* readlink：/proc/self/exe（可执行文件目录） */
#if defined(__APPLE__)
#include <mach-o/dyld.h> /* _NSGetExecutablePath */
#endif
#endif

#ifdef CLUX_HAVE_LIBFFI
#include <ffi.h>   /* third_party/libffi-port 提供（Windows x64） */
#endif
struct ffi_lib_t {
    allocator_t *alloc;
    void        *handle; /* HMODULE / void*（dlopen）；进程伪库为 NULL */
    char        *path;   /* owned：实际加载成功的参数（诊断用；进程伪库为 "*"） */
    char        *name;   /* owned：**声明的通用库名**（序列化用它 → 产物可跨平台） */
    bool         is_process; /* true = `extern lib x from "*";`（当前进程自身） */
};

/* allocator 分类名（泄漏报告里据此显示来源） */
static class_t g_ffi_lib_class = {
    .name       = "clux.ffi.lib",
    .size       = sizeof(ffi_lib_t),
    .clone_fn   = NULL,
    .move_fn    = NULL,
    .dispose_fn = NULL,
};
static class_t g_ffi_str_class = {
    .name       = "clux.ffi.path",
    .size       = 1,
    .clone_fn   = NULL,
    .move_fn    = NULL,
    .dispose_fn = NULL,
};


static char *ffi_dup(allocator_t *alloc, const char *s) {
    size_t n = strlen(s);
    char *p = (char *)allocator_new(alloc, &g_ffi_str_class, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static void ffi_set_err(char *err, size_t ecap, const char *fmt, ...) {
    if (!err || ecap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, ecap, fmt, ap);
    va_end(ap);
}

/* ---- 通用库名 → 候选文件名（CMake 的 find_library 风格；**不区分操作系统**）----
 *
 * `extern lib libc from "c";` 里的 `"c"` 是**通用名**：加载器把名字展开成一组
 * 候选文件名，逐个交给操作系统加载器尝试，第一个能加载的就是它。这样同一份
 * 源码、同一份 `.cxb` 在 Windows/Linux/macOS 上都能工作。
 *
 * 候选表是**跨平台的并集**，不按编译期 OS 分支（用户要求：不直接区分操作系统）：
 *
 *     1) <name>                  as-is：显式路径 / 已带后缀的文件名 / 裸名字
 *     2) <name>.dll              Windows 常规
 *     3) lib<name>.so            Linux / 其它 ELF 平台的常规（GNU 前缀）
 *     4) <name>.so               不带 lib 前缀的 .so
 *     5) lib<name>.dylib         macOS 常规
 *     6) <name>.dylib            不带 lib 前缀的 .dylib
 *
 * 为什么不做 OS 分支：扩展名只影响"能否被当前加载器装载"，不影响语义——
 *   - Windows 上 `LoadLibraryA("libfoo.so")` 能装载**任何** PE（扩展名不参与
 *     判定），MinGW/MSYS 产出的 `.so` 因此可以直接用；
 *   - Linux 上 `dlopen("foo.dll")` 只是失败，代价是一次失败的 open；
 *   - 同一份 `.cxb` 搬到另一个平台不必改名字，候选并集让它自动命中。
 * 唯一有代价的是"名字存在但不可装载"的诊断稍长（列出全部候选）。
 */
#define FFI_CANDIDATE_MAX 8
#define FFI_CANDIDATE_LEN 512

/* 单个候选的实际装载（只按内容判定）。失败时把**第一条** OS 错误写进
   first_err（供最终诊断用），返回 NULL。 */
static void *ffi_try_load(const char *path, char *first_err, size_t ecap) {
    void *h = NULL;
#ifdef _WIN32
    h = (void *)LoadLibraryA(path);
    if (!h && first_err && !first_err[0])
        snprintf(first_err, ecap, "LoadLibrary error %lu",
                 (unsigned long)GetLastError());
#else
    h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h && first_err && !first_err[0]) {
        const char *e = dlerror();
        snprintf(first_err, ecap, "%s", e ? e : "unknown dlopen error");
    }
#endif
    return h;
}

/* 可执行文件所在目录（带结尾分隔符；失败返回 false）。
   Linux/BSD 用 /proc/self/exe，macOS 用 _NSGetExecutablePath，Windows 用
   GetModuleFileNameA。用途：库与程序放一起分发时，第二轮候选按它拼绝对路径。 */
static bool ffi_exe_dir(char *out, size_t cap) {
    if (!out || cap < 2) return false;
    char buf[FFI_CANDIDATE_LEN * 2];
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof buf);
    if (n == 0 || n >= sizeof buf) return false;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) != 0) return false;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return false;
    buf[n] = '\0';
#endif
    size_t len = strlen(buf);
    /* 去掉文件名，再去掉末尾分隔符（调用方用 '/' 拼接） */
    size_t cut = len;
    for (size_t i = len; i > 0; i--) {
        char ch = buf[i - 1];
        if (ch == '/' || ch == '\\') { cut = i - 1; break; }
    }
    while (cut > 0 && (buf[cut - 1] == '/' || buf[cut - 1] == '\\')) cut--;
    if (cut == 0 || cut >= cap) return false;
    memcpy(out, buf, cut);
    out[cut] = '\0';
    return true;
}
size_t ffi_library_candidates(const char *name, char out[][FFI_CANDIDATE_LEN],
                              size_t cap) {
    static const struct {
        const char *prefix;
        const char *suffix;
    } kForms[] = {
        { "",    ""       }, /* 1) as-is */
        { "",    ".dll"   }, /* 2) Windows */
        { "lib", ".so"    }, /* 3) ELF（GNU 前缀） */
        { "",    ".so"    }, /* 4) ELF（无前缀） */
        { "lib", ".dylib" }, /* 5) Mach-O（GNU 前缀） */
        { "",    ".dylib" }, /* 6) Mach-O（无前缀） */
    };
    if (cap == 0) return 0;
    size_t n = 0;
    for (size_t i = 0; i < sizeof(kForms) / sizeof(kForms[0]) && n < cap; i++) {
        if (i == 0) {
            snprintf(out[n], FFI_CANDIDATE_LEN, "%s", name);
        } else {
            snprintf(out[n], FFI_CANDIDATE_LEN, "%s%s%s", kForms[i].prefix, name,
                     kForms[i].suffix);
        }
        n++;
    }
    return n;
}

ffi_lib_t *ffi_lib_open(allocator_t *alloc, const char *name, char *err,
                        size_t ecap) {
    if (!alloc || !name || !*name) {
        ffi_set_err(err, ecap, "ffi: empty library name");
        return NULL;
    }
    /* 特殊名 `"*"` = **当前进程自身**（RTLD_DEFAULT 语义）——libc 这类库进程
       启动时就已链接进来，没有文件需要加载。返回不带 OS 句柄的伪库，符号解析
       转交 ffi_sym_current_process（见 ffi_lib_sym）。 */
    if (strcmp(name, FFI_PROCESS_ALIAS) == 0) {
        ffi_lib_t *lib =
            (ffi_lib_t *)allocator_new(alloc, &g_ffi_lib_class, 1);
        if (!lib) {
            ffi_set_err(err, ecap, "ffi: out of memory");
            return NULL;
        }
        lib->alloc = alloc;
        lib->handle = NULL;
        lib->is_process = true;
        lib->path = ffi_dup(alloc, name);
        lib->name = ffi_dup(alloc, name);
        if (!lib->path || !lib->name) {
            if (lib->path) allocator_free(alloc, (void **)&lib->path);
            if (lib->name) allocator_free(alloc, (void **)&lib->name);
            allocator_free(alloc, (void **)&lib);
            ffi_set_err(err, ecap, "ffi: out of memory");
            return NULL;
        }
        return lib;
    }

    /* 通用名展开候选 → 逐个交给操作系统加载器（**只按内容判定**，不做后缀
       检查）。两轮尝试：
         ① 按名字（系统搜索路径：Windows 含 exe 目录与 PATH；Linux/macOS 是
            ld.so 缓存与 LD_LIBRARY_PATH）；
         ② 按 **可执行文件所在目录** 前缀——Linux/macOS 没有"exe 目录"这个默认
            搜索项，而"库与程序放一起分发"是最常见的用法。放在第二轮而不是第一
            轮：系统库优先，避免同目录同名文件劫持（安全默认）。
       记录第一个 OS 错误文本用于诊断；全部失败时把试过的候选名一起报出来。 */
    char cand[FFI_CANDIDATE_MAX][FFI_CANDIDATE_LEN];
    const size_t ncand = ffi_library_candidates(name, cand, FFI_CANDIDATE_MAX);
    void *h = NULL;
    char hit[FFI_CANDIDATE_LEN * 2] = {0};
    char first_err[256] = {0};
    char tried_names[FFI_CANDIDATE_LEN * 2] = {0};
    for (size_t i = 0; i < ncand && !h; i++) {
        size_t used = strlen(tried_names);
        snprintf(tried_names + used, sizeof(tried_names) - used, "%s%s",
                 used ? ", " : "", cand[i]);
        h = ffi_try_load(cand[i], first_err, sizeof(first_err));
        if (h) snprintf(hit, sizeof(hit), "%s", cand[i]);
    }
    /* 第二轮：<可执行文件目录>/<候选>（仅相对名；绝对路径已由 as-is 命中）。
       诊断保持紧凑：名字列表只报一次，第二轮用 "(+ <dir>/{same})" 概括。 */
    char exe_note[FFI_CANDIDATE_LEN + 16] = {0};
    if (!h && name[0] != '/' && !(name[0] && name[1] == ':')) {
        char exe_dir[FFI_CANDIDATE_LEN] = {0};
        if (ffi_exe_dir(exe_dir, sizeof(exe_dir))) {
            snprintf(exe_note, sizeof(exe_note), " + %s/{same}", exe_dir);
            for (size_t i = 0; i < ncand && !h; i++) {
                char full[FFI_CANDIDATE_LEN * 2];
                snprintf(full, sizeof(full), "%s/%s", exe_dir, cand[i]);
                h = ffi_try_load(full, first_err, sizeof(first_err));
                if (h) snprintf(hit, sizeof(hit), "%s", full);
            }
        }
    }
    if (!h) {
        ffi_set_err(err, ecap,
                    "ffi: cannot load library '%s' (tried: %s%s; %s)", name,
                    tried_names, exe_note,
                    first_err[0] ? first_err : "load failed");
        return NULL;
    }
    ffi_lib_t *lib = (ffi_lib_t *)allocator_new(alloc, &g_ffi_lib_class, 1);
    if (!lib) {
#ifdef _WIN32
        FreeLibrary((HMODULE)h);
#else
        dlclose(h);
#endif
        ffi_set_err(err, ecap, "ffi: out of memory");
        return NULL;
    }
    lib->alloc  = alloc;
    lib->handle = h;
    lib->is_process = false;
    lib->path   = ffi_dup(alloc, hit[0] ? hit : name); /* 实际命中的那一个 */
    lib->name   = ffi_dup(alloc, name);            /* 声明的通用名（序列化用） */
    if (!lib->path || !lib->name) {
#ifdef _WIN32
        FreeLibrary((HMODULE)h);
#else
        dlclose(h);
#endif
        if (lib->path) allocator_free(alloc, (void **)&lib->path);
        if (lib->name) allocator_free(alloc, (void **)&lib->name);
        allocator_free(alloc, (void **)&lib);
        ffi_set_err(err, ecap, "ffi: out of memory");
        return NULL;
    }
    return lib;
}
void *ffi_lib_sym(const ffi_lib_t *lib, const char *name, char *err,
                  size_t ecap) {
    if (!lib || !name || !*name) {
        ffi_set_err(err, ecap, "ffi: empty symbol name");
        return NULL;
    }
    /* 进程伪库（`extern lib x from "*";`）：查当前进程的全局作用域 */
    if (lib->is_process) return ffi_sym_current_process(name, err, ecap);
#ifdef _WIN32
    FARPROC p = GetProcAddress((HMODULE)lib->handle, name);
    if (!p) {
        ffi_set_err(err, ecap,
                    "ffi: cannot resolve symbol '%s' in '%s' "
                    "(GetProcAddress error %lu)",
                    name, lib->path, (unsigned long)GetLastError());
        return NULL;
    }
    return (void *)(uintptr_t)p;
#else
    dlerror(); /* 清空先前的错误状态 */
    void *p = dlsym(lib->handle, name);
    if (!p) {
        const char *e = dlerror();
        ffi_set_err(err, ecap, "ffi: cannot resolve symbol '%s' in '%s' (%s)",
                    name, lib->path, e ? e : "unknown dlsym error");
        return NULL;
    }
    return p;
#endif
}

void ffi_lib_close(ffi_lib_t *lib) {
    if (!lib) return;
    allocator_t *alloc = lib->alloc;
#ifdef _WIN32
    if (lib->handle) FreeLibrary((HMODULE)lib->handle);
#else
    if (lib->handle) dlclose(lib->handle);
#endif
    if (lib->path) allocator_free(alloc, (void **)&lib->path);
    if (lib->name) allocator_free(alloc, (void **)&lib->name);
    allocator_free(alloc, (void **)&lib);
}

/* ---- 特殊外部模块 `*`：当前进程自身（RTLD_DEFAULT 语义）---- */

#ifdef _WIN32
/* EnumProcessModules 的两种来源：Win7+ 的 K32EnumProcessModules 在 kernel32 里，
   旧路径在 psapi.dll 里。都**动态取**，避免为端口增加链接依赖。 */
typedef BOOL(WINAPI *ffi_enum_modules_fn)(HANDLE, HMODULE *, DWORD, LPDWORD);

static ffi_enum_modules_fn ffi_enum_modules(void) {
    static ffi_enum_modules_fn fn = NULL;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (k32)
            fn = (ffi_enum_modules_fn)(void *)GetProcAddress(k32,
                                                             "K32EnumProcessModules");
        if (!fn) {
            HMODULE psapi = GetModuleHandleA("psapi.dll");
            if (psapi)
                fn = (ffi_enum_modules_fn)(void *)GetProcAddress(psapi,
                                                                 "EnumProcessModules");
        }
    }
    return fn;
}
#endif

void *ffi_sym_current_process(const char *name, char *err, size_t ecap) {
    if (!name || !*name) {
        ffi_set_err(err, ecap, "ffi: empty symbol name");
        return NULL;
    }
#ifdef _WIN32
    /* ① 主模块（EXE）自身的导出 */
    HMODULE main_mod = GetModuleHandleA(NULL);
    if (main_mod) {
        void *sym = (void *)GetProcAddress(main_mod, name);
        if (sym) return sym;
    }
    /* ② 已加载模块的导出：进程 import 的 CRT/系统库都在这里（abs/strlen/…）。
       模块列表顺序 = 加载顺序，先命中先用（与 RTLD_DEFAULT 的"按全局作用域
       搜索"语义同构：谁先被加载谁先被看到）。 */
    ffi_enum_modules_fn em = ffi_enum_modules();
    if (em) {
        HMODULE mods[256];
        DWORD needed = 0;
        if (em(GetCurrentProcess(), mods, (DWORD)sizeof(mods), &needed)) {
            size_t n = (size_t)needed / sizeof(HMODULE);
            if (n > sizeof(mods) / sizeof(mods[0])) n = sizeof(mods) / sizeof(mods[0]);
            for (size_t i = 0; i < n; i++) {
                if (mods[i] == main_mod || !mods[i]) continue;
                void *sym = (void *)GetProcAddress(mods[i], name);
                if (sym) return sym;
            }
        }
    }
    ffi_set_err(err, ecap,
                "ffi: cannot resolve symbol '%s' in the current process "
                "(searched the main module and all loaded modules)",
                name);
    return NULL;
#else
    dlerror(); /* 清空先前的错误状态 */
    void *sym = dlsym(RTLD_DEFAULT, name);
    if (!sym) {
        const char *d = dlerror();
        ffi_set_err(err, ecap,
                    "ffi: cannot resolve symbol '%s' in the current process (%s)",
                    name, d ? d : "not found via RTLD_DEFAULT");
    }
    return sym;
#endif
}

const char *ffi_lib_path(const ffi_lib_t *lib) {
    return (lib && lib->path) ? lib->path : "";
}

const char *ffi_lib_name(const ffi_lib_t *lib) {
    return (lib && lib->name) ? lib->name : "";
}

/* ---- 调用层 ----
 *
 * **libffi 承担 ABI**（CLUX_HAVE_LIBFFI 时）：`ffi_prep_cif` 描述签名 + `ffi_call`
 * 执行调用。这是 Phase 4/5 的正式路径——整型/指针、浮点（x64 走 XMM）与结构体
 * 都由 libffi 决定传递方式；回调（Phase 5）将用 `ffi_closure`。
 * 未接 libffi 的构建回退到"最小 ABI 层"（整数/指针，按参数个数分派强转）。
 */
#define FFI_MAX_ARGS 16
typedef int64_t (*ffi_fn0)(void);
typedef int64_t (*ffi_fn1)(int64_t);
typedef int64_t (*ffi_fn2)(int64_t, int64_t);
typedef int64_t (*ffi_fn3)(int64_t, int64_t, int64_t);
typedef int64_t (*ffi_fn4)(int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*ffi_fn5)(int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*ffi_fn6)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*ffi_fn7)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t);
typedef int64_t (*ffi_fn8)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t);

bool ffi_call_int(void *sym, size_t argc, const int64_t *argv, int64_t *out,
                  char *err, size_t ecap) {
    if (!sym) {
        ffi_set_err(err, ecap, "ffi: cannot call a null symbol");
        return false;
    }
    if (argc > FFI_MAX_ARGS) {
        ffi_set_err(err, ecap,
                    "ffi: %zu arguments exceed the ABI layer limit (%d)",
                    argc, (int)FFI_MAX_ARGS);
        return false;
    }
    int64_t r = 0;
    switch (argc) {
    case 0: r = ((ffi_fn0)sym)(); break;
    case 1: r = ((ffi_fn1)sym)(argv[0]); break;
    case 2: r = ((ffi_fn2)sym)(argv[0], argv[1]); break;
    case 3: r = ((ffi_fn3)sym)(argv[0], argv[1], argv[2]); break;
    case 4: r = ((ffi_fn4)sym)(argv[0], argv[1], argv[2], argv[3]); break;
    case 5: r = ((ffi_fn5)sym)(argv[0], argv[1], argv[2], argv[3], argv[4]); break;
    case 6: r = ((ffi_fn6)sym)(argv[0], argv[1], argv[2], argv[3], argv[4],
                              argv[5]); break;
    case 7: r = ((ffi_fn7)sym)(argv[0], argv[1], argv[2], argv[3], argv[4],
                              argv[5], argv[6]); break;
    default: r = ((ffi_fn8)sym)(argv[0], argv[1], argv[2], argv[3], argv[4],
                                argv[5], argv[6], argv[7]); break;
    }
    if (out) *out = r;
    return true;
}

bool ffi_call_typed(void *sym, size_t argc, const struct _ffi_type **arg_types,
                    const struct _ffi_type *ret_type, void **arg_values,
                    void *ret_value, char *err, size_t ecap) {
    if (!sym) {
        ffi_set_err(err, ecap, "ffi: cannot call a null symbol");
        return false;
    }
#if defined(CLUX_HAVE_LIBFFI)
    /* libffi 原生调用路径。Windows x64 下**必须**用 libffi 的 MASM/Intel 语法变体
       （win64_intel.S）经 ml64 汇编：clang 集成汇编器把 GAS 变体 win64.S 汇编成
       COFF 时产出的 ffi_call_win64 会在运行期写坏 rsp（见 docs/m6-design.md §14.4
       与 libffi-port/CMakeLists.txt 的说明）。 */
    ffi_cif cif;
    ffi_status st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned int)argc,
                                 (ffi_type *)ret_type, (ffi_type **)arg_types);
    if (st != FFI_OK) {
        ffi_set_err(err, ecap, "ffi: ffi_prep_cif failed (status=%d)", (int)st);
        return false;
    }
    ffi_call(&cif, FFI_FN(sym), ret_value, arg_values);
    return true;
#else
    (void)argc; (void)arg_types; (void)ret_type; (void)arg_values;
    (void)ret_value;
    ffi_set_err(err, ecap,
                "ffi: typed calls (float/struct) require the libffi call path; "
                "this build has no libffi (CLUX_HAVE_LIBFFI)");
    return false;
#endif
}

/* 定参 / 变参统一入口：`nfixed == argc` 走 ffi_prep_cif，`nfixed < argc` 走
 * ffi_prep_cif_var（C 的 `...`）。变参部分的**提升**由调用方完成——只有它知道
 * 哪些位置是变参（见 ffi_lib.h）。 */
bool ffi_call_typed_items(void *sym, size_t nfixed, size_t argc,
                          const void *const *arg_types, const void *ret_type,
                          void **arg_values, void *ret_value, char *err,
                          size_t ecap) {
    if (!sym) {
        ffi_set_err(err, ecap, "ffi: cannot call a null symbol");
        return false;
    }
    if (nfixed > argc) {
        ffi_set_err(err, ecap, "ffi: fixed argument count exceeds total");
        return false;
    }
#if defined(CLUX_HAVE_LIBFFI)
    /* ret_type == NULL 表示 void：libffi **要求**非空返回描述符（会直接解引用，
       传 NULL 会崩），所以这里补上 ffi_type_void。 */
    if (!ret_type) ret_type = &ffi_type_void;
    ffi_cif cif;
    ffi_status st;
    if (nfixed == argc) {
        st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned int)argc,
                          (ffi_type *)ret_type, (ffi_type **)arg_types);
    } else {
        /* 变参：libffi 需要"固定参数个数 + 总个数"以决定寄存器/栈的铺法
           （x64 变参还要按需把寄存器参数复制进 shadow store）。 */
        st = ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, (unsigned int)nfixed,
                              (unsigned int)argc, (ffi_type *)ret_type,
                              (ffi_type **)arg_types);
    }
    if (st != FFI_OK) {
        ffi_set_err(err, ecap, "ffi: ffi_prep_cif%s failed (status=%d)",
                    nfixed == argc ? "" : "_var", (int)st);
        return false;
    }
    ffi_call(&cif, FFI_FN(sym), ret_value, arg_values);
    return true;
#else
    (void)nfixed; (void)argc; (void)arg_types; (void)ret_type; (void)arg_values;
    (void)ret_value;
    ffi_set_err(err, ecap,
                "ffi: typed calls require the libffi call path; this build has "
                "no libffi (CLUX_HAVE_LIBFFI)");
    return false;
#endif
}
/* ---- 浮点调用（double → double），不依赖 libffi ---- */
typedef double (*ffi_f64_0)(void);
typedef double (*ffi_f64_1)(double);
typedef double (*ffi_f64_2)(double, double);
typedef double (*ffi_f64_3)(double, double, double);
typedef double (*ffi_f64_4)(double, double, double, double);

bool ffi_call_f64(void *sym, size_t argc, const double *args, double *out,
                  char *err, size_t ecap) {
    if (!sym) {
        ffi_set_err(err, ecap, "ffi: cannot call a null symbol");
        return false;
    }
    if (argc > 4) {
        ffi_set_err(err, ecap,
                    "ffi: %zu double arguments exceed the typed layer limit (4)",
                    argc);
        return false;
    }
    double r = 0.0;
    switch (argc) {
    case 0: r = ((ffi_f64_0)sym)(); break;
    case 1: r = ((ffi_f64_1)sym)(args[0]); break;
    case 2: r = ((ffi_f64_2)sym)(args[0], args[1]); break;
    case 3: r = ((ffi_f64_3)sym)(args[0], args[1], args[2]); break;
    default: r = ((ffi_f64_4)sym)(args[0], args[1], args[2], args[3]); break;
    }
    if (out) *out = r;
    return true;
}

/* ---- §2 C ABI 类型约束（m6-design §2.1/§2.2）----
 *
 * `extern cfunc` 的参数/返回类型必须是 C 可表达的类型；clux 专有的类型（option、
 * tag union、切片、元组、闭包、类型值）没有 C 对应，必须**在编译期拒绝**而不是
 * 猜着传参。返回 NULL 表示"可用于 C ABI"，否则返回人类可读的拒绝原因。
 *
 * 允许：整数/浮点/bool/str/opaque/enum/struct/cunion/指针（fatal·ref）
 *       + void（仅作返回类型；作为参数由调用方另行判断）。
 * 拒绝：option、tag union、切片（胖指针）、元组、闭包（函数类型）、类型值、
 *       模块/错误/中断等引擎内部类型。
 *
 * `own` 是**该位置的所有权**，由调用方传入（见 ffi_c_abi_reject_reason_own）：
 * 所有权已从类型层迁移到值/符号层，`type_t` 不携带 ownership，所以 own 的
 * 判定必须由调用方从类型槽位解析后传入，不能靠类型本身。
 */
const char *ffi_c_abi_reject_reason_own(const type_t *t, ownership_t own) {
    if (!t) return "missing type";
    /* 所有权现在记在类型上：`ref *T` 的有效类型是 `*T`、所有权是 REF。
       结构/ABI 判定一律看裸类型；所有权从类型读（调用方给的 own 与之取并）。 */
    if (type_ownership_of(t) != OWN_NONE) {
        if (own == OWN_NONE) own = type_ownership_of(t);
        t = type_strip_ownership(t);
    }
    /* 所有权修饰在 C ABI 里是**契约**，不是表示层：
         - **参数** `own *T` / `own opaque`：**合法**——C 接管这个指针
           （例如 `free(p)`），clux 侧交出所有权。C 侧看到的始终是裸指针，
           所有权只是 clux 侧的值状态。
         - **返回** `own`：非法（在 check_extern_cfunc 处单独拒绝）——把所有权
           交给调用方要用 `fatal T`，与用户函数返回类型的规则一致。
         - `own []T`：**非法**——切片是胖指针，C 侧根本没有这个表示。
         - `own T`（聚合按值）：非法——按值传聚合时字段所有权无法在 C 侧表达。
       ref/fatal 指针本来就按指针宽度放行。 */
    /* `own` 能修饰的 C ABI 类型：指针、opaque、以及**有 C 按值表示**的聚合
       （struct / cunion —— 字段还需递归可表示，见 ffi_c_abi_check_aggregate）。
       数组**不在此列**：C 里数组形参退化为指针、且没有数组返回值，所以它根本没有
       "按值表示"（要传数组请声明指针 + `data(a)`）。切片（胖指针）、元组、
       tag union、option、func、type value 同样没有 C 表示 → 拒绝。 */
    if (own == OWN_OWN && t->kind != TYPE_KIND_PTR &&
        t->kind != TYPE_KIND_OPAQUE && t->kind != TYPE_KIND_STRUCT &&
        t->kind != TYPE_KIND_CUNION)
        return "the type is owned (own T) and cannot cross the C ABI: own can "
               "qualify pointers ('own *T' / 'own opaque') and aggregates with a "
               "C by-value representation (struct / cunion); arrays decay to "
               "pointers in C (use 'ref *T' + data(a)), and slices/tuples/tag "
               "unions/options have no C representation";
    if (t->kind == TYPE_KIND_ARRAY)
        return "array has no by-value C representation: arrays decay to pointers "
               "in C (and C cannot return arrays) — declare the parameter as "
               "'ref *T' / 'ref opaque' and pass data(a) / a.& instead";
    switch (t->kind) {
    case TYPE_KIND_INT:
    case TYPE_KIND_FLOAT:
    case TYPE_KIND_BOOL:
    case TYPE_KIND_STR:
    case TYPE_KIND_OPAQUE:
    case TYPE_KIND_ENUM:
    case TYPE_KIND_STRUCT:
    case TYPE_KIND_CUNION:
    case TYPE_KIND_VOID: /* 仅作返回类型；参数位置的检查由调用方负责 */
    case TYPE_KIND_PTR:
        /* 指针一律指针宽度（own/ref/fatal 只影响 clux 侧所有权契约） */
        return NULL;
    case TYPE_KIND_CFUNC:
        /* C 函数指针（`cfunc(i32) -> i32`）：C 侧就是裸函数指针，指针宽，无
           env/捕获——这正是它区别于 TYPE_KIND_FUNC（clux 函数值，带 env）的
           地方。FFI 边界**只**允许这个类型。 */
        return NULL;
    case TYPE_KIND_OPTION:
        return "option has no C representation";
    case TYPE_KIND_UNION:
        return "tag union has no C representation (use cunion for C unions)";
    case TYPE_KIND_SLICE:
        return "slice is a fat pointer (not C ABI)";
    case TYPE_KIND_TUPLE:
        return "tuple has no C representation";
    case TYPE_KIND_FUNC:
        return "closures/functions cannot cross the C ABI";
    case TYPE_KIND_TYPE:
        return "type values cannot cross the C ABI";
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE:
        return "const/volatile wrapper cannot cross the C ABI";
    default:
        return "type is not C ABI compatible";
    }
}

/* 聚合类型的**递归** C ABI 校验（声明处用）：
 * struct / cunion 的字段本身必须能跨 C ABI（标量、枚举、嵌套 struct/cunion、
 * 指针、opaque……）。失败时把 libffi 描述符构造给出的具体原因（含字段类型名）
 * 写进 err —— 比在调用点才暴露 "parameter N: ..." 更早、更准。
 * 非聚合类型直接通过（由 ffi_c_abi_reject_reason_own 管）。 */
bool ffi_c_abi_check_aggregate(const type_t *t, const struct vm_t *vm, char *err,
                               size_t ecap) {
    t = type_strip_ownership(t);
    if (!t) return true;
    if (t->kind != TYPE_KIND_STRUCT && t->kind != TYPE_KIND_CUNION) return true;
    bool owned = false;
    const void *desc = ffi_type_for(t, vm, &owned, err, ecap);
    if (!desc) return false;
    if (owned) ffi_type_release(desc);
    return true;
}

/* 便捷包装：不关心所有权（运行期编组路径）时用。 */
const char *ffi_c_abi_reject_reason(const type_t *t) {
    return ffi_c_abi_reject_reason_own(t, OWN_NONE);
}

/* ---- M6 FFI：数据符号（extern var）的类型 ↔ 宽度/类别码 ----
 *
 * 两张表成对（编码 / 解码）放在一起：EXTERN_LOAD 用解码、sema 校验用编码，
 * 任何一侧新增类型都能立刻看出另一侧是否跟上。`bool` 有自己的码（宽度 1，
 * 但类型必须还原成 `bool`——复用 U8 会让 `flag != true` 报类型不匹配）；
 * 指针类型（opaque）用 PTR = 指针宽度。 */
bool ffi_data_kind_of(const type_t *t, const struct vm_t *vm, uint32_t *out_kind) {
    if (!t || !vm || !out_kind) return false;
    t = type_strip_ownership(t);
    const vm_t *v = (const vm_t *)vm;
    if (t == v->type_i8)  { *out_kind = BCODE_FFI_DATA_I8;  return true; }
    if (t == v->type_i16) { *out_kind = BCODE_FFI_DATA_I16; return true; }
    if (t == v->type_i32) { *out_kind = BCODE_FFI_DATA_I32; return true; }
    if (t == v->type_i64) { *out_kind = BCODE_FFI_DATA_I64; return true; }
    if (t == v->type_u8)  { *out_kind = BCODE_FFI_DATA_U8;  return true; }
    if (t == v->type_u16) { *out_kind = BCODE_FFI_DATA_U16; return true; }
    if (t == v->type_u32) { *out_kind = BCODE_FFI_DATA_U32; return true; }
    if (t == v->type_u64) { *out_kind = BCODE_FFI_DATA_U64; return true; }
    /* usize（C 的 size_t）是独立类型：自己的码，宽度按运行平台 */
    if (t == v->type_usize) { *out_kind = BCODE_FFI_DATA_USIZE; return true; }
    if (t == v->type_f32) { *out_kind = BCODE_FFI_DATA_F32; return true; }
    if (t == v->type_f64) { *out_kind = BCODE_FFI_DATA_F64; return true; }
    if (t == v->type_bool) { *out_kind = BCODE_FFI_DATA_BOOL; return true; }
    if (t == v->type_opaque) { *out_kind = BCODE_FFI_DATA_PTR; return true; }
    /* `str` 在 C 里就是 `char *`：库里的 char* 全局可以按 str 读/写
       （value->data 存的就是那个指针，与 C 同构） */
    if (t == v->type_str) { *out_kind = BCODE_FFI_DATA_STR; return true; }
    return false;
}

const type_t *ffi_data_type_of(const struct vm_t *vm, uint32_t kind,
                               size_t *out_size) {
    if (!vm) return NULL;
    const vm_t *v = (const vm_t *)vm;
    switch (kind) {
    case BCODE_FFI_DATA_I8:  if (out_size) *out_size = 1; return v->type_i8;
    case BCODE_FFI_DATA_I16: if (out_size) *out_size = 2; return v->type_i16;
    case BCODE_FFI_DATA_I32: if (out_size) *out_size = 4; return v->type_i32;
    case BCODE_FFI_DATA_I64: if (out_size) *out_size = 8; return v->type_i64;
    case BCODE_FFI_DATA_U8:  if (out_size) *out_size = 1; return v->type_u8;
    case BCODE_FFI_DATA_U16: if (out_size) *out_size = 2; return v->type_u16;
    case BCODE_FFI_DATA_U32: if (out_size) *out_size = 4; return v->type_u32;
    case BCODE_FFI_DATA_U64: if (out_size) *out_size = 8; return v->type_u64;
    case BCODE_FFI_DATA_USIZE:
        if (out_size) *out_size = sizeof(void *);
        return v->type_usize;
    case BCODE_FFI_DATA_F32: if (out_size) *out_size = 4; return v->type_f32;
    case BCODE_FFI_DATA_F64: if (out_size) *out_size = 8; return v->type_f64;
    case BCODE_FFI_DATA_BOOL:
        if (out_size) *out_size = 1;
        return v->type_bool;
    case BCODE_FFI_DATA_PTR:
        if (out_size) *out_size = sizeof(void *);
        return v->type_opaque;
    case BCODE_FFI_DATA_STR:
        if (out_size) *out_size = sizeof(char *);
        return v->type_str;
    default: return NULL;
    }
}

/* 码 → libffi 描述符：clux 标量与 C 的 1:1 映射（`bool` 就是 1 字节 _Bool）。
 * 与上面的编/解码表同源，新增类型时三处一起看。 */
const void *ffi_data_ffi_type(uint32_t kind) {
#if defined(CLUX_HAVE_LIBFFI)
    switch (kind) {
    case BCODE_FFI_DATA_I8:  return &ffi_type_sint8;
    case BCODE_FFI_DATA_I16: return &ffi_type_sint16;
    case BCODE_FFI_DATA_I32: return &ffi_type_sint32;
    case BCODE_FFI_DATA_I64: return &ffi_type_sint64;
    case BCODE_FFI_DATA_U8:  return &ffi_type_uint8;
    case BCODE_FFI_DATA_U16: return &ffi_type_uint16;
    case BCODE_FFI_DATA_U32: return &ffi_type_uint32;
    case BCODE_FFI_DATA_U64: return &ffi_type_uint64;
    /* size_t：宽度随目标架构（与 C 的 size_t 同宽） */
    case BCODE_FFI_DATA_USIZE:
        return sizeof(void *) == 4 ? &ffi_type_uint32 : &ffi_type_uint64;
    case BCODE_FFI_DATA_F32: return &ffi_type_float;
    case BCODE_FFI_DATA_F64: return &ffi_type_double;
    case BCODE_FFI_DATA_BOOL: return &ffi_type_uint8;
    case BCODE_FFI_DATA_PTR: return &ffi_type_pointer;
    case BCODE_FFI_DATA_STR: return &ffi_type_pointer;
    default: return NULL;
    }
#else
    (void)kind;
    return NULL;
#endif
}

/* 该类型作数据符号时是否**必须**把类型随指令落盘（宽度/布局要查类型表）。 */
bool ffi_data_kind_needs_type(const type_t *t) {
    t = type_strip_ownership(t);
    if (!t) return false;
    switch (t->kind) {
    case TYPE_KIND_STRUCT:
    case TYPE_KIND_CUNION:
    case TYPE_KIND_ENUM:
        return true;
    default:
        return false;
    }
}

#if defined(CLUX_HAVE_LIBFFI)
/* 新造一个 ffi_type（elements 以 NULL 结尾）。 */
static ffi_type *ffi_type_new_struct(ffi_type **elems) {
    ffi_type *st = (ffi_type *)malloc(sizeof(ffi_type));
    if (!st) {
        free(elems);
        return NULL;
    }
    st->size = 0;          /* libffi 在 ffi_prep_cif 里算 */
    st->alignment = 0;
    st->type = FFI_TYPE_STRUCT;
    st->elements = elems;
    return st;
}
#endif

const void *ffi_type_for(const type_t *t, const struct vm_t *vm, bool *out_owned,
                         char *err, size_t ecap) {
    if (out_owned) *out_owned = false;
    if (!t) {
        ffi_set_err(err, ecap, "ffi: missing type");
        return NULL;
    }
    /* 运行期/描述符只看裸类型（所有权是 clux 编译期契约）。 */
    t = type_strip_ownership(t);
    /* 标量（含 str → 指针）：libffi 静态描述符，无需释放 */
    uint32_t dk = 0;
    if (vm && ffi_data_kind_of(t, vm, &dk)) return ffi_data_ffi_type(dk);
    switch (t->kind) {
    case TYPE_KIND_PTR:
    case TYPE_KIND_CFUNC: /* C 函数指针：与 void* 同宽同传法 */
        return ffi_data_ffi_type(BCODE_FFI_DATA_PTR);
    case TYPE_KIND_ENUM: {
        const type_t *u = enum_type_underlying(t);
        /* 底层类型一定是内建整数 → 递归一次即可 */
        return ffi_type_for(u, vm, out_owned, err, ecap);
    }
#if defined(CLUX_HAVE_LIBFFI)
    case TYPE_KIND_STRUCT: {
        size_t n = struct_type_field_count(t);
        ffi_type **elems = (ffi_type **)malloc((n + 1) * sizeof(ffi_type *));
        if (!elems) {
            ffi_set_err(err, ecap, "ffi: out of memory (struct descriptor)");
            return NULL;
        }
        for (size_t i = 0; i < n; i++) {
            const struct_field_t *f = struct_type_field(t, i);
            bool child_owned = false;
            const void *child =
                f ? ffi_type_for(f->type, vm, &child_owned, err, ecap) : NULL;
            if (!child) {
                /* 递归失败：清掉已建的子描述符 */
                for (size_t k = 0; k < i; k++)
                    ffi_type_release(elems[k]);
                free(elems);
                return NULL;
            }
            elems[i] = (ffi_type *)child;
        }
        elems[n] = NULL;
        ffi_type *st = ffi_type_new_struct(elems);
        if (!st) {
            for (size_t k = 0; k < n; k++) ffi_type_release(elems[k]);
            free(elems);
            ffi_set_err(err, ecap, "ffi: out of memory (struct descriptor)");
            return NULL;
        }
        if (out_owned) *out_owned = true;
        return st;
    }
    case TYPE_KIND_CUNION: {
        /* libffi 没有 union：建模成 k = size/align 个 align 宽度的整数成员。
           union 的 size 必是 align 的整数倍（size = align_up(max member, align)），
           因此这样建出来的 struct size/align 与真 union 完全一致，ABI 分类相同。 */
        size_t align = t->align ? t->align : 1;
        size_t k = (t->size + align - 1) / align;
        if (k == 0) k = 1;
        const void *member = NULL;
        switch (align) {
        case 1: member = &ffi_type_sint8; break;
        case 2: member = &ffi_type_sint16; break;
        case 4: member = &ffi_type_sint32; break;
        default: member = &ffi_type_sint64; break;
        }
        ffi_type **elems = (ffi_type **)malloc((k + 1) * sizeof(ffi_type *));
        if (!elems) {
            ffi_set_err(err, ecap, "ffi: out of memory (cunion descriptor)");
            return NULL;
        }
        for (size_t i = 0; i < k; i++) elems[i] = (ffi_type *)member;
        elems[k] = NULL;
        ffi_type *st = ffi_type_new_struct(elems);
        if (!st) {
            free(elems);
            ffi_set_err(err, ecap, "ffi: out of memory (cunion descriptor)");
            return NULL;
        }
        if (out_owned) *out_owned = true;
        return st;
    }
#endif
    default:
        ffi_set_err(err, ecap, "ffi: type cannot cross the C ABI (%s)",
                    t->name.ptr ? t->name.ptr : "?");
        return NULL;
    }
}

void ffi_type_release(const void *desc) {
#if defined(CLUX_HAVE_LIBFFI)
    if (!desc) return;
    const ffi_type *st = (const ffi_type *)desc;
    /* 只释放"新建"的 STRUCT 描述符（静态标量描述符的元素表为 NULL/非本函数分配） */
    if (st->type != FFI_TYPE_STRUCT || !st->elements) return;
    for (size_t i = 0; st->elements[i]; i++) ffi_type_release(st->elements[i]);
    free((void *)st->elements);
    free((void *)st);
#else
    (void)desc;
#endif
}

/* ---- 闭包（Phase 5 回调基础设施）----
 * 内存：trampoline 由 libffi 分配（不属于本项目 allocator），box/cif 用 malloc/free，
 * 因此必须调用 clux_ffi_closure_destroy，否则静默泄漏可执行内存。 */
#if defined(CLUX_HAVE_LIBFFI)
typedef struct {
    ffi_closure          *closure;
    ffi_cif              *cif;
    ffi_type            **arg_types; /* owned 副本：libffi 的 cif **借用**该数组 */
    ffi_closure_handler_t handler;
    void                 *user;
    void                 *code;
} ffi_closure_box_t;

static void ffi_closure_trampoline(ffi_cif *cif, void *ret, void **args,
                                   void *user) {
    ffi_closure_box_t *box = (ffi_closure_box_t *)user;
    (void)cif;
    if (box && box->handler) box->handler(ret, args, box->user);
}
#endif

bool clux_ffi_closure_create(allocator_t *alloc, size_t argc, void **arg_types,
                             void *ret_type, ffi_closure_handler_t handler,
                             void *user, void **out_handle, void **out_code,
                             char *err, size_t ecap) {
    if (!handler || !out_handle || !out_code) {
        ffi_set_err(err, ecap, "ffi: closure_create: bad argument");
        return false;
    }
    *out_handle = NULL;
    *out_code = NULL;
#if defined(CLUX_HAVE_LIBFFI)
    ffi_closure_box_t *box = (ffi_closure_box_t *)malloc(sizeof(ffi_closure_box_t));
    ffi_cif *cif = (ffi_cif *)malloc(sizeof(ffi_cif));
    if (!box || !cif) {
        free(box); free(cif);
        ffi_set_err(err, ecap, "ffi: closure_create: out of memory");
        return false;
    }
    box->handler = handler; box->user = user; box->cif = cif;
    box->closure = NULL; box->code = NULL; box->arg_types = NULL;
    /* ffi_prep_cif **借用** arg_types 数组（cif->arg_types 指向它，调用时按需读取），
       所以不能直接用调用方的栈数组——必须复制一份，生命周期与本闭包一致。
       调用方因此不必让 arg_types 活到 C 侧调用时刻（语言层 `cfunc_ptr(f)` 的
       arg_types 就在已被弹出的解释器栈帧里）。 */
    if (argc > 0) {
        box->arg_types = (ffi_type **)malloc(sizeof(ffi_type *) * argc);
        if (!box->arg_types) {
            free(cif); free(box);
            ffi_set_err(err, ecap, "ffi: closure_create: out of memory");
            return false;
        }
        for (size_t i = 0; i < argc; i++) {
            box->arg_types[i] = ((ffi_type **)arg_types)[i];
        }
    }
    ffi_status st = ffi_prep_cif(cif, FFI_DEFAULT_ABI, (unsigned int)argc,
                                 (ffi_type *)ret_type, box->arg_types);
    if (st == FFI_OK)
        box->closure = ffi_closure_alloc(sizeof(ffi_closure), &box->code);
    if (st != FFI_OK || !box->closure || !box->code) {
        if (box->closure) ffi_closure_free(box->closure);
        free(box->arg_types); free(cif); free(box);
        ffi_set_err(err, ecap, "ffi: closure_create: prep/alloc failed");
        return false;
    }
    st = ffi_prep_closure_loc(box->closure, cif, ffi_closure_trampoline, box, box->code);
    if (st != FFI_OK) {
        ffi_closure_free(box->closure);
        free(box->arg_types); free(cif); free(box);
        ffi_set_err(err, ecap, "ffi: closure_create: prep_closure_loc failed");
        return false;
    }
    *out_handle = box; *out_code = box->code;
    (void)alloc;
    return true;
#else
    (void)alloc; (void)argc; (void)arg_types; (void)ret_type; (void)user;
    ffi_set_err(err, ecap, "ffi: closures require the libffi path");
    return false;
#endif
}

void clux_ffi_closure_destroy(allocator_t *alloc, void *handle) {
#if defined(CLUX_HAVE_LIBFFI)
    ffi_closure_box_t *box = (ffi_closure_box_t *)handle;
    if (box) {
        if (box->closure) ffi_closure_free(box->closure);
        free(box->arg_types);
        free(box->cif);
        free(box);
    }
#else
    (void)handle;
#endif
    (void)alloc;
}