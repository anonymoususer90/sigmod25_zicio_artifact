#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/zicio_notify.h>
#include <linux/bits.h>
#include <linux/nospec.h>
#include <linux/stat.h>
#include <uapi/linux/zicio.h>
#include <linux/blkdev.h>
#include <linux/printk.h>

#include "zicio_atomic.h"
#include "zicio_cmd.h"
#include "zicio_desc.h"
#include "zicio_device.h"
#include "zicio_extent.h"
#include "zicio_files.h"
#include "zicio_shared_pool.h"
#include "zicio_shared_pool_mgr.h"
#include "zicio_req_submit.h"
#include "zicio_ghost.h"
#include "zicio_req_timer.h"
#include "zicio_firehose_ctrl.h"
#include "zicio_md_flow_ctrl.h"

#include "zicio_nvme_cmd_timer_wheel.h"
#include "zicio_flow_ctrl.h"

static atomic_t zicio_cpu_idx_counter;

/*
 * zicio_task_exit
 *
 * Clean up zicio resources, when the process ends up
 */
void
zicio_task_exit(struct task_struct *exit_task)
{
	zicio_id_list *id_list = &exit_task->zicio_ids;
	zicio_id_node *ids;
	unsigned id;

	/*
	 * Pop channel ID from the list until the channels is empty.
	 */
	while(!zicio_id_list_empty(id_list)) {
		ids = zicio_id_list_pop(id_list);
		id = ids->id;

		zicio_free_idnode(ids);
		/*
		 * Close the channel using the ID.
		 */
		zicio_close_id(id, true);
	}
}
EXPORT_SYMBOL(zicio_task_exit);

/*
 * zicio_has_files
 *
 * Checking if this channel has files to ingest
 */
static inline bool
zicio_has_files(struct zicio_args *zicio_args)
{
	/* Checking this channel refers to files */
	if ((zicio_args->nr_local_fd && zicio_args->local_fds) ||
		(zicio_args->nr_shareable_fd && zicio_args->shareable_fds)) {
		return true;
	}
	return false;
}

static unsigned int*
zicio_get_nr_batches(int nr_fd, unsigned int __user *user_nr_batches) {
	unsigned int *nr_batches;

	nr_batches =
		(unsigned int*)kmalloc(sizeof(unsigned int) * nr_fd, GFP_KERNEL);

	if (unlikely(!nr_batches))
		return NULL;

	if (copy_from_user(nr_batches, user_nr_batches,
					   sizeof(unsigned int) * nr_fd)) {
		kfree(nr_batches);
		return NULL;
	}

	return nr_batches;
}

static unsigned int**
zicio_get_batches_array(int nr_fd, unsigned int __user **user_batches_array,
						unsigned int *nr_batches, int **vma_flags) {
	unsigned int **batches_array;
	unsigned int *batches;
	unsigned int nbytes, i;

	*vma_flags = (int*)kmalloc(sizeof(int) * nr_fd, GFP_KERNEL | __GFP_ZERO);

	if (unlikely(!(*vma_flags)))
		return NULL;

	batches_array =
		(unsigned int**)kmalloc(sizeof(unsigned int*) * nr_fd, GFP_KERNEL);

	if (unlikely(!batches_array)) {
		kfree(*vma_flags);
		return NULL;
	}

	if (copy_from_user(batches_array, user_batches_array,
					   sizeof(unsigned int*) * nr_fd)) {
		kfree(*vma_flags);
		kfree(batches_array);
		return NULL;
	}

	for (i = 0; i < nr_fd; i++) {
		nbytes = nr_batches[i] * 2 * sizeof(unsigned int);

		if (nbytes <= (1 << 22)) // 4 MiB
			batches = (unsigned int*)kmalloc(nbytes, GFP_KERNEL);
		else {
			batches = (unsigned int*)vmalloc(nbytes);
			(*vma_flags)[i] = 1;
		}

		if (unlikely(!batches))
			goto failed;

		if (copy_from_user(batches, batches_array[i], nbytes)) {
			kfree(batches);
			goto failed;
		}

		batches_array[i] = batches;
	}

	return batches_array;

failed:
	for (i--; i > 0; i--) {
		if (!(*vma_flags)[i])
			kfree(batches_array[i]);
		else
			vfree(batches_array[i]);
	}
	kfree(batches_array);
	kfree(*vma_flags);

	return NULL;
}

/*
 * Initialize shared pool resources
 *
 * Allocate and initialize shared pool
 *
 * @fds - file descriptor array
 * @fs - file struct array matched with fds
 * @nr_shareable_fd - number of file descriptor
 *
 * Return : The ID of shared pool key
 */
static long
zicio_initialize_shared_pool_resources(unsigned int *fds, struct fd* fs,
			unsigned int nr_shareable_fd)
{
	struct device **devices = NULL;
	int *file_dev_map = NULL;
	long ret = 0;

	/* Get the distinct device set */
	devices = zicio_get_distinct_file_dev_set(&file_dev_map, fs,
			nr_shareable_fd);

	if (unlikely(!devices)) {
		return -ENOMEM;
	}

	/* Allocate and initialize shared pool for channel */
	if ((ret = zicio_allocate_and_initialize_shared_pool(devices, fds, fs,
				nr_shareable_fd, file_dev_map)) < 0) {
		goto l_zicio_initialize_shared_resource_out;
	}

	zicio_free_if_not_null(file_dev_map);
	return ret;

l_zicio_initialize_shared_resource_out:
	zicio_free_if_not_null(devices);
	return ret;
}

/*
 * zicio_initialize_cpu_affinity
 *
 * Get and set cpu id and affinity for channel and process
 */
static void
zicio_initialize_cpu_affinity(zicio_descriptor *desc)
{
	int cpu_group_id = 0;
	int cpu_idx_in_group = 0;
	int nr_cpu_per_hw_queue = 4;
	int nr_cpu_group = nr_cpu_ids / nr_cpu_per_hw_queue;
	int zicio_counter;

	/*
	 * Initialize the cpu id the first time the current process opens the
	 * zicio channel.
	 */
	if (zicio_id_list_empty(&current->zicio_ids)) {
		BUG_ON(current->zicio_cpu_id != -1);
		zicio_counter = atomic_fetch_add(1, &zicio_cpu_idx_counter);
		cpu_group_id = zicio_counter % nr_cpu_group;
		cpu_idx_in_group = (zicio_counter / nr_cpu_group) % nr_cpu_per_hw_queue;
		current->zicio_cpu_id =
			cpu_group_id * nr_cpu_per_hw_queue + cpu_idx_in_group;
	}

	BUG_ON(current->zicio_cpu_id == -1);
	BUG_ON(current->zicio_cpu_id >= nr_cpu_ids);
	desc->cpu_id = current->zicio_cpu_id;
}


