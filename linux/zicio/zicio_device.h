#ifndef __ZICIO_DEVICE_H
#define __ZICIO_DEVICE_H

#include <linux/types.h>
#include <linux/zicio_notify.h>

#include "zicio_cmd.h"
#include "zicio_desc.h"
#include "zicio_shared_pool.h"
#include "../drivers/md/raid0.h"
#include "../drivers/md/md.h"

/* The number of supported device */
#define ZICIO_NUM_SUPP_DEV (ZICIO_NUM_SUPP_RAW_DEV + 1) /* MD */

/**
 * From drivers/base/base.h
 * struct zicio_device_private - structure to hold the private to the driver
 * core portions of the device structure.
 *
 * @klist_children - klist containing all children of this device
 * @knode_parent - node in sibling list
 * @knode_driver - node in driver list
 * @knode_bus - node in bus list
 * @knode_class - node in class list
 * @deferred_probe - entry in deferred_probe_list which is used to retry the
 *	binding of drivers which were unable to get all the resources needed by
 *	the device; typically because it depends on another driver getting
 *	probed first.
 * @async_driver - pointer to device driver awaiting probe via async_probe
 * @device - pointer back to the struct device that this structure is
 * associated with.
 * @dead - This device is currently either in the process of or has been
 *	removed from the system. Any asynchronous events scheduled for this
 *	device should exit without taking any action.
 *
 * Nothing outside of the driver core should ever touch these fields.
 */
struct zicio_device_private {
	struct klist klist_children;
	struct klist_node knode_parent;
	struct klist_node knode_driver;
	struct klist_node knode_bus;
	struct klist_node knode_class;
	struct list_head deferred_probe;
	struct device_driver *async_driver;
	char *deferred_probe_reason;
	struct device *device;
	u8 dead:1;
};
#define zicio_to_device_private_bus(obj)	\
	container_of(obj, struct zicio_device_private, knode_bus)

/**
 * From drivers/base/base.h
 * struct zicio_subsys_private - structure to hold the private to the driver
 * core portions of the bus_type/class structure.
 *
 * @subsys - the struct kset that defines this subsystem
 * @devices_kset - the subsystem's 'devices' directory
 * @interfaces - list of subsystem interfaces associated
 * @mutex - protect the devices, and interfaces lists.
 *
 * @drivers_kset - the list of drivers associated
 * @klist_devices - the klist to iterate over the @devices_kset
 * @klist_drivers - the klist to iterate over the @drivers_kset
 * @bus_notifier - the bus notifier list for anything that cares about things
 *                 on this bus.
 * @bus - pointer back to the struct bus_type that this structure is associated
 *        with.
 *
 * @glue_dirs - "glue" directory to put in-between the parent device to
 *              avoid namespace conflicts
 * @class - pointer back to the struct class that this structure is associated
 *          with.
 *
 * This structure is the one that is the actual kobject allowing struct
 * bus_type/class to be statically allocated safely.  Nothing outside of the
 * driver core should ever touch these fields.
 */
struct zicio_subsys_private {
	struct kset subsys;
	struct kset *devices_kset;
	struct list_head interfaces;
	struct mutex mutex;

	struct kset *drivers_kset;
	struct klist klist_devices;
	struct klist klist_drivers;
	struct blocking_notifier_head bus_notifier;
	unsigned int drivers_autoprobe:1;
	struct bus_type *bus;

	struct kset glue_dirs;
	struct class *class;
};

typedef struct zicio_md_private_data {
	struct mddev *mddev;
	/* Raw device index array */
	int *raw_device_idx_array;
	/* Number of real device consisting of logical device */
	int num_inner_dev;
} zicio_md_private_data;

typedef struct zicio_nvme_private_data {
	zicio_id_allocator fs_dev_array;
	atomic_t num_fs_dev;
} zicio_nvme_private_data;

#define to_subsys_private(obj) container_of(obj,	\
		struct zicio_subsys_private, subsys.kobj)
zicio_device ** zicio_initialize_device_to_desc(
		struct device **devices, int nr_fd);
int zicio_get_num_raw_zicio_device(void);
struct device *zicio_get_block_device_from_file(struct fd* fs);
zicio_device *zicio_get_zicio_device(int dev_id);
zicio_device ** zicio_alloc_inner_zicio_device_array(
			zicio_device *zicio_device);
