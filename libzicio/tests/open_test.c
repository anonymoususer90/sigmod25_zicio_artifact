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

static inline void print_error(const char* err_msg) 
{
  fprintf(stderr, "[ERROR] %s at %s:%d\n", err_msg, __FILE__, __LINE__);
}

/* 
 * Open test - open zicio and check values
 */
int main()
{
	struct zicio zicio;

	/* Init zicio structure */
	zicio_init(&zicio);

	/* Set values before call open */
	zicio.local_fds = NULL;
	zicio.shareable_fds = NULL;
	zicio.nr_local_fd = 0;
	zicio.nr_shareable_fd = 0;
	zicio.zicio_flag = 0;

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

	/* Close zicio */
	zicio_close(&zicio);

	/* Check zicio close success */
	if (zicio.close_status != ZICIO_CLOSE_SUCCESS) {
	  	print_error("zicio close fail");
		return -1;
	}

    return 0;	
}
