#ifndef _UAPI_ZICIO_H
#define _UAPI_ZICIO_H

#define KZICIO_PAGE_SIZE	(4096)

enum entry_status {
	ENTRY_EMPTY=0, 		/* 0b00 */
	ENTRY_READY,	 	/* 0b01 */
	ENTRY_INUSE,		/* 0b10 */
	ENTRY_DONE,			/* 0b11 */
};

typedef unsigned int zicio_shared_pool_key_t;
typedef struct {
	int counter;
} zicio_atomic_t;

/* Maximum number of buffer chunk per buffer */
#define ZICIO_MAX_NUM_CHUNK (16)

/* Macros for memory switchboard */
#define ZICIO_MAX_NUM_GHOST_ENTRY (512ULL)
#define ZICIO_GHOST_ENTRY_MASK (~(ZICIO_MAX_NUM_GHOST_ENTRY - 1))

/**
 * Data structure for users to use libzicio
 *
 * @local_fds: file descriptors the user do not share with others
 * @shareable_fds: file descriptors the user share with others
 * @nr_local_fd: how many fds are in the @local_fds
 * @nr_shareable_fd: how many fds are in the @shareable_fds
 * @switch_board_address: currently acquired chunk address
 * @stat_board_addr: user stat board address
 * @user_base_address: base address of user map
 * @zicio_flag: options
 * @batches_array: 2D array of batch ([file][batch])
 * @nr_batches: how many batches are in batches_array
 */
struct zicio_args {
	int *local_fds;
	int *shareable_fds;
	int	nr_local_fd;
	int nr_shareable_fd;

	zicio_shared_pool_key_t *zicio_shared_pool_key;
	unsigned long *switch_board_addr;
	unsigned long *stat_board_addr;
	unsigned long user_base_address;
	int zicio_flag;
	unsigned **batches_array;
	unsigned *nr_batches;
};

/**
 * zicio_memory_switchboard_entry - entry for each chunk 
 * @status: chunk status
 * @chunk_id: assigned chunk id
 *
 * On the user side, application is doing data ingestion for fast analysis. 
 * To find and read exact chunk which is not collapsed is significant for
 * correct result. To do this, our zicio library checks whether the chunk is
 * available through the status bit atomically.
 *
 * On the kernel side, the kernel checks the status of chunk before mapping and
 * providing another chunk.
 */
typedef struct zicio_memory_switchboard_entry {
	//unsigned chunk_id:18; 	/* 18 bits, covering [0, 262144) */

	/*
	 *	          structure of val ( 4 bytes ) 
	 *
	 *	+---------------------------------------------------------------+
	 *	|               bytes (30 bits)               | status (2 bits) |
	 *	+---------------------------------------------------------------+
	 *
	 */
	zicio_atomic_t val; 		/* 4 bytes, aligned for atomic instruction */
} zicio_memory_switchboard_entry;

/**
 * zicio_switch_board - data structure for communicating with the kernel
 * @user_buffer_idx: current user buffer index
 * @consumed: counter to determine lib's chunk id
 * @avg_tsc_delta: average tsc the lib takes to consume a chunk
 * @data_buffer: data buffer's start address
 * @entries: status bits for each chunk
 *
 * This data structure is used to communicate with the zicio module in
 * kernel.
 *
 * Therefore, it should have the same structure in the kernel and library.
 */
typedef struct zicio_switch_board {
	zicio_atomic_t		user_buffer_idx;
	unsigned long	consumed;
	unsigned long	avg_tsc_delta;
	unsigned long	data_buffer;
	zicio_memory_switchboard_entry entries[ZICIO_MAX_NUM_GHOST_ENTRY];
	unsigned long	nr_consumed_chunk;
} zicio_switch_board;

/**
 * zicio_stat_board - data structure for monitoring stats of data ingestion
 */
