#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/hashtable.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/nospec.h>
#include <linux/slab.h>
#include <linux/zicio_notify.h>
#include <linux/random.h>

#include "zicio_atomic.h"
#include "zicio_cmd.h"
#include "zicio_device.h"
#include "zicio_extent.h"
#include "zicio_files.h"
#include "zicio_firehose_ctrl.h"
#include "zicio_md_flow_ctrl.h"
#include "zicio_ghost.h"
#include "zicio_mem.h"
#include "zicio_shared_pool.h"
#include "zicio_shared_pool_mgr.h"
#include "zicio_req_submit.h"

static DEFINE_PER_CPU(struct list_head, per_cpu_shared_callback_list);
static DEFINE_PER_CPU(struct list_head, per_cpu_shared_gc_list);
static DEFINE_PER_CPU(zicio_descriptor *, current_active_shared_desc);

static void zicio_reactivate_lag_user_request(struct timer_list *timer);
unsigned int zicio_get_pagenum_in_a_jiffy(zicio_descriptor *desc);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
static void zicio_dump_shared_bitvector(
			zicio_shared_pool *zicio_shared_pool);
static void zicio_dump_channels_in_shared_pool(
			zicio_shared_pool *zicio_shared_pool);
#endif /* CONFIG_ZICIO_DEBUG */

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
/**
 * zicio_calc_io_watermark_shared
 * @avg_consumable_chunk_num_in_jiffy: average user consumable chunk count.
 *
 * In shared mode, the number of consumable chunk is used to deteremine whether
 * we can do more I/O or not.
 *
 * If there are currently more chunks that can be premapped than this number, no
 * I/O is needed. We call this point as a watermark.
 *
 * This function returns the watermark based on the given argument, average
 * consumable chunk number in a jiffy.
 */
static inline unsigned int
zicio_calc_io_watermark_shared(
	unsigned int avg_consumable_chunk_num_in_jiffy)
{
	return (avg_consumable_chunk_num_in_jiffy << 1);
}

/**
 * zicio_determine_new_request_needed_shared
 * @zicio_desc: zicio descriptor
 * @distance_from_head: distance between average ingestion point and head
 *
 * In shared mode, we need to compare distance from head and watermark. The
 * distance from head represents the distance between the average user ingestion
 * point and the head of monotonic chunk id. The watermark represents the amount
 * of chunks users can consume during a specific period (mainly 1 jiffy).
 *
 * If the distance from head is smaller than the watermark, it means that users
 * can consume enough even if they do more I/O.
 *
 * So, I/O will be allowed to the extent that the difference between distance
 * from head and watermark. At this time, it is possible that multiple users can
 * perform I/O at the same time, so the distance from head can exceed the
 * watermakr. To prevent this, we control I/O casting using atomic variable.
 */
static int
zicio_determine_new_request_needed_shared(zicio_descriptor *zicio_desc,
		unsigned int distance_from_head)
{
	zicio_shared_pool *zicio_shared_pool 
		= zicio_get_shared_pool(zicio_desc);
	unsigned int avg_consumable_chunk_num_in_jiffy
		= zicio_get_pagenum_in_a_jiffy(zicio_desc);
	unsigned int watermark
		= zicio_calc_io_watermark_shared(avg_consumable_chunk_num_in_jiffy);
	int prev_reserved_new_request_count = 0;

	/* Quick exit */
	if (watermark <= distance_from_head)
		return ZICIO_IO_NOT_NEEDED;

	prev_reserved_new_request_count
		= atomic_fetch_add(1, &zicio_shared_pool->cur_requested_chunk_count);

	/*  Already enough chunks are requested. Do not request chunk */
	if (watermark <= distance_from_head + prev_reserved_new_request_count) {
		/* roll back the state */
		atomic_dec_if_positive(&zicio_shared_pool->cur_requested_chunk_count);
		return ZICIO_IO_NOT_NEEDED;
	}

	return ZICIO_IO_REQUIRED;
}
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

/*
 * zicio_get_bitvector_depth
 *
 * Get the depth of bitvector.
 */
static inline short
zicio_get_bitvector_depth(unsigned long leaf_vector_bytes)
{
	return ZICIO_DEFAULT_BITVECTOR_DEPTH;
}

/*
 * zicio_get_bivector_length
 *
 * Get the bytes length of bitvector.
 *
 * @num_chunks: number of chunks
 * @shared_depth0: shared depth 0 bit vector flag
 */
static inline unsigned int
zicio_get_bitvector_length(unsigned int num_chunks, bool shared_depth0)
{
	return ((num_chunks - 1) >> (ZICIO_BYTE_MIN_ORDER -
				(int)shared_depth0)) + 1;
}

/*
 * __zicio_allocate_and_initialize_bitvector
 *
 * Allcoate and initialize bitvector.
 *
 * @bitvector_length: The bytes of depth 0 bitvector
 * @depth: Max depth
 * @length: length array by depth
 * @is_shared: Flag to show this bitvector is shared.
 */
zicio_bitvector_t **
__zicio_allocate_and_initialize_bitvector(unsigned bitvector_length,
			short depth, unsigned *length, bool is_shared)
{
	zicio_bitvector_t **bit_vector;
	unsigned int num_longs;
	short i;

	bit_vector = (zicio_bitvector_t **)kmalloc(
			sizeof(zicio_bitvector_t *) * depth, GFP_KERNEL);

	for (i = 0 ; i < depth ; i++) {
		/* Get the number of bitvector elemets in one bitvector */
		num_longs = DIV_ROUND_UP(bitvector_length,
				1UL << ZICIO_INTERNAL_BITVECTOR_ORDER);
		/* Set length of bitvector using the number of bitvector elements */
		bitvector_length = num_longs << ZICIO_INTERNAL_BITVECTOR_ORDER;
		bit_vector[i] = (zicio_bitvector_t *)kmalloc(bitvector_length,
					GFP_KERNEL|__GFP_ZERO);

		length[i] = bitvector_length;
		bitvector_length = ((bitvector_length - 1) >>
					ZICIO_INTERNAL_BIT_CHUNK_COVER_ORDER) + 1;

		/* Local bitvector's internal bitvector has 2bit for area */
		if (!is_shared) {
			bitvector_length <<= 1;
		}
	}

	return bit_vector;
}


/*
 * __zicio_allocate_and_initialize_debug_bitvector
 *
 * Allcoate and initialize bitvector.
 *
 * @bitvector_length: The bytes of depth 0 bitvector
 */
zicio_bitvector_t **
__zicio_allocate_and_initialize_debug_bitvector(unsigned bitvector_length)
{
	zicio_bitvector_t **bit_vector;
	unsigned int num_longs;
	short i;

	bit_vector = (zicio_bitvector_t **)kmalloc(
			sizeof(zicio_bitvector_t *) * 2, GFP_KERNEL);

	/* Get the number of bitvector elemets in one bitvector */
	num_longs = DIV_ROUND_UP(bitvector_length,
			1UL << ZICIO_INTERNAL_BITVECTOR_ORDER);
	/* Set length of bitvector using the number of bitvector elements */
	bitvector_length = num_longs << ZICIO_INTERNAL_BITVECTOR_ORDER;

	for (i = 0 ; i < 2 ; i++) {
		bit_vector[i] = (zicio_bitvector_t *)kmalloc(bitvector_length,
					GFP_KERNEL|__GFP_ZERO);
	}

	return bit_vector;
}

/*
 * zicio_allocate_and_initialize_bitvector
 *
 * Allocate and initialize bitvector and its metadata.
 */
zicio_bitvector *
zicio_allocate_and_initialize_bitvector(zicio_bitvector *bitvector,
			unsigned long num_chunks, bool shared)
{
	unsigned bitvector_bytes;
	short depth;

	/* Get the bytes length of bitvector */
	bitvector_bytes = zicio_get_bitvector_length(num_chunks, shared);
	/* Get the depth of bitvector: currently, set it to 2 */
	depth = zicio_get_bitvector_depth(bitvector_bytes);

	/* Allocate bitvector length array */
	bitvector->bitvector_length = kmalloc(sizeof(unsigned) * depth, GFP_KERNEL);

	if (unlikely(!bitvector->bitvector_length)) {
		return NULL;
	}

	/* Set bitvector */
	bitvector->bit_vector = 
		__zicio_allocate_and_initialize_bitvector(bitvector_bytes, depth,
				bitvector->bitvector_length, shared);
	/* Set the number of pages */
	bitvector->num_chunks = num_chunks;
	/* Set the depth of bitvector */
	bitvector->depth = depth;
	bitvector->shared = shared;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
	if (!shared) {
		bitvector->debug_bit_vector =
				__zicio_allocate_and_initialize_debug_bitvector(
						bitvector_bytes);
	}
#endif /* (CONFIG_ZICIO_DEBUG_LEVEL >= 2) */
	return bitvector;
}

/*
 * zicio_delete_zicio_bitvector
 *
 * Delete zicio bitvector.
 */
void
zicio_delete_zicio_bitvector(zicio_bitvector *zicio_bitvector)
{
	int i;
	/*
	 * free bit vector
	 */
	kfree(zicio_bitvector->bitvector_length);
	for (i = 0 ; i < zicio_bitvector->depth ; i++) {
		kfree(zicio_bitvector->bit_vector[i]);
	}
	kfree(zicio_bitvector->bit_vector);
}

/*
 * zicio_allocate_and_initialize_wait_free_queue
 *
 * Allocate and initialize shared queue to manage page ID.
 */
static bool
zicio_allocate_and_initialize_wait_free_queue(
		zicio_wait_free_queue *zicio_queue, int num_elems, bool page_queue)
{
	int queue_size;
	/*
	 * If wait free queue use a page for page_queue, then allocate a page for
	 * it. Otherwise, set a queue size to twice of number of elements.
	 */
	if (page_queue) {
		/* Allocate a circular queue page */
		zicio_queue->circular_queue = (atomic_t *)page_to_virt(
				alloc_page(GFP_KERNEL|__GFP_ZERO));
		zicio_queue->queue_size = 0;
		/* Set id mask */
		zicio_queue->point_mask = (~(unsigned)((ZICIO_PAGE_SIZE >> 2) - 1));
	} else {
		queue_size = roundup_pow_of_two(num_elems << 1);
		if (queue_size < (ZICIO_PAGE_SIZE >> 3)) {
			zicio_queue->queue_size = queue_size;
		} else {
			zicio_queue->queue_size = ZICIO_PAGE_SIZE >> 3;
		}
		/* Set id mask */
		zicio_queue->circular_queue = (atomic_t *)kmalloc(sizeof(atomic_t) *
			zicio_queue->queue_size, GFP_KERNEL|__GFP_ZERO);
		zicio_queue->point_mask = ~(((unsigned)zicio_queue->queue_size) - 1);
	}

	if (unlikely(!zicio_queue->circular_queue)) {
		return false;
	}

	return true;
}

/*
 * zicio_free_wait_free_queue
 *
 * Free an allocated page for circular queue.
 */
static void
zicio_free_wait_free_queue(zicio_wait_free_queue *zicio_queue)
{
	if (zicio_queue->queue_size) {
		kfree(zicio_queue->circular_queue);
	} else {
		free_page((unsigned long)zicio_queue->circular_queue);
	}
}

/*
 * zicio_allocate_and_initialize_page_id_queue
 *
 * Allocate and initialize page id managing queue
 */
static bool
zicio_allocate_and_initialize_page_id_queue(
			zicio_shared_pool *zicio_shared_pool)
{
	/* Get the number of huge page in data buffer */
	int num_pages = zicio_get_num_shared_buffer(zicio_shared_pool);
	int idx;

	BUG_ON(num_pages > (ZICIO_PAGE_SIZE >> 3));

	/* Allocate and initialize wait free queue */
	if (!(zicio_allocate_and_initialize_wait_free_queue(
			&zicio_shared_pool->shared_page_id_queue, num_pages, true))) {
		return false;
	}

	/* Set allocate pointer and free pointer */
	zicio_set_allocate_position_in_queue(
			&zicio_shared_pool->shared_page_id_queue, num_pages - 1);
	zicio_set_free_position_in_queue(
			&zicio_shared_pool->shared_page_id_queue, UINT_MAX);

	/* Set value to page ID */
	for (idx = 0 ; idx < num_pages ; idx++) {
		zicio_set_value_to_queue(&zicio_shared_pool->shared_page_id_queue,
			idx, num_pages - idx);
	}

	mb();

	return true;
}

void
zicio_print_page_id_queue(zicio_descriptor *desc)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_wait_free_queue *zicio_queue = &zicio_shared_pool->shared_page_id_queue;
	unsigned int free_point = atomic_read(&zicio_queue->free_point);
	unsigned int allocate_point = atomic_read(&zicio_queue->allocate_point);
	unsigned int idx, pos, value;

	printk(KERN_WARNING "[DUMP PAGE ID QUEUE]\n");
	printk(KERN_WARNING "Free point: %u[<= %u]\n",
			free_point & ~(zicio_queue->point_mask), free_point);
	printk(KERN_WARNING "Allocate point: %u[<= %u]\n",
			allocate_point & ~(zicio_queue->point_mask), allocate_point);

	printk(KERN_WARNING "[IN WINDOW]\n");
	for (idx = free_point ; idx < allocate_point ; idx++) {
		pos = idx & ~(zicio_queue->point_mask);
		value = atomic_read(zicio_queue->circular_queue + pos);
		printk(KERN_WARNING "idx[%u{<=%u}]: %u\n", pos, idx, value);
	}

	printk(KERN_WARNING "[ALL QUEUE]\n");
	for (idx = 0 ; idx < 1024 ; idx++) {
		value = atomic_read(zicio_queue->circular_queue + idx);
		printk(KERN_WARNING "idx[%u]: %u\n", idx, value);
	}
}

/*
 * zicio_free_page_id_queue
 *
 * Free page id managing queue.
 */
static void
zicio_free_page_id_queue(zicio_shared_pool *zicio_shared_pool)
{
	zicio_free_wait_free_queue(&zicio_shared_pool->shared_page_id_queue);
}

/*
 * zicio_get_page_id_from_queue
 *
 * Get local huge page ID from page id queue
 */
unsigned int
zicio_get_page_id_from_queue(zicio_descriptor *desc,
			unsigned int *page_id_queue_idx, bool wait)
{
	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool_local *zicio_shared_pool_local;
	unsigned int pos, value;

	zicio_shared_pool = zicio_get_shared_pool(desc);
	pos = zicio_get_allocate_position_in_queue(
			&zicio_shared_pool->shared_page_id_queue);
	value = zicio_check_queue_ready(&zicio_shared_pool->shared_page_id_queue,
			pos, wait);

	if (!value) {
		zicio_shared_pool_local = zicio_get_shared_pool_local(desc);
		*page_id_queue_idx = pos;
		return -1;
	}

	return value - 1;
}

/*
 * zicio_raed_page_id_from_queue
 *
 * Read local huge page ID from page id queue
 */
unsigned int
zicio_read_page_id_from_queue(zicio_descriptor *desc, unsigned int pos)
{
	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool_local *zicio_shared_pool_local;
	unsigned int value;

	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);
	zicio_shared_pool = zicio_get_shared_pool(desc);

	value = zicio_check_queue_ready(&zicio_shared_pool->shared_page_id_queue,
			pos, false);

	mb();
	if (!value) {
		return -1;
	}

	return value - 1;
}

/*
 * zicio_set_page_id_to_queue
 *
 * Set local huge page ID to page id queue
 */
void
zicio_set_page_id_to_queue(zicio_descriptor *desc, int page_id)
{
	zicio_shared_pool *zicio_shared_pool;
	unsigned int pos, value;

	BUG_ON(page_id < 0 || page_id >= 64);

	mb();
	zicio_shared_pool = zicio_get_shared_pool(desc);
	pos = zicio_get_free_position_in_queue(
			&zicio_shared_pool->shared_page_id_queue);
	value = zicio_check_queue_unready(&zicio_shared_pool->shared_page_id_queue,
			pos, false);
	BUG_ON(value);
	zicio_set_value_to_queue(&zicio_shared_pool->shared_page_id_queue,
			pos, (unsigned int)(page_id + 1));
}

/*
 * zicio_get_attached_channel
 *
 * Get an unused id from the attached channel table in the shared pool.
 */
zicio_attached_channel*
zicio_get_attached_channel(zicio_id_allocator *zicio_attached_channels,
			unsigned int id)
{
	return zicio_get_zicio_struct(zicio_attached_channels, id, true);
}

/*
 * zicio_get_unusd_attached_channel_id
 *
 * Get the channel id value in used shared pool.
 */
int
zicio_get_unused_attached_channel_id(
			zicio_id_allocator *zicio_attached_channels)
{
	return zicio_get_unused_id(zicio_attached_channels);
}

/*
 * zicio_put_unused_attached_channel_id
 *
 * Put the channel id value in shared pool that will not be used in the future.
 */
void
zicio_put_unused_attached_channel_id(
			zicio_id_allocator *zicio_attached_channels, unsigned int id)
{
	zicio_put_unused_id(zicio_attached_channels, id);
}

/*
 * zicio_pick_attached_channel
 *
 * Pick the attached channel matched with id from shared pool.
 */
zicio_attached_channel *
zicio_pick_attached_channel(
			zicio_id_allocator *zicio_attached_channels, unsigned int id)
{
	return zicio_pick_id(zicio_attached_channels, id);
}

/*
 * zicio_install_attached_channel
 *
 * Bind attached channel structure with channel id.
 */
void
zicio_install_attached_channel(zicio_id_allocator *zicio_attached_channels,
			unsigned int attached_channel_id,
			zicio_attached_channel *zicio_channel)
{
	zicio_install_zicio_struct(zicio_attached_channels, attached_channel_id,
				zicio_channel);
}

/*
 * zicio_cleanup_attached_channel
 *
 * Clean up the id table of attached channel from shared pool.
 */
void
zicio_cleanup_attached_channel(
			zicio_id_allocator *zicio_attached_channels)
{
	zicio_delete_idtable(zicio_attached_channels);
}

/*
 * zicio_init_attached_channel
 *
 * Initialize attached channel id table.
 */
void 
zicio_init_attached_channel(zicio_id_allocator *zicio_attached_channels)
{
	zicio_init_id_allocator(zicio_attached_channels);
}

/*
 * zicio_allocate_and_initialize_shared_files
 *
 * Allocate and initialize shared files.
 */
long
zicio_allocate_and_initialize_shared_files(
			zicio_shared_pool *zicio_shared_pool, unsigned int *fds,
			struct fd *fs, int *file_dev_map, int nr_fd)
{
	zicio_shared_files *shared_files = &zicio_shared_pool->shared_files;

	memset(shared_files, 0, sizeof(zicio_shared_files));

	/* Initialize shared file's info */
	if (zicio_initialize_read_files(&shared_files->registered_read_files,
				fs, fds, file_dev_map, nr_fd) < 0) {
		goto l_zicio_allocate_and_initialize_shared_files_ret;
	}

	shared_files->total_chunk_nums = zicio_get_total_filesize(shared_files);

	return 0;
l_zicio_allocate_and_initialize_shared_files_ret:
	return -ENOMEM;
}

/*
 * zicio_delete_shared_files
 *
 * Delete the resource of shared files.
 */
void
zicio_delete_shared_files(zicio_shared_files *zicio_shared_files)
{
	kfree(zicio_shared_files->start_chunk_nums);

	/*
	 * Free registered file information
	 */
	zicio_free_read_files(&zicio_shared_files->registered_read_files);
}

/*
 * zicio_init_shared_hash
 *
 * Initialize shared hash table of shared pool
 */
void
zicio_init_shared_hash(zicio_shared_pool *zicio_shared_pool)
{
	unsigned int i;
	unsigned int rcu_hash_size
				= ZICIO_RCU_HASH_SIZE(zicio_shared_pool->shared_pool_hash);

	for (i = 0 ; i < rcu_hash_size ; i++) {
		zicio_init_hlist_head(&zicio_shared_pool->shared_pool_hash[i]);
	}
}

/*
 * zicio_allocate_and_initialize_mem_shared
 *
 * Allocate and initialize data buffer for shared pool and device maps for
 * shared_pool.
 *
 */
int
zicio_allocate_and_initialize_mem_shared(
			zicio_shared_pool *zicio_shared_pool, struct device **devs,
			zicio_device **zicio_devices, int nr_fd,
			zicio_shared_pool_key_t shared_pool_key)
{
	int num_dev, ret;

	/* Get the number of distinct device array */
	num_dev = zicio_distinct_nr_dev(devs, nr_fd);

	/* Allocate spcb array page */
	zicio_shared_pool->zicio_spcb = (atomic64_t *)page_to_virt(
			alloc_page(GFP_KERNEL));

	/* ALlocate device map */
	if ((ret = zicio_allocate_dev_map_shared(zicio_shared_pool, devs,
			zicio_devices, num_dev, shared_pool_key)) < 0) {
		goto l_zicio_allocate_and_initialize_mem_shared_free_dev_map_out;
	}

	/* Allocate shared data buffer */
	zicio_allocate_shared_buffer_cache(zicio_shared_pool);

	return ret;
l_zicio_allocate_and_initialize_mem_shared_free_dev_map_out:
	zicio_free_device_map(&zicio_shared_pool->shared_dev_maps.dev_maps, true);

	return ret;
}

/*
 * zicio_free_mem_shared
 *
 * Free shared pool's memory and mapping elements.
 * 
 * @zicio_shared_pool: zicio shared pool singleton
 */
void
zicio_free_mem_shared(zicio_shared_pool *zicio_shared_pool)
{
	int num_pages;
	/*
	 * Free data buffer for shared pool.
	 */
	num_pages = zicio_get_num_shared_buffer(zicio_shared_pool);
	zicio_destroy_shared_pool_spcbs(zicio_shared_pool, num_pages);

	/*
	 * Free device map for data buffer.
	 */
	zicio_free_device_map_shared(zicio_shared_pool);
}

/*
 * zicio_create_shared_pool
 *
 * Create shared pool
 */
