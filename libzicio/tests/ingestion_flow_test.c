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

/* include zicio library */
#include <libzicio.h>

#define MAX_NUM_FD 64
#define NUM_FILES 8

#define __ZICIO_LIB_CHUNK_SIZE_SHIFT (21)
#define __ZICIO_LIB_CHUNK_SIZE	(1UL << __ZICIO_LIB_CHUNK_SIZE_SHIFT) /* 2 MiB */

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

	while (1) {
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

	/* 
	 * If there are other values(i.e. etc) or v1 and v2 have different value,
	 * there is error.
	 */
	if (etc > 0 || v1 != v2)
		return -1;
	else
		return 0;
}

/* 
 * Data ingestion flow test - read 32 GiB from 8 files
 */
int main(int argc, char *args[])
{
	int fds[MAX_NUM_FD];
	struct zicio zicio;
	char *data_path;
	int ret = 0;
	
	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc != 2) {
			print_error("the number of parameter is invalid\n");
			return -1;
	}

	data_path = args[1];

	/* Open multiple file */
	for (int i = 0; i < NUM_FILES; ++i) {
		char path[256];

		sprintf(path, "%s/data.%d", data_path, i);

		if ((fds[i] = open(path, O_RDONLY | O_DIRECT)) < 0) {
			print_error("open file failed\n");
	 		return -1;
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

	/* 
	 * Call zicio_get_page() repeatedly in loop 
	 */
	ret = do_data_ingestion(&zicio);

	/* Close zicio */
	zicio_close(&zicio);

	/* Check zicio close success */
	if (zicio.close_status != ZICIO_CLOSE_SUCCESS) {
	  	print_error("zicio close fail");
		return -1;
	}

	for (int i = 0; i < NUM_FILES; ++i) {
		close(fds[i]);
	}

	if (ret == 0)
	  return 0;
	else
	  return -1;
}
