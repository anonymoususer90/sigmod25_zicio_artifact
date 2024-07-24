#ifndef	_LINUX_MD_ZICIO_FLOW_CTRL_H
#define	_LINUX_MD_ZICIO_FLOW_CTRL_H

#include <linux/blkdev.h>
#include <linux/ktime.h>
#include <linux/zicio_notify.h>

#define ZICIO_SOFTTIMER_IRQ_CYCLE		(0)
#define ZICIO_SOFTTIMER_TIMER_SOFTIRQ	(1)
#define ZICIO_SOFTTIMER_IDLE_LOOP		(2)

extern int zicio_do_softtimer_timer_softirq(int cpu, int dev_idx);

extern int zicio_do_softtimer_idle_loop(int cpu, int dev_idx);

extern void zicio_do_softtimer_irq_cycle(struct request *req);

extern void zicio_update_flow_ctrl(struct request *req, u32 queue_depth);

extern int zicio_descriptor_flow_in(
		struct zicio_descriptor *zicio_desc, int global_device_idx);

extern void zicio_descriptor_flow_out(
		struct zicio_descriptor *zicio_desc, int global_device_idx,
		int channel_device_idx);

extern void zicio_request_flow_in(
		struct zicio_descriptor *zicio_desc, int global_device_idx);

extern void zicio_request_flow_out(
		struct zicio_descriptor *zicio_desc, int global_device_idx);

extern ktime_t zicio_get_io_time(
		int nr_nvme_pages, int cpu, int global_device_idx);

extern ktime_t zicio_calc_release_time(struct zicio_descriptor *zicio_desc,
		int device_idx, int huge_page_id, ktime_t know, ktime_t io_time,
		bool is_shared_cmd);

extern ktime_t zicio_tsc_to_ktime(unsigned long tsc);

extern void zicio_update_shared_pool_avg_ingestion_time(
		struct zicio_descriptor *zicio_desc, ktime_t know);

extern int zicio_get_current_io_size(
		struct zicio_descriptor *zicio_desc, int channel_device_idx);

extern int zicio_get_current_io_size_for_md(
		struct zicio_descriptor *zicio_desc, int channel_device_idx);

extern void __init zicio_init_md_flow_controller(int num_dev);

extern void zicio_dump_md_flow_ctrl(int cpu, int global_device_idx);
#endif	/* _LINUX_MD_ZICIO_FIREHOSE_CTRL_H */