zicio_shared_pool *
zicio_create_shared_pool(struct device **devices, unsigned int *fds,
			struct fd *fs, int *file_dev_map, int nr_fd,
			zicio_shared_pool_key_t shared_pool_key)
{
	zicio_shared_pool *zicio_shared_pool;
	zicio_device **zicio_devices;

	/*
	 * Allocate zicio_shared_pool struct
	 */
	zicio_shared_pool = zicio_allocate_shared_pool();

	if (unlikely(!zicio_shared_pool)) {
		return NULL;
	}

	zicio_devices = zicio_initialize_device_to_desc(devices, nr_fd);

	if (unlikely(IS_ERR_OR_NULL(zicio_devices))) {
		printk("[ZICIO] Error in device allocation\n");
		goto l_zicio_create_shared_pool_out;
	}

	if ((zicio_allocate_and_initialize_mem_shared(zicio_shared_pool, devices,
			zicio_devices, nr_fd, shared_pool_key)) < 0) {
		printk("[ZICIO] Error in shared pool memory allocation\n");
		goto l_zicio_create_shared_pool_dev_out;
	}

	/*
	 * Allocate and initialize shared page id managing queue
	 */
	if (unlikely(!zicio_allocate_and_initialize_page_id_queue(
				zicio_shared_pool))) {
		printk("[ZICIO] Error in shared page managing queue allocation\n");
		goto l_zicio_create_shared_pool_buffer_out;
	}

	/*
	 * Initialize member variables of zicio shared pool
	 */
	atomic_set(&zicio_shared_pool->pin, 1);
	atomic_set(&zicio_shared_pool->head, 0);

	zicio_allocate_and_initialize_shared_files(zicio_shared_pool, fds, fs,
				file_dev_map, nr_fd);

	if (unlikely(!zicio_allocate_and_initialize_bitvector(
				&zicio_shared_pool->bitvector,
				zicio_shared_pool->shared_files.total_chunk_nums, true))) {
		goto l_zicio_create_shared_pool_queue_out;
	}

	zicio_init_shared_hash(zicio_shared_pool);

	zicio_init_attached_channel(&zicio_shared_pool->zicio_channels);

	zicio_free_if_not_null(zicio_devices);
	return zicio_shared_pool;

l_zicio_create_shared_pool_queue_out:
	zicio_free_page_id_queue(zicio_shared_pool);
l_zicio_create_shared_pool_buffer_out:
	zicio_free_mem_shared(zicio_shared_pool);
l_zicio_create_shared_pool_dev_out:
	zicio_free_if_not_null(zicio_devices);
l_zicio_create_shared_pool_out:
	zicio_free_shared_pool(zicio_shared_pool);
	return NULL;
}

/*
 * zicio_allocate_and_initialize_channel_shared
 *
 * Initialize and allocate attached channel resources for sharing
 */
zicio_attached_channel *
zicio_allocate_and_initialize_channel_shared(zicio_descriptor *desc)
{
	zicio_attached_channel *zicio_channel;
	int id;

	/* Get attached channel resources */
	zicio_channel = zicio_allocate_channel();

	if (unlikely(!zicio_channel)) {
		return ERR_PTR(-ENOMEM);
	}

	atomic_set(&zicio_channel->last_forcefully_unmapped_file_chunk_id, UINT_MAX);
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	atomic_set(&zicio_channel->last_forcefully_unmapped_monotonic_chunk_id,
		UINT_MAX);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	for (id = 0 ; id < ZICIO_LOCAL_DATABUFFER_CHUNK_NUM ; id++) {
		zicio_channel->local_zicio_spcb[id] = zicio_allocate_spcb();
		zicio_channel->local_zicio_spcb[id]->zicio_spcb.chunk_ptr =
				desc->buffers.data_buffer[id];
		zicio_channel->local_zicio_spcb[id]->zicio_spcb.local_page_idx = id;
	}

	return zicio_channel;
}

/*
 * zicio_get_head_chunk_id_from_shared_pool
 *
 * Get the current head of memory chunk
 *
 * @desc: zicio channel descriptor
 */
static unsigned long
zicio_get_head_chunk_id_from_shared_pool(zicio_descriptor *desc)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	unsigned int current_head_no_mod;

	/*
	 * When the head is incremented only by atomic_inc, it is possible to check
	 * that the derailed channel has been derailed after incrementing the head
	 * of the shared pool after the derailed channel completes one round. To
	 * prevent this, the head is incremented using the following logic.
	 */
	do {
		current_head_no_mod = atomic_read(&zicio_shared_pool->head);

		/*
		 * TODO: double check
		 * 
		 * The time to look at head to see if I/O should be requested (during
		 * zicio_check_io_required()) and the time to increment head to
		 * actually request I/O are not the same.
		 * 
		 * So even though this channel has already checked to see if it should
		 * request I/O with head in zicio_check_io_required(), there may be
		 * cases where it shouldn't request I/O when it actually does.
		 * (head may have already been incremented by other concurrently running
		 * channels and exceeded the HIGH_WATERMARK)
		 * 
		 * Therefore, if "too many chunk I/O requests problem" occurs even after
		 * adjusting the amount of I/O requests in the flow control logic, here
		 * is a secondary place to adjust the amount of I/O requests.
		 */

		if ((uint64_t)current_head_no_mod >=
			zicio_shared_pool_local->consume_indicator.start_chunk_id_no_mod +
				(uint64_t)zicio_shared_pool->shared_files.total_chunk_nums) {
			atomic_set(&zicio_shared_pool_local->zicio_channel->derailed, true);
			zicio_shared_pool_local->consume_indicator.current_chunk_id_mod = 0;
			return 0;
		}
	} while (atomic_cmpxchg(&zicio_shared_pool->head, current_head_no_mod,
			current_head_no_mod + 1) != current_head_no_mod);

	return current_head_no_mod;
}

/*
 * zicio_calc_shared_pool_sum_consumption_tsc_delta
 *
 * Calculate the summation of user consumption speed
 *
 * @zicio_channel_args: local_channels' information
 * @ret: summation of user consumption time.
 */
static void
zicio_calc_shared_pool_sum_consumption_tsc_delta(void *zicio_channel_args,
			void *ret)
{
	zicio_attached_channel *zicio_channel = zicio_channel_args;
	zicio_descriptor *desc = zicio_channel->desc;
	zicio_raw_user_consumption_time_sum *zicio_consumption_time = ret;

	BUG_ON(!desc);

	if (zicio_check_channel_derailed(desc) ||
		(!desc->switch_board->avg_tsc_delta)) {
		return;
	}

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	zicio_consumption_time->user_consumption_time_arr[zicio_consumption_time->nr_channels]
		= zicio_tsc_to_ktime(desc->switch_board->avg_tsc_delta);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	zicio_consumption_time->nr_channels++;
	zicio_consumption_time->user_consumption_time_sum +=
		desc->switch_board->avg_tsc_delta;
	zicio_consumption_time->square_user_consumption_time_sum +=
		desc->switch_board->avg_tsc_delta * desc->switch_board->avg_tsc_delta;
}

/*
 * zicio_get_shared_pool_consumption_tsc_delta
 *
 * Get the user consumption time representing the shared pool.
 */
unsigned long
zicio_get_shared_pool_consumption_tsc_delta(zicio_descriptor *desc)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_raw_user_consumption_time_sum zicio_consumption_time = {0, 0, 0};
	unsigned long long user_consumption_time_variance = 0;
	unsigned long long avg_square_user_consumption_time = 0;
	unsigned long avg_user_consumption_time = 0;
	unsigned long user_consumption_time_stdev = 0;
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	int i = 0;
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	
	/*
	 * Iterate channels as many as registered channels and calculate average
	 * user consumption time.
	 */
	zicio_iterate_all_zicio_struct_with_ret(&zicio_shared_pool->zicio_channels,
			zicio_calc_shared_pool_sum_consumption_tsc_delta,
			&zicio_consumption_time, atomic_read(&zicio_shared_pool->pin) - 1,
			false);

	/*
	 * If we don't have user consumption time evaluated yet, then use default
	 * time setting.
	 */
	if (!zicio_consumption_time.nr_channels) {
		return ZICIO_DEFAULT_EXP_TIME_IN_NANO;
	}

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	if (desc->channel_index == 2)
	{
		printk(KERN_WARNING "[ZICIO] zicio_get_shared_pool_consumption_tsc_delta() start -------\n");

		for (i = 0; i < zicio_consumption_time.nr_channels; i++)
		{
			printk(KERN_WARNING "[ZICIO] zicio_get_shared_pool_consumption_tsc_delta(), user consumption time: %d\n",
					zicio_consumption_time.user_consumption_time_arr[i]);
		}

		printk(KERN_WARNING "[ZICIO] zicio_get_shared_pool_consumption_tsc_delta() end -------\n");
	}
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

#if 0
	/* Average of (consumption time)^2 */
	avg_square_user_consumption_time
		= zicio_consumption_time.square_user_consumption_time_sum
			/ zicio_consumption_time.nr_channels;

	/* Average of consumption time */
	avg_user_consumption_time
		= zicio_consumption_time.user_consumption_time_sum
			/ zicio_consumption_time.nr_channels;

	/* V(x) = E(x^2) - (E(x))^2 */
	user_consumption_time_variance
		= avg_square_user_consumption_time -
			(avg_user_consumption_time * avg_user_consumption_time);

	user_consumption_time_stdev = int_sqrt64(user_consumption_time_variance);

	/*
	 * We calculate representing consumption time for the shared pool.
	 *
	 * If the variation in consumption time is large, slow users may have to
	 * forcefully unmap a very large number of chunks.
	 *
	 * To prevent this, avoid setting the chunk expiration time too short by
	 * adding deviation to the average consumption time.
	 */
	return avg_user_consumption_time + user_consumption_time_stdev;
#else
	avg_user_consumption_time
		= zicio_consumption_time.user_consumption_time_sum
			/ zicio_consumption_time.nr_channels;

	return avg_user_consumption_time;
#endif
}

/*
 * zicio_get_shared_pool_raw_expiration_time
 *
 * Get the representing user consumption speed of the shared pool.
 */
static inline unsigned long
zicio_get_shared_pool_raw_expiration_time(zicio_descriptor *desc)
{
	return zicio_get_shared_pool_consumption_tsc_delta(desc);
}

/*
 * zicio_get_pagenum_in_a_jiffy
 *
 * Calculate the number of consumable pages per 1 jiffy using the average
 * consumption speed per page of the shared pool.
 */
unsigned int
zicio_get_pagenum_in_a_jiffy(zicio_descriptor *desc)
{
	unsigned long avg_tsc_delta =
			zicio_get_shared_pool_raw_expiration_time(desc);
	/* Get user consumption time. */
	ktime_t user_consumption_time = (avg_tsc_delta) ? zicio_tsc_to_ktime(
			avg_tsc_delta) : ZICIO_DEFAULT_EXP_TIME_IN_NANO;
	/* Get nanoseconds from a jiffy. */
	u64 nsecs_in_a_jiffy = zicio_get_nsecs_from_jiffy();

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	if (desc->channel_index == 2)
	{
		if (nsecs_in_a_jiffy < user_consumption_time)
			printk(KERN_WARNING "[ZICIO] zicio_get_pagenum_in_a_jiffy(), users are slower than jiffy,  nsecs_in_a_jiffy: %lld, avg user_consumption_time: %lld\n",
					nsecs_in_a_jiffy, user_consumption_time);
	}
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	return (nsecs_in_a_jiffy < user_consumption_time) ?
			1 : nsecs_in_a_jiffy / user_consumption_time + 1;
}

/*
 * zicio_reclaim_spcb
 *
 * Reclaim shared pages from shared pool
 */
void
zicio_reclaim_spcb(zicio_descriptor *desc, bool blocking)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local *zicio_shared_pool_local = 
				zicio_get_shared_pool_local(desc);
	zicio_attached_channel *zicio_channel = zicio_shared_pool_local->zicio_channel;
	zicio_shared_page_control_block **zicio_spcb, *cur_zicio_spcb;
	unsigned long current_jiffies = get_jiffies_64();
	unsigned int next_reclaim_start, start, end, id, i;
	bool update_set = true;

	/* Step 1. Reclaim contributed chunks and hand in it to shared pool */
	next_reclaim_start = start =
			atomic_read(&zicio_shared_pool_local->start_spcb_iter);
	end = atomic_read(&zicio_shared_pool_local->end_spcb_iter);

	/* Step 1-1. Checking spcb of contributed pages*/
	for (id = start ; id < end ; id++) {
		i = id & ~ZICIO_LOCAL_SPCB_MASK;

		/* Get the shared page control block matched with i. */
		cur_zicio_spcb =
			zicio_get_contribute_shared_page_control_block(desc, i);

		/* If we access freed spcb, then check next spcb directly. But,
		 * we can need to increment next reclaim start position. For handling
		 * this, we check this, and we increment id if we needed. */
		if (!cur_zicio_spcb) {
			if (update_set) {
				next_reclaim_start = id + 1;
			}
			continue;
		}

		BUG_ON(atomic64_read(&cur_zicio_spcb->zicio_spcb.reclaimer_jiffies));

		/*
		 * Set current jiffies to reclaimer jiffies.
		 */
		current_jiffies = get_jiffies_64();
		atomic64_set(&cur_zicio_spcb->zicio_spcb.reclaimer_jiffies, current_jiffies);

		mb();
		/*
		 * We need to checking if current shared page control block should be
		 * reclaimed. For these, checking 3 points.
		 * (1) lifetime is expired, (2) the reference counter is 0.
		 * (3) It is currently mapped.
		 */
		if ((atomic64_read(&cur_zicio_spcb->zicio_spcb.exp_jiffies) < current_jiffies)
				&& !(atomic_read(&cur_zicio_spcb->zicio_spcb.ref_count))
				&& (atomic_read(&cur_zicio_spcb->zicio_spcb.is_used))) {
			/* If the conditions are met, then we start reclaim page from
			 * shared pool. */

			/*
			 * (a) clear shared pool bitvector.
			 */
			zicio_clear_shared_pool_bitvector(desc, cur_zicio_spcb->chunk_id,
					cur_zicio_spcb, ZICIO_BIT_VALID|ZICIO_BIT_REF);
			mb();

			/*
			 * (b) Delete the spcb from rcu hash table.
			 */
			zicio_rcu_hash_del(zicio_shared_pool->shared_pool_hash,
					&cur_zicio_spcb->hash_node, cur_zicio_spcb->chunk_id + 1);

			/*
			 * (c) Clear shared page control block's shared information.
			 */
			atomic_set(&cur_zicio_spcb->zicio_spcb.is_shared, false);
			atomic_set(&cur_zicio_spcb->zicio_spcb.is_used, false);
			atomic64_set(&cur_zicio_spcb->zicio_spcb.exp_jiffies, 0);
			atomic_dec(&zicio_shared_pool_local->num_using_pages);
			atomic_dec(&zicio_shared_pool_local->num_shared_pages);

			/*
			 * (d) If we can increment contributed spcb's array then do it.
			 */
			if (update_set) {
				next_reclaim_start = id + 1;
			}
			/*
			 * (e) Clear shared page control block from per-channel spcb cache.
			 */
			zicio_clear_contribute_shared_page_control_block(desc, i);
			mb();

			/*
			 * (f) Lastly, return page id to queue.
			 */
			zicio_set_page_id_to_queue(desc,
					cur_zicio_spcb->zicio_spcb.local_page_idx);
			BUG_ON(atomic_read(
						&zicio_shared_pool_local->num_using_pages) < 0);

			PRINT_SPCB_DEBUG(cur_zicio_spcb->zicio_spcb, desc->cpu_id,
					__FILE__, __LINE__, __FUNCTION__);
		} else {
			update_set = false;
			atomic64_set(&cur_zicio_spcb->zicio_spcb.reclaimer_jiffies, 0);
			break;
		}
		mb();
		atomic64_set(&cur_zicio_spcb->zicio_spcb.reclaimer_jiffies, 0);
	}

	atomic_set(&zicio_shared_pool_local->start_spcb_iter, next_reclaim_start);

	/* If channel was derailed, then we need to check local page is consumed. */
	if (zicio_check_channel_derailed(desc) &&
		zicio_get_num_using_pages(desc)) {
		zicio_spcb = zicio_channel->local_zicio_spcb;

		for (i = 0 ; i < ZICIO_LOCAL_DATABUFFER_CHUNK_NUM ; i++) {
			if (!atomic_read(&zicio_spcb[i]->zicio_spcb.is_used)) {
				continue;
			}
			if (!(atomic_read(&zicio_spcb[i]->zicio_spcb.ref_count))) {
				mb();
				atomic_set(&zicio_spcb[i]->zicio_spcb.is_used, false);
				atomic_dec(&zicio_shared_pool_local->num_using_pages);
				BUG_ON(atomic_read(&zicio_shared_pool_local->num_using_pages) < 0);
			}
		}
	}
}

/*
 * zicio_destroy_alloced_spcb
 *
 * Fetch and destroy one shared's spcb
 */
int
zicio_destroy_alloced_spcb(zicio_descriptor *desc)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_page_control_block *zicio_spcb;
	unsigned int pos = -1;
	int page_id;

	page_id  = zicio_get_page_id_from_queue(desc, &pos, false);

	while (page_id == ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX) {
		page_id = zicio_read_page_id_from_queue(desc, pos);
	}

	zicio_spcb = zicio_get_spcb_with_id(desc, page_id);
	zicio_set_spcb_with_id(desc, NULL, page_id);

	zicio_destroy_spcb(zicio_shared_pool, zicio_spcb);
	atomic_dec(&zicio_shared_pool->num_spcb);

	return page_id;
}

/*
 * zicio_check_set_internal_bitvector
 *
 * Deciding whether the internal bitvector should be set or not
 */
bool
zicio_check_set_internal_bitvector(zicio_bitvector_t **bit_vector,
			unsigned *length, unsigned int chunk_id, bool shared)
{
	unsigned int start, end, i;
	int shared_depth0 = (int)shared;

	BUG_ON(shared_depth0 > 1);
	BUG_ON(length == NULL);

	/* Determine the start and end values of the leaf bitvector to be checked */
	start = (chunk_id >>
			(ZICIO_INTERNAL_BIT_COVER_SHIFT - shared_depth0)) <<
					ZICIO_BYTE_MIN_ORDER;
	end = ((chunk_id >>
			(ZICIO_INTERNAL_BIT_COVER_SHIFT - shared_depth0)) + 1) <<
					ZICIO_BYTE_MIN_ORDER;

	end = (end > DIV_ROUND_UP(length[0], sizeof(unsigned long))) ?
				DIV_ROUND_UP(length[0], sizeof(unsigned long)) : end;

	BUG_ON(bit_vector == NULL);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "zicio_check_set_internal_bitvector, start: %d,"
			" end: %d [%s:%d][%s]\n", start, end, __FILE__, __LINE__,
			__FUNCTION__);
#endif

	/* Read the leaf bitvector and check its status */
	for (i = start ; i < end ; i++) {
		if (~atomic64_read(zicio_get_bitvector(bit_vector, 0, i))) {
			return false;
		}
	}

	return true;
}

/*
 * __zicio_clear_bitvector_atomic
 *
 * Turn off the bit at the position corresponding to bit idx.
 */
bool
__zicio_clear_bitvector_atomic(zicio_bitvector *zicio_bitvector, int depth,
			unsigned int bit_idx, unsigned long flag, bool shared)
{
	unsigned int in_bitvector_offset;
	unsigned int in_byte_offset;
	unsigned long bit;
	atomic64_t *bit_vector;
	bool use_2bit = ((shared && depth == 0) || (!shared && depth == 1));
	BUG_ON(flag >= 4);

	in_bitvector_offset = zicio_get_in_bitvector_offset(bit_idx, use_2bit);
	in_byte_offset = zicio_get_in_byte_offset(bit_idx, use_2bit);
	bit = (use_2bit) ? flag << in_byte_offset : 1UL << in_byte_offset;
	bit_vector = zicio_get_bitvector(zicio_bitvector->bit_vector, depth,
				in_bitvector_offset);

	return (atomic64_fetch_and(~bit, bit_vector) & (~bit));
}

/*
 * zicio_set_bitvector_atomic
 *
 * Atomically set the bits of the bitvector
 */
static bool
zicio_set_bitvector_atomic(zicio_bitvector *zicio_bitvector, int depth,
			unsigned int bit_idx, unsigned long flag, bool shared)
{
	unsigned int in_bitvector_offset;
	unsigned int in_byte_offset;
	unsigned long bit;
	atomic64_t *bit_vector;
	bool shared_depth0 = (shared && depth == 0);
	bool local_depth1 = (!shared && depth == 1);

	BUG_ON(flag >= 4);

	in_bitvector_offset = zicio_get_in_bitvector_offset(bit_idx,
				shared_depth0|local_depth1);
	in_byte_offset = zicio_get_in_byte_offset(bit_idx,
				shared_depth0|local_depth1);
	bit = (shared_depth0|local_depth1) ? flag << in_byte_offset :
				1UL << in_byte_offset;
	bit_vector = zicio_get_bitvector(zicio_bitvector->bit_vector, depth,
				in_bitvector_offset);

	atomic64_or(bit, bit_vector);
	return true;
}

/*
 * zicio_check_and_set_leaf_internal_bitvector
 *
 * Checking leaf internal bitvector can turn on. If we can trun on one bit
 * of internal bitvector, then turn on the internal bitvector.
 *
 * @zicio_bitvector: local bitvector pointer
 * @bit_idx: leaf bit number turned on.
 */
bool
zicio_check_and_set_leaf_internal_bitvector(
		zicio_bitvector *zicio_bitvector, unsigned int bit_idx, bool derailed)
{
	/* if depth of bitvector is more than one and all covered area of local
	 * bitvector is turned on, then we can turn on internal bitvector. */
	if (zicio_bitvector->depth > 1 &&
		zicio_check_set_internal_bitvector(zicio_bitvector->bit_vector,
			zicio_bitvector->bitvector_length, bit_idx, false)) {
		/* Turn on a bit of internal bitvector */
		zicio_set_bitvector_atomic(zicio_bitvector, 1,
					bit_idx >> ZICIO_INTERNAL_BIT_CHUNK_COVER_ORDER,
					ZICIO_BIT_PREMAP, false);

		if (derailed) {
			__zicio_clear_bitvector_atomic(zicio_bitvector, 1,
						bit_idx >> ZICIO_INTERNAL_BIT_CHUNK_COVER_ORDER,
						ZICIO_BIT_FORCEFUL_UNMAP, false);
		}
		return true;
	}
	return false;
}

/*
 * zicio_test_and_set_leaf_internal_bitvector
 *
 * Test and set leaf bitvector atomically.
 *
 * @zicio_bitvector: leaf bitvector to access
 * @bit_idx: leaf bit number to access
 */
