# clux 包系统设计

## 包标识

- 目录根下存在 `clux.mod` 文件即视为包
- `clux.mod` 为 JSON 格式

## clux.mod 结构

- `name`：包名
- `version`：版本号，必须与 git tag 对应（非 tag 提交不校验）
- `entry`：入口，格式 `string | Record<string, string>`（二选一）
  - `string`：单入口文件，如 `"src/main.clux"`
  - `Record<string, string>`：多子包，如 `{ "std/array": "src/array.clux" }`
  - 两种形式互斥
- 依赖描述（协议:名字@版本）

## 子包

- 子包名必须以当前包 `name` 为前缀（如包 `std` → 子包 `std/xxxx`）

## 依赖管理

- 去中心化，多种协议，格式：`协议:名字[@tag|分支|commit]`
  - `file:/usr/local/clux-std`（本地路径）
  - `github:clux/std@0.1.0`（Git 协议）
- 依赖下载到 `deps/` 目录，平铺不嵌套
  - 传递依赖平铺：A→B→C，A 的 `deps/` 同时有 B 和 C，B 用软链接指向外部的 C
- 版本冲突直接报错，不允许
- `clux install`：下载依赖到 `deps/`，生成/更新 `clux.lock`
- `clux.lock`：锁定依赖版本与 hash（类似 `package-lock.json` / `go.mod`）

## import 查找

- 代码中非路径的 import 自动从 `deps/` 下查找

## 标准库

- 普通依赖，无特殊地位
- 仓库：`https://github.com/clux-lang/std.git`，本地 `D:\projects\std`（暂空）
- 编译器自带对应版本的压缩标准库，创建项目时本地复制
- 用户可手动升级

## 命令行为

- 单 entry 项目：`clux run`/`fmt`/`test`/`build` 无需跟文件名，自动从 entry 查找；`fmt` 对全项目文件生效
- 多 entry 子包项目：命令跟随子包名，如 `clux run std/array`
