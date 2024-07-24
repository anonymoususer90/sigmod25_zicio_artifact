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
#include <time.h>

/* include zicio library */
#include <libzicio.h>

int mem_stat;
int num_process;
char test_data_path[512];
#define MAX_NUM_FD 64
#define NUM_FILES 8
#define NANOS 1000000000LL

#define __ZICIO_LIB_CHUNK_SIZE_SHIFT (21)
#define __ZICIO_LIB_CHUNK_SIZE	(1UL << __ZICIO_LIB_CHUNK_SIZE_SHIFT) /* 2 MiB */

struct zicio zicio_shared_pool;
int proc_id;

static inline void print_error(const char* err_msg) 
{
  fprintf(stderr, "[ERROR] %s at %s:%d\n", err_msg, __FILE__, __LINE__);
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
		(__ZICIO_LIB_CHUNK_SIZE / sizeof(unsigned long));
	unsigned long v1 = 0;
	unsigned long v2 = 0;
	unsigned long etc = 0;
	unsigned long long *arr = NULL;
	int cnt = 0;
	char file_path[1024];
	char mem_consumption_latency[512];
	struct timespec begin, end;
	struct timespec first_begin, first_end;
	long long time_diff;
	int fd = 0, first_fd = 0;

	if (mem_stat) {
		sprintf(file_path, "%s/worker_#%d", test_data_path, proc_id);
		fd = open(file_path, O_WRONLY|O_CREAT, 0644);
		sprintf(file_path, "%s/worker_first_read_#%d", test_data_path, proc_id);
		first_fd = open(file_path, O_WRONLY|O_CREAT, 0644);
	}

	while (1) {
		while (zicio->get_status != ZICIO_GET_PAGE_SUCCESS) {
			zicio_get_page(zicio);
		}

		if (mem_stat) {
			clock_gettime(CLOCK_MONOTONIC, &begin);
			clock_gettime(CLOCK_MONOTONIC, &first_begin);
		}

		assert(zicio->page_addr != NULL);
		arr = (unsigned long long *)zicio->page_addr;
		for (int ull_idx = 0; ull_idx < ull_count_per_page; ull_idx++) {
			if (mem_stat) {
				if (ull_idx == 0) {
					clock_gettime(CLOCK_MONOTONIC, &first_end);
				}
			}
			if (arr[ull_idx] == 127)
				v1++;
			else if (arr[ull_idx] == 126)
				v2++;
			else
				etc++;
		}

		if (mem_stat) {
			clock_gettime(CLOCK_MONOTONIC, &end);

			time_diff = ((long long)(end.tv_sec - begin.tv_sec) * NANOS) +
				(end.tv_nsec - begin.tv_nsec);

			sprintf(mem_consumption_latency, "%d: %lld\n", cnt, time_diff);
			if (write(fd, mem_consumption_latency,
						strlen(mem_consumption_latency)) < 0) {
				printf("write error\n");
			}


			time_diff = ((long long)(first_end.tv_sec - first_begin.tv_sec) *
				NANOS) + (first_end.tv_nsec - first_begin.tv_nsec);

			sprintf(mem_consumption_latency, "%d: %lld\n", cnt, time_diff);
			if (write(first_fd, mem_consumption_latency,
						strlen(mem_consumption_latency)) < 0) {
				printf("write error\n");
			}
		}

		zicio_put_page(zicio);
		if (zicio->put_status == ZICIO_PUT_PAGE_EOF)
			break;
		cnt++;
	}

	if (mem_stat) {
		close(fd);
		close(first_fd);
	}

	/* 
	 * If there are other values(i.e. etc) or v1 and v2 have different value,
	 * there is error.
	 */
	if (etc > 0 || v1 != v2) {
		printf("[Test failure]v1: %lu, v2: %lu, etc: %lu\n", v1, v2, etc);
		return -1;
	} else
		return 0;
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
	pid_t pid = getpid();
	pid_t *child_pid;
	int fds[MAX_NUM_FD];
	char *data_path;
	struct stat st = {0};

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc < 3) {
		print_error("the number of parameter is invalid\n");
		return -1;
	}

	data_path = args[1];

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

	if (argc == 4) {
		mem_stat = 1;
	}

	if (mem_stat) {
		char user[32];
		FILE *cmd;

		/* Get username */
		cmd = popen("whoami", "r");
		fgets(user, sizeof(user), cmd);
		user[strlen(user)-1] = '\0'; /* remove newline ('\n') */
		pclose(cmd);

		sprintf(test_data_path, "/home/%s/zicio_mem/worker_%d/",
				user, num_process);

		if (stat(test_data_path, &st) == -1) {
			if (errno == ENOENT) {
				if (mkdir(test_data_path, 0755) == -1) {
					perror("mkdir");
					return 1;
				}
			} else {
				perror("stat");
				return 1;
			}
		}
	}

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


	for (int i = 0 ; i < num_process ; i++) {
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
