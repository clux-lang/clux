# clux cmd: 子命令分发框架

> 本文档是 `src/cmd/cmd.c` 及各子命令实现的代码级约束文档。
> 后续开发者在修改 cmd 模块或新增子命令前必须阅读并遵守本文档中的规则。

---

## 1. 核心抽象

### 1.1 cmd_t

```
typedef struct {
    const char *name;        // 子命令名称，如 "build"
    const char *usage;       // 一行用法，如 "clux build [options]"
    const char *help;        // 多行描述，--help 时打印
    cmd_handler_fn_t handler; // 命令处理函数
} cmd_t;
```

命令表为 `static const cmd_t[]` 数组，定义在 `main.c` 中。新增子命令只需：
1. 在 `src/cmd/xxx.c` + `include/cmd/xxx.h` 中实现 `cmd_xxx`
2. 在 `main.c` 中 `#include "cmd/xxx.h"` 并在命令表中添加条目

### 1.2 cmd_args_t

```
typedef struct {
    const cmd_opt_t *opts;  // 解析后的选项数组
    size_t optc;            // 选项数量
    char *const *posargs;   // 位置参数（指向 argv 原始槽位）
    size_t posc;            // 位置参数数量
} cmd_args_t;
```

**零堆分配：** 所有指针指向原始 argv 字符串，opts 数组写入调用者提供的栈上 buffer。

### 1.3 cmd_opt_t

```
typedef struct {
    const char *key;    // 不含 "--" 前缀，如 "verbose" 对应 --verbose
    const char *value;  // NULL 表示 flag，否则为 = 后的值字符串
} cmd_opt_t;
```

对于 `--key=value`，原地将 `=` 替换为 `\0`（argv 字符串按 C 标准可修改），key 和 value 分别指向分割后的两段。

---

## 2. 参数解析

### 2.1 cmd_args_parse

```
size_t cmd_args_parse(int argc, char **argv, cmd_opt_t *opt_buf,
                      size_t opt_buf_len, cmd_args_t *out);
```

识别规则：
- `--key=value` → opts[i] = {key="key", value="value"}
- `--key`       → opts[i] = {key="key", value=NULL}
- `--`          → 跳过（bare separator）
- 其他          → 位置参数

**argv 修改：** 对于 `--key=value`，原地将 `=` 替换为 `\0`。调用方必须意识到 argv 内容被修改。

**opt_buf 溢出：** 若选项数量超过 `opt_buf_len`，超出部分被静默丢弃。

### 2.2 位置参数

位置参数通过 `posargs` 和 `posc` 访问。当没有选项时，`posargs` 指向 argv[0]，`posc` 等于 argc。当有选项时，`posargs` 指向第一个位置参数。

**注意：** 当前实现要求位置参数出现在所有选项之后。混合排列时，选项前的位置参数会被计入 posc 但 pos_start 可能不正确。

---

## 3. 分发流程

### 3.1 cmd_dispatch

```
int cmd_dispatch(const cmd_t *cmds, size_t ncmds, int argc, char **argv);
```

1. `argc < 2`：打印顶层帮助，返回 1
2. `argv[1]` 为 `--help` 或 `-h`：打印顶层帮助，返回 0
3. 在 cmds 中查找 `argv[1]` 匹配的子命令
4. 未找到：报错并提示 `--help`，返回 1
5. 解析 `argv[2..]` 为 `cmd_args_t`（栈上 `opt_buf[32]`）
6. 若子命令收到 `--help`：打印子命令帮助，返回 0
7. 调用 `cmd->handler(&args)`

### 3.2 cmd_print_help

```
void cmd_print_help(const char *prog, const cmd_t *cmds, size_t ncmds);
```

输出格式：
```
Usage: <prog> <command> [options]

Commands:
  <name>       <usage>
  ...

Options:
  --help       Show this help message
```

---

## 4. 子命令文件组织

```
include/cmd/   cmd.h, path.h, format.h, build.h, run.h, bc.h, test.h, version.h, eval.h
src/cmd/       cmd.c, path.c, format.c, build.c, run.c, bc.c, test.c, version.c, eval.c
include/parser/ fmt.h（格式化核心）
src/parser/     fmt.c
```

`path.h/path.c` 提供输出路径推导（`cmd_derive_out_path`）与选项/推导解析（`cmd_resolve_output`），供需要产出文件的子命令复用，避免各命令重复实现扩展名替换逻辑。

### 4.1 约定

- 每个子命令一个 `.c`/`.h` 对，文件名与命令名一致
- `.h` 仅声明 `int cmd_xxx(const cmd_args_t *args);`
- `.c` 实现 handler，`#include "cmd/xxx.h"`
- handler 返回 0 表示成功，非零表示失败
- 占位命令输出 `<name>: not implemented` 到 stderr 并返回 1

### 4.2 CMake

子命令源文件加入 `clux_cmd` 静态库。需要 ICU 的子命令（如 version）通过 `clux_cmd` 的 `target_link_libraries` 获取。

---

## 5. 查找函数

### 5.1 cmd_args_get

```
const char *cmd_args_get(const cmd_args_t *args, const char *key);
```

线性搜索 opts 数组，返回第一个匹配 key 的 value。key 不存在或为 flag（value=NULL）时返回 NULL。

**限制：** 无法区分"key 不存在"和"key 存在但是 flag"——需用 `cmd_args_has` 判断存在性。

### 5.2 cmd_args_has

```
bool cmd_args_has(const cmd_args_t *args, const char *key);
```

