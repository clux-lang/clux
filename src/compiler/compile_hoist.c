#include <stdio.h>
#include "compiler/compiler.h"
#include "core/panic.h"
#include "vm/type_array.h"
#include "vm/type_enum.h"
#include "vm/type_cfunc.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/type_cunion.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"

#include <string.h>

/* ===========================================================================
 * 类型提升区（hoist）——两遍扫描
 *
 * sema 把解析过的类型登记进 sema->types（sema_type_t* 数组），编译器在此
 * 生成运行时构造字节码，把所有程序类型构造进 types_by_id 表。函数体/注册段
 * 中的类型槽位（AST_TYPE_REF）随后发 LOAD_TYPE <id> 直接查表——类型构造
 * 收敛到提升区，AST 保持平凡可解耦。
 *
 * **两遍扫描**（解决类型向前声明，为未来 struct 字段引用后声明类型 /
 * 指针等自引用类型铺路；func 签名类型亦纳入——签名本质是普通类型，且
 * 函数指针作参数时签名引用签名，须依赖后序构造）：
 *
 *   pass 1（声明所有类型）：遍历全部程序类型，逐个创建开放 type 对象并
 *     DEFINE_TYPE <id> 登记进 types_by_id（不设字段）。声明完成后所有类型
 *     id 在表中都有登记（开放或密封），后续任何类型的字段构造都可
 *     LOAD_TYPE <id> 拿到对象（向前引用安全）。
 *     - 数组 [N]T：PUSH_ARRAY（开放对象，未成形）→ DEFINE_TYPE <id>
 *     - func 签名：PUSH_FUNC_TYPE（开放签名对象）→ DEFINE_TYPE <id>
 *     - const/volatile：PUSH_CONST / PUSH_VOLATILE（开放对象，sub=NULL）→
 *       DEFINE_TYPE <id>
 *     - struct：PUSH_STRUCT（开放对象，fields=NULL）→ DEFINE_TYPE <id>
 *     - tuple：PUSH_TUPLE（开放对象，elems=NULL）→ DEFINE_TYPE <id>
 *     - union：PUSH_UNION（开放对象，members=NULL）→ DEFINE_TYPE <id>
 *     - cunion：PUSH_CUNION（开放对象，members=NULL）→ DEFINE_TYPE <id>
 *     - 指针（own/ref/fatal *T）：PUSH_PTR <kind>（开放对象，base=NULL）→
 *       DEFINE_TYPE <id>（引用依赖，不设 base）
 *     - 内建别名（防御分支）：LOAD_TYPE <内建 id> → DEFINE_TYPE <id>
 *
 *   pass 2（定义所有类型）：遍历全部程序类型，逐个 LOAD_TYPE <id> 拉回
 *     开放对象 → 设置字段 → SEAL 封闭计算内存布局。**依赖后序**（递归
 *     define_one + done 数组去重共享依赖）：数组 SEAL 算布局需 elem 已
 *     密封（elem->size/align），const/volatile 拷贝 size/align 需 sub 已
 *     密封，func 签名设参数/返回需依赖已构造（签名可引用签名），故先定义
 *     依赖再定义自身。
 *     - 数组：LOAD_TYPE <id> 拉回开放对象 →（递归定义 elem）→
 *       LOAD_TYPE <elem id> → DEFINE_BOUND N → SEAL（去重 intern 可能
 *       返回已有密封实例；SEAL 按开放对象自身 id 重绑登记表，无悬垂）
 *     - func 签名：LOAD_TYPE <id> 拉回开放签名对象 → 参数类型（递归
 *       定义）→ FUNC_TYPE_PARAM → 返回类型 → FUNC_TYPE_RETURN → SEAL
 *     - const/volatile：LOAD_TYPE <id> 拉回开放对象 →（递归定义 sub）→
 *       LOAD_TYPE <sub id> → SET_TYPE（设 sub）→ SEAL（intern 创建即密封，
 *       拷贝 size/align；去重时重绑登记）
 *     - 指针：LOAD_TYPE <id> 拉回开放对象 → 依赖 base 先定义（emit_dep_type；
 *       指针 size/align 恒为指针宽，base 只作身份）→ SET_TYPE（设 base）→ SEAL
 *     - 内建别名：pass 1 已完成，跳过
 *
 * 依赖后序：sema 登记时父先入队、依赖递归登记在后（sema_type_register →
 * sema_type_register_deps），此处 pass 2 对每个类型递归定义其依赖（数组
 * elem / 限定符 sub），保证 LOAD_TYPE <依赖 id> 时依赖已密封。类型图当前
 * 为 DAG（数组/const/volatile 无环）可递归；若未来引入指针/自引用类型
 * （成环），两遍模型天然支持：pass 1 开放对象已登记，字段构造 LOAD_TYPE
 * 拿到开放对象（未密封），SEAL 后再重绑——见 compile.c 拓扑 TODO。
 *
 * 净栈深 0：每个类型构造序列弹压平衡，提升区整体对操作数栈无影响。
 * 两遍之间由 compile.c 插入顶层 typedef 名字绑定（compile_stmt
 * AST_TYPE_DEF 分支），见 compile.c 产物布局注释。
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

/* 按类型 id 压入 type value（LOAD_TYPE <id>） */
static void emit_load_type(compiler_t *c, uint32_t id) {
  bcode_write_op(c->bc, BCODE_LOAD_TYPE);
  bcode_write_u32(c->bc, id);
  st_push(c, 1);
}