static int
zicio_test_and_set_bitvector_atomic(zicio_bitvector *zicio_bitvector,
			unsigned int bit_idx, bool derailed)
{
	unsigned int in_bitvector_offset;
	unsigned int in_byte_offset;
	int already_turned_on;
	atomic64_t *bit_vector;

	/* Get the offset in overall bitvectors */
	in_bitvector_offset = zicio_get_in_bitvector_offset(bit_idx, false);
	/* Get the offset in one bitvector element(8bytes) */
	in_byte_offset = zicio_get_in_byte_offset(bit_idx, false);
	/* Get a bitvector element */
	bit_vector = zicio_get_bitvector(zicio_bitvector->bit_vector, 0,
				in_bitvector_offset);

	/* Test and set a bit by using the in-byte offset. */
	already_turned_on = test_and_set_bit(in_byte_offset,
			(volatile unsigned long *)bit_vector);

	/* If other workers already turned on the bit, just return the result. */
	if (already_turned_on) {
		return already_turned_on;
	}

	mb();
	/* If this worker turned on a bit, then check and set the internal
	   bitvector */
	zicio_check_and_set_leaf_internal_bitvector(
			zicio_bitvector, bit_idx, derailed);
	return already_turned_on;
}

/*
 * zicio_test_and_clear_bitvector_atomic
 *
 * Test and set leaf bitvector atomically.
 *
 * @zicio_bitvector: leaf bitvector to access
 * @bit_idx: leaf bit number to access
 */
int
zicio_test_and_clear_bitvector_atomic(zicio_bitvector *zicio_bitvector,
			unsigned int bit_idx)
{
	unsigned int in_bitvector_offset;
	unsigned int in_byte_offset;
	unsigned int bit_idx_level_1;
	bool old_bit;
	atomic64_t *bit_vector;

	/* Get the offset in overall bitvectors */
	in_bitvector_offset = zicio_get_in_bitvector_offset(bit_idx, false);
	/* Get the offset in one bitvector element(8bytes) */
	in_byte_offset = zicio_get_in_byte_offset(bit_idx, false);
	/* Get a bitvector element */
	bit_vector = zicio_get_bitvector(zicio_bitvector->bit_vector, 0,
				in_bitvector_offset);

	/* Test and set a bit by using the in-byte offset. */
	if (!(old_bit = test_and_clear_bit(in_byte_offset,
			(volatile unsigned long *)bit_vector))) {
		return old_bit;
	}

	/* One bit of internal bitvector is turned on, only when all bits of
	 * coverage area are turned on. If we clear one bits, then turn off it. */
	bit_idx_level_1 = bit_idx >> ZICIO_INTERNAL_BIT_CHUNK_COVER_ORDER;
	zicio_set_bitvector_atomic(zicio_bitvector, 1, bit_idx_level_1,
			ZICIO_BIT_FORCEFUL_UNMAP, false);

	return old_bit;
}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
unsigned int
zicio_traverse_all_local_bitvector(zicio_descriptor *desc,
			zicio_shared_pool_local *zicio_shared_pool_local,
			unsigned int total_chunk_numbers)
{
	zicio_bitvector *zicio_local_bitvector;
	unsigned long *bitvector;
	unsigned int bitvector_id, bitvector_size, leaf_idx;
	unsigned int leaf_max = ((total_chunk_numbers - 1) >>
			ZICIO_BIT_TO_ULONG_SHIFT) + 1;

	zicio_local_bitvector = zicio_get_local_bitvector(desc);

	for (bitvector_id = 0 ; bitvector_id < leaf_max ; bitvector_id++) {
		bitvector = zicio_get_bitvector_ul(
				zicio_local_bitvector->debug_bit_vector, 0, bitvector_id);
		bitvector_size = zicio_get_last_leaf_bitvector_size(bitvector_id,
				total_chunk_numbers);

		while ((leaf_idx = find_first_zero_bit(bitvector, bitvector_size) !=
					bitvector_size)) {
			printk(KERN_WARNING "[ZICIO] Unread chunk id :%u\n",
					ZICIO_GET_ULONG_TO_BIT(bitvector_id) + leaf_idx);
			test_and_set_bit(leaf_idx, (volatile unsigned long *)bitvector);
		}
	}

	for (bitvector_id = 0 ; bitvector_id < leaf_max ; bitvector_id++) {
		bitvector = zicio_get_bitvector_ul(
				zicio_local_bitvector->debug_bit_vector, 1, bitvector_id);
		bitvector_size = zicio_get_last_leaf_bitvector_size(bitvector_id,
				total_chunk_numbers);

		while ((leaf_idx = find_first_zero_bit(bitvector, bitvector_size) !=
					bitvector_size)) {
			printk(KERN_WARNING "[ZICIO] Unread chunk id :%u\n",
					ZICIO_GET_ULONG_TO_BIT(bitvector_id) + leaf_idx);
			test_and_set_bit(leaf_idx, (volatile unsigned long *)bitvector);
		}
	}

	return ZICIO_LOCAL_BITVECTOR_COMPLETE;
}
#endif

/*
 * zicio_find_unread_chunk_area
 *
 * Find an internal bit covered area that can has unread chunks.
 *
 * @bitvector: bitvector pointer
 * @bit_start: start of bitvector
 * @bitvector_size: size of bitvector
 */
unsigned int
zicio_find_unread_chunk_area(atomic64_t *bitvector,
			unsigned int bit_start, unsigned int bitvector_size)
{
	BUG_ON(bitvector_size > ZICIO_GET_ULONG_TO_BIT(1UL));

	/*
	 * To check the unread chunk, internal bits are checked by 2 bits,
	 * and the leaf bitvector is checked based on this.
	 */
	while (bit_start < bitvector_size) {
		/*
		 * Only when no bits are cleared and all bits are premapped once, it can
		 * be confirmed that the areas of leaf bits covered by internal bits are
		 * safely used.
		 */
		if (ZICIO_GET_LOCAL_INTERNAL_BITMAP_FLAG(atomic64_read(bitvector),
				bit_start) != ZICIO_BIT_PREMAP) {
			return bit_start;
		}
		bit_start += 2;
	}

	return bit_start;
}

/*
 * zicio_traverse_local_internal_bitvector
 *
 * Traverse local internal bitvector and return the location of first 0 bit
 */
unsigned int
zicio_traverse_local_internal_bitvector(zicio_descriptor *desc,
			zicio_shared_pool_local *zicio_shared_pool_local,
			unsigned int total_chunk_nums)
{
	zicio_bitvector *zicio_local_bitvector;
	atomic64_t *bitvector;
	unsigned int start, internal_max;
	unsigned int bitvector_id, internal_num;
	unsigned int bitvector_size;

	zicio_local_bitvector = zicio_get_local_bitvector(desc);

	/*
	 * Calculate the bitmap search start point and maximum value in units of 64
	 * bits.
	 */
	start = zicio_shared_pool_local->consume_indicator.current_chunk_id_mod >>
		(ZICIO_INTERNAL_BITVECTOR_IDX_SHIFT - 1);
	internal_max = ((total_chunk_nums - 1) >>
			(ZICIO_INTERNAL_BITVECTOR_IDX_SHIFT - 1)) + 1;
	/*
	 * By traversing the internal bitvector, find the first search position.
	 */
	for (bitvector_id = start ; bitvector_id < internal_max ; bitvector_id++) {
		bitvector = zicio_get_bitvector(
			zicio_local_bitvector->bit_vector, 1, bitvector_id);
		/*
		 * Get the current size of bitvector.
		 */
		bitvector_size = zicio_get_last_internal_bitvector_size(
				ZICIO_GET_CHUNKNUM_FROM_INTERNAL_BIT(bitvector_id),
						total_chunk_nums);
		/*
		 * Find internal bit number for checking area
		 */
		internal_num = zicio_find_unread_chunk_area(bitvector,
				0, bitvector_size);

		if (bitvector_size != internal_num) {
			break;
		}
	}

	/*
	 * Not all data has been supplied to the user yet, but all local internal
	 * bitvectors are turned on.
	 */
	if (bitvector_id == internal_max) {
		return ZICIO_LOCAL_BITVECTOR_COMPLETE;
	}

	/*
	 * Using the location of the obtained internal bitvector, check the chunk id
	 * of the local bitvector to start searching
	 */
	return zicio_get_chunk_id_from_internal_bitvector(bitvector_id,
				internal_num);
}


/*
 * zicio_traverse_local_leaf_bitvector
 *
 * Traverse local leaf bitvector and return the id of unread file chunk.
 */
static unsigned int
zicio_traverse_local_leaf_bitvector(zicio_descriptor *desc,
			zicio_shared_pool_local *zicio_shared_pool_local,
			unsigned int leaf_start_chunk, unsigned int total_chunk_nums)
{
	zicio_bitvector *zicio_local_bitvector;
	unsigned long *bitvector;
	unsigned int start, leaf_max, bitvector_size;
	unsigned int bitvector_id, leaf_idx;

	zicio_local_bitvector = zicio_get_local_bitvector(desc);

	start = ZICIO_GET_BIT_TO_ULONG(leaf_start_chunk);
	leaf_max = zicio_get_max_leaf_num_in_one_internal_bit(leaf_start_chunk,
			total_chunk_nums);

	/*
	 * After obtaining the bit vector area to be read from the internal
	 * bitvector, traversing this area to find the off bit.
	 */
	for (bitvector_id = start ; bitvector_id < start + leaf_max ;
				bitvector_id++) {
		bitvector = zicio_get_bitvector_ul(
					zicio_local_bitvector->bit_vector, 0, bitvector_id);
		bitvector_size = zicio_get_last_leaf_bitvector_size(bitvector_id,
					total_chunk_nums);

		leaf_idx = find_first_zero_bit(bitvector, bitvector_size);

		if (bitvector_size != leaf_idx) {
			break;
		}
	}

	/*
	 * A situation where all local bitvectors are turned on even though the
	 * internal bitvector is turned off.
	 */
	if (bitvector_id == start + leaf_max) {
		return ZICIO_LOCAL_BITVECTOR_COMPLETE;
	}

	/* Calculate the chunk id. */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "Channel[%d] read after derailed: %d", desc->cpu_id,
			ZICIO_GET_ULONG_TO_BIT(bitvector_id) + leaf_idx);
#endif

	return ZICIO_GET_ULONG_TO_BIT(bitvector_id) + leaf_idx;
}

/*
 * zicio_get_next_unread_chunk_id_shared
 *
 * The chunk id to be read next is obtained from the local bitvector.
 */
unsigned int
zicio_get_next_unread_chunk_id_shared(zicio_descriptor *desc,
			zicio_shared_page_control_block *zicio_spcb)
{
	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool_local *zicio_shared_pool_local;
	zicio_bitvector *zicio_local_bitvector;
	unsigned int leaf_start_chunk;
	unsigned int chunk_id;
	unsigned int total_chunk_nums;

	zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);
	zicio_local_bitvector = zicio_get_local_bitvector(desc);

get_next_unread_file_chunk_id_retry:
	total_chunk_nums = zicio_shared_pool->shared_files.total_chunk_nums;

	/*
	 * If the page provided to the user is equal to the total chunk number,
	 * no more chunks are issued.
	 */
	if (atomic_read(&zicio_shared_pool_local->num_mapped) == total_chunk_nums) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
		zicio_traverse_all_local_bitvector(desc, zicio_shared_pool_local,
				total_chunk_nums);
#endif
		return ZICIO_LOCAL_BITVECTOR_COMPLETE;
	}

	/*
	 * Using the location of the obtained internal bitvector, check the chunk id
	 * of the local bitvector to start searching
	 */
	if ((leaf_start_chunk = zicio_traverse_local_internal_bitvector(desc,
				zicio_shared_pool_local, total_chunk_nums)) ==
						ZICIO_LOCAL_BITVECTOR_COMPLETE) {
		BUG_ON(atomic_read(&zicio_shared_pool_local->num_mapped) !=
				total_chunk_nums);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
		zicio_traverse_all_local_bitvector(desc, zicio_shared_pool_local,
				total_chunk_nums);
#endif
		return ZICIO_LOCAL_BITVECTOR_COMPLETE;
	} else {
		/* Calculate the chunk id. */
		chunk_id = zicio_traverse_local_leaf_bitvector(desc,
				zicio_shared_pool_local, leaf_start_chunk, total_chunk_nums);
	}

	if (chunk_id == ZICIO_LOCAL_BITVECTOR_COMPLETE) {
		goto get_next_unread_file_chunk_id_retry;
	}

	/* Set local bitvector */
	if (zicio_test_and_set_bitvector_atomic(
			zicio_local_bitvector, chunk_id, true)) {
		goto get_next_unread_file_chunk_id_retry;
	}

	mb();

	zicio_spcb->chunk_id = chunk_id;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	zicio_spcb->zicio_spcb.file_chunk_id = chunk_id;
#endif /*CONFIG_ZICIO_DEBUG */
	zicio_shared_pool_local->consume_indicator.current_chunk_id_mod = chunk_id;
	zicio_inc_mapped_chunks_num(desc);

	return chunk_id;
}

/*
 * zicio_get_chunk_id_from_shared_pool
 *
 * Get the next file chunk id for read from the shared pool
 *
 * @desc: zicio channel descriptor
 * @zicio_spcb: zicio shared page control block pointer
 * @cur_head_no_mod: current head ID
 */
unsigned int 
zicio_get_next_file_chunk_id_from_shared_pool(zicio_descriptor *desc,
		zicio_shared_page_control_block *zicio_spcb,
		unsigned int *cur_head_no_mod)
{
	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool_local *zicio_shared_pool_local;
	unsigned long cur_head;
	unsigned int in_bitvector_offset;
	unsigned int in_byte_offset;
	atomic64_t *cur_bitmap;
	unsigned long cur_bitmap_value;

	zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);

	if (atomic_read(&zicio_shared_pool_local->num_mapped) ==
				zicio_shared_pool->shared_files.total_chunk_nums) {
		return UINT_MAX;
	}

get_next_file_chunk_id_retry:
	/* Get current head */
	cur_head = zicio_get_head_chunk_id_from_shared_pool(desc);
	if (unlikely(cur_head_no_mod)) {
		*cur_head_no_mod = cur_head;
	}

	if (unlikely(zicio_check_channel_derailed(desc))) {
#ifdef CONFIG_ZICIO_STAT
		zicio_set_derailing_to_stat(desc);
#endif /* #if (CONFIG_ZICIO_STAT) */
		return -2;
	}

	cur_head %= zicio_shared_pool->shared_files.total_chunk_nums;

	if (zicio_test_and_set_bitvector_atomic(
			&zicio_shared_pool_local->local_bitvector, cur_head, false)) {
		goto get_next_file_chunk_id_retry;
	}

	/*
	 * Get bit status of shared pool
	 */
	/* Get in-bitvector offset and in-byte offset */
	in_bitvector_offset = zicio_get_in_bitvector_offset(cur_head,
				true);
	in_byte_offset = zicio_get_in_byte_offset(cur_head, true);
	cur_bitmap = zicio_get_bitvector(zicio_shared_pool->bitvector.bit_vector, 0,
				in_bitvector_offset);
	cur_bitmap_value = atomic64_read(cur_bitmap);

	/* Set reference count for read */
	if (test_and_set_bit(in_byte_offset,
				(volatile unsigned long *)cur_bitmap)) {
		zicio_test_and_clear_bitvector_atomic(
				&zicio_shared_pool_local->local_bitvector, cur_head);
		/* 
		 * Referred by others already
		 * Case 1) Wrap-around
		 * Case 2) All file chunks are cached in pool.
		 * In these case, retry get head
		 */
		goto get_next_file_chunk_id_retry;
	}

	zicio_set_last_io_file_chunk_id(desc, cur_head);
	mb();

	/* Increase the number of mapped channel for user */
	zicio_inc_mapped_chunks_num(desc);

	/* Update shared page control block */
	zicio_spcb->chunk_id = cur_head;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	zicio_spcb->zicio_spcb.file_chunk_id = cur_head;
#endif /* CONFIG_ZICIO_DEBUG */

	return cur_head;
}

/*
 * zicio_get_init_chunk_id
 *
 * Get an init file chunk id and set shared page control block
 */
unsigned int
zicio_get_init_chunk_id(zicio_descriptor *desc, int local_page_idx)
{
	unsigned int chunk_id, chunk_id_no_mod;
	zicio_shared_page_control_block *zicio_spcb =
			zicio_get_spcb_with_id(desc, local_page_idx);

	BUG_ON(!desc->zicio_shared_pool_desc);
	BUG_ON(zicio_check_channel_derailed(desc));

	chunk_id = zicio_get_next_file_chunk_id_from_shared_pool(
				desc, zicio_spcb, &chunk_id_no_mod);
	zicio_set_consume_indicator(desc, chunk_id_no_mod);

	PRINT_SPCB_DEBUG(zicio_spcb->zicio_spcb, desc->cpu_id, __FILE__, __LINE__,
				__FUNCTION__);

	return chunk_id;
}

/*
 * zicio_clear_bitvector_atomic
 *
 * Turn off the bit at the position corresponding to chunk id.
 */
bool
zicio_clear_bitvector_atomic(zicio_bitvector *zicio_bitvector,
			unsigned int chunk_id, unsigned flag, bool shared)
{
	unsigned int bit_idx_level_1;
	BUG_ON(flag >= 4);

	if (!shared) {
		bit_idx_level_1 = chunk_id >> ZICIO_INTERNAL_BIT_CHUNK_COVER_ORDER;
		__zicio_clear_bitvector_atomic(zicio_bitvector, 0, chunk_id, flag,
					shared);
		zicio_set_bitvector_atomic(zicio_bitvector, 1, bit_idx_level_1,
				ZICIO_BIT_FORCEFUL_UNMAP, false);
		return true;
	}

	/* If the 8byte of level 0 is not 0, we don't have to clear level 1 */
	if (__zicio_clear_bitvector_atomic(zicio_bitvector, 0, chunk_id, flag,
				shared)) {
		/*
		 * Currently, access to the shared bitvector is performed without any
		 * traverse. Access to the bitvector is performed with the chunk id
		 * already acquired. In other words, turning off and turning on the
		 * level 1 bitvector of the shared bitvector is additional management
		 * for unused logic.
		 */
		return false;
	}

	return true;
}

/*
 * zicio_set_shared_pool_bitvector
 *
 * Turn on the bit of shared pool bitvector at the position corresponding to
 * chunk id.
 */
void
zicio_set_shared_pool_bitvector(zicio_descriptor *desc, int chunk_id,
			zicio_shared_page_control_block *zicio_spcb, unsigned long flag)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_set_bitvector_atomic(&zicio_shared_pool->bitvector, 0, chunk_id,
			flag, true);
}
EXPORT_SYMBOL(zicio_set_shared_pool_bitvector);

/*
 * zicio_clear_shared_pool_bitvector
 *
 * Turn off the bit of shared pool bitvector at the position corresponding to
 * chunk id.
 */
void
zicio_clear_shared_pool_bitvector(zicio_descriptor *desc, int chunk_id,
			zicio_shared_page_control_block *zicio_spcb, unsigned long flag)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);

	zicio_clear_bitvector_atomic(&zicio_shared_pool->bitvector,
				chunk_id, flag, true);
}
EXPORT_SYMBOL(zicio_clear_shared_pool_bitvector);

/*
 * zicio_create_shared_pool_local
 *
 * Creating shared pool local info for zicio
 */
zicio_shared_pool_local *
zicio_create_shared_pool_local(zicio_descriptor *desc,
			zicio_shared_pool *zicio_shared_pool)
{
	zicio_shared_pool_local *zicio_shared_pool_local;
	zicio_bitvector *zicio_local_bitvector;
	zicio_attached_channel *zicio_channel;
	int channel_id;

	/*
	 * Allocate zicio_shared_pool struct
	 */
	zicio_shared_pool_local = zicio_allocate_shared_pool_local();

	if (unlikely(!zicio_shared_pool_local)) {
		printk("[ZICIO] Error in local shared pool structure allocation\n");
		return NULL;
	}

	zicio_local_bitvector = &(zicio_shared_pool_local->local_bitvector);
	if (unlikely(!zicio_allocate_and_initialize_bitvector(
			zicio_local_bitvector, zicio_shared_pool->shared_files.total_chunk_nums,
					false))) {
		printk("[ZICIO] Error in local bitvector allocation\n");
		goto l_error_shared_pool_local_alloc;
	}

	/*
	 * Initialize variables related with contributed spcbs.
	 */
	zicio_shared_pool_local->contribute_zicio_spcb =
			(atomic64_t *)page_to_virt(
					alloc_page(GFP_KERNEL|__GFP_ZERO));

	if (unlikely(!zicio_shared_pool_local->contribute_zicio_spcb)) {
		printk("[ZICIO] Error in contribute spcb array allocation\n");
		goto l_error_contribute_zicio_spcb_alloc;
	}

	spin_lock_init(&zicio_shared_pool_local->reclaimer_guard);

	/* Allocate page for shared page control block back pointer */
	zicio_shared_pool_local->zicio_spcb_arrays =
			(zicio_shared_page_control_block**)page_to_virt(
					alloc_page(GFP_KERNEL|__GFP_ZERO));

	if (unlikely(!zicio_shared_pool_local->zicio_spcb_arrays)) {
		printk("[ZICIO] Error in premapped spcb array allocation\n");
		goto l_error_zicio_spcb_arrays_alloc;
	}

	/* Allocate a page for ingestion point track array */
	zicio_shared_pool_local->ingestion_point_track_array =
			(atomic64_t *)page_to_virt(alloc_page(GFP_KERNEL|__GFP_ZERO));

	if (unlikely(!zicio_shared_pool_local->ingestion_point_track_array)) {
		printk("[ZICIO] Error in ingestion point track array allocation\n");
		goto l_error_ingestion_point_track_array_alloc;
	}
	memset(zicio_shared_pool_local->ingestion_point_track_array, 0xFF,
			ZICIO_PAGE_SIZE);

	/*
	 *  Allocate and initialize attached channel's page parameters
	 */
	zicio_channel = zicio_allocate_and_initialize_channel_shared(desc);

	if (unlikely(IS_ERR_OR_NULL(zicio_channel))) {
		printk("[ZICIO] Error in attached channel allocation\n");
		goto l_error_in_channel_alloc;
	}

	zicio_shared_pool_local->zicio_channel = zicio_channel;
	zicio_shared_pool_local->consume_indicator.start_chunk_id_no_mod = UINT_MAX;

	/* Initialize attached channel's mapping inforamation */
	zicio_channel->desc = desc;
	atomic_set(&zicio_channel->previous_low_premap_point, UINT_MAX);
	atomic_set(&zicio_channel->previous_high_premap_point, UINT_MAX);

	channel_id = zicio_get_unused_attached_channel_id(
			&zicio_shared_pool->zicio_channels);
	zicio_shared_pool_local->channel_id = channel_id;
	mb();
	zicio_install_attached_channel(&zicio_shared_pool->zicio_channels, channel_id,
				zicio_channel);

	return zicio_shared_pool_local;

l_error_in_channel_alloc:
	free_page((unsigned long)zicio_shared_pool_local->ingestion_point_track_array);
l_error_ingestion_point_track_array_alloc:
	free_page((unsigned long)zicio_shared_pool_local->zicio_spcb_arrays);
l_error_zicio_spcb_arrays_alloc:
	free_page((unsigned long)zicio_shared_pool_local->contribute_zicio_spcb);
l_error_contribute_zicio_spcb_alloc:
	zicio_delete_zicio_bitvector(zicio_local_bitvector);
l_error_shared_pool_local_alloc:
	zicio_free_shared_pool_local(zicio_shared_pool_local);	
	return NULL;
}

