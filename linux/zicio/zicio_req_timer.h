#ifndef _LINUX_ZICIO_REQUEST_TIMER_H
#define _LINUX_ZICIO_REQUEST_TIMER_H

#include <linux/blkdev.h>
#include <linux/list.h>
#include <linux/ktime.h>
#include <linux/zicio.h>
#include <linux/stddef.h>
#include <linux/debugobjects.h>
#include <linux/stringify.h>

/**
 * zicio_reuqest_timer
 * @entry: list_head to link timers with the same index on the timer wheel
 * @sibling: list_head to link timers with the same zicio descriptors
 * @expires: time to expire
 * @zicio_cmd_list: I/O command linked list
 * @zicio_desc: zicio descriptor to which the timer belongs
 * @flags: contains request timer's features like cpuid, idex, etc...
 * @local_huge_page_idx: local huge page idx of nvme commands
 * @num_dev: the number of device pending in this request timer
 * @last_cmd_lists: the last command per each device
 *
 * This data structure mimics timer_list in include/linux/timer.h to represent
 * timer. ZicIO uses this data structure to submit I/O requests at the 
 * right time.
 *
 * Unlike the original data structure, there are three differences.
 *
 * (1) No callback function but a request is added. 
 * (2) ZicIO does not use jiffies, the data type of ->expires is changed to
 *     ktime_t.
 * (3) Level 0 of request timer wheel uses a unit obtained by processing
 *     ktime_t, not a raw ktime_t. @expires represents the unit of
 *     level 0. 
 * (4) ZicIO use list_head to implement queue, not a hlist_node.
 */
struct zicio_request_timer {
	/*
	 * All fields that change during normal runtime grouped to the
	 * same cacheline
	 */
	struct list_head				entry;
	struct list_head				sibling;
	ktime_t							expires;
	struct zicio_descriptor		*zicio_desc;
	struct zicio_nvme_cmd_list	*zicio_cmd_list;
	u32								flags;
	s32								local_huge_page_idx;
	s32								num_dev;
	struct zicio_nvme_cmd_list	**zicio_start_cmd_lists;
} ____cacheline_aligned;

/**
 * zicio_zombie_reuqest_timer
 * @entry: list_head to link timers with the same index on the timer wheel
 * @sibling: list_head to link timers with the same zicio descriptors
 * @expires: time to expire
 * @zicio_cmd_list_set: I/O command linked list
 * @zicio_desc: zicio descriptor to which the timer belongs
 * @flags: contains request timer's features like cpuid, idex, etc...
 * @local_huge_page_idx: local huge page idx of nvme commands
 *
 * This data structure mimics timer_list in include/linux/timer.h to represent
 * timer. ZicIO uses this data structure to submit I/O requests at the
 * right time.
 *
 * Unlike the original data structure, there are three differences.
 *
 * (1) No callback function but a request is added.
 * (2) ZicIO does not use jiffies, the data type of ->expires is changed to
 *     ktime_t.
 * (3) Level 0 of request timer wheel uses a unit obtained by processing
 *     ktime_t, not a raw ktime_t. @expires represents the unit of
 *     level 0.
 * (4) ZicIO use list_head to implement queue, not a hlist_node.
 */
struct zicio_zombie_request_timer {
	/*
	 * All fields that change during normal runtime grouped to the
	 * same cacheline
	 */
	struct list_head				entry;
	struct list_head				sibling;
	ktime_t							expires;
	struct zicio_descriptor		*zicio_desc;
	struct zicio_nvme_cmd_list	*zicio_cmd_list;
	u32								flags;
	s32								local_huge_page_idx;
	s32								num_dev;
	struct zicio_nvme_cmd_list	**zicio_start_cmd_lists;
} ____cacheline_aligned;

/**
 * ZICIO_INIT_REQUEST_TIMER - Initialize a request timer
 * @timer: zicio_request_timer structure to be initialized
 *
 * Initializes the zicio_request_timer's entry to point to itself.
 */
static inline void 
ZICIO_INIT_REQUEST_TIMER(
		struct zicio_request_timer *timer,
		struct zicio_descriptor *zicio_desc,
		struct zicio_nvme_cmd_list *zicio_cmd_list)
{
	INIT_LIST_HEAD(&timer->entry);
	INIT_LIST_HEAD(&timer->sibling);
	timer->expires = 0;
	timer->zicio_desc = zicio_desc;
	timer->zicio_cmd_list = zicio_cmd_list;
	timer->flags = 0;
	timer->local_huge_page_idx = ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX;
}

