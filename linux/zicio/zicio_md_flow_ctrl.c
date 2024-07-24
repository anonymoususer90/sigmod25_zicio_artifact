#include <asm/timer.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpuset.h>
#include <linux/irq_work.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/irqflags.h>

#include "zicio_cmd.h"
#include "zicio_device.h"
#include "zicio_req_timer.h"
#include "zicio_firehose_ctrl.h"
#include "zicio_md_flow_ctrl.h"
#include "zicio_shared_pool.h"
#include "zicio_shared_pool_mgr.h"
#include "zicio_req_submit.h"

#define ZICIO_NUM_IO_SIZE_TYPE	(5)
#define ZICIO_512K_IDX	(4)
#define ZICIO_256K_IDX	(3)
#define ZICIO_128K_IDX	(2)
#define ZICIO_64K_IDX	(1)
#define ZICIO_32K_IDX	(0)

#define ZICIO_CHUNK_TO_MIN_SIZE_SHIFT (6)
#define ZICIO_MAX_REQ_COUNT(nr_req)	(nr_req - ((nr_req) >> 2))

#define ZICIO_CONTROL_PERIOD_TICK (64)
#define ZICIO_CONTROL_PERIOD_MASK (ZICIO_CONTROL_PERIOD_TICK - 1)

#define ZICIO_PBR_PERIOD_MSEC (100)
#define ZICIO_PBR_TOLERANCE (5)
#define ZICIO_DEVICE_SATURATED_POINT(nr_cpu) (nr_cpu >> 1)
#define ZICIO_IO_TIME_SATURATED_POINT(io_time) (io_time)

#define ZICIO_SHARED_UPDATE_AVG_PERIOD_MSEC (100)

#define ZICIO_MASK_MAX_NUM_LOCAL_HUGE_PAGE (ZICIO_MAX_NUM_CHUNK - 1)

struct zicio_flow_ctrl {
	raw_spinlock_t		lock;
	u64					tick;
	int					cpu;
	int					global_device_idx;
	int					pbr_div;
	int					pbr_mult;
	int					pbr_delay_on;
	int					pbr_prefire_on;
	int					pbr_tolerance_count;
	atomic_t			getting_req;
	int					cur_req_count;
	int					min_req_count;
	int					zicio_channel_count;
	s64					user_bandwidth;
	s64					device_bandwidth;
	ktime_t				last_pbr_check_time;
	int					recounting_enabled;
	int					rate_adaptive_enabled;
	int					cur_io_idx;
	int					io_size[ZICIO_NUM_IO_SIZE_TYPE];
	ktime_t				io_time_ema[ZICIO_NUM_IO_SIZE_TYPE];
	ktime_t				io_time_max[ZICIO_NUM_IO_SIZE_TYPE];
	ktime_t				io_time_total_avg[ZICIO_NUM_IO_SIZE_TYPE];
	u64					io_time_measurment_count[ZICIO_NUM_IO_SIZE_TYPE];
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	unsigned long		request_recounting_call_counter;
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
	ktime_t (*calc_io_time_ema[ZICIO_NUM_IO_SIZE_TYPE])(ktime_t, ktime_t);
} ____cacheline_aligned;

static DEFINE_PER_CPU(struct zicio_flow_ctrl*, flow_ctrl);

/**
 * zicio_get_io_size_idx - get index of I/O size
 * @nr_nvme_pages: the number of nvme pages
 *
 * Get index of I/O size according to given number of nvme pages.
 *
 * Since the flow controller does not keep track of all sizes, the index is
 * determined based on the nearest value.
 */
static int zicio_get_io_size_idx(int nr_nvme_pages)
{
	if (nr_nvme_pages > 64) {
		return ZICIO_512K_IDX;
	} else if (nr_nvme_pages > 32) {
		return ZICIO_256K_IDX;
	} else if (nr_nvme_pages > 16) {
		return ZICIO_128K_IDX;
	} else if (nr_nvme_pages > 8) {
		return ZICIO_64K_IDX;
	} else {
		return ZICIO_32K_IDX;
	}
}

/**
 * zicio_tsc_to_ktime - convert tsc to ktime_t
 * @tsc: tsc value to convert
 * 
 * The switch board has an average tsc for a user to consume one huge page.
 * To set the request timer's expiration time, change it to ktime_t.
 *
 * Note that we assuming that zicio is using the x86 architecture.
 */
ktime_t zicio_tsc_to_ktime(unsigned long tsc)
{
	struct cyc2ns_data data;
	u64 ns;
	cyc2ns_read_begin(&data);
	ns = mul_u64_u32_shr(tsc, data.cyc2ns_mul, data.cyc2ns_shift);
	cyc2ns_read_end();
	return ns_to_ktime(ns);
}
EXPORT_SYMBOL(zicio_tsc_to_ktime);


/**
 * zicio_calc_huge_page_distance - calculate distance between two huge pages.
 * @c1: start huge page
 * @c2: end huge page
 *
 * Calculate how many huge pages @c2 is from @c1 in the data buffer the user is
 * loocking at.
 */
static int zicio_calc_huge_page_distance(int c1, int c2)
{
	if (c1 < c2) {
		return c2 - c1 - 1;
	} else {
		return ZICIO_MAX_NUM_CHUNK - c1 - 1 + c2;
	}
}

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
 * __zicio_calc_user_arrival_interval - get arrival interval
 * @avg_ktime_delta: average huge page consumption time
 * @user_consumed: indicates the huge page the user is in
 * @new_huge_page_id: newly requested local huge page id
 *
 * Get the time it takes to reach the new local huge page, based on how far 
 * the new local huge page differs from the huge page the user is in.
 */
static inline ktime_t
__zicio_calc_user_arrival_interval(
	struct zicio_descriptor *zicio_desc, int new_huge_page_id)
{
	int cur_huge_page_id, huge_page_id_delta;
	struct zicio_switch_board *sb = zicio_desc->switch_board;
	ktime_t avg_ktime_delta = zicio_tsc_to_ktime(sb->avg_tsc_delta);

	if (sb->consumed == 0)
		return 0;

	cur_huge_page_id = zicio_calc_local_huge_page_idx(sb->consumed - 1);
	huge_page_id_delta = zicio_calc_huge_page_distance(
		cur_huge_page_id, new_huge_page_id);

	return huge_page_id_delta * avg_ktime_delta;
}

/**
 * zicio_calc_user_arrival_interval_shared - get arrival interval
 * @zicio_desc: zicio descriptor
 * 
 * Unlike the local mode, it is not determined at which point on the user's data
 * buffer the currently requested huge page will be mapped.
 *
 * Therefore, it is meaningless to consider the distance from the user's current
 * point. Inevitably, only the time of one huge page is considered.
 */
static inline ktime_t 
__zicio_calc_user_arrival_interval_shared(
	struct zicio_descriptor *zicio_desc)
{
	return zicio_tsc_to_ktime(
		zicio_get_shared_pool_consumption_tsc_delta(zicio_desc));
}

/**
 * zicio_calc_user_arrival_interval - get arrival interval to the given data
 * @zicio_desc: zicio descriptor
 * @huge_page_id: newly requesting local huge page id
 * @is_shared: is this command for shared data?
 *
 * The switch board has user's consumption speed.
 * Using it, calculate how long will take the user to reach a given huge page.
 */
