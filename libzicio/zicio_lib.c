#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif	/* _GNU_SOURCE */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/unistd.h>
#include <linux/zicio.h>
#include <sys/mman.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <stdint.h>
#include <sched.h>

#include "include/libzicio.h"
#include "include/zicio_atomic.h"

/* memory barrier */
#define mb() __asm__ __volatile__("":::"memory")

#define MIN(a, b) (a < b ? a : b);
#define MAX(a, b) (a < b ? b : a);

/*
 * The number of chunks is written in linux/zicio.h
 * Based on this, the size of the bitmaps in switch board are determined.
 *
 * TODO: linux may be able to provide these macros. In that case, instead of 
 *       writing down the values like this, use them. 
 */
#define __ZICIO_LIB_MAX_NUM_CHUNK				(16)
#define __ZICIO_LIB_MAX_NUM_SHARING_CHUNK		(512)
#define __ZICIO_LIB_CHUNK_SIZE_SHIFT			(21)
#define __ZICIO_LIB_CHUNK_SIZE					(1UL << __ZICIO_LIB_CHUNK_SIZE_SHIFT)

#define MIN_READ_CHUNK_SIZE	(512)					/* 512 bytes */
#define MAX_READ_CHUNK_SIZE	(__ZICIO_LIB_CHUNK_SIZE)	/* 2 MiB */

#define GET_READ_CHUNK(addr, offset) \
	((void *) (((unsigned long) addr) + offset))

/*
 * As in zicio_firehose_ctrl.c, we modify the switch board's bitmap .
 * At this time, modular operation is needed. So use mask.
 */
#define __ZICIO_LIB_MASK_MAX_NUM_CHUNK         (__ZICIO_LIB_MAX_NUM_CHUNK - 1)
#define __ZICIO_LIB_MASK_MAX_NUM_SHARING_CHUNK (__ZICIO_LIB_MAX_NUM_SHARING_CHUNK - 1)

/*
 * To share switch board content with the kernel,
 * declare bitmap the same way.
 */
#define __ZICIO_LIB_DIV_ROUND_UP(n,d)		(((n) + (d) - 1) / (d))
#define __ZICIO_LIB_BITS_PER_BYTE			(8)
#define __ZICIO_LIB_BITS_PER_TYPE(type)	(sizeof(type) *__ZICIO_LIB_BITS_PER_BYTE)
#define __ZICIO_LIB_BITS_TO_LONGS(nr)		\
	__ZICIO_LIB_DIV_ROUND_UP(nr, __ZICIO_LIB_BITS_PER_TYPE(long))
#define __ZICIO_LIB_DECLARE_BITMAP(name, bits) \
	unsigned long name[__ZICIO_LIB_BITS_TO_LONGS(bits)]

/*
 * These macros are used to modify the bitmap of the switch board in
 * zicio_firehose_ctrl.c
 *
 * libzicio also uses the same macros to modify the bitmap.
 */
#define __SB_BITMAP_WORD(bit)			(bit >> 6)
#define __SB_BITMAP_WORD_MASK			(__ZICIO_LIB_BITS_PER_TYPE(unsigned long) - 1)
#define __SB_BITMAP_WORD_START_BIT_IDX	(0)
#define __SB_BITMAP_WORD_LAST_BIT_IDX	(__SB_BITMAP_WORD_MASK)
#define __SB_BITMAP_WORD_FULL_SET		(~0UL)

#define BATCH_INFO_FILE_MIN_CAPACITY	(8)
#define BATCH_INFO_BATCH_MIN_CAPACITY	(512)

/*
 * Is the current size greater or equal than the minimum capacity and a power
 * of 2?
 */
#define need_to_be_resized(cur_size, min_capacity) \
	(cur_size >= min_capacity && (cur_size & (cur_size - 1)) == 0)

/**
 * zicio_channel_info - data structure containing each channel's information
 * @switch_board: pointer of channel's switch board
 * @stat_board: pointer of channel's stat board
 * @zicio: pointer of zicio structure for user
 * @begin_tsc: tsc at the time the chunk was obtained for each channel
 * @zicio_id: zicio channel id
 * @get_chunk_offset: read offset
 * @put_chunk_offset: exhausted offset
 * @total_nr_chunk: the total # of chunk to read
 * @cur_nr_chunk: current # of chunk to read
 * @wait_begin: begin time of user waiting
 * @wait_begin_time: timespec for measuring
 * @total_wait_time_ns: total wait time in nanosecond
 * @min_wait_time_ns: minimum wait time for a chunk
 * @max_wait_time_ns: max wait time for a chunk
 * @ingestion_begin_time: timespec for measuring
 * @total_ingestion_time_ns: total ingestion time in nanosecond
 *
 * This is the element of zicio_channel_vector.
 */
struct zicio_channel_info {
	struct zicio_switch_board *sb;
	struct zicio_stat_board *stat_board;
	struct zicio *zicio;
	unsigned long begin_tsc;
	int zicio_id;
	unsigned long get_chunk_offset;
	unsigned long put_chunk_offset;
	unsigned long read_page_size;
	unsigned long total_nr_chunk;
	unsigned long cur_nr_chunk;
	bool wait_begin;
	struct timespec wait_begin_time;
	unsigned long total_wait_time_ns;
	unsigned long min_wait_time_ns;
	unsigned long max_wait_time_ns;
	bool sharing_mode;
	struct timespec zicio_start_time;
	unsigned long cur_read_bytes;
};

/**
 * zicio_channel_vector - vector containing zicio_channel_info
 * @data:
 * @size:
 * @capacity:
 *
 * User can open multiple zicio channels. Also, rather than completely
 * consuming one channel and consuming another channel, it is possible to
 * consume them simultaneously.
 *
 * Therefore, for all channels, both the address of the switch board and the
 * start tsc of the currently consuming chunk must be managed.
 */
struct zicio_channel_vector {
	struct zicio_channel_info *data;
	int size;
	int capacity;
};

/*
 * In some database engined, a thread can be a single user. So manage channels
 * via thread local variable.
 *
 * Also use the "static" keyword to not expose the switch board outside the lib.
 */
static __thread struct zicio_channel_vector *zicio_ch_vec = NULL;

/*
 * __zicio_get_total_chunk_num - get total chunk number
 *
 * Return the total number of chunk with round up.
 */
static unsigned long
__zicio_get_total_chunk_num(int *fds, int nr_fd)
{
	unsigned long total_nr_chunk = 0;
	int i;

	for (i = 0; i < nr_fd; ++i) {
		unsigned long size, nr_chunk;

		size = lseek(fds[i], 0, SEEK_END);
		
		nr_chunk = size / __ZICIO_LIB_CHUNK_SIZE; 
		if (size % __ZICIO_LIB_CHUNK_SIZE)
		  nr_chunk += 1;

		total_nr_chunk += nr_chunk;
	}

	return total_nr_chunk;
}

/**
 * zicio_channel_vector_init - initialize zicio channel vector
 *
 * Initialize the thread local vector.
 */
static int zicio_channel_vector_init(void)
{
	if (zicio_ch_vec == NULL) {
		zicio_ch_vec = (struct zicio_channel_vector *)malloc(
			sizeof(struct zicio_channel_vector));

		if (zicio_ch_vec == NULL)
			return ZICIO_OPEN_CH_VEC_INIT_ERROR;

		zicio_ch_vec->data = NULL;
		zicio_ch_vec->size = 0;
		zicio_ch_vec->capacity = 0;
	}
	return ZICIO_OPEN_SUCCESS;
}

