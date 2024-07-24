#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/zicio_notify.h>
#include <linux/bits.h>
#include <linux/nospec.h>
#include <linux/stat.h>

#include "zicio_desc.h"
#include "zicio_extent.h"
#include "zicio_files.h"
#include "zicio_mem.h"
#include "zicio_req_submit.h"
#include "zicio_shared_pool.h"
#include "zicio_shared_pool_mgr.h"

static zicio_id_allocator zicio_shared_pools;
u64 zicio_nsecs_from_jiffy;

/*
 * zicio_get_nsecs_from_jiffy
 *
 * Get nano seconds from a jiffy
 */
u64
zicio_get_nsecs_from_jiffy(void)
{
	BUG_ON(!zicio_nsecs_from_jiffy);
	return zicio_nsecs_from_jiffy;
}

/*
 * zicio_init_nsecs_from_jiffy
 *
 * Initialize nano seconds from a jiffy
 */
static void
zicio_init_nsecs_from_jiffy(void)
{
	zicio_nsecs_from_jiffy = jiffies_to_nsecs(1);
}

/*
 * zicio_get_shared_pool_from_id
 *
 * Get shared pool singleton using shared pool id
 */
zicio_shared_pool *
zicio_get_shared_pool_from_id(unsigned int encoded_id)
{
	return zicio_get_zicio_struct(&zicio_shared_pools,
				ZICIO_SHARED_POOL_KEY_DECODE(encoded_id), false);
}

/*
 * zicio_get_unused_shared_pool_id
 *
 * Get an unused shared pool id for allocation
 */
unsigned int
zicio_get_unused_shared_pool_id(void)
{
	/*
	 * Make the compiler quiet in error checking. The return value is 32 bits.
	 */
	unsigned long raw_shared_pool_key = zicio_get_unused_id(
				&zicio_shared_pools);

	return (IS_ERR_VALUE(raw_shared_pool_key)) ? raw_shared_pool_key :
				ZICIO_SHARED_POOL_KEY_ENCODE(raw_shared_pool_key);
}

/*
 * zicio_unref_shared_pool
 *
 * When detaching shared pool, get shared pool from and decrease ref count and
 * return it.
 */
zicio_shared_pool *
zicio_unref_shared_pool(zicio_shared_pool_key_t encoded_key)
{
	zicio_shared_pool *zicio_shared_pool;

	/* Get shared pool using ID */
	zicio_shared_pool = zicio_get_shared_pool_from_id(encoded_key);

	if (zicio_shared_pool) {
		atomic_dec(&zicio_shared_pool->pin);
	}

	return zicio_shared_pool;
}

/*
 * zicio_install_shared_pool
 *
 * Install zicio shared pool to shared pool table
 */
void 
zicio_install_shared_pool(unsigned int encoded_id,
		zicio_shared_pool* zicio_shared_pool)
{
	/* Install zicio shared pool with shared pool ID */
	zicio_install_zicio_struct(&zicio_shared_pools,
			ZICIO_SHARED_POOL_KEY_DECODE(encoded_id), zicio_shared_pool);
}

/*
 * zicio_pick_shared_pool - pick up zicio shared pool with id
 * Picked pool removed from global manager.
 */
zicio_shared_pool *
zicio_pick_shared_pool(unsigned int encoded_id)
{
	return zicio_pick_id(&zicio_shared_pools,
				ZICIO_SHARED_POOL_KEY_DECODE(encoded_id));
}

/*
 * zicio_allocate_and_bind_shared_pool
 *
 * Allocate shared pool and bind it to the shared pool manager
 *
 * @devices: device array
 * @fs: file id array
 * @fd: file struct array
 * @nr_shareable_fd: number of shareable file descriptor
 * @file_dev_map: file device map
 */
long
zicio_allocate_and_bind_shared_pool(struct device **devices,
		unsigned int *fs, struct fd *fd, unsigned int nr_shareable_fd,
		int *file_dev_map)
{
	struct zicio_shared_pool *zicio_shared_pool;
	long ret = -ENOMEM;
	zicio_shared_pool_key_t shared_pool_key;

	/*
	 * Get shared pool key
	 */
	ret = zicio_get_unused_shared_pool_id();

	if (ret < 0) {
		return ret;
	}

	shared_pool_key = ret;
	/*
	 * Allocate shared pool
	 */
	zicio_shared_pool = zicio_create_shared_pool(devices, fs, fd, file_dev_map,
				nr_shareable_fd, shared_pool_key);

	if (unlikely(!zicio_shared_pool)) {
		zicio_pick_shared_pool(shared_pool_key);
		return -ENOMEM;
	}

	if ((ret = zicio_initialize_extent_and_metadata_shared(zicio_shared_pool,
				fd, nr_shareable_fd))) {
		printk(KERN_WARNING 
			"[ZICIO Message] Error in initialize shared pool metadata\n");
		goto l__zicio_allocate_and_bind_shared_pool_err;
	}

#ifdef CONFIG_ZICIO_STAT
	zicio_set_channel_start_to_stat_shared(&zicio_shared_pool->stat_board);
#endif /* CONFIG_ZICIO_STAT */

	/*
	 * Bind key and shared pool
	 */
	zicio_install_shared_pool(shared_pool_key, zicio_shared_pool);

	return shared_pool_key;

l__zicio_allocate_and_bind_shared_pool_err:
	zicio_delete_shared_pool(zicio_shared_pool);
	zicio_pick_shared_pool(shared_pool_key);
	return ret;
}

