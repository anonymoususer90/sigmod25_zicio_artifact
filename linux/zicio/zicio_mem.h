#ifndef __ZICIO_MEM_H
#define __ZICIO_MEM_H

#include <linux/types.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/zicio_notify.h>

#include "zicio_req_timer.h"
#include "zicio_shared_pool.h"

/* Buffer layout of zicio */
#define ZICIO_NUM_MGMT_CACHE 2
/* Number of page in buffer */
#define ZICIO_NUM_BUFFER_PAGE(sz)\
			(ZICIO_PAGE_ROUNDUP(sz)/ZICIO_PAGE_SIZE)
/* Number of buffer type */
#define ZICIO_NUM_CACHE 10
/* The number of PRP entries in a chunk */
#define ZICIO_NUM_PRP_ENTRIES_IN_PAGE 512
/* The number of SGL entries in a chunk */
#define ZICIO_NUM_SGL_ENTRIES_IN_PAGE 256
/* Start of user mapping */
#define ZICIO_BUFFER_BASE \
			((unsigned long)ZICIO_CHUNK_SIZE + mmap_min_addr)
#define ZICIO_BUFFER_CHUNK_PAGENUM (ZICIO_CHUNK_SIZE >> \
			ZICIO_PAGE_SHIFT)

#define ZICIO_FIRST_LOCAL_DATABUFFER_PAGENUM (16UL)
/* Memory alignment for zicio data buffer */
#define ZICIO_DATABUFFER_SLAB_ALIGN ZICIO_CHUNK_SIZE
/* Slab flag for zicio data buffer */
#define ZICIO_DATABUFFER_SLAB_FLAG (SLAB_PANIC|SLAB_HWCACHE_ALIGN)
#define ZICIO_DATABUFFER_CHUNK_PAGENUM ZICIO_BUFFER_CHUNK_PAGENUM

/* Size of request buffer */
#define ZICIO_COMMANDELEM_SIZE sizeof(zicio_nvme_cmd_list)
/* Order of request buffer */
#define ZICIO_COMMANDELEM_SLAB_ALIGN ZICIO_COMMANDELEM_SIZE
/* Slab flag for zicio data buffer */
#define ZICIO_COMMANDELEM_SLAB_FLAG (SLAB_PANIC|SLAB_HWCACHE_ALIGN)

/* Size of metadata buffer */
#define ZICIO_METADATABUFFER_SIZE ZICIO_CHUNK_SIZE
/* Memory alignment for zicio metadata buffer */
#define ZICIO_METADATABUFFER_SLAB_ALIGN ZICIO_CHUNK_SIZE
#define ZICIO_METADATABUFFER_PAGENUM (ZICIO_METADATABUFFER_SIZE >> \
			ZICIO_PAGE_SHIFT)

/* Size of inode buffer */
#define ZICIO_INODEBUFFER_SIZE ZICIO_CHUNK_SIZE
/* Memory alignment for zicio request buffer */
#define ZICIO_INODEBUFFER_SLAB_ALIGN ZICIO_CHUNK_SIZE
#define ZICIO_INODEBUFFER_PAGENUM ZICIO_BUFFER_CHUNK_PAGENUM
#define ZICIO_INODEBUFFER_MASK (~(ZICIO_INODEBUFFER_PAGENUM - 1))

/* Size of inode buffer */
#define ZICIO_EXTENTTREEBUFFER_SIZE ZICIO_CHUNK_SIZE 
/* Memory alignment for zicio request buffer */
#define ZICIO_EXTENTTREEBUFFER_SLAB_ALIGN ZICIO_CHUNK_SIZE

/* Size of timer buffer */
#define ZICIO_TIMERELEM_SIZE sizeof(struct zicio_request_timer)
/* Memory alignment for timer buffer */
#define ZICIO_TIMERELEM_SLAB_ALIGN ZICIO_TIMERELEM_SIZE

/* Size of timer buffer */
#define ZICIO_SHARED_TIMERELEM_SIZE	\
		sizeof(struct zicio_shared_request_timer)
/* Memory alignment for timer buffer */
#define ZICIO_SHARED_TIMERELEM_SLAB_ALIGN ZICIO_SHARED_TIMERELEM_SIZE

