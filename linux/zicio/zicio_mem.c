// SPDX-License-Identifier: GPL-2.0
#include <asm/elf.h>
#include <asm/processor.h>
#include <asm/io.h>
#include <linux/blk_types.h>
#include <linux/blk-mq.h>
#include <linux/backing-dev-defs.h>
#include <linux/device.h>
#include <linux/genhd.h>
#include <linux/mempolicy.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/gfp.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/swiotlb.h>
#include <linux/security.h>
#include <linux/dma-mapping.h>
#include <uapi/asm-generic/mman-common.h>
#include <uapi/linux/mman.h>
#include <asm/atomic.h>

#include <linux/syscalls.h>

#include <linux/time_namespace.h>
#include <asm/timer.h>

#include "zicio_cmd.h"
#include "zicio_device.h"
#include "zicio_ghost.h"
#include "zicio_files.h"
#include "zicio_mem.h"

/* Slab allocators for zicio descriptor */
static struct kmem_cache *zicio_descriptor_cache;
/* Slab allocators for zicio shared pool */
static struct kmem_cache *zicio_shared_pool_cache;
/* Slab allocators for zicio descriptor */
static struct kmem_cache *zicio_command_cache;
/* Slab allocators for zicio request timer */
static struct kmem_cache *zicio_req_timer_cache;
/* Slab allocators for zicio shared request timer */
static struct kmem_cache *zicio_shared_req_timer_cache;
/* Slab allocators for zicio dio */
static struct kmem_cache *zicio_dio_cache;
/* Slab allocators for zicio idlist */
static struct kmem_cache *zicio_idlist_cache;
/* Slab allocators for zicio idlist */
static struct kmem_cache *zicio_spcb_cache;

/* Slab allocator setting definition for current support device */
__zicio_slab_info zicio_cache_info[ZICIO_NUM_ALLOC_CACHE] = {
	/* Start of mgmt cache */
	{"zicio_descriptor", ZICIO_DESC_SIZE,
		ZICIO_DESC_SLAB_ALIGN, 0, 0,
		ZICIO_DESC_SLAB_FLAG, &zicio_descriptor_cache},
	{"zicio_shared_pool", ZICIO_SPOOL_SIZE,
		ZICIO_SPOOL_SLAB_ALIGN, 0, 0,
		ZICIO_SPOOL_SLAB_FLAG, &zicio_shared_pool_cache},
	/* End of mgmt cache */
	{"zicio_block_data_buffer", ZICIO_DATABUFFER_SIZE, 
		ZICIO_DATABUFFER_SLAB_ALIGN, ZICIO_CHUNK_SIZE,
		ZICIO_CHUNK_ORDER, ZICIO_DATABUFFER_SLAB_FLAG, NULL},
	{"zicio_block_command_buffer", ZICIO_COMMANDELEM_SIZE,
		ZICIO_COMMANDELEM_SLAB_ALIGN, 0, 0,
		SLAB_HWCACHE_ALIGN|SLAB_TYPESAFE_BY_RCU, &zicio_command_cache},
	{"zicio_block_metadata_buffer", ZICIO_METADATABUFFER_SIZE,
		ZICIO_METADATABUFFER_SLAB_ALIGN, ZICIO_CHUNK_SIZE,  
		ZICIO_CHUNK_ORDER, SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL},
	{"zicio_inode_buffer", ZICIO_INODEBUFFER_SIZE,
		ZICIO_INODEBUFFER_SLAB_ALIGN, ZICIO_CHUNK_SIZE,
		ZICIO_CHUNK_ORDER, SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL},
	{"zicio_extent_tree_buffer", ZICIO_EXTENTTREEBUFFER_SIZE,
		ZICIO_EXTENTTREEBUFFER_SLAB_ALIGN, ZICIO_CHUNK_SIZE,
		ZICIO_CHUNK_ORDER, SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL},
	{"zicio_timer_buffer", ZICIO_TIMERELEM_SIZE,
		ZICIO_TIMERELEM_SLAB_ALIGN, 0, 0, SLAB_HWCACHE_ALIGN|SLAB_PANIC,
		&zicio_req_timer_cache},
	{"zicio_shared_timer_buffer", ZICIO_SHARED_TIMERELEM_SIZE,
		ZICIO_SHARED_TIMERELEM_SLAB_ALIGN, 0, 0,
		SLAB_HWCACHE_ALIGN|SLAB_PANIC, &zicio_shared_req_timer_cache},
	{"zicio_dio_buffer", ZICIO_DIOELEM_SIZE,
		ZICIO_DIOELEM_SLAB_ALIGN, 0,
		0, SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_TYPESAFE_BY_RCU,
		&zicio_dio_cache},
	{"zicio_idlist_buffer", ZICIO_IDLISTELEM_SIZE,
		ZICIO_IDLISTELEM_SLAB_ALIGN, 0,
		0, SLAB_HWCACHE_ALIGN|SLAB_PANIC, &zicio_idlist_cache},
	{"zicio_spcbblem_buffer", ZICIO_SPCB_HASH_ELEM_SIZE,
		ZICIO_SPCB_HASH_ELEM_SLAB_ALIGN, 0,
		0, SLAB_HWCACHE_ALIGN|SLAB_PANIC, &zicio_spcb_cache},
};

/*
 * Real worker to initialize slab cache of zicio
 */
static void __init __zicio_init_slab_cache(const char * name,
			unsigned int size, unsigned int align, slab_flags_t flags,
			struct kmem_cache ** slab_cache)
{
	/* create kmem cache */
	*slab_cache = kmem_cache_create(name, size, align, flags, NULL);

	if (!(*slab_cache)) {
		printk(KERN_WARNING "[Kernel Message] Fail to create %s slab\n", name);
	}
}

/*
 * Initialize zicio slab caches
 */
void __init zicio_init_slab_caches(void)
{
	__zicio_slab_info *s_info;

	for (s_info = zicio_cache_info; 
			s_info < &zicio_cache_info[ZICIO_NUM_ALLOC_CACHE] ;
			s_info++) {

		/* Initialize zicio slab cache with info */
		if (s_info->cache_addr) {
			__zicio_init_slab_cache(s_info->name, s_info->size,
					s_info->align, s_info->flags, s_info->cache_addr);
		}
	}
}

/*
 * zicio_get_nvme_cmd_list
 *
 * Allocate nvme command
 */
struct zicio_nvme_cmd_list *
zicio_get_nvme_cmd_list(void) 
{
	return kmem_cache_alloc(zicio_command_cache, GFP_KERNEL|__GFP_ZERO);
}

/*
 * zicio_free_nvme_cmd_list
 *
 *  Free nvme command
 */
void 
zicio_free_nvme_cmd_list(void *cmd)
{
	kmem_cache_free(zicio_command_cache, cmd);
}
EXPORT_SYMBOL(zicio_free_nvme_cmd_list);

/*
 * zicio_get_zicio_bitvector
 *
 * Get the memory area for zicio shared bitvector
 */
void*
zicio_get_zicio_bitvector(void)
{
	return kmalloc(sizeof(zicio_bitvector), GFP_KERNEL);
}

/*
 * zicio_free_bitvector
 *
 * Free zicio shared bitvector areas
 */
void
zicio_free_zicio_bitvector(void *shared_bits)
{
	kfree(shared_bits);
}

/*
 * zicio_get_dio
 *
 * Allocate dio cache
 */
void *
zicio_get_dio(void)
{
	return kmem_cache_alloc(zicio_dio_cache, GFP_KERNEL);
}

/*
 * zicio_free_dio
 *
 * Free dio cache
 */
void
zicio_free_dio(void *dio)
{
	kmem_cache_free(zicio_dio_cache, dio);
}
EXPORT_SYMBOL(zicio_free_dio);

/*
 * zicio_get_idnode
 *
 * Allocate idnode
 */
void *
zicio_get_idnode(int id)
{
	zicio_id_node *id_node = kmem_cache_alloc(zicio_idlist_cache,
				GFP_KERNEL);
	id_node->id = id;
	return id_node;
}

/*
 * zicio_free_idnode
 *
 * Free idnode
 */
void
zicio_free_idnode(void *node) 
{
	kmem_cache_free(zicio_idlist_cache, node);
}

/*
 * Free memory buffer chunks
 */
static void
__zicio_free_buffer_chunk(zicio_descriptor *desc)
{
	free_pages((unsigned long)desc->buffers.metadata_buffer,
			ZICIO_CHUNK_ORDER);
	free_pages((unsigned long)desc->metadata_ctrl.inode_buffer,
			ZICIO_CHUNK_ORDER);
}

static void
__zicio_free_data_buffer_page(void *data_buffer_page)
{
	struct page *free_page;

	free_page = virt_to_page(data_buffer_page);
	ClearPageReserved(free_page);
	__free_pages(free_page, compound_order(free_page));
}

/*
 * __zicio_free_data_buffer_pages
 *
 * Free data buffer as many pages as num_pages
 */
static void
__zicio_free_data_buffer_pages(zicio_buffer *buffers, int num_pages)
{
	int i;

	for (i = 0 ; i < num_pages ; i++) {
		__zicio_free_data_buffer_page(buffers->data_buffer[i]);
	}
}

/*
 * zicio_get_device_type
 *
 * @ dev: dev to register
 *
 * Return : the type of device
 *
 * Currently, we only consider the block device
 */
static inline int 
zicio_get_device_type(void) {
	/* Currently we only consider this type of device */
	return ZICIO_BLOCK_CACHE;
}

/*
 * zicio_allocate_data_buffer
 *
 * @zicio_buffers: zicio buffer address structures
 * @num_huge_pages: number of huge pages to allocate
 *
 * Return: If succeeding, than return 0. Otherwise, return 1.
 */
int 
zicio_allocate_data_buffer(zicio_buffer *zicio_buffers, int num_huge_pages)
{
	struct page *huge_page;
	int i;

	/* Allocate data buffer and its address array */
	zicio_buffers->data_buffer = (void **)kmalloc(
			sizeof(void *) * num_huge_pages, GFP_KERNEL);
	zicio_buffers->num_data_buffer_pages = num_huge_pages;

	for (i = 0 ; i < num_huge_pages ; i++) {
		huge_page = zicio_allocate_hugepage();
		if (huge_page) {
			zicio_buffers->data_buffer[i] = page_to_virt(huge_page);
		} else {
			printk(KERN_WARNING "[Kernel Message] Error in alloc hugepage\n");
			__zicio_free_data_buffer_pages(zicio_buffers, i);

			return -1;
		}
	}

	return 0;
}

/*
 * zicio_allocate_metadata_buffer
 *
 * @zicio_buffers: zicio buffer address structures
 *
 * Return: If succeeding, than return 0. Otherwise, return 1.
 */
static int
zicio_allocate_metadata_buffer(zicio_buffer *zicio_buffers)
{
	zicio_buffers->metadata_buffer = page_to_virt(
			alloc_pages(GFP_KERNEL, ZICIO_CHUNK_ORDER));

	if (unlikely(!zicio_buffers->metadata_buffer)) {
		return -1;
	}

	return 0;
}

/*
 * zicio_allocate_buffer_cache
 *
 * @ devs : device array to register
 * @ alloced : the number of devices in array
 *
 * Return : allocated zicio buffer pointer if sucessful.
 * Otherwise, return NULL or errror code
 *
 * Real worker to allocate memory buffer of zicio
 */