typedef struct zicio_stat_board {
	/* Flow control stats */
	unsigned long soft_irq_cnt; 	/* # of request count at soft IRQ */
	unsigned long io_interrupt_cnt; /* # of request count at I/O interrupt handler*/
	unsigned long cpu_idle_loop_cnt;	/* # of request count at cpu idle loop */
	signed long long io_completion_time; /* I/O completion time (nsec) */

	/* Sharing stats */
	/* # of chunk that user read from shared pool on track*/
	unsigned long num_mapped_chunk_on_track;
	/* # of chunk that user read from shared pool after derail */
	unsigned long num_mapped_chunk_derailed;
	/* # of pages contributed to the pool */
	unsigned long num_pages_contributed;
	/* # of pages shared in the pool */
	unsigned long num_pages_shared;
	/* # of premapped pages */
	unsigned long num_premapped_pages;
	/* # of forcefully unmapped pages */
	unsigned long num_forcefully_unmapped_pages;
	/* channel start time */
	signed long long channel_start_time;
	/* Latency on track */
	signed long long latency_on_track;
	/* Latency after derail */
	signed long long latency_derailed;
	/* Wait time for softirq */
	unsigned long wait_time_at_softirq_on_track;
	/* Wait time for softirq */
	unsigned long wait_time_at_softirq_derailed;
	/* # of request count on track */
	unsigned long io_on_track;
	/* # of request count after derail */
	unsigned long io_derailed;
	/* # of request count on track at I/O interrupt handler */
	unsigned long io_interrupt_cnt_on_track;
	/* # of request count after derail at I/O interrupt handler */
	unsigned long io_interrupt_cnt_derailed;
	/* # of request count on track at soft IRQ */
	unsigned long soft_irq_cnt_on_track;
	/* # of request count after derail at soft IRQ */
	unsigned long soft_irq_cnt_derailed;
	/* # of request count on track at cpu idle loop */
	unsigned long cpu_idle_loop_cnt_on_track;
	/* # of request count after derail at cpu idle loop */
	unsigned long cpu_idle_loop_cnt_derailed;

	/* I/O completion time (nsec) on track */
	signed long long io_completion_time_on_track;
	/* I/O completion time (nsec) after derail */
	signed long long io_completion_time_derailed;

	/* # of soft IRQ trigger */
	unsigned long soft_irq_trigger_cnt_shared;

	/* # of soft IRQ trigger on track */
	unsigned long soft_irq_trigger_on_track;
	/* # of soft IRQ trigger after derail */
	unsigned long soft_irq_trigger_derailed;

	/* # of soft IRQ trigger for no local page */
	unsigned long soft_irq_trigger_no_local_page;
	/* # of soft IRQ trigger for enough pages */
	unsigned long soft_irq_trigger_no_IO;

	/* # of soft IRQ trigger on track for no local page */
	unsigned long soft_irq_trigger_on_track_no_local_page;
	/* # of soft IRQ trigger on track for enough pages */
	unsigned long soft_irq_trigger_on_track_no_IO;
	/* # of soft IRQ trigger after derail for no local page */
	unsigned long soft_irq_trigger_derailed_no_local_page;
	/* # of soft IRQ trigger after derail for enough pages */
	unsigned long soft_irq_trigger_derailed_no_IO;

	/* # of IO reserved from consumable page */
	unsigned long io_reserved_from_consumable_page;
	/* # of IO reserved from average user ingestion point*/
	unsigned long io_reserved_from_avg_user_ingestion_point;

	/* # of forceful unmap from ktime */
	unsigned long num_forceful_unmap_from_ktime;
	/* # of premap failure from ktime */
	unsigned long num_premap_failure_from_ktime;
	/* # of premap failure from spcb */
	unsigned long num_premap_failure_from_spcb;
	/* # of premap failure from exptime */
	unsigned long num_premap_failure_from_exptime;
	/* # of premap failure from expected safe time */
	unsigned long num_premap_failure_from_expected_safe_time;
	/* # of premap failure from invalid chunk id */
	unsigned long num_premap_failure_from_invalid_chunk_id;
	/* # of premapped pages that consumed after 1 jiffy */
	unsigned long num_premapped_pages_consumed_after_a_jiffy;
	/* # of real premap pages at one premap function call */
	unsigned long num_real_premap;
	/* # of real try premap pages at one premap function call */
	unsigned long num_real_premap_try;
	/* # of premapped distance */
	unsigned long num_premapped_distance;
	/* # of premap tried */
	unsigned long num_premapped_try;
	/* # of premapped distance */
	unsigned long check_page_gap;
	/* # of premap tried */
	unsigned long sum_page_gap;

	/* Num channel */
	int nr_channels;
	/* Flag indicating shared mode */
	int is_shared;

	/* Number of consumed chunk */
	int nr_consumed_chunk;

	/* # of premapped chunk in softirq */
	unsigned long num_premap_in_softirq;

	/* # of premapped chunk in interrupt handler */
	unsigned long num_premap_in_nvme_irq;

	/* # of bytes contributed I/O */
	unsigned long long derailed_io_bytes;

	/* # of bytes of derailed I/O */
	unsigned long long contributed_io_bytes;
} zicio_stat_board;

