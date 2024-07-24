#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/irq_work.h>
#include <linux/jiffies.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/syscalls.h>
#include <linux/workqueue.h>

#include "zicio_cmd.h"
#include "zicio_device.h"
#include "zicio_desc.h"
#include "zicio_mem.h"
#include "zicio_req_timer.h"
#include "zicio_firehose_ctrl.h"
#include "zicio_md_flow_ctrl.h"
#include "zicio_req_submit.h"

/*
 * Why does ZicIO use a timer wheel concept for request control, not a
 * hrtimer's red-black tree?
 *
 * The timer wheel implementation has changed over time. In the ealry days,
 * "cascade" operation was used to manage timers precisely. However, as noted in
 * this document(https://lwn.net/Articles/152436/), the cascade process can be
 * too expensive if the timer interrupt frequency is raised.
 *
 * So, the data structure for the high-resolution timer(hrtimer) does not use
 * the timer wheel. hrtimer manages timers in a red-black tree based on
 * expriation time. This may increase the time complexity of insertion and
 * deletetion compared to the timer wheel, but it does not request cascade
 * operation.
 *
 * After all, the hrtimer, rather than the "cascading" timer wheel, was used for
 * accurate timer management. Accordingly, the cascade operation of the timer
 * wheel is no longer meaningful.
 *
 * Today's timer wheel implementation do not use cascade operation as shown in
 * this document(https://lwn.net/Articles/646950/). Of curse, the timer wheel
 * now loses the accuracy it had before for timers above level 1. However, as a
 * benefit of this, it still has fast insertion, deletion, and search time
 * complexity.
 *
 * This trade-off led us to the timer wheel concept to manage I/O requests.
 * (1) ZicIO resubmits I/O requests from the I/O interrupt handler.
 * Therefore fast insertion time complextity is requried. (2) ZicIO needs
 * the cycle of I/O resubmits. It does not require nanosecond accuracy like
 * hrtimer does. It just needs to defer the request until the appropriate timer
 * slack.
 *
 * Unfortunately, kernel/time/timer.c cannot be used as-is. Because it is an
 * implementation that is very closely related to millsecond jiffies. So we
 * created this new file to implement the new timer wheel since we want
 * granularity in microseconds.
 *
 * This file is almost same with kernel/time/timer.c except the two things.
 * (1) This timer wheel has microsecond granularity. (2) The level is much lower
 * than the original timer wheel because this is good enough.
 */

/*
 * Unlike the original timer wheel, zicio uses ktime_t instead of jiffies.
 *
 * ktime_t is a nanosecond. Since this value is too small, zicio does not use
 * it as a minimum unit (level 0).
 *
 * ZICIO_KTIME_DEFAULT_CLK_SHIT is used to transform the ktime_t to the 
 * unit of the level 0. If ZICIO_DEFULAT_CLK_SHIT is 10, which means 
 * that the unit of level 0 is 1 microsecond. (We will process the raw
 * ktime_t like this, ktime_t >> ZICIO_KTIME_DEFAULT_CLK_SHIT)
 *
 * So, changed granulartity and ragne levels are:
 *
 * ZICIO_KTIME_DEFAULT_CLK_SHIFT 10
 * Level Offset  Granularity              Range
 *  0      0         1 us                  0 us -          63 us
 *  1     64         8 us                 64 us -         511 us
 *  2    128        64 us                512 us -        4095 us (512us - ~4ms)
 *  3    192       512 us               4096 us -       32767 us (~4ms - ~32ms)
 *  4    256      4096 us (~4ms)       32768 us -      262143 us (~32ms - ~256ms)
 *  5    320     32768 us (~32ms)     262144 us -     2097151 us (~256ms - ~2s)
 *  6    384    262144 us (~256ms)   2097152 us -    16777215 us (~2s - ~16s)
 *  7    448   2097152 us (~2s)     16777216 us -   134217727 us (~16s - ~2m)
 *  8    512  16777216 us (~16s)   134217728 us -  1073741822 us (~2m - ~17m)
 *
 * ZICIO_KTIME_DEFAULT_CLK_SHIFT 8
 * Level Offset  Granularity              Range
 *  0	   0         4 us                  0 us -         255 us
 *  1	  64        32 us                256 us -        2047 us (256us - ~2ms)
 *  2	 128       256 us               2048 us -       16383 us (~2ms - ~16ms)
 *  3	 192      2048 us (~2ms)       16384 us -      131071 us (~16ms - ~128ms)
 *  4	 256     16384 us (~16ms)     131072 us -     1048575 us (~128m - ~1s)
 *  5	 320    131072 us (~128ms)   1048576 us -     8388607 us (~1s - ~8s)
 *  6	 384   1048576 us (~1s)      8388608 us -    67108863 us (~8s - ~64s)
 *  7	 448   8388608 us (~8s)     67108864 us -   536870911 us (~64s - ~8m)
 *  8    512  67108864 us (~64s)   536870912 us -  4294967288 us (~8m - ~70m)
 */
#define ZICIO_KTIME_DEFAULT_CLK_SHIFT	(6)

/* Clock divisor for the next level */
#define ZICIO_LVL_CLK_SHIFT	(3)
#define ZICIO_LVL_CLK_DIV	(1UL << ZICIO_LVL_CLK_SHIFT)
#define ZICIO_LVL_CLK_MASK	(ZICIO_LVL_CLK_DIV - 1)
#define ZICIO_LVL_SHIFT(n)	((n) * ZICIO_LVL_CLK_SHIFT)
#define ZICIO_LVL_GRAN(n)	(1UL << ZICIO_LVL_SHIFT(n))

/*
 * The time start value for each level to select the bucket at enqueue time. We
 * start from the last possible delta of the previous level so that we can later
 * add an extra ZICIO_LVL_GRAN(n) to n (see zicio_calc_index()).
 */
#define ZICIO_LVL_START(n) \
	((ZICIO_LVL_SIZE - 1) << (((n) - 1) * ZICIO_LVL_CLK_SHIFT))

/* Size of each clock level */
#define ZICIO_LVL_BITS		(6)
#define ZICIO_LVL_SIZE		(1UL << ZICIO_LVL_BITS)
#define ZICIO_LVL_MASK		(ZICIO_LVL_SIZE - 1)
#define ZICIO_LVL_OFFS(n)	((n) * ZICIO_LVL_SIZE)

/* Level depth.*/
#define ZICIO_LVL_DEPTH		(9)

/* The cutoff (max. capacity of the wheel) */
#define ZICIO_WHEEL_TIMEOUT_CUTOFF	(ZICIO_LVL_START(ZICIO_LVL_DEPTH))
#define ZICIO_WHEEL_TIMEOUT_MAX	\
	(ZICIO_WHEEL_TIMEOUT_CUTOFF - ZICIO_LVL_GRAN(ZICIO_LVL_DEPTH - 1))

/* The resulting wheel size */
#define ZICIO_WHEEL_SIZE	(ZICIO_LVL_SIZE * ZICIO_LVL_DEPTH)

/**
 * zicio_request_timer_base - timer wheel for zicio's request control.
 * @lock: spinlock for used for raw_spin_(lock/unlock)_irq(save/restore).
 * @clk: base time for adding a new timer
 * @next_expiry: next expiring time
 * @running_timers: fetched request timers
 * @expired_queue: expired requests are moved to this queue
 * @zombie_timers: request timers with invalid chunk id
 * @cpu: timer base's cpu
 * @trigger: request trigger timer
 * @trigger_running: is trigger running?
 * @dev_idx: global raw device index
 * @pending_map: bitmap representing the index of the pending timer
 * @vectors: array representing the timer wheel for each level
 *
 * This data structure mimics timer_base in kernel/time/timer.c to represent
 * timer wheel. ZicIO use this data structure to submit I/O requests at the
 * right time.
 *
 * Unlike the original timer_base, there are changes to some members:
 *
 * - Removed
 *
 *   (1) is_idle, timers_pending, next_expiry_recalc
 *     This member is related to timer interrupt. We don't need it because
 *     zicio doesn't expire the request timers in the timer interrupt. 
 * 
 *   (2) expiry_lock, timer_waiters
 *     See the comment (1) of __zicio_fetch_next_request_timers() for these
 *     members.
 *
 * - Added
 * 
 *   (1) expired_queue
 *     Expired request timers come into this queue to reserve timer wheel's
 *     space.
 *
 *   (2) trigger
 *     If the next request timer is too far away, zicio can return the
 *     assigned struct request. In this case, the request must be reassigned at
 *     an appropriate time, using a timer softirq.
 *
 *   (3) zombie_timers
 *     If the user's consumption rate is very slow and the chunk id is not
 *     allocated in the I/O handler, create a zombie request timer and store it
 *     here. Afterwards, the timer softirq will periodically check this zombie
 *     request timers to determine if it can recreate the live request timers
 *     for that zicio descriptor.
 *
 * - Changed
 *
 *   (1) data type of clk and next_expiry
 *     The type of these members are changed from unsigned long to the ktime_t
 *     to represent microsecond time scale.
 *
 *   (2) data type of vectors
 *     As mentioned above, expired request timers are moved to expired_queue.
 *     To do this efficiently, zicio uses one more pointer for each index
 *     of the timer wheel. 
 */