struct device ** zicio_alloc_inner_device_array(
			zicio_device *zicio_device, struct device *device);
int zicio_set_inner_device(zicio_device *zicio_device,
		struct device **md_devs, int inner_dev_idx);
unsigned long zicio_get_next_fs_device_idx(zicio_device *zicio_device,
		zicio_id_iterator *zicio_id_iter);

/*
 * zicio_get_zicio_global_device_idx
 *
 * Get the global device order for zicio.
 */
static inline int
zicio_get_zicio_global_device_idx(zicio_descriptor *desc, int device_idx)
{
	zicio_device *zicio_device;

	if (device_idx < desc->dev_maps.nr_dev) {
		zicio_device = desc->dev_maps.dev_node_array[device_idx].zicio_devs;
	} else {
		zicio_device = desc->dev_maps.zicio_inner_dev_maps[device_idx -
					desc->dev_maps.nr_dev].zicio_inner_dev;
	}

	return zicio_device->zicio_global_device_idx;
}

/*
 * zicio_get_zicio_channel_fsdev_idx
 *
 * Get the global inner device order for zicio.
 */
static inline int
zicio_get_zicio_channel_fsdev_idx(zicio_descriptor *desc, int device_idx)
{
	int channel_mddev_idx;

	if (device_idx < desc->dev_maps.nr_dev) {
		return desc->dev_maps.dev_node_array[device_idx].zicio_devs->
				zicio_global_device_idx;
	}

	channel_mddev_idx = desc->dev_maps.zicio_inner_dev_maps[device_idx -
			desc->dev_maps.nr_dev].mddev_idx;

	return zicio_get_zicio_global_device_idx(desc, channel_mddev_idx);
}

/*
 * zicio_get_zicio_channel_mddev_idx
 *
 * Get the channel md device order for zicio.
 */
static inline int
zicio_get_zicio_channel_mddev_idx(zicio_descriptor *desc, int device_idx)
{
	BUG_ON(device_idx < desc->dev_maps.nr_dev);
	return desc->dev_maps.zicio_inner_dev_maps[device_idx -
			desc->dev_maps.nr_dev].mddev_idx;
}

/*
 * zicio_get_shared_pool_mddev_idx
 *
 * Get the zicio shared pool md device order.
 */
static inline int
zicio_get_shared_pool_mddev_idx(zicio_shared_pool *zicio_shared_pool,
			int device_idx)
{
	zicio_dev_maps *dev_maps;
	BUG_ON(device_idx < zicio_shared_pool->shared_dev_maps.dev_maps.nr_dev);
	dev_maps = &zicio_shared_pool->shared_dev_maps.dev_maps;

	return dev_maps->zicio_inner_dev_maps[device_idx - dev_maps->nr_dev].mddev_idx;
}

/*
 * zicio_get_zicio_channel_dev_idx
 *
 * Get the channel device order for zicio.
 */
static inline int
zicio_get_zicio_channel_dev_idx(zicio_descriptor *desc, int device_idx)
{
	if (device_idx < desc->dev_maps.nr_dev) {
		return device_idx;
	} else {
		return zicio_get_zicio_channel_mddev_idx(desc, device_idx);
	}
}

/*
 * zicio_get_zicio_channel_raw_dev_idx
 *
 * Get the channel raw device order for zicio.
 */
static inline int
zicio_get_zicio_channel_raw_dev_idx(zicio_descriptor *desc, int device_idx)
{
	if (device_idx < desc->dev_maps.nr_dev) {
		BUG_ON(desc->dev_maps.dev_node_array[device_idx].zicio_devs->device_type ==
				ZICIO_MD);
		return desc->dev_maps.dev_node_array[device_idx].raw_dev_idx_in_channel;
	} else {
		return desc->dev_maps.zicio_inner_dev_maps[device_idx -
				desc->dev_maps.nr_dev].raw_dev_idx_in_channel;
	}
}

/*
 * zicio_get_zicio_channel_innerdev_idx
 *
 * Get the channel inner device order for zicio.
 */
