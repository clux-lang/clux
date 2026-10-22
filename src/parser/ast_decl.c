#include "parser/ast_decl.h"

#include "parser/ast_cunion_def.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_func_def.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_type_def.h"
#include "parser/ast_union_def.h"

bool ast_decl_is_generic(const ast_node_t *node) {
    if (!node) return false;
    switch (node->kind) {
    case AST_FUNC_DEF:
        return ((const ast_func_def_t *)node)->generic.count > 0;
    case AST_STRUCT_DEF:
        return ((const ast_struct_def_t *)node)->generic.count > 0;
    case AST_ENUM_DEF:
        return ((const ast_enum_def_t *)node)->generic.count > 0;
    case AST_UNION_DEF:
        return ((const ast_union_def_t *)node)->generic.count > 0;
    case AST_TYPE_DEF:
        return ((const ast_type_def_t *)node)->generic.count > 0;
    default:
        return false;
    }
}