static inline ktime_t zicio_calc_user_arrival_interval(
	struct zicio_descriptor *zicio_desc, int huge_page_id, bool is_shared)
{
	if (zicio_desc->zicio_shared_pool_desc && is_shared) {
		return __zicio_calc_user_arrival_interval_shared(zicio_desc);
	} else {
		return __zicio_calc_user_arrival_interval(zicio_desc, huge_page_id);
	}
}

/**
 * zicio_calc_release_time - calculate release time of nvme commands
 * @zicio_desc: zicio descriptor
 * @global_device_idx: global raw device index
 * @huge_page_id: newly requested local huge page id
 * @know: current ktime_t
 * @io_time: how much I/O time is required
 * @is_shared_cmd: is this command for shared data?
 *
 * Calculate when the nvme commands must be submitted to the device.
 *
 * Note that this function is only called for chunk data nvme commands.
 */
ktime_t zicio_calc_release_time(struct zicio_descriptor *zicio_desc,
	int global_device_idx, int huge_page_id, ktime_t know, ktime_t io_time,
	bool is_shared_cmd)
{
	struct zicio_flow_ctrl *ctrl = zicio_get_per_cpu_ptr_with_dev(
			flow_ctrl, zicio_desc->cpu_id, global_device_idx);
	ktime_t interval = 0;

	interval += zicio_calc_user_arrival_interval(
			zicio_desc, huge_page_id, is_shared_cmd);
	interval -= io_time;

	if (ctrl->pbr_delay_on) {
		/* 
		 * ->pbr_div is a variable used to divide the number of requests, but
		 * here it is used to reduce bandwidth usage by increasing the release
		 * time.
		 */
		interval *= ctrl->pbr_div;
	} else if (ctrl->pbr_prefire_on) {
		/*
		 * ->pbr_mult is a variable used to multiple the number of requests, but
		 * here it is used to increase bandwidth usage by reducing the relase
		 * time.
		 */
		interval /= ctrl->pbr_mult;
	}

	return ktime_add(know, interval);
}
EXPORT_SYMBOL(zicio_calc_release_time);

/**
 * zicio_calc_bandwidth - how many chunk the user can consume
 * @t: user's consumption time for each chunk
 *
 * Return how many chunks the user can consume during the standard amount of
 * time. The standard time is KTIME_MAX.
 */
static inline s64 zicio_calc_bandwidth(ktime_t t)
{
	return (KTIME_MAX / t);
}

/**
 * zicio_reset_user_bandwidth - update user bandwidth
 * @ctrl: data buffer contoller
 * @avg_tsc_delta: average time spent consuming huge page
 * 
 * Update bandwidth based on user's consumption speed.
 */
