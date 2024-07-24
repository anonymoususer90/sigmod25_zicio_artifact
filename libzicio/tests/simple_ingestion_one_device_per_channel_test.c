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

#define MAX_NUM_FD 64
#define NUM_THREADS 2
#define NUM_FILES 2

#define __ZICIO_LIB_CHUNK_SIZE_SHIFT (21)
#define __ZICIO_LIB_CHUNK_SIZE	(1UL << __ZICIO_LIB_CHUNK_SIZE_SHIFT) /* 2 MiB */

_Thread_local int fds[MAX_NUM_FD];
_Thread_local int nr_fd;
_Thread_local struct zicio zicio;

_Atomic int success_cnt;
int num_data_path;
const char **data_path;
volatile int exit_program = 0;

static void sigintHandler()
{
	zicio_close(&zicio);
	exit_program = 1;
	exit(-1);
}

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
static int do_data_ingestion(struct zicio *zicio, int thread_id)
{
	const int ull_count_per_page =
		(__ZICIO_LIB_CHUNK_SIZE / sizeof(unsigned long));
	unsigned long v1 = 0;
	unsigned long v2 = 0;
	unsigned long etc = 0;
	unsigned long long *arr = NULL;

	while (!exit_program) {
		while (zicio->get_status != ZICIO_GET_PAGE_SUCCESS) {
			zicio_get_page(zicio);
		}

		assert(zicio->page_addr != NULL);
		arr = (unsigned long long *)zicio->page_addr;
		for (int ull_idx = 0; ull_idx < ull_count_per_page; ull_idx++) {
			if (arr[ull_idx] == 127)
				v1++;
			else if (arr[ull_idx] == 126)
				v2++;
			else
				etc++;
		}

		zicio_put_page(zicio);
		if (zicio->put_status == ZICIO_PUT_PAGE_EOF)
			break;
	}

	printf("[THREAD %d COMPLETE] v1: %lu, v2: %lu, etc: %lu\n", thread_id, v1, v2, etc);

	/*
	 * If there are other values(i.e. etc) or v1 and v2 have different value,
	 * there is error.
	 */
	if (etc > 0 || v1 != v2)
		return -1;
	else
		return 0;
}

void *thread_func(void *args)
{
	int thread_id;

	thread_id = *(int *)args;

	/* Open multiple file */
	for (int i = 0; i < NUM_FILES; ++i) {
		char path[256];

		/* Open a file */
		sprintf(path, "%s/data.%d", data_path[thread_id % num_data_path], i);
		if ((fds[i] = open(path, O_RDONLY | O_DIRECT)) < 0) {
			print_error("open file failed\n");
			return NULL;
		}
	}

	/* Init zicio structure */
	zicio_init(&zicio);

	/* Set values before call open */
	zicio.local_fds = fds;
	zicio.shareable_fds = NULL;
	zicio.nr_local_fd = NUM_FILES;
	zicio.nr_shareable_fd = 0;
	zicio.zicio_flag = 0;

	/* Open zicio */
	zicio_open(&zicio);
	if (zicio.open_status != ZICIO_OPEN_SUCCESS) {
		print_error("zicio open fail");
		return NULL;
	}

	/* Check zicio id */
	if (zicio.zicio_id == -1) {
		print_error("zicio opened, but zicio id is invalid");
		return NULL;
	}

	/* Check zicio channel idx used for user */
	if (zicio.zicio_channel_idx == -1) {
		print_error("zicio opened, but channel idx is invalid");
		return NULL;
	}

	/* Call zicio_get_page() repeatedly in loop */
	if (do_data_ingestion(&zicio, thread_id) == 0) {
		/* Success */
		atomic_fetch_add(&success_cnt, 1);
	}

	for (int i = 0; i < NUM_FILES; ++i) {
		close(fds[i]);
	}

	zicio_close(&zicio);

	return NULL;
}


/*
 * Data ingestion test with multi thread - read 4 GiB (i.e. 2 files) with 16 threads
 */
int main(int argc, char *args[])
{
	pthread_t threads[NUM_THREADS];
    int thread_args[NUM_THREADS];

	/* Set signal handler for forceful exit */
	signal(SIGINT, sigintHandler);
	signal(SIGTERM, sigintHandler);

	atomic_init(&success_cnt, 0);

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc < 3) {
		print_error("the number of parameter is invalid\n");
		return -1;
	}

	num_data_path = atoi(args[1]);

	if (num_data_path < 1) {
		print_error("the number of data path is invalid\n");
		return -1;
	}

	data_path = (const char **)malloc(sizeof(const char *) * num_data_path);

	for (int i = 0 ; i < num_data_path ; i++) {
		if (access(args[i + 2], 0)) {
			printf("data path[%s] is invalid\n", args[i + 2]);
			free(data_path);
			return -1;
		}
		data_path[i] = args[i + 2];
	}

    /* Create threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_args[i] = i;
        int rc = pthread_create(&threads[i], NULL, thread_func, (void *)&thread_args[i]);
        if (rc) {
			print_error("thread creation fail");
			return -1;
        }
    }

    /* Wait until all threads end */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

	if (atomic_load(&success_cnt) == NUM_THREADS)
	  return 0;
	else
	  return -1;
}