struct zicio_request_timer_base {
	raw_spinlock_t		lock;
	ktime_t				clk;
	ktime_t				next_expiry;
	struct list_head	running_timers;
	struct list_head 	expired_queue;
	unsigned int		cpu;
	struct timer_list	trigger;
	atomic_t			trigger_running;
	int					dev_idx;
	DECLARE_BITMAP(pending_map, ZICIO_WHEEL_SIZE);
	struct list_head 	vectors[ZICIO_WHEEL_SIZE];
} ____cacheline_aligned;

struct zicio_request_zombie_timer_base {
	raw_spinlock_t		lock;
	struct list_head	zombie_timers;
};

static DEFINE_PER_CPU(struct zicio_request_timer_base *,
	request_timer_base);

static DEFINE_PER_CPU(struct zicio_id_allocator, global_zombie_timers);

#define zicio_get_zombie_timer(ptr, cpu, dev_idx) ({					  \
	zicio_id_allocator __percpu *zombie_timer = (per_cpu_ptr(ptr, cpu));  \
	zicio_get_zicio_struct(zombie_timer, dev_idx + 1, false); })

static void zicio_softtimer_timer_softirq_callback(
	struct timer_list *timer);

/**
 * zicio_ktime_get_default_unit - apply default clk shift to the ktime_t
 * @know: raw ktime_t to apply ZICIO_KTIME_DEFAULT_CLK_SHIFT
 *
 * In most cases, level 0 of the zicio timer wheel does not use ktime_t as
 * it is. Therefore, before using raw ktime_t, process it in unit of level 0
 * through this function.
 */
static inline ktime_t 
zicio_ktime_get_default_unit(ktime_t know)
{
	return (know >> ZICIO_KTIME_DEFAULT_CLK_SHIFT);
}

/**
 * zicio_ktime_get_from_default_unit - get back from default to raw ktime_t
 * @t: time to transform
 *
 * ZicIO uses its own time resolution. Transform it to the raw ktime_t.
 */
static inline ktime_t
zicio_ktime_get_from_default_unit(ktime_t t)
{
	return (t << ZICIO_KTIME_DEFAULT_CLK_SHIFT);
}

/**
 * These macros are part of the macros present in include/linux/timer.h
 * Only the necessary parts for zicio were excerpted. So, the remaining
 * flags may be added later as needed.
 */
#define ZICIO_REQUEST_TIMER_CPUMASK		0x0003FFFF
#define ZICIO_REQUEST_TIMER_BASEMASK	(ZICIO_REQUEST_TIMER_CPUMASK)
#define ZICIO_REQUEST_TIMER_ARRAYSHIFT	22
#define ZICIO_REQUEST_TIMER_ARRAYMASK	0xFFC00000

/*
 * This function corresponds to the get_timer_cpu_base()
 * in kernel/time/timer.c
 */
static inline struct zicio_request_timer_base * 
zicio_get_request_timer_cpu_base(u32 cpu, int global_device_idx)
{
	struct zicio_request_timer_base *base =
		zicio_get_per_cpu_ptr_with_dev(request_timer_base, cpu,
			global_device_idx);
	return base;
}

/* This function corresponds to the get_timer_base() in kernel/time/timer.c */
static inline struct zicio_request_timer_base *
zicio_get_request_timer_base(u32 tflags, int global_device_idx)
{
	return zicio_get_request_timer_cpu_base(
			tflags & ZICIO_REQUEST_TIMER_CPUMASK, global_device_idx);
}

/* This function corresponds to the timer_get_idx() in kernel/time/timer.c */
static inline unsigned int 
zicio_request_timer_get_idx(struct zicio_request_timer *timer)
{
	return (timer->flags & ZICIO_REQUEST_TIMER_ARRAYMASK) 
			>> ZICIO_REQUEST_TIMER_ARRAYSHIFT;
}

/* This function corresponds to the timer_get_idx() in kernel/time/timer.c */
static inline unsigned int
zicio_zombie_request_timer_get_idx(
	struct zicio_zombie_request_timer *timer)
{
	return (timer->flags & ZICIO_REQUEST_TIMER_ARRAYMASK)
			>> ZICIO_REQUEST_TIMER_ARRAYSHIFT;
}

/* This function corresponds to the timer_set_idx() in kernel/time/timer.c */
static inline void
zicio_request_timer_set_idx(
	struct zicio_request_timer *timer,
	unsigned int idx)
{
	timer->flags = (timer->flags & ~ZICIO_REQUEST_TIMER_ARRAYMASK) 
					| (idx << ZICIO_REQUEST_TIMER_ARRAYSHIFT);
}

/**
 * zicio_request_timer_pending - is a timer pending?
 * @timer: the request timer in question
 *
 * This function corresponds to the timer_pending() in include/linux/timer.h
 *
 * return value: 1 if the timer is pending, 0 if not.
 */
static inline int 
zicio_request_timer_pending(const struct zicio_request_timer *timer)
{
	return !list_empty(&timer->entry)
			&& !(timer->entry.next == LIST_POISON1);
}

/**
 * zicio_zombie_request_timer_pending - is a timer pending?
 * @zombie_timer: the request timer in question
 *
 * This function corresponds to the timer_pending() in include/linux/timer.h
 *
 * return value: 1 if the timer is pending, 0 if not.
 */
static inline int
zicio_zombie_request_timer_pending(
	const struct zicio_zombie_request_timer *timer)
{
	return !list_empty(&timer->entry)
			&& !(timer->entry.next == LIST_POISON1);
}

/* This functino corresponds to the detach_timer() in kernel/time/timer.c */
static inline void
zicio_detach_request_timer(struct zicio_request_timer *timer)
{
	struct list_head *entry = &timer->entry;
	list_del_init(entry);
}

/* This functino corresponds to the detach_timer() in kernel/time/timer.c */
static inline void
zicio_detach_zombie_request_timer(
	struct zicio_zombie_request_timer *timer)
{
	struct list_head *entry = &timer->entry;
	list_del_init(entry);
}

/* 
 * This function corresponds to the detach_if_pending()
 * in kernel/time/timer.c
 */
static int 
zicio_detach_request_timer_if_pending(
	struct zicio_request_timer *timer,
	struct zicio_request_timer_base *base)
{
	unsigned idx = zicio_request_timer_get_idx(timer);
	struct list_head *vector;

	if (!zicio_request_timer_pending(timer))
		return 0;

	if (idx < ZICIO_WHEEL_SIZE)
		vector = base->vectors + idx;
	else
		vector = &base->expired_queue;

	/*
	 * Note that even if idx is less than ZICIO_WHEEL_SIZE, this request
	 * timer can exist in ->expired_queue(@base). So check once more with
	 * list_is_first().
	 */
	if (list_is_singular(vector) && list_is_first(&timer->entry, vector))
		__clear_bit(idx, base->pending_map);

	zicio_detach_request_timer(timer);
	return 1;
}

/*
 * This function corresponds to the detach_if_pending()
 * in kernel/time/timer.c
 */
static int
zicio_detach_zombie_request_timer_if_pending(
	struct zicio_zombie_request_timer *timer,
	struct zicio_request_zombie_timer_base *base)
{
	if (!zicio_zombie_request_timer_pending(timer))
		return 0;

	zicio_detach_zombie_request_timer(timer);
	return 1;
}

/*
 * Helper function to calculate the array index for a given expiry time.
 * This corresponds to the calc_index() in kernel/time/timer.c
 *
 * Note that the ktime_t values used here are in the context of
 * ZICIO_KTIME_DEFAULT_CLK_SHIFT applied.
 */
static inline unsigned 
zicio_calc_index(ktime_t expires, unsigned lvl, ktime_t *bucket_expiry)
{
	/*
	 * Unlike calc_index(), zicio have to round down the expire time. This
	 * is because, in the original timer wheel, the timer must be prevented from
	 * expiring before the specified time. But in zicio, it must be
	 * prevented from expiring later than the specified time as much as possible.
	 */
	expires = expires >> ZICIO_LVL_SHIFT(lvl);
	*bucket_expiry = expires << ZICIO_LVL_SHIFT(lvl);
	return ZICIO_LVL_OFFS(lvl) + (expires & ZICIO_LVL_MASK);
}

/*
 * This function corresponds to the calc_wheel_index() in kernel/time/timer.c
 *
 * Calculate the index at which the new request will go. The level to enter is
 * determined by how far the request's expire is relative to base clk.
 *
 * If somone expired current interval, clk will be at leat +1 above the
 * current interval. Then delta will be negative if the new timer
 * corresponds to the current interval. In this case, return -1.
 *
 * Note that the ktime_t values used here are in the context of
 * ZICIO_KTIME_DEFAULT_CLK_SHIFT applied.
 */
