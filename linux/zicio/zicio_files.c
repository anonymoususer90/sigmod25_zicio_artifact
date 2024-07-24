#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/zicio_notify.h>
#include <linux/bits.h>
#include <linux/nospec.h>
#include <linux/stat.h>
#include <linux/sort.h>

#include "zicio_device.h"
#include "zicio_extent.h"
#include "zicio_files.h"
#include "zicio_mem.h"
#include "zicio_shared_pool.h"

extern __zicio_slab_info zicio_cache_info[ZICIO_NUM_ALLOC_CACHE];
/*
 * zicio_get_distinct_device_and_map
 *
 * Get the distinct set of device and its mapping information to file
 */
static long
zicio_get_distinct_device_and_map(struct fd* fs, struct device **devs,
		int nr_fd, int *file_dev_map, int dev_alloced)
{
	struct device *cur_dev;
	int i, j, ret = dev_alloced;
	bool found;

	/* Get the distinct set of devices */
	for (i = 0 ; i < nr_fd ; i++) {
		found = false;
		cur_dev = zicio_get_block_device_from_file(&fs[i]);

		if (!cur_dev) {
			printk(KERN_WARNING "[Kernel Message] Cannot get a device "
					" struct\n");

			ret = -ENODEV;
			break;
		}

		for (j = 0 ; j < ret ; j++) {
			if (devs[j] == cur_dev) {
				file_dev_map[i] = j;
				found = true;
				break;
			}
		}

		if (!found) {
			file_dev_map[i] = ret;
			devs[ret++] = cur_dev;
		}
	}

	return ret;
}

/*
 * zicio_get_distinct_file_dev_set
 *
 * Get distinct file-dev map and device set
 */
struct device **
zicio_get_distinct_file_dev_set(int **file_dev_map, struct fd* fs,
		int nr_fd)
{
	struct device **devs = NULL;
	int dev_alloced = 0, err;

	if (!nr_fd) {
		return NULL;	
	}

	*file_dev_map = kmalloc(sizeof(int) * nr_fd, GFP_KERNEL|__GFP_ZERO);

	if (!unlikely(*file_dev_map)) {
		printk(KERN_WARNING "[Kernel Message] Cannot allocate file_devs\n");
		err = -ENOMEM;

		goto l_zicio_get_distinct_file_dev_set_err;
	}

	devs = kmalloc(sizeof(struct device*) * nr_fd, GFP_KERNEL);

	if (unlikely(!devs)) {
		printk(KERN_WARNING "[Kernel Message] Cannot allocate devs\n");

		err = -ENOMEM;
		goto l_zicio_get_distinct_file_dev_set_err_file_dev_map;
	}

	memset(devs, 0, sizeof(struct device *) * nr_fd);

	if (IS_ERR(ERR_PTR(err =
			zicio_get_distinct_device_and_map(fs, devs, nr_fd,
					*file_dev_map, dev_alloced)))) {
		goto l_zicio_get_distinct_file_dev_set_err_devs;
	}

	return devs;

l_zicio_get_distinct_file_dev_set_err_devs:
	zicio_free_if_not_null(devs);
l_zicio_get_distinct_file_dev_set_err_file_dev_map:
	zicio_free_if_not_null(*file_dev_map);
	*file_dev_map = NULL;

l_zicio_get_distinct_file_dev_set_err:
	return ERR_PTR(err);
}

/* 
 * zicio_copy_fds_from_user
 * @ fds: file descriptor array from user
 * @ nr_fd : number of fd from user
 * return : fd array of kernel
 *
 * Allocate and copy file descriptor array from user
 */
unsigned int *
zicio_copy_fds_from_user(unsigned int __user *fds, int nr_fd)
{
	unsigned int * _fds; /* Kernel file descriptor array */

	/* Check if number of fd from user is zero */
	if (!nr_fd) {
		return NULL;
	}

	/* Allocate unsigned int array for fd array */
	_fds = (unsigned int *)kmalloc(sizeof(unsigned int) * nr_fd, GFP_KERNEL);

	if (unlikely(!_fds)) {
		printk(KERN_WARNING"[Kernel Message] Cannot allocate a fd array\n");
		return ERR_PTR(-ENOMEM);
	}

	/* Copy file descriptor from user */
	if (copy_from_user(_fds, fds, sizeof(unsigned int) * nr_fd)) {
		kfree(_fds);

		printk("[Kernel Message] Cannot copy fds from user\n");
		return ERR_PTR(-1);
	}

	return _fds;
}

