#include <linux/types.h>

#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/fsnotify.h>
#include <linux/iomap.h>
#include <linux/uio.h>
#include <linux/sched/xacct.h>
#include <uapi/linux/uio.h>
#include <linux/zicio_notify.h>

#include "zicio_cmd.h"
#include "zicio_desc.h"
#include "zicio_device.h"
#include "zicio_extent.h"
#include "zicio_firehose_ctrl.h"
#include "zicio_files.h"
#include "zicio_md_flow_ctrl.h"
#include "zicio_mem.h"
#include "zicio_req_submit.h"
#include "zicio_shared_pool.h"
#include "../block/blk-rq-qos.h"

#include "zicio_nvme_cmd_timer_wheel.h"
#include "zicio_flow_ctrl.h"

/*
 * __zicio_iomap_dio_bio_end_io
 *
 * functions to free zicio direct io structure
 */
void __zicio_iomap_dio_bio_end_io(struct zicio_iomap_dio *dio)
{
	struct kiocb *iocb = dio->iocb;

	/*
	 * Using kiocb, zicio stats are managed and delivered to the bio layer.
	 * At this time, the kiocb structure must go through the process of dynamic
	 * allocation and deallocation. Release is done explicitly in the callback
	 * function.
	 */
	kfree(iocb);
	/*
	 * free zicio_iomap_dio
	 */
	zicio_free_dio(dio);
}

/*
 * zicio_iomap_dio_bio_end_io
 *
 * callback function to free bio
 */
void zicio_iomap_dio_bio_end_io(struct bio *bio)
{
	BUG_ON(!bio->zicio_enabled);
	/* Free resources of zicio direct I/O */
	__zicio_iomap_dio_bio_end_io(bio->bi_private);

	/* Free resources of zicio bio */
	bio->bi_end_io = NULL;
	bio->zicio_enabled = false;
	bio->zicio_from_softirq = false;
	bio->zicio_desc = NULL;
	bio->zicio_cpu_id = -1;

	/* Free bio structure */
	bio_release_pages(bio, false);
	bio_put(bio);
}

/*
 * zicio_set_bio_vector
 *
 * Set bio vector for zicio
 */
void
zicio_set_bio_vector(struct bio *bio, zicio_descriptor *desc,
			zicio_nvme_cmd_list *cmd_list)
{
	unsigned long page_no = cmd_list->start_mem &
				(~((unsigned long)(ZICIO_PAGE_SIZE - 1UL)));
	unsigned long page_offset = cmd_list->start_mem & (ZICIO_PAGE_SIZE - 1);
	/* start page pointer of bio vector */
	bio->bi_io_vec->bv_page = virt_to_page(page_no);
	/* Length of next cmd */
	bio->bi_io_vec->bv_len = (cmd_list->cmd.rw.length + 1) << 9;
	/* In page offset */
	bio->bi_io_vec->bv_offset = page_offset;

	/* I/O start sector number */
	bio->bi_iter.bi_sector = cmd_list->cmd.rw.slba;
	/* I/O sector size */
	bio->bi_iter.bi_size = (cmd_list->cmd.rw.length + 1) << 9;
	/* Current idx of bvl_vec */
	bio->bi_iter.bi_idx = 0;
	/* Number of bytes to complete */
	bio->bi_iter.bi_bvec_done = 0;
}

/*
 * zicio_submit_bio
 *
 * Submit block io through zicio call path
 */
