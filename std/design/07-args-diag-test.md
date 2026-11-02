# std/design/07-args-diag-test.md — `std::args` / `std::diag` / `std::test`

> 状态：设计稿 v0.1（待评审）。依据：D-F1（确定性）、D-D1（FFI 边界）、D-E2（长协议名）。
> 规范对应：§3.14 test 块、§11 测试、§12 工具链（CLI）。

## 1. `std::args`：命令行原语

```clux
namespace std::args

struct Args {
    alloc: mem.Allocator,
    items: []str,        /* 不含 argv[0]（程序名单独取） */
}

func Args.from_process(alloc: mem.Allocator) -> Args      /* 经 sys 取 argv；复制成 arena 存储 */
func (a: *Args) len(self: *Args) -> usize
func (a: *Args) at(self: *Args, i: usize) -> ?str
func (a: *Args) all(self: *Args) -> []str
func (a: *Args) has_flag(self: *Args, name: str) -> bool          /* `--name` 精确匹配 */
func (a: *Args) value_of(self: *Args, name: str) -> ?str          /* `--name=v` 或 `--name v` */
func (a: *Args) positionals(self: *Args, alloc: mem.Allocator) -> []str
```

- **只提供原语**：不实现子命令/选项表/`--help`——这些属于各工具（编译器驱动）自己的策略。
- `value_of` 的优先级：先找 `--name=v`（同一 token），再找 `--name` 的下一个 token（不是以 `-` 开头）。
- 全部分配走显式 `Allocator`；`Args` 不拷贝（`__init_copy__` 移动 + `__dispose__`）。

## 2. `std::diag`：诊断是数据

```clux
namespace std::diag

tag union Severity { Error, Warning, Note }

struct Span {
    file: u32,     /* SourceMap 里的文件序号（稳定：按加载顺序） */
    lo: u32,       /* 字节偏移 */
    hi: u32,
}

struct Diag {
    severity: Severity,
    code: u32,            /* 稳定编号（枚举值），用于测试断言与 --json */
    span: Span,
    message: String,
    notes: []Diag,        /* 子诊断（"这行之前定义于…"） */
}

struct SourceFile {
    path: str,            /* 用户传入的原样路径；不规范化 */
    text: str,
    line_starts: []u32,   /* 行首偏移，按需构建（惰性） */
}

struct SourceMap {
    alloc: mem.Allocator,
    files: Vec[SourceFile],
}

func SourceMap.new(alloc: mem.Allocator) -> SourceMap
func (m: *SourceMap) add(self: *SourceMap, path: str, text: str) -> u32
func (m: *SourceMap) file(self: *SourceMap, id: u32) -> ?*SourceFile

struct Renderer {
    w: *fmt.Writer,
    color: bool,          /* 显式注入，不读环境变量 */
    source: *SourceMap,
}

func Renderer.new(w: *fmt.Writer, source: *SourceMap, color: bool) -> Renderer
func (r: *Renderer) render(self: *Renderer, d: *Diag): void
func (r: *Renderer) render_all(self: *Renderer, ds: []Diag): void
```

**确定性规则（硬性）**：

1. 输出前按 `(severity 权重, file, span.lo, code)` 排序；`render_all` 内部先排序。
2. 同一 `(code, span)` 只打印一次（去重）；节点级去重由**产生诊断的一方**负责（不靠位置字符串比较）。
3. 行/列从 1 开始；列按**字节**计（与 `str` 语义一致），不按字符。
4. 不输出地址、容量、hash、绝对路径规范化结果。
5. `color` 只影响 ANSI 转义，不影响文本内容；`--json` 渲染不含颜色。
6. 消息文本里嵌入的类型名/路径必须来自格式化函数（`fmt`），不在各处手写 `printf`。

## 3. `std::test`：`test` 块运行时

```clux
namespace std::test

struct Stats {
    passed: u32,
    failed: u32,
}

struct Runner {
    w: *fmt.Writer,       /* 失败报告的去处（stderr writer） */
    stats: Stats,
    filter: ?str,         /* `--filter=substr`，nil = 全部 */
}

func Runner.new(w: *fmt.Writer, filter: ?str) -> Runner
func (r: *Runner) begin(self: *Runner, name: str): void
func (r: *Runner) finish(self: *Runner): void
func (r: *Runner) fail(self: *Runner, message: str, span: Span): void   /* 记录一次失败 */
func (r: *Runner) check(self: *Runner, cond: bool, message: str, span: Span): void
func (r: *Runner) stats(self: *Runner) -> Stats
```

- 语言的 `test "name" { ... }` 块（§11）编译成对 `Runner` 的调用；`assert` 展开为 `check`。
- **不 panic 传播失败**：一次断言失败只记录并继续（与当前 conformance 行为一致），
  测试运行结束由驱动检查 `stats.failed == 0` 决定退出码。
- 输出格式固定（`test "<name>" ... ok/FAILED`），便于 CI 与 golden；`--filter` 不匹配的用例跳过且不计数。

## 4. 错误契约与确定性

| 场景 | 行为 |
|---|---|
| `Args.from_process` 分配失败 | panic `"out of memory"` |
| `value_of` 缺参数 | `nil` |
| 渲染未知文件 id | 打印 `<unknown file #id>`（不崩） |
| `render_all` 输入为空 | 不输出 |
| 测试失败 | 记录 + 继续；退出码由驱动决定 |
| 测试输出 | 不含时间戳/耗时（确定性）；耗时只进 `--verbose` 且不进 golden |

## 5. 测试清单

| 用例 | 断言 |
|---|---|
| `args.flag` | `--name` / `--name=v` / `--name v` 三种形态 |
| `args.positionals` | 混合选项后仍能取出位置参数 |
| `diag.sort` | 乱序输入渲染结果稳定（两次逐字节一致） |
| `diag.dedupe` | 同 `(code, span)` 只出现一次 |
| `diag.colByte` | 多字节内容下的列按字节计 |
| `diag.json` | JSON 转义合法（`\n`/`"`/`\\`）且字段顺序固定 |
| `test.fail` | 断言失败继续执行后续断言；`stats` 正确 |
| `test.filter` | 过滤生效且不影响计数 |

## 6. 开放问题

1. `diag` 的 `message` 用 `String`（拥有）还是 `str` + arena：倾向 `String`（诊断跨越阶段存活）。
2. JSON 渲染器是否 S0 就需要：倾向 S3（先文本）。
3. `test` 的 `assert` 是否要支持自定义消息（`assert(cond, "msg")`）：语言侧已有；std 侧只接 `check`。