static zicio_descriptor*
zicio_allocate_buffer_cache(unsigned long user_base_address, bool is_shared)
{
	zicio_descriptor *desc;
	int num_pages = (is_shared) ? ZICIO_LOCAL_DATABUFFER_CHUNK_NUM :
			ZICIO_DATABUFFER_CHUNK_NUM;

	/* Get the zicio descriptor's pointer from slab */
	if (!(desc = kmem_cache_alloc(zicio_descriptor_cache,
				GFP_KERNEL|__GFP_ZERO))) {
		goto l_zicio_allocate_buffer_cache_out;
	}

	/*
	 * Allocate data buffer
	 */
	if ((zicio_allocate_data_buffer(&desc->buffers, num_pages)) < 0) {
		goto l_zicio_allocate_buffer_cache_free_desc;
	}

	/*
	 * Allocate buffer's for data channel.
	 * metadata and inode buffer are allocated in here.
	 * In case of extent tree buffer, the buffer for toggling is allocated
	 */
	if ((zicio_allocate_metadata_buffer(&desc->buffers)) < 0) {
		goto l_zicio_allocate_buffer_cache_free_data_and_desc;
	}

	desc->metadata_ctrl.inode_buffer = page_to_virt(
			alloc_pages(GFP_KERNEL, ZICIO_CHUNK_ORDER));

	if (unlikely(!desc->metadata_ctrl.inode_buffer)) {
		goto l_zicio_allocate_buffer_cache_free_data_and_desc;
	}

	/* Set current */
	desc->zicio_current = current;

	/* Do initialization for ghost table */
	zicio_ghost_init(desc, user_base_address);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk("[Kernel Message] zicio buffer cache alloc finishes\n");
#endif
	return desc;

l_zicio_allocate_buffer_cache_free_data_and_desc:
	printk("[Kernel Message] zicio alloc fail\n");
	__zicio_free_data_buffer_pages(&desc->buffers, num_pages);
	__zicio_free_buffer_chunk(desc);

l_zicio_allocate_buffer_cache_free_desc:
	kmem_cache_free(zicio_descriptor_cache, desc);
l_zicio_allocate_buffer_cache_out:
	return NULL;
}

/*
 * zicio_unmap_dma_buffer_for_spcb
 *
 * Unmap DMA buffer for one spcb
 *
 * @zicio_shared_pool: zicio shared pool descriptor
 * @zicio_spcb: shared page control block pointer
 * @dev_idx: device idx
 */
void
zicio_unmap_dma_buffer_for_spcb(zicio_shared_pool *zicio_shared_pool,
		zicio_shared_page_control_block *zicio_spcb, int dev_idx)
{
	/* Get device information for DMA unmapping */	
	zicio_device *zicio_device =
			zicio_get_zicio_device_with_shared_pool(zicio_shared_pool, dev_idx);
	zicio_command_creator *cmd_creator =
			zicio_get_command_creator(zicio_device);

	/* Unmap dma buffer for target device */
	cmd_creator->zicio_unmap_dma_buffer_shared(zicio_shared_pool, zicio_spcb,
			dev_idx);
}

/*
 * zicio_destroy_dev_map_for_spcb
 *
 * Destroy device map for one spcb
 */
int
zicio_destroy_dev_map_for_spcb(zicio_shared_pool *zicio_shared_pool,
		zicio_dev_map *dev_maps, int dev_idx, int start_idx)
{
	zicio_device *zicio_device =
			zicio_get_zicio_device_with_shared_pool(zicio_shared_pool, dev_idx);
	int idx, nr_maps = zicio_get_num_inner_device(zicio_device);

	for (idx = 0 ; idx < nr_maps ; idx++) {
		free_page((unsigned long)dev_maps[idx].prp_data_address);
		kfree(dev_maps[idx].prp_data_array);

		if (nr_maps != 1) {
			free_pages((unsigned long)dev_maps[idx].sgl_data_address, 1);
			kfree(dev_maps[idx].sgl_data_array);
		}
	}

	return start_idx + nr_maps;
}

/*
 * zicio_allocate_dev_map_for_spcb
 *
 * Allocate device map for spcb
 */
int 
zicio_allocate_dev_map_for_spcb(zicio_shared_pool *zicio_shared_pool,
		zicio_dev_map *dev_maps, int dev_idx, int start_idx)
{
	zicio_device *zicio_device =
			zicio_get_zicio_device_with_shared_pool(zicio_shared_pool, dev_idx);
	int idx, nr_maps = zicio_get_num_inner_device(zicio_device);

	for (idx = 0 ; idx < nr_maps ; idx++) {
		dev_maps[idx].prp_data_address = page_to_virt(alloc_page(GFP_KERNEL));
		dev_maps[idx].prp_data_array = kmalloc(sizeof(dma_addr_t *),
			GFP_KERNEL);
		if (nr_maps != 1) {
			dev_maps[idx].sgl_data_address = page_to_virt(
					alloc_pages(GFP_KERNEL, 1));
			dev_maps[idx].sgl_data_array = kmalloc(sizeof(dma_addr_t *) << 1,
					GFP_KERNEL);
		}
	}

	return start_idx + nr_maps;
}

static int
zicio_get_total_inner_dev_map(zicio_dev_maps *dev_maps)
{
	int dev_id, sum = 0;

	for (dev_id = 0 ; dev_id < dev_maps->nr_dev ; dev_id++) {
		sum += dev_maps->dev_node_array[dev_id].nr_maps;
	}

	return sum;
}

/*
 * zicio_get_dev_map_for_spcb
 *
 * Get device map array pointer from slab allocator
 */
static zicio_dev_map *
zicio_get_dev_map_for_spcb(zicio_shared_pool *zicio_shared_pool)
{
	return (zicio_dev_map *)kmem_cache_alloc(
			zicio_shared_pool->shared_dev_maps.dev_maps_cache, GFP_KERNEL);
}

/*
 * zicio_free_dev_map_for_spcb
 *
 * Free dev map array area for spcb
 */
static void
zicio_free_dev_map_for_spcb(zicio_shared_pool *zicio_shared_pool,
		zicio_dev_map *dev_map)
{
	kmem_cache_free(zicio_shared_pool->shared_dev_maps.dev_maps_cache, dev_map);
}

/*
 * zicio_destroy_spcb
 *
 * Destroy shared pool's spcbs
 *
 * @zicio_shared_pool: zicio shared pool descriptor 
 * @zicio_spcb: shared page control block to destroy
 */
void
zicio_destroy_spcb(zicio_shared_pool *zicio_shared_pool,
			zicio_shared_page_control_block *zicio_spcb)
{
	struct page *huge_page;
	int i, nr_dev = zicio_shared_pool->shared_dev_maps.dev_maps.nr_dev;
	int start_idx = 0;

	/*
	 * Get the huge page and free it.
	 */
	huge_page = virt_to_page(zicio_spcb->zicio_spcb.chunk_ptr);
	ClearPageReserved(huge_page);
	__free_pages(huge_page, compound_order(huge_page));

	/* Unmap dma buffer for spcb and remove its table. */
	for (i = 0 ; i < nr_dev ; i++) {
		zicio_unmap_dma_buffer_for_spcb(zicio_shared_pool, zicio_spcb, i);
		start_idx = zicio_destroy_dev_map_for_spcb(zicio_shared_pool,
				zicio_spcb->zicio_spcb.dev_map, i, start_idx);
	}

	/* Free device map for spcb */
	zicio_free_dev_map_for_spcb(zicio_shared_pool, zicio_spcb->zicio_spcb.dev_map);

	/* Free shared page control block */
	zicio_free_spcb(zicio_spcb);
}

/*
 * zicio_destroy_shared_pool_spcbs
 *
 * Destroy shared pool's spcbs
 *
 * @zicio_shared_pool: zicio shared pool descriptor 
 * @num_pages: the number of pages used by zicio shared pool
 */
void
zicio_destroy_shared_pool_spcbs(zicio_shared_pool *zicio_shared_pool,
			int num_pages)
{
	zicio_shared_page_control_block *zicio_spcb;
	int i;

	for (i = 0 ; i < num_pages ; i++) {
		if ((zicio_spcb = zicio_get_spcb_with_id_from_shared_pool(
				zicio_shared_pool, i))) {
			zicio_destroy_spcb(zicio_shared_pool, zicio_spcb);
		} else {
			BUG();
		}
	}

	/* Free a page for spcb array */
	free_page((unsigned long)zicio_shared_pool->zicio_spcb);
}

/*
 * zicio_create_spcb
 *
 * Create shared page control block and it's memory components
 */
zicio_shared_page_control_block *
zicio_create_spcb(zicio_shared_pool *zicio_shared_pool, int page_id)
{
	/* Allocate zicio spcb from slab allocator */
	zicio_shared_page_control_block *zicio_spcb = zicio_allocate_spcb();
	zicio_dev_map *dev_maps;
	struct page *huge_page;
	int i, nr_dev = zicio_shared_pool->shared_dev_maps.dev_maps.nr_dev;
	int start_idx = 0;

	if (!zicio_spcb) {
		return NULL;
	}

	/* Allocate huge page */
	if (!(huge_page = zicio_allocate_hugepage())) {
		zicio_free_spcb(zicio_spcb);
		return NULL;
	}

	/* Allocate device map array from slab allocator */
	if (!(dev_maps = zicio_get_dev_map_for_spcb(zicio_shared_pool))) {
		ClearPageReserved(huge_page);
		__free_pages(huge_page, compound_order(huge_page));
		zicio_free_spcb(zicio_spcb);
		return NULL;
	}

	/* Allocate per-device dma address table from slab allocator */
	for (i = 0 ; i < nr_dev ; i++) {
		start_idx = zicio_allocate_dev_map_for_spcb(zicio_shared_pool,
				dev_maps + start_idx, i, start_idx);
	}

	zicio_spcb->zicio_spcb.local_page_idx = page_id;
	zicio_spcb->zicio_spcb.chunk_ptr = page_to_virt(huge_page);
	zicio_spcb->zicio_spcb.dev_map = dev_maps;

	/* DMA mapping per device */
	for (i = 0 ; i < nr_dev ; i++) {
		zicio_map_dma_buffer_shared(zicio_shared_pool, zicio_spcb, i);
	}

	return zicio_spcb;
}

/*
 * zicio_allocate_shared_buffer_cache
 *
 * Allocate data buffer for shared pool
 */
int
zicio_allocate_shared_buffer_cache(zicio_shared_pool *zicio_shared_pool)
{
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	struct mempolicy *pol = get_task_policy(current);
	unsigned short original_mempolicy_mode = pol->mode;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	/*
	 * Allocate data buffers for shared pool
	 */
	zicio_shared_page_control_block *zicio_spcb;
	unsigned page_id;

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	/* Allocate memories across multiple NUMA nodes to reduce memory pressure */
	pol->mode = MPOL_INTERLEAVE;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	atomic_set(&zicio_shared_pool->num_spcb, ZICIO_NUM_INIT_SPCB);

	/* Allocate shared page control block and set it to page array */
	for (page_id = 0 ; page_id < ZICIO_NUM_INIT_SPCB ; page_id++) {
		zicio_spcb = zicio_create_spcb(zicio_shared_pool, page_id);

		if (!zicio_spcb) {
			zicio_destroy_shared_pool_spcbs(zicio_shared_pool, page_id);

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
			/* Switch the memory policy to the original */
			pol->mode = original_mempolicy_mode;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

			return -ENOMEM;
		}
		atomic64_set(zicio_shared_pool->zicio_spcb + page_id, (unsigned long)zicio_spcb);
	}

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	/* Switch the memory policy to the original */
	pol->mode = original_mempolicy_mode;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	
	return 0;
}

