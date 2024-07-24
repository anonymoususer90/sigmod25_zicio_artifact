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

#define print_error(err_msg) \
	print_error_internal(err_msg, __FILE__, __LINE__)

unsigned long long BATCH_SHIFT = 21;
unsigned long long BATCH_SIZE;

unsigned long long PAGES_PER_BATCH;

unsigned long long *page_nums;
unsigned long long burning_loop_cnt;

unsigned long total_wait_time_ns;
unsigned long total_ingestion_time_ns;

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

static inline void set_pages(struct zicio *zicio, int fd, unsigned long file_idx,
							  unsigned long start, unsigned long end,
							  int *nr_page, unsigned long long *page_nums) {
	zicio_notify_ranges(zicio, fd, start, end);
	for (unsigned long long page = start + file_idx * PAGES_PER_FILE;
		 page <= end + file_idx * PAGES_PER_FILE; page++)
		page_nums[(*nr_page)++] = page;
}

/*
 * do_data_ingestion
 *
 * Return 0, success
 * Return -1, error
 */
static int do_data_ingestion(struct zicio *zicio, int nr_page,
							 unsigned long long *page_nums)
{
	const int ull_count_per_page = 
		(PAGE_SIZE / sizeof(unsigned long));
	unsigned long long *arr = NULL;
	unsigned long long page_num;
	int cnt = 0;
	struct timespec begin_time, end_time;

	while (1) {
		while (zicio->get_status != ZICIO_GET_PAGE_SUCCESS) {
			zicio_get_page(zicio);
		}
		clock_gettime(CLOCK_MONOTONIC, &begin_time);

		mb();

		assert(zicio->page_addr != NULL);
		arr = (unsigned long long *)zicio->page_addr;

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
		zicio_put_page(zicio);
		if (zicio->put_status == ZICIO_PUT_PAGE_EOF)
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
	struct zicio zicio;
	char *data_path;
	int ret = 0;
	int nr_page = 0;

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc != 3 && argc != 4) {
			print_error("the number of parameter is invalid");
			return -1;
	}

	/* Init zicio structure */
	zicio_init(&zicio);

	/* Set values before call open */
	zicio.local_fds = fds;
	zicio.shareable_fds = NULL;
	zicio.nr_shareable_fd = 0;
	zicio.zicio_flag = 0;
	zicio.read_page_size = PAGE_SIZE;

	data_path = args[1];
	burning_loop_cnt = (unsigned long long)atoll(args[2]);

	if (argc == 4)
		BATCH_SHIFT = (unsigned int)atoi(args[3]);

	BATCH_SIZE = 1 << BATCH_SHIFT;
	PAGES_PER_BATCH = 1 << (BATCH_SHIFT - PAGE_SHIFT);

	page_nums =
		(unsigned long long*)malloc(PAGES_PER_FILE * sizeof(unsigned long long) *
									NUM_FILES);

	/* Open multiple files */
	for (int i = 0; i < NUM_FILES; ++i) {
		char path[256];

		sprintf(path, "%s/data_seq.%d", data_path, i);

		if ((fd = open(path, O_RDONLY | O_DIRECT)) < 0) {
			print_error("open file failed");
	 		return -1;
		}

		for (unsigned long long j = 0; j < PAGES_PER_FILE;
			 j += (2 * PAGES_PER_BATCH)) {
			set_pages(&zicio, fd, i, j, j + PAGES_PER_BATCH - 1, &nr_page,
					   page_nums);
		}
	}

	/* Open zicio */
	zicio_open(&zicio);
	if (zicio.open_status != ZICIO_OPEN_SUCCESS) {
	  	print_error("zicio open fail");
		return -1;
	}

	/* Check zicio id */
	if (zicio.zicio_id == -1) {
	  	print_error("zicio opened, but zicio id is invalid");
		return -1;
	}

	/* Check zicio channel idx used for user */
	if (zicio.zicio_channel_idx == -1) {
	  	print_error("zicio opened, but channel idx is invalid");
		return -1;
	}

	total_ingestion_time_ns = 0;

	/* 
	 * Call zicio_get_page() repeatedly in loop 
	 */
	ret = do_data_ingestion(&zicio, nr_page, page_nums);

	total_wait_time_ns = zicio_get_wait_time(&zicio);
	fprintf(stderr, "total ingestion time(ns): %lu, total wait time(ns): %lu, total submission time(ns): 0, total submission cnt: 0\n",
			total_ingestion_time_ns,
			total_wait_time_ns);
	fprintf(stderr, "avg ingestion time(ns): %.3lf, avg wait time(ns): %.3lf\n",
			(float)total_ingestion_time_ns / (float)nr_page,
			(float)total_wait_time_ns / (float)(nr_page / PAGES_PER_ZICIO_CHUNK));

	/* Close zicio */
	zicio_close(&zicio);

	/* Check zicio close success */
	if (zicio.close_status != ZICIO_CLOSE_SUCCESS) {
	  	print_error("zicio close fail");
		return -1;
	}

	free(page_nums);
	for (int i = 0; i < NUM_FILES; ++i) {
		close(fds[i]);
	}

	if (ret == 0)
	  return 0;
	else
	  return -1;
}
