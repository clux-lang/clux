#include "vm/ffi_lib.h"
#include "vm/module.h"
#include "vm/vm.h"     /* vm_t 内建类型单例（数据符号的类型码编解码） */
#include "vm/bcode.h"  /* BCODE_FFI_DATA_*（EXTERN_LOAD/STORE 的类型码） */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef CLUX_HAVE_LIBFFI
#include <ffi.h>   /* third_party/libffi-port 提供（Windows x64） */
#endif
struct ffi_lib_t {
    allocator_t *alloc;
    void        *handle; /* HMODULE / void*（dlopen）；进程伪库为 NULL */
    char        *path;   /* owned 副本 */
    bool         is_process; /* true = `extern lib "*" as x;`（当前进程自身） */
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

ffi_lib_t *ffi_lib_open(allocator_t *alloc, const char *path, char *err,
                        size_t ecap) {
    if (!alloc || !path || !*path) {
        ffi_set_err(err, ecap, "ffi: empty library path");
        return NULL;
    }
    /* M6 FFI：特殊路径 `"*"` = **当前进程自身**（RTLD_DEFAULT 语义）——libc 这类库
       进程启动时就已链接进来，没有文件需要加载。返回一个不带 OS 句柄的伪库，
       符号解析转交 ffi_sym_current_process（见 ffi_lib_sym）。 */
    if (strcmp(path, FFI_PROCESS_ALIAS) == 0) {
        ffi_lib_t *lib =
            (ffi_lib_t *)allocator_new(alloc, &g_ffi_lib_class, 1);
        if (!lib) {
            ffi_set_err(err, ecap, "ffi: out of memory");
            return NULL;
        }
        lib->alloc = alloc;
        lib->handle = NULL;
        lib->is_process = true;
        lib->path = ffi_dup(alloc, path);
        if (!lib->path) {
            allocator_free(alloc, (void **)&lib);
            ffi_set_err(err, ecap, "ffi: out of memory");
            return NULL;
        }
        return lib;
    }
    /* **只按内容判定**：把路径原样交给操作系统加载器；不做后缀检查。 */
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        ffi_set_err(err, ecap,
                    "ffi: cannot load library '%s' (LoadLibrary error %lu)",
                    path, (unsigned long)GetLastError());
        return NULL;
    }
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        ffi_set_err(err, ecap, "ffi: cannot load library '%s' (%s)", path,
                    e ? e : "unknown dlopen error");
        return NULL;
    }
#endif
    ffi_lib_t *lib = (ffi_lib_t *)allocator_new(alloc, &g_ffi_lib_class, 1);
    if (!lib) {
#ifdef _WIN32
        FreeLibrary(h);
#else
        dlclose(h);
#endif
        ffi_set_err(err, ecap, "ffi: out of memory");
        return NULL;
    }
    lib->alloc  = alloc;
    lib->handle = (void *)h;
    lib->is_process = false;
    lib->path   = ffi_dup(alloc, path);
    if (!lib->path) {
#ifdef _WIN32
        FreeLibrary(h);
#else
        dlclose(h);
#endif
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
    /* 进程伪库（`extern lib "*" as x;`）：查当前进程的全局作用域 */
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

/* ---- 调用层 ----
 *
 * **libffi 承担 ABI**（CLUX_HAVE_LIBFFI 时）：`ffi_prep_cif` 描述签名 + `ffi_call`
 * 执行调用。这是 Phase 4/5 的正式路径——整型/指针、浮点（x64 走 XMM）与结构体
 * 都由 libffi 决定传递方式；回调（Phase 5）将用 `ffi_closure`。
 * 未接 libffi 的构建回退到"最小 ABI 层"（整数/指针，按参数个数分派强转）。
 */
#define FFI_MAX_ARGS 8
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
 * 允许：整数/浮点/bool/str/opaque/enum/struct/cunion/指针（weak·fatal·ref/share）
 *       + void（仅作返回类型；作为参数由调用方另行判断）。
 * 拒绝：option、tag union、切片（胖指针）、元组、闭包（函数类型）、类型值、
 *       模块/错误/中断等引擎内部类型。
 */
const char *ffi_c_abi_reject_reason(const type_t *t) {
    if (!t) return "missing type";
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
    case TYPE_KIND_PTR_WEAK:
    case TYPE_KIND_PTR_FATAL:
    case TYPE_KIND_PTR_REF:
    case TYPE_KIND_PTR_SHARE:
        return NULL;
    case TYPE_KIND_OPTION:
        return "option has no C representation";
    case TYPE_KIND_UNION:
        return "tag union has no C representation (use cunion for C unions)";
    case TYPE_KIND_SLICE_OWN:
    case TYPE_KIND_SLICE_FATAL:
    case TYPE_KIND_SLICE_REF:
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

/* ---- M6 FFI：数据符号（extern var）的类型 ↔ 宽度/类别码 ----
 *
 * 两张表成对（编码 / 解码）放在一起：EXTERN_LOAD 用解码、sema 校验用编码，
 * 任何一侧新增类型都能立刻看出另一侧是否跟上。`bool` 有自己的码（宽度 1，
 * 但类型必须还原成 `bool`——复用 U8 会让 `flag != true` 报类型不匹配）；
 * 指针类型（opaque）用 PTR = 指针宽度。 */
bool ffi_data_kind_of(const type_t *t, const struct vm_t *vm, uint32_t *out_kind) {
    if (!t || !vm || !out_kind) return false;
    const vm_t *v = (const vm_t *)vm;
    if (t == v->type_i8)  { *out_kind = BCODE_FFI_DATA_I8;  return true; }
    if (t == v->type_i16) { *out_kind = BCODE_FFI_DATA_I16; return true; }
    if (t == v->type_i32) { *out_kind = BCODE_FFI_DATA_I32; return true; }
    if (t == v->type_i64) { *out_kind = BCODE_FFI_DATA_I64; return true; }
    if (t == v->type_u8)  { *out_kind = BCODE_FFI_DATA_U8;  return true; }
    if (t == v->type_u16) { *out_kind = BCODE_FFI_DATA_U16; return true; }
    if (t == v->type_u32) { *out_kind = BCODE_FFI_DATA_U32; return true; }
    if (t == v->type_u64) { *out_kind = BCODE_FFI_DATA_U64; return true; }
    if (t == v->type_f32) { *out_kind = BCODE_FFI_DATA_F32; return true; }
    if (t == v->type_f64) { *out_kind = BCODE_FFI_DATA_F64; return true; }
    if (t == v->type_bool) { *out_kind = BCODE_FFI_DATA_BOOL; return true; }
    if (t == v->type_opaque) { *out_kind = BCODE_FFI_DATA_PTR; return true; }
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
    case BCODE_FFI_DATA_F32: if (out_size) *out_size = 4; return v->type_f32;
    case BCODE_FFI_DATA_F64: if (out_size) *out_size = 8; return v->type_f64;
    case BCODE_FFI_DATA_BOOL:
        if (out_size) *out_size = 1;
        return v->type_bool;
    case BCODE_FFI_DATA_PTR:
        if (out_size) *out_size = sizeof(void *);
        return v->type_opaque;
    default: return NULL;
    }
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