static int 
zicio_calc_wheel_index(ktime_t expires, ktime_t clk, ktime_t *bucket_expiry)
{
	ktime_t delta = expires - clk;
	unsigned int idx;

	/*
	 * Unlike the original function, we first check whether the delta is
	 * negative or not. This is because the data type of delta is changed to
	 * ktime_t instead of unsigned long.
	 */
	if (delta < 0) {
		idx = -1;
		*bucket_expiry = clk;
	} else if (delta < ZICIO_LVL_START(1)) {
		idx = zicio_calc_index(expires, 0, bucket_expiry);
	} else if (delta < ZICIO_LVL_START(2)) {
		idx = zicio_calc_index(expires, 1, bucket_expiry);
	} else if (delta < ZICIO_LVL_START(3)) {
		idx = zicio_calc_index(expires, 2, bucket_expiry);
	} else if (delta < ZICIO_LVL_START(4)) {
		idx = zicio_calc_index(expires, 3, bucket_expiry);
	} else if (delta < ZICIO_LVL_START(5)) {
		idx = zicio_calc_index(expires, 4, bucket_expiry);
	} else if (delta < ZICIO_LVL_START(6)) {
		idx = zicio_calc_index(expires, 5, bucket_expiry);
	} else if (delta < ZICIO_LVL_START(7)) {
		idx = zicio_calc_index(expires, 6, bucket_expiry);
	} else if (delta < ZICIO_LVL_START(8)) {
		idx = zicio_calc_index(expires, 7, bucket_expiry);
	} else {
		/*
		 * Force expire obscene large timeouts to expire at the capacity limit
		 * of the wheel.
		 */
		if (delta >= ZICIO_WHEEL_TIMEOUT_CUTOFF)
			expires = clk + ZICIO_WHEEL_TIMEOUT_MAX;

		idx = zicio_calc_index(expires, ZICIO_LVL_DEPTH - 1,
							bucket_expiry);
	}
	return idx;
}

/*
 * This function corresponds to enqueue_timer() in kernel/timer/time.c
 *
 * Enqueue the request timer into the hash bucket, mark it pending in the
 * bitmap, store the index in the timer flags.
 *
 * Unlike the original equeue_timer(), we don't have to call the
 * trigger_dyntick_cpu(). This is because enqueued timers have nothing to do
 * with timer interrupts.
 *
 * Note that the ktime_t values used here are in a context of
 * ZICIO_KTIME_DEFAULT_CLK_SHIFT-applied context.
 */
static void 
zicio_enqueue_request_timer(struct zicio_request_timer_base *base,
	struct zicio_request_timer *timer, int idx, ktime_t bucket_expiry)
{
	if (idx >= 0) {
		list_add_tail(&timer->entry, base->vectors + idx);
		__set_bit(idx, base->pending_map);
		zicio_request_timer_set_idx(timer, idx);
	} else {
		BUG_ON(idx != -1);
		list_add_tail(&timer->entry, &base->expired_queue);
		zicio_request_timer_set_idx(timer, ZICIO_WHEEL_SIZE);
	}

	/*
	 * Check whether this is the new frist expiring timer. The effective expiry
	 * time of the timer is required here (bucket_expriy) instead of
	 * timer->expires.
	 */
	if (ktime_before(bucket_expiry, base->next_expiry))
		base->next_expiry = bucket_expiry;
}

/* 
 * This function corresponds to the internal_add_timer() in kernel/time/timer.c 
 *
 * Note that the ktime_t values used here are in a context of
 * ZICIO_KTIME_DEFAULT_CLK_SHIFT-applied.
 */
static void
zicio_internal_add_request_timer(struct zicio_request_timer_base *base,
	struct zicio_request_timer *timer)
{
	ktime_t bucket_expiry;
	int idx;

	idx = zicio_calc_wheel_index(timer->expires, base->clk, &bucket_expiry);
	zicio_enqueue_request_timer(base, timer, idx, bucket_expiry);
}

/* 
 * This function corresponds to the forward_timer_base() in kernel/time/timer.c 
 *
 * Note that the ktime_t values used here are in a context of
 * ZICIO_KTIME_DEFAULT_CLK_SHIFT-applied context, except for "know"
 */
static inline void
zicio_forward_request_timer_base(struct zicio_request_timer_base *base)
{
	/* Change raw ktime_t to the default unit for level 0 */
	ktime_t know = zicio_ktime_get_default_unit(ktime_get());

	/*
	 * No need to forward if we are close enough below time.
	 * Also while executing timers, base->clk is 1 offset ahead of current time
	 * to avoid endless requeueing to current time.
	 */
	if (know - base->clk < 1)
		return;
 
	/*
	 * If the next expiry value is > current time, then we fast forward to know
	 * otherwise we forward to the next expiry value.
	 */
	if (ktime_after(base->next_expiry, know)) {
		base->clk = know;
	} else {
		if (WARN_ON_ONCE(ktime_before(base->next_expiry, base->clk)))
			return;
		base->clk = base->next_expiry;
	}
}

/*
 * This function corresponds to the lock_timer_base in kernel/time/timer.c
 *
 * We are using hashed locking: Holding per_cpu(zicio_timer_bases[x]). lock
 * means that all timers which are tied to this base are locked, and the base
 * itself is locked too.
 *
 * Note that we do not consider migrating the request timer to another base. So
 * unlike the original function, we do not loop to check whether the timer is
 * migrating or not.
 */
static struct zicio_request_timer_base *
zicio_lock_request_timer_base(struct zicio_request_timer *timer,
	int global_device_idx, unsigned long *flags)
{
	struct zicio_request_timer_base *base;
	base = zicio_get_request_timer_base(timer->flags, global_device_idx);
	raw_spin_lock_irqsave(&base->lock, *flags);
	return base;
}

/*
 * This function corresponds to the lock_timer_base in kernel/time/timer.c
 *
 * We are using hashed locking: Holding per_cpu(zicio_timer_bases[x]). lock
 * means that all timers which are tied to this base are locked, and the base
 * itself is locked too.
 *
 * Note that we do not consider migrating the request timer to another base. So
 * unlike the original function, we do not loop to check whether the timer is
 * migrating or not.
 */
static struct zicio_request_zombie_timer_base *
zicio_lock_zombie_request_timer_base(
	struct zicio_zombie_request_timer *timer,
	int global_device_idx, unsigned long *flags)
{
	struct zicio_request_zombie_timer_base *base;
	int cpu = timer->flags & ZICIO_REQUEST_TIMER_CPUMASK;
	base = zicio_get_zombie_timer(&global_zombie_timers, cpu,
				global_device_idx);
	raw_spin_lock_irqsave(&base->lock, *flags);
	return base;
}

#define ZICIO_MOD_REQUEST_TIMER_PENDING_ONLY	(0x01)
#define ZICIO_MOD_REQUEST_TIMER_REDUCE			(0x02)
#define ZICIO_MOD_REQUEST_TIMER_NOTPENDING		(0x04)

/* 
 * This function corresponds to __mod_timer() in kernel/time/timer.c
 *
 * The function returns whether it has modified a pending timer or not.
 * (ie. __mod_timer() of an inactive timer returns 0, __mod_timer() of an active
 * timer returns 1.)
 *
 * Note that:
 *
 * (1)	The argument "expires" is raw ktime_t. So we have to change it
 *		using ZICIO_KTIME_DEFAULT_CLK_SHIFT.
 *
 * (2)	Currently we do not consider migrating the rquest timer to another base.
 */