/* 弹栈顶 type value → 声明登记到 id（DEFINE_TYPE <id>，消费栈） */
static void emit_define_type(compiler_t *c, uint32_t id) {
  bcode_write_op(c->bc, BCODE_DEFINE_TYPE);
  bcode_write_u32(c->bc, id);
  st_push(c, -1);
}

/* 弹栈顶 type value → 密封（SEAL，无操作数，消费栈；封闭算布局） */
static void emit_seal(compiler_t *c) {
  bcode_write_op(c->bc, BCODE_SEAL);
  st_push(c, -1);
}

/* ===========================================================================
 * pass 1：声明所有类型
 *
 * 顺序无关（开放对象创建不依赖其他类型）；遍历全部程序类型逐一登记。
 * 内建别名（防御分支）LOAD_TYPE <内建 id> → DEFINE_TYPE <id> 一步登记。
 * =========================================================================== */

/* 内建类型：LOAD_TYPE <内建 id> → DEFINE_TYPE <program id>（别名登记） */
static void hoist_builtin(compiler_t *c, const sema_type_t *st) {
  emit_load_type(c, st->type->id);
  emit_define_type(c, st->id);
}

static void declare_one(compiler_t *c, const sema_type_t *st) {
  if (!st || !st->type) return;
  switch (st->type->kind) {
    case TYPE_KIND_ARRAY:
      /* PUSH_ARRAY 压开放对象 → DEFINE_TYPE <id> 声明登记（不设字段） */
      bcode_write_op(c->bc, BCODE_PUSH_ARRAY);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    case TYPE_KIND_FUNC:
      /* PUSH_FUNC_TYPE 压开放签名对象 → DEFINE_TYPE <id> 声明登记（不设
         参数/返回字段；签名引用签名时开放对象已可 LOAD_TYPE 拉回） */
      bcode_write_op(c->bc, BCODE_PUSH_FUNC_TYPE);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    case TYPE_KIND_CFUNC:
      /* cfunc（C 函数指针签名）：同一套两步模型，open 类型是 CFUNC kind。 */
      bcode_write_op(c->bc, BCODE_PUSH_CFUNC_TYPE);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    case TYPE_KIND_CONST:
      /* PUSH_CONST 压开放对象（sub=NULL，不入池）→ DEFINE_TYPE <id> 声明
         登记（不设 sub；向前引用安全——sub 可后声明） */
      bcode_write_op(c->bc, BCODE_PUSH_CONST);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    case TYPE_KIND_VOLATILE:
      /* PUSH_VOLATILE 同上（volatile 修饰） */
      bcode_write_op(c->bc, BCODE_PUSH_VOLATILE);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    case TYPE_KIND_OWN:
    case TYPE_KIND_REF:
    case TYPE_KIND_FATAL:
      /* 所有权修饰：开放构造（PUSH_OWN/REF/FATAL）→ DEFINE_TYPE 登记。
         运行期 SEAL 会**剥成裸 sub**（所有权零运行期行为），所以这个 id 最终
         指向 sub 的实例。 */
      bcode_write_op(c->bc,
                     st->type->kind == TYPE_KIND_OWN     ? BCODE_PUSH_OWN
                     : st->type->kind == TYPE_KIND_REF   ? BCODE_PUSH_REF
                                                         : BCODE_PUSH_FATAL);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    case TYPE_KIND_OPTION:
      /* PUSH_OPT 压开放对象（inner=NULL，不入池）→ DEFINE_TYPE <id> 声明
         登记（不设 inner；向前引用安全——inner 可后声明） */
      bcode_write_op(c->bc, BCODE_PUSH_OPT);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    case TYPE_KIND_ENUM:
      /* PUSH_ENUM 压开放 enum 类型（underlying=NULL，不入池）→
         DEFINE_TYPE <id> 声明登记（不设底层；向前引用安全——底层是内建
         整型，无自引用） */
      bcode_write_op(c->bc, BCODE_PUSH_ENUM);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    case TYPE_KIND_STRUCT:
      /* PUSH_STRUCT 压开放 struct 类型（fields=NULL，不入池）→
         DEFINE_TYPE <id> 声明登记（设 id + 注册到 types_by_id）→
         LOAD_TYPE <id> 拉回 + SET_TYPE_NAME 设显示名（peek 不弹栈）→
         POP 消费拉回的 type value。
         name 来自 sema 注册时 type_set_name 设的 type_t->name
        （struct_compose.c 物化阶段）。SET_TYPE_NAME 要求 id >=
         TYPE_ID_PROGRAM_BASE，须在 DEFINE_TYPE 设 id 之后执行。 */
      bcode_write_op(c->bc, BCODE_PUSH_STRUCT);
      st_push(c, 1);
      emit_define_type(c, st->id);
      if (st->type && st->type->name.ptr) {
        bcode_write_op(c->bc, BCODE_LOAD_TYPE);
        bcode_write_u32(c->bc, st->id);
        st_push(c, 1);
        bcode_write_op(c->bc, BCODE_SET_TYPE_NAME);
        bcode_write_str(c->bc, st->type->name);
        bcode_write_op(c->bc, BCODE_POP);
        st_push(c, -1);
      }
      break;
    case TYPE_KIND_TUPLE:
      /* PUSH_TUPLE 压开放 tuple 类型（elems=NULL，不入池）→
         DEFINE_TYPE <id> 声明登记（不设元素；元素类型是布局依赖，pass 2
         依赖后序定义 + 环检测） */
      bcode_write_op(c->bc, BCODE_PUSH_TUPLE);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    case TYPE_KIND_UNION:
      /* PUSH_UNION 压开放 union 类型（members=NULL，不入池）→
         DEFINE_TYPE <id> 声明登记（不设 member；payload 字段类型是布局
         依赖，pass 2 依赖后序定义 + 环检测） */
      bcode_write_op(c->bc, BCODE_PUSH_UNION);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    case TYPE_KIND_CUNION:
      /* PUSH_CUNION 压开放 cunion 类型（members=NULL，不入池）→
         DEFINE_TYPE <id> 声明登记（不设 member；member 类型是布局依赖，
         pass 2 依赖后序定义 + 环检测） */
      bcode_write_op(c->bc, BCODE_PUSH_CUNION);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    case TYPE_KIND_PTR:
      /* PUSH_PTR 压开放指针类型（base_type=NULL，不入池）→
         DEFINE_TYPE <id> 声明登记（不设 base；base 是引用依赖，pass 2
         LOAD_TYPE 拉回即可，无环——指针密封不依赖 base 布局）。
         share/weak 是**真实类型**（RC 形态），故 PUSH_PTR 带 u8 操作数：
         0 = 普通指针（own/ref/fatal 纯编译期状态）、1 = share、2 = weak。 */
      bcode_write_op(c->bc, BCODE_PUSH_PTR);
      bcode_write_u8(c->bc, ((const ptr_type_t *)st->type)->rc);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    case TYPE_KIND_SLICE:
      /* PUSH_SLICE 压开放切片类型（elem=NULL，不入池）→
         DEFINE_TYPE <id> 声明登记（不设 elem；elem 是引用依赖，pass 2
         LOAD_TYPE 拉回即可，无环——切片密封不依赖 elem 布局）。
         所有权纯编译期——类型不区分 ownership，PUSH_SLICE 无 ownership 操作数。 */
      bcode_write_op(c->bc, BCODE_PUSH_SLICE);
      st_push(c, 1);
      emit_define_type(c, st->id);
      break;
    default:
      hoist_builtin(c, st); /* 内建别名（防御分支） */
      break;
  }
}

