#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/printk.h>

#include <linux/zicio_notify.h>

#include "zicio_cmd.h"
#include "zicio_device.h"
#include "zicio_files.h"
#include "zicio_firehose_ctrl.h"
#include "zicio_md_flow_ctrl.h"
#include "zicio_mem.h"
#include "zicio_req_timer.h"
#include "zicio_atomic.h"
#include "zicio_shared_pool.h"
#include "zicio_ghost.h"

/**
 * @ZICIO_HUGE_PAGE_SIZE_SHIFT:
 *   This macro is used to calculate the size of each huge page.
 *
 * @ZICIO_NUM_NVME_PAGE_PER_HUGE_PAGE_SHIFT:
 *   This macro is used to calculate the number of NVMe page per huge page.
 *
 * @ZICIO_NUM_NVME_PAGE_PER_HUGE_PAGE:
 *   This macro represents the number of NVMe page per huge page.
 *
 * @ZICIO_MASK_MAX_NUM_LOCAL_HUGE_PAGE:
 *   This macro is used to calculate huge page id in local user buffer.
 *
 * @ZICIO_NEW_CHUNK_THRESHOLD:
 *   This macro returns threshold whether or not to request a new chunk based on
 *   the given chunk size (number of nvme pages).
 *
 */
#define ZICIO_HUGE_PAGE_SIZE_SHIFT \
	(ZICIO_CHUNK_ORDER + ZICIO_PAGE_SHIFT)
#define ZICIO_NUM_NVME_PAGE_PER_HUGE_PAGE_SHIFT \
	(ZICIO_HUGE_PAGE_SIZE_SHIFT - ZICIO_NVME_CTRL_PAGE_SHIFT)
#define ZICIO_NUM_NVME_PAGE_PER_HUGE_PAGE \
	(1UL << ZICIO_NUM_NVME_PAGE_PER_HUGE_PAGE_SHIFT)
#define ZICIO_MASK_MAX_NUM_LOCAL_HUGE_PAGE (ZICIO_MAX_NUM_CHUNK - 1)
#define ZICIO_NEW_CHUNK_THRESHOLD(nr_needed_pages) \
	(nr_needed_pages >> 2)

/**
 * zicio_calc_local_huge_page_idx - calculate local huge page idx
 * @counter: determines the local huge page id
 *
 * The kernel and libzicio use fetch_add() on the counter, and the value
 * determines the huge page location.
 *
 * Therefore, knowing the counter value, we can get the local huge page id
 * using modular operation.
 */
static inline int
zicio_calc_local_huge_page_idx(unsigned long counter)
{
	return (counter & ZICIO_MASK_MAX_NUM_LOCAL_HUGE_PAGE);
}

/**
 * zicio_nvme_length_to_page - translate NVMe command's length field
 * @len: NVMe command's length field value
 *
 * NVMe command has length field. This field indicates how many 512B blocks the
 * command is requesting.
 *
 * This function translate it to the number of NVMe pages. For example, if the
 * field has a value 7, it means that the command has requsted eight 512B. So we
 * can translate it one NVMe page.
 *
 * This implementation assumes that NVMe command is requested in NVMe page
 * units. That is, the remainder when length is divided by 8 must always be 7.
 * So this function made an assertion about it.
 */
static inline int
zicio_nvme_length_to_page(int len)
{
	BUG_ON((len & 7) != 7 || len == 0);
	return (len >> 3) + 1;
}

/**
 * zicio_set_needed_pages - set needed nvme pages of the given huge page
 * @zicio_desc: zicio descriptor
 * @huge_page_idx: local huge page id
 * @needed_pages: needed nvme pages of the given huge page
 *
 * Set how many nvme pages are needed for this huge page.
 */
void
zicio_set_needed_nvme_pages(struct zicio_descriptor *zicio_desc,
	int huge_page_idx, int needed_pages)
{
	struct zicio_firehose_ctrl *ctrl = &zicio_desc->firehose_ctrl;
	BUG_ON(needed_pages == 0);
	atomic_set(ctrl->needed_pages_per_local_huge_page + huge_page_idx,
		needed_pages);
}
EXPORT_SYMBOL(zicio_set_needed_nvme_pages);

/**
 * zicio_set_needed_pages_shared - set needed nvme pages of the given huge page
 * @zicio_desc: zicio descriptor
 * @huge_page_idx: local huge page id
 * @needed_pages: needed nvme pages of the given huge page
 *
 * Set how many nvme pages are needed for this huge page.
 */
static inline void
zicio_set_needed_nvme_pages_shared(struct zicio_descriptor *zicio_desc,
	int huge_page_idx, int needed_pages)
{
	struct zicio_shared_page_control_block *zicio_spcb =
			zicio_get_spcb_with_id(zicio_desc, huge_page_idx);
	BUG_ON(needed_pages == 0);
	atomic_set(&zicio_spcb->zicio_spcb.needed_pages_per_local_huge_page,
			needed_pages);
}

/**
 * zicio_fill_local_huge_page - fill nvme pages to corresponding huge page
 * @zicio_desc: zicio descriptor
 * @huge_page_idx: local huge page id
 * @nr_nvme_page: number of nvme page
 *
 * Add the number of nvme pages processed by this interrupt handler to the
 * corresponding counter of the huge page.
 *
 * Return the previous value.
 */
static inline long
zicio_fill_local_huge_page(struct zicio_firehose_ctrl *zicio_firehose_ctrl,
	int huge_page_idx, int nr_nvme_page)
{
	return atomic_fetch_add(nr_nvme_page,
		zicio_firehose_ctrl->filled_pages_per_local_huge_page + huge_page_idx);
}

/**
 * zicio_fill_shared_huge_page - fill nvme pages to corresponding huge page
 * @zicio_shared_pool: zicio shared pool
 * @huge_page_idx: local huge page id
 * @nr_nvme_page: number of nvme page
 *
 * Add the number of nvme pages processed by this interrupt handler to the
 * corresponding counter of the huge page.
 *
 * Return the previous value.
 */
static inline long
zicio_fill_shared_huge_page(zicio_shared_pool *zicio_shared_pool,
	int huge_page_idx, int nr_nvme_page)
{
	struct zicio_shared_page_control_block *zicio_spcb =
			zicio_get_spcb_with_id_from_shared_pool(zicio_shared_pool,
					huge_page_idx);
	return atomic_fetch_add(nr_nvme_page,
			&zicio_spcb->zicio_spcb.filled_pages_per_local_huge_page);
}

/**
 * zicio_produce_local_huge_page - set the READY status in switch board
 * @zicio_desc: zicio descritptor
 * @zicio_cmd: zicio nvme command
 *
 * Set the corresponding bits in the swicth board.
 */
