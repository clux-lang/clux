#include "core/rbtree.h"
#include "core/allocator.h"
#include <stdbool.h>

typedef struct _rbnode_t {
  uintptr_t key;
  void *value;
  struct _rbnode_t *left;
  struct _rbnode_t *right;
  struct _rbnode_t *parent;
  bool red;
} rbnode_t;

struct _rbtree_t {
  rbnode_t *root;
  rbnode_t *nil; /* 哨兵节点 */
  size_t size;
  allocator_t *allocator;
};

static rbnode_t *node_alloc(rbtree_t *tree, uintptr_t key, void *value) {
  rbnode_t *node = allocator_alloc(tree->allocator, sizeof(struct _rbnode_t));
  node->key = key;
  node->value = value;
  node->left = tree->nil;
  node->right = tree->nil;
  node->parent = tree->nil;
  node->red = true;
  return node;
}

rbtree_t *create_rbtree(allocator_t *allocator) {
  rbtree_t *tree = allocator_alloc(allocator, sizeof(struct _rbtree_t));
  tree->allocator = allocator;
  tree->size = 0;
  /* 哨兵节点：黑色，所有叶子指向它 */
  rbnode_t *nil = allocator_alloc(allocator, sizeof(struct _rbnode_t));
  nil->key = 0;
  nil->value = NULL;
  nil->left = nil;
  nil->right = nil;
  nil->parent = nil;
  nil->red = false;
  tree->nil = nil;
  tree->root = nil;
  return tree;
}

/* 只释放节点自身；value 归调用方所有。 */
static void node_free(rbtree_t *tree, rbnode_t *node) {
  if (node == tree->nil) return;
  node_free(tree, node->left);
  node_free(tree, node->right);
  allocator_free(tree->allocator, (void **)&node);
}

void delete_rbtree(rbtree_t **tree) {
  if (!tree || !*tree) return;
  node_free(*tree, (*tree)->root);
  allocator_free((*tree)->allocator, (void **)&(*tree)->nil);
  allocator_free((*tree)->allocator, (void **)tree);
}

size_t rbtree_get_size(rbtree_t *self) { return self->size; }

/* ---- 旋转 ---- */
static void rotate_left(rbtree_t *tree, rbnode_t *x) {
  rbnode_t *y = x->right;
  x->right = y->left;
  if (y->left != tree->nil) y->left->parent = x;
  y->parent = x->parent;
  if (x->parent == tree->nil)
    tree->root = y;
  else if (x == x->parent->left)
    x->parent->left = y;
  else
    x->parent->right = y;
  y->left = x;
  x->parent = y;
}

static void rotate_right(rbtree_t *tree, rbnode_t *x) {
  rbnode_t *y = x->left;
  x->left = y->right;
  if (y->right != tree->nil) y->right->parent = x;
  y->parent = x->parent;
  if (x->parent == tree->nil)
    tree->root = y;
  else if (x == x->parent->right)
    x->parent->right = y;
  else
    x->parent->left = y;
  y->right = x;
  x->parent = y;
}

/* ---- 插入修复 ---- */
static void insert_fixup(rbtree_t *tree, rbnode_t *z) {
  while (z->parent->red) {
    if (z->parent == z->parent->parent->left) {
      rbnode_t *y = z->parent->parent->right;
      if (y->red) {
        z->parent->red = false;
        y->red = false;
        z->parent->parent->red = true;
        z = z->parent->parent;
      } else {
        if (z == z->parent->right) {
          z = z->parent;
          rotate_left(tree, z);
        }
        z->parent->red = false;
        z->parent->parent->red = true;
        rotate_right(tree, z->parent->parent);
      }
    } else {
      rbnode_t *y = z->parent->parent->left;
      if (y->red) {
        z->parent->red = false;
        y->red = false;
        z->parent->parent->red = true;
        z = z->parent->parent;
      } else {
        if (z == z->parent->left) {
          z = z->parent;
          rotate_right(tree, z);
        }
        z->parent->red = false;
        z->parent->parent->red = true;
        rotate_left(tree, z->parent->parent);
      }
    }
  }
  tree->root->red = false;
}