/* Size of request buffer */
#define ZICIO_DIOELEM_SIZE sizeof(struct zicio_iomap_dio)
/* Order of request buffer */
#define ZICIO_DIOELEM_SLAB_ALIGN ZICIO_DIOELEM_SIZE
/* Slab flag for zicio data buffer */
#define ZICIO_DIOELEM_SLAB_FLAG (SLAB_PANIC|SLAB_HWCACHE_ALIGN)

/* Size of request buffer */
#define ZICIO_IDLISTELEM_SIZE sizeof(struct zicio_id_node)
/* Order of request buffer */
#define ZICIO_IDLISTELEM_SLAB_ALIGN ZICIO_IDLISTELEM_SIZE
/* Slab flag for zicio data buffer */
#define ZICIO_IDLISTELEM_SLAB_FLAG (SLAB_PANIC|SLAB_HWCACHE_ALIGN)

#define ZICIO_DATA_PRP_PAGE_ORDER 4
#define ZICIO_INODE_PRP_PAGE_ORDER 1
#define ZICIO_DATA_SGL_PAGE_ORDER 5
#define ZICIO_INODE_SGL_PAGE_ORDER 1

#define ZICIO_BLOCK_CACHE 0
#define ZICIO_DATA 0
#define ZICIO_COMMAND 1
#define ZICIO_METADATA 2
#define ZICIO_INODE 3
#define ZICIO_EXTENTTREE 4
#define ZICIO_REQTIMER 5
#define ZICIO_DIO 6
#define ZICIO_IDLIST 7
#define ZICIO_SPCB 8

/* ZicIO descriptor information */
/* The size of zicio descriptor type */
#define ZICIO_DESC_SIZE sizeof(zicio_notify_descriptor)
/* The memory alignment for zicio descriptor */
#define ZICIO_DESC_SLAB_ALIGN ZICIO_DESC_SIZE
/* Flag for zicio request flag */
#define ZICIO_DESC_SLAB_FLAG (SLAB_PANIC|SLAB_HWCACHE_ALIGN)

/* ZicIO shared pool information */
/* The size of zicio shared pool */
#define ZICIO_SPOOL_SIZE sizeof(zicio_shared_pool)
/* The memory alignment for zicio descriptor */
#define ZICIO_SPOOL_SLAB_ALIGN ZICIO_SPOOL_SIZE
/* Flag for zicio request flag */
#define ZICIO_SPOOL_SLAB_FLAG (SLAB_PANIC|SLAB_HWCACHE_ALIGN)

/* Support device information */
/* NVME device information */
/* NVME control space shift */
#define ZICIO_NVME_CTRL_PAGE_SHIFT 12
/* The size of NVME control page */
#define ZICIO_NVME_CTRL_PAGE_SIZE (1 << ZICIO_NVME_CTRL_PAGE_SHIFT)
/* The number of cache types */
#define ZICIO_NUM_ALLOC_CACHE \
			((ZICIO_NUM_SUPP_RAW_DEV * ZICIO_NUM_CACHE) + \
			ZICIO_NUM_MGMT_CACHE)

/* Option flags for zicio */
#define ZICIO_RDONLY 0x1

#define zicio_get_buffer(_mem, _page, _off, _size) ((char*)_mem + \
			(_page * ZICIO_PAGE_SIZE) + (_off * _size))

#define zicio_get_last_elem(_mem, _p, _size) (((char*)_mem - \
			(char*)_p - _size) / _size)

/* Structure for  zicio slab allocator definition */
typedef struct zicio_slab_info {
	const char * name;
	unsigned int size;
	unsigned int align;
	unsigned int chunk_size;
	unsigned int chunk_order;
	slab_flags_t flags;
	struct kmem_cache ** cache_addr;
} __zicio_slab_info;

/*
 * zicio_get_dma_map_page_interval
 *
 * Get the page id of sgl mapping table.
 *
 * @num_pages: the number of hugepges
 * @dma_slot: device location in MD
 * @num_dev: number of device consiting of MD
 */
static inline int
zicio_get_dma_map_page_interval(int num_pages, int dma_slot, int num_dev,
			int *in_page_offset, bool is_sgl)
{
	unsigned int num_entries_per_page = ZICIO_NUM_PRP_ENTRIES_IN_PAGE;
	unsigned int entries_per_dma_slot;
	unsigned int current_dma_slot_start_point;
	unsigned int page_interval;

	do_div(num_entries_per_page, num_dev);
	entries_per_dma_slot = num_pages * num_entries_per_page;
	current_dma_slot_start_point = dma_slot * entries_per_dma_slot;

	*in_page_offset = zicio_do_div(
			page_interval, current_dma_slot_start_point,
			(is_sgl) ? ZICIO_NUM_SGL_ENTRIES_IN_PAGE :
					ZICIO_NUM_PRP_ENTRIES_IN_PAGE);

	return page_interval;
}

