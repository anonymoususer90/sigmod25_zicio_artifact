#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/zicio_notify.h>
#include <linux/bits.h>
#include <linux/nospec.h>
#include <linux/stat.h>

#include "zicio_desc.h"
#include "zicio_extent.h"
#include "zicio_req_submit.h"

static zicio_id_allocator zicio_descs;

/*
 * zicio_lookup_zicio_struct_raw
 *
 * The caller must ensure that id table isn't shared or hold rcu or file lock
 */
static inline void *
zicio_lookup_zicio_struct_raw(zicio_id_allocator *zicio_id_allocator,
			unsigned int id)
{
	zicio_id_table *zicio_id_table =
			rcu_dereference_raw(zicio_id_allocator->id_table);
	unsigned int s_id;

	if (id < zicio_id_table->max_ids) {
		s_id = array_index_nospec(id, zicio_id_table->max_ids);
		return rcu_dereference_raw(zicio_id_table->zicio_struct[s_id]);
	}
	return NULL;
}

/*
 * zicio_lookup_id_rcu
 *
 * Before accessing the id array and acquiring the zicio_struct, check if the rcu
 * lock is held.
 */
static inline void*
zicio_lookup_id_rcu(zicio_id_allocator *zicio_id_allocator,
			unsigned int id)
{
	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
				"suspicious rcu_deference_check() usage");
	return zicio_lookup_zicio_struct_raw(zicio_id_allocator, id);
}

/*
 * __zicio_get_zicio_struct
 *
 * Hold a read lock and use the ID to get an id from the id allocator.
 */
static inline void*
__zicio_get_zicio_struct(zicio_id_allocator *zicio_id_allocator,
			unsigned int id, unsigned int refs)
{
	void *zicio_struct;

	rcu_read_lock();

	zicio_struct = zicio_lookup_id_rcu(zicio_id_allocator, id);

	rcu_read_unlock();

	return zicio_struct;
}

static inline void *
__zicio_get_zicio_struct_from_id(zicio_id_allocator *zicio_id_allocator,
			unsigned int id, bool is_scan)
{
	void *zicio_struct;

	if (atomic_read(&zicio_id_allocator->count) == 1 || is_scan) {
		zicio_struct = zicio_lookup_zicio_struct_raw(zicio_id_allocator, id);
	} else {
		zicio_struct = __zicio_get_zicio_struct(zicio_id_allocator, id, 1);
	}

	return zicio_struct;
}

static inline void *
zicio_get_zicio_struct_from_id(zicio_id_allocator *zicio_id_allocator,
			unsigned int id, bool is_scan)
{
	void *zicio_struct=
			(void *)((unsigned long)__zicio_get_zicio_struct_from_id(
						zicio_id_allocator, id, is_scan));

	if (!zicio_struct && !is_scan) {
		printk(KERN_WARNING "[Kernel Message] cannot found sf struct\n");
	}

	return zicio_struct;
}

/*
 * zicio_get_zicio_struct
 *
 * Find zicio_struct from zicio_id_allocator using ID.
 */
void *
zicio_get_zicio_struct(zicio_id_allocator *zicio_id_allocator,
			unsigned int id, bool is_scan)
{
	return zicio_get_zicio_struct_from_id(zicio_id_allocator, id, is_scan);
}

zicio_descriptor *
zicio_get_desc(unsigned int id)
{
	return zicio_get_zicio_struct(&zicio_descs, id, false);
}

static inline void
__zicio_set_open_id(unsigned int id, zicio_id_table *zicio_id_table)
{
	__set_bit(id, zicio_id_table->open_ids);
	id /= BITS_PER_LONG;
	if (!~zicio_id_table->open_ids[id])
		__set_bit(id, zicio_id_table->full_ids_bits);
}

static void
__zicio_free_idtable(zicio_id_table *zicio_id_table)
{
	kvfree(zicio_id_table->zicio_struct);
	kvfree(zicio_id_table->open_ids);
	kfree(zicio_id_table);
}

static void
zicio_free_idtable_rcu(struct rcu_head *rcu)
{
	__zicio_free_idtable(container_of(rcu,
				struct zicio_id_table, rcu));
}