/**
 * zicio_channel_vector_free - Free the zicio channel vector
 *
 * Free the thread local vector
 */
static void zicio_channel_vector_free(void)
{
	if (zicio_ch_vec == NULL)
		return;

	if (zicio_ch_vec->data)
		free(zicio_ch_vec->data);
	free(zicio_ch_vec);
	zicio_ch_vec = NULL;
}

/**
 * zicio_channel_vector_resize - resize zicio_channel_vector
 * @capacity: new capacity
 *
 * This function is mainly used to increase the capacity of
 * zicio_channel_vector.
 *
 * Return 0 if success, otherwise return -1;
 */
static int zicio_channel_vector_resize(int capacity)
{
	struct zicio_channel_info *new_data;

	if (zicio_ch_vec == NULL)
		return -1;

	new_data = (struct zicio_channel_info *)realloc(zicio_ch_vec->data,
		capacity * sizeof(struct zicio_channel_info));

	if (new_data == NULL)
		return -1;	

	zicio_ch_vec->data = new_data;
	zicio_ch_vec->capacity = capacity;

	return 0;
}

/**
 * zicio_channel_vector_push_back - push back new channel info
 * @sb: pointer of switch board
 * @stat_board: pointer of stat board
 * @zicio_id: zicio channel id
 * 
 * Returns the index in a vector. If there is no vector, return -1.
 */
static int zicio_channel_vector_push_back(
	struct zicio_switch_board *sb, struct zicio_stat_board *stat_board,
	int zicio_id)
{
	int new_capacity;
	int idx;

	if (zicio_ch_vec == NULL)
		return -1;

	if (zicio_ch_vec->size == zicio_ch_vec->capacity) {
		new_capacity =  zicio_ch_vec->capacity == 0 ? 4 : zicio_ch_vec->capacity * 2;
		if (zicio_channel_vector_resize(new_capacity) == -1)
			return -1;
	}
	
	idx = zicio_ch_vec->size++;
	zicio_ch_vec->data[idx].sb = sb;
	zicio_ch_vec->data[idx].stat_board = stat_board;
	zicio_ch_vec->data[idx].zicio_id = zicio_id;
	zicio_ch_vec->data[idx].begin_tsc = 0;

	return idx;
}

/**
 * zicio_channel_vector_erase - remove element
 * @idx: index in vector
 *
 * Remove the zicio channel info and pull the remaining elements.
 */
static void zicio_channel_vector_erase(int idx)
{
	if (zicio_ch_vec == NULL)
		return;
	if (zicio_ch_vec->size <= idx)
		return;

	for (int i = idx + 1; i < zicio_ch_vec->size; i++) {
		struct zicio_channel_info *zicio_ch_info;
		struct zicio *zicio;

		/* Get zicio structure from zicio channel structure */
		zicio_ch_info = 
			(struct zicio_channel_info *) &zicio_ch_vec->data[i];
		zicio = zicio_ch_info->zicio;

		/* Modify channel index */
		zicio->zicio_channel_idx -= 1;

		zicio_ch_vec->data[i - 1] = zicio_ch_vec->data[i];
	}
	zicio_ch_vec->size--;
}

/**
 * zicio_channel_vector_get - get the channel info using the given index
 * @idx: index in vector
 *
 * Return the pointer of zicio_channel_info.
 * If there is no vector, return NULL.
 */
static struct zicio_channel_info *zicio_channel_vector_get(int idx)
{
	struct zicio_channel_info *ch_info = NULL;

	if (zicio_ch_vec == NULL)
		return ch_info;

	if (idx >= 0 && idx <= zicio_ch_vec->size - 1)
		ch_info = zicio_ch_vec->data + idx;

	return ch_info;
}

static inline void zicio_batch_info_init(struct zicio_batch_info *batch_info) {
	batch_info->batches_array = NULL;
	batch_info->nr_batches = NULL;
	batch_info->nr_notified_pages = 0;
}

static inline void zicio_batch_info_free(struct zicio_batch_info *batch_info,
										 int nr_fd) {
	int i;
	for (i = 0; i < nr_fd; i++)
		free(batch_info->batches_array[i]);

	free(batch_info->batches_array);
	free(batch_info->nr_batches);
}

/**
 * @brief Check if batch_info can be added with information about the new file
 * and resize it if necessary.
 * 
 * @param batch_info: pointer of zicio_batch_info
 * @param cur_nr_fd: current number of fd(file)
 * @return 1 if there is not enough memory, otherwise 0
 */
static int zicio_batch_info_check_resize_fd(struct zicio_batch_info *batch_info,
											int cur_nr_fd) {
	if (unlikely(cur_nr_fd == 0)) { /* init */
		batch_info->nr_batches =
			(unsigned*)calloc(BATCH_INFO_FILE_MIN_CAPACITY, sizeof(unsigned));
		batch_info->batches_array =
			(unsigned**)calloc(BATCH_INFO_FILE_MIN_CAPACITY, sizeof(unsigned*));

		/* no memory */
		if (batch_info->nr_batches == NULL || batch_info->batches_array == NULL)
			return 1;
	} else if (unlikely(need_to_be_resized(cur_nr_fd,
										   BATCH_INFO_FILE_MIN_CAPACITY))) { /* resize */
		unsigned *nr_batches;
		unsigned **batches_array;

		nr_batches = (unsigned*)realloc(batch_info->nr_batches,
										2 * cur_nr_fd * sizeof(unsigned));
		batches_array = (unsigned**)realloc(batch_info->batches_array,
										   2 * cur_nr_fd * sizeof(unsigned*));

		/* no memory */
		if (nr_batches == NULL || batches_array == NULL)
			return 1;

		/* zero out newly allocated area */
		memset(nr_batches + cur_nr_fd, 0, cur_nr_fd * sizeof(unsigned));
		memset(batches_array + cur_nr_fd, 0, cur_nr_fd * sizeof(unsigned*));

		batch_info->nr_batches = nr_batches;
		batch_info->batches_array = batches_array;
	}

	return 0;
}

/**
 * @brief Check if batch_info can be added with information about the new batch
 * and resize it if necessary.
 * 
 * @param batch_info: pointer of zicio_batch_info
 * @param fd_idx index of the current fd(file)
 * @return 1 if there is not enough memory, otherwise 0
 */
static int zicio_batch_info_check_resize_batch(struct zicio_batch_info *batch_info,
											   int fd_idx) {
	int cur_nr_batches = batch_info->nr_batches[fd_idx];

	if (unlikely(cur_nr_batches == 0)) { /* init */
		assert(batch_info->batches_array[fd_idx] == NULL);

		batch_info->batches_array[fd_idx] =
			(unsigned*)calloc(BATCH_INFO_BATCH_MIN_CAPACITY,
							  2 * sizeof(unsigned)); /* start, end */

		/* no memory */
		if (batch_info->batches_array[fd_idx] == NULL)
			return 1;
	} else if (unlikely(need_to_be_resized(cur_nr_batches,
										   BATCH_INFO_BATCH_MIN_CAPACITY))) { /* resize */
		unsigned *batches;

		batches = (unsigned*)realloc(batch_info->batches_array[fd_idx],
									 2 * cur_nr_batches * 2 * sizeof(unsigned)); /* start, end */

		/* no memory */
		if (batches == NULL)
			return 1;

		/* zero out newly allocated area */
		memset(batches + 2 * cur_nr_batches, 0,
			   cur_nr_batches * 2 * sizeof(unsigned));

		batch_info->batches_array[fd_idx] = batches;
	}

	return 0;
}

