#ifndef __ZICIO_FILES_H
#define __ZICIO_FILES_H

#include <linux/zicio_notify.h>
#include <uapi/linux/zicio.h>

typedef struct zicio_shared_pool zicio_shared_pool;
typedef struct zicio_shared_files zicio_shared_files;

zicio_file_struct *zicio_get_current_file_struct(
			zicio_descriptor *desc);
zicio_file_struct * zicio_get_current_file_struct_shared(
			zicio_descriptor *desc, unsigned long file_chunk_id,
			bool set_file, bool recheck);
unsigned int * zicio_copy_fds_from_user(
			unsigned int __user *fds, int nr_fd);
struct fd* zicio_get_fdstruct(unsigned int *fds, int nr_fd);
long zicio_initialize_fds_to_desc(zicio_descriptor *desc, struct fd* fs,
			unsigned int *fds, int *file_dev_map, int nr_fd);
struct device **zicio_get_distinct_file_dev_set(int **file_dev_map,
			struct fd* fs, int nr_fd);
long zicio_initialize_read_files(zicio_read_files *add_files,
			struct fd* fs, unsigned int *fds, int *file_dev_map, int nr_fd);
unsigned long zicio_get_total_filesize(zicio_shared_files *shared_files);
void zicio_free_read_files(zicio_read_files *add_files);
zicio_file_struct* zicio_get_next_file_struct(
			zicio_read_files *zicio_rfile);
zicio_file_struct* zicio_get_next_unready_file_struct(
			zicio_read_files *zicio_rfile);
zicio_file_struct* zicio_get_id_file_struct(
			zicio_read_files *zicio_rfile, unsigned int id);
zicio_file_struct* zicio_get_file_struct_for_cmd(
			zicio_descriptor *desc, unsigned long current_file_page_idx,
			unsigned int *in_file_page_idx);

/*
 * zicio_distinct_nr_dev
 *
 * Get the number of device in a distinct set
 */
static inline unsigned
zicio_distinct_nr_dev(struct device **devs, unsigned nr_dev) {
	unsigned i;

	for (i = 0 ; i < nr_dev ; i++) {
		if (!devs[i]) {
			return i;
		}
	}
	return nr_dev;
}

/*
 * Functions for local channels. Issues the chunk id to dispatch the next
 * request.
 */
static inline unsigned long
zicio_get_current_file_page_idx(zicio_read_files *zicio_rfile)
{
	unsigned long current_file_page_idx = atomic64_fetch_add(
				1UL << ZICIO_PAGE_TO_CHUNK_SHIFT,
				&zicio_rfile->total_requested_size);

	if (current_file_page_idx >= zicio_rfile->total_file_size) {
		return ULONG_MAX;
	}
	return current_file_page_idx;
}

/*
 * zicio_check_current_file_page_idx
 *
 * Checks whether the file chunk id received as a parameter belongs to the
 * range of the file received as a parameter.
 */
static inline bool
zicio_check_current_file_page_idx(zicio_file_struct *zicio_file_struct,
			unsigned long current_file_page_idx)
{
	return (zicio_file_struct->start_page_idx_in_set <= current_file_page_idx &&
			zicio_file_struct->start_page_idx_in_set + zicio_file_struct->file_size >
			current_file_page_idx);
}

/*
 * zicio_set_file_cursor
 *
 * Set file cursor to file_id
 */
static inline void
zicio_set_file_cursor(zicio_descriptor *desc, unsigned int file_id)
{
	BUG_ON(file_id >= desc->read_files.num_fds);

	atomic_set(&desc->read_files.fd_cursor, file_id);
}

static inline unsigned int
zicio_get_in_file_page_idx(zicio_file_struct *current_file,
		unsigned long current_file_page_idx)
{
	return current_file_page_idx - current_file->start_page_idx_in_set;
}

static inline bool
zicio_check_all_extent_consumed(zicio_file_struct *zicio_file)
{
	return (zicio_file->extent_tree_meter.consumed_no_mod ==
			zicio_file->extent_tree_meter.produced_no_mod);
}

#define __get_zicio_file_array(conds, dest, src, nr, func)				\
	do {			\
		if (conds)			\
			dest = func(src, nr);			\
	} while (0)

#define get_zicio_file_array(conds, dest, src, nr, func, ret, label)			\
	do {			\
		__get_zicio_file_array(conds, dest, src, nr, func);			\
		if (dest && IS_ERR(dest)) {			\
			ret = PTR_ERR(dest);			\
			goto label;			\
		}			\
	} while (0)

/* Get zicio file array for update */
#define zicio_rcu_dereference_check_files(read_files, sfile_array) \
	rcu_dereference_check((read_files->zicio_file_array), \
		lockdep_is_held(&(read_files)->read_file_guard))

#define zicio_get_files_from_rcu(read_files) \
	zicio_rcu_dereference_check_files((read_files), \
		(read_files)->zicio_file_array)

#endif /* ZICIO_TRIGGER_H */