static inline void zicio_reset_user_bandwidth(
	struct zicio_descriptor *zicio_desc, struct zicio_firehose_ctrl *ctrl,
	int channel_device_idx, unsigned long avg_tsc_delta)
{
	int channel_raw_device_idx = zicio_get_zicio_channel_raw_dev_idx(zicio_desc,
			channel_device_idx);
	ktime_t chunk_consumption_time = zicio_tsc_to_ktime(avg_tsc_delta);
	if (chunk_consumption_time) {
		ctrl->bandwidth = zicio_calc_bandwidth(chunk_consumption_time);
		ctrl->last_update_bandwidth[channel_raw_device_idx] = ctrl->bandwidth;
		ctrl->last_user_avg_tsc_delta = avg_tsc_delta;
	}
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
 * XXX: This implementation assumes that NVMe command are requested in NVMe page
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
 * __zicio_calc_io_time_ema_32k - calculate new average for 32k size
 * @ema: old ema
 * @new: new value
 *
 * Use EMA to find the average elapsed time.
 *
 * Here we use the "e" as 1/1024, it can be changed in later.
 * EMA(t) = val(t) * (1/1024) + (1 - 1/1024) * EMA(t-1)
 */
static inline ktime_t
__zicio_calc_io_time_ema_32k(ktime_t ema, ktime_t new)
{
	return ((new << 1) + 2046 * ema) >> 11;
}

/**
 * __zicio_calc_io_time_ema_64k - calculate new average for 64k size
 * @ema: old ema
 * @new: new value
 *
 * Use EMA to find the average elapsed time.
 *
 * Here we use the "e" as 1/512, it can be changed in later.
 * EMA(t) = val(t) * (1/512) + (1 - 1/512) * EMA(t-1)
 */
static inline ktime_t
__zicio_calc_io_time_ema_64k(ktime_t ema, ktime_t new)
{
	return ((new << 2) + 2044 * ema) >> 11;
}

/**
 * __zicio_calc_io_time_ema_128k - calculate new average for 128k size
 * @ema: old ema
 * @new: new value
 *
 * Use EMA to find the average elapsed time.
 *
 * Here we use the "e" as 1/256, it can be changed in later.
 * EMA(t) = val(t) * (1/256) + (1 - 1/256) * EMA(t-1)
 */
static inline ktime_t
__zicio_calc_io_time_ema_128k(ktime_t ema, ktime_t new)
{
	return ((new << 3) + 2040 * ema) >> 11;
}

/**
 * __zicio_calc_io_time_ema_256k - calculate new average for 256k size
 * @ema: old ema
 * @new: new value
 *
 * Use EMA to find the average elapsed time.
 *
 * Here we use the "e" as 1/128, it can be changed in later.
 * EMA(t) = val(t) * (1/128) + (1 - 1/128) * EMA(t-1)
 */
static inline ktime_t
__zicio_calc_io_time_ema_256k(ktime_t ema, ktime_t new)
{
	return ((new << 4) + 2032 * ema) >> 11;
}

/**
 * __zicio_calc_io_time_ema_512k - calculate new average for 512k size
 * @ema: old ema
 * @new: new value
 *
 * Use EMA to find the average elapsed time.
 *
 * Here we use the "e" as 1/64, it can be changed in later.
 * EMA(t) = val(t) * (1/64) + (1 - 1/64) * EMA(t-1)
 */
static inline ktime_t
__zicio_calc_io_time_ema_512k(ktime_t ema, ktime_t new)
{
	return ((new << 5) + 2016 * ema) >> 11;
}

/**
 * zicio_calc_io_time_ema - calcualte EMA of I/O time for given index
 * @ctrl: flow controller
 * @idx: I/O size index
 * @new_io_time: new elapsed time
 *
 * The user's consumption rate is based on the average consumption of each huge
 * page for a given 16 huge pages.
 *
 * Therefore, when calculating the average of the I/O consumption time,
 * different criteria must be applied according to the I/O size.
 */
static inline ktime_t zicio_calc_io_time_ema(
	struct zicio_flow_ctrl *ctrl, int idx, ktime_t new_io_time)
{
	return ctrl->calc_io_time_ema[idx](ctrl->io_time_ema[idx], new_io_time);
}

/**
 * zicio_calc_io_time_total_avg - calculate total average of I/O time
 * @ctrl: flow controller
 * @idx: I/O size index
 * @new_io_time: new elapsed time
 *
 * The overall average of I/O time is used as a criterion for starting PBR.
 */
static inline ktime_t zicio_calc_io_time_total_avg(
	struct zicio_flow_ctrl *ctrl, int idx, ktime_t new_io_time)
{
	ktime_t total_io_time_sum = 
		ctrl->io_time_total_avg[idx] * ctrl->io_time_measurment_count[idx]++;
	return ((total_io_time_sum + new_io_time) / 
		ctrl->io_time_measurment_count[idx]);
}

/**
 * zicio_test_and_inc_request_count - test and increase request count
 * @ctrl: flow controller
 *
 * Check whether a new requests are needed. If so, indicate that we will get
 * new requests by incrementing the reuqest counter.
 *
 * If the attempt to get the new request fails, we have to call
 * zicio_dec_request_count() to rollback the state.
 *
 * Return how many request is needed.
 */
static int zicio_test_and_inc_request_count(
	struct zicio_flow_ctrl *ctrl)
{
	unsigned long flags;
	int ret = 0;
	raw_spin_lock_irqsave(&ctrl->lock, flags);
	if (ctrl->cur_req_count < ctrl->min_req_count) {
		ret = ctrl->min_req_count - ctrl->cur_req_count;
		ctrl->cur_req_count = ctrl->min_req_count;
	}
	raw_spin_unlock_irqrestore(&ctrl->lock, flags);
	return ret;
}

/**
 * zicio_dec_request_count - decrease request count
 * @ctrl: flow controller
 * @count: amount to decrease
 *
 * If we failed to get new requests, we have to call this function
 * to rollback the state.
 */
static void zicio_dec_request_count(
	struct zicio_flow_ctrl *ctrl, int count)
{
	unsigned long flags;
	raw_spin_lock_irqsave(&ctrl->lock, flags);
	ctrl->cur_req_count -= count;
	BUG_ON(ctrl->cur_req_count < 0);
	raw_spin_unlock_irqrestore(&ctrl->lock, flags);
}

/**
 * zicio_test_and_dec_request_count - test and decrease request count
 * @ctrl: flow controller
 *
 * Check whetehr the request need to be droped. If so, decrease request count
 * and return 1. Otherwise return 0.
 */
static int zicio_test_and_dec_request_count(
	struct zicio_flow_ctrl *ctrl)
{
	unsigned long flags;
	int ret = 0;
	raw_spin_lock_irqsave(&ctrl->lock, flags);
	if (ctrl->cur_req_count > ctrl->min_req_count) {
		ctrl->cur_req_count--;
		BUG_ON(ctrl->cur_req_count < 0);
		ret = 1;
	}
	raw_spin_unlock_irqrestore(&ctrl->lock, flags);
	return ret;
}

/**
 * zicio_flow_ctrl_tick - increase flow controller's tick
 * @ctrl: flow controller
 *
 * Increase the tick to manage contol period. If a control period is reached,
 * turn on the flag.
 */
static inline void
zicio_flow_ctrl_tick(struct zicio_flow_ctrl *ctrl)
{
	unsigned long flags;
	raw_spin_lock_irqsave(&ctrl->lock, flags);
	if ((ctrl->tick++ & ZICIO_CONTROL_PERIOD_MASK) == 0)
		ctrl->recounting_enabled = 1;
	raw_spin_unlock_irqrestore(&ctrl->lock, flags);
}

#ifdef CONFIG_ZICIO_STAT
/**
 * zicio_count_softtimer - update stat board of zicio
 * @zicio_desc: zicio descriptor
 * @type: type of softtimer
 *
 * ZicIO has stat board and track how many softtimers are used. Update it.
 */
static inline void zicio_count_softtimer(
	struct zicio_descriptor *zicio_desc, int type)
{
	BUG_ON(zicio_desc->stat_board == NULL);

	switch (type) {
		case ZICIO_SOFTTIMER_IRQ_CYCLE:
			zicio_desc->stat_board->io_interrupt_cnt++;
			break;
		case ZICIO_SOFTTIMER_TIMER_SOFTIRQ:
			zicio_desc->stat_board->soft_irq_cnt++;
			break;
		case ZICIO_SOFTTIMER_IDLE_LOOP:
			zicio_desc->stat_board->cpu_idle_loop_cnt++;
			break;
		default:
			BUG_ON(true);
	}

	if (zicio_desc->zicio_shared_pool_desc) {
		if (zicio_check_channel_derailed(zicio_desc)) {
			zicio_desc->stat_board->io_derailed++;
			switch (type) {
				case ZICIO_SOFTTIMER_IRQ_CYCLE:
					zicio_desc->stat_board->io_interrupt_cnt_derailed++;
					break;
				case ZICIO_SOFTTIMER_TIMER_SOFTIRQ:
					zicio_desc->stat_board->soft_irq_cnt_derailed++;
					break;
				case ZICIO_SOFTTIMER_IDLE_LOOP:
					zicio_desc->stat_board->cpu_idle_loop_cnt_derailed++;
					break;
				default:
					BUG_ON(true);
			}
		} else {
			zicio_desc->stat_board->io_on_track++;
			switch (type) {
				case ZICIO_SOFTTIMER_IRQ_CYCLE:
					zicio_desc->stat_board->io_interrupt_cnt_on_track++;
					break;
				case ZICIO_SOFTTIMER_TIMER_SOFTIRQ:
					zicio_desc->stat_board->soft_irq_cnt_on_track++;
					break;
				case ZICIO_SOFTTIMER_IDLE_LOOP:
					zicio_desc->stat_board->cpu_idle_loop_cnt_on_track++;
					break;
				default:
					BUG_ON(true);
			}
		}
	}
}
#endif /* (CONFIG_ZICIO_STAT) */

/**
 * __zicio_acquire_new_requests - try to acquire new requests
 * @cpu: cpu id
 * @global_device_idx: global raw device index
 * @type: type of softtimer
 *
 * Return 0 if retry is required. Otherwise return 1.
 */
static int __zicio_acquire_new_requests(
	int cpu, int global_device_idx, int type)
{
	struct zicio_nvme_cmd_desc zicio_cmd_desc;
	struct zicio_request_timer *req_timer;
	struct zicio_flow_ctrl *ctrl;
	int i, count, retry = 0, ret = 1;

	ctrl = zicio_get_per_cpu_ptr_with_dev(
		flow_ctrl, cpu, global_device_idx);

	if (!ctrl)
		return ret;

	zicio_flow_ctrl_tick(ctrl);

	/* Only allow one execution flow to avoid unexpected bugs */
	BUG_ON(raw_smp_processor_id() != cpu);
	if (atomic_cmpxchg(&ctrl->getting_req, 0, 1) != 0)
		return 1;

	/* How many new requests should we get? */
	count = zicio_test_and_inc_request_count(ctrl);
	if (!count)
		goto out_getting_req;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu[%d] __zicio_acquire_new_requests [%s:%d][%s]\n", 
			cpu, __FILE__, __LINE__, __FUNCTION__);
#endif

	/* We need to get new requests as much as @count */
	for (i = 0; i < count; i++) {
		zicio_fetch_nvme_command_desc(
			&zicio_cmd_desc, 0, cpu, global_device_idx);

		/* If there is a fetched nvme command, try submit it */
		if (zicio_cmd_desc.zicio_cmd) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "cpu[%d] __zicio_acquire_new_requests [%s:%d][%s]\n", 
				cpu, __FILE__, __LINE__, __FUNCTION__);
#endif
			if (zicio_trigger_read_from_softirq(
				zicio_cmd_desc.zicio_desc, zicio_cmd_desc.zicio_cmd, cpu) < 0) {
				/* Request is not acquired, we have to retry */
				req_timer = zicio_get_request_timer(
					zicio_cmd_desc.zicio_desc, zicio_cmd_desc.zicio_cmd);
				zicio_add_request_timer_on(req_timer, ktime_get(), cpu,
					global_device_idx);
				retry++;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
				printk(KERN_WARNING "cpu[%d] __zicio_acquire_new_requests [%s:%d][%s]\n", 
					cpu, __FILE__, __LINE__, __FUNCTION__);
#endif
			} else {
#ifdef CONFIG_ZICIO_STAT
				/* Record stat */
				zicio_count_softtimer(zicio_cmd_desc.zicio_desc, type);
#endif /* (CONFIG_ZICIO_STAT) */
			}
		} else {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
			printk(KERN_WARNING "cpu[%d] __zicio_acquire_new_requests [%s:%d][%s]\n", 
				cpu, __FILE__, __LINE__, __FUNCTION__);
#endif
			/*
			 * There is no command to submit.
			 * So we cannot get a new request and cannot retry.
			 * Just rollback the request count.
			 */
			zicio_dec_request_count(ctrl, 1);
		}
	}
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu[%d] __zicio_acquire_new_requests [%s:%d][%s]\n", 
		cpu, __FILE__, __LINE__, __FUNCTION__);
