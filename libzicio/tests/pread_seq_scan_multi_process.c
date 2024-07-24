#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sched.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>

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

int proc_id;

unsigned long long BUFFER_PAGE_SHIFT = 12;
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
static int do_data_ingestion(struct pread_data *pd)
{
	const int ull_count_per_page = 
		(PAGE_SIZE / sizeof(unsigned long));
	int nr_page = PAGES_PER_FILE * NUM_FILES;
	unsigned long long *arr = NULL;
	unsigned long long expected_page_num_sum = // (start + last) * page_cnt / 2
		(unsigned long long)(0 + (nr_page - 1)) * nr_page / 2;
	unsigned long long page_num_sum = 0;
	int cnt = 0;
	struct timespec begin_time, end_time;

	while (1) {
		pread_data_get_page(pd);
		clock_gettime(CLOCK_MONOTONIC, &begin_time);

		mb();

		assert(pd->page_addr != NULL);
		arr = (unsigned long long *)pd->page_addr;

		page_num_sum += (arr[0] >> PAGE_SHIFT); // page_num
		for (int i = 0; i < 10; i++) {
			for (int ull_idx = 0; ull_idx < ull_count_per_page; ull_idx++) {
				if (arr[ull_idx] % PAGE_SIZE != ull_idx * sizeof(unsigned long long)) {
					print_error("mismatched value");
					fprintf(stderr, "expected value: %ld, arr[ull_idx]: %llu, ull_idx: %d, cnt: %d\n", ull_idx * sizeof(unsigned long long), arr[ull_idx] % PAGE_SIZE, ull_idx, cnt);
					return -1;
				}
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

	/* 
	 * If there are other values(i.e. etc) or v1 and v2 have different value or
	 * cnt is different with nr_page, there is error.
	 */
	if (cnt != nr_page) {
		print_error("mismatched cnt");
		fprintf(stderr, "expected value: %d, cnt: %d\n", nr_page, cnt);
		return -1;
	} else if (page_num_sum != expected_page_num_sum) {
		print_error("mismatched page_num_sum");
		fprintf(stderr, "expected value: %llu, page_num_sum: %llu\n", expected_page_num_sum, page_num_sum);
		return -1;
	} else
		return 0;
}

void proc_func(int *fds)
{
	struct pread_data pd;
	int ret = 0;
	int nr_page = PAGES_PER_FILE * NUM_FILES;
	zicio_pread_breakdown_result pread_result;

	pread_data_init(&pd);

	pd.fds = fds;
	pd.nr_page = nr_page;

	total_ingestion_time_ns = 0;
	total_wait_time_ns = 0;

	zicio_u_pread_breakdown_start(true);

	/* 
	 * Call zicio_get_page() repeatedly in loop 
	 */
	ret = do_data_ingestion(&pd);

	pread_result = zicio_u_pread_breakdown_end();
	fprintf(stderr, "proc_id: %2d, ingestion(ns): %12lu, wait: %13lu, mode_switch: %12llu, data_copy: %12llu, storage_latency(ns): %12llu, io_latency(ns): %12llu\n",
			proc_id, total_ingestion_time_ns, total_wait_time_ns,
			pread_result.mode_switch_latency,
			pread_result.data_copy_latency,
			pread_result.storage_stacks_latency,
			pread_result.physical_io_latency);

	/* Close pread_data */
	pread_data_close(&pd);

	for (int i = 0; i < NUM_FILES; ++i) {
		close(fds[i]);
	}

	if (ret == 0)
	  exit(EXIT_SUCCESS);
	else
	  exit(EXIT_FAILURE);
}

/* 
 * Batched data ingestion test - read 80 GiB from 8 files
 */
int main(int argc, char *args[])
{
	pid_t pid = getpid();
	pid_t *child_pid;
	int fds[MAX_NUM_FD];
	char *data_path;
	int nr_procs = 16;
	int *child_status;
	int open_flag = O_RDONLY;

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc < 4 || argc > 6) {
		print_error("the number of parameter is invalid");
		return -1;
	}

	data_path = args[1];

	if (strcmp(args[2], "on") == 0) {
		open_flag |= O_DIRECT;
	} else if (strcmp(args[2], "off") != 0) {
		print_error("os_cache_bypass value is wrong (on|off)");
		return -1;
	}

	burning_loop_cnt = (unsigned long long)atoll(args[3]);

	if (argc >= 5)
		nr_procs = atoi(args[4]);

	if (argc >= 6)
		BUFFER_PAGE_SHIFT = (unsigned int)atoi(args[5]);

	BUFFER_PAGE_SIZE = 1 << BUFFER_PAGE_SHIFT;
	PAGES_PER_BUFFER = 1 << (BUFFER_PAGE_SHIFT - PAGE_SHIFT);

	/* Open multiple files */
	for (int i = 0; i < NUM_FILES; ++i) {
		char path[256];

		sprintf(path, "%s/data_seq.%d", data_path, i);

		if ((fds[i] = open(path, open_flag)) < 0) {
			print_error("open file failed");
	 		return -1;
		}

		accumulated_nr_pages[i] = i * PAGES_PER_FILE;
	}

	child_pid = (pid_t *)malloc(sizeof(pid_t) * nr_procs);
	child_status = (int *)malloc(sizeof(int) * nr_procs);

	for (int i = 0 ; i < nr_procs ; i++) {
		if (pid != 0) {
			pid = fork();
			if (pid) {
				child_pid[i] = pid;
			}
			proc_id++;
		} else {
			break;
		}
	}

	if (!pid) {
		proc_func(fds);
	}

	if (pid) {
		for (int i = 0 ; i < nr_procs ; i++) {
			waitpid(child_pid[i], &(child_status[i]), 0);
		}
	}

	for (int i = 0 ; i < nr_procs ; i++) {
		if (!WIFEXITED(child_status[i]))
			return 1;
		if (WEXITSTATUS(child_status[i]))
			return 1;
	}

	for (int i = 0 ; i < NUM_FILES ; i++) {
		close(fds[i]);
	}

	free(child_pid);
	free(child_status);

	if (errno == ECHILD) {
		return -1;
	}

	return 0;
}