/* 
 * zicio_get_fdstruct
 * @ fds: file descriptor array from user
 * @ nr_fd : number of fd from user
 * return : fd array of kernel
 *
 * Allocate and initialize struct fd array using fds
 */
struct fd*
zicio_get_fdstruct(unsigned int *fds, int nr_fd)
{
	int fd_idx; /* iterator idx for fds */
	struct fd* fs; /* file info array to send ZicIO */

	if (!nr_fd) {
		return NULL;
	}

	fs = (struct fd*)kmalloc(sizeof(struct fd) * nr_fd, GFP_KERNEL);

	/* Get file information from fds */
	for (fd_idx = 0 ; fd_idx < nr_fd ; fd_idx++) {
		fs[fd_idx] = fdget_raw(fds[fd_idx]);

		/* Validate file descriptors from user */
		if (!fs[fd_idx].file) {
			printk("[Kernel Message] error in file validate\n");
			/* If a file descriptor is invalid, then return bad file error */

			kfree(fs);
			return ERR_PTR(-EBADF);
		}
	}

	return fs;
}

/*
 * zicio_initialize_fds_to_desc
 *
 * Initialize file information for channel
 */
long
zicio_initialize_fds_to_desc(zicio_descriptor *desc, struct fd* fs,
		unsigned int *fds, int *file_dev_map, int nr_fd)
{
	return zicio_initialize_read_files(&desc->read_files, fs, fds,
				file_dev_map, nr_fd);
}

/*
 * zicio_get_tot_filesize
 *
 * Get the total chunk number of all files and per-file number of chunks array
 * TODO:Currently, we do not consider the situation where a file is missing in
 * the middle. If necessary later, it is necessary to deal with the situation.
 */
unsigned long
zicio_get_total_filesize(zicio_shared_files *shared_files)
{
	zicio_read_files *add_files = &shared_files->registered_read_files;
	zicio_file_struct **zicio_file_arr;
	zicio_file_struct *zicio_file_struct;
	unsigned long tot_size = 0;
	int num_fds, i;
	int num_chunks;

	num_fds = add_files->num_fds;
	shared_files->start_chunk_nums = kmalloc(sizeof(unsigned long) * num_fds,
				GFP_KERNEL);

	rcu_read_lock();

	zicio_file_arr = rcu_dereference(add_files->zicio_file_array);

	for (i = 0 ; i < num_fds ; i++) {
		zicio_file_struct = rcu_dereference_raw(zicio_file_arr[i]);
		num_chunks = DIV_ROUND_UP(zicio_file_struct->file_size,
					((1UL) << ZICIO_PAGE_TO_CHUNK_SHIFT));

		shared_files->start_chunk_nums[i] = tot_size;
		tot_size += num_chunks;
	}

	rcu_read_unlock();

	return tot_size;
}

/*
 * zicio_set_read_files
 *
 * If we need to manage more files than the initially allocated file array,
 * allocate more read file arrays.
 */
static zicio_file_struct **
zicio_set_read_files(struct fd* new_fs, unsigned int *new_fds,
			int *new_file_dev_map, int nr_old_fd, int nr_new_fd,
			unsigned long *total_file_size, unsigned long total_old_file_size)
{
	int i, cur_idx;
	struct kstat stat;
	zicio_file_struct **new_arr;
	zicio_file_struct *new_data;

	new_arr = kmalloc(sizeof(zicio_file_struct*) * (nr_new_fd + nr_old_fd),
			GFP_KERNEL);

	if (unlikely(!new_arr)) {
		return NULL;
	}

	for (cur_idx = 0 ; cur_idx < nr_new_fd ; cur_idx++) {
		new_data = kmalloc(sizeof(zicio_file_struct),
				GFP_KERNEL|__GFP_ZERO);

		if (unlikely(!new_data)) {
			goto l_zicio_set_read_files_err;
		}

		vfs_fstat(new_fds[cur_idx], &stat);

		new_data->fd = new_fs[cur_idx];
		new_data->file_size = DIV_ROUND_UP(stat.size, ZICIO_PAGE_SIZE);
		new_data->device_idx_in_channel = new_file_dev_map[cur_idx];
		new_data->start_page_idx_in_set = *total_file_size +
				total_old_file_size;
		*total_file_size += new_data->file_size;

		new_arr[cur_idx + nr_old_fd] = new_data;
	}

	return new_arr;
l_zicio_set_read_files_err:
	for (i = 0 ; i < cur_idx ; i++) {
		kfree(new_arr[i + nr_old_fd]);
	}
	kfree(new_arr);
	return NULL;
}

