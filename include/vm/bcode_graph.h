#ifndef _H_CLUX_VM_BCODE_GRAPH_
#define _H_CLUX_VM_BCODE_GRAPH_

#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"
#include "core/vec.h"
#include "vm/bcode.h"
#include "vm/module.h"
#include "vm/vm.h"

#include <stddef.h>
#include <stdint.h>

/* ===========================================================================
 * 模块图容器（`.cxb` v2）
 *
 * M5 的模块语义必须在字节码层闭环：一个可执行的 `.cxb` 必须自包含它 import
 * 的全部模块，否则 `clux run x.cxb` 会在 `IMPORT` 指令处报 module not found
 * （运行期按规范路径查 `vm->modules`，而依赖模块不在容器里）。因此 v2 容器
 * 从"单模块"扩展为"模块表"。
 *
 * 每个模块在容器里的内容：
 *   - path：规范路径（`vm->modules` 的 key）。**入口模块写空串**——运行期只有
 *     `IMPORT` 会按路径查表，而入口不可能被 import（那构成循环依赖，编译期
 *     已拒绝），因此入口路径无需存储。
 *   - 字符串表 + 字节码：见 `vm/bcode_serial.h` 的单模块段格式。
 *   - 导出表：`(name, owner_index)`。导出名单来自 AST 的 `is_exported`
 *     （**不在字节码里**），所以必须随容器携带；`owner_index` 指向"值真正
 *     所在的模块"（自身或更早的依赖模块）——`export * from "./x"` 代理导出
 *     的值借用依赖模块的 `global_scope`，编译期已解析成最终表，容器只需记录
 *     值的位置，运行期即可原样重建。
 *
 * 模块顺序 = `vm->modules` 的插入顺序 = **依赖优先**（依赖先编译先登记），
 * 因此加载方按表序逐个"执行注册段 → 重建导出表 → 绑定模块"即可保证依赖
 * 先于使用者就绪。
 * =========================================================================== */

/** 容器格式版本：1 = 单模块（`bcode_serial`），2 = 模块表（本文件），
 *  3 = 模块表 + **每模块 FFI 库表**（M6：`extern lib` 的 别名→路径）。
 *  加载方接受 1/2/3；v1/v2 产物按"无 FFI 声明"读取（旧产物继续可运行）。 */
#define BCODE_GRAPH_VERSION 3u

/** 单个模块的落盘映像。 */
typedef struct {
    /* **源码里写的路径**（`"./math"`、`"./lib/api"`）——不是规范化后的宿主
       路径；入口模块为空串。加载方以"导入者的规范路径"为基准重新规范化，
       得到 vm->modules 的 key（与运行期 op_import 对 IMPORT 操作数所用的
       规则完全一致）。 */
    char       *import_path;
    /* 导入者模块下标（BCODE_NO_MODULE = 入口模块）。 */
    uint32_t    importer_index;
    /* **加载时**按导入链重新规范化得出的规范路径（alloc 拥有，NUL 终止）。
       序列化不写它——由 import_path + importer 链推导，产物因此与宿主目录
       布局解耦（换个目录照样解析）。 */
    char       *canonical;
    bytecode_t *bc;            /* 模块字节码（alloc 拥有） */
    char      **export_names;  /* export_count 个名字（alloc 拥有，NUL 终止） */
    uint32_t   *export_owners; /* 与 export_names 一一对应的 owner 模块下标 */
    size_t      export_count;

    /* ---- M6 FFI：本模块的 `extern lib` 声明（别名 → 库路径）----
     * 容器**必须**携带它们：`module_t.ffi_libs` 是编译期扫描 AST 建的，字节码里
     * 只剩 `EXTERN_CALL "别名::符号"`——加载方无从知道该 dlopen 哪个文件。
     * 缺这一段时 FFI 程序的 `emit → run .cxb` 会在第一次 `EXTERN_CALL` 处报
     * "no loaded library"。两个数组同长 ffi_count（alloc 拥有，NUL 终止），
     * 顺序即源文件里的声明顺序。 */
    char      **ffi_aliases;
    char      **ffi_paths;
    size_t      ffi_count;
} bcode_module_image_t;

/** importer_index 的"无导入者"取值（入口模块）。 */
#define BCODE_NO_MODULE 0xFFFFFFFFu

/** 模块图（依赖优先顺序；entry_index 指向入口模块）。 */
typedef struct {
    allocator_t          *alloc;
    bcode_module_image_t *modules;
    size_t                count;
    size_t                entry_index;
} bcode_graph_t;

/** 初始化为空图（加载失败时也是这个状态，可安全 destroy）。 */
void bcode_graph_init(bcode_graph_t *g);

/** 释放图内全部内存（bc / path / 导出表）并复位。允许 NULL / 空图。 */
void bcode_graph_free(bcode_graph_t *g);