线性搜索 opts 数组，返回 key 是否存在（不论是否有 value）。

### 5.3 cmd_args_pos

```
const char *cmd_args_pos(const cmd_args_t *args, size_t i);
```

返回第 i 个位置参数，越界返回 NULL。

---

## 6. 选项风格约定

### 6.1 统一为双横线 `--key`

clux 的选项语法统一为 GNU 风格 `--key` / `--key=value` / `--key value`：

- `cmd_args_parse` **只识别 `--` 前缀**为选项；单横线 `-key` 会落入位置参数
- **禁止**在 handler 中为单横线形式手写特判（历史上 `run` 曾特判 `-asm`/`-bin`，已移除）——这会造成各命令风格分裂，且与 `cmd_args_parse` 的契约冲突
- 若首个位置参数以 `-` 开头（用户误用单横线），handler 应给出"unknown option (clux options use '--')"类明确诊断，而非将其当作文件名尝试打开
- **唯一例外**：`-o`（输出路径）是业界通用短选项，`bc` handler 可显式扫描识别 `-o PATH` / `-o=PATH`。除 `-o` 外不得再引入单横线选项

### 6.2 命令职责划分（主线 vs 非主线）

**`build` 的唯一职责是产出机器码二进制（主线产物）**，由未来的转译/原生后端实现（M7 / M12）。当前未实现，命令直接报 `not implemented`。

字节码（`.cxb`）与汇编文本（`.cxs`）都是**非主线**的中间/调试产物，归入独立的 `bc`（bytecode）子命令，**不得挂到 `build` 下**：

| 命令 | 职责 | 产物 |
|------|------|------|
| `build <file.cx>` | 编译为机器码二进制（**未实现**） | 原生可执行文件 |
| `bc emit <file.cx>` | 源码 → 字节码 | `.cxb` |
| `bc asm <file.cxs>` | 汇编文本 → 字节码 | `.cxb` |
| `bc disasm <file.cxb>` | 字节码 → 汇编文本 | `.cxs` |
| `run <file>` | 运行（按内容判定源码/字节码） | — |
| `format <file.cx>` | 源码格式化（原地 / `-o` / stdin） | 格式化后的源码 |

**理由**：`build` 是编译器的对外主入口，语义应聚焦"产出可执行的机器码"。若把字节码/汇编产出塞进 `build`，会让它看起来像字节码工具，掩盖其真正的目标。

### 6.2.1 format 的实现策略与风格

`format` 走 **token 流规整** 路线（`src/parser/fmt.c`），不遍历 AST、不重排代码结构：
- 输入是 lexer 产出的**完整 token 池**（含 WHITESPACE / COMMENT / MULTILINE_COMMENT），故注释天然被保留——这是不走"AST 打印"路线的根本原因（AST 不含注释）
- 空白 token 中是否含换行/空行，用于保留用户的分行与分组意图
- 风格规则：4 空格缩进（TAB 被替换）、`{` 跟随前行、`{` 后换行（空块 `{}` 紧凑）、`}` 单独一行、`;` 后换行（`()` 内除外）、`} else {` 同行
- **必须幂等**：格式化结果再格式化不变（由 `tests/fmt_test.cpp` 保证）
- **必须语义等价**：格式化后代码运行结果不变（测试覆盖 + 示例回归验证）

> 该契约在 2026-10 曾被违反（顶层 `union`/`import`/`export` 被静默删除、单遍不幂等），
> 现已修复并补齐回归测试（`FmtRegression.*`）；历史缺陷与根因见
> [../docs/format-rules.md](../docs/format-rules.md) §9。

### 6.3 `run` 的输入判定：按内容，不按扩展名

`run` **不感知汇编文本（`.cxs`）这一中间态**，只认两种输入：

| 判定 | 行为 |
|------|------|
| 文件以 `"CXBC"` 二进制头开头 | 当字节码加载执行（`driver_run_bin`） |
| 否则 | 当 clux 源码编译执行（`driver_run_file`） |

- 判定依据是**内容**（`driver_detect_input` 的 magic 检查），扩展名完全不参与
- 传入 `.cxs` 时会走源码路径并按其真实内容报编译错误，**不会**隐式汇编执行——汇编是独立的中间态，需要显式经 `bc asm` 转成 `.cxb` 再运行
- 因此 `run` **没有任何模式选项**（无 `--asm` / `--bin` / `--input`）

### 6.4 `bc` 的输出路径

`bc` 的输出路径按优先级取：`-o PATH` / `-o=PATH` → `--output=PATH` → 第二位置参数 → 按输入同名换扩展名（`.cxb` / `.cxs`）。

`-o` 是**单横线短选项**，`cmd_args_parse` 只识别 `--` 前缀，故会落入位置参数，由 `bc` handler 自行扫描识别（见 6.1 例外说明）。

---

## 7. 禁止事项

1. **禁止** 依赖 argv 内容在 `cmd_args_parse` 后保持原样——`--key=value` 中的 `=` 被替换为 `\0`。
2. **禁止** 在 handler 中使用 malloc/free——遵循项目统一内存通道规则。
3. **禁止** 缓存 `cmd_args_t` 的指针超出 handler 生命周期——opts 指向栈上 buffer。
4. **禁止** 在 opt_buf 溢出时假设所有选项都被解析——超过 32 个选项时静默截断。
5. **禁止** 混合排列选项和位置参数——当前解析器不保证正确处理 `posarg --opt posarg` 的顺序。
