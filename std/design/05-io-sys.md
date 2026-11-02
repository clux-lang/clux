# std/design/05-io-sys.md — `std::io` 与 `std::sys`

> 状态：设计稿 v0.1（待评审）。依据：D-A4（`sys` 是唯一 FFI 边界）、D-D1~D-D6（FFI 约定）、
> D-B1/C5（显式分配器）、D-F1（确定性）。
> 规范对应：§8.8 动态库声明、§8.9 外部 C 函数、§8.10 外部变量、§3.8 cunion、§3.13 extern。

## 1. 分层

```
std::io   平台无关 API：File / OpenMode / Error / 路径拼接 / stdout·stderr writer
   │  只调 sys
   ▼
std::sys  唯一允许 extern 的模块：libc 文件/进程/时间；错误码；C 字符串
```

- `std::io` 不出现任何 `extern`；`std::sys` 不出现业务逻辑。
- 跨平台差异只在 `sys`：`sys::posix.*` / `sys::win32.*` 由编译期选择（`comptime if` 或构建期
  选择模块，具体方式在实现文档定；对外 API 不变）。

## 2. `std::sys` API（最小面）

```clux
namespace std::sys

/* 内存：供 mem.heap() 封装 */
func malloc(size: usize) -> opaque          /* 0 = 失败 */
func free(ptr: opaque): void

/* 文件 */
func open_read(path: CStr) -> opaque        /* 返回句柄；0 = 失败 */
func open_write(path: CStr, truncate: bool) -> opaque
func open_append(path: CStr) -> opaque
func read(handle: opaque, buf: *u8, len: usize) -> isize    /* <0 = 错误；0 = EOF */
func write(handle: opaque, buf: *u8, len: usize) -> isize
func seek(handle: opaque, offset: i64, whence: u8) -> ?u64  /* whence: 0=set 1=cur 2=end */
func close(handle: opaque): void

/* 标准流 */
func stdout_handle() -> opaque
func stderr_handle() -> opaque
func stdin_handle() -> opaque

/* 错误 */
func errno() -> i32
struct CStr { ptr: *u8 }                    /* NUL 结尾视图（见 §4） */

/* 时间（后续 S5：编译耗时输出） */
func now_nanos() -> u64                     /* 单调时钟；不用于输出确定性内容 */
```

- `read`/`write` 的返回值为 `isize`：`>=0` 是字节数，`<0` 是错误（随后 `errno()`）。
- `seek` 失败返回 `nil`（不 panic），便于调用方决定是否报错。
- **`sys` 不 panic**（除 libc 不可用这类启动期错误）；错误统一以返回值 + `errno()` 表达。

## 3. `std::io` API

```clux
namespace std::io

tag union OpenMode {
    Read,
    Write { truncate: bool },     /* truncate=false ⇒ 不存在则失败 */
    Append,
}

tag union Error {
    NotFound,
    PermissionDenied,
    AlreadyExists,
    InvalidInput,
    IsDirectory,
    Io,                            /* 其它 I/O 错误，原因见 sys.errno() */
    OutOfMemory,
}

struct File {
    handle: opaque,
    alloc: mem.Allocator,
    closed: bool,
}

func open(alloc: mem.Allocator, path: str, mode: OpenMode) -> ?File
func create(alloc: mem.Allocator, path: str) -> ?File               /* Write{truncate=true} 的糖 */
func (f: *File) read(self: *File, buf: []u8) -> isize               /* <0 = 错误 */
func (f: *File) write(self: *File, data: []u8) -> isize
func (f: *File) seek(self: *File, offset: i64, whence: u8) -> ?u64
func (f: *File) read_all(self: *File, alloc: mem.Allocator) -> ?String
func (f: *File) write_all(self: *File, data: []u8) -> isize         /* 循环直到写完或出错 */
func (f: *File) __dispose__(self: *File): void                      /* close；幂等 */

/* 标准流（进程级，不关闭） */
func stdout(alloc: mem.Allocator) -> File
func stderr(alloc: mem.Allocator) -> File
func stdin(alloc: mem.Allocator) -> File

/* Writer 能力值（fmt 用） */
func file_writer(f: *File) -> fmt.Writer
func stdout_writer() -> fmt.Writer
func stderr_writer() -> fmt.Writer

/* 路径（编译器只需要拼接与比较） */
func path_join(alloc: mem.Allocator, a: str, b: str) -> String
func path_is_absolute(p: str) -> bool
func path_sep() -> u8                                                /* '/' 或 '\\' */
```

- **`File` 不拷贝**：`__init_copy__` 移动（源 `closed = true`），`__dispose__` 关闭。
- `stdout()`/`stderr()` 返回的 `File` 的 `__dispose__` **不关闭**真实句柄（文档化）。
- 错误映射：`open` 失败 → `nil`（若需要原因，另提供 `open_ex -> ?File` + `?Error`，S3 再加）；
  `read`/`write` 返回 `<0` 时由调用方决定（`read_all`/`write_all` 内部转成 `nil`）。

## 4. C 字符串与路径

- `str` 是 `{ptr,len}`，**不带 NUL**；`sys.CStr = { ptr: *u8 }` 是 NUL 结尾视图。
- 转换只提供显式 API：`String.as_cstr(self, alloc) -> CStr`（复制并追加 NUL，分配器显式传入）；
  调用方负责在合适的时机释放（编译器里通常用 arena）。
- **路径分隔符**：`path_sep()` 返回平台值；`path_join` 只在需要时插入分隔符；
  诊断里打印"用户传入的原样路径"，不做规范化（确定性 + 可复现）。

## 5. 错误契约与确定性

| 场景 | 行为 |
|---|---|
| `open` 失败 | `nil` |
| `read`/`write` 错误 | 返回负数；`File` 不 panic |
| `__dispose__` 重复 | 幂等（`closed` 标记） |
| 关闭标准流 | 不关闭（文档化） |
| 输出路径/内容 | 原样；不做大小写/分隔符归一化 |
| 诊断中的错误码 | 输出稳定的枚举名（`NotFound`…），**不**输出原始 `errno` 数字（平台相关） |

## 6. 测试清单

| 用例 | 断言 |
|---|---|
| `io.writeRead` | 写临时文件再读回，内容一致（含二进制字节） |
| `io.append` | `Append` 不覆盖已有内容 |
| `io.notFound` | 打开不存在文件返回 `nil` |
| `io.pathJoin` | 绝对/相对、尾部分隔符、空串 |
| `io.dispose` | 关闭幂等；标准流不被关闭（写 stdout 后仍可写） |
| `io.writerAdapter` | `file_writer` 写出的内容与 `File.write` 一致 |

## 7. 开放问题

1. 目录遍历（`read_dir`）编译器需要（模块发现）：放到 S3 还是现在？倾向 S3（先用显式文件列表）。
2. 跨平台选择机制：`comptime if` + 目标三元组，还是构建期选模块；实现文档定。
3. `File` 打开失败是否需要 `?Error` 版本（`open_ex`）：S3 再评估。
4. 进程/环境变量（`getenv`/`args`）：`06-args` 在 `07-args-diag-test` 里定。
