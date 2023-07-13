// SPDX-License-Identifier: GPL-2.0-only
/*
这段代码来自DRBD项目中的`drbd_interval.c`文件，定义了处理DRBD（Distributed Replicated Block Device）项目中所使用的区间树（interval tree）的一系列函数。
一个区间树是一种特殊的数据结构，可以有效地查询一个给定区间是否与树中的任何区间重叠。
下面是函数的概述：
- `interval_end`: 返回指定节点的结束位置。
- `drbd_insert_interval`: 在区间树中插入一个新的区间。
- `drbd_contains_interval`: 检查区间树是否包含给定的区间。
- `drbd_remove_interval`: 从区间树中移除一个区间。
- `drbd_find_overlap`: 查找一个与给定区间重叠的区间。
- `drbd_next_overlap`: 获取下一个与给定区间重叠的区间。
在上述函数中，区间被表示为`struct drbd_interval`类型，这个结构可能包含了区间的开始（sector）和大小（size）等信息。
这些函数在插入，查询，删除和查找区间树中的重叠区间等操作中被使用。
注意：上述解释是基于代码的读取，没有上下文的其他信息，如特定的项目文档，可能会对理解有所帮助。
此外，部分解释可能基于对相似代码模式的理解，而这些模式可能具有一般性，但可能在特定情况下有所不同。
*/
#include <asm/bug.h>
#include <linux/rbtree_augmented.h>
#include "drbd_interval.h"

/*
 * interval_end  -  return end of @node
 */
static inline
sector_t interval_end(struct rb_node *node)
{
	struct drbd_interval *this = rb_entry(node, struct drbd_interval, rb);
	return this->end;
}

#define NODE_END(node) ((node)->sector + ((node)->size >> 9))
RB_DECLARE_CALLBACKS_MAX(static, augment_callbacks, struct drbd_interval, rb,
		sector_t, end, NODE_END);

/*
 * drbd_insert_interval  -  insert a new interval into a tree
 */
bool
drbd_insert_interval(struct rb_root *root, struct drbd_interval *this)
{
	struct rb_node **new = &root->rb_node, *parent = NULL;
	sector_t this_end = this->sector + (this->size >> 9);

	BUG_ON(!IS_ALIGNED(this->size, 512));

	while (*new) {
		struct drbd_interval *here =
			rb_entry(*new, struct drbd_interval, rb);

		parent = *new;
		if (here->end < this_end)
			here->end = this_end;
		if (this->sector < here->sector)
			new = &(*new)->rb_left;
		else if (this->sector > here->sector)
			new = &(*new)->rb_right;
		else if (this < here)
			new = &(*new)->rb_left;
		else if (this > here)
			new = &(*new)->rb_right;
		else
			return false;
	}

	this->end = this_end;
	rb_link_node(&this->rb, parent, new);
	rb_insert_augmented(&this->rb, root, &augment_callbacks);
	return true;
}

/**
 * drbd_contains_interval  -  check if a tree contains a given interval
 * @root:	red black tree root
 * @sector:	start sector of @interval
 * @interval:	may be an invalid pointer
 *
 * Returns if the tree contains the node @interval with start sector @start.
 * Does not dereference @interval until @interval is known to be a valid object
 * in @tree.  Returns %false if @interval is in the tree but with a different
 * sector number.
 */
bool
drbd_contains_interval(struct rb_root *root, sector_t sector,
		       struct drbd_interval *interval)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct drbd_interval *here =
			rb_entry(node, struct drbd_interval, rb);

		if (sector < here->sector)
			node = node->rb_left;
		else if (sector > here->sector)
			node = node->rb_right;
		else if (interval < here)
			node = node->rb_left;
		else if (interval > here)
			node = node->rb_right;
		else
			return true;
	}
	return false;
}

/*
 * drbd_remove_interval  -  remove an interval from a tree
 */
void
drbd_remove_interval(struct rb_root *root, struct drbd_interval *this)
{
	/* avoid endless loop */
	if (drbd_interval_empty(this))
		return;

	rb_erase_augmented(&this->rb, root, &augment_callbacks);
}

/**
 * drbd_find_overlap  - search for an interval overlapping with [sector, sector + size)
 * @root:	red black tree root
 * @sector:	start sector
 * @size:	size, aligned to 512 bytes
 *
 * Returns an interval overlapping with [sector, sector + size), or NULL if
 * there is none.  When there is more than one overlapping interval in the
 * tree, the interval with the lowest start sector is returned, and all other
 * overlapping intervals will be on the right side of the tree, reachable with
 * rb_next().
 */
struct drbd_interval *
drbd_find_overlap(struct rb_root *root, sector_t sector, unsigned int size)
{
	struct rb_node *node = root->rb_node;
	struct drbd_interval *overlap = NULL;
	sector_t end = sector + (size >> 9);

	BUG_ON(!IS_ALIGNED(size, 512));

	while (node) {
		struct drbd_interval *here =
			rb_entry(node, struct drbd_interval, rb);

		if (node->rb_left &&
		    sector < interval_end(node->rb_left)) {
			/* Overlap if any must be on left side */
			node = node->rb_left;
		} else if (here->sector < end &&
			   sector < here->sector + (here->size >> 9)) {
			overlap = here;
			break;
		} else if (sector >= here->sector) {
			/* Overlap if any must be on right side */
			node = node->rb_right;
		} else
			break;
	}
	return overlap;
}

struct drbd_interval *
drbd_next_overlap(struct drbd_interval *i, sector_t sector, unsigned int size)
{
	sector_t end = sector + (size >> 9);
	struct rb_node *node;

	for (;;) {
		node = rb_next(&i->rb);
		if (!node)
			return NULL;
		i = rb_entry(node, struct drbd_interval, rb);
		if (i->sector >= end)
			return NULL;
		if (sector < i->sector + (i->size >> 9))
			return i;
	}
}
