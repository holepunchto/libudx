#include "internal.h"
#include "udx_sack_tree.h"
#include <stdbool.h>
#include <string.h>

// in most cases we return the sack block that contains or ends
// on the sequence number, in order to easily append data to existing
// sack blocks.
// there is an exception: if two sack blocks are adjacent to each
// other at seq, ie seq=4 and we have sacks [3:4] and [4:5], we
// return the sack block that includes 4
udx_sack_block_t *
udx_sack_tree_find (udx_sack_tree_t *tree, uint32_t seq) {
  udx_sack_block_t *block = tree->root;
  udx_sack_block_t *sentinel = tree->sentinel;

  while (block != sentinel) {
    if (seq_diff(seq, block->start) < 0) {
      block = block->left;
      continue;
    }
    if (seq_diff(seq, block->end) > 0) {
      block = block->right;
      continue;
    }

    // if there are two adjacent sack blocks, ie [3:4] and [4:5]
    // then return [4:5]
    if (block->end == seq) {
      udx_sack_block_t *next = udx_sack_tree_next(tree, block);
      if (next && next->start == seq) {
        return next;
      }
    }

    return block;
  }
  return NULL;
}

static inline void
rbtree_left_rotate (udx_sack_block_t **root, udx_sack_block_t *sentinel, udx_sack_block_t *node) {
  udx_sack_block_t *temp;

  temp = node->right;
  node->right = temp->left;

  if (temp->left != sentinel) {
    temp->left->parent = node;
  }

  temp->parent = node->parent;

  if (node == *root) {
    *root = temp;
  } else if (node == node->parent->left) {
    node->parent->left = temp;
  } else {
    node->parent->right = temp;
  }

  temp->left = node;
  node->parent = temp;
}

static inline void
rbtree_right_rotate (udx_sack_block_t **root, udx_sack_block_t *sentinel, udx_sack_block_t *node) {
  udx_sack_block_t *temp;

  temp = node->left;
  node->left = temp->right;

  if (temp->right != sentinel) {
    temp->right->parent = node;
  }

  temp->parent = node->parent;

  if (node == *root) {
    *root = temp;

  } else if (node == node->parent->right) {
    node->parent->right = temp;

  } else {
    node->parent->left = temp;
  }

  temp->right = node;
  node->parent = temp;
}

void
udx_sack_tree_init (udx_sack_tree_t *tree) {
  memset(tree, 0, sizeof(*tree));
  tree->sentinel = &tree->_sentinel;
  rbt_black(tree->sentinel);
  tree->root = tree->sentinel;
}

static void
actual_sack_insert_fn (udx_sack_block_t *temp, udx_sack_block_t *block, udx_sack_block_t *sentinel) {
  udx_sack_block_t **p = NULL;
  while (1) {
    p = ((int) (block->start - temp->start) < 0) ? &temp->left : &temp->right;

    if (*p == sentinel) {
      break;
    }
    temp = *p;
  }

  *p = block;
  block->parent = temp;
  block->left = sentinel;
  block->right = sentinel;
  rbt_red(block);
}

void
udx_sack_tree_insert (udx_sack_tree_t *tree, udx_sack_block_t *block) {
  udx_sack_block_t **root = &tree->root;
  udx_sack_block_t *sentinel = tree->sentinel;

  if (*root == sentinel) {
    block->parent = NULL;
    block->left = sentinel;
    block->right = sentinel;
    rbt_black(block);
    *root = block;
    return;
  }

  actual_sack_insert_fn(*root, block, sentinel);

  // re-balance tree

  while (block != *root && rbt_is_red(block->parent)) {
    if (block->parent == block->parent->parent->left) {
      udx_sack_block_t *temp = block->parent->parent->right;

      if (rbt_is_red(temp)) {
        rbt_black(block->parent);
        rbt_black(temp);
        rbt_red(block->parent->parent);
        block = block->parent->parent;
      } else {
        if (block == block->parent->right) {
          block = block->parent;
          rbtree_left_rotate(root, sentinel, block);
        }
        rbt_black(block->parent);
        rbt_red(block->parent->parent);
        rbtree_right_rotate(root, sentinel, block->parent->parent);
      }
    } else {
      udx_sack_block_t *temp = block->parent->parent->left;

      if (rbt_is_red(temp)) {
        rbt_black(block->parent);
        rbt_black(temp);
        rbt_red(block->parent->parent);
        block = block->parent->parent;
      } else {
        if (block == block->parent->left) {
          block = block->parent;
          rbtree_right_rotate(root, sentinel, block);
        }

        rbt_black(block->parent);
        rbt_red(block->parent->parent);
        rbtree_left_rotate(root, sentinel, block->parent->parent);
      }
    }
  }
  rbt_black(*root);
}