static void
zicio_produce_local_huge_page(struct zicio_descriptor *zicio_desc,
	zicio_nvme_cmd_list *zicio_cmd)
{
	struct zicio_switch_board *sb = zicio_desc->switch_board;
	struct zicio_firehose_ctrl *ctrl = &zicio_desc->firehose_ctrl;
	int nr_nvme_pages, huge_page_idx = zicio_cmd->local_huge_page_idx;

	nr_nvme_pages =
			ctrl->needed_pages_per_local_huge_page[huge_page_idx].counter;
	BUG_ON(nr_nvme_pages == 0);
	zicio_set_bytes(sb, huge_page_idx,
		nr_nvme_pages * ZICIO_NVME_PAGE_SIZE);
	mb();
	zicio_set_status(sb, huge_page_idx, ENTRY_READY);
	atomic_set(ctrl->requested_flag_per_local_huge_page + huge_page_idx, 0);
}

/**
 * zicio_is_local_huge_page_full - check whether the huge page is fulled
 * @zicio_desc: zicio descriptor
 * @cur_huge_page_filled: amount of filled
 * @huge_page_idx: huge page id
 *
 * Compare @needed_pages_per_chunk and @cur_chunk_filled.
 * Return true when the chunk is fully filled.
 */
static inline bool
zicio_is_local_huge_page_full(struct zicio_descriptor *zicio_desc,
	long cur_huge_page_filled, int huge_page_idx)
{
	struct zicio_firehose_ctrl *ctrl = &zicio_desc->firehose_ctrl;
	BUG_ON(ctrl->needed_pages_per_local_huge_page[huge_page_idx].counter == 0);
	return (ctrl->needed_pages_per_local_huge_page[huge_page_idx].counter
		== cur_huge_page_filled);
}

/**
 * zicio_is_shared_huge_page_full - check whether the huge page is fulled
 * @zicio_shared_pool: zicio shared pool descriptor
 * @cur_huge_page_filled: amount of filled
 * @huge_page_idx: huge page id
 *
 * Compare @needed_pages_per_chunk and @cur_chunk_filled.
 * Return true when the chunk is fully filled.
 */
static inline bool
zicio_is_shared_huge_page_full(struct zicio_shared_pool *zicio_shared_pool,
	long cur_huge_page_filled, int huge_page_idx)
{

	struct zicio_shared_page_control_block *zicio_spcb =
			zicio_get_spcb_with_id_from_shared_pool(zicio_shared_pool,
					huge_page_idx);
	BUG_ON(zicio_spcb->zicio_spcb.
			needed_pages_per_local_huge_page.counter == 0);
	return (zicio_spcb->zicio_spcb.
			needed_pages_per_local_huge_page.counter ==
			cur_huge_page_filled);
}

/**
 * zicio_is_passed_threshold - check whether new chunk is needed or not
 * @zicio_desc: zicio descriptor
 * @prev_huge_page_filled: previous amount of filled
 * @cur_huge_page_filled: amount of filled
 * @huge_page_idx: currently handling huge page index
 *
 * New commands for the next chunk can be made only when a certain amount of
 * huge page is filled, and this threshold depends on the execution mode.
 *
 * Return true if threshold is passed.
 */
static inline bool
zicio_is_passed_threshold(struct zicio_descriptor *zicio_desc,
	int prev_huge_page_filled, int cur_huge_page_filled, int huge_page_idx)
{
	struct zicio_firehose_ctrl *ctrl = &zicio_desc->firehose_ctrl;
	int needed_nr_pages 
		= atomic_read(&ctrl->needed_pages_per_local_huge_page[huge_page_idx]);
	int nr_chunk_threshold = ZICIO_NEW_CHUNK_THRESHOLD(needed_nr_pages);

	BUG_ON(needed_nr_pages == 0);

	return (prev_huge_page_filled < nr_chunk_threshold) &&
			(cur_huge_page_filled >= nr_chunk_threshold);
}

/**
 * zicio_is_passed_threshold_shared - check whether new chunk is needed or not
 * @zicio_desc: zicio descriptor
 * @prev_huge_page_filled: previous amount of filled
 * @cur_huge_page_filled: amount of filled
 * @huge_page_idx: currently handling huge page index
 *
 * New commands for the next chunk can be made only when a certain amount of
 * huge page is filled, and this threshold depends on the execution mode.
 *
 * Return true if threshold is passed.
 *
 * Note that unlike the local pages, shared pages are managed using shared page
 * control block and needed pages are remembered by this data structure.
 */
static inline bool
zicio_is_passed_threshold_shared(struct zicio_descriptor *zicio_desc,
	int prev_huge_page_filled, int cur_huge_page_filled, int huge_page_idx)
{
	struct zicio_shared_page_control_block *zicio_spcb
		= zicio_get_spcb_with_id(zicio_desc, huge_page_idx);
	int needed_nr_pages = 0, nr_chunk_threshold = 0;

	BUG_ON(zicio_spcb == NULL);

	needed_nr_pages 
		= atomic_read(&zicio_spcb->zicio_spcb.needed_pages_per_local_huge_page);
	nr_chunk_threshold = ZICIO_NEW_CHUNK_THRESHOLD(needed_nr_pages);

	return (prev_huge_page_filled < nr_chunk_threshold) &&
			(cur_huge_page_filled >= nr_chunk_threshold);
}

/**
 * zicio_complete_firehose_command_shared - nvme completion for zicio's
 * request
 * @zicio_desc: handling zicio descriptor
 * @zicio_cmd: handling nvme command
 *
 * Fill the requested chunk as read by nvme.
 */
int
zicio_complete_firehose_command_shared(
	struct zicio_descriptor *zicio_desc, zicio_nvme_cmd_list *zicio_cmd)
{
	struct zicio_shared_pool *zicio_shared_pool =
			zicio_get_shared_pool(zicio_desc);
	zicio_shared_page_control_block *zicio_spcb;
	int nr_nvme_page, local_huge_page_idx;
	long prev_huge_page_filled, cur_huge_page_filled;

	BUG_ON(zicio_desc == NULL || zicio_cmd == NULL);

	/*
	 * Get spcb for this command
	 */
	if (zicio_cmd->is_on_track_cmd) {
		zicio_spcb = zicio_get_spcb_with_id(zicio_desc,
				zicio_cmd->local_huge_page_idx);
	} else {
		zicio_spcb = zicio_get_local_spcb_with_id(zicio_desc,
				zicio_cmd->local_huge_page_idx);
	}

	BUG_ON(zicio_spcb->chunk_id != zicio_cmd->file_chunk_id);