/*
 * zicio_find_next_id
 *
 * find next id using bitmap
 */
static unsigned int
zicio_find_next_id(zicio_id_table *zicio_id_table, unsigned int start)
{
	unsigned int maxid = zicio_id_table->max_ids;
	unsigned int maxbit = maxid / BITS_PER_LONG;
	unsigned int bitbit = start / BITS_PER_LONG;

	/* Compute the first seek position in the bitmap. */
	bitbit = find_next_zero_bit(zicio_id_table->full_ids_bits, maxbit, bitbit)
				* BITS_PER_LONG;

	if (bitbit > maxid) {
		return maxid;
	}

	if (bitbit > start) {
		start = bitbit;
	}

	/* Gets the empty bit position in the bitmap and returns the index to it. */
	return find_next_zero_bit(zicio_id_table->open_ids, maxid, start);
}

/*
 * zicio_alloc_idtable
 *
 * Allocate zicio id table. If the id table of the id allocator needs to be
 * extended, a table with a larger size needs to be allocated.
 */
static zicio_id_table *
zicio_alloc_idtable(unsigned int nr)
{
	zicio_id_table *id_table;
	void *data;

	/*
	 * Figure out how many zicio_struct  we actually want to support in this
	 * idtable. Allocation steps are keyed to the size of the sf struct array
	 * (zicio_struct_array), since it grows far faster than any of the other
	 * dynamic data. We try to fit the zicio_struct_array into comfortable
	 * page-tuned chunks: starting at 1024B and growing in powers of two from
	 * there on.
	 */
	nr /= (1024 / sizeof(void *));
	nr = roundup_pow_of_two(nr + 1);
	nr *= (1024 / sizeof(void *));

	if (unlikely(nr > ZICIO_MAX_NUM_ALLOC))
		nr = ((ZICIO_MAX_NUM_ALLOC - 1) | (BITS_PER_LONG - 1)) + 1;

	id_table = kmalloc(sizeof(struct zicio_id_table), GFP_KERNEL);

	if (unlikely(!id_table))
		goto l_zicio_alloc_idtable_out;
	id_table->max_ids = nr;
	data = kvmalloc_array(nr, sizeof(void *), GFP_KERNEL);

	if (!data)
		goto l_zicio_alloc_idtable_out_sdt;

	id_table->zicio_struct = data;

	data = kvmalloc(max_t(size_t,
				2 * nr / BITS_PER_BYTE + ZICIO_BITBIT_SIZE(nr),
				L1_CACHE_BYTES), GFP_KERNEL);

	if (!data)
		goto l_zicio_alloc_idtable_out_arr;
	id_table->open_ids = data;
	data += nr / BITS_PER_BYTE;
	id_table->close_on_exec = data;
	data += nr / BITS_PER_BYTE;
	id_table->full_ids_bits = data;

	return id_table;

l_zicio_alloc_idtable_out_arr:
	kfree(id_table->zicio_struct);
l_zicio_alloc_idtable_out_sdt:
	kfree(id_table);
l_zicio_alloc_idtable_out:
	return NULL;
}

/*
 * zicio_copy_id_bitmaps
 *
 * Copy 'count' id bits from the old table to the new table
 * and clear the extra space if any. This does not copy the
 * zicio_id_allocator. Called with the files spinlock held for write.
 */
static void 
zicio_copy_id_bitmaps(zicio_id_table *new_idtable,
			zicio_id_table *old_idtable, unsigned int count)
{
	unsigned int cpy, set;
	cpy = count / BITS_PER_BYTE;
	set = (new_idtable->max_ids - count) / BITS_PER_BYTE;

	memcpy(new_idtable->open_ids, old_idtable->open_ids, cpy);
	memset((char *)new_idtable->open_ids + cpy, 0, set);
	memcpy(new_idtable->close_on_exec, old_idtable->close_on_exec, cpy);
	memset((char *)new_idtable->close_on_exec + cpy, 0, set);

	cpy = ZICIO_BITBIT_SIZE(count);
	set = ZICIO_BITBIT_SIZE(new_idtable->max_ids) - cpy;
	memcpy(new_idtable->full_ids_bits, old_idtable->full_ids_bits, cpy);
	memset((char *)new_idtable->full_ids_bits + cpy, 0, set);
}