/**
 * 序列化 `vm->modules` 的整张模块图（依赖优先），`entry` 为入口模块。
 *
 * 入口模块在容器里的 path 写为空串（见文件头说明）。导出表的 owner 下标按
 * "依赖优先"顺序解析：值指针与更早模块的同名导出相同即归该模块。
 *
 * @return 0 成功；非 0 失败（参数非法 / 入口不在 vm->modules / 空图 / 写失败）
 */
int bcode_graph_serial_file(allocator_t *alloc, const vm_t *vm,
                            const module_t *entry, const char *out_path);

/** 序列化到内存（调用方用 allocator_free 释放；失败返回 NULL）。 */
uint8_t *bcode_graph_serial_mem(allocator_t *alloc, const vm_t *vm,
                                const module_t *entry, size_t *out_len);

/**
 * 直接序列化一张已加载/已汇编的图（`.cxs` → `.cxb` 路径：导出表的 owner
 * 下标由文本携带，原样落盘，因此与源 `.cxb` 逐字节一致）。
 */
int bcode_graph_serial_image_file(allocator_t *alloc, const bcode_graph_t *g,
                                  const char *out_path);
uint8_t *bcode_graph_serial_image_mem(allocator_t *alloc,
                                      const bcode_graph_t *g,
                                      size_t *out_len);

/**
 * 从内存缓冲反序列化模块图（v2 模块表；v1 单模块文件也接受——按"单模块、
 * 空路径、无导出"载入，保持旧产物可运行）。
 *
 * 严格校验魔数/版本/各段边界；失败时 `out` 保持空图并打印诊断到 stderr。
 *
 * @return 0 成功；非 0 失败
 */
int bcode_graph_deserial(allocator_t *alloc, const uint8_t *data, size_t len,
                         bcode_graph_t *out);

/** 从 `.cxb` 文件读取并反序列化（语义同 bcode_graph_deserial）。 */
int bcode_graph_from_file(allocator_t *alloc, const char *path,
                          bcode_graph_t *out);

/**
 * **按导入链重新规范化路径**（加载方必调，VM 层路径推导的唯一入口）：
 * 入口模块的规范路径取 `entry_root`（通常 = `.cxb`/`.cxs` 文件自身的规范化
 * 路径），其余模块 `canonical = module_path_resolve(import_path, 其导入者的
 * canonical)`——与运行期 op_import 对 `IMPORT <源码路径>` 的推导逐字一致，
 * 因此两侧算出的 key 必然吻合，容器也与宿主目录布局解耦。
 *
 * @return 0 成功；非 0 = 有模块无法从入口到达（格式损坏）
 */
int bcode_graph_resolve_paths(allocator_t *alloc, bcode_graph_t *g,
                              const char *entry_root);

/* ===========================================================================
 * 模块图的文本形态（`.cxs`）
 *
 * 单模块产物的文本与旧格式**逐字一致**（无任何指令行）——5 个手写 `.cxs`
 * 样本与既有往返测试因此不受影响。多模块产物在每段模块代码前插入指令：
 *
 *     MODULE "./lib.cx"      依赖模块：给出规范路径（IMPORT 查找的 key）
 *     export add 0           导出项：名字 + owner 模块下标
 *     _start:                该模块的反汇编代码（原样交给单模块汇编器）
 *         ...
 *     MODULE                 入口模块：路径为空（裸 MODULE）
 *     ...
 *
 * 模块顺序 = 容器表顺序（依赖优先），入口模块可以是任意一段（用裸 MODULE
 * 标记）。`export` 的 owner 下标是模块表下标，与容器中的编码一致。
 * =========================================================================== */

/** 渲染已加载的图为 `.cxs` 文本（alloc 拥有，NUL 终止）。 */
char *bcode_graph_disasm_mem(allocator_t *alloc, const bcode_graph_t *g,
                             size_t *out_len);

/** 渲染图为 `.cxs` 文本并写文件。 */
int bcode_graph_disasm_to_file(allocator_t *alloc, const bcode_graph_t *g,
                               const char *out_path);

/**
 * emit 侧便捷入口：把"已编译的 vm + 入口模块"渲染为 `.cxs` 文本并写文件。
 * 内部走"序列化到内存 → 反序列化 → 渲染"，因此与 `.cxb → .cxs` 的输出**逐字
 * 一致**（单一渲染器，无需重复实现 owner 下标推导）。
 */
int bcode_graph_disasm_file(allocator_t *alloc, const vm_t *vm,
                            const module_t *entry, const char *out_path);

/** 解析 `.cxs` 文本为模块图（无 MODULE 指令 = 单模块入口，向后兼容）。 */
int bcode_graph_asm_parse(allocator_t *alloc, const char *text, size_t len,
                          bcode_graph_t *out);

/** 从 `.cxs` 文件读取并解析为模块图。 */
int bcode_graph_asm_from_file(allocator_t *alloc, const char *path,
                              bcode_graph_t *out);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_BCODE_GRAPH_ */