static unsigned int
zicio_submit_bio(struct bio *bio)
{
	struct request *rq;
	struct request_queue *q = bio->bi_bdev->bd_disk->queue;
	struct zicio_blk_mq_alloc_data data = {
		.q		= q,
	};

	BUG_ON(!bio->zicio_enabled);

	current->ioac.read_bytes += bio->bi_iter.bi_size;
	/*
	 * bio_list is a list used to suspend the bio structure to be split or 
	 * resubmitted in the bio layer. Since we do not perform merge, split, etc.,
	 * the corresponding BUG_ON should be checked.
	 */
	BUG_ON(current->bio_list);

	if (unlikely(zicio_bio_queue_enter(bio) != 0)) {
		return -2;
	}	

	if (!zicio_submit_bio_checks(bio)) {
		blk_queue_exit(data.q);
		return -2;
	}

	zicio_rq_qos_throttle(q, bio);

	data.cmd_flags = bio->bi_opf;

	rq = zicio_blk_mq_alloc_request(&data, bio);

	if (unlikely(!rq)) {
		zicio_rq_qos_cleanup(data.q, bio);
		blk_queue_exit(data.q);

		bio_wouldblock_error(bio);
		return -2;
	}

	rq->rq_flags |= RQF_DONTPREP;

	zicio_rq_qos_track(q, rq, bio);

	zicio_blk_mq_bio_to_request(rq, bio, 1);

	zicio_blk_mq_sched_insert_request(rq, false, true, false);

	return 0;
}

/*
 * __zicio_iomap_dio_submit_bio
 *
 * A function called to manage the status of submitted bio through dio.
 */
static void
__zicio_iomap_dio_submit_bio(const struct iomap_iter *iter,
			struct zicio_iomap_dio *dio, struct bio *bio, loff_t pos)
{
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	int cpu = get_cpu();
	put_cpu();
	printk("[Kernel Message] submit direct bio, cpu:%d\n",cpu);
#endif /* CONFIG_ZICIO_DEBUG */

	/*
	 * Before submit bio, increment reference counter and get its block device
	 * queue.
	 */ 
	atomic_inc(&dio->ref);
	dio->submit.last_queue = bdev_get_queue(bio->bi_bdev);

	/*
	 * Submit bio through zicio call path
	 */
	dio->submit.cookie = zicio_submit_bio(bio);
}

/*
 * zicio_iomap_dio_submit_bio
 *
 * Allocate and initialize bio and submit it
 */
static loff_t
zicio_iomap_dio_submit_bio(const struct iomap_iter *iter,
			struct zicio_iomap_dio *dio, struct file *filp)
{
	zicio_nvme_cmd_list * zicio_cmd = dio->iocb->zicio_cmd;
	struct device *dev;
	struct bio* bio;

	dev = zicio_get_inner_device_with_desc(dio->iocb->zicio_desc,
			zicio_cmd->device_idx);
	/* Allocate bio */
	bio = bio_alloc(GFP_KERNEL, 1);

	/* Set device to bio and set its data */
	bio_set_dev(bio, dev_to_bdev(dev));
	bio->bi_iter.bi_sector = zicio_cmd->cmd.rw.slba;

	/* If current command is metadata then, set priority higher */
	if (zicio_cmd->is_metadata) {
		bio->bi_ioprio = IOPRIO_CLASS_RT;
	} else {
		bio->bi_ioprio = IOPRIO_CLASS_BE;
	}

	/* Set dio to free it with callback */
	bio->bi_private = dio;
	/* Set callback function */
	bio->bi_end_io = zicio_iomap_dio_bio_end_io;
	/* Out request should not be merged and should be processed
	 * asynchronously */
	bio->bi_opf |= (REQ_NOMERGE|REQ_NOWAIT);

	/* Set zicio-related information contained in kiocb(kernel I/O control
	 * block to bio. */
	bio->zicio_enabled = dio->iocb->zicio_enabled;
	bio->zicio_desc = dio->iocb->zicio_desc;
	bio->zicio_from_softirq = dio->iocb->zicio_from_softirq;
	bio->zicio_cpu_id = dio->iocb->zicio_cpu_id;
	bio->zicio_cmd = dio->iocb->zicio_cmd;
	/*
	 * BIO_NO_PAGE_REF: The ref count for the data buffer is managed by
	 * zicio.
	 * BIO_REMAPPED: It is already set in consideration of the partition
	 * location in the command.
	 */
	bio_set_flag(bio, BIO_NO_PAGE_REF|BIO_REMAPPED);
	zicio_set_bio_vector(bio, bio->zicio_desc, bio->zicio_cmd);

	/*
	 * Submit bio through zicio direct I/O call path
	 */
	__zicio_iomap_dio_submit_bio(iter, dio, bio, iter->pos);