/*
 * zicio_allocate_dev_map_for_md
 *
 * Allocate mapping array for md device
 */
void zicio_allocate_dev_map_for_md(zicio_descriptor *desc,
			zicio_device *zicio_device, struct device *device,
			zicio_dev_map_node *zicio_dev_map_node, int num_dev,
			void *shared_pool)
{
	int idx, num_pages, order;

	zicio_dev_map_node->nr_maps = num_dev;
	zicio_dev_map_node->zicio_devs = zicio_device;
	zicio_dev_map_node->devs = device;
	zicio_dev_map_node->zicio_inner_devs =
			zicio_alloc_inner_zicio_device_array(zicio_device);
	zicio_dev_map_node->inner_devs =
			zicio_alloc_inner_device_array(zicio_device, device);

	/* Currently, the number of channel huge page is different single channel
	 * from shared channel. By consider this, set the number of pages and
	 * the order of pages */
	if (!shared_pool) {
		num_pages = (desc->zicio_shared_pool_desc) ?
				ZICIO_LOCAL_DATABUFFER_CHUNK_NUM :
				ZICIO_DATABUFFER_CHUNK_NUM;
		order = (desc->zicio_shared_pool_desc) ?
				ilog2(ZICIO_LOCAL_DATABUFFER_CHUNK_NUM) :
				ilog2(ZICIO_DATABUFFER_CHUNK_NUM);
		zicio_dev_map_node->dev_map = kmalloc(sizeof(zicio_dev_map) * num_dev,
					GFP_KERNEL|__GFP_ZERO);
		for (idx = 0 ; idx < num_dev ; idx++) {
			zicio_dev_map_node->dev_map[idx].prp_data_address = page_to_virt(
					alloc_pages(GFP_KERNEL, order));
			zicio_dev_map_node->dev_map[idx].prp_inode_address = page_to_virt(
					alloc_pages(GFP_KERNEL, ZICIO_INODE_PRP_PAGE_ORDER));
			zicio_dev_map_node->dev_map[idx].sgl_data_address = page_to_virt(
					alloc_pages(GFP_KERNEL, order + 1));
			zicio_dev_map_node->dev_map[idx].prp_data_array = (dma_addr_t *)
					kmalloc(sizeof(dma_addr_t *) * num_pages,
									GFP_KERNEL|__GFP_ZERO);
			zicio_dev_map_node->dev_map[idx].sgl_data_array = (dma_addr_t *)
					kmalloc(sizeof(dma_addr_t *) * num_pages << 1,
									GFP_KERNEL|__GFP_ZERO);
		}
	} else {
		zicio_dev_map_node->dev_map = NULL;
	}
}

/*
 * zicio_allocate_dev_map_for_nvme
 *
 * Allocate mapping array for NVMe device
 */
void zicio_allocate_dev_map_for_nvme(zicio_descriptor *desc,
			zicio_device *zicio_device, struct device *device,
			zicio_dev_map_node *zicio_dev_map_node, int num_dev,
			void *shared_pool)
{
	int idx, num_pages, order;

	zicio_dev_map_node->nr_maps = num_dev;
	zicio_dev_map_node->zicio_devs = zicio_device;
	zicio_dev_map_node->zicio_inner_devs =
		zicio_alloc_inner_zicio_device_array(zicio_device);
	zicio_dev_map_node->devs = device;
	zicio_dev_map_node->inner_devs =
		zicio_alloc_inner_device_array(zicio_device, device);

	/* Currently, the number of channel huge page is different single channel
	 * from shared channel. By consider this, set the number of pages and
	 * the order of pages */
	if (!shared_pool) {
		num_pages = (desc->zicio_shared_pool_desc) ?
				ZICIO_LOCAL_DATABUFFER_CHUNK_NUM :
				ZICIO_DATABUFFER_CHUNK_NUM;
		order = (desc->zicio_shared_pool_desc) ?
				ilog2(ZICIO_LOCAL_DATABUFFER_CHUNK_NUM) :
				ilog2(ZICIO_DATABUFFER_CHUNK_NUM);
		zicio_dev_map_node->dev_map = kmalloc(sizeof(zicio_dev_map) * num_dev,
				GFP_KERNEL|__GFP_ZERO);
		for (idx = 0 ; idx < num_dev ; idx++) {
			zicio_dev_map_node->dev_map[idx].prp_data_address = page_to_virt(
					alloc_pages(GFP_KERNEL, order));
			zicio_dev_map_node->dev_map[idx].prp_inode_address = page_to_virt(
					alloc_pages(GFP_KERNEL, ZICIO_INODE_PRP_PAGE_ORDER));
			zicio_dev_map_node->dev_map[idx].prp_data_array = (dma_addr_t *)
					kmalloc(sizeof(dma_addr_t *) * num_pages,
							GFP_KERNEL|__GFP_ZERO);
		}
	} else {
		zicio_dev_map_node->dev_map = NULL;
	}
}

/*
 * zicio_allocate_dev_map_node
 *
 * Call per-devivce device mapping allocation function
 */
static zicio_dev_map_node *
zicio_allocate_dev_map_node(zicio_descriptor *desc, 
		zicio_device **zicio_devs, struct device **devs, int nr_dev,
		zicio_shared_pool *zicio_shared_pool)
{
	int idx;
	zicio_dev_map_node *dev_node_array;
	zicio_command_creator *cmd_creator;

	dev_node_array = kmalloc(sizeof(zicio_dev_map_node) * nr_dev,
				GFP_KERNEL|__GFP_ZERO);

	for (idx = 0 ; idx < nr_dev ; idx++) {
		/* Get command creator per-device */
		cmd_creator = zicio_get_command_creator(zicio_devs[idx]);
		/* Allocate device mapping array */
		cmd_creator->zicio_allocate_dev_map(desc, zicio_devs[idx], devs[idx],
				&dev_node_array[idx],
				zicio_get_num_inner_device(zicio_devs[idx]), zicio_shared_pool);
	}

	return dev_node_array;
}

/*
 * __zicio_set_inner_dev_maps
 *
 * set inner device map for multi device
 */
static int
__zicio_set_inner_dev_maps(zicio_dev_maps *dev_maps, int dev_idx,
			int inner_dev_idx)
{
	int nr_inner_devs = dev_maps->dev_node_array[dev_idx].nr_maps, i;

	for (i = inner_dev_idx ; i < inner_dev_idx + nr_inner_devs ; i++) {
		dev_maps->zicio_inner_dev_maps[i].raw_dev_idx_in_channel = -1;
		dev_maps->zicio_inner_dev_maps[i].mddev_idx = dev_idx;
		dev_maps->zicio_inner_dev_maps[i].zicio_inner_dev =
			dev_maps->dev_node_array[dev_idx].zicio_inner_devs[i - inner_dev_idx];
	}

	dev_maps->dev_node_array[dev_idx].start_point_inner_dev_map = inner_dev_idx;
	return i;
}

/*
 * __zicio_set_raw_dev_idx
 *
 * Set raw device idx in channel
 */
static int
__zicio_set_raw_dev_idx(zicio_dev_maps *dev_maps,
		zicio_device *zicio_device, int raw_dev_idx)
{
	int idx;

	for (idx = 0 ; idx < dev_maps->nr_dev ; idx++) {
		if (dev_maps->dev_node_array[idx].zicio_devs == zicio_device) {
			return dev_maps->dev_node_array[idx].raw_dev_idx_in_channel;
		}
	}

	for (idx = 0 ; idx < dev_maps->nr_md_inner_dev ; idx++) {
		if (dev_maps->zicio_inner_dev_maps[idx].zicio_inner_dev == zicio_device &&
			dev_maps->zicio_inner_dev_maps[idx].raw_dev_idx_in_channel != -1) {
			return dev_maps->dev_node_array[idx].raw_dev_idx_in_channel;
		}
	}

	return raw_dev_idx + 1;
}

/*
 * zicio_set_raw_dev_idx
 *
 * Set global device index in channel
 */
static int
zicio_set_raw_dev_idx(zicio_dev_maps *dev_maps, int raw_dev_idx)
{
	zicio_device *zicio_device;
	int md_inner_dev_num = dev_maps->nr_md_inner_dev;
	int idx, current_raw_dev_idx;

	for (idx = 0 ; idx < md_inner_dev_num ; idx++) {
		zicio_device = dev_maps->zicio_inner_dev_maps[idx].zicio_inner_dev;
		current_raw_dev_idx =
				__zicio_set_raw_dev_idx(dev_maps, zicio_device, raw_dev_idx);
		dev_maps->zicio_inner_dev_maps[idx].raw_dev_idx_in_channel =
				current_raw_dev_idx;
		if (raw_dev_idx < current_raw_dev_idx) {
			raw_dev_idx = current_raw_dev_idx;
		}
	}

	return raw_dev_idx;
}

/*
 * zicio_set_inner_dev_maps
 *
 * Set inner device maps for channel
 */
void
zicio_set_inner_dev_maps(zicio_descriptor *desc)
{
	zicio_dev_map_node *dev_map_node;
	zicio_device *zicio_device;
	int num_all_inner_devs = 0, idx, inner_dev_idx = 0, raw_dev_idx = 0;

	for (idx = 0 ; idx < desc->dev_maps.nr_dev ; idx++) {
		dev_map_node = &desc->dev_maps.dev_node_array[idx];
		zicio_device = zicio_get_zicio_device_with_desc(desc, idx);
		if (zicio_device->device_type == ZICIO_MD) {
			num_all_inner_devs += zicio_get_num_inner_device(zicio_device);
			dev_map_node->raw_dev_idx_in_channel = -1;
		} else {
			dev_map_node->raw_dev_idx_in_channel = raw_dev_idx++;
		}
	}

	if (!num_all_inner_devs) {
		desc->dev_maps.nr_raw_dev = raw_dev_idx;
		return;
	}

	desc->dev_maps.nr_md_inner_dev = num_all_inner_devs;
	desc->dev_maps.zicio_inner_dev_maps = kmalloc(sizeof(zicio_inner_dev_map) *
			num_all_inner_devs, GFP_KERNEL|__GFP_ZERO);

	for (idx = 0 ; idx < desc->dev_maps.nr_dev ; idx++) {
		zicio_device = zicio_get_zicio_device_with_desc(desc, idx);
		if (zicio_device->device_type == ZICIO_MD) {
			inner_dev_idx += __zicio_set_inner_dev_maps(
					&desc->dev_maps, idx, inner_dev_idx);
		}
	}

	desc->dev_maps.nr_raw_dev = zicio_set_raw_dev_idx(&desc->dev_maps,
			raw_dev_idx - 1) + 1;
}

/*
 * zicio_set_inner_dev_maps_shared
 *
 * Set inner device maps for shared pool
 *
 * @zicio_shared_pool: zicio shared pool singleton
 */
