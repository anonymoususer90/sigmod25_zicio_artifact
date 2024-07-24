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
#include <stdatomic.h>
#include <signal.h>

/* include zicio library */
#include <libzicio.h>

#define MAX_NUM_FD	(64)
#define NUM_FILES	(8)
#define FILE_SHIFT	(31)
#define FILE_SIZE	(5 * ((unsigned long long)1 << FILE_SHIFT)) // 10 GiB

#define PAGE_SHIFT	(13)
#define PAGE_SIZE	(1 << PAGE_SHIFT) // 8 KiB

#define PAGES_PER_FILE	((unsigned long long)(FILE_SIZE) / \
						 (unsigned long long)(PAGE_SIZE))

#define print_error(err_msg) \
	print_error_internal(err_msg, __FILE__, __LINE__)

_Thread_local int fds[MAX_NUM_FD];
_Thread_local int nr_fd; 
_Thread_local struct zicio zicio;
_Thread_local unsigned long long *page_nums;

_Atomic int success_cnt;
char *data_path;
volatile int exit_program = 0;

static void sigintHandler()
{
	zicio_close(&zicio);
	exit_program = 1;
	exit(-1);
}

static inline void print_error_internal(const char* err_msg,
										char *file, int lineno) 
{
	fprintf(stderr, "[ERROR] \"%s\" at %s:%d\n", err_msg, file, lineno);
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
static int do_data_ingestion(struct zicio *zicio, int thread_id, int nr_page,
							 unsigned long long *page_nums)
{
	const int ull_count_per_page = 
		(PAGE_SIZE / sizeof(unsigned long));
	unsigned long long *arr = NULL;
	unsigned long long page_num;
	int cnt = 0;

	while (!exit_program) {
		while (zicio->get_status != ZICIO_GET_PAGE_SUCCESS) {
			zicio_get_page(zicio);
		}

		assert(zicio->page_addr != NULL);
		arr = (unsigned long long *)zicio->page_addr;

		for (int ull_idx = 0; ull_idx < ull_count_per_page; ull_idx++) {
			page_num = arr[ull_idx] >> PAGE_SHIFT;
			if (page_num != page_nums[cnt]) {
				print_error("mismatched page_num");
				fprintf(stderr, "expected value: %llu, page_num: %llu, ull_idx: %d\n", page_nums[cnt], page_num, ull_idx);
				return -1;
			}

			if (arr[ull_idx] % PAGE_SIZE != ull_idx * sizeof(unsigned long long)) {
				print_error("mismatched value");
				fprintf(stderr, "expected value: %ld, arr[ull_idx]: %llu, ull_idx: %d\n", ull_idx * sizeof(unsigned long long), arr[ull_idx] % PAGE_SIZE, ull_idx);
				return -1;
			}
		}
		cnt++;

		zicio_put_page(zicio);
		if (zicio->put_status == ZICIO_PUT_PAGE_EOF)
			break;
	}

	printf("[THREAD %d COMPLETE] cnt: %d, nr_page: %d\n", thread_id, cnt, nr_page);

	/* 
	 * If there are other values(i.e. etc) or v1 and v2 have different value or
	 * cnt is different with nr_page, there is error.
	 */
	if (cnt != nr_page)
		return -1;
	else
		return 0;
}

void *thread_func(void *args)
{
	int thread_id, fd;
	int nr_page = 0;

	thread_id = *(int *)args;

	/* Init zicio structure */
	zicio_init(&zicio);

	/* Set values before call open */
	zicio.local_fds = fds;
	zicio.shareable_fds = NULL;
	zicio.nr_shareable_fd = 0;
	zicio.zicio_flag = 0;
	zicio.read_page_size = PAGE_SIZE;

	page_nums =
		(unsigned long long*)malloc(PAGES_PER_FILE * sizeof(unsigned long long) *
									NUM_FILES);

	/* Open multiple file */
	for (int i = 0; i < NUM_FILES; ++i) {
		char path[256];

		sprintf(path, "%s/data_seq.%d", data_path, i);

		if ((fd = open(path, O_RDONLY | O_DIRECT)) < 0) {
			print_error("open file failed");
	 		return (void*)(1);
		}

		set_pages(&zicio, fd, i, 0, PAGES_PER_FILE - 1, &nr_page, page_nums);
	}

	/* Open zicio */
	zicio_open(&zicio);
	if (zicio.open_status != ZICIO_OPEN_SUCCESS) {
	  	print_error("zicio open fail");
		return (void*)(1);
	}

	/* Check zicio id */
	if (zicio.zicio_id == -1) {
	  	print_error("zicio opened, but zicio id is invalid");
		return (void*)(1);
	}

	/* Check zicio channel idx used for user */
	if (zicio.zicio_channel_idx == -1) {
	  	print_error("zicio opened, but channel idx is invalid");
		return (void*)(1);
	}

	/* Call zicio_get_page() repeatedly in loop */
	if (do_data_ingestion(&zicio, thread_id, nr_page, page_nums) == 0) {
		/* Success */
		atomic_fetch_add(&success_cnt, 1);
	}

	/* Close zicio */
	zicio_close(&zicio);

	/* Check zicio close success */
	if (zicio.close_status != ZICIO_CLOSE_SUCCESS) {
	  	print_error("zicio close fail");
		return (void*)(1);
	}

	free(page_nums);
	for (int i = 0; i < NUM_FILES; ++i) {
		close(fds[i]);
	}

	return NULL;
}

/* 
 * Batched data ingestion test - read 80 GiB from 8 files with 16 threads
 */
int main(int argc, char *args[])
{
	pthread_t *threads;
    int *thread_args;
	int num_threads;

	/* Set signal handler for forceful exit */
	signal(SIGINT, sigintHandler);
	signal(SIGTERM, sigintHandler);

	atomic_init(&success_cnt, 0);

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc != 3) {
			print_error("the number of parameter is invalid");
			return -1;
	}

	data_path = args[1];
	num_threads = atoi(args[2]);
	threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t));
	thread_args = (int*)malloc(num_threads * sizeof(int));

    /* Create threads */
    for (int i = 0; i < num_threads; i++) {
        thread_args[i] = i;
        int rc = pthread_create(&threads[i], NULL, thread_func, (void *)&thread_args[i]);
        if (rc) {
		  	print_error("thread creation fail");
			return -1;
        }
    }

    /* Wait until all threads end */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

	free(threads);
	free(thread_args);

	if (atomic_load(&success_cnt) == num_threads)
	  return 0;
	else
	  return -1;
}
