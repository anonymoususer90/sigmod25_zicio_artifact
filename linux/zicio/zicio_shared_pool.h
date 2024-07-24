#ifndef __ZICIO_SHARED_POOL_H
#define __ZICIO_SHARED_POOL_H

#include <linux/gfp.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/rcupdate.h>
#include <linux/rculist.h>
#include <linux/types.h>
#include <linux/nospec.h>
#include <linux/hashtable.h>
#include <linux/zicio_notify.h>

#include "zicio_atomic.h"
#include "zicio_desc.h"
#include "zicio_extent.h"
#include "zicio_files.h"
#include "zicio_shared_pool.h"

/**************************************
 * Definitions of main data structure *
 *************************************/

/*
 * zicio_bitvector
 */
typedef atomic64_t ____cacheline_aligned zicio_bitvector_t;

typedef struct zicio_bitvector {
	unsigned int num_chunks; /* Number of managed page */
	zicio_bitvector_t **bit_vector; /* Bit vector tree */
	unsigned int *bitvector_length; /* Length of bit vector */
	short depth; /* Depth of current bit vector */
	bool shared; /* Shared or non-shared */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
	zicio_bitvector_t **debug_bit_vector; /* Bit vector tree for debug */
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
} zicio_bitvector;

/*
 * RCU hash table
 */
/* RCU hash table creation macro */
#define ZICIO_DECLARE_RCU_HASHTABLE(name, bits) \
		struct zicio_rcu_hlist_head name[1 << (bits)]
/* Order of shared hash table */
#define ZICIO_HASH_ORDER 12
/* Initial number of spcb */
#define ZICIO_NUM_INIT_SPCB 64
/* Maximum number of spcb */
#define ZICIO_NUM_MAX_SPCB (ZICIO_PAGE_SIZE >> 3)

/* zicio_rcu_hlist_head */
struct zicio_rcu_hlist_head {
	struct hlist_head head; /* Hash list head for spcb hash table */
	spinlock_t write_guard; /* Guard for write */
};

/* zicio wait free queue for managing unused shared data buffer page */
typedef struct zicio_wait_free_queue {
	/* Circular queue for caching page ID */
	atomic_t *circular_queue;
	/* Allocate point to give an unique position to worker */
	atomic_t allocate_point;
	/* Free point to give an unique position to worker */
	atomic_t free_point;
	/* Current size of queue */
	unsigned int queue_size;
	/* Current mask of queue */
	unsigned int point_mask;
} zicio_wait_free_queue;

/*
 * zicio_shared_metadata_ctrl
 *
 * Metadata and files stream located in the shared pool.
 */
typedef struct zicio_shared_metadata_ctrl {
	/* The array of start extents within the entire file stream of shared
	 * pool */
	unsigned long *file_start_point_extent;
	/* Number of extent in shared pool */
	unsigned long num_metadata;
	/* Extent array */
	void *shared_metadata_buffer;
} zicio_shared_metadata_ctrl;

/*
 * zicio_shared_files
 */
typedef struct zicio_shared_files {
	/* Total chunk numbers in shared pool */
	unsigned int total_chunk_nums;
	/* The array of start chunk id within the entire file stream of shared
	 * pool */
	unsigned int *start_chunk_nums;
	/* File information in shared pools */
	zicio_read_files registered_read_files;
} zicio_shared_files;

/*
 * zicio_shared_page_control_block_data
 */
typedef struct zicio_shared_page_control_block_data {
	/* Data field area */
	void * chunk_ptr; /* Memory chunk pointer */
	zicio_dev_map *dev_map; /* Device map */
	
	unsigned int chunk_size; /* Size of chunk */
	int local_page_idx; /* Local page idx */
	int channel_page_idx_mod; /* Page index of channel */


	/* Control field area */
	atomic_t ref_count; /* Reference cnt for shared page */

	atomic_t is_shared; /* Is this page shared? */
	atomic_t is_used; /* Is this page used? */
	atomic_t requested_flag_per_local_huge_page; /* Flag to show requested */

	atomic_t needed_pages_per_local_huge_page; /* Number of needed pages */
	atomic_t filled_pages_per_local_huge_page; /* Number of filled pages  */

	atomic64_t reclaimer_jiffies; /* Reclaim standard time */
	atomic64_t exp_jiffies; /* Expiration time */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	/* Debugging variables to show first failure when prmap */
	atomic64_t first_premap_failure_time;
#endif /* (CONFIG_ZICIO_DEBUG_LEVEL >= 2) */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	unsigned int file_chunk_id;
	int user_buffer_idx;
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
} zicio_shared_page_control_block_data;

/*
 * zicio_shared_page_control_block
 */
typedef struct zicio_shared_page_control_block {
	unsigned int chunk_id; /* Key of hash table : chunk ID of file */
	zicio_shared_page_control_block_data zicio_spcb; /* Value of hash table */
	/* hash node structure for kernel rcu hash table */
	struct hlist_node hash_node;
} zicio_shared_page_control_block;

typedef struct zicio_firehose_ctrl zicio_firehose_ctrl;

/*
 * zicio_shared_dev_maps
 */
typedef struct zicio_shared_dev_maps {
	zicio_dev_maps dev_maps;
	struct kmem_cache *dev_maps_cache;	
	int *dev_map_start_point;
} zicio_shared_dev_maps;

/*
 * zicio_shared_pool
 *
 * zicio shared pool data structure
 */
typedef struct zicio_shared_pool {
	/* Reference counter */
	atomic_t pin;
	/* Head of bit vector */
	atomic_t head;
	/* Shared bit vector */
	zicio_bitvector bitvector;
	/* Hash map mapping bit vector to memory chunks */
	ZICIO_DECLARE_RCU_HASHTABLE(shared_pool_hash, ZICIO_HASH_ORDER);
	/* Device mapping info */
	zicio_shared_dev_maps shared_dev_maps;
	/* Shared files */
	zicio_shared_files shared_files;
	/* ZicIO channels in shared pool */
	zicio_shared_metadata_ctrl shared_metadata_ctrl;
	/* Santface channels in shared pool */
	zicio_id_allocator zicio_channels;
	/* Shared queue to managing page id */
	zicio_wait_free_queue shared_page_id_queue;
	/* Shared page control block array */
	atomic64_t *zicio_spcb;
	/* Number of shared page control block */
	atomic_t num_spcb;
#ifdef CONFIG_ZICIO_STAT
	zicio_stat_board stat_board;
#endif /* CONFIG_ZICIO_STAT */
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	/* Track currently requested chunk count to control premapping amount */
	atomic_t cur_requested_chunk_count;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
} zicio_shared_pool;

#define ZICIO_LOCAL_DATABUFFER_CHUNK_NUM (8UL)
#define ZICIO_LOCAL_SPCB_MASK (~((ZICIO_PAGE_SIZE >> 3) - 1))

/*
 * zicio_attached_channel
 *
 * Information for each channel attached to the shared pool
 */
typedef struct zicio_attached_channel {
	/* show this channle is derailed or not */
	atomic_t derailed;
	/* Page moving avg per channel */
	unsigned page_moving_avg;
	/* Average consumed speed per channel */
	atomic64_t consumed_speed_avg;
	/* The lowest premapped chunk id */
	atomic_t previous_low_premap_point;
	/* The maximum value of previous premap */
	atomic_t previous_high_premap_point;
	/* The maximum value of forcefully unmapped file chunk id */
	atomic_t last_forcefully_unmapped_file_chunk_id;
	/* ZicIO shared page control block */
	zicio_shared_page_control_block *
			local_zicio_spcb[ZICIO_LOCAL_DATABUFFER_CHUNK_NUM];
	/* Channel descriptor pointer matched with this structure */
	zicio_descriptor *desc;
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	atomic_t last_forcefully_unmapped_monotonic_chunk_id;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
} zicio_attached_channel;

/*
 * zicio_channel_consumption_indicator
 *
 * Indicates the file chunk id, metadata, etc. where the user is located
 */
typedef struct zicio_channel_consumption_indicator {
	/* File position within the file stream */
	int current_file_idx;
	/* The first chunk id initially read by the user */
	unsigned long start_chunk_id_no_mod;
	/* The file chunk id currently read by the user */
	unsigned long current_chunk_id_mod;
	/* The first metadata idx initially read by the user */
	unsigned long start_metadata;
	/* The metadata idx initially read by the user */
	unsigned long current_metadata;
	/* Members to convert logical file chunk id to monotonic chunk id */
	unsigned int chunk_id_high;
	unsigned int chunk_id_low;
} zicio_channel_consumption_indicator;

/*
 * zicio_shared_pool_local
 *
 * Data structure of zicio shared pool that is attached to and operated in
 * shared pool
 */
typedef struct zicio_shared_pool_local {
	int channel_id;
	/* ZicIO attached channel information related with ingestion */
	zicio_attached_channel *zicio_channel;
	/*
	 * Local shared bit vector
	 * It has 2bits of internal bitvector matched with 512 bits in leaf
	 * bitvector.
	 * 1bit: Show forceful unmap was occurred.
	 * 0bit: Show at least all bits in covered area was premapped.
	 */
	zicio_bitvector local_bitvector;
	/* Number of requested chunks */
	atomic_t num_mapped;
	/* Metadata meter for metadata buffer */
	zicio_channel_consumption_indicator consume_indicator;
	/* Number of shared pages */
	atomic_t num_shared_pages;
	/* Number of using pages */
	atomic_t num_using_pages;
	/* Start id of contributed spcb */
	atomic_t start_spcb_iter;
	/* End id of contributed spcb */
	atomic_t end_spcb_iter;
	/* Last I/O chunk ID */
	atomic_t last_io_file_chunk_id;
	/* ZicIO shared page control block matched with contributed pages */
	atomic64_t *contribute_zicio_spcb;
	/* Shared page control block array matched with premapped pages */
	zicio_shared_page_control_block **zicio_spcb_arrays;
	/*
	 * ingestion point tracking info array
	 * It is corresponded to user bufer idx.
	 * | ** file chunk id ** | ** distance from head ** |
	 */
	atomic64_t *ingestion_point_track_array;
	/* Gaurd to protect conflicts between reclaimers. THIS SHOULD USE FOR
	 * TRYLOCK. */
	spinlock_t reclaimer_guard;


} zicio_shared_pool_local;

/*
 * zicio_shared_request_timer
 *
 * Request timer structure timer
 */
typedef struct zicio_shared_request_timer {
	/* Timer list for waking up on time */
	struct timer_list consume_wait_timer;
	/* Consume wait list for jobs of SoftIRQ Daemon */
	struct list_head user_consume_wait_list;
	/* ZicIO channel descriptor for work */
	zicio_descriptor *desc;
	/* The reason of timer creation */
	int reason;
	/* ID of page id wait-free queue */
	unsigned page_id_queue_idx;
	/* Flags to show reques timer is created on derail */
	bool derailed;
	/* Flags to show shared request timer was already pended its job */
	bool pended;
	struct zicio_shared_request_timer *next;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
	/* Wait time to get shared page */
	ktime_t first_page_get_time;
#endif
} ____cacheline_aligned zicio_shared_request_timer;

/*
 * zicio_raw_user_ingestion_sum
 */
typedef struct zicio_raw_user_ingestion_sum {
	/* # of channels */
	int nr_channels;
	/* Summation of current user ingestion point */
	unsigned long current_ingestion_point_sum_no_mod;
	/* Current head */
	unsigned int current_head;
	/* average consumption time */
	unsigned int avg_consumable_chunk_num_in_jiffy;
} zicio_raw_user_ingestion_sum;

/*
 * zicio_raw_user_consumption_speed_sum
 */
typedef struct zicio_raw_user_consumption_time_sum {
	/* # of channels */
	int nr_channels;
	/* Summation of user consumption time */
	unsigned long user_consumption_time_sum;
	/* Summation of squares of user consumption time */
	unsigned long long square_user_consumption_time_sum;
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	unsigned long long user_consumption_time_arr[256];
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
} zicio_raw_user_consumption_time_sum;

#define ZICIO_SPOOL_LOCAL_INFO_SIZE sizeof(zicio_shared_pool_local)

/*
 * zicio_is_shared_channel
 *
 * Checking if this channel reads data from shared pool
 */
static inline bool
zicio_is_shared_channel(struct zicio_args *zicio_args)
{
	/* If reading data from shared pool, then return true */
	if (zicio_args->shareable_fds && zicio_args->nr_shareable_fd) {
		return true;
	}
	return false;
}

/*
 * zicio_check_channel_derailed
 *
 * Checking whether a channel is derailed or not.
 */
static inline bool
zicio_check_channel_derailed(zicio_descriptor *desc)
{
	zicio_shared_pool_local *zicio_shared_pool_local;

	BUG_ON(!desc->zicio_shared_pool_desc);
	zicio_shared_pool_local = desc->zicio_shared_pool_desc->zicio_shared_pool_local;

	/*
	 * When the channel is opened, there is a possibility that it has not been
	 * set yet.
	 */
	if (!zicio_shared_pool_local) {
		return true;
	}

	return atomic_read(&zicio_shared_pool_local->zicio_channel->derailed);
}