	/*
	 * To deal with simultaneous production of multiple chunks, the firehose
	 * controller tracks the number of NVMe pages filled in each chunk.
	 *
	 * To do this, add the currently completed NVMe pages to the corresponding
	 * chunk's counter.
	 */
	nr_nvme_page = zicio_nvme_length_to_page(zicio_cmd->cmd.rw.length);
	local_huge_page_idx = zicio_cmd->local_huge_page_idx;
	if (zicio_cmd->is_on_track_cmd) {
		prev_huge_page_filled = zicio_fill_shared_huge_page(zicio_shared_pool,
				local_huge_page_idx, nr_nvme_page);
	} else {
		prev_huge_page_filled = zicio_fill_local_huge_page(
				&zicio_desc->firehose_ctrl, local_huge_page_idx, nr_nvme_page);
	}

	cur_huge_page_filled = prev_huge_page_filled + nr_nvme_page;
	put_cpu();
	BUG_ON(cur_huge_page_filled > ZICIO_NUM_NVME_PAGE_PER_HUGE_PAGE);

	/*
	 * If this nvme command filled the huge page entirely,
	 * set the status bit of swicth board. 
	 */
	if (zicio_cmd->is_on_track_cmd) {
		if (zicio_is_shared_huge_page_full(
					zicio_shared_pool, cur_huge_page_filled, local_huge_page_idx))
		{
			zicio_produce_local_huge_page_shared(zicio_desc, zicio_spcb, zicio_cmd);

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
			/* One chunk is filled. So enable one more chunk requesting */
			atomic_dec_if_positive(&zicio_shared_pool->cur_requested_chunk_count);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
		}

		/*
		 * Currently, only the first IRQ that passed the threshold can make a
		 * request for the next chunk.
		 */
		if (zicio_is_passed_threshold_shared(
				zicio_desc, prev_huge_page_filled,
				cur_huge_page_filled, local_huge_page_idx))
			return ZICIO_NEXT_CHUNK_ENABLED;
	} else {
		if (zicio_is_local_huge_page_full(zicio_desc, cur_huge_page_filled,
				local_huge_page_idx))
		{
			zicio_produce_local_huge_page_shared(zicio_desc, zicio_spcb, zicio_cmd);

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
			/* One chunk is filled. So enable one more chunk requesting */
			atomic_dec_if_positive(&zicio_shared_pool->cur_requested_chunk_count);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
		}

		/*
		 * Currently, only the first IRQ that passed the threshold can make a
		 * request for the next chunk.
		 */
		if (zicio_is_passed_threshold(
				zicio_desc, prev_huge_page_filled,
				cur_huge_page_filled, local_huge_page_idx))
			return ZICIO_NEXT_CHUNK_ENABLED;
	}

	return ZICIO_NEXT_CHUNK_DISABLED;
}
EXPORT_SYMBOL(zicio_complete_firehose_command_shared);


/**
 * zicio_complete_firehose_command - nvme completion for zicio's request
 * @zicio_desc: handling zicio descriptor
 * @zicio_cmd: handling nvme command
 *
 * Fill the requested chunk as read by nvme.
 */
int
zicio_complete_firehose_command(
	struct zicio_descriptor *zicio_desc, zicio_nvme_cmd_list *zicio_cmd)
{
	int nr_nvme_page, local_huge_page_idx;
	long prev_huge_page_filled, cur_huge_page_filled;

	BUG_ON(zicio_desc == NULL || zicio_cmd == NULL);

	/*
	 * To deal with simultaneous production of multiple chunks, the firehose
	 * controller tracks the number of NVMe pages filled in each chunk.
	 *
	 * To do this, add the currently completed NVMe pages to the corresponding
	 * chunk's counter.
	 */
	nr_nvme_page = zicio_nvme_length_to_page(zicio_cmd->cmd.rw.length);
	local_huge_page_idx = zicio_cmd->local_huge_page_idx;
	prev_huge_page_filled = zicio_fill_local_huge_page(
		&zicio_desc->firehose_ctrl, local_huge_page_idx, nr_nvme_page);
	cur_huge_page_filled = prev_huge_page_filled + nr_nvme_page;
	put_cpu();
	BUG_ON(cur_huge_page_filled > ZICIO_NUM_NVME_PAGE_PER_HUGE_PAGE);

	/*
	 * If this nvme command filled the local huge page entirely,
	 * set the status bit of swicth board. 
	 */
	if (zicio_is_local_huge_page_full(
			zicio_desc, cur_huge_page_filled, local_huge_page_idx))
		zicio_produce_local_huge_page(zicio_desc, zicio_cmd);

	/*
	 * Currently, only the first IRQ that passed the threshold can make a
	 * request for the next chunk.
	 */
	if (zicio_is_passed_threshold(
			zicio_desc, prev_huge_page_filled,
			cur_huge_page_filled, local_huge_page_idx))
		return ZICIO_NEXT_CHUNK_ENABLED;

	return ZICIO_NEXT_CHUNK_DISABLED;
}
EXPORT_SYMBOL(zicio_complete_firehose_command);

/**
 * zicio_init_next_local_huge_page - initialize for the next huge page
 * @zicio_desc: zicio descriptor
 * @huge_page_idx: initializing local huge page id
 *
 * Initialize counters corresponding to the given huge page.
 */
static void
zicio_init_next_local_huge_page(struct zicio_descriptor *zicio_desc,
	int huge_page_idx)
{
	struct zicio_firehose_ctrl *ctrl = &zicio_desc->firehose_ctrl;
	ctrl->filled_pages_per_local_huge_page[huge_page_idx].counter = 0;
	ctrl->needed_pages_per_local_huge_page[huge_page_idx].counter = 0;
}

/**
 * zicio_init_next_local_huge_page_shared - initialize for the next huge
 * page
 * @zicio_desc: zicio descriptor
 * @huge_page_idx: initializing local huge page id
 *
 * Initialize counters corresponding to the given huge page.
 */
void
zicio_init_next_local_huge_page_shared(struct zicio_descriptor *zicio_desc,
	int huge_page_idx)
{
	struct zicio_shared_page_control_block *zicio_spcb =
			zicio_get_spcb_with_id(zicio_desc, huge_page_idx);
	zicio_spcb->
		zicio_spcb.filled_pages_per_local_huge_page.counter = 0;
	zicio_spcb->
		zicio_spcb.needed_pages_per_local_huge_page.counter = 0;
}

/**
 * zicio_get_next_local_huge_page_id_shared_after_derailed - get next id of
 * local huge page after derailed.
 *
 * @zicio_desc: zicio descriptor
 *
 * Return the local shared page control block ID
 */
