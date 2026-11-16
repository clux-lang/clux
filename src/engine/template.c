#include "engine/template.h"

/* 模板值 data 只存原始 AST 指针（定长平点位图）：参数表不驻留，
   每次实例化时由上层从 AST 重新计算。vtable 完全留空。 */
static clux_vtable_t g_type_template_vtable = {0};
static clux_vtable_t g_func_template_vtable = {0};

clux_vtable_t *get_clux_type_template_vtable(void) {
  return &g_type_template_vtable;
}

clux_vtable_t *get_clux_func_template_vtable(void) {
  return &g_func_template_vtable;
}
