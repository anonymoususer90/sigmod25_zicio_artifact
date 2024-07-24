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
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <errno.h>

/* include zicio library */
#include <libzicio.h>

#define MAX_NUM_FD 64
#define NUM_PROCESS 40
#define NUM_FILES 8

#define __ZICIO_LIB_CHUNK_SIZE_SHIFT (21)
#define __ZICIO_LIB_CHUNK_SIZE	(1UL << __ZICIO_LIB_CHUNK_SIZE_SHIFT) /* 2 MiB */

_Thread_local int fds[MAX_NUM_FD];
_Thread_local int nr_fd; 
_Thread_local struct zicio zicio;

_Atomic int success_cnt;
char *data_path;
volatile int exit_program = 0;
int num_process = NUM_PROCESS;

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
static int do_data_ingestion(struct zicio *zicio, int process_id)
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

	printf("[PROCESS%d COMPLETE] v1: %lu, v2: %lu, etc: %lu\n", process_id, v1, v2, etc);

	/* 
	 * If there are other values(i.e. etc) or v1 and v2 have different value,
	 * there is error.
	 */
	if (etc > 0 || v1 != v2)
		return -1;
	else
		return 0;
}

int proc_id;

void proc_func()
{
	/* Open multiple file */
	for (int i = 0; i < NUM_FILES; ++i) {
		char path[256];

		/* Open a file */
		sprintf(path, "%s/data.%d", data_path, i);
		if ((fds[i] = open(path, O_RDONLY | O_DIRECT)) < 0) {
			print_error("open file failed\n");
			exit(EXIT_FAILURE);
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

	/* Call zicio_get_page() repeatedly in loop */
	if (do_data_ingestion(&zicio, proc_id) == 0) {
		/* Success */
		atomic_fetch_add(&success_cnt, 1);
	}

	for (int i = 0; i < NUM_FILES; ++i) {
		close(fds[i]);
	}

	zicio_close(&zicio);

	exit(EXIT_SUCCESS);
}


/* 
 * Data ingestion test with multi thread - read 4 GiB (i.e. 2 files) with 16 threads
 */
int main(int argc, char *args[])
{
	pid_t pid = getpid();
	pid_t *child_pid;

	/* Set signal handler for forceful exit */
	signal(SIGINT, sigintHandler);
	signal(SIGTERM, sigintHandler);

	atomic_init(&success_cnt, 0);

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc != 3) {
		print_error("the number of parameter is invalid\n");
		return -1;
	}

	data_path = args[1];
	num_process = atoi(args[2]);
	child_pid = (pid_t *)malloc(sizeof(pid_t) * num_process);
	
    /* Create process */
    for (int i = 0; i < num_process; i++) {
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
		proc_func();	
	}

	if (pid) {
		for (int i = 0 ; i < NUM_PROCESS ; i++) {
			waitpid(child_pid[i], NULL, 0);
		}
	}

	free(child_pid);

	if (errno == ECHILD) {
		return -1;
	}

	return 0;
}