/**
 * @brief Notify a new batch of pages
 * 
 * @param zicio zicio interface
 * @param fd fd of the batch
 * @param start the first page of this batch
 * @param end the last page of this batch
 * 
 * @return 1 if there is not enough memory, otherwise 0
 */
int zicio_notify_ranges(struct zicio *zicio, int fd, unsigned start,
					   unsigned end) {
	int idx = zicio->nr_local_fd - 1; /* index of latest fd */
	int start_idx, end_idx, amplifier;
	struct zicio_batch_info *batch_info = &(zicio->batch_info);

	if (unlikely(end < start))
		return 1;

	/* first time, or new fd */
	if (unlikely(idx < 0 || zicio->local_fds[idx] != fd)) {
		int i;

		for (i = 0; i < idx; i++) {
			if (zicio->local_fds[i] == fd)
				break;
		}

		if (likely(i == idx || idx == -1)) {
			if (unlikely(zicio_batch_info_check_resize_fd(batch_info,
														  zicio->nr_local_fd)))
				return 1;

			zicio->local_fds[++idx] = fd;
			zicio->nr_local_fd++;
		} else
			idx = i;
	}

	/* Adjust granularity of the page: read_page_size -> KZICIO_PAGE_SIZE */
	amplifier = zicio->read_page_size / KZICIO_PAGE_SIZE;
	start = start * amplifier;
	end = (end + 1) * amplifier - 1;
	batch_info->nr_notified_pages += (end - start + 1);

	/*
	 * Set indexes to the pages of the previous batch to try compaction.
	 * (even number is for start page, and odd number is for end page)
	 */
	start_idx = 2 * ((int)(batch_info->nr_batches[idx]) - 1);
	end_idx = start_idx + 1;

	/*
	 * If the previous batch and the new batch are consecutive, compact those
	 * two batches.
	 */
	if (end_idx > 0 && batch_info->batches_array[idx][end_idx] + 1 == start) {
		batch_info->batches_array[idx][end_idx] = end;
		return 0;
	}

	if (unlikely(zicio_batch_info_check_resize_batch(batch_info, idx)))
		return 1;

	/* Move to the current(new) batch */
	start_idx += 2;
	end_idx += 2;

	batch_info->batches_array[idx][start_idx] = start;
	batch_info->batches_array[idx][end_idx] = end;
	batch_info->nr_batches[idx]++;

	return 0;
}

/**
 * zicio_channel_info_init - init zicio channel info
 * @zicio: zicio structure
 */
static void zicio_channel_info_init(struct zicio *zicio, bool sharing_mode)
{
	struct zicio_channel_info *zicio_ch_info;

	/* Get zicio channel */
	zicio_ch_info = zicio_channel_vector_get(zicio->zicio_channel_idx);

	/* Init zicio channel info */
	zicio_ch_info->zicio = zicio;
	zicio_ch_info->read_page_size = zicio->read_page_size;
	zicio_ch_info->get_chunk_offset = 0;
	zicio_ch_info->put_chunk_offset = 0;
	zicio_ch_info->cur_nr_chunk = 0;
	zicio_ch_info->total_wait_time_ns = 0;
	zicio_ch_info->min_wait_time_ns = UINT64_MAX;
	zicio_ch_info->max_wait_time_ns = 0;
	zicio_ch_info->sharing_mode = sharing_mode;
	zicio_ch_info->wait_begin = false;
	clock_gettime(CLOCK_MONOTONIC, &zicio_ch_info->zicio_start_time);

	memset(&zicio_ch_info->wait_begin_time, 0, sizeof(struct timespec));

	/* Calculate total chunk size */
	if (zicio->batch_info.batches_array == NULL) {
		zicio_ch_info->total_nr_chunk = 
			__zicio_get_total_chunk_num(zicio->local_fds, zicio->nr_local_fd);
		zicio_ch_info->total_nr_chunk += 
			__zicio_get_total_chunk_num(zicio->shareable_fds,
				zicio->nr_shareable_fd);
	} else {
		zicio_ch_info->total_nr_chunk = (zicio->batch_info.nr_notified_pages - 1) /
									 (__ZICIO_LIB_CHUNK_SIZE / KZICIO_PAGE_SIZE) + 1;
	}
}

/**
 * __zicio_calc_time_span - calculate time span
 * @start: start time
 * @end: end time
 *
 * Return time span between start time and end time in nanoseconds.
 */
static inline unsigned long __zicio_calc_time_span(struct timespec *start, 
														struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000000000ULL + 
										(end->tv_nsec - start->tv_nsec);
}

/**
 * __zicio_rdtsc - read tsc value
 *
 * The tsc value is used to figure out how long it takes to consume chunk.
 */
static inline unsigned long __zicio_rdtsc(void)
{
	unsigned hi, lo;
	__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long)lo) | (((unsigned long)hi) << 32);
}

/**
 * __zicio_calc_ema - calculate new average tsc delta
 * @ema: recent EMA
 * @new_val: newly added value
 *
 * Use EMA to find the average time difference.
 *
 * Here we use the "e" as 1/16, it can be changed in later.
 * EMA(t) = val(t) * (1/16) + (1 - 1/16) * EMA(t-1)
 */
static inline unsigned long
__zicio_calc_ema(unsigned long ema, unsigned long new_val)
{
	return ((new_val << 7) + 1920 * ema) >> 11;
}

/**
 * __zicio_calc_bitmap_ulong_idx - calculate the index of word in a bitmap
 * @bit: global bit index
 *
 * Bitmap is composed of "unsigned long" type.
 * Return the index of "unsigned long" the given bit corresponds to.
 */
static inline int
__zicio_calc_bitmap_word_idx(int bit_idx_in_bitmap)
{
	return __SB_BITMAP_WORD(bit_idx_in_bitmap);
}

/**
 * __zicio_calc_bit_idx_in_ulong - calculate the bit index in a word
 * @bit: global bit index
 *
 * Return the index of bit in a "unsigned long" word.
 */
static inline int
__zicio_calc_bit_idx_in_word(int bit_idx_in_bitmap)
{
	return (bit_idx_in_bitmap & __SB_BITMAP_WORD_MASK);
}

/**
 * __zicio_calc_bitmask - calculate bitmask which is set from start to end
 * @start: start bit index in a word
 * @end: end bit index in a word
 *
 * This function uses the formula of the sum of geometric sequence.
 * 
 * For example, if the start is 0 and end is 3, we want to make a bitmask like
 * "11110000...000".
 *
 * This value is 64bit which represents one word in a bitmap and this is the
 * following value.
 *
 * 2^63 + 2^62 + 2^61 + 2^60 = 2^60 * (2^4 - 1)
 * 
 * Here, (__SB_BITMAP_WORD_LAST_BIT_IDX - end) is 60, and bitcount is 4 in the
 * below function.
 */
static inline unsigned long
__zicio_calc_bitmask(int start_bit_idx_in_word,
	int end_bit_idx_in_word)
{
	int bitcount = end_bit_idx_in_word - start_bit_idx_in_word + 1;
	return (1UL << (__SB_BITMAP_WORD_LAST_BIT_IDX - end_bit_idx_in_word)) *
		((1UL << bitcount) - 1);
}