/*
 * __zicio_update_read_files
 *
 * Used to add a new file
 */
static zicio_file_struct **
__zicio_update_read_files(zicio_read_files *add_files,
		struct fd* new_fs, unsigned int *new_fds, int *new_file_dev_map,
		int nr_fd, int *num_old_fds)
{
	zicio_file_struct **old_arr, **new_arr;
	unsigned long new_file_size = 0;

	spin_unlock(&add_files->read_file_guard);

	*num_old_fds = add_files->num_fds;

	new_arr = zicio_set_read_files(new_fs, new_fds, new_file_dev_map,
			*num_old_fds, nr_fd, &new_file_size, add_files->total_file_size);

	if (atomic_read(&add_files->count) > 1)
		synchronize_rcu();

	spin_lock(&add_files->read_file_guard);
	if (!new_arr) {
		return ERR_PTR(-ENOMEM);
	}

	old_arr = zicio_get_files_from_rcu(add_files);

	if (*num_old_fds) {
		memcpy(new_arr, old_arr, sizeof(zicio_file_struct*) *
					(*num_old_fds));
	}

	rcu_assign_pointer(add_files->zicio_file_array, new_arr);
	add_files->num_fds = *num_old_fds + nr_fd;
	add_files->total_file_size += new_file_size;

	smp_wmb();
	return old_arr;
}

/*
 * zicio_update_read_files
 *
 * Initialize file information for channel
 */
long
zicio_update_read_files(zicio_read_files *add_files, struct fd* new_fs,
		unsigned int *new_fds, int *new_file_dev_map, int nr_fd)
	__acquires(add_files->read_file_guard)
	__releases(add_files->read_file_guard)
{
	zicio_file_struct **old_arr;
	int num_old_fds;

	spin_lock(&add_files->read_file_guard);

l_zicio_update_read_files_repeat:
	/* Check if the resize is progress */
	if (unlikely(add_files->resize_in_progress)) {
		/* If another guy is updating file array, then waiting him. */
		spin_unlock(&add_files->read_file_guard);
		wait_event(add_files->resize_wait, !add_files->resize_in_progress);
		spin_lock(&add_files->read_file_guard);
		goto l_zicio_update_read_files_repeat;
	}

	add_files->resize_in_progress = true;

	old_arr = __zicio_update_read_files(add_files, new_fs, new_fds,
		new_file_dev_map, nr_fd, &num_old_fds);

	add_files->resize_in_progress = false;

	wake_up_all(&add_files->resize_wait);

	spin_unlock(&add_files->read_file_guard);

	synchronize_rcu();

	if (IS_ERR(old_arr)) {
		return -ENOMEM;
	}

	if (!old_arr) {
		return 0;
	}

	if (num_old_fds) {
		kfree(old_arr);
	}

	return 0;
}

/*
 * zicio_initialize_read_files
 *
 * Initialize file information for channel
 */
long
zicio_initialize_read_files(zicio_read_files *add_files, struct fd* fs,
		unsigned int *fds, int *file_dev_map, int nr_fd)
{
	long err;

	/*
	 * Initialize spin lock for zicio file array
	 */ 
	spin_lock_init(&add_files->read_file_guard);
	spin_lock_init(&add_files->resize_wait.lock);
	INIT_LIST_HEAD(&add_files->resize_wait.head);

	if (!nr_fd) {
		return 0;
	}

	atomic_set(&add_files->fd_cursor, 0);

	err = zicio_update_read_files(add_files, fs, fds, file_dev_map, nr_fd);

	return err;
}

/*
 * zicio_free_read_files
 *
 * When deleting a channel or shared pool, it is used to remove the file
 * information under management.
 */
void
zicio_free_read_files(zicio_read_files *add_files)
{
	zicio_file_struct **old_arr;
	int num_fd, i;

	spin_lock(&add_files->read_file_guard);

	num_fd = add_files->num_fds;
	old_arr = zicio_get_files_from_rcu(add_files);

	rcu_assign_pointer(add_files->zicio_file_array, NULL);

	spin_unlock(&add_files->read_file_guard);

	synchronize_rcu();

	if (old_arr) {
		for (i = 0 ; i < num_fd ; i++) {
			if (old_arr[i]) {
				/* After finishing direct I/O, releasing inode */
				inode_dio_end(file_inode(old_arr[i]->fd.file));
				fdput(old_arr[i]->fd);
				if (old_arr[i]->extent_tree_buffer) {
					kfree(old_arr[i]->extent_tree_buffer);
				}
				kfree(old_arr[i]);
			}
		}
		kfree(old_arr);
	}
}