int zicio_get_next_local_huge_page_id_after_derailed(
	struct zicio_descriptor *zicio_desc)
{
	zicio_shared_pool_local *zicio_shared_pool_local;
	zicio_shared_page_control_block *zicio_spcb;
	zicio_attached_channel *zicio_channel;
	int local_spcb_idx;

	zicio_shared_pool_local = zicio_get_shared_pool_local(zicio_desc);
	zicio_channel = zicio_shared_pool_local->zicio_channel;

	/* Get the local spcb idx */
	for (local_spcb_idx = 0 ;
			local_spcb_idx < ZICIO_LOCAL_DATABUFFER_CHUNK_NUM ;
			local_spcb_idx++) {
		zicio_spcb = zicio_channel->local_zicio_spcb[local_spcb_idx];
		BUG_ON(!zicio_spcb);

		if (atomic_read(&zicio_spcb->zicio_spcb.is_used)) {
			continue;
		}

		if (atomic_inc_return(&zicio_spcb->zicio_spcb.ref_count) != 1) {
			atomic_dec(&zicio_spcb->zicio_spcb.ref_count);
			continue;
		}

		mb();
		atomic_set(&zicio_spcb->zicio_spcb.is_used, true);
		atomic_inc(&zicio_shared_pool_local->num_using_pages);

		zicio_init_next_local_huge_page(zicio_desc, local_spcb_idx);
		return local_spcb_idx;
	}
	return ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX;
}
EXPORT_SYMBOL(zicio_get_next_local_huge_page_id_after_derailed);

/**
 * zicio_get_next_local_huge_page_id_shared - get next id of local huge page
 * @zicio_desc: zicio descriptor
 */
int zicio_get_next_local_huge_page_id_shared(
	struct zicio_descriptor *zicio_desc, unsigned int *page_id_queue_idx,
	bool *derailed, bool irq_nolocalpage)
{
	struct zicio_shared_pool_local *zicio_shared_pool_local;
	struct zicio_shared_page_control_block *zicio_spcb;
	int spcb_idx;

	/* If channel is derailed, then get a local channel's page*/
	if (!irq_nolocalpage && zicio_check_channel_derailed(zicio_desc) && 
		*page_id_queue_idx == -1) {
		spcb_idx =
				zicio_get_next_local_huge_page_id_after_derailed(zicio_desc);
		*derailed = true;
		return spcb_idx;
	} else {
		zicio_shared_pool_local = zicio_get_shared_pool_local(zicio_desc);
		*derailed = false;
		if (*page_id_queue_idx == -1) {
			spcb_idx = zicio_get_page_id_from_queue(zicio_desc,
					page_id_queue_idx, false);
		} else {
			spcb_idx = zicio_read_page_id_from_queue(zicio_desc,
					*page_id_queue_idx);
		}

		if (spcb_idx == ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX) {
			return spcb_idx;
		}

		BUG_ON(spcb_idx < 0 || spcb_idx >= 64);
		zicio_spcb = zicio_get_spcb_with_id(zicio_desc, spcb_idx);
		BUG_ON(!zicio_spcb);

		atomic_inc(&zicio_spcb->zicio_spcb.ref_count);
		if (atomic_read(&zicio_spcb->zicio_spcb.ref_count) != 1) {
			zicio_print_page_id_queue(zicio_desc);
			BUG();
		}
		BUG_ON((atomic_read(&zicio_spcb->zicio_spcb.ref_count) != 1));
		atomic_set(&zicio_spcb->zicio_spcb.is_shared, true);
		atomic_set(&zicio_spcb->zicio_spcb.is_used, true);
		atomic_inc(&zicio_shared_pool_local->num_shared_pages);
		atomic_inc(&zicio_shared_pool_local->num_using_pages);

		zicio_init_next_local_huge_page_shared(zicio_desc, spcb_idx);

		atomic64_set(&zicio_spcb->zicio_spcb.exp_jiffies, ULONG_MAX);

		mb();
		zicio_set_contribute_shared_page_control_block(zicio_desc, zicio_spcb);
	}

	BUG_ON(spcb_idx == ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX);

	return spcb_idx;
}
EXPORT_SYMBOL(zicio_get_next_local_huge_page_id_shared);


/**
 * zicio_get_next_local_huge_page_id - get the next index for new huge page
 * @zicio_desc: zicio descriptor
 *
 * Get the index of the huge page available in the user buffer. To know this
 * efficiently, we use a counter called @requested to determine where to start
 * the search.
 */
static int
zicio_get_next_local_huge_page_id(struct zicio_descriptor *zicio_desc)
{
	struct zicio_firehose_ctrl *ctrl = &zicio_desc->firehose_ctrl;
	struct zicio_switch_board *sb = zicio_desc->switch_board;
	int idx, i, found = 0;
	unsigned long flags;

	BUG_ON(sb == NULL);

	spin_lock_irqsave(&ctrl->lock, flags);

	for (i = 0; i < ZICIO_MAX_NUM_CHUNK; i++) {
		idx = zicio_calc_local_huge_page_idx(ctrl->requested + i);
		if (atomic_read(ctrl->requested_flag_per_local_huge_page + idx) == 0
				&& zicio_read_status(sb, idx) != ENTRY_READY
				&& zicio_read_status(sb, idx) != ENTRY_INUSE) {
			atomic_set(ctrl->requested_flag_per_local_huge_page + idx, 1);
			ctrl->requested++;
			found = 1;
			break;
		}
	}

	if (!found)
		idx = ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX;

	spin_unlock_irqrestore(&ctrl->lock, flags);

	return idx;
}

/**
 * zicio_prepare_next_page_id_shared - prepare for the next chunk
 * @zicio_desc: handling zicio descriptor
 *
 * Return the next chunk id for new request.
 * If the next chunk is not acquired, return chunk_id as -1.
 */
int zicio_prepare_next_local_huge_page_id_shared(
	struct zicio_descriptor *zicio_desc, bool *derailed,
	unsigned int *page_id_queue_idx, bool irq_nolocalpage)
{
    return zicio_get_next_local_huge_page_id_shared(
			zicio_desc, page_id_queue_idx, derailed, irq_nolocalpage);
}
EXPORT_SYMBOL(zicio_prepare_next_local_huge_page_id_shared);

/**
 * zicio_prepare_next_chunk - prepare for the next chunk
 * @zicio_desc: handling zicio descriptor
 *
 * Return the next chunk id for new request.
 * If the next chunk is not acquired, return chunk_id as -1.
 */
int zicio_prepare_next_local_huge_page_id(
	struct zicio_descriptor *zicio_desc)
{
	int huge_page_id = zicio_get_next_local_huge_page_id(zicio_desc);
	if (huge_page_id != ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX)
		zicio_init_next_local_huge_page(zicio_desc, huge_page_id);
	return huge_page_id;
}
EXPORT_SYMBOL(zicio_prepare_next_local_huge_page_id);

