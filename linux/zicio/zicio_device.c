#include <linux/kernel.h>
#include <linux/zicio_notify.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/slab.h>

#include "zicio_cmd.h"
#include "zicio_device.h"
#include "zicio_desc.h"
#include "zicio_mem.h"
#include "zicio_req_timer.h"

#include "../drivers/md/raid0.h"
#include "../drivers/md/md.h"

/*
 * The order of array elem must be the same as the order of enum
 * zicio_device_type.
 */
struct pci_driver *zicio_supported_pci_driver[ZICIO_NUM_SUPP_RAW_DEV];
static zicio_id_allocator zicio_device_mgmt;
static int num_raw_zicio_device;

/*
 * zicio_get_num_raw_zicio_device - get the number of supported devices
 */
int
zicio_get_num_raw_zicio_device(void)
{
	return num_raw_zicio_device;
}

/*
 * zicio_set_pci_driver - set supported device driver to zicio device
 * mgmt
 * @zicio_pci_driver: device-specific data
 * @dev_idx: idx in array
 */
void
zicio_set_pci_driver(void *zicio_pci_driver, int dev_idx)
{
	zicio_supported_pci_driver[dev_idx] = zicio_pci_driver;
}

/*
 * __zicio_match_device_to_driver - Checking current pci device is matched
 * to driver
 *
 * @drv: target device driver
 * @dev: device to check
 */
static inline int __zicio_match_device_to_driver(struct device_driver *drv,
				      struct device *dev)
{
	return drv->bus->match ? drv->bus->match(dev, drv) : 1;
}

/*
 * zicio_match_device_to_driver- check if device is matched with driver 
 * @dev: device
 * @name: name of target device
 *
 * return true if the name of device has target device name
 */
static inline bool
zicio_match_device_to_driver(struct pci_driver *pci_drv, struct device *dev)
{
	return __zicio_match_device_to_driver(&pci_drv->driver, dev);
}

/*
 * zicio_next_device - Get next device in device list
 *
 * @i: iterator for pci device
 */
static struct device *
zicio_next_device(struct klist_iter *i)
{
	struct klist_node *n = klist_next(i);
	struct device *dev = NULL;
	struct zicio_device_private *dev_prv;

	if (n) {
		dev_prv = zicio_to_device_private_bus(n);
		dev = dev_prv->device;
	}

	return dev;
}

static bool
__zicio_find_global_device_idx_with_device(void *zicio_struct, void *args)
{
	zicio_device *zicio_device = zicio_struct;
	struct device *device = args;

	while (device) {
		if (zicio_device->device == device) {
			return true;
		}
		device = device->parent;
	}
	return false;
}

int
zicio_get_zicio_global_device_idx_with_device(struct device *device)
{
	return zicio_find_id(&zicio_device_mgmt,
			__zicio_find_global_device_idx_with_device, device) - 1;
}

void
zicio_install_global_device_idx_to_private_array(
			zicio_nvme_private_data *zicio_nvme_data,	
			unsigned long zicio_global_device_idx)
{
	int zicio_nvme_dev_id = zicio_get_unused_id(&zicio_nvme_data->fs_dev_array);
	zicio_install_zicio_struct(&zicio_nvme_data->fs_dev_array, zicio_nvme_dev_id,
			(void *)zicio_global_device_idx);
}

static void
__zicio_set_md_device(zicio_device *zicio_dev,
			zicio_md_private_data *zicio_md_data, char *zicio_dev_name)
{
	struct r0conf *conf = zicio_md_data->mddev->private;
	struct md_rdev **devlist = conf->devlist;
	zicio_device *zicio_nvme_device;
	zicio_nvme_private_data *zicio_nvme_private_data;
	int num_inner_dev = conf->strip_zone->nb_dev, idx, inner_dev_idx;

	zicio_md_data->num_inner_dev = num_inner_dev;
	zicio_md_data->raw_device_idx_array = kmalloc(sizeof(int) * num_inner_dev,
			GFP_KERNEL);
	zicio_dev->cmd_lists_cache = kmem_cache_create(zicio_dev_name,
			sizeof(zicio_nvme_cmd_list *) * num_inner_dev << 2,
			sizeof(zicio_nvme_cmd_list *) * num_inner_dev << 2,
			SLAB_PANIC, NULL);
	
	for (idx = 0 ; idx < num_inner_dev ; idx++) {
		inner_dev_idx = zicio_get_zicio_global_device_idx_with_device(
						&devlist[idx]->bdev->bd_device);
		zicio_md_data->raw_device_idx_array[idx] = inner_dev_idx;
		zicio_nvme_device = zicio_get_zicio_device(inner_dev_idx);
		zicio_nvme_private_data = zicio_get_nvme_private_data(zicio_nvme_device);
		atomic_inc(&zicio_nvme_private_data->num_fs_dev);
		zicio_install_global_device_idx_to_private_array(
				zicio_nvme_private_data, zicio_dev->zicio_global_device_idx);
	}

	zicio_dev->device_private.device_private_data = zicio_md_data;
}