	return bio->bi_iter.bi_size;
}

/*
 * zicio_dio_read
 *
 * Allocate and initialize direct I/O structure for reads and call the functions
 * for bio submission
 */
static struct zicio_iomap_dio*
zicio_dio_read(zicio_descriptor *desc, struct file *filp,
			struct kiocb *iocb, struct iov_iter *iter)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct blk_plug plug;
	struct zicio_iomap_dio *dio;
	struct iomap_iter iomi = {
		.inode			= inode,
		.pos			= iocb->ki_pos,
		.len			= iov_iter_count(iter),
		.flags			= IOMAP_DIRECT,
	};
	bool wait_for_completion = false; //TODO: It should be moved to param

	if (unlikely(zicio_ext4_forced_shutdown(
			ZICIO_EXT4_SB(inode->i_sb)))) {
		return ERR_PTR(-EIO);
	}

	if (!iov_iter_count(iter)) 
		return NULL; /* skip atime */

	/* Allocate direct io structure */
	dio = zicio_get_dio();

	if (unlikely(!dio)) {
		return ERR_PTR(-ENOMEM);
	}

	/* Initialize direct io structure */
	memset(dio, 0, sizeof(*dio));
	dio->iocb = iocb;
	atomic_set(&dio->ref , 1);
	dio->size = 0;
	dio->i_size = i_size_read(inode);
	dio->dops = NULL;
	dio->error = 0;
	dio->flags = 0;

	/* Initialize direct io submit information and
	 * synchronous I/O (Previously used for testing purposes.) */
	dio->submit.iter = iter;
	dio->submit.waiter = current;
	dio->submit.cookie = BLK_QC_T_NONE;
	dio->submit.last_queue = NULL;

	/*
	 * If the area currently being read/written is later than the total file
	 * size managed by inode, it is returned immediately.
	 */
	if (iocb->ki_pos >= dio->i_size) {
		goto l_zicio_dio_read_out_free_dio;
	}

	blk_start_plug(&plug);
	
	dio->wait_for_completion = wait_for_completion;

	iomi.processed = zicio_iomap_dio_submit_bio(&iomi, dio, filp);

	blk_finish_plug(&plug);

	WRITE_ONCE(iocb->ki_cookie, dio->submit.cookie);
	WRITE_ONCE(iocb->private, dio->submit.last_queue);

	if (!atomic_dec_and_test(&dio->ref)) {
		if (!wait_for_completion) {
			return dio;
		}
		for (;;) {
			set_current_state(TASK_UNINTERRUPTIBLE);

			if (!READ_ONCE(dio->submit.waiter))
				break;

			if (!(iocb->ki_flags & IOCB_HIPRI) ||
				!dio->submit.last_queue ||
				!blk_poll(dio->submit.last_queue,
					dio->submit.cookie, true)) {
				blk_io_schedule();
			}
		}
		__set_current_state(TASK_RUNNING);
	}

	return dio;
l_zicio_dio_read_out_free_dio:
	zicio_free_dio(dio);

	return NULL;
}

/*
 * zicio_init_iov_iter
 *
 * Initialize io vector that manages DRAM and block locations
 */
static void
zicio_init_iov_iter(struct iov_iter *i, unsigned int direction,
			const struct bio_vec* bio_vec, unsigned long nr_segs,
			size_t count)
{
	WARN_ON(direction & ~(READ | WRITE));
	*i = (struct iov_iter) {
		.iter_type = ITER_KVEC,
		.data_source = direction,
		.bvec = bio_vec,
		.nr_segs = nr_segs,
		.iov_offset = 0,
		.count = count
	};
}

/*
 * __zicio_read_trigger
 *
 * Allocate and initialize kiocb(kernel I/O control block)
 * Set DRAM and block location and zicio-related information
 */