/*
 * zicio_copy_idtable
 *
 * Copy idtable
 */
static void 
zicio_copy_idtable(zicio_id_table *new_idtable,
			zicio_id_table *old_idtable)
{
	size_t cpy, set;

	cpy = old_idtable->max_ids * sizeof(void *);
	set = (new_idtable->max_ids - old_idtable->max_ids) * sizeof(void *);
	memcpy(new_idtable->zicio_struct, old_idtable->zicio_struct, cpy);
	memset((char*)new_idtable->zicio_struct + cpy, 0, set);

	/* Copy bitmap */
	zicio_copy_id_bitmaps(new_idtable, old_idtable, old_idtable->max_ids);
}

/*
 * zicio_expand_idtable
 *
 * Expand ID table
 */
static int
zicio_expand_idtable(zicio_id_allocator *zicio_id_allocator,
			unsigned int nr)
	__releases(zicio_id_allocator->zicio_id_allocator_lock)
	__acquires(zicio_id_allocator->zicio_id_allocator_lock)
{
	zicio_id_table *new_idtable, *current_idtable;

	spin_unlock(&zicio_id_allocator->zicio_id_allocator_lock);
	new_idtable = zicio_alloc_idtable(nr);

	if (atomic_read(&zicio_id_allocator->count) > 1)
		synchronize_rcu();

	spin_lock(&zicio_id_allocator->zicio_id_allocator_lock);
	if (!new_idtable)
		return -ENOMEM;

	current_idtable = zicio_idtable(zicio_id_allocator);
	BUG_ON(nr < current_idtable->max_ids);
	zicio_copy_idtable(new_idtable, current_idtable);
	rcu_assign_pointer(zicio_id_allocator->id_table, new_idtable);

	/* If we need to free array and table */
	if (current_idtable != &zicio_id_allocator->id_table_init) {
		call_rcu(&current_idtable->rcu, zicio_free_idtable_rcu);
	}

	smp_wmb();
	return 1;
}

/* 
 * zicio_expand_id_allocator
 *
 * If table expansion is needed, then expand the table and return 1.
 */
static int
zicio_expand_id_allocator(zicio_id_allocator *zicio_id_allocator,
			unsigned int nr)
	__releases(zicio_id_allocator->zicio_id_allocator_lock)
	__acquires(zicio_id_allocator->zicio_id_allocator_lock)
{
	zicio_id_table *zicio_id_table;
	int expanded = 0;

l_zicio_expand_descs_repeat:
	zicio_id_table = zicio_idtable(zicio_id_allocator);

	/* Do we need to expand? */
	if (nr < zicio_id_table->max_ids) {
		return expanded;
	}

	/* Can we expand? */
	if (nr >= ZICIO_MAX_NUM_ALLOC) {
		return -EMFILE;
	}

	if (unlikely(zicio_id_allocator->resize_in_progress)) {
		spin_unlock(&zicio_id_allocator->zicio_id_allocator_lock);
		expanded = 1;
		wait_event(zicio_id_allocator->resize_wait,
					!zicio_id_allocator->resize_in_progress);
		spin_lock(&zicio_id_allocator->zicio_id_allocator_lock);
		goto l_zicio_expand_descs_repeat;
	}

	/* All good, so we try */
	zicio_id_allocator->resize_in_progress = true;
	expanded = zicio_expand_idtable(zicio_id_allocator, nr);
	zicio_id_allocator->resize_in_progress = false;

	wake_up_all(&zicio_id_allocator->resize_wait);
	return expanded;
}

/*
 * zicio_alloc_id
 * Allocate ID from id allocator
 *
 * @zicio_id_allocator: zicio ID allocator
 * @start: start point for unused ID
 * @end: maximum value which can be allocated for zicio desc ID
 */
