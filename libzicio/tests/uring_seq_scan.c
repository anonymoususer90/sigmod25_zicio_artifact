#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sched.h>
#include <pthread.h>
#include <liburing.h>
#include <string.h>

/* include zicio library */
#include <libzicio.h>

/* memory barrier */
#define mb() __asm__ __volatile__("":::"memory")

#define MAX_NUM_FD	(64)
#define NUM_FILES	(8)
#define FILE_SHIFT	(31)
#define FILE_SIZE	(5 * ((unsigned long long)1 << FILE_SHIFT)) // 10 GiB

#define PAGE_SHIFT	(12)
#define PAGE_SIZE	(1 << PAGE_SHIFT) // 4 KiB

#define PAGES_PER_FILE	((unsigned long long)(FILE_SIZE) / \
						 (unsigned long long)(PAGE_SIZE))

#define PAGES_PER_ZICIO_CHUNK	((unsigned long long)(ZICIO_CHUNK_SIZE) / \
								 (unsigned long long)(PAGE_SIZE))

#define QUEUE_DEPTH	(512)

#define print_error(err_msg) \
	print_error_internal(err_msg, __FILE__, __LINE__)

unsigned long long BUFFER_PAGE_SHIFT = 21;
unsigned long long BUFFER_PAGE_SIZE;

unsigned long long PAGES_PER_BUFFER;

struct uring_data {
	int *fds;
	int nr_fd;
	int cur_fd_idx;
	int cur_ingested_page_cnt;
	int nr_page;
	char *page_addr;

	struct io_uring ring;
	struct io_uring_params params;

	int cur_ingested_buffer_cnt;
	char *buffers[2];
	int prefetched[2];
};

unsigned long long *page_nums;
int accumulated_nr_pages[NUM_FILES]; // start from 1
unsigned long long burning_loop_cnt;

unsigned long total_wait_time_ns;
unsigned long total_ingestion_time_ns;
unsigned long total_submission_time_ns;
unsigned long total_submission_cnt;

#pragma GCC push_options
#pragma GCC optimize ("O0")

static void burn_cpu() {
	volatile unsigned long long cnt = 0;
	for (unsigned long long i = 0; i < burning_loop_cnt; i++) {
		cnt++;
	}
}

#pragma GCC pop_options

static inline void print_error_internal(const char* err_msg,
										char *file, int lineno) 
{
	fprintf(stderr, "[ERROR] \"%s\" at %s:%d\n", err_msg, file, lineno);
}

static unsigned long get_ns_delta(struct timespec start, struct timespec end) {
	unsigned long start_ns = start.tv_sec * 1000000000 + start.tv_nsec;
	unsigned long end_ns = end.tv_sec * 1000000000 + end.tv_nsec;
	return end_ns - start_ns;
}

static void uring_data_init(struct uring_data *ud) {
	memset(ud, 0, sizeof(struct uring_data));
	memset(&ud->params, 0, sizeof(struct io_uring_params));
	posix_memalign((void**)(&ud->buffers[0]), BUFFER_PAGE_SIZE, BUFFER_PAGE_SIZE);
	posix_memalign((void**)(&ud->buffers[1]), BUFFER_PAGE_SIZE, BUFFER_PAGE_SIZE);
}

static int uring_data_open(struct uring_data *ud, unsigned int sq_thread_idle,
						   int sq_polling) {
	int ret;
	struct io_uring_sqe *sqe;
	struct timespec begin_time, end_time;

	if (ud->nr_page == 0) {
		print_error("nr_page of ud is 0");
		return 1;
	}

	if (sq_polling) {
		ud->params.flags |= IORING_SETUP_SQPOLL;
		ud->params.sq_thread_idle = sq_thread_idle;
	}

	ret = io_uring_queue_init_params(QUEUE_DEPTH, &ud->ring, &ud->params);
	if (ret) {
		print_error("Unable to setup io_uring");
		return 1;
	}

	ret = io_uring_register_files(&ud->ring, ud->fds, ud->nr_fd);
	if (ret) {
		print_error("Error registering files");
		return 1;
	}

	sqe = io_uring_get_sqe(&ud->ring);
	if (!sqe) {
		print_error("Could not get sqe");
		return 1;
	}

	io_uring_prep_read(sqe, 0, ud->buffers[0], BUFFER_PAGE_SIZE, 0);
	sqe->flags |= IOSQE_FIXED_FILE;
	sqe->user_data = 0;
	clock_gettime(CLOCK_MONOTONIC, &begin_time);
	io_uring_submit(&ud->ring);
	clock_gettime(CLOCK_MONOTONIC, &end_time);

	total_submission_time_ns += get_ns_delta(begin_time, end_time);
	total_submission_cnt++;

	return 0;
}