/* ===========================================================================
 * pass 2：定义所有类型（依赖后序）
 *
 * 对每个类型递归定义其依赖（数组 elem / 限定符 sub）后再定义自身；
 * done 数组去重共享依赖。数组 SEAL 算布局需 elem 已密封，const/volatile
 * intern 需 sub 已密封——依赖先定义保证 LOAD_TYPE <依赖 id> 时已密封。
 * =========================================================================== */

static bool type_id_seen(vm_t *vm, uint32_t id) {
  if (!vm || !vm->defined_type_ids_global) return false;
  size_t n = vec_len(vm->defined_type_ids_global);
  for (size_t i = 0; i < n; i++)
    if ((uintptr_t)vec_get(vm->defined_type_ids_global, i) == (uintptr_t)id)
      return true;
  return false;
}

static void type_id_mark(vm_t *vm, uint32_t id) {
  if (!vm || !vm->defined_type_ids_global || id < TYPE_ID_PROGRAM_BASE) return;
  if (!type_id_seen(vm, id))
    vec_push(vm->defined_type_ids_global, vm->alloc, (void *)(uintptr_t)id);
}
static void define_one(compiler_t *c, const sema_type_t *st, uint8_t *done,
                       size_t count);
static void emit_ref_dep_type(compiler_t *c, const type_t *dep, uint8_t *done,
                              size_t count);
/* 槽位所有权写入（定义在 define_struct 之前；array/slice/option 也要用） */

/* 引用依赖类型压栈辅助：先把依赖**定义出来**（密封）再 LOAD_TYPE <id> 拉回。
 *
 * 早期版本对"引用依赖"（func 签名/指针/切片）直接 LOAD_TYPE 而不定义依赖，
 * 理由是"这些类型 size/align 恒定，密封不依赖内部布局"。但**身份**仍会被
 * 影响：pass 1 为每个 id 建的是开放对象，若某类型在依赖尚未定义时就把它捕获
 * 进自己的字段/基底，依赖随后 seal 时可能**去重命中既有等价类型而被回收**
 * （例：pass 1 的开放 `const i32` 在 seal 时命中 sema 早已 intern 好的
 * `const i32`，开放对象被 allocator_free）——捕获者就指向了已释放对象，
 * 运行期表现为 "ptr: unsupported implicit cast for pointer"。
 *
 * 因此这里一律走 emit_dep_type：依赖先定义（done 三态处理成环——环上再次
 * 进入的类型直接跳过并 LOAD 其（可能是开放的）对象；成环时被捕获的开放对象
 * 都是**本程序新建、不会与 sema 既有类型去重**的类型（结构体 unique /
 * 结构体上派生的包装），身份稳定，不会悬垂）。
 * 结构体自身的布局依赖（字段类型）本来就走 emit_dep_type；本函数用于
 * array elem / 限定符 sub / struct 字段 / tuple 元素 / union member /
 * 指针 base / 切片 elem / func 参数与返回。 */
static void emit_dep_type(compiler_t *c, const type_t *dep, uint8_t *done,
                          size_t count) {
  const sema_type_t *dst = c_sema_type_find_ptr(c->sema_types, dep);
  if (!dst) {
    /* 依赖是内建类型（未登记）：LOAD_TYPE <内建 id> 直接查表（已密封） */
    emit_load_type(c, dep->id);
  } else {
    define_one(c, dst, done, count);   /* 依赖后序：先定义依赖（密封） */
    emit_load_type(c, dst->id);
  }
}