static int 
zicio_alloc_id(zicio_id_allocator *zicio_id_allocator,
		unsigned int start, unsigned int end)
{
	zicio_id_table *zicio_id_table;
	unsigned int id;
	int error;
	
	spin_lock(&zicio_id_allocator->zicio_id_allocator_lock);
l_zicio_alloc_desc_repeat:
	zicio_id_table = zicio_idtable(zicio_id_allocator);
	id = start;

	/* If we can set next_id directly, then set it */
	if (id < zicio_id_allocator->next_id) {
		id = zicio_id_allocator->next_id;
	}

	/* Find next id using bitmap */
	if (id < zicio_id_table->max_ids) {
		id = zicio_find_next_id(zicio_id_table, id);
	}

	error = -EMFILE;

	/* If allocated id is larger than the maximum value which can be allocted
       then, return error */
	if (id >= end) {
		goto l_zicio_alloc_desc_out;
	}

	error = zicio_expand_id_allocator(zicio_id_allocator, id);

	if (error < 0)
		goto l_zicio_alloc_desc_out;

	/* 
	 * If we needed to expand the fs array we
	 * might have blocked - try again.
	 */
	if (error)
		goto l_zicio_alloc_desc_repeat;

	if (start <= zicio_id_allocator->next_id)
		zicio_id_allocator->next_id = id + 1;

	/* Set bitmap for allocated id */
	__zicio_set_open_id(id, zicio_id_table);

	error = id;

l_zicio_alloc_desc_out:
	spin_unlock(&zicio_id_allocator->zicio_id_allocator_lock);
	return error;
}

static int
__zicio_get_unused_id(zicio_id_allocator *zicio_id_allocator,
		unsigned int start, unsigned int end)
{
	return zicio_alloc_id(zicio_id_allocator, start, end);
}

/*
 * zicio_get_unused_id
 *
 * Function used to get an id from ID allocator
 * Finds an id that can be assigned from the allocator received as a parameter
 * and returns it.
 */
int
zicio_get_unused_id(zicio_id_allocator *zicio_id_allocator)
{
	return __zicio_get_unused_id(zicio_id_allocator, 0,
				ZICIO_MAX_NUM_ALLOC);
}

int
zicio_get_unused_desc_id(void)
{
	return zicio_get_unused_id(&zicio_descs);
}

/*
 * zicio_install_zicio_struct
 *
 * Install a zicio_struct pointer in the zicio_struct array
 */
void
zicio_install_zicio_struct(zicio_id_allocator *zicio_id_allocator,
			unsigned int id, void *zicio_struct)
{
	zicio_id_table *zicio_id_table;

	rcu_read_lock_sched();

	if (unlikely(zicio_id_allocator->resize_in_progress)) {
		rcu_read_unlock_sched();
		spin_lock(&zicio_id_allocator->zicio_id_allocator_lock);
		zicio_id_table = zicio_idtable(zicio_id_allocator);
		rcu_assign_pointer(zicio_id_table->zicio_struct[id], zicio_struct);
		spin_unlock(&zicio_id_allocator->zicio_id_allocator_lock);

		return;
	}
	/* coupled with smp_wmb() in zicio_expend_idtable */
	smp_rmb();
	zicio_id_table = rcu_dereference_sched(zicio_id_allocator->id_table);
	BUG_ON(zicio_id_table->zicio_struct[id] != NULL);
	rcu_assign_pointer(zicio_id_table->zicio_struct[id], zicio_struct);
	rcu_read_unlock_sched();
}

/*
 * install zicio descriptor to sdtable
 */
void
zicio_install_desc(unsigned int id, zicio_descriptor *desc)
{
	zicio_install_zicio_struct(&zicio_descs, id, desc);
}

static inline void 
__zicio_clear_open_id(unsigned int id, zicio_id_table *zicio_id_table)
{
	__clear_bit(id, zicio_id_table->open_ids);
	__clear_bit(id / BITS_PER_LONG, zicio_id_table->full_ids_bits);
}

static void
__zicio_put_unused_id(zicio_id_allocator *zicio_id_allocator,
			unsigned int id)
{
	zicio_id_table *zicio_id_table = zicio_idtable(zicio_id_allocator);
	__zicio_clear_open_id(id, zicio_id_table);
	if (id < zicio_id_allocator->next_id)
		zicio_id_allocator->next_id = id;
}

