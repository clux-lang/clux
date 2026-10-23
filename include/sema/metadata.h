#ifndef _H_CLUX_SEMA_METADATA_
#define _H_CLUX_SEMA_METADATA_

#include "core/strslice.h"
#include "parser/ast_node.h"
#include "parser/ast_program.h"
#include "parser/ast_struct_def.h"
#include "sema/symbol.h"
#include "vm/type.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sema_type_t sema_type_t;
struct sema_t;

typedef enum {
    SEMA_META_TYPE = 0,
    SEMA_META_FIELD,
    SEMA_META_METHOD,
} sema_meta_kind_t;

typedef struct sema_meta_entry {
    strslice_t        key;
    strslice_t        owner;   /* field/method name；类型元信息为空 */
    sema_ct_const_t   value;
    struct sema_meta_entry *next;
} sema_meta_entry_t;

const sema_meta_entry_t *sema_meta_find(const sema_type_t *st,
                                         sema_meta_kind_t kind,
                                         strslice_t owner, strslice_t key);

/* 返回值条目；duplicate 返回 NULL（由调用方报诊断）。 */
sema_meta_entry_t *sema_meta_add(struct sema_t *sema, sema_type_t *st,
                                  sema_meta_kind_t kind, strslice_t owner,
                                  strslice_t key, const sema_ct_const_t *value);

void sema_metadata_apply_type(struct sema_t *sema, const type_t *type,
                              ast_node_t *decorators,
                              struct _sema_scope_t *scope);
void sema_metadata_apply_members(struct sema_t *sema, const type_t *type,
                                 ast_struct_def_t *sd,
                                 struct _sema_scope_t *scope);
const sema_meta_entry_t *sema_metadata_lookup(struct sema_t *sema,
                                              const type_t *type,
                                              sema_meta_kind_t kind,
                                              strslice_t owner,
                                              strslice_t key);

/* 若 node 是 get_metadata/get_field_metadata/get_method_metadata 调用，
   折叠为字面量并返回 true。 */
bool sema_metadata_try_get(struct sema_t *sema, ast_node_t **node,
                           struct _sema_scope_t *scope, value_t **out);

/* 在函数签名/全局实体就绪后统一应用类型/字段/方法装饰器。 */
void sema_metadata_apply_all(struct sema_t *sema, ast_program_t *prog);

#ifdef __cplusplus
}
#endif
#endif
