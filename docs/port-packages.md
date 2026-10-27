# Port 包与 C 第三方依赖

> 状态：`export extern` + `clux.mod` 的 `link` / `libdirs` / `runtime` /
> `platforms`、`clux install` 的 `postinstall` 钩子与 `clux.lock` 已实现。
> clux **不编译 C 源码、不读取 C 头文件、不处理 C 宏**；C 库必须先在外部
> 编译成静态库/动态库，port 只描述其 ABI。

## 1. 两层模型

- **port（`.cx`）**：只描述 C 库的 ABI——类型、函数、全局变量、常量。平台无关。
- **`clux.mod` 的 `native` 段**：描述这个包如何获得并链接 C 库——平台相关。
- 一个 port 包是**普通 clux 包**：`entry` 指向 `port.cx`，通过
  `import <alias> from "<包名>"` 使用；不引入新的文件类型或包类型。

## 2. 包布局

```
deps/zlib/
  clux.mod
  port.cx                 # 纯声明
  vendor/
    lib/win64/zlibstatic.lib
    lib/linux-x64/libz.a
    bin/win64/zlib1.dll
```

`clux.mod`：

```json
{
  "name": "zlib",
  "version": "1.3.1",
  "entry": "port.cx",
  "native": {
    "link": ["z"],
    "libdirs": ["vendor/lib/linux-x64"],
    "runtime": [],
    "platforms": {
      "WIN64": {
        "libdirs": ["vendor/lib/win64"],
        "link": ["zlibstatic"],
        "runtime": ["vendor/bin/win64/zlib1.dll"]
      },
      "LINUX": {
        "libdirs": ["vendor/lib/linux-x64"],
        "link": ["z"]
      }
    }
  }
}
```

`port.cx`：

```clux
// 链接由 clux.mod native 段负责；from "*" = 符号已由最终链接提供
extern lib zlib from "*";

export struct z_stream {
    next_in: ref *const u8;
    avail_in: u32;
    next_out: ref *u8;
    avail_out: u32;
}

export extern cfunc zlib::zlibVersion(): str;
export extern cfunc zlib::compress(dest: ref *u8, dest_len: ref *usize,
                                   source: ref *const u8, source_len: usize): i32;
export extern var zlib::zlib_errno: i32;

export comptime var Z_OK: const i32 = 0;
export comptime var Z_STREAM_END: const i32 = 1;
```

导入方：

```clux
import zlib from "zlib";

func main(): i32 {
    var bound: usize = zlib::compressBound(1024usize);
    _ = bound;
    return zlib::Z_OK;
}
```

## 3. `export extern`

- `export extern cfunc <lib>::<name>(...): T;`：把 C 函数声明放进模块导出表；
  导入方以 `<port>::<name>(...)` 调用，codegen 直接调用真实 C 符号。
- `export extern var <lib>::<name>: T;`：数据符号；导入方可读可写。
- 显式 `own` / `ref` / `fatal` 仍是 clux 侧契约：`own` 实参必须 `move`/`clone`，
  `share`/`weak` 被拒绝。
- 调用约定：`[[cdecl]]` / `[[stdcall]]`（`[[winapi]]` 等价于 stdcall）只允许
  修饰 `cfunc` 定义或 `extern cfunc` 声明；生成 C 用 `CLUX_STDCALL` 宏
  （Windows x64 无区分，x86 才生效）。`cfunc(...)` 函数指针类型暂不支持
  单独标调用约定。
- port 模块里的 `extern lib` 链接参数仍会传播；推荐 `from "*"` + `native` 段，
  避免平台库名写进源码。

## 4. `native` 字段

| 字段 | 含义 |
|---|---|
| `link` | 库名（→ `-l<name>`）、显式库文件（相对包根展开）、或以 `-` 开头的链接参数 |
| `libdirs` | `-L<包根/目录>` |
| `runtime` | 链接成功后复制到可执行文件旁的 DLL/so |
| `platforms` | 平台覆盖；键按 `OS_TYPE-OS_ARCH` → `OS_TYPE` → `OS_ARCH` → `"*"` 匹配 |

平台条目中的非空数组**覆盖** base 的同名字段（不做拼接）；未覆盖的字段继承
base。平台键复用 `OS_TYPE` / `OS_ARCH`（`WIN64`、`LINUX`、`X64`、`ARM64`…）。

### 链接参数规则