/*
 * zicio_access_pool_shared
 *
 * Checking pool is accessed by more than one channel
 */
static inline bool
zicio_access_pool_shared(zicio_descriptor *desc)
{
	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool = zicio_get_shared_pool(desc);
	return (atomic_read(&zicio_shared_pool->pin) > 2);
}

/*
 * zicio_check_reclaiming_in_progress
 *
 * Checking reclaiming is progressed by others
 */
static inline bool
zicio_check_reclaiming_in_progress(zicio_descriptor *desc)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);

	return spin_trylock(&zicio_shared_pool_local->reclaimer_guard);
}

/*
 * zicio_set_reclaiming_end
 *
 * Set flag to show that reclaiming is ended.
 */
static inline void
zicio_set_reclaiming_end(zicio_descriptor *desc)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);

	spin_unlock(&zicio_shared_pool_local->reclaimer_guard);
}

long zicio_allocate_and_initialize_shared_pool(struct device **devices,
		unsigned int *fs, struct fd *fd, unsigned int nr_sharable_fd,
		int *file_dev_map);
zicio_shared_pool *zicio_create_shared_pool(struct device **devices,
			unsigned int *fds, struct fd *fs, int *file_dev_map, int nr_fd,
			zicio_shared_pool_key_t shared_pool_key);
void zicio_delete_shared_pool(void *zicio_shared_pool);
zicio_shared_pool_local *zicio_create_shared_pool_local(
			zicio_descriptor *desc, zicio_shared_pool *zicio_shared_pool);
void zicio_delete_shared_pool_local(zicio_descriptor *desc,
			zicio_shared_pool *zicio_shared_pool,
			zicio_shared_pool_local *zicio_shared_pool_local);
zicio_attached_channel* zicio_get_attached_channel(
			zicio_id_allocator *zicio_attached_channels, unsigned int id);
unsigned int zicio_get_init_chunk_id(zicio_descriptor *desc,
			int local_page_idx);
void __init zicio_init_shared_callback_list(void);
void zicio_remove_lag_user_request(zicio_descriptor *desc);

/******************************************************************
 * Definitions and macros for zicio bitvector for shared pool *
 ******************************************************************/

/* Check whether all the data of the corresponding chunk has been read */
#define ZICIO_BIT_VALID 0B10UL
#define ZICIO_BIT_INVALID 0B00UL
/* Check whether the corresponding chunk has been referred */
#define ZICIO_BIT_REF 0B01UL
#define ZICIO_BIT_UNREF 0B00UL
#define ZICIO_BIT_PREMAP 0B01UL
#define ZICIO_BIT_FORCEFUL_UNMAP 0B10UL

/* Shift macro to translate bit to unsigned long */
#define ZICIO_BIT_TO_ULONG_SHIFT (ZICIO_BYTE_MIN_ORDER + \
			ZICIO_ULONG_ORDER)
/* Minimum size of depth 0 bits */
#define ZICIO_MIN_LEAF_BYTE_SIZE (1UL << ZICIO_BYTE_MIN_ORDER)
/* Shift macro to translate internal bit to leaf cover area */
#define ZICIO_INTERNAL_BIT_COVER_SHIFT (ZICIO_BYTE_MIN_ORDER + \
			ZICIO_INTERNAL_BITVECTOR_ORDER)
/* Shift macro to translate internal 64 bits to leaf cover area */
#define ZICIO_INTERNAL_BITVECTOR_IDX_SHIFT ( \
			ZICIO_BYTE_MIN_ORDER + \
			ZICIO_INTERNAL_BITVECTOR_ORDER + \
			ZICIO_INTERNAL_BITVECTOR_ORDER)
/* Coverage of internal 64 bits */
#define ZICIO_64_INTERNAL_BITS_COVERAGE (1UL << \
			ZICIO_INTERNAL_BITVECTOR_IDX_SHIFT)
/* Change bits to unsigned long */
#define ZICIO_GET_BIT_TO_ULONG(x) ((x) >> ZICIO_BIT_TO_ULONG_SHIFT)
/* Change unsigned long to bit */
#define ZICIO_GET_ULONG_TO_BIT(x) ((x) << ZICIO_BIT_TO_ULONG_SHIFT)
/* Get the internel bit number from file chunk id */
#define ZICIO_GET_START_CHUNKNUM(bits) ((bits) << \
			ZICIO_INTERNAL_BIT_COVER_SHIFT)
/* Coverage of one internal bits */
#define ZICIO_ONE_INTERNAL_BIT_COVERAGE ZICIO_GET_START_CHUNKNUM(1UL)
/* Get start chunk number from internal bits */
#define ZICIO_GET_CHUNKNUM_FROM_INTERNAL_BIT(x) ((x) << \
			(ZICIO_INTERNAL_BITVECTOR_IDX_SHIFT - 1))
/* Value returned when no bit is turned on in the bitvector */
#define ZICIO_LOCAL_BITVECTOR_COMPLETE UINT_MAX

/* Default bitvector depth */
#define ZICIO_DEFAULT_BITVECTOR_DEPTH 2
/* Shared leaf bitvector mask */
#define ZICIO_SHARED_FLAG_MASK 0B11
/* Local internal bitvector mask */
#define ZICIO_INTERNAL_LOCAL_FLAG_MASK 0B11
/* Local leaf bitvector mask */
#define ZICIO_LOCAL_FLAG_MASK 0B1
/* Get shared leaf bitvector */
#define ZICIO_GET_SHARED_BITMAP_FLAG(cur_bitmap_value, bit_idx) \
	((cur_bitmap_value >> bit_idx) & ZICIO_SHARED_FLAG_MASK)
/* Get local internal bitvector */
#define ZICIO_GET_LOCAL_INTERNAL_BITMAP_FLAG(cur_bitmap_value, bit_idx) \
	((cur_bitmap_value >> bit_idx) & ZICIO_INTERNAL_LOCAL_FLAG_MASK)
/* Get local leaf bitvector */
#define ZICIO_GET_LOCAL_BITMAP_FLAG(cur_bitmap_value, bit_idx) \
	((cur_bitmap_value >> bit_idx) & ZICIO_LOCAL_FLAG_MASK)

/*
 * zicio_get_in_bitvector_offset
 *
 * Find the position in a bit vector
 */
static inline unsigned int
zicio_get_in_bitvector_offset(unsigned int bit_idx, bool shared_depth0)
{
	BUG_ON((int)shared_depth0 > 1);

	bit_idx <<= (int)shared_depth0;
	return (bit_idx >> ZICIO_INTERNAL_BITVECTOR_ORDER);
}

/*
 * zicio_get_in_byte_offset
 *
 * Find the position in a 64bits bitvector
 */
static inline unsigned int
zicio_get_in_byte_offset(unsigned int bit_idx, bool shared_depth0)
{
	bit_idx <<= (int)shared_depth0;
	return (bit_idx & ~ZICIO_INTERNAL_BITVECTOR_MASK);
}

/*
 * zicio_get_bitvector
 *
 * Find 64bits bitvector matched with depth and bitvector offset
 */
static inline atomic64_t*
zicio_get_bitvector(zicio_bitvector_t **bit_vector, int depth,
			int in_bitvector_offset)
{
	BUG_ON(bit_vector == NULL);
	return bit_vector[depth] + in_bitvector_offset;
}

/*
 * zicio_get_bitvector
 *
 * Find 64bits bitvector matched with depth and bitvector offset and
 * converts it to unsigned long and returns it.
 */
#define zicio_get_bitvector_ul(bit_vector, depth, in_bitvector_offset) \
			((unsigned long *)zicio_get_bitvector(bit_vector, \
						depth, in_bitvector_offset))

/*
 * zicio_get_last_internal_bitvector_size
 *
 * Calculate the length of the internal bitvector to which the file chunk id
 * corresponding to the current current belongs.
 */
static inline unsigned long
zicio_get_last_internal_bitvector_size(unsigned int current_chunk_id,
			unsigned int total_chunk_nums)
{
	if (total_chunk_nums < (ZICIO_64_INTERNAL_BITS_COVERAGE >> 1)) {
		/* Case 1: If all chunks can fit in a single 64bit vector */
		return ((total_chunk_nums - 1) >>
					(ZICIO_INTERNAL_BIT_COVER_SHIFT - 1)) + 1;
	} else if (total_chunk_nums - current_chunk_id <
		(ZICIO_64_INTERNAL_BITS_COVERAGE >> 1)) {
		/* Case 2: If the current chunk is located in the last bitvector */
		return ((total_chunk_nums - current_chunk_id - 1) >>
					(ZICIO_INTERNAL_BIT_COVER_SHIFT - 1)) + 1;
	}

	/* Case 3: If located inside the bitvector except for the above two cases */
	return (sizeof(unsigned long) << ZICIO_BYTE_MIN_ORDER);
}

/*
 * zicio_get_last_leaf_bitvector_size
 *
 * Calculate the length of the leaf bitvector to which the file chunk id
 * corresponding to the current current belongs.
 */
static inline unsigned int
zicio_get_last_leaf_bitvector_size(unsigned long current_bitvector_id,
			unsigned long total_chunk_nums)
{
	unsigned int current_chunk_id = ZICIO_GET_ULONG_TO_BIT(
			current_bitvector_id);
	if (total_chunk_nums < ZICIO_GET_ULONG_TO_BIT(1UL)) {
		/* Case 1: If all chunks can fit in a single 64bit vector */
		return total_chunk_nums;
	} else if (total_chunk_nums - current_chunk_id <
				ZICIO_GET_ULONG_TO_BIT(1UL)) {
		/* Case 2: If the current chunk is located in the last bitvector */
		return total_chunk_nums - current_chunk_id;
	}

	/* Case 3: If located inside the bitvector except for the above two cases */
	return (sizeof(unsigned long) << ZICIO_BYTE_MIN_ORDER);
}

/*
 * zicio_get_chunk_id_from_internal_bitvector
 *
 * Get chunk id from internal bitvector
 */
static inline unsigned int
zicio_get_chunk_id_from_internal_bitvector(unsigned int start,
			unsigned int internal_num)
{
	/* Get the bit position of internal bitvector. */
	unsigned int bit_start = ZICIO_GET_ULONG_TO_BIT(start);
	return (ZICIO_GET_START_CHUNKNUM(bit_start + internal_num) >> 1);
}

/*
 * zicio_get_max_leaf_num_in_one_internal_bit
 *
 * Calculate the length of the area covered by one bit of the internal
 * bitvector.
 */
static inline unsigned int
zicio_get_max_leaf_num_in_one_internal_bit(unsigned int leaf_start,
			unsigned int total_chunk_nums)
{
	if (leaf_start + ZICIO_ONE_INTERNAL_BIT_COVERAGE < total_chunk_nums) {
		/* Case 1 : If it is the last bit */
		return ((total_chunk_nums - leaf_start - 1) >>
					ZICIO_BIT_TO_ULONG_SHIFT) + 1;
	}
	/* Case 2 : If it is not the last bit */
	return ZICIO_MIN_LEAF_BYTE_SIZE;
}

/*
 * zicio_get_bit_status
 *
 * Read and return flag of leaf bitvector corresponding to idx.
 */
static inline unsigned long
zicio_get_bit_status(zicio_bitvector *bitvector, unsigned long idx,
			bool shared, int depth)
{
	bool read_2bit = (shared && depth == 0);
	unsigned int in_bitvector_offset = zicio_get_in_bitvector_offset(idx,
				read_2bit);
	unsigned int in_byte_offset = zicio_get_in_byte_offset(idx, read_2bit);
	atomic64_t *cur_bitmap = zicio_get_bitvector(bitvector->bit_vector,
				depth, in_bitvector_offset);
	unsigned long cur_bitmap_value = atomic64_read(cur_bitmap);

	if (shared) {
		return ZICIO_GET_SHARED_BITMAP_FLAG(cur_bitmap_value,
					in_byte_offset);
	} else {
		return ZICIO_GET_LOCAL_BITMAP_FLAG(cur_bitmap_value,
					in_byte_offset);
	}
}

/*
 * zicio_get_shared_bitvector
 *
 * Get shared bitvector from descriptor
 */
static inline zicio_bitvector *
zicio_get_shared_bitvector(zicio_descriptor * desc)
{
	zicio_shared_pool *zicio_shared_pool;

	BUG_ON(!desc->zicio_shared_pool_desc);
	zicio_shared_pool = zicio_get_shared_pool(desc);
	return &(zicio_shared_pool->bitvector);
}

/*
 * zicio_get_local_bitvector
 *
 * Get local bitvector from descriptor
 */