static inline int
__zicio_mod_request_timer(struct zicio_request_timer *timer,
	ktime_t expires, unsigned int options, int global_device_idx)
{
	struct zicio_request_timer_base *base;
	ktime_t clk = 0, bucket_expiry;
	unsigned long flags = 0;
	int ret = 0, idx = -2;

	/* Change raw ktime_t to the default unit for level 0 */
	expires = zicio_ktime_get_default_unit(expires);

	/*
	 * If the timer is re-modified to have the same timeout or ends up in the
	 * same array bucket then just return:
	 */
	if (!(options & ZICIO_MOD_REQUEST_TIMER_NOTPENDING) && 
		zicio_request_timer_pending(timer)) {
		/*
		 * The downside of this optimization is that it can result in larger
		 * granularity than you would get from adding a new timer with this
		 * expiry.
		 */
		ktime_t diff = timer->expires - expires;

		if (!diff)
			return 1;
		if (options & ZICIO_MOD_REQUEST_TIMER_REDUCE && diff <= 0)
			return 1;

		/*
		 * We lock timer base and calculate the bucket index right here. If the
		 * timer ends up in the same bucekt, then we just update the expiry
		 * time and avoid the whole dequeue/enqueue dance.
		 */
		base = zicio_lock_request_timer_base(
			timer, global_device_idx, &flags);
		zicio_forward_request_timer_base(base);

		if (zicio_request_timer_pending(timer) && 
			(options & ZICIO_MOD_REQUEST_TIMER_REDUCE) &&
			timer->expires <= expires) {
			ret = 1;
			goto out_unlock;
		}

		clk = base->clk;
		idx = zicio_calc_wheel_index(expires, clk, &bucket_expiry);

		/*
		 * Retrieve and compare the array index of the pending timer. If it
		 * matched set the expiry to the new value so a subsequent call will
		 * exit in the expires check above.
		 */
		if (idx == zicio_request_timer_get_idx(timer)) {
			if (!(options & ZICIO_MOD_REQUEST_TIMER_REDUCE))
				timer->expires = expires;
			else if (ktime_after(timer->expires, expires))
				timer->expires = expires;
			ret = 1;
			goto out_unlock;
		}
	} else {
		base = zicio_lock_request_timer_base(
			timer, global_device_idx, &flags);
		zicio_forward_request_timer_base(base);
	}

	ret = zicio_detach_request_timer_if_pending(timer, base);
	if (!ret && (options & ZICIO_MOD_REQUEST_TIMER_PENDING_ONLY))
		goto out_unlock;

	timer->expires = expires;
	/*
	 * If 'idx' was calculated above and the base time did not advance between
	 * calculating 'idx' and possibly switching the base, 
	 * only zicio_enqueue_timer() is requeired. Otherwise we need to
	 * (re)calculate the wheel index via zicio_internal_add_timer().
	 */
	if (idx >= -1 && clk == base->clk)
		zicio_enqueue_request_timer(base, timer, idx, bucket_expiry);
	else
		zicio_internal_add_request_timer(base, timer);

out_unlock:
	raw_spin_unlock_irqrestore(&base->lock, flags);
	return ret;
}

/**
 * zicio_mod_request_timer_pending - modify a pending timer's timeout
 * @timer: the pending timer to be modified
 * @expires: new timeout in ktime_t
 * @global_device_idx: global raw device index
 *
 * This function corresponds to the mod_timer_pending() in kernel/time/timer.c
 *
 * Note that "expires" is a raw ktime_t. 
 * That is, ZICIO_KTIME_DEFAULT_CLK_SHIFT is not applied yet. 
 */
int zicio_mod_request_timer_pending(
	struct zicio_request_timer *timer,
	ktime_t expires, int global_device_idx)
{
	if (WARN_ON_ONCE(timer == NULL))
		return -1;
	return __zicio_mod_request_timer(timer, expires, 
			ZICIO_MOD_REQUEST_TIMER_PENDING_ONLY, global_device_idx);
}
EXPORT_SYMBOL(zicio_mod_request_timer_pending);

/**
 * zicio_mod_request_timer - modify a timer's timeout
 * @timer: the timer to be modified
 * @expires: new timeout in ktime_t
 * @global_device_idx: global raw device index
 *
 * This function corresponds to the mod_timer() in kernel/time/timer.c
 *
 * Note that "expires" is a raw ktime_t.
 * That is, ZICIO_KTIME_DEFAULT_CLK_SHIFT is not applied yet. 
 */
int zicio_mod_request_timer(
	struct zicio_request_timer *timer, 
	ktime_t expires, int global_device_idx)
{
	if (WARN_ON_ONCE(timer == NULL))
		return -1;
	return __zicio_mod_request_timer(timer, expires, 0, global_device_idx);
}
EXPORT_SYMBOL(zicio_mod_request_timer);

/**
 * zicio_request_timer_reduce - Modify a timer's timeout if it would reduce the
 * timeout
 * @timer: The timer to be modified
 * @expires: New timeout in ktime_t
 * @global_device_idx: global raw device index
 *
 * This function corresponds to the timer_reduce() in kernel/time/timer.c
 *
 * Note that "expires" is a raw ktime_t.
 * That is, ZICIO_KTIME_DEFAULT_CLK_SHIFT is not applied yet. 
 */
int zicio_request_timer_reduce(
	struct zicio_request_timer *timer, 
	ktime_t expires, int global_device_idx)
{
	if (WARN_ON_ONCE(timer == NULL))
		return -1;
	return __zicio_mod_request_timer(timer, expires,
			ZICIO_MOD_REQUEST_TIMER_REDUCE, global_device_idx);
}
EXPORT_SYMBOL(zicio_request_timer_reduce);

/**
 * zicio_add_request_timer - add a request timer on this CPU.
 * @timer: the timer to be added
 * @expire: time to expire
 * @global_device_idx: global raw device index
 *
 * This function corresponds to the add_timer() in kernel/time/timer.c
 *
 * The kernel will submit a ->command(@timer) from the I/O interrupt handler. 
 * It could possibly be done at a point prior to expiration timimng.
 *
 * The timer's ->expires, ->command fields must be set prior calling this
 * function.
 *
 * By default, the timer enters the timer base of the current cpu.
 * If caller wants to set this differenctly, the caller must call
 * zicio_add_request_timer_on().
 *
 * Note that "expires" is a raw ktime_t.
 * That is, ZICIO_KTIME_DEFAULT_CLK_SHIFT is not applied yet. 
 */
void zicio_add_request_timer(
	struct zicio_request_timer *timer,
	ktime_t expires, int global_device_idx)
{
	struct zicio_request_timer_base *base;

	if (WARN_ON_ONCE(timer == NULL))
		return;
	if (WARN_ON_ONCE(zicio_request_timer_pending(timer)))
		return;

	base = zicio_get_this_cpu_ptr_with_dev(request_timer_base, 0);

	/* Set the timer's cpu to this cpu as default */
	timer->flags = (timer->flags & ~ZICIO_REQUEST_TIMER_CPUMASK) 
					| (base->cpu & ZICIO_REQUEST_TIMER_CPUMASK);

	__zicio_mod_request_timer(timer, expires,
		ZICIO_MOD_REQUEST_TIMER_NOTPENDING, global_device_idx);
}
EXPORT_SYMBOL(zicio_add_request_timer);

/**
 * zicio_add_request_timer_on - add a request timer on a particular CPU.
 * @timer: the timer to be added
 * @expire: time to expire
 * @cpu: the CPU to start it on
 * @global_device_idx: global raw device index
 *
 * This function corresponds to the add_timer_on() in kernel/time/timer.c
 *
 * Note that "expires" is a raw ktime_t.
 * That is, ZICIO_KTIME_DEFAULT_CLK_SHIFT is not applied yet. 
 */
void zicio_add_request_timer_on(
	struct zicio_request_timer *timer,
	ktime_t expires, int cpu, int global_device_idx)
{
	struct zicio_request_timer_base *base;
	unsigned long flags;

	if (WARN_ON_ONCE(timer == NULL))
		return;
	if (WARN_ON_ONCE(zicio_request_timer_pending(timer)))
		return;

	/* Change raw ktime_t to the default unit for level 0 */
	expires = zicio_ktime_get_default_unit(expires);
	timer->expires = expires;

	/* Set the timer's cpu to the specified cpu */
	timer->flags = (timer->flags & ~ZICIO_REQUEST_TIMER_CPUMASK) 
					| (cpu & ZICIO_REQUEST_TIMER_CPUMASK);

	base = zicio_lock_request_timer_base(timer, global_device_idx, &flags);
	zicio_forward_request_timer_base(base);
	zicio_internal_add_request_timer(base, timer);
	raw_spin_unlock_irqrestore(&base->lock, flags);
}

/**
 * zicio_del_request_timer - deactivate a timer.
 * @timer: the timer to be deactivated
 * @global_device_idx: global raw device index
 *
 * This function corresponds to the del_timer() in kernel/time/timer.c
 */
int zicio_del_request_timer(struct zicio_request_timer *timer,
			int global_device_idx)
{
	struct zicio_request_timer_base *base;
	unsigned long flags;
	int ret = 0;

	if (WARN_ON_ONCE(timer == NULL))
		return -1;

	if (zicio_request_timer_pending(timer)) {
		base = zicio_lock_request_timer_base(
			timer, global_device_idx, &flags);
		ret = zicio_detach_request_timer_if_pending(timer, base);
		raw_spin_unlock_irqrestore(&base->lock, flags);
	}

	return ret;
}
EXPORT_SYMBOL(zicio_del_request_timer);

/**
 * zicio_del_zombie_request_timer - deactivate a timer.
 * @zombie_timer: the zombie timer to be deactivated
 * @global_device_idx: global raw device index
 *
 * This function corresponds to the del_timer() in kernel/time/timer.c
 */
