#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h> 
#include <string.h>
#include <stdbool.h>

#include <libzicio.h>
#include <linux/zicio.h>

#define ZICIO_PREMAPPING_TEST_SIZE 512

static inline void print_error(const char* err_msg) {
  fprintf(stderr, "[ERROR] %s at %s:%d\n", err_msg, __FILE__, __LINE__);
}

/*
static void set_page(char *addr, unsigned long idx) {
	memcpy((void *) (addr), &idx, sizeof(unsigned long));
	memcpy((void *) (addr + 1024), &idx, sizeof(unsigned long));
	memcpy((void *) (addr + 2048 - 128), &idx, sizeof(unsigned long));
}

static int check_page(char *addr, unsigned long expected_idx, bool print) {
  	if (print == true) {
		printf("first:	%lu\n", *((unsigned long *) (addr)));
		printf("middle:	%lu\n", *((unsigned long *) (addr + 1024)));
		printf("last:	%lu\n", *((unsigned long *) (addr + 2048 - 128)));
	}

	if (*((unsigned long *) (addr)) != expected_idx)
		return -1;

	if (*((unsigned long *) (addr + 1024)) != expected_idx)
		return -1;

	if (*((unsigned long *) (addr + 2048 - 128)) != expected_idx)
		return -1;

	return 0;
}
*/

/*
 * ghost mapping test - do mapping and unmapping repeatedly
 */
int main()
{
	struct zicio zicio;
	int ret;
	struct zicio_args zicio_args;

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

	/* ZICIO_FLAG_FORCEFUL_UNMAPPING_TEST flag */
	//zicio_args.zicio_flag = 2;
	//zicio_args.user_base_address = zicio.zicio_id;

	/* Try unmapping */
	/*
	ret = syscall(548, &zicio_args);
	if (ret < 0) {
		print_error("1 unmapping fail\n");
		return -1;
	}
	*/

	/* ZICIO_FLAG_PREMAPPING_TEST flag */
	zicio_args.zicio_flag = 1;
	zicio_args.user_base_address = zicio.zicio_id;

	/* Try mapping */
	ret = syscall(548, &zicio_args);
	if (ret < 0) {
		print_error("2 mapping fail\n");
		return -1;
	}

	/* ZICIO_FLAG_FORCEFUL_UNMAPPING_TEST flag */
	zicio_args.zicio_flag = 2;
	zicio_args.user_base_address = zicio.zicio_id;

	/* Try unmapping */
	ret = syscall(548, &zicio_args);
	if (ret < 0) {
		print_error("3 unmapping fail\n");
		return -1;
	}

	/* ZICIO_FLAG_PREMAPPING_TEST flag */
	zicio_args.zicio_flag = 1;
	zicio_args.user_base_address = zicio.zicio_id;

	/* Try mapping */
	ret = syscall(548, &zicio_args);
	if (ret < 0) {
		print_error("4 mapping fail\n");
		return -1;
	}

	/* ZICIO_FLAG_FORCEFUL_UNMAPPING_TEST flag */
	zicio_args.zicio_flag = 2;
	zicio_args.user_base_address = zicio.zicio_id;

	/* Try unmapping */
	ret = syscall(548, &zicio_args);
	if (ret < 0) {
		print_error("5 unmapping fail\n");
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