static void emit_ref_dep_type(compiler_t *c, const type_t *dep, uint8_t *done,
                              size_t count);
static void emit_slot_type(compiler_t *c, const type_t *t, uint8_t *done,
                           size_t count);

/* 数组定义：LOAD_TYPE <id> 拉回开放对象 → 依赖 elem 先定义（密封）→
 * LOAD_TYPE <elem id> → DEFINE_BOUND N → SEAL（按自身 id 幂等更新登记） */
static void define_array(compiler_t *c, const sema_type_t *st, uint8_t *done,
                         size_t count) {
  const type_t *t = st->type;
  const type_t *elem = array_type_elem(t);

  emit_load_type(c, st->id);               /* 栈: [open_array_type] */
  emit_slot_type(c, elem, done, count);  /* 栈: [open, elem] */
  bcode_write_op(c->bc, BCODE_DEFINE_BOUND); /* 弹 elem → 设进 open */
  bcode_write_u32(c->bc, (uint32_t)array_type_len(t));
  st_push(c, -1);
  emit_seal(c);                            /* 封闭算布局（去重时重绑登记） */
}

/* 定义状态（done 数组三态）：
 *   0 = 未处理    1 = 处理中（正在定义，依赖递归尚未完成）    2 = 已完成（已密封）
 * 三态区分"处理中"与"已完成"：需要内部先密封的依赖（option/tuple/union/struct
 * 字段）回到处理中节点时报 "circular type definition"（fail-fast，替代无限递归）；
 * 而**可延迟定型**的依赖（指针 base、切片 elem、func 参数/返回、const/volatile
 * 的 sub、数组 elem——见 emit_ref_dep_type）回到处理中节点时直接 LOAD 其开放
 * 对象（自引用/互相引用类型正是靠这条路径成立）。 */
#define TYPE_DEF_UNTOUCHED 0
#define TYPE_DEF_IN_PROGRESS 1
#define TYPE_DEF_DONE 2

/* 引用/延迟定型依赖压栈辅助：依赖未处理 → 先定义（身份+布局定型）；依赖**正在
 * 定义中**（环）→ 不递归、不报错，直接 LOAD_TYPE 它的（仍开放的）对象。
 *
 * 用于布局**可以延迟定型**的关系：指针 base、切片 elem、func 参数/返回、
 * const/volatile 的 sub、数组的 elem（它们在 sub seal 后由 VM 侧的
 * type_sync_derived_layout 同步布局，见 vm/type.h）。
 *
 * 环上被捕获的开放对象必须"身份稳定"：它是本程序 pass 1 新建、且不会与 sema
 * 既有等价类型去重回收的类型（具名 struct 走 unique 不去重；结构体上派生的
 * 包装其 base/elem 是本程序的结构体对象，池里没有等价项）——因此捕获后
 * 其后的 seal 不会把对象换掉，指针/包装指向它始终有效。
 *
 * 真正需要"内部先密封"的关系（option/tuple/union 按值内联元素布局、struct 字段
 * 布局）仍走 emit_dep_type：环上直接报 "circular type definition"（sema 已拦截
 * 源语言里的按值递归，这里是安全网）。 */
static void emit_ref_dep_type(compiler_t *c, const type_t *dep, uint8_t *done,
                              size_t count) {
  const sema_type_t *dst = c_sema_type_find_ptr(c->sema_types, dep);
  if (!dst) {
    emit_load_type(c, dep->id); /* 内建依赖：已密封 */
    return;
  }
  size_t idx = (size_t)dst->index;
  if (idx < count && done[idx] == TYPE_DEF_IN_PROGRESS) {
    emit_load_type(c, dst->id); /* 环：用其开放对象（身份稳定，布局随后同步） */
    return;
  }
  define_one(c, dst, done, count);
  emit_load_type(c, dst->id);
}

/* 槽位类型：所有权修饰要在**运行期类型图**里保留（`own *T` 的字段/元素/内层
   类型就是 `own *T`），销毁/克隆据此判定。做法：先把裸类型压栈，再用
   LOAD_TYPE_OWNED 套上修饰。无所有权修饰时退化为普通依赖加载。 */
static void emit_slot_type(compiler_t *c, const type_t *t, uint8_t *done,
                           size_t count) {
  ownership_t own = type_ownership_of(t);
  if (own == OWN_NONE) {
    emit_ref_dep_type(c, t, done, count);
    return;
  }
  emit_ref_dep_type(c, type_strip_quals(t), done, count);
  bcode_write_op(c->bc, BCODE_LOAD_TYPE_OWNED);
  bcode_write_u8(c->bc, (uint8_t)own);
}

/* func 签名定义：LOAD_TYPE <id> 拉回开放签名对象 → 参数类型（emit_ref_dep_type：
 * 先把依赖定义出来，见其说明）→ FUNC_TYPE_PARAM 追加 → 返回类型 →
 * FUNC_TYPE_RETURN → SEAL。签名自身 size/align 恒为 func_t*（指针），密封不
 * 依赖参数/返回类型的布局；但依赖的**身份**必须先定型（否则去重回收留下悬垂）。
 * 签名引用签名（函数指针作参数）形成环时由 define_one 的 done 三态跳过重复
 * 定义、LOAD 其（可能仍开放的）对象——环上被捕获的开放对象都是本程序新建、
 * 不会与 sema 既有类型去重的类型，身份稳定。 */