#endif

	if (retry) {
		zicio_dec_request_count(ctrl, retry);
		ret = 0;
	}

out_getting_req:
	BUG_ON(atomic_read(&ctrl->getting_req) != 1);
	atomic_set(&ctrl->getting_req, 0);
	return ret;
}

/**
 * zicio_do_softtimer_timer_softirq - softtimer function for timer softirq
 * cpu: cpu id
 * global_device_idx: global raw device index
 *
 * Timer softirq is one of the softtimers used by zicio. Try to get a new
 * request if the request is not enough for this cpu.
 *
 * Note that we do not use the ZICIO_REQUEST_TIMER_FETCH_ONLY_EXPIRED flag
 * when we fetch the request timer.
 *
 * The reason is that timer softirq have high inacuracy than other softtimers.
 * In other words, if we miss one timer softirq, the next attempt will happen
 * inaccurately. So even if it's not expired, try resubmit now.
 *
 * Return 0 if retry is required. Otherwise return 1.
 */
int zicio_do_softtimer_timer_softirq(int cpu, int global_device_idx)
{
	return  __zicio_acquire_new_requests(cpu, global_device_idx,
		ZICIO_SOFTTIMER_TIMER_SOFTIRQ);
}
EXPORT_SYMBOL(zicio_do_softtimer_timer_softirq);

/**
 * zicio_do_softtimer_idle_loop - softtimer function for idle loop
 * cpu: cpu id
 * global_device_idx: the idx of device information in channel
 *
 * Idle loop is one of the softtimers used by zicio. Try to get a new
 * request if the request is not enough for this cpu.
 *
 * Return 0 if retry is required. Otherwise return 1.
 *
 * Note that we have to preemption disable. If not, system will be stopped
 * because the user cannot advance if the idle loop is scheuled out.
 */
int zicio_do_softtimer_idle_loop(int cpu, int global_device_idx)
{
	int ret;
	preempt_disable();
	ret = __zicio_acquire_new_requests(
		cpu, global_device_idx, ZICIO_SOFTTIMER_IDLE_LOOP);
	preempt_enable();
	return ret;
}
EXPORT_SYMBOL(zicio_do_softtimer_idle_loop);

/**
 * zicio_prepare_resubmit - replace nvme command in request for the next I/O
 * @ctrl: flow controller
 * @req: handling request
 *
 * Resubmit by NVMe IRQ is one of the softtimers used by zicio.
 *
 * Compared to other softtimers, this can achieve a relatively short period and
 * high accuracy. Therefore, use the ZICIO_REQUEST_TIMER_FETCH_ONLY_EXPIRED
 * flag.
 */
static void zicio_prepare_resubmit(struct zicio_flow_ctrl *ctrl,
	struct request *req)
{
	unsigned int flag = 0;
	struct zicio_nvme_cmd_desc zicio_cmd_desc;

	/* If the device is not stable, regulate I/O to reduce load on the device */
	if (!ctrl->rate_adaptive_enabled)
		flag = ZICIO_REQUEST_TIMER_FETCH_ONLY_EXPIRED;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu[%d] prepare resubmit start [%s:%d][%s]\n", 
			ctrl->cpu, __FILE__, __LINE__, __FUNCTION__);
#endif

	zicio_fetch_nvme_command_desc(
		&zicio_cmd_desc, flag, ctrl->cpu, ctrl->global_device_idx);

	/* If there is no nvme command to submit, drop the request */
	if (zicio_cmd_desc.zicio_cmd == NULL) {
		zicio_dec_request_count(ctrl, 1);
		req->zicio_cmd = NULL;
		req->zicio_io_start_time = 0;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "cpu[%d] prepare resubmit NULL [%s:%d][%s]\n", 
				ctrl->cpu, __FILE__, __LINE__, __FUNCTION__);
#endif
		return;
	}

	BUG_ON(zicio_cmd_desc.zicio_desc == NULL || zicio_cmd_desc.zicio_cmd == NULL);
	req->bio->zicio_desc = zicio_cmd_desc.zicio_desc;
	req->bio->zicio_cmd = zicio_cmd_desc.zicio_cmd;
	req->zicio_cmd = zicio_cmd_desc.zicio_cmd;
	req->zicio_io_start_time = ktime_get();

#ifdef CONFIG_ZICIO_STAT
	/* Record stat */
	zicio_count_softtimer(zicio_cmd_desc.zicio_desc,
		ZICIO_SOFTTIMER_IRQ_CYCLE);
#endif /* (CONFIG_ZICIO_STAT) */
}

/**
 * zicio_do_softtimer_irq_cycle - determine whether to do resubmit
 * req: handling request
 *
 * This function is called every ticks of NVMe IRQ softtimer. It decides whether
 * or not to resubmit next NVMe command.
 *
 * If so, try to prepare next NVMe command.
 */
void zicio_do_softtimer_irq_cycle(struct request *req)
{
	struct zicio_descriptor *zicio_desc;
	struct zicio_flow_ctrl *ctrl;
	int global_device_idx;

	BUG_ON(req->bio->zicio_desc == NULL);
	zicio_desc = req->bio->zicio_desc;
	global_device_idx = zicio_get_zicio_global_device_idx(
		zicio_desc, req->zicio_cmd->device_idx);

	zicio_free_nvme_cmd_list(req->zicio_cmd);

	ctrl = zicio_get_per_cpu_ptr_with_dev(
		flow_ctrl, zicio_desc->cpu_id, global_device_idx);
	zicio_flow_ctrl_tick(ctrl);

	/* If there are too much requests, drop the current request */
	if (zicio_test_and_dec_request_count(ctrl)) {
		req->zicio_cmd = NULL;
		req->zicio_io_start_time = 0;
		return;
	}

	mb();
	zicio_prepare_resubmit(ctrl, req);
}
EXPORT_SYMBOL(zicio_do_softtimer_irq_cycle);

/**
 * zicio_update_avg_io_time - update io statistics
 * @cpu: cpu id
 * @global_device_idx: global raw device index
 * @idx: index of I/O size
 * @new_io_time: currently elapsed time
 *
 * Update average time based on how long an I/O of a given size took.
 */
