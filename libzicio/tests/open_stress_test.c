#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>

/* Include zicio library */
#include <libzicio.h>

#define NUM_THREADS 64
#define IN_THREAD_WORK 4

static inline void print_error(const char* err_msg) 
{
  fprintf(stderr, "[ERROR] %s at %s:%d\n", err_msg, __FILE__, __LINE__);
}

void *thread_func(void *args)
{
	struct zicio zicio[IN_THREAD_WORK];

	if (args) {
	}

	for (int i = 0 ; i < IN_THREAD_WORK ; i++) {
		/* Init zicio structure */
		zicio_init(&zicio[i]);

		/* Set values before call open */
		zicio[i].local_fds = NULL;
		zicio[i].shareable_fds = NULL;
		zicio[i].nr_local_fd = 0;
		zicio[i].nr_shareable_fd = 0;
		zicio[i].zicio_flag = 0;


		/* Open zicio */
		zicio_open(&zicio[i]);
		if (zicio[i].open_status != ZICIO_OPEN_SUCCESS) {
			print_error("zicio open fail");
			return (void*)-1;
		}

		/* Check zicio id */
		if (zicio[i].zicio_id == -1) {
			print_error("zicio opened, but zicio id is invalid");
			return (void*)-1;
		}

		/* Check zicio channel idx used for user */
		if (zicio[i].zicio_channel_idx == -1) {
			print_error("zicio opened, but channel idx is invalid");
			return (void*)-1;
		}
	}

	for (int i = 0 ; i < IN_THREAD_WORK ; i++) {
		/* Close zicio */
		zicio_close(&zicio[i]);

		/* Check zicio close success */
		if (zicio[i].close_status != ZICIO_CLOSE_SUCCESS) {
			print_error("zicio close fail");
			return (void*)-1;
		}
	}

	return NULL;
}

/* 
 * Open test - open zicio and check values
 */
int main()
{
	pthread_t threads[NUM_THREADS];
	int threads_args[NUM_THREADS];
	int threads_ret[NUM_THREADS];

	for (int i = 0 ; i < NUM_THREADS ; i++) {
		threads_args[i] = i;
		int rc = pthread_create(&threads[i], NULL, thread_func, (void *)&threads_args[i]);

		if (rc) {
			print_error("thread creation fail");
			return -1;
		}
	}

	for (int i = 0 ; i < NUM_THREADS; i++) {
		pthread_join(threads[i], (void **)(&threads_ret[i]));
		if (threads_ret[i]) {
			return -1;
		}
	}

	
    return 0;	
}