void
zicio_put_unused_id(zicio_id_allocator *zicio_id_allocator,
		unsigned int id)
{
	spin_lock(&zicio_id_allocator->zicio_id_allocator_lock);
	__zicio_put_unused_id(zicio_id_allocator, id);
	spin_unlock(&zicio_id_allocator->zicio_id_allocator_lock);
}

void
zicio_put_unused_desc_id(unsigned int id)
{
	zicio_put_unused_id(&zicio_descs, id);
}

/*
 * zicio_pick_id - return zicio struct matched with id from id allocator
 *					   and remove the corresponding id from the id allocator.
 * @zicio_id_allocator: id allocator to pick zicio struct
 * @id: zicio desc id to retrieve zicio struct for
 */
void *
zicio_pick_id(zicio_id_allocator *zicio_id_allocator, unsigned int id)
{
	zicio_id_table *zicio_id_table;
	void *zicio_struct;

	spin_lock(&zicio_id_allocator->zicio_id_allocator_lock);
	zicio_id_table = zicio_idtable(zicio_id_allocator);

	if (id >= zicio_id_table->max_ids) {
		zicio_struct  = ERR_PTR(-EINVAL);
		goto l_zicio_pick_id_out_unlock;
	}

	zicio_struct = zicio_id_table->zicio_struct[id];
	if (!zicio_struct) {
		zicio_struct = ERR_PTR(-EINVAL);
		goto l_zicio_pick_id_out_unlock;
	}

	/* Set clear */
	rcu_assign_pointer(zicio_id_table->zicio_struct[id], NULL);
	__zicio_put_unused_id(zicio_id_allocator, id);

l_zicio_pick_id_out_unlock:
	spin_unlock(&zicio_id_allocator->zicio_id_allocator_lock);
	/* Return cleaned zicio descriptor */
	return zicio_struct;
}

/*
 * zicio_iterate_all_zicio_struct - iterate all zicio struct
 * @zicio_id_allocator: ID allocator to delete all of managed zicio struct
 * @zicio_id_iterate_callback: callback function
 * @remove: is deletion needed?
 */
long
zicio_iterate_all_zicio_struct(zicio_id_allocator *zicio_id_allocator,
		void(*zicio_id_iterate_callback) (void *zicio_struct), bool remove)
{
	zicio_id_table *zicio_id_table;
	void *zicio_struct;
	unsigned int zicio_id_idx = 0, max_ids;

	rcu_read_lock();

	zicio_id_table = zicio_idtable(zicio_id_allocator);
	max_ids = zicio_id_table->max_ids;

	while (zicio_id_idx < max_ids) {
		if (remove) {
			zicio_struct = zicio_pick_id(zicio_id_allocator, zicio_id_idx);
		} else {
			zicio_struct = zicio_get_zicio_struct(zicio_id_allocator, zicio_id_idx,
						true);
		}

		/* zicio_struct matched with zicio_id_idx is not allocated. Continue loops. */
		if (IS_ERR_OR_NULL(zicio_struct)) {
			zicio_id_idx++;
			continue;
		}

		zicio_id_iterate_callback(zicio_struct);
		zicio_id_idx++;
	}
	rcu_read_unlock();

	return 0;
}

static void
zicio_iterate_range_zicio_struct_with_ret(
		zicio_id_allocator *zicio_id_allocator,
		void(*zicio_id_iterate_callback) (void *zicio_struct, void *ret),
		void *zicio_ret, int break_num, bool remove)
{
	zicio_id_table *zicio_id_table;
	void *zicio_struct;
	unsigned int zicio_id_idx = 0, max_ids, nr_structs = 0;

	rcu_read_lock();

	zicio_id_table = zicio_idtable(zicio_id_allocator);
	max_ids = zicio_id_table->max_ids;

	while (zicio_id_idx < max_ids) {
		if (remove) {
			zicio_struct = zicio_pick_id(zicio_id_allocator, zicio_id_idx);
		} else {
			zicio_struct = zicio_get_zicio_struct(zicio_id_allocator, zicio_id_idx,
						true);
		}

		/* zicio_struct matched with zicio_id_idx is not allocated. Continue loops. */
		if (IS_ERR_OR_NULL(zicio_struct)) {
			zicio_id_idx++;
			continue;
		}
		nr_structs++;
		zicio_id_idx++;

		zicio_id_iterate_callback(zicio_struct, zicio_ret);

		if (nr_structs == break_num) {
			break;
		}
	}
	rcu_read_unlock();
}