/*
 * zicio_get_raw_avg_user_ingestion_point
 *
 * Calculate the average of raw user ingestion point
 *
 * @zicio_channel_args: local channel's information
 * @ret: user ingestion point information
 */
static void
__zicio_get_raw_avg_user_ingestion_point(void *zicio_channel_args, void *ret)
{
	zicio_attached_channel *zicio_channel = zicio_channel_args;
	zicio_descriptor *desc = zicio_channel->desc;
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local *zicio_shared_pool_local =
				zicio_get_shared_pool_local(desc);
	zicio_raw_user_ingestion_sum *zicio_ingestion_point = ret;
	unsigned int file_chunk_id;
	unsigned int forcefully_unmapped_chunk_id;
	int current_user_buffer_idx_no_mod = zicio_get_user_buffer_idx(desc);
	int current_user_buffer_idx, idx_distance;

	BUG_ON(!desc);

	/*
	 * If this channel was derailed then, just return.
	 */
	if (zicio_check_channel_derailed(desc) ||
		current_user_buffer_idx_no_mod == INT_MAX) {
		return;
	}

	/*
	 * Checking other channel's file chunk id using the number of channels.
	 */
	current_user_buffer_idx = current_user_buffer_idx_no_mod %
			ZICIO_MAX_NUM_GHOST_ENTRY;
	zicio_get_tracking_ingestion_point(desc, current_user_buffer_idx,
			&file_chunk_id, &idx_distance);
	forcefully_unmapped_chunk_id = atomic_read(
			&zicio_channel->last_forcefully_unmapped_file_chunk_id);

	if (file_chunk_id == UINT_MAX) {
		return;
	}

	BUG_ON(idx_distance == UINT_MAX);
	/*
	 *  Get the monotonic chunk ID by using user consumption point.
	 */
	file_chunk_id = zicio_convert_chunk_id_to_monotonic_id(
			zicio_shared_pool, zicio_shared_pool_local, file_chunk_id);

	zicio_ingestion_point->nr_channels++;

	if (zicio_ingestion_point->current_head < file_chunk_id + idx_distance) {
		zicio_ingestion_point->current_ingestion_point_sum_no_mod +=
			((forcefully_unmapped_chunk_id == UINT_MAX) ||
			 (forcefully_unmapped_chunk_id < zicio_ingestion_point->current_head)) ?
				zicio_ingestion_point->current_head : forcefully_unmapped_chunk_id;
	} else {
		zicio_ingestion_point->current_ingestion_point_sum_no_mod +=
			((forcefully_unmapped_chunk_id == UINT_MAX) ||
			 (forcefully_unmapped_chunk_id < file_chunk_id + idx_distance)) ?
				file_chunk_id + idx_distance : forcefully_unmapped_chunk_id;
	}
}

/*
 * zicio_get_raw_avg_user_ingestion_point
 *
 * Get user ingestion point. For this, read directly channels' switch board.
 */
static unsigned long
zicio_get_raw_avg_user_ingestion_point(zicio_descriptor *desc,
		int *nr_channels, unsigned int current_head)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_raw_user_ingestion_sum zicio_ingestion_point = {0, 0, current_head};

	/*
	 * Iterate channels as many as registered channels and calculate average
	 * user ingestion point.
	 */
	zicio_iterate_all_zicio_struct_with_ret(&zicio_shared_pool->zicio_channels,
			__zicio_get_raw_avg_user_ingestion_point, &zicio_ingestion_point,
			atomic_read(&zicio_shared_pool->pin) - 1, false);

	if (!zicio_ingestion_point.nr_channels) {
		return 0;
	}

	*nr_channels = zicio_ingestion_point.nr_channels;
	return zicio_ingestion_point.current_ingestion_point_sum_no_mod;
}

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
/*
 * Workhorse for the zicio_get_avg_user_ingestion_monotonic_chunk_id().
 *
 * If you want to know where this function is called, check the
 * zicio_iterate_all_zicio_struct_with_ret() and its callers.
 */
static void
__zicio_get_avg_user_ingestion_monotonic_chunk_id(
		void *zicio_channel_args, void *ret)
{
	zicio_attached_channel *zicio_channel = zicio_channel_args;
	zicio_descriptor *zicio_desc = zicio_channel->desc;
	zicio_shared_pool_local *zicio_shared_pool_local =
				zicio_get_shared_pool_local(zicio_desc);
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(zicio_desc);
	zicio_raw_user_ingestion_sum *zicio_ingestion_point = ret;
	int current_user_buffer_idx = zicio_get_user_buffer_idx(zicio_desc);
	unsigned int ingestion_monotonic_chunk_id = UINT_MAX;
	unsigned int last_forcefully_unmapped_monotonic_chunk_id = UINT_MAX;
	zicio_switch_board *sb = zicio_desc->switch_board;
	unsigned int avg_consumable_chunk_num_in_jiffy
		= zicio_ingestion_point->avg_consumable_chunk_num_in_jiffy;
	unsigned int watermark
		= zicio_calc_io_watermark_shared(avg_consumable_chunk_num_in_jiffy);

	BUG_ON(!zicio_desc);

	if (zicio_check_channel_derailed(zicio_desc))
		return;

	/*
	 * If this channel fully ingested all chunk, do not consider it.
	 *
	 * For example, when reading the same table twice in DBMS, some process can
	 * attach a new channel without detaching the existing channel even after
	 * the first reading is completed.
	 *
	 * If the first channel's ingestion point is still included in the average
	 * calculation, eventually the ingestion point will stop. This increases the
	 * distance from head and eventually causes I/O to stall.
	 */
	if (atomic_read(&zicio_shared_pool_local->num_mapped)
			>= (uint64_t) zicio_shared_pool->shared_files.total_chunk_nums)
		return;

	
	zicio_ingestion_point->nr_channels++;

	/* 
	 * If the user does not get the chunk yet, we can't know the ingestion
	 * point. So use the head as a ingestion point temporarily.
	 */
	if (current_user_buffer_idx == INT_MAX) {
		zicio_ingestion_point->current_ingestion_point_sum_no_mod
			+= zicio_ingestion_point->current_head;
		return;
	}

	ingestion_monotonic_chunk_id
		= zicio_get_tracking_ingestion_monotonic_chunk_id(
			zicio_desc, current_user_buffer_idx);

	last_forcefully_unmapped_monotonic_chunk_id
		= atomic_read(&zicio_channel->last_forcefully_unmapped_monotonic_chunk_id);

	/*
	 * If the user intentionally stops, we have to use alternative monotonic
	 * chunk id to prevent the I/O stalls.
	 *
	 * However, since we cannot know whether the user stopped intentionally, we
	 * use the larger value of the user's ingestion point and alternative chunk
	 * ids (forceful unmapped chunk id).
	 */
	if ((last_forcefully_unmapped_monotonic_chunk_id == UINT_MAX) ||
		(last_forcefully_unmapped_monotonic_chunk_id
			< ingestion_monotonic_chunk_id)) {
		/*
		 * If the number of consumed chunk is same with number of mapped chunks,
		 * it means that there is no more premapped chunks.
		 *
		 * If the user intentionally stops, the location is calculated using the
		 * forceful unmapped chunk id instead, but if there is no premapped chunk,
		 * there is not even a forceful unmapped chunk.
	  	 *
		 * XXX comment must be added... what the alternative value maens...
		 */
		if (atomic_read(&zicio_shared_pool_local->num_mapped)
				== sb->nr_consumed_chunk) {
			if (zicio_ingestion_point->current_head - watermark
					< ingestion_monotonic_chunk_id) {
				zicio_ingestion_point->current_ingestion_point_sum_no_mod
					+= ingestion_monotonic_chunk_id;
			} else {
				zicio_ingestion_point->current_ingestion_point_sum_no_mod
					+= zicio_ingestion_point->current_head - watermark;
			}

			return;
		}

		zicio_ingestion_point->current_ingestion_point_sum_no_mod
			+= ingestion_monotonic_chunk_id;
	} else {
		zicio_ingestion_point->current_ingestion_point_sum_no_mod
			+= last_forcefully_unmapped_monotonic_chunk_id;
	}
}

/**
 * zicio_get_avg_user_ingestion_monotonic_chunk_id
 * @zicio_desc: zicio descriptor
 * @current_head: current head of monotonic chunk id
 *
 * Existing function, zicio_get_raw_avg_user_ingestion_point() does not use
 * user's exact ingestion point and does not returns average user point, just
 * return summation.
 *
 * So use the exact chunk id to calculate average user ingestion point and
 * return the average point.
 */
static unsigned int
zicio_get_avg_user_ingestion_monotonic_chunk_id(
	zicio_descriptor *zicio_desc, unsigned int current_head)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(zicio_desc);
	unsigned int avg_consumable_chunk_num_in_jiffy
		= zicio_get_pagenum_in_a_jiffy(zicio_desc);
	zicio_raw_user_ingestion_sum zicio_ingestion_point
		= {0, 0, current_head, avg_consumable_chunk_num_in_jiffy};

	zicio_iterate_all_zicio_struct_with_ret(&zicio_shared_pool->zicio_channels,
			__zicio_get_avg_user_ingestion_monotonic_chunk_id,
			&zicio_ingestion_point, atomic_read(&zicio_shared_pool->pin) - 1, false);

	if (!zicio_ingestion_point.nr_channels)
		return current_head;

	return zicio_ingestion_point.current_ingestion_point_sum_no_mod
			/ zicio_ingestion_point.nr_channels;
}
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

/*
 * zicio_check_io_required
 *
 * Checking where I/O is required or not.
 */
static int 
zicio_check_io_required(zicio_descriptor *desc,
		unsigned long current_head, unsigned int distance_from_head)
{
	/*
	 * Currently, premap is mainly performed using two metrics. It seems that
	 * additional metrics will need to be added in the further optimization
	 * process.
	 *
	 * (1) We want to locate current head is between premap low watermark and
	 * premap high watermark.
	 *
	 * (2) Whether or not there is enough pages to consumed be consumed during
	 * a jiffy.
	 */

	/*
	 * To measure the distance to the average consumption point, a code that
	 * records an accurate premap low point must be entered and measured.
	 */
	unsigned int premap_low_watermark = zicio_get_pagenum_in_a_jiffy(desc);
	unsigned int premap_high_watermark = (premap_low_watermark) << 1;
	int nr_channels = 0;
	unsigned long tot_user_ingestion_point =
			zicio_get_raw_avg_user_ingestion_point(desc, &nr_channels,
					current_head);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	zicio_add_consumable_chunk_number(desc, premap_low_watermark);
#endif /* (CONFIG_ZICIO_DEBUG_LEVEL >= 2) */

	/*
	 * Checking whether I/O is required or not.
	 */
	if (!tot_user_ingestion_point) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "[ZICIO] cpu[%d] zicio_check_io_required(), tot_user_ingestion_point: %d, distance_from_head: %d, premap_low_watermark: %d\n",
					desc->cpu_id,
					tot_user_ingestion_point,
					distance_from_head,
					premap_low_watermark);
#endif
		/*
		 * If we don't have the average of user ingestion point, then we can
		 * only check whether the distance from head is larger than premap low
		 * watermark or not. That distance from head is less then premap low
		 * watermark means currently user doesn't have enogh data buffer for
		 * a jiffy.
		 */
		if (distance_from_head < premap_low_watermark) {
			return ZICIO_IO_REQUIRED;
		} else {
			return ZICIO_IO_NOT_NEEDED;
		}
	} else {
		/*
		 * If distance from head is larger than premap low watermark, a channel
		 * has enough pages to be consumed during a jiffy. So, do not preform
		 * I/O.
		 */
		if (distance_from_head >= premap_low_watermark) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "[ZICIO] cpu[%d],  zicio_check_io_required(), distance_from_head: %d, premap_low_watermark: %d\n",
					desc->cpu_id,
					distance_from_head,
					premap_low_watermark);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
			return ZICIO_IO_NOT_NEEDED;
		}

		/*
		 * If current head is larger than premap high watermark, the I/O
		 * performed now means that I/O can be performed on chunks that are much
		 * farther away than average. So call softirq daemon.
		 */
		if (tot_user_ingestion_point + premap_high_watermark * nr_channels <=
					zicio_get_last_io_file_chunk_id(desc) * nr_channels) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "[ZICIO] cpu[%d], zicio_check_io_required(), tot_user_ingestion_point: %d, premap_high_watermark: %d, nr_channels: %d, last_io_file_chunk_id: %d, distance_from_head: %d, premap_low_watermark: %d\n",
					desc->cpu_id,
					tot_user_ingestion_point,
					premap_high_watermark,
					nr_channels,
					zicio_get_last_io_file_chunk_id(desc),
					distance_from_head,
					premap_low_watermark);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
			return ZICIO_IO_NOT_NEEDED;
		}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "[ZICIO] cpu[%d], zicio_check_io_required(), REQUIRED, premap_low_watermark: %ld, distance_from_head: %ld, prev_requested_chunk_count: %d\n",
				desc->cpu_id,
				premap_low_watermark,
				distance_from_head,
				prev_requested_chunk_count);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */

		return ZICIO_IO_REQUIRED;
	}
}

/*
 * zicio_allocate_and_initialize_shared_metadata_ctrl
 *
 * Allocate and initialize file's extent array to shared pool
 */
void
zicio_allocate_and_initialize_shared_metadata_ctrl(
			zicio_shared_pool *zicio_shared_pool,
			unsigned long total_idx_ext_cnt, unsigned long *file_start_point)
{
	/*
	 * Allocate and initialize metadata control
	 */
	zicio_shared_pool->shared_metadata_ctrl.file_start_point_extent =
			file_start_point;
	zicio_shared_pool->shared_metadata_ctrl.num_metadata = 0;
	/* Currently, metadata buffer is set to one page per a idx extent */
	zicio_shared_pool->shared_metadata_ctrl.shared_metadata_buffer =
				kmalloc(sizeof(struct zicio_ext4_extent) *
				total_idx_ext_cnt * ZICIO_NUM_EXTENT_IN_PAGE,
				GFP_KERNEL|__GFP_ZERO);
}

/*
 * zicio_delete_shared_metadata_ctrl
 *
 * If metadata information of a file is managed in the shared pool, then free
 * them.
 */
void
zicio_delete_shared_metadata_ctrl(zicio_shared_pool *zicio_shared_pool)
{
	zicio_free_if_not_null(
			zicio_shared_pool->shared_metadata_ctrl.file_start_point_extent);
	zicio_free_if_not_null(
			zicio_shared_pool->shared_metadata_ctrl.shared_metadata_buffer);
}

/*
 * zicio_reset_shared_metadata_buffer
 *
 * Reset shared metadata buffer
 */
void
zicio_reset_shared_metadata_buffer(zicio_shared_pool *zicio_shared_pool)
{
	struct zicio_ext4_extent *old_extent_buffer =
			zicio_shared_pool->shared_metadata_ctrl.shared_metadata_buffer;
	unsigned long size_metadata = sizeof(struct zicio_ext4_extent) *
			zicio_shared_pool->shared_metadata_ctrl.num_metadata;

	BUG_ON(!size_metadata);
	BUG_ON(!old_extent_buffer);

	zicio_shared_pool->shared_metadata_ctrl.shared_metadata_buffer = kmalloc(
			size_metadata, GFP_KERNEL|__GFP_ZERO);

	memcpy(zicio_shared_pool->shared_metadata_ctrl.shared_metadata_buffer,
			old_extent_buffer, size_metadata);

	kfree(old_extent_buffer);
}

/*
 * zicio_wait_shared_page_reclaim
 *
 * Wait to reclaim shared pages
 */
void
zicio_wait_shared_page_reclaim(zicio_descriptor *desc)
{
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	int num_logged = 0;
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
#endif
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	volatile unsigned int num_shared_pages = 0, num_using_pages = 0;

	/*
	 * Unmap should be performed whether or not the contributed page is present.
	 * Otherwise, this channel can be terminated without a page ref count
	 * decrement of other chanenls.
	 */
	zicio_unmap_pages_from_local_page_table(desc);
	/* 
	 * Unmap and reclaim pages
	 */
	while ((num_shared_pages =
			atomic_read(&zicio_shared_pool_local->num_shared_pages)) || 
		   (num_using_pages =
			 atomic_read(&zicio_shared_pool_local->num_using_pages))) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		if (num_logged < 2) {
			num_logged++;
			if (num_logged == 2) {
				zicio_dump_shared_bitvector(zicio_shared_pool);
				zicio_dump_channels_in_shared_pool(zicio_shared_pool);
			}
		}
#endif /* CONFIG_ZICIO_DEBUG */
		if (zicio_get_num_using_pages(desc) &&
				zicio_check_reclaiming_in_progress(desc)) {
			zicio_reclaim_spcb(desc, false);
			zicio_set_reclaiming_end(desc);
		}
		zicio_unmap_pages_from_local_page_table(desc);
	}
#ifdef CONFIG_ZICIO_STAT
	zicio_set_latency_on_derailed_mode_to_stat(desc);
#endif /* (CONFIG_ZICIO_STAT) */
}

/*
 * zicio_delete_attached_channel
 *
 * Delete attached channel data to flow control
 */
void
zicio_delete_attached_channel(zicio_shared_pool *zicio_shared_pool,
		zicio_shared_pool_local *zicio_shared_pool_local)
{
	zicio_attached_channel *zicio_channel;
	int page_id;
	/*
	 * Free local channel data to be shared
	 */
	zicio_channel = zicio_pick_attached_channel(
			&zicio_shared_pool->zicio_channels, zicio_shared_pool_local->channel_id);

	/*
	 * There may be processes currently reading this channel and data related to
	 * this channel. In order to prevent data from being deleted during this
	 * process, synchronize_rcu() should be called.
	 */
	synchronize_rcu();

	for (page_id = 0 ; page_id < ZICIO_LOCAL_DATABUFFER_CHUNK_NUM ; page_id++) {
		if (zicio_channel->local_zicio_spcb[page_id]) {
			while (atomic_read(
				&zicio_channel->local_zicio_spcb[page_id]->zicio_spcb.ref_count)) {
				msleep(10);
			}
			mb();
			zicio_free_spcb(zicio_channel->local_zicio_spcb[page_id]);
		}
	}

	zicio_free_channel(zicio_channel);
}

/*
 * zicio_free_shared_pool
 *
 * Deleting shared pool for zicio
 */
void
zicio_delete_shared_pool_local(zicio_descriptor *desc,
			zicio_shared_pool *zicio_shared_pool,
			zicio_shared_pool_local *zicio_shared_pool_local)
{
	/*
	 * Free bit vector
	 */
	zicio_delete_zicio_bitvector(zicio_get_local_bitvector(desc));

	/*
	 * Free channel data of shared pool
	 */
	zicio_delete_attached_channel(zicio_shared_pool, zicio_shared_pool_local);

	/*
	 * Free the page for shared page control block pointer array
	 */
	free_page((unsigned long)zicio_shared_pool_local->zicio_spcb_arrays);

	/*
	 * Free the page for ingestion point tracking information
	 */
	free_page((unsigned long)zicio_shared_pool_local->ingestion_point_track_array);

	/*
	 * Free the page for contritube spcb
	 */
	free_page((unsigned long)zicio_shared_pool_local->contribute_zicio_spcb);

	zicio_free_shared_pool_local(zicio_shared_pool_local);
}

/*
 * __zicio_delete_shared_pool
 *
 * Deleting shared pool for zicio
 */
void
__zicio_delete_shared_pool(zicio_shared_pool *zicio_shared_pool)
{
	/*
	 * Delete zicio shared file structure
	 */
	zicio_delete_shared_files(&zicio_shared_pool->shared_files);

	/*
	 * Free shared pool's buffer and device mapping elements.
	 */
	zicio_free_mem_shared(zicio_shared_pool);

	/*
	 * Free shared data buffe ID managing queue.
	 */
	zicio_free_page_id_queue(zicio_shared_pool);

	/*
	 * Delete zicio shared metadata buffer
	 */
	zicio_delete_shared_metadata_ctrl(zicio_shared_pool);

	/*
	 * Free bit vector
	 */
	zicio_delete_zicio_bitvector(&zicio_shared_pool->bitvector);

	/*
	 * Cleanup zicio channels in shared pool
	 */
	zicio_cleanup_attached_channel(&zicio_shared_pool->zicio_channels);

	/*
	 * Free zicio_shared_pool struct
	 */
	zicio_free_shared_pool(zicio_shared_pool);
}

void
zicio_delete_shared_pool(void *zicio_shared_pool)
{
#ifdef CONFIG_ZICIO_STAT
	zicio_dump_shared_stat_board(zicio_shared_pool);
#endif
	__zicio_delete_shared_pool(zicio_shared_pool);
}

/*
 * zicio_set_consume_indicator
 *
 * set channel's information for file
 */