static inline zicio_bitvector *
zicio_get_local_bitvector(zicio_descriptor * desc)
{
	zicio_shared_pool_local *zicio_shared_pool_local;

	BUG_ON(!desc->zicio_shared_pool_desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);
	return &(zicio_shared_pool_local->local_bitvector);
}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
static inline void
zicio_set_turn_on_premap_bitvector(zicio_descriptor *desc, int bit_idx)
{
	zicio_shared_pool_local *zicio_shared_pool_local;
	zicio_bitvector *zicio_local_bitvector;
	atomic64_t *bit_vector;
	unsigned int in_bitvector_offset, in_byte_offset;
	bool already_turned_on;

	BUG_ON(!desc->zicio_shared_pool_desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);
	zicio_local_bitvector = &(zicio_shared_pool_local->local_bitvector);

	/* Get the offset in overall bitvectors */
	in_bitvector_offset = zicio_get_in_bitvector_offset(bit_idx, false);
	/* Get the offset in one bitvector element(8bytes) */
	in_byte_offset = zicio_get_in_byte_offset(bit_idx, false);
	/* Get a bitvector element */
	bit_vector = zicio_get_bitvector(zicio_local_bitvector->debug_bit_vector,
				0, in_bitvector_offset);

	already_turned_on = test_and_set_bit(in_byte_offset,
			(volatile unsigned long*)bit_vector);
	BUG_ON(already_turned_on);
}

static inline void
zicio_set_turn_on_unmap_bitvector(zicio_descriptor *desc, int bit_idx)
{
	zicio_shared_pool_local *zicio_shared_pool_local;
	zicio_bitvector *zicio_local_bitvector;
	atomic64_t *bit_vector;
	unsigned int in_bitvector_offset, in_byte_offset;
	bool already_turned_on;

	BUG_ON(!desc->zicio_shared_pool_desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);
	zicio_local_bitvector = &(zicio_shared_pool_local->local_bitvector);

	/* Get the offset in overall bitvectors */
	in_bitvector_offset = zicio_get_in_bitvector_offset(bit_idx, false);
	/* Get the offset in one bitvector element(8bytes) */
	in_byte_offset = zicio_get_in_byte_offset(bit_idx, false);
	/* Get a bitvector element */
	bit_vector = zicio_get_bitvector(zicio_local_bitvector->debug_bit_vector,
				1, in_bitvector_offset);

	already_turned_on = test_and_set_bit(in_byte_offset,
			(volatile unsigned long*)bit_vector);
	BUG_ON(already_turned_on);
}
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */

void zicio_set_shared_pool_bitvector(zicio_descriptor *desc,
			int chunk_id, zicio_shared_page_control_block *zicio_spcb,
			unsigned long flag);
void zicio_clear_shared_pool_bitvector(zicio_descriptor *desc,
			int chunk_id, zicio_shared_page_control_block *zicio_spcb,
			unsigned long flag);
bool zicio_clear_bitvector_atomic(zicio_bitvector *zicio_bitvector,
			unsigned int chunk_id, unsigned flag, bool shared);
unsigned int zicio_get_next_file_chunk_id_from_shared_pool(
			zicio_descriptor *desc,
			zicio_shared_page_control_block *zicio_spcb,
			unsigned int *chunk_id_no_mod);
unsigned int zicio_get_next_unread_chunk_id_shared(
			zicio_descriptor *desc,
			zicio_shared_page_control_block *zicio_spcb);
unsigned int zicio_get_next_unread_chunk_id(zicio_descriptor *desc);
unsigned int zicio_get_next_file_chunk_id_shared(
			zicio_descriptor *desc, int local_page_idx, bool is_on_track);

/******************************************************************
 * Definitions and macros for zicio wait free queue			  *
 ******************************************************************/
/*
 * zicio_get_allocate_position_in_queue
 *
 * Get an unique position to allocate
 */
static inline unsigned int
zicio_get_allocate_position_in_queue(zicio_wait_free_queue *zicio_queue)
{
	return atomic_fetch_dec(&zicio_queue->allocate_point)  & ~zicio_queue->point_mask;
}

/*
 * zicio_get_free_position_in_queue
 *
 * Get an unique position to free
 */
static inline unsigned int
zicio_get_free_position_in_queue(zicio_wait_free_queue *zicio_queue)
{
	return atomic_fetch_dec(&zicio_queue->free_point) & ~zicio_queue->point_mask;
}

/*
 * zicio_set_allocate_position_in_queue
 *
 * Set an unique position to allocate
 */
static inline void
zicio_set_allocate_position_in_queue(zicio_wait_free_queue *zicio_queue,
			unsigned int allocate_pos)
{
	atomic_set(&zicio_queue->allocate_point, allocate_pos);
}

/*
 * zicio_set_free_position_in_queue
 *
 * Set an unique position to free
 */
static inline void
zicio_set_free_position_in_queue(zicio_wait_free_queue *zicio_queue,
			unsigned int free_pos)
{
	atomic_set(&zicio_queue->free_point, free_pos);
}

/*
 * zicio_set_value_to_queue
 *
 * Set value to wait free queue matched with a position
 */
static inline void
zicio_set_value_to_queue(zicio_wait_free_queue *zicio_queue,
			unsigned int pos, u32 value)
{
	BUG_ON(pos > (~zicio_queue->point_mask));
	atomic_set(zicio_queue->circular_queue + pos, value);
}

/*
 * zicio_check_queue_ready
 *
 * Check the queue element in a position is ready.
 */
static inline u32
zicio_check_queue_ready(zicio_wait_free_queue *zicio_queue,
			unsigned int pos, bool wait)
{
	volatile unsigned int value;

	BUG_ON(pos > (~zicio_queue->point_mask));
	while ((!(value = atomic_xchg(zicio_queue->circular_queue + pos, 0))) &&
			wait);
	return value;
}

/*
 * zicio_check_queue_unready
 *
 * Check the queue lement in a position is unready.
 */
static inline u32
zicio_check_queue_unready(zicio_wait_free_queue *zicio_queue,
			unsigned int pos, bool wait)
{
	volatile unsigned int value;

	BUG_ON(pos > (~zicio_queue->point_mask));
	while ((value = atomic_read(zicio_queue->circular_queue + pos)) && wait);
	return value;
}

unsigned int zicio_get_page_id_from_queue(zicio_descriptor *desc,
			unsigned int *previous_page_id, bool wait);
unsigned int zicio_read_page_id_from_queue(zicio_descriptor *desc,
			unsigned int pos);
void zicio_set_page_id_to_queue(zicio_descriptor *desc, int id);
void zicio_print_page_id_queue(zicio_descriptor *desc);
/******************************************************************
 * Definitions and macros for zicio shared page control block *
 ******************************************************************/
/*
 * zicio shared page control block size and its alignment in slab allocator
 */
#define ZICIO_SPCB_HASH_ELEM_SIZE \
			sizeof(zicio_shared_page_control_block)
#define ZICIO_SPCB_HASH_ELEM_SLAB_ALIGN ZICIO_SPCB_HASH_ELEM_SIZE
#define ZICIO_NUM_SPCB_IN_PAGE (ZICIO_PAGE_SIZE >> 3)

/* Print spcb's mapping information */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
#define PRINT_SPCB_DEBUG(x, cpu_id, file, line, func) do { \
		printk(KERN_WARNING "cpu_id[%d] :file_chunk_id: %u, page_id: %u, " \
							"buffer_id: %u [%s:%d] [%s]\n", \
				cpu_id, (x).file_chunk_id, (x).local_page_idx, \
				(x).user_buffer_idx, file, line, func); \
		} while (0)
#else
#define PRINT_SPCB_DEBUG(x, cpu_id, file, line, func) do { \
		} while (0)
#endif

#define ZICIO_CANNOT_GET_NEXT_SPCB ULONG_MAX

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
#define ZICIO_TRACKING_MONOTONIC_CHUNK_ID_SHIFT (24)
#define ZICIO_TRACKING_INFO_SHIFT (48)
#define ZICIO_TRACKING_IDX_MASK \
			(0x0000000000FFFFFFULL)
#define ZICIO_TRACKING_MONOTONIC_CHUNK_ID_MASK \
			(0x0000FFFFFF000000ULL)
#define ZICIO_TRACKING_DIST_MASK \
			(0xFFFF000000000000ULL)

#define ZICIO_CONCATENATE_TRACKING_INFO(dist, idx) (		\
		((unsigned long long)dist << ZICIO_TRACKING_INFO_SHIFT) | idx)
#define ZICIO_SEPERATE_TRACKING_INFO(concat, p_dist, p_idx) ({		\
		((*p_dist) = (concat & ZICIO_TRACKING_DIST_MASK) >>		\
				ZICIO_TRACKING_INFO_SHIFT);						\
		((*p_idx) = concat & ZICIO_TRACKING_IDX_MASK); })
#define ZICIO_GET_TRACKING_INFO_MONOTONIC_CHUNK_ID(concat) \
		((concat & ZICIO_TRACKING_MONOTONIC_CHUNK_ID_MASK) \
			>> ZICIO_TRACKING_MONOTONIC_CHUNK_ID_SHIFT)

#else /* !CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

#define ZICIO_TRACKING_INFO_SHIFT 32
#define ZICIO_TRACKING_IDX_MASK (~(0xFFFFFFFFUL))
#define ZICIO_TRACKING_DIST_MASK (~(0xFFFFFFFF00000000UL))

#define ZICIO_CONCATENATE_TRACKING_INFO(dist, idx) (		\
		((unsigned long)dist << ZICIO_TRACKING_INFO_SHIFT) | idx)
#define ZICIO_SEPERATE_TRACKING_INFO(concat, p_dist, p_idx) ({		\
		((*p_dist) = (concat & ~ZICIO_TRACKING_DIST_MASK) >>		\
				ZICIO_TRACKING_INFO_SHIFT);							\
		((*p_idx) = concat & ~ZICIO_TRACKING_IDX_MASK); })

#endif /* !CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */


/*
 * zicio_set_shared_page_control_block
 *
 * Set shared page control block to spcb array matched with ghost entry idx
 */
static inline void
zicio_set_shared_page_control_block(zicio_descriptor *desc,
			int ghost_entry_idx, zicio_shared_page_control_block *zicio_spcb)
{
	zicio_shared_pool_local *zicio_shared_pool_local
				= zicio_get_shared_pool_local(desc);
	BUG_ON(ghost_entry_idx >= ZICIO_MAX_NUM_GHOST_ENTRY);
	zicio_shared_pool_local->zicio_spcb_arrays[ghost_entry_idx] = zicio_spcb;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
	printk(KERN_WARNING "cpu[%d] Ghost Entry Idx: %d  [%s:%d][%s]\n",
		desc->cpu_id, ghost_entry_idx, __FILE__, __LINE__, __FUNCTION__);
	if (!zicio_spcb) {
		printk(KERN_WARNING "cpu[%d] Ghost Entry Idx: %d zicio_spcb is NULL "
					"[%s:%d][%s]\n", desc->cpu_id, ghost_entry_idx, __FILE__,
					__LINE__, __FUNCTION__);
	} else {
		PRINT_SPCB_DEBUG(zicio_spcb->zicio_spcb, desc->cpu_id, __FILE__, __LINE__,
				__FUNCTION__);
	}
#endif /* CONFIG_ZICIO_DEBUG_LEVEL >= 3 */
}

/*
 * zicio_get_shared_page_control_block
 *
 * Get shared page control block to spcb array matched with ghost entry idx
 */
static inline zicio_shared_page_control_block *
zicio_get_shared_page_control_block(zicio_descriptor *desc,
			int ghost_entry_idx)
{
	zicio_shared_pool_local *zicio_shared_pool_local
				= zicio_get_shared_pool_local(desc);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
	zicio_shared_page_control_block *zicio_spcb
			= (zicio_shared_pool_local->zicio_spcb_arrays[ghost_entry_idx]);
	printk(KERN_WARNING "cpu[%d] Ghost Entry Idx: %d  [%s:%d][%s]\n",
		desc->cpu_id, ghost_entry_idx, __FILE__, __LINE__, __FUNCTION__);
	if (!zicio_spcb) {
		printk(KERN_WARNING "cpu[%d] Ghost Entr Idx: %d zicio_spcb is NULL "
					"[%s:%d][%s]\n", desc->cpu_id, ghost_entry_idx, __FILE__,
					__LINE__, __FUNCTION__);
	} else {
		PRINT_SPCB_DEBUG(zicio_spcb->zicio_spcb, desc->cpu_id, __FILE__, __LINE__,
				__FUNCTION__);
		printk(KERN_WARNING "cpu[%d] Ghost Entr Idx: %d spcb: %p ref_cnt: %d "
					"[%s:%d][%s]\n", desc->cpu_id, ghost_entry_idx, zicio_spcb,
					atomic_read(&zicio_spcb->zicio_spcb.ref_count), __FILE__,
					__LINE__, __FUNCTION__);
	}
	return zicio_spcb;
#else
	BUG_ON(ghost_entry_idx >= ZICIO_MAX_NUM_GHOST_ENTRY);
	BUG_ON(!zicio_shared_pool_local->zicio_spcb_arrays);
	return (zicio_shared_pool_local->zicio_spcb_arrays[ghost_entry_idx]);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL >= 3 */
}