/**
 * __zicio_calc_chunk_id - calculate chunk id using the given counter
 * @counter: determines the chunk id
 *
 * The kernel and libzicio use fetch_add() on ther counter, and the value 
 * determines the chunk location.
 *
 * Therefore, knowing the counter value, we can get the chunk id using modular
 * operation.
 */
static inline int
__zicio_calc_chunk_id(unsigned long counter, bool sharing_mode)
{
	if (sharing_mode)
		return (counter & __ZICIO_LIB_MASK_MAX_NUM_SHARING_CHUNK);
	else
		return (counter & __ZICIO_LIB_MASK_MAX_NUM_CHUNK);
}

static inline void
zicio_set_user_buffer_idx(zicio_switch_board *sb, int val)
{
    atomic_store((int *) &sb->user_buffer_idx, val);
}

/**
 * zicio_get_page - advance to the next chunk
 * @zicio: zicio interface
 *
 * Check the switch board managed in this file to get the next chunk.
 * Then return its address.
 *
 * If we cannot move to the next chunk, set the page_addr to NULL.
 *
 * Return chunk address.
 */
void zicio_get_page(struct zicio *zicio)
{
	struct zicio_switch_board *sb;
	int bit_idx_in_bitmap, chunk_id;
	struct zicio_channel_info *zicio_ch_info;
	unsigned long read_page_size;
	unsigned long get_chunk_offset, put_chunk_offset;
	int chunk_status;

	/* There is no channel */
	if (zicio->zicio_id < 0) {
		zicio->get_status = ZICIO_GET_PAGE_NO_CHANNEL;
		return;
	}

	/* Invalid channel index */
	if (zicio->zicio_channel_idx < 0) {
		zicio->get_status = ZICIO_GET_PAGE_INVALID_CH_IDX;
		return;
	}

	mb();

	zicio_ch_info = zicio_channel_vector_get(zicio->zicio_channel_idx);
	assert(zicio_ch_info != NULL && zicio_ch_info->sb != NULL);
	sb = zicio_ch_info->sb;
	assert(sb != NULL);

	read_page_size = zicio_ch_info->read_page_size;
	get_chunk_offset = zicio_ch_info->get_chunk_offset;
	put_chunk_offset = zicio_ch_info->put_chunk_offset;

	assert(put_chunk_offset <= get_chunk_offset);

	if (likely(zicio->chunk_start_addr)) {
	  /*
		 * In here, we have a chunk for reading. We need to check end of chunk.
		 */

		if (unlikely(__ZICIO_LIB_CHUNK_SIZE <= get_chunk_offset || 
		  get_chunk_offset == put_chunk_offset + read_page_size)) {
		  	/* end of chunk */
			zicio->get_status = ZICIO_GET_PAGE_NEED_PUT_PAGE;
			return;
		} else {
		  	assert(get_chunk_offset == put_chunk_offset);
			zicio->page_addr = 
				GET_READ_CHUNK(zicio->chunk_start_addr, get_chunk_offset);
			zicio_ch_info->get_chunk_offset += read_page_size;

			zicio->get_status = ZICIO_GET_PAGE_SUCCESS;
			zicio->put_status = ZICIO_PUT_PAGE_INIT;
			return;
		}
	}

	/*
	 * Can we move to the next chunk?
	 * 
	 * To identify it, we need to check the @requested_chunks bitmap and the
	 * @produced_chunks bitmap in the switch board.
	 *
	 * If the bits corresponding to the next chunk are turned on in both
	 * bitmaps, we can move to that chunk.
	 */
	bit_idx_in_bitmap = 
        __zicio_calc_chunk_id(sb->consumed, zicio_ch_info->sharing_mode);

	/* Get chunk status */
	chunk_status = zicio_read_status(sb, bit_idx_in_bitmap);
 		
	/*
	 * Is the next chunk ready?
	 * If so, change the stat to say it's in use.
	 *
	 * To avoid atomic instruction as much as possible,
	 * check the ready state first.
	 */
	if (chunk_status == ENTRY_READY) {
		if (zicio_cas_status(sb, bit_idx_in_bitmap,
				ENTRY_READY, ENTRY_INUSE)) {
			/* 
			 * Fail: READY -> DONE by kernel
			 *
			 * If user gets entry_done status of chunk, the kernel change its value
			 * on forceful unmapping. So, we move to next chunk.
			 */
			sb->consumed++;
			zicio->get_status = ZICIO_GET_PAGE_NOT_PRODUCED;

			return;
		}
	} else if (zicio_ch_info->sharing_mode && chunk_status == ENTRY_DONE) {
		/* 
		 * If user gets entry_done status of chunk, the kernel change its value
		 * on forceful unmapping. So, we move to next chunk.
		 */
		sb->consumed++;
		zicio->get_status = ZICIO_GET_PAGE_NOT_PRODUCED;

		return;
	} else {
		/* If zicio is local and last round to read chunk, see next chunk */
		if (!zicio_ch_info->sharing_mode && zicio->batch_info.batches_array == NULL &&
				(zicio_ch_info->total_nr_chunk - zicio_ch_info->cur_nr_chunk <=
				__ZICIO_LIB_MASK_MAX_NUM_CHUNK)) {
			sb->consumed += 1;
		}

		if (zicio_ch_info->wait_begin == false) {
			/* The first time the user cannot get a chunk. */
			clock_gettime(CLOCK_MONOTONIC, &zicio_ch_info->wait_begin_time);
			zicio_ch_info->wait_begin = true;

			/* Yield to kernel */
			sched_yield();
		}
		zicio->get_status = ZICIO_GET_PAGE_NOT_PRODUCED;
		return;
	}

	/*
	 * CAS for status modification was succeszicioul: READY -> INUSE 
	 */
	if (zicio_ch_info->wait_begin == true) {
		unsigned long wait_time_span_ns;
		struct timespec wait_end_time;

		clock_gettime(CLOCK_MONOTONIC, &wait_end_time);

		wait_time_span_ns = 
			__zicio_calc_time_span(&zicio_ch_info->wait_begin_time,
														&wait_end_time);

		zicio_ch_info->min_wait_time_ns = 
			MIN(zicio_ch_info->min_wait_time_ns, wait_time_span_ns);
		zicio_ch_info->max_wait_time_ns = 
			MAX(zicio_ch_info->max_wait_time_ns, wait_time_span_ns);
		zicio_ch_info->total_wait_time_ns += wait_time_span_ns;
		zicio_ch_info->wait_begin = false;
	}

	mb();

	/*
	 * The kernel sent a request to this chunk, and the data was also filled.
	 * Consumed it.
	 */
	chunk_id = 
  	__zicio_calc_chunk_id(sb->consumed, zicio_ch_info->sharing_mode);
	zicio->chunk_start_addr = 
		(void *)(sb->data_buffer + chunk_id * __ZICIO_LIB_CHUNK_SIZE);
	zicio->page_addr = zicio->chunk_start_addr;
	zicio_ch_info->get_chunk_offset = zicio_ch_info->read_page_size;
	zicio_ch_info->put_chunk_offset = 0;
	zicio_ch_info->cur_nr_chunk += 1;

	zicio_set_user_buffer_idx(sb, chunk_id);
	sb->consumed++;
	sb->nr_consumed_chunk++;

	zicio_ch_info->cur_read_bytes =
		(unsigned long)zicio_read_bytes(sb, chunk_id);

	/*
	 * Note that we have to initialize put_status here.
	 *
	 * Otherwise, the user may mistakenly believe that they currently put the
	 * current chunk.
	 */
	zicio->get_status = ZICIO_GET_PAGE_SUCCESS;
	zicio->put_status = ZICIO_PUT_PAGE_INIT;

	/* Remember when we start consuming this chunk */
	zicio_ch_info->begin_tsc = __zicio_rdtsc();
}