void
zicio_set_consume_indicator(zicio_descriptor *desc,
			unsigned int chunk_id_no_mod)
{
	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool_local *zicio_shared_pool_local;
	zicio_shared_metadata_ctrl *zicio_shared_metadata_ctrl;
	struct zicio_ext4_extent *cur_meta_buffer;
	unsigned long chunk_max;
	unsigned int lblk_max = UINT_MAX;
	unsigned int cur_lblk_off;
	unsigned int num_ext_per_file;
	unsigned int start, end;
	unsigned int chunk_id_mod;
	int file_idx;

	zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);
	zicio_shared_metadata_ctrl = &zicio_shared_pool->shared_metadata_ctrl;
	chunk_max = zicio_shared_pool->shared_files.total_chunk_nums;
	chunk_id_mod =
			chunk_id_no_mod % zicio_shared_pool->shared_files.total_chunk_nums;
		
	/* Set start chunk id when starting read in shared pool */
	zicio_shared_pool_local->consume_indicator.start_chunk_id_no_mod =
				chunk_id_no_mod;
	/* Set current chunk id */
	zicio_shared_pool_local->consume_indicator.current_chunk_id_mod =
				chunk_id_mod;

	/* Set members to convert locigal file chunk id to monotonic chunk id */
	zicio_shared_pool_local->consume_indicator.chunk_id_high = chunk_id_no_mod /
		zicio_shared_pool->shared_files.total_chunk_nums;
	zicio_shared_pool_local->consume_indicator.chunk_id_low = chunk_id_no_mod %
		zicio_shared_pool->shared_files.total_chunk_nums;

	/* Specifies which file the file chunk id to be read is located in. */
	for (file_idx = 0 ;
		 file_idx < zicio_get_num_files_shared(zicio_shared_pool);
		 file_idx++) {
		start = zicio_get_file_chunk_start(zicio_shared_pool, file_idx);
		end = zicio_get_next_file_chunk_start(zicio_shared_pool, file_idx);

		if (start <= chunk_id_mod && chunk_id_mod < end) {
			zicio_shared_pool_local->consume_indicator.current_file_idx =
						file_idx;
			break;
		}
	}

	BUG_ON(file_idx >=
			zicio_get_num_files_shared(zicio_shared_pool));
	BUG_ON(chunk_id_mod <
			zicio_shared_pool->shared_files.start_chunk_nums[file_idx]);
	/* Set file index */
	zicio_shared_pool_local->consume_indicator.current_file_idx = file_idx;

	cur_lblk_off = (chunk_id_mod -
			zicio_shared_pool->shared_files.start_chunk_nums[file_idx]);
	cur_lblk_off <<= ZICIO_PAGE_TO_CHUNK_SHIFT;

	num_ext_per_file = zicio_get_num_extent_per_file(zicio_shared_pool,
				file_idx);

	/* Set metadata index */
	cur_meta_buffer =
			zicio_shared_pool->shared_metadata_ctrl.shared_metadata_buffer;
	cur_meta_buffer += zicio_shared_metadata_ctrl->
			file_start_point_extent[file_idx];

	zicio_shared_pool_local->consume_indicator.start_metadata =
			zicio_binsearch_range(&cur_lblk_off, &lblk_max, cur_meta_buffer,
					num_ext_per_file, sizeof(struct zicio_ext4_extent),
							zicio_compare_extent_to_lblk);

	zicio_shared_pool_local->consume_indicator.current_metadata =
				zicio_shared_pool_local->consume_indicator.start_metadata;
}

/*
 * zicio_get_next_file_chunk_id_shared
 *
 * Acquires the file chunk id to be read next.
 *
 * @desc: zicio channel descriptor
 * @local_page_idx: Local page idx of shared pool.
 * @is_on_track: Flag to show if this funcion is called with or track status or
 *				 derailed status.
 */
unsigned int
zicio_get_next_file_chunk_id_shared(zicio_descriptor *desc,
			int local_page_idx, bool is_on_track)
{
	zicio_shared_page_control_block *zicio_spcb = NULL;
	unsigned int file_chunk_id;

	BUG_ON(local_page_idx < 0);

	if (is_on_track) {
		/* If channel is not derailed, get chunk id from shared pool. */
		zicio_spcb = zicio_get_spcb_with_id(desc, local_page_idx);
		file_chunk_id = zicio_get_next_file_chunk_id_from_shared_pool(
				desc, zicio_spcb, NULL);
		/*
		 * In the process of allocating a local page index right before
		 * derailing, there was a problem of being allocated a shared page
		 * control block with the shared flag turned on despite being derailed.
		 * In the case of the first derailed channel, the code was modified to
		 * turn off the shared flag in consideration of this.
		 */
		if (unlikely(file_chunk_id == -2) ||
			unlikely(file_chunk_id == ZICIO_LOCAL_BITVECTOR_COMPLETE)) {
			mb();
			atomic_dec(&zicio_spcb->zicio_spcb.ref_count);
			atomic64_set(&zicio_spcb->zicio_spcb.exp_jiffies, 0);
		}
	} else {
		zicio_spcb = zicio_get_local_spcb_with_id(desc, local_page_idx);
		/* Otherwise, get chunk id from local bitvector. */
		file_chunk_id = zicio_get_next_unread_chunk_id_shared(desc, zicio_spcb);

		if (file_chunk_id == ZICIO_LOCAL_BITVECTOR_COMPLETE) {
			mb();
			atomic_dec(&zicio_spcb->zicio_spcb.ref_count);
		}
	}

	PRINT_SPCB_DEBUG(zicio_spcb->zicio_spcb, desc->cpu_id, __FILE__, __LINE__,
			__FUNCTION__);

	return file_chunk_id;
}
EXPORT_SYMBOL(zicio_get_next_file_chunk_id_shared);

/*
 * zicio_get_previous_low_premap_point
 *
 * Get previous premap point
 */
unsigned long
zicio_get_previous_low_premap_point(zicio_shared_pool *zicio_shared_pool,
		zicio_shared_pool_local *zicio_shared_pool_local)
{
	unsigned int previous_low_premap_point = atomic_read(
			&zicio_shared_pool_local->zicio_channel->previous_low_premap_point);
	unsigned int num_additional_premap = atomic_read(&zicio_shared_pool->pin);

	if ((previous_low_premap_point == UINT_MAX) ||
			((zicio_shared_pool_local->consume_indicator.start_chunk_id_no_mod +
			  num_additional_premap) > previous_low_premap_point) ||
			(previous_low_premap_point < num_additional_premap)) {
		/* If it is not set yet, premap is performed from the first starting
		 * point. */
		return zicio_shared_pool_local->consume_indicator.start_chunk_id_no_mod;
	} else {
		/* Premap is performed from before the number of channels in the
		 * previously premapped chunk. */
		return previous_low_premap_point - num_additional_premap;
	}
}

/*
 * zicio_set_premap_start_point
 *
 * Select a starting point for premap.
 * If it is completely derailed and premap cannot be performed, false is
 * returned. If there is room for premap, premap is performed.
 */
static bool
zicio_set_premap_start_point(zicio_descriptor *desc,
		unsigned long *premap_start_point_no_mod,
		unsigned long previous_low_premap_point_no_mod)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local *zicio_shared_pool_local =
		zicio_get_shared_pool_local(desc);

	if (*premap_start_point_no_mod >=
			zicio_shared_pool_local->consume_indicator.start_chunk_id_no_mod +
			zicio_shared_pool->shared_files.total_chunk_nums) {
		if (previous_low_premap_point_no_mod + 1 <
				zicio_shared_pool_local->consume_indicator.start_chunk_id_no_mod +
				zicio_shared_pool->shared_files.total_chunk_nums) {
			*premap_start_point_no_mod =
				(zicio_shared_pool_local->consume_indicator.start_chunk_id_no_mod +
				 zicio_shared_pool->shared_files.total_chunk_nums - 1);
			return true;
		}
		/* In case of derailed on the head. */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "cpu[%d] Derailed [%s:%d][%s]\n", desc->cpu_id,
				__FILE__, __LINE__, __FUNCTION__);
#endif
		atomic_set(&zicio_shared_pool_local->zicio_channel->derailed, true);
#ifdef CONFIG_ZICIO_STAT
		zicio_set_derailing_to_stat(desc);
#endif /* #if (CONFIG_ZICIO_STAT) */
		zicio_shared_pool_local->consume_indicator.current_chunk_id_mod = 0;

		return false;
	}
	(*premap_start_point_no_mod)--;
	return true;
}

/*
 * zicio_set_previous_low_premap_point
 *
 * Update the low premap point value
 */
static void
zicio_set_previous_low_premap_point(
		zicio_shared_pool_local *zicio_shared_pool_local,
		unsigned int new_previous_low_premap_point)
{
	zicio_attached_channel *zicio_channel;
	zicio_channel_consumption_indicator *consume_indicator;
	unsigned int previous_low_premap_point;

	zicio_channel = zicio_shared_pool_local->zicio_channel;
	consume_indicator = &zicio_shared_pool_local->consume_indicator;
	previous_low_premap_point = atomic_read(
			&zicio_channel->previous_low_premap_point);

	if (unlikely(previous_low_premap_point == UINT_MAX)) {
		/* For processing the first set, the first chunk id is used as a premap
		 * standard. */
		atomic_set(&zicio_channel->previous_low_premap_point,
				consume_indicator->start_chunk_id_no_mod);
	} else if ((new_previous_low_premap_point != UINT_MAX) &&
			(previous_low_premap_point <
			 new_previous_low_premap_point)) {
		/* Set new value to premap low premap if we need to increase the low
		 * premap. */
		atomic_set(&zicio_channel->previous_low_premap_point,
				new_previous_low_premap_point);
	}
}

/*
 * zicio_get_page_gap_from_user_ingestion_point
 *
 * Get the number of pages to be read ahead
 */
static int
zicio_get_page_gap_from_user_ingestion_point(zicio_descriptor *desc,
		zicio_shared_pool *zicio_shared_pool,
		zicio_shared_pool_local *zicio_shared_pool_local,
		zicio_switch_board *sb, int last_user_buffer_idx_mod)
{
	int current_user_buffer_idx_mod = zicio_get_user_buffer_idx(desc);
	int page_gap = 0;
	unsigned int start, end, sb_idx;
	unsigned int chunk_id_no_mod;
	unsigned int min_chunk_id_no_mod = atomic_read(
			&zicio_shared_pool_local->zicio_channel->previous_low_premap_point);

	/* Get the start point in switch board */
	if (unlikely(current_user_buffer_idx_mod == INT_MAX)) {
		start = 0;
	} else {
		start = current_user_buffer_idx_mod;
	}

	/* Get the end point in switch board */
	if (unlikely(last_user_buffer_idx_mod == INT_MAX)) {
		end = 0;
	} else if (start > last_user_buffer_idx_mod) {
		/* In case of warp around */
		end = last_user_buffer_idx_mod + ZICIO_MAX_NUM_GHOST_ENTRY;
	} else {
		end = last_user_buffer_idx_mod;
	}

	while (start < end) {
		/* It should be confirmed in the ghost entry */
		sb_idx = start & (~ZICIO_GHOST_ENTRY_MASK);

		if (zicio_read_status(sb, sb_idx) == ENTRY_READY) {
			chunk_id_no_mod = zicio_get_user_file_chunk_id(desc, sb_idx);

			/*
			 * An entry whose status is ready must not have a chunk id value of
			 * UINT_MAX.
			 */
			BUG_ON(chunk_id_no_mod == UINT_MAX);
			chunk_id_no_mod = zicio_convert_chunk_id_to_monotonic_id(
					zicio_shared_pool, zicio_shared_pool_local, chunk_id_no_mod);
			if (chunk_id_no_mod < min_chunk_id_no_mod) {
				min_chunk_id_no_mod = chunk_id_no_mod;
			}
			page_gap++;
		}
		start++;
	}

	zicio_set_previous_low_premap_point(zicio_shared_pool_local,
			min_chunk_id_no_mod);

	return page_gap;
}

/*
 * zicio_check_bitmaps_to_premap
 *
 * Read shared bitvector and local bitvector for checking where current chunk
 * can be shared or not and has read on not.
 */
static inline bool
zicio_check_bitmaps_to_premap(zicio_bitvector *zicio_shared_bitvector,
		zicio_bitvector *zicio_local_bitvector,
		unsigned long current_chunk_id_mod)
{
	/*
	 * Sharing is possible if both shared bitvectors are turned on.
	 * If the local bitvector is off, it is not mapped yet, so sharing is
	 * possible.
	 */
	return ((zicio_get_bit_status(zicio_shared_bitvector,
					current_chunk_id_mod, true, 0)
				== (ZICIO_BIT_VALID|ZICIO_BIT_REF)) &&
			!(zicio_test_and_set_bitvector_atomic(zicio_local_bitvector,
									  current_chunk_id_mod, false)));
}

/*
 * zicio_check_bitmaps_to_premap_init
 * When first premap is performed, we check whether the shared bit is 00 or not.
 * This function do this.
 *
 * @zicio_shared_bitvector: shared bitvector to show shared chunk information
 * @zicio_local_bitvector: local bitvector to show channel's read chunk
 * @current_chink_id_mod: current chunk ID
 */
static inline int
zicio_check_bitmaps_to_premap_init(zicio_bitvector *zicio_shared_bitvector,
		zicio_bitvector *zicio_local_bitvector,
		unsigned long current_chunk_id_mod)
{
	int bit_status = zicio_get_bit_status(zicio_shared_bitvector,
			current_chunk_id_mod, true, 0);
	bool already_turn_on;
	switch (bit_status) {
		case (ZICIO_BIT_VALID|ZICIO_BIT_REF):
			/* Current chunk can be premapped. */
			already_turn_on = zicio_test_and_set_bitvector_atomic(
					zicio_local_bitvector, current_chunk_id_mod, false);
			BUG_ON(already_turn_on);
			return 1;
		case 0:
			/* Do not need to check previous chunks. */
			return 0;
		default:
			/* Checking if previous chunks are shared or not. */
			return -1;
	}
}

/*
 * zicio_check_spcb_to_premap
 *
 * Gets the shared page control block corresponding to the current chunk id
 * received as a parameter and checks it to verify whether premap is possible.
 */
static inline zicio_shared_page_control_block *
zicio_check_spcb_to_premap(zicio_descriptor *desc,
		zicio_shared_pool *zicio_shared_pool,
		unsigned long current_chunk_id_mod, unsigned long current_jiffies,
		unsigned long expected_safe_jiffies)
{
	zicio_shared_page_control_block *zicio_spcb;
	unsigned long reclaimer_jiffies;

	/* Examine the hash table of the shared pool. */
	zicio_spcb = zicio_rcu_hash_find(
		zicio_shared_pool->shared_pool_hash, current_chunk_id_mod + 1);

	/* If the corresponding spcb is not obtained, it returns immediately. */
	if (!zicio_spcb) {
#ifdef CONFIG_ZICIO_STAT
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		zicio_inc_premap_failure_from_spcb(desc);
#endif
#endif /* CONFIG_ZICIO_STAT */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "Cannot get spcb\n");
#endif
		return NULL;
	}

	/*
	 * When reclaiming, exptime and refcount are checked and shares are
	 * finished. When premapping, exptime and refcount are checked and page
	 * sharing is not atomic. In addition, the time used to check exptime is
	 * also not equal.
	 * It can cause problems during simultaneous execution. Here is the code
	 * to prevent this problem.
	 */

	/*
	 * Firstly, increment reference counter of spcb. we don't want to spcb
	 * freed during this operation.
	 */
	atomic_inc(&zicio_spcb->zicio_spcb.ref_count);

	/*
	 * If there's concurrent reclaimer, then we read its time to compare
	 * equal time.
	 */
	reclaimer_jiffies = atomic64_read(&zicio_spcb->zicio_spcb.reclaimer_jiffies);
	if (reclaimer_jiffies) {
		current_jiffies = reclaimer_jiffies;
	}

	mb();

	/* Check whether spcb has expired based on the current time. */
	if (atomic64_read(&zicio_spcb->zicio_spcb.exp_jiffies) < current_jiffies) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "Fail premap cpu[%d] File chunk ID: %lu "
					"Exp jiffies: %llu Current jiffies: %lu  [%s:%d][%s]\n",
					desc->cpu_id, current_chunk_id_mod,
					atomic64_read(&zicio_spcb->zicio_spcb.exp_jiffies),
					current_jiffies, __FILE__, __LINE__, __FUNCTION__);
#endif
#ifdef CONFIG_ZICIO_STAT
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		zicio_inc_premap_failure_from_exptime(desc);
#endif
#endif /* CONFIG_ZICIO_STAT */
		atomic_dec(&zicio_spcb->zicio_spcb.ref_count);
		return NULL;
	}

	mb();

	if (zicio_spcb->chunk_id != current_chunk_id_mod) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "Do not match file chunk id\n");
#endif
#ifdef CONFIG_ZICIO_STAT
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		zicio_inc_premap_failure_from_invalid_chunk_id(desc);
#endif
#endif /* CONFIG_ZICIO_STAT */
		atomic_dec(&zicio_spcb->zicio_spcb.ref_count);
		return NULL;
	}

	if (atomic64_read(&zicio_spcb->zicio_spcb.exp_jiffies) < expected_safe_jiffies) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "Fail premap cpu[%d] File chunk ID: %lu "
					"Exp_time: %llu Expected safe time: %lu  [%s:%d][%s]\n",
					desc->cpu_id, current_chunk_id_mod,
					atomic64_read(&zicio_spcb->zicio_spcb.exp_jiffies),
					expected_safe_jiffies,	 __FILE__, __LINE__, __FUNCTION__);
#endif
		atomic_dec(&zicio_spcb->zicio_spcb.ref_count);
		return NULL;
	}
	mb();

	return zicio_spcb;
}

/*
 * zicio_do_premap_page_from_shared_pool
 *
 * Premapping is performed using the information recorded in the shared page
 * control block.
 */
static inline int
zicio_do_premap_page_from_shared_pool(zicio_descriptor *desc,
	zicio_shared_page_control_block *zicio_spcb,
	unsigned int previous_high_premap_point, unsigned int distance_from_start)
{
	zicio_switch_board *sb = desc->switch_board;
	int user_buffer_idx_mod;

	/*
	 * Perform premap using spcb.
	 */
	if ((user_buffer_idx_mod = zicio_ghost_premap(desc, zicio_spcb,
			zicio_spcb->zicio_spcb.chunk_ptr, previous_high_premap_point,
			distance_from_start)) < 0) {
		atomic_dec(&zicio_spcb->zicio_spcb.ref_count);
		return user_buffer_idx_mod;
	}

	/*
	 * If successful, update the switch board.
	 *
	 * Considering the premapping in different CPUs, it works by turning on the
	 * bit first using test_and_set in the bit confirmation process and then
	 * checking the result value.
	 * For this reason, it operates by checking only the internal bitvector and
	 * then turning it on.
	 */
	zicio_set_bytes(sb, user_buffer_idx_mod, zicio_spcb->zicio_spcb.chunk_size);
	zicio_inc_mapped_chunks_num(desc);

	/*
	 * Returns the premapped position.
	 */
	return user_buffer_idx_mod;
}

/*
 * zicio_premap_pages_from_shared_pool
 *
 * Traversing the shared bitvector to find a sharable page and premaps it.
 */
#ifdef CONFIG_ZICIO_STAT
unsigned int
zicio_premap_pages_from_shared_pool(zicio_descriptor *desc,
			unsigned long *premap_start_point_no_mod,
			unsigned long *current_user_file_chunk_id,
			unsigned int *nr_premapped_ptr)
#else /* !CONFIG_ZICIO_STAT */
unsigned int
zicio_premap_pages_from_shared_pool(zicio_descriptor *desc,
			unsigned long *premap_start_point_no_mod,
			unsigned long *current_user_file_chunk_id)