static void
__zicio_set_nvme_device(zicio_device *zicio_dev, char *zicio_dev_name)
{
	zicio_nvme_private_data * zicio_nvme_data = kmalloc(
			sizeof(zicio_nvme_private_data), GFP_KERNEL|__GFP_ZERO);
	zicio_dev->device_private.device_private_data = zicio_nvme_data;
	zicio_dev->cmd_lists_cache = kmem_cache_create(zicio_dev_name,
			sizeof(zicio_nvme_cmd_list *) << 2,
			sizeof(zicio_nvme_cmd_list *) << 2,
			SLAB_PANIC, NULL);

	atomic_inc(&zicio_nvme_data->num_fs_dev);

	zicio_init_id_allocator(&zicio_nvme_data->fs_dev_array);
	zicio_install_global_device_idx_to_private_array(zicio_nvme_data,
			zicio_dev->zicio_global_device_idx);
}

unsigned long
zicio_get_next_fs_device_idx(zicio_device *zicio_device,
			zicio_id_iterator *zicio_id_iter)
{
	zicio_nvme_private_data *zicio_nvme_private_data =
			zicio_get_nvme_private_data(zicio_device);
	void *ret = zicio_iterate_zicio_struct_to_dest(
			&zicio_nvme_private_data->fs_dev_array, zicio_id_iter);
	return (unsigned long)ret;
}

/*
 * zicio_set_device - set supported devices to zicio device mgmt
 *
 * @dev: device pointer to set
 * @device_private_data: device-specific data
 * @device_type: type of device
 */
void
zicio_set_device(struct device *dev, void *device_private_data,
			int device_type)
{
	zicio_device *zicio_dev = kmalloc(sizeof(zicio_device),
			GFP_KERNEL|__GFP_ZERO);
	int zicio_dev_id = zicio_get_unused_id(&zicio_device_mgmt);
	char zicio_dev_name[128];

	zicio_dev->device_type = device_type;
	zicio_dev->device = dev;
	zicio_dev->device_private.device_private_data = device_private_data;
	zicio_dev->zicio_global_device_idx = zicio_dev_id - 1;
	if (device_type == ZICIO_NVME) {
		sprintf(zicio_dev_name, "nvme%d", zicio_dev_id - 1);
		__zicio_set_nvme_device(zicio_dev, zicio_dev_name);
		num_raw_zicio_device++;
	} else if (device_type == ZICIO_MD) {
		sprintf(zicio_dev_name, "md%d", zicio_dev_id - 1);
		__zicio_set_md_device(zicio_dev, device_private_data, zicio_dev_name);
	} else {
		BUG();
	}

	zicio_set_command_creator(zicio_dev);
	zicio_install_zicio_struct(&zicio_device_mgmt, zicio_dev_id, zicio_dev);
}

/*
 * zicio_get_block_device_from_file
 *
 * Get device struct of block device from file
 */
struct device *
zicio_get_block_device_from_file(struct fd* fs)
{
	struct address_space * file_mapping;
	struct inode *file_owner_inode;
	struct super_block *inode_sb;
	struct block_device *sblock_bdev;
	struct gendisk *block_device_disk;
	struct device * dev = NULL;

	if (unlikely(!(file_mapping = fs->file->f_mapping))) {
		printk(KERN_WARNING "File mapping does not exist\n");
		goto l_zicio_get_block_device_from_file_err;
	}

	if (unlikely(!(file_owner_inode = file_mapping->host))) {
		printk(KERN_WARNING "File owner inode does not exist\n");
		goto l_zicio_get_block_device_from_file_err;
	}

	if (unlikely(!(inode_sb = file_owner_inode->i_sb))) {
		printk(KERN_WARNING "Super block of inode does not exist\n");
		goto l_zicio_get_block_device_from_file_err;
	}

	if (unlikely(!(sblock_bdev = inode_sb->s_bdev))) {
		printk(KERN_WARNING "Super block doesn't refer to block device\n");
		goto l_zicio_get_block_device_from_file_err;
	}

	if (unlikely(!(block_device_disk = sblock_bdev->bd_disk))) {
		printk(KERN_WARNING "Block device doesn't refer to disk info\n");
		goto l_zicio_get_block_device_from_file_err;
	}

	if (unlikely(!(dev = disk_to_dev(block_device_disk)))) {
		printk(KERN_WARNING "Cannot get device info\n");
		goto l_zicio_get_block_device_from_file_err;
	}

l_zicio_get_block_device_from_file_err:
	return dev;
}