static ssize_t 
__zicio_read_trigger(zicio_descriptor * desc, struct file *filp,
			char *data_buf, size_t len, loff_t *ppos, 
			zicio_nvme_cmd_list *cmd, bool from_irq, unsigned int cpu_id)
{
	/* Init bio vector of current dio */
	struct bio_vec bio_vec = { .bv_page = virt_to_page(data_buf),
							   .bv_len = len,
							   .bv_offset = 0 };
	/* Allocate kiocb */
	struct kiocb *kiocb = kmalloc(sizeof(struct kiocb), GFP_KERNEL);
	struct iov_iter iter;
	struct zicio_iomap_dio* dio;

	if (unlikely(!kiocb)) {
		return -2;
	}

	/* Initialize kiocb's file information and the members for zicio */
	init_sync_kiocb(kiocb, filp);
	kiocb->zicio_enabled = true;
	kiocb->zicio_desc = desc;
	kiocb->zicio_from_softirq = from_irq;

	if (from_irq) {
		kiocb->zicio_cpu_id = cpu_id;
	}

	if (cmd) {
		kiocb->zicio_cmd = cmd;
	}

	kiocb->ki_pos = (ppos ? *ppos : 0);
	zicio_init_iov_iter(&iter, READ, &bio_vec, 1, len);

	/* Perform direct I/O using kiocb */
	dio = zicio_dio_read(desc, filp, kiocb, &iter);

	if (IS_ERR_OR_NULL(dio)) {
		kfree(kiocb);
		printk(KERN_WARNING "[Kernel Message] Error in dio\n");
		return -2;
	}

	/* dio is freed when bio sumbission error is handled with bio_endio call
	 * back function. so we do not free bio and dio */
	if (kiocb->ki_cookie == -2U) {
		return -2;
	}

	return 0;
}

/*
 * zicio_do_read_trigger
 *
 * Performs dio after verifying information related to the file to be performed
 * i/o.
 */
static ssize_t
zicio_do_read_trigger(zicio_descriptor *desc, struct file *file,
		char *data_buf, size_t count, loff_t *pos,
		zicio_nvme_cmd_list *cmd, bool from_soft_irq, unsigned cpu_id)
{
	ssize_t ret;

	/* Checking if this file is readable */
	if (!(file->f_mode & FMODE_READ)) {
		printk("[Kernel Message] Read mode error\n");
		return -EBADF;
	}

	if (!(file->f_mode & FMODE_CAN_READ)) {
		printk("[Kernel Message] Read mode error\n");
		return -EINVAL;
	}

	/* Verifying file area for reading */
	ret = zicio_rw_verify_area(READ, file, pos, count);

	if (ret) {
		printk("[Kernel Message] Invalid file area\n");
		return ret;
	}

	/* Checking if the valid RW range is exceeded */
	if (count > MAX_RW_COUNT)
		count = MAX_RW_COUNT;


	/*
	 * ext4 uses the read_iter hook instead of the read hook to perform I/O.
	 * We assume that the file system is ext4.
	 * Because of this, an error is returned if a read callback exists or if
	 * there is no read_iter callback.
	 */
	if (file->f_op->read) {
		printk(KERN_WARNING "[Kernel Message] File is not located in ext4\n");
		return -EINVAL;
	} else if (file->f_op->read_iter) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "[Kernel Message] Read triggering start\n");
#endif
		ret = __zicio_read_trigger(desc, file, data_buf, count, pos, cmd,
				from_soft_irq, cpu_id);
	} else {
		printk(KERN_WARNING "[Kernel Message] Relevant read calls isn't " 
					"registered\n");
		return -EINVAL;
	}

	if (ret > 0) {
		fsnotify_access(file);
		add_rchar(current, ret);
	}
	inc_syscr(current);
	return ret;
}

/*
 * zicio_read_trigger
 *
 * When a softirq other than a handler or the first read is performed, the call
 * path starts from this point. Set the variables necessary for verification and
 * start reading using them.
 */