/*
 * zicio_clear_shared_page_control_block
 *
 * Clear shared page control block to spcb array matched with ghost entry idx
 */
static inline void
zicio_clear_shared_page_control_block(zicio_descriptor *desc,
			int ghost_entry_idx)
{
	zicio_shared_pool_local *zicio_shared_pool_local
			= zicio_get_shared_pool_local(desc);
	zicio_shared_page_control_block *zicio_spcb
			= zicio_get_shared_page_control_block(desc, ghost_entry_idx);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
	printk(KERN_WARNING "cpu[%d] Ghost Entry Idx: %d  [%s:%d][%s]\n",
		desc->cpu_id, ghost_entry_idx, __FILE__, __LINE__, __FUNCTION__);
	if (!zicio_spcb) {
		printk(KERN_WARNING "cpu[%d] Ghost Entr Idx: %d zicio_spcb is NULL "
					"[%s:%d][%s]\n", desc->cpu_id, ghost_entry_idx, __FILE__,
					__LINE__, __FUNCTION__);
	} else {
		PRINT_SPCB_DEBUG(zicio_spcb->zicio_spcb, desc->cpu_id, __FILE__, __LINE__,
				__FUNCTION__);
		printk(KERN_WARNING "cpu[%d] Ghost Entr Idx: %d spcb: %p ref_cnt: %d "
					"[%s:%d][%s]\n", desc->cpu_id, ghost_entry_idx, zicio_spcb,
					atomic_read(&zicio_spcb->zicio_spcb.ref_count), __FILE__,
					__LINE__, __FUNCTION__);
	}
#endif /* CONFIG_ZICIO_DEBUG_LEVEL >= 3 */
	BUG_ON(ghost_entry_idx >= ZICIO_MAX_NUM_GHOST_ENTRY);

	/* This spcb is cleaned up when forcefully unmapped */
	if (!zicio_spcb) {
		return;
	}

	atomic_dec(&zicio_spcb->zicio_spcb.ref_count);
	zicio_shared_pool_local->zicio_spcb_arrays[ghost_entry_idx] = NULL;
}

/*
 * zicio_get_shared_page_expiration_jiffies
 *
 * Get the expiration time from shared page control block
 */
static inline ktime_t
zicio_get_shared_page_expiration_jiffies(zicio_descriptor *desc,
			int ghost_entry_idx)
{
	zicio_shared_page_control_block *zicio_spcb
			= zicio_get_shared_page_control_block(desc, ghost_entry_idx);

	BUG_ON(!zicio_spcb);
	return atomic64_read(&zicio_spcb->zicio_spcb.exp_jiffies);
}

/*
 * zicio_get_shared_page_block_with_id
 *
 * Get shared page control block using local page idx of channel
 */
static inline zicio_shared_page_control_block *
zicio_get_spcb_with_id_from_shared_pool(
			zicio_shared_pool *zicio_shared_pool, int local_page_idx)
{
	return (zicio_shared_page_control_block *)atomic64_read(
			zicio_shared_pool->zicio_spcb + local_page_idx);
}

/*
 * zicio_get_shared_page_block_with_id
 *
 * Get shared page control block using local page idx of channel
 */
static inline void
zicio_set_spcb_with_id_from_shared_pool(
			zicio_shared_pool *zicio_shared_pool,
			zicio_shared_page_control_block *zicio_spcb, int local_page_idx)
{
	atomic64_set(zicio_shared_pool->zicio_spcb + local_page_idx,
			(unsigned long)zicio_spcb);
}

/*
 * zicio_get_shared_page_block_with_id
 *
 * Get shared page control block using local page idx of channel
 */
static inline zicio_shared_page_control_block *
zicio_get_spcb_with_id(zicio_descriptor *desc,
			int local_page_idx)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);

	return zicio_get_spcb_with_id_from_shared_pool(zicio_shared_pool,
			local_page_idx);
}

/*
 * zicio_set_spcb_with_id
 *
 * Set shared page control block matched with local page idx of channel
 */
static inline void
zicio_set_spcb_with_id(zicio_descriptor *desc,
			zicio_shared_page_control_block *zicio_spcb, int local_page_idx)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);

	zicio_set_spcb_with_id_from_shared_pool(zicio_shared_pool, zicio_spcb,
			local_page_idx);
}

/*
 * zicio_get_shared_page_block_with_id
 *
 * Get shared page control block using local page idx of channel
 */
static inline zicio_shared_page_control_block *
zicio_get_local_spcb_with_id(zicio_descriptor *desc,
			int local_page_idx)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	zicio_attached_channel *zicio_channel = zicio_shared_pool_local->zicio_channel;

	return zicio_channel->local_zicio_spcb[local_page_idx];
}

/*
 * zicio_get_user_file_chunk_id
 *
 * Get file chunk id using user buffer page idx of channel
 */
static inline unsigned int
zicio_get_user_file_chunk_id(zicio_descriptor *desc,
			int user_buffer_idx)
{
	zicio_shared_page_control_block *zicio_spcb =
			zicio_get_shared_page_control_block(desc, user_buffer_idx);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
	printk(KERN_WARNING "cpu[%d] Ghost Entry Idx: %d  [%s:%d][%s]\n",
		desc->cpu_id, user_buffer_idx, __FILE__, __LINE__, __FUNCTION__);
	if (!zicio_spcb) {
		printk(KERN_WARNING "cpu[%d] Ghost Entr Idx: %d zicio_spcb is NULL "
					"[%s:%d][%s]\n", desc->cpu_id, user_buffer_idx, __FILE__,
					__LINE__, __FUNCTION__);
	} else {
		PRINT_SPCB_DEBUG(zicio_spcb->zicio_spcb, desc->cpu_id, __FILE__, __LINE__,
				__FUNCTION__);
	}
#endif /* CONFIG_ZICIO_DEBUG_LEVEL >= 3 */

	if (!zicio_spcb) {
		return UINT_MAX;
	}

	return zicio_spcb->chunk_id;
}

static inline void
zicio_set_contribute_shared_page_control_block(zicio_descriptor *desc,
			zicio_shared_page_control_block *zicio_spcb)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
				zicio_get_shared_pool_local(desc);
	atomic64_t *contribute_spcb = zicio_shared_pool_local->contribute_zicio_spcb;
	unsigned int max_spcb_id = atomic_fetch_inc(
			&zicio_shared_pool_local->end_spcb_iter) & ~ZICIO_LOCAL_SPCB_MASK;
	
	zicio_spcb->zicio_spcb.channel_page_idx_mod = max_spcb_id;
	BUG_ON(atomic64_read(contribute_spcb + max_spcb_id));

	atomic64_set(contribute_spcb + max_spcb_id, (u64)zicio_spcb);
}

static inline zicio_shared_page_control_block *
zicio_get_contribute_shared_page_control_block(zicio_descriptor *desc,
			unsigned int idx_mod)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
				zicio_get_shared_pool_local(desc);
	atomic64_t *contribute_spcb = zicio_shared_pool_local->contribute_zicio_spcb;

	return (zicio_shared_page_control_block *)(atomic64_read(contribute_spcb
			+ idx_mod));
}

static inline zicio_shared_page_control_block *
zicio_clear_contribute_shared_page_control_block(zicio_descriptor *desc,
			unsigned int idx_mod)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
				zicio_get_shared_pool_local(desc);
	atomic64_t *contribute_spcb = zicio_shared_pool_local->contribute_zicio_spcb;

	return (zicio_shared_page_control_block *)
			atomic64_xchg(contribute_spcb + idx_mod, 0);
}

/*
 * zicio_is_shared_page
 *
 * Check if this page is shared page or not
 */
static inline bool
zicio_is_shared_page(zicio_shared_page_control_block *zicio_spcb)
{
	return atomic_read(&zicio_spcb->zicio_spcb.is_shared);
}

void zicio_reclaim_spcb(zicio_descriptor *desc, bool blocking);

/******************************************************************
 * Definitions and macros for zicio RCU shared pcb hash table *
 ******************************************************************/

/* Get the number of RCU hash slot */
#define ZICIO_RCU_HASH_SIZE(name) (ARRAY_SIZE(name))
/* Get the number of bits used to create RCU hash slot */
#define ZICIO_RCU_HASH_BITS(name) ilog2(ZICIO_RCU_HASH_SIZE(name))
/* Return hash value from hash function */
#define zicio_rcu_hash_min(val, bits) \
	(sizeof(val) <= 4 ? hash_32(val, bits) : hash_long(val, bits))

/*
 * __zicio_rcu_hlist_add_head
 *
 * Real worker to add spcb to hash slost list head.
 * Deletion is performed using only node pointers. That is, both adding to the
 * tail and adding to the head have the same time complexity. However, in the
 * case of tail insertion, tarverse from head to tail and then insert data. And
 * this is protected by write gaurd. Add a node to the head to eliminate this
 * process.
 */
static inline void
__zicio_rcu_hlist_add_head(struct zicio_rcu_hlist_head *rcu_hash,
			zicio_shared_page_control_block *zicio_spcb)
{
	unsigned long flags;

	BUG_ON(!zicio_spcb);
	spin_lock_irqsave(&rcu_hash->write_guard, flags);
	hlist_add_head_rcu(&zicio_spcb->hash_node, &rcu_hash->head);
	spin_unlock_irqrestore(&rcu_hash->write_guard, flags);
}

/*
 * __zicio_rcu_hlist_find_for_each_entry
 *
 * Real worker to find spcb from linked list of slot.
 * Error handling for situations in which an invalid zicio_spcb is not found must
 * be performed by the caller.
 */
static inline zicio_shared_page_control_block *
__zicio_rcu_hlist_find_for_each_entry(
		struct zicio_rcu_hlist_head *rcu_hash, unsigned int chunk_id)
{
	zicio_shared_page_control_block *zicio_spcb = NULL;

	rcu_read_lock();
	hlist_for_each_entry_rcu(zicio_spcb, &rcu_hash->head, hash_node) {
		if (zicio_spcb->chunk_id + 1 == chunk_id) {
			break;
		}
	}
	rcu_read_unlock();

	return zicio_spcb;
}

/*
 * __zicio_rcu_hlist_del_for_each_entry
 *
 * Real worker to delete spcb from linked list of slot.
 */
static inline bool 
__zicio_rcu_hlist_del_from_list(struct zicio_rcu_hlist_head *rcu_hash,
			struct hlist_node *del_hash_node)
{
	unsigned long flags;

	spin_lock_irqsave(&rcu_hash->write_guard, flags);

	hlist_del_init_rcu(del_hash_node);

	spin_unlock_irqrestore(&rcu_hash->write_guard, flags);

	return true;
}

/*
 * zicio_init_hlist_head
 *
 * Initialize spcb rcu hash table.
 */
static inline void
zicio_init_hlist_head(struct zicio_rcu_hlist_head *zicio_hlist_head)
{
	INIT_HLIST_HEAD(&zicio_hlist_head->head);
	spin_lock_init(&zicio_hlist_head->write_guard);
}

/*
 * zicio_rcu_hash_add
 *
 * Add shared page control block(spcb) to shared pool hash table
 */
#define zicio_rcu_hash_add(hashtable, node, key)				\
		__zicio_rcu_hlist_add_head(								\
				&hashtable[zicio_rcu_hash_min(key,				\
						ZICIO_RCU_HASH_BITS(hashtable))], node)

/*
 * zicio_rcu_hash_find
 *
 * Find shared page control block(spcb) from shared pool hash table
 */
#define zicio_rcu_hash_find(hashtable, key)						\
		__zicio_rcu_hlist_find_for_each_entry(					\
				&hashtable[zicio_rcu_hash_min(key,				\
						ZICIO_RCU_HASH_BITS(hashtable))], key)

/*
 * zicio_rcu_hash_del
 *
 * Delete shared page control block(spcb) from shared pool hash table
 */
#define zicio_rcu_hash_del(hashtable, del_hash_node, key)		\
		__zicio_rcu_hlist_del_from_list(						\
				&hashtable[zicio_rcu_hash_min(key,				\
						ZICIO_RCU_HASH_BITS(hashtable))], del_hash_node)

#define ZICIO_CHANNEL_INFO_SIZE sizeof(zicio_attached_channel)

/* Flag for designating user ingestion point */
#define ZICIO_USER_BUFFER_IDX 0
#define ZICIO_FILE_CHUNK_ID 1

/**************************************************************************
 * Definitions and macros for zicio file and metadata for shared pool *
 **************************************************************************/