static inline int
zicio_get_zicio_channel_innerdev_idx(zicio_descriptor *desc,
		int device_idx)
{
	int mddev = zicio_get_zicio_channel_mddev_idx(desc, device_idx);
	BUG_ON(device_idx <
			desc->dev_maps.dev_node_array[mddev].start_point_inner_dev_map);
	return (device_idx - desc->dev_maps.nr_dev -
			desc->dev_maps.dev_node_array[mddev].start_point_inner_dev_map);
}

/*
 * zicio_get_zicio_channel_innerdev_idx
 *
 * Get the channel inner device order for zicio.
 */
static inline int
zicio_get_shared_pool_innerdev_idx(zicio_shared_pool *zicio_shared_pool,
		int device_idx)
{
	int mddev = zicio_get_shared_pool_mddev_idx(zicio_shared_pool, device_idx);
	BUG_ON(device_idx < zicio_shared_pool->shared_dev_maps.dev_maps.dev_node_array[
			mddev].start_point_inner_dev_map);
	return (device_idx - zicio_shared_pool->shared_dev_maps.dev_maps.nr_dev -
			zicio_shared_pool->shared_dev_maps.dev_maps.dev_node_array[
					mddev].start_point_inner_dev_map);
}

/*
 * zicio_get_zicio_device_with_desc
 *
 * Get the zicio_device with zicio descriptor
 */
static inline zicio_device *
zicio_get_zicio_device_with_desc(zicio_descriptor *desc, int device_idx)
{
	if (device_idx < desc->dev_maps.nr_dev) {
		return desc->dev_maps.dev_node_array[device_idx].zicio_devs;
	} else {
		return desc->dev_maps.zicio_inner_dev_maps[device_idx -
					desc->dev_maps.nr_dev].zicio_inner_dev;
	}
}

static inline zicio_device *
zicio_get_zicio_device_with_shared_pool(
			zicio_shared_pool *zicio_shared_pool, int device_idx)
{
	if (device_idx < zicio_shared_pool->shared_dev_maps.dev_maps.nr_dev) {
		return zicio_shared_pool->
				shared_dev_maps.dev_maps.dev_node_array[device_idx].zicio_devs;
	} else {
		return zicio_shared_pool->shared_dev_maps.dev_maps.zicio_inner_dev_maps[
				device_idx - zicio_shared_pool->shared_dev_maps.dev_maps.nr_dev].zicio_inner_dev;
	}
}

/*
 * zicio_get_zicio_fs_device_with_desc
 *
 * Get the logical device index from device
 */
static inline zicio_device *
zicio_get_zicio_fs_device_with_desc(zicio_descriptor *desc, int device_idx)
{
	int mddev;
	if (device_idx < desc->dev_maps.nr_dev) {
		return desc->dev_maps.dev_node_array[device_idx].zicio_devs;
	} else {
		mddev = zicio_get_zicio_channel_mddev_idx(desc, device_idx);
		return desc->dev_maps.dev_node_array[mddev].zicio_devs;
	}
}

/*
 * zicio_get_inner_device_with_desc
 *
 * Get the channel device index with channel
 */
static inline struct device*
zicio_get_inner_device_with_desc(zicio_descriptor *desc,
		int device_idx)
{
	int mddev_idx, dev_start_idx;
	if (device_idx < desc->dev_maps.nr_dev) {
		return desc->dev_maps.dev_node_array[device_idx].devs;
	}
	mddev_idx = desc->dev_maps.zicio_inner_dev_maps[device_idx -
		desc->dev_maps.nr_dev].mddev_idx;
	dev_start_idx =
		desc->dev_maps.dev_node_array[mddev_idx].start_point_inner_dev_map;
	return desc->dev_maps.dev_node_array[mddev_idx].inner_devs[device_idx -
		dev_start_idx - desc->dev_maps.nr_dev];
}

/*
 * zicio_get_command_creator
 *
 * Get command creator from zicio device descriptor
 */
static inline zicio_command_creator *
zicio_get_command_creator(zicio_device *zicio_device)
{
	return &zicio_device->device_private.zicio_cmd_creator;
}

/*
 * zicio_get_nvme_private_data
 *
 * Get nvme private data from zicio device
 */
static inline zicio_nvme_private_data *
zicio_get_nvme_private_data(zicio_device *zicio_device)
{
	return zicio_device->device_private.device_private_data;
}

/*
 * zicio_get_md_private_data
 *
 * Get md private data from zicio device
 */