/**
 * zicio_find_last_metadata_cmd - find last metadata nvme command
 * @first_meta_cmd: first metadata nvme command
 *
 * Find last metadata nvme command
 */
static inline struct zicio_nvme_cmd_list *
zicio_find_last_metadata_cmd(struct zicio_nvme_cmd_list *first_meta_cmd)
{
	struct zicio_nvme_cmd_list *cur_cmd = first_meta_cmd->next;
	struct zicio_nvme_cmd_list *prev_cmd = first_meta_cmd;
	while (cur_cmd && cur_cmd->is_metadata) {
		prev_cmd = cur_cmd;
		cur_cmd = cur_cmd->next;
	}
	return prev_cmd;
}

/**
 * zicio_distinguish_cmd_list - distinghush meta and chunk data commands
 * @cmd_list: command list
 * @meta_cmd_list_head: meta data command list's head
 * @chunk_cmd_list_head: chunk data command list's head
 *
 * zicio nvme command list is composed of metadata commands first, followed
 * by chunk data commands. Also metadata commands may or may not exist.
 *
 * Separate the commands in the given command list into metadata and chunk data
 * and put them into corresponding list.
 */
static void __zicio_distinguish_cmd_list(
	struct zicio_nvme_cmd_list **cmd_list,
	struct zicio_nvme_cmd_list **meta_cmd_list_head,
	struct zicio_nvme_cmd_list **last_meta_cmd, int num_inner_dev,
	int list_idx)
{
	if (cmd_list[list_idx] && cmd_list[list_idx]->is_metadata) {
		if (!(*meta_cmd_list_head)) {
			*meta_cmd_list_head = cmd_list[list_idx];
		}

		if (*last_meta_cmd) {
			(*last_meta_cmd)->next = cmd_list[list_idx];
		}
		*last_meta_cmd = zicio_find_last_metadata_cmd(cmd_list[list_idx]);

		/*
		 * Disconnect metadata command and chunk data command because metadata
		 * commands and chunk data commands will be located in different request
		 * timer.
		 */
		cmd_list[list_idx] = (*last_meta_cmd)->next;
		(*last_meta_cmd)->next = NULL;

		if (list_idx) {
			cmd_list[list_idx - 1 + num_inner_dev]->next = NULL;
		}
	}
}

/**
 * zicio_distinguish_cmd_list - distinghush meta and chunk data commands
 * @cmd_list: command list
 * @meta_cmd_list_head: meta data command list's head
 *
 * zicio nvme command list is composed of metadata commands first, followed
 * by chunk data commands. Also metadata commands may or may not exist.
 *
 * Separate the commands in the given command list into metadata and chunk data
 * and put them into corresponding list.
 */
static void zicio_distinguish_cmd_list(
	struct zicio_nvme_cmd_list **start_cmd_lists,
	struct zicio_nvme_cmd_list **meta_cmd_list_head, int num_dev)
{
	struct zicio_nvme_cmd_list *last_meta_cmd = NULL;
	int i;

	for (i = 0 ; i < num_dev ; i++) {
		__zicio_distinguish_cmd_list(start_cmd_lists,
				meta_cmd_list_head, &last_meta_cmd, num_dev, i);
	}
}

static unsigned int zicio_clean_cmd_list_set(
	struct zicio_nvme_cmd_list **zicio_start_cmd_lists, int num_device)
{
	struct zicio_nvme_cmd_list **meta_cmd_lists = zicio_start_cmd_lists +
			(num_device << 1);
	unsigned int file_chunk_id = UINT_MAX;
	int i;

	for (i = 0 ; i < num_device ; i++) {
		meta_cmd_lists[i] = meta_cmd_lists[i + num_device] = NULL;
		if (zicio_start_cmd_lists[i + num_device]) {
			zicio_start_cmd_lists[i + num_device]->next = NULL;
			file_chunk_id = zicio_start_cmd_lists[i]->file_chunk_id;
		}
	}
	return file_chunk_id;
}

/**
 * zicio_preprocess_chunk_cmds - preprocess chunk nvme commands
 * @zicio_desc: zicio descriptor
 * @cmd: chunk data nvme command list
 * @nr_nvme_pages: stores how many nvme pages are needed
 * @io_time: stores the total I/O time for nvme commands
 *
 * Calculate how many nvme pages are needed for the chunk and how much I/O time
 * will be required for this chunk.
 *
 * Save them to the @nr_nvme_pages, @io_time.
 */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
static inline void zicio_preprocess_chunk_cmds(
	struct zicio_descriptor *zicio_desc,
	struct zicio_nvme_cmd_list *cmd,
	int *nr_nvme_pages, ktime_t *io_time,
	int *nr_command)
#else
static inline void zicio_preprocess_chunk_cmds(
	struct zicio_descriptor *zicio_desc,
	struct zicio_nvme_cmd_list *cmd,
	int *nr_nvme_pages, ktime_t *io_time)
#endif
{
	int cur_nr_nvme_pages;
	int global_device_idx;

	global_device_idx = zicio_get_zicio_global_device_idx(
		zicio_desc, cmd->device_idx);
	while (cmd) {
		BUG_ON(cmd->is_metadata);
		cur_nr_nvme_pages = zicio_nvme_length_to_page(cmd->cmd.rw.length);
		*nr_nvme_pages += cur_nr_nvme_pages;
		*io_time += zicio_get_io_time(
				cur_nr_nvme_pages, zicio_desc->cpu_id, global_device_idx);
		cmd = cmd->next;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		*nr_command += 1;
#endif
	}

	BUG_ON(*nr_nvme_pages > ZICIO_NUM_NVME_PAGE_PER_HUGE_PAGE);
}

/**
 * __zicio_lock_and_load - hang the request timers to the wheel
 * @zicio_desc: zicio descriptor
 * @global_device_idx: global raw device index
 * @zicio_cmd_list: nvme command list to be hanged to wheel
 * @release: release time 
 *
 * Create request timer and hang it to the request timer wheel.
 */
void __zicio_lock_and_load(
	struct zicio_descriptor *zicio_desc, int global_device_idx,
	struct zicio_nvme_cmd_list *zicio_cmd_list, ktime_t release)
{
	struct zicio_request_timer *req_timer =
		zicio_get_request_timer(zicio_desc, zicio_cmd_list);

	zicio_add_request_timer_on(
		req_timer, release, zicio_desc->cpu_id, global_device_idx);
}
EXPORT_SYMBOL(__zicio_lock_and_load);