/*
 * zicio_allocate_and_initialize_shared_pool
 *
 * Allocate and initialize shared pool
 */
long
zicio_allocate_and_initialize_shared_pool(struct device **devices,
		unsigned int *fs, struct fd *fd, unsigned int nr_shareable_fd,
		int *file_dev_map)
{
	return zicio_allocate_and_bind_shared_pool(devices, fs, fd,
			nr_shareable_fd, file_dev_map);
}

/*
 * zicio_attach_channel_to_shared_pool
 *
 * Allocate and initialize shared pool
 */
long
zicio_attach_channel_to_shared_pool(unsigned int *fs, struct fd *fd,
		struct zicio_args *zicio_args, zicio_descriptor *desc)
{
	struct zicio_shared_pool *zicio_shared_pool;
	struct zicio_shared_pool_local *zicio_shared_pool_local;
	struct zicio_attached_channel *zicio_channel;

	BUG_ON(desc->zicio_shared_pool_desc);

	desc->zicio_shared_pool_desc = kmalloc(ZICIO_SHARED_POOL_DESC_SIZE,
							GFP_KERNEL|__GFP_ZERO);

	if (copy_from_user(&desc->zicio_shared_pool_desc->shared_pool_key,
				zicio_args->zicio_shared_pool_key, sizeof(zicio_shared_pool_key_t))) {
		return -EINVAL;
	}

	zicio_shared_pool = zicio_get_shared_pool_from_id(
					desc->zicio_shared_pool_desc->shared_pool_key);

	if (!zicio_shared_pool) {
		return -EINVAL;
	}

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	desc->channel_index = atomic_inc_return(&zicio_shared_pool->pin);
	printk(KERN_WARNING "[ZICIO] channel_index: %d, cpu: %d\n", desc->channel_index, desc->cpu_id);
#else /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	atomic_inc(&zicio_shared_pool->pin);
#endif /* !CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	desc->zicio_shared_pool_desc->zicio_shared_pool = zicio_shared_pool;
	desc->zicio_shared_pool_desc->zicio_shared_pool_local =
		zicio_create_shared_pool_local(desc,
				desc->zicio_shared_pool_desc->zicio_shared_pool);

	zicio_shared_pool_local = desc->zicio_shared_pool_desc->zicio_shared_pool_local;
	BUG_ON(zicio_shared_pool_local == NULL);
	zicio_channel = zicio_shared_pool_local->zicio_channel;
	BUG_ON(zicio_channel == NULL);
	atomic_set(&zicio_channel->derailed, false);

	return 0;
}

/*
 * zicio close shared pool
 *
 * close global shared pool
 */
long
zicio_close_shared_pool(zicio_shared_pool_key_t zicio_shared_pool_key)
{
	zicio_shared_pool *zicio_shared_pool =
				zicio_pick_shared_pool(zicio_shared_pool_key);

	if (IS_ERR(zicio_shared_pool)) {
		printk(KERN_WARNING "[ZicIO Message] Cannot shared pool\n");
		return -EINVAL;
	}

	if (!zicio_shared_pool) {
		printk(KERN_WARNING "[ZicIO Message] Cannot shared pool\n");
		return -EINVAL;
	}

	zicio_delete_shared_pool(zicio_shared_pool);
	
	return 0;
}

/*
 * zicio_delete_all_shared_pool
 *
 * Delete all shared pool in system
 */
long
zicio_delete_all_shared_pool(void)
{
	return zicio_iterate_all_zicio_struct(&zicio_shared_pools,
				zicio_delete_shared_pool, true);
}

long
zicio_detach_shared_pool(zicio_descriptor *desc,
			zicio_global_shared_pool_desc *shared_pool_desc)
{
	zicio_shared_pool_key_t zicio_shared_pool_key = shared_pool_desc->shared_pool_key;
	zicio_shared_pool_local *zicio_shared_pool_local =
				shared_pool_desc->zicio_shared_pool_local;
	zicio_shared_pool *zicio_shared_pool =
				zicio_unref_shared_pool(zicio_shared_pool_key);

#ifdef CONFIG_ZICIO_STAT
	zicio_update_shared_stat_board(desc);
#endif /* CONFIG_ZICIO_STAT */

	if (IS_ERR(zicio_shared_pool)) {
		printk(KERN_WARNING "[ZicIO Message] Cannot shared pool\n");
		return -EINVAL;
	}

	if (!zicio_shared_pool) {
		printk(KERN_WARNING "[ZicIO Message] Cannot shared pool\n");
		return 0;
	}

	zicio_delete_shared_pool_local(desc, zicio_shared_pool,
				zicio_shared_pool_local);

	kfree(shared_pool_desc);

	return 0;
}

/*
 * Initialize zicio global shared pool manager
 */
void __init zicio_init_shared_pool_mgr(void)
{
	zicio_init_id_allocator(&zicio_shared_pools);

	zicio_init_shared_callback_list();

	zicio_init_nsecs_from_jiffy();
}

/*
 * zicio_dump_all_shared_pool
 *
 * Dump all shared pool in system
 */
void
zicio_dump_all_shared_pool(void)
{
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
	zicio_iterate_all_zicio_struct(&zicio_shared_pools,
		zicio_dump_shared_pool, false);
#else
	printk(KERN_WARNING "Dump code is not included in release mode.\n");
#endif /* CONFIG_ZICIO_DEBUG */
}
