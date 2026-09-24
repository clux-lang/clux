#include "compiler/compiler.h"
#include "core/panic.h"
#include "vm/type_array.h"
#include "vm/type_enum.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"

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
         DEFINE_TYPE <id> 声明登记（不设字段；字段类型是布局依赖，pass 2
         依赖后序定义 + 环检测） */
      bcode_write_op(c->bc, BCODE_PUSH_STRUCT);
      st_push(c, 1);
      emit_define_type(c, st->id);
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

static void define_one(compiler_t *c, const sema_type_t *st, uint8_t *done,
                       size_t count);

/* 布局依赖类型压栈辅助：内建依赖直接 LOAD_TYPE <内建 id>；程序类型先递归
 * define_one（依赖后序密封）再 LOAD_TYPE <st->id>。返回后栈顶即依赖类型。
 * 布局依赖（array/option/const/volatile/enum/struct）的密封需要内部类型
 * size/align 已确定 → 内部必须已密封，故递归定义 + done 三态环检测。 */
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

/* 引用依赖类型压栈辅助：直接 LOAD_TYPE <id> 拉回（不递归、不检测环、不要求
 * 内部已密封）。引用依赖（func 签名、未来指针 *T）size/align 恒定（如
 * sizeof(func_t*)），密封不依赖内部类型的布局，实例存在即可——实例由 pass 1
 * 声明登记保证。内部未密封时名字走 "?" 兜底（type_func.c func_sig_name）。 */
static void emit_ref_type(compiler_t *c, const type_t *dep) {
  emit_load_type(c, dep->id);
}

/* 数组定义：LOAD_TYPE <id> 拉回开放对象 → 依赖 elem 先定义（密封）→
 * LOAD_TYPE <elem id> → DEFINE_BOUND N → SEAL（按自身 id 幂等更新登记） */
static void define_array(compiler_t *c, const sema_type_t *st, uint8_t *done,
                         size_t count) {
  const type_t *t = st->type;
  const type_t *elem = array_type_elem(t);

  emit_load_type(c, st->id);               /* 栈: [open_array_type] */
  emit_dep_type(c, elem, done, count);     /* 栈: [open, elem] */
  bcode_write_op(c->bc, BCODE_DEFINE_BOUND); /* 弹 elem → 设进 open */
  bcode_write_u32(c->bc, (uint32_t)array_type_len(t));
  st_push(c, -1);
  emit_seal(c);                            /* 封闭算布局（去重时重绑登记） */
}

/* func 签名定义：LOAD_TYPE <id> 拉回开放签名对象 → 参数类型（引用依赖，直接
 * LOAD_TYPE 拉回，不要求已密封）→ FUNC_TYPE_PARAM 追加 → 返回类型 →
 * FUNC_TYPE_RETURN → SEAL。签名归引用依赖：size/align 恒为 func_t*（指针），
 * 密封不依赖参数/返回类型的布局，实例存在即可（pass 1 登记保证）；签名引用
 * 签名（函数指针作参数）即使形成循环也放行，未密封参数名字 "?" 兜底。 */
static void define_func(compiler_t *c, const sema_type_t *st, uint8_t *done,
                        size_t count) {
  const type_t *t = st->type;

  emit_load_type(c, st->id);               /* 栈: [open_func_type] */

  size_t nparams = func_type_param_count(t);
  for (size_t i = 0; i < nparams; i++) {
    const type_t *pt = func_type_param(t, i);
    if (!pt) continue; /* NULL = 无类型约束（防御） */
    emit_ref_type(c, pt);                  /* 栈: [open, param] */
    bcode_write_op(c->bc, BCODE_FUNC_TYPE_PARAM); /* 弹 param → 追加进 open */
    st_push(c, -1);
  }

  const type_t *rt = func_type_return(t);
  if (rt) {
    emit_ref_type(c, rt);                  /* 栈: [open, ret] */
    bcode_write_op(c->bc, BCODE_FUNC_TYPE_RETURN); /* 弹 ret → 设为返回 */
    st_push(c, -1);
  }

  /* M1 用户函数签名非 variadic（sema type_func_sig 固定 false），省略
     FUNC_TYPE_VARARG；未来用户变参函数在此发。 */
  emit_seal(c);                            /* 密封签名（去重时重绑登记） */
  (void)done; (void)count; /* 签名归引用依赖：不递归，done 三态不参与 */
}

/* 限定符（const/volatile）定义：LOAD_TYPE <id> 拉回开放对象 → 依赖 sub 先
 * 定义（密封）→ LOAD sub → SET_TYPE（设 sub）→ SEAL 封闭（按 sub 去重
 * intern，拷贝 size/align，置 sealed；去重时按自身 id 重绑登记） */