#endif /* !CONFIG_ZICIO_STAT */
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local *zicio_shared_pool_local =
				zicio_get_shared_pool_local(desc);
	struct zicio_switch_board *sb = desc->switch_board;
	zicio_bitvector *zicio_shared_bitvector, *zicio_local_bitvector;
	unsigned long bitvector_idx_no_mod;
	unsigned long current_chunk_id_mod;
	zicio_shared_page_control_block *zicio_spcb;
	unsigned long current_jiffies, expected_safe_jiffies;
	int user_buffer_idx_mod;
	unsigned int distance_from_head, num_premapped_page = 0;
	unsigned int previous_high_premap_point;
	unsigned int current_low_premap_point;
	int page_gap = -1;
	int i;
	int prepared_chunk_nums = 0;
	u32 prepared_chunk_id_mod[64];
	zicio_shared_page_control_block *prepared_spcb[64];
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	int num_pages = zicio_get_pagenum_in_a_jiffy(desc);
#endif
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	unsigned int avg_user_ingestion_point = 0;
	zicio_attached_channel *zicio_channel = zicio_shared_pool_local->zicio_channel;
	unsigned int monotonic_chunk_id = 0;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	/* Get bitvectors */
	zicio_shared_bitvector = zicio_get_shared_bitvector(desc);
	zicio_local_bitvector = zicio_get_local_bitvector(desc);

	/* Get current and previous head idx */
	*premap_start_point_no_mod = atomic_read(&(zicio_shared_pool->head));

	/* Specifies at what point the user is currently consuming. */
	previous_high_premap_point =
			zicio_get_previous_high_premap_point(desc);
	current_low_premap_point = zicio_get_previous_low_premap_point(
			zicio_shared_pool, zicio_shared_pool_local);

	if (unlikely(previous_high_premap_point == UINT_MAX)) {
		previous_high_premap_point =
				zicio_set_previous_high_premap_point(desc,
						zicio_shared_pool_local->
						consume_indicator.start_chunk_id_no_mod);
	}

	BUG_ON(zicio_check_channel_derailed(desc));

	/*
	 * Using the current head and premap low point values, it is determined
	 * whether derailing or premap is possible.
	 */
	if (!zicio_set_premap_start_point(desc, premap_start_point_no_mod,
			current_low_premap_point)) {
		return 0;
	}

	user_buffer_idx_mod = GET_PREMAPPING_ITER(desc);

	if (likely(user_buffer_idx_mod != INT_MAX)) {
		user_buffer_idx_mod &= ~(ZICIO_GHOST_ENTRY_MASK);
	}

	current_jiffies = get_jiffies_64();
	expected_safe_jiffies = current_jiffies + 1;
	page_gap = zicio_get_page_gap_from_user_ingestion_point(desc,
			zicio_shared_pool, zicio_shared_pool_local, sb, user_buffer_idx_mod);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "Premap start: %lu ~ Premap end: %u distance: %lu\n",
			*premap_start_point_no_mod, current_low_premap_point,
			*premap_start_point_no_mod - current_low_premap_point);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL >= 2 */

	/* 
	 * Loops to check file chunk can be pre-mapped and do pre-mapping
	 */
	for (bitvector_idx_no_mod = *premap_start_point_no_mod ;
		 bitvector_idx_no_mod > current_low_premap_point ;
		 bitvector_idx_no_mod--) {

		/* Get current chunk id */
		current_chunk_id_mod = bitvector_idx_no_mod %
					zicio_shared_pool->shared_files.total_chunk_nums;

		/*
		 * Checking if the page can be premapped
		 */

		/* Firstly, read shared bitvectors and local bitvector for checking
		 * current chunk can be shared and isn't read */
		if (zicio_check_bitmaps_to_premap(zicio_shared_bitvector,
					zicio_local_bitvector, current_chunk_id_mod)) {
			/*
			 * Find the zicio_spcb corresponding to the read chunk id and check if
			 * it is safe to use it.
			 */
			zicio_spcb = zicio_check_spcb_to_premap(desc, zicio_shared_pool,
					current_chunk_id_mod, current_jiffies, expected_safe_jiffies);

			if (!zicio_spcb) {
				zicio_test_and_clear_bitvector_atomic(zicio_local_bitvector,
						current_chunk_id_mod);
				continue;
			}

			prepared_chunk_id_mod[prepared_chunk_nums] = current_chunk_id_mod;
			prepared_spcb[prepared_chunk_nums] = zicio_spcb;
			prepared_chunk_nums += 1;

			/*
			 * If premap has been performed as much as the chunk number on the
			 * current switch board, there is no need to provide more pages.
			 */
			if (atomic_read(&zicio_shared_pool_local->num_mapped) +
				prepared_chunk_nums >=
				zicio_shared_pool->shared_files.total_chunk_nums) {
				break;
			}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "cpu[%d] File chunk ID: %lu Exp_time: %llu "
					"Expected safe time: %lu  [%s:%d][%s]\n", desc->cpu_id,
					current_chunk_id_mod,
					atomic64_read(&zicio_spcb->zicio_spcb.exp_jiffies),
					expected_safe_jiffies,	 __FILE__, __LINE__, __FUNCTION__);
#endif
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
		} else {
			printk(KERN_WARNING "cpu[%d] File chunk ID: %lu is not ready in "
					"the pool Shared bit: %ld, Local bit: %ld [%s:%d][%s]\n",
					desc->cpu_id, current_chunk_id_mod,
					zicio_get_bit_status(zicio_shared_bitvector,
						current_chunk_id_mod, true, 0),
					zicio_get_bit_status(zicio_local_bitvector,
						current_chunk_id_mod, false, 0),
					__FILE__, __LINE__, __FUNCTION__);
		}
#else
		}
#endif
	}

	BUG_ON(prepared_chunk_nums > 64);

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	/* Do pre-mapping. */
	if (desc->channel_index == 2)
		printk(KERN_WARNING "[ZICIO] pre-mapping start ----------------\n");
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	for (i = prepared_chunk_nums - 1; i >= 0; --i) {
		/* Get target spcb. */
		zicio_spcb = prepared_spcb[i];
		current_chunk_id_mod = prepared_chunk_id_mod[i];

		/* Perform ghost premapping using spcb. */
		if ((user_buffer_idx_mod =
					zicio_do_premap_page_from_shared_pool(desc, zicio_spcb,	
								((previous_high_premap_point >
								  current_low_premap_point) ?
										previous_high_premap_point :
										current_low_premap_point),
								num_premapped_page)) < 0) {
			zicio_test_and_clear_bitvector_atomic(zicio_local_bitvector,
					current_chunk_id_mod);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "Error in ghost premap\n");
#endif
			continue;
		}

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
		monotonic_chunk_id
			= zicio_convert_chunk_id_to_monotonic_id(
				zicio_shared_pool, zicio_shared_pool_local, zicio_spcb->chunk_id);

		/*
		 * Set user ingestion monotonic chunk id. This could be used to
		 * calculate the average ingestion point of users.
		 */
		zicio_set_tracking_ingestion_monotonic_chunk_id(
			desc, user_buffer_idx_mod, monotonic_chunk_id);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

		num_premapped_page++;

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		if (desc->channel_index == 2) {
			printk(KERN_WARNING "[ZICIO] pre-mapping, cpu-id: %d, chunk id: %lu, current jiffies: %lu, expiration jiffies: %lu, cur user buffer idx: %d, premapped user buffer idx: %d, pagenum in jiffy: %d\n",
				desc->cpu_id,
				current_chunk_id_mod,
				get_jiffies_64(),
				atomic64_read(&zicio_spcb->zicio_spcb.exp_jiffies),
				zicio_get_user_buffer_idx(desc),
				user_buffer_idx_mod,
				zicio_get_pagenum_in_a_jiffy(desc));
		}
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

		/*
		 * The point of notifying the user of memory preparation should
		 * always be after all preparations have been completed.
		 */
		mb();
		zicio_set_status(sb, user_buffer_idx_mod, ENTRY_READY);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "cpu[%d] user buffer idx : %d status : %d\n",
				desc->cpu_id, user_buffer_idx_mod,
				zicio_read_status(sb, user_buffer_idx_mod));
#endif

#ifdef CONFIG_ZICIO_STAT
		zicio_add_endowed_pages(desc);
		zicio_add_premapped_pages(desc);
#endif

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		if (num_pages < page_gap + num_premapped_page) {
			printk(KERN_WARNING "cpu[%d] premapped with page_gap: %d ,"
					"current user consumable page: %d\n",
					desc->cpu_id, page_gap + num_premapped_page, num_pages);
			zicio_inc_num_premapped_pages_consumed_after_a_jiffy(desc);
		}
#endif
	}

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	if (desc->channel_index == 2)
		printk(KERN_WARNING "[ZICIO] pre-mapping finish ---------------\n");
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	BUG_ON(page_gap == -1);
	zicio_set_previous_high_premap_point(desc, *premap_start_point_no_mod);

	*current_user_file_chunk_id = zicio_get_user_ingestion_chunk_id(desc,
			zicio_get_user_buffer_idx(desc));

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	avg_user_ingestion_point =
		zicio_get_avg_user_ingestion_monotonic_chunk_id(
			desc, *premap_start_point_no_mod);

	distance_from_head =
		*premap_start_point_no_mod <= avg_user_ingestion_point ?
		0 : *premap_start_point_no_mod - avg_user_ingestion_point;

#else /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	distance_from_head = page_gap + num_premapped_page;

	BUG_ON(distance_from_head > ZICIO_MAX_NUM_GHOST_ENTRY);
#endif /* !CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	zicio_add_premapped_distance(desc, *premap_start_point_no_mod -
			current_low_premap_point, num_premapped_page);
#endif

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu[%d] distance_from_head is %d [%s:%d][%s]\n",
			desc->cpu_id, distance_from_head, __FILE__, __LINE__, __FUNCTION__);
#endif

#ifdef CONFIG_ZICIO_STAT
	*nr_premapped_ptr = num_premapped_page;
#endif /* CONFIG_ZICIO_STAT */

	return distance_from_head;
}

/*
 * zicio_set_exp_jiffies_to_spcb
 *
 * Set expiration time for sharing
 */
static void
zicio_set_exp_jiffies_to_spcb(zicio_descriptor *desc,
		zicio_shared_page_control_block *zicio_spcb)
{
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	unsigned long avg_tsc_delta =
			zicio_get_shared_pool_raw_expiration_time(desc);

	/* Get user consumption time. */
	ktime_t user_consumption_time = (avg_tsc_delta) ? zicio_tsc_to_ktime(
			avg_tsc_delta) : ZICIO_DEFAULT_EXP_TIME_IN_NANO;

	/* Get nanoseconds from a jiffy. */
	u64 nsecs_in_a_jiffy = zicio_get_nsecs_from_jiffy();

	/* Calculate new expiration time (2 * average_consumption_time for slow users) */
	u64 new_exp_jiffies = user_consumption_time > nsecs_in_a_jiffy ?
		DIV_ROUND_UP(2 * user_consumption_time, nsecs_in_a_jiffy) : 4;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "[ZICIO] user_consumption_time: %llu, nsecs_in_a_jiffy: %llu, new_exp_jiffies: %llu\n",
			user_consumption_time, nsecs_in_a_jiffy, new_exp_jiffies);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */

	atomic64_set(&zicio_spcb->zicio_spcb.exp_jiffies, get_jiffies_64() + new_exp_jiffies);
#else /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	atomic64_set(&zicio_spcb->zicio_spcb.exp_jiffies, get_jiffies_64() + 3);
#endif /* !CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
}

/*
 * zicio_contribute_shared_page_to_pool
 *
 * Contribute shared page to pool
 * To do this, change the bitvector, shared hash table.
 */
static void
zicio_contribute_shared_page_to_pool(zicio_descriptor *desc,
		zicio_shared_pool *zicio_shared_pool,
		zicio_shared_pool_local *zicio_shared_pool_local,
		zicio_shared_page_control_block *zicio_spcb, int file_chunk_id)
{
	BUG_ON(zicio_spcb->chunk_id != file_chunk_id);
	/* Marks the shared bitvector that all requests have been processed and
	 * contributes the read pages to the hash table. */
	zicio_rcu_hash_add(zicio_shared_pool->shared_pool_hash, zicio_spcb,
			file_chunk_id + 1);
	BUG_ON(!atomic_read(&zicio_spcb->zicio_spcb.is_shared));

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu[%d] zicio_cmd->file_chunk_id: %d [%s:%d][%s]\n",
			desc->cpu_id, file_chunk_id, __FILE__, __LINE__, __FUNCTION__);
#endif
#ifdef CONFIG_ZICIO_STAT
	zicio_add_contributed_pages(desc);
#endif

	/*
	 * Turning a bit on and off must be done after data is inserted into the
	 * shared hash table.
	 */
	mb();

	zicio_set_shared_pool_bitvector(desc,
			file_chunk_id, zicio_spcb, ZICIO_BIT_VALID);
}

/*
 * zicio_produce_local_huge_page_shared
 *
 * Produce a filled huge page to user
 *
 * @zicio_desc: channel descriptor pointer
 * @zicio_spcb: shared page control block for current zicio command
 * @zicio_cmd: command descriptor pointer
 */
bool
zicio_produce_local_huge_page_shared(struct zicio_descriptor *zicio_desc,
			struct zicio_shared_page_control_block *zicio_spcb,
			struct zicio_nvme_cmd_list *zicio_cmd)
{
	struct zicio_switch_board *sb = zicio_desc->switch_board;
	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool_local *zicio_shared_pool_local;
	zicio_firehose_ctrl *ctrl = &zicio_desc->firehose_ctrl;
	int nr_nvme_pages, user_buffer_idx_mod;
	unsigned int previous_high_premap_point;
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	uint64_t monotonic_chunk_id;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
#ifdef CONFIG_ZICIO_STAT
	zicio_stat_board *stat_board = (zicio_stat_board *)zicio_desc->stat_board;
#endif /* CONFIG_ZICIO_STAT */

	BUG_ON(!zicio_desc->zicio_shared_pool_desc);

	/*
	 * Get attached shared pool and channel's local shared pool information
	 * descriptor pointer.
	 */
	zicio_shared_pool = zicio_get_shared_pool(zicio_desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(zicio_desc);

	previous_high_premap_point =
		zicio_get_previous_high_premap_point(zicio_desc);

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	monotonic_chunk_id =
		zicio_convert_chunk_id_to_monotonic_id(
						zicio_shared_pool, zicio_shared_pool_local,
						zicio_spcb->chunk_id);

	if (previous_high_premap_point == UINT_MAX) {
		previous_high_premap_point = zicio_set_previous_high_premap_point(
				zicio_desc, monotonic_chunk_id);
	}
#else /* !CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	if (previous_high_premap_point == UINT_MAX) {
		previous_high_premap_point = zicio_set_previous_high_premap_point(
				zicio_desc, zicio_convert_chunk_id_to_monotonic_id(
						zicio_shared_pool, zicio_shared_pool_local,
						zicio_spcb->chunk_id));
	}
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	user_buffer_idx_mod = zicio_ghost_premap(zicio_desc, zicio_spcb,
			zicio_spcb->zicio_spcb.chunk_ptr, previous_high_premap_point, 0);
	if (user_buffer_idx_mod < 0) {
		/*
		 * With many process, the number of entries whose status is changed to
		 * done by forceful unmap and the number of premapped entries and the
		 * entry used by a user can be 512 or more with a very rare probability.
		 */
		if (zicio_desc->cpu_id == raw_smp_processor_id()) {
			zicio_unmap_pages_from_local_page_table(zicio_desc);
		}
		user_buffer_idx_mod = zicio_ghost_premap(zicio_desc, zicio_spcb,
			zicio_spcb->zicio_spcb.chunk_ptr, previous_high_premap_point, 0);
	}

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	/*
	 * Set user ingestion monotonic chunk id.
	 */
	zicio_set_tracking_ingestion_monotonic_chunk_id(zicio_desc,
		user_buffer_idx_mod, monotonic_chunk_id);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	zicio_spcb->zicio_spcb.user_buffer_idx = user_buffer_idx_mod;

	if (user_buffer_idx_mod < 0) {
		zicio_dump_premap_unmap_iter(zicio_desc);
		BUG_ON(user_buffer_idx_mod < 0);
	}
#else
	BUG_ON(user_buffer_idx_mod < 0);
#endif /* CONFIG_ZICIO_DEBUG */
	
	zicio_set_exp_jiffies_to_spcb(zicio_desc, zicio_spcb);
	mb();

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu id[%d] Exp time : %llu, Current time : %llu\n",
			zicio_desc->cpu_id, atomic64_read(&zicio_spcb->zicio_spcb.exp_jiffies),
			get_jiffies_64());
	printk(KERN_WARNING "cpu id[%d] :zicio_spcb->chunk_id : %u\n",
			zicio_desc->cpu_id, zicio_spcb->chunk_id);
#endif

	/*
	 * A channel that was not derailed at the time of creating the command for
	 * the corresponding chunk may have been derailed after completing the I/O
	 * for the chunk. It needs processing.
	 */
	if (zicio_cmd->is_on_track_cmd) {
		zicio_contribute_shared_page_to_pool(zicio_desc, zicio_shared_pool,
				zicio_shared_pool_local, zicio_spcb, zicio_cmd->file_chunk_id);
		nr_nvme_pages =
				zicio_spcb->zicio_spcb.needed_pages_per_local_huge_page.counter;
#ifdef CONFIG_ZICIO_STAT
		stat_board->contributed_io_bytes
			+= nr_nvme_pages * ZICIO_NVME_PAGE_SIZE;
#endif /* CONFIG_ZICIO_STAT */
	} else {
		nr_nvme_pages = ctrl->needed_pages_per_local_huge_page[
				zicio_cmd->local_huge_page_idx].counter;
		BUG_ON(nr_nvme_pages == 0);
#ifdef CONFIG_ZICIO_STAT
		stat_board->derailed_io_bytes
			+= nr_nvme_pages * ZICIO_NVME_PAGE_SIZE;
#endif /* CONFIG_ZICIO_STAT */
	}
	zicio_spcb->zicio_spcb.chunk_size = nr_nvme_pages * ZICIO_NVME_PAGE_SIZE;

	BUG_ON(nr_nvme_pages == 0);

	PRINT_SPCB_DEBUG(zicio_spcb->zicio_spcb, zicio_desc->cpu_id,  __FILE__, __LINE__,
		__FUNCTION__);

	zicio_set_bytes(sb, user_buffer_idx_mod, zicio_spcb->zicio_spcb.chunk_size);
	/*
	 * The point of notifying the user of memory preparation should always be
	 * after all preparations have been completed.
	 */
	mb();
	zicio_set_status(sb, user_buffer_idx_mod, ENTRY_READY);

	if (atomic_read(&zicio_spcb->zicio_spcb.is_shared)) {
		atomic_set(&zicio_spcb->zicio_spcb.requested_flag_per_local_huge_page, 0);
	} else {
		atomic_set(ctrl->requested_flag_per_local_huge_page +
				zicio_cmd->local_huge_page_idx, 0);
	}
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu[%d] user buffer idx : %d status : %d\n",
			zicio_desc->cpu_id, user_buffer_idx_mod,
			zicio_read_status(sb, user_buffer_idx_mod));
#endif
	return true;
}

/*
 * zicio_adjust_mapping_and_reclaim_pages
 *
 * (1) Forceful unmapping is performed. Among the premapped pages,
 * check if there are expired pages, and if they are expired, use
 * the compare and swap operation to change the ghost entry to EMPTY.
 * (2) Read the shared bitvector, check if there is a page to be premapping
 * based on the chunk id of the current request processing, and perform it
 * for two pages. Access the hash map and increment the ref count.
 * (3) And the used pages are also unmapping if possible.
 */
int
zicio_adjust_mapping_and_reclaim_pages(zicio_descriptor *desc,
			unsigned int *distance_from_head, bool from_softirq)
{
	unsigned long premap_start_point;
	unsigned long current_user_file_chunk_id;
	unsigned int num_forceful_unmapped = 0;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	unsigned int nr_channels = 0;
#endif
#ifdef CONFIG_ZICIO_STAT
	unsigned int nr_premapped = 0;
	zicio_stat_board *stat_board = desc->stat_board;
#endif /* CONFIG_ZICIO_STAT */

	if (desc->cpu_id == raw_smp_processor_id()) {
		zicio_unmap_pages_from_local_page_table(desc);
	}

	if (!zicio_check_channel_derailed(desc)) {
		/* Premap pages */
#ifdef CONFIG_ZICIO_STAT
		*distance_from_head = zicio_premap_pages_from_shared_pool(
					desc, &premap_start_point, &current_user_file_chunk_id,
					&nr_premapped);

		if (from_softirq)
			stat_board->num_premap_in_softirq += nr_premapped;
		else
			stat_board->num_premap_in_nvme_irq += nr_premapped;
#else /* !CONFIG_ZICIO_STAT */
		*distance_from_head = zicio_premap_pages_from_shared_pool(
					desc, &premap_start_point, &current_user_file_chunk_id);
#endif /* CONFIG_ZICIO_STAT */

#ifndef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
		BUG_ON(ZICIO_MAX_NUM_GHOST_ENTRY <= *distance_from_head);
#endif /* !CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	
		/* Do forceful unmapping */
		if (zicio_access_pool_shared(desc)) {
			/*
			 * If distance_from_head is less than or equal to 1, it means that
			 * there is only one page or no page that the user will consume in
			 * the future. And If only one channel is attached in this pool,
			 * we do not need to forceful unmap for sharing.
			 * A forceful unmap should not be performed in this case.
			 */
			num_forceful_unmapped = zicio_ghost_forceful_unmap(desc);
		}
	}

	if (zicio_get_num_using_pages(desc) &&
			zicio_check_reclaiming_in_progress(desc)) {
		/*
		 * This code segment should be protected between different CPUs,
		 * between SoftIRQ Daemon and interrupt handler.
		 */
		zicio_reclaim_spcb(desc, false);
		zicio_set_reclaiming_end(desc);
	}

	/* If this channel is derailed, then IO is required */
	if (zicio_check_channel_derailed(desc)) {
		return 0;
	}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu[%d] distance_from_head %d, num_forceful_unmapped: "
		"%d, premap_start_point: %ld, current_user_file_chunk_id: %ld, "
		"avg_user_point: %ld[%s:%d][%s]\n",
			desc->cpu_id, *distance_from_head, num_forceful_unmapped,
			premap_start_point, current_user_file_chunk_id,
			zicio_get_raw_avg_user_ingestion_point(desc,
					&nr_channels, premap_start_point),
			__FILE__, __LINE__, __FUNCTION__);
#endif

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	return zicio_determine_new_request_needed_shared(
		desc, *distance_from_head);
#else /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	/* Calculate io is requured from the number of remaining page. */
	return zicio_check_io_required(desc, premap_start_point,
			(num_forceful_unmapped >= *distance_from_head) ? 0 :
					*distance_from_head - num_forceful_unmapped);
#endif /* !CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
}

/*
 * zicio_produce_data_chunk_to_user
 *
 * Produce file chunks that user will read next in the SoftIRQ Daemon.
 */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
void
zicio_produce_data_chunk_to_user(zicio_descriptor *desc, int reason,
			unsigned page_id_queue_idx, ktime_t first_page_get_time,
			bool request_for_derail)
#else
void
zicio_produce_data_chunk_to_user(zicio_descriptor *desc, int reason,
			unsigned page_id_queue_idx, bool request_for_derail)
#endif
{
	zicio_shared_pool *zicio_shared_pool;
	struct zicio_nvme_cmd_list **start_cmd_lists;
	int local_page_idx, num_device, device_idx;
	unsigned int distance_from_head;
	int num_wait_consumption;
	bool derailed;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
	ktime_t current_ktime;
#endif
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	int num_log = 0;
#endif

	/* Step 1. mapping */
	num_wait_consumption = zicio_adjust_mapping_and_reclaim_pages(desc,
				&distance_from_head, true);

	if (num_wait_consumption && !request_for_derail) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "cpu[%d] desc[%p] no IO[%s:%d][%s]\n", desc->cpu_id,
				desc, __FILE__, __LINE__, __FUNCTION__);
#endif
		/*
		 * This is a situation where after resources for a local page ID have
		 * been allocated, they are not used and must be returned.
		 *
		 * If page idx is not allocated in a situation where I/O is required,
		 * SoftIRQ will handle this. However, when SoftIRQ processes this in the
		 * future, there may already be enough data supplied to the user, or it
		 * may be beyond the range of files that can be supplied according to the
		 * consumption speed. In this case, I/O should not be performed, and
		 * previously allocated page idx must be deallocated.
		 *
		 * The code below has been implemented to handle this.
		 */
		if (reason == ZICIO_NOLOCALPAGE) {
			local_page_idx = zicio_read_page_id_from_queue(desc,
					page_id_queue_idx);
			if (local_page_idx == ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX) {
				/*
				 * (1) If the page has not been acquired, the work will be
				 *     pending in softirq until it can be acquired.
				 */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
				zicio_create_reactivate_trigger_shared(desc,
						ZICIO_NOLOCALPAGE, false, page_id_queue_idx, false,
						first_page_get_time);
#else
				zicio_create_reactivate_trigger_shared(desc,
						ZICIO_NOLOCALPAGE, false, page_id_queue_idx, false);
#endif
				return;
			} else {
				/*
				 * (2) If acquired, the channel returns the acquired page to the
				 *     page id queue. Returns are performed using free page idx.
				 */
				zicio_set_page_id_to_queue(desc, local_page_idx);
			}
		}
