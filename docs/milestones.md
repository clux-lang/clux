# clux 里程碑规划

## M1: 最小可执行子集 ✅ 已完成

**目标**：实现"裁剪后的 C"子集的解释器

**语言特性**：基础类型(i8/i16/i32/i64/u8/u16/u32/u64/f32/f64/bool/void)、var 变量声明(含推断)、func 函数定义(a:type 风格)、if/else、while、for、break/continue/return、算术/比较/逻辑/位/赋值运算、true/false 字面量、printf 硬编码、C 风格注释

**不支持**：指针、所有权、struct/union/enum、数组、字符串、模块、错误处理、预编译指令

**交付标准**：`clux run hello.cx` 能正确执行含变量、运算、控制流、函数调用的程序 ✅ 8 个 examples 端到端通过、764 单元测试全过

**任务**：T1 骨架 → T2 Lexer → T3 AST → T4 Parser → T5 类型系统 → T6 语义分析 → T7 解释器 → T8 端到端集成 ✅ 全部完成

---

---

## M2: 与 C 表达力齐平（指针除外）✅ 已完成

**目标**：补全语言功能达到 C 的表达力（指针除外），保持 clux 自己的语法风格

**语言特性**：struct、enum（严格分离）、静态数组 `[N]T`、元组 `<T1,T2>`、switch（if 语法糖）、do-while、type 别名 + 类型计算、sizeof/alignof/typeof、函数类型、位运算复合赋值、三元表达式、元组↔数组互转、鸭子类型协议

**交付标准**：全部单元测试通过 + 8+ 个新 examples 端到端通过 ✅ 1421 单元测试全过、examples 端到端通过（2026-09-29）

**设计文档**：[m2-design.md](m2-design.md)

**任务**：
- Phase 0: Lexer + AST 基础（新关键字/符号 + 新 AST 节点 + `parse_type_expr`）
- Phase 2: VM 类型系统扩展（复合类型 + interning + vtable + `is_own` 借用字段）
- Phase 3: Sema 扩展（`resolve_type` 升级 + 类型计算 + 鸭子类型检查）
- Phase 4: 构造 + 访问（`.<type>{...}` + 字段访问 + enum variant + 新字节码）
- Phase 5: 控制流 + 表达式补全（switch desugar + do-while + 三元 + 位运算复合赋值）
- Phase 6: 集成 + 测试（examples + 全测试通过 + 文档更新）
- 工具链（非主线）: 字节码 `.cxb` 与汇编 `.cxs`，统一由 `bc` 子命令提供（`bc emit` 源码→字节码 / `bc asm` 汇编 / `bc disasm` 反汇编，`-o` 指定输出）；`run` 按内容判定（有 `CXBC` 头当字节码，否则源码）；`build` 专责未来的机器码二进制（未实现）✅ 已完成（2026-09-12）

---

## 后续里程碑（待详细设计）

- **M3**: 指针与所有权系统 Phase 1
- **M4**: 切片与字符串（静态数组已提前到 M2）
- **M5**: FFI 系统
- **M6**: 模块系统
- **M7**: 转译 C 后端
- **M8**: 所有权系统 Phase 2（借用与生命周期）
- **M9**: 错误处理机制
- **M10**: 标准库 v1