struct zicio_nvme_cmd_list *zicio_get_nvme_cmd_list(void);
void zicio_free_nvme_cmd_list(void *cmd);
void* zicio_get_zicio_bitvector(void);
void zicio_free_zicio_bitvector(void *bits);
struct hlist_head * zicio_get_hash_entry(int hash_bits);
void zicio_free_hash_entry(void *shared_pool_hash_entry);
void *zicio_get_dio(void);
int zicio_allocate_dev_map(zicio_descriptor *desc,
			struct device ** devs, zicio_device **zicio_devs, int nr_dev);
void *zicio_get_idnode(int id);
void zicio_free_idnode(void *node);
void *zicio_allocate_shared_pool(void);
void zicio_free_shared_pool(void *shared_pool);
void *zicio_allocate_shared_pool_local(void);
void zicio_free_shared_pool_local(void *shared_pool_local);
void *zicio_alloc_request_timer(void);
void zicio_free_request_timer(void *req_timer);
void *zicio_alloc_shared_request_timer(zicio_descriptor *desc);
void zicio_free_shared_request_timer(zicio_descriptor *desc,
		void *req_timer);
void *zicio_allocate_spcb(void);
void zicio_free_spcb(void *spcb);
void zicio_free_desc(void *desc);
void *zicio_allocate_channel(void);
void zicio_free_channel(void *zicio_channel);
void zicio_map_dma_buffer_for_nvme(void *desc, int dev_idx);
void zicio_map_dma_buffer_for_md(void *desc, int dev_id);
void zicio_map_dma_buffer_shared_for_nvme(void *shared_pool, void *zicio_spcb,
			int dev_idx);
void zicio_map_dma_buffer_shared_for_md(void *shared_pool, void *zicio_spcb,
			int dev_id);
void zicio_unmap_dma_buffer_shared_for_nvme(void *shared_pool,
			void *zicio_spcb, int dev_idx);
void zicio_unmap_dma_buffer_shared_for_md(void *shared_pool, void *zicio_spcb,
			int dev_id);
void zicio_map_dma_buffer_shared(zicio_shared_pool *zicio_shared_pool,
			zicio_shared_page_control_block *zicio_spcb, int dev_idx);
void zicio_unmap_dma_buffer_for_nvme(void *desc, int dev_idx);
void zicio_unmap_dma_buffer_for_md(void *desc, int dev_idx);
void zicio_allocate_dev_map_for_nvme(zicio_descriptor *desc,
			zicio_device *zicio_device, struct device *dev,
			zicio_dev_map_node *zicio_dev_map_node, int num_dev,
			void *zicio_shared_pool);
void zicio_allocate_dev_map_for_md(zicio_descriptor *desc,
			zicio_device *zicio_device, struct device *dev,
			zicio_dev_map_node *zicio_dev_map_node, int num_dev,
			void *zicio_shared_pool);
int zicio_allocate_shared_buffer_cache(
			zicio_shared_pool *zicio_shared_pool);
int zicio_allocate_dev_map_shared(zicio_shared_pool *zicio_shared_pool,
			struct device **devs, zicio_device **zicio_devs, int nr_dev,
			zicio_shared_pool_key_t shared_pool_key);
int zicio_mmap_buffers_shared(zicio_shared_pool *zicio_shared_pool);
void zicio_free_device_map(zicio_dev_maps *dev_maps, bool is_shared);
void zicio_free_device_map_shared(zicio_shared_pool *zicio_shared_pool);
zicio_shared_page_control_block * zicio_create_spcb(
			zicio_shared_pool *zicio_shared_pool, int page_id);
void zicio_destroy_spcb(zicio_shared_pool *zicio_shared_pool,
			zicio_shared_page_control_block *zicio_spcb);
void zicio_destroy_shared_pool_spcbs(zicio_shared_pool *zicio_shared_pool,
			int num_pages);
#endif /* ZICIO_MEM_H */