static void zicio_update_avg_io_time(
	int cpu, int global_device_idx, int idx, ktime_t new_io_time)
{
	struct zicio_flow_ctrl *ctrl =
		zicio_get_per_cpu_ptr_with_dev(flow_ctrl, cpu, global_device_idx);
	unsigned long flags;

	raw_spin_lock_irqsave(&ctrl->lock, flags);
	ctrl->io_time_ema[idx] = 
		zicio_calc_io_time_ema(ctrl, idx, new_io_time);
	ctrl->io_time_max[idx] =
		new_io_time > ctrl->io_time_max[idx] ?
			new_io_time : ctrl->io_time_max[idx];
	ctrl->io_time_total_avg[idx] =
		zicio_calc_io_time_total_avg(ctrl, idx, new_io_time);
	raw_spin_unlock_irqrestore(&ctrl->lock, flags);
}

/**
 * zicio_update_device_bandwidth - update device bandwidth
 * @ctrl: flow controller
 *
 * Update the device's bandwidth based on the size indicated by
 * ->cur_io_idx(@ctrl).
 *
 * Note that we use EMA to calculate bandwidth.
 */
static inline void zicio_update_device_bandwidth(
	struct zicio_flow_ctrl *ctrl)
{
	int idx = ctrl->cur_io_idx;
	s64 single_request_bandwidth;
	ktime_t io_time_ema = ctrl->io_time_ema[idx];

	if (io_time_ema) {
		single_request_bandwidth = zicio_calc_bandwidth(io_time_ema)
			>> (ZICIO_CHUNK_TO_MIN_SIZE_SHIFT - idx);
		ctrl->device_bandwidth = ctrl->cur_req_count * single_request_bandwidth;
	} else {
		ctrl->device_bandwidth = 0;
	}
}

/**
 * zicio_get_avg_consumption_tsc - get average huge page consumption time
 * @zicio_desc: zicio descriptor
 *
 * Return the time spent consuming huge page on average in tsc.
 *
 * Note that in shared mode, this function returns the representing consumption
 * time of the shared pool, not the individual consumption time of the given
 * channel.
 */
static inline unsigned long zicio_get_avg_consumption_tsc(
	struct zicio_descriptor *zicio_desc)
{
	struct zicio_switch_board *sb;

	if (zicio_desc->zicio_shared_pool_desc &&
			!zicio_check_channel_derailed(zicio_desc)) {
		return zicio_get_shared_pool_consumption_tsc_delta(zicio_desc);
	} else {
		sb = zicio_desc->switch_board;
		BUG_ON(sb == NULL);
		return sb->avg_tsc_delta;
	}
}

/**
 * zicio_update_user_bandwidth - update user bandwidth
 * @ctrl: flow controller
 * @zicio_desc: zicio descriptor
 *
 * Update the user's bandwidth when the consumption speed is changed.
 */
static inline void zicio_update_user_bandwidth(
	struct zicio_flow_ctrl *ctrl, struct zicio_descriptor *zicio_desc,
	int channel_device_idx)
{
	unsigned long avg_tsc_delta = zicio_get_avg_consumption_tsc(zicio_desc);
	int channel_raw_device_idx = zicio_get_zicio_channel_raw_dev_idx(zicio_desc,
			channel_device_idx);
	struct zicio_firehose_ctrl *fctrl = &zicio_desc->firehose_ctrl;

	/* Update user bandwidth if consumption speed has been changed */
	if (fctrl->last_user_avg_tsc_delta != avg_tsc_delta) {
		ctrl->user_bandwidth -=
			fctrl->last_update_bandwidth[channel_raw_device_idx];
		zicio_reset_user_bandwidth(
			zicio_desc, fctrl, channel_device_idx, avg_tsc_delta);
		ctrl->user_bandwidth += fctrl->bandwidth; 
	}
}

/**
 * zicio_rate_adaptive_request_counting - update user/device bandwidth
 * @zicio_desc: zicio_descriptor
 * @global_device_idx: global raw device index
 *
 * Update user/device bandwidth.
 */
static inline void zicio_update_bandwidth(
	struct zicio_descriptor *zicio_desc, int global_device_idx,
	int channel_device_idx)
{
	struct zicio_flow_ctrl *ctrl = zicio_get_per_cpu_ptr_with_dev(
		flow_ctrl, zicio_desc->cpu_id, global_device_idx);
	unsigned long flags;

	raw_spin_lock_irqsave(&ctrl->lock, flags);
	zicio_update_user_bandwidth(ctrl, zicio_desc, channel_device_idx);
	zicio_update_device_bandwidth(ctrl);
	raw_spin_unlock_irqrestore(&ctrl->lock, flags);
}

/**
 * zicio_do_pbr - Do PBR
 * @ctrl: flow controller
 *
 * Gather the device bandwidth all cpus are getting and the user bandwidth
 * required. Based on that, calculate how much the current cpu needs to
 * reshuffle requests.
 */
static int zicio_do_pbr(
	struct zicio_flow_ctrl *ctrl, int num_inner_dev)
{
	s64 user_bandwidth_inverse_ratio, device_bandwidth_inverse_ratio;
	s64 user_bandwidth_sum = 0, device_bandwidth_sum = 0;
	int cpu, io_idx, nr_valid_cpu = 0, nr_saturated = 0;
	struct zicio_flow_ctrl *tmp_ctrl;

	/* Initial states, quick exit */
	if (ctrl->user_bandwidth == 0 || ctrl->device_bandwidth == 0)
		return 0;

	for_each_possible_cpu(cpu) {
		tmp_ctrl = zicio_get_per_cpu_ptr_with_dev(
				flow_ctrl, cpu, ctrl->global_device_idx);

		if (tmp_ctrl->user_bandwidth == 0)
			continue;

		user_bandwidth_sum += tmp_ctrl->user_bandwidth;
		device_bandwidth_sum += tmp_ctrl->device_bandwidth;

		io_idx = tmp_ctrl->cur_io_idx;
		if (tmp_ctrl->io_time_ema[io_idx] >
				ZICIO_IO_TIME_SATURATED_POINT(tmp_ctrl->io_time_total_avg[io_idx]))
			nr_saturated++;
		nr_valid_cpu++;
	}

	/*
	 * To avoid floating point calculation, summation is placed in the numerator.
	 * The higher the ratio, the smaller the proportion of the total.
	 */
	user_bandwidth_inverse_ratio =
		user_bandwidth_sum / ctrl->user_bandwidth;
	device_bandwidth_inverse_ratio =
		device_bandwidth_sum / ctrl->device_bandwidth;
	BUG_ON(user_bandwidth_inverse_ratio == 0 ||
		device_bandwidth_inverse_ratio == 0);

	device_bandwidth_inverse_ratio *= num_inner_dev;
	if (user_bandwidth_inverse_ratio >= device_bandwidth_inverse_ratio) {
		/* We have too much device bandwidth, it should be reduced */
		ctrl->pbr_mult = 1;
		ctrl->pbr_div = user_bandwidth_inverse_ratio /
			device_bandwidth_inverse_ratio;
	} else {
		/* We have insufficient device bandwidth, it should be increased */ 
		ctrl->pbr_mult = device_bandwidth_inverse_ratio /
			user_bandwidth_inverse_ratio;
		ctrl->pbr_div = 1;
	}

	if (nr_saturated >= ZICIO_DEVICE_SATURATED_POINT(nr_valid_cpu) &&
		nr_valid_cpu > 1)
		return 1;
	return 0;
}

