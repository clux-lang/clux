# 8. 模块、包与 FFI

## 8.1 模块

一个 `.cx` 文件是一个模块。模块通过顶层 `export` 暴露声明：

```clux
export func add(a: i32, b: i32): i32 { return a + b; }
export struct User { name: str; }
```

未导出的顶层声明只在当前模块可见。

## 8.2 导入

```clux
import math from "math.cx";
import app from "app/main.cx";
```

语法：

```text
import <alias> from "<path>";
```

路径可以是相对路径或包内路径。

## 8.3 访问导入成员

```clux
import math from "math.cx";

func main(): i32 {
    return math::add(1, 2);
}
```

命名空间访问使用 `::`。

## 8.4 重新导出

```clux
export * from "other.cx";
```

重新导出把目标模块的导出表转发到当前模块。

## 8.5 包清单

包根目录包含 `clux.mod`，JSON 格式：

```json
{
  "name": "example",
  "version": "0.1.0",
  "entry": "src/main.cx"
}
```

多入口/子包：

```json
{
  "name": "example",
  "version": "0.1.0",
  "entry": {
    "example/app": "src/app.cx",
    "example/lib": "src/lib.cx"
  }
}
```

字段：

- `name`：包名，必需。
- `version`：版本字符串，可选。
- `entry`：入口文件，必需；可以是单个路径或子包名到路径的映射。
- `deps`：依赖表，可选；键为依赖名，值为依赖规格。

子包名必须以包名为前缀。

## 8.6 依赖

```json
{
  "deps": {
    "std": "file:../std",
    "http": "github:clux/http@0.1.0"
  }
}
```

依赖安装在包根目录的 `deps/` 下；依赖解析不嵌套。

## 8.7 非路径导入

```clux
import std from "std/string";
```

非相对路径导入从当前包的 `deps/` 中解析；包名是路径第一段。

## 8.8 动态库声明

```clux
extern lib crt from "c";
```

`"*"` 表示当前进程自身已链接的符号。

## 8.9 外部 C 函数

```clux
extern cfunc crt::puts(s: *const u8): i32;
```

`extern cfunc` 的函数没有函数体，由运行时动态解析符号。

## 8.10 外部变量

```clux
extern var crt::errno: i32;
```

外部变量可读写，读写直接作用于动态库中的符号地址。

## 8.11 C ABI 定义

```clux
cfunc clux_add(a: i32, b: i32): i32 {
    return a + b;
}
```

`cfunc` 定义按 C ABI 暴露；它没有捕获环境，不能泛型。