/*
 * zicio_binsearch_range
 *
 * Binary search function used to find the area where the key is located in the
 * array
 *
 * @key: key we are look for
 * @max_val: The maxium value of array
 * @base: Start pointer of array
 * @num_elem: The number of element in array
 * @size: The size of element
 * @compare: Callback function to compare
 */
static inline size_t 
zicio_binsearch_range(void *key, void *max_val, void *base, size_t num_elem,
			size_t size, int (*compare)(const void *, const void *))
{
	size_t low = 0, high = num_elem - 1, mid;
	char *mid_ptr, *next_ptr;
	int cmp, next_cmp;

	while (low <= high) {
		mid = (low + high) / 2;
		mid_ptr = (char*)base + mid * size;
		next_ptr = (mid == num_elem - 1) ? (char*)max_val : (char*)base +
					(mid + 1) * size;

		cmp = compare(mid_ptr, key);
		next_cmp = compare(next_ptr, key);

		if (cmp <= 0 && next_cmp > 0) {
			return mid;
		} else if (cmp > 0) {
			high = mid - 1;
		} else {
			low = mid + 1;
		}
	}
	return -1;
}

/*
 * zicio_compare_extent_to_lblk
 *
 * Return the result of  comparison the current local page number with the
 * start logical block number written in the extent.
 */
static inline int zicio_compare_extent_to_lblk(const void *a, const void *b)
{
	const struct zicio_ext4_extent *pa =
				(const struct zicio_ext4_extent*)a;
	const unsigned int *pb = (unsigned int *)b;
	const unsigned int ext_lblk = le32_to_cpu(pa->ee_block);

	return (ext_lblk > *pb) - (ext_lblk < *pb);
}

/*
 * zicio_get_current_file_id
 *
 * Get ID of the file most recently accessed by the channel
 */
static inline int
zicio_get_current_file_id(
				zicio_shared_pool_local *zicio_shared_pool_local)
{
	return zicio_shared_pool_local->consume_indicator.current_file_idx;
}

/*
 * zicio_get_file_extent_start
 *
 * Returns the start position of the extent of the current file in the file
 * extent stream of the shared pool.
 */
static inline unsigned long
zicio_get_file_extent_start(zicio_shared_pool *zicio_shared_pool, int idx)
{
	return zicio_shared_pool->shared_metadata_ctrl.file_start_point_extent[idx];
}

/*
 * zicio_get_file_chunk_start
 *
 * Returns the start position of the file chunk id of the current file in the
 * file stream of the shared pool.
 */
static inline unsigned int
zicio_get_file_chunk_start(zicio_shared_pool *zicio_shared_pool, int idx)
{
	return zicio_shared_pool->shared_files.start_chunk_nums[idx];
}

/*
 * zicio_get_num_files_shared
 *
 * Returns the number of files referenced in the shared pool.
 */
static inline int
zicio_get_num_files_shared(zicio_shared_pool *zicio_shared_pool)
{
	return zicio_shared_pool->shared_files.registered_read_files.num_fds;
}

/*
 * zicio_get_next_file_extent_start
 *
 * Returns the start position of the extent of the next file in the file
 * extent stream of the shared pool.
 */
static inline unsigned long
zicio_get_next_file_extent_start(
				zicio_shared_pool *zicio_shared_pool, int idx)
{
	if (idx + 1 == zicio_get_num_files_shared(zicio_shared_pool)) {
		return zicio_shared_pool->shared_metadata_ctrl.num_metadata;
	} else {
		return (zicio_shared_pool->
				shared_metadata_ctrl.file_start_point_extent[idx + 1]);
	}
}

/*
 * zicio_get_next_file_chunk_start
 *
 * Return the start position of file chunk id of next file in the file stream
 * of shared pool.
 */
static inline unsigned int
zicio_get_next_file_chunk_start(zicio_shared_pool *zicio_shared_pool,
				int idx)
{
	if (idx + 1 == zicio_get_num_files_shared(zicio_shared_pool)) {
		return zicio_shared_pool->shared_files.total_chunk_nums;
	} else {
		return zicio_shared_pool->shared_files.start_chunk_nums[idx + 1];
	}
}

/*
 * zicio_get_in_file_chunk_id
 *
 * Get the chunk id in file
 */
static inline unsigned int
zicio_get_in_file_chunk_id(zicio_shared_pool *zicio_shared_pool,
			zicio_shared_pool_local *zicio_shared_pool_local,
			int current_file_idx, unsigned int file_chunk_id)
{
	unsigned int start_chunk_id_mod;

	start_chunk_id_mod = zicio_get_file_chunk_start(zicio_shared_pool,
			current_file_idx) % zicio_shared_pool->shared_files.total_chunk_nums;
	/* BUG current file chunk id is smaller than start file chunk id of this
	 * file */
	BUG_ON(file_chunk_id < start_chunk_id_mod);

	return file_chunk_id - start_chunk_id_mod;
}

/*
 * zicio_get_num_extent_per_file
 *
 * Get the number of extent per file
 */
static inline unsigned int
zicio_get_num_extent_per_file(zicio_shared_pool *zicio_shared_pool,
			int cur_file_id)
{
	return (zicio_get_next_file_extent_start(zicio_shared_pool, cur_file_id)
		 - zicio_get_file_extent_start(zicio_shared_pool, cur_file_id));
}

/*
 * zicio_check_current_metadata
 *
 * Check whether the logical block offset received as a parameter is within the
 * range of the extent received as a parameter.
 */
static inline bool
zicio_check_current_metadata(struct zicio_ext4_extent *cur_extent,
			unsigned int cur_lblk_offset)
{
	unsigned int lblock = le32_to_cpu(cur_extent->ee_block);
	unsigned short len = le16_to_cpu(cur_extent->ee_len);

	return (lblock <= cur_lblk_offset && cur_lblk_offset < lblock + len);
}

/*
 * zicio_get_current_metadata
 *
 * Get the value of current metadata cursor.
 */
static inline unsigned long
zicio_get_current_metadata(zicio_shared_pool *zicio_shared_pool,
		zicio_shared_pool_local *zicio_shared_pool_local)
{
	if (zicio_shared_pool->shared_metadata_ctrl.num_metadata <=
		zicio_shared_pool_local->consume_indicator.current_metadata) {
		return zicio_shared_pool->shared_metadata_ctrl.num_metadata - 1;
	}
	return zicio_shared_pool_local->consume_indicator.current_metadata;
}

/*
 * zicio_set_current_metadata
 *
 * Set the value received as a parameter to metadata.
 */
static inline void
zicio_set_current_metadata(
			zicio_shared_pool_local *zicio_shared_pool_local,
			unsigned long consumed_metadata)
{
	zicio_shared_pool_local->consume_indicator.current_metadata
				= consumed_metadata;
}

/*
 * zicio_get_metadata_idx_for_chunk
 *
 * Using the logical page number, obtains and returns the index of the extent
 * that includes the logical page number from the extent stream buffer.
 */
static inline int
zicio_get_metadata_for_chunk(zicio_shared_pool *zicio_shared_pool,
			zicio_shared_pool_local *zicio_shared_pool_local,
			unsigned int file_idx, unsigned int start_logical_block)
{
	unsigned int lblk_max, num_extent_per_file;
	struct zicio_ext4_extent *metadata_buffer;
	unsigned int in_file_current_metadata;
	unsigned int current_file_metadata_start;
	unsigned long current_metadata;
	zicio_file_struct* zicio_file;

	zicio_file = zicio_get_id_file_struct(
			&zicio_shared_pool->shared_files.registered_read_files, file_idx);
	metadata_buffer =
		zicio_shared_pool->shared_metadata_ctrl.shared_metadata_buffer;
	current_metadata = zicio_get_current_metadata(zicio_shared_pool,
			zicio_shared_pool_local);
	lblk_max = zicio_file->file_size;
	current_file_metadata_start = zicio_get_file_extent_start(
			zicio_shared_pool, file_idx);
	/*
	 * First, we check if the file we know and the file currently pointed
	 * by the cursor are the same.
	 */
	if ((current_metadata < current_file_metadata_start) ||
		(current_metadata > zicio_get_next_file_extent_start(
				zicio_shared_pool, file_idx))) {
		/*
		 * If they are not the same, you must go to the extent area in the array
		 * of other files and verify the metadata.
		 */
		num_extent_per_file = zicio_get_num_extent_per_file(zicio_shared_pool,
					file_idx);
		/*
		 * Binary search extent array for logical block of next chunk.
		 * Binary search must be performed in the location where the extent of
		 * the file is cached.
		 */
		current_metadata = zicio_binsearch_range(&start_logical_block,
				&lblk_max, metadata_buffer + current_file_metadata_start,
				num_extent_per_file, sizeof(struct zicio_ext4_extent),
				zicio_compare_extent_to_lblk);
		current_metadata += current_file_metadata_start;

		zicio_shared_pool_local->consume_indicator.current_metadata
				= current_metadata;

		BUG_ON(zicio_shared_pool_local->consume_indicator.current_metadata >=
				zicio_shared_pool->shared_metadata_ctrl.num_metadata);

		BUG_ON(!zicio_check_current_metadata(
				metadata_buffer + current_metadata, start_logical_block));

		return current_metadata;
	}

	/*
	 * If the current extent cursor value is included in the area of the file to
	 * be read, an appropriate cursor is set using the extent indicated by the
	 * cursor value and the start logical block number, and this value is
	 * returned.
	 */

	/* Firtly, checking current metadata idx. */
	if (zicio_check_current_metadata(metadata_buffer + current_metadata,
				start_logical_block)) {
		return current_metadata;
	}

	/* Nextly, checking neighbor metadata idx */
	/* Get the index of metadata in file */
	in_file_current_metadata = current_metadata -
				zicio_get_file_extent_start(zicio_shared_pool, file_idx);
	/* Get the number of extent per file */
	num_extent_per_file = zicio_get_num_extent_per_file(zicio_shared_pool,
				file_idx);

	if (in_file_current_metadata + 1 != num_extent_per_file) {
		if (zicio_check_current_metadata(metadata_buffer +
				current_metadata + 1, start_logical_block)) {
			zicio_set_current_metadata(zicio_shared_pool_local,
						current_metadata + 1);
			return current_metadata + 1;
		}
	}

	if (in_file_current_metadata != 0) {
		if (zicio_check_current_metadata(metadata_buffer +
				current_metadata - 1, start_logical_block)) {
			zicio_set_current_metadata(zicio_shared_pool_local,
						current_metadata - 1);
			return current_metadata - 1;
		}
	}

	/*
	 * Binary search extent array for logical block of next chunk.
	 * Binary search must be performed in the location where the extent of
	 * the file is cached.
	 */
	current_metadata = zicio_binsearch_range(&start_logical_block,
				&lblk_max, metadata_buffer + current_file_metadata_start,
				num_extent_per_file, sizeof(struct zicio_ext4_extent),
				zicio_compare_extent_to_lblk);
	current_metadata += current_file_metadata_start;

	zicio_shared_pool_local->consume_indicator.current_metadata
			= current_metadata;

	BUG_ON(zicio_shared_pool_local->consume_indicator.current_metadata >=
				zicio_shared_pool->shared_metadata_ctrl.num_metadata);

	return current_metadata;
}

/*
 * zicio_check_current_file_id
 *
 * Checks whether the file pointed to by the current file cursor contains the
 * file chunk id.
 */
static inline bool
zicio_check_current_file_id(zicio_shared_pool *zicio_shared_pool,
			int file_idx, unsigned int file_chunk_id)
{
	unsigned int start, end;

	start = zicio_get_file_chunk_start(zicio_shared_pool, file_idx);
	end = zicio_get_next_file_chunk_start(zicio_shared_pool, file_idx);

	return ((start <= file_chunk_id) && (file_chunk_id < end));
}

/*
 * zicio_get_current_file_id_shared
 *
 * Get current file id from shared pool
 */