void
zicio_set_inner_dev_maps_shared(zicio_shared_pool *zicio_shared_pool)
{
	zicio_dev_map_node *dev_node_array;
	zicio_device *zicio_device;
	int num_all_inner_devs = 0, idx, inner_dev_idx = 0, raw_dev_idx = 0;

	for (idx = 0 ;
			idx < zicio_shared_pool->shared_dev_maps.dev_maps.nr_dev ; idx++) {
		dev_node_array =
				zicio_shared_pool->shared_dev_maps.dev_maps.dev_node_array + idx;
		zicio_device = zicio_get_zicio_device_with_shared_pool(
				zicio_shared_pool, idx);
		if (zicio_device->device_type == ZICIO_MD) {
			num_all_inner_devs += zicio_get_num_inner_device(zicio_device);
			dev_node_array->raw_dev_idx_in_channel = -1;
		} else {
			dev_node_array->raw_dev_idx_in_channel = raw_dev_idx++;
		}
	}

	if (!num_all_inner_devs) {
		zicio_shared_pool->shared_dev_maps.dev_maps.nr_raw_dev = raw_dev_idx;
		return;
	}

	zicio_shared_pool->shared_dev_maps.dev_maps.nr_md_inner_dev =
			num_all_inner_devs;
	zicio_shared_pool->shared_dev_maps.dev_maps.zicio_inner_dev_maps = kmalloc(
			sizeof(zicio_inner_dev_map) * num_all_inner_devs,
					GFP_KERNEL|__GFP_ZERO);

	for (idx = 0 ;
			idx < zicio_shared_pool->shared_dev_maps.dev_maps.nr_dev ; idx++) {
		zicio_device = zicio_get_zicio_device_with_shared_pool(
				zicio_shared_pool, idx);
		if (zicio_device->device_type == ZICIO_MD) {
			inner_dev_idx += __zicio_set_inner_dev_maps(
					&zicio_shared_pool->shared_dev_maps.dev_maps, idx,
							inner_dev_idx);
		}
	}

	zicio_shared_pool->shared_dev_maps.dev_maps.nr_raw_dev =
			zicio_set_raw_dev_idx(&zicio_shared_pool->shared_dev_maps.dev_maps,
					raw_dev_idx - 1) + 1;
}

/*
 * zicio_allocate_dev_map
 *
 * @desc : zicio_descriptor
 * @devs : device information to be registered
 * @nr_dev : number of devices to be registered
 *
 * return : previous number of cached devices
 *
 * Allocate dev map array. This function just MUST append device information to
 * array. The file already allocated are refered by others using its index.
 */
int
zicio_allocate_dev_map(zicio_descriptor *desc, struct device **devs,
			zicio_device **zicio_devs, int nr_dev)
{
	/* Check if we have additional devices to allocate */
	if (!nr_dev) {
		return 0;
	}

	desc->dev_maps.devs = devs;
	desc->dev_maps.nr_dev = nr_dev;
	desc->dev_maps.dev_node_array = zicio_allocate_dev_map_node(desc,
			zicio_devs, devs, nr_dev, NULL);
	zicio_set_inner_dev_maps(desc);

	return nr_dev;
}

int
zicio_allocate_dev_map_shared(zicio_shared_pool *zicio_shared_pool,
			struct device **devs, zicio_device **zicio_devs, int nr_dev,
			zicio_shared_pool_key_t shared_pool_key)
{
	/* Check if we have additional devices to allocate */
	char slab_cache_name[256];
	int dev_map_size, idx;
	int total_num_dev_map;
	int start = 0;

	if (!nr_dev) {
		return 0;
	}

	zicio_shared_pool->shared_dev_maps.dev_maps.devs = devs;
	zicio_shared_pool->shared_dev_maps.dev_maps.nr_dev = nr_dev;
	zicio_shared_pool->shared_dev_maps.dev_maps.dev_node_array =
			zicio_allocate_dev_map_node(NULL, zicio_devs, devs, nr_dev,
					zicio_shared_pool);
	zicio_set_inner_dev_maps_shared(zicio_shared_pool);

	sprintf(slab_cache_name, "shared_pool%u", shared_pool_key);
	total_num_dev_map = zicio_get_total_inner_dev_map(
			&zicio_shared_pool->shared_dev_maps.dev_maps);

	dev_map_size = sizeof(zicio_dev_map) * total_num_dev_map;
	dev_map_size = ((dev_map_size + 32) >> 6) << 6;
	zicio_shared_pool->shared_dev_maps.dev_maps_cache = kmem_cache_create(
			slab_cache_name, dev_map_size, dev_map_size, SLAB_PANIC, NULL);

	zicio_shared_pool->shared_dev_maps.dev_map_start_point = kmalloc(
			sizeof(int) * nr_dev, GFP_KERNEL);
	for (idx = 0 ; idx < nr_dev ; idx++) {
		zicio_shared_pool->shared_dev_maps.dev_map_start_point[idx] = start;
		start += zicio_shared_pool->
				shared_dev_maps.dev_maps.dev_node_array[idx].nr_maps;
	}

	return nr_dev;
}

/*
 * Allocate and initialize zicio
 *
 * @fs : struct fd array to register
 * @nr_fd : number of file descriptor
 */
zicio_descriptor *
zicio_allocate_and_initialize_mem(struct device **devs,
		zicio_device **zicio_devs, struct zicio_args *zicio_args)
{
	zicio_descriptor *desc;
	int ret, num_dev, nr_fd;

	nr_fd = (zicio_args->nr_local_fd) ? zicio_args->nr_local_fd :
			zicio_args->nr_shareable_fd;

	num_dev = zicio_distinct_nr_dev(devs, nr_fd);

	/*
	 * Allocate data buffer and create zicio descriptor
	 */
	if (!(desc = zicio_allocate_buffer_cache(zicio_args->user_base_address,
			zicio_is_shared_channel(zicio_args)))) {
		printk(KERN_WARNING "[Kernel Message] Buffer cache alloc error\n");

		ret = -ENOMEM;
		goto l_zicio_allocate_and_initialize_mem_free_devs_out;
	}

	/*
	 * Set operation flags
	 */
	desc->channel_operation_flags = zicio_args->zicio_flag;

	/*
	 * Allocate and initilize the information of devices which will be used
	 */
	if ((ret = zicio_allocate_dev_map(desc, devs, zicio_devs, num_dev)) < 0) {
		goto l_zicio_allocate_and_initialize_mem_free_desc_out;
	}

	/*
	 * Allocate and initialize switchboard
	 */ 
	desc->switch_board = page_to_virt(alloc_pages(GFP_KERNEL|__GFP_ZERO,
				ZICIO_SWITCHBOARD_ORDER));

	if (unlikely(!desc->switch_board)) {
		ret = -ENOMEM;
		goto l_zicio_allocate_and_initialize_mem_free_dev_map_out;
	}

    atomic_set((atomic_t *)&desc->switch_board->user_buffer_idx, INT_MAX);

#ifdef CONFIG_ZICIO_STAT
	/*
	 * Allocate stat board
	 */
	desc->stat_board = page_to_virt(alloc_pages(GFP_KERNEL|__GFP_ZERO,
									ZICIO_STATBOARD_ORDER));

	if (unlikely(!desc->stat_board)) {
		ret = -ENOMEM;
		goto l_zicio_allocate_and_initialize_mem_free_dev_map_out;
	}
#endif /* CONFIG_ZICIO_STAT */

	return desc;

l_zicio_allocate_and_initialize_mem_free_dev_map_out:
	zicio_free_device_map(&desc->dev_maps, (bool)desc->zicio_shared_pool_desc);

	if (desc->switch_board)
		kfree(desc->switch_board);

#ifdef CONFIG_ZICIO_STAT
	if (desc->stat_board) {
		kfree(desc->stat_board);
	}
#endif /* CONFIG_ZICIO_STAT */

l_zicio_allocate_and_initialize_mem_free_desc_out:
	kfree(desc);

l_zicio_allocate_and_initialize_mem_free_devs_out:
	kfree(devs);

	return ERR_PTR(ret);
}

/*
 * __zicio_get_unmapped_area
 *
 * Real worker to get unmapped area for mmap
 */
static unsigned long
__zicio_get_unmapped_area(const unsigned long addr0,
			const unsigned long len, const unsigned long flag)
{
	struct vm_area_struct *vma, *prev;
	struct mm_struct *mm = current->mm;
	unsigned long addr = addr0;
	unsigned long start_addr = addr;
	struct rb_node **rb_link, *rb_parent;

	/* Requested length too big for entire address space */
	if (len > TASK_SIZE) {
		printk(KERN_WARNING "[Kernel Message] Request length too big\n");
		return -ENOMEM;
	}

	while(1) {
		/* Requesting a specific address */
		addr &= ZICIO_PAGE_MASK;
		if (mmap_address_hint_valid(addr, len) &&
			!zicio_check_vm_mapped(mm, addr, len, &prev, &rb_link,
						&rb_parent)) {
			/* If current area isn't mapped, then use this address */
			vma = find_vma(mm, addr);
			if (!vma || addr + len <= vm_start_gap(vma)) {
				return addr;
			}
		}
		/* Current area was already mapped. */
		addr += len;
		if (addr == start_addr) {
			printk(KERN_WARNING "[Kernel Message] Cannot get unmapped area\n");
			return -ENOMEM;
		}
	}
}

/*
 * __zicio_mmap_buffers
 *
 * Real worker to mmap buffers 
 */
static unsigned long 
__zicio_mmap_buffers(void ** buffer_vm, unsigned long mem_base, 
				unsigned mem_size, unsigned chunk_size, int stflg,
				unsigned long id)
{
	struct mm_struct *mm = current->mm;
	unsigned long prot = PROT_READ | PROT_WRITE;
	unsigned long start = ZICIO_PAGE_ROUNDUP(mem_base);
	unsigned long size = ZICIO_PAGE_ROUNDUP(mem_size);
	unsigned long flags = MAP_SHARED | MAP_NORESERVE;

	unsigned long addr, uaddr;
	struct vm_area_struct *vma;
	vm_flags_t vm_flags;
	unsigned long err;	
	unsigned alloced_size, num_alloc;

	if (!size) {
		printk(KERN_WARNING "[Kernel Message] Cannot get size\n");
		return -ENOMEM;
	}

	/* For mmap, we have to achieve exclusive lock to mm structure */
	if (mmap_write_lock_killable(mm)) {
		printk(KERN_WARNING "[Kernel Message] Cannot get exclusive lock mm \n");
		err = -EINTR;
		goto l___zicio_mmap_buffers_err;
	}

	/* Too many mmapings? */
	if (mm->map_count > sysctl_max_map_count) {
		printk(KERN_WARNING "[Kernel Message] Too many mappings\n");
		err = -ENOMEM;
		goto l___zicio_mmap_buffers_err;
	}

	/* Checking if the current area has a valid start address and size */
	if (start + size <= start) {
		printk(KERN_WARNING "[Kernel Message] Current area doesn't have start "
							"address and size\n");
		err = -EINVAL;
		goto l___zicio_mmap_buffers_err;
	}

	/* Get the start address of unmapped area */
	addr = __zicio_get_unmapped_area(start + size, size, flags);

	if (IS_ERR_VALUE(addr)) {
		printk(KERN_WARNING "[Kernel Message] invalid address\n");
		err = -EEXIST;
		goto l___zicio_mmap_buffers_err;
	}

	num_alloc = mem_size / chunk_size;
	/* Allocate memory for chunk */
	for (alloced_size = 0, uaddr = addr ; alloced_size < mem_size ; 
			alloced_size += chunk_size, uaddr += chunk_size, buffer_vm++) {
		/* find VMA struct */
		vma = find_vma(mm, uaddr);

		if (vma && vma->vm_start < uaddr + chunk_size) {
			printk(KERN_WARNING "[Kernel Message] Cannot find VMA for buffers\n");
			err = -EEXIST;
			goto l___zicio_mmap_buffers_map_err;
		}

		/* Get the flag of vm */
		vm_flags = calc_vm_prot_bits(prot, 0) | calc_vm_flag_bits(flags) |
					mm->def_flags | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;

		if (flags & MAP_LOCKED) {
			if (!can_do_mlock()) {
				printk(KERN_WARNING "[Kernel Message] cannot acquire map lock\n");
				err = -EPERM;
				goto l___zicio_mmap_buffers_map_err;
			}
		}

		if (vm_flags & (VM_GROWSDOWN|VM_GROWSUP)) {
			printk(KERN_WARNING "[Kernel Message] invalid vm flags VM_GROWSDOWN\n");
			err = -EINVAL;
			goto l___zicio_mmap_buffers_map_err;
		}

		vm_flags |= VM_SHARED | VM_MAYSHARE;

		if (sysctl_overcommit_memory != OVERCOMMIT_NEVER) {
			vm_flags |= VM_NORESERVE;
		}
	
		/* Attach real memory area to allocated vm */
		uaddr = zicio_mmap_region_buffers(*buffer_vm, uaddr, chunk_size,
						vm_flags, NULL, 0);

		if (IS_ERR_VALUE(uaddr)) {
			printk(KERN_WARNING "[Kernel Message] error in mmap region buffers\n");
		}
	}

	mmap_write_unlock(mm);
	return addr;

l___zicio_mmap_buffers_map_err:
l___zicio_mmap_buffers_err:
	mmap_write_unlock(mm);
	return err;
}

