/* ffi_cfi.h —— libffi 的"无 CFI"汇编辅助宏变体
 *
 * 上游 libffi 在 autotools 构建时由 configure 依据 HAVE_AS_CFI_PSEUDO_OP 生成
 * 本文件：支持 `.cfi_*` 伪指令的目标展开为真实指令，不支持的目标（如 Windows
 * COFF —— clang 集成汇编器不接受 .cfi_*）全部展开为**空**。
 *
 * 本仓库的 libffi 是纯源码 clone（无 configure 产物），故这里手写"空宏"变体，
 * 供 src/x86/win64.S 使用（该文件 include <ffi_cfi.h>）。
 */

#ifndef LIBFFI_CFI_H
#define LIBFFI_CFI_H

#define CFI_STARTPROC
#define CFI_ENDPROC
#define CFI_DEF_CFA
#define CFI_DEF_CFA_REGISTER
#define CFI_DEF_CFA_OFFSET
#define CFI_ADJUST_CFA_OFFSET
#define CFI_OFFSET
#define CFI_REL_OFFSET
#define CFI_REGISTER
#define CFI_RESTORE
#define CFI_REMEMBER_STATE
#define CFI_RESTORE_STATE
#define CFI_SIGNAL_FRAME
#define CFI_SAME_VALUE
#define CFI_DEF_CFA_REGISTER
#define CFI_ESCAPE
#define CFI_PERSONALITY
#define CFI_LSDA
#define CFI_GNU_ARGS_SIZE

#endif /* LIBFFI_CFI_H */