/**
 * zicio_put_page - release the chunk
 * @zicio: zicio interface
 *
 * If the user has consumed the chunk, this function should be called.
 *
 * To synchronize this function with zicio_get_next_chunk_id() in
 * zicio/zicio_firehose_ctrl.c, the following sequence must be performed.
 *
 * 1. Set the status bit to DONE in switch board.
 * 2. Turn off the bit in @requested_chunks bitmap in switch board.
 *
 * Be sure this order!
 * Otherwise, the kernel can enter this bit in interim, then the @produced bit
 * cannot be trusted.
 */
void zicio_put_page(struct zicio *zicio)
{
	unsigned long delta, end_tsc;
	int bit_idx_in_bitmap, chunk_id;
	struct zicio_channel_info *zicio_ch_info;
	struct zicio_switch_board *sb;
	unsigned long begin_tsc;
	unsigned long read_page_size;
	unsigned long get_chunk_offset, put_chunk_offset;

	/* There is no channel */
	if (zicio->zicio_id < 0) {
		zicio->put_status = ZICIO_PUT_PAGE_NO_CHANNEL;
		return;
	}

	/* There is no chunk */
	if (zicio->chunk_start_addr == NULL) {
		zicio->put_status = ZICIO_PUT_PAGE_NO_CHUNK;
		return;
	}

	/* Invalid channel index */
	if (zicio->zicio_channel_idx < 0) {
		zicio->put_status = ZICIO_PUT_PAGE_INVALID_CH_IDX;
		return;
	}

	mb();

	zicio_ch_info = zicio_channel_vector_get(zicio->zicio_channel_idx);
	assert(zicio_ch_info != NULL && zicio_ch_info->sb != NULL);

	read_page_size = zicio_ch_info->read_page_size;
	get_chunk_offset = zicio_ch_info->get_chunk_offset;
	put_chunk_offset = zicio_ch_info->put_chunk_offset;

	assert(put_chunk_offset <= get_chunk_offset);

	if (unlikely(put_chunk_offset == get_chunk_offset)) {
		zicio->put_status = ZICIO_PUT_PAGE_NO_CHUNK;
		return;
	}

	zicio_ch_info->put_chunk_offset += read_page_size;
	assert(zicio_ch_info->put_chunk_offset == get_chunk_offset);

	mb();

	sb = zicio_ch_info->sb;
	begin_tsc = zicio_ch_info->begin_tsc;

	/* To initialize bit in bitmap, get the chunk id */
	assert((unsigned long)zicio->chunk_start_addr >= sb->data_buffer);
	chunk_id = ((unsigned long)zicio->chunk_start_addr - sb->data_buffer) >>
		__ZICIO_LIB_CHUNK_SIZE_SHIFT;

	if (likely(zicio_ch_info->put_chunk_offset < zicio_ch_info->cur_read_bytes)) {
		zicio->get_status = ZICIO_GET_PAGE_INIT;
		zicio->put_status = ZICIO_PUT_PAGE_SUCCESS;
		return;
	}

	/*
	 * We totally consumed chunk.
	 * Update stat by comparing current time to start time of consumption.
	 */
	end_tsc = __zicio_rdtsc();
	assert(end_tsc >= begin_tsc);
	delta = end_tsc - begin_tsc;
	sb->avg_tsc_delta = __zicio_calc_ema(sb->avg_tsc_delta, delta);

	/* Turn off the bit corresponding to this chunk */
	bit_idx_in_bitmap = chunk_id;

	/*
	 * Set the status bits to DONE in switch board.
	 */
	assert(zicio_read_status(sb, bit_idx_in_bitmap) == ENTRY_INUSE);
	zicio_set_status(sb, bit_idx_in_bitmap, ENTRY_DONE);

	mb();

	/*
	 * The chunk is succeszicioully released.
	 *
	 * Note that we have to initialize get_status here!!!
	 * Otherwise, the user may mistakenly believe that they currently have the
	 * next chunk!!!
	 */
	zicio->put_status = ZICIO_PUT_PAGE_SUCCESS;
	zicio->get_status = ZICIO_GET_PAGE_INIT;
	zicio_ch_info->get_chunk_offset = 0;
	zicio_ch_info->put_chunk_offset = 0;
	zicio->page_addr = NULL;
	zicio->chunk_start_addr = NULL;

	/* 
	 * If we put last chunk of channel stream, this channel is done for data
	 * ingest.
	 */
	if (zicio_ch_info->cur_nr_chunk == zicio_ch_info->total_nr_chunk) {
		zicio->put_status = ZICIO_PUT_PAGE_EOF;
		if (zicio_ch_info->sharing_mode) {
			syscall(555, zicio->zicio_id);
		}
	}
}

/**
 * zicio_create_pool - create new zicio shared pool
 * @zicio: zicio interface
 *
 * Call zicio_u_open(), and remember the result to the status.
 * The shared pool key will be set to the pointer of shared pool key.
 */
void zicio_create_pool(struct zicio *zicio)
{
	long ret;

	/* Checking parameter for shared pool */
	if (zicio->zicio_shared_pool_key || !zicio->nr_shareable_fd || !zicio->shareable_fds) {
		printf("To create a shared pool shared pool key pointer, fds, num of fd"
					" is needed.\n");
		return;
	}

	/* Use systemcall: zicio_u_open() */
	ret = syscall(551, zicio->shareable_fds, zicio->nr_shareable_fd);

	zicio->zicio_shared_pool_key = ret;

	if (ret <= 0) {
		zicio->open_status = ZICIO_SHARED_POOL_INIT_ERROR;
	} else {
		zicio->open_status = ZICIO_OPEN_SUCCESS;
	}
}

/**
 * zicio_destroy_pool - destroy zicio shared pool
 *
 * Call zicio_u_shared_pool_close syscall
 */
long zicio_destroy_pool(zicio_shared_pool_key_t zicio_shared_pool_key)
{
	long ret = syscall(553, zicio_shared_pool_key);

	return ret;
}

/**
 * zicio_open - open new zicio
 * @zicio: zicio interface
 *
 * Call zicio_u_open(), and remember the result to the status.
 */