/*
 * __zicio_map_dma_prp_buffer_for_nvme
 *
 * Create DMA mapping array for nvme 
 * For the memory area received as a parameter, dma mapping information is
 * recorded in units of 4KiB pages.
 */
static int
__zicio_map_dma_prp_buffer_for_nvme(dma_addr_t *dma_addrs,
			struct device *dev, void **cpu_addrs, unsigned long buffer_size,
			unsigned chunk_size, void **prp_pages)
{
	struct page *_page;
	dma_addr_t *prp_page = (dma_addr_t *)(*prp_pages);
	unsigned alloced, i, j;

	for (i = 0, alloced = 0 ; alloced < buffer_size ;
			i++, alloced += chunk_size) {

		_page = virt_to_page((unsigned long)cpu_addrs[i]);

		prp_page[0] = dma_map_page_attrs(dev, _page, 0,
					(1UL) << 21, DMA_FROM_DEVICE, 0);

		for (j = 1 ; j < ZICIO_NUM_PRP_ENTRIES_IN_PAGE ; j++) {
			prp_page[j] = prp_page[0] + (j << ZICIO_PAGE_SHIFT);
		}

		dma_addrs[i] = dma_map_page_attrs(dev, virt_to_page(prp_page), 0,
			ZICIO_PAGE_SIZE, DMA_FROM_DEVICE, 0);
		prp_page = (dma_addr_t *)((unsigned long)prp_page +
					ZICIO_PAGE_SIZE);
	}

	return 0;
}

/*
 * __zicio_map_dma_prp_buffer_for_md
 *
 * Create DMA mapping array for md
 * We already set DMA mapping address, when creating SGL array. So, just read
 * data from SGL array
 */
static int
__zicio_map_dma_prp_buffer_for_md(void **prp_pages, void **sgl_pages,
			dma_addr_t *prp_dma_addrs, struct device *dev, int num_pages)
{
	int i, j;
	struct nvme_sgl_desc *sgl_desc;
	dma_addr_t *prp_array;

	/*
	 * We move address sgl_desc to prp_array. Set the first point
	 */
	sgl_desc = (struct nvme_sgl_desc *)(*sgl_pages);
	prp_array = (dma_addr_t *)(*prp_pages);

	/*
	 * Now, move dma address from sgl descriptor array to prp address array.
	 */
	for (i = 0 ; i < num_pages ; i++) {
		for (j = 0 ; j < ZICIO_NUM_SGL_ENTRIES_IN_PAGE ; j++) {
			prp_array[j] = sgl_desc[j].addr;
		}
		sgl_desc += ZICIO_NUM_SGL_ENTRIES_IN_PAGE;

		for ( ; j < ZICIO_NUM_PRP_ENTRIES_IN_PAGE ; j++) {
			prp_array[j] = sgl_desc[j - ZICIO_NUM_SGL_ENTRIES_IN_PAGE].addr;
		}
		sgl_desc += ZICIO_NUM_SGL_ENTRIES_IN_PAGE;
		prp_array += ZICIO_NUM_PRP_ENTRIES_IN_PAGE;
	}

	/*
	 * Now, we start DMA mapping prp address array.
	 */
	prp_array = (dma_addr_t *)(*prp_pages);

	for (i = 0 ; i < num_pages ; i++) {
		prp_dma_addrs[i] = dma_map_page_attrs(dev, virt_to_page(prp_array), 0,
				ZICIO_PAGE_SIZE, DMA_FROM_DEVICE, 0);
		prp_array += ZICIO_NUM_PRP_ENTRIES_IN_PAGE;
	}

	return 0;
}

/*
 * zicio_set_nvme_sgl_desc
 *
 * Set one sgl descriptor with @dma_addr, @length
 */
static void zicio_set_nvme_sgl_desc(struct nvme_sgl_desc *sge,
		dma_addr_t dma_addr, int length)
{
	sge->addr = cpu_to_le64(dma_addr);
	sge->length = cpu_to_le32(length << ZICIO_PAGE_SHIFT);
	/* This type of sge points data location */
	sge->type = NVME_SGL_FMT_DATA_DESC << 4;
}

/*
 * zicio_shift_sgl_desc
 *
 * Our target MD is 4KiB, striped MD device.
 * So, just shifting the previous mapping information as much as the page.
 */
static void
zicio_shift_sgl_desc(void **sgl_pages, int stride, int page_length,
			int num_pages, int num_entries_in_dma_slot)
{
	struct nvme_sgl_desc *first_sgl_desc = (struct nvme_sgl_desc *)(*sgl_pages);
	struct nvme_sgl_desc *src_sgl_desc, *dest_sgl_desc;
	int i, j;

	for (i = 1 ; i < stride ; i++) {
		src_sgl_desc = first_sgl_desc;
		dest_sgl_desc = first_sgl_desc + num_entries_in_dma_slot * i;
		for (j = 0 ; j < num_entries_in_dma_slot ; j++) {
			zicio_set_nvme_sgl_desc(dest_sgl_desc + j,
					le64_to_cpu(src_sgl_desc[j].addr) +
							(i << ZICIO_PAGE_SHIFT), page_length);
		}
	}
}

/*
 * __zicio_map_dma_sgl_buffer_for_md
 *
 * Create SGL descriptor array for MD.
 * Set DMA address and length of segments.
 */
static int
__zicio_map_dma_sgl_buffer_for_md(int nr_maps, void **sgl_pages,
		dma_addr_t *sgl_dma_addrs, struct device *dev, int dev_idx,
		void **cpu_addrs, long buffer_size, unsigned chunk_size,
		int chunk_page_length)
{
	unsigned alloced, i, j, sgl_base, sgl_idx, entries_per_page;
	unsigned long _page;
	int stride = nr_maps;
	int mapped_pages_in_page;
	int num_pages = buffer_size >> ZICIO_CHUNK_SHIFT;
	int num_entries_in_dma_slot;
	dma_addr_t data_dma_addr;
	struct nvme_sgl_desc *sgl_desc;

	/* Get the information for striped storage */
	stride *= chunk_page_length;
	entries_per_page = ZICIO_NUM_PRP_ENTRIES_IN_PAGE / stride;
	mapped_pages_in_page = ZICIO_NUM_SGL_ENTRIES_IN_PAGE / entries_per_page;
	num_entries_in_dma_slot = ZICIO_NUM_PRP_ENTRIES_IN_PAGE * num_pages;
	do_div(num_entries_in_dma_slot, stride);

	/*
	 * Set sgl array start point
	 */
	sgl_desc = (struct nvme_sgl_desc *)(*sgl_pages);

	/*
	 * Data to be read
	 * ----------------------------------------------------------
	 * | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | ...					|
	 * ----------------------------------------------------------
	 *
	 * Examples.
	 * There's two cases depending on the starting position of the data
	 * within the chunk if we use two devices.
	 *
	 * Devices
	 * Dev A					Dev B
	 * --------------------		--------------------
	 * | 0 | 2 | 4 | ...  |		| 1 | 3 | 5 | ...  |
	 * --------------------     --------------------
	 *
	 *
	 * Dev A					Dev B
	 * --------------------		--------------------
	 * | 1 | 3 | 5 | ...  |		| 0 | 2 | 4 | ...  |
	 * --------------------     --------------------
	 *
	 * A contiguous area on the device will be read into a memory segment.
	 * And we want to process it with one command.
	 * Since striping of 4KiB units will be used, only the information of the
	 * area to be read consecutively can be managed as a continuous array.
	 * 
	 * Both PRP and SGL set the array information containing the dma address to
	 * the NVMe command.
	 * And taking this into consideration, create an array containing the
	 * address to be delivered in advance and make the dma address of this array
	 * as well.
	 *
	 * DMA mapping array each device
	 * -------------------------------------------------------------
	 * | Mem(0)| Mem(2) | Mem(4)| ..... | Mem(1) | Mem(3) | Mem(5) |
	 * -------------------------------------------------------------
	 *
	 * Take the dma address indicating the data of the area to read and set it
	 * to the command.
	 */

	/*
	 * The code below initializes the dma address array above.
	 */

	/*
	 * Step 1. Create DMA mapping and set its address to array
	 */
	for (i = 0, alloced = 0 ; alloced < buffer_size ;
			i++, alloced += (chunk_size * mapped_pages_in_page)) {
		memset(sgl_desc, 0, ZICIO_PAGE_SIZE);
		for (j = 0 ; j < mapped_pages_in_page ; j++) {
			if (i * mapped_pages_in_page + j < num_pages) {
				_page = (unsigned long)virt_to_page(
						(unsigned long)cpu_addrs[i * mapped_pages_in_page + j]);
				data_dma_addr = dma_map_page_attrs(dev,
						(struct page *)_page, 0, (1UL) << ZICIO_CHUNK_SHIFT,
						DMA_FROM_DEVICE, 0);
				zicio_set_nvme_sgl_desc(sgl_desc + j * entries_per_page,
						data_dma_addr, chunk_page_length);
			}
		}
		sgl_desc = (struct nvme_sgl_desc *)((unsigned long)sgl_desc +
				ZICIO_PAGE_SIZE);
	}

	sgl_desc = (struct nvme_sgl_desc *)(*sgl_pages);

	for (i = 0, alloced = 0 ; alloced < buffer_size ;
			i++, alloced += (chunk_size * mapped_pages_in_page)) {
		for (j = 1 ; j < ZICIO_NUM_SGL_ENTRIES_IN_PAGE ; j++) {
			if ((sgl_idx =
					zicio_sector_div(sgl_base, j, entries_per_page))) {
				zicio_set_nvme_sgl_desc(sgl_desc + j, le64_to_cpu(
						sgl_desc[entries_per_page * sgl_base].addr) +
						((sgl_idx * stride) << ZICIO_PAGE_SHIFT),
						chunk_page_length);
			}
		}
		sgl_desc = (struct nvme_sgl_desc *)((unsigned long)sgl_desc +
				ZICIO_PAGE_SIZE);
	}

	/*
	 * Step 2. Shift DMA addresses in array.
	 */
	zicio_shift_sgl_desc(sgl_pages, stride, chunk_page_length, num_pages,
			num_entries_in_dma_slot);

	/*
	 * Step 3. Create DMA mapping for DMA address array.
	 */
	sgl_desc = (struct nvme_sgl_desc *)(*sgl_pages);

	for (i = 0 ; i < num_pages << 1 ; i++) {
		sgl_dma_addrs[i] = dma_map_page_attrs(dev, virt_to_page(sgl_desc), 0,
				ZICIO_PAGE_SIZE, DMA_FROM_DEVICE , 0);
		sgl_desc = (struct nvme_sgl_desc *)((unsigned long)sgl_desc +
				ZICIO_PAGE_SIZE);
	}

	return 0;
}