/*
 * zicio_check_zicio_target_device - Check whether a device is zicio
 * target devices or not.
 *
 * @device - a device to check.
 *
 * Return true if the device is our target devices, otherwise then return false.
 */
static bool
zicio_check_zicio_target_device(struct device *device)
{
	int name_idx;

	for (name_idx = 0 ; name_idx < ZICIO_NUM_SUPP_RAW_DEV ; name_idx++) {
		/* If current device struct is device supported by zicio,
		 * then return true */
		while (device) {
			if (zicio_match_device_to_driver(
						zicio_supported_pci_driver[name_idx], device)) {
				return true;
			}
			device = device->parent;
		}
	}

	return false;
}

/*
 * zicio_check_devs_in_zone - Checking if an devices in zone supports
 * zicio
 *
 * @devlist: start of device array
 * @nb_dev: number of device in zone
 *
 * Return true, if the device array consists of zicio supported
 * devices. Otherwise, then return false.
 */
static bool
zicio_check_devs_in_zone(struct md_rdev **devlist, int nb_dev)
{
	int i;
	struct md_rdev *md_device;

	/* Check devices composing the zone */
	for (i = 0 ; i < nb_dev ; i++) {
		md_device = devlist[i];
		if (!zicio_check_zicio_target_device(
				&md_device->bdev->bd_device)) {
			return false;
		}
	}

	return true;
}

/*
 * zicio_check_md_devs - Checking if an md array consists of zicio
 * supported devices.
 *
 * @conf: strip configuration to check
 *
 * Return true, if the md array consists of zicio supported devices.
 * Otherwise, then return false.
 */
static bool
zicio_check_md_devs(struct r0conf *conf)
{
	struct strip_zone *z;
	struct md_rdev **devlist;
	int i;

	for (i = 0, z = conf->strip_zone, devlist = conf->devlist ;
			i < conf->nr_strip_zones ; i++, z++, devlist += z->nb_dev) {
		if (!zicio_check_devs_in_zone(devlist, z->nb_dev)) {
			return false;
		}
	}

	return true;
}

/*
 * zicio_set_md_device - set md device to zicio device array
 *
 * @bdev: md device image
 */
void
zicio_set_md_device(void *mddev)
{
	struct mddev *zicio_mddev = mddev;
	struct block_device *bdev = zicio_mddev->gendisk->part0;
	struct zicio_md_private_data *zicio_md_private_data;

	/* Checking if an md array consists of zicio supported devices. */
	if (zicio_check_md_devs(zicio_mddev->private)) {
		/* Set md device to device array. */
		zicio_md_private_data = kmalloc(sizeof(struct zicio_md_private_data),
					GFP_KERNEL);
		zicio_md_private_data->mddev = zicio_mddev;
		zicio_set_device(&(bdev->bd_device), zicio_md_private_data, ZICIO_MD);
		zicio_install_zombie_timers(1);
	}
}
EXPORT_SYMBOL(zicio_set_md_device);

/*
 * __zicio_check_file_of_device
 */
static struct device *
__zicio_check_file_of_device(zicio_device *zicio_dev, struct device *dev)
{
	while (dev && zicio_dev->device != dev) {
		dev = dev->parent;
	}

	return dev;
}

/*
 * zicio_get_zicio_device
 *
 * Get zicio device structure using id
 *
 * @dev_id: id of device in zicio device array
 */
zicio_device *
zicio_get_zicio_device(int dev_id)
{
	return zicio_get_zicio_struct(&zicio_device_mgmt, dev_id + 1, false);
}

/*
 * zicio_check_file_of_device
 *
 * Make sure the file is located on a device that supports zicio
 */
zicio_device *
zicio_check_supported_device(struct device *dev)
{
	zicio_device *zicio_dev = NULL;
	int max_ids = zicio_get_max_ids_from_idtable(&zicio_device_mgmt), i;

	BUG_ON(!dev);

	for (i = 0 ; i < max_ids ; i++) {
		zicio_dev = zicio_get_zicio_device(i);

		if (!zicio_dev) {
			continue;
		}

		if (__zicio_check_file_of_device(zicio_dev, dev)) {
			return zicio_dev;
		}
	}
	return zicio_dev;
}

/*
 * zicio_initialize_device_to_desc
 *
 * Create zicio_device struct array matched with files
 */