/**
 * zicio_do_reshuffling - reshuffle bandwidth by recounting the request
 * @ctrl:
 * @max_nr_req: maximum request count
 *
 * Recount requests based on values obtained from PBR.
 * Note that request count has limitaion. If we cannot recount the request,
 * use alternatives like delayed firing or prefiring.
 *
 * Return 1 if new request is needed, otherwise return 0.
 */
static int zicio_do_reshuffling(struct zicio_flow_ctrl *ctrl,
	int max_nr_req)
{
	int new_req_count, cur_req_count, ret = 0;

	cur_req_count = ctrl->cur_req_count ? ctrl->cur_req_count : 1;
	if (ctrl->pbr_mult != 1) {
		BUG_ON(ctrl->pbr_div != 1);
		BUG_ON(ctrl->pbr_mult == 0);
		new_req_count = cur_req_count * ctrl->pbr_mult;
		if (new_req_count < max_nr_req) {
			ctrl->min_req_count = new_req_count;
			ctrl->pbr_prefire_on = 0;
			ctrl->pbr_delay_on = 0;
			ret = 1;
		} else {
			/*
			 * Since we cannot get more request,
			 * try prefetching the nvme commands as an alternative.
			 */
			BUG_ON(max_nr_req == 0);
			ctrl->min_req_count = max_nr_req;
			ctrl->pbr_prefire_on = 1;
			ctrl->pbr_delay_on = 0;
		}		
	} else if (ctrl->pbr_div != 1) {
		BUG_ON(ctrl->pbr_mult != 1);
		new_req_count = cur_req_count / ctrl->pbr_div;
		if (new_req_count > 0) {
			ctrl->min_req_count = new_req_count;
			ctrl->pbr_prefire_on = 0;
			ctrl->pbr_delay_on = 0;
		} else {
			/*
			 * Since we cannot drop more request,
			 * try delayed nvme command fetching  as an alternative.
			 */
			ctrl->min_req_count = 1;
			ctrl->pbr_prefire_on = 0;
			ctrl->pbr_delay_on = 1;
		}
	} else {
		ctrl->pbr_prefire_on = 0;
		ctrl->pbr_delay_on = 0;
	}

	return ret;
}

/**
 * zicio_test_and_do_pbr - Do PBR if it needed.
 * @ctrl: flow controller
 * @max_nr_req: maximum request count
 * @know: current ktime_t
 *
 * Proportional Bandwidth Reshuffling (PBR) is an operation that compares the
 * bandwidth of all cores and resuffles the request count. Since this operation
 * is quite expensive, we do it in long period, typically 100 milliseconds.
 *
 * Return 1 if new request is needed.
 */
static int zicio_test_and_do_pbr(
	struct zicio_flow_ctrl *ctrl, int max_nr_req, ktime_t know,
	int num_inner_dev)
{
	ktime_t interval = ktime_sub(know, ctrl->last_pbr_check_time);

	if (ktime_to_ms(interval) >= ZICIO_PBR_PERIOD_MSEC) {
		ctrl->last_pbr_check_time = know;
		if (zicio_do_pbr(ctrl, num_inner_dev)) {
			ctrl->rate_adaptive_enabled = 0;
			ctrl->pbr_tolerance_count = 0;
		} else if (++ctrl->pbr_tolerance_count == ZICIO_PBR_TOLERANCE) {
			/* We reached stable state. No need to reshuffle */
			ctrl->pbr_mult = 1;
			ctrl->pbr_div = 1;
			ctrl->rate_adaptive_enabled = 1;

		}
	}
	return zicio_do_reshuffling(ctrl, max_nr_req);
}

/**
 * zicio_rate_adaptive_request_recounting - adjust request rate adaptively
 * @ctrl: flow controller
 * @max_nr_req: maximum request count
 *
 * Adjust the ->min_req_count(@ctrl) by comparing the bandwidth of the user and
 * the device.
 *
 * Return 1 if we have to get new requests, otherwise return 0.
 */
static int zicio_rate_adaptive_request_recounting(
	struct zicio_flow_ctrl *ctrl, int max_nr_req)
{
	int new_count, ret = 0;

	BUG_ON(max_nr_req == 0);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	ctrl->request_recounting_call_counter++;
	printk("user: %lld, device: %lld\n", ctrl->user_bandwidth,
			ctrl->device_bandwidth);
#endif	

	/* Initial states, quick exit */
	if (ctrl->user_bandwidth == 0 || ctrl->device_bandwidth == 0)
		return 0;
	/* Rate adaptive recounting is disabled, quick exit */
	if (!ctrl->rate_adaptive_enabled)
		return 0;

	if (ctrl->user_bandwidth >= ctrl->device_bandwidth) {
		new_count = ctrl->cur_req_count + 1;
		if (new_count >= max_nr_req)
			new_count = max_nr_req;
	} else {
		new_count = ctrl->cur_req_count - 1;
		if (new_count < 1)
			new_count = 1;
	}

	if (new_count > ctrl->cur_req_count)
		ret = 1;
	BUG_ON(new_count == 0);
	ctrl->min_req_count = new_count;
	return ret;
}

/**
 * zicio_request_recounting - recount request
 * @cpu: cpu id
 * @global_device_idx: global raw device index
 * @max_nr_req: maximum request count
 * @know: current ktime_t
 *
 * The change in bandwidth of users and device is slow, but the frequency of
 * function calls is very high.
 *
 * To avoid meaningless computational overhead, we adjust the number of
 * requests only when the contol period is reached.
 */
static inline void zicio_request_recounting(
	int cpu, int global_device_idx, int max_nr_req, ktime_t know,
	int num_inner_dev)
{
	struct zicio_flow_ctrl *ctrl =
		zicio_get_per_cpu_ptr_with_dev(flow_ctrl, cpu, global_device_idx);
	unsigned long flags;
	int newreq = 0;

	raw_spin_lock_irqsave(&ctrl->lock, flags);

	if (ctrl->recounting_enabled) {
		/* Eagerly acquire new requests */
		newreq = zicio_rate_adaptive_request_recounting(ctrl, max_nr_req);

		/* Proportional Bandwidth Reshuffling */
		//newreq |= zicio_test_and_do_pbr(ctrl, max_nr_req, know,
		//	num_inner_dev);

		/* If new request is needed, create timer softirq */
		if (newreq)
			zicio_trigger_softtimer_timer_softirq(
				ctrl->cpu, global_device_idx, 0);

		ctrl->recounting_enabled = 0;
	}

	raw_spin_unlock_irqrestore(&ctrl->lock, flags);
}

/**
 * zicio_update_flow_ctrl - update flow controller's information
 * req: handling request
 * @queue_depth: nvme queue depth
 *
 * Update flow controller using the given request.
 *
 * Note that we limit the maximum number of requests to prevent zicio from
 * monopolizing hardware resources.
 */