/*
 * __zicio_map_dma_buffer_shared_for_nvme
 *
 * Create DMA mapping array for nvme
 */
static void
__zicio_map_dma_buffer_shared_for_nvme(
		zicio_shared_pool *zicio_shared_pool,
		zicio_shared_page_control_block *zicio_spcb, int dev_idx)
{
	zicio_dev_map_node *dev_map_node;
	zicio_dev_map *dev_map;
	zicio_device *zicio_device;
	int start = zicio_get_dma_map_start_point_shared_with_pool(
			zicio_shared_pool, dev_idx);

	dev_map_node =
			&zicio_shared_pool->shared_dev_maps.dev_maps.dev_node_array[dev_idx];
	zicio_device = dev_map_node->zicio_devs;
	dev_map = zicio_spcb->zicio_spcb.dev_map + start;

	__zicio_map_dma_prp_buffer_for_nvme(dev_map->prp_data_array,
			zicio_device->device, &zicio_spcb->zicio_spcb.chunk_ptr,
			1 << ZICIO_CHUNK_SHIFT, ZICIO_CHUNK_SIZE,
			&dev_map->prp_data_address);
}

/*
 * __zicio_map_dma_buffer_for_nvme
 *
 * Create DMA mapping array for nvme
 */
static void
__zicio_map_dma_buffer_for_nvme(zicio_descriptor *desc, int dev_idx)
{
	zicio_device *zicio_device;
	zicio_dev_map_node *dev_map_node;
	int num_pages;

	dev_map_node = &desc->dev_maps.dev_node_array[dev_idx];
	zicio_device = dev_map_node->zicio_devs;
	num_pages = (desc->zicio_shared_pool_desc) ?
			ZICIO_LOCAL_DATABUFFER_CHUNK_NUM :
			ZICIO_DATABUFFER_CHUNK_NUM;

	BUG_ON(dev_map_node->dev_map->prp_data_array[0]);

	__zicio_map_dma_prp_buffer_for_nvme(
			dev_map_node->dev_map->prp_data_array, zicio_device->device,
			&(desc->buffers.data_buffer[0]), num_pages << ZICIO_CHUNK_SHIFT,
			ZICIO_CHUNK_SIZE, &dev_map_node->dev_map->prp_data_address);

	__zicio_map_dma_prp_buffer_for_nvme(
			dev_map_node->dev_map->prp_inode_array, zicio_device->device,
			&(desc->metadata_ctrl.inode_buffer), ZICIO_INODEBUFFER_SIZE,
			ZICIO_CHUNK_SIZE, &dev_map_node->dev_map->prp_inode_address);
}

/*
 * __zicio_map_dma_buffer_shared_for_md
 *
 * Create DMA mapping array for md
 */
void
__zicio_map_dma_buffer_shared_for_md(zicio_shared_pool *zicio_shared_pool,
		zicio_shared_page_control_block *zicio_spcb, int dev_idx)
{
	zicio_dev_map_node *dev_map_node;
	zicio_dev_map *dev_map;
	zicio_device **zicio_device;
	int i, start, stride;

	start = zicio_get_dma_map_start_point_shared_with_pool(
			zicio_shared_pool, dev_idx);
	dev_map_node =
			&zicio_shared_pool->shared_dev_maps.dev_maps.dev_node_array[dev_idx];
	dev_map = zicio_spcb->zicio_spcb.dev_map + start;
	zicio_device = dev_map_node->zicio_inner_devs;
	stride = dev_map_node->nr_maps;

	for (i = 0 ; i < dev_map_node->nr_maps ; i++) {
		__zicio_map_dma_sgl_buffer_for_md(dev_map_node->nr_maps,
				&(dev_map->sgl_data_address), dev_map->sgl_data_array,
				zicio_device[i]->device, dev_idx, &(zicio_spcb->zicio_spcb.chunk_ptr),
				1 << ZICIO_CHUNK_SHIFT, ZICIO_CHUNK_SIZE,
				zicio_get_mddev_stride_with_shared_pool(zicio_shared_pool,
					dev_idx));

		__zicio_map_dma_prp_buffer_for_md(&(dev_map->prp_data_address),
				&(dev_map->sgl_data_address), dev_map->prp_data_array,
				zicio_device[i]->device, 1);
		dev_map++;
	}
}

/*
 * __zicio_map_dma_buffer_for_md
 *
 * Create DMA mapping array for md
 */
void
__zicio_map_dma_buffer_for_md(zicio_descriptor *desc, int dev_idx)
{
	zicio_device **zicio_device;
	zicio_dev_map_node *dev_map_node;
	int i, stride, num_pages;

	dev_map_node = &desc->dev_maps.dev_node_array[dev_idx];
	zicio_device = dev_map_node->zicio_inner_devs;
	stride = dev_map_node->nr_maps;
	num_pages = (desc->zicio_shared_pool_desc) ?
			ZICIO_LOCAL_DATABUFFER_CHUNK_NUM :
			ZICIO_DATABUFFER_CHUNK_NUM;

	for (i = 0 ; i < dev_map_node->nr_maps; i++) {
		__zicio_map_dma_sgl_buffer_for_md(dev_map_node->nr_maps,
				&(dev_map_node->dev_map[i].sgl_data_address),
				dev_map_node->dev_map[i].sgl_data_array,
				zicio_device[i]->device, dev_idx, &(desc->buffers.data_buffer[0]),
				num_pages << ZICIO_CHUNK_SHIFT, ZICIO_CHUNK_SIZE,
				zicio_get_mddev_stride_with_desc(desc, dev_idx));

		__zicio_map_dma_prp_buffer_for_md(
				&(dev_map_node->dev_map[i].prp_data_address),
				&(dev_map_node->dev_map[i].sgl_data_address),
				dev_map_node->dev_map[i].prp_data_array,
				zicio_device[i]->device, num_pages);

		__zicio_map_dma_prp_buffer_for_nvme(
				dev_map_node->dev_map[i].prp_inode_array,
				zicio_device[i]->device, &(desc->metadata_ctrl.inode_buffer),
				ZICIO_INODEBUFFER_SIZE, ZICIO_CHUNK_SIZE,
				&dev_map_node->dev_map[i].prp_inode_address);
	}
}

void
zicio_map_dma_buffer_for_nvme(void *desc, int dev_idx)
{
	zicio_descriptor *zicio_desc = desc;
	__zicio_map_dma_buffer_for_nvme(zicio_desc, dev_idx);
}

void
zicio_map_dma_buffer_for_md(void *zicio_desc, int dev_idx)
{
	zicio_descriptor *desc = zicio_desc;
	__zicio_map_dma_buffer_for_md(desc, dev_idx);
}

void
zicio_map_dma_buffer_shared_for_nvme(void *shared_pool, void *spcb,
			int dev_idx)
{
	zicio_shared_pool *zicio_shared_pool = shared_pool;
	zicio_shared_page_control_block *zicio_spcb = spcb;

	__zicio_map_dma_buffer_shared_for_nvme(zicio_shared_pool, zicio_spcb,
			dev_idx);
}

void
zicio_map_dma_buffer_shared_for_md(void *shared_pool, void *spcb,
			int dev_idx)
{
	zicio_shared_pool *zicio_shared_pool = shared_pool;
	zicio_shared_page_control_block *zicio_spcb = spcb;

	__zicio_map_dma_buffer_shared_for_md(zicio_shared_pool, zicio_spcb, dev_idx);
}

/*
 * zicio_map_dma_buffer_shared
 *
 * Create DMA mapping array for shared pool
 */
void
zicio_map_dma_buffer_shared(zicio_shared_pool *zicio_shared_pool,
			zicio_shared_page_control_block *zicio_spcb, int dev_idx)
{
	zicio_device *zicio_device = zicio_get_zicio_device_with_shared_pool(
				zicio_shared_pool, dev_idx);
	zicio_command_creator *cmd_creator = zicio_get_command_creator(
				zicio_device);

	cmd_creator->zicio_map_dma_buffer_shared(
			zicio_shared_pool, zicio_spcb, dev_idx);
}

/*
 * zicio_map_dma_buffer
 *
 * Create DMA mapping array
 */
void
zicio_map_dma_buffer(zicio_descriptor *desc, int dev_idx)
{
	zicio_device *zicio_device = zicio_get_zicio_device_with_desc(
				desc, dev_idx);
	/* Get per-device function pointer */
	zicio_command_creator *cmd_creator = zicio_get_command_creator(
				zicio_device);

	/* Call DMA mapping function for each device */
	cmd_creator->zicio_map_dma_buffer(desc, dev_idx);
}

/*
 * __zicio_ummap_dma_buffer
 *
 * Remove DMA mapping.
 */
void
__zicio_unmap_dma_prp_buffer(zicio_dev_map_node *dev_map_node,
			int nr_maps, int dev_idx, int num_pages)
{
	dma_addr_t *prp_page;
	dma_addr_t ptr_prp_page;
	struct device *dev = dev_map_node->zicio_inner_devs[dev_idx]->device;
	int i, j;
	unsigned entries_per_page = ZICIO_NUM_PRP_ENTRIES_IN_PAGE / nr_maps;

	prp_page = (dma_addr_t *)(dev_map_node->dev_map[dev_idx].prp_data_address);
	for (i = 0 ; i <  num_pages / nr_maps ; i++) {
		for (j = 0 ; j < nr_maps ; j++) {
			dma_unmap_page_attrs(dev, prp_page[j * entries_per_page],
						ZICIO_DATABUFFER_CHUNK_SIZE, DMA_FROM_DEVICE, 0);
		}
		ptr_prp_page = dev_map_node->dev_map[dev_idx].prp_data_array[i];
		dma_unmap_page_attrs(dev, ptr_prp_page, ZICIO_PAGE_SIZE,
					DMA_FROM_DEVICE, 0);
		prp_page = (dma_addr_t *)((unsigned long)prp_page +
				ZICIO_PAGE_SIZE);
	}

	if (dev_map_node->dev_map[dev_idx].prp_inode_array[0]) {
		prp_page = (dma_addr_t *)(&dev_map_node->
				dev_map[dev_idx].prp_inode_address);
		ptr_prp_page = dev_map_node->dev_map[dev_idx].prp_inode_array[0];
		dma_unmap_page_attrs(dev, prp_page[0], ZICIO_INODEBUFFER_SIZE,
				DMA_FROM_DEVICE, 0);
		dma_unmap_page_attrs(dev, ptr_prp_page, ZICIO_PAGE_SIZE,
				DMA_FROM_DEVICE, 0);
	}
}

