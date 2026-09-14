#include "compiler/compiler.h"
#include "core/panic.h"
#include "vm/type_array.h"

#include <string.h>

/* ===========================================================================
 * 类型提升区（hoist）
 *
 * sema 把解析过的类型登记进 sema->types（sema_type_t* 数组），编译器在此
 * 遍历生成运行时构造字节码，把每个类型 BIND_TYPE <id> 绑定到 types_by_id
 * 表。函数体/注册段中的类型槽位（AST_TYPE_REF）随后发 LOAD_TYPE <id>
 * 直接查表——类型构造收敛到提升区，AST 保持平凡可解耦。
 *
 * 构造分派（按 type_t 结构，单遍递归依赖后序）：
 *   - 内建类型（type->id < TYPE_ID_PROGRAM_BASE）：vm_register_builtin_types
 *     已把 type value 绑内建 id 0..16，此处仅 LOAD_TYPE <内建 id> →
 *     BIND_TYPE <sema 分配的 program id>（别名，供槽位统一 LOAD_TYPE
 *     <program id>）。
 *   - 数组 [N]T：PUSH_ARRAY 开放对象 → LOAD_TYPE <elem id>（elem 已先构造）
 *     → DEFINE_BOUND N → SEAL（去重 intern，可能返回已有密封实例）→
 *     BIND_TYPE <id>（绑定密封实例，无悬垂）。
 *   - const/volatile：LOAD_TYPE <sub id>（sub 已先构造）→ CREATE_CONST /
 *     CREATE_VOLATILE（intern 密封）→ BIND_TYPE <id>。
 *
 * 依赖后序：sema 登记时父先入队、依赖递归登记在后，故此处对每个类型递归
 * 构造其依赖（数组 elem / 限定符 sub），保证 LOAD_TYPE <依赖 id> 时依赖已
 * 绑定。类型图当前为 DAG（数组/const/volatile 无环）可递归；若未来引入
 * 指针/自引用类型（成环），须改为拓扑排序或两阶段构造（开放对象先 BIND、
 * 密封后再重绑），见 compile.c 拓扑 TODO。
 *
 * 净栈深 0：每个类型构造序列弹压平衡，提升区整体对操作数栈无影响。
 * =========================================================================== */

/* 登记表查找：AST_TYPE_REF 名字 / type_t 指针 → sema_type_t（线性扫描） */
const sema_type_t *c_sema_type_find_name(vec_t *types, strslice_t name) {
  if (!types || !name.ptr) return NULL;
  size_t n = vec_len(types);
  for (size_t i = 0; i < n; i++) {
    const sema_type_t *st = (const sema_type_t *)vec_get(types, i);
    if (st && st->name.len == name.len &&
        memcmp(st->name.ptr, name.ptr, name.len) == 0)
      return st;
  }
  return NULL;
}

const sema_type_t *c_sema_type_find_ptr(vec_t *types, const type_t *t) {
  if (!types || !t) return NULL;
  size_t n = vec_len(types);
  for (size_t i = 0; i < n; i++) {
    const sema_type_t *st = (const sema_type_t *)vec_get(types, i);
    if (st && st->type == t) return st;
  }
  return NULL;
}

static void hoist_one(compiler_t *c, const sema_type_t *st, uint8_t *done,
                      size_t count);

/* 按类型 id 压入 type value（LOAD_TYPE <id>） */
static void emit_load_type(compiler_t *c, uint32_t id) {
  bcode_write_op(c->bc, BCODE_LOAD_TYPE);
  bcode_write_u32(c->bc, id);
  st_push(c, 1);
}

/* 弹栈顶 type value 登记到 id（BIND_TYPE <id>） */
static void emit_bind_type(compiler_t *c, uint32_t id) {
  bcode_write_op(c->bc, BCODE_BIND_TYPE);
  bcode_write_u32(c->bc, id);
  st_push(c, -1);
}