static void define_func(compiler_t *c, const sema_type_t *st, uint8_t *done,
                        size_t count) {
  const type_t *t = st->type; /* FUNC 或 CFUNC：两条协议完全同构 */

  emit_load_type(c, st->id);               /* 栈: [open_func_type] */

  size_t nparams = func_type_param_count(t);
  for (size_t i = 0; i < nparams; i++) {
    const type_t *pt = func_type_param(t, i);
    if (!pt) continue; /* NULL = 无类型约束（防御） */
    emit_ref_dep_type(c, pt, done, count); /* 栈: [open, param] */
    bcode_write_op(c->bc, BCODE_FUNC_TYPE_PARAM); /* 弹 param → 追加进 open */
    st_push(c, -1);
  }

  const type_t *rt = func_type_return(t);
  if (rt) {
    emit_ref_dep_type(c, rt, done, count); /* 栈: [open, ret] */
    bcode_write_op(c->bc, BCODE_FUNC_TYPE_RETURN); /* 弹 ret → 设为返回 */
    st_push(c, -1);
  }

  /* M1 用户函数签名非 variadic（sema type_func_sig 固定 false），省略
     FUNC_TYPE_VARARG；未来用户变参函数在此发。 */
  emit_seal(c);                            /* 密封签名（去重时重绑登记） */
  /* 参数/返回也先定义依赖（见 emit_dep_type 的说明）：签名自身 layout 恒定，
     但内部类型的**身份**必须先定型，否则 seal 去重回收会留下悬垂基底。 */
}

/* cfunc 定义：与 define_func 同构（LOAD_TYPE → 逐个参数 → 返回 → SEAL），
 * 签名从 cfunc_type_sig 取（func_type_* 访问器只认 TYPE_KIND_FUNC）。 */
static void define_cfunc(compiler_t *c, const sema_type_t *st, uint8_t *done,
                         size_t count) {
  const type_t *t = st->type;
  const func_sig_t *sig = cfunc_type_sig(t);
  if (!sig) return;

  emit_load_type(c, st->id); /* 栈: [open_cfunc_type] */
  for (size_t i = 0; i < sig->param_count; i++) {
    const type_t *pt = sig->params ? sig->params[i] : NULL;
    if (!pt) continue;
    emit_ref_dep_type(c, pt, done, count); /* 栈: [open, param] */
    bcode_write_op(c->bc, BCODE_FUNC_TYPE_PARAM);
    st_push(c, -1);
  }
  if (sig->return_type) {
    emit_ref_dep_type(c, sig->return_type, done, count);
    bcode_write_op(c->bc, BCODE_FUNC_TYPE_RETURN);
    st_push(c, -1);
  }
  if (sig->is_variadic) {
    bcode_write_op(c->bc, BCODE_FUNC_TYPE_VARARG);
  }
  emit_seal(c);
}

/* 限定符（const/volatile）定义：LOAD_TYPE <id> 拉回开放对象 → 依赖 sub 先
 * 定义（密封）→ LOAD sub → SET_TYPE（设 sub）→ SEAL 封闭（按 sub 去重
 * intern，拷贝 size/align，置 sealed；去重时按自身 id 重绑登记） */
static void define_qual(compiler_t *c, const sema_type_t *st, uint8_t *done,
                        size_t count) {
  const type_t *t = st->type;
  const type_t *sub = type_qualifier_sub(t);

  emit_load_type(c, st->id);             /* 栈: [open_qual_type] */
  emit_ref_dep_type(c, sub, done, count);  /* 栈: [open, sub] */
  bcode_write_op(c->bc, BCODE_SET_TYPE); /* 弹 sub → 设进 open */
  st_push(c, -1);
  emit_seal(c);                          /* 封闭（去重时重绑登记） */
}

/* enum 定义：LOAD_TYPE <id> 拉回开放对象 → 依赖 underlying（内建整型，
 * 直接 LOAD_TYPE <内建 id>）→ SET_TYPE（设底层）→ ENUM_VARIANT×N →
 * SEAL 封闭（拷贝 variant 表 + 布局 = 底层布局 + 去重 intern；去重时按
 * 自身 id 重绑登记）。底层是内建类型（不登记 sema->types），无依赖递归。 */
static void define_enum(compiler_t *c, const sema_type_t *st, uint8_t *done,
                        size_t count) {
  const type_t *t = st->type;
  const type_t *u = enum_type_underlying(t);

  emit_load_type(c, st->id);             /* 栈: [open_enum_type] */
  emit_load_type(c, u->id);              /* 栈: [open, underlying] */
  bcode_write_op(c->bc, BCODE_SET_TYPE); /* 弹 underlying → 设进 open */
  st_push(c, -1);

  size_t n = enum_type_variant_count(t);
  for (size_t i = 0; i < n; i++) {
    const enum_variant_t *v = enum_type_variant(t, i);
    if (!v) continue;
    bcode_write_op(c->bc, BCODE_ENUM_VARIANT);
    bcode_write_str(c->bc, v->name);
    bcode_write_i64(c->bc, v->value);
    /* ENUM_VARIANT 无弹栈副作用（peek 开放对象追加） */
  }

  emit_seal(c);                          /* 封闭（去重时重绑登记） */
  (void)done; (void)count;
}