static void zicio_lock_and_load_metadata_command(
	struct zicio_descriptor *zicio_desc,
	struct zicio_nvme_cmd_list *meta_cmds,
	ktime_t release)
{
	int global_device_idx;

	while (meta_cmds) {
		global_device_idx = zicio_get_zicio_global_device_idx(
				zicio_desc, meta_cmds->device_idx);
		__zicio_lock_and_load(
			zicio_desc, global_device_idx, meta_cmds, release);
		meta_cmds = meta_cmds->next;
	}
}

static bool
zicio_check_cmd_list_is_shared(struct zicio_descriptor *zicio_desc,
		struct zicio_nvme_cmd_list **zicio_start_cmd_list, int num_device)
{
	int idx;

	if (!zicio_desc->zicio_shared_pool_desc) {
		return false;
	}

	for (idx = 0 ; idx < num_device ; idx++) {
		if (zicio_start_cmd_list[idx]->is_on_track_cmd) {
			return true;
		}
	}
	return false;
}

/**
 * zicio_lock_and_load - make request timers and hang out them
 * @zicio_desc: zicio descriptor
 * @zicio_cmd_list: nvme command list
 * @huge_page_id: requesting local huge page id
 *
 * In zicio's I/O handler, the nvme commands for the new chunk are obtained
 * as a linke list (@zicio_cmd_list). Also sometimes nvme commands for metadata are
 * contained in the list.
 *
 * Calculate when these nvme commands must be submitted to the device and wrap
 * them to the request timer. Then hang the timer to the request timer wheel.
 *
 * Note that request timers must be put on the wheel after we calculate how many
 * nvme pages the corresponding chunk consist of. Otherwise, the IRQ may
 * mistakenly believe that the chunk is full.
 */
void zicio_lock_and_load(
	struct zicio_descriptor *zicio_desc,
	struct zicio_nvme_cmd_list **zicio_start_cmd_list, int num_device,
	int huge_page_id)
{
	struct zicio_shared_page_control_block *zicio_spcb = NULL;
	struct zicio_nvme_cmd_list *meta_cmds = NULL;
	ktime_t know = ktime_get(), io_time = 0, release;
	int global_device_idx, list_idx, device_idx;
	int nr_pages = 0;
	unsigned int file_chunk_id;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	int nr_command = 0;
#endif

	zicio_distinguish_cmd_list(zicio_start_cmd_list, &meta_cmds, num_device);
	file_chunk_id = zicio_clean_cmd_list_set(zicio_start_cmd_list, num_device);

	/* If this channel is attached to shared pool, then get spcb and use it. */
	if (zicio_desc->zicio_shared_pool_desc) {
		if (zicio_check_cmd_list_is_shared(zicio_desc, zicio_start_cmd_list,
					num_device)) {
			zicio_spcb = zicio_get_spcb_with_id(zicio_desc, huge_page_id);
		} else {
			zicio_spcb = zicio_get_local_spcb_with_id(zicio_desc, huge_page_id);
		}
	}

	for (list_idx = 0 ; list_idx < num_device ; list_idx++) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		zicio_preprocess_chunk_cmds(zicio_desc, zicio_start_cmd_list[list_idx],
			&nr_pages, &io_time, &nr_command);
#else
		zicio_preprocess_chunk_cmds(zicio_desc, zicio_start_cmd_list[list_idx],
			&nr_pages, &io_time);
#endif
	}

	/* Set the number of pages to fill for data chunk. */
	if (zicio_spcb && atomic_read(&zicio_spcb->zicio_spcb.is_shared)) {
		zicio_set_needed_nvme_pages_shared(zicio_desc, huge_page_id, nr_pages);
	} else {
		zicio_set_needed_nvme_pages(zicio_desc, huge_page_id, nr_pages);
	}
	mb();

	/*
	 * Chunk data's expiration time is determined by the user's consumption
	 * rate, but metadata's expiration time is set to the current time
	 * unconditionally.
	 */
	zicio_lock_and_load_metadata_command(zicio_desc, meta_cmds, know);

	for (list_idx = 0 ; list_idx < num_device ; list_idx++) {
		global_device_idx = zicio_get_zicio_global_device_idx(
				zicio_desc, zicio_start_cmd_list[list_idx]->device_idx);
		release = zicio_calc_release_time(
			zicio_desc, global_device_idx, huge_page_id, know, io_time,
				(zicio_spcb) ? atomic_read(&zicio_spcb->zicio_spcb.is_shared) : false);
		__zicio_lock_and_load(
			zicio_desc, global_device_idx, zicio_start_cmd_list[list_idx], release);
	}
	device_idx = zicio_get_zicio_channel_dev_idx(zicio_desc,
				zicio_start_cmd_list[0]->device_idx);
	zicio_free_cmd_lists_set_with_desc(zicio_desc, device_idx,
				zicio_start_cmd_list);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu[%d] lock_and_load, nr_command: %d nr_pages: %d [%s:%d][%s]\n",
					zicio_desc->cpu_id, nr_command, nr_pages,
					 __FILE__, __LINE__, __FUNCTION__);
#endif
}
EXPORT_SYMBOL(zicio_lock_and_load);

/**
 * zicio_defer_lock_and_load - defer hanging nvme commands to wheel
 * @zicio_desc: zicio descriptor
 * @zicio_cmd_list: nvme command list
 *
 * If there is no valid chunk, make nvme commands to the zombie and defer the
 * haning them to the wheel.
 *
 * Note that meta data commands should not be defered.
 */
void zicio_defer_lock_and_load(
	struct zicio_descriptor *zicio_desc,
	struct zicio_nvme_cmd_list **zicio_start_cmd_list,
	struct zicio_nvme_cmd_list *cur_cmd, int num_device)
{
	struct zicio_nvme_cmd_list *meta_cmds = NULL;
	ktime_t know = ktime_get();
	int global_zicio_device_idx = zicio_get_zicio_channel_fsdev_idx(
		zicio_desc, cur_cmd->device_idx);

	zicio_distinguish_cmd_list(zicio_start_cmd_list, &meta_cmds, num_device);
	zicio_clean_cmd_list_set(zicio_start_cmd_list, num_device);
	zicio_lock_and_load_metadata_command(zicio_desc, meta_cmds, know);

	if (zicio_has_cmd_list_in_set(zicio_start_cmd_list, num_device)) {
		zicio_hang_zombie_request_timer(zicio_desc, zicio_start_cmd_list,
			num_device, global_zicio_device_idx);
	}
}
EXPORT_SYMBOL(zicio_defer_lock_and_load);

/**
 * zicio_require_next_chunk - prepare the nvme commands for next chunk
 * @zicio_desc: zicio_descriptor
 * @cur_cmd: NVMe command being processed by the current I/O handler
 *
 * Prepare the NVMe commands for the next chunk, and attach them to the request
 * timer wheel.
 *
 * If the page id is invalid (-1), make the commands to a zombie request and
 * store them to the timer wheel.
 *
 * A retry to get a valid page id will be performed in a later I/O handler or
 * timer softirq.
 */