/* 数组：依赖 elem 先构造 → PUSH_ARRAY → LOAD elem → DEFINE_BOUND → SEAL → BIND */
static void hoist_array(compiler_t *c, const sema_type_t *st, uint8_t *done,
                        size_t count) {
  const type_t *t = st->type;
  const type_t *elem = array_type_elem(t);
  const sema_type_t *est = c_sema_type_find_ptr(c->sema_types, elem);

  bcode_write_op(c->bc, BCODE_PUSH_ARRAY); /* 栈: [open_array_type] */
  st_push(c, 1);

  if (!est) {
    /* 依赖是内建类型（未登记）：LOAD_TYPE <内建 id> 直接查表 */
    emit_load_type(c, elem->id);           /* 栈: [open, elem] */
  } else {
    hoist_one(c, est, done, count);
    emit_load_type(c, est->id);            /* 栈: [open, elem] */
  }

  bcode_write_op(c->bc, BCODE_DEFINE_BOUND); /* 弹 elem → 设进 open */
  bcode_write_u32(c->bc, (uint32_t)array_type_len(t));
  st_push(c, -1);
  bcode_write_op(c->bc, BCODE_SEAL);       /* 开放对象 → 密封实例（可能去重） */
  emit_bind_type(c, st->id);               /* 绑定密封实例到 program id */
}

/* 限定符（const/volatile）：依赖 sub 先构造 → LOAD sub → CREATE_* → BIND */
static void hoist_qual(compiler_t *c, const sema_type_t *st, uint8_t *done,
                       size_t count, bcode_op_t create_op) {
  const type_t *t = st->type;
  const type_t *sub = type_qualifier_sub(t);
  const sema_type_t *sst = c_sema_type_find_ptr(c->sema_types, sub);
  if (!sst) {
    /* 依赖是内建类型（未登记）：LOAD_TYPE <内建 id> 直接查表 */
    emit_load_type(c, sub->id);
  } else {
    hoist_one(c, sst, done, count);
    emit_load_type(c, sst->id);
  }
  bcode_write_op(c->bc, create_op); /* 弹 sub → intern 密封 → 压回 */
  emit_bind_type(c, st->id);
}

/* 内建类型：LOAD_TYPE <内建 id> → BIND_TYPE <program id>（别名） */
static void hoist_builtin(compiler_t *c, const sema_type_t *st) {
  emit_load_type(c, st->type->id);
  emit_bind_type(c, st->id);
}

static void hoist_one(compiler_t *c, const sema_type_t *st, uint8_t *done,
                      size_t count) {
  if (!st) return;
  size_t idx = (size_t)(st->id - TYPE_ID_PROGRAM_BASE);
  if (idx >= count) return;
  if (done[idx]) return; /* 共享依赖只提升一次 */
  const type_t *t = st->type;
  if (!t) return;

  switch (t->kind) {
    case TYPE_KIND_ARRAY:
      hoist_array(c, st, done, count);
      break;
    case TYPE_KIND_CONST:
      hoist_qual(c, st, done, count, BCODE_CREATE_CONST);
      break;
    case TYPE_KIND_VOLATILE:
      hoist_qual(c, st, done, count, BCODE_CREATE_VOLATILE);
      break;
    default:
      hoist_builtin(c, st); /* 内建/其他：仅别名绑定 */
      break;
  }
  done[idx] = true;
}

void compile_hoist(compiler_t *c) {
  if (!c || !c->sema_types) return;
  size_t n = vec_len(c->sema_types);
  if (n == 0) return;

  /* done 数组：sema 分配的 program id 连续（TYPE_ID_PROGRAM_BASE + 下标） */
  uint8_t *done = (uint8_t *)allocator_new_ex(
      c->alloc, "uint8_t", sizeof(uint8_t), NULL, NULL, NULL, n);
  if (!done) panic("compiler: out of memory allocating hoist done set");
  memset(done, 0, n);

  for (size_t i = 0; i < n; i++) {
    const sema_type_t *st = (const sema_type_t *)vec_get(c->sema_types, i);
    hoist_one(c, st, done, n);
    if (c->failed) break;
  }
  allocator_free(c->alloc, (void **)&done);
}