static void uring_data_close(struct uring_data *ud) {
	io_uring_queue_exit(&ud->ring);
	free(ud->buffers[0]);
	free(ud->buffers[1]);
}

static inline void set_pages(struct uring_data *ud, int fd, unsigned long file_idx,
							  unsigned long start, unsigned long end,
							  int *nr_page, unsigned long long *page_nums) {
	int i;

	for (i = ud->nr_fd - 1; i >= 0; i--) {
		if (ud->fds[i] == fd)
			break;
	}

	if (i == -1) {
		accumulated_nr_pages[ud->nr_fd] = *nr_page; // start from 1
		ud->fds[(ud->nr_fd++)] = fd;
	}

	for (unsigned long long page = start + file_idx * PAGES_PER_FILE;
		 page <= end + file_idx * PAGES_PER_FILE; page++)
		page_nums[(*nr_page)++] = page;
}

static int uring_data_get_page(struct uring_data *ud) {
	int idx = ud->cur_ingested_page_cnt % PAGES_PER_BUFFER;
	struct timespec begin_time, end_time;

	if (idx == 0) {
		struct io_uring_cqe *cqe = NULL;
		int ret, cur_buffer_idx = ud->cur_ingested_buffer_cnt % 2,
			next_buffer_idx = (~cur_buffer_idx) & 0x1,
			target_page_cnt = ud->cur_ingested_page_cnt + PAGES_PER_BUFFER;

		if (target_page_cnt < ud->nr_page) {
			struct io_uring_sqe *sqe;
			unsigned long long offset;
			unsigned int page_num_diff;
			unsigned int nbytes;

			if (target_page_cnt == accumulated_nr_pages[ud->cur_fd_idx + 1])
				++(ud->cur_fd_idx);

			sqe = io_uring_get_sqe(&ud->ring);
			if (!sqe) {
				print_error("Could not get sqe");
				return 1;
			}

			offset = ((unsigned long long)target_page_cnt -
					  accumulated_nr_pages[ud->cur_fd_idx]) * PAGE_SIZE;
			page_num_diff = ud->nr_page - target_page_cnt;
			nbytes = page_num_diff < (unsigned int)PAGES_PER_BUFFER ?
				page_num_diff * PAGE_SIZE : BUFFER_PAGE_SIZE;

			io_uring_prep_read(sqe, ud->cur_fd_idx,
							   ud->buffers[next_buffer_idx], nbytes, offset);
			sqe->flags |= IOSQE_FIXED_FILE;
			sqe->user_data = next_buffer_idx;
			clock_gettime(CLOCK_MONOTONIC, &begin_time);
			io_uring_submit(&ud->ring);
			clock_gettime(CLOCK_MONOTONIC, &end_time);

			total_submission_time_ns += get_ns_delta(begin_time, end_time);
			total_submission_cnt++;
		}

		mb();

		clock_gettime(CLOCK_MONOTONIC, &begin_time);
		while (!(ud->prefetched[cur_buffer_idx])) {
			ret = io_uring_wait_cqe(&ud->ring, &cqe);
			if (ret) {
				print_error("Could not get cqe");
				return 1;
			}
			ud->prefetched[cqe->user_data] = 1;
			io_uring_cqe_seen(&ud->ring, cqe);
		}
		clock_gettime(CLOCK_MONOTONIC, &end_time);

		total_wait_time_ns += get_ns_delta(begin_time, end_time);
	}

	ud->page_addr =
		ud->buffers[ud->cur_ingested_buffer_cnt % 2] + (idx * PAGE_SIZE);

	return 0;
}

static void uring_data_put_page(struct uring_data *ud) {
	if ((++ud->cur_ingested_page_cnt) % PAGES_PER_BUFFER == 0) {
		ud->prefetched[(ud->cur_ingested_buffer_cnt++) % 2] = 0;
	}
}

/*
 * do_data_ingestion
 *
 * Return 0, success
 * Return -1, error
 */
