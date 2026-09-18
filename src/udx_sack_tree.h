#ifndef UDX_SACK_BLOCK_TREE_H
#define UDX_SACK_BLOCK_TREE_H
#include <stdint.h>
#include <stdlib.h>

#include "udx.h"

void
udx_sack_tree_init (udx_sack_tree_t *tree);

udx_sack_block_t *
udx_sack_tree_find (udx_sack_tree_t *tree, uint32_t seq);

void
udx_sack_tree_insert (udx_sack_tree_t *tree, udx_sack_block_t *block);
void
udx_sack_tree_remove (udx_sack_tree_t *tree, udx_sack_block_t *block);

// walk the sack tree from the left to write the sack packet
udx_sack_block_t *
udx_sack_tree_next (udx_sack_tree_t *tree, udx_sack_block_t *block);

udx_sack_block_t *
udx_sack_tree_min (udx_sack_tree_t *tree);

// red-black tree macros

#define rbt_red(node)          ((node)->color = 1)
#define rbt_black(node)        ((node)->color = 0)
#define rbt_is_red(node)       ((node)->color)
#define rbt_is_black(node)     (!rbt_is_red(node))
#define rbt_copy_color(n1, n2) (n1->color = n2->color)

#endif
