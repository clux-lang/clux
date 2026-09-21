#ifndef _H_CLUX_VM_SCOPE_FRAME_
#define _H_CLUX_VM_SCOPE_FRAME_
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* ---- 前向声明（反推宏展开需要完整类型，故由包含方提供） ---- */

typedef struct scope_t      scope_t;
typedef struct _sema_scope_t sema_scope_t;

/**
 * scope_frame_t: 作用域父子关系节点（与 scope 同步的轻量结构）
 *
 * value 持有指向所属作用域 frame 的指针（借用 &scope->frame），而非直接
 * 持有 scope_t*：
 *   - 避免 value 与 scope 强耦合（value.h 无需 include scope.h，防止
 *     scope.h 已 include value.h 造成的循环依赖）
 *   - frame 生命周期 = 所属 scope 生命周期；scope 销毁前其内所有 value
 *     必已销毁（scope_destroy 遍历 owned），无悬空指针
 *   - 所有权分析（作用域逃逸判定）时经 parent 链比较两个 frame 的祖先
 *     关系即可，无需触碰 scope 本体
 *
 * vm 侧 scope_t 与 sema 侧 sema_scope_t 均内嵌本结构，反推宏用
 * offsetof/container_of 模式由 frame 指针还原所属 scope。
 */
typedef struct scope_frame_t {
    struct scope_frame_t *parent; /* 父 frame（与 scope->parent 同步；根=NULL） */
} scope_frame_t;

/** 由 vm 侧 frame 指针反推所属 scope_t（offsetof/container_of 模式） */
#define scope_from_frame(f) \
    ((scope_t *)((char *)(f) - offsetof(scope_t, frame)))

/** 由 sema 侧 frame 指针反推所属 sema_scope_t（offsetof/container_of 模式） */
#define sema_scope_from_frame(f) \
    ((sema_scope_t *)((char *)(f) - offsetof(sema_scope_t, frame)))

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_SCOPE_FRAME_ */
