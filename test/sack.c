
#include "../include/udx.h"
#include "../src/udx_sack_tree.h"
#include <assert.h>

udx_sack_block_t block[10];

int
main () {
  udx_sack_tree_t tree;
  udx_sack_block_t sentinel;

  udx_sack_tree_init(&tree);

  block[0].start = 1;
  block[0].end = 2;
  block[1].start = 6;
  block[1].end = 8;
  block[2].start = 8;
  block[2].end = 9;
  block[3].start = 11;
  block[3].end = 12;
  block[4].start = 13;
  block[4].end = 15;
  block[5].start = 15;
  block[5].end = 16;
  block[6].start = 17;
  block[6].end = 19;
  block[7].start = 22;
  block[7].end = 23;
  block[8].start = 25;
  block[8].end = 26;
  block[9].start = 27;
  block[9].end = 50;

  int i = 0;
  for (i = 0; i < 10; i++) {
    udx_sack_tree_insert(&tree, block + i);
  }

  int middle_sack_block_start = tree.root->start;
  assert(middle_sack_block_start == 11);

  i = 0;
  // todo: sack_block_min private api?
  // for public, cache tree.min, or have a udx_sack_block_min(udx_tree_t *tree) API
  udx_sack_block_t *p = udx_sack_tree_min(&tree);

  while (p != NULL) {
    assert(p->start == block[i].start);
    assert(p->end == block[i].end);
    p = udx_sack_tree_next(&tree, p);
    i++;
  }

  i = 0;

  for (i = 0; i < 10; i++) {
    udx_sack_block_t *min = udx_sack_tree_min(&tree);

    assert(min != NULL);
    assert(min->start == block[i].start && min->end == block[i].end);

    udx_sack_tree_remove(&tree, min);
  }

  assert(tree.root == tree.sentinel);

  // some assurance that the code works with wrapped sequence numbers
  // the following code re-inserts the sack blocks, but shifts them over to be near UINT32_MAX so that
  // roughly half of them are sequence wrapped, then we check that the tree is still in the correct order
  // [4294967285:4294967286] [4294967290:4294967292] [4294967292:4294967293] [4294967295:0]
  // [1:3] [3:4] [5:7] [10:11] [13:14] [15:38]

  int offset = UINT32_MAX - middle_sack_block_start;
  for (i = 0; i < 10; i++) {
    block[i].start += offset;
    block[i].end += offset;
    udx_sack_tree_insert(&tree, block + i);
  }

  i = 0;

  p = udx_sack_tree_min(&tree);

  while (p != NULL) {
    assert(p->start == block[i].start);
    assert(p->end == block[i].end);
    p = udx_sack_tree_next(&tree, p);
    i++;
  }

  for (i = 0; i < 10; i++) {
    udx_sack_block_t *min = udx_sack_tree_min(&tree);

    assert(min != NULL);
    assert(min->start == block[i].start && min->end == block[i].end);

    udx_sack_tree_remove(&tree, min);
  }
  assert(tree.root == tree.sentinel);

  return 0;
}