void zicio_open(struct zicio *zicio)
{
	struct zicio_switch_board *sb = NULL;
	struct zicio_stat_board *stat_board = NULL;
	struct zicio_args zicio_args;
	void *data_buffer;
	unsigned long switch_board_ptr = 0;
	unsigned long stat_board_ptr = 0;
	zicio_shared_pool_key_t zicio_shared_pool_key = zicio->zicio_shared_pool_key;
	struct zicio_batch_info *batch_info = &(zicio->batch_info);
	long ret, ch_idx;
	int errno = 0;
	bool sharing_mode = false;

	if ((__ZICIO_LIB_CHUNK_SIZE % zicio->read_page_size) ||
	  	zicio->read_page_size < MIN_READ_CHUNK_SIZE ||
	  	zicio->read_page_size > MAX_READ_CHUNK_SIZE) {
		printf("error in read chunk size setting: %lu\n", zicio->read_page_size);
		return;
	}

	ret = zicio_channel_vector_init();
	if (ret == ZICIO_OPEN_CH_VEC_INIT_ERROR)
		zicio->open_status = ZICIO_OPEN_CH_VEC_INIT_ERROR;

	errno = posix_memalign(&data_buffer, 1UL << 30, 1UL << 30);

	if (errno) {
		printf("error in posix_memalign\n");
		return;
	}

	errno = madvise(data_buffer, 1UL << 30, MADV_HUGEPAGE);

	if (errno) {
		printf("error in madvise\n");
		return;
	}

	memset(&zicio_args, 0, sizeof(zicio_args));

	/* Local data ingestion setting */
	zicio_args.local_fds = zicio->local_fds;
	zicio_args.nr_local_fd = zicio->nr_local_fd;

	/* Sharable data ingestion setting */
	zicio_args.shareable_fds = zicio->shareable_fds;
	zicio_args.nr_shareable_fd = zicio->nr_shareable_fd;
	zicio_args.zicio_shared_pool_key = &zicio_shared_pool_key;

	/* Switchboard and data buffer address setting */
	zicio_args.switch_board_addr = &switch_board_ptr;
	zicio_args.stat_board_addr = &stat_board_ptr;
	zicio_args.user_base_address = (unsigned long)data_buffer;

	if (batch_info->batches_array != NULL) {
		zicio_args.batches_array = batch_info->batches_array;
		zicio_args.nr_batches = batch_info->nr_batches;
	}

	/* Use systemcall: zicio_u_open() */
	ret = syscall(548, &zicio_args);
	assert(zicio_args.switch_board_addr != 0 && zicio_args.stat_board_addr != 0);

	if (batch_info->batches_array != NULL)
		zicio_batch_info_free(batch_info, zicio->nr_local_fd);

	/* Store the switch board pointer in a static variable in this source */
	sb = (struct zicio_switch_board *)switch_board_ptr;

	/* Store the stat board pointer in a static variable in this source */
	stat_board = (struct zicio_stat_board *)stat_board_ptr;

	if (ret >= 0) {
		assert(zicio->zicio_channel_idx == -1);
		ch_idx = zicio_channel_vector_push_back(sb, stat_board, ret);
		if (ch_idx >= 0) {
			zicio->open_status = ZICIO_OPEN_SUCCESS;
			zicio->zicio_id = ret;
			zicio->zicio_channel_idx = ch_idx;

			if (zicio->nr_shareable_fd > 0 && zicio->shareable_fds != NULL &&
				zicio_args.zicio_shared_pool_key) {
				sharing_mode = true;
			}

			/* Init zicio channel info */
			zicio_channel_info_init(zicio, sharing_mode);
		} else {
			zicio->open_status = ZICIO_OPEN_CH_VEC_PUSH_BACK_ERROR;
			zicio_close(zicio);
		}
	} else if (ret == -1) {
		zicio->open_status = ZICIO_OPEN_COPY_ERROR;
	} else if (ret == -2) {
		zicio->open_status = ZICIO_OPEN_USER_VM_ERROR;
	} else if (ret == -3) {
		zicio->open_status = ZICIO_OPEN_AFFINITY_ERROR;
	} else {
		zicio->open_status = ZICIO_OPEN_ETC_ERROR;
	}
}

unsigned long zicio_get_current_avg_tsc(struct zicio *zicio)
{
	struct zicio_channel_info *zicio_ch_info;
	struct zicio_switch_board *sb;

	zicio_ch_info = zicio_channel_vector_get(zicio->zicio_channel_idx);
	sb = zicio_ch_info->sb;

	return sb->avg_tsc_delta;
}

/**
 * zicio_close - close zicio
 * @zicio: zicio interface
 *
 * Call zicio_u_close(), and remember the result to the status.
 */
void zicio_close(struct zicio *zicio)
{
	long ret;
	struct zicio_channel_info *zicio_ch_info;
	struct zicio_switch_board *sb;
	void *data_buffer;
	// int errno = 0;
	
	/* Isn't the zicio channel open yet? */ 
	if (zicio->zicio_id < 0) {
		zicio->close_status = ZICIO_CLOSE_NO_CHANNEL;
		return;
	}

#if 0
	/* Is the chunk not returned yet? */
	if (zicio->page_addr != NULL) {
		zicio->close_status = ZICIO_CLOSE_NEED_PUT_PAGE;
		return;
	}
#endif

	/* Is the channel vector not allocated yet? */
	if (zicio_ch_vec == NULL) {
		zicio->close_status = ZICIO_CLOSE_CH_VEC_NULL;
		return;
	}

	/* Get data buffer address */
	zicio_ch_info = zicio_channel_vector_get(zicio->zicio_channel_idx);
	sb = zicio_ch_info->sb;
	data_buffer = (void *) sb->data_buffer;

	/* Use systemcall: zicio_u_close() */
	ret = syscall(549, zicio->zicio_id);
	
	if (ret == 0) {
		assert(zicio->zicio_channel_idx >= 0);
		zicio_channel_vector_erase(zicio->zicio_channel_idx);
		zicio_init(zicio);
		zicio->close_status = ZICIO_CLOSE_SUCCESS;
	
		/* Remove hugepage flag */
		madvise(data_buffer, 1UL << 30, MADV_NOHUGEPAGE);

		/* Free */
		free(data_buffer);

		if (zicio_ch_vec->size == 0)
			zicio_channel_vector_free();
	} else {
		zicio->close_status = ZICIO_CLOSE_ETC_ERROR;
	}
}

/**
 * zicio_init - initialize zicio data structure
 * @zicio: zicio interface
 *
 * Initialize variables in zicio
 */
void zicio_init(struct zicio *zicio)
{
	zicio->local_fds = NULL;
	zicio->shareable_fds = NULL;
	zicio->nr_local_fd = 0;
	zicio->nr_shareable_fd = 0;
	zicio->get_status = ZICIO_GET_PAGE_INIT;
	zicio->put_status = ZICIO_PUT_PAGE_INIT;
	zicio->open_status = ZICIO_OPEN_INIT;
	zicio->close_status = ZICIO_CLOSE_INIT;
	zicio->page_addr = NULL;
	zicio->chunk_start_addr = NULL;
	zicio->zicio_id = -1;
	zicio->zicio_channel_idx = -1;
	zicio->zicio_flag = 0;
	zicio->read_page_size = __ZICIO_LIB_CHUNK_SIZE; /* 2 MiB default */

	memset(zicio->zicio_input_msg, 0, MESSAGE_SIZE);

	zicio_batch_info_init(&(zicio->batch_info));
}

/**
 * zicio_get_wait_time
 */
unsigned long zicio_get_wait_time(struct zicio *zicio) 
{
	struct zicio_channel_info *zicio_ch_info;

	zicio_ch_info = zicio_channel_vector_get(zicio->zicio_channel_idx);

	return zicio_ch_info->total_wait_time_ns;
}

zicio_sharing_result zicio_get_sharing_result(struct zicio *zicio)
{
	zicio_sharing_result ret = { 0, };
#if 0
	struct zicio_channel_info *zicio_ch_info;
	struct zicio_stat_board *stat_board;

	zicio_ch_info = zicio_channel_vector_get(zicio->zicio_channel_idx);
	stat_board = zicio_ch_info->stat_board;
	
	if (stat_board != NULL) {
		ret.derailed_io_bytes = stat_board->derailed_io_bytes;
		ret.contributed_io_bytes = stat_board->contributed_io_bytes;
	}
#endif
	return ret;
}