/*
 * zicio_cleanup_cpu_affinity
 *
 * Clean up cpu id and affinity for channel and process
 */
static void
zicio_cleanup_cpu_affinity(void)
{
	/* Enable migration when all channels are close */
	if (zicio_id_list_empty(&current->zicio_ids)) {
		BUG_ON(current->zicio_cpu_id == -1);
		current->zicio_cpu_id = -1;
		sched_setaffinity(0, cpu_possible_mask);
	}
}

/*
 * zicio_initialize_shared_channel_resources
 *
 * @shareable_fs - struct fd array to register channel
 * @shareable_fds -  file descriptor array matched with shareable_fs
 * @zicio_args - zicio arguments
 *
 * Return : Return the allocated zicio descriptor point if success,
 * otherwise return NULL
 *
 * Initializing resources for a channel attached to shared pool
 */
zicio_descriptor *
zicio_initialize_shared_channel_resources(struct fd* shareable_fs,
			unsigned int *shareable_fds, struct zicio_args *zicio_args)
{
	zicio_descriptor *desc = NULL;
	struct device **devices = NULL;
	zicio_device **zicio_devices = NULL;
	int *file_dev_map = NULL;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk("[Kernel Message] zicio initialize resources\n");
#endif

	/*
	 * Get the distinct device set
	 */
	devices = zicio_get_distinct_file_dev_set(&file_dev_map, shareable_fs,
			zicio_args->nr_shareable_fd);

	if (unlikely(!devices)) {
		return ERR_PTR(-ENOMEM);
	}

	/*
	 * Get the zicio device matched with the device array.
	 */
	zicio_devices = zicio_initialize_device_to_desc(devices,
			zicio_args->nr_shareable_fd);

	if (unlikely(IS_ERR_OR_NULL(zicio_devices))) {
		printk("[ZICIO] error in device allocation\n");
		goto l_zicio_allocate_and_initialize_err;
	}

	/* 
	 * Get global zicio descriptor
	 */
	desc = zicio_allocate_and_initialize_mem(devices, zicio_devices, zicio_args);

	if (unlikely(IS_ERR_OR_NULL(desc))) {
		printk("[Kernel Message] error in buffer allocation\n");
		goto l_zicio_allocate_and_initialize_err;
	}

	/*
	 * Initialize channel's cpu ID
	 */
	zicio_initialize_cpu_affinity(desc);

	/*
	 * Initialize additional files for channel
	 */
	if (zicio_initialize_fds_to_desc(desc, shareable_fs, shareable_fds,
			file_dev_map, zicio_args->nr_shareable_fd) < 0) {
		printk("[Kernel Message] error in file adder\n");
		goto l_zicio_allocate_and_initialize_err;
	}

	/*
	 * Get file device map and device structure
	 */
	if (zicio_attach_channel_to_shared_pool(shareable_fds, shareable_fs,
				zicio_args, desc)) {
		goto l_zicio_allocate_and_initialize_err;
	}

	/*
	 * mmap buffers for zicio channel
	 */
	if (IS_ERR_VALUE(zicio_mmap_buffers(desc, zicio_args->zicio_flag,
			zicio_args->user_base_address))) {
		printk(KERN_WARNING "Error in mmap buffers\n");
		goto l_zicio_allocate_and_initialize_err;
	}

#ifdef CONFIG_ZICIO_STAT
	zicio_set_channel_start_to_stat_shared(desc->stat_board);
#endif /* (CONFIG_ZICIO_STAT) */
	zicio_free_if_not_null(file_dev_map);
	zicio_free_if_not_null(zicio_devices);

	return desc;
l_zicio_allocate_and_initialize_err:
	zicio_free_if_not_null(zicio_devices);
	zicio_free_if_not_null(devices);
	zicio_free_buffers(desc);

	return NULL;
}

/*
 * zicio_initialize_resources
 *
 * @fs - struct fd array to register channel
 * @nr_fd - the number of file descriptor
 * @stflg - flag
 * @fds_ - array of file descript id
 *
 * Return : Return the allocated zicio descriptor point if success,
 * otherwise return NULL
 *
 * Initializing resources for firehose.
 */
zicio_descriptor *
zicio_initialize_resources(struct fd* local_fs, unsigned int *local_fds,
			struct zicio_args *zicio_args)
{
	zicio_descriptor *desc = NULL;
	zicio_notify_descriptor *zicio_notify_desc = NULL;
	struct device **devices = NULL;
	zicio_device **zicio_devices = NULL;
	int *file_dev_map = NULL;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk("[Kernel Message] zicio initialize resources\n");
#endif
	/*
	 * Get file device map and device structure
	 */
	devices = zicio_get_distinct_file_dev_set(&file_dev_map, local_fs,
			zicio_args->nr_local_fd);

	/*
	 * Get the zicio device matched with the device array.
	 */
	zicio_devices = zicio_initialize_device_to_desc(devices,
			zicio_args->nr_local_fd);

	/*
	 * Error handle for device array allocation
	 */
	if (unlikely(IS_ERR(zicio_devices))) {
		printk("[ZICIO] error in zicio device array allocation\n");
		goto l_zicio_allocate_and_initialize_err_before_alloc_desc;
	}

	/*
	 * Get global zicio descriptor
	 */
	desc = zicio_allocate_and_initialize_mem(devices, zicio_devices, zicio_args);

	if (unlikely(IS_ERR_OR_NULL(desc))) {
		printk("[Kernel Message] error in buffer allocation\n");
		goto l_zicio_allocate_and_initialize_err_before_alloc_desc;
	}

	zicio_notify_desc = (zicio_notify_descriptor*)desc;
	if (zicio_args->batches_array != NULL) {
		unsigned int **batches_array;
		unsigned int *nr_batches;

		/* start to copy */
		nr_batches =
			zicio_get_nr_batches(zicio_args->nr_local_fd, zicio_args->nr_batches);

		if (unlikely(!nr_batches)) {
			printk(KERN_WARNING"[Kernel Message] Cannot allocate nr_batches\n");
			goto l_zicio_allocate_and_initialize_err;
		}

		batches_array =
			zicio_get_batches_array(zicio_args->nr_local_fd, zicio_args->batches_array,
									nr_batches, &(zicio_notify_desc->vma_flags));

		if (unlikely(!batches_array)) {
			kfree(nr_batches);
			printk(KERN_WARNING"[Kernel Message] Cannot get batches_array\n");
			goto l_zicio_allocate_and_initialize_err;
		}

		zicio_notify_desc->nr_fd_of_batches = zicio_args->nr_local_fd;
		zicio_notify_desc->nr_batches = nr_batches;
		zicio_notify_desc->batches_array = batches_array;

		/* TODO we assumed only one device */
		zicio_notify_desc->bd_start_sector
			= local_fs[0].file->f_mapping->host->i_sb->s_bdev->bd_start_sect;

		zicio_channel_init(zicio_notify_desc);
	} else
		zicio_notify_desc->nr_fd_of_batches = 0;