static inline zicio_md_private_data *
zicio_get_md_private_data(zicio_device *zicio_device)
{
	return zicio_device->device_private.device_private_data;
}

/*
 * zicio_get_num_inner_device
 *
 * Get the number of inner device from fs device
 */
static inline int
zicio_get_num_inner_device(zicio_device *zicio_device)
{
	if (zicio_device->device_type != ZICIO_MD) {
		return 1;
	}
	return zicio_get_md_private_data(zicio_device)->num_inner_dev;
}

/*
 * zicio_get_num_fs_device
 *
 * Get the number of device which refers to @zicio_device
 */
static inline unsigned int
zicio_get_num_fs_device(zicio_device *zicio_device)
{
	return atomic_read(
			&(zicio_get_nvme_private_data(zicio_device)->num_fs_dev));
}

/*
 * zicio_get_num_raw_device_idx_array
 *
 * Get the number of global device idx array from zicio_device
 */
static inline int*
zicio_get_num_raw_device_idx_array(zicio_device *zicio_device)
{
	BUG_ON(zicio_device->device_type != ZICIO_MD);
	return zicio_get_md_private_data(zicio_device)->raw_device_idx_array;
}

/*
 * zicio_get_num_inner_device_with_id
 *
 * Get the number of inner device from fs device with global device idx
 */
static inline int
zicio_get_num_inner_device_with_id(int global_device_idx)
{
	return zicio_get_num_inner_device(
				zicio_get_zicio_device(global_device_idx));
}

/*
 * zicio_get_num_inner_device
 *
 * Get the number of page stride from md device
 */
static inline int
zicio_get_mddev_stride_with_desc(zicio_descriptor *desc, int mddev_idx)
{
	zicio_device *zicio_device =
			zicio_get_zicio_device_with_desc(desc, mddev_idx);
	zicio_md_private_data *zicio_md_data =
			zicio_get_md_private_data(zicio_device);

	return zicio_md_data->mddev->chunk_sectors >> ZICIO_PAGE_TO_SECTOR_SHIFT;
}

/*
 * zicio_get_mddev_stride_with_shared_pool
 *
 * Get the number of page stride from md device idx in shared pool
 */
static inline int
zicio_get_mddev_stride_with_shared_pool(
			zicio_shared_pool *zicio_shared_pool, int mddev_idx)
{
	zicio_device *zicio_device =
			zicio_get_zicio_device_with_shared_pool(zicio_shared_pool, mddev_idx);
	zicio_md_private_data *zicio_md_data =
			zicio_get_md_private_data(zicio_device);

	return zicio_md_data->mddev->chunk_sectors >> ZICIO_PAGE_TO_SECTOR_SHIFT;
}

/*
 * zicio_alloc_cmd_lists_set
 *
 * Allocate command list set array
 */
static inline void *
zicio_alloc_cmd_lists_set(zicio_device *zicio_device)
{
	return kmem_cache_alloc(zicio_device->cmd_lists_cache, GFP_KERNEL|__GFP_ZERO);
}

/*
 * zicio_free_cmd_lists_set
 *
 * Free command list set array
 */
static inline void
zicio_free_cmd_lists_set(zicio_device *zicio_device, void *cmd_lists_set)
{
	kmem_cache_free(zicio_device->cmd_lists_cache, cmd_lists_set);
}

/*
 * zicio_alloc_cmd_lists_set_with_desc
 *
 * Allocate command list set array with zicio descriptor
 */
static inline void *
zicio_alloc_cmd_lists_set_with_desc(zicio_descriptor *desc,
			int channel_dev_idx)
{
	zicio_device *zicio_device =
			zicio_get_zicio_device_with_desc(desc, channel_dev_idx);
	return zicio_alloc_cmd_lists_set(zicio_device);
}

/*
 * zicio_free_cmd_lists_set_with_desc
 *
 * Free command list set array with zicio descriptor
 */
static inline void
zicio_free_cmd_lists_set_with_desc(zicio_descriptor *desc,
			int channel_dev_idx, void *cmd_lists_set)
{
	zicio_device *zicio_device =
			zicio_get_zicio_device_with_desc(desc, channel_dev_idx);
	zicio_free_cmd_lists_set(zicio_device, cmd_lists_set);
}
#endif /* __ZICIO_DEVICE_H_ */