/* 指针定义（own/ref/fatal *T）：LOAD_TYPE <id> 拉回开放对象 → base 先定义
 * （emit_dep_type：指针自身 layout 恒定 = 指针宽，但 base 的**身份**必须先
 * 定型，否则 seal 去重回收会留下悬垂基底）→ SET_TYPE（设 base）→
 * SEAL（去重时按自身
 * id 重绑登记）。指针可引用指针（own *own *T 递归），实例存在即可（pass 1
 * 登记保证），天然放行指针自引用。 */
static void define_ptr(compiler_t *c, const sema_type_t *st, uint8_t *done,
                       size_t count) {
  const type_t *t = st->type;
  const type_t *base = ptr_type_base(t);

  emit_load_type(c, st->id);             /* 栈: [open_ptr_type] */
  emit_ref_dep_type(c, base, done, count); /* 栈: [open, base] */
  bcode_write_op(c->bc, BCODE_SET_TYPE); /* 弹 base → 设进 open */
  st_push(c, -1);
  emit_seal(c);                          /* 封闭（去重时重绑登记） */
  /* base 先定义（emit_dep_type）：指针自身 layout 恒定，但 base 的身份必须已定型 */
}

/* 切片定义（own/ref/fatal []T）：LOAD_TYPE <id> 拉回开放对象 → elem 先定义
 * （emit_dep_type：切片自身 layout 恒定 = 2 指针宽，但 elem 的身份必须先定型）→
 * SET_TYPE（设 elem）→
 * SEAL（去重时按自身
 * id 重绑登记）。与指针同属引用依赖：切片可引用切片，实例存在即可（pass 1
 * 登记保证），天然放行切片自引用。 */
static void define_slice(compiler_t *c, const sema_type_t *st, uint8_t *done,
                         size_t count) {
  const type_t *t = st->type;
  const type_t *elem = slice_type_elem(t);

  emit_load_type(c, st->id);             /* 栈: [open_slice_type] */
  emit_slot_type(c, elem, done, count);  /* 栈: [open, elem] */
  bcode_write_op(c->bc, BCODE_SET_TYPE); /* 弹 elem → 设进 open */
  st_push(c, -1);
  emit_seal(c);                          /* 封闭（去重时重绑登记） */
  /* elem 先定义（emit_dep_type）：切片自身 layout 恒定，但 elem 的身份必须已定型 */
}

/* struct 定义：LOAD_TYPE <id> 拉回开放对象 → 逐字段：依赖字段类型先定义
 * （密封；emit_dep_type 递归 + done 三态环检测）→ LOAD 字段类型 →
 * DEFINE_FIELD <name> → ADD_EMBED <embed_id>×N（设置嵌入表，seal 前）→
 * SET_TYPE_NAME（设显示名，seal 后 LOAD_TYPE 拉回再设）→
 * SEAL 封闭（C 对齐布局 + 去重 intern；去重时按自身 id 重绑登记）。
 * 字段类型是布局依赖（密封计算 offset/size 需要字段 size/
 * align 已确定）→ 依赖后序 + 环检测。 */
/* 槽位所有权由槽位类型携带：define_struct 用 emit_slot_type 把字段类型
   （`own *T` 等）带进运行期类型图，不再需要 SET_FIELD_OWNERSHIP 补发。 */
static void define_struct(compiler_t *c, const sema_type_t *st, uint8_t *done,
                          size_t count) {
  const type_t *t = st->type;

  emit_load_type(c, st->id);               /* 栈: [open_struct_type] */

  size_t n = struct_type_field_count(t);
  for (size_t i = 0; i < n; i++) {
    const struct_field_t *f = struct_type_field(t, i);
    if (!f || !f->type) continue;
    emit_slot_type(c, f->type, done, count); /* 栈: [open, field_type] */
    bcode_write_op(c->bc, BCODE_DEFINE_FIELD);
    bcode_write_str(c->bc, f->name);        /* 弹 field_type → 追加进 open */
    st_push(c, -1);
  }

  /* 嵌入表（`...Base`）：sema 组合阶段已设置 embeds，此处逐个 ADD_EMBED
     在 seal 前复制到运行时构造的类型对象上（find_attr_owner 沿 embeds 链
     查找 attributes）。embed 类型必须先定义（递归 define_one 密封）。 */
  size_t ne = 0;
  const type_t *const *emb = type_struct_embeds(t, &ne);
  for (size_t i = 0; i < ne; i++) {
    if (!emb[i]) continue;
    const sema_type_t *es = c_sema_type_find_ptr(c->sema_types, emb[i]);
    if (!es) continue;
    define_one(c, es, done, count);  /* 确保 embed 类型已密封 */
    bcode_write_op(c->bc, BCODE_ADD_EMBED);
    bcode_write_u32(c->bc, es->id);
  }

  /* 具名 struct 标记独立身份（seal 跳过去重 intern）——sema 已在
     struct_compose.c 物化阶段调 type_struct_mark_unique，此处对 compiler
     重建的开放类型复制标记。须在 SEAL 之前。 */
  if (type_struct_is_unique(t)) {
    bcode_write_op(c->bc, BCODE_MARK_UNIQUE);
  }

  emit_seal(c);                            /* 封闭（去重时重绑登记） */

  /* 设置类型显示名（seal 后 LOAD_TYPE 拉回密封类型 → SET_TYPE_NAME 设名）。
     struct 的 name 用于 find_attr_owner 沿嵌入链查找时通过 sema global_scope
     查符号（VM 层 find_attr_owner 直接用 embeds 递归查 attributes，但 sema
     层 find_own_member/find_inherited_member 用 name 查 global_scope）。 */
  if (t->name.ptr && t->name.len > 0) {
    emit_load_type(c, st->id);
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_SET_TYPE_NAME);
    bcode_write_str(c->bc, t->name);
    bcode_write_op(c->bc, BCODE_POP);
    st_push(c, -1);
  }
}