int zicio_del_zombie_request_timer(
			struct zicio_zombie_request_timer *zombie_timer,
			int global_device_idx)
{
	struct zicio_request_zombie_timer_base *base;
	unsigned long flags;
	int ret = 0;

	if (WARN_ON_ONCE(zombie_timer == NULL))
		return -1;

	if (zicio_zombie_request_timer_pending(zombie_timer)) {
		base = zicio_lock_zombie_request_timer_base(
			zombie_timer, global_device_idx, &flags);
		ret = zicio_detach_zombie_request_timer_if_pending(zombie_timer,
			base);
		raw_spin_unlock_irqrestore(&base->lock, flags);
	}

	return ret;
}
EXPORT_SYMBOL(zicio_del_zombie_request_timer);

/**
 * zicio_next_pending_bucket - find next bit in the pending map.
 * @base: timer wheel base
 * @offset: first offset of the target level
 * @clk: offset within the same level of the target clock
 *
 * This function corresponds to the next_pending_bucket() in kernel/time/timer.c
 *
 * Find the next pending bucket of a level. Search from level start (@offset) +
 * @clk upwards and if nothing there, search from start of the level (@offset)
 * up to @offset + clk.
 *
 * Note that the ktime_t values used here are in the context of
 * ZICIO_KTIME_DEFAULT_CLK_SHIFT applied.
 */
static int zicio_next_pending_bucket(
			struct zicio_request_timer_base *base, unsigned offset,
			ktime_t clk)
{
	unsigned pos, start = offset + clk;
	unsigned end = offset + ZICIO_LVL_SIZE;

	pos = find_next_bit(base->pending_map, end, start);
	if (pos < end)
		return pos - start;

	pos = find_next_bit(base->pending_map, start, offset);
	return pos < start ? pos + ZICIO_LVL_SIZE - start : -1;
}

/**
 * zicio_next_expire_time - calculate new expire time
 * @base: timer wheel base
 *
 * This function corresponds to the __next_timer_interrupt()
 * in kernel/time/timer.c
 *
 * Search the first expiring timer in the various clock levels. Caller must hold
 * ->lock(@base).
 *
 * Returns next expiry time.
 *
 * Note that the ktime_t values used here are in the context of
 * ZICIO_KTIME_DEFAULT_CLK_SHIFT-applied.
 */
static ktime_t
zicio_next_expire_time(struct zicio_request_timer_base *base)
{
	ktime_t clk, next;
	unsigned lvl, offset, adj = 0;

	next = KTIME_MAX;
	clk = base->clk;
	for (lvl = 0, offset = 0; lvl < ZICIO_LVL_DEPTH; 
			lvl++, offset += ZICIO_LVL_SIZE) {
		int pos = zicio_next_pending_bucket(base, offset,
							clk & ZICIO_LVL_MASK);
		ktime_t lvl_clk = clk & ZICIO_LVL_CLK_MASK;

		if (pos >= 0) {
			ktime_t tmp = clk + (ktime_t) pos;

			tmp <<= ZICIO_LVL_SHIFT(lvl);
			if (ktime_before(tmp, next))
				next = tmp;

			/*
			 * If the next expiration happens before we reach the next level, no
			 * need to check further.
			 */
			if (pos <= ((ZICIO_LVL_CLK_DIV - lvl_clk) 
						& ZICIO_LVL_CLK_MASK))
				break;
		}

		/*
		 * If the next expiration is outside the granularity of the current
		 * level, the next level must be checked.
		 */
		adj = lvl_clk ? 1 : 0;
		clk >>= ZICIO_LVL_CLK_SHIFT;
		clk += adj;
	}

	return next;
}

/**
 * zicio_collect_expired_request_timers - collect expired request timers
 * @base: timer wheel base
 *
 * This function corresponds to the collect_expired_timers()
 * in kernel/time/timer.c
 *
 * It is guaranteed that the current time has passed ->next_expiry(@base). 
 * So set the ->clk(@base) to the ->next_expiry(@base) and move the expired
 * request timers to the ->expired_queue(@base).
 */
static void
zicio_collect_expired_request_timers(
	struct zicio_request_timer_base *base)
{
	ktime_t clk = base->clk = base->next_expiry;
	struct list_head *vec;
	unsigned int idx;
	int i;

	for (i = 0; i < ZICIO_LVL_DEPTH; i++) {
		idx = (clk & ZICIO_LVL_MASK) + i * ZICIO_LVL_SIZE;

		if (__test_and_clear_bit(idx, base->pending_map)) {
			vec = base->vectors + idx;
			list_bulk_move_tail(&base->expired_queue, vec->next, vec->prev);
		}
		/* Is it time to look at the next level? */
		if (clk & ZICIO_LVL_CLK_MASK)
			break;
		/* Shift clock for the next level granularity */
		clk >>= ZICIO_LVL_CLK_SHIFT;
	}
}

/**
 * __zicio_dequeue_request_timers - dequeue request timer
 * @old_head: old head 
 * @new_head: new head
 * @reuqest_count: maximum number of request to move
 *
 * This is a helper function to dequeue request timers.
 *
 * Returns moved request timer count
 */
static unsigned int
__zicio_dequeue_request_timers(
	struct list_head *old_head, struct list_head *new_head,
	unsigned int request_count)
{
	unsigned int i = 0;

	if (!list_empty(old_head) && request_count) {
		struct list_head *first = old_head->next;
		struct list_head *last = first;

		for (i = 1; i < request_count; i++) {
			if (list_is_last(last, old_head))
				break;
			last = last->next;
		}

		list_bulk_move_tail(new_head, first, last);
	}

	return i;
}

/**
 * zicio_dequeue_expired_request_timers - dequeue request timers from
 * expired_queue
 * @base: timer wheel base
 * @request_count: maximum number of reqeust to move
 *
 * Returns remaining count.
 */
static unsigned int
zicio_dequeue_expired_request_timers(
	struct zicio_request_timer_base *base, unsigned int request_count)
{
	unsigned int count = __zicio_dequeue_request_timers(
					&base->expired_queue, &base->running_timers, request_count);
	return (request_count - count);
}

/**
 * zicio_dequeue_next_expire_request_timer - dequeue timers
 * @base: timer wheel base
 * @request_count: maximum coun of moved request timers
 * @next_expiry_recalc: whether to recalculate the expiry time or not
 *
 * This function is similar to the zicio_collect_expired_request_timers().
 * However it is not guaranteed that the current time has passed. So do not set
 * the ->clk(@base). Just find the nearest pending bit and move the request
 * timers to ret->head.
 *
 * Note that @next_expiry_recalc is assumed true at the begining.
 *
 * Returns remaining count.
 */
static unsigned int
__zicio_dequeue_next_expire_request_timers(
	struct zicio_request_timer_base *base,
	unsigned int request_count,
	bool *next_expiry_recalc)
{
	ktime_t clk = base->next_expiry;
	unsigned int count, idx;
	struct list_head *vec;
	int i;

	BUG_ON(*next_expiry_recalc == false);

	for (i = 0; i < ZICIO_LVL_DEPTH; i++) {
		idx = (clk & ZICIO_LVL_MASK) + i * ZICIO_LVL_SIZE;

		if (test_bit(idx, base->pending_map)) {
			vec = base->vectors + idx;
			count = __zicio_dequeue_request_timers(
						vec, &base->running_timers, request_count);
			request_count -= count;

			if (list_empty(vec))
				__clear_bit(idx, base->pending_map);
			else
				*next_expiry_recalc = false;
		}

		/* Is it time to look at the next level? */
		if (clk & ZICIO_LVL_CLK_MASK)
			break;
		/* Shift clock for the next level granularity */
		clk >>= ZICIO_LVL_CLK_SHIFT;
	}

	return request_count;
}

/**
 * zicio_dequeue_non_expired_request_timer - dequeue non-expired timers
 * @base: timer wheel base
 * @request_count: maximum coun of moved request timers
 *
 * This function fetches requests that have not yet expired to get as many
 * requests as desired.
 *
 * Therefore, if it is not satisfied at the current next expiration time, the
 * next expiration time is continuously updated.
 *
 * Returns remaining count.
 */
static unsigned int
zicio_dequeue_non_expired_request_timers(
	struct zicio_request_timer_base *base,
	unsigned int request_count)
{
	bool next_expiry_recalc = true;

	while (request_count) {
		request_count = __zicio_dequeue_next_expire_request_timers(
						base, request_count, &next_expiry_recalc);
		
		if (next_expiry_recalc) {
			base->next_expiry = zicio_next_expire_time(base);
		} else {
			next_expiry_recalc = true;
		}

		if (base->next_expiry == KTIME_MAX)
			request_count = 0;
	}

	return request_count;
}