static void
__zicio_iterate_all_zicio_struct_with_ret(
		zicio_id_allocator *zicio_id_allocator,
		void(*zicio_id_iterate_callback) (void *zicio_struct, void *ret),
		void *zicio_ret, bool remove)
{
	zicio_id_table *zicio_id_table;
	void *zicio_struct;
	unsigned int zicio_id_idx = 0, max_ids;

	rcu_read_lock();

	zicio_id_table = zicio_idtable(zicio_id_allocator);
	max_ids = zicio_id_table->max_ids;

	while (zicio_id_idx < max_ids) {
		if (remove) {
			zicio_struct = zicio_pick_id(zicio_id_allocator, zicio_id_idx);
		} else {
			zicio_struct = zicio_get_zicio_struct(zicio_id_allocator, zicio_id_idx,
						true);
		}

		/* zicio_struct matched with zicio_id_idx is not allocated. Continue loops. */
		if (IS_ERR_OR_NULL(zicio_struct)) {
			zicio_id_idx++;
			continue;
		}

		zicio_id_iterate_callback(zicio_struct, zicio_ret);
		zicio_id_idx++;
	}
	rcu_read_unlock();
}

void
zicio_iterate_all_zicio_struct_with_ret(
		zicio_id_allocator *zicio_id_allocator,
		void(*zicio_id_iterate_callback) (void *zicio_struct, void *ret),
		void *zicio_ret, int break_num, bool remove)
{
	if (break_num) {
		zicio_iterate_range_zicio_struct_with_ret(zicio_id_allocator,
			zicio_id_iterate_callback, zicio_ret, break_num, remove);
	} else {
		__zicio_iterate_all_zicio_struct_with_ret(zicio_id_allocator,
				zicio_id_iterate_callback, zicio_ret, remove);
	}
}

void *
zicio_iterate_zicio_struct_to_dest(zicio_id_allocator *zicio_id_allocator,
		zicio_id_iterator *zicio_id_iter)
{
	zicio_id_table *zicio_id_table;
	void *zicio_struct;

	if (zicio_id_iter->curr >= zicio_id_iter->dest) {
		return ERR_PTR(-1);
	}

	rcu_read_lock();

	zicio_id_table = zicio_idtable(zicio_id_allocator);

	if (zicio_id_iter->curr >= zicio_id_table->max_ids) {
		return ERR_PTR(-1);
	}

	zicio_struct = zicio_get_zicio_struct(zicio_id_allocator, zicio_id_iter->curr,
			true);

	rcu_read_unlock();

	zicio_id_iter->curr++;

	return zicio_struct;
}

int
zicio_find_id(zicio_id_allocator *zicio_id_allocator,
		bool(*zicio_find_callback) (void *zicio_struct, void *args),
		void *zicio_args)
{
	zicio_id_table *zicio_id_table;
	void *zicio_struct;
	int ret = INT_MAX;
	unsigned int zicio_id_idx = 0, max_ids;

	rcu_read_lock();
	zicio_id_table = zicio_idtable(zicio_id_allocator);
	max_ids = zicio_id_table->max_ids;

	while (zicio_id_idx < max_ids) {
		zicio_struct = zicio_get_zicio_struct(zicio_id_allocator, zicio_id_idx, true);

		if (IS_ERR_OR_NULL(zicio_struct)) {
			zicio_id_idx++;
			continue;
		}

		if (zicio_find_callback(zicio_struct, zicio_args)) {
			ret = zicio_id_idx;
			break;
		}

		zicio_id_idx++;
	}
	rcu_read_unlock();

	return ret;
}