static ssize_t
zicio_read_trigger(zicio_descriptor *desc, struct fd fd,
			zicio_nvme_cmd_list *cmd, bool from_soft_irq, unsigned cpu_id)
{
	loff_t pos;
	ssize_t ret = -1;
	struct file* file = fd.file;
	char *buf;
	ssize_t count;

	BUG_ON(file == NULL);
	if (file->f_mode & FMODE_PREAD) {
		/* Set zicio channel's buffer and its init a request info and then,
		 * trigger it */
		buf = (char *)cmd->start_mem;
		count = (le16_to_cpu(cmd->cmd.rw.length) + 1) <<
					ZICIO_NVME_SECTOR_SHIFT;
		pos = cmd->start_lpos * ZICIO_PAGE_SIZE;
		ret = zicio_do_read_trigger(desc, file, buf, count, &pos, cmd,
				from_soft_irq, cpu_id);
	} else {
		printk(KERN_WARNING "[Kernel Message] unexpected mode set\n");
	}

	return ret;
}

/*
 * zicio_read_trigger_from_softirq
 *
 * start zicio dio from softirq
 */
ssize_t
zicio_trigger_read_from_softirq(zicio_descriptor *desc,
			zicio_nvme_cmd_list *cmd, unsigned cpu_id)
{
	if (!cmd) {
		return -EINVAL;
	}

	return zicio_read_trigger(desc, cmd->fd, cmd, true, cpu_id);
}

/*
 * zicio_init_read_trigger
 *
 * Init read trigger for zicio
 */