/*
 * zicio_get_id_file_struct
 *
 * Get zicio file struct using id
 */
zicio_file_struct*
zicio_get_id_file_struct(zicio_read_files *zicio_rfile, unsigned int id)
{
	zicio_file_struct **zicio_file_arr;
	zicio_file_struct *cur_fobj;

	BUG_ON(id >= zicio_rfile->num_fds);

	rcu_read_lock();

	zicio_file_arr = rcu_dereference(zicio_rfile->zicio_file_array);
	cur_fobj = zicio_file_arr[id];

	rcu_read_unlock();
	return cur_fobj;
}

/*
 * __zicio_get_next_file_struct
 *
 * Return the next zicio file struct pointed by the cursor
 */
static inline zicio_file_struct*
__zicio_get_next_file_struct(zicio_read_files *zicio_rfile)
{
	unsigned int id;
	zicio_file_struct **zicio_file_arr;
	zicio_file_struct *cur_fobj;

	rcu_read_lock();
	if (*(long *)(&zicio_rfile->fd_cursor) + 1 < zicio_rfile->num_fds) {
		id = array_index_nospec(*(unsigned long *)(&zicio_rfile->fd_cursor) + 1,
					zicio_rfile->num_fds);
		zicio_file_arr = rcu_dereference(zicio_rfile->zicio_file_array);
		cur_fobj = zicio_file_arr[id];
		rcu_read_unlock();

		return cur_fobj;
	}

	rcu_read_unlock();
	return NULL;
}

/*
 * __zicio_get_next_unready_file_struct
 *
 * Return the next unready zicio file struct pointed by the cursor
 */
static inline zicio_file_struct*
__zicio_get_next_unready_file_struct(zicio_read_files *zicio_rfile)
{
	unsigned int id;
	unsigned long fd_cursor;
	zicio_file_struct **zicio_file_arr;
	zicio_file_struct *cur_fobj;

	rcu_read_lock();

	fd_cursor = *(unsigned long*)(&zicio_rfile->fd_cursor) + 1;

check_next_file:
	if (fd_cursor < zicio_rfile->num_fds) {
		id = array_index_nospec(fd_cursor, zicio_rfile->num_fds);
		zicio_file_arr = rcu_dereference(zicio_rfile->zicio_file_array);
		cur_fobj = zicio_file_arr[id];

		if (zicio_check_all_extent_consumed(cur_fobj)) {
			fd_cursor++;
			goto check_next_file;
		}
		rcu_read_unlock();

		return cur_fobj;
	}

	rcu_read_unlock();
	return NULL;
}



/*
 * zicio_get_next_file_struct
 *
 * Return the next zicio file struct pointed by the cursor
 */
zicio_file_struct*
zicio_get_next_file_struct(zicio_read_files *zicio_rfile)
{
	return __zicio_get_next_file_struct(zicio_rfile);
}


/*
 * zicio_get_next_unready_file_struct
 *
 * Return the next zicio file struct pointed by the cursor
 */
zicio_file_struct*
zicio_get_next_unready_file_struct(zicio_read_files *zicio_rfile)
{
	return __zicio_get_next_unready_file_struct(zicio_rfile);
}



/*
 * __zicio_get_current_file_struct
 *
 * Returns the file struct where the current file cursor is located.
 */
static inline zicio_file_struct*
__zicio_get_current_file_struct(zicio_read_files *zicio_rfile)
{
	unsigned int id, fd_cursor;
	zicio_file_struct **zicio_file_arr;
	zicio_file_struct *cur_file_struct;

	rcu_read_lock();
	fd_cursor = atomic_read(&zicio_rfile->fd_cursor);

	if (fd_cursor < zicio_rfile->num_fds) {
		id = array_index_nospec(fd_cursor, zicio_rfile->num_fds);
		zicio_file_arr = rcu_dereference(zicio_rfile->zicio_file_array);
		cur_file_struct = zicio_file_arr[id];
		rcu_read_unlock();

		return cur_file_struct;
	}
	rcu_read_unlock();
	return NULL;
}

/*
 * zicio_get_current_file
 *
 * Get current file for reading
 */
