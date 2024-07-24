/* SPDX-License-Identifier: MIT */

#ifndef LIB_ZICIO_H
#define LIB_ZICIO_H

#include <sys/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <linux/zicio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef likely
#ifdef __GNUC__
#define likely(x)       __builtin_expect(!!(x), 1)
#else
#define likely(x)       (x)
#endif
#endif

#ifndef unlikely
#ifdef __GNUC__
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define unlikely(x)     (x)
#endif
#endif


//TODO: Move uapi to uapi
#define ZICIO_CHUNK_SIZE (2 * 1024 * 1024ULL)	/* 2 MiB */
#define ZICIO_DATA_BUFFER_SIZE (1 * 1024 * 1024 * 1024ULL) /* 1 GiB */

/* Stat message size */
#define OUTPUT_MESSAGE_SIZE (8192)
#define MESSAGE_SIZE (512)

/**
 * Return values for zicio_open()
 *
 * @ZICIO_OPEN_SUCCESS: new channel is opened.
 *
 * @ZICIO_OPEN_COPY_ERROR: syscall return -1.
 *
 * @ZICIO_OPEN_USER_VM_ERROR: syscall return -2.
 *
 * @ZICIO_OPEN_AFFINITY_ERROR: syscall return -3.
 *
 * @ZICIO_OPEN_CH_VEC_INIT_ERROR: channel vector is NULL.
 *
 * @ZICIO_OPEN_CH_VEC_PUSH_BACK_ERROR: channel vector push back failed.
 *
 * @ZICIO_OPEN_ETC_ERROR: etc
 *
 * @ZICIO_OPEN_INIT: initial state
 *
 * @ZICIO_SHARED_POOL_INIT_ERROR: cannot initialize shared pool
 */
enum zicio_open_status {
	ZICIO_OPEN_SUCCESS = 0,
	ZICIO_OPEN_COPY_ERROR,
	ZICIO_OPEN_USER_VM_ERROR,
	ZICIO_OPEN_AFFINITY_ERROR,
	ZICIO_OPEN_CH_VEC_INIT_ERROR,
	ZICIO_OPEN_CH_VEC_PUSH_BACK_ERROR,
	ZICIO_OPEN_ETC_ERROR,
	ZICIO_OPEN_INIT,
	ZICIO_SHARED_POOL_INIT_ERROR,
};

/**
 * Return values for zicio_close()
 *
 * @ZICIO_CLOSE_SUCCESS: zicio channel is closed.
 *
 * @ZICIO_CLOSE_NO_CHANNEL: there is no channel to close.
 *
 * @ZICIO_CLOSE_NEED_PUT_PAGE: chunk is still remaining.
 *
 * @ZICIO_CLOSE_CH_VEC_NULL: channel vector is null.
 *
 * @ZICIO_CLOSE_ETC_ERROR: etc
 *
 * @ZICIO_CLOSE_INIT: initial state
 */
enum zicio_close_status {
	ZICIO_CLOSE_SUCCESS = 0,
	ZICIO_CLOSE_NO_CHANNEL,
	ZICIO_CLOSE_NEED_PUT_PAGE,
	ZICIO_CLOSE_CH_VEC_NULL,
	ZICIO_CLOSE_ETC_ERROR,
	ZICIO_CLOSE_INIT,
	ZICIO_CLOSE_SHARED_POOL_ERROR,
};

/**
 * Return values for zicio_get_page()
 *
 * @ZICIO_GET_PAGE_SUCCESS: new chunk is acquired

 * @ZICIO_GET_PAGE_NOT_REQUESTED: next chunk is not requested by kernel yet

 * @ZICIO_GET_PAGE_NOT_PRODUCED: next chunk is not produced by kernel yet

 * @ZICIO_GET_PAGE_NEED_PUT_PAGE: user does not put it's chunk yet
 *
 * @ZICIO_GET_PAGE_NO_CHANNEL: there is no zicio channel
 *
 * @ZICIO_GET_PAGE_INVALID_CH_IDX: invalid channel index in zicio_ch_vec.
 *
 * @ZICIO_GET_PAGE_ETC_ERROR: etc
 *
 * @ZICIO_GET_PAGE_INIT: initial state
 */
enum zicio_get_page_status {
	ZICIO_GET_PAGE_SUCCESS = 0,
	ZICIO_GET_PAGE_NOT_REQUESTED,
	ZICIO_GET_PAGE_NOT_PRODUCED,
	ZICIO_GET_PAGE_NEED_PUT_PAGE,
	ZICIO_GET_PAGE_NO_CHANNEL,
	ZICIO_GET_PAGE_INVALID_CH_IDX,
	ZICIO_GET_PAGE_ETC_ERROR,
	ZICIO_GET_PAGE_INIT,
};