- `-x` / `--x`：原样传给链接器（用于 `-framework` 等）。
- 含 `/`、`\` 或 `.a/.lib/.so/.dll/.dylib/.tbd/.o/.obj` 后缀：按包根展开成路径。
- 其它：`-l<name>`。

### 运行时文件

`runtime` 只在真正产出可执行文件时处理（`clux build` / `clux run` / `clux test`）；
`--emit-c` / `--emit-obj` 不复制，由调用方负责。默认优先静态库，避免
`LD_LIBRARY_PATH` / DLL 搜索路径问题。

## 5. 宏

C 宏没有 ABI，clux 也不读 C 头文件。port 必须**手工**把宏写成等价物：

- 对象宏/常量：`export comptime var NAME: const i32 = 0;`（类型须为
  `const T`，或省略类型让编译器推断；纯编译期，import 方引用点折叠）。
- 函数宏/类型相关宏：在 port 里写等价的 `export comptime func`，或直接把
  逻辑内联到调用点；clux 不展开 C 宏。
- 条件宏：在 port 里写多个 `assert(OS_TYPE == ...)` 的声明/常量，或让
  `native.platforms` 选择不同常量。
- 不要把 `#define`、`#include`、C 头文件带进 clux 构建。

## 6. C 库的构建不属于 clux

clux 只做两件事：**编译 clux 源码**、**把已经编译好的静态/动态库链接进来**。
C 库必须先由外部构建系统（CMake/autotools/手工）编译好；`build.ps1` /
`build.sh` 属于那个 C 库自己的构建，不由 `clux build` 执行。

因此 `clux.mod` 只声明链接结果：

```json
"native": {
  "link": ["z"],
  "libdirs": ["vendor/lib/linux-x64"],
  "runtime": ["vendor/bin/win64/zlib1.dll"]
}
```

“用户必须先系统安装库”的解决办法是**包内 vendored 预编译产物**：把目标平台的
`.a` / `.lib`（以及运行期 DLL/so）放进依赖包，`clux build` 直接链接/复制；
`system` 库只是显式 fallback（`link` 里写系统库名）。

## 7. postinstall：下载/构建 C 库

如果包不想 vendored 预编译产物，可以在 `clux.mod` 里声明安装钩子：

```json
"postinstall": "sh scripts/build.sh"
```

平台差异（也支持 `"script"` 作为基础命令）：

```json
"postinstall": {
  "script": "sh scripts/build.sh",
  "WIN64": "powershell -NoProfile -File scripts/build.ps1"
}
```

`clux install` 负责执行：

```sh
clux install                  # 只列出 hook，不执行
clux install --allow-scripts  # 先依赖、后当前包，执行 hook
clux install --force          # 忽略缓存重跑
clux install --locked         # 只校验 clux.lock，不执行/不重写
```

- 脚本是**任意代码**，`clux build` / `run` / `test` 不会自动执行。
- cwd = 包根；注入 `CLUX_PKG_ROOT` / `CLUX_OS` / `CLUX_ARCH` / `CLUX_TARGET`。
- 成功后写 `.clux/postinstall.ok`（命令 + 推导脚本的 hash）；命令/脚本变化或
  `--force` 时重跑。
- 脚本负责固定下载源（commit/tag + checksum）、调用 CMake/autotools 等；
  产出的库由 `native.link` 指向。

### clux.lock

`clux install` 成功后会生成 `<包根>/clux.lock`，记录：

```json
{
  "version": 1,
  "target": "LINUX-X64",
  "packages": [
    {
      "name": "zlib",
      "version": "1.3.1",
      "path": "deps/zlib",
      "mod_sha256": "...",
      "postinstall": {
        "command": "sh scripts/build.sh",
        "command_sha256": "...",
        "script": "scripts/build.sh",
        "script_sha256": "..."
      },
      "artifacts": {
        "vendor/lib/libz.a": "..."
      }
    }
  ]
}
```

- 每个包的 `clux.mod`、postinstall 命令/可推导脚本、`native.link` /
  `native.runtime` 中**路径形式**的产物都会记录 SHA-256。
- `clux install --locked` 只校验，不运行 hook、不重写 lockfile。
- `clux build --locked` 在编译前校验；包图、`clux.mod`、脚本或产物变化都会
  报错并提示重新 `clux install`。
- `native.link` 里的裸库名（如 `"z"` → `-lz`）是系统库，不参与产物 hash。
- lockfile 不记录下载 URL/commit；脚本必须自己固定版本并校验 checksum。
- 还没有依赖自动下载/解析（`deps/` 仍需存在）；lockfile 只描述当前包图。

## 8. 已知限制

- 不支持 C 源码、C 头文件、宏、`shim.c`；这些必须在 clux 之外完成。
- DLL 数据符号（`extern var`）经 port 导入时目前按普通 `extern` 处理；需要
  `__declspec(dllimport)` 的 Windows DLL 数据符号暂不支持（函数不受影响）。
- 静态库链接顺序按模块/包顺序追加，复杂互相依赖的静态库可能需要
  `-Wl,--start-group` 或调整包顺序。
- `__stdcall` / `#pragma pack` / C bitfield 不能仅靠 `extern cfunc` 表达；
  `__stdcall` 用 `[[stdcall]]` 装饰器（见下），其余需要 C 侧 wrapper。
- C++ 库要求 `extern "C"` wrapper；v1 不承诺 C++ ABI。
- 依赖自动下载/解析尚未实现；`clux.lock` 只记录当前 `deps/` 图与产物 hash。