static inline int
zicio_get_current_file_id_shared(zicio_shared_pool *zicio_shared_pool,
			zicio_shared_pool_local *zicio_shared_pool_local,
			unsigned int file_chunk_id, bool set_shared_file, bool cached)
{
	int cur_file_idx = zicio_get_current_file_id(zicio_shared_pool_local);

	/* File index and metadata idx in array was set before */
	if (cached) {
		/* Fistly, checking current file */
		if (zicio_check_current_file_id(zicio_shared_pool, cur_file_idx,
						file_chunk_id)) {
			return cur_file_idx;
		}

		/*
		 * Nextly, checking neighbor
		 * If current file chunk id is in next file, then use its file chunk idx
		 * and, set it to zicio shared local pool.
		 */
		if (cur_file_idx !=
				zicio_get_num_files_shared(zicio_shared_pool)) {
			if (zicio_check_current_file_id(zicio_shared_pool,
						cur_file_idx + 1, file_chunk_id)) {
				if (set_shared_file) {
					zicio_shared_pool_local->consume_indicator.current_file_idx
								= cur_file_idx + 1;
					/* When file idx is set, then set its metadata meter */
					zicio_shared_pool_local->consume_indicator.current_metadata
								= zicio_get_file_extent_start(
										zicio_shared_pool, cur_file_idx + 1);
				}
				return cur_file_idx + 1;
			}
		} else {
			if (zicio_check_current_file_id(zicio_shared_pool, 0,
					file_chunk_id)) {
				if (set_shared_file) {
					zicio_shared_pool_local->consume_indicator.current_file_idx
							= 0;
					zicio_shared_pool_local->consume_indicator.current_metadata
							= zicio_get_file_extent_start(
									zicio_shared_pool, 0);
				}
				return 0;
			}
		}

		if (cur_file_idx != 0) {
			if (zicio_check_current_file_id(zicio_shared_pool,
						cur_file_idx - 1, file_chunk_id)) {
				if (set_shared_file) {
					zicio_shared_pool_local->consume_indicator.current_file_idx
							= cur_file_idx - 1;
					/* When file idx is set, then set its metadata meter */
					zicio_shared_pool_local->consume_indicator.current_metadata
							= zicio_get_file_extent_start(zicio_shared_pool,
									cur_file_idx - 1);
				}
				return cur_file_idx - 1;
			}
		} else {
			if (zicio_check_current_file_id(zicio_shared_pool,
					zicio_get_num_files_shared(zicio_shared_pool) - 1,
						file_chunk_id)) {
				if (set_shared_file) {
					zicio_shared_pool_local->consume_indicator.current_file_idx =
						zicio_get_num_files_shared(zicio_shared_pool) - 1;
					/* When file idx is set, then set its metadata meter */
					zicio_shared_pool_local->consume_indicator.current_metadata =
						zicio_get_file_extent_start(zicio_shared_pool,
							zicio_get_num_files_shared(zicio_shared_pool)
								- 1);
				}
				return zicio_get_num_files_shared(zicio_shared_pool) - 1;
			}
		}
	}

	/* Lastly, Checking all files or file index and metadata index array was not
	 * set before */
	for (cur_file_idx = 0 ; 
			cur_file_idx < zicio_get_num_files_shared(zicio_shared_pool) ;
				cur_file_idx++) {
		if (zicio_check_current_file_id(zicio_shared_pool, cur_file_idx,
					file_chunk_id)) {
			if (set_shared_file) {
				zicio_shared_pool_local->consume_indicator.current_file_idx =
						cur_file_idx;
				zicio_shared_pool_local->consume_indicator.current_metadata =
						zicio_get_file_extent_start(zicio_shared_pool,
								cur_file_idx);
			}
			return cur_file_idx;
		}
	}

	BUG_ON(true);
	return INT_MAX;
}

/*
 * zicio_get_current_tot_req_cnt_shared
 *
 * Returns the size of the chunk to perform I/O on now.
 */
static inline int
zicio_get_current_tot_req_cnt_shared(zicio_shared_pool *zicio_shared_pool,
			zicio_shared_pool_local *zicio_shared_pool_local,
			zicio_file_struct *zicio_obj, unsigned int in_file_chunk_id,
			int tot_req_cnt)
{
	/*
	 * If this is the last chunk, calculate the count of commands.
	 * (1 ~ ZICIO_CHUNK_SIZE >> ZICIO_PAGE_SHIFT)
	 */
	if (DIV_ROUND_UP(zicio_obj->file_size,
				((1UL) << ZICIO_PAGE_TO_CHUNK_SHIFT)) == in_file_chunk_id + 1) {
		tot_req_cnt = ((zicio_obj->file_size - 1) & ~ZICIO_PAGE_TO_CHUNK_MASK) + 1;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "[Kernel Message] last chunk, tot_req_cnt: %u\n",
				tot_req_cnt);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
	}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "[Kernel Message] in_file_chunk_id: %u, round_up: %u, tot_req_cnt: %u\n",
			in_file_chunk_id, DIV_ROUND_UP(zicio_obj->file_size, ((1UL) << ZICIO_PAGE_TO_CHUNK_SHIFT)), tot_req_cnt);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */

	return tot_req_cnt;
}

/*
 * zicio_convert_chunk_id_to_monotonic_id
 *
 * Convert logical file chunk id to monotonic id
 */
static inline unsigned int
zicio_convert_chunk_id_to_monotonic_id(
			zicio_shared_pool *zicio_shared_pool,
			zicio_shared_pool_local *zicio_shared_pool_local,
			unsigned int logical_file_chunk_id)
{
	zicio_channel_consumption_indicator *consume_indicator;
	unsigned int monotonic_id;

	consume_indicator = &zicio_shared_pool_local->consume_indicator;

	/*
	 * After checking whether it is wrapped using the information calculated at
	 * the time of initialization, if it is, calculate the monotonic id
	 * considering it.
	 */
	if (logical_file_chunk_id < consume_indicator->chunk_id_low) {
		monotonic_id = (consume_indicator->chunk_id_high + 1) *
			zicio_shared_pool->shared_files.total_chunk_nums +
			logical_file_chunk_id;
	} else {
		monotonic_id = (consume_indicator->chunk_id_high) *
			zicio_shared_pool->shared_files.total_chunk_nums +
			logical_file_chunk_id;
	}

	return monotonic_id;
}

/*
 * zicio_is_first_command-create
 *
 * Check this command creation is the first operation.
 */
static inline bool
zicio_is_first_command_create(
			zicio_shared_pool_local *zicio_shared_pool_local)
{
	return (zicio_shared_pool_local->consume_indicator.start_chunk_id_no_mod ==
			UINT_MAX);
}

void zicio_reset_shared_metadata_buffer(
			zicio_shared_pool *zicio_shared_pool);
void zicio_allocate_and_initialize_shared_metadata_ctrl(
			zicio_shared_pool *zicio_shared_pool, unsigned long total_ext_cnt,
			unsigned long *file_start_point);
int zicio_do_init_premapping(zicio_descriptor *desc,
		unsigned int *premap_start_point_no_mod);

/******************************************************
 * Definitions and macros for i/o and mapping control *
 *****************************************************/
void zicio_set_consume_indicator(zicio_descriptor *desc,
			unsigned int chunk_id);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
void zicio_create_reactivate_trigger_shared(zicio_descriptor *desc,
			int reason, bool derailed, unsigned page_id_queue_idx, bool pended,
			ktime_t first_page_get_time);
#else
void zicio_create_reactivate_trigger_shared(zicio_descriptor *desc,
			int reason, bool derailed, unsigned page_id_queue_idx, bool pended);
#endif
bool zicio_produce_local_huge_page_shared(
			struct zicio_descriptor *zicio_desc,
			struct zicio_shared_page_control_block *zicio_spcb,
			struct zicio_nvme_cmd_list *zicio_cmd);
int zicio_adjust_mapping_and_reclaim_pages(
			zicio_descriptor *desc, unsigned int *distance_from_head,
			bool from_softirq);
unsigned long zicio_get_shared_pool_consumption_tsc_delta(
			zicio_descriptor *desc);

/* Default exp time in nanoseconds */
#define ZICIO_DEFAULT_EXP_TIME_IN_NANO (1000000UL)
/* Flags to show I/O is requried */
#define ZICIO_IO_REQUIRED 0
/* Flags to show I/O is not requried */
#define ZICIO_IO_NOT_NEEDED 1
/* Flags to show the reason of softirq calling. */
#define ZICIO_NOIO 0
#define ZICIO_NOLOCALPAGE 1

/*
 * zicio_get_num_using_pages
 *
 * Get the number of pages currently being used on the local channel
 */
static inline int
zicio_get_num_using_pages(zicio_descriptor *desc)
{
	zicio_shared_pool_local	*zicio_shared_pool_local =
				zicio_get_shared_pool_local(desc);
	return atomic_read(&zicio_shared_pool_local->num_using_pages);
}

/*
 * zicio_get_max_num_channel_in_shared_pool
 *
 * Maximum id of id allocator managing channel descriptors currently attached to
 * shared pool
 */
static inline int
zicio_get_max_num_channel_in_shared_pool(
			zicio_shared_pool *zicio_shared_pool)
{
	return zicio_get_max_ids_from_idtable(&zicio_shared_pool->zicio_channels);
}

/*
 * zicio_inc_mapped_chunks_num
 *
 * Increase the number of mapped chunks of channel
 */
static inline void
zicio_inc_mapped_chunks_num(zicio_descriptor *desc)
{
	zicio_shared_pool_local	*zicio_shared_pool_local =
				zicio_get_shared_pool_local(desc);
	atomic_inc(&zicio_shared_pool_local->num_mapped);
}

/*
 * zicio_dec_mapped_chunks_num
 *
 * Decrease the number of mapped chunks of channel
 */
static inline void
zicio_dec_mapped_chunks_num(zicio_descriptor *desc)
{
	zicio_shared_pool_local	*zicio_shared_pool_local =
				zicio_get_shared_pool_local(desc);
	atomic_dec(&zicio_shared_pool_local->num_mapped);
}

static inline void
zicio_set_high_premap_point_only(zicio_descriptor *desc,
		int user_buffer_idx, unsigned int high_premap_point)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);

	atomic64_or(high_premap_point,
		&zicio_shared_pool_local->ingestion_point_track_array[user_buffer_idx]);
}

/*
 * __zicio_get_tracking_ingestion_point
 *
 * Get concatenate form of ingestion point information
 * file chunk id + distance
 */
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
static inline unsigned long long
#else /* !CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
static inline unsigned long
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
__zicio_get_tracking_ingestion_point(zicio_descriptor *desc,
		 int user_buffer_idx)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);

	return atomic64_read(
		&zicio_shared_pool_local->ingestion_point_track_array[user_buffer_idx]);
}

/*
 * zicio_get_tracking_ingestion_point
 *
 *  Get ingestion point of mapped buffer
 */
static inline unsigned long
zicio_get_tracking_ingestion_point(zicio_descriptor *desc,
		int user_buffer_idx, unsigned int *max_file_chunk_id,
		int *idx_distance)
{
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	unsigned long long concat_tracking_info =
			__zicio_get_tracking_ingestion_point(desc,
					user_buffer_idx);
#else /* !CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	unsigned long concat_tracking_info =
			__zicio_get_tracking_ingestion_point(desc,
					user_buffer_idx);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	ZICIO_SEPERATE_TRACKING_INFO(concat_tracking_info, idx_distance,
			max_file_chunk_id);

	return concat_tracking_info;
}

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
/*
 * zicio_get_tracking_ingestion_monotonic_chunk_id
 *
 * I don't think the above function can tell the exact chunk id of the buffer
 * where the user is located...
 *
 * So in this CONFIG_ZICIO_OPTIMIZE_SHARED_MODE, temporarily reserve the
 * bits for the real chunk id and read it.
 *
 * XXX 
 */
static inline uint64_t
zicio_get_tracking_ingestion_monotonic_chunk_id(zicio_descriptor *zicio_desc,
		int user_buffer_idx)
{
	uint64_t concat_tracking_info =
		__zicio_get_tracking_ingestion_point(zicio_desc, user_buffer_idx);

	return ZICIO_GET_TRACKING_INFO_MONOTONIC_CHUNK_ID(
			concat_tracking_info);
}
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

/*
 * zicio_set_tracking_ingestion_point
 *
 * Set tracking ingestion point array
 */
static inline void
zicio_set_tracking_ingestion_point(zicio_descriptor *desc,
		int user_buffer_idx, unsigned int max_file_chunk_id,
		int idx_distance_mod)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	unsigned long long concat_tracking_info;
#else /* !CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
	unsigned long concat_tracking_info;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED MODE */

	concat_tracking_info = ZICIO_CONCATENATE_TRACKING_INFO(
			idx_distance_mod, max_file_chunk_id);
	atomic64_set(
			&zicio_shared_pool_local->ingestion_point_track_array[user_buffer_idx],
			concat_tracking_info);
}

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
/*
 * Set monotonic chunk id
 */
static inline void
zicio_set_tracking_ingestion_monotonic_chunk_id(
		zicio_descriptor *zicio_desc, int user_buffer_idx,
		uint64_t monotonic_chunk_id)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
		zicio_get_shared_pool_local(zicio_desc);
	uint64_t concat_tracking_info =
		__zicio_get_tracking_ingestion_point(zicio_desc, user_buffer_idx);

	concat_tracking_info |=
		(monotonic_chunk_id << ZICIO_TRACKING_MONOTONIC_CHUNK_ID_SHIFT);

	atomic64_set(
		&zicio_shared_pool_local->ingestion_point_track_array[user_buffer_idx],
		concat_tracking_info);
}
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

/*
 * zicio_get_previous_high_premap_point
 *
 * Get previous premap high point
 */
