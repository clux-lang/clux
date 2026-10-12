#ifndef _H_CLUX_MODULE_MODULE_PATH_
#define _H_CLUX_MODULE_MODULE_PATH_

#include "core/allocator.h"
#include "core/strslice.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * import 路径分类。
 *
 * **只按路径形式判定，绝不嗅探文件扩展名**：扩展名不表示文件类型
 * （`libc.so.6` 可能只是普通文件，某个 `foo.dll` 也可能根本不是动态库），
 * 靠后缀猜类型会给出误导性诊断、并把错误的语义假设固化进语言。
 *
 * 因此 M5 只有两类：
 *   - 相对路径（`./x` / `../y`）→ 源码模块，M5 支持；
 *   - 其它一切 → M5 不支持（包导入；动态库绑定留给 M6，且必须由**显式语法**
 *     表达，而不是由文件名后缀推断）。
 */
typedef enum {
    MODULE_IMPORT_MODULE = 0,  /* ./x、../y：源码模块（M5 实现） */
    MODULE_IMPORT_NON_PATH,    /* 非相对路径：M5 不支持（诊断见 driver 的 import 解析回调） */
} module_import_kind_t;

/** 按路径文本分类（纯词法：只看是否相对路径，不访问文件系统、不看后缀）。 */
module_import_kind_t module_import_classify(strslice_t raw);

/** 分类的稳定名称（诊断用）："module" / "non-relative path" */
const char *module_import_kind_name(module_import_kind_t kind);

/**
 * 词法规范化路径（不访问文件系统）：
 *   - `\` → `/`（Windows 分隔符统一）
 *   - 折叠 `.` 段与 `..` 段、合并重复 `/`
 *   - 保留 Windows 盘符前缀（`C:`）与 POSIX 前导 `/`
 * 返回 alloc 分配、NUL 结尾的字符串；失败返回 NULL。
 */
char *module_path_normalize(allocator_t *alloc, const char *path);

/**
 * 把 import 路径解析为**规范模块路径**（相对被导入文件所在目录）。
 *
 * - MODULE_IMPORT_MODULE：返回规范路径；最后一段无扩展名时补 `.cx`
 *   （显式写 `.cx` 亦可）；`importer_path` 为 NULL 时按当前工作目录解析。
 * - 其余类别：`*out_kind` 置为对应类别并返回 NULL，由调用方按类别报错。
 *
 * 返回 alloc 分配、NUL 结尾的字符串；失败返回 NULL（此时 *out_kind 仍有效）。
 */
char *module_path_resolve(allocator_t *alloc, strslice_t raw,
                          const char *importer_path,
                          module_import_kind_t *out_kind);

/** 模块文件是否存在且可读（用于 "module not found" 精确诊断）。 */
bool module_path_exists(const char *path);

/**
 * 统一 import 路径解析：相对路径 + 非路径（deps 包导入）。
 *
 *   - 相对路径（./ ../）：同 module_path_resolve，相对 importer 拼接 + 补 .cx。
 *   - 非路径（如 "std"、"std/array"）：从 importer 向上查找 clux.mod 得到包根，
 *     再在 deps/<第一段>/ 下解析依赖包的 clux.mod 得到入口文件规范路径。
 *
 * 返回 alloc 分配、NUL 结尾的规范路径；失败返回 NULL（*out_kind 仍有效）。
 * 对于非路径 import，*out_kind 设为 MODULE_IMPORT_NON_PATH（即使成功），
 * 以便调用方区分诊断措辞。
 */
char *module_resolve_import(allocator_t *alloc, strslice_t raw,
                            const char *importer_path,
                            module_import_kind_t *out_kind);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_MODULE_MODULE_PATH_ */