/**
 * zicio_set_input_msg
 */
void zicio_set_input_msg(struct zicio *zicio, char *input) 
{
	if (zicio->zicio_input_msg[0] != 0) {
		fprintf(stderr, "ZicIO input message already exists\n");
		return;
	}

	memcpy(zicio->zicio_input_msg, input, sizeof(char) * MESSAGE_SIZE);
}

/**
 * zicio_get_stat_msg 
 *
 * Return character array need to free
 */
char *zicio_get_stat_msg(struct zicio *zicio)
{
	char *stat_msg = NULL;
	char *user_stat_msg = NULL;
	char *control_stat_msg = NULL;
	char *sharing_stat_msg = NULL;
	struct zicio_channel_info *zicio_ch_info;
	struct zicio_stat_board *stat_board;
	double total_elapsed_time_s;
	struct timespec now;

	zicio_ch_info = zicio_channel_vector_get(zicio->zicio_channel_idx);
	stat_board = zicio_ch_info->stat_board;

	if (stat_board == NULL)
		goto zicio_get_stat_msg_out;

	stat_msg = (char *) malloc(sizeof(char) * OUTPUT_MESSAGE_SIZE);
	user_stat_msg = (char *) malloc(sizeof(char) * MESSAGE_SIZE);
	control_stat_msg = (char *) malloc(sizeof(char) * MESSAGE_SIZE);
	sharing_stat_msg = (char *) malloc(sizeof(char) * MESSAGE_SIZE);
	if (stat_msg == NULL || user_stat_msg == NULL || 
	  		control_stat_msg == NULL || sharing_stat_msg == NULL)
		goto zicio_get_stat_msg_out;

	/* Init buffers */
	memset(stat_msg, 0, sizeof(char) * OUTPUT_MESSAGE_SIZE);
	memset(user_stat_msg, 0, sizeof(char) * MESSAGE_SIZE);
	memset(control_stat_msg, 0, sizeof(char) * MESSAGE_SIZE);
	memset(sharing_stat_msg, 0, sizeof(char) * MESSAGE_SIZE);

	strcat(stat_msg, zicio->zicio_input_msg);

	clock_gettime(CLOCK_MONOTONIC, &now);

	/* Calculate total elapsed time */
	total_elapsed_time_s =
		(double) (__zicio_calc_time_span(&zicio_ch_info->zicio_start_time, &now) / 
			1000000ULL) / 1e3;

	/* Make user ingestion/wait time stats */
	sprintf(user_stat_msg, 
		"elapsed_total(s) %.3lf, wait_total(ms) %.3lf, avg(us) %.3lf, min(us) %.3lf, max(us) %.3lf, ",
		total_elapsed_time_s,
		zicio_ch_info->total_wait_time_ns / 1e6,
		zicio_ch_info->total_nr_chunk == 0 ? 
			0 : zicio_ch_info->total_wait_time_ns / zicio_ch_info->total_nr_chunk / 1e3,
		zicio_ch_info->min_wait_time_ns / 1e3,
		zicio_ch_info->max_wait_time_ns == UINT64_MAX ? 
			0 :zicio_ch_info->max_wait_time_ns / 1e3);
	strcat(stat_msg, user_stat_msg);

	/* Make control flow stats */
	sprintf(control_stat_msg,
		"soft_irq %lu, io_handler %lu, cpu_idle %lu, io_completion_time(ns) %llu, ",
		stat_board->soft_irq_cnt,
		stat_board->io_interrupt_cnt,
		stat_board->cpu_idle_loop_cnt,
		stat_board->io_completion_time);
	strcat(stat_msg, control_stat_msg);

	sprintf(sharing_stat_msg,
		"read_chunk_num %lu, ", zicio_ch_info->total_nr_chunk);
	strcat(stat_msg, sharing_stat_msg);

zicio_get_stat_msg_out:
	if (user_stat_msg)
		free(user_stat_msg);
	
	if (control_stat_msg)
		free(control_stat_msg);

	if (sharing_stat_msg)
		free(sharing_stat_msg);

	return stat_msg;
}

static __thread unsigned long pread_stat_board_ptr;

void zicio_u_pread_breakdown_start(bool enable_breakdown)
{
	zicio_pread_stat_board *pread_stat
		= (zicio_pread_stat_board *)pread_stat_board_ptr;

	if (pread_stat != NULL) {
		fprintf(stderr, "[WARNING] zicio_u_pread_breakdown_start() is called before end\n");

		memset(pread_stat, 0, sizeof(zicio_pread_stat_board));
	} else {
		syscall(557, &pread_stat_board_ptr);

		mb();

		pread_stat = (zicio_pread_stat_board *)pread_stat_board_ptr;
	}

	pread_stat->enable_breakdown = enable_breakdown ? 1 : 0;
}

void zicio_enable_pread_breakdown(void)
{
	zicio_pread_stat_board *pread_stat
		= (zicio_pread_stat_board *)pread_stat_board_ptr;

	if (pread_stat == NULL)
		return;

	pread_stat->enable_breakdown = 1;
}

void zicio_disable_pread_breakdown(void)
{
	zicio_pread_stat_board *pread_stat
		= (zicio_pread_stat_board *)pread_stat_board_ptr;

	if (pread_stat == NULL)
		return;

	pread_stat->enable_breakdown = 0;
}