static inline unsigned int
zicio_get_previous_high_premap_point(zicio_descriptor *desc)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	zicio_attached_channel *zicio_channel = zicio_shared_pool_local->zicio_channel;

	return atomic_read(&zicio_channel->previous_high_premap_point);
}

/*
 * zicio_get_user_ingestion_chunk_id
 *
 * Get user ingestion chunk id using ingestion point information.
 */
static inline unsigned int
zicio_get_user_ingestion_chunk_id(zicio_descriptor *desc,
		int user_buffer_idx)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	unsigned int chunk_id;
	int distance;

	if (user_buffer_idx == INT_MAX) {
		return zicio_shared_pool_local->consume_indicator.start_chunk_id_no_mod;
	}
	zicio_get_tracking_ingestion_point(desc, user_buffer_idx,
			&chunk_id, &distance);
	return chunk_id + distance;
}

/*
 * zicio_set_previous_high_premap_point
 *
 * Set high premap point to @new_high_premap_point
 */
static inline unsigned int
zicio_set_previous_high_premap_point(zicio_descriptor *desc,
		unsigned int new_high_premap_point)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	zicio_attached_channel *zicio_channel = zicio_shared_pool_local->zicio_channel;
	unsigned int old_high_premap_point =
			zicio_get_previous_high_premap_point(desc);

	if (old_high_premap_point < new_high_premap_point ||
			unlikely(old_high_premap_point == UINT_MAX)) {
		atomic_cmpxchg(&zicio_channel->previous_high_premap_point,
				old_high_premap_point, new_high_premap_point);
		return new_high_premap_point;
	}
	return old_high_premap_point;
}

void *zicio_alloc_shared_request_timer(zicio_descriptor *desc);
void zicio_free_shared_request_timer(zicio_descriptor *desc, 
		void *shared_req_timer);
int zicio_create_new_spcb(zicio_descriptor *desc);
int zicio_get_new_spcb(zicio_descriptor *desc);

/*
 * zicio_get_shared_request_timer
 *
 * Allocate and initialize zicio shared timer for shared pool mapping.
 */
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
static inline zicio_shared_request_timer *
zicio_get_shared_request_timer(zicio_descriptor *desc, int reason,
			bool derailed, unsigned int page_id_queue_idx, bool pended,
			ktime_t first_page_get_time)
#else
static inline zicio_shared_request_timer *
zicio_get_shared_request_timer(zicio_descriptor *desc, int reason,
			bool derailed, unsigned int page_id_queue_idx, bool pended)
#endif
{
	zicio_shared_request_timer *shared_req_timer =
			zicio_alloc_shared_request_timer(desc);

	INIT_LIST_HEAD(&shared_req_timer->user_consume_wait_list);
	shared_req_timer->desc = desc;
	shared_req_timer->reason = reason;
	shared_req_timer->derailed = derailed;
	shared_req_timer->pended = pended;
	shared_req_timer->next = NULL;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
	shared_req_timer->first_page_get_time = first_page_get_time;
#endif

	if (reason == ZICIO_NOLOCALPAGE) {
		shared_req_timer->page_id_queue_idx = page_id_queue_idx;
	} else {
		shared_req_timer->page_id_queue_idx = -1;
	}
	return shared_req_timer;
}

/*
 * zicio_get_num_shared_buffer
 *
 * Get the current number of shared huge pages.
 */
static inline int
zicio_get_num_shared_buffer(zicio_shared_pool *zicio_shared_pool)
{
	return atomic_read(&zicio_shared_pool->num_spcb);
}

/*
 * zicio_get_last_io_file_chunk_id
 *
 * Get the last file chunk id of which the I/O is performed to the chunk.
 */
static inline unsigned int
zicio_get_last_io_file_chunk_id(zicio_descriptor *desc)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	return atomic_read(&zicio_shared_pool_local->last_io_file_chunk_id);
}

/*
 * zicio_set_last_io_file_chunk_id
 *
 * Set the last file chunk id of which the I/O is performed to the chunk.
 */
static inline void
zicio_set_last_io_file_chunk_id(zicio_descriptor *desc,
			unsigned int last_io_file_chunk_id)
{
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	atomic_set(&zicio_shared_pool_local->last_io_file_chunk_id,
			last_io_file_chunk_id);
}

/*
 * zicio_get_dma_map_start_point_shared
 *
 * Get DMA map start point in device map array.
 */
int zicio_get_dma_map_start_point_shared(zicio_descriptor *desc,
			int device_idx);
int zicio_get_dma_map_start_point_shared_with_pool(
			zicio_shared_pool *zicio_shared_pool, int device_idx);

void zicio_wait_shared_page_reclaim(zicio_descriptor *desc);

/*
 * Start point of code segments related with shared pool.
 */
#ifdef CONFIG_ZICIO_STAT
/*
 * When a channel is created in shared mode, it indicates that it is in shared
 * mode to stat board.
 */
static inline void
zicio_set_channel_start_to_stat_shared(zicio_stat_board *stat_board)
{
	stat_board->is_shared = true;
	stat_board->channel_start_time = ktime_get();
}

/*
 * Record stats when derailed.
 */
static inline void
zicio_set_derailing_to_stat(zicio_descriptor *desc)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local *zicio_shared_pool_local =
				zicio_get_shared_pool_local(desc);

	desc->stat_board->latency_on_track = ktime_get() -
			desc->stat_board->channel_start_time;
	desc->stat_board->latency_derailed = ktime_get();
	desc->stat_board->num_mapped_chunk_on_track =
				atomic_read(&zicio_shared_pool_local->num_mapped);
	desc->stat_board->num_mapped_chunk_derailed =
				zicio_shared_pool->shared_files.total_chunk_nums -
				atomic_read(&zicio_shared_pool_local->num_mapped);
}

/*
 * Calculate the latency (nsec) after derailing.
 */
static inline void
zicio_set_latency_on_derailed_mode_to_stat(zicio_descriptor *desc)
{
	desc->stat_board->latency_derailed = ktime_get() -
				desc->stat_board->latency_derailed;
}

/*
 * Track the cause of the soft irq trigger in the shared pool.
 */
static inline void
zicio_count_softirq_trigger_shared(zicio_descriptor *desc, int type)
{
	desc->stat_board->soft_irq_trigger_cnt_shared++;
	if (zicio_check_channel_derailed(desc)) {
		desc->stat_board->soft_irq_trigger_derailed++;
		switch(type) {
			case ZICIO_NOIO:
				desc->stat_board->soft_irq_trigger_no_IO++;
				desc->stat_board->soft_irq_trigger_derailed_no_IO++;
				break;
			case ZICIO_NOLOCALPAGE:
				desc->stat_board->soft_irq_trigger_no_local_page++;
				desc->stat_board->soft_irq_trigger_derailed_no_local_page++;
				break;
			default:
				BUG_ON(true);
		}
	} else {
		desc->stat_board->soft_irq_trigger_on_track++;
		switch(type) {
			case ZICIO_NOIO:
				desc->stat_board->soft_irq_trigger_no_IO++;
				desc->stat_board->soft_irq_trigger_on_track_no_IO++;
				break;
			case ZICIO_NOLOCALPAGE:
				desc->stat_board->soft_irq_trigger_no_local_page++;
				desc->stat_board->soft_irq_trigger_on_track_no_local_page++;
				break;
			default:
				BUG_ON(true);
		}
	}
}

/*
 * Increase the number of contributed pages.
 */
static inline void
zicio_add_contributed_pages(zicio_descriptor *desc)
{
	desc->stat_board->num_pages_contributed++;
}

/*
 * Increase the number of owed pages.
 */
static inline void
zicio_add_endowed_pages(zicio_descriptor *desc)
{
	desc->stat_board->num_pages_shared++;
}

/*
 * Decrease the number of owed pages.
 */
static inline void
zicio_dec_endowed_pages(zicio_descriptor *desc)
{
	desc->stat_board->num_pages_shared--;
}

/*
 * Increase the number of premapped pages
 */
static inline void
zicio_add_premapped_pages(zicio_descriptor *desc)
{
	desc->stat_board->num_premapped_pages++;
}

/*
 * Decrease the number of premapped pages
 */
static inline void
zicio_add_forcefully_unmapped_pages(zicio_descriptor *desc)
{
	desc->stat_board->num_forcefully_unmapped_pages++;
}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
/*
 * Add excessive wait time
 */
static inline void
zicio_add_excessive_wait_time(zicio_descriptor *desc,
			ktime_t excessive_wait_time, bool request_for_derail)
{
	
	if (!request_for_derail) {
		desc->stat_board->wait_time_at_softirq_on_track += excessive_wait_time;
	} else {
		desc->stat_board->wait_time_at_softirq_derailed += excessive_wait_time;
	}
}
#endif

/*
 * Increament IO reserved count from no local page
 */
static inline void
zicio_inc_num_io_reserved_from_consumable_page_io(
			zicio_descriptor *desc)
{
	desc->stat_board->io_reserved_from_consumable_page++;
}

/*
 * Increament IO reserved count from head distance
 */
static inline void
zicio_inc_num_io_reserved_from_head_distance(
			zicio_descriptor *desc)
{
	desc->stat_board->io_reserved_from_avg_user_ingestion_point++;
}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
/*
 * Increment number of forceful unmap from ktime
 */
static inline void
zicio_inc_forceful_unmap_from_ktime(zicio_descriptor *desc)
{
	desc->stat_board->num_forceful_unmap_from_ktime++;
}

/*
 * Increment number of premap from ktime
 */
static inline void
zicio_inc_premap_failure_from_ktime(zicio_descriptor *desc)
{
	desc->stat_board->num_premap_failure_from_ktime++;
}

/*
 * Increment number of premap from spcb
 */
static inline void
zicio_inc_premap_failure_from_spcb(zicio_descriptor *desc)
{
	desc->stat_board->num_premap_failure_from_spcb++;
}

/*
 * Increment number of premap from exptime
 */
static inline void
zicio_inc_premap_failure_from_exptime(zicio_descriptor *desc)
{
	desc->stat_board->num_premap_failure_from_exptime++;
}

/*
 * Increment number of premap from expected safe time
 */
static inline void
zicio_inc_premap_failure_from_expected_safe_time(zicio_descriptor *desc)
{
	desc->stat_board->num_premap_failure_from_expected_safe_time++;
}

/*
 * Increment number of premap from invalid chunk id
 */
static inline void
zicio_inc_premap_failure_from_invalid_chunk_id(zicio_descriptor *desc)
{
	desc->stat_board->num_premap_failure_from_invalid_chunk_id++;
}

static inline void
zicio_inc_num_premapped_pages_consumed_after_a_jiffy(
			zicio_descriptor *desc)
{
	desc->stat_board->num_premapped_pages_consumed_after_a_jiffy++;
}

static inline void
zicio_add_consumable_chunk_number(
			zicio_descriptor *desc, unsigned int page_gap)
{
	desc->stat_board->check_page_gap++;
	desc->stat_board->sum_page_gap += page_gap;
}

static inline void
zicio_add_premapped_distance(zicio_descriptor *desc,
			unsigned int distance, unsigned int num_premap)
{
	desc->stat_board->num_real_premap += num_premap;
	desc->stat_board->num_real_premap_try++;
	desc->stat_board->num_premapped_distance += distance;
	desc->stat_board->num_premapped_try++;
}

#endif /* (CONFIG_ZICIO_DEBUG_LEVEL >= 2) */

#define ZICIO_ATOMIC64_ADD_STAT(stat) (atomic64_add((stat_board->stat), \
		((atomic64_t *)(&(shared_stat_board->stat)))))
#define ZICIO_DIV_NRCHANNEL_STAT(stat_board, stat) do { \
			(((stat_board)->stat) = (((stat_board)->stat) + \
			((stat_board)->nr_channels / 2)) / ((stat_board)->nr_channels)); \
		} while(0)

/*
 * zicio_update_shared_stat_board
 *
 * Called to update the shared pool's stats when the local channel closes.
 */