/* optional 定义：LOAD_TYPE <id> 拉回开放对象 → 依赖 inner 先定义（密封）→
 * LOAD inner → SET_TYPE（设 inner）→ SEAL 封闭（按 inner 去重 intern，
 * 计算 C 布局，置 sealed；去重时按自身 id 重绑登记） */
static void define_option(compiler_t *c, const sema_type_t *st, uint8_t *done,
                          size_t count) {
  const type_t *t = st->type;
  const type_t *inner = type_option_inner(t);

  emit_load_type(c, st->id);             /* 栈: [open_option_type] */
  emit_slot_type(c, inner, done, count); /* 栈: [open, inner] */
  bcode_write_op(c->bc, BCODE_SET_TYPE); /* 弹 inner → 设进 open */
  st_push(c, -1);
  emit_seal(c);                          /* 封闭（去重时重绑登记） */
}

/* tuple 定义：LOAD_TYPE <id> 拉回开放对象 → 逐元素：依赖元素类型先定义
 * （密封；emit_dep_type 递归 + done 三态环检测）→ LOAD 元素类型 →
 * APPEND_ELEM（元素匿名，追加次数=元素数）→ SEAL 封闭（C 对齐布局 + 去重
 * intern；去重时按自身 id 重绑登记）。元素类型是布局依赖（密封计算 offset/
 * size 需要元素 size/align 已确定）→ 依赖后序 + 环检测。 */
static void define_tuple(compiler_t *c, const sema_type_t *st, uint8_t *done,
                         size_t count) {
  const type_t *t = st->type;

  emit_load_type(c, st->id);               /* 栈: [open_tuple_type] */

  size_t n = tuple_type_elem_count(t);
  for (size_t i = 0; i < n; i++) {
    const tuple_elem_t *e = tuple_type_elem(t, i);
    if (!e || !e->type) continue;
    emit_slot_type(c, e->type, done, count); /* 栈: [open, elem_type] */
    bcode_write_op(c->bc, BCODE_APPEND_ELEM); /* 弹 elem_type → 追加进 open */
    st_push(c, -1);
  }

  emit_seal(c);                            /* 封闭（去重时重绑登记） */
}

/* union 定义：LOAD_TYPE <id> 拉回开放对象 → 逐 member：UNION_MEMBER <名>
   （追加 member，tag = 追加序）→ 依赖 member 类型先定义（密封；
   emit_dep_type 递归 + done 三态环检测）→ LOAD member 类型 →
   DEFINE_FIELD <name>（设当前 member 的 payload 类型）→ SEAL 封闭
   （tag 宽度自适应 + payload 联合体布局 + 去重 intern；去重时按自身 id
   重绑登记）。member 类型是布局依赖（密封计算 payload size/align 需要
   member size/align 已确定）→ 依赖后序 + 环检测。 */
static void define_union(compiler_t *c, const sema_type_t *st, uint8_t *done,
                         size_t count) {
  const type_t *t = st->type;

  emit_load_type(c, st->id);               /* 栈: [open_union_type] */

  size_t n = union_type_member_count(t);
  for (size_t i = 0; i < n; i++) {
    const union_member_t *m = union_type_member(t, i);
    if (!m) continue;
    bcode_write_op(c->bc, BCODE_UNION_MEMBER);
    bcode_write_str(c->bc, m->name);       /* peek 追加 member（不弹栈） */

    const type_t *pt = m->payload_type;
    if (!pt) continue;                     /* 防御：member 类型必存在 */
    emit_slot_type(c, pt, done, count);    /* 栈: [open, member_type] */
    bcode_write_op(c->bc, BCODE_DEFINE_FIELD);
    bcode_write_str(c->bc, m->name);       /* 弹 member_type → 设当前 member */
    st_push(c, -1);
  }

  emit_seal(c);                            /* 封闭（去重时重绑登记） */
}

/* cunion 定义：LOAD_TYPE <id> 拉回开放对象 → 逐 member：依赖 member 类型
 * 先定义（密封；emit_dep_type 递归 + done 三态环检测）→ LOAD member 类型 →
 * DEFINE_FIELD <name>（追加 member 名 + 设 member 类型；与 struct 同款
 * DEFINE_FIELD 协议，无需 UNION_MEMBER——cunion 无 tag）→ SEAL 封闭
 * （size=max(member size)/align=max(member align)/offset 全 0 + 去重 intern；
 * 去重时按自身 id 重绑登记）。member 类型是布局依赖（密封计算 size/align
 * 需要 member size/align 已确定）→ 依赖后序 + 环检测。 */
static void define_cunion(compiler_t *c, const sema_type_t *st, uint8_t *done,
                          size_t count) {
  const type_t *t = st->type;

  emit_load_type(c, st->id);               /* 栈: [open_cunion_type] */

  size_t n = cunion_type_member_count(t);
  for (size_t i = 0; i < n; i++) {
    const cunion_member_t *m = cunion_type_member(t, i);
    if (!m) continue;

    const type_t *mt = m->type;
    if (!mt) continue;                     /* 防御：member 类型必存在 */
    emit_dep_type(c, mt, done, count);     /* 栈: [open, member_type] */
    bcode_write_op(c->bc, BCODE_DEFINE_FIELD);
    bcode_write_str(c->bc, m->name);       /* 弹 member_type → 追加 member + 设类型 */
    st_push(c, -1);
  }

  emit_seal(c);                            /* 封闭（去重时重绑登记） */
}