static udx_sack_block_t *
rbtree_min (udx_sack_block_t *node, udx_sack_block_t *sentinel) {
  while (node->left != sentinel) {
    node = node->left;
  }
  return node;
}

udx_sack_block_t *
udx_sack_tree_min (udx_sack_tree_t *tree) {
  if (tree->root == tree->sentinel) {
    return NULL;
  }
  return rbtree_min(tree->root, tree->sentinel);
}

void
udx_sack_tree_remove (udx_sack_tree_t *tree, udx_sack_block_t *block) {

  udx_sack_block_t **root = &tree->root;
  udx_sack_block_t *sentinel = tree->sentinel;

  udx_sack_block_t *temp;
  udx_sack_block_t *subst;

  if (block->left == sentinel) {
    temp = block->right;
    subst = block;

  } else if (block->right == sentinel) {
    temp = block->left;
    subst = block;

  } else {
    subst = rbtree_min(block->right, sentinel);
    temp = subst->right;
  }

  if (subst == *root) {
    *root = temp;
    rbt_black(temp);

    /* DEBUG stuff */
    block->left = NULL;
    block->right = NULL;
    block->parent = NULL;

    return;
  }

  bool red = rbt_is_red(subst);

  if (subst == subst->parent->left) {
    subst->parent->left = temp;

  } else {
    subst->parent->right = temp;
  }

  if (subst == block) {

    temp->parent = subst->parent;

  } else {

    if (subst->parent == block) {
      temp->parent = subst;

    } else {
      temp->parent = subst->parent;
    }

    subst->left = block->left;
    subst->right = block->right;
    subst->parent = block->parent;
    rbt_copy_color(subst, block);

    if (block == *root) {
      *root = subst;

    } else {
      if (block == block->parent->left) {
        block->parent->left = subst;
      } else {
        block->parent->right = subst;
      }
    }

    if (subst->left != sentinel) {
      subst->left->parent = subst;
    }

    if (subst->right != sentinel) {
      subst->right->parent = subst;
    }
  }

  /* DEBUG stuff */
  block->left = NULL;
  block->right = NULL;
  block->parent = NULL;

  if (red) {
    return;
  }

  /* a remove fixup */

  udx_sack_block_t *w;

  while (temp != *root && rbt_is_black(temp)) {

    if (temp == temp->parent->left) {
      w = temp->parent->right;

      if (rbt_is_red(w)) {
        rbt_black(w);
        rbt_red(temp->parent);
        rbtree_left_rotate(root, sentinel, temp->parent);
        w = temp->parent->right;
      }

      if (rbt_is_black(w->left) && rbt_is_black(w->right)) {
        rbt_red(w);
        temp = temp->parent;

      } else {
        if (rbt_is_black(w->right)) {
          rbt_black(w->left);
          rbt_red(w);
          rbtree_right_rotate(root, sentinel, w);
          w = temp->parent->right;
        }

        rbt_copy_color(w, temp->parent);
        rbt_black(temp->parent);
        rbt_black(w->right);
        rbtree_left_rotate(root, sentinel, temp->parent);
        temp = *root;
      }

    } else {
      w = temp->parent->left;

      if (rbt_is_red(w)) {
        rbt_black(w);
        rbt_red(temp->parent);
        rbtree_right_rotate(root, sentinel, temp->parent);
        w = temp->parent->left;
      }

      if (rbt_is_black(w->left) && rbt_is_black(w->right)) {
        rbt_red(w);
        temp = temp->parent;

      } else {
        if (rbt_is_black(w->left)) {
          rbt_black(w->right);
          rbt_red(w);
          rbtree_left_rotate(root, sentinel, w);
          w = temp->parent->left;
        }

        rbt_copy_color(w, temp->parent);
        rbt_black(temp->parent);
        rbt_black(w->left);
        rbtree_right_rotate(root, sentinel, temp->parent);
        temp = *root;
      }
    }
  }

  rbt_black(temp);
}

// get the next sack block
udx_sack_block_t *
udx_sack_tree_next (udx_sack_tree_t *tree, udx_sack_block_t *block) {
  udx_sack_block_t *sentinel = tree->sentinel;

  if (block->right != sentinel) {
    return rbtree_min(block->right, sentinel);
  }

  udx_sack_block_t *root = tree->root;

  while (1) {
    udx_sack_block_t *parent = block->parent;

    if (block == root) {
      return NULL;
    }
    if (block == parent->left) {
      return parent;
    }
    block = parent;
  }
}