/**
 * __zicio_fetch_next_request_timers
 * @base: the timer wheel to be processed
 * @request_count: the number of requests to fetch
 * @fetch_flag: determines the types of requests that can be fetched
 *
 * This function corresponds to the __run_timers() in kernel/time/timer.c
 *
 * However, __zicio_fetch_next_request_timers() has differences from
 * __run_timers().
 *
 * (1) __zicio_fetch_next_request_timers does not use expiry_lock.
 *
 *   The original timer wheel has expiry_lock. This is used to increase the
 *   responsiveness of real-time systems. The original timer has a callback
 *   function and expiry_lock is acquired/released every callback function's
 *   invoke/end. This gave an opportunity to execute the callback function in 
 *   other execution flows that want to run on the same timer base.
 *
 *   However, zicio's request timer has no callback function. So all the
 *   __zicio_fetch_next_request_timers do here is only changing the
 *   link of the timers on the timer base.
 *
 *   That's why __zicio_fetch_next_request_timers only holds base->lock like
 *   the __zicio_mod_request_timer.
 *
 * (2) A new memeber, ->expired_queue is used.
 *
 *   The original timer wheel executes callback functions of *all* timers
 *   corresponding to the expire time. However, here, only the requests
 *   corresponding to the expire time need to be returned to the caller (even
 *   those not corresponding to the expire time can be returned depnding on the
 *   flag).
 *
 *   The problem is that the caller does not require all requests that match the
 *   condition. Therefore, the remaining request timers may be taking up space
 *   on the timer wheel.
 *
 *   This causes new request timers added in the future to go to a higher
 *   level. This may be a factor that unnecessarily lowers the accuracy of the
 *   timer wheel.
 *
 *   Therefore, in this function, request timers whose expire time has passed
 *   are detached from the timer wheel and moved to the queue. The reason we can
 *   use a queue here is that theses requests already have a fixed priority.
 *
 * Note that this function must be protected by caller.
 */
static inline void
__zicio_fetch_next_request_timers(
	struct zicio_request_timer_base *base,
	unsigned int request_count, int fetch_flag)
{
	/* Newly obtained ktime_t must be processed to the unit of level 0 */	
	ktime_t know = zicio_ktime_get_default_unit(ktime_get());

	/* Move expired requests to ->expired_queue(@base) */
	while (know >= base->clk && know >= base->next_expiry) {
		zicio_collect_expired_request_timers(base);
		base->clk++;
		base->next_expiry = zicio_next_expire_time(base);
	}

	/* expired queue => returing list (expired) */
	request_count = zicio_dequeue_expired_request_timers(
					base, request_count);

	/* timer wheel => returning list (non-expired) */
	if (!(fetch_flag & ZICIO_REQUEST_TIMER_FETCH_ONLY_EXPIRED))
		zicio_dequeue_non_expired_request_timers(base, request_count);
}

/**
 * zicio_get_reqest_timer - get request timer from firehose controller
 * @zicio_desc: target zicio descriptor
 * @zicio_cmd_list: zicio nvme command list
 *
 * Get fresh request timer from firehose controller.
 */
struct zicio_request_timer *
zicio_get_request_timer(struct zicio_descriptor *zicio_desc,
	struct zicio_nvme_cmd_list *zicio_cmd_list)
{
	struct zicio_request_timer *req_timer = zicio_alloc_request_timer();
	struct zicio_firehose_ctrl *firehose_ctrl = &zicio_desc->firehose_ctrl;
	unsigned long flags;

	/*
	 * If the process terminates unexpectedly, all allocated request timers must
	 * be removed. Therefore, we keep track of valid request timers in the
	 * firehose controller.
	 */
	spin_lock_irqsave(&firehose_ctrl->lock, flags);
	ZICIO_INIT_REQUEST_TIMER(req_timer, zicio_desc, zicio_cmd_list);
	list_add_tail(&req_timer->sibling, &firehose_ctrl->active_req_timers);
	spin_unlock_irqrestore(&firehose_ctrl->lock, flags);
	return req_timer;
}
EXPORT_SYMBOL(zicio_get_request_timer);

/**
 * zicio_get_zombie_reqest_timer - get request timer from firehose controller
 * @zicio_desc: target zicio descriptor
 * @zicio_cmd_list: zicio nvme command list
 *
 * Get fresh request timer from firehose controller.
 */
struct zicio_zombie_request_timer *
zicio_get_zombie_request_timer(struct zicio_descriptor *zicio_desc,
	struct zicio_nvme_cmd_list *zicio_cmd_list)
{
	struct zicio_zombie_request_timer *req_timer =
				zicio_alloc_request_timer();
	struct zicio_firehose_ctrl *firehose_ctrl = &zicio_desc->firehose_ctrl;
	unsigned long flags;

	/*
	 * If the process terminates unexpectedly, all allocated request timers must
	 * be removed. Therefore, we keep track of valid request timers in the
	 * firehose controller.
	 */
	spin_lock_irqsave(&firehose_ctrl->lock, flags);
	ZICIO_INIT_ZOMBIE_REQUEST_TIMER(req_timer, zicio_desc, zicio_cmd_list);
	list_add_tail(&req_timer->sibling,
			&firehose_ctrl->active_zombie_req_timers);
	spin_unlock_irqrestore(&firehose_ctrl->lock, flags);
	return req_timer;
}
EXPORT_SYMBOL(zicio_get_zombie_request_timer);

/**
 * zicio_put_request_timer - give back the timer to descriptor
 * @req_timer: recycling request timer
 *
 * Give the request timer back to the slab.
 */
void zicio_put_request_timer(
	struct zicio_request_timer *req_timer)
{
	struct zicio_descriptor *zicio_desc = req_timer->zicio_desc;
	struct zicio_firehose_ctrl *firehose_ctrl;
	unsigned long flags;

	BUG_ON(req_timer->zicio_cmd_list != NULL);
	BUG_ON(req_timer->zicio_desc == NULL);
	zicio_desc = req_timer->zicio_desc;
	firehose_ctrl = &zicio_desc->firehose_ctrl;

	/*
	 * This request timer is no longer active, so there is no need to track it
	 * in the firehose controller.
	 */
	spin_lock_irqsave(&firehose_ctrl->lock, flags);
	list_del_init(&req_timer->sibling);
	ZICIO_INIT_REQUEST_TIMER(req_timer, NULL, NULL);
	zicio_free_request_timer(req_timer);
	spin_unlock_irqrestore(&firehose_ctrl->lock, flags);
}
EXPORT_SYMBOL(zicio_put_request_timer);

/**
 * zicio_put_zombie_request_timer - give back the timer to descriptor
 * @req_timer: recycling request timer
 *
 * Give the request timer back to the slab.
 */
void zicio_put_zombie_request_timer(
	struct zicio_zombie_request_timer *req_timer)
{
	struct zicio_descriptor *zicio_desc = req_timer->zicio_desc;
	struct zicio_firehose_ctrl *firehose_ctrl;
	unsigned long flags;

	BUG_ON(req_timer->zicio_cmd_list != NULL);
	BUG_ON(req_timer->zicio_desc == NULL);
	zicio_desc = req_timer->zicio_desc;
	firehose_ctrl = &zicio_desc->firehose_ctrl;

	/*
	 * This request timer is no longer active, so there is no need to track it
	 * in the firehose controller.
	 */
	spin_lock_irqsave(&firehose_ctrl->lock, flags);
	list_del_init(&req_timer->sibling);
	ZICIO_INIT_ZOMBIE_REQUEST_TIMER(req_timer, NULL, NULL);
	zicio_free_request_timer(req_timer);
	spin_unlock_irqrestore(&firehose_ctrl->lock, flags);
}
EXPORT_SYMBOL(zicio_put_zombie_request_timer);

/**
 * zicio_hang_zombie_request_timer - make zombie request timer
 * @zicio_desc: zicio descriptor
 * @zicio_cmd_list: nvme commands to be revived when a zomibe request is activated.
 * @global_device_idx: global raw device index
 *
 * When a chunk id is not allocated in the I/O handler, call this function to
 * create a zombie requset timer.
 */
void zicio_hang_zombie_request_timer(
	struct zicio_descriptor *zicio_desc,
	struct zicio_nvme_cmd_list **zicio_start_cmd_lists, int num_device,
	int global_fs_device_idx)
{
	struct zicio_request_zombie_timer_base *zombie_base;
	struct zicio_zombie_request_timer *zombie;
	unsigned long flags;

	zombie_base = zicio_get_zombie_timer(&global_zombie_timers,
		zicio_desc->cpu_id, global_fs_device_idx);

	/* Create zombie request timer */
	zombie = zicio_get_zombie_request_timer(zicio_desc, zicio_start_cmd_lists[0]);
	zombie->flags = (zombie->flags & ~ZICIO_REQUEST_TIMER_CPUMASK) 
					| (zicio_desc->cpu_id & ZICIO_REQUEST_TIMER_CPUMASK);
	zombie->local_huge_page_idx = ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX;
	zombie->num_dev = num_device;
	zombie->zicio_start_cmd_lists = zicio_start_cmd_lists;

	/* Push it to the zombie timer list */
	raw_spin_lock_irqsave(&zombie_base->lock, flags);
	list_add_tail(&zombie->entry, &zombie_base->zombie_timers);
	raw_spin_unlock_irqrestore(&zombie_base->lock, flags);
}
EXPORT_SYMBOL(zicio_hang_zombie_request_timer);