	/*
	 * Initialize channel's cpu ID
	 */
	zicio_initialize_cpu_affinity(desc);

	/*
	 * Initialize additional files for channel
	 */
	if (zicio_initialize_fds_to_desc(desc, local_fs, local_fds,
			file_dev_map, zicio_args->nr_local_fd) < 0) {
		printk("[Kernel Message] error in file adder\n");
		goto l_zicio_allocate_and_initialize_err;
	}

	/*
	 * Initialize extent tree buffer
	 */
	if (zicio_initialize_extent_and_metadata(desc, local_fs,
			zicio_args->nr_local_fd) < 0) {
		printk("[Kernel Message] error in initializing file metadata\n");
		goto l_zicio_allocate_and_initialize_err;
	}

	zicio_free_if_not_null(zicio_devices);

	if (zicio_args->nr_local_fd) {
		zicio_free_if_not_null(file_dev_map);
	}

	/*
	 * Mapping data buffer to user area and get its id
	 */
	if (IS_ERR_VALUE(zicio_mmap_buffers(desc, zicio_args->zicio_flag,
			zicio_args->user_base_address))) {
		printk(KERN_WARNING "Error in map buffers\n");
		goto l_zicio_allocate_and_initialize_err;
	}

	return desc;
l_zicio_allocate_and_initialize_err:
	zicio_free_buffers(desc);
l_zicio_allocate_and_initialize_err_before_alloc_desc:
	zicio_free_if_not_null(zicio_devices);
	zicio_free_if_not_null(devices);

	/* Clean up dev maps */
	if (zicio_args->nr_local_fd) {
		zicio_free_if_not_null(file_dev_map);
	}

	return NULL;
}

/*
 * sys_zicio_k_open_shared
 *
 * call zicio functions for initializing resources
 *
 * struct zicio_args *zicio_args: pointer of zicio args structure
 * 
 * return : if success, return the id of firehose data buffer
 *			otherwise, return the kernel error code
 */
long
sys_zicio_k_open_shared(struct zicio_args *zicio_args,
	unsigned long *user_vm_switch_board, unsigned long *user_vm_stat_board)
{
	long ret = -EBADF; /* return value */
	unsigned int *shareable_fds = NULL; /* shareable file descriptor array */
	struct fd* shareable_fs = NULL; /* shareable file struct array */
	zicio_descriptor *desc; /* zicio descriptor */

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk("[Kernel Message] zicio kernel open start\n");
#endif

	/* Copy fd array from user */
	get_zicio_file_array(zicio_args->nr_shareable_fd, shareable_fds,
			zicio_args->shareable_fds, zicio_args->nr_shareable_fd,
			zicio_copy_fds_from_user, ret, l_sys_zicio_k_open_out);

	get_zicio_file_array(shareable_fds, shareable_fs, shareable_fds,
			zicio_args->nr_shareable_fd, zicio_get_fdstruct, ret,
			l_sys_zicio_k_open_out);
	
	/* Get an unused id of zicio descriptor */
	ret = zicio_get_unused_desc_id();

	if (ret >= 0) {
		/*
		 * After getting the id of zicios deceriptor,
		 * allocate and initilize resources for zicio. And then, installing
		 * descriptor and its ID to zicio descriptor table.
		 */
		desc = zicio_initialize_shared_channel_resources(shareable_fs,
				shareable_fds, zicio_args);

		if (desc) {
			/* Install zicio descriptor to table */
			*user_vm_switch_board = desc->user_map.switch_board;
#ifdef CONFIG_ZICIO_STAT
			*user_vm_stat_board = desc->user_map.stat_board;
#else /* !(CONFIG_ZICIO_STAT) */
			*user_vm_stat_board = 0;
#endif /* (CONFIG_ZICIO_STAT) */
			zicio_install_desc(ret, desc);
		} else {
			zicio_put_unused_desc_id(ret);
			ret = -ENOMEM;
		}
	}

l_sys_zicio_k_open_out:
	zicio_free_if_not_null(shareable_fds);
	zicio_free_if_not_null(shareable_fs);

	return ret;
}

/*
 * sys_zicio_k_open
 *
 * call zicio functions for initializing resources
 *
 * struct zicio_args *zicio_args: pointer of zicio args structure
 *
 * return : if success, return the id of firehose data buffer
 *			otherwise, return the kernel error code
 */
long
sys_zicio_k_open(struct zicio_args *zicio_args,
	unsigned long *user_vm_switch_board, unsigned long *user_vm_stat_board)
{
	long ret = -EBADF; /* return value */
	unsigned int *local_fds = NULL; /* local file descriptor array */
	struct fd* local_fs = NULL; /* local file struct array */
	zicio_descriptor *desc; /* zicio descriptor */

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk("[Kernel Message] zicio kernel open start\n");
#endif

	/* Copy fd array from user */
	get_zicio_file_array(zicio_args->nr_local_fd, local_fds, zicio_args->local_fds,
			zicio_args->nr_local_fd, zicio_copy_fds_from_user, ret,
			l_sys_zicio_k_open_out);

	get_zicio_file_array(local_fds, local_fs, local_fds, zicio_args->nr_local_fd,
			zicio_get_fdstruct, ret, l_sys_zicio_k_open_out);

	/* Get an unused id of zicio descriptor */
	ret = zicio_get_unused_desc_id();

	if (ret >= 0) {

		/*
		 * After getting the id of zicios descriptor, 
		 * allocate and initialize resources for zicio. And then, installing
		 * descriptor and its ID to zicio descriptor table.
		 */
		desc = zicio_initialize_resources(local_fs, local_fds, zicio_args);

		if (desc) {
			/* Install zicio descriptor to table */
			*user_vm_switch_board = desc->user_map.switch_board;
#ifdef CONFIG_ZICIO_STAT
			*user_vm_stat_board = desc->user_map.stat_board;
#else /* !(CONFIG_ZICIO_STAT) */
			*user_vm_stat_board = 0;
#endif /* (CONFIG_ZICIO_STAT) */
			zicio_install_desc(ret, desc);
		} else {
			zicio_put_unused_desc_id(ret);
			ret = -ENOMEM;
		}
	}

l_sys_zicio_k_open_out:
	zicio_free_if_not_null(local_fds);
	zicio_free_if_not_null(local_fs);

	return ret;
}