/*
 * __zicio_ummap_dma_buffer
 *
 * Remove DMA mapping.
 */
void
__zicio_unmap_dma_prp_buffer_shared(zicio_dev_map *dev_map,
			struct device *dev)
{
	dma_addr_t *prp_page;
	dma_addr_t ptr_prp_page;

	prp_page = (dma_addr_t *)dev_map->prp_data_address;
	dma_unmap_page_attrs(dev, prp_page[0], ZICIO_DATABUFFER_CHUNK_SIZE,
			DMA_FROM_DEVICE, 0);
	ptr_prp_page = dev_map->prp_data_array[0];
	dma_unmap_page_attrs(dev, ptr_prp_page, ZICIO_PAGE_SIZE,
			DMA_FROM_DEVICE, 0);
}

/*
 * __zicio_ummap_dma_buffer
 *
 * Remove DMA mapping.
 */
void
__zicio_unmap_dma_sgl_buffer(zicio_dev_map_node *dev_map_node,
			int dev_idx, int num_pages)
{
	int i;

	for (i = 0 ; i < num_pages << 1 ; i++) {
		dma_unmap_page_attrs(dev_map_node->zicio_inner_devs[dev_idx]->device,
				dev_map_node->dev_map[dev_idx].sgl_data_array[i],
						ZICIO_PAGE_SIZE, DMA_FROM_DEVICE, 0);
	}
}

/*
 * __zicio_ummap_dma_buffer
 *
 * Remove DMA mapping.
 */
void
__zicio_unmap_dma_sgl_buffer_shared(zicio_dev_map *dev_map,
			struct device *dev, int num_pages)
{
	int i;

	for (i = 0 ; i < num_pages << 1 ; i++) {
		dma_unmap_page_attrs(dev, dev_map->sgl_data_array[i],
				ZICIO_PAGE_SIZE, DMA_FROM_DEVICE, 0);
	}
}

/*
 * zicio_unmap_dma_buffer_for_md
 *
 * Unmap the dma buffer.
 */
void
zicio_unmap_dma_buffer_for_md(void *zicio_desc, int dev_idx)
{
	zicio_dev_map_node *dev_map_node;
	zicio_descriptor *desc = zicio_desc;
	int i, num_pages;

	num_pages = (desc->zicio_shared_pool_desc) ?
			ZICIO_LOCAL_DATABUFFER_CHUNK_NUM :
			ZICIO_DATABUFFER_CHUNK_NUM;
	dev_map_node = &desc->dev_maps.dev_node_array[dev_idx];

	for (i = 0 ; i < dev_map_node->nr_maps ; i++) {
		__zicio_unmap_dma_prp_buffer(dev_map_node,
				dev_map_node->nr_maps, i, num_pages);
		__zicio_unmap_dma_sgl_buffer(dev_map_node, i, num_pages);
	}
}

void
zicio_unmap_dma_buffer_shared_for_md(void *shared_pool, void *spcb,
			int dev_idx)
{
	zicio_shared_pool *zicio_shared_pool = shared_pool;
	zicio_shared_page_control_block *zicio_spcb = spcb;
	zicio_dev_map_node *dev_map_node;
	int i, start;

	dev_map_node =
			&zicio_shared_pool->shared_dev_maps.dev_maps.dev_node_array[dev_idx];
	start = zicio_get_dma_map_start_point_shared_with_pool(
			zicio_shared_pool, dev_idx);
	

	for (i = 0 ; i < dev_map_node->nr_maps ; i++) {
		__zicio_unmap_dma_prp_buffer_shared(
				zicio_spcb->zicio_spcb.dev_map + start + i,
				dev_map_node->zicio_devs->device);
		__zicio_unmap_dma_sgl_buffer_shared(
				zicio_spcb->zicio_spcb.dev_map + start + i,
				dev_map_node->zicio_devs->device, 1);

	}
}

/*
 * zicio_unmap_dma_buffer_for_nvme
 *
 * Unmap the dma buffer.
 */
void
zicio_unmap_dma_buffer_for_nvme(void *zicio_desc, int dev_idx)
{
	zicio_descriptor *desc = zicio_desc;
	zicio_dev_map_node *dev_map_node;
	int num_pages;

	num_pages = (desc->zicio_shared_pool_desc) ?
			ZICIO_LOCAL_DATABUFFER_CHUNK_NUM :
			ZICIO_DATABUFFER_CHUNK_NUM;
	dev_map_node = &desc->dev_maps.dev_node_array[dev_idx];
	__zicio_unmap_dma_prp_buffer(dev_map_node, 1, 0, num_pages);
}

void
zicio_unmap_dma_buffer_shared_for_nvme(void *shared_pool, void *spcb,
			int dev_idx)
{
	zicio_shared_pool *zicio_shared_pool = shared_pool;
	zicio_shared_page_control_block *zicio_spcb = spcb;
	zicio_dev_map_node *zicio_dev_map_node =
			&zicio_shared_pool->shared_dev_maps.dev_maps.dev_node_array[dev_idx];
	int start = zicio_get_dma_map_start_point_shared_with_pool(
			zicio_shared_pool, dev_idx);

	__zicio_unmap_dma_prp_buffer_shared(zicio_spcb->zicio_spcb.dev_map + start,
			zicio_dev_map_node->zicio_devs->device);
}

/*
 * zicio_unmap_dma_buffer
 *
 * Unmap the dma buffer.
 */
void
zicio_unmap_dma_buffer(zicio_descriptor *desc)
{
	zicio_device *zicio_device;
	zicio_dev_map_node *dev_map_node;
	zicio_command_creator *zicio_cmd_creator;
	unsigned nr_dev = desc->dev_maps.nr_dev;
	unsigned i;

	for (i = 0 ; i < nr_dev ; i++) {
		dev_map_node = &desc->dev_maps.dev_node_array[i];
		zicio_device = dev_map_node->zicio_devs;
		zicio_cmd_creator = zicio_get_command_creator(zicio_device);

		zicio_cmd_creator->zicio_unmap_dma_buffer(desc, i);
	}
}

/*
 * zicio_mmap_buffer
 *
 * Start point of mmap buffers to user and device
 */
unsigned long
zicio_mmap_buffers(zicio_descriptor *desc, int stflg,
			unsigned long user_base_address)
{
	/* Currently, considering only block device */
	int nr_dev, ret;
	unsigned i, j;

	/* First mapping for local pages */
	SET_PREMAPPING_ITER(desc, 0);
	if (!desc->zicio_shared_pool_desc) {
		for (i = 0 ; i < ZICIO_FIRST_LOCAL_DATABUFFER_PAGENUM ; i++) {
			ret = zicio_ghost_mapping_hugepage(desc, i,
				virt_to_page(desc->buffers.data_buffer[i]));
			if (ret != 0) {
				for (j = i - 1 ; j >= 0 ; j--) {
				    zicio_ghost_unmapping_hugepage(desc, j);
				}
				return -ENOMEM;
			}
		}
	}

	nr_dev = desc->dev_maps.nr_dev;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "[Kernel Message] complete in user mapping\n");
#endif

	/* Make DMA mapping for buffers */
	for (i = 0 ; i < nr_dev ; i++) {
		zicio_map_dma_buffer(desc, i);
	}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "[Kernel Message] complete in DMA mapping\n");
#endif

	/*
	 * Save user address to zicio descriptor
	 */
	desc->user_map.data_buffer = user_base_address;

	/*
	 * Allocate switch board and mmap its memory area
	 */
	desc->user_map.switch_board = 
				__zicio_mmap_buffers(
				(void**)(&desc->switch_board),
				ZICIO_BUFFER_BASE, ZICIO_PAGE_SIZE, ZICIO_PAGE_SIZE,
				stflg, 0);

	if (IS_ERR_VALUE(desc->user_map.switch_board)) {
		printk(KERN_WARNING "[Kernel Message] error in switch board mapping\n");
		goto l_zicio_mmap_buffer_err;
	}

#ifdef CONFIG_ZICIO_STAT
	/*
	 * Allocate stat board and mmap its memory area
	 */
	desc->user_map.stat_board = 
				__zicio_mmap_buffers(
				(void**)(&desc->stat_board),
				ZICIO_BUFFER_BASE, ZICIO_PAGE_SIZE, ZICIO_PAGE_SIZE,
				stflg, 0);

	if (IS_ERR_VALUE(desc->user_map.stat_board)) {
		printk(KERN_WARNING "[Kernel Message] error in switch board mapping\n");
		goto l_zicio_mmap_buffer_err;
	}
#endif /* CONFIG_ZICIO_STAT */

	/*
	 * Set the start ingestion buffer pointer of user
	 */
	desc->switch_board->data_buffer = desc->user_map.data_buffer;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "[Kernel Message] complete in switch board mapping\n");
#endif

	return desc->user_map.switch_board;

l_zicio_mmap_buffer_err:
	return -ENOMEM;
}

/*
 * zicio_munmap_buffers
 *
 * Unmap buffers from zicio
 */
int
zicio_munmap_buffers(zicio_descriptor *desc)
{
	zicio_unmap_dma_buffer(desc);
	zicio_munmap_buffer(desc->user_map.switch_board, ZICIO_PAGE_SIZE);
#ifdef CONFIG_ZICIO_STAT
	zicio_munmap_buffer(desc->user_map.stat_board, ZICIO_PAGE_SIZE);
#endif /* CONFIG_ZICIO_STAT */

	return 0;
}

/*
 * zicio_free_dev_map_node
 *
 * Free device map arrays.
 */
void
zicio_free_dev_map_node(zicio_dev_map_node *dev_map_node,
			bool is_shared)
{
	int i, order;

	/* Currently, the number of channel huge page is different single channel
	 * from shared channel. By consider this, set the number of pages and
	 * the order of pages */
	order = (is_shared) ? ilog2(ZICIO_LOCAL_DATABUFFER_CHUNK_NUM) :
			ilog2(ZICIO_DATABUFFER_CHUNK_NUM);

	for (i = 0 ; i < dev_map_node->nr_maps ; i++) {
		free_pages((unsigned long)dev_map_node->dev_map[i].prp_data_address,
				order);
		kfree(dev_map_node->dev_map[i].prp_data_array);

		if (dev_map_node->dev_map[i].prp_inode_address) {
			free_pages((unsigned long)
					dev_map_node->dev_map[i].prp_inode_address,
							ZICIO_INODE_PRP_PAGE_ORDER);
		}
		if (dev_map_node->dev_map[i].sgl_data_address) {
			free_pages((unsigned long)dev_map_node->dev_map[i].sgl_data_address,
					order + 1);
			kfree(dev_map_node->dev_map[i].sgl_data_array);
		}
	}

	zicio_free_if_not_null(dev_map_node->dev_map);
	zicio_free_if_not_null(dev_map_node->zicio_inner_devs);
	zicio_free_if_not_null(dev_map_node->inner_devs);
}