void zicio_require_next_chunk(struct zicio_descriptor *zicio_desc,
	struct zicio_nvme_cmd_list *cur_cmd)
{
	int num_device, device_idx;
	int local_page_idx = zicio_prepare_next_local_huge_page_id(zicio_desc);
	struct zicio_nvme_cmd_list **zicio_start_cmd_lists =
		zicio_create_command(zicio_desc, local_page_idx, &num_device, true);

	if (!zicio_has_cmd_list_in_set(zicio_start_cmd_lists, num_device)) {
		if (zicio_start_cmd_lists) {
			device_idx = zicio_get_zicio_channel_dev_idx(zicio_desc,
						zicio_start_cmd_lists[0]->device_idx);
			zicio_free_cmd_lists_set_with_desc(zicio_desc, device_idx,
						zicio_start_cmd_lists);
		}
	} else if (local_page_idx != ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX) {
		zicio_lock_and_load(zicio_desc, zicio_start_cmd_lists, num_device,
			local_page_idx);
	} else {
		zicio_defer_lock_and_load(zicio_desc, zicio_start_cmd_lists, cur_cmd,
			num_device);
	}
}
EXPORT_SYMBOL(zicio_require_next_chunk);

/**
 * zicio_require_next_chunk_shared - prepare the nvme commands for next chunk
 * @zicio_desc: zicio_descriptor
 * @cur_cmd: NVMe command being processed by the current I/O handler
 * 
 * TODO: write comment
 */
void
zicio_require_next_chunk_shared(struct zicio_descriptor *zicio_desc)
{
	struct zicio_shared_pool *zicio_shared_pool =
		zicio_get_shared_pool(zicio_desc);
	struct zicio_nvme_cmd_list **start_cmd_lists;
	int local_page_idx, num_device, device_idx;
	unsigned int page_id_queue_idx = -1;
	bool derailed;

l_retry_create_next_chunk_command:
	local_page_idx = zicio_prepare_next_local_huge_page_id_shared(
			zicio_desc, &derailed, &page_id_queue_idx, false);
 
	/*
	 * If the local page idx is not assigned, it returns after hanging the work
	 * in the callback.
	 */
	if (local_page_idx == ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "cpu[%d] no local page [%s:%d][%s]\n",
					zicio_desc->cpu_id, __FILE__, __LINE__, __FUNCTION__);
#endif
#ifdef CONFIG_ZICIO_STAT
		zicio_count_softirq_trigger_shared(zicio_desc,
				ZICIO_NOLOCALPAGE);
#endif /* (CONFIG_ZICIO_STAT) */
		if (!derailed) {
			BUG_ON(page_id_queue_idx == -1);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
			zicio_create_reactivate_trigger_shared(zicio_desc,
					ZICIO_NOLOCALPAGE, false, page_id_queue_idx, false,
					ktime_get());
#else
			zicio_create_reactivate_trigger_shared(zicio_desc,
					ZICIO_NOLOCALPAGE, false, page_id_queue_idx, false);
#endif
		} else {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
			zicio_create_reactivate_trigger_shared(zicio_desc,
					ZICIO_NOLOCALPAGE, true, -1, false, ktime_get());
#else
			zicio_create_reactivate_trigger_shared(zicio_desc,
					ZICIO_NOLOCALPAGE, true, -1, false);
#endif
		}
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
		/* roll back the state */
		atomic_dec_if_positive(&zicio_shared_pool->cur_requested_chunk_count);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
		return;
	}

	start_cmd_lists = zicio_create_command_shared(zicio_desc, local_page_idx,
			&num_device, !derailed);

	if (start_cmd_lists == ERR_PTR(UINT_MAX - 1)) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "cpu[%d] no local page [%s:%d][%s]\n",
					zicio_desc->cpu_id, __FILE__, __LINE__, __FUNCTION__);
#endif
		page_id_queue_idx = -1;
		goto l_retry_create_next_chunk_command;
	}

	if (zicio_has_cmd_list_in_set(start_cmd_lists, num_device)) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "cpu[%d] no local page [%s:%d][%s]\n",
					zicio_desc->cpu_id, __FILE__, __LINE__, __FUNCTION__);
#endif
		BUG_ON(local_page_idx == ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX);
		zicio_lock_and_load(zicio_desc, start_cmd_lists,
				num_device, local_page_idx);
	} else { 
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "cpu[%d] no local page [%s:%d][%s]\n",
					zicio_desc->cpu_id, __FILE__, __LINE__, __FUNCTION__);
#endif
		if (start_cmd_lists) {
			device_idx = zicio_get_zicio_channel_dev_idx(zicio_desc,
					start_cmd_lists[0]->device_idx);
			zicio_free_cmd_lists_set_with_desc(zicio_desc, device_idx,
					start_cmd_lists);
		}
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
		/* roll back the state */
		atomic_dec_if_positive(&zicio_shared_pool->cur_requested_chunk_count);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	}
}
EXPORT_SYMBOL(zicio_require_next_chunk_shared);

/*
 * Initialize zicio firehose controller.
 */
