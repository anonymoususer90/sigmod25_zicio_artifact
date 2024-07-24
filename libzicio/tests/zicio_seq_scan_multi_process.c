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

struct zicio zicio_shared_pool;
int proc_id;

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

/*
 * do_data_ingestion
 *
 * Return 0, success
 * Return -1, error
 */
static int do_data_ingestion(struct zicio *zicio)
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
		while (zicio->get_status != ZICIO_GET_PAGE_SUCCESS) {
			zicio_get_page(zicio);
		}
		clock_gettime(CLOCK_MONOTONIC, &begin_time);

		mb();

		assert(zicio->page_addr != NULL);
		arr = (unsigned long long *)zicio->page_addr;

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
		zicio_put_page(zicio);
		if (zicio->put_status == ZICIO_PUT_PAGE_EOF)
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

void proc_func(int *fds, int is_sharing)
{
	struct zicio zicio;
	int ret = 0;
	int local_fds[NUM_FILES];

	zicio_init(&zicio);

	if (is_sharing) {
		zicio.shareable_fds = fds;
		zicio.local_fds = NULL;
		zicio.nr_shareable_fd = NUM_FILES;
		zicio.nr_local_fd = 0;
		zicio.zicio_flag = 0;
		zicio.read_page_size = PAGE_SIZE;
		zicio.zicio_shared_pool_key = zicio_shared_pool.zicio_shared_pool_key;
	} else {
		zicio.local_fds = local_fds;
		zicio.shareable_fds = NULL;
		zicio.nr_shareable_fd = 0;
		zicio.zicio_flag = 0;
		zicio.read_page_size = PAGE_SIZE;

		for (int i = 0; i < NUM_FILES; ++i) {
			zicio_notify_ranges(&zicio, fds[i], 0, PAGES_PER_FILE - 1);
		}
	}

	/* Open zicio */
	zicio_open(&zicio);
	if (zicio.open_status != ZICIO_OPEN_SUCCESS) {
	  	print_error("zicio open fail");
		exit(EXIT_FAILURE);
	}

	/* Check zicio id */
	if (zicio.zicio_id == -1) {
	  	print_error("zicio opened, but zicio id is invalid");
		exit(EXIT_FAILURE);
	}

	/* Check zicio channel idx used for user */
	if (zicio.zicio_channel_idx == -1) {
	  	print_error("zicio opened, but channel idx is invalid");
		exit(EXIT_FAILURE);
	}

	total_ingestion_time_ns = 0;

	/* 
	 * Call zicio_get_page() repeatedly in loop 
	 */
	ret = do_data_ingestion(&zicio);

	total_wait_time_ns = zicio_get_wait_time(&zicio);
	fprintf(stderr, "proc_id: %2d, ingestion(ns): %12lu, wait: %13lu\n",
			proc_id, total_ingestion_time_ns, total_wait_time_ns);

	/* Close zicio */
	zicio_close(&zicio);

	/* Check zicio close success */
	if (zicio.close_status != ZICIO_CLOSE_SUCCESS) {
	  	print_error("zicio close fail");
		exit(EXIT_FAILURE);
	}

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
	int is_sharing = 0;

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc < 3 || argc > 5) {
			print_error("the number of parameter is invalid");
			return -1;
	}

	data_path = args[1];
	burning_loop_cnt = (unsigned long long)atoll(args[2]);

	if (argc >= 4)
		nr_procs = atoi(args[3]);

	if (argc == 5) {
		if (strcmp(args[4], "on") == 0) {
			is_sharing = 1;
		} else if (strcmp(args[4], "off") != 0) {
			print_error("is_sharing value is wrong (on|off)");
			return -1;
		}
	}

	/* Open multiple files */
	for (int i = 0; i < NUM_FILES; ++i) {
		char path[256];

		sprintf(path, "%s/data_seq.%d", data_path, i);

		if ((fds[i] = open(path, O_RDONLY | O_DIRECT)) < 0) {
			print_error("open file failed");
	 		return -1;
		}
	}

	child_pid = (pid_t *)malloc(sizeof(pid_t) * nr_procs);
	child_status = (int *)malloc(sizeof(int) * nr_procs);

	if (is_sharing) {
		/* Init zicio structure */
		zicio_init(&zicio_shared_pool);

		/* Set values before call open */
		zicio_shared_pool.shareable_fds = fds;
		zicio_shared_pool.local_fds = NULL;
		zicio_shared_pool.nr_shareable_fd = NUM_FILES;
		zicio_shared_pool.nr_local_fd = 0;
		zicio_shared_pool.zicio_flag = 0;
		zicio_shared_pool.zicio_shared_pool_key = 0;

		/* Create zicio shared pool. If succeeding shared pool,
		 * then zic_shared_pool_key is set to a valid value */
		zicio_create_pool(&zicio_shared_pool);

		if (zicio_shared_pool.open_status != ZICIO_OPEN_SUCCESS) {
			print_error("zicio shared pull open fail");
			return -1;
		}
	}

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
		proc_func(fds, is_sharing);
	}

	if (pid) {
		for (int i = 0 ; i < nr_procs ; i++) {
			waitpid(child_pid[i], &(child_status[i]), 0);
		}
	}

	if (is_sharing) {
		/* Destroy zicio shared pool and check*/
		if (zicio_destroy_pool(zicio_shared_pool.zicio_shared_pool_key) < 0) {
			print_error("zicio shared pool destroy fail");
			exit(EXIT_FAILURE);
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