zicio_file_struct *
zicio_get_current_file_struct_shared(zicio_descriptor *desc,
			unsigned long file_chunk_id, bool set_shared_file, bool recheck)
{
	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool_local *zicio_shared_pool_local;
	zicio_shared_files *zicio_shared_files;
	int id;

	zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);
	zicio_shared_files = &zicio_shared_pool->shared_files;

	id = zicio_get_current_file_id_shared(zicio_shared_pool,
				zicio_shared_pool_local, file_chunk_id, set_shared_file, recheck);
	return zicio_get_id_file_struct(
				&zicio_shared_files->registered_read_files, id);
}

/*
 * zicio_get_current_file
 *
 * Get current file for reading
 */
zicio_file_struct *
zicio_get_current_file_struct(zicio_descriptor *desc)
{
	return __zicio_get_current_file_struct(&desc->read_files);
}

/*
 * zicio_check_neighbor_file_page_idx
 *
 * Check if the current file chunk id is located in the files adjacent to the
 * current cursor
 */
static zicio_file_struct *
zicio_check_neighbor_file_page_idx(zicio_descriptor *desc,
			unsigned long current_file_page_idx)
{
	unsigned int id = atomic_read(&desc->read_files.fd_cursor);
	zicio_file_struct *neighbor_file_struct;

	if (id > 0) {
		neighbor_file_struct = zicio_get_id_file_struct(&desc->read_files,
					id - 1);
	}

	if (zicio_check_current_file_page_idx(neighbor_file_struct,
				current_file_page_idx)) {
		return neighbor_file_struct;
	}

	if (id < desc->read_files.num_fds - 1) {
		neighbor_file_struct = zicio_get_id_file_struct(&desc->read_files,
					id + 1);
	}

	if (zicio_check_current_file_page_idx(neighbor_file_struct,
				current_file_page_idx)) {
		zicio_set_file_cursor(desc, id + 1);
		return neighbor_file_struct;
	}

	return NULL;
}

/*
 * zicio_set_in_file_page_idx
 *
 * Finds and sets the location of the page received as a parameter in the
 * current file.
 */
static inline void
zicio_set_in_file_page_idx(zicio_file_struct *current_file,
			unsigned long current_file_page_idx, unsigned int *in_file_page_idx)
{
	if (in_file_page_idx) {
		*in_file_page_idx = current_file_page_idx -
					current_file->start_page_idx_in_set;
	}
}

/*
 * zicio_find_current_file_struct
 *
 * While traversing the file array, find the file where the current chunk id is
 * located.
 */
static zicio_file_struct*
zicio_find_current_file_struct(zicio_descriptor *desc,
			unsigned long current_file_page_idx)
{
	zicio_file_struct *current_file_struct;
	unsigned int num_fds = desc->read_files.num_fds;
	unsigned int id;	

	for (id = 0 ; id < num_fds ; id++) {
		current_file_struct =
				zicio_get_id_file_struct(&desc->read_files, id);

		if (zicio_check_current_file_page_idx(current_file_struct,
					current_file_page_idx)) {
			return current_file_struct;
		}
	}
	return NULL;
}

/*
 * zicio_get_file_struct_for_cmd
 *
 * Get file object for command processing
 * As multiple requests can be sent in one channel, it is necessary to track
 * when the file cursor moves.
 */
zicio_file_struct*
zicio_get_file_struct_for_cmd(zicio_descriptor *desc,
			unsigned long current_file_page_idx,
			unsigned int *in_file_page_idx)
{
	zicio_file_struct *current_file =
			zicio_get_current_file_struct(desc);

	if (!current_file) {
		return current_file;
	}

	BUG_ON(current_file_page_idx == ULONG_MAX);

	/* Checking file cursor is valid firstly*/
	if (zicio_check_current_file_page_idx(current_file,
				current_file_page_idx)) {
		zicio_set_in_file_page_idx(current_file, current_file_page_idx,
					in_file_page_idx);
		return current_file;
	}

	/* If our cursor isn't matched, then checking previous and next cursor */
	if ((current_file =	zicio_check_neighbor_file_page_idx(desc,
				current_file_page_idx)) != NULL) {
		zicio_set_in_file_page_idx(current_file, current_file_page_idx,
					in_file_page_idx);
		return current_file;
	}

	/* At this time, we are in a situation where we lost our location.
	 * then, find it directly */
	current_file = zicio_find_current_file_struct(desc,
				current_file_page_idx);
	if (current_file) {
		zicio_set_in_file_page_idx(current_file, current_file_page_idx,
					in_file_page_idx);
	}
	return current_file;
}