static void define_one(compiler_t *c, const sema_type_t *st, uint8_t *done,
                       size_t count) {
  if (!st) return;
  if (type_id_seen(c->vm, st->id)) return;
  /* done 下标用**登记下标**（sema_type_t::index），不能用 id - BASE 推算：
     id 已是全 VM 全局分配（跨模块不连续），见 vm/type.h 的 type_id_alloc。 */
  size_t idx = (size_t)st->index;
  if (idx >= count) return;
  if (done[idx] == TYPE_DEF_DONE) return; /* 共享依赖只定义一次 */
  if (done[idx] == TYPE_DEF_IN_PROGRESS) {
    /* 依赖链回到处理中节点 = 循环引用：内部类型无法先于自身密封。
       sema 已拦截源语言循环（防御分支，正常情况下不触发）。 */
    if (st->type && st->type->name.ptr) {
      c_error(c, NULL, "circular type definition involving '%.*s'",
              (int)st->type->name.len, st->type->name.ptr);
    } else {
      c_error(c, NULL, "circular type definition");
    }
    return;
  }
  done[idx] = TYPE_DEF_IN_PROGRESS; /* 进入时置位：递归返回检测环 */
  const type_t *t = st->type;
  if (!t) {
    done[idx] = TYPE_DEF_DONE;
    return;
  }

  switch (t->kind) {
    case TYPE_KIND_ARRAY:
      define_array(c, st, done, count);
      break;
    case TYPE_KIND_FUNC:
      define_func(c, st, done, count);
      break;
    case TYPE_KIND_CFUNC:
      define_cfunc(c, st, done, count);
      break;
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE:
      define_qual(c, st, done, count);
      break;
    case TYPE_KIND_OWN:
    case TYPE_KIND_REF:
    case TYPE_KIND_FATAL:
      define_qual(c, st, done, count); /* SET_TYPE sub + SEAL（SEAL 剥成 sub） */
      break;
    case TYPE_KIND_OPTION:
      define_option(c, st, done, count);
      break;
    case TYPE_KIND_ENUM:
      define_enum(c, st, done, count);
      break;
    case TYPE_KIND_STRUCT:
      define_struct(c, st, done, count);
      break;
    case TYPE_KIND_TUPLE:
      define_tuple(c, st, done, count);
      break;
    case TYPE_KIND_UNION:
      define_union(c, st, done, count);
      break;
    case TYPE_KIND_CUNION:
      define_cunion(c, st, done, count);
      break;
    case TYPE_KIND_PTR:
      define_ptr(c, st, done, count);
      break;
    case TYPE_KIND_SLICE:
      define_slice(c, st, done, count);
      break;
    default:
      done[idx] = TYPE_DEF_DONE; /* 内建别名：pass 1 已完成，无定义 */
      return;
  }
  done[idx] = TYPE_DEF_DONE;
  type_id_mark(c->vm, st->id);
}

/* pass 1：声明所有类型（开放对象登记进 types_by_id，顺序无关）。
 * 与 pass 2 拆分为独立入口，compile.c 在两遍之间插入顶层 typedef
 * 名字绑定（类型定义自动提升）。 */
void compile_hoist_declare(compiler_t *c) {
  if (!c || !c->sema_types) return;
  size_t n = vec_len(c->sema_types);
  if (n == 0) return;

  for (size_t i = 0; i < n; i++) {
    const sema_type_t *st = (const sema_type_t *)vec_get(c->sema_types, i);
    if (st && st->id >= TYPE_ID_PROGRAM_BASE && type_id_seen(c->vm, st->id))
      continue;
    declare_one(c, st);
    if (c->failed) break;
  }
}

/* pass 2：定义所有类型（依赖后序递归 + done 三态去重/环检测）。
 * 数组 SEAL 算布局需 elem 已密封，const/volatile 拷贝 size/align 需 sub
 * 已密封，func 签名设参数/返回需依赖已构造——先定义依赖再定义自身。
 * done 三态：0 未处理 / 1 处理中 / 2 已完成；递归回到处理中节点 =
 * 循环引用 → 编译期报错（见 define_one）。 */
void compile_hoist_define(compiler_t *c) {
  if (!c || !c->sema_types) return;
  size_t n = vec_len(c->sema_types);
  if (n == 0) return;

  /* done 数组：按 sema->types 的**登记下标**索引（sema_type_t::index）；
     id 是全 VM 全局分配的，模块内不再连续（见 vm/type.h 的 type_id_alloc）。 */
  uint8_t *done = (uint8_t *)allocator_new_ex(
      c->alloc, "uint8_t", sizeof(uint8_t), NULL, NULL, NULL, n);
  if (!done) panic("compiler: out of memory allocating hoist done set");
  memset(done, 0, n);

  for (size_t i = 0; i < n; i++) {
    const sema_type_t *st = (const sema_type_t *)vec_get(c->sema_types, i);
    define_one(c, st, done, n);
    if (c->failed) break;
  }
  allocator_free(c->alloc, (void **)&done);
}
