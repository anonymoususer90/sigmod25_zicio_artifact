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

int num_process;
unsigned long rand_sum;
#define MAX_NUM_FD 64
#define NUM_FILES 8

#define __ZICIO_LIB_CHUNK_SIZE_SHIFT (21)
#define __ZICIO_LIB_CHUNK_SIZE	(1UL << __ZICIO_LIB_CHUNK_SIZE_SHIFT) /* 2 MiB */

struct zicio zicio_shared_pool;
char dir_path[256];
char test_path[512];
char test_avg_path[512];
int test_fd[128];
int avg_test_fd[128];
int proc_id;
int num_pages;
unsigned long num_ops;

static inline unsigned long __zicio_test_rdtsc(void)
{
	unsigned hi, lo;
	__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long)lo) | (((unsigned long)hi) << 32);
}

static inline double get_nsecs_from_tsc(unsigned long tsc)
{
	/* TODO: get ghz of current cpu */
	return (double)tsc / 2.250;
}

static inline void print_error(const char* err_msg) 
{
  fprintf(stderr, "[ERROR] %s at %s:%d\n", err_msg, __FILE__, __LINE__);
}

__attribute__ ((optimize(0)))
unsigned long wait_without_memory_access()
{
	unsigned long start_tsc, end_tsc;
	start_tsc = __zicio_test_rdtsc();
	for (unsigned long i = 0 ; i < num_ops ; i++) {
		rand_sum += rand();
	}
	end_tsc = __zicio_test_rdtsc();

	assert(end_tsc >= start_tsc);

	return end_tsc - start_tsc;
}

/*
 * do_data_ingestion
 *
 * Return 0, success
 * Return -1, error
 */
static int do_data_ingestion(struct zicio *zicio)
{
	unsigned long long *arr = NULL;
	unsigned long sum = 0;
	char tsc_buffer[256];

	while (1) {
		while (zicio->get_status != ZICIO_GET_PAGE_SUCCESS) {
			zicio_get_page(zicio);
		}

		assert(zicio->page_addr != NULL);
		arr = (unsigned long long *)zicio->page_addr;

		for (int ull_idx = 0; ull_idx < num_pages ; ull_idx++) {
			sum += wait_without_memory_access();
		}

		zicio_put_page(zicio);

		sprintf(tsc_buffer, "%lf\n", get_nsecs_from_tsc(sum));
		if (write(test_fd[proc_id], tsc_buffer, strlen(tsc_buffer)) < 0) {
			printf("ERROR in test\n");
		}

		sprintf(tsc_buffer, "%lf\n", get_nsecs_from_tsc(
			zicio_get_current_avg_tsc(zicio)));
		if (write(avg_test_fd[proc_id], tsc_buffer, strlen(tsc_buffer)) < 0) {
			printf("ERROR in test\n");
		}
		sum = 0;
		
		if (zicio->put_status == ZICIO_PUT_PAGE_EOF)
			break;
	}

	close(test_fd[proc_id]);
	close(avg_test_fd[proc_id]);

	return (arr) ? 0 : 1;
}

void do_init_shared_zicio_args(struct zicio *zicio, int *fds, int nr_fd,
			zicio_shared_pool_key_t zicio_shared_pool_key)
{
	zicio->shareable_fds = fds;
	zicio->local_fds = NULL;
	zicio->nr_shareable_fd = nr_fd;
	zicio->nr_local_fd = 0;
	zicio->zicio_flag = 0;
	zicio->zicio_shared_pool_key = zicio_shared_pool_key;
}

void proc_func(int *fds, int nr_fd)
{
	struct zicio zicio;
	int ret = 0;

	assert(zicio_shared_pool.zicio_shared_pool_key);

	zicio_init(&zicio);
	do_init_shared_zicio_args(&zicio, fds, nr_fd, zicio_shared_pool.zicio_shared_pool_key);

	sprintf(test_path, "%s/test%d", dir_path, proc_id);
	test_fd[proc_id] = open(test_path, O_RDWR|O_CREAT, 0664);
	sprintf(test_avg_path, "%s/avg_test%d", dir_path, proc_id);
	avg_test_fd[proc_id] = open(test_avg_path, O_RDWR|O_CREAT, 0664);

	/* Open zicio channel and attach it to shared pool */
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

	/* 
	 * Call zicio_get_page() repeatedly in loop 
	 */
	ret = do_data_ingestion(&zicio);

	/* Close zicio */
	zicio_close(&zicio);

	/* Check zicio close success */
	if (zicio.close_status != ZICIO_CLOSE_SUCCESS) {
	  	print_error("zicio close fail");
		exit(EXIT_FAILURE);
	}

	for (int i = 0 ; i < NUM_FILES ; i++) {
		close(fds[0]);
	}

	if (ret == 0) {
		exit(EXIT_SUCCESS);
	} else {
		exit(EXIT_FAILURE);
	}
}

/* 
 * Simple data ingestion test - read 2 GiB from one file
 */
int main(int argc, char *args[])
{
	struct stat st = {0};
	pid_t pid = getpid();
	pid_t *child_pid;
	int fds[MAX_NUM_FD];
	char *data_path;

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH $NUM_TEST_RECORD */
	if (argc != 5) {
		print_error("the number of parameter is invalid\n");
		return -1;
	}

	data_path = args[1];

	num_ops = atoll(args[3]);

	num_pages = atoi(args[4]);

	/* Open multiple file */
	for (int i = 0 ; i < NUM_FILES ; i++) {
		char path[256];

		sprintf(path, "%s/data.%d", data_path, i);

		if ((fds[i] = open(path, O_RDONLY|O_DIRECT)) < 0) {
			print_error("open file failed\n");
			return -1;
		}
	}

	num_process = atoi(args[2]);
	child_pid = malloc(sizeof(pid_t) * num_process);

	/* Init zicio structure */
	zicio_init(&zicio_shared_pool);

	/* Set values before call open */
	do_init_shared_zicio_args(&zicio_shared_pool, fds, NUM_FILES, 0);
	/* Create zicio create pool. If succeeding shared pool,
	 * then zicio_shared_pool_key is set to a valid value */
	zicio_create_pool(&zicio_shared_pool);

	if (zicio_shared_pool.open_status != ZICIO_OPEN_SUCCESS) {
		print_error("zicio shared pull open fail");
		return -1;
	}


	sprintf(dir_path, "test_result_proc%d_page%d_loops%lu", num_process,
			num_pages, num_ops);

	if (stat(dir_path, &st) == -1) {
		mkdir(dir_path, 0755);
	}

	for (int i = 0 ; i < num_process ; i++) {
		if (pid != 0) {
			pid = fork();
			if (pid) {
				child_pid[proc_id] = pid;
			}
			proc_id++;
		} else {
			break;
		}
	}

	if (!pid) {
		proc_func(fds, NUM_FILES);
	}

	if (pid) {
		for (int i = 0 ; i < num_process ; i++) {
			waitpid(child_pid[i], NULL, 0);
		}
	}

	/* Destroy zicio create pool and check*/
	if (zicio_destroy_pool(zicio_shared_pool.zicio_shared_pool_key) < 0) {
		print_error("zicio shared pool destroy fail");
		exit(EXIT_FAILURE);
	}

	for (int i = 0 ; i < NUM_FILES ; i++) {
		close(fds[0]);
	}

	if (errno == ECHILD) {
		return -1;
	}

	return 0;
}