zicio_device **
zicio_initialize_device_to_desc(struct device **devices, int nr_fd)
{
	zicio_device **zicio_devices;
	int i;

	if (!nr_fd) {
		return NULL;
	}

	/* Allocate zicio device pointer array */
	zicio_devices = kmalloc(sizeof(zicio_device *) * nr_fd,
			GFP_KERNEL|__GFP_ZERO);

	if (unlikely(!zicio_devices)) {
		printk(KERN_WARNING "[ZICIO] Error in zicio_devices allocation\n");
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0 ; i < nr_fd ; i++) {
		if (!devices[i]) {
			break;
		}

		if (!(zicio_devices[i] = zicio_check_supported_device(devices[i]))) {
			kfree(zicio_devices);
			return ERR_PTR(-ENODEV);
		}
	}
	return zicio_devices;
}

/*
 * zicio_set_inner_device
 */
int
zicio_set_inner_device(zicio_device *device, struct device **md_devs,
			int inner_dev_idx)
{
	zicio_device *raw_device;
	int *raw_device_idx_array;
	int num_inner_devs, idx;

	BUG_ON(device->device_type != ZICIO_MD);
	num_inner_devs = zicio_get_num_inner_device(device);
	raw_device_idx_array = zicio_get_num_raw_device_idx_array(device);

	for (idx = 0 ; idx < num_inner_devs ; idx++) {
		raw_device = zicio_get_zicio_device(raw_device_idx_array[idx]);
		md_devs[inner_dev_idx++] = raw_device->device;
	}

	return num_inner_devs;
}

zicio_device **
zicio_alloc_inner_zicio_device_array(zicio_device *device)
{
	zicio_device **inner_device_array;
	int *device_idx_array;
	int idx = 0;
	int num_inner_device = zicio_get_num_inner_device(device);

	inner_device_array = kmalloc(sizeof(zicio_device *) * num_inner_device,
			GFP_KERNEL|__GFP_ZERO);

	if (num_inner_device == 1) {
		inner_device_array[idx] = device;
		return inner_device_array;
	}

	device_idx_array = zicio_get_num_raw_device_idx_array(device);

	while (idx < num_inner_device) {
		inner_device_array[idx] =
				zicio_get_zicio_device(device_idx_array[idx]);
		idx++;
	}

	return inner_device_array;
}

struct device **
zicio_alloc_inner_device_array(zicio_device *zicio_device,
			struct device *device)
{
	zicio_md_private_data *zicio_md_data;
	struct mddev *mddev;
	struct r0conf *conf;
	struct strip_zone *zone;
	struct device **inner_device_array;
	struct md_rdev *current_dev;
	int num_inner_device = zicio_get_num_inner_device(zicio_device);
	int idx = 0;

	inner_device_array = kmalloc(sizeof(struct device *) * num_inner_device,
			GFP_KERNEL|__GFP_ZERO);

	if (num_inner_device == 1) {
		inner_device_array[idx] = device;
		return inner_device_array;
	}

	zicio_md_data = zicio_get_md_private_data(zicio_device);
	mddev = zicio_md_data->mddev;
	conf = mddev->private;
	zone = conf->strip_zone;

	for (idx = 0 ; idx < zone->nb_dev ; idx++) {
		current_dev = conf->devlist[idx];
		inner_device_array[idx] = &current_dev->bdev->bd_device;
	}

	return inner_device_array;
}

/*
 * zicio_allocate_and_initialize_devices - allocate and initialize pci
 * devices for zicio channel
 */
void __init zicio_allocate_and_initialize_devices(void)
{
	struct bus_type *pci_bus = &pci_bus_type;
	struct zicio_subsys_private *pci_bus_private;
	struct klist_iter i;
	struct device *dev;
	int name_idx;

	pci_bus_private = (struct zicio_subsys_private *)pci_bus->p;
	BUG_ON(!pci_bus || !pci_bus_private);

	/* Find connected pci device */
	klist_iter_init_node(&pci_bus_private->klist_devices, &i, NULL);
	for (name_idx = 0 ; name_idx < ZICIO_NUM_SUPP_RAW_DEV ; name_idx++) {
		while ((dev = zicio_next_device(&i))) {
			/* If current device struct is device supported by zicio,
			 * then set it to zicio array */
			if (zicio_match_device_to_driver(
					zicio_supported_pci_driver[name_idx], dev)) {
				zicio_set_device(dev, NULL, name_idx);
			}
		}
	}
	klist_iter_exit(&i);
}

/*
 * zicio_init_device_mgmt - allocate and initialize zicio device array
 */
void __init zicio_init_device_mgmt(void)
{
	/*
	 * (1) Initializing zicio device structs and get the number of device
	 * (2) Allocating and initizizling data structures such as timer wheel
	 *     created for each device are allocated to each CPU.
	 * (3)-1 Initializing zicio device structs for md
	 * (3)-2 Maps previously set devices with md.
	 */
	zicio_init_id_allocator(&zicio_device_mgmt);
	zicio_allocate_and_initialize_devices();
}
