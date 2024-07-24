#ifndef _LINUX_ZICIO_FIREHOSE_CTRL_H
#define _LINUX_ZICIO_FIREHOSE_CTRL_H

#include <linux/blkdev.h>
#include <linux/zicio_notify.h>
#include <linux/types.h>
#include <linux/nvme.h>
#include <linux/ktime.h>

#include "zicio_req_timer.h"

/**
 * Return values for zicio_complete_firehose_command()
 *
 * @ZICIO_NEED_NEW_CHUNK: This value tells that the caller should create new
 * nvme_commands for a next chunk.
 *
 * @ZICIO_NO_NEED_NEW_CHUNK: This value tells that the caller does not need
 * to make a new nvme_command for a next chunk.
 */
#define ZICIO_NEXT_CHUNK_DISABLED	(0)
#define ZICIO_NEXT_CHUNK_ENABLED	(1)

extern int zicio_complete_firehose_command(
			struct zicio_descriptor *zicio_desc,
			struct zicio_nvme_cmd_list *cmd_desc);

extern int zicio_complete_firehose_command_shared(
			struct zicio_descriptor *zicio_desc,
			struct zicio_nvme_cmd_list *cmd_desc);

extern int zicio_prepare_next_local_huge_page_id(
			struct zicio_descriptor *zicio_desc);

extern int zicio_prepare_next_local_huge_page_id_shared(
			struct zicio_descriptor *zicio_desc, bool *derailed,
			unsigned int *page_id_queue_idx, bool irq_nolocalpage);

extern void __zicio_lock_and_load(struct zicio_descriptor *zicio_desc,
			int global_device_idx, struct zicio_nvme_cmd_list *zicio_cmd_list,
			ktime_t release);

extern void zicio_lock_and_load(
			struct zicio_descriptor *zicio_desc,
			struct zicio_nvme_cmd_list **zicio_start_cmd_list, int num_device,
			int chunk_id);

extern void zicio_defer_lock_and_load(
			struct zicio_descriptor *zicio_desc,
			struct zicio_nvme_cmd_list **zicio_start_cmd_list,
			struct zicio_nvme_cmd_list *cur_cmd, int num_device);

extern int zicio_get_next_local_huge_page_id_shared(
			struct zicio_descriptor *zicio_desc,
			unsigned int * page_id_queue_idx, bool *derailed,
			bool irq_nolocalpage);

extern void zicio_require_next_chunk(
			struct zicio_descriptor *zicio_desc,
			struct zicio_nvme_cmd_list *cur_cmd);

extern void zicio_require_next_chunk_shared(
			struct zicio_descriptor *zicio_desc);

extern void zicio_init_firehose_ctrl(
			struct zicio_descriptor *zicio_desc);

extern void zicio_close_firehose_ctrl(
			struct zicio_descriptor *zicio_desc, bool from_doexit);

extern void zicio_dump_firehose_ctrl(struct zicio_firehose_ctrl *ctrl);

extern void zicio_set_init_needed_nvme_pages(
		struct zicio_descriptor *zicio_desc, int init_local_page_idx,
		int init_trigger_pages);

extern void zicio_init_next_local_huge_page_shared(
		struct zicio_descriptor *zicio_desc, int huge_page_idx);

extern void zicio_set_needed_nvme_pages(
		struct zicio_descriptor *zicio_desc, int huge_page_idx,
		int needed_pages);
#endif	/* _LINUX_ZICIO_FIREHOSE_CTRL_H */