void zicio_update_flow_ctrl(struct request *req, u32 queue_depth)
{
	struct zicio_nvme_cmd_list *zicio_cmd;
	struct zicio_descriptor *zicio_desc;
	ktime_t io_time, know = ktime_get();
	int idx, nr_nvme_pages, max_nr_req, global_device_idx;
	int num_inner_dev;

	/* Get the information about I/O size */
	BUG_ON(req->bio->zicio_desc == NULL || req->zicio_cmd == NULL);
	zicio_desc = req->bio->zicio_desc;
	zicio_cmd = req->zicio_cmd;
	nr_nvme_pages = zicio_nvme_length_to_page(zicio_cmd->cmd.rw.length);
	idx = zicio_get_io_size_idx(nr_nvme_pages);
	BUG_ON(zicio_desc->switch_board == NULL);

	/* Update I/O elapsed time and bandwidth */
	BUG_ON(req->zicio_io_start_time == 0);
	io_time = know - req->zicio_io_start_time;
	global_device_idx = zicio_get_zicio_global_device_idx(
		zicio_desc, zicio_cmd->device_idx);
	zicio_update_avg_io_time(
		zicio_desc->cpu_id, global_device_idx, idx, io_time);
	zicio_update_bandwidth(zicio_desc, global_device_idx, zicio_cmd->device_idx);
	num_inner_dev = zicio_get_num_inner_device(
				zicio_get_zicio_fs_device_with_desc(zicio_desc,
						zicio_cmd->device_idx));

	/* Recount request */
	max_nr_req = ZICIO_MAX_REQ_COUNT(queue_depth);
	zicio_request_recounting(
		zicio_desc->cpu_id, global_device_idx, max_nr_req, know, num_inner_dev);
}
EXPORT_SYMBOL(zicio_update_flow_ctrl);

/**
 * zicio_descriptor_flow_in - let the new channel flow in
 * @zicio_desc: zicio descriptor
 * @global_device_idx: global raw device index
 *
 * This function is called when a new zicio channel is created.
 *
 * Note that currently zicio must avoid cpu migration. So remember which cpu
 * id is used for this zicio.
 *
 * Return 0 if success, otherwise return -1.
 */
int zicio_descriptor_flow_in(
	struct zicio_descriptor *zicio_desc, int global_device_idx)
{
	struct zicio_flow_ctrl *ctrl;
	unsigned long flags;
	int cpu = zicio_desc->cpu_id;

	BUG_ON(cpu < 0);
	if (sched_setaffinity(0, cpumask_of(cpu)))
		return -1;

	ctrl = zicio_get_per_cpu_ptr_with_dev(
		flow_ctrl, cpu, global_device_idx);
	BUG_ON(zicio_desc->firehose_ctrl.bandwidth != 0);

	raw_spin_lock_irqsave(&ctrl->lock, flags);
	ctrl->min_req_count++;
	ctrl->zicio_channel_count++;
	BUG_ON(ctrl->zicio_channel_count <= 0);
	raw_spin_unlock_irqrestore(&ctrl->lock, flags);

	return 0;
}
EXPORT_SYMBOL(zicio_descriptor_flow_in);

/**
 * zicio_descriptor_flow_out - let the old channel flow out
 * zicio_desc: zicio descriptor
 * global_device_idx: global raw device index
 *
 * This function is called when a zicio channel is closed.
 *
 * Adjust user bandwidth.
 *
 * Note that this function allows migration again. Therefore, it should be
 * called after all I/O is completed.
 */
void zicio_descriptor_flow_out(
	struct zicio_descriptor *zicio_desc, int global_device_idx,
	int channel_raw_device_idx)
{
	struct zicio_flow_ctrl *ctrl = zicio_get_per_cpu_ptr_with_dev(
		flow_ctrl, zicio_desc->cpu_id, global_device_idx);
	struct zicio_firehose_ctrl *fctrl = &zicio_desc->firehose_ctrl;
	unsigned long flags;

	raw_spin_lock_irqsave(&ctrl->lock, flags);
	ctrl->user_bandwidth -=
			fctrl->last_update_bandwidth[channel_raw_device_idx];
	ctrl->zicio_channel_count--;

	BUG_ON(ctrl->zicio_channel_count < 0);
	if (ctrl->zicio_channel_count == 0)
		ctrl->min_req_count = 0;
	raw_spin_unlock_irqrestore(&ctrl->lock, flags);
}
EXPORT_SYMBOL(zicio_descriptor_flow_out);

/**
 * zicio_request_flow_in - new request is assigned to this cpu
 * @zicio_desc:
 * @global_device_idx: global raw device index
 *
 * This function is called in zicio_init_read_trigger() to manage request
 * count.
 */
void zicio_request_flow_in(
	struct zicio_descriptor *zicio_desc, int global_device_idx)
{
	struct zicio_flow_ctrl *ctrl = zicio_get_per_cpu_ptr_with_dev(
		flow_ctrl, zicio_desc->cpu_id, global_device_idx);
	unsigned long flags;

	raw_spin_lock_irqsave(&ctrl->lock, flags);
	ctrl->cur_req_count++;
	raw_spin_unlock_irqrestore(&ctrl->lock, flags);
}
EXPORT_SYMBOL(zicio_request_flow_in);

/**
 * zicio_request_flow_out - request acquiring is failed
 * @zicio_desc:
 * @global_device_idx: global raw device index
 *
 * This function is called in zicio_init_read_trigger() to manage request
 * count.
 */
void zicio_request_flow_out(
	struct zicio_descriptor *zicio_desc, int global_device_idx)
{
	struct zicio_flow_ctrl *ctrl = zicio_get_per_cpu_ptr_with_dev(
		flow_ctrl, zicio_desc->cpu_id, global_device_idx);
	unsigned long flags;

	raw_spin_lock_irqsave(&ctrl->lock, flags);
	ctrl->cur_req_count--;
	BUG_ON(ctrl->cur_req_count < 0);
	raw_spin_unlock_irqrestore(&ctrl->lock, flags);
}
EXPORT_SYMBOL(zicio_request_flow_out);

/**
 * zicio_get_io_time - get how much I/O time is requried for given pages
 * @nr_nvme_pages: the number of nvme pages
 * @cpu: cpu id
 * @global_device_idx: global raw device index
 *
 * Return how long it will take to I/O the given nvme pages using EMA.
 *
 * Note that we do not keep track of all tthe page sizes, so we compute the
 * number of nvme pagesby converting it to one of the values we track.
 */
ktime_t zicio_get_io_time(int nr_nvme_pages, int cpu, int global_device_idx)
{
	struct zicio_flow_ctrl *ctrl =
		zicio_get_per_cpu_ptr_with_dev(flow_ctrl, cpu, global_device_idx);
	int idx = zicio_get_io_size_idx(nr_nvme_pages);
	return ctrl->io_time_ema[idx];
}
EXPORT_SYMBOL(zicio_get_io_time);

/**
 * zicio_get_current_io_size - get I/O size currently used
 * zicio_desc: zicio_descriptor
 * global_device_idx: global raw device index
 *
 * Return the I/O size used by the flow controller.
 */
static int __zicio_get_current_io_size(
	struct zicio_descriptor *zicio_desc, int global_device_idx)
{
	struct zicio_flow_ctrl *ctrl = zicio_get_per_cpu_ptr_with_dev(
		flow_ctrl, zicio_desc->cpu_id, global_device_idx);
	return ctrl->io_size[ctrl->cur_io_idx];
}

/**
 * zicio_get_current_io_size - get I/O size currently used
 * zicio_desc: zicio_descriptor
 * global_device_idx: global raw device index
 *
 * Return the I/O size used by the flow controller.
 */
int zicio_get_current_io_size(
	struct zicio_descriptor *zicio_desc, int channel_device_idx)
{
	int global_device_idx = zicio_get_zicio_global_device_idx(zicio_desc,
			channel_device_idx);
	return __zicio_get_current_io_size(zicio_desc, global_device_idx);
}
EXPORT_SYMBOL(zicio_get_current_io_size);