#ifdef CONFIG_ZICIO_STAT
		zicio_count_softirq_trigger_shared(desc, ZICIO_NOIO);
#endif /* (CONFIG_ZICIO_STAT) */
		/* Step 2-1. if mapping is enough, then we do not send I/O. */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
		zicio_create_reactivate_trigger_shared(
				desc, ZICIO_NOIO, false, -1, false, 0);
#else
		zicio_create_reactivate_trigger_shared(
				desc, ZICIO_NOIO, false, -1, false);
#endif
	} else {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "cpu[%d] desc[%p] IO[%s:%d][%s]\n", desc->cpu_id,
				desc, __FILE__, __LINE__, __FUNCTION__);
#endif
retry_create_next_chunk_in_irq:
		/* Step 2-2. if mapping is not enough, then we should send I/O. */
		local_page_idx = zicio_prepare_next_local_huge_page_id_shared(
				desc, &derailed, &page_id_queue_idx,
				(reason == ZICIO_NOLOCALPAGE) && !request_for_derail);

		zicio_shared_pool = zicio_get_shared_pool(desc);
		if (local_page_idx == -1) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "cpu[%d] desc[%p] IO[%s:%d][%s]\n", desc->cpu_id,
					desc, __FILE__, __LINE__, __FUNCTION__);
#endif
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
			if (reason != ZICIO_NOLOCALPAGE) {
#ifdef CONFIG_ZICIO_STAT
				zicio_count_softirq_trigger_shared(desc,
					ZICIO_NOLOCALPAGE);
#endif /* (CONFIG_ZICIO_STAT) */
				current_ktime = ktime_get();
			} else {
				current_ktime = first_page_get_time;
			}
#endif
			if (!request_for_derail) {
				BUG_ON(page_id_queue_idx == -1);
				/* All of local pages are used. */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
				zicio_create_reactivate_trigger_shared(desc,
						ZICIO_NOLOCALPAGE, false, page_id_queue_idx, false,
						current_ktime);
#else
				zicio_create_reactivate_trigger_shared(desc,
						ZICIO_NOLOCALPAGE, false, page_id_queue_idx, false);
#endif
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
				printk(KERN_WARNING "cpu[%d] desc[%p] IO[%s:%d][%s]\n", desc->cpu_id,
						desc, __FILE__, __LINE__, __FUNCTION__);
#endif
			} else {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
				zicio_create_reactivate_trigger_shared(desc,
						ZICIO_NOLOCALPAGE, true, -1, false, current_ktime);
#else
				zicio_create_reactivate_trigger_shared(desc,
						ZICIO_NOLOCALPAGE, true, -1, false);
#endif
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
				printk(KERN_WARNING "cpu[%d] desc[%p] IO[%s:%d][%s]\n", desc->cpu_id,
						desc, __FILE__, __LINE__, __FUNCTION__);
#endif
			}
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
			/* roll back the state */
			atomic_dec_if_positive(&zicio_shared_pool->cur_requested_chunk_count);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
			return;
		}

		/* Step 3. Create commands and pend them. */
		start_cmd_lists = zicio_create_command_shared(desc, local_page_idx,
				&num_device, !request_for_derail);

		if (start_cmd_lists == ERR_PTR(UINT_MAX - 1)) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			if (num_log++ <= 10)
				printk(KERN_WARNING "cpu[%d] desc[%p] no IO[%s:%d][%s]\n", desc->cpu_id,
						desc, __FILE__, __LINE__, __FUNCTION__);
#endif
			page_id_queue_idx = -1;
			request_for_derail = true;
			goto retry_create_next_chunk_in_irq;
		}

		if (zicio_has_cmd_list_in_set(start_cmd_lists, num_device)) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
			if (reason == ZICIO_NOLOCALPAGE) {
				zicio_add_excessive_wait_time(desc,
						ktime_get() - first_page_get_time, request_for_derail);
			}
#endif
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "cpu[%d] desc[%p] lock-and-load[%s:%d][%s]\n", desc->cpu_id,
					desc, __FILE__, __LINE__, __FUNCTION__);
#endif
			/* If we have command set to send I/O, then send it to device */
			zicio_lock_and_load(desc, start_cmd_lists, num_device,
					local_page_idx);
		} else {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "cpu[%d] desc[%p] no IO[%s:%d][%s]\n", desc->cpu_id,
					desc, __FILE__, __LINE__, __FUNCTION__);
#endif
			/* Otherwise, clean up it */
			if (start_cmd_lists) {
				device_idx = zicio_get_zicio_channel_dev_idx(desc,
						start_cmd_lists[0]->device_idx);
				zicio_free_cmd_lists_set_with_desc(desc, device_idx,
						start_cmd_lists);
			}
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
			/* roll back the state */
			atomic_dec_if_positive(&zicio_shared_pool->cur_requested_chunk_count);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
		}
	}
}

/*
 * zicio_produced_data_chunk_to_shared_pool
 *
 * Shared pool function to be executed in SoftIRQ daemon.
 */
void
zicio_produced_data_chunk_to_shared_pool(unsigned int cpu_id)
{
	zicio_shared_request_timer *shared_req_timer, *pos, *tmp;
	zicio_descriptor **active_desc_in_irq;
	struct list_head *shared_callback_list =
				per_cpu_ptr(&per_cpu_shared_callback_list, cpu_id);
	struct list_head *shared_gc_list =
				per_cpu_ptr(&per_cpu_shared_gc_list, cpu_id);
	unsigned int queue_value;
	unsigned long flags;

	while (true) {
		local_irq_save(flags);
		preempt_disable();

		if (list_empty(shared_callback_list)) {
			preempt_enable();
			local_irq_restore(flags);
			break;
		}

		shared_req_timer = list_entry(shared_callback_list->next,
				zicio_shared_request_timer, user_consume_wait_list);
		list_del(&shared_req_timer->user_consume_wait_list);

		active_desc_in_irq = per_cpu_ptr(
				&current_active_shared_desc, raw_smp_processor_id());
		*active_desc_in_irq = shared_req_timer->desc;

		preempt_enable();
		local_irq_restore(flags);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "cpu[%d] produce data chunk start while loop [%s:%d][%s]\n",
				cpu_id, __FILE__, __LINE__, __FUNCTION__);
#endif
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
		zicio_produce_data_chunk_to_user(shared_req_timer->desc,
				shared_req_timer->reason, shared_req_timer->page_id_queue_idx,
				shared_req_timer->first_page_get_time,
				shared_req_timer->derailed);
#else
		zicio_produce_data_chunk_to_user(shared_req_timer->desc,
				shared_req_timer->reason, shared_req_timer->page_id_queue_idx,
				shared_req_timer->derailed);
#endif
		zicio_free_shared_request_timer(shared_req_timer->desc,
				shared_req_timer);

		*active_desc_in_irq = NULL;
	}

	local_irq_save(flags);
	preempt_disable();

	/*
	 * Clean up request timers already created.
	 */
	list_for_each_entry_safe(pos, tmp, shared_gc_list,
			user_consume_wait_list) {
		list_del(&pos->user_consume_wait_list);
		if (pos->reason == ZICIO_NOLOCALPAGE && !pos->derailed &&
			!pos->pended) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "cpu[%d] produce data chunk [%s:%d][%s]\n", 
				cpu_id, __FILE__, __LINE__, __FUNCTION__);
#endif
			queue_value = zicio_read_page_id_from_queue(pos->desc,
					pos->page_id_queue_idx);
			if (queue_value == ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
				printk(KERN_WARNING "cpu[%d] produce data chunk [%s:%d][%s]\n", 
					cpu_id, __FILE__, __LINE__, __FUNCTION__);
#endif
				timer_setup(&pos->consume_wait_timer,
						zicio_reactivate_lag_user_request,
						TIMER_PINNED|TIMER_IRQSAFE);
				pos->consume_wait_timer.expires = get_jiffies_64();
				add_timer_on(&pos->consume_wait_timer, pos->desc->cpu_id);
			} else {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
				printk(KERN_WARNING "cpu[%d] produce data chunk [%s:%d][%s]\n", 
					cpu_id, __FILE__, __LINE__, __FUNCTION__);
#endif
				zicio_set_page_id_to_queue(pos->desc, queue_value);
				if (pos->next) {
					zicio_free_shared_request_timer(pos->next->desc,
							pos->next);
				}
				zicio_free_shared_request_timer(pos->desc, pos);
			}
		} else {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "cpu[%d] produce data chunk [%s:%d][%s]\n", 
					cpu_id, __FILE__, __LINE__, __FUNCTION__);
#endif
			if (pos->next) {
				zicio_free_shared_request_timer(pos->next->desc, pos->next);
			}
			zicio_free_shared_request_timer(pos->desc, pos);
		}
	}

	preempt_enable();
	local_irq_restore(flags);
}
EXPORT_SYMBOL(zicio_produced_data_chunk_to_shared_pool);

/*
 * zicio_wakeup_softirqd_shared
 *
 * Wakeup SoftIRQ Daemon for shared pool works(mapping, I/O).
 */
static int
zicio_wakeup_softirqd_shared(
		zicio_shared_request_timer *shared_req_timer, int cpu)
{
	struct task_struct *tsk = per_cpu(ksoftirqd, cpu);
	struct list_head *shared_callback_list =
			per_cpu_ptr(&per_cpu_shared_callback_list, cpu);
	struct zicio_shared_request_timer *new_shared_req_timer;
	unsigned long flags;
	int wakeup = 0;

	local_irq_save(flags);
	preempt_disable();

	if (!shared_req_timer->pended) {
		shared_req_timer->pended = true;
		/*
		 * Before waking up softirq daemon thread, we should add work timer
		 * to shared callback list.
		 */
		new_shared_req_timer = shared_req_timer->next;

		shared_req_timer->next = NULL;
		list_add(&new_shared_req_timer->user_consume_wait_list,
				shared_callback_list);
	}

	if (!task_is_running(tsk) || __kthread_should_park(tsk)) {
		wakeup = wake_up_process(tsk);
	}
	preempt_enable();
	local_irq_restore(flags);

	return wakeup;
}

/*
 * zicio_reactivate_lag_user_requset
 *
 * In the case of lag users or users who failed to send i/o, the softirq deamon
 * should perform the work. For this purpose, the descriptor of the channel to
 * be operated is transmitted to the softirq daemon.
 */
static void
zicio_reactivate_lag_user_request(struct timer_list *timer)
{
	zicio_shared_request_timer *shared_req_timer = from_timer(
			shared_req_timer, timer, consume_wait_timer);
	zicio_descriptor *desc = shared_req_timer->desc;
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	struct list_head *shared_gc_list =
			per_cpu_ptr(&per_cpu_shared_gc_list, desc->cpu_id);
	unsigned long flags;

	/*
	 * If channel provide all of data, then do not make procedures for SoftIRQ.
	 */
	if ((atomic_read(&zicio_shared_pool_local->num_mapped) <
		 zicio_shared_pool->shared_files.total_chunk_nums)) {
		if (!(zicio_wakeup_softirqd_shared(shared_req_timer,
				desc->cpu_id))) {
			/*
			 * Wakeup soft irq daemon for shared pool works.
			 */
			local_irq_save(flags);
			preempt_disable();

			timer_setup(&shared_req_timer->consume_wait_timer,
					zicio_reactivate_lag_user_request,
					TIMER_PINNED|TIMER_IRQSAFE);
			shared_req_timer->consume_wait_timer.expires = get_jiffies_64();
			add_timer_on(&shared_req_timer->consume_wait_timer, desc->cpu_id);

			preempt_enable();
			local_irq_restore(flags);

			return;
		}
	}

	local_irq_save(flags);
	preempt_disable();

	list_add(&shared_req_timer->user_consume_wait_list, shared_gc_list);

	preempt_enable();
	local_irq_restore(flags);
}

/*
 * zicio_get_dma_map_start_point_shared
 *
 * Get DMA map start point in device map array.
 */
int
zicio_get_dma_map_start_point_shared_with_pool(
			zicio_shared_pool *zicio_shared_pool, int device_idx)
{
	int mddev_idx, inner_dev_idx, dev_start_idx;

	if (zicio_shared_pool->shared_dev_maps.dev_maps.nr_dev > device_idx) {
		return zicio_shared_pool->shared_dev_maps.dev_map_start_point[device_idx];
	} else {
		mddev_idx = zicio_get_shared_pool_mddev_idx(
				zicio_shared_pool, device_idx);
		dev_start_idx = zicio_shared_pool->shared_dev_maps.dev_maps.dev_node_array[
				mddev_idx].start_point_inner_dev_map;
		inner_dev_idx = device_idx - dev_start_idx -
			zicio_shared_pool->shared_dev_maps.dev_maps.nr_dev;

		return zicio_shared_pool->shared_dev_maps.dev_map_start_point[mddev_idx] +
				inner_dev_idx;
	}
}
/*
 * zicio_get_dma_map_start_point_shared
 *
 * Get DMA map start point in device map array.
 */
int
zicio_get_dma_map_start_point_shared(zicio_descriptor *desc,
			int device_idx)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	return zicio_get_dma_map_start_point_shared_with_pool(
			zicio_shared_pool, device_idx);
}

/*
 * zicio_create_reactivate_trigger_shared
 *
 * Create a reactivate trigger for the shared pool.
 */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
void
zicio_create_reactivate_trigger_shared(zicio_descriptor *desc,
		int reason, bool derailed, unsigned page_id_queue_idx, bool pended,
		ktime_t first_page_get_time)
#else
void
zicio_create_reactivate_trigger_shared(zicio_descriptor *desc,
		int reason, bool derailed, unsigned page_id_queue_idx, bool pended)
#endif
{
	zicio_shared_request_timer *shared_req_timer;
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);

	if ((atomic_read(&zicio_shared_pool_local->num_mapped) >=
			zicio_shared_pool->shared_files.total_chunk_nums) &&
			(reason != ZICIO_NOLOCALPAGE || derailed)) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		if (zicio_check_channel_derailed(desc)) {
			printk(KERN_WARNING "cpu[%d] Stop reactivate trigger in the derailed mode "
				"[%s:%d][%s]\n", desc->cpu_id, __FILE__, __LINE__, __FUNCTION__);
		} else {
			printk(KERN_WARNING "cpu[%d] Stop reactivate trigger in the shared mode "
					"[%s:%d][%s]\n", desc->cpu_id, __FILE__, __LINE__, __FUNCTION__);
		}
#endif
		return;
	}

	mb();

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
	/* Get shared request timer to set */
	shared_req_timer = zicio_get_shared_request_timer(desc, reason,
			derailed, page_id_queue_idx, pended, first_page_get_time);
	shared_req_timer->next = zicio_get_shared_request_timer(desc, reason,
			derailed, page_id_queue_idx, pended, first_page_get_time);
#else
	/* Get shared request timer to set */
	shared_req_timer = zicio_get_shared_request_timer(desc, reason,
			derailed, page_id_queue_idx, pended);
	shared_req_timer->next = zicio_get_shared_request_timer(desc, reason,
			derailed, page_id_queue_idx, pended);
#endif
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	if (zicio_check_channel_derailed(desc)) {
		printk(KERN_WARNING "cpu[%d] Reactivate trigger in the derailed mode "
				"[%s:%d][%s]\n", desc->cpu_id, __FILE__, __LINE__, __FUNCTION__);
	} else {
		printk(KERN_WARNING "cpu[%d] Reactivate trigger in the shared mode "
				"[%s:%d][%s]\n", desc->cpu_id, __FILE__, __LINE__, __FUNCTION__);
	}
#endif

	/*
	 * Set time and callback to execute and pend linux timer to our job.
	 */
	timer_setup(&shared_req_timer->consume_wait_timer,
				zicio_reactivate_lag_user_request,
				TIMER_PINNED|TIMER_IRQSAFE);
	shared_req_timer->consume_wait_timer.expires = get_jiffies_64();
	add_timer_on(&shared_req_timer->consume_wait_timer, desc->cpu_id);
}
EXPORT_SYMBOL(zicio_create_reactivate_trigger_shared);

/*
 * zicio_init_shared_callback_list
 *
 * Initialize per cpu shared callback function list
 */
void __init zicio_init_shared_callback_list(void)
{
	zicio_descriptor **active_desc_in_irq;
	int i;

	for (i = 0 ; i < NR_CPUS ; i++) {
		/*
		 * Initialize per cpu shared callback list
		 */
		INIT_LIST_HEAD(per_cpu_ptr(&per_cpu_shared_callback_list, i));
		INIT_LIST_HEAD(per_cpu_ptr(&per_cpu_shared_gc_list, i));
		/*
		 * Initialize current active shared decriptor
		 */
		active_desc_in_irq = per_cpu_ptr(&current_active_shared_desc, i);
		*active_desc_in_irq = NULL;
	}
}

/*
 * zicio_remove_lag_user_request
 *
 * Remove lag user request matched with @desc
 */
void
zicio_remove_lag_user_request(zicio_descriptor *desc)
{
	zicio_shared_request_timer *pos, *tmp;
	struct list_head *shared_callback_list =
				per_cpu_ptr(&per_cpu_shared_callback_list, desc->cpu_id);
	struct list_head *shared_gc_list =
				per_cpu_ptr(&per_cpu_shared_gc_list, desc->cpu_id);
	unsigned long flags;
	unsigned int queue_value;
	volatile unsigned int num_shared_jobs;

	num_shared_jobs = atomic_read(
			(atomic_t *)&desc->zicio_shared_pool_desc->zicio_num_works);
	do {
		local_irq_save(flags);
		preempt_disable();
		/*
		 * Clean up request timers already created.
		 */
		list_for_each_entry_safe(pos, tmp, shared_callback_list,
				user_consume_wait_list) {
			if (pos->desc == desc) {
				list_del(&pos->user_consume_wait_list);
				if (!pos->derailed && pos->reason == ZICIO_NOLOCALPAGE) {
					do {
						queue_value = zicio_read_page_id_from_queue(desc,
								pos->page_id_queue_idx);
					} while (queue_value == ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX);
					zicio_set_page_id_to_queue(desc, queue_value);
				}
				zicio_free_shared_request_timer(pos->desc, pos);
			}
		}
		/*
		 * Clean up request timers already created.
		 */
		list_for_each_entry_safe(pos, tmp, shared_gc_list,
				user_consume_wait_list) {
			if (pos->desc == desc) {
				list_del(&pos->user_consume_wait_list);
				if (!pos->derailed && pos->reason == ZICIO_NOLOCALPAGE &&
					!pos->pended) {
					do {
						queue_value = zicio_read_page_id_from_queue(desc,
								pos->page_id_queue_idx);
					} while (queue_value == ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX);
					zicio_set_page_id_to_queue(desc, queue_value);
				}
				if (pos->next) {
					zicio_free_shared_request_timer(pos->next->desc,
							pos->next);
				}
				zicio_free_shared_request_timer(pos->desc, pos);
			}
		}
		preempt_enable();
		local_irq_restore(flags);

		mb();
		/* There's a possibility to SoftIRQ Daemon refer to a timer from this
		 * descriptor. To handle this, wait that jobs are finishied. */
wait_reserved_jobs:
		local_irq_save(flags);
		if (this_cpu_read(current_active_shared_desc) == desc) {
			local_irq_restore(flags);
			goto wait_reserved_jobs;
		}
		local_irq_restore(flags);

		mb();
		num_shared_jobs = atomic_read(
				(atomic_t *)&desc->zicio_shared_pool_desc->zicio_num_works);
		if (num_shared_jobs) {
			yield();
		}
	} while (num_shared_jobs);
}

/*
 * zicio_create_new_spcb
 */
int
zicio_create_new_spcb(zicio_descriptor *desc)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_page_control_block *zicio_spcb;
	int page_id = atomic_fetch_inc(&zicio_shared_pool->num_spcb);

	if (page_id >= ZICIO_NUM_MAX_SPCB) {
		atomic_dec(&zicio_shared_pool->num_spcb);
		return ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX;
	}

	zicio_spcb = zicio_create_spcb(zicio_shared_pool, page_id);

	if (!zicio_spcb) {
		atomic_dec(&zicio_shared_pool->num_spcb);
		return ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX;
	}

	atomic64_set(zicio_shared_pool->zicio_spcb + page_id, (unsigned long)zicio_spcb);

	return page_id;
}


/*
 * zicio_get_new_spcb
 *
 * Create new spcb for attached shared pool. And initialize its variables.
 *
 * desc: zicio_descriptor pointer
 */
int
zicio_get_new_spcb(zicio_descriptor *desc)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	zicio_shared_page_control_block *zicio_spcb;
	/* Create new spcb */
	int page_id = zicio_create_new_spcb(desc);

	zicio_spcb = zicio_get_spcb_with_id(desc, page_id);

	atomic_inc(&zicio_spcb->zicio_spcb.ref_count);
	BUG_ON((atomic_read(&zicio_spcb->zicio_spcb.ref_count) != 1));
	atomic_set(&zicio_spcb->zicio_spcb.is_shared, true);
	atomic_set(&zicio_spcb->zicio_spcb.is_used, true);
	atomic_inc(&zicio_shared_pool_local->num_shared_pages);
	atomic_inc(&zicio_shared_pool_local->num_using_pages);

	zicio_init_next_local_huge_page_shared(desc, page_id);

	mb();
	zicio_set_contribute_shared_page_control_block(desc, zicio_spcb);

	return page_id;
}

/*
 * zicio_do_init_premapping
 *
 * Do initial premapping for channel started.
 */