static void
zicio_free_dev_map_node_shared(zicio_dev_map_node *dev_map_node,
			int num_pages)
{
	int i, order_pages = ilog2(num_pages - 1) + 1;

	if (dev_map_node->dev_map) {
		for (i = 0 ; i < dev_map_node->nr_maps ; i++) {
			free_pages((unsigned long)dev_map_node->dev_map[i].prp_data_address,
					order_pages);
			if (dev_map_node->dev_map[i].prp_inode_address) {
				free_pages((unsigned long)
						dev_map_node->dev_map[i].prp_inode_address,
						ZICIO_INODE_PRP_PAGE_ORDER);
				kfree(dev_map_node->dev_map[i].prp_data_array);
			}
			if (dev_map_node->dev_map[i].sgl_data_address) {
				free_pages((unsigned long)
						dev_map_node->dev_map[i].sgl_data_address,
						ZICIO_DATA_SGL_PAGE_ORDER);
				kfree(dev_map_node->dev_map[i].sgl_data_array);
			}
		}
		zicio_free_if_not_null(dev_map_node->dev_map);
	}

	zicio_free_if_not_null(dev_map_node->zicio_inner_devs);
	zicio_free_if_not_null(dev_map_node->inner_devs);
}

/*
 * zicio_free_device_map
 *
 * Freeing device map of zicio descriptor
 */
void
zicio_free_device_map(zicio_dev_maps *dev_maps, bool is_shared)
{
	int i;

	zicio_free_if_not_null(dev_maps->devs);
	zicio_free_if_not_null(dev_maps->zicio_inner_dev_maps);

	if (dev_maps->dev_node_array) {
		for (i = 0 ; i < dev_maps->nr_dev ; i++) {
			zicio_free_dev_map_node(dev_maps->dev_node_array + i,
					is_shared);
		}
		kfree(dev_maps->dev_node_array);
	}
}

/*
 * zicio_free_device_map_shared
 *
 * Freeing device map of shared pool
 */
void
zicio_free_device_map_shared(zicio_shared_pool *zicio_shared_pool)
{
	zicio_dev_maps *dev_maps = &zicio_shared_pool->shared_dev_maps.dev_maps;
	int i, num_pages;

	zicio_free_if_not_null(dev_maps->devs);
	zicio_free_if_not_null(dev_maps->zicio_inner_dev_maps);

	if (dev_maps->dev_node_array) {
		num_pages = zicio_get_num_shared_buffer(zicio_shared_pool);
		for (i = 0 ; i < dev_maps->nr_dev ; i++) {
			zicio_free_dev_map_node_shared(dev_maps->dev_node_array + i,
					num_pages);
		}
		kfree(dev_maps->dev_node_array);
	}
	kmem_cache_destroy(zicio_shared_pool->shared_dev_maps.dev_maps_cache);
	kfree(zicio_shared_pool->shared_dev_maps.dev_map_start_point);
}

/*
 * zicio_allocate_shared_pool
 *
 * Allocate zicio shared pool
 */
void *
zicio_allocate_shared_pool(void) {
	return kmem_cache_alloc(zicio_shared_pool_cache, GFP_KERNEL|__GFP_ZERO);
}

/*
 * zicio_free_shared_pool
 *
 * Free zicio shared pool
 */
void
zicio_free_shared_pool(void *zicio_shared_pool) {
	kmem_cache_free(zicio_shared_pool_cache, zicio_shared_pool);
}

/*
 * zicio_allocate_shared_pool_local
 *
 * Allocate zicio shared pool channel local data
 */
void *
zicio_allocate_shared_pool_local(void) {
	return kmalloc(ZICIO_SPOOL_LOCAL_INFO_SIZE, GFP_KERNEL|__GFP_ZERO);
}

/*
 * zicio_free_shared_pool
 *
 * Free zicio shared pool channel local data
 */
void
zicio_free_shared_pool_local(void *shared_pool_local) {
	kfree(shared_pool_local);
}

/*
 * zicio_allocate_shared_pool_local
 *
 * Allocate zicio shared pool channel local data
 */
void *
zicio_allocate_channel(void) {
	return kmalloc(ZICIO_CHANNEL_INFO_SIZE, GFP_KERNEL|__GFP_ZERO);
}

/*
 * zicio_free_shared_pool
 *
 * Free zicio shared pool channel local data
 */
void
zicio_free_channel(void *zicio_channel) {
	kfree(zicio_channel);
}

/*
 * zicio_allocate_spcb
 *
 * Allocate shared page control blocks for zicio
 */
void *zicio_allocate_spcb(void) {
	return kmem_cache_alloc(zicio_spcb_cache, GFP_KERNEL|__GFP_ZERO);
}

/*
 * zicio_free_spcb
 *
 * Free shared page control blocks for zicio
 */
void zicio_free_spcb(void *spcb) {
	kmem_cache_free(zicio_spcb_cache, spcb);
}

/*
 * zicio_free_desc
 *
 * Free zicio channel descriptor
 */
void zicio_free_desc(void *desc) {
	kmem_cache_free(zicio_descriptor_cache, desc);
}

/*
 * zicio_free_buffers
 *
 * Free allocated buffer for channel
 */
long
zicio_free_buffers(zicio_descriptor *desc)
{
	zicio_buffer *zicio_buffers = &desc->buffers;
	struct page *free_page;
	int i;

	if (!desc) {
		return 0;
	}

	/* free data buffer pages */
	for (i = 0; i < zicio_buffers->num_data_buffer_pages ; ++i) {
		/* free data buffers */
		if (desc->buffers.data_buffer[i] != NULL) {
			free_page = virt_to_page(desc->buffers.data_buffer[i]); 
			ClearPageReserved(free_page);

			__free_pages(free_page, compound_order(free_page));
		}
	}

	kfree(zicio_buffers->data_buffer);

	/* free metadata buffer pages */
	if (desc->buffers.metadata_buffer) {
		free_pages((unsigned long)desc->buffers.metadata_buffer,
				ZICIO_CHUNK_ORDER);
	}

	if (desc->metadata_ctrl.inode_buffer) {
		free_pages((unsigned long)desc->metadata_ctrl.inode_buffer,
				ZICIO_CHUNK_ORDER);
	}

	zicio_free_device_map(&desc->dev_maps, (bool)desc->zicio_shared_pool_desc);

	zicio_free_read_files(&desc->read_files);

	if (desc->switch_board) {
		free_pages((unsigned long)desc->switch_board,
					ZICIO_SWITCHBOARD_ORDER);
	}

#ifdef CONFIG_ZICIO_STAT
	if (desc->stat_board) {
		free_pages((unsigned long)desc->stat_board,
					ZICIO_STATBOARD_ORDER);
	}
#endif /* CONFIG_ZICIO_STAT */

	kmem_cache_free(zicio_descriptor_cache, desc);

	return 0;
}

/**
 * zicio_get_request_timer - allocate the struct zicio_request_timer
 *
 * Allocate the struct zicio_request_timer using slab.
 */
void *zicio_alloc_request_timer(void)
{
	return kmem_cache_alloc(zicio_req_timer_cache, GFP_KERNEL);
}
EXPORT_SYMBOL(zicio_alloc_request_timer);

/**
 * zicio_free_request_timer - free the struct zicio_request_timer
 * @req_timer: addres of request timer to be freed
 *
 * Free the struct zicio_request_timer using slab.
 */
void zicio_free_request_timer(void *req_timer)
{
	kmem_cache_free(zicio_req_timer_cache, req_timer);
}
EXPORT_SYMBOL(zicio_free_request_timer);

/**
 * zicio_alloc_shared_request_timer
 *
 * Allocate the struct zicio_shared_request_timer using slab.
 */
void *zicio_alloc_shared_request_timer(zicio_descriptor *desc)
{
	atomic_inc((atomic_t *)&desc->zicio_shared_pool_desc->zicio_num_works);
	return kmem_cache_alloc(zicio_shared_req_timer_cache,
			GFP_KERNEL|__GFP_ZERO);
}
EXPORT_SYMBOL(zicio_alloc_shared_request_timer);

/**
 * zicio_free_shared_request_timer
 * @shared_req_timer: addrres of request timer to be freed
 *
 * Free the struct zicio_shared_request_timer using slab.
 */
void zicio_free_shared_request_timer(zicio_descriptor *desc,
		void *shared_req_timer)
{
	atomic_dec((atomic_t *)&desc->zicio_shared_pool_desc->zicio_num_works);
	kmem_cache_free(zicio_shared_req_timer_cache, shared_req_timer);
}
EXPORT_SYMBOL(zicio_free_shared_request_timer);

SYSCALL_DEFINE1(zicio_u_pread_breakdown_start, unsigned long __user*, u_pread_stat)
{
	BUG_ON(current->zicio_pread_stat != 0);

	current->zicio_pread_stat
		= page_to_virt(alloc_pages(GFP_KERNEL|__GFP_ZERO,
			ZICIO_SWITCHBOARD_ORDER));

	current->zicio_user_map_pread_stat
		= __zicio_mmap_buffers((void**)&current->zicio_pread_stat,
			ZICIO_BUFFER_BASE, ZICIO_PAGE_SIZE,
			ZICIO_PAGE_SIZE, 0, 0);

	copy_to_user(u_pread_stat,
		&current->zicio_user_map_pread_stat, sizeof(unsigned long));

	/* This will be set at the start of ksys_pread64() */
	current->zicio_enable_pread_stat = false;

	return 0;
}

SYSCALL_DEFINE0(zicio_u_pread_breakdown_end)
{
	BUG_ON(current->zicio_pread_stat == 0);

	zicio_munmap_buffer(current->zicio_user_map_pread_stat,
		ZICIO_PAGE_SIZE);

	free_pages(current->zicio_pread_stat,
		ZICIO_SWITCHBOARD_ORDER);

	current->zicio_enable_pread_stat = false;

	current->zicio_user_map_pread_stat = 0;

	current->zicio_pread_stat = 0;

	return 0;
}

SYSCALL_DEFINE0(zicio_u_pread_breakdown_get_time)
{
	zicio_pread_stat_board *pread_stat;
	struct cyc2ns_data data;

	BUG_ON(current->zicio_pread_stat == 0);

	pread_stat = (zicio_pread_stat_board *)current->zicio_pread_stat;

	cyc2ns_read_begin(&data);
	pread_stat->total_nsec_mode_switch_from_user
		= mul_u64_u32_shr(pread_stat->total_tsc_mode_switch_from_user,
			data.cyc2ns_mul, data.cyc2ns_shift);

	pread_stat->total_nsec_mode_switch_from_kernel
		= mul_u64_u32_shr(pread_stat->total_tsc_mode_switch_from_kernel,
			data.cyc2ns_mul, data.cyc2ns_shift);

	pread_stat->total_nsec_copy_page_to_iter
		= mul_u64_u32_shr(pread_stat->total_tsc_copy_page_to_iter,
			data.cyc2ns_mul, data.cyc2ns_shift);

	pread_stat->total_nsec_dio_wait_for_completion
		= mul_u64_u32_shr(pread_stat->total_tsc_dio_wait_for_completion,
			data.cyc2ns_mul, data.cyc2ns_shift);

	pread_stat->total_nsec_filemap_update_page
		= mul_u64_u32_shr(pread_stat->total_tsc_filemap_update_page,
			data.cyc2ns_mul, data.cyc2ns_shift);

	pread_stat->total_nsec_pread
		= mul_u64_u32_shr(pread_stat->total_tsc_pread,
			data.cyc2ns_mul, data.cyc2ns_shift);
	cyc2ns_read_end();
	
	return 0;
}