static int do_data_ingestion(struct uring_data *ud, int nr_page,
							 unsigned long long *page_nums)
{
	const int ull_count_per_page = 
		(PAGE_SIZE / sizeof(unsigned long));
	unsigned long long *arr = NULL;
	unsigned long long page_num;
	int cnt = 0;
	struct timespec begin_time, end_time;

	while (1) {
		uring_data_get_page(ud);
		clock_gettime(CLOCK_MONOTONIC, &begin_time);

		mb();

		assert(ud->page_addr != NULL);
		arr = (unsigned long long *)ud->page_addr;

		for (int ull_idx = 0; ull_idx < ull_count_per_page; ull_idx++) {
			page_num = arr[ull_idx] >> PAGE_SHIFT;
			if (page_num != page_nums[cnt]) {
				print_error("mismatched page_num");
				fprintf(stderr, "expected value: %llu, page_num: %llu, ull_idx: %d, cnt: %d\n", page_nums[cnt], page_num, ull_idx, cnt);
				return -1;
			}

			if (arr[ull_idx] % PAGE_SIZE != ull_idx * sizeof(unsigned long long)) {
				print_error("mismatched value");
				fprintf(stderr, "expected value: %ld, arr[ull_idx]: %llu, ull_idx: %d, cnt: %d\n", ull_idx * sizeof(unsigned long long), arr[ull_idx] % PAGE_SIZE, ull_idx, cnt);
				return -1;
			}
		}
		cnt++;

		burn_cpu();

		mb();

		clock_gettime(CLOCK_MONOTONIC, &end_time);
		total_ingestion_time_ns += get_ns_delta(begin_time, end_time);
		uring_data_put_page(ud);
		if (ud->cur_ingested_page_cnt == ud->nr_page)
			break;
	}

	/* 
	 * If there are other values(i.e. etc) or v1 and v2 have different value or
	 * cnt is different with nr_page, there is error.
	 */
	if (cnt != nr_page)
		return -1;
	else
		return 0;
}

/* 
 * Batched data ingestion test - read 80 GiB from 8 files
 */
int main(int argc, char *args[])
{
	int fds[MAX_NUM_FD];
	int fd;
	struct uring_data ud;
	char *data_path;
	int ret = 0;
	int sq_polling = 1;
	int open_flag = O_RDONLY;
	int nr_page = 0;
	unsigned int sq_thread_idle = 1000; // (ms), default value of liburing

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc < 5 || argc > 7) {
		print_error("the number of parameter is invalid");
		return -1;
	}

	data_path = args[1];

	if (strcmp(args[2], "on") == 0)
		sq_polling = 1;
	else if (strcmp(args[2], "off") == 0) {
		sq_polling = 0;
		if (argc == 7)
			fprintf(stderr, "[WARNING] sq_polling is off, so sq_thread_idle will not be used\n");
	}
	else {
		print_error("sq_polling value is wrong (on|off)");
		return -1;
	}

	if (strcmp(args[3], "on") == 0) {
		open_flag |= O_DIRECT;
	}
	else if (strcmp(args[3], "off") != 0) {
		print_error("os_cache_bypass value is wrong (on|off)");
		return -1;
	}

	burning_loop_cnt = (unsigned long long)atoll(args[4]);	

	if (argc >= 6)
		BUFFER_PAGE_SHIFT = (unsigned int)atoi(args[5]);

	if (argc == 7)
		sq_thread_idle = (unsigned int)atoi(args[6]);

	BUFFER_PAGE_SIZE = 1 << BUFFER_PAGE_SHIFT;
	PAGES_PER_BUFFER = 1 << (BUFFER_PAGE_SHIFT - PAGE_SHIFT);

	/* Init uring_data structure */
	uring_data_init(&ud);

	/* Set values before call open */
	ud.fds = fds;

	page_nums =
		(unsigned long long*)malloc(PAGES_PER_FILE * sizeof(unsigned long long) *
									NUM_FILES);

	/* Open multiple files */
	for (int i = 0; i < NUM_FILES; ++i) {
		char path[256];

		sprintf(path, "%s/data_seq.%d", data_path, i);

		if ((fd = open(path, open_flag)) < 0) {
			print_error("open file failed");
	 		return -1;
		}

		set_pages(&ud, fd, i, 0, PAGES_PER_FILE - 1, &nr_page, page_nums);
	}

	ud.nr_page = nr_page;

	total_ingestion_time_ns = 0;
	total_wait_time_ns = 0;
	total_submission_time_ns = 0;
	total_submission_cnt = 0;

	/* Open uring_data */
	ret = uring_data_open(&ud, sq_thread_idle, sq_polling);
	if (ret) {
	  	print_error("uring_data open fail");
		return -1;
	}

	/* 
	 * Call zicio_get_page() repeatedly in loop 
	 */
	ret = do_data_ingestion(&ud, nr_page, page_nums);

	fprintf(stderr, "total ingestion time(ns): %lu, total wait time(ns): %lu, total submission time(ns): %lu, total submission cnt: %lu\n",
			total_ingestion_time_ns,
			total_wait_time_ns,
			total_submission_time_ns, total_submission_cnt);
	fprintf(stderr, "avg ingestion time(ns): %.3lf, avg wait time(ns): %.3lf, avg submission time(ns): %.3lf\n",
			(float)total_ingestion_time_ns / (float)nr_page,
			(float)total_wait_time_ns / (float)(nr_page / PAGES_PER_BUFFER),
			(float)total_submission_time_ns / (float)total_submission_cnt);

	/* Close uring_data */
	uring_data_close(&ud);

	free(page_nums);
	for (int i = 0; i < NUM_FILES; ++i) {
		close(fds[i]);
	}

	if (ret == 0)
	  return 0;
	else
	  return -1;
}