static void define_qual(compiler_t *c, const sema_type_t *st, uint8_t *done,
                        size_t count) {
  const type_t *t = st->type;
  const type_t *sub = type_qualifier_sub(t);

  emit_load_type(c, st->id);             /* 栈: [open_qual_type] */
  emit_dep_type(c, sub, done, count);    /* 栈: [open, sub] */
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

/* struct 定义：LOAD_TYPE <id> 拉回开放对象 → 逐字段：依赖字段类型先定义
 * （密封；emit_dep_type 递归 + done 三态环检测）→ LOAD 字段类型 →
 * DEFINE_FIELD <name> → SEAL 封闭（C 对齐布局 + 去重 intern；去重时按自身
 * id 重绑登记）。字段类型是布局依赖（密封计算 offset/size 需要字段 size/
 * align 已确定）→ 依赖后序 + 环检测。 */
static void define_struct(compiler_t *c, const sema_type_t *st, uint8_t *done,
                          size_t count) {
  const type_t *t = st->type;

  emit_load_type(c, st->id);               /* 栈: [open_struct_type] */

  size_t n = struct_type_field_count(t);
  for (size_t i = 0; i < n; i++) {
    const struct_field_t *f = struct_type_field(t, i);
    if (!f || !f->type) continue;
    emit_dep_type(c, f->type, done, count); /* 栈: [open, field_type] */
    bcode_write_op(c->bc, BCODE_DEFINE_FIELD);
    bcode_write_str(c->bc, f->name);        /* 弹 field_type → 追加进 open */
    st_push(c, -1);
  }

  emit_seal(c);                            /* 封闭（去重时重绑登记） */
}

/* optional 定义：LOAD_TYPE <id> 拉回开放对象 → 依赖 inner 先定义（密封）→
 * LOAD inner → SET_TYPE（设 inner）→ SEAL 封闭（按 inner 去重 intern，
 * 计算 C 布局，置 sealed；去重时按自身 id 重绑登记） */
static void define_option(compiler_t *c, const sema_type_t *st, uint8_t *done,
                          size_t count) {
  const type_t *t = st->type;
  const type_t *inner = type_option_inner(t);

  emit_load_type(c, st->id);             /* 栈: [open_option_type] */
  emit_dep_type(c, inner, done, count);  /* 栈: [open, inner] */
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
    emit_dep_type(c, e->type, done, count); /* 栈: [open, elem_type] */
    bcode_write_op(c->bc, BCODE_APPEND_ELEM); /* 弹 elem_type → 追加进 open */
    st_push(c, -1);
  }

  emit_seal(c);                            /* 封闭（去重时重绑登记） */
}

/* union 定义：LOAD_TYPE <id> 拉回开放对象 → 逐 member：UNION_MEMBER <名>
   （追加 member，tag = 追加序）→ 逐 payload 字段：依赖字段类型先定义
   （密封；emit_dep_type 递归 + done 三态环检测）→ LOAD 字段类型 →
   DEFINE_FIELD <name>（追加到当前 member 的开放 payload struct）→ SEAL
   封闭（构建各 payload struct + tag 宽度自适应 + payload 联合体布局 + 去重
   intern；去重时按自身 id 重绑登记）。纯 tag member（无 payload 字段）直接
   UNION_MEMBER 追加，无字段发射。payload 字段类型是布局依赖（密封计算
   payload size/align 需要字段 size/align 已确定）→ 依赖后序 + 环检测。 */
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

    const type_t *ps = m->payload_struct;
    if (!ps) continue;                     /* 纯 tag member：无 payload 字段 */
    const struct_type_t *pst = (const struct_type_t *)ps;
    size_t fcount = struct_type_field_count(ps);
    for (size_t fi = 0; fi < fcount; fi++) {
      const struct_field_t *f = struct_type_field(ps, fi);
      if (!f || !f->type) continue;
      emit_dep_type(c, f->type, done, count); /* 栈: [open, field_type] */
      bcode_write_op(c->bc, BCODE_DEFINE_FIELD);
      bcode_write_str(c->bc, f->name);       /* 弹 field_type → 追加进 open */
      st_push(c, -1);
    }
  }

  emit_seal(c);                            /* 封闭（去重时重绑登记） */
}

/* 定义状态（done 数组三态）：
 *   0 = 未处理
 *   1 = 处理中（正在定义，布局依赖递归尚未完成）
 *   2 = 已完成（已密封）
 * 三态区分"处理中"与"已完成"：布局依赖（array/option/const/volatile/enum/
 * struct）密封要求内部类型已 seal（依赖后序），循环引用（A → B → A）递归回到
 * 处理中节点时内部类型永远无法密封 → 编译期报错（fail-fast），替代无限递归
 * 崩溃。引用依赖（func 签名、未来指针 *T）不递归（emit_ref_type 直接拉取），
 * 不参与环检测，天然放行循环签名。当前源语言类型 DAG（sema 按声明序求值拦截
 * 前向引用）不会触发；未来指针/自引用类型（成环）时此检测是安全网。 */
#define TYPE_DEF_UNTOUCHED 0
#define TYPE_DEF_IN_PROGRESS 1
#define TYPE_DEF_DONE 2

static void define_one(compiler_t *c, const sema_type_t *st, uint8_t *done,
                       size_t count) {
  if (!st) return;
  size_t idx = (size_t)(st->id - TYPE_ID_PROGRAM_BASE);
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
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE:
      define_qual(c, st, done, count);
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
    default:
      done[idx] = TYPE_DEF_DONE; /* 内建别名：pass 1 已完成，无定义 */
      return;
  }
  done[idx] = TYPE_DEF_DONE;
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

  /* done 数组：sema 分配的 program id 连续（TYPE_ID_PROGRAM_BASE + 下标） */
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