/**
 * zicio_get_current_io_size_for_md - get I/O size currently used
 * zicio_desc: zicio_descriptor
 * global_device_idx: global raw device index
 *
 * Return the I/O size used by the flow controller.
 */
int zicio_get_current_io_size_for_md(
	struct zicio_descriptor *zicio_desc, int channel_device_idx)
{
	zicio_device *zicio_device = zicio_get_zicio_fs_device_with_desc(
			zicio_desc, channel_device_idx);
	int *global_inner_dev_idx_array =
			zicio_get_num_raw_device_idx_array(zicio_device);
	int current_io_size, max_io_size = INT_MIN, idx, num_inner_dev =
			zicio_get_num_inner_device(zicio_device);

	for (idx = 0 ; idx < num_inner_dev ; idx++) {
		current_io_size = __zicio_get_current_io_size(
				zicio_desc, global_inner_dev_idx_array[idx]);
		if (max_io_size < current_io_size) {
			max_io_size = current_io_size;
		}
	}
	return max_io_size;
}
EXPORT_SYMBOL(zicio_get_current_io_size_for_md);

/*
 * Initialize flow controller for the given cpu
 */
static void __init __zicio_init_flow_controller_cpu(
			struct zicio_flow_ctrl *ctrl, int cpu, int dev_idx)
{
	int i;

	ctrl->tick = 0;
	ctrl->cpu = cpu;
	ctrl->global_device_idx = dev_idx;
	ctrl->pbr_div = 1;
	ctrl->pbr_mult = 1;
	ctrl->pbr_delay_on = 0;
	ctrl->pbr_prefire_on = 0;
	ctrl->pbr_tolerance_count = 0;
	ctrl->cur_req_count = 0;
	ctrl->min_req_count = 0;
	ctrl->user_bandwidth = 0;
	ctrl->device_bandwidth = 0;
	ctrl->recounting_enabled = 0;
	ctrl->rate_adaptive_enabled = 1;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	ctrl->request_recounting_call_counter = 0;
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
	raw_spin_lock_init(&ctrl->lock);

	atomic_set(&ctrl->getting_req, 0);

	for (i = 0; i < ZICIO_NUM_IO_SIZE_TYPE; i++) {
		ctrl->io_time_ema[i] = 0;
		ctrl->io_time_max[i] = 0;
		ctrl->io_time_total_avg[i] = 0;
		ctrl->io_time_measurment_count[i] = 0;
	}

	ctrl->io_size[ZICIO_512K_IDX] = 512;
	ctrl->calc_io_time_ema[ZICIO_512K_IDX] =
		__zicio_calc_io_time_ema_512k;

	ctrl->io_size[ZICIO_256K_IDX] = 256;
	ctrl->calc_io_time_ema[ZICIO_256K_IDX] =
		__zicio_calc_io_time_ema_256k;

	ctrl->io_size[ZICIO_128K_IDX] = 128;
	ctrl->calc_io_time_ema[ZICIO_128K_IDX] =
		__zicio_calc_io_time_ema_128k;

	ctrl->io_size[ZICIO_64K_IDX] = 64;
	ctrl->calc_io_time_ema[ZICIO_64K_IDX] =
		__zicio_calc_io_time_ema_64k;

	ctrl->io_size[ZICIO_32K_IDX] = 32;
	ctrl->calc_io_time_ema[ZICIO_32K_IDX] =
		__zicio_calc_io_time_ema_32k;

	ctrl->cur_io_idx = ZICIO_256K_IDX;
	ctrl->zicio_channel_count = 0;
}


/*
 * Initialize flow controller for the given cpu
 */
static void __init zicio_init_flow_controller_cpu(int cpu, int num_dev)
{
	struct zicio_flow_ctrl **ctrl;
	int i;

	ctrl = per_cpu_ptr(&flow_ctrl, cpu);
	*ctrl = (struct zicio_flow_ctrl *)kmalloc(
		sizeof(struct zicio_flow_ctrl) * num_dev, GFP_KERNEL|__GFP_ZERO);

	for (i = 0 ; i < num_dev ; i++) {
		__zicio_init_flow_controller_cpu((*ctrl) + i, cpu, i);
	}
}

/*
 * Initialize flow controller for each cpu
 */
void __init zicio_init_md_flow_controller(int num_dev)
{
	int cpu;
	for_each_possible_cpu(cpu)
		zicio_init_flow_controller_cpu(cpu, num_dev);
}
EXPORT_SYMBOL(zicio_init_md_flow_controller);

/*
 * Debugging function.
 */
void zicio_dump_md_flow_ctrl(int cpu, int global_device_idx)
{
	struct zicio_flow_ctrl *ctrl =
		zicio_get_per_cpu_ptr_with_dev(flow_ctrl, cpu, global_device_idx);
	unsigned long flags;

	raw_spin_lock_irqsave(&ctrl->lock, flags);

	printk(KERN_WARNING "[ZICIO_FLOW] cpu: %d, 512K, ema: %lld, max: %lld, total_avg: %lld\n",
		ctrl->cpu,
		ctrl->io_time_ema[ZICIO_512K_IDX],
		ctrl->io_time_max[ZICIO_512K_IDX],
		ctrl->io_time_total_avg[ZICIO_512K_IDX]);
	printk(KERN_WARNING "[ZICIO_FLOW] cpu: %d, 256K, ema: %lld, max: %lld, total_avg: %lld\n",
		ctrl->cpu,
		ctrl->io_time_ema[ZICIO_256K_IDX],
		ctrl->io_time_max[ZICIO_256K_IDX],
		ctrl->io_time_total_avg[ZICIO_256K_IDX]);
	printk(KERN_WARNING "[ZICIO_FLOW] cpu: %d, 128K, ema: %lld, max: %lld, total_avg: %lld\n",
		ctrl->cpu,
		ctrl->io_time_ema[ZICIO_128K_IDX],
		ctrl->io_time_max[ZICIO_128K_IDX],
		ctrl->io_time_total_avg[ZICIO_128K_IDX]);
	printk(KERN_WARNING "[ZICIO_FLOW] cpu: %d, 64K, ema: %lld, max: %lld, total_avg: %lld\n",
		ctrl->cpu,
		ctrl->io_time_ema[ZICIO_64K_IDX],
		ctrl->io_time_max[ZICIO_64K_IDX],
		ctrl->io_time_total_avg[ZICIO_64K_IDX]);
	printk(KERN_WARNING "[ZICIO_FLOW] cpu: %d, 32K, ema: %lld, max: %lld, total_avg: %lld\n",
		ctrl->cpu,
		ctrl->io_time_ema[ZICIO_32K_IDX],
		ctrl->io_time_max[ZICIO_32K_IDX],
		ctrl->io_time_total_avg[ZICIO_32K_IDX]);
	printk(KERN_WARNING "[ZICIO_FLOW] cpu: %d, cur_req_count: %d, min_req_count:%d\n",
		ctrl->cpu, ctrl->cur_req_count, ctrl->min_req_count);
	printk(KERN_WARNING "[ZICIO_FLOW] cpu: %d, user bandwidth: %lld, device bandwidth: %lld\n",
		ctrl->cpu, ctrl->user_bandwidth, ctrl->device_bandwidth);

	raw_spin_unlock_irqrestore(&ctrl->lock, flags);
}
EXPORT_SYMBOL(zicio_dump_md_flow_ctrl);