/*
 * sys_zicio_u_open
 *
 * system call interface for creating zicio resources.
 *
 * param
 * struct *zicio_args : arguments of zicio
 *
 * return : the id of fire hose data buffer 
 */
SYSCALL_DEFINE1(zicio_u_open, struct zicio_args __user*, zicio_user_args)
{
	struct zicio_args zicio_args;
	unsigned long user_vm_switch_board;
	unsigned long user_vm_stat_board;

	long id;
	zicio_id_node *id_node;

	struct zicio_notify_descriptor *zicio_notify_desc;
	zicio_descriptor *desc;
	int dev_idx, inner_dev_idx, inner_dev_num, global_device_idx;
	zicio_dev_map_node *zicio_dev_map_node;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	int cpu = get_cpu();
	put_cpu();

	printk("[Kernel Message] zicio open start, cpu: %d\n", cpu);
#endif

	if (copy_from_user(&zicio_args, zicio_user_args, sizeof(struct zicio_args))) {
		return -1;
	}

	if (zicio_is_shared_channel(&zicio_args)) {
		/* Call kernel zicio open function for shared mode */
		id = sys_zicio_k_open_shared(&zicio_args,
				&user_vm_switch_board, &user_vm_stat_board);
	} else {
		/* Call kernel zicio open function */
		id = sys_zicio_k_open(&zicio_args, &user_vm_switch_board,
				&user_vm_stat_board);
	}

	if (id < 0) {
		return id;
	}

	id_node = zicio_get_idnode(id);
	zicio_id_list_add(&current->zicio_ids, id_node);

	if (copy_to_user(zicio_args.switch_board_addr, &user_vm_switch_board,
				sizeof(unsigned long))) {
		return -1;
	}

	if (copy_to_user(zicio_args.stat_board_addr, &user_vm_stat_board,
				sizeof(unsigned long))) {
		return -1;
	}

	desc = zicio_get_desc(id);

	/* XXX zicio */
	zicio_notify_desc = (zicio_notify_descriptor *)desc;

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	desc->derail_try_count = 0;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	/*
	 * Before triggering the first I/O, two perparations are required.
	 *
	 * 1. ZicIO stores NVMe commands to be submitted next in a per-core data
	 *    structure. Therefore, it does not allow cpu migration. Set the cpu
	 *    using the setaffinity function.
	 *
	 * 2. ZicIO's firehose controller has data structures to track I/O
	 *    handling results. In order to handle I/O, these data structures must
	 *    be initialized first.
	 */
	for (dev_idx = 0 ; dev_idx < desc->dev_maps.nr_dev ; dev_idx++) {
		if (desc->dev_maps.dev_node_array[dev_idx].zicio_devs->device_type ==
				ZICIO_MD) {
			zicio_dev_map_node = desc->dev_maps.dev_node_array + dev_idx;
			for (inner_dev_idx = 0 ; inner_dev_idx < zicio_dev_map_node->nr_maps ;
					inner_dev_idx++) {
				inner_dev_num = inner_dev_idx +
					zicio_dev_map_node->start_point_inner_dev_map;
				global_device_idx = desc->dev_maps.zicio_inner_dev_maps[
					inner_dev_num].zicio_inner_dev->zicio_global_device_idx;

				if (zicio_descriptor_flow_in(desc, global_device_idx)) {
					printk("[Kernel Message] zicio open set cpu err\n");
					return -3;
				}
			} 
		} else {
			/* XXX zicio */
			if (zicio_notify_desc->nr_fd_of_batches != 0) {
				if (zicio_register_channel_into_flow_ctrl(zicio_notify_desc)) {
					printk("[Kernel Message] zicio open set cpu err\n");
					return -3;
				}
			} else {
				if (zicio_descriptor_flow_in(desc,
							zicio_get_zicio_global_device_idx(desc, dev_idx))) {
					printk("[Kernel Message] zicio open set cpu err\n");
					return -3;
				}
			}
		}
	}

	zicio_init_firehose_ctrl(desc);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	for (dev_idx = 0 ; dev_idx < desc->dev_maps.nr_dev ; dev_idx++) {
		zicio_dump_md_flow_ctrl(
			cpu, zicio_get_zicio_global_device_idx(desc, dev_idx));
	}
#endif

	/*
	 * Initial trigger function for zicio read
	 */
	if (zicio_has_files(&zicio_args)) {
		if ((zicio_init_read_trigger(id)) < 0) {
			/* We failed to trigger the first I/O, shutdown controllers */
			zicio_close_firehose_ctrl(zicio_get_desc(id), false);
			for (dev_idx = 0 ; dev_idx < desc->dev_maps.nr_dev ; dev_idx++) {
				if (desc->dev_maps.dev_node_array[dev_idx].zicio_devs->device_type
						== ZICIO_MD) {
					zicio_dev_map_node = desc->dev_maps.dev_node_array + dev_idx;
					for (inner_dev_idx = 0 ;
						 inner_dev_idx < zicio_dev_map_node->nr_maps ;
						 inner_dev_idx++) {
						inner_dev_num = inner_dev_idx +
							zicio_dev_map_node->start_point_inner_dev_map;
						global_device_idx = desc->dev_maps.zicio_inner_dev_maps[
							inner_dev_num].zicio_inner_dev->zicio_global_device_idx;
						zicio_descriptor_flow_out(desc, global_device_idx,
							desc->dev_maps.zicio_inner_dev_maps[
								inner_dev_num].raw_dev_idx_in_channel);
					}
				} else {
					/* XXX zicio */
					if (zicio_notify_desc->nr_fd_of_batches != 0) {
						zicio_remove_channel_from_flow_ctrl(zicio_notify_desc);
					} else {
						zicio_descriptor_flow_out(desc,
								zicio_get_zicio_global_device_idx(desc, dev_idx),
								dev_idx);
					}
				}
			}
			zicio_cleanup_cpu_affinity();
			return -4;
		}
	}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
	printk(KERN_WARNING "[ZICIO] zicio_u_open()\n");
#endif

	return id;
}

