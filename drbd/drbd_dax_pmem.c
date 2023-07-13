// SPDX-License-Identifier: GPL-2.0-only
/*
   drbd_dax.c

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2017, LINBIT HA-Solutions GmbH.


 */

/*
 * 你给出的这个文件`drbd_dax.c`是DRBD项目的一部分，它与DRBD在处理元数据时对DAX（Direct Access）功能的使用有关。
 * DAX是一个在持久内存（persistent memory）环境中，允许应用程序直接访问存储设备，避免了页缓存，从而实现了数据访问的低延迟和高吞吐量的特性。
 * 在这个文件中，主要定义了一些函数，它们允许DRBD直接访问存储在持久内存中的元数据，这样可以提高元数据的访问性能。这里的元数据可能包括DRBD的位图和活动日志等。

主要的函数包括：

- `map_superblock_for_dax`：这个函数将元数据的超级块映射到DAX设备。
- `drbd_dax_open`：这个函数打开DAX设备，并映射元数据的超级块。
- `drbd_dax_close`：这个函数关闭DAX设备。
- `drbd_dax_map`：这个函数映射元数据到DAX设备。
- `drbd_dax_al_update`和`drbd_dax_al_begin_io_commit`：这两个函数用于更新存储在持久内存中的活动日志。
- `drbd_dax_al_initialize`：这个函数初始化存储在持久内存中的活动日志。
- `drbd_dax_bitmap`：这个函数返回指向DAX设备上的DRBD位图的指针。

所有这些函数都充分利用了DAX的优势，即能够直接访问持久内存，而无需通过页缓存，从而提高了访问持久内存的性能。
这在DRBD的上下文中尤其重要，因为DRBD需要频繁地读写元数据，如果这些操作可以在持久内存上以低延迟和高吞吐量的方式执行，那么DRBD的总体性能将得到提高。
*/ 

/*
  In case DRBD's meta-data resides in persistent memory do a few things
   different.

   1 direct access the bitmap in place. Do not load it into DRAM, do not
     write it back from DRAM.
   2 Use a better fitting format for the on-disk activity log. Instead of
     writing transactions, the unmangled LRU-cache hash table is there.
*/

#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/dax.h>
#include <linux/pfn_t.h>
#include <linux/libnvdimm.h>
#include <linux/blkdev.h>
#include "drbd_int.h"
#include "drbd_dax_pmem.h"
#include "drbd_meta_data.h"

static int map_superblock_for_dax(struct drbd_backing_dev *bdev, struct dax_device *dax_dev)
{
	long want = 1;
	pgoff_t pgoff = bdev->md.md_offset >> (PAGE_SHIFT - SECTOR_SHIFT);
	void *kaddr;
	long len;
	pfn_t pfn_unused; /* before 4.18 it is required to pass in non-NULL */
	int id;

	id = dax_read_lock();
	len = dax_direct_access(dax_dev, pgoff, want, DAX_ACCESS, &kaddr, &pfn_unused);
	dax_read_unlock(id);

	if (len < want)
		return -EIO;

	bdev->md_on_pmem = kaddr;

	return 0;
}

/**
 * drbd_dax_open() - Open device for dax and map metadata superblock
 * @bdev: backing device to be opened
 */
int drbd_dax_open(struct drbd_backing_dev *bdev)
{
	struct dax_device *dax_dev;
	int err;
	u64 part_off;

	dax_dev = fs_dax_get_by_bdev(bdev->md_bdev, &part_off, NULL, NULL);
	if (!dax_dev)
		return -ENODEV;

	err = map_superblock_for_dax(bdev, dax_dev);
	if (!err)
		bdev->dax_dev = dax_dev;
	else
		put_dax(dax_dev);

	return err;
}

void drbd_dax_close(struct drbd_backing_dev *bdev)
{
	put_dax(bdev->dax_dev);
}

/**
 * drbd_dax_map() - Map metadata for dax
 * @bdev: backing device whose metadata is to be mapped
 */
int drbd_dax_map(struct drbd_backing_dev *bdev)
{
	struct dax_device *dax_dev = bdev->dax_dev;
	sector_t first_sector = drbd_md_first_sector(bdev);
	sector_t al_sector = bdev->md.md_offset + bdev->md.al_offset;
	long want = (drbd_md_last_sector(bdev) + 1 - first_sector) >> (PAGE_SHIFT - SECTOR_SHIFT);
	pgoff_t pgoff = first_sector >> (PAGE_SHIFT - SECTOR_SHIFT);
	long md_offset_byte = (bdev->md.md_offset - first_sector) << SECTOR_SHIFT;
	long al_offset_byte = (al_sector - first_sector) << SECTOR_SHIFT;
	void *kaddr;
	long len;
	pfn_t pfn_unused; /* before 4.18 it is required to pass in non-NULL */
	int id;

	id = dax_read_lock();
	len = dax_direct_access(dax_dev, pgoff, want, DAX_ACCESS, &kaddr, &pfn_unused);
	dax_read_unlock(id);

	if (len < want)
		return -EIO;

	bdev->md_on_pmem = kaddr + md_offset_byte;
	bdev->al_on_pmem = kaddr + al_offset_byte;

	return 0;
}

void drbd_dax_al_update(struct drbd_device *device, struct lc_element *al_ext)
{
	struct al_on_pmem *al_on_pmem = device->ldev->al_on_pmem;
	__be32 *slot = &al_on_pmem->slots[al_ext->lc_index];

	*slot = cpu_to_be32(al_ext->lc_new_number);
	arch_wb_cache_pmem(slot, sizeof(*slot));
}


void drbd_dax_al_begin_io_commit(struct drbd_device *device)
{
	struct lc_element *e;

	spin_lock_irq(&device->al_lock);

	list_for_each_entry(e, &device->act_log->to_be_changed, list)
		drbd_dax_al_update(device, e);

	lc_committed(device->act_log);

	spin_unlock_irq(&device->al_lock);
}

int drbd_dax_al_initialize(struct drbd_device *device)
{
	struct al_on_pmem *al_on_pmem = device->ldev->al_on_pmem;
	__be32 *slots = al_on_pmem->slots;
	int i, al_slots = (device->ldev->md.al_size_4k << (12 - 2)) - 1;

	al_on_pmem->magic = cpu_to_be32(DRBD_AL_PMEM_MAGIC);
	/* initialize all slots rather than just the configured number in case
	 * the configuration is later changed */
	for (i = 0; i < al_slots; i++) {
		unsigned int extent_nr = i < device->act_log->nr_elements ?
			lc_element_by_index(device->act_log, i)->lc_number :
			LC_FREE;
		slots[i] = cpu_to_be32(extent_nr);
	}

	return 0;
}

void *drbd_dax_bitmap(struct drbd_device *device, unsigned long want)
{
	struct drbd_backing_dev *bdev = device->ldev;
	unsigned char *md_on_pmem = (unsigned char *)bdev->md_on_pmem;

	return md_on_pmem + (long)bdev->md.bm_offset * SECTOR_SIZE;
}
