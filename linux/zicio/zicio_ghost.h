/* SPDX-License-Identifier: GPL-2.0 */
#ifdef CONFIG_ZICIO

#ifndef ZICIO_GHOST_H
#define ZICIO_GHOST_H

#include <linux/limits.h>
#include <linux/types.h>
#include <linux/zicio_notify.h>
#include <asm/atomic.h>

#define INVALID_FILE_CHUNK_ID (-1)

#define SET_PREMAPPING_ITER(desc, val) \
	(atomic64_set(&desc->ghost.premapping_iter, val))
#define GET_PREMAPPING_ITER(desc) \
	(atomic64_read(&desc->ghost.premapping_iter))
#define GET_INC_PREMAPPING_ITER(desc) \
	(atomic64_fetch_add(1, &desc->ghost.premapping_iter))
#define INCREMENT_PREMAPPING_ITER(desc) \
	(atomic64_inc(&desc->ghost.premapping_iter))
#define DECREMENT_PREMAPPING_ITER(desc) \
	(atomic64_dec(&desc->ghost.premapping_iter))

#define SET_UNMAPPING_ITER(desc, val) \
	(atomic64_set(&desc->ghost.unmapping_iter, val))
#define GET_UNMAPPING_ITER(desc) \
	(atomic64_read(&desc->ghost.unmapping_iter))
#define INCREMENT_UNMAPPING_ITER(desc) \
	(atomic64_inc(&desc->ghost.unmapping_iter))

struct page *zicio_allocate_hugepage(void);
void zicio_free_hugepage(struct page *page);

int zicio_ghost_init(zicio_descriptor *desc, 
				unsigned long user_base_address);
void zicio_ghost_close(zicio_descriptor *desc);

int zicio_ghost_mapping_hugepage(zicio_descriptor *desc, 
				unsigned long entry_idx, struct page* page);
void *zicio_ghost_unmapping_hugepage(zicio_descriptor *desc,
				int unmappable_entry_idx);
void zicio_unmap_pages_from_local_page_table(zicio_descriptor *desc);

void *zicio_ghost_unmap(zicio_descriptor *desc, bool forceful);
int zicio_ghost_premap(zicio_descriptor *desc,
			zicio_shared_page_control_block *zicio_spcb, void *k_chunk_addr,
			unsigned int premap_high_watermark, int distance_idx);
unsigned zicio_ghost_forceful_unmap(zicio_descriptor *desc);
void zicio_swap_ghost_entries(zicio_descriptor *desc, 
			unsigned long entry_idx_1, unsigned long entry_idx_2);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
void zicio_dump_premap_unmap_iter(zicio_descriptor *desc);
#endif

/*
 * zicio_get_user_buffer_idx - get current user ingestion point(index)
 */
static inline int zicio_get_user_buffer_idx(zicio_descriptor *desc)
{
	zicio_switch_board *sb = desc->switch_board;

	return atomic_read((atomic_t *) &sb->user_buffer_idx); 
}

/*
 * last_premapped_idx - get last premapped index
 * @desc: zicio descriptor
 *
 * Return INT_MAX, if there are no premapped chunks.
 * Return index, last premapped index
 */
static inline int last_premapped_idx(zicio_descriptor *desc)
{
	unsigned long current_premapping_iter = GET_PREMAPPING_ITER(desc);
	if (current_premapping_iter == 0)
		return INT_MAX;

	return (int) (current_premapping_iter - 1) % ZICIO_MAX_NUM_GHOST_ENTRY;
}
 
#endif /* ZICIO_GHOST_H */
#endif /* CONFIG_ZICIO */