int zicio_descriptor_close(zicio_descriptor *sd, bool from_doexit)
{
	zicio_dev_map_node *zicio_dev_map_node;
	int dev_idx, inner_dev_idx, inner_dev_num, global_device_idx, i;
	zicio_notify_descriptor *zicio_notify_desc = (zicio_notify_descriptor*)sd;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
	int cpu = sd->cpu_id;

	printk("[cpu %d] zicio_descriptor_close()\n", cpu);
#endif
	if (sd->zicio_shared_pool_desc) {
		zicio_remove_lag_user_request(sd);
	}

	if (zicio_notify_desc->nr_fd_of_batches != 0) {
		/* after consumption */
		kfree(zicio_notify_desc->nr_batches);
		for (i = 0; i < zicio_notify_desc->nr_fd_of_batches; i++) {
			if (!(zicio_notify_desc->vma_flags[i]))
				kfree(zicio_notify_desc->batches_array[i]);
			else
				vfree(zicio_notify_desc->batches_array[i]);
		}
		kfree(zicio_notify_desc->batches_array);
		kfree(zicio_notify_desc->vma_flags);
		for (i = 0; i < (zicio_notify_desc->nr_nvme_cmd_infos - 1) /
						ZICIO_NVME_CMD_INFO_PER_CHUNK + 1; i++)
			kfree(zicio_notify_desc->nvme_cmd_infos[i]);
		for (i = 0; i < (zicio_notify_desc->nr_nvme_cmd_info_start_offsets - 1) /
						ZICIO_NVME_CMD_INFO_START_OFFSET_PER_CHUNK + 1; i++)
			kfree(zicio_notify_desc->nvme_cmd_info_start_offsets[i]);

		zicio_release_timer_resources(zicio_notify_desc);
		zicio_remove_channel_from_flow_ctrl(zicio_notify_desc);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
		printk("[cpu %d] zicio_desc close\n", cpu);
#endif
	}

	for (dev_idx = 0 ; dev_idx < sd->dev_maps.nr_dev ; dev_idx++) {
		if (sd->dev_maps.dev_node_array[dev_idx].zicio_devs->device_type ==
			ZICIO_MD) {
			zicio_dev_map_node = sd->dev_maps.dev_node_array + dev_idx;
			for (inner_dev_idx = 0 ; inner_dev_idx < zicio_dev_map_node->nr_maps ;
					inner_dev_idx++) {
				inner_dev_num = inner_dev_idx +
					zicio_dev_map_node->start_point_inner_dev_map;
				global_device_idx = sd->dev_maps.zicio_inner_dev_maps[
					inner_dev_num].zicio_inner_dev->zicio_global_device_idx;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
				zicio_dump_md_flow_ctrl(cpu, global_device_idx);
#endif
				zicio_descriptor_flow_out(sd, global_device_idx,
					sd->dev_maps.zicio_inner_dev_maps[
						inner_dev_num].raw_dev_idx_in_channel);
			}
		} else {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
			zicio_dump_md_flow_ctrl(
				cpu, zicio_get_zicio_global_device_idx(sd, dev_idx));
#endif
			if (zicio_notify_desc->nr_fd_of_batches == 0)
				zicio_descriptor_flow_out(sd,
						zicio_get_zicio_global_device_idx(sd, dev_idx), dev_idx);
		}
	}
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	PRINT_ZICIO_DEBUG(sd->cpu_id, current->pid, __FILE__, __LINE__,
			__FUNCTION__);
#endif

	zicio_close_firehose_ctrl(sd, from_doexit);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	PRINT_ZICIO_DEBUG(sd->cpu_id, current->pid, __FILE__, __LINE__,
			__FUNCTION__);
#endif

	if (sd->zicio_shared_pool_desc) {
		zicio_detach_shared_pool(sd, sd->zicio_shared_pool_desc);
	}
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	PRINT_ZICIO_DEBUG(sd->cpu_id, current->pid, __FILE__, __LINE__,
			__FUNCTION__);
#endif
	zicio_ghost_close(sd);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	PRINT_ZICIO_DEBUG(sd->cpu_id, current->pid, __FILE__, __LINE__,
			__FUNCTION__);
#endif
	zicio_munmap_buffers(sd);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	PRINT_ZICIO_DEBUG(sd->cpu_id, current->pid, __FILE__, __LINE__,
			__FUNCTION__);
#endif
	zicio_free_buffers(sd);
	zicio_cleanup_cpu_affinity();
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	PRINT_ZICIO_DEBUG(sd->cpu_id, current->pid, __FILE__, __LINE__,
			__FUNCTION__);
#endif

	return 0;
}

/*
 * zicio_close_id
 *
 * close zicio channel matched to id
 *
 * Params
 * @id - id of zicio descriptor
 * @from_doexit - Whether this function call is from doexit or not
 */
long zicio_close_id(unsigned int id, bool from_doexit)
{
	zicio_descriptor *sd;
	zicio_id_node *id_node;

	/* Clear descriptor table and bitmap and get zicio descriptor */
	sd = zicio_pick_desc(id);

	if (IS_ERR_OR_NULL(sd)) {
		printk(KERN_WARNING "[Kernel Message] Cannot find descriptor\n");
		return -EINVAL;
	}

	/* If this function wasn't called through doexit, then remove id node */
	if (!from_doexit) {
		id_node = zicio_id_list_pick(&current->zicio_ids, id);
		zicio_free_idnode(id_node);
	}


	/* Close zicio descriptor channel */
	return zicio_descriptor_close(sd, from_doexit);
}