long
zicio_init_read_trigger(long id)
{
	zicio_descriptor *sd = zicio_get_desc(id);
	zicio_notify_descriptor *zicio_notify_desc = (zicio_notify_descriptor*)sd;
	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_page_control_block *zicio_spcb;
	int global_device_idx;
	size_t requested = 0;
	zicio_nvme_cmd_list **start_cmd_lists, *next_nvme_cmd;
	zicio_nvme_cmd_info nvme_cmd_info;
	int local_page_idx, num_device, list_idx, device_idx;
	unsigned int current_head = 0, page_id_queue_idx = -1;
	int num_wait_consumption;
	bool derailed;

	if (IS_ERR(sd)) {
		printk("[Kernel Message] Cannot get descriptor using id\n");
		return -EINVAL;
	}

	/*
	 * Setting the necessary information(files, chunk id, local page idx
	 * for reading.
	 */
	if (sd->zicio_shared_pool_desc) {
		zicio_shared_pool = zicio_get_shared_pool(sd);
		/*
		 * If this channel is shared channel, then this channel should get
		 * pages from shared pool. For this, do premapping first.
		 */
		num_wait_consumption = zicio_do_init_premapping(sd, &current_head);

		/*
		 * If this channel can get enough pages to consume, then do not need to
		 * perform I/O.
		 */
		if (num_wait_consumption) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
			zicio_create_reactivate_trigger_shared(
					sd, ZICIO_NOIO, false, -1, false, 0);
#else
			zicio_create_reactivate_trigger_shared(
					sd, ZICIO_NOIO, false, -1, false);
#endif
			return 0;
		} else {
			/*
			 * If this channel cannot get enough pages to consume, then do I/O.
			 * For this, get an empty huge page from attached shared pool.
			 */
			local_page_idx = zicio_get_next_local_huge_page_id_shared(sd,
					&page_id_queue_idx, &derailed, false);
		}
		BUG_ON(derailed);
		/*
		 * If the pages of channel is exhausted, currently
		 * wait until we can get a huge page.
		 */
		if (local_page_idx == ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX) {
			BUG_ON(page_id_queue_idx == -1);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
			zicio_create_reactivate_trigger_shared(sd,
						ZICIO_NOLOCALPAGE, false, page_id_queue_idx, false,
						ktime_get());
#else
			zicio_create_reactivate_trigger_shared(sd,
						ZICIO_NOLOCALPAGE, false, page_id_queue_idx, false);
#endif
			return 0;
		}

		/*
		 * We can fetch a shared page from shared pool. Now, spcb matched with
		 * the allocated page should be initialized.
		 */
		zicio_spcb = (zicio_shared_page_control_block *)atomic64_read(
				zicio_shared_pool->zicio_spcb + local_page_idx);

		zicio_spcb->zicio_spcb.filled_pages_per_local_huge_page.counter = 0;
		zicio_spcb->zicio_spcb.needed_pages_per_local_huge_page.counter = 512;
		zicio_spcb->zicio_spcb.requested_flag_per_local_huge_page.counter = 1;

		BUG_ON(local_page_idx < 0);
		/*
		 * Create command for shared channel
		 */
		start_cmd_lists = zicio_create_command_shared(sd, local_page_idx,
				&num_device, true);
	} else {
		local_page_idx = ZICIO_LOCAL_INIT_PAGE_IDX;
		BUG_ON(local_page_idx < 0);
		BUG_ON(local_page_idx >= ZICIO_DATABUFFER_CHUNK_NUM);
		/* Create a command list for the first I/O. */
		if (zicio_notify_desc->nr_fd_of_batches != 0) {
			BUG_ON(atomic_read(zicio_notify_desc->buf_desc.round));
			unsigned long start_mem =
				(unsigned long)sd->buffers.data_buffer[0];
			nvme_cmd_info = zicio_notify_desc->nvme_cmd_infos[0][0];
			start_cmd_lists = zicio_notify_create_command(sd, 0, nvme_cmd_info,
												   start_mem);
			num_device = 1;
			atomic_set(zicio_notify_desc->buf_desc.round, 1);
		} else {
			start_cmd_lists = zicio_create_command(sd, local_page_idx,
					&num_device, false);
		}
	}

	if (unlikely(!start_cmd_lists)) {
		return -EINVAL;
	}

	for (list_idx = 0 ; list_idx < num_device ; list_idx++) {
		/* Pending the commands except the first command on a timer wheel */
		next_nvme_cmd = start_cmd_lists[list_idx]->next;
		start_cmd_lists[list_idx]->next = NULL;
		global_device_idx = zicio_get_zicio_global_device_idx(sd,
				start_cmd_lists[list_idx]->device_idx);

		if (zicio_notify_desc->nr_fd_of_batches != 0) {
			unsigned int nvme_cmd_info_start, nvme_cmd_info_end;
			int nr_remained_nvme_cmd_info;

			nvme_cmd_info_start = zicio_notify_desc->nvme_cmd_info_start_offsets[0][0];

			if (zicio_notify_desc->nr_nvme_cmd_info_start_offsets == 1) {
				nvme_cmd_info_end
					= zicio_notify_desc->nr_nvme_cmd_infos - 1;
			} else {
				nvme_cmd_info_end
					= zicio_notify_desc->nvme_cmd_info_start_offsets[0][1] - 1;
			}

			/* the first one is already in zicio_cmd */
			nr_remained_nvme_cmd_info = nvme_cmd_info_end - nvme_cmd_info_start;
			if (nr_remained_nvme_cmd_info > 0) {
				zicio_init_trigger_insert_nvme_cmd_timer(zicio_notify_desc,
					nr_remained_nvme_cmd_info,
					ZICIO_NVME_CMD_INFO_GET_LENGTH(nvme_cmd_info) + 1);
			}
		} else {
			if (next_nvme_cmd) {
				__zicio_lock_and_load(sd, global_device_idx, next_nvme_cmd,
										  ktime_get());
			}
		}

		/* Try to get new request and notice it to the flow controller */
		if (zicio_notify_desc->nr_fd_of_batches != 0)
			zicio_init_trigger_flow_control(zicio_notify_desc);
		else
			zicio_request_flow_in(sd, global_device_idx);

		if ((requested = zicio_read_trigger(sd,
				start_cmd_lists[list_idx]->fd, start_cmd_lists[list_idx],
						true, sd->cpu_id)) < 0) {

			if (zicio_notify_desc->nr_fd_of_batches != 0)
				zicio_init_trigger_rollback_flow_control(zicio_notify_desc);
			else
				zicio_request_flow_out(sd, global_device_idx);

			break;
		}
	}
	device_idx = zicio_get_zicio_channel_dev_idx(sd,
			start_cmd_lists[0]->device_idx);
	zicio_free_cmd_lists_set_with_desc(sd, device_idx,
			start_cmd_lists);
	return requested;
}
