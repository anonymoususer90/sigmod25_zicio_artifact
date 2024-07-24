#ifndef __ZICIO_DESC_H
#define __ZICIO_DESC_H

#include <linux/buffer_head.h>
#include <linux/types.h>
#include <linux/zicio_notify.h>

/*
 * The data structure and logic for zicIO's descriptor id is based on linux's
 * fd.
 *
 * Reference to: include/linux/fdtable.h, fs/file.c
 */

typedef struct zicio_id_iterator {
	unsigned int curr;
	unsigned int dest;
} zicio_id_iterator;

void zicio_init_id_allocator(zicio_id_allocator *zicio_id_allocator);
int zicio_get_unused_id(zicio_id_allocator *zicio_id_allocator);
void zicio_put_unused_id(zicio_id_allocator *zicio_id_allocator,
			unsigned int id);
void *zicio_pick_id(zicio_id_allocator *zicio_id_allocator,
			unsigned int id);
void *zicio_get_zicio_struct(zicio_id_allocator *zicio_id_allocator,
			unsigned int id, bool is_scan);
void zicio_install_zicio_struct(zicio_id_allocator *zicio_id_allocator,
			unsigned int id, void *zicio_struct);
void *zicio_iterate_zicio_struct_to_dest(
			zicio_id_allocator *zicio_id_allocator,
			zicio_id_iterator *zicio_id_iter);
int zicio_delete_idtable(zicio_id_allocator *zicio_id_allocator);
long zicio_iterate_all_zicio_struct(zicio_id_allocator *zicio_id_allocator,
			void(*zicio_id_iterate_callback) (void *zicio_struct),
			bool remove);
void zicio_iterate_all_zicio_struct_with_ret(
		zicio_id_allocator *zicio_id_allocator,
		void(*zicio_id_iterate_callback) (void *zicio_struct, void *ret),
		void *zicioret, int break_num, bool remove);
int zicio_find_id(zicio_id_allocator *zicio_id_allocator,
			bool(*zicio_find_callback) (void *zicio_struct, void *args),
			void *zicioargs);
long zicio_get_num_allocated_struct(
			zicio_id_allocator *zicio_id_allocator);
int zicio_get_max_ids_from_idtable(zicio_id_allocator *zicio_id_allocator);

void __init zicio_init_desc_allocator(void);

int zicio_get_unused_desc_id(void);
void zicio_put_unused_desc_id(unsigned int id);
zicio_descriptor *zicio_pick_desc(unsigned int id);
zicio_descriptor *zicio_get_desc(unsigned int id);
void zicio_install_desc(unsigned int id, zicio_descriptor *desc);

#define zicio_rcu_dereference_check_idtable(zicio_id_allocator, zicio_idtable) \
		rcu_dereference_check((zicio_idtable), \
				lockdep_is_held(&(zicio_id_allocator)->zicio_id_allocator_lock))

#define zicio_idtable(zicio_id_allocator) \
		zicio_rcu_dereference_check_idtable((zicio_id_allocator), \
				(zicio_id_allocator)->id_table)

#define get_desc_rcu_many(x, cnt) \
	atomic_add_unless(&(x)->count, (cnt), 0)

#endif /* ZICIO_EXTENT_H */