#ifdef CONFIG_ZICIO_STAT
static
void zicio_dump_stat_board(unsigned int id)
{
	zicio_descriptor *sd;
	zicio_stat_board *stat_board;

	/* Get zicio descriptor */
	sd = zicio_get_desc(id);
	stat_board = sd->stat_board;

	printk(KERN_WARNING "[ZICIO] cpu: %d, softirq: %ld, io_handler: %ld, "
		"idle: %ld, io_completion_time: %lld\n",
		sd->cpu_id, stat_board->soft_irq_cnt,
		stat_board->io_interrupt_cnt, stat_board->cpu_idle_loop_cnt,
		stat_board->io_completion_time);
	if (!stat_board->is_shared) {
		return;
	}

	printk(KERN_WARNING "[ZICIO] shared channel stat\n");
	printk(KERN_WARNING "[ZICIO] cpu: %d channel[%d] "
		"Read chunks on track: %lu, Read chunks derailed: %lu, "
		"Contributed pages: %lu, Shared page: %lu , "
		"Premapped pages: %lu, Forcefully unmapped page: %lu, "
		"Latency on track: %lld, Latency derailed: %lld\n", sd->cpu_id, id,
			stat_board->num_mapped_chunk_on_track,
			stat_board->num_mapped_chunk_derailed,
			stat_board->num_pages_contributed,
			stat_board->num_pages_shared,
			stat_board->num_premapped_pages,
			stat_board->num_forcefully_unmapped_pages,
			stat_board->latency_on_track,
			stat_board->latency_derailed);
	printk(KERN_WARNING "[ZICIO] cpu: %d channel[%d]: "
		"softirq trigger count: %lu, softirq trigger no local page: %lu, "
		"softirq trigger no IO:%lu\n", sd->cpu_id, id,
			stat_board->soft_irq_trigger_cnt_shared,
			stat_board->soft_irq_trigger_no_local_page,
			stat_board->soft_irq_trigger_no_IO);
	printk(KERN_WARNING "[ZICIO] cpu: %d channel[%d] stats on track\n",
		sd->cpu_id, id);
	printk(KERN_WARNING "[ZICIO] cpu: %d channel[%d]: "
		"io cnt on track: %lu, io_handler on track: %lu, idle on track: %lu, "
		"io softirq on track:%lu, io completion time :%lld\n", sd->cpu_id, id,
			stat_board->io_on_track,
			stat_board->io_interrupt_cnt_on_track,
			stat_board->cpu_idle_loop_cnt_on_track,
			stat_board->soft_irq_cnt_on_track,
			stat_board->io_completion_time_on_track);
	printk(KERN_WARNING "[ZICIO] cpu: %d channel[%d]: "
		"Reactivate softirq trigger on track: %lu, "
		"Reactivate softirq trigger no local page on track: %lu, "
		"Reactivate softirq trigger no IO on track: %lu\n", sd->cpu_id, id,
			stat_board->soft_irq_trigger_on_track,
			stat_board->soft_irq_trigger_on_track_no_local_page,
			stat_board->soft_irq_trigger_on_track_no_IO);

	printk(KERN_WARNING "[ZICIO] cpu: %d channel[%d] stats derailed\n",
		sd->cpu_id, id);
	printk(KERN_WARNING "[ZICIO] cpu: %d channel[%d]: "
		"io cnt derailed: %lu, io_handler derailed: %lu, idle derailed: %lu, "
		"io softirq derailed:%lu, io completion time :%lld\n", sd->cpu_id, id,
			stat_board->io_derailed,
			stat_board->io_interrupt_cnt_derailed,
			stat_board->cpu_idle_loop_cnt_derailed,
			stat_board->soft_irq_cnt_derailed,
			stat_board->io_completion_time_derailed);
	printk(KERN_WARNING "[ZICIO] cpu: %d channel[%d]: "
		"Reactivate softirq trigger derailed: %lu, "
		"Reactivate softirq trigger no local page derailed: %lu, "
		"Reactivate softirq trigger no IO derailed: %lu\n", sd->cpu_id, id,
			stat_board->soft_irq_trigger_derailed,
			stat_board->soft_irq_trigger_derailed_no_local_page,
			stat_board->soft_irq_trigger_derailed_no_IO);
}
#endif /* (CONFIG_ZICIO_STAT) */

/*
 * user system call for close zicio
 */
SYSCALL_DEFINE1(zicio_u_close, unsigned int, id)
{
	long ret;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
	printk(KERN_WARNING "[ZICIO] zicio_u_close()\n");
#endif
#ifdef CONFIG_ZICIO_STAT
	zicio_dump_stat_board(id);
#endif

	ret = zicio_close_id(id, false);

	if (ret) {
		return -1;
	}

	return 0;
}

/*
 * sys_zicio_k_destroy_shared_pool
 *
 * destroy shared pool macthed to @shared_pool_key
 *
 * Return: If successful, then return 0. Otherwise, return an negative value
 */
long
sys_zicio_k_destroy_shared_pool(zicio_shared_pool_key_t shared_pool_key)
{
	if (shared_pool_key) {
		return zicio_close_shared_pool(shared_pool_key);
	} else {
		return zicio_delete_all_shared_pool();
	}
}

/*
 * User system call to destroy shared pool
 */
SYSCALL_DEFINE1(zicio_u_destroy_shared_pool, zicio_shared_pool_key_t,
			shared_pool_key)
{
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk("[Kernel Message] zicio close start\n");
#endif

	return sys_zicio_k_destroy_shared_pool(shared_pool_key);
}

/*
 * sys_zicio_k_create_shared_pool
 *
 * Create shared pool and allocate and initialize its resources
 *
 * @user_sharable_fds: shareable file descriptor array
 * @nr_fd: the number of file descriptor
 */
long
sys_zicio_k_create_shared_pool(unsigned int __user* user_shareable_fds,
		unsigned int nr_fd)
{
	long ret = -EBADF;
	unsigned int *shareable_fds = NULL;
	struct fd* shareable_fs = NULL;

	/* Copy shareable file descriptor array from user */
	get_zicio_file_array(nr_fd, shareable_fds, user_shareable_fds, nr_fd,
			zicio_copy_fds_from_user, ret,
			l_sys_zicio_k_create_shared_pool_out);

	/* Get shareable file strcture from fd array */
	get_zicio_file_array(shareable_fds, shareable_fs, shareable_fds,
			nr_fd, zicio_get_fdstruct, ret,
			l_sys_zicio_k_create_shared_pool_out);

	/* Initialize shared pool resources */
	if ((ret = zicio_initialize_shared_pool_resources(shareable_fds,
			shareable_fs, nr_fd)) < 0) {
		goto l_sys_zicio_k_create_shared_pool_out;
	}

l_sys_zicio_k_create_shared_pool_out:
	zicio_free_if_not_null(shareable_fds);
	zicio_free_if_not_null(shareable_fs);

	return ret;
}

/*
 * User system call from shared pool
 */
SYSCALL_DEFINE2(zicio_u_create_shared_pool, unsigned int __user*, fds,
		unsigned int, nr_fd)
{
	return sys_zicio_k_create_shared_pool(fds, nr_fd);
}

/*
 * zicio_init
 *
 * Initialize zicio resources
 */
void __init zicio_init(void)
{
	zicio_init_desc_allocator();
	zicio_init_slab_caches();
	zicio_init_shared_pool_mgr();
	atomic_set(&zicio_cpu_idx_counter, 0);
}

/*
 * zicio_init_with_device_number
 *
 * Initialize zicio resources after device initialization.
 */
void __init zicio_init_with_device_number(void)
{
	int num_dev = zicio_get_num_raw_zicio_device();

	zicio_init_zombie_timers();
	zicio_init_request_timer_wheel(num_dev);
	zicio_install_zombie_timers(num_dev);
	zicio_init_md_flow_controller(num_dev);
}

/**
 * __zicio_complete_command_local - complete nvme command of local channel
 * @zicio_desc: zicio descriptor
 * @cur_cmd: currently handling nvme command
 *
 * ZicIO local channel's nvme command has two category.
 *
 * First, metadata command is a request for data such as inode needed to
 * read blocks of a device.
 *
 * Second, normal data command is a request for data to be uploaded to the
 * user-visible data buffer.
 *
 * Note that since creating new commands for the next chunk in every IRQ is
 * expensive, we set the threshold which have to be passed to create new nvme
 * commands.
 *
 * However, as an exception in metadata completeion, commands for the next chunk
 * are unconditionally created. Otherwise, it may mistakenly believe that the
 * channel is over even though there is still data left to consume.
 */