typedef struct zicio_pread_stat_board {
	int enable_breakdown;

	/* total pread count */
	unsigned long long total_pread_count;

	/* pread's elapsed time */
	unsigned long long total_tsc_pread;
	unsigned long long total_nsec_pread;

	/* mode swicth */
	unsigned long long start_tsc_mode_switch_from_user;
	unsigned long long start_tsc_mode_switch_from_kernel;
	unsigned long long total_tsc_mode_switch_from_user;
	unsigned long long total_tsc_mode_switch_from_kernel;
	unsigned long long total_nsec_mode_switch_from_user;
	unsigned long long total_nsec_mode_switch_from_kernel;
	unsigned long long total_mode_switch_count;

	/* page cache copy */
	unsigned long long total_tsc_copy_page_to_iter;
	unsigned long long total_nsec_copy_page_to_iter;

	/* filemap_get_pages() */
	unsigned long long total_nsec_filemap_get_pages;

	/* ondemand_readahead() from  page_cache_sync_ra() */
	unsigned long long total_nsec_ondemand_readahead_sync;

	/* ondemand_readahead() from  page_cache_async_ra() */
	unsigned long long total_nsec_ondemand_readahead_async;

	/* ext4_mpage_readpages() total elapsed time */
	unsigned long long total_nsec_ext4_mpage_readpages;

	/* filemap_get_read_batch() */
	unsigned long long total_nsec_filemap_get_read_batch;

	/* page_cache_sync_readahead() */
	unsigned long long total_nsec_page_cache_sync_readahead;

	/* page_cache_async_readahead() */
	unsigned long long total_nsec_page_cache_async_readahead;

	/* filemap_create_page() */
	unsigned long long total_nsec_filemap_create_page;

	/* filemap_readahead() */
	unsigned long long total_nsec_filemap_readahead;

	/* filemap_update_page() */
	unsigned long long total_tsc_filemap_update_page;
	unsigned long long total_nsec_filemap_update_page;

	/* do_page_cache_ra() */
	unsigned long long total_nsec_do_page_cache_ra_sync;
	unsigned long long total_nsec_do_page_cache_ra_async;

	/* read_pages() */
	unsigned long long total_nsec_read_pages;

	/* blk_start_plug() */
	unsigned long long total_nsec_blk_start_plug;

	/* ext4_readahead() */
	unsigned long long total_nsec_ext4_readahead;

	/* blk_finish_plug() */
	unsigned long long total_nsec_blk_finish_plug;

	/* filemap_read_page() */
	unsigned long long total_nsec_filemap_read_page;

	/* ext4_readpage() */
	unsigned long long total_nsec_ext4_readpage;

	/* put_and_wait_on_page_locked() */
	unsigned long long total_nsec_put_and_wait_on_page_locked;

	/* __lock_page_async() */
	unsigned long long total_nsec_lock_page_async;

	/* filemap_range_uptodate() */
	unsigned long long total_nsec_filemap_range_uptodate;

	/* ext4_map_blocks() */
	unsigned long long total_nsec_ext4_map_blocks;

	/* submit_bio() */
	unsigned long long total_nsec_submit_bio;

	/* nvme */
	unsigned long long total_nsec_device;

	/* put_page() */
	unsigned long long total_nsec_put_page;

	/* cond_resched() */
	unsigned long long total_nsec_cond_resched;

	/* mark_page_accessed() */
	unsigned long long total_nsec_mark_page_accessed;

	/* flush_dcache_page() */
	unsigned long long total_nsec_flush_dcache_page;

	/* __page_cache_alloc() */
	unsigned long long total_nsec_page_cache_alloc;

	/* add_to_page_cache_lru() */
	unsigned long long total_nsec_add_to_page_cache_lru;

	/* requested sectors */
	unsigned long long total_nr_submitted_sectors;

	/* O_DIRECT, wait_for_completion */
	unsigned long long total_tsc_dio_wait_for_completion;
	unsigned long long total_nsec_dio_wait_for_completion;
} zicio_pread_stat_board;

#endif /* _UAPI_ZICIO_H */