zicio_pread_breakdown_result zicio_u_pread_breakdown_end(void)
{
	zicio_pread_breakdown_result result = { 0, };
	zicio_pread_stat_board *pread_stat
		= (zicio_pread_stat_board *)pread_stat_board_ptr;

	if (pread_stat == NULL) {
		fprintf(stderr, "[WARNING] zicio_u_pread_breakdown_end() is called before start\n");
		return result;
	}

	mb();

	/* change tsc to nanosecond */
	syscall(559);

#if 0
	if (pread_stat->total_pread_count) {
		fprintf(stderr, "pread count: %lld\n",
			pread_stat->total_pread_count);
		fprintf(stderr, "submitted sectors: %lld\n",
			pread_stat->total_nr_submitted_sectors);
		fprintf(stderr, "\n");

		fprintf(stderr, "total pread elapsed time (nsec): %lld\n", pread_stat->total_nsec_pread);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- mode switch from user: %lld\n",
			pread_stat->total_nsec_mode_switch_from_user);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- cond_resched(): %lld\n",
			pread_stat->total_nsec_cond_resched);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- filemap_get_pages(): %lld\n",
			pread_stat->total_nsec_filemap_get_pages);
		fprintf(stderr, "|  |\n");
		fprintf(stderr, "|  |-- filemap_get_read_batch(): %lld\n",
			pread_stat->total_nsec_filemap_get_read_batch);
		fprintf(stderr, "|  |\n");
		fprintf(stderr, "|  |-- page_cache_sync_readahead(): %lld\n",
			pread_stat->total_nsec_page_cache_sync_readahead);
		fprintf(stderr, "|  |  |\n");
		fprintf(stderr, "|  |  -- ondemand_readahead(): %lld\n",
			pread_stat->total_nsec_ondemand_readahead_sync);
		fprintf(stderr, "|  |    |\n");
		fprintf(stderr, "|  |    -- do_page_cache_ra(): %lld\n",
			pread_stat->total_nsec_do_page_cache_ra_sync);
		fprintf(stderr, "|  |\n");
		fprintf(stderr, "|  |-- filemap_create_page(): %lld\n",
			pread_stat->total_nsec_filemap_create_page);
		fprintf(stderr, "|  |\n");
		fprintf(stderr, "|  |-- filemap_readahead(): %lld\n",
			pread_stat->total_nsec_filemap_readahead);
		fprintf(stderr, "|  |  |\n");
		fprintf(stderr, "|  |  -- page_cache_async_readahead(): %lld\n",
			pread_stat->total_nsec_page_cache_async_readahead);
		fprintf(stderr, "|  |    |\n");
		fprintf(stderr, "|  |    -- ondemand_readahead(): %lld\n",
			pread_stat->total_nsec_ondemand_readahead_async);
		fprintf(stderr, "|  |      |\n");
		fprintf(stderr, "|  |      -- do_page_cache_ra(): %lld\n",
			pread_stat->total_nsec_do_page_cache_ra_async);
		fprintf(stderr, "|  |\n");
		fprintf(stderr, "|  -- filemap_update_page(): %lld\n",
			pread_stat->total_nsec_filemap_update_page);
		fprintf(stderr, "|     |\n");
		fprintf(stderr, "|     -- put_and_wait_on_page_locked(): %lld\n",
			pread_stat->total_nsec_put_and_wait_on_page_locked);
		fprintf(stderr, "|     |\n");
		fprintf(stderr, "|     -- __lock_page_async(): %lld\n",
			pread_stat->total_nsec_lock_page_async);
		fprintf(stderr, "|     |\n");
		fprintf(stderr, "|     -- filemap_range_uptodate(): %lld\n",
			pread_stat->total_nsec_filemap_range_uptodate);
		fprintf(stderr, "|     |\n");
		fprintf(stderr, "|     -- filemap_read_page(): %lld\n",
			pread_stat->total_nsec_filemap_read_page);
		fprintf(stderr, "|         |\n");
		fprintf(stderr, "|         -- ext4_readpage(): %lld\n",
			pread_stat->total_nsec_ext4_readpage);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- copy_page_to_iter(): %lld\n",
			pread_stat->total_nsec_copy_page_to_iter);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- mark_page_accessed(): %lld\n",
			pread_stat->total_nsec_mark_page_accessed);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- flush_dcache_page(): %lld\n",
			pread_stat->total_nsec_flush_dcache_page);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- put_page(): %lld\n",
			pread_stat->total_nsec_put_page);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- mode switch from kernel: %lld\n",
			pread_stat->total_nsec_mode_switch_from_kernel);

		fprintf(stderr, "\n");
		fprintf(stderr, "----------------------------------------------\n");
		fprintf(stderr, "\n");

		fprintf(stderr, "total do_page_cache_ra(): %lld (sync + async)\n",
			pread_stat->total_nsec_do_page_cache_ra_sync
			+ pread_stat->total_nsec_do_page_cache_ra_async);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- __page_cache_alloc(): %lld\n",
			pread_stat->total_nsec_page_cache_alloc);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- add_to_page_cache_lru(): %lld\n",
			pread_stat->total_nsec_add_to_page_cache_lru);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- read_pages(): %lld\n",
			pread_stat->total_nsec_read_pages);
		fprintf(stderr, "   |\n");
		fprintf(stderr, "   -- blk_start_plug(): %lld\n",
			pread_stat->total_nsec_blk_start_plug);
		fprintf(stderr, "   |\n");
		fprintf(stderr, "   -- ext4_readahead(): %lld\n",
			pread_stat->total_nsec_ext4_readahead);
		fprintf(stderr, "   |\n");
		fprintf(stderr, "   -- blk_finish_plug(): %lld\n",
			pread_stat->total_nsec_blk_finish_plug);

		fprintf(stderr, "\n");
		fprintf(stderr, "----------------------------------------------\n");
		fprintf(stderr, "\n");

		fprintf(stderr, "ext4_mpage_readpages(): %lld\n",
			pread_stat->total_nsec_ext4_mpage_readpages);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- ext4_map_blocks(): %lld\n",
			pread_stat->total_nsec_ext4_map_blocks);
		fprintf(stderr, "|\n");
		fprintf(stderr, "-- submit_bio(): %lld\n",
			pread_stat->total_nsec_submit_bio);

		fprintf(stderr, "\n");
		fprintf(stderr, "----------------------------------------------\n");
		fprintf(stderr, "\n");

		fprintf(stderr, "storage device: %lld\n",
			pread_stat->total_nsec_device);

		fprintf(stderr, "\n");
		fprintf(stderr, "----------------------------------------------\n");
		fprintf(stderr, "\n");

		fprintf(stderr, "O_DIRECT, __iomap_dio_rw(), wait_for_completion: %lld\n",
			pread_stat->total_nsec_dio_wait_for_completion);
	}
#endif

	if (pread_stat->total_pread_count) {
		result.mode_switch_latency
			= pread_stat->total_nsec_mode_switch_from_user
				+ pread_stat->total_nsec_mode_switch_from_kernel;

		result.data_copy_latency
			= pread_stat->total_nsec_copy_page_to_iter;

		result.physical_io_latency
			= pread_stat->total_nsec_filemap_update_page
				+ pread_stat->total_nsec_dio_wait_for_completion;

		result.storage_stacks_latency
			= pread_stat->total_nsec_pread
				- result.mode_switch_latency
				- result.data_copy_latency
				- result.physical_io_latency;

		result.io_bytes
			= pread_stat->total_nr_submitted_sectors * 512;

		result.total_nsec
			= pread_stat->total_tsc_pread;
	}

	mb();

	/* release resources */
	syscall(558);

	pread_stat_board_ptr = 0;

	return result;
}

ssize_t zicio_pread(int fd, void *buf, size_t count, off_t pos)
{
	unsigned long long pread_start_tsc, pread_end_tsc;
	zicio_pread_stat_board *pread_stat
		= (zicio_pread_stat_board *)pread_stat_board_ptr;
	ssize_t ret;

	if (pread_stat == NULL || pread_stat->enable_breakdown == 0)
		return pread(fd, buf, count, pos);

	pread_start_tsc = __zicio_rdtsc(); 

	pread_stat->start_tsc_mode_switch_from_user = pread_start_tsc;

	ret = pread(fd, buf, count, pos);

	pread_end_tsc = __zicio_rdtsc();

	pread_stat->total_tsc_mode_switch_from_kernel
		+= (pread_end_tsc - pread_stat->start_tsc_mode_switch_from_kernel);

	pread_stat->total_pread_count++;

	pread_stat->total_tsc_pread += (pread_end_tsc - pread_start_tsc);

	return ret;
}

ssize_t zicio_read(int fd, void *buf, size_t count)
{
	unsigned long long pread_start_tsc, pread_end_tsc;
	zicio_pread_stat_board *pread_stat
		= (zicio_pread_stat_board *)pread_stat_board_ptr;
	ssize_t ret;

	if (pread_stat == NULL || pread_stat->enable_breakdown == 0)
		return read(fd, buf, count);

	pread_start_tsc = __zicio_rdtsc(); 

	pread_stat->start_tsc_mode_switch_from_user = pread_start_tsc;

	ret = read(fd, buf, count);

	pread_end_tsc = __zicio_rdtsc();

	pread_stat->total_tsc_mode_switch_from_kernel
		+= (pread_end_tsc - pread_stat->start_tsc_mode_switch_from_kernel);

	pread_stat->total_pread_count++;

	pread_stat->total_tsc_pread += (pread_end_tsc - pread_start_tsc);

	return ret;
}