/**
 * ZICIO_INIT_ZOMBIE_REQUEST_TIMER - Initialize a request timer
 * @timer: zicio_zombie_request_timer structure to be initialized
 *
 * Initializes the zicio_zombie_request_timer's entry to point to itself.
 */
static inline void
ZICIO_INIT_ZOMBIE_REQUEST_TIMER(
		struct zicio_zombie_request_timer *timer,
		struct zicio_descriptor *zicio_desc,
		struct zicio_nvme_cmd_list *zicio_cmd_list)
{
	INIT_LIST_HEAD(&timer->entry);
	INIT_LIST_HEAD(&timer->sibling);
	timer->expires = 0;
	timer->zicio_desc = zicio_desc;
	timer->zicio_cmd_list = zicio_cmd_list;
	timer->flags = 0;
	timer->local_huge_page_idx = ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX;
}

extern int zicio_mod_request_timer_pending(
			struct zicio_request_timer *timer,
			ktime_t expires, int global_device_idx);

extern int zicio_mod_request_timer(
			struct zicio_request_timer *timer,
			ktime_t expires, int global_device_idx);

extern int zicio_del_request_timer(
			struct zicio_request_timer *timer, int global_device_idx);

extern void zicio_add_request_timer(
			struct zicio_request_timer *timer,
			ktime_t expires, int global_device_idx);

extern void zicio_add_request_timer_on(
			struct zicio_request_timer *timer,
			ktime_t expires, int cpu, int global_device_idx);

extern int zicio_mod_zombie_request_timer_pending(
			struct zicio_zombie_request_timer *timer,
			ktime_t expires, int global_device_idx);

extern int zicio_mod_zombie_request_timer(
			struct zicio_zombie_request_timer *timer,
			ktime_t expires, int global_device_idx);

extern int zicio_del_zombie_request_timer(
			struct zicio_zombie_request_timer *timer,
			int global_device_idx);

extern void zicio_add_zombie_request_timer(
			struct zicio_zombie_request_timer *timer,
			ktime_t expires, int global_device_idx);

extern void zicio_add_zombie_request_timer_on(
			struct zicio_zombie_request_timer *timer,
			ktime_t expires, int cpu, int global_device_idx);

/**
 * Flags to determine the type of requests that can be retrieved by
 * zicio_fetch_next_reuqest_timers().
 *
 * @ZICIO_REQUEST_TIMER_FETCH_ONLY_EXPIRED: This flag tell us the caller
 * wants to get the expired requests.
 */
#define ZICIO_REQUEST_TIMER_FETCH_ONLY_EXPIRED		(0x01)

struct zicio_nvme_cmd_desc {
	struct zicio_descriptor *zicio_desc;
	struct zicio_nvme_cmd_list *zicio_cmd;
};

extern void zicio_fetch_nvme_command_desc(
	struct zicio_nvme_cmd_desc *ret, unsigned int flag, int cpu,
	int global_device_idx);

extern int zicio_wakeup_softirqd(int cpu);

extern struct zicio_request_timer *zicio_get_request_timer(
			struct zicio_descriptor *zicio_desc,
			struct zicio_nvme_cmd_list *zicio_cmd_list);

extern struct zicio_zombie_request_timer *
			zicio_get_zombie_request_timer(
						struct zicio_descriptor *zicio_desc,
						struct zicio_nvme_cmd_list *zicio_cmd_list);

extern void zicio_put_request_timer(
			struct zicio_request_timer *req_timer);

extern void zicio_put_zombie_request_timer(
			struct zicio_zombie_request_timer *req_timer);

extern void zicio_hang_zombie_request_timer(
			struct zicio_descriptor *zicio_desc,
			struct zicio_nvme_cmd_list **zicio_start_cmd_lists, int num_device,
			int global_fs_device_idx);

extern void __init zicio_init_request_timer_wheel(int num_dev);
extern void zicio_install_zombie_timers(int num_dev);
extern void __init zicio_init_zombie_timers(void);
extern void zicio_dump_request_timer_wheel(int cpu, int global_device_idx);
#endif	/* _LINUX_ZICIO_REQUEST_TIMER_H */