int
zicio_do_init_premapping(zicio_descriptor *desc,
		unsigned int *premap_start_point_no_mod)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	zicio_shared_page_control_block *zicio_spcb;
	zicio_bitvector *zicio_shared_bitvector, *zicio_local_bitvector;
	int num_premapped = 0, user_buffer_idx_mod, buffer_idx;
	int prev_user_buffer_idx_mod = -1;
	unsigned long expected_safe_jiffies, current_jiffies;
	unsigned long bitvector_idx_no_mod;
	unsigned int current_chunk_id_mod;
	int distance_from_head;
	int bitvector_check_result;
	unsigned int high_premap_point;
	int i;
	int prepared_chunk_nums = 0;
	u32 prepared_bitvector_id_no_mod[64];
	u32 prepared_chunk_id_mod[64];
	zicio_shared_page_control_block *prepared_spcb[64];
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	unsigned int avg_user_ingestion_point = 0;
	zicio_attached_channel *zicio_channel = zicio_shared_pool_local->zicio_channel;
	unsigned int monotonic_chunk_id = 0;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	/* Get bitvectors */
	zicio_shared_bitvector = zicio_get_shared_bitvector(desc);
	zicio_local_bitvector = zicio_get_local_bitvector(desc);

	/* Get current and previous head idx */
	*premap_start_point_no_mod = atomic_read(&(zicio_shared_pool->head));

	/* Get the current time and calculate the expected safe time. */
	current_jiffies = get_jiffies_64();
	expected_safe_jiffies = current_jiffies + 1;

	/* Get start point for premap */
	bitvector_idx_no_mod = *premap_start_point_no_mod;

	/* Do premap from head to a premap available point */
	while (bitvector_idx_no_mod != ULONG_MAX) {
		current_chunk_id_mod = bitvector_idx_no_mod %
				zicio_shared_pool->shared_files.total_chunk_nums;

		/* Check the shared bitvector and local bitvector matched with chunk */
		if (!(bitvector_check_result = zicio_check_bitmaps_to_premap_init(
				zicio_shared_bitvector, zicio_local_bitvector,
				current_chunk_id_mod))) {
			/*
			 * Checking the bits matched with chunk id is 00.  If value is 00,
			 * then it means we don't have to check previous result to premap.
			 */
			break;
		} else if (bitvector_check_result == -1) {
			bitvector_idx_no_mod--;
			continue;
		} else {
			/* If we can premap this chunk, than premap it */
			zicio_spcb = zicio_check_spcb_to_premap(desc, zicio_shared_pool,
					current_chunk_id_mod, current_jiffies,
					expected_safe_jiffies);

			/* If we cannot get spcb, than turn off bitvector atomic */
			if (!zicio_spcb) {
				zicio_test_and_clear_bitvector_atomic(zicio_local_bitvector,
						current_chunk_id_mod);
				bitvector_idx_no_mod--;
				continue;
			}

			prepared_bitvector_id_no_mod[prepared_chunk_nums] = 
														bitvector_idx_no_mod;
			prepared_chunk_id_mod[prepared_chunk_nums] = current_chunk_id_mod;
			prepared_spcb[prepared_chunk_nums] = zicio_spcb;
			prepared_chunk_nums += 1;

			if ((atomic_read(&zicio_shared_pool_local->num_mapped) +
					prepared_chunk_nums) ==
					zicio_shared_pool->shared_files.total_chunk_nums) {
				break;
			}
		}

		bitvector_idx_no_mod--;
	}

	BUG_ON(prepared_chunk_nums > 64);

	/* Do pre-mapping. */
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	if (desc->channel_index == 2)
		printk(KERN_WARNING "[ZICIO] pre-mapping start ----------------\n");
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	for (i = prepared_chunk_nums - 1; i >= 0; --i) {
		/* Get target spcb. */
		zicio_spcb = prepared_spcb[i];
		current_chunk_id_mod = prepared_chunk_id_mod[i];
		bitvector_idx_no_mod = prepared_bitvector_id_no_mod[i];

		/* Do premapping */
		if ((user_buffer_idx_mod =
				zicio_do_premap_page_from_shared_pool(desc,
						zicio_spcb, 0, num_premapped)) < 0) {
			zicio_test_and_clear_bitvector_atomic(zicio_local_bitvector,
					current_chunk_id_mod);
			continue;
		} else {
			/* If premap is successful, we set its ID and user buffer index
			 * to initialize this channel */
			prev_user_buffer_idx_mod = user_buffer_idx_mod;
			if (high_premap_point < bitvector_idx_no_mod)
				high_premap_point = bitvector_idx_no_mod;
		}

		num_premapped++;

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
		monotonic_chunk_id
			= zicio_convert_chunk_id_to_monotonic_id(
				zicio_shared_pool, zicio_shared_pool_local, zicio_spcb->chunk_id);

		/*
		 * Set user ingestion monotonic chunk id. This could be used to
		 * calculate the average ingestion point of users.
		 */
		zicio_set_tracking_ingestion_monotonic_chunk_id(
			desc, user_buffer_idx_mod, monotonic_chunk_id);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

		/*
		 * The point of notifying the user of memory preparation should
		 * always be after all preparations have been completed.
		 */
		mb();
		zicio_set_status(desc->switch_board, 
									user_buffer_idx_mod, ENTRY_READY);

#ifdef CONFIG_ZICIO_STAT
		zicio_add_endowed_pages(desc);
		zicio_add_premapped_pages(desc);
#endif
	}
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	if (desc->channel_index == 2)
		printk(KERN_WARNING "[ZICIO] pre-mapping finish ---------------\n");
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	/*
	 * If we can do ghost premapping pages, then initializing channel by using
	 * this information.
	 */
	if (num_premapped) {
		/* Set high premap point */
		zicio_set_previous_high_premap_point(desc,
				*premap_start_point_no_mod);
		/* The high premap point is set using the largest previously
		 * premapped chunk id. As this information is not currently available,
		 * we currently use the smallest premapped chunk ID. */
		for (buffer_idx = 0 ;
			 buffer_idx < prev_user_buffer_idx_mod ; buffer_idx++) {
			zicio_set_high_premap_point_only(desc, buffer_idx,
					high_premap_point);
		}
		/* Set consume indicator */
		zicio_set_consume_indicator(desc, high_premap_point);
	}

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	/* If this channel is derailed, then IO is required */
	if (zicio_check_channel_derailed(desc))
		return ZICIO_IO_REQUIRED;

	avg_user_ingestion_point =
		zicio_get_avg_user_ingestion_monotonic_chunk_id(
			desc, *premap_start_point_no_mod);

	distance_from_head =
		*premap_start_point_no_mod <= avg_user_ingestion_point ?
		0 : *premap_start_point_no_mod - avg_user_ingestion_point;

	return zicio_determine_new_request_needed_shared(
		desc, distance_from_head);
#else /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	distance_from_head = num_premapped;

	return zicio_check_io_required(desc, *premap_start_point_no_mod,
			distance_from_head);
#endif /* !CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
/*
 * __zicio_dump_shared_page_control_block
 *
 * Dump shared pool hash slot
 */
static void 
zicio_dump_shared_page_control_block(
		zicio_shared_page_control_block *zicio_spcb)
{
	const char *stat_string[2] = {"non-shared", "shared"};
	int shared = atomic_read(&zicio_spcb->zicio_spcb.is_shared);

	printk(KERN_WARNING "page's status is %s\n", stat_string[shared]);

	printk(KERN_WARNING "Ref counter: %u\n",
				atomic_read(&zicio_spcb->zicio_spcb.ref_count));
	printk(KERN_WARNING "Expiration time: %llu, Current time: %llu\n",
			atomic64_read(&zicio_spcb->zicio_spcb.exp_jiffies), get_jiffies_64());

	if (get_jiffies_64() > atomic64_read(&zicio_spcb->zicio_spcb.exp_jiffies)) {
		printk(KERN_WARNING "Shared page is currently expired(not accurate)\n");
	}
	PRINT_SPCB_DEBUG(zicio_spcb->zicio_spcb, raw_smp_processor_id(),
				__FILE__, __LINE__, __FUNCTION__);
}

/*
 * __zicio_rcu_hlist_dump_for_each_entry
 *
 * Dump shared pool hash slot
 */
static inline void
__zicio_rcu_hlist_dump_for_each_entry(
			struct zicio_rcu_hlist_head *rcu_hash)
{
	zicio_shared_page_control_block *zicio_shared_pool_hash = NULL;
	zicio_shared_page_control_block *zicio_spcb = NULL;

	rcu_read_lock();

	hlist_for_each_entry_rcu(zicio_spcb, &rcu_hash->head, hash_node) {
		zicio_shared_pool_hash = zicio_spcb;
		zicio_dump_shared_page_control_block(zicio_shared_pool_hash);
	}

	rcu_read_unlock();
}

#define zicio_rcu_hash_dump(hashtable, num_slot)					\
		__zicio_rcu_hlist_dump_for_each_entry(&hashtable[num_slot])

/*
 * zicio_dump_shared_pool_hash_table
 *
 * Dump shared pool hash table
 */
static void
zicio_dump_shared_pool_hash_table(zicio_shared_pool *zicio_shared_pool)
{
	int i;

	for (i = 0 ; i < (1UL << ZICIO_HASH_ORDER) ; i++) {
		zicio_rcu_hash_dump(zicio_shared_pool->shared_pool_hash, i);
	}
}

/*
 * zicio_get_flags_bitvector_status_level0_for_dump
 *
 * Get the flag bitvector status of level 0
 */
static unsigned int
zicio_get_flags_bitvector_status_level0_for_dump(
			zicio_bitvector_t *bit_vector, unsigned int in_long_offset)
{
	unsigned long bitmap = *((unsigned long *)bit_vector);

	return ((bitmap >> (in_long_offset << 1)) &
			(ZICIO_BIT_VALID|ZICIO_BIT_REF));
}

/*
 * zicio_dump_shared_bitvector_status_level0
 *
 * Dump the status of shared pool bitvector of level 0
 */
static void
zicio_dump_shared_bitvector_status_level0(zicio_shared_pool *zicio_shared_pool)
{
	zicio_bitvector_t *bit_vector;
	unsigned int num_chunks;
	unsigned int i, in_bitvector_offset, in_long_offset;
	zicio_bitvector_t *current_bitvector;
	zicio_shared_page_control_block *zicio_spcb;
	unsigned long flags;

	bit_vector = zicio_shared_pool->bitvector.bit_vector[0];
	num_chunks = zicio_shared_pool->bitvector.num_chunks;

	for (i = 0 ; i < num_chunks ; i++) {
		in_bitvector_offset = i / 32;
		in_long_offset = i % 32;
		current_bitvector = &bit_vector[in_bitvector_offset];
		if ((flags = zicio_get_flags_bitvector_status_level0_for_dump(
						current_bitvector, in_long_offset))) {
			printk(KERN_WARNING "File chunkd[%u] :", i);
			if (flags & ZICIO_BIT_VALID) {
				printk(KERN_WARNING "Read ");
			}
			if (flags & ZICIO_BIT_REF) {
				printk(KERN_WARNING "Referred ");
				zicio_spcb = zicio_rcu_hash_find(
						zicio_shared_pool->shared_pool_hash, i);
				if (!zicio_spcb) {
					/* If changed by another CPU, the cached value can be
					 * read. */
					if (zicio_get_flags_bitvector_status_level0_for_dump(
							current_bitvector, in_long_offset) &
							ZICIO_BIT_REF) {
						mb();
					}
				} else {
					if (i != zicio_spcb->chunk_id) {
						printk(KERN_WARNING "!!!!!!!! Referred block isn't "
								"matched !!!!!!!!\n");
					}
					zicio_dump_shared_page_control_block(zicio_spcb);
				}
			}
		}
	}
}

/*
 * zicio_dump_shared_bitvector_status_level1
 *
 * Dump the status of shared pool bitvector of level 1
 */
static void
zicio_dump_shared_bitvector_status_level1(
			zicio_shared_pool *zicio_shared_pool)
{
	unsigned int num_chunks, max_bits;
	unsigned int i, in_bitvector_offset, in_long_offset;
	zicio_bitvector_t *current_bitvector;
	zicio_bitvector_t *bit_vector;
	unsigned long flags;

	bit_vector = zicio_shared_pool->bitvector.bit_vector[1];
	num_chunks = zicio_shared_pool->bitvector.num_chunks;
	max_bits = DIV_ROUND_UP(num_chunks, 256);

	for (i = 0 ; i < max_bits ; i++) {
		in_bitvector_offset = i / 64;
		in_long_offset = i % 64;
		current_bitvector = &bit_vector[in_bitvector_offset];
		flags = 1UL << in_long_offset;

		if (flags & *((unsigned long *)current_bitvector)) {
			printk(KERN_WARNING "Internal bit[%u][%u ~ %u] is turned on\n",
				i, i * 512, (i + 1) * 512 - 1);
		}
	}
}

/*
 * zicio_dump_local_bitvector_status
 *
 * Dump the status of local bitvector
 */
static void
zicio_dump_local_bitvector_status(
			zicio_bitvector *local_bitvector, int depth)
{
	unsigned int num_chunks, max_bits;
	unsigned int i, in_bitvector_offset, in_long_offset;
	zicio_bitvector_t *current_bitvector;
	zicio_bitvector_t *bit_vector;
	unsigned long flags;

	bit_vector = local_bitvector->bit_vector[depth];
	num_chunks = local_bitvector->num_chunks;

	if (depth == 1) {
		max_bits = DIV_ROUND_UP(num_chunks, 512);
	} else if (depth == 0) {
		max_bits = num_chunks;
	}

	for (i = 0 ; i < max_bits ; i++) {
		in_bitvector_offset = i / 64;
		in_long_offset = i % 64;
		current_bitvector = &bit_vector[in_bitvector_offset];
		flags = 1UL << in_long_offset;

		if (!(flags & *((unsigned long *)current_bitvector))) {
			if (depth == 1) {
				printk(KERN_WARNING "Internal bit[%u][%u ~ %u] is turned on\n",
						i, i * 512, (i + 1) * 512 - 1);
			} else if (depth == 0) {
				printk(KERN_WARNING "Leaf bit[%u] is turned on\n", i);
			}
		}
	}
}

/*
 * zicio_dump_shared_bitvector
 *
 * Dump shared bitvector of shared pool
 */
static void
zicio_dump_shared_bitvector(zicio_shared_pool *zicio_shared_pool)
{
	zicio_bitvector *shared_zicio_bitvector = &zicio_shared_pool->bitvector;
	int i;
	printk(KERN_WARNING "[SHARED BITVECTOR DUMP]\n");
	if (!shared_zicio_bitvector->shared) {
		printk(KERN_WARNING "!!!!!!!! WARNING shared bitvector's flag turned off"
					" !!!!!!!!\n");
	}

	printk(KERN_WARNING "Numbers of managed chunk : %u\n",
				shared_zicio_bitvector->num_chunks);
	printk(KERN_WARNING "Depth of shared bitvector : %d\n",
				shared_zicio_bitvector->depth);

	printk(KERN_WARNING "\n");
	for (i = shared_zicio_bitvector->depth  - 1 ; i >=  0 ; i--) {
		printk(KERN_WARNING "[SHARED BITVECTOR DEPTH[%d]\n", i);
		printk(KERN_WARNING "Length : %u, Num u64 : %u\n",
					shared_zicio_bitvector->bitvector_length[i],
					shared_zicio_bitvector->bitvector_length[i] >>
					ZICIO_BYTE_MIN_ORDER);
		if (i == 1) {
			zicio_dump_shared_bitvector_status_level1(zicio_shared_pool);
			printk(KERN_WARNING "\n");
		} else if (i == 0) {
			zicio_dump_shared_bitvector_status_level0(zicio_shared_pool);
		}
	}
}

/*
 * zicio_dump_shared_files
 *
 * Dump shared file struct
 */
static void
zicio_dump_shared_files(zicio_shared_pool *zicio_shared_pool)
{
	unsigned int i;
	zicio_shared_files *shared_files = &zicio_shared_pool->shared_files;
	unsigned int num_files = shared_files->registered_read_files.num_fds;

	printk(KERN_WARNING "[SHARED FILE DUMP]\n");
	printk(KERN_WARNING "Number of files : %u\n", num_files);
	printk(KERN_WARNING "Number of file chunks : %u\n", 
				shared_files->total_chunk_nums);
	for (i = 0 ; i < num_files ; i++) {
		printk(KERN_WARNING "%u ", shared_files->start_chunk_nums[i]);
	}
}

/*
 * zicio_dump_shared_metadata_ctrl
 *
 * Dump shared metadata ctrl
 */
static void
zicio_dump_shared_metadata_ctrl(zicio_shared_pool *zicio_shared_pool,
			unsigned int num_files)
{
	unsigned int i;

	printk(KERN_WARNING "[SHARED METADATA CTRL DUMP]\n");
	printk(KERN_WARNING "Number of files : %u\n", num_files);
	printk(KERN_WARNING "Numbers of metadata : ");
	for (i = 0 ; i < num_files ; i++) {
		printk(KERN_WARNING "%lu ", zicio_shared_pool->
				shared_metadata_ctrl.file_start_point_extent[i]);
	}
	printk(KERN_WARNING "%lu\n",
			zicio_shared_pool->shared_metadata_ctrl.num_metadata);
}

/*
 * zicio_dump_channel
 *
 * Dump one channel
 */
static void
__zicio_dump_channel(zicio_attached_channel *zicio_channel)
{
	zicio_shared_pool_local *zicio_shared_pool_local;
	zicio_descriptor *desc;
	int user_buffer_idx;
	int ingestion_monotonic_chunk_id;
	int i;

	if (atomic_read(&zicio_channel->derailed)) {
		printk(KERN_WARNING "DERAILED\n");
	} else {
		printk(KERN_WARNING "ON-TRACK\n");
	}

	for  (i = 0 ; i < ZICIO_LOCAL_DATABUFFER_CHUNK_NUM ; i++) {
		zicio_dump_shared_page_control_block(
					zicio_channel->local_zicio_spcb[i]);
	}

	desc = zicio_channel->desc;

	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);

	user_buffer_idx = zicio_get_user_buffer_idx(desc);

	ingestion_monotonic_chunk_id =
		zicio_get_tracking_ingestion_monotonic_chunk_id(desc, user_buffer_idx);

	printk(KERN_WARNING "num_processed : %u\n",
			atomic_read(&zicio_shared_pool_local->num_mapped));
	printk(KERN_WARNING "num_using_pages : %u\n",
			atomic_read(&zicio_shared_pool_local->num_using_pages));
	printk(KERN_WARNING "start_chunk_id : %lu\n",
			zicio_shared_pool_local->consume_indicator.start_chunk_id_no_mod);
	printk(KERN_WARNING "current_chunk_id : %lu\n",
			zicio_shared_pool_local->consume_indicator.current_chunk_id_mod);
	printk(KERN_WARNING "start_metadata : %lu\n",
			zicio_shared_pool_local->consume_indicator.start_metadata);
	printk(KERN_WARNING "current_metadata : %lu\n",
			zicio_shared_pool_local->consume_indicator.current_metadata);
	printk(KERN_WARNING "current_file_idx : %d\n",
			zicio_shared_pool_local->consume_indicator.current_file_idx);

	printk(KERN_WARNING "cpu[%d] desc->channel_index : %d\n",
			desc->cpu_id, desc->channel_index);

	printk(KERN_WARNING "last_forcefully_unmapped_monotonic_chunk_id: %d\n",
			atomic_read(&zicio_channel->last_forcefully_unmapped_monotonic_chunk_id));

	printk(KERN_WARNING "desc->stat_board->nr_consumed_chunk: %d\n",
			desc->switch_board->nr_consumed_chunk);

	printk(KERN_WARNING "user ingestion monotonic chunk id: %d\n",
			ingestion_monotonic_chunk_id);

	printk(KERN_WARNING "user buffer idx: %d\n",
			user_buffer_idx);

	printk(KERN_WARNING "low watermark: %d\n",
				zicio_get_pagenum_in_a_jiffy(desc));

	printk(KERN_WARNING "pid: %ld\n",
				desc->zicio_current->pid);

	printk(KERN_WARNING "Depth : %d\n", 1);
	//zicio_dump_local_bitvector_status(
	//		&zicio_shared_pool_local->local_bitvector, 1);

	printk(KERN_WARNING "Depth : %d\n", 0);
	//zicio_dump_local_bitvector_status(
	//		&zicio_shared_pool_local->local_bitvector, 0);
}

/*
 * zicio_dump_channel
 *
 * Dump one channel in shared pool
 */
static void
zicio_dump_channel(void *zicio_channel)
{
	__zicio_dump_channel(zicio_channel);
}

/*
 * zicio_dump_channels_in_shared_pool
 *
 * Dump channels in shared_pool
 */
static void
zicio_dump_channels_in_shared_pool(zicio_shared_pool *zicio_shared_pool)
{
	printk(KERN_WARNING "[SHARED POOL CHANNEL DUMP]\n");

	printk(KERN_WARNING "\n");

	zicio_iterate_all_zicio_struct(&zicio_shared_pool->zicio_channels,
			zicio_dump_channel, false);
}

/*
 * zicio_dump_shared_pool
 *
 * Dump one zicio shared pool
 */
void
__zicio_dump_shared_pool(zicio_shared_pool *zicio_shared_pool)
{
	printk(KERN_WARNING "[ZICIO SHAREDPOOL DUMP]\n");
			
	printk(KERN_WARNING "Pin : %u\n", atomic_read(&zicio_shared_pool->pin));
	printk(KERN_WARNING "Head : %u\n", atomic_read(&zicio_shared_pool->head));
	printk(KERN_WARNING "total chunk num: %lu\n",
		(uint64_t) zicio_shared_pool->shared_files.total_chunk_nums);

	/**** bit vector dump ****/
	printk(KERN_WARNING "\n");
	//zicio_dump_shared_bitvector(zicio_shared_pool);

	/**** hash table dump ****/
	printk(KERN_WARNING "\n");
	//zicio_dump_shared_pool_hash_table(zicio_shared_pool);

	/**** shared files dump ****/
	printk(KERN_WARNING "\n");
	zicio_dump_shared_files(zicio_shared_pool);

	/**** channels in shared pool dump ****/
	printk(KERN_WARNING "\n");
	zicio_dump_channels_in_shared_pool(zicio_shared_pool);

	/**** shared metadata buffer dump ****/
	printk(KERN_WARNING "\n");
	zicio_dump_shared_metadata_ctrl(zicio_shared_pool,
				zicio_get_num_files_shared(zicio_shared_pool));
}

/*
 * zicio_dump_shared_pool
 *
 * Dump one zicio shared pool
 */
void
zicio_dump_shared_pool(void *zicio_shared_pool)
{
	__zicio_dump_shared_pool(zicio_shared_pool);
}
#endif