/**
 * zicio_revive_zombie - hang the revived zombie's zicio_cmd_list to wheel
 * @base: request timer wheel
 * @zombie: zombie request timer to revive
 * @local_huge_page_idx: newly activated local huge page id
 *
 * If a chunk is not allocated at the time of creating the NVMe command, it
 * becomes a zombie request timer. If the chunk id is newly assigned, reactivate
 * the zombie request timer and put it on the request timer wheel.
 */
static void
zicio_revive_zombie(struct zicio_request_timer_base *base,
	struct zicio_zombie_request_timer *zombie, int local_huge_page_idx)
{
	struct zicio_nvme_cmd_list *zicio_cmd_list = zombie->zicio_cmd_list;
	BUG_ON(zicio_cmd_list == NULL);

	zicio_set_zombie_command_list(zombie->zicio_desc,
		zombie->zicio_start_cmd_lists, local_huge_page_idx);
	zicio_lock_and_load(zombie->zicio_desc, zombie->zicio_start_cmd_lists,
		zombie->num_dev, local_huge_page_idx);
	zombie->zicio_cmd_list = NULL;
	zombie->zicio_start_cmd_lists = NULL;
}

/**
 * zicio_try_reactivate_zombies - make invalid the nvme commands in zomibes.
 * @base: request timer wheel
 *
 * Zombie request timers are caused by not being allocated a chunk to send NVMe
 * commands.
 *
 * Try to reassign chunks to bring them back to life. If assigned, hang the live
 * request timers on the timer wheel as we did in the original zicio I/O
 * handler.
 *
 * Return the number of remained zombie timers.
 */
static int zicio_try_reactivate_zombies(
	struct zicio_request_timer_base *base,
	struct zicio_request_zombie_timer_base *zombie_base)
{
	struct list_head *pos, *n, active_timers;
	struct zicio_zombie_request_timer *zombie;
	unsigned long flags;
	int remain = 0;

	/* Move revivable zomibe timers to the local timer list */
	INIT_LIST_HEAD(&active_timers);
	raw_spin_lock_irqsave(&zombie_base->lock, flags);
	list_for_each_safe(pos, n, &zombie_base->zombie_timers) {
		zombie = (struct zicio_zombie_request_timer *)pos;
		BUG_ON(zombie->zicio_desc == NULL || zombie->zicio_cmd_list == NULL);

		/*
	 	 * Try to prepare new chunk and reactivate the zombie timer when the
	 	 * returned chunk id is not a ZICIO_INVALID_CHUNK_ID.
	 	 */
		zombie->local_huge_page_idx = zicio_prepare_next_local_huge_page_id(
								zombie->zicio_desc);
		if (zombie->local_huge_page_idx != ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX)
			list_move_tail(&zombie->entry, &active_timers);
		else
			remain++;
	}
	raw_spin_unlock_irqrestore(&zombie_base->lock, flags);

	/* Revive zombie timers */
	list_for_each_safe(pos, n, &active_timers) {
		zombie = (struct zicio_zombie_request_timer *)pos;
		zicio_revive_zombie(base, zombie, zombie->local_huge_page_idx);
		zicio_put_zombie_request_timer(zombie);
	}

	return remain;
}

/**
 * zicio_try_trigger_softtimer_timer_softirq - make softirq for triggering
 * @base: request timer wheel
 * @interval: jiffies interval to expire
 *
 * If the request timer wheel stops, a softirq is issued later to restart the
 * timer wheel.
 *
 * Note that we have to avoid the same timer is added to the wheel.
 * So use the @trigger_running.
 */
static void zicio_try_trigger_softtimer_timer_softirq(
	struct zicio_request_timer_base *base, unsigned long interval)
{
	if (atomic_cmpxchg(&base->trigger_running, 0, 1) == 0) {
		BUG_ON(timer_pending(&base->trigger));
		timer_setup(&base->trigger,
			zicio_softtimer_timer_softirq_callback, TIMER_PINNED);
		base->trigger.expires = get_jiffies_64() + interval;
		add_timer_on(&base->trigger, base->cpu);
	}
}

/**
 * zicio_trigger_softtimer_timer_softirq - try to trigger softirq
 * @cpu: cpu id
 * @global_device_idx: global raw device index
 * @interval: jiffies interval to expire
 *
 * Try to create softirq which try to get new requests.
 */
void zicio_trigger_softtimer_timer_softirq(
			int cpu, int global_device_idx, unsigned long interval)
{
	struct zicio_request_timer_base *base;
	base = zicio_get_per_cpu_ptr_with_dev(
		request_timer_base, cpu, global_device_idx);
	zicio_try_trigger_softtimer_timer_softirq(base, interval);
}
EXPORT_SYMBOL(zicio_trigger_softtimer_timer_softirq);

/**
 * zicio_expiry_interval_as_jiffies - get the time interval to next expiry
 * @base: request timer wheel
 *
 * Using the ->next_expiry(@base) and current time, get the time interval to the
 * next expiration time as jiffies.
 *
 * Note that an interval is at least 1. This is because the interval value will
 * be reduced by 1 in consideration of scheduling.
 */
static unsigned long zicio_expiry_interval_as_jiffies(
	struct zicio_request_timer_base *base)
{
	unsigned long interval = ZICIO_DEFAULT_TIMER_SOFTIRQ_INTERVAL;
	ktime_t next_expiry, know, diff;

	if (base->next_expiry != KTIME_MAX) {
		next_expiry = zicio_ktime_get_from_default_unit(base->next_expiry);
		know = ktime_get();
		diff = next_expiry - know;
		interval = 1;
		if (diff > 0) {
			interval = nsecs_to_jiffies(ktime_to_ns(diff));
			if (interval < 1)
				interval = 1;
		}
	}
	return interval;
}

/**
 * zicio_wakeup_softirqd - wake up softirq deamon
 * @cpu: cpu id
 *
 * Try to wake up softirq deamon if it is not running.
 */
int zicio_wakeup_softirqd(int cpu)
{
	struct task_struct *tsk = per_cpu(ksoftirqd, cpu);
	unsigned long flags;
	int wakeup = 0;

	BUG_ON(tsk == NULL);

	local_irq_save(flags);
	if (!task_is_running(tsk) || __kthread_should_park(tsk))
		wakeup = wake_up_process(tsk);
	local_irq_restore(flags);

	return wakeup;
}
EXPORT_SYMBOL(zicio_wakeup_softirqd);

/**
 * zicio_softtimer_timer_softirq_callback - callback function of softirq.
 * timer: timer softirq's argument.
 *
 * This function is a callback function of timer softirq which is one of the
 * softtimer for zicio.
 */
static void zicio_softtimer_timer_softirq_callback(struct timer_list *timer)
{
	struct zicio_request_timer_base *base = from_timer(base,timer,trigger);
	int wakeup = 0;

	/* We already detached timer_list, so we can re-add it */
	atomic_set(&base->trigger_running, 0);

	/*
	 * This callback function may be executed immediately after handling
	 * interrupts. Therefore, instead of running softtimer directly here, let
	 * ksoftirqd run the softtimer function in the context of the process. 
	 */
	wakeup = zicio_wakeup_softirqd(base->cpu);
	if (!wakeup)
		zicio_trigger_softtimer_timer_softirq(base->cpu, base->dev_idx, 0);
}

/**
 * __zicio_fetch_nvme_command - fetch the next nvme command
 * @ret: returning zicio_nvme_cmd_desc structure
 * @fetch_flag: determines the types of requests that can be fetched
 * @base: request timer wheel
 *
 * Helper function for zicio_fetch_nvme_command().
 *
 * Fetch the nvme command from ->running_timers(@base).
 * If the list was empty, try to dequeue request timers.
 */
static int  __zicio_fetch_nvme_command(
	struct zicio_nvme_cmd_desc *ret, unsigned int fetch_flag,
	struct zicio_request_timer_base *base)
{
	struct zicio_request_timer *req_timer = NULL;
	int fetched = 0, req_timer_free = 0;
	unsigned long flags;

	raw_spin_lock_irqsave(&base->lock, flags);

	/* If there is no running timers, prepare it */
	if (list_empty(&base->running_timers))
		__zicio_fetch_next_request_timers(base, 1, fetch_flag);

	if (!list_empty(&base->running_timers)) {
		req_timer = (struct zicio_request_timer *)base->running_timers.next;
		BUG_ON(req_timer->zicio_desc == NULL || req_timer->zicio_cmd_list == NULL);

		/* Save the results */
		ret->zicio_desc = req_timer->zicio_desc;
		ret->zicio_cmd = req_timer->zicio_cmd_list;

		/* Disconnect the fetched one from wheel */
		req_timer->zicio_cmd_list = ret->zicio_cmd->next;
		ret->zicio_cmd->next = NULL;		
		fetched = 1;

		/*
		 * If this request timer has no nvme command anymore, detach it from the
		 * wheel. Note that we free it after unlock the ->lock(@base) to avoid
		 * deadlock.
		 */
		if (req_timer->zicio_cmd_list == NULL) {
			list_del(&req_timer->entry);
			req_timer_free = 1;
		}
	}