void *rbtree_insert(rbtree_t *self, uintptr_t key, void *value) {
  if (value == NULL) return NULL;
  rbnode_t *z = node_alloc(self, key, value);
  rbnode_t *y = self->nil;
  rbnode_t *x = self->root;
  while (x != self->nil) {
    y = x;
    if (key < x->key)
      x = x->left;
    else if (key > x->key)
      x = x->right;
    else {
      /* key 已存在：替换并交还旧值，由调用方负责释放。 */
      void *old_value = x->value;
      x->value = value;
      allocator_free(self->allocator, (void **)&z);
      return old_value;
    }
  }
  z->parent = y;
  if (y == self->nil)
    self->root = z;
  else if (key < y->key)
    y->left = z;
  else
    y->right = z;
  self->size++;
  insert_fixup(self, z);
  return NULL;
}

/* ---- 查找 ---- */
void *rbtree_find(rbtree_t *self, uintptr_t key) {
  rbnode_t *x = self->root;
  while (x != self->nil) {
    if (key < x->key)
      x = x->left;
    else if (key > x->key)
      x = x->right;
    else
      return x->value;
  }
  return NULL;
}

/* ---- 删除修复 ---- */
static void delete_fixup(rbtree_t *tree, rbnode_t *x) {
  while (x != tree->root && !x->red) {
    if (x == x->parent->left) {
      rbnode_t *w = x->parent->right;
      if (w->red) {
        w->red = false;
        x->parent->red = true;
        rotate_left(tree, x->parent);
        w = x->parent->right;
      }
      if (!w->left->red && !w->right->red) {
        w->red = true;
        x = x->parent;
      } else {
        if (!w->right->red) {
          w->left->red = false;
          w->red = true;
          rotate_right(tree, w);
          w = x->parent->right;
        }
        w->red = x->parent->red;
        x->parent->red = false;
        w->right->red = false;
        rotate_left(tree, x->parent);
        x = tree->root;
      }
    } else {
      rbnode_t *w = x->parent->left;
      if (w->red) {
        w->red = false;
        x->parent->red = true;
        rotate_right(tree, x->parent);
        w = x->parent->left;
      }
      if (!w->right->red && !w->left->red) {
        w->red = true;
        x = x->parent;
      } else {
        if (!w->left->red) {
          w->right->red = false;
          w->red = true;
          rotate_left(tree, w);
          w = x->parent->left;
        }
        w->red = x->parent->red;
        x->parent->red = false;
        w->left->red = false;
        rotate_right(tree, x->parent);
        x = tree->root;
      }
    }
  }
  x->red = false;
}

/* 求子树最小节点 */
static rbnode_t *tree_minimum(rbtree_t *tree, rbnode_t *x) {
  while (x->left != tree->nil)
    x = x->left;
  return x;
}

/* 用 v 子树替换 u 子树的位置 */
static void rb_transplant(rbtree_t *tree, rbnode_t *u, rbnode_t *v) {
  if (u->parent == tree->nil)
    tree->root = v;
  else if (u == u->parent->left)
    u->parent->left = v;
  else
    u->parent->right = v;
  v->parent = u->parent;
}

void *rbtree_erase(rbtree_t *self, uintptr_t key) {
  rbnode_t *z = self->root;
  while (z != self->nil) {
    if (key < z->key)
      z = z->left;
    else if (key > z->key)
      z = z->right;
    else
      break;
  }
  if (z == self->nil) return NULL;
  void *value = z->value;

  rbnode_t *y = z;
  rbnode_t *x;
  bool y_orig_red = y->red;
  if (z->left == self->nil) {
    x = z->right;
    rb_transplant(self, z, z->right);
  } else if (z->right == self->nil) {
    x = z->left;
    rb_transplant(self, z, z->left);
  } else {
    y = tree_minimum(self, z->right);
    y_orig_red = y->red;
    x = y->right;
    if (y->parent == z) {
      x->parent = y;
    } else {
      rb_transplant(self, y, y->right);
      y->right = z->right;
      y->right->parent = y;
    }
    rb_transplant(self, z, y);
    y->left = z->left;
    y->left->parent = y;
    y->red = z->red;
  }
  if (!y_orig_red) delete_fixup(self, x);
  allocator_free(self->allocator, (void **)&z);
  self->size--;
  return value;
}

/* ---- 遍历（中序） ---- */
static void foreach_inorder(rbtree_t *tree,
                            rbnode_t *node,
                            rbtree_callback_t callback,
                            void *user_data) {
  if (node == tree->nil) return;
  foreach_inorder(tree, node->left, callback, user_data);
  callback(node->key, node->value, user_data);
  foreach_inorder(tree, node->right, callback, user_data);
}

void rbtree_foreach(rbtree_t *self,
                    rbtree_callback_t callback,
                    void *user_data) {
  foreach_inorder(self, self->root, callback, user_data);
}
