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
#include <string.h>

#include <libzicio.h>

/* memory barrier */
#define mb() __asm__ __volatile__("":::"memory")

#define MAX_NUM_FD	(64)
#define NUM_FILES	(2)
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

unsigned long long BUFFER_PAGE_SHIFT = 21;
unsigned long long BUFFER_PAGE_SIZE;

unsigned long long PAGES_PER_BUFFER;

struct pread_data {
	int *fds;
	int nr_fd;
	int cur_fd_idx;
	int cur_ingested_page_cnt;
	int nr_page;
	char *page_addr;

	int cur_ingested_buffer_cnt;
	char *buffer;
	int prefetched[2];
};

unsigned long long *page_nums;
int accumulated_nr_pages[NUM_FILES]; // start from 1
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

static void pread_data_init(struct pread_data *pd) {
	memset(pd, 0, sizeof(struct pread_data));
	posix_memalign((void**)(&pd->buffer), BUFFER_PAGE_SIZE, BUFFER_PAGE_SIZE);
}

static void pread_data_close(struct pread_data *pd) {
	free(pd->buffer);
}

static inline void set_pages(struct pread_data *pd, int fd, unsigned long file_idx,
							  unsigned long start, unsigned long end,
							  int *nr_page, unsigned long long *page_nums) {
	int i;

	for (i = pd->nr_fd - 1; i >= 0; i--) {
		if (pd->fds[i] == fd)
			break;
	}

	if (i == -1) {
		accumulated_nr_pages[pd->nr_fd] = *nr_page; // start from 1
		pd->fds[(pd->nr_fd++)] = fd;
	}

	for (unsigned long long page = start + file_idx * PAGES_PER_FILE;
		 page <= end + file_idx * PAGES_PER_FILE; page++)
		page_nums[(*nr_page)++] = page;
}

static int pread_data_get_page(struct pread_data *pd) {
	int idx = pd->cur_ingested_page_cnt % PAGES_PER_BUFFER;
	struct timespec begin_time, end_time;

	if (idx == 0) {
		unsigned long long offset;
		unsigned int page_num_diff;
		unsigned int nbytes;

		if (pd->cur_ingested_page_cnt == accumulated_nr_pages[pd->cur_fd_idx + 1])
				++(pd->cur_fd_idx);

		offset = ((unsigned long long)(pd->cur_ingested_page_cnt) -
				  accumulated_nr_pages[pd->cur_fd_idx]) * PAGE_SIZE;
		page_num_diff = pd->nr_page - pd->cur_ingested_page_cnt;
		nbytes = page_num_diff < (unsigned int)PAGES_PER_BUFFER ?
			page_num_diff * PAGE_SIZE : BUFFER_PAGE_SIZE;

		clock_gettime(CLOCK_MONOTONIC, &begin_time);
		zicio_pread(pd->fds[pd->cur_fd_idx], pd->buffer, nbytes, offset);
		clock_gettime(CLOCK_MONOTONIC, &end_time);

		total_wait_time_ns += get_ns_delta(begin_time, end_time);
	}

	pd->page_addr = pd->buffer + (idx * PAGE_SIZE);

	return 0;
}

static void pread_data_put_page(struct pread_data *pd) {
	if ((++pd->cur_ingested_page_cnt) % PAGES_PER_BUFFER == 0) {
		pd->prefetched[(pd->cur_ingested_buffer_cnt++) % 2] = 0;
	}
}

/*
 * do_data_ingestion
 *
 * Return 0, success
 * Return -1, error
 */
static int do_data_ingestion(struct pread_data *pd, int nr_page,
							 unsigned long long *page_nums)
{
	const int ull_count_per_page = 
		(PAGE_SIZE / sizeof(unsigned long));
	unsigned long long *arr = NULL;
	unsigned long long page_num;
	int cnt = 0;
	struct timespec begin_time, end_time;
	zicio_pread_breakdown_result pread_result;

	zicio_u_pread_breakdown_start(true);

	while (1) {
		pread_data_get_page(pd);
		clock_gettime(CLOCK_MONOTONIC, &begin_time);

		mb();

		assert(pd->page_addr != NULL);
		arr = (unsigned long long *)pd->page_addr;

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
		pread_data_put_page(pd);
		if (pd->cur_ingested_page_cnt == pd->nr_page)
			break;
	}

	pread_result = zicio_u_pread_breakdown_end();
	fprintf(stderr, "mode_switch(ns, L2) %llu, data_copy(ns, L3) %llu, storage_stacks(ns, L4): %llu, physical_io(ns, L5): %llu\n",
            pread_result.mode_switch_latency, pread_result.data_copy_latency, pread_result.storage_stacks_latency, pread_result.physical_io_latency);

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
	struct pread_data pd;
	char *data_path;
	int ret = 0;
	int open_flag = O_RDONLY;
	int nr_page = 0;

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc < 4 || argc > 5) {
		print_error("the number of parameter is invalid");
		return -1;
	}

	data_path = args[1];

	if (strcmp(args[2], "on") == 0) {
		open_flag |= O_DIRECT;
	}
	else if (strcmp(args[2], "off") != 0) {
		print_error("os_cache_bypass value is wrong (on|off)");
		return -1;
	}

	burning_loop_cnt = (unsigned long long)atoll(args[3]);	

	if (argc >= 5)
		BUFFER_PAGE_SHIFT = (unsigned int)atoi(args[4]);

	BUFFER_PAGE_SIZE = 1 << BUFFER_PAGE_SHIFT;
	PAGES_PER_BUFFER = 1 << (BUFFER_PAGE_SHIFT - PAGE_SHIFT);

	/* Init pread_data structure */
	pread_data_init(&pd);

	/* Set values before call open */
	pd.fds = fds;

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

		set_pages(&pd, fd, i, 0, PAGES_PER_FILE - 1, &nr_page, page_nums);
	}

	pd.nr_page = nr_page;

	total_ingestion_time_ns = 0;
	total_wait_time_ns = 0;

	/* 
	 * Call zicio_get_page() repeatedly in loop 
	 */
	ret = do_data_ingestion(&pd, nr_page, page_nums);

	fprintf(stderr, "total ingestion time(ns): %lu, total wait time(ns): %lu, total submission time(ns): 0, total submission cnt: 0\n",
			total_ingestion_time_ns,
			total_wait_time_ns);
	fprintf(stderr, "avg ingestion time(ns): %.3lf, avg wait time(ns): %.3lf\n",
			(float)total_ingestion_time_ns / (float)nr_page,
			(float)total_wait_time_ns / (float)(nr_page / PAGES_PER_BUFFER));

	/* Close pread_data */
	pread_data_close(&pd);

	free(page_nums);
	for (int i = 0; i < NUM_FILES; ++i) {
		close(fds[i]);
	}

	if (ret == 0)
	  return 0;
	else
	  return -1;
}