void zicio_init_firehose_ctrl(struct zicio_descriptor *zicio_desc)
{
	struct zicio_notify_descriptor *zicio_notify_desc = (zicio_notify_descriptor *)zicio_desc;
	struct zicio_firehose_ctrl *ctrl = &zicio_desc->firehose_ctrl;
	unsigned int nvme_cmd_info_start, nvme_cmd_info_end;
	int i, nr_nvme_pages = 0, chunk_idx, cmd_idx;

	/* Initialize fine-grained chunk counters to track nvme pages */
	for (i = 0; i < ZICIO_MAX_NUM_CHUNK; i++) {
		ctrl->filled_pages_per_local_huge_page[i].counter = 0;
		ctrl->needed_pages_per_local_huge_page[i].counter = 0;
		ctrl->requested_flag_per_local_huge_page[i].counter = 0;
	}
	INIT_LIST_HEAD(&ctrl->active_req_timers);
	INIT_LIST_HEAD(&ctrl->active_zombie_req_timers);
	spin_lock_init(&ctrl->lock);
	ctrl->last_user_avg_tsc_delta = 0;
	ctrl->requested = 1;
	ctrl->bandwidth = 0;
	ctrl->last_update_bandwidth = kmalloc(sizeof(s64) *
		zicio_desc->dev_maps.nr_raw_dev, GFP_KERNEL|__GFP_ZERO);

	/*
	 * TODO: This code is the cause of zicio not being able to handle files
	 * smaller than 2MB. Fix it later
	 */
	ctrl->needed_pages_per_local_huge_page[0].counter = 512;
	ctrl->requested_flag_per_local_huge_page[0].counter = 1;

	/*
	 * XXX zicio
	 * 
	 * The code below resolves the above TODO using the design of zicIO.
	 */
	if (zicio_notify_desc->nr_fd_of_batches != 0) {
		nvme_cmd_info_start = zicio_notify_desc->nvme_cmd_info_start_offsets[0][0];

		if (zicio_notify_desc->nr_nvme_cmd_info_start_offsets == 1) {
			nvme_cmd_info_end
				= zicio_notify_desc->nr_nvme_cmd_infos - 1;
		} else {
			nvme_cmd_info_end
				= zicio_notify_desc->nvme_cmd_info_start_offsets[0][1] - 1;
		}

		chunk_idx = nvme_cmd_info_start / ZICIO_NVME_CMD_INFO_PER_CHUNK;
		cmd_idx = nvme_cmd_info_start % ZICIO_NVME_CMD_INFO_PER_CHUNK;
		for (i = nvme_cmd_info_start; i <= nvme_cmd_info_end; i++) {
			zicio_nvme_cmd_info info
				= zicio_notify_desc->nvme_cmd_infos[chunk_idx][cmd_idx];

			nr_nvme_pages
				+= ZICIO_NVME_CMD_INFO_GET_LENGTH(info) + 1;

			if (++cmd_idx == ZICIO_NVME_CMD_INFO_PER_CHUNK) {
				++chunk_idx;
				cmd_idx = 0;
			}
		}

		ctrl->needed_pages_per_local_huge_page[0].counter = nr_nvme_pages;
	}
}
EXPORT_SYMBOL(zicio_init_firehose_ctrl);

void __zicio_clean_timer(struct zicio_descriptor *zicio_desc,
			int global_device_idx, bool from_doexit)
{
	struct zicio_firehose_ctrl *ctrl = &zicio_desc->firehose_ctrl;
	struct zicio_request_timer *timer;
	struct zicio_nvme_cmd_list *zicio_cmd_list, *tmp;
	struct list_head *pos, *n;

	list_for_each_safe(pos, n, &ctrl->active_req_timers) {
		timer = (struct zicio_request_timer *)container_of(pos,
				struct zicio_request_timer, sibling);
		zicio_del_request_timer(timer, global_device_idx);

		zicio_cmd_list = timer->zicio_cmd_list;
		while (from_doexit && zicio_cmd_list) {
			tmp = zicio_cmd_list;
			zicio_cmd_list = zicio_cmd_list->next;
			zicio_free_nvme_cmd_list(tmp);
		}

		ZICIO_INIT_REQUEST_TIMER(timer, NULL, NULL);
		zicio_free_request_timer(timer);
	}
}

void zicio_clean_timer_for_md(struct zicio_descriptor *zicio_desc,
			struct zicio_device *zicio_device, bool from_doexit)
{
	int num_inner_dev = zicio_get_num_inner_device(zicio_device), idx;
	int *global_device_idx_array =
			zicio_get_num_raw_device_idx_array(zicio_device);

	for (idx = 0 ; idx < num_inner_dev ; idx++) {
		__zicio_clean_timer(zicio_desc, global_device_idx_array[idx],
				from_doexit);
	}
}

void zicio_clean_zombie_timer(zicio_descriptor *zicio_desc,
			int global_device_idx, bool from_doexit)
{
	struct zicio_firehose_ctrl *ctrl = &zicio_desc->firehose_ctrl;
	struct zicio_zombie_request_timer *zombie_timer;
	struct zicio_nvme_cmd_list *zicio_cmd_list, *tmp;
	struct list_head *pos, *n;

	list_for_each_safe(pos, n, &ctrl->active_zombie_req_timers) {
		zombie_timer =
			(struct zicio_zombie_request_timer *)container_of(pos,
					struct zicio_zombie_request_timer, sibling);
		zicio_del_zombie_request_timer(zombie_timer, global_device_idx);

		zicio_cmd_list = zombie_timer->zicio_cmd_list;
		while (from_doexit && zicio_cmd_list) {
			tmp = zicio_cmd_list;
			zicio_cmd_list = zicio_cmd_list->next;
			zicio_free_nvme_cmd_list(tmp);
			zicio_free_cmd_lists_set_with_desc(zicio_desc,
					global_device_idx, zombie_timer->zicio_start_cmd_lists);
		}

		ZICIO_INIT_ZOMBIE_REQUEST_TIMER(zombie_timer, NULL, NULL);
		zicio_free_request_timer(zombie_timer);
	}
}

/*
 * Close firehose controller.
 */
void zicio_close_firehose_ctrl(struct zicio_descriptor *zicio_desc,
			bool from_doexit)
{
	struct zicio_firehose_ctrl *ctrl = &zicio_desc->firehose_ctrl;
	struct zicio_device *zicio_device;
	unsigned long flags;
	int dev_idx, num_dev = zicio_desc->dev_maps.nr_dev;
	int global_device_idx;

	spin_lock_irqsave(&ctrl->lock, flags);
	for (dev_idx = 0 ; dev_idx < num_dev ; dev_idx++) {
		zicio_device = zicio_get_zicio_device_with_desc(zicio_desc, dev_idx);
		global_device_idx = zicio_get_zicio_global_device_idx(
				zicio_desc, dev_idx);

		if (zicio_device->device_type == ZICIO_MD) {
			zicio_clean_timer_for_md(zicio_desc, zicio_device, from_doexit);
		} else {
			__zicio_clean_timer(zicio_desc, global_device_idx, from_doexit);
		}

		zicio_clean_zombie_timer(zicio_desc, global_device_idx, from_doexit);
	}
	spin_unlock_irqrestore(&ctrl->lock, flags);
	kfree(ctrl->last_update_bandwidth);
}
EXPORT_SYMBOL(zicio_close_firehose_ctrl);

/*
 * Dump function for debugging
 */
void zicio_dump_firehose_ctrl(struct zicio_firehose_ctrl *fctrl)
{
	int i;
	for (i = 0; i < ZICIO_MAX_NUM_CHUNK; i++) {
		printk(KERN_WARNING "[ZICIO_FIREHOSE] chunk%d, needed_pages: %d, filled_pages: %d, requested: %lld, requested_flag: %d\n",
		i, fctrl->needed_pages_per_local_huge_page[i].counter,
		fctrl->filled_pages_per_local_huge_page[i].counter, fctrl->requested,
		atomic_read(fctrl->requested_flag_per_local_huge_page + i));
	}
}
EXPORT_SYMBOL(zicio_dump_firehose_ctrl);