/*
 * zicio_get_num_allocated_struct - get the number of allocated struct in
 * table.
 * @zicio_id_allocator: ID allocator to checking allocated numbers
 */
long
zicio_get_num_allocated_struct(zicio_id_allocator *zicio_id_allocator)
{
	zicio_id_table *zicio_id_table;
	void *zicio_struct;
	unsigned int zicio_id_idx = 0;
	unsigned int max_ids;
	long num_struct = 0;

	rcu_read_lock();

	zicio_id_table = zicio_idtable(zicio_id_allocator);
	max_ids = zicio_id_table->max_ids;

	while (zicio_id_idx < max_ids) {
		zicio_struct = zicio_get_zicio_struct(zicio_id_allocator, zicio_id_idx, true);

		/* zicio_struct matched with zicio_id_idx is not allocated. Continue loops. */
		if (IS_ERR_OR_NULL(zicio_struct)) {
			zicio_id_idx++;
			continue;
		}
		zicio_id_idx++;
		num_struct++;
	}
	rcu_read_unlock();

	return num_struct;
}

/*
 * zicio_pick_desc - return zicio descriptor with id
 * @sds: zicio struct to retrieve file from
 * @id: zicio desc id to retrieve zicio desc for
 */
zicio_descriptor *
zicio_pick_desc(unsigned int id)
{
	return zicio_pick_id(&zicio_descs, id);
}

/*
 * zicio_delete_idtable
 *
 * Delete ID table
 */
int
zicio_delete_idtable(zicio_id_allocator *zicio_id_allocator)
	__acquires(zicio_id_allocator->zicio_id_allocator_lock)
	__releases(zicio_id_allocator->zicio_id_allocator_lock)
{
	zicio_id_table *delete_idtable;

	if (atomic_read(&zicio_id_allocator->count) > 1)
		synchronize_rcu();

	spin_lock(&zicio_id_allocator->zicio_id_allocator_lock);

	delete_idtable = zicio_idtable(zicio_id_allocator);
	rcu_assign_pointer(zicio_id_allocator->id_table, NULL);

	/* If we need to free array and table */
	if (delete_idtable != &zicio_id_allocator->id_table_init) {
		call_rcu(&delete_idtable->rcu, zicio_free_idtable_rcu);
	}

	spin_unlock(&zicio_id_allocator->zicio_id_allocator_lock);

	return 0;
}

int
zicio_get_max_ids_from_idtable(zicio_id_allocator *zicio_id_allocator)
{
	zicio_id_table *zicio_id_table =
			rcu_dereference_raw(zicio_id_allocator->id_table);

	BUG_ON(!zicio_id_table);
	return zicio_id_table->max_ids;
}

/*
 * zicio_init_id_allocator
 *
 * Initialize id table structure
 */
void
zicio_init_id_allocator(zicio_id_allocator *zicio_id_allocator)
{
	spin_lock_init(&zicio_id_allocator->zicio_id_allocator_lock);

	/* Id array start from 1 */
	zicio_id_allocator->next_id = 1;

	zicio_id_allocator->id_table = &zicio_id_allocator->id_table_init;

	zicio_id_allocator->id_table_init.max_ids = NR_ZICIO_OPEN_DEFAULT;
	zicio_id_allocator->id_table_init.zicio_struct =
				&zicio_id_allocator->zicio_struct_array[0];

	zicio_id_allocator->id_table_init.close_on_exec =
				zicio_id_allocator->close_on_exec_init;
	zicio_id_allocator->id_table_init.open_ids =
				zicio_id_allocator->open_ids_init;
	zicio_id_allocator->id_table_init.full_ids_bits =
				zicio_id_allocator->full_ids_bits_init;

	rcu_head_init(&zicio_id_allocator->id_table_init.rcu);
	spin_lock_init(&zicio_id_allocator->resize_wait.lock);
	INIT_LIST_HEAD(&zicio_id_allocator->resize_wait.head);
}

/*
 * zicio_init_desc_allocator
 *
 * Initialize zicio global descriptor allocator
 */ 
void __init zicio_init_desc_allocator(void)
{
	zicio_init_id_allocator(&zicio_descs);
}