/**
 * Return values for zicio_put_page()
 *
 * @ZICIO_PUT_PAGE_SUCCESS: the chunk is released
 *
 * @ZICIO_PUT_PAGE_NO_CHUNK: there is no chunk to release
 *
 * @ZICIO_PUT_PAGE_NO_CHANNEL: there is no zicio channel
 *
 * @ZICIO_PUT_PAGE_INVALID_CH_IDX: invalid channel index in zicio_ch_vec.
 *
 * @ZICIO_PUT_PAGE_EOF: we reached end of file.
 *
 * @ZICIO_PUT_PAGE_ETC_ERROR: etc
 *
 * @ZICIO_PUT_PAGE_INIT: initial state
 */
enum zicio_put_page_status {
	ZICIO_PUT_PAGE_SUCCESS = 0,
	ZICIO_PUT_PAGE_NO_CHUNK,
	ZICIO_PUT_PAGE_NO_CHANNEL,
	ZICIO_PUT_PAGE_INVALID_CH_IDX,
	ZICIO_PUT_PAGE_EOF,
	ZICIO_PUT_PAGE_ETC_ERROR,
	ZICIO_PUT_PAGE_INIT,
};

/**
 * A structure containing information of batches
 * 
 * @nr_notified_pages: the total # of notified pages (4 KiB)
 * @batches_array: 2D array of batch ([file][batch])
 * @nr_batches: how many batches are in batches_array
 * 
 * A single batch is represented by two page numbers.
 * (start page number, end page number)
 * Therefore, the even-numbered values of the array are the start page number of
 * each batch, and the odd-numbered values are the end page number of each
 * batch.
 */
struct zicio_batch_info {
	unsigned long nr_notified_pages;
	unsigned **batches_array;
	unsigned *nr_batches;
};

/**
 * Data structure for users to use libzicio
 *
 * @local_fds: file descriptors the user do not share with others
 * @shareable_fds: file descriptors the user share with others
 * @nr_local_fd: how many fds are in the @local_fds
 * @nr_shareable_fd: how many fds are in the @shareable_fds
 * @chunk_start_addr: currently acquired chunk address
 * @open_status: the result of the last zicio_open()
 * @get_status: the result of the last zicio_get_page()
 * @zicio_id: non-error return value of zicio_open()
 * @zicio_channel_idx: channel index
 * @zicio_flag: options
 * @read_page_size: read data size per get_page()
 * @batch_info: batch_info struct for batching pages
 */
struct zicio {
	int *local_fds;
	int *shareable_fds;
	int	nr_local_fd;
	int nr_shareable_fd;
	zicio_shared_pool_key_t zicio_shared_pool_key;
	enum zicio_get_page_status	get_status;
	enum zicio_put_page_status	put_status;
	enum zicio_open_status	open_status;
	enum zicio_close_status	close_status;
	void *page_addr;
	void *chunk_start_addr;
	int zicio_id;
	int zicio_channel_idx;
	int zicio_flag;
	unsigned long read_page_size;
	struct zicio_batch_info batch_info;
	char zicio_input_msg[MESSAGE_SIZE];
};

typedef struct zicio_pread_breakdown_result {
	unsigned long long mode_switch_latency;
	unsigned long long data_copy_latency;
	unsigned long long storage_stacks_latency;
	unsigned long long physical_io_latency;
	unsigned long long io_bytes;
	unsigned long long total_nsec;
} zicio_pread_breakdown_result;

typedef struct zicio_sharing_result {
	unsigned long long derailed_io_bytes;
	unsigned long long contributed_io_bytes;
} zicio_sharing_result;

extern int zicio_notify_ranges(struct zicio *zicio, int fd, unsigned start,
							   unsigned end);

extern void zicio_open(struct zicio *zicio);

extern void zicio_close(struct zicio *zicio);

extern void zicio_get_page(struct zicio *zicio);

extern void zicio_put_page(struct zicio *zicio);

extern void zicio_init(struct zicio *zicio);

extern void zicio_set_input_msg(struct zicio *zicio, char *input);

extern char *zicio_get_stat_msg(struct zicio *zicio);

extern unsigned long zicio_get_wait_time(struct zicio *zicio);

extern void zicio_create_pool(struct zicio *zicio);

extern long zicio_destroy_pool(zicio_shared_pool_key_t spool_key);

extern unsigned long zicio_get_current_avg_tsc(struct zicio *zicio);

extern void zicio_u_pread_breakdown_start(bool enable_breakdown);

extern zicio_pread_breakdown_result zicio_u_pread_breakdown_end(void);

extern ssize_t zicio_pread(int fd, void *buf, size_t count, off_t pos);

extern ssize_t zicio_read(int fd, void *buf, size_t count);

extern void zicio_enable_pread_breakdown(void);

extern void zicio_disable_pread_breakdown(void);

extern zicio_sharing_result zicio_get_sharing_result(struct zicio *zicio);

#ifdef __cplusplus
}
#endif

#endif /* LIB_ZICIO_H */