	raw_spin_unlock_irqrestore(&base->lock, flags);

	if (req_timer_free)
		zicio_put_request_timer(req_timer);

	return fetched;
}

/**
 * zicio_fetch_nvme_command - fetch the next nvme command
 * @ret: returning zicio_nvme_cmd_desc structure
 * @fetch_flag: determines the types of requests that can be fetched
 * @cpu: cpu id
 * @global_device_idx: global raw device index
 *
 * This function is called to fetch the next request in the I/O handler that
 * handles zicio's I/O request.
 *
 * Returns the most urgent nvme command from the timer wheel.
 *
 * The number of requests received may be smaller than the value entered as a
 * argument(@request_count).
 *
 * Note that if there is no received request timers, we expire the timer softirq
 * to the next expiry time.
 */
void zicio_fetch_nvme_command_desc(
	struct zicio_nvme_cmd_desc *ret, unsigned int fetch_flag, int cpu,
	int global_device_idx)
{
	struct zicio_device *zicio_device;
	struct zicio_id_iterator zicio_id_iter;
	struct zicio_request_timer_base *base;
	struct zicio_request_zombie_timer_base *zombie_base;
	unsigned long interval;
	int fetched, nr_zombie = 0, global_fs_device_idx;

	if (WARN_ON_ONCE(ret == NULL))
		return;

	/* Initialize returning descriptor */
	ret->zicio_desc = NULL;
	ret->zicio_cmd = NULL;

	base = zicio_get_per_cpu_ptr_with_dev(
		request_timer_base, cpu, global_device_idx);
	zicio_device = zicio_get_zicio_device(global_device_idx);

	zicio_id_iter.curr = 1;
	zicio_id_iter.dest = zicio_get_num_fs_device(zicio_device) + 1;

	while ((global_fs_device_idx = zicio_get_next_fs_device_idx(zicio_device,
					&zicio_id_iter)) != -1) {
		zombie_base = zicio_get_zombie_timer(&global_zombie_timers, cpu,
				(int)global_fs_device_idx);

		/* Before getting nvme command, try to revive zombie commands */
		nr_zombie += zicio_try_reactivate_zombies(base, zombie_base);
	}

	fetched = __zicio_fetch_nvme_command(ret, fetch_flag, base);

	if (!fetched) {
		interval = zicio_expiry_interval_as_jiffies(base);

		/* If there is no active timers, but zombie, adjust interval */
		if (interval == ZICIO_DEFAULT_TIMER_SOFTIRQ_INTERVAL && nr_zombie)
			interval = ZICIO_DEFAULT_ZOMBIE_WAKEUP_INTERVAL;

		/* Subtract one jiffies to consider scheduling */
		zicio_try_trigger_softtimer_timer_softirq(base, interval - 1);
	}
}
EXPORT_SYMBOL(zicio_fetch_nvme_command_desc);

/*
 * Initialize request timer wheel for the given cpu
 */
static void __init __zicio_init_request_timer_wheel_cpu(
		struct zicio_request_timer_base *base, int cpu, int dev_idx)
{
	int i;

	base->cpu = cpu;
	raw_spin_lock_init(&base->lock);
	base->clk = zicio_ktime_get_default_unit(ktime_get());
	base->next_expiry = KTIME_MAX;
	base->dev_idx= dev_idx;

	for (i = 0; i < ZICIO_WHEEL_SIZE; i++) {
		INIT_LIST_HEAD(base->vectors + i);
	}
	INIT_LIST_HEAD(&base->running_timers);
	INIT_LIST_HEAD(&base->expired_queue);
	atomic_set(&base->trigger_running, 0);
}

static void zicio_install_zombie_timers_cpu(int cpu, int num_dev)
{
	struct zicio_request_zombie_timer_base *zombie_timer_base;
	struct zicio_id_allocator *zombie_timer_table;
	int global_device_idx, i;

	zombie_timer_table = per_cpu_ptr(&global_zombie_timers, cpu);

	for (i = 0 ; i < num_dev ; i++) {
		zombie_timer_base = kmalloc(
				sizeof(struct zicio_request_zombie_timer_base), GFP_KERNEL);
		INIT_LIST_HEAD(&zombie_timer_base->zombie_timers);
		raw_spin_lock_init(&zombie_timer_base->lock);

		global_device_idx = zicio_get_unused_id(zombie_timer_table);
		zicio_install_zicio_struct(zombie_timer_table, global_device_idx,
				zombie_timer_base);
	}
}

void zicio_install_zombie_timers(int num_dev)
{
	int cpu;
	for_each_possible_cpu(cpu)
		zicio_install_zombie_timers_cpu(cpu, num_dev);
}
EXPORT_SYMBOL(zicio_install_zombie_timers);

static void __init zicio_init_zombie_timers_cpu(int cpu)
{
	struct zicio_id_allocator *zombie_timers;

	zombie_timers = per_cpu_ptr(&global_zombie_timers, cpu);
	zicio_init_id_allocator(zombie_timers);
}

static void __init zicio_init_request_timer_wheel_cpu(int cpu, int num_dev)
{
	struct zicio_request_timer_base **base;
	int i;

	base = per_cpu_ptr(&request_timer_base, cpu);

	*base = (struct zicio_request_timer_base *)kmalloc(
				sizeof(struct zicio_request_timer_base) * num_dev,
						GFP_KERNEL|__GFP_ZERO);
	for (i = 0 ; i < num_dev ; i++) {
		__zicio_init_request_timer_wheel_cpu((*base) + i, cpu, i);
	}
}

void __init zicio_init_request_timer_wheel(int num_dev)
{
	int cpu;
	for_each_possible_cpu(cpu)
		zicio_init_request_timer_wheel_cpu(cpu, num_dev);
}
EXPORT_SYMBOL(zicio_init_request_timer_wheel);

void __init zicio_init_zombie_timers(void)
{
	int cpu;
	for_each_possible_cpu(cpu)
		zicio_init_zombie_timers_cpu(cpu);
}
EXPORT_SYMBOL(zicio_init_zombie_timers);

/*
 * Dump function for debugging
 */
void zicio_dump_request_timer_wheel(int cpu, int global_device_idx)
{
	struct zicio_request_timer_base *base;
	struct zicio_request_zombie_timer_base *zombie_base;
	struct zicio_id_iterator zicio_id_iter;
	struct zicio_device *zicio_device;
	struct list_head *pos;
	unsigned long flags;
	int i, count = 0, global_fs_device_idx;

	base = zicio_get_per_cpu_ptr_with_dev(
		request_timer_base, cpu, global_device_idx);
	zicio_device = zicio_get_zicio_device(global_device_idx);

	zicio_id_iter.curr = 1;
	zicio_id_iter.dest = zicio_get_num_fs_device(zicio_device) + 1;

	raw_spin_lock_irqsave(&base->lock, flags);

	list_for_each(pos, &base->running_timers) {
		count++;
	}
	printk(KERN_WARNING "[ZICIO_REQ] cpu: %d, running: %d\n", cpu, count);

	count = 0;
	list_for_each(pos, &base->expired_queue) {
		count++;
	}
	printk(KERN_WARNING "[ZICIO_REQ] cpu: %d, expired: %d\n", cpu, count);

	for (i = 0; i < ZICIO_WHEEL_SIZE; i++) {
		count = 0;
		list_for_each(pos, base->vectors + i) {
			count++;
		}
		printk(KERN_WARNING "[ZICIO_REQ] cpu: %d, vec%d: %d\n", cpu, i, count);
	}

	while ((global_fs_device_idx = zicio_get_next_fs_device_idx(zicio_device,
			&zicio_id_iter)) != -1) {
		zombie_base = zicio_get_zombie_timer(&global_zombie_timers,
			cpu, global_device_idx);
		raw_spin_lock_irqsave(&zombie_base->lock, flags);
		count = 0;
		list_for_each(pos, &zombie_base->zombie_timers) {
			count++;
		}
		printk(KERN_WARNING "[ZICIO_REQ] cpu: %d, zombie: %d\n", cpu, count);
		raw_spin_unlock_irqrestore(&zombie_base->lock, flags);
	}

	printk(KERN_WARNING "[ZICIO_REQ] cpu: %d, trigger_running: %d\n",
		cpu, atomic_read(&base->trigger_running));

	printk(KERN_WARNING "[ZICIO_REQ] cpu: %d, trigger expires: %ld, current jiffies: %lld\n",
		cpu, base->trigger.expires, get_jiffies_64());

	raw_spin_unlock_irqrestore(&base->lock, flags);
}
EXPORT_SYMBOL(zicio_dump_request_timer_wheel);