static inline void
zicio_update_shared_stat_board(zicio_descriptor *desc)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);

	zicio_stat_board *stat_board = desc->stat_board;
	zicio_stat_board *shared_stat_board = &zicio_shared_pool->stat_board;

	BUG_ON(!stat_board->is_shared);
	BUG_ON(!shared_stat_board->is_shared);

	atomic_inc((atomic_t*)(&shared_stat_board->nr_channels));
	ZICIO_ATOMIC64_ADD_STAT(soft_irq_cnt);
	ZICIO_ATOMIC64_ADD_STAT(io_interrupt_cnt);
	ZICIO_ATOMIC64_ADD_STAT(cpu_idle_loop_cnt);
	ZICIO_ATOMIC64_ADD_STAT(io_completion_time);
	ZICIO_ATOMIC64_ADD_STAT(num_mapped_chunk_on_track);
	ZICIO_ATOMIC64_ADD_STAT(num_mapped_chunk_derailed);
	ZICIO_ATOMIC64_ADD_STAT(num_pages_contributed);
	ZICIO_ATOMIC64_ADD_STAT(num_pages_shared);
	ZICIO_ATOMIC64_ADD_STAT(num_premapped_pages);
	ZICIO_ATOMIC64_ADD_STAT(num_forcefully_unmapped_pages);
	ZICIO_ATOMIC64_ADD_STAT(latency_on_track);
	ZICIO_ATOMIC64_ADD_STAT(latency_derailed);
	ZICIO_ATOMIC64_ADD_STAT(wait_time_at_softirq_on_track);
	ZICIO_ATOMIC64_ADD_STAT(wait_time_at_softirq_derailed);
	ZICIO_ATOMIC64_ADD_STAT(io_on_track);
	ZICIO_ATOMIC64_ADD_STAT(io_derailed);
	ZICIO_ATOMIC64_ADD_STAT(io_interrupt_cnt_on_track);
	ZICIO_ATOMIC64_ADD_STAT(io_interrupt_cnt_derailed);
	ZICIO_ATOMIC64_ADD_STAT(soft_irq_cnt_on_track);
	ZICIO_ATOMIC64_ADD_STAT(soft_irq_cnt_derailed);
	ZICIO_ATOMIC64_ADD_STAT(cpu_idle_loop_cnt_on_track);
	ZICIO_ATOMIC64_ADD_STAT(cpu_idle_loop_cnt_derailed);
	ZICIO_ATOMIC64_ADD_STAT(io_completion_time_on_track);
	ZICIO_ATOMIC64_ADD_STAT(io_completion_time_derailed);
	ZICIO_ATOMIC64_ADD_STAT(soft_irq_trigger_cnt_shared);
	ZICIO_ATOMIC64_ADD_STAT(soft_irq_trigger_on_track);
	ZICIO_ATOMIC64_ADD_STAT(soft_irq_trigger_derailed);
	ZICIO_ATOMIC64_ADD_STAT(soft_irq_trigger_no_local_page);
	ZICIO_ATOMIC64_ADD_STAT(soft_irq_trigger_no_IO);
	ZICIO_ATOMIC64_ADD_STAT(soft_irq_trigger_on_track_no_local_page);
	ZICIO_ATOMIC64_ADD_STAT(soft_irq_trigger_on_track_no_IO);
	ZICIO_ATOMIC64_ADD_STAT(soft_irq_trigger_derailed_no_local_page);
	ZICIO_ATOMIC64_ADD_STAT(soft_irq_trigger_derailed_no_IO);
	ZICIO_ATOMIC64_ADD_STAT(num_forceful_unmap_from_ktime);
	ZICIO_ATOMIC64_ADD_STAT(num_premap_failure_from_ktime);
	ZICIO_ATOMIC64_ADD_STAT(num_premap_failure_from_spcb);
	ZICIO_ATOMIC64_ADD_STAT(num_premap_failure_from_exptime);
	ZICIO_ATOMIC64_ADD_STAT(num_premap_failure_from_expected_safe_time);
	ZICIO_ATOMIC64_ADD_STAT(num_premap_failure_from_invalid_chunk_id);
	ZICIO_ATOMIC64_ADD_STAT(num_premapped_pages_consumed_after_a_jiffy);
	ZICIO_ATOMIC64_ADD_STAT(num_premapped_distance);
	ZICIO_ATOMIC64_ADD_STAT(num_real_premap_try);
	ZICIO_ATOMIC64_ADD_STAT(num_premapped_try);
	ZICIO_ATOMIC64_ADD_STAT(check_page_gap);
	ZICIO_ATOMIC64_ADD_STAT(sum_page_gap);
	ZICIO_ATOMIC64_ADD_STAT(num_premap_in_softirq);
	ZICIO_ATOMIC64_ADD_STAT(num_premap_in_nvme_irq);
}

/*
 * zicio_calculate_average_from_shared_stat_board
 *
 * Updates the average of stat board members.
 */
static inline void
zicio_calculate_average_from_shared_stat_board(
		zicio_shared_pool *zicio_shared_pool)
{
	zicio_stat_board *stat_board = &zicio_shared_pool->stat_board;

	BUG_ON(!stat_board->is_shared);

	ZICIO_DIV_NRCHANNEL_STAT(stat_board, soft_irq_cnt);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, io_interrupt_cnt);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, cpu_idle_loop_cnt);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, io_completion_time);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_mapped_chunk_on_track);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_mapped_chunk_derailed);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_pages_contributed);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_pages_shared);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_premapped_pages);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_forcefully_unmapped_pages);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, latency_on_track);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, latency_derailed);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, wait_time_at_softirq_on_track);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, wait_time_at_softirq_derailed);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, io_on_track);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, io_derailed);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, io_interrupt_cnt_on_track);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, io_interrupt_cnt_derailed);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, soft_irq_cnt_on_track);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, soft_irq_cnt_derailed);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, cpu_idle_loop_cnt_on_track);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, cpu_idle_loop_cnt_derailed);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, io_completion_time_on_track);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, io_completion_time_derailed);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, soft_irq_trigger_cnt_shared);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, soft_irq_trigger_on_track);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, soft_irq_trigger_derailed);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, soft_irq_trigger_no_local_page);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, soft_irq_trigger_no_IO);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board,
				soft_irq_trigger_on_track_no_local_page);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board,
				soft_irq_trigger_on_track_no_IO);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board,
				soft_irq_trigger_derailed_no_local_page);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board,
				soft_irq_trigger_derailed_no_IO);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_forceful_unmap_from_ktime);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_premap_failure_from_ktime);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_premap_failure_from_spcb);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_premap_failure_from_exptime);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board,
				num_premap_failure_from_expected_safe_time);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board,
				num_premap_failure_from_invalid_chunk_id);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board,
				num_premapped_pages_consumed_after_a_jiffy);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_premapped_distance);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_real_premap);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_real_premap_try);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_premapped_try);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, sum_page_gap);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, check_page_gap);

	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_premap_in_softirq);
	ZICIO_DIV_NRCHANNEL_STAT(stat_board, num_premap_in_nvme_irq);
}

static inline void
zicio_dump_shared_stat_board(zicio_shared_pool *zicio_shared_pool)
{
	zicio_stat_board *stat_board = &zicio_shared_pool->stat_board;
	signed long long latency_on_track_integer_part;
	signed long long latency_on_track_fractional_part;
	signed long long latency_derailed_integer_part;
	signed long long latency_derailed_fractional_part;

	if (!zicio_shared_pool->stat_board.nr_channels) {
		return;
	}

	zicio_calculate_average_from_shared_stat_board(zicio_shared_pool);

	latency_on_track_integer_part = stat_board->latency_on_track / 1000000;
	latency_on_track_fractional_part = stat_board->latency_on_track % 1000000;
	latency_derailed_integer_part = stat_board->latency_derailed / 1000000;
	latency_derailed_fractional_part = stat_board->latency_derailed % 1000000;

	printk(KERN_WARNING "[ZICIO SHARED POOL] "
		"softirq: %ld, io_handler: %ld, idle: %ld, io_completion_time: %lld, "
		"excessive_wait_time_in_softirq(msec) on track: %lu.%lu, "
		"excessive_wait_time_in_softirq(msec) derailed: %lu.%lu\n",
		stat_board->soft_irq_cnt, stat_board->io_interrupt_cnt,
		stat_board->cpu_idle_loop_cnt, stat_board->io_completion_time,
		stat_board->wait_time_at_softirq_on_track / 1000000,
		stat_board->wait_time_at_softirq_on_track % 1000000,
		stat_board->wait_time_at_softirq_derailed / 1000000,
		stat_board->wait_time_at_softirq_derailed % 1000000);

	printk(KERN_WARNING "[ZICIO] shared channel stat num proc[%d]\n",
			stat_board->nr_channels);
	printk(KERN_WARNING "[ZICIO] "
		"Read chunks on track: %lu, Read chunks derailed: %lu, "
		"Contributed pages: %lu, Shared page: %lu , "
		"Premapped pages: %lu, Forcefully unmapped page: %lu, "
		"Latency(msec) on track: %lld.%lld, Latency(msec) derailed: %lld.%lld\n",
			stat_board->num_mapped_chunk_on_track,
			stat_board->num_mapped_chunk_derailed,
			stat_board->num_pages_contributed,
			stat_board->num_pages_shared,
			stat_board->num_premapped_pages,
			stat_board->num_forcefully_unmapped_pages,
			latency_on_track_integer_part, latency_on_track_fractional_part,
			latency_derailed_integer_part, latency_derailed_fractional_part);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "[ZICIO] Number of premapped pages consumed after a jiffy: %lu, "
			"Average of premapped distance: %lu, Average of user consumable pages for a jiffy: %lu, "
			"Average of premap pages at one premap function call: %lu\n",
				stat_board->num_premapped_pages_consumed_after_a_jiffy,
				stat_board->num_premapped_distance / stat_board->num_premapped_try,
				stat_board->sum_page_gap / stat_board->check_page_gap,
				stat_board->num_real_premap / stat_board->num_real_premap_try);
	printk(KERN_WARNING "[ZICIO] Number of forceful unmap from ktime: %lu, "
		"Number of premap failure from ktime: %lu, "
		"Number of premap failure from spcb: %lu, "
		"Number of premap failure from exptime: %lu, "
		"Number of premap failure from expected safe time: %lu, "
		"Number of premap failure from invalid chunk id : %lu\n",
			stat_board->num_forceful_unmap_from_ktime,
			stat_board->num_premap_failure_from_ktime,
			stat_board->num_premap_failure_from_spcb,
			stat_board->num_premap_failure_from_exptime,
			stat_board->num_premap_failure_from_expected_safe_time,
			stat_board->num_premap_failure_from_invalid_chunk_id);
#endif /* (CONFIG_ZICIO_DEBUG_LEVEL >= 2) */

	printk(KERN_WARNING "[ZICIO] "
		"softirq trigger count: %lu, softirq trigger no local page: %lu, "
		"softirq trigger no IO:%lu\n",
			stat_board->soft_irq_trigger_cnt_shared,
			stat_board->soft_irq_trigger_no_local_page,
			stat_board->soft_irq_trigger_no_IO);
	printk(KERN_WARNING "[ZICIO] stats on track\n");
	printk(KERN_WARNING "[ZICIO] "
		"io cnt on track: %lu, io_handler on track: %lu, idle on track: %lu, "
		"io softirq on track:%lu, io completion time :%lld\n",
			stat_board->io_on_track,
			stat_board->io_interrupt_cnt_on_track,
			stat_board->cpu_idle_loop_cnt_on_track,
			stat_board->soft_irq_cnt_on_track,
			stat_board->io_completion_time_on_track);
	printk(KERN_WARNING "[ZICIO] "
		"Reactivate softirq trigger on track: %lu, "
		"Reactivate softirq trigger no local page on track: %lu, "
		"Reactivate softirq trigger no IO on track: %lu\n",
			stat_board->soft_irq_trigger_on_track,
			stat_board->soft_irq_trigger_on_track_no_local_page,
			stat_board->soft_irq_trigger_on_track_no_IO);

	printk(KERN_WARNING "[ZICIO] stats derailed\n");
	printk(KERN_WARNING "[ZICIO] "
		"io cnt derailed: %lu, io_handler derailed: %lu, idle derailed: %lu, "
		"io softirq derailed:%lu, io completion time :%lld\n",
			stat_board->io_derailed,
			stat_board->io_interrupt_cnt_derailed,
			stat_board->cpu_idle_loop_cnt_derailed,
			stat_board->soft_irq_cnt_derailed,
			stat_board->io_completion_time_derailed);
	printk(KERN_WARNING "[ZICIO] "
		"Reactivate softirq trigger derailed: %lu, "
		"Reactivate softirq trigger no local page derailed: %lu, "
		"Reactivate softirq trigger no IO derailed: %lu\n",
			stat_board->soft_irq_trigger_derailed,
			stat_board->soft_irq_trigger_derailed_no_local_page,
			stat_board->soft_irq_trigger_derailed_no_IO);

	printk(KERN_WARNING "[ZICIO] "
		"Number of premapped chunks in softirq: %lu, "
		"Number of premapped chunks in nvme irq: %lu\n",
		stat_board->num_premap_in_softirq,
		stat_board->num_premap_in_nvme_irq);
}
#endif /* (CONFIG_ZICIO_STAT) */

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 1)
void zicio_dump_shared_pool(void *zicio_shared_pool);
void zicio_dump_switch_board_shared(zicio_descriptor *desc,
	struct zicio_switch_board *sb, int user_buffer_idx);
#endif /* (CONFIG_ZICIO_DEBUG_LEVEL >= 2) */

#endif /* ZICIO_SHARED_POOL_H_ */