static void __zicio_complete_command_local(
	struct zicio_descriptor *zicio_desc,
	struct zicio_nvme_cmd_list *cur_cmd)
{
	int need_next;

	if (cur_cmd->is_metadata) {
		zicio_set_inode_bitmap(&zicio_desc->metadata_ctrl.inode_meter,
				cur_cmd->local_huge_page_idx);
		zicio_produce_metadata(zicio_desc);

		mb();
		zicio_require_next_chunk(zicio_desc, cur_cmd);
	} else {
		need_next = zicio_complete_firehose_command(zicio_desc, cur_cmd);
		if (need_next == ZICIO_NEXT_CHUNK_ENABLED)
			zicio_require_next_chunk(zicio_desc, cur_cmd);
	}
}

/**
 * __zicio_complete_command_shared - complete nvme command of shared channel
 * @zicio_desc: zicio descriptor
 * @cur_cmd: currently handling nvme command
 *
 * ZicIO in shared mode handles the completion of the nvme command through
 * the following process.
 *
 * Step 1. Handle request completion.
 * Step 2 & 3. Unmap pages and pre-map shareable pages from the pool.
 * Step 4. Send I/O or register timer to the linux timing wheel.
 *
 * Note that since creating new commands for the next chunk in every IRQ is
 * expensive, we set the threshold which have to be passed to create new nvme
 * commands.
 */
static void __zicio_complete_command_shared(
	struct zicio_descriptor *zicio_desc,
	struct zicio_nvme_cmd_list *cur_cmd)
{
	unsigned int distance_from_head = 0;
	int num_wait_consumption;
	bool need_next;
	bool derailed;

	/* Step 1 */
	need_next = zicio_complete_firehose_command_shared(zicio_desc, cur_cmd);

	if (need_next == ZICIO_NEXT_CHUNK_ENABLED) {
		/* Step 2 & 3 */
		num_wait_consumption = zicio_adjust_mapping_and_reclaim_pages(
				zicio_desc, &distance_from_head, false);
		/* Step 4 */
		if (num_wait_consumption &&
			!(derailed = zicio_check_channel_derailed(zicio_desc))) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "cpu[%d] desc[%p] no IO[%s:%d][%s]\n",
					zicio_desc->cpu_id, zicio_desc, __FILE__, __LINE__, __FUNCTION__);
#endif
#ifdef CONFIG_ZICIO_STAT
			zicio_count_softirq_trigger_shared(zicio_desc, ZICIO_NOIO);
#endif /* (CONFIG_ZICIO_STAT) */

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
			zicio_create_reactivate_trigger_shared(zicio_desc,
						ZICIO_NOIO, derailed, -1, false, 0);
#else
			zicio_create_reactivate_trigger_shared(zicio_desc,
						ZICIO_NOIO, derailed, -1, false);
#endif
		} else {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "cpu[%d] desc[%p] no IO[%s:%d][%s]\n",
					zicio_desc->cpu_id, zicio_desc, __FILE__, __LINE__, __FUNCTION__);
#endif
			zicio_require_next_chunk_shared(zicio_desc);
		}
	}
}

/**
 * __zicio_complete_command - complete function for handling nvme command
 * @zicio_desc: zicio descriptor
 * @cur_cmd: currently handling nvme command
 *
 * ZicIO has two types of execution modes, local and shared. 
 * In each mode, the action to be performed in the process of completing the
 * nvme command are different.
 */
static void __zicio_complete_command(
	struct zicio_descriptor *zicio_desc,
	struct zicio_nvme_cmd_list *cur_cmd)
{
	if (zicio_desc->zicio_shared_pool_desc) {
		__zicio_complete_command_shared(zicio_desc, cur_cmd);
	} else {
		__zicio_complete_command_local(zicio_desc, cur_cmd);
	}
}

/**
 * zicio_complete_command - function to handle zicio nvme command
 * @req_void_ptr: handling request
 * @queue_depth: nvme queue depth
 *
 * This function is called from nvme_handle_cqe(). 
 *
 * Process the given nvme command and replace it to the resubmit command.
 * If there is no nvme command to resubmit, set @req->zicio_cmd to NULL.
 */
void zicio_complete_command(void *req_void_ptr, u32 queue_depth)
{
	ktime_t start_time, end_time;
	struct request *req = (struct request *)req_void_ptr;
	struct zicio_descriptor *zicio_desc = req->bio->zicio_desc;

	BUG_ON(req->zicio_cmd == NULL);
	BUG_ON(zicio_desc == NULL);

	start_time = ktime_get();

	zicio_update_flow_ctrl(req, queue_depth);
	__zicio_complete_command(zicio_desc, req->zicio_cmd);
	zicio_do_softtimer_irq_cycle(req);

	end_time = ktime_get();

#ifdef CONFIG_ZICIO_STAT
	/* Record elapsed time */
	BUG_ON(zicio_desc->stat_board == NULL);
	zicio_desc->stat_board->io_completion_time += (end_time - start_time);
	if (zicio_desc->zicio_shared_pool_desc) {
		if (zicio_check_channel_derailed(zicio_desc)) {
			zicio_desc->stat_board->io_completion_time_derailed
				+= (end_time - start_time);
		} else {
			zicio_desc->stat_board->io_completion_time_on_track
				+= (end_time - start_time);
		}
	}
#endif /* (CONFIG_ZICIO_STAT) */
}
EXPORT_SYMBOL(zicio_complete_command);

/*
 * Function for softtimer - idle loop
 */
int zicio_do_softtimer_idle(int cpu, int global_device_idx)
{
	return zicio_do_softtimer_idle_loop(cpu, global_device_idx);
}
EXPORT_SYMBOL(zicio_do_softtimer_idle);

/*
 * Function for softtimer - softirq
 */
int zicio_do_softtimer_softirq(int cpu, int global_device_idx)
{
	return zicio_do_softtimer_timer_softirq(cpu, global_device_idx);
}
EXPORT_SYMBOL(zicio_do_softtimer_softirq);

/*
 * Dump function for debugging
 */
static inline void zicio_dump_switch_board(
	struct zicio_switch_board *sb)
{
	int i;
	printk(KERN_WARNING "[ZICIO_SWITCH_BOARD] consumed: %lu,"
				" avg_tsc_delta: %lu, data_buffer: %lu, user_idx: %d, nr_consumed_chunk: %lu\n",
				sb->consumed, sb->avg_tsc_delta,
				sb->data_buffer, sb->user_buffer_idx.counter,
				sb->nr_consumed_chunk);
	for (i = 0; i < ZICIO_MAX_NUM_CHUNK; i++) {
		printk(KERN_WARNING "[ZICIO_SWITCH_BOARD] chunk%d, status:%d\n",
			i, zicio_read_status(sb, i));
	}
}

/*
 * zicio_do_softtimer_jobs - do reserved zicio work in softirq
 */
void zicio_do_softtimer_jobs(int cpu)
{
	int dev_idx, num_raw_dev = zicio_get_num_raw_zicio_device();

	zicio_produced_data_chunk_to_shared_pool(cpu);

	for (dev_idx = 0 ; dev_idx < num_raw_dev ; dev_idx++) {
		if (!zicio_do_softtimer_softirq(cpu, dev_idx)) {
			zicio_trigger_softtimer_timer_softirq(cpu, dev_idx,
					ZICIO_DEFAULT_TIMER_SOFTIRQ_INTERVAL);
		}
	}

	/* XXX ZICIO */
	if (!zicio_notify_do_softtimer_timer_softirq(cpu)) {
		printk("[ZICIO SOFTIRQ] zicio_do_softtimer_timer_softirq() failed\n");
		zicio_notify_trigger_softtimer_timer_softirq(cpu,
			ZICIO_DEFAULT_TIMER_SOFTIRQ_INTERVAL);
	}
}
EXPORT_SYMBOL(zicio_do_softtimer_jobs);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
/*
 * Dump function for debugging
 */
void zicio_dump_switch_board_shared(zicio_descriptor *desc,
	struct zicio_switch_board *sb, int user_buffer_idx)
{
	int i;
	printk(KERN_WARNING "[ZICIO_SWITCH_BOARD] consumed: %lu,"
				" avg_tsc_delta: %lu, data_buffer: %lu, user_idx: %d, nr_consumed_chunk: %lu\n",
				sb->consumed, sb->avg_tsc_delta,
				sb->data_buffer, sb->user_buffer_idx.counter,
				sb->nr_consumed_chunk);
	for (i = 0; i < user_buffer_idx ; i++) {
		if (zicio_read_status(sb, i)) {
			printk(KERN_WARNING "[ZICIO_SWITCH_BOARD] cpu_id[%d] :  chunk%d, status:%d\n",
				desc->cpu_id, i, zicio_read_status(sb, i));
		}
	}
}
#endif /* (CONFIG_ZICIO_DEBUG_LEVEL >= 2) */

/*
 * Dump function for debugging
 */
static inline void zicio_dump_descriptor(
	struct zicio_descriptor *zicio_desc)
{
	struct zicio_firehose_ctrl *fctrl = &zicio_desc->firehose_ctrl;
	struct zicio_switch_board *sb = zicio_desc->switch_board;
	printk(KERN_WARNING "[ZICIO_DESC] cpu: %d\n", zicio_desc->cpu_id);
	zicio_dump_firehose_ctrl(fctrl);
	zicio_dump_switch_board(sb);
}

/*
 * Dump function for debugging
 */
SYSCALL_DEFINE1(zicio_u_dump_desc, int, id)
{
	zicio_dump_descriptor(zicio_get_desc(id));
	return 0;
}

/*
 * Dump function for debugging
 */
SYSCALL_DEFINE1(zicio_u_dump_per_cpu, int, cpu)
{
	int dev_idx, num_dev = zicio_get_num_raw_zicio_device();
	for (dev_idx = 0 ; dev_idx < num_dev ; dev_idx++) {
		zicio_dump_request_timer_wheel(cpu, dev_idx);
		zicio_dump_md_flow_ctrl(cpu, dev_idx);
	}
	return 0;
}

/*
 * Dump all shared pool in system
 */
SYSCALL_DEFINE0(zicio_u_dump_shared_pool)
{
	zicio_dump_all_shared_pool();
	return 0;
}

/*
 * After waiting for the shared page control block to finish sharing and then
 * reclaim this page.
 */
SYSCALL_DEFINE1(zicio_u_wait_page_reclaim, unsigned int, id)
{
	zicio_descriptor *desc;

	desc = zicio_get_desc(id);

	if (IS_ERR_OR_NULL(desc)) {
		printk(KERN_WARNING "[Kernel Message] Cannot find descriptor\n");
		return -EINVAL;
	}
	zicio_wait_shared_page_reclaim(desc);
	return 0;
}

bool zicio_is_notify(void *req_ptr)
{
	struct request *req = (struct request *)req_ptr;
	struct zicio_notify_descriptor *zicio_notify_desc
		= (zicio_notify_descriptor *)req->bio->zicio_desc;
	return zicio_notify_desc->nr_fd_of_batches != 0;
}
EXPORT_SYMBOL(zicio_is_notify);

/*
 * Initialization for individual channels.
 */
void zicio_channel_init(struct zicio_notify_descriptor *zicio_notify_desc)
{
	zicio_huge_page_consumption_stat *consumption_stat
		= &zicio_notify_desc->consumption_stat;
	zicio_data_buffer_descriptor *buf_desc
		= &zicio_notify_desc->buf_desc;
	int i;

	consumption_stat->huge_page_consumption_time = 0;
	consumption_stat->huge_page_consumption_throughput = 0;

	for (i = 0; i < ZICIO_MAX_NUM_CHUNK; i++) {
		atomic_set(buf_desc->round + i, 0);
	}

	INIT_LIST_HEAD(&zicio_notify_desc->active_nvme_cmd_timers);
	spin_lock_init(&zicio_notify_desc->lock);

	atomic_set(&zicio_notify_desc->trigger_running, 0);
}
EXPORT_SYMBOL(zicio_channel_init);

void zicio_release_timer_resources(struct zicio_notify_descriptor *zicio_notify_desc)
{
	struct zicio_nvme_cmd_timer *timer;
	struct list_head *pos, *n;

	/* disable trigger */
	while (atomic_cmpxchg(&zicio_notify_desc->trigger_running, 0, 1) != 0) {}

	list_for_each_safe(pos, n, &zicio_notify_desc->active_nvme_cmd_timers) {
		timer = (struct zicio_nvme_cmd_timer *)container_of(pos,
			struct zicio_nvme_cmd_timer, sibling);
		zicio_delete_nvme_cmd_timer(timer);
	}
}
EXPORT_SYMBOL(zicio_release_timer_resources);

/*
 * Initialization of the zicIO data structures accessed by all channels.
 */
void __init zicio_notify_kernel_init(void)
{
	zicio_init_nvme_cmd_timer_wheel();

	zicio_init_nvme_cmd_timer_slab_cache();

	zicio_init_flow_controller();
}
EXPORT_SYMBOL(zicio_notify_kernel_init);

SYSCALL_DEFINE1(zicio_u_dump_cpu, int, cpu)
{
	zicio_dump_nvme_cmd_timer_wheel(cpu);

	zicio_dump_flow_ctrl(cpu);

	return 0;
}